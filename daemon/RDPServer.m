#import "daemon/RDPServer.h"
#import "daemon/RDPSession.h"
#import <sys/socket.h>
#import <netinet/in.h>
#import <arpa/inet.h>
#import <fcntl.h>
#import <unistd.h>
#define RDP_LOG_COMPONENT "server"
#include "logging/RDPLog.h"

@interface RDPServer () <RDPSessionDelegate>
@property (nonatomic, assign) int listenFd;
@property (nonatomic, assign) uint16_t portValue;
@property (nonatomic, assign) BOOL running;
@property (nonatomic, strong) NSMutableArray<RDPSession *> *sessions;
@property (nonatomic, strong) dispatch_source_t acceptSource;
@property (nonatomic, strong) dispatch_queue_t acceptQueue;
/* The single active (authenticated) session that owns the display. Guarded by
 * @synchronized(self) along with `sessions`. A new client that authenticates
 * supersedes whatever is here. */
@property (nonatomic, strong, nullable) RDPSession *activeSession;
@end

/* Process-wide active-session flag. The daemon runs a single RDPServer, so a
 * class-level flag faithfully mirrors that instance's `_activeSession`. The
 * auto-updater reads this (off any thread) to decide whether to defer a swap. */
static volatile int32_t g_hasActiveSession = 0;

@implementation RDPServer

+ (BOOL)hasActiveSession {
    return g_hasActiveSession != 0;
}

- (void)updateActiveSessionFlag {
    /* Caller holds @synchronized(self). */
    g_hasActiveSession = (_activeSession != nil) ? 1 : 0;
}

- (instancetype)initWithPort:(uint16_t)port {
    if ((self = [super init])) {
        _portValue = port;
        _listenFd  = -1;
        _sessions  = [NSMutableArray array];
        _acceptQueue = dispatch_queue_create("com.macosrdp.accept",
                                             DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (uint16_t)port    { return _portValue; }
- (BOOL)isRunning   { return _running; }

- (BOOL)startWithError:(NSError **)error {
    rdp_verbose("creating IPv6 dual-stack listen socket on port %u", _portValue);

    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        rdp_error("socket() failed: %s", strerror(errno));
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                code:errno userInfo:nil];
        return NO;
    }

    int yes = 1, no = 0;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(_portValue);
    addr.sin6_addr   = in6addr_any;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        rdp_error("bind() failed on port %u: %s", _portValue, strerror(errno));
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                code:errno userInfo:nil];
        close(fd);
        return NO;
    }
    if (listen(fd, 8) < 0) {
        rdp_error("listen() failed: %s", strerror(errno));
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                code:errno userInfo:nil];
        close(fd);
        return NO;
    }

    _listenFd = fd;
    _running  = YES;
    rdp_debug("listen socket fd=%d ready", fd);

    _acceptSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                                           (uintptr_t)fd, 0, _acceptQueue);
    __weak typeof(self) weak = self;
    dispatch_source_set_event_handler(_acceptSource, ^{
        [weak acceptConnection];
    });
    dispatch_resume(_acceptSource);
    return YES;
}

- (void)acceptConnection {
    struct sockaddr_in6 clientAddr = {0};
    socklen_t len = sizeof(clientAddr);
    int clientFd = accept(_listenFd, (struct sockaddr *)&clientAddr, &len);
    if (clientFd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            rdp_error("accept() failed: %s", strerror(errno));
        return;
    }

    char addrBuf[INET6_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET6, &clientAddr.sin6_addr, addrBuf, sizeof(addrBuf));
    NSString *addr = [NSString stringWithUTF8String:addrBuf];

    rdp_verbose("accepted connection from %s (fd=%d)", addrBuf, clientFd);

    RDPSession *session = [[RDPSession alloc] initWithFileDescriptor:clientFd
                                                       clientAddress:addr];
    session.delegate = self;

    @synchronized(self) { [_sessions addObject:session]; }
    rdp_debug("active sessions: %lu", (unsigned long)_sessions.count);

    [self.delegate serverDidAcceptSession:session];
    [session start];
}

- (void)stop {
    rdp_info("stopping server");
    _running = NO;
    if (_acceptSource) {
        dispatch_source_cancel(_acceptSource);
        _acceptSource = nil;
    }
    if (_listenFd >= 0) {
        close(_listenFd);
        _listenFd = -1;
    }
    @synchronized(self) {
        rdp_verbose("disconnecting %lu active sessions", (unsigned long)_sessions.count);
        for (RDPSession *s in _sessions) [s disconnect];
        [_sessions removeAllObjects];
        _activeSession = nil;
        [self updateActiveSessionFlag];
    }
}

/* A session authenticated its client and wants to own the display. Make it the
 * single active session, superseding (disconnecting + waiting on the teardown
 * of) any session currently active. Called on the NEW session's serial queue;
 * the blocking wait happens off-lock so concurrent connection attempts don't
 * stall. Returns NO if the server is stopping. */
- (BOOL)sessionDidAuthenticateAndRequestActivation:(RDPSession *)session {
    if (!_running) {
        rdp_info("activation refused: server is stopping");
        return NO;
    }

    RDPSession *previous = nil;
    @synchronized(self) {
        previous = _activeSession;
        if (previous == session) {
            rdp_verbose("session %s already active", session.clientAddress.UTF8String);
            return YES;
        }
        /* Claim active immediately so a third concurrent client that authenticates
         * supersedes US in turn rather than racing on a stale pointer. */
        _activeSession = session;
        [self updateActiveSessionFlag];
    }

    if (previous) {
        rdp_info("TAKEOVER: %s superseding active session %s",
                 session.clientAddress.UTF8String,
                 previous.clientAddress.UTF8String);
        /* Tell the old session to exit its peer loop + tear down (releases its
         * virtual display + display assertions), then block until it has done so
         * BEFORE we let the new session create its own virtual display — two live
         * virtual displays would both map to the main display and fight. */
        [previous disconnect];
        if (![previous waitForTeardown:10.0])
            rdp_error("TAKEOVER: previous session %s did not tear down in time — "
                      "proceeding anyway (display may briefly conflict)",
                      previous.clientAddress.UTF8String);
        else
            rdp_info("TAKEOVER: previous session %s released the display",
                     previous.clientAddress.UTF8String);
    } else {
        rdp_info("session %s is now the active session (no prior session)",
                 session.clientAddress.UTF8String);
    }

    /* If a still-newer client superseded us while we waited, abort. */
    @synchronized(self) {
        if (_activeSession != session) {
            rdp_info("session %s was superseded while activating — aborting",
                     session.clientAddress.UTF8String);
            [self updateActiveSessionFlag];
            return NO;
        }
    }
    return YES;
}

- (void)sessionDidEnd:(RDPSession *)session error:(NSError *)error {
    @synchronized(self) {
        [_sessions removeObject:session];
        if (_activeSession == session) _activeSession = nil;
        [self updateActiveSessionFlag];
    }
    rdp_debug("session removed; %lu remaining", (unsigned long)_sessions.count);
    [self.delegate serverSession:session didEndWithError:error];
}

@end
