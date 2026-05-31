#import "display/ScreenCapture.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#define RDP_LOG_COMPONENT "capture"
#include "logging/RDPLog.h"

/*
 * ScreenCaptureKit (SCStream) capture.
 *
 * Replaces CGDisplayStream, which is deprecated since macOS 14 and on macOS 26
 * (Tahoe) delivers ZERO frames — even with kCGDisplayStreamShowCursor and active
 * cursor movement — producing a black remote desktop. SCStream is the sanctioned
 * API: it hands us IOSurface-backed CMSampleBuffers we feed straight to the encoder.
 *
 * A heartbeat timer re-feeds the most recent surface at a steady rate, because RDP
 * clients (mstsc) drop a GFX session that receives no frames for a couple seconds —
 * and a static desktop produces no capture callbacks. The heartbeat keeps frames
 * flowing (keepalive + ensures the client always has current content), and the
 * encoder turns unchanged frames into tiny P-frames so the cost is negligible.
 */

@interface ScreenCapture () <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) CGDirectDisplayID displayID;
@property (nonatomic, strong) SCStream *stream;
@property (nonatomic, assign) BOOL capturing;
@property (nonatomic, strong) dispatch_queue_t captureQueue;
@property (nonatomic, assign) uint64_t frameCount;
@property (nonatomic, assign) uint32_t capW;
@property (nonatomic, assign) uint32_t capH;
/* Most recent IOSurface, retained (IOSurfaceIncrementUseCount + CFRetain) so the
 * heartbeat can re-feed it after SCStream goes idle on a static screen. */
@property (nonatomic, assign) IOSurfaceRef lastSurface;
@property (nonatomic, strong) dispatch_source_t heartbeat;
@property (nonatomic, assign) uint64_t lastHeartbeatFrameCount;
@end

@implementation ScreenCapture

