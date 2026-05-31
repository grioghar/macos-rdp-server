#import "display/CursorCapture.h"
#import <AppKit/AppKit.h>

#define RDP_LOG_COMPONENT "cursor"
#include "logging/RDPLog.h"

/* Cap the bitmap we send. Most macOS cursors are 32x32 (or 64x64 at 2x).
 * RDP color/large pointers tolerate large sizes, but mstsc renders best with
 * a modest cap and it keeps the bytestream small. */
static const uint32_t kMaxCursorDim = 96;

/* ~12 Hz: fast enough that a shape change (I-beam, resize, hand) feels
 * instantaneous without burning CPU rasterizing the cursor every frame. */
static const NSTimeInterval kPollInterval = 1.0 / 12.0;

@interface CursorCapture ()
@property (nonatomic, strong, nullable) NSTimer *timer;
@property (nonatomic, assign) uint64_t lastHash;   /* 0 == nothing sent yet */
@end

@implementation CursorCapture

- (void)start {
    /* NSCursor is main-thread-affine; drive the timer on the main run loop. */
    if ([NSThread isMainThread]) {
        [self startTimer];
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{ [self startTimer]; });
    }
}

- (void)startTimer {
    if (_timer) return;
    _lastHash = 0;
    __weak typeof(self) weak = self;
    _timer = [NSTimer scheduledTimerWithTimeInterval:kPollInterval
                                             repeats:YES
                                               block:^(NSTimer *t) {
        (void)t;
        [weak poll];
    }];
    /* Fire once promptly so the client gets the real cursor without waiting a
     * full poll interval. */
    [self poll];
    rdp_verbose("cursor capture started (%.0f Hz)", 1.0 / kPollInterval);
}

- (void)stop {
    if ([NSThread isMainThread]) {
        [self stopTimer];
    } else {
        dispatch_sync(dispatch_get_main_queue(), ^{ [self stopTimer]; });
    }
}

- (void)stopTimer {
    [_timer invalidate];
    _timer = nil;
    rdp_verbose("cursor capture stopped");
}

/* FNV-1a over the bitmap bytes + dimensions + hotspot, to detect shape changes. */
static uint64_t fnv1a(const uint8_t *p, size_t n, uint64_t seed) {
    uint64_t h = seed ? seed : 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

- (void)poll {
    NSCursor *cursor = [NSCursor currentSystemCursor];
    if (!cursor) cursor = [NSCursor currentCursor];
    if (!cursor) return;

    NSImage *image = cursor.image;
    if (!image) return;
    NSPoint hot = cursor.hotSpot;

    uint32_t w = 0, h = 0;
    uint16_t hotX = 0, hotY = 0;
    /* Owned malloc'd BGRA buffer; freed below after the handler runs. */
    uint8_t *bgra = [self rasterize:image hotSpot:hot
                              width:&w height:&h hotX:&hotX hotY:&hotY];
    if (!bgra || w == 0 || h == 0) { free(bgra); return; }

    uint64_t hash = fnv1a(bgra, (size_t)w * h * 4, 0);
    hash = fnv1a((const uint8_t *)&w, sizeof(w), hash);
    hash = fnv1a((const uint8_t *)&h, sizeof(h), hash);
    hash = fnv1a((const uint8_t *)&hotX, sizeof(hotX), hash);
    hash = fnv1a((const uint8_t *)&hotY, sizeof(hotY), hash);

    if (hash == _lastHash) { free(bgra); return; }
    _lastHash = hash;

    rdp_debug("cursor shape changed: %ux%u hot=(%u,%u)", w, h, hotX, hotY);
    if (self.handler) self.handler(bgra, w, h, hotX, hotY);
    free(bgra);
}

/* Render an NSImage into a freshly malloc'd 32-bit BGRA premultiplied,
 * top-left-origin, tightly packed buffer. Scales down to kMaxCursorDim if the
 * source is larger. Returns NULL on failure. Caller frees. */
- (uint8_t *)rasterize:(NSImage *)image
               hotSpot:(NSPoint)hot
                 width:(uint32_t *)outW height:(uint32_t *)outH
                  hotX:(uint16_t *)outHotX hotY:(uint16_t *)outHotY {
    /* hotSpot is in the image's point coordinate space (top-left origin for
     * cursor images). Use the image's nominal point size for output dims and
     * the hotspot mapping. */
    NSSize pointSize = image.size;
    if (pointSize.width <= 0 || pointSize.height <= 0) return NULL;

    uint32_t w = (uint32_t)llround(pointSize.width);
    uint32_t h = (uint32_t)llround(pointSize.height);
    if (w == 0 || h == 0) return NULL;

    double scale = 1.0;
    uint32_t maxDim = w > h ? w : h;
    if (maxDim > kMaxCursorDim) {
        scale = (double)kMaxCursorDim / (double)maxDim;
        w = (uint32_t)llround(pointSize.width  * scale);
        h = (uint32_t)llround(pointSize.height * scale);
        if (w == 0) w = 1;
        if (h == 0) h = 1;
    }

    size_t stride = (size_t)w * 4;
    size_t bytes  = stride * h;
    uint8_t *buf = calloc(1, bytes);
    if (!buf) return NULL;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) { free(buf); return NULL; }

    /* kCGImageAlphaPremultipliedFirst + ByteOrder32Little == BGRA byte order
     * (B,G,R,A) in memory, premultiplied — exactly the RDP color-pointer xor
     * layout we want. Top-left origin (default CG context is bottom-left, but
     * we flip the draw below to land top-left). */
    CGContextRef cgctx = CGBitmapContextCreate(
        buf, w, h, 8, stride, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!cgctx) { free(buf); return NULL; }

    NSGraphicsContext *gctx =
        [NSGraphicsContext graphicsContextWithCGContext:cgctx flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:gctx];

    /* Draw the image to fill the (possibly downscaled) target rect. The CG
     * context is bottom-left origin, so the image lands bottom-up; we flip the
     * rows after drawing to produce a top-left-origin bitmap. */
    [image drawInRect:NSMakeRect(0, 0, w, h)
             fromRect:NSZeroRect
            operation:NSCompositingOperationCopy
             fraction:1.0
       respectFlipped:NO
                hints:nil];

    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(cgctx);

    /* Flip vertically: CG produced bottom-up rows; we want top-down. */
    uint8_t *tmp = malloc(stride);
    if (tmp) {
        for (uint32_t y = 0; y < h / 2; y++) {
            uint8_t *a = buf + (size_t)y * stride;
            uint8_t *b = buf + (size_t)(h - 1 - y) * stride;
            memcpy(tmp, a, stride);
            memcpy(a, b, stride);
            memcpy(b, tmp, stride);
        }
        free(tmp);
    }

    *outW = w;
    *outH = h;
    /* Hotspot in source points -> scaled target pixels, clamped to bounds. */
    long hx = llround(hot.x * scale);
    long hy = llround(hot.y * scale);
    if (hx < 0) hx = 0; if ((uint32_t)hx >= w) hx = w ? (long)w - 1 : 0;
    if (hy < 0) hy = 0; if ((uint32_t)hy >= h) hy = h ? (long)h - 1 : 0;
    *outHotX = (uint16_t)hx;
    *outHotY = (uint16_t)hy;
    return buf;
}

@end
