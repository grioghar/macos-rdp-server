#import "daemon/RDPSession.h"
#import "protocol/RDPPeer.h"
#import "display/VirtualDisplay.h"
#import "display/ScreenCapture.h"
#import "display/FrameEncoder.h"
#import "input/InputInjector.h"
#import "input/ClipboardSync.h"
#import "audio/AudioCapture.h"
#import "audio/AudioRedirect.h"
#import <unistd.h>
#include <freerdp/freerdp.h>
#define RDP_LOG_COMPONENT "session"
#include "logging/RDPLog.h"

static const uint32_t kDefaultWidth   = 1920;
static const uint32_t kDefaultHeight  = 1080;
static const uint32_t kDefaultBitrate = 8000;

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

- (int)clientFd             { return _fd; }
- (RDPSessionState)state    { return _sessionState; }
- (NSString *)clientAddress { return _address; }

- (void)start {
    rdp_info("starting session for %s", _address.UTF8String);
    dispatch_async(_sessionQueue, ^{ [self setupAndRun]; });
}

- (void)setupAndRun {
    rdp_verbose("creating RDP peer for fd=%d", _fd);
    RDPPeerCallbacks cb = {
        .onKeyboard  = rdp_on_keyboard,
        .onMouse     = rdp_on_mouse,
        .onMouseEx   = rdp_on_mouse_ex,
        .onClipboard = rdp_on_clipboard,
        .onReady     = rdp_on_ready,
        .onKeyframeRequest = rdp_on_keyframe_request,
        .userdata    = (__bridge void *)self,
    };

    _peer = rdp_peer_create(_fd, &cb);
    if (!_peer) {
        rdp_error("rdp_peer_create failed for %s", _address.UTF8String);
        [self endWithError:[NSError errorWithDomain:@"RDPError" code:1
                            userInfo:@{NSLocalizedDescriptionKey: @"peer_create failed"}]];
        return;
    }

    _sessionState = RDPSessionStateNegotiating;
    rdp_verbose("RDP negotiation started with %s", _address.UTF8String);

    while (_sessionState != RDPSessionStateDisconnecting &&
           _sessionState != RDPSessionStateDisconnected) {
        if (!rdp_peer_run_once(_peer)) {
            rdp_verbose("peer loop ended for %s", _address.UTF8String);
            break;
        }
    }

    [self teardown];
}

static void rdp_on_ready(void *ud, uint32_t w, uint32_t h, uint32_t depth) {
    RDPSession *self = (__bridge RDPSession *)ud;
    rdp_info("client activated: %ux%u @%ubpp", w, h, depth);
    /* Called from peer_activate INSIDE the peer loop, which already runs on the
     * serial sessionQueue. dispatch_async back onto that same queue would starve
     * this behind the blocking loop — it would only run after the loop exits and
     * teardown has freed _peer (use-after-free crash), and media would never be
     * set up during a live session. Run it inline: the peer is alive here. */
    [self setupDisplayAndMediaForWidth:w height:h];
}

static void rdp_on_keyboard(void *ud, uint16_t flags, uint16_t code) {
    RDPSession *self = (__bridge RDPSession *)ud;
    rdp_debug("key flags=0x%04x code=0x%02x", flags, code);
    [self.injector injectKeyEvent:flags scanCode:code];
}

static void rdp_on_mouse(void *ud, uint16_t flags, uint16_t x, uint16_t y) {
    RDPSession *self = (__bridge RDPSession *)ud;
    rdp_debug("mouse flags=0x%04x x=%u y=%u", flags, x, y);
    if (flags & (RDP_PTR_WHEEL | RDP_PTR_HWHEEL))
        [self.injector injectMouseWheelEvent:flags x:x y:y];
    else
        [self.injector injectMouseEvent:flags x:x y:y];
}

static void rdp_on_mouse_ex(void *ud, uint16_t flags, uint16_t x, uint16_t y) {
    RDPSession *self = (__bridge RDPSession *)ud;
    rdp_debug("mouse_ex flags=0x%04x x=%u y=%u", flags, x, y);
    [self.injector injectMouseEvent:flags x:x y:y];
}

static void rdp_on_clipboard(void *ud, const uint8_t *data, size_t len,
                              uint32_t format) {
    RDPSession *self = (__bridge RDPSession *)ud;
    rdp_verbose("clipboard from client: format=0x%08x len=%zu", format, len);
    [self.clipboard receiveFromClient:data length:len format:format];
}

static void rdp_on_keyframe_request(void *ud) {
    RDPSession *self = (__bridge RDPSession *)ud;
    rdp_debug("keyframe requested by peer");
    [self.encoder forceKeyframe];
}

