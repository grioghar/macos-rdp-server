#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

@interface VirtualDisplay : NSObject

@property (nonatomic, readonly) CGDirectDisplayID displayID;
@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;
@property (nonatomic, readonly) BOOL isCreated;

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height hiDPI:(BOOL)hiDPI;
- (BOOL)create;
- (void)destroy;
- (void)setResolutionWidth:(uint32_t)width height:(uint32_t)height;

@end

NS_ASSUME_NONNULL_END
