/* Derived from VoodooI2CHID's Multitouch/PTP event drivers (alexandred/VoodooI2C)
 * and the Linux driver drivers/hid/hid-alps.c. */
#include "AlpsHID.hpp"

#define super IOHIDEventService
OSDefineMetaClassAndStructors(AlpsHIDEventDriver, IOHIDEventService);

#define ELOG(fmt, ...) IOLog("%s::%s " fmt, getName(), name, ##__VA_ARGS__)
#if DEBUG
#define DLOG(fmt, ...) ELOG(fmt, ##__VA_ARGS__)
#else
#define DLOG(...) do {} while (0)
#endif

namespace {
struct Dev { UInt16 pid; bool t4; };
const Dev kDevices[] = {
    {0x1216, true}, {0x1217, true}, {0x120F, true}, {0x1218, true}, {0x120D, true}, {0x120C, true},   // T4
    {0x1209, false}, {0x1215, false}, {0x121E, false}, {0x120B, false}, {0x121F, false},              // U1
    {0x1207, false}, {0x120A, false}, {0x1220, false}, {0x1222, false},
};
IOPMPowerState kPowerStates[2] = {
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
};
}

bool AlpsHIDEventDriver::init(OSDictionary* properties) {
    if (!super::init(properties)) return false;
    bzero(&inputMessage, sizeof(inputMessage));
    bzero(&spInputMessage, sizeof(spInputMessage));
    return true;
}

// ---------------------------------------------------------------- start/stop
bool AlpsHIDEventDriver::handleStart(IOService* provider) {
    hid_interface = OSDynamicCast(IOHIDInterface, provider);
    if (!hid_interface) return false;

    UInt32 vid = hid_interface->getVendorID();
    UInt32 pid = hid_interface->getProductID();
    if (vid != ALPS_VENDOR && vid != CIRQUE_VENDOR) { hid_interface = nullptr; return false; }

    bool found = false;
    for (const Dev& d : kDevices) if (d.pid == pid) { isT4 = d.t4; found = true; break; }
    if (!found) { hid_interface = nullptr; return false; }

    OSString* product = hid_interface->getProduct();   // OSString*, not char*
    strlcpy(name, product ? product->getCStringNoCopy() : "AlpsHID", sizeof(name));

    // Tunables (Info.plist personality)
    if (OSNumber* n = OSDynamicCast(OSNumber, getProperty("QuietTimeAfterTyping")))
        max_after_typing = n->unsigned64BitValue() * 1000000;
    if (OSBoolean* b = OSDynamicCast(OSBoolean, getProperty("PressureSupport")))
        pressureEnabled = b->isTrue();
    if (OSNumber* n = OSDynamicCast(OSNumber, getProperty("ForceTouchThreshold"))) {
        UInt32 t = n->unsigned32BitValue();
        if (t > 127) t = 127;
        tracker.forceOn = (uint8_t)t;
        tracker.forceOff = (uint8_t)(t > 15 ? t - 15 : 0);
    }
    tracker.forceEnabled = pressureEnabled && !isT4;   // T4 reports no pressure

    if (!hid_interface->open(this, 0, OSMemberFunctionCast(IOHIDInterface::InterruptReportAction,
                             this, &AlpsHIDEventDriver::handleInterruptReport), nullptr)) {
        hid_interface = nullptr;
        return false;
    }

    if (!deviceInit()) {
        ELOG("device init failed, not attaching\n");
        hid_interface->close(this);
        hid_interface = nullptr;
        return false;
    }

    PMinit();
    registerPowerDriver(this, kPowerStates, 2);
    hid_interface->joinPMtree(this);
    pmStarted = true;

    setProperty("IOFBTransform", 0ull, 32);
    setProperty("VoodooInputSupported", kOSBooleanTrue);
    setProperty("VoodooI2CServices Supported", kOSBooleanTrue);
    return true;
}

void AlpsHIDEventDriver::handleStop(IOService* provider) {
    ready = false;
    if (pmStarted) { PMstop(); pmStarted = false; }
    super::handleStop(provider);
}

