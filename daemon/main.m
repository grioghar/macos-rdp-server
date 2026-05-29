#import <Foundation/Foundation.h>
#import <syslog.h>
#import <signal.h>
#import "daemon/RDPServer.h"
#import "daemon/RDPSession.h"

static volatile sig_atomic_t g_should_exit = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
}

@interface AppDelegate : NSObject <RDPServerDelegate>
@end

@implementation AppDelegate

- (void)serverDidAcceptSession:(RDPSession *)session {
    syslog(LOG_INFO, "RDP client connected: %s",
           session.clientAddress.UTF8String);
}

- (void)serverSession:(RDPSession *)session didEndWithError:(NSError *)error {
    if (error) {
        syslog(LOG_WARNING, "Session ended with error: %s",
               error.localizedDescription.UTF8String);
    } else {
        syslog(LOG_INFO, "RDP session ended cleanly: %s",
               session.clientAddress.UTF8String);
    }
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        openlog("macos-rdp-daemon", LOG_PID | LOG_NDELAY, LOG_DAEMON);
        syslog(LOG_INFO, "macos-rdp-daemon starting (version %s)",
               MACOS_RDP_VERSION);

        signal(SIGTERM, handle_signal);
        signal(SIGINT,  handle_signal);
        signal(SIGPIPE, SIG_IGN);

        uint16_t port = 3389;
        for (int i = 1; i < argc - 1; i++) {
            if (strcmp(argv[i], "--port") == 0) {
                port = (uint16_t)atoi(argv[i + 1]);
            }
        }

        AppDelegate *delegate = [[AppDelegate alloc] init];
        RDPServer *server = [[RDPServer alloc] initWithPort:port];
        server.delegate = delegate;

        NSError *error = nil;
        if (![server startWithError:&error]) {
            syslog(LOG_ERR, "Failed to start RDP server: %s",
                   error.localizedDescription.UTF8String);
            return 1;
        }

        syslog(LOG_INFO, "Listening for RDP connections on port %u", port);

        /* Run the main run loop; signals will flip g_should_exit. */
        while (!g_should_exit) {
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
        }

        syslog(LOG_INFO, "Shutting down");
        [server stop];
        closelog();
        return 0;
    }
}
