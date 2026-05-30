#pragma once
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <CoreMedia/CoreMedia.h>

NS_ASSUME_NONNULL_BEGIN

/* Delivers an encoded H.264 frame plus the damage rectangle (surface pixels)
 * that this frame updates. The dirty rect is carried per-frame through the
 * encoder so the GFX layer can signal the correct AVC420 region. */
typedef void (^FrameEncoderOutputBlock)(const uint8_t *data, size_t len,
                                         BOOL isKeyFrame,
                                         uint16_t dirtyX, uint16_t dirtyY,
                                         uint16_t dirtyW, uint16_t dirtyH);

@interface FrameEncoder : NSObject

@property (nonatomic, copy, nullable) FrameEncoderOutputBlock outputHandler;
@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                      bitrate:(uint32_t)bitrateKbps;
- (BOOL)start;
- (void)encodeFrame:(IOSurfaceRef)surface
             dirtyX:(uint16_t)dirtyX dirtyY:(uint16_t)dirtyY
             dirtyW:(uint16_t)dirtyW dirtyH:(uint16_t)dirtyH;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
