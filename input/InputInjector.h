#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/* RDP keyboard flags (MS-RDPBCGR 2.2.8.1.1.3.1.1.1) */
#define RDP_KBD_RELEASE   0x8000   /* KBDFLAGS_RELEASE */
#define RDP_KBD_EXTENDED  0x0100   /* KBDFLAGS_EXTENDED */

/* RDP pointer flags (MS-RDPBCGR 2.2.8.1.1.3.1.1.3). Button press vs release is
 * the DOWN bit — NOT separate values; identity is the BUTTONn bit. */
#define RDP_PTR_MOVE              0x0800
#define RDP_PTR_DOWN              0x8000
#define RDP_PTR_BUTTON1           0x1000   /* left   */
#define RDP_PTR_BUTTON2           0x2000   /* right  */
#define RDP_PTR_BUTTON3           0x4000   /* middle */
#define RDP_PTR_WHEEL             0x0200
#define RDP_PTR_HWHEEL            0x0400
#define RDP_PTR_WHEEL_NEGATIVE    0x0100
#define RDP_PTR_WHEEL_ROTATION    0x01FF

@interface InputInjector : NSObject

- (instancetype)initWithDisplayID:(CGDirectDisplayID)displayID;

- (void)injectKeyEvent:(uint16_t)flags scanCode:(uint16_t)code;
- (void)injectMouseEvent:(uint16_t)flags x:(uint16_t)x y:(uint16_t)y;
/* Wheel rotation is encoded entirely within flags; decoded internally. */
- (void)injectMouseWheelEvent:(uint16_t)flags x:(uint16_t)x y:(uint16_t)y;

@end

NS_ASSUME_NONNULL_END
