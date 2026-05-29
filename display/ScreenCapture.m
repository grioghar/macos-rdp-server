#import "display/ScreenCapture.h"
#import <CoreGraphics/CGDisplayStream.h>
#import <syslog.h>

@interface ScreenCapture ()
@property (nonatomic, assign) CGDirectDisplayID displayID;
@property (nonatomic, assign) CGDisplayStreamRef stream;
@property (nonatomic, assign) BOOL capturing;
@property (nonatomic, strong) dispatch_queue_t captureQueue;
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

    NSDictionary *opts = @{
        /* BGRA is the native format for CGDisplayStream — zero-copy path. */
        (__bridge NSString *)kCGDisplayStreamSourceRect:
            (__bridge id)CFBridgingRelease(
                CGRectCreateDictionaryRepresentation(CGRectMake(0,0,width,height))),
        (__bridge NSString *)kCGDisplayStreamPreserveAspectRatio: @NO,
        (__bridge NSString *)kCGDisplayStreamMinimumFrameTime:    @(1.0/60.0),
        (__bridge NSString *)kCGDisplayStreamShowCursor:          @YES,
    };

    __weak typeof(self) weak = self;
    _stream = CGDisplayStreamCreateWithDispatchQueue(
        _displayID,
        (size_t)width, (size_t)height,
        'BGRA',   /* kCVPixelFormatType_32BGRA */
        (__bridge CFDictionaryRef)opts,
        _captureQueue,
        ^(CGDisplayStreamFrameStatus status,
          uint64_t displayTime,
          IOSurfaceRef frameSurface,
          CGDisplayStreamUpdateRef updateRef) {
            (void)displayTime;
            if (status != kCGDisplayStreamFrameStatusFrameComplete) return;
            if (!frameSurface) return;

            /* Compute the dirty rect union for this update. */
            size_t rectCount = 0;
            const CGRect *rects = CGDisplayStreamUpdateGetRects(
                updateRef, kCGDisplayStreamUpdateDirtyRects, &rectCount);
            CGRect dirty = CGRectZero;
            for (size_t i = 0; i < rectCount; i++) {
                dirty = CGRectUnion(dirty, rects[i]);
            }
            if (CGRectIsEmpty(dirty)) {
                dirty = CGRectMake(0, 0, width, height);
            }

            ScreenCaptureFrameBlock handler = weak.frameHandler;
            if (handler) handler(frameSurface, width, height, dirty);
        }
    );

    if (!_stream) {
        syslog(LOG_ERR, "CGDisplayStreamCreate failed for display %u", _displayID);
        return NO;
    }

    CGError err = CGDisplayStreamStart(_stream);
    if (err != kCGErrorSuccess) {
        syslog(LOG_ERR, "CGDisplayStreamStart failed: %d", err);
        CFRelease(_stream);
        _stream = NULL;
        return NO;
    }

    _capturing = YES;
    return YES;
}

- (void)stop {
    if (!_capturing) return;
    _capturing = NO;
    if (_stream) {
        CGDisplayStreamStop(_stream);
        CFRelease(_stream);
        _stream = NULL;
    }
}

- (void)dealloc {
    [self stop];
}

@end
