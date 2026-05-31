#import "display/CursorCapture.h"
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#define RDP_LOG_COMPONENT "cursor"
#include "logging/RDPLog.h"

/*
 * HEADLESS CURSOR CAPTURE (read before changing the timer or read path)
 * ---------------------------------------------------------------------
 * This daemon has NO running main run loop: the main thread blocks forever in
 * kevent() (see daemon/main.m), and there is no NSApplication. So:
 *   - We CANNOT drive an NSTimer on the main run loop (it would never fire).
 *     Instead we run a dispatch_source timer on a dedicated background serial
 *     queue, mirroring how DisplayControl.m drives work off the main thread.
 *   - We CANNOT rely on NSCursor.currentSystemCursor: it is AppKit/main-thread-
 *     affine and unreliable in a headless LaunchAgent. Instead we read the live
 *     on-screen system cursor via the private CoreGraphics (CGS) cursor API,
 *     which answers from any thread without AppKit. NSCursor remains only as a
 *     last-resort fallback if CGS yields nothing.
 */

/* ── Private CoreGraphics (CGS / SkyLight) cursor API ─────────────────────────
 * These are private symbols (no public header) used to read the actual on-screen
 * system cursor from a daemon. Signatures cross-checked against the community
 * CGSInternal headers; stable across macOS releases including 26. */
typedef int CGSConnectionID;
extern CGSConnectionID CGSMainConnectionID(void);
/* Bumps whenever the system cursor changes — a cheap poll-time change detector. */
extern int CGSCurrentCursorSeed(void);
/* Byte size of the active cursor's bitmap, for allocating the data buffer. */
extern CGError CGSGetGlobalCursorDataSize(CGSConnectionID cid, size_t *outSize);
/* Full cursor read: bitmap bytes + layout + on-screen rect + hotspot.
 *   outData            caller-provided buffer of CGSGetGlobalCursorDataSize bytes
 *   outDataSize        bytes actually written
 *   outRowBytes        source stride
 *   outRect            cursor rect (size = bitmap dimensions in pixels)
 *   outHotSpot         hotspot in cursor-local points (top-left origin)
 *   outDepth           bits per pixel (typically 32)
 *   outComponents      components per pixel (typically 4 = RGBA/ARGB)
 *   outBitsPerComponent typically 8 */
extern CGError CGSGetGlobalCursorData(CGSConnectionID cid, void *outData,
                                      int *outDataSize, int *outRowBytes,
                                      CGRect *outRect, CGPoint *outHotSpot,
                                      int *outDepth, int *outComponents,
                                      int *outBitsPerComponent);

/* Cap the bitmap we send. Most macOS cursors are 32x32 (or 64x64 at 2x).
 * RDP color/large pointers tolerate large sizes, but mstsc renders best with
 * a modest cap and it keeps the bytestream small. */
static const uint32_t kMaxCursorDim = 96;

/* ~12 Hz: fast enough that a shape change (I-beam, resize, hand) feels
 * instantaneous without burning CPU rasterizing the cursor every frame. */
static const uint64_t kPollIntervalNanos = (uint64_t)(NSEC_PER_SEC / 12);

@interface CursorCapture ()
@property (nonatomic, strong, nullable) dispatch_queue_t queue;
@property (nonatomic, strong, nullable) dispatch_source_t timer;
@property (nonatomic, assign) uint64_t lastHash;   /* 0 == nothing sent yet */
@property (nonatomic, assign) int      lastSeed;    /* CGS cursor seed last read */
@property (nonatomic, assign) BOOL     haveSeed;
@end

@implementation CursorCapture

- (void)start {
    if (_timer) return;
    _lastHash = 0;
    _haveSeed = NO;

    /* Dedicated background serial queue — NOT the main queue, which never
     * drains in this daemon (see header note). */
    _queue = dispatch_queue_create("com.macos-rdp.cursor-capture",
                                    DISPATCH_QUEUE_SERIAL);

    __weak typeof(self) weak = self;
    dispatch_source_t t =
        dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
    dispatch_source_set_timer(t,
                              dispatch_time(DISPATCH_TIME_NOW, 0),  /* fire promptly */
                              kPollIntervalNanos,
                              kPollIntervalNanos / 4);              /* leeway */
    dispatch_source_set_event_handler(t, ^{ [weak poll]; });
    _timer = t;
    dispatch_resume(t);

    rdp_verbose("cursor capture started (~12 Hz, dispatch timer, CGS live cursor)");
}