bool AlpsHIDEventDriver::didTerminate(IOService* provider, IOOptionBits options, bool* defer) {
    ready = false;
    if (hid_interface) hid_interface->close(this);
    hid_interface = nullptr;
    return super::didTerminate(provider, options, defer);
}

bool AlpsHIDEventDriver::handleOpen(IOService* forClient, IOOptionBits options, void* arg) {
    if (forClient && forClient->getProperty(VOODOO_INPUT_IDENTIFIER)) {
        if (!voodooInputInstance) { voodooInputInstance = forClient; voodooInputInstance->retain(); }
        return true;
    }
    return super::handleOpen(forClient, options, arg);
}

void AlpsHIDEventDriver::handleClose(IOService* forClient, IOOptionBits options) {
    if (forClient && forClient == voodooInputInstance) { OSSafeReleaseNULL(voodooInputInstance); return; }
    super::handleClose(forClient, options);
}

IOReturn AlpsHIDEventDriver::message(UInt32 type, IOService* provider, void* argument) {
    if (type == kKeyboardKeyPressTime && argument) {
        key_time = *((uint64_t*)argument);
        return kIOReturnSuccess;
    }
    return super::message(type, provider, argument);
}

IOReturn AlpsHIDEventDriver::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this) return kIOReturnInvalid;
    if (!whichState) {
        awake = false;
        ready = false;
    } else if (!awake) {
        IOSleep(10);
        if (hid_interface) {
            DLOG("awake, re-entering absolute mode\n");
            deviceInit();
            resetPending = true;
        }
        awake = true;
    }
    return kIOPMAckImplied;
}

// ------------------------------------------------------ feature report I/O
IOReturn AlpsHIDEventDriver::xfer(UInt8 reportID, UInt8* buf, size_t len, bool readBack) {
    if (!hid_interface) return kIOReturnNotAttached;
    IOBufferMemoryDescriptor* d = IOBufferMemoryDescriptor::withBytes(buf, len, kIODirectionInOut);
    if (!d) return kIOReturnNoMemory;
    IOReturn r = hid_interface->setReport(d, kIOHIDReportTypeFeature, reportID);
    if (readBack) {   // a failed write is tolerated; the reply is validated by the caller
        r = hid_interface->getReport(d, kIOHIDReportTypeFeature, reportID);
        if (r == kIOReturnSuccess) d->readBytes(0, buf, len);
    }
    d->release();
    return r;
}

bool AlpsHIDEventDriver::t4Read(UInt32 addr, UInt8* val) {
    for (int attempt = 0; attempt < 3; attempt++) {
        UInt8 buf[alps::kT4ReportLen];
        alps::t4Request(buf, addr, true, 0);
        if (xfer(alps::kT4FeatureId, buf, sizeof(buf), true) == kIOReturnSuccess &&
            alps::t4Response(buf, addr, val))
            return true;
        IOSleep(5);
    }
    ELOG("T4 read of %x failed\n", (unsigned)addr);
    return false;
}

IOReturn AlpsHIDEventDriver::t4Write(UInt32 addr, UInt8 val) {
    UInt8 buf[alps::kT4ReportLen];
    alps::t4Request(buf, addr, false, val);
    return xfer(alps::kT4FeatureId, buf, sizeof(buf), false);
}

bool AlpsHIDEventDriver::u1Read(UInt32 addr, UInt8* val) {
    for (int attempt = 0; attempt < 3; attempt++) {
        UInt8 buf[alps::kU1FeatureLen];
        alps::u1Request(buf, addr, true, 0);
        if (xfer(alps::kU1FeatureId, buf, sizeof(buf), true) == kIOReturnSuccess) {
            *val = alps::u1Response(buf);
            return true;
        }
        IOSleep(5);
    }
    ELOG("U1 read of %x failed\n", (unsigned)addr);
    return false;
}

IOReturn AlpsHIDEventDriver::u1Write(UInt32 addr, UInt8 val) {
    UInt8 buf[alps::kU1FeatureLen];
    alps::u1Request(buf, addr, false, val);
    return xfer(alps::kU1FeatureId, buf, sizeof(buf), false);
}

// ------------------------------------------------------------ device init
bool AlpsHIDEventDriver::deviceInit() {
    ready = false;
    bool ok = isT4 ? t4Init() : u1Init();
    ready = ok;
    return ok;
}

