#import <Foundation/Foundation.h>
#import <syslog.h>
#import <signal.h>
#import "daemon/RDPServer.h"
#import "daemon/RDPSession.h"
#define RDP_LOG_COMPONENT "main"
#include "logging/RDPLog.h"

static volatile sig_atomic_t g_should_exit = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
}

@interface AppDelegate : NSObject <RDPServerDelegate>
@end

@implementation AppDelegate

- (void)serverDidAcceptSession:(RDPSession *)session {
    rdp_info("client connected from %s", session.clientAddress.UTF8String);
}

- (void)serverSession:(RDPSession *)session didEndWithError:(NSError *)error {
    if (error) {
        rdp_error("session %s ended: %s",
                  session.clientAddress.UTF8String,
                  error.localizedDescription.UTF8String);
    } else {
        rdp_info("session %s ended cleanly", session.clientAddress.UTF8String);
    }
}

@end

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port <n>       TCP port to listen on (default: 3389)\n"
        "  --log-level <l>  Log level: error|info|verbose|debug (default: info)\n",
        prog);
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        openlog("macos-rdp-daemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);

        uint16_t port = 3389;

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = (uint16_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
                int lvl = rdp_log_level_from_string(argv[++i]);
                if (lvl < 0) {
                    fprintf(stderr, "Unknown log level '%s'\n", argv[i]);
                    print_usage(argv[0]);
                    return 1;
                }
                rdp_log_set_level((RDPLogLevel)lvl);
            } else if (strcmp(argv[i], "--help") == 0) {
                print_usage(argv[0]);
                return 0;
            }
        }

        /* Also honour the RDP_LOG_LEVEL env var (useful in launchd plists). */
        const char *env_level = getenv("RDP_LOG_LEVEL");
        if (env_level) {
            int lvl = rdp_log_level_from_string(env_level);
            if (lvl >= 0) rdp_log_set_level((RDPLogLevel)lvl);
        }

        rdp_info("macos-rdp-daemon %s starting (log level: %s)",
                 MACOS_RDP_VERSION,
                 (const char *[]){"error","info","verbose","debug"}[rdp_log_get_level()]);

        signal(SIGTERM, handle_signal);
        signal(SIGINT,  handle_signal);
        signal(SIGPIPE, SIG_IGN);

        AppDelegate *delegate = [[AppDelegate alloc] init];
        RDPServer *server = [[RDPServer alloc] initWithPort:port];
        server.delegate = delegate;

        NSError *error = nil;
        if (![server startWithError:&error]) {
            rdp_error("failed to start server on port %u: %s",
                      port, error.localizedDescription.UTF8String);
            return 1;
        }

        rdp_info("listening for RDP connections on port %u", port);

        while (!g_should_exit) {
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        }

        rdp_info("shutting down");
        [server stop];
        closelog();
        return 0;
    }
}
