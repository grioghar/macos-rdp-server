#import "input/InputInjector.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <syslog.h>
#define RDP_LOG_COMPONENT "input"
#include "logging/RDPLog.h"

/* RDP scan code → macOS virtual key code mapping (subset, covering common keys).
   Full table follows USB HID Usage Tables §10 and Mac OS X keycodes. */
static const uint16_t kScanToVK[256] = {
    [0x01] = 53,  /* Esc */
    [0x02] = 18,  /* 1 */  [0x03] = 19,  /* 2 */  [0x04] = 20,  /* 3 */
    [0x05] = 21,  /* 4 */  [0x06] = 23,  /* 5 */  [0x07] = 22,  /* 6 */
    [0x08] = 26,  /* 7 */  [0x09] = 28,  /* 8 */  [0x0A] = 25,  /* 9 */
    [0x0B] = 29,  /* 0 */  [0x0C] = 27,  /* - */  [0x0D] = 24,  /* = */
    [0x0E] = 51,  /* BS */
    [0x0F] = 48,  /* Tab */
    [0x10] = 12,  /* Q */  [0x11] = 13,  /* W */  [0x12] = 14,  /* E */
    [0x13] = 15,  /* R */  [0x14] = 17,  /* T */  [0x15] = 16,  /* Y */
    [0x16] = 32,  /* U */  [0x17] = 34,  /* I */  [0x18] = 31,  /* O */
    [0x19] = 35,  /* P */  [0x1A] = 33,  /* [ */  [0x1B] = 30,  /* ] */
    [0x1C] = 36,  /* Return */
    [0x1D] = 59,  /* LCtrl */
    [0x1E] = 0,   /* A */  [0x1F] = 1,   /* S */  [0x20] = 2,   /* D */
    [0x21] = 3,   /* F */  [0x22] = 5,   /* G */  [0x23] = 4,   /* H */
    [0x24] = 38,  /* J */  [0x25] = 40,  /* K */  [0x26] = 37,  /* L */
    [0x27] = 41,  /* ; */  [0x28] = 39,  /* ' */  [0x29] = 50,  /* ` */
    [0x2A] = 56,  /* LShift */
    [0x2B] = 42,  /* \ */
    [0x2C] = 6,   /* Z */  [0x2D] = 7,   /* X */  [0x2E] = 8,   /* C */
    [0x2F] = 9,   /* V */  [0x30] = 11,  /* B */  [0x31] = 45,  /* N */
    [0x32] = 46,  /* M */  [0x33] = 43,  /* , */  [0x34] = 47,  /* . */
    [0x35] = 44,  /* / */
    [0x36] = 60,  /* RShift */
    [0x37] = 67,  /* KP* */
    [0x38] = 58,  /* LAlt */
    [0x39] = 49,  /* Space */
    [0x3A] = 57,  /* CapsLk */
    [0x3B] = 122, /* F1 */  [0x3C] = 120, /* F2 */  [0x3D] = 99, /* F3 */
    [0x3E] = 118, /* F4 */  [0x3F] = 96,  /* F5 */  [0x40] = 97, /* F6 */
    [0x41] = 98,  /* F7 */  [0x42] = 100, /* F8 */  [0x43] = 101,/* F9 */
    [0x44] = 109, /* F10 */ [0x57] = 103, /* F11 */ [0x58] = 111,/* F12 */
    [0x47] = 71,  /* KP7/Home */
    [0x48] = 126, /* Up */
    [0x4B] = 123, /* Left */
    [0x4D] = 124, /* Right */
    [0x50] = 125, /* Down */
    [0x52] = 114, /* Ins */
    [0x53] = 117, /* Del */
    [0x4F] = 119, /* End */
    [0x49] = 116, /* PgUp */
    [0x51] = 121, /* PgDn */
};

/* Extended scan codes (prefixed 0xE0 in RDP) → macOS VK */
static const uint16_t kExtScanToVK[256] = {
    [0x1C] = 76,  /* KP Enter */
    [0x1D] = 62,  /* RCtrl */
    [0x35] = 75,  /* KP/ */
    [0x38] = 61,  /* RAlt */
    [0x47] = 115, /* Home */
    [0x48] = 126, /* Up */
    [0x49] = 116, /* PgUp */
    [0x4B] = 123, /* Left */
    [0x4D] = 124, /* Right */
    [0x4F] = 119, /* End */
    [0x50] = 125, /* Down */
    [0x51] = 121, /* PgDn */
    [0x52] = 114, /* Insert */
    [0x53] = 117, /* Delete */
    [0x5B] = 55,  /* LCmd */
    [0x5C] = 55,  /* RCmd */
};

@interface InputInjector ()
@property (nonatomic, assign) CGDirectDisplayID displayID;
/* Button state — needed to emit MouseDragged (not MouseMoved) while a button
 * is held, otherwise drags / text selection / window moves don't work. */