bool AlpsHIDEventDriver::t4Init() {
    UInt8 tmp = 0, linesX = 20, linesY = 12;

    if (hid_interface->getProductID() == HID_PRODUCT_ID_T4_BTNLESS) {
        if (!t4Read(T4_PRM_ID_CONFIG_3, &tmp)) return false;
        linesX = 16 + ((tmp & 0x0F) | (tmp & 0x08 ? 0xF0 : 0));
        linesY = 12 + (((tmp & 0xF0) >> 4) | (tmp & 0x80 ? 0xF0 : 0));
        if (!t4Read(PRM_SYS_CONFIG_1, &tmp)) return false;
    }
    if (!linesX || !linesY) return false;

    UInt32 xMax = linesX * T4_COUNT_PER_ELECTRODE;
    UInt32 yMax = linesY * T4_COUNT_PER_ELECTRODE;
    // Y is mirrored; for the default 12-line pad this is exactly the original 3060 - y + 255.
    t4YFlip = yMax + T4_COUNT_PER_ELECTRODE - alps::kT4YEdgeTrim;

    tmp |= 0x02;
    if (t4Write(PRM_SYS_CONFIG_1, tmp) != kIOReturnSuccess) ELOG("PRM_SYS_CONFIG_1 write failed\n");
    if (t4Write(T4_PRM_FEED_CONFIG_1, T4_I2C_ABS) != kIOReturnSuccess) ELOG("FEED_CONFIG_1 write failed\n");
    if (t4Write(T4_PRM_FEED_CONFIG_4, T4_FEEDCFG4_ADVANCED_ABS_ENABLE) != kIOReturnSuccess)
        ELOG("FEED_CONFIG_4 write failed\n");

    setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, xMax, 32);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, yMax, 32);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_X_KEY, 10240, 32);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_Y_KEY, 6140, 32);
    return true;
}

bool AlpsHIDEventDriver::u1Init() {
    UInt8 devCtrl, linesX, linesY, pitchX, pitchY, resolution, devType;

    if (!u1Read(ADDRESS_U1_DEV_CTRL_1, &devCtrl)) return false;
    devCtrl &= ~U1_DISABLE_DEV;
    devCtrl |= U1_TP_ABS_MODE;
    if (u1Write(ADDRESS_U1_DEV_CTRL_1, devCtrl) != kIOReturnSuccess) ELOG("absolute-mode write failed\n");

    if (!u1Read(ADDRESS_U1_NUM_SENS_X, &linesX) || !u1Read(ADDRESS_U1_NUM_SENS_Y, &linesY) ||
        !u1Read(ADDRESS_U1_PITCH_SENS_X, &pitchX) || !u1Read(ADDRESS_U1_PITCH_SENS_Y, &pitchY) ||
        !u1Read(ADDRESS_U1_RESO_DWN_ABS, &resolution))
        return false;
    if (linesX < 2 || linesY < 2 || !resolution) { ELOG("implausible geometry\n"); return false; }

    UInt32 xMax = (UInt32(resolution) << 2) * (linesX - 1);
    UInt32 yMax = (UInt32(resolution) << 2) * (linesY - 1);
    UInt32 xLen = UInt32(pitchX) * (linesX - 1);
    UInt32 yLen = UInt32(pitchY) * (linesY - 1);

    if (u1Read(ADDRESS_U1_DEVICE_TYP, &devType) && (devType & U1_DEVTYPE_SP_SUPPORT)) {
        devCtrl |= U1_SP_ABS_MODE;
        u1Write(ADDRESS_U1_DEV_CTRL_1, devCtrl);
    }

    setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, xMax, 32);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, yMax, 32);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_X_KEY, xLen * 10, 32);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_Y_KEY, yLen * 10, 32);
    return true;
}

