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

/* Polls NSCursor.currentSystemCursor on a timer and invokes `handler` only
 * when the cursor shape changes (detected via a content hash). The handler is
 * always called on the main queue (NSCursor APIs are main-thread-affine). */
@interface CursorCapture : NSObject

@property (nonatomic, copy, nullable) CursorCaptureBlock handler;

/* Start polling at ~12 Hz. The first shape fires almost immediately. */
- (void)start;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
