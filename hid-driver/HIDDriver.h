#pragma once
#include <DriverKit/DriverKit.h>
#include <HIDDriverKit/IOUserHIDDevice.h>

/* Virtual HID device that the macos-rdp-daemon feeds input reports to.
   The driver activates as a System Extension; the daemon communicates
   via a custom IOUserClient (RDPHIDUserClient). */

class RDPHIDDriver : public IOUserHIDDevice {
    OSDeclareDefaultStructors(RDPHIDDriver)

public:
    virtual bool init() override;
    virtual kern_return_t Start(IOService *provider) override;
    virtual kern_return_t Stop(IOService *provider) override;
    virtual void free() override;

    /* IOUserHIDDevice overrides */
    virtual OSDictionary *newDeviceDescription() override;
    virtual OSData       *newReportDescriptor() override;
    virtual kern_return_t getReport(IOMemoryDescriptor *report,
                                     IOHIDReportType     reportType,
                                     IOOptionBits        options,
                                     uint32_t            completionTimeout,
                                     OSAction           *action) override;

    /* Called by RDPHIDUserClient to inject keyboard/mouse reports. */
    kern_return_t injectKeyboardReport(uint8_t modifiers, uint8_t keycode);
    kern_return_t injectMouseReport(int8_t dx, int8_t dy,
                                     uint8_t buttons, int8_t wheel);

private:
    struct RDPHIDDriverIVars *ivars;
};
