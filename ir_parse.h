#pragma once
#include <cstdint>

struct IrCursor { bool visible; uint16_t x; uint16_t y; };

static IrCursor decode_ir_basic(const uint8_t* data) {
    unsigned sx = 0, sy = 0, found = 0;
    for (int i = 0; i < 2; ++i) {
        const uint8_t* p = data + i * 5;
        const unsigned x[2] = {unsigned(p[0] | ((p[2] & 0x30) << 4)), unsigned(p[3] | ((p[2] & 3) << 8))};
        const unsigned y[2] = {unsigned(p[1] | ((p[2] & 0xc0) << 2)), unsigned(p[4] | ((p[2] & 0x0c) << 6))};
        for (int j = 0; j < 2; ++j) if (!(x[j] == 1023 && y[j] == 1023) && y[j] <= 767) {
            sx += x[j]; sy += y[j]; ++found;
        }
    }
    if (!found) return {false, 0, 0};
    const unsigned x = sx / found, y = sy / found;
    return {true, static_cast<uint16_t>((1023 - x) * 1919 / 1023), static_cast<uint16_t>(y * 941 / 767)};
}

static IrCursor decode_ir_extended(const uint8_t* data) {
    unsigned sx = 0, sy = 0, found = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* p = data + i * 3;
        const unsigned x = p[0] | ((p[2] & 0x30) << 4);
        const unsigned y = p[1] | ((p[2] & 0xc0) << 2);
        if (x == 1023 && y == 1023) continue;
        if (y > 767) continue;
        sx += x; sy += y; ++found;
    }
    if (!found) return {false, 0, 0};
    const unsigned x = sx / found, y = sy / found;
    return {true, static_cast<uint16_t>((1023 - x) * 1919 / 1023), static_cast<uint16_t>(y * 941 / 767)};
}
