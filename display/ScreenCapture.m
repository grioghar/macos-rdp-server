#import "display/ScreenCapture.h"
#import <CoreGraphics/CGDisplayStream.h>
#define RDP_LOG_COMPONENT "capture"
#include "logging/RDPLog.h"

@interface ScreenCapture ()
@property (nonatomic, assign) CGDirectDisplayID displayID;
@property (nonatomic, assign) CGDisplayStreamRef stream;
@property (nonatomic, assign) BOOL capturing;
@property (nonatomic, strong) dispatch_queue_t captureQueue;
@property (nonatomic, assign) uint64_t frameCount;
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

- (BOOL)startWithWidth:(uint32_t)width height:(uint32_t)height {
    if (_capturing) return YES;
    rdp_verbose("starting CGDisplayStream on displayID=%u %ux%u", _displayID, width, height);

    NSDictionary *opts = @{
        (__bridge NSString *)kCGDisplayStreamPreserveAspectRatio: @NO,
        (__bridge NSString *)kCGDisplayStreamMinimumFrameTime:    @(1.0/60.0),
        (__bridge NSString *)kCGDisplayStreamShowCursor:          @YES,
    };

    __weak typeof(self) weak = self;
    _stream = CGDisplayStreamCreateWithDispatchQueue(
        _displayID, (size_t)width, (size_t)height,
        'BGRA', (__bridge CFDictionaryRef)opts, _captureQueue,
        ^(CGDisplayStreamFrameStatus status, uint64_t displayTime,
          IOSurfaceRef frameSurface, CGDisplayStreamUpdateRef updateRef) {
            (void)displayTime;
            if (status == kCGDisplayStreamFrameStatusStopped) {
                rdp_verbose("display stream stopped");
                return;
            }
            if (status != kCGDisplayStreamFrameStatusFrameComplete || !frameSurface) return;

            /* Dirty-rect gate: skip encode entirely when nothing changed.
               On a static screen this saves 100% of encode CPU at idle. */
            size_t rectCount = 0;
            const CGRect *rects = CGDisplayStreamUpdateGetRects(
                updateRef, kCGDisplayStreamUpdateDirtyRects, &rectCount);
            if (rectCount == 0) {
                rdp_debug("capture: frame skipped (no dirty rects)");
                return;
            }

            weak.frameCount++;
            if (weak.frameCount % 300 == 0)
                rdp_verbose("capture: %llu frames encoded", (unsigned long long)weak.frameCount);

            /* Compute dirty union for callers that want it. */
            CGRect dirty = rects[0];
            for (size_t i = 1; i < rectCount; i++)
                dirty = CGRectUnion(dirty, rects[i]);

            rdp_debug("frame dirty=(%.0f,%.0f,%.0fx%.0f) rects=%zu",
                      dirty.origin.x, dirty.origin.y,
                      dirty.size.width, dirty.size.height, rectCount);

            ScreenCaptureFrameBlock handler = weak.frameHandler;
            if (handler) handler(frameSurface, width, height, dirty);
        }
    );

    if (!_stream) {
        rdp_error("CGDisplayStreamCreate failed for displayID=%u", _displayID);
        return NO;
    }

    CGError err = CGDisplayStreamStart(_stream);
    if (err != kCGErrorSuccess) {
        rdp_error("CGDisplayStreamStart failed: %d (check Screen Recording permission)", err);
        CFRelease(_stream); _stream = NULL;
        return NO;
    }

    _capturing = YES;
    rdp_info("capture started on displayID=%u", _displayID);
    return YES;
}

- (void)stop {
    if (!_capturing) return;
    rdp_verbose("stopping capture after %llu frames", (unsigned long long)_frameCount);
    _capturing = NO;
    if (_stream) { CGDisplayStreamStop(_stream); CFRelease(_stream); _stream = NULL; }
}

- (void)dealloc { [self stop]; }

@end