- (void)stop {
    if (_timer) {
        dispatch_source_cancel(_timer);
        _timer = nil;
    }
    _queue = nil;
    rdp_verbose("cursor capture stopped");
}

/* FNV-1a over the bitmap bytes + dimensions + hotspot, to detect shape changes. */
static uint64_t fnv1a(const uint8_t *p, size_t n, uint64_t seed) {
    uint64_t h = seed ? seed : 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

- (void)poll {
    /* Cheap pre-check: the CGS seed bumps on every cursor change. If it hasn't
     * moved since our last read, the shape is identical — skip the full read. */
    int seed = CGSCurrentCursorSeed();
    if (_haveSeed && seed == _lastSeed) return;
    _lastSeed = seed;
    _haveSeed = YES;

    uint32_t w = 0, h = 0;
    uint16_t hotX = 0, hotY = 0;
    /* Owned malloc'd BGRA buffer; freed below after the handler runs. */
    uint8_t *bgra = [self copyLiveCursorBGRA:&w height:&h hotX:&hotX hotY:&hotY];
    if (!bgra || w == 0 || h == 0) { free(bgra); return; }

    uint64_t hash = fnv1a(bgra, (size_t)w * h * 4, 0);
    hash = fnv1a((const uint8_t *)&w, sizeof(w), hash);
    hash = fnv1a((const uint8_t *)&h, sizeof(h), hash);
    hash = fnv1a((const uint8_t *)&hotX, sizeof(hotX), hash);
    hash = fnv1a((const uint8_t *)&hotY, sizeof(hotY), hash);

    if (hash == _lastHash) { free(bgra); return; }
    _lastHash = hash;

    rdp_verbose("cursor shape changed: %ux%u hot=(%u,%u) — sending", w, h, hotX, hotY);
    if (self.handler) self.handler(bgra, w, h, hotX, hotY);
    free(bgra);
}

/* Read the live on-screen system cursor via the private CGS API and return it as
 * a freshly malloc'd 32-bit BGRA premultiplied, top-left-origin, tightly-packed
 * buffer (stride == w*4), scaled down to kMaxCursorDim. Returns NULL on failure,
 * in which case the caller may try the NSCursor fallback. Caller frees. */
- (uint8_t *)copyLiveCursorBGRA:(uint32_t *)outW height:(uint32_t *)outH
                           hotX:(uint16_t *)outHotX hotY:(uint16_t *)outHotY {
    CGSConnectionID cid = CGSMainConnectionID();
    if (cid == 0) return [self copyFallbackCursorBGRA:outW height:outH
                                                 hotX:outHotX hotY:outHotY];

    size_t dataSize = 0;
    if (CGSGetGlobalCursorDataSize(cid, &dataSize) != kCGErrorSuccess ||
        dataSize == 0 || dataSize > (64u * 1024u * 1024u)) {
        return [self copyFallbackCursorBGRA:outW height:outH
                                       hotX:outHotX hotY:outHotY];
    }

    uint8_t *raw = malloc(dataSize);
    if (!raw) return NULL;

    int    rdDataSize = 0, rowBytes = 0, depth = 0, comps = 0, bpc = 0;
    CGRect rect = CGRectZero;
    CGPoint hot = CGPointZero;
    CGError ge = CGSGetGlobalCursorData(cid, raw, &rdDataSize, &rowBytes,
                                        &rect, &hot, &depth, &comps, &bpc);
    if (ge != kCGErrorSuccess || rowBytes <= 0 || depth != 32 || bpc != 8 ||
        rect.size.width <= 0 || rect.size.height <= 0) {
        rdp_debug("CGSGetGlobalCursorData unusable (err=%d depth=%d bpc=%d %gx%g) "
                  "— falling back", ge, depth, bpc,
                  rect.size.width, rect.size.height);
        free(raw);
        return [self copyFallbackCursorBGRA:outW height:outH
                                       hotX:outHotX hotY:outHotY];
    }

    uint32_t srcW = (uint32_t)llround(rect.size.width);
    uint32_t srcH = (uint32_t)llround(rect.size.height);
    if (srcW == 0 || srcH == 0) { free(raw); return NULL; }

    /* Wrap the raw cursor bytes in a CGImage using its reported layout, then let
     * a known-format CGBitmapContext re-render it. This makes us agnostic to the
     * exact CGS byte order/alpha layout: the destination context defines BGRA
     * premultiplied, top-left origin, tightly packed — exactly what RDP wants. */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    if (!cs) { free(raw); return NULL; }

    CGDataProviderRef provider =
        CGDataProviderCreateWithData(NULL, raw, (size_t)rowBytes * srcH, NULL);
    if (!provider) { CGColorSpaceRelease(cs); free(raw); return NULL; }

    /* CGS global cursor data is 32-bit ARGB, premultiplied, big-endian component
     * order; PremultipliedFirst + ByteOrder32Big matches it. */
    CGImageRef src = CGImageCreate(
        srcW, srcH, 8, 32, (size_t)rowBytes, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Big,
        provider, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(cs);
    if (!src) { free(raw); return NULL; }

    uint8_t *out = [self renderCGImage:src
                              srcWidth:srcW srcHeight:srcH
                               hotSpot:hot
                                 width:outW height:outH
                                  hotX:outHotX hotY:outHotY];
    CGImageRelease(src);
    free(raw);   /* CGImage held its own ref via the provider's copied data */
    return out;
}

/* Render a CGImage into a freshly malloc'd 32-bit BGRA premultiplied,
 * top-left-origin, tightly-packed buffer, scaled down to kMaxCursorDim. The
 * destination context is top-left origin (flipped), so no manual row flip is
 * needed. Returns NULL on failure. Caller frees. */
- (uint8_t *)renderCGImage:(CGImageRef)src
                  srcWidth:(uint32_t)srcW srcHeight:(uint32_t)srcH
                   hotSpot:(CGPoint)hot
                     width:(uint32_t *)outW height:(uint32_t *)outH
                      hotX:(uint16_t *)outHotX hotY:(uint16_t *)outHotY {
    uint32_t w = srcW, h = srcH;
    double scale = 1.0;
    uint32_t maxDim = w > h ? w : h;
    if (maxDim > kMaxCursorDim) {
        scale = (double)kMaxCursorDim / (double)maxDim;
        w = (uint32_t)llround(srcW * scale);
        h = (uint32_t)llround(srcH * scale);
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
     * layout we want. */
    CGContextRef cgctx = CGBitmapContextCreate(
        buf, w, h, 8, stride, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!cgctx) { free(buf); return NULL; }

    /* CG draws bottom-up by default. Flip the context vertically so the image
     * lands top-left-origin in `buf` (RDP/our convention). */
    CGContextTranslateCTM(cgctx, 0, h);
    CGContextScaleCTM(cgctx, 1.0, -1.0);
    CGContextSetInterpolationQuality(cgctx, kCGInterpolationHigh);
    CGContextDrawImage(cgctx, CGRectMake(0, 0, w, h), src);
    CGContextRelease(cgctx);

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

/* Last-resort fallback when the CGS cursor read is unavailable. NSCursor is
 * AppKit/main-thread-affine and unreliable headless, but reading its `.image`
 * (a plain NSImage) and rasterizing via CoreGraphics does not require a run
 * loop, so it is safe to attempt from our background queue. Logged so the
 * limitation is visible in a hardware test. */
- (uint8_t *)copyFallbackCursorBGRA:(uint32_t *)outW height:(uint32_t *)outH
                               hotX:(uint16_t *)outHotX hotY:(uint16_t *)outHotY {
    NSCursor *cursor = [NSCursor currentSystemCursor];
    if (!cursor) cursor = [NSCursor currentCursor];
    NSImage *image = cursor.image;
    if (!image) return NULL;

    CGImageRef cg = [image CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) return NULL;
    uint32_t srcW = (uint32_t)CGImageGetWidth(cg);
    uint32_t srcH = (uint32_t)CGImageGetHeight(cg);
    if (srcW == 0 || srcH == 0) return NULL;

    NSPoint hs = cursor.hotSpot;
    /* hotSpot is in image points; CGImage may be at a higher pixel scale. */
    NSSize pt = image.size;
    CGPoint hot = CGPointMake(
        pt.width  > 0 ? hs.x * (srcW / pt.width)  : hs.x,
        pt.height > 0 ? hs.y * (srcH / pt.height) : hs.y);

    rdp_debug("cursor read via NSCursor fallback (%ux%u) — CGS unavailable", srcW, srcH);
    return [self renderCGImage:cg srcWidth:srcW srcHeight:srcH hotSpot:hot
                         width:outW height:outH hotX:outHotX hotY:outHotY];
}

@end
