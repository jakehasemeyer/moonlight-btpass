#pragma once
#include "ir_parse.h"

static bool write_ir_block(Pad& pad, uint32_t address, const uint8_t* data, size_t length) {
    if (!length || length > 16) return false;
    uint8_t report[22]{};
    report[0] = 0x16;
    report[1] = static_cast<uint8_t>(0x04 | (pad.rumble ? 1 : 0));
    report[2] = address >> 16; report[3] = address >> 8; report[4] = address;
    report[5] = static_cast<uint8_t>(length);
    memcpy(report + 6, data, length);
    return write_output(pad, report, sizeof(report));
}

static bool init_ir(Pad& pad, bool basic) {
    const uint8_t on = 0x08, mode = static_cast<uint8_t>(basic ? 0x01 : 0x03);
    const uint8_t s1[] = {0x02, 0, 0, 0x71, 0x01, 0, 0xaa, 0, 0x64};
    const uint8_t s2[] = {0x63, 0x03};
    const uint8_t first[] = {0x13, static_cast<uint8_t>(0x04 | (pad.rumble ? 1 : 0))};
    const uint8_t second[] = {0x1a, static_cast<uint8_t>(0x04 | (pad.rumble ? 1 : 0))};
    struct Step { uint32_t address; const uint8_t* data; size_t length; };
    const Step steps[] = {{0xb00030, &on, 1}, {0xb00000, s1, sizeof(s1)},
                          {0xb0001a, s2, sizeof(s2)}, {0xb00033, &mode, 1}, {0xb00030, &on, 1}};
    if (!write_output(pad, first, sizeof(first))) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    if (!write_output(pad, second, sizeof(second))) return false;
    for (const auto& step : steps) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!write_ir_block(pad, step.address, step.data, step.length)) return false;
    }
    return true;
}
