#include "HIDDriver.h"
#include <DriverKit/DriverKit.h>
#include <HIDDriverKit/IOUserHIDDevice.h>

/* Combined keyboard + mouse HID report descriptor.
   Keyboard: 8-byte boot-protocol report (ID 1).
   Mouse:    4-byte relative report (ID 2). */
static const uint8_t kReportDescriptor[] = {
    /* Keyboard */
    0x05, 0x01,       /* Usage Page: Generic Desktop */
    0x09, 0x06,       /* Usage: Keyboard */
    0xA1, 0x01,       /* Collection: Application */
    0x85, 0x01,       /*   Report ID: 1 */
    0x05, 0x07,       /*   Usage Page: Keyboard/Keypad */
    0x19, 0xE0,       /*   Usage Min: 0xE0 (LCtrl) */
    0x29, 0xE7,       /*   Usage Max: 0xE7 (RGui) */
    0x15, 0x00,       /*   Logical Min: 0 */
    0x25, 0x01,       /*   Logical Max: 1 */
    0x75, 0x01,       /*   Report Size: 1 bit */
    0x95, 0x08,       /*   Report Count: 8 */
    0x81, 0x02,       /*   Input: Data, Variable, Absolute (modifier byte) */
    0x95, 0x01,       /*   Report Count: 1 */
    0x75, 0x08,       /*   Report Size: 8 bits */
    0x81, 0x03,       /*   Input: Constant (reserved byte) */
    0x95, 0x06,       /*   Report Count: 6 */
    0x75, 0x08,       /*   Report Size: 8 bits */
    0x15, 0x00,       /*   Logical Min: 0 */
    0x25, 0xFF,       /*   Logical Max: 255 */
    0x05, 0x07,       /*   Usage Page: Keyboard */
    0x19, 0x00,       /*   Usage Min: 0 */
    0x29, 0xFF,       /*   Usage Max: 255 */
    0x81, 0x00,       /*   Input: Data, Array (keycodes) */
    0xC0,             /* End Collection */

    /* Mouse */
    0x05, 0x01,       /* Usage Page: Generic Desktop */
    0x09, 0x02,       /* Usage: Mouse */
    0xA1, 0x01,       /* Collection: Application */
    0x85, 0x02,       /*   Report ID: 2 */
    0x09, 0x01,       /*   Usage: Pointer */
    0xA1, 0x00,       /*   Collection: Physical */
    0x05, 0x09,       /*     Usage Page: Buttons */
    0x19, 0x01,       /*     Usage Min: 1 */
    0x29, 0x03,       /*     Usage Max: 3 */
    0x15, 0x00,       /*     Logical Min: 0 */
    0x25, 0x01,       /*     Logical Max: 1 */
    0x95, 0x03,       /*     Report Count: 3 */
    0x75, 0x01,       /*     Report Size: 1 */
    0x81, 0x02,       /*     Input: Data, Variable, Absolute */
    0x95, 0x01,       /*     Report Count: 1 */
    0x75, 0x05,       /*     Report Size: 5 (padding) */
    0x81, 0x03,       /*     Input: Constant */
    0x05, 0x01,       /*     Usage Page: Generic Desktop */
    0x09, 0x30,       /*     Usage: X */
    0x09, 0x31,       /*     Usage: Y */
    0x09, 0x38,       /*     Usage: Wheel */
    0x15, 0x81,       /*     Logical Min: -127 */
    0x25, 0x7F,       /*     Logical Max: 127 */
    0x75, 0x08,       /*     Report Size: 8 bits */
    0x95, 0x03,       /*     Report Count: 3 */
    0x81, 0x06,       /*     Input: Data, Variable, Relative */
    0xC0,             /*   End Collection */
    0xC0,             /* End Collection */
};

struct RDPHIDDriverIVars {
    OSAction *completionAction;
};

bool RDPHIDDriver::init() {
    if (!super::init()) return false;
    ivars = IONewZero(RDPHIDDriverIVars, 1);
    return ivars != nullptr;
}

void RDPHIDDriver::free() {
    IOSafeDeleteNULL(ivars, RDPHIDDriverIVars, 1);
    super::free();
}

kern_return_t RDPHIDDriver::Start(IOService *provider) {
    kern_return_t ret = super::Start(provider);
    if (ret != kIOReturnSuccess) return ret;
    RegisterService();
    return kIOReturnSuccess;
}

kern_return_t RDPHIDDriver::Stop(IOService *provider) {
    return super::Stop(provider);
}

OSDictionary *RDPHIDDriver::newDeviceDescription() {
    OSDictionary *desc = OSDictionary::withCapacity(8);
    if (!desc) return nullptr;

    desc->setObject(kIOHIDTransportKey,        OSString::withCString("USB"));
    desc->setObject(kIOHIDVendorIDKey,         OSNumber::withNumber(0x05AC, 32)); /* Apple */
    desc->setObject(kIOHIDProductIDKey,        OSNumber::withNumber(0x029C, 32));
    desc->setObject(kIOHIDVersionNumberKey,    OSNumber::withNumber(0x0100, 32));
    desc->setObject(kIOHIDManufacturerKey,     OSString::withCString("macOS RDP"));
    desc->setObject(kIOHIDProductKey,          OSString::withCString("RDP Virtual HID"));
    desc->setObject(kIOHIDSerialNumberKey,     OSString::withCString("MACRDP1"));
    desc->setObject(kIOHIDCountryCodeKey,      OSNumber::withNumber(0, 32));
    return desc;
}

OSData *RDPHIDDriver::newReportDescriptor() {
    return OSData::withBytes(kReportDescriptor, sizeof(kReportDescriptor));
}

kern_return_t RDPHIDDriver::getReport(IOMemoryDescriptor *report,
                                       IOHIDReportType     reportType,
                                       IOOptionBits        options,
                                       uint32_t            completionTimeout,
                                       OSAction           *action) {
    (void)report; (void)reportType; (void)options;
    (void)completionTimeout; (void)action;
    return kIOReturnUnsupported;
}

kern_return_t RDPHIDDriver::injectKeyboardReport(uint8_t modifiers,
                                                   uint8_t keycode) {
    /* Boot-protocol keyboard report: ID(1) + modifiers + reserved + 6 keycodes */
    uint8_t report[9] = {0x01, modifiers, 0x00,
                         keycode, 0x00, 0x00, 0x00, 0x00, 0x00};
    return handleReport(mach_absolute_time(),
                        report, sizeof(report),
                        kIOHIDReportTypeInput, 0);
}

kern_return_t RDPHIDDriver::injectMouseReport(int8_t dx, int8_t dy,
                                               uint8_t buttons, int8_t wheel) {
    /* Mouse report: ID(2) + buttons + dx + dy + wheel */
    uint8_t report[5] = {0x02, buttons,
                         (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
    return handleReport(mach_absolute_time(),
                        report, sizeof(report),
                        kIOHIDReportTypeInput, 0);
}
