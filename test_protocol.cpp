#define BTPASS_TEST
#include "main.cpp"

#include <cassert>
#include <cmath>

int main() {
    assert(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926);
    auto request = packet(0x100001, 8);
    memcpy(request.data(), "DSUC", 4);
    put32(request.data() + 20, 4);
    for (int i = 0; i < 4; ++i) request[24 + i] = i;
    put32(request.data() + 8, crc32(request.data(), request.size()));
    assert(valid_request(request.data(), static_cast<int>(request.size())));
    request[24] ^= 1;
    assert(!valid_request(request.data(), static_cast<int>(request.size())));

    Pad pad;
    pad.connected = true;
    const uint8_t report[] = {0x31, 0x0d, 0x8c, 128, 154, 128};
    apply_report(pad, report, sizeof(report));
    assert(pad.buttons1 == 0xd0);
    assert(pad.buttons2 == 0x60);
    assert(pad.home == 1);
    assert(pad.accel[1] == 1.0f);
    auto data = pad_data(0, pad);
    assert(data.size() == 100);
    assert(get16(data.data() + 6) == 84);
    assert(data[20] == 0 && data[21] == 2 && data[31] == 1);
    assert(get32(data.data() + 32) == 1);
    assert(data[36] == 0xd0 && data[37] == 0x60 && data[38] == 1);
    assert(data[44] == 255 && data[45] == 255 && data[47] == 255);
    auto info = port_info(0, true);
    assert(info.size() == 32 && info[21] == 2);

    Pad plus;
    plus.motion_plus = true;
    uint8_t still[] = {0x35, 0, 0, 128, 154, 128, 0x7f, 0x7f, 0x7f, 0x7f, 0x7e, 0x7e};
    for (int i = 0; i < 100; ++i) apply_report(plus, still, sizeof(still));
    assert(plus.gyro_ready && plus.gyro_samples == 100);
    still[6] = 0x09;
    still[9] = 0x83;
    apply_report(plus, still, sizeof(still));
    assert(std::abs(plus.gyro[1] - 10.0f) < 0.1f);
    auto gyro_data = pad_data(0, plus);
    float sent_yaw = 0;
    memcpy(&sent_yaw, gyro_data.data() + 20 + 72, sizeof(sent_yaw));
    assert(std::abs(sent_yaw - 10.0f) < 0.1f);
    assert(gyro_data[22] == 2);
    return 0;
}