@property (nonatomic, assign) BOOL leftDown;
@property (nonatomic, assign) BOOL rightDown;
@property (nonatomic, assign) BOOL middleDown;
@end

@implementation InputInjector

- (instancetype)initWithDisplayID:(CGDirectDisplayID)did {
    if ((self = [super init])) {
        _displayID = did;
        /* Prompt for Accessibility (required for CGEventPost) and register the
         * binary in the Privacy list. In the GUI session this pops the system
         * dialog the first time; thereafter it just reports the trust state. */
        NSDictionary *opts = @{ (__bridge id)kAXTrustedCheckOptionPrompt: @YES };
        if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts))
            rdp_error("Accessibility not granted — input injection will not work "
                      "until enabled (grant the prompt or System Settings > Privacy "
                      "& Security > Accessibility)");
    }
    return self;
}

- (CGKeyCode)keyCodeForScanCode:(uint16_t)code extended:(BOOL)ext {
    const uint16_t *table = ext ? kExtScanToVK : kScanToVK;
    if (code >= 256) return 0xFFFF;
    uint16_t vk = table[code];
    return (CGKeyCode)vk;
}

- (void)injectKeyEvent:(uint16_t)flags scanCode:(uint16_t)code {
    BOOL isRelease = (flags & RDP_KBD_RELEASE) != 0;
    BOOL isExtended = (flags & RDP_KBD_EXTENDED) != 0;
    CGKeyCode vk = [self keyCodeForScanCode:code extended:isExtended];
    if (vk == 0xFFFF) return;

    CGEventRef event = CGEventCreateKeyboardEvent(NULL, vk, !isRelease);
    if (!event) return;

    /* Post to the session event stream so it reaches the frontmost app. */
    CGEventPost(kCGSessionEventTap, event);
    CFRelease(event);
}

- (void)injectMouseEvent:(uint16_t)flags x:(uint16_t)x y:(uint16_t)y {
    CGPoint pos = CGPointMake(x, y);

    /* Map virtual display coordinates to global screen coordinates. */
    CGRect bounds = CGDisplayBounds(_displayID);
    pos.x += bounds.origin.x;
    pos.y += bounds.origin.y;

    BOOL down = (flags & RDP_PTR_DOWN) != 0;
    CGEventType type;
    CGMouseButton button;

    if (flags & RDP_PTR_BUTTON1) {
        button = kCGMouseButtonLeft;
        type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        _leftDown = down;
    } else if (flags & RDP_PTR_BUTTON2) {
        button = kCGMouseButtonRight;
        type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        _rightDown = down;
    } else if (flags & RDP_PTR_BUTTON3) {
        button = kCGMouseButtonCenter;
        type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
        _middleDown = down;
    } else if (flags & RDP_PTR_MOVE) {
        /* A move with a button held is a drag — macOS needs the dragged event
         * type or the gesture (selection, window move) is dropped. */
        if (_leftDown)        { type = kCGEventLeftMouseDragged;  button = kCGMouseButtonLeft; }
        else if (_rightDown)  { type = kCGEventRightMouseDragged; button = kCGMouseButtonRight; }
        else if (_middleDown) { type = kCGEventOtherMouseDragged; button = kCGMouseButtonCenter; }
        else                  { type = kCGEventMouseMoved;        button = kCGMouseButtonLeft; }
    } else {
        return; /* nothing actionable */
    }

    CGEventRef event = CGEventCreateMouseEvent(NULL, type, pos, button);
    if (!event) return;
    CGEventPost(kCGSessionEventTap, event);
    CFRelease(event);
}

- (void)injectMouseWheelEvent:(uint16_t)flags x:(uint16_t)x y:(uint16_t)y {
    (void)x; (void)y;
    BOOL horizontal = (flags & RDP_PTR_HWHEEL) != 0;

    /* Rotation magnitude is the low 8 bits; PTR_FLAGS_WHEEL_NEGATIVE flips sign.
     * Windows sends multiples of WHEEL_DELTA (120) per notch. */
    int32_t rotation = flags & 0xFF;
    if (flags & RDP_PTR_WHEEL_NEGATIVE) rotation = -rotation;

    int32_t lines = rotation / 120;
    if (lines == 0) lines = (rotation > 0) ? 1 : (rotation < 0 ? -1 : 0);
    if (lines == 0) return;

    /* macOS scroll sign is opposite RDP for vertical (natural direction). */
    CGEventRef event = CGEventCreateScrollWheelEvent(
        NULL, kCGScrollEventUnitLine,
        horizontal ? 2 : 1,
        horizontal ? 0 : lines,
        horizontal ? lines : 0
    );
    if (!event) return;
    CGEventPost(kCGSessionEventTap, event);
    CFRelease(event);
}

@end
