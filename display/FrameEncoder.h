#pragma once
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <CoreMedia/CoreMedia.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^FrameEncoderOutputBlock)(const uint8_t *data, size_t len,
                                         BOOL isKeyFrame);

@interface FrameEncoder : NSObject

@property (nonatomic, copy, nullable) FrameEncoderOutputBlock outputHandler;
@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                      bitrate:(uint32_t)bitrateKbps;
- (BOOL)start;
- (void)encodeFrame:(IOSurfaceRef)surface;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
