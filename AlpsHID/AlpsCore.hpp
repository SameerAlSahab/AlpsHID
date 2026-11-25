// AlpsCore.hpp - hardware-independent logic for AlpsHID (no IOKit, no std lib).
// Kept separate so it can be unit tested on any host: see Tests/core_test.cpp.
// Protocol details derived from the Linux driver drivers/hid/hid-alps.c.
#ifndef AlpsCore_hpp
#define AlpsCore_hpp

#include <stdint.h>
#include <stddef.h>

namespace alps {

constexpr int kMaxTouches = 5;

// ---- little-endian helpers (no unaligned casts) ----------------------------
inline uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }
inline void putLe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// ---- decoded input ---------------------------------------------------------
struct Contact {
    bool     active;
    uint32_t x, y;
    uint8_t  z;        // raw strength 0..127 (U1) or a nominal constant (T4)
};

struct Frame {
    Contact c[kMaxTouches];
    bool    button;       // physical clickpad button
    uint8_t rawButtons;   // U1 button bits (also forwarded as pointer buttons)
};

inline void clearFrame(Frame& f) { f = Frame(); }

// T4: id, count, 5 x {palm,xlo,xhi,ylo,yhi}, button, ... (51 bytes total)
constexpr size_t   kT4ReportLen = 51;
constexpr uint8_t  kT4NominalZ  = 62;   // T4 reports no per-finger pressure
constexpr uint32_t kT4YEdgeTrim = 13;   // empirical calibration from the original driver

// yFlip: raw Y is mirrored as (yFlip - y); see AlpsHID.cpp for how it is derived.
inline bool parseT4(const uint8_t* in, size_t len, uint32_t yFlip, Frame& f) {
    if (len < 28) return false;
    uint8_t b[kT4ReportLen] = {};
    for (size_t i = 0; i < len && i < kT4ReportLen; i++) b[i] = in[i];
    clearFrame(f);
    for (int i = 0; i < kMaxTouches; i++) {
        const uint8_t* c = b + 2 + i * 5;
        uint8_t palm = c[0];
        // palm == 0: no contact; 0x80 bit: palm contact -> ignore just this contact
        f.c[i].active = palm > 0 && palm < 0x80;
        f.c[i].x = (uint32_t)(c[2] << 8 | c[1]);
        uint32_t y = (uint32_t)(c[4] << 8 | c[3]);
        f.c[i].y = y > yFlip ? 0 : yFlip - y;
        f.c[i].z = kT4NominalZ;
    }
    f.button = b[27] != 0;
    f.rawButtons = b[27];
    return true;
}

// U1: id, buttons, count, 5 x {xlo,xhi,ylo,yhi,z}  (28 bytes). Short reports are
// zero-extended, so missing contacts read as "not touching" instead of stack garbage.
constexpr size_t kU1ReportLen = 28;

inline bool parseU1(const uint8_t* in, size_t len, Frame& f) {
    if (len < 3) return false;
    uint8_t b[kU1ReportLen] = {};
    for (size_t i = 0; i < len && i < kU1ReportLen; i++) b[i] = in[i];
    clearFrame(f);
    for (int i = 0; i < kMaxTouches; i++) {
        const uint8_t* c = b + 3 + i * 5;
        uint8_t z = c[4] & 0x7F;
        f.c[i].active = z != 0;
        f.c[i].x = (uint32_t)(c[1] << 8 | c[0]);
        f.c[i].y = (uint32_t)(c[3] << 8 | c[2]);
        f.c[i].z = z;
    }
    f.rawButtons = b[1];
    f.button = b[1] != 0;
    return true;
}

// Pointing stick: id, buttons, int16 x, int16 y, uint16 z
inline bool parseStick(const uint8_t* b, size_t len, int* dx, int* dy, int* buttons) {
    if (len < 6) return false;
    *buttons = b[1] & 0x07;
    *dx = (int16_t)le16(b + 2);
    *dy = (int16_t)le16(b + 4);
    return true;
}

// Carries the remainder of integer division so slow stick movement is not lost.
struct SubPixel {
    int rx = 0, ry = 0;
    void reset() { rx = ry = 0; }
    void apply(int dx, int dy, int div, int* ox, int* oy) {
        rx += dx; ry += dy;
        *ox = rx / div; *oy = ry / div;
        rx -= *ox * div; ry -= *oy * div;
    }
};

// ---- finger tracking -------------------------------------------------------
struct Slot {
    bool     valid;    // include in the VoodooInput event
    bool     active;   // finger currently down (valid && !active == lift frame)
    bool     force;    // force-touch engaged
    uint32_t x, y;
    uint8_t  z;
};

class Tracker {
public:
    bool    forceEnabled = false;
    uint8_t forceOn  = 105;   // engage at/above this raw z
    uint8_t forceOff = 100;   // release below this raw z (hysteresis)
    // A finger that moves more than kMoveLimit raw units in one frame is dragging, not pressing:
    // force is blocked for kLockFrames frames so a hard drag never becomes a force click.
    static constexpr uint32_t kMoveLimit = 30;
    static constexpr uint8_t  kLockFrames = 20;

