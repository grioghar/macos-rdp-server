#import "daemon/RDPServer.h"
#import "daemon/RDPSession.h"
#import <sys/socket.h>
#import <netinet/in.h>
#import <arpa/inet.h>
#import <fcntl.h>
#import <unistd.h>
#import <syslog.h>

@interface RDPServer () <RDPSessionDelegate>
@property (nonatomic, assign) int listenFd;
@property (nonatomic, assign) uint16_t portValue;
@property (nonatomic, assign) BOOL running;
@property (nonatomic, strong) NSMutableArray<RDPSession *> *sessions;
@property (nonatomic, strong) dispatch_source_t acceptSource;
@property (nonatomic, strong) dispatch_queue_t acceptQueue;
@end

@implementation RDPServer

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
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                code:errno userInfo:nil];
        return NO;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    /* Dual-stack: accept IPv4-mapped addresses on the IPv6 socket. */
    int no = 0;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(_portValue);
    addr.sin6_addr   = in6addr_any;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                                code:errno userInfo:nil];
        close(fd);
        return NO;
    }

    _listenFd = fd;
    _running  = YES;

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
    if (clientFd < 0) return;

    char addrBuf[INET6_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET6, &clientAddr.sin6_addr, addrBuf, sizeof(addrBuf));
    NSString *addr = [NSString stringWithUTF8String:addrBuf];

    RDPSession *session = [[RDPSession alloc] initWithFileDescriptor:clientFd
                                                       clientAddress:addr];
    session.delegate = self;

    @synchronized(self) {
        [_sessions addObject:session];
    }

    [self.delegate serverDidAcceptSession:session];
    [session start];
}

- (void)stop {
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
        for (RDPSession *s in _sessions) [s disconnect];
        [_sessions removeAllObjects];
    }
}

/* RDPSessionDelegate */
- (void)sessionDidEnd:(RDPSession *)session error:(NSError *)error {
    @synchronized(self) {
        [_sessions removeObject:session];
    }
    [self.delegate serverSession:session didEndWithError:error];
}

@end
