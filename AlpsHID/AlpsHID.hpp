/* Derived from VoodooI2CHID's Multitouch/PTP event drivers (alexandred/VoodooI2C)
 * and the Linux driver drivers/hid/hid-alps.c. */
#ifndef AlpsHIDEventDriver_hpp
#define AlpsHIDEventDriver_hpp

#define _IOKIT_HID_IOHIDEVENTDRIVER_H

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/hid/IOHIDInterface.h>
#include <kern/clock.h>

#include "IOKit/hid/IOHIDEvent.h"
#include "IOKit/hid/IOHIDEventService.h"
#include "IOKit/hid/IOHIDEventTypes.h"
#include <IOKit/hidsystem/IOHIDTypes.h>
#include "IOKit/hid/IOHIDPrivateKeys.h"
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hid/IOHIDDevice.h>

#include "../VoodooInput/VoodooInput/VoodooInputMultitouch/VoodooInputTransducer.h"
#include "../VoodooInput/VoodooInput/VoodooInputMultitouch/VoodooInputMessages.h"

#include "AlpsCore.hpp"

#define ALPS_VENDOR    0x044E
#define CIRQUE_VENDOR  0x0488

// Register addresses (hid-alps.c)
#define T4_ADDRESS_BASE        0xC2C0
#define PRM_SYS_CONFIG_1       (T4_ADDRESS_BASE + 0x0002)
#define T4_PRM_FEED_CONFIG_1   (T4_ADDRESS_BASE + 0x0004)
#define T4_PRM_FEED_CONFIG_4   (T4_ADDRESS_BASE + 0x001A)
#define T4_PRM_ID_CONFIG_3     (T4_ADDRESS_BASE + 0x00B0)
#define T4_FEEDCFG4_ADVANCED_ABS_ENABLE 0x01
#define T4_I2C_ABS             0x78
#define T4_COUNT_PER_ELECTRODE 256
#define T4_INPUT_REPORT_ID     0x09
#define HID_PRODUCT_ID_T4_BTNLESS 0x120C

#define U1_ABSOLUTE_REPORT_ID       0x03
#define U1_ABSOLUTE_REPORT_ID_SECD  0x02
#define U1_SP_ABSOLUTE_REPORT_ID    0x06
#define U1_DEVTYPE_SP_SUPPORT       0x10
#define U1_DISABLE_DEV              0x01
#define U1_TP_ABS_MODE              0x02
#define U1_SP_ABS_MODE              0x80
#define ADDRESS_U1_DEV_CTRL_1       0x00800040
#define ADDRESS_U1_DEVICE_TYP       0x00800043
#define ADDRESS_U1_NUM_SENS_X       0x00800047
#define ADDRESS_U1_NUM_SENS_Y       0x00800048
#define ADDRESS_U1_PITCH_SENS_X     0x00800049
#define ADDRESS_U1_PITCH_SENS_Y     0x0080004A
#define ADDRESS_U1_RESO_DWN_ABS     0x0080004E

// Messages sent by ApplePS2Keyboard / VoodooPS2
enum {
    kKeyboardKeyPressTime = iokit_vendor_specific_msg(110)  // data: uint64_t* (ns)
};

class AlpsHIDEventDriver : public IOHIDEventService {
    OSDeclareDefaultStructors(AlpsHIDEventDriver);

public:
    bool init(OSDictionary* properties) override;
    bool handleStart(IOService* provider) override;
    void handleStop(IOService* provider) override;
    bool handleOpen(IOService* forClient, IOOptionBits options, void* arg) override;
    void handleClose(IOService* forClient, IOOptionBits options) override;
    IOReturn message(UInt32 type, IOService* provider, void* argument) override;
    bool didTerminate(IOService* provider, IOOptionBits options, bool* defer) override;
    IOReturn setPowerState(unsigned long whichState, IOService* whatDevice) override;

    void handleInterruptReport(AbsoluteTime timestamp, IOMemoryDescriptor* report,
                               IOHIDReportType report_type, UInt32 report_id);

private:
    bool deviceInit();
    bool t4Init();
    bool u1Init();
    IOReturn xfer(UInt8 reportID, UInt8* buf, size_t len, bool readBack);
    bool t4Read(UInt32 addr, UInt8* val);
    IOReturn t4Write(UInt32 addr, UInt8 val);
    bool u1Read(UInt32 addr, UInt8* val);
    IOReturn u1Write(UInt32 addr, UInt8 val);

    void sendTouch(AbsoluteTime ts, const alps::Frame& f);
    void sendPointer(AbsoluteTime ts, int dx, int dy, int buttons);

    IOHIDInterface* hid_interface {nullptr};
    IOService* voodooInputInstance {nullptr};
    char name[64] {};

    bool isT4 {false};
    volatile bool ready {false};
    volatile bool resetPending {false};
    bool awake {true};
    bool pmStarted {false};

    uint64_t max_after_typing {500000000};
    uint64_t key_time {0};

    UInt32 t4YFlip {0};
    bool pressureEnabled {true};
    bool wasButton {false};
    int lastPtrButtons {0};

    alps::Tracker tracker;
    alps::SubPixel stickAccum;
    VoodooInputEvent inputMessage;
    RelativePointerEvent spInputMessage;
};

#endif