    // Fills out[]; returns the VoodooInput contact_count (highest used slot + 1).
    // A finger that disappeared yields exactly one lift frame (valid, !active).
    int update(const Frame& f, Slot out[kMaxTouches]) {
        int n = 0;
        for (int i = 0; i < kMaxTouches; i++) {
            Slot& s = out[i];
            s = Slot();
            const Contact& c = f.c[i];
            if (c.active) {
                if (down_[i]) {
                    uint32_t mx = c.x > lx_[i] ? c.x - lx_[i] : lx_[i] - c.x;
                    uint32_t my = c.y > ly_[i] ? c.y - ly_[i] : ly_[i] - c.y;
                    if (mx + my > kMoveLimit) lock_[i] = kLockFrames;
                    else if (lock_[i]) lock_[i]--;
                }
                if (forceEnabled) {
                    if (f.button || lock_[i]) force_[i] = false;   // clicking or dragging: never force
                    else if (force_[i]) { if (c.z < forceOff) force_[i] = false; }
                    else if (c.z >= forceOn) force_[i] = true;
                }
                s.valid = s.active = true;
                s.force = force_[i];
                s.x = lx_[i] = c.x; s.y = ly_[i] = c.y; s.z = c.z;
                down_[i] = true;
                n = i + 1;
            } else if (down_[i]) {
                s.valid = true; s.active = false;
                s.x = lx_[i]; s.y = ly_[i];
                down_[i] = force_[i] = false;
                lock_[i] = 0;
                n = i + 1;
            }
        }
        return n;
    }

    bool anyDown() const {
        for (int i = 0; i < kMaxTouches; i++) if (down_[i]) return true;
        return false;
    }
    void reset() { for (int i = 0; i < kMaxTouches; i++) { down_[i] = force_[i] = false; lock_[i] = 0; } }

private:
    bool     down_[kMaxTouches]  = {};
    bool     force_[kMaxTouches] = {};
    uint8_t  lock_[kMaxTouches]  = {};
    uint32_t lx_[kMaxTouches]    = {};
    uint32_t ly_[kMaxTouches]    = {};
};

// Index of the lowest active finger (largest y) among slots, or -1.
inline int lowestFinger(const Slot* s, int n) {
    int idx = -1; uint32_t best = 0;
    for (int i = 0; i < n; i++)
        if (s[i].active && (idx < 0 || s[i].y >= best)) { best = s[i].y; idx = i; }
    return idx;
}
inline int activeCount(const Slot* s, int n) {
    int k = 0; for (int i = 0; i < n; i++) k += s[i].active; return k;
}

// Pressure data is only sent for a finger in a force press; every other finger uses
// VoodooInput's defaults (pressure 5, size 10, touch 20), exactly like the original driver.
constexpr uint8_t kForcePressure = 255;
constexpr uint8_t kForceWidth    = 20;

// ---- feature-report register access ---------------------------------------
// T4 uses a Fletcher-16 style checksum, folded every 20 bytes (as in Linux).
inline uint16_t t4Checksum(const uint8_t* buf, size_t offset, size_t length) {
    uint16_t sum1 = 0xFF, sum2 = 0xFF;
    size_t i = 0;
    if (offset + length >= 50) return 0;
    while (length > 0) {
        size_t tlen = length > 20 ? 20 : length;
        length -= tlen;
        do { sum1 += buf[offset + i]; sum2 += sum1; i++; } while (--tlen > 0);
        sum1 = (uint16_t)((sum1 & 0xFF) + (sum1 >> 8));
        sum2 = (uint16_t)((sum2 & 0xFF) + (sum2 >> 8));
    }
    sum1 = (uint16_t)((sum1 & 0xFF) + (sum1 >> 8));
    sum2 = (uint16_t)((sum2 & 0xFF) + (sum2 >> 8));
    return (uint16_t)(sum2 << 8 | sum1);
}

constexpr uint8_t kT4FeatureId = 0x07, kT4CmdRead = 0x08, kT4CmdWrite = 0x07;

inline void t4Request(uint8_t out[kT4ReportLen], uint32_t addr, bool read, uint8_t val) {
    for (size_t i = 0; i < kT4ReportLen; i++) out[i] = 0;
    out[0] = kT4FeatureId;
    out[1] = read ? kT4CmdRead : kT4CmdWrite;
    putLe32(out + 2, addr);
    out[6] = 1;
    out[8] = read ? 0 : val;
    uint16_t cs = t4Checksum(out, 1, 8);
    out[9] = (uint8_t)cs; out[10] = (uint8_t)(cs >> 8);
}

// Validates address, size and checksum of a register read reply.
inline bool t4Response(const uint8_t in[kT4ReportLen], uint32_t addr, uint8_t* val) {
    if (le32(in + 6) != addr) return false;
    if (le16(in + 10) != 1) return false;
    if (le16(in + 13) != t4Checksum(in, 6, 7)) return false;
    *val = in[12];
    return true;
}

constexpr uint8_t  kU1FeatureId = 0x05, kU1CmdRead = 0xD1, kU1CmdWrite = 0xD2;
constexpr size_t   kU1FeatureLen = 8;

inline void u1Request(uint8_t out[kU1FeatureLen], uint32_t addr, bool read, uint8_t val) {
    for (size_t i = 0; i < kU1FeatureLen; i++) out[i] = 0;
    out[0] = kU1FeatureId;
    out[1] = read ? kU1CmdRead : kU1CmdWrite;
    putLe32(out + 2, addr);
    out[6] = read ? 0 : val;
    uint8_t cs = 0x0A;   // U1_FEATURE_REPORT_LEN_ALL
    for (size_t i = 0; i < kU1FeatureLen - 1; i++) cs = (uint8_t)(cs + out[i]);
    out[7] = cs;
}
inline uint8_t u1Response(const uint8_t in[kU1FeatureLen]) { return in[6]; }

} // namespace alps

#endif
