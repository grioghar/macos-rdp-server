#import "daemon/RDPSession.h"
#import "protocol/RDPPeer.h"
#import "display/VirtualDisplay.h"
#import "display/ScreenCapture.h"
#import "display/FrameEncoder.h"
#import "input/InputInjector.h"
#import "input/ClipboardSync.h"
#import "audio/AudioCapture.h"
#import "audio/AudioRedirect.h"
#import <syslog.h>
#import <unistd.h>

static const uint32_t kDefaultWidth   = 1920;
static const uint32_t kDefaultHeight  = 1080;
static const uint32_t kDefaultBitrate = 8000; /* kbps */

@interface RDPSession ()
@property (nonatomic, assign) int fd;
@property (nonatomic, assign) RDPSessionState sessionState;
@property (nonatomic, strong) NSString *address;
@property (nonatomic, assign) freerdp_peer *peer;
@property (nonatomic, strong) VirtualDisplay  *display;
@property (nonatomic, strong) ScreenCapture   *capture;
@property (nonatomic, strong) FrameEncoder    *encoder;
@property (nonatomic, strong) InputInjector   *injector;
@property (nonatomic, strong) ClipboardSync   *clipboard;
@property (nonatomic, strong) AudioCapture    *audio;
@property (nonatomic, strong) dispatch_queue_t sessionQueue;
@property (nonatomic, strong) dispatch_source_t readSource;
@end

@implementation RDPSession

- (instancetype)initWithFileDescriptor:(int)fd clientAddress:(NSString *)address {
    if ((self = [super init])) {
        _fd             = fd;
        _address        = [address copy];
        _sessionState   = RDPSessionStateConnecting;
        _sessionQueue   = dispatch_queue_create("com.macosrdp.session",
                                                DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (int)clientFd           { return _fd; }
- (RDPSessionState)state  { return _sessionState; }
- (NSString *)clientAddress { return _address; }

- (void)start {
    dispatch_async(_sessionQueue, ^{ [self setupAndRun]; });
}

- (void)setupAndRun {
    /* Build peer callbacks bridging into self. */
    RDPPeerCallbacks cb = {
        .onKeyboard  = rdp_on_keyboard,
        .onMouse     = rdp_on_mouse,
        .onMouseEx   = rdp_on_mouse_ex,
        .onClipboard = rdp_on_clipboard,
        .onReady     = rdp_on_ready,
        .userdata    = (__bridge void *)self,
    };

    _peer = rdp_peer_create(_fd, &cb);
    if (!_peer) {
        [self endWithError:[NSError errorWithDomain:@"RDPError" code:1
                                          userInfo:@{NSLocalizedDescriptionKey: @"peer_create failed"}]];
        return;
    }

    _sessionState = RDPSessionStateNegotiating;

    /* Poll loop — runs until session ends. */
    while (_sessionState != RDPSessionStateDisconnecting &&
           _sessionState != RDPSessionStateDisconnected) {
        if (!rdp_peer_run_once(_peer)) {
            break;
        }
    }

    [self teardown];
}

/* Called from RDP peer when client is fully activated and has sent desktop size. */
static void rdp_on_ready(void *ud, uint32_t w, uint32_t h, uint32_t depth) {
    RDPSession *self = (__bridge RDPSession *)ud;
    dispatch_async(self.sessionQueue, ^{
        [self setupDisplayAndMediaForWidth:w height:h];
    });
}

static void rdp_on_keyboard(void *ud, uint16_t flags, uint16_t code) {
    RDPSession *self = (__bridge RDPSession *)ud;
    [self.injector injectKeyEvent:flags scanCode:code];
}

static void rdp_on_mouse(void *ud, uint16_t flags, uint16_t x, uint16_t y) {
    RDPSession *self = (__bridge RDPSession *)ud;
    if (flags & RDP_MOUSE_WHEEL) {
        [self.injector injectMouseWheelEvent:flags
                                       delta:(int16_t)(flags >> 8)
                                           x:x y:y];
    } else {
        [self.injector injectMouseEvent:flags x:x y:y];
    }
}

static void rdp_on_mouse_ex(void *ud, uint16_t flags, uint16_t x, uint16_t y) {
    RDPSession *self = (__bridge RDPSession *)ud;
    [self.injector injectMouseEvent:flags x:x y:y];
}

static void rdp_on_clipboard(void *ud, const uint8_t *data, size_t len,
                              uint32_t format) {
    RDPSession *self = (__bridge RDPSession *)ud;
    [self.clipboard receiveFromClient:data length:len format:format];
}

- (void)setupDisplayAndMediaForWidth:(uint32_t)w height:(uint32_t)h {
    uint32_t width  = w  ?: kDefaultWidth;
    uint32_t height = h ?: kDefaultHeight;

    /* Virtual display */
    _display = [[VirtualDisplay alloc] initWithWidth:width height:height hiDPI:NO];
    if (![_display create]) {
        syslog(LOG_WARNING, "VirtualDisplay creation failed, falling back to main display");
        _display = nil;
    }

    CGDirectDisplayID displayID = _display
        ? _display.displayID
        : CGMainDisplayID();

    /* Screen capture → encoder → peer */
    _encoder = [[FrameEncoder alloc] initWithWidth:width height:height
                                           bitrate:kDefaultBitrate];
    __weak typeof(self) weak = self;
    _encoder.outputHandler = ^(const uint8_t *data, size_t len, BOOL keyFrame) {
        (void)keyFrame;
        rdp_peer_send_h264_frame(weak.peer, data, len, width, height);
    };
    [_encoder start];

    _capture = [[ScreenCapture alloc] initWithDisplayID:displayID];
    _capture.frameHandler = ^(IOSurfaceRef surface, uint32_t fw, uint32_t fh,
                               CGRect dirty) {
        (void)fw; (void)fh; (void)dirty;
        [weak.encoder encodeFrame:surface];
    };
    [_capture startWithWidth:width height:height];

    /* Input injection */
    _injector = [[InputInjector alloc] initWithDisplayID:displayID];

    /* Clipboard */
    _clipboard = [[ClipboardSync alloc] init];
    _clipboard.sendToClientBlock = ^(const uint8_t *data, size_t len,
                                      uint32_t fmt) {
        rdp_peer_send_clipboard(weak.peer, data, len, fmt);
    };
    [_clipboard start];

    /* Audio */
    _audio = [[AudioCapture alloc] init];
    _audio.captureBlock = ^(const int16_t *samples, uint32_t frameCount) {
        RDPPeerContext *ctx = (RDPPeerContext *)weak.peer->context;
        if (ctx && ctx->rdpsnd) {
            audio_redirect_send(ctx->rdpsnd, samples, frameCount, 48000, 2);
        }
    };
    NSError *audioErr = nil;
    if (![_audio startWithError:&audioErr]) {
        syslog(LOG_WARNING, "Audio capture failed: %s",
               audioErr.localizedDescription.UTF8String);
    }

    _sessionState = RDPSessionStateActive;
}

- (void)disconnect {
    _sessionState = RDPSessionStateDisconnecting;
}

- (void)teardown {
    [_capture stop];
    [_encoder stop];
    [_audio stop];
    [_clipboard stop];
    [_display destroy];

    if (_peer) {
        rdp_peer_destroy(_peer);
        _peer = NULL;
    }
    if (_fd >= 0) {
        close(_fd);
        _fd = -1;
    }

    _sessionState = RDPSessionStateDisconnected;
    [self endWithError:nil];
}

- (void)endWithError:(NSError *)error {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.delegate sessionDidEnd:self error:error];
    });
}

@end
