#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/* RDP scan code flags (from RDP spec §2.2.8.1.1.3.1) */
#define RDP_KEYRELEASE    0x8000
#define RDP_KEY_EXTENDED  0x0100

/* RDP mouse flags */
#define RDP_MOUSE_LDOWN   0x1000
#define RDP_MOUSE_LUP     0x0800
#define RDP_MOUSE_RDOWN   0x4000
#define RDP_MOUSE_RUP     0x2000
#define RDP_MOUSE_MDOWN   0x0020
#define RDP_MOUSE_MUP     0x0010
#define RDP_MOUSE_MOVE    0x0800
#define RDP_MOUSE_WHEEL   0x0200
#define RDP_MOUSE_HWHEEL  0x0400

@interface InputInjector : NSObject

- (instancetype)initWithDisplayID:(CGDirectDisplayID)displayID;

- (void)injectKeyEvent:(uint16_t)flags scanCode:(uint16_t)code;
- (void)injectMouseEvent:(uint16_t)flags x:(uint16_t)x y:(uint16_t)y;
- (void)injectMouseWheelEvent:(uint16_t)flags delta:(int16_t)delta
                            x:(uint16_t)x y:(uint16_t)y;

@end

NS_ASSUME_NONNULL_END
