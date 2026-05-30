#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * VirtualDisplay: manages which CGDirectDisplayID the session captures.
 *
 * Without the com.apple.developer.virtual-display entitlement (which requires
 * an explicit Apple approval), CGVirtualDisplay symbols are not exported from
 * the SDK. We default to CGMainDisplayID() and add the virtual display path
 * behind MACOS_RDP_VIRTUAL_DISPLAY so it compiles only when the entitlement
 * is present and the binary is properly signed.
 */
@interface VirtualDisplay : NSObject

@property (nonatomic, readonly) CGDirectDisplayID displayID;
@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height;
- (BOOL)create;
- (void)destroy;
- (void)setResolutionWidth:(uint32_t)width height:(uint32_t)height;

@end

NS_ASSUME_NONNULL_END