- (instancetype)initWithDisplayID:(CGDirectDisplayID)did {
    if ((self = [super init])) {
        _displayID    = did;
        _captureQueue = dispatch_queue_create("com.macosrdp.capture",
                                              DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (BOOL)isCapturing { return _capturing; }

- (void)setLastSurface:(IOSurfaceRef)surface {
    if (_lastSurface == surface) return;
    if (surface) { IOSurfaceIncrementUseCount(surface); CFRetain(surface); }
    if (_lastSurface) { IOSurfaceDecrementUseCount(_lastSurface); CFRelease(_lastSurface); }
    _lastSurface = surface;
}

- (BOOL)startWithWidth:(uint32_t)width height:(uint32_t)height {
    if (_capturing) return YES;
    _capW = width; _capH = height;
    rdp_verbose("starting SCStream on displayID=%u %ux%u", _displayID, width, height);

    /* Screen Recording must be granted (same TCC service SCStream uses). In the GUI
     * session this pops the prompt the first time and registers the binary. */
    if (!CGPreflightScreenCaptureAccess()) {
        rdp_error("Screen Recording not granted — requesting access (grant the prompt, "
                  "or System Settings > Privacy & Security > Screen Recording, then reconnect)");
        CGRequestScreenCaptureAccess();
    }

    _capturing = YES;

    __weak typeof(self) weak = self;
    /* SCShareableContent enumeration is async — set up the stream in its callback. */
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content,
                                                                   NSError *error) {
        typeof(self) self_ = weak;
        if (!self_ || !self_.capturing) return;
        if (error || !content) {
            rdp_error("SCShareableContent failed: %s",
                      error.localizedDescription.UTF8String ?: "unknown");
            return;
        }

        SCDisplay *target = nil;
        for (SCDisplay *d in content.displays) {
            if (d.displayID == self_.displayID) { target = d; break; }
        }
        if (!target) target = content.displays.firstObject;
        if (!target) { rdp_error("no SCDisplay available for capture"); return; }

        SCContentFilter *filter =
            [[SCContentFilter alloc] initWithDisplay:target excludingWindows:@[]];

        SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
        cfg.width                = width;
        cfg.height               = height;
        cfg.pixelFormat          = kCVPixelFormatType_32BGRA;
        cfg.minimumFrameInterval = CMTimeMake(1, 30);   /* cap ~30 fps (RDP-sane) */
        cfg.queueDepth           = 5;
        cfg.showsCursor          = NO;   /* let the RDP client draw the cursor — compositing
                                          * the Mac cursor too yields a laggy double cursor */

        SCStream *stream = [[SCStream alloc] initWithFilter:filter
                                              configuration:cfg
                                                   delegate:self_];
        NSError *addErr = nil;
        if (![stream addStreamOutput:self_ type:SCStreamOutputTypeScreen
                  sampleHandlerQueue:self_.captureQueue error:&addErr]) {
            rdp_error("SCStream addStreamOutput failed: %s",
                      addErr.localizedDescription.UTF8String ?: "unknown");
            return;
        }
        self_.stream = stream;

        [stream startCaptureWithCompletionHandler:^(NSError *startErr) {
            if (startErr) {
                rdp_error("SCStream startCapture failed: %s",
                          startErr.localizedDescription.UTF8String ?: "unknown");
                return;
            }
            rdp_info("capture started (SCStream) on displayID=%u", self_.displayID);
        }];

        /* Heartbeat: re-feed the latest surface so a static screen keeps streaming
         * (mstsc drops a GFX session that stops receiving frames). ~20 fps. */
        dispatch_source_t hb = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,
                                                      0, 0, self_.captureQueue);
        dispatch_source_set_timer(hb, dispatch_time(DISPATCH_TIME_NOW, 0),
                                  (uint64_t)(NSEC_PER_SEC / 4), NSEC_PER_SEC / 8);
        dispatch_source_set_event_handler(hb, ^{
            typeof(self) s2 = weak;
            if (!s2 || !s2.capturing) return;
            /* Coalesce with SCStream: if it delivered a frame since the last tick the
             * stream is active, so skip — otherwise we'd double the frame rate and
             * flood the client (which dropped us with an "invalid format" decode error
             * under ~110fps). The heartbeat only re-feeds when the screen is idle. */
            if (s2.frameCount != s2.lastHeartbeatFrameCount) {
                s2.lastHeartbeatFrameCount = s2.frameCount;
                return;
            }
            IOSurfaceRef surf = s2.lastSurface;
            ScreenCaptureFrameBlock handler = s2.frameHandler;
            if (surf && handler)
                handler(surf, s2.capW, s2.capH, CGRectMake(0, 0, s2.capW, s2.capH));
        });
        self_.heartbeat = hb;
        dispatch_resume(hb);
    }];

    return YES;
}

/* SCStreamOutput */
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) return;
    if (!sampleBuffer || !CMSampleBufferIsValid(sampleBuffer)) return;

    /* Only act on complete/started frames; idle/blank carry no fresh surface. */
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef att = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFNumberRef statusRef =
            (CFNumberRef)CFDictionaryGetValue(att, (__bridge CFStringRef)SCStreamFrameInfoStatus);
        int status = -1;
        if (statusRef) CFNumberGetValue(statusRef, kCFNumberIntType, &status);
        if (status != SCFrameStatusComplete && status != SCFrameStatusStarted)
            return;
    }

    CVImageBufferRef pixbuf = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixbuf) return;
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixbuf);
    if (!surface) return;

    self.lastSurface = surface;          /* retains; releases previous */
    self.frameCount++;
    if (self.frameCount % 300 == 0)
        rdp_verbose("capture: %llu frames", (unsigned long long)self.frameCount);

    ScreenCaptureFrameBlock handler = self.frameHandler;
    if (handler)
        handler(surface, self.capW, self.capH, CGRectMake(0, 0, self.capW, self.capH));
}

/* SCStreamDelegate */
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    rdp_verbose("SCStream stopped: %s",
                error.localizedDescription.UTF8String ?: "(no error)");
}

- (void)stop {
    if (!_capturing) return;
    rdp_verbose("stopping capture after %llu frames", (unsigned long long)_frameCount);
    _capturing = NO;

    if (_heartbeat) { dispatch_source_cancel(_heartbeat); _heartbeat = nil; }
    if (_stream) {
        [_stream stopCaptureWithCompletionHandler:^(NSError *e) { (void)e; }];
        _stream = nil;
    }
    self.lastSurface = NULL;  /* releases */
}

- (void)dealloc { [self stop]; }

@end
