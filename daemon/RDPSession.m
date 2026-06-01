#import "daemon/RDPSession.h"
#import "daemon/DisplayControl.h"
#import "daemon/Authenticator.h"
#import "protocol/RDPPeer.h"
#import "display/VirtualDisplay.h"
#import "display/ScreenCapture.h"
#import "display/FrameEncoder.h"
#import "display/CursorCapture.h"
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
@property (nonatomic, strong) DisplayControl  *displayControl;
@property (nonatomic, strong) ScreenCapture   *capture;
@property (nonatomic, strong) FrameEncoder    *encoder;
@property (nonatomic, strong) InputInjector   *injector;
@property (nonatomic, strong) ClipboardSync   *clipboard;
@property (nonatomic, strong) AudioCapture    *audio;
@property (nonatomic, strong) CursorCapture   *cursor;
@property (nonatomic, strong) dispatch_queue_t sessionQueue;
/* Signaled exactly once when teardown completes; lets the takeover path wait
 * for this session's display to be released without touching the main queue. */
@property (nonatomic, strong) dispatch_semaphore_t teardownSem;
/* Set once this session has authenticated + become the active session, so we
 * skip re-authenticating / re-activating on a subsequent Activate (mstsc may
 * re-activate on a resize). Guarded by the serial session queue. */
@property (nonatomic, assign) BOOL activated;
@end

@implementation RDPSession

