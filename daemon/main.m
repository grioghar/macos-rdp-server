#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <syslog.h>
#import <signal.h>
#import <sys/event.h>
#import <unistd.h>
#import "daemon/RDPServer.h"
#import "daemon/RDPSession.h"
#import "daemon/AutoUpdate.h"
#define RDP_LOG_COMPONENT "main"
#include "logging/RDPLog.h"

/* Report this binary's TCC permission state using the official preflight APIs
 * (no Full Disk Access or TCC.db poking needed — they answer for this exact
 * binary's identity). Prints a parseable summary; exits 0 only if BOTH are
 * granted, so scripts can gate on the exit code. */
static int check_permissions(void) {
    BOOL screen = CGPreflightScreenCaptureAccess();
    BOOL access = AXIsProcessTrusted();
    fprintf(stdout, "screen_recording=%s\n", screen ? "granted" : "denied");
    fprintf(stdout, "accessibility=%s\n",    access ? "granted" : "denied");
    return (screen && access) ? 0 : 1;
}

@interface AppDelegate : NSObject <RDPServerDelegate>
@end

@implementation AppDelegate

- (void)serverDidAcceptSession:(RDPSession *)session {
    rdp_info("client connected from %s", session.clientAddress.UTF8String);
}

- (void)serverSession:(RDPSession *)session didEndWithError:(NSError *)error {
    if (error)
        rdp_error("session %s ended: %s",
                  session.clientAddress.UTF8String,
                  error.localizedDescription.UTF8String);
    else
        rdp_info("session %s ended cleanly", session.clientAddress.UTF8String);
}

@end

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port <n>            TCP port to listen on (default: 3389)\n"
        "  --log-level <l>       error|info|verbose|debug (default: info)\n"
        "  --check-permissions   report Screen Recording + Accessibility state, then exit\n",
        prog);
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        openlog("macos-rdp-daemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);

        /* A LaunchDaemon starts with no HOME. WinPR's WTSOpenServer (used to
         * create the virtual-channel manager) resolves paths under HOME, so a
         * missing HOME makes freerdp_peer_context_new fail outright. Root's
         * home is /var/root. Set TMPDIR too for any scratch-file paths. */
        if (!getenv("HOME"))   setenv("HOME", "/var/root", 1);
        if (!getenv("TMPDIR")) setenv("TMPDIR", "/var/tmp", 1);

        uint16_t port = 3389;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = (uint16_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
                int lvl = rdp_log_level_from_string(argv[++i]);
                if (lvl >= 0) rdp_log_set_level((RDPLogLevel)lvl);
                else { fprintf(stderr, "Unknown log level '%s'\n", argv[i]); return 1; }
            } else if (strcmp(argv[i], "--check-permissions") == 0) {
                return check_permissions();
            } else if (strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]); return 0;
            }
        }
        const char *env = getenv("RDP_LOG_LEVEL");
        if (env) { int l = rdp_log_level_from_string(env); if (l >= 0) rdp_log_set_level((RDPLogLevel)l); }

        rdp_info("macos-rdp-daemon %s (build %s) starting (log level: %s)",
                 MACOS_RDP_VERSION, MACOS_RDP_BUILD_VERSION,
                 (const char *[]){"error","info","verbose","debug"}[rdp_log_get_level()]);

        /* Block SIGTERM/SIGINT at the signal level; catch via kqueue instead.
           This gives us a clean shutdown path with zero polling overhead. */
        signal(SIGPIPE, SIG_IGN);
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigprocmask(SIG_BLOCK, &mask, NULL);

        int kq = kqueue();
        struct kevent kev[2];
        EV_SET(&kev[0], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        EV_SET(&kev[1], SIGINT,  EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        kevent(kq, kev, 2, NULL, 0, NULL);

        AppDelegate *delegate = [[AppDelegate alloc] init];
        RDPServer *server = [[RDPServer alloc] initWithPort:port];
        server.delegate = delegate;

        NSError *error = nil;
        if (![server startWithError:&error]) {
            rdp_error("failed to start server on port %u: %s",
                      port, error.localizedDescription.UTF8String);
            return 1;
        }
        rdp_info("listening on port %u", port);

        /* Keep the system from idle-sleeping while the daemon is loaded, so an
         * open-lid Mac stays reachable on the LAN / Tailscale even when the user
         * is away. Without this the Mac goes idle, drops off the network, and
         * remote connections fail with 0x904. Per-session display-sleep assertions
         * are still held in DisplayControl for the duration of each session.
         * RDP_ALLOW_IDLE_SLEEP=1 disables this for setups where battery life
         * matters more than always-on reachability. */
        IOPMAssertionID daemonWakeAssertion = kIOPMNullAssertionID;
        const char *allowSleep = getenv("RDP_ALLOW_IDLE_SLEEP");
        if (!(allowSleep && strcmp(allowSleep, "1") == 0)) {
            IOReturn ar = IOPMAssertionCreateWithName(
                kIOPMAssertionTypePreventUserIdleSystemSleep,
                kIOPMAssertionLevelOn,
                CFSTR("macos-rdp: daemon loaded (keep reachable)"),
                &daemonWakeAssertion);
            if (ar == kIOReturnSuccess)
                rdp_info("holding system-sleep assertion for daemon lifetime "
                         "(set RDP_ALLOW_IDLE_SLEEP=1 to disable)");
            else
                rdp_error("daemon-lifetime sleep assertion failed (0x%x) — "
                          "Mac may sleep and become unreachable", ar);
        }

        /* Start the silent self-updater (no-op if RDP_UPDATE_ENABLED=0). It
         * runs entirely on its own background serial queue + dispatch timer —
         * it never touches the main thread (which is about to park in kevent). */
        [AutoUpdate start];

        /* Spin the run loop on a background thread so CFRunLoop/dispatch works,
           while this thread blocks on kqueue — zero CPU until a signal arrives. */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            [[NSRunLoop currentRunLoop] run];
        });

        struct kevent got;
        struct timespec zero = {0, 0};
        while (1) {
            int n = kevent(kq, NULL, 0, &got, 1, NULL); /* blocks until signal */
            if (n > 0 && (got.ident == SIGTERM || got.ident == SIGINT)) break;
        }
        close(kq);

        rdp_info("shutting down");
        if (daemonWakeAssertion != kIOPMNullAssertionID)
            IOPMAssertionRelease(daemonWakeAssertion);
        [server stop];
        closelog();
        return 0;
    }
}