// -------------------------------------------------------------- input path
void AlpsHIDEventDriver::handleInterruptReport(AbsoluteTime ts, IOMemoryDescriptor* report,
                                               IOHIDReportType type, UInt32 id) {
    if (!ready || !report || !voodooInputInstance || type != kIOHIDReportTypeInput) return;

    bool isTouch, isStick = false;
    if (isT4) isTouch = (id == T4_INPUT_REPORT_ID);
    else {
        isTouch = (id == U1_ABSOLUTE_REPORT_ID || id == U1_ABSOLUTE_REPORT_ID_SECD);
        isStick = (id == U1_SP_ABSOLUTE_REPORT_ID);
    }
    if (!isTouch && !isStick) return;

    if (resetPending) {
        resetPending = false;
        tracker.reset(); stickAccum.reset(); wasButton = false; lastPtrButtons = 0;
    }

    // Ignore touchpad input shortly after typing; but release fingers that were already down.
    uint64_t nowAbs, nowNs;
    clock_get_uptime(&nowAbs);
    absolutetime_to_nanoseconds(nowAbs, &nowNs);
    bool quiet = (nowNs - key_time) < max_after_typing;
    if (quiet && !(isTouch && tracker.anyDown())) return;

    UInt8 buf[64];
    size_t len = report->getLength();
    if (len > sizeof(buf)) len = sizeof(buf);
    if (report->readBytes(0, buf, len) != len) return;

    if (isStick) {
        int dx, dy, btn;
        if (!alps::parseStick(buf, len, &dx, &dy, &btn)) return;
        int ox, oy;
        stickAccum.apply(dx, dy, 4, &ox, &oy);
        sendPointer(ts, ox, oy, btn);
        return;
    }

    alps::Frame f;
    if (!(isT4 ? alps::parseT4(buf, len, t4YFlip, f) : alps::parseU1(buf, len, f))) return;
    if (quiet) alps::clearFrame(f);

    sendTouch(ts, f);

    if (!isT4) {   // clickpad button doubles as a pointer button (original behaviour)
        int btn = f.rawButtons & 0x07;
        if (btn != lastPtrButtons) sendPointer(ts, 0, 0, btn);
    }
}

void AlpsHIDEventDriver::sendTouch(AbsoluteTime ts, const alps::Frame& f) {
    alps::Slot slots[alps::kMaxTouches];
    int n = tracker.update(f, slots);
    if (n == 0 && !f.button && !wasButton) return;   // nothing to say
    wasButton = f.button;

    bzero(&inputMessage, sizeof(inputMessage));
    inputMessage.contact_count = n;
    inputMessage.timestamp = ts;

    int thumb = -1;
    if (alps::activeCount(slots, n) >= 4 || f.button) thumb = alps::lowestFinger(slots, n);

    // transducers[0] carries the button state even if slot 0 itself is empty
    for (int i = 0; i < (n ? n : 1); i++) {
        VoodooInputTransducer* t = &inputMessage.transducers[i];
        t->isPhysicalButtonDown = f.button;
        if (i >= n) break;
        const alps::Slot& s = slots[i];
        t->timestamp = ts;
        t->type = VoodooInputTransducerType::FINGER;
        t->secondaryId = i;
        t->fingerType = (i == thumb) ? kMT2FingerTypeThumb
                                     : (MT2FingerType)(kMT2FingerTypeIndexFinger + (i % 4));
        t->isValid = s.valid;
        t->isTransducerActive = s.active;
        t->supportsPressure = pressureEnabled && !isT4;
        t->maxPressure = 255;
        t->currentCoordinates.x = s.x;
        t->currentCoordinates.y = s.y;
        t->currentCoordinates.pressure = alps::pressureFor(s);
        t->currentCoordinates.width = alps::widthFor(s);
        t->previousCoordinates = t->currentCoordinates;
    }
    messageClient(kIOMessageVoodooInputMessage, voodooInputInstance, &inputMessage, sizeof(inputMessage));
}

void AlpsHIDEventDriver::sendPointer(AbsoluteTime ts, int dx, int dy, int buttons) {
    lastPtrButtons = buttons;
    spInputMessage.timestamp = ts;
    spInputMessage.dx = dx;
    spInputMessage.dy = dy;
    spInputMessage.buttons = buttons;
    messageClient(kIOMessageVoodooTrackpointRelativePointer, voodooInputInstance,
                  &spInputMessage, sizeof(spInputMessage));
}
