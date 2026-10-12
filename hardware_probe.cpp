#define BTPASS_TEST
#include "main.cpp"
#include <cmath>
#include <cstdio>

int main(int argc, char**) {
    auto devices = find_remotes();
    printf("devices=%zu\n", devices.size());
    if (devices.empty()) return 2;
    if (argc > 1) {
        std::vector<std::unique_ptr<Pad>> pads;
        pads.push_back(std::make_unique<Pad>());
        if (!open_remote(*pads[0], devices[0])) return 3;
        running = true;
        pads[0]->reader = std::thread(read_remote, std::ref(*pads[0]));
        std::thread server([&] { serve(htonl(INADDR_LOOPBACK), pads); });
        puts("rumble server ready for 30 seconds");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(30));
        running = false;
        server.join();
        pads[0]->reader.join();
        set_rumble(*pads[0], false);
        CloseHandle(pads[0]->handle);
        return 0;
    }
    Pad pad;
    if (!open_remote(pad, devices[0])) {
        printf("open failed error=%lu\n", GetLastError());
        pad.handle = CreateFileW(devices[0].path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
        printf("manual handle=%d error=%lu\n", pad.handle != INVALID_HANDLE_VALUE, GetLastError());
        if (pad.handle != INVALID_HANDLE_VALUE) {
            uint8_t output[] = {0x12, 0x04, 0x31};
            OVERLAPPED operation{};
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD count = 0;
            BOOL ok = WriteFile(pad.handle, output, sizeof(output), &count, &operation);
            DWORD error = GetLastError();
            if (!ok && error == ERROR_IO_PENDING) {
                DWORD wait = WaitForSingleObject(operation.hEvent, 1000);
                ok = GetOverlappedResult(pad.handle, &operation, &count, wait != WAIT_TIMEOUT);
                error = GetLastError();
            }
            printf("report mode=%d error=%lu count=%lu\n", ok, error, count);
            CloseHandle(operation.hEvent);
            CloseHandle(pad.handle);
        }
        return 3;
    }
    printf("product=%04x motion_plus=%d\n", devices[0].product, int(pad.motion_plus));
    const uint8_t rumble_on[] = {0x10, 0x01};
    const uint8_t rumble_off[] = {0x10, 0x00};
    printf("rumble_on_write=%d\n", write_output(pad, rumble_on, sizeof(rumble_on)));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    printf("rumble_off_write=%d\n", write_output(pad, rumble_off, sizeof(rumble_off)));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    int seen = 0;
    float peak[3]{};
    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t bytes[32]{};
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD count = 0;
        BOOL ok = ReadFile(pad.handle, bytes, pad.input_size, &count, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(operation.hEvent, 500);
            if (wait == WAIT_TIMEOUT) CancelIoEx(pad.handle, &operation);
            ok = GetOverlappedResult(pad.handle, &operation, &count, TRUE);
        }
        CloseHandle(operation.hEvent);
        if (!ok) continue;
        if (seen++ < 15) {
            printf("input %02x count=%lu", bytes[0], count);
            for (DWORD i = 1; i < std::min<DWORD>(count, 14); ++i) printf(" %02x", bytes[i]);
            puts("");
        }
        if (count >= 3 && bytes[0] == 0x20) write_report(pad, 0x12, 0x04, 0x35);
        apply_report(pad, bytes, count);
        if (pad.gyro_ready) {
            for (int i = 0; i < 3; ++i) peak[i] = std::max(peak[i], std::abs(pad.gyro[i]));
        }
    }
    printf("reports=%u gyro_ready=%d gyro=%f,%f,%f\n", pad.sequence, int(pad.gyro_ready),
           pad.gyro[0], pad.gyro[1], pad.gyro[2]);
    printf("peak_deg_s=%f,%f,%f\n", peak[0], peak[1], peak[2]);
    CloseHandle(pad.handle);
    return 0;
}