- (instancetype)initWithFileDescriptor:(int)fd clientAddress:(NSString *)address {
    if ((self = [super init])) {
        _fd             = fd;
        _address        = [address copy];
        _sessionState   = RDPSessionStateConnecting;
        _sessionQueue   = dispatch_queue_create("com.macosrdp.session",
                                                DISPATCH_QUEUE_SERIAL);
        _teardownSem    = dispatch_semaphore_create(0);
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

    uint32_t lastAudioRate = 0;
    while (_sessionState != RDPSessionStateDisconnecting &&
           _sessionState != RDPSessionStateDisconnected) {
        if (!rdp_peer_run_once(_peer)) {
            rdp_verbose("peer loop ended for %s", _address.UTF8String);
            break;
        }
        /* rdpsnd format negotiation completes asynchronously (Activated callback)
         * AFTER audio capture starts. The client plays our PCM at the rate IT
         * selected — rdpsnd does not resample — so the tap must be resampled to
         * that exact rate or the audio is pitch-shifted. Poll the negotiated rate
         * and push it to AudioCapture once known/changed. Cheap; the setter
         * no-ops when unchanged. */
        if (_audio) {
            uint32_t rate = rdp_peer_get_audio_rate(_peer);
            if (rate && rate != lastAudioRate) {
                _audio.outputSampleRate = rate;
                rdp_info("audio playback rate negotiated: %u Hz — "
                         "tap resampled to match (no pitch shift)", (unsigned)rate);
                lastAudioRate = rate;
            }
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
    /* mstsc can re-activate (e.g. on a client-side resize). We only authenticate
     * and bring the display up once — re-entry after we're active is a no-op. */
    if (_activated || _sessionState == RDPSessionStateActive) {
        rdp_verbose("re-activation ignored (session already active) for %s",
                    _address.UTF8String);
        return;
    }

    /* ── Authenticate BEFORE creating any virtual display ─────────────────────
     * Credentials arrived in the RDP logon (peer_activate captured them). Validate
     * against the local macOS account. Fail-closed: bad/missing creds → tear the
     * connection down. This runs inline on the serial session queue (inside the
     * peer loop), so a synchronous check is safe. NEVER log the password. */
    const char *cuser = NULL, *cpass = NULL, *cdomain = NULL;
    rdp_peer_get_credentials(_peer, &cuser, &cpass, &cdomain);
    NSString *user   = cuser   ? [NSString stringWithUTF8String:cuser]   : nil;
    NSString *pass   = cpass   ? [NSString stringWithUTF8String:cpass]   : nil;
    NSString *domain = cdomain ? [NSString stringWithUTF8String:cdomain] : nil;

    if (![Authenticator validateUsername:user password:pass domain:domain]) {
        rdp_error("authentication FAILED for %s (user='%s') — rejecting connection",
                  _address.UTF8String, user.length ? user.UTF8String : "(none)");
        _sessionState = RDPSessionStateDisconnecting;
        return;  /* peer loop sees Disconnecting and exits → teardown, no display */
    }
    rdp_info("authentication OK for %s (user='%s')",
             _address.UTF8String, user.length ? user.UTF8String : "(none)");

    /* ── Become the single active session (takeover) ──────────────────────────
     * Ask the server to make us active. It signals any currently-active session
     * to disconnect and BLOCKS here until that old session has released its
     * virtual display, so we never have two virtual displays / two main displays
     * fighting. If it returns NO we were superseded (or the server is stopping)
     * and must abort. */
    if ([self.delegate respondsToSelector:@selector(sessionDidAuthenticateAndRequestActivation:)]) {
        if (![self.delegate sessionDidAuthenticateAndRequestActivation:self]) {
            rdp_info("activation refused for %s — aborting session", _address.UTF8String);
            _sessionState = RDPSessionStateDisconnecting;
            return;
        }
    }
    _activated = YES;

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

    /* Wake the Mac + keep it awake for the whole session (replaces caffeinate),
     * and privacy-dim the BUILT-IN panel (brightness -> 0) so a bystander can't
     * watch the remote session. Pass the virtual display id so we never dim the
     * screen the remote actually captures. RDP_SHARED_MODE=1 skips dimming. */
    _displayControl = [[DisplayControl alloc] initWithVirtualDisplayID:displayID];
    [_displayControl start];

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
            /* No per-buffer logging here: buffers arrive ~100x/sec and flooded the
             * log. AudioCapture itself logs a throttled frame total. */
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

    /* Advertise a client-side system cursor first so the very first pointer the
     * client gets is valid (and the cursor is smooth, decoupled from the video
     * frame rate) before the real shapes start streaming. */
    rdp_peer_send_default_cursor(_peer);

    /* Stream the REAL Mac cursor shapes (I-beam, resize, hand, …) as RDP
     * color-pointer PDUs. OPT-IN via RDP_CURSOR_SHAPES=1 (default OFF): the
     * current 32bpp color-pointer renders INVISIBLE on mstsc (shapes are captured
     * and sent at correct sizes, but the client draws nothing — a 32bpp-alpha
     * color-pointer quirk under investigation). Default keeps the client-side
     * SYSPTR_DEFAULT arrow above, which is always visible. Re-enable once the
     * pointer encoding is fixed (likely switch to 24bpp XOR + 1bpp AND mask). */
    const char *curShapes = getenv("RDP_CURSOR_SHAPES");
    if (curShapes && strcmp(curShapes, "1") == 0) {
        _cursor = [[CursorCapture alloc] init];
        _cursor.handler = ^(const uint8_t *bgra, uint32_t cw, uint32_t ch,
                            uint16_t hotX, uint16_t hotY) {
            rdp_peer_send_cursor_shape(weak.peer, bgra, cw, ch, hotX, hotY);
        };
        [_cursor start];
        rdp_info("cursor-shape streaming ON (RDP_CURSOR_SHAPES=1)");
    } else {
        rdp_info("cursor-shape streaming OFF (default) — client draws the system "
                 "arrow; set RDP_CURSOR_SHAPES=1 to stream real Mac cursor shapes");
    }
}

- (void)disconnect {
    rdp_info("disconnecting %s", _address.UTF8String);
    /* The peer loop polls this state every <=50ms and exits when it sees
     * Disconnecting, after which teardown runs on the session queue. */
    _sessionState = RDPSessionStateDisconnecting;
}

- (BOOL)waitForTeardown:(NSTimeInterval)timeout {
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW,
                                             (int64_t)(timeout * NSEC_PER_SEC));
    long rc = dispatch_semaphore_wait(_teardownSem, deadline);
    if (rc != 0)
        rdp_error("timed out after %.0fs waiting for %s to release its display",
                  timeout, _address.UTF8String);
    return rc == 0;
}

- (void)teardown {
    rdp_verbose("tearing down session for %s", _address.UTF8String);
    [_cursor stop];
    [_capture stop];
    [_encoder stop];
    [_audio stop];
    [_clipboard stop];
    [_displayControl stop];
    [_display destroy];

    if (_peer) { rdp_peer_destroy(_peer); _peer = NULL; }
    if (_fd >= 0) { close(_fd); _fd = -1; }

    _sessionState = RDPSessionStateDisconnected;
    rdp_info("session torn down for %s", _address.UTF8String);

    /* Signal anyone waiting on takeover that our display is now released. Done
     * here (on the session queue, after destroy) rather than via the main-queue
     * delegate hop below — the main run loop does not run in this daemon, so the
     * semaphore is the reliable cross-thread completion signal. */
    dispatch_semaphore_signal(_teardownSem);

    [self endWithError:nil];
}

- (void)endWithError:(NSError *)error {
    /* Notify on a background queue, NOT the main queue: this daemon's main thread
     * blocks in kevent() with no running main run loop, so a main-queue async
     * would never fire and the server would never remove this session. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        [self.delegate sessionDidEnd:self error:error];
    });
}

@end
