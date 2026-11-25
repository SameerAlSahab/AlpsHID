// Host test for AlpsCore.hpp:  g++ -std=gnu++14 -Wall -Wextra -I../AlpsHID core_test.cpp && ./a.out
#include "AlpsCore.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using namespace alps;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// Verbatim copy of the ORIGINAL 2021 checksum, for regression comparison.
static uint16_t orig_cs(uint8_t* buffer, unsigned long offset, unsigned long length) {
    uint16_t sum1 = 0xFF, sum2 = 0xFF; unsigned long i = 0;
    if (offset + length >= 50) return 0;
    while (length > 0) {
        unsigned long tlen = length > 20 ? 20 : length; length -= tlen;
        do { sum1 += buffer[offset + i]; sum2 += sum1; i++; } while (--tlen > 0);
        sum1 = (sum1 & 0xFF) + (sum1 >> 8); sum2 = (sum2 & 0xFF) + (sum2 >> 8);
    }
    sum1 = (sum1 & 0xFF) + (sum1 >> 8); sum2 = (sum2 & 0xFF) + (sum2 >> 8);
    return (uint16_t)(sum2 << 8 | sum1);
}

int main() {
    srand(1);
    // 1. checksum identical to original, and congruent to plain Fletcher mod 255
    for (int t = 0; t < 20000; t++) {
        uint8_t b[64]; for (auto& x : b) x = rand();
        size_t off = rand() % 20, len = 1 + rand() % 30;
        CHECK(t4Checksum(b, off, len) == orig_cs(b, off, len));
        if (off + len < 50) {
            uint32_t s1 = 0xFF, s2 = 0xFF;
            for (size_t i = 0; i < len; i++) { s1 += b[off + i]; s2 += s1; }
            uint16_t c = t4Checksum(b, off, len);
            CHECK((c & 0xFF) % 255 == s1 % 255 && (c >> 8) % 255 == s2 % 255);
        }
    }
    // 2. T4 request layout + fabricated reply round-trip; corruption rejected
    uint8_t req[kT4ReportLen]; t4Request(req, 0xC2C2, false, 0x02);
    CHECK(req[0] == 7 && req[1] == 7 && le32(req + 2) == 0xC2C2 && req[8] == 2);
    CHECK(le16(req + 9) == t4Checksum(req, 1, 8));
    uint8_t rep[kT4ReportLen] = {}; putLe32(rep + 6, 0xC2B0); rep[10] = 1; rep[12] = 0x5A;
    uint16_t cs = t4Checksum(rep, 6, 7); rep[13] = cs; rep[14] = cs >> 8;
    uint8_t v = 0; CHECK(t4Response(rep, 0xC2B0, &v) && v == 0x5A);
    rep[12] ^= 1; CHECK(!t4Response(rep, 0xC2B0, &v));          // bad checksum
    rep[12] ^= 1; CHECK(!t4Response(rep, 0xC2B1, &v));          // wrong address
    // 3. U1 request
    uint8_t u[8]; u1Request(u, 0x00800040, true, 0);
    CHECK(u[0] == 5 && u[1] == 0xD1 && le32(u + 2) == 0x00800040);
    uint8_t s = 0x0A; for (int i = 0; i < 7; i++) s += u[i]; CHECK(u[7] == s);
    // 4. U1 parse incl. short report (no garbage) and 7-bit z
    uint8_t r[28] = {3, 1, 2, 0x34, 0x12, 0x78, 0x56, 0xFF /*z=127 w/ bit7*/, 1, 0, 2, 0, 0x10};
    Frame f; CHECK(parseU1(r, sizeof r, f));
    CHECK(f.c[0].active && f.c[0].x == 0x1234 && f.c[0].y == 0x5678 && f.c[0].z == 127);
    CHECK(f.c[1].active && f.c[1].x == 1 && f.c[1].y == 2 && f.c[1].z == 0x10);
    CHECK(f.button);
    uint8_t shortr[8] = {3, 0, 1, 1, 0, 1, 0, 9}; CHECK(parseU1(shortr, 8, f));
    CHECK(f.c[0].active && !f.c[1].active && !f.c[4].active && !f.button);
    CHECK(!parseU1(shortr, 2, f));
    // 5. T4 parse: palm contact ignored alone, y mirrored, button
    uint8_t t[51] = {9, 2};
    t[2] = 0x10; t[3] = 0x00; t[4] = 0x01; t[5] = 0x00; t[6] = 0x01;     // c0 valid x=256 y=256
    t[7] = 0x90; t[8] = 0x00; t[9] = 0x02;                               // c1 palm
    t[27] = 1;
    CHECK(parseT4(t, sizeof t, 3315, f));
    CHECK(f.c[0].active && f.c[0].x == 256 && f.c[0].y == 3315 - 256);
    CHECK(!f.c[1].active && f.button);
    CHECK(!parseT4(t, 10, 3315, f));
    // 6. tracker: lift frame once, sparse slots, contact_count = highest slot + 1
    Tracker tr; Slot o[kMaxTouches]; Frame g; clearFrame(g);
    g.c[0] = {true, 10, 20, 50}; g.c[1] = {true, 30, 40, 50};
    CHECK(tr.update(g, o) == 2 && o[0].active && o[1].active);
    g.c[0].active = false;                                  // finger 0 lifts, 1 stays
    CHECK(tr.update(g, o) == 2);
    CHECK(o[0].valid && !o[0].active && o[0].x == 10);      // single lift frame w/ last coords
    CHECK(o[1].active);
    CHECK(tr.update(g, o) == 2 && !o[0].valid);             // lift only reported once
    g.c[1].active = false; CHECK(tr.update(g, o) == 2 && !o[1].active && o[1].valid);
    CHECK(tr.update(g, o) == 0 && !tr.anyDown());
    // 7. force touch hysteresis
    tr.forceEnabled = true; clearFrame(g); g.c[0] = {true, 1, 1, 100};
    tr.update(g, o); CHECK(!o[0].force && pressureFor(o[0]) < 100);
    g.c[0].z = 105; tr.update(g, o); CHECK(o[0].force && pressureFor(o[0]) == 255);
    g.c[0].z = 95;  tr.update(g, o); CHECK(o[0].force);     // still held (between on/off)
    g.c[0].z = 89;  tr.update(g, o); CHECK(!o[0].force);
    g.c[0].active = false; tr.update(g, o); CHECK(pressureFor(o[0]) == 0);
    // 8. thumb = lowest finger
    Slot th[3] = {{true, true, false, 0, 100, 1}, {true, true, false, 0, 900, 1}, {true, true, false, 0, 500, 1}};
    CHECK(lowestFinger(th, 3) == 1 && activeCount(th, 3) == 3);
    // 9. stick sub-pixel carry: 4 events of dx=1 with div 4 must yield exactly 1 total
    SubPixel sp; int ox, oy, tot = 0;
    for (int i = 0; i < 4; i++) { sp.apply(1, -1, 4, &ox, &oy); tot += ox; }
    CHECK(tot == 1);
    sp.reset(); tot = 0; for (int i = 0; i < 8; i++) { sp.apply(-1, 0, 4, &ox, &oy); tot += ox; } CHECK(tot == -2);
    int dx, dy, b; uint8_t st[8] = {6, 0x05, 0xFE, 0xFF, 8, 0}; CHECK(parseStick(st, 8, &dx, &dy, &b) && dx == -2 && dy == 8 && b == 5);

    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
