#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* Delivers the current system cursor as a 32-bit BGRA (premultiplied),
 * top-left-origin bitmap plus its hotspot, in surface pixels. The bytes are
 * only valid for the duration of the block — copy if you need them longer.
 * Rows are tightly packed: stride == width * 4. */
typedef void (^CursorCaptureBlock)(const uint8_t *bgra,
                                   uint32_t width, uint32_t height,
                                   uint16_t hotX, uint16_t hotY);

/* Polls the live on-screen system cursor on a background dispatch_source timer
 * (~12 Hz) and invokes `handler` only when the cursor shape changes (detected
 * via the CGS cursor seed + a content hash). Works headless: it does NOT use
 * NSTimer/the main run loop (this daemon has none) and reads the cursor via the
 * private CoreGraphics CGS API rather than main-thread-affine NSCursor.
 *
 * The handler is invoked on the capture's background serial queue. */
@interface CursorCapture : NSObject

@property (nonatomic, copy, nullable) CursorCaptureBlock handler;

/* Start polling at ~12 Hz. The first shape fires almost immediately. Safe to
 * call from any thread. */
- (void)start;
/* Stop polling and tear down the timer. Safe to call from any thread. */
- (void)stop;

@end

NS_ASSUME_NONNULL_END