- (void)setupDisplayAndMediaForWidth:(uint32_t)w height:(uint32_t)h {
    uint32_t width  = w  ?: kDefaultWidth;
    uint32_t height = h ?: kDefaultHeight;

    rdp_info("setting up display %ux%u for %s", width, height, _address.UTF8String);

    _display = [[VirtualDisplay alloc] initWithWidth:width height:height];
    if (![_display create]) {
        rdp_verbose("VirtualDisplay unavailable, falling back to main display");
        _display = nil;
    } else {
        rdp_verbose("VirtualDisplay created: displayID=%u", _display.displayID);
    }

    CGDirectDisplayID displayID = _display ? _display.displayID : CGMainDisplayID();

    _encoder = [[FrameEncoder alloc] initWithWidth:width height:height
                                           bitrate:kDefaultBitrate];
    __weak typeof(self) weak = self;
    _encoder.outputHandler = ^(const uint8_t *data, size_t len, BOOL keyFrame,
                               uint16_t dx, uint16_t dy, uint16_t dw, uint16_t dh) {
        rdp_debug("encoded frame: len=%zu keyFrame=%d dirty=(%u,%u,%ux%u)",
                  len, keyFrame, dx, dy, dw, dh);
        rdp_peer_send_h264_frame(weak.peer, data, len, width, height,
                                 keyFrame ? true : false, dx, dy, dw, dh);
    };
    [_encoder start];
    rdp_verbose("H.264 encoder started at %u kbps", kDefaultBitrate);

    _capture = [[ScreenCapture alloc] initWithDisplayID:displayID];
    _capture.frameHandler = ^(IOSurfaceRef surface, uint32_t fw, uint32_t fh,
                               CGRect dirty) {
        (void)fw; (void)fh;
        /* Clamp the dirty origin/size to UINT16 surface-pixel coordinates. */
        uint16_t dx = (uint16_t)MAX(0.0, dirty.origin.x);
        uint16_t dy = (uint16_t)MAX(0.0, dirty.origin.y);
        uint16_t dw = (uint16_t)MIN((double)width,  dirty.size.width);
        uint16_t dh = (uint16_t)MIN((double)height, dirty.size.height);
        rdp_debug("captured frame: dirty=(%u,%u,%ux%u)", dx, dy, dw, dh);
        [weak.encoder encodeFrame:surface dirtyX:dx dirtyY:dy dirtyW:dw dirtyH:dh];
    };
    if ([_capture startWithWidth:width height:height])
        rdp_verbose("screen capture started on displayID=%u", displayID);
    else
        rdp_error("screen capture FAILED on displayID=%u — desktop will be black "
                  "until Screen Recording is granted", displayID);

    _injector  = [[InputInjector alloc] initWithDisplayID:displayID
                                              sourceWidth:width
                                             sourceHeight:height];
    _clipboard = [[ClipboardSync alloc] init];
    _clipboard.sendToClientBlock = ^(const uint8_t *data, size_t len, uint32_t fmt) {
        rdp_verbose("sending clipboard to client: format=0x%08x len=%zu", fmt, len);
        rdp_peer_send_clipboard(weak.peer, data, len, fmt);
    };
    [_clipboard start];

    /* Only start audio capture if the client actually declared audio support.
       Saves a CoreAudio IO proc registration for clients that don't want audio. */
    BOOL clientWantsAudio = freerdp_settings_get_bool(
        _peer->context->settings, FreeRDP_AudioPlayback);
    if (clientWantsAudio) {
        _audio = [[AudioCapture alloc] init];
        _audio.captureBlock = ^(const int16_t *samples, uint32_t frameCount) {
            rdp_debug("audio: %u frames", frameCount);
            rdp_peer_send_audio(weak.peer, samples, frameCount);
        };
        NSError *audioErr = nil;
        if (![_audio startWithError:&audioErr]) {
            rdp_verbose("audio capture unavailable: %s",
                        audioErr.localizedDescription.UTF8String);
        } else {
            rdp_verbose("audio capture started");
        }
    } else {
        rdp_verbose("client did not request audio — capture skipped");
    }

    _sessionState = RDPSessionStateActive;
    rdp_info("session active for %s", _address.UTF8String);
}

- (void)disconnect {
    rdp_info("disconnecting %s", _address.UTF8String);
    _sessionState = RDPSessionStateDisconnecting;
}

- (void)teardown {
    rdp_verbose("tearing down session for %s", _address.UTF8String);
    [_capture stop];
    [_encoder stop];
    [_audio stop];
    [_clipboard stop];
    [_display destroy];

    if (_peer) { rdp_peer_destroy(_peer); _peer = NULL; }
    if (_fd >= 0) { close(_fd); _fd = -1; }

    _sessionState = RDPSessionStateDisconnected;
    rdp_info("session torn down for %s", _address.UTF8String);
    [self endWithError:nil];
}

- (void)endWithError:(NSError *)error {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.delegate sessionDidEnd:self error:error];
    });
}

@end
