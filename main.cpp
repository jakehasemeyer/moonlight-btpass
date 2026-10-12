#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
extern "C" {
#include <hidsdi.h>
}
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Device {
    std::wstring path;
    std::wstring name;
    unsigned short product;
};

struct Pad {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD input_size = 22;
    std::mutex mutex;
    std::mutex output_mutex;
    std::atomic<bool> connected{false};
    uint8_t buttons1 = 0;
    uint8_t buttons2 = 0;
    uint8_t home = 0;
    float accel[3] = {0, 0, 0};
    float gyro[3] = {0, 0, 0};
    float gyro_zero[3] = {0, 0, 0};
    int gyro_samples = 0;
    std::atomic<bool> motion_plus{false};
    std::atomic<bool> gyro_ready{false};
    std::atomic<bool> rumble{false};
    uint64_t motion_timestamp_us = 0;
    uint32_t sequence = 0;
    std::thread reader;
};

static std::atomic<bool> running{true};

static std::vector<Device> find_remotes() {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    std::vector<Device> devices;
    if (info == INVALID_HANDLE_VALUE) return devices;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, index, &iface)) break;
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<uint8_t> buffer(bytes);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, bytes, nullptr, nullptr)) continue;

        HANDLE handle = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes) && attributes.VendorID == 0x057e &&
            (attributes.ProductID == 0x0306 || attributes.ProductID == 0x0330)) {
            wchar_t name[128]{};
            if (!HidD_GetProductString(handle, name, sizeof(name))) {
                wcscpy_s(name, L"Nintendo Wii Remote");
            }
            devices.push_back({detail->DevicePath, name, attributes.ProductID});
        }
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(info);
    return devices;
}

static bool write_output(Pad& pad, const uint8_t* bytes, DWORD size) {
    std::lock_guard<std::mutex> lock(pad.output_mutex);
    uint8_t report[22]{};
    if (size > sizeof(report)) return false;
    memcpy(report, bytes, size);
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(pad.handle, report, size, &written, &operation);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(operation.hEvent, 1000) == WAIT_TIMEOUT) CancelIoEx(pad.handle, &operation);
        ok = GetOverlappedResult(pad.handle, &operation, &written, TRUE);
    }
    CloseHandle(operation.hEvent);
    return ok && written >= size;
}

static bool write_report(Pad& pad, uint8_t report, uint8_t first, uint8_t second) {
    const uint8_t bytes[] = {report, static_cast<uint8_t>(first | (pad.rumble ? 1 : 0)), second};
    return write_output(pad, bytes, sizeof(bytes));
}

static void set_rumble(Pad& pad, bool enabled) {
    if (pad.rumble.exchange(enabled) == enabled) return;
    const uint8_t bytes[] = {0x10, static_cast<uint8_t>(enabled ? 1 : 0)};
    write_output(pad, bytes, sizeof(bytes));
}

static bool write_register(Pad& pad, uint32_t address, uint8_t value) {
    uint8_t bytes[22]{};
    bytes[0] = 0x16;
    bytes[1] = static_cast<uint8_t>(0x04 | (pad.rumble ? 1 : 0));
    bytes[2] = static_cast<uint8_t>(address >> 16);
    bytes[3] = static_cast<uint8_t>(address >> 8);
    bytes[4] = static_cast<uint8_t>(address);
    bytes[5] = 1;
    bytes[6] = value;
    return write_output(pad, bytes, sizeof(bytes));
}

static bool read_register(Pad& pad, uint32_t address, uint8_t* data, size_t length) {
    const uint8_t request[] = {0x17, static_cast<uint8_t>(0x04 | (pad.rumble ? 1 : 0)),
                               static_cast<uint8_t>(address >> 16),
                               static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address),
                               static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
    if (!write_output(pad, request, sizeof(request))) return false;
    std::vector<uint8_t> reply(pad.input_size);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    size_t copied = 0;
    while (std::chrono::steady_clock::now() < deadline && copied < length) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        DWORD received = 0;
        BOOL ok = ReadFile(pad.handle, reply.data(), pad.input_size, &received, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(operation.hEvent, 100) == WAIT_TIMEOUT) CancelIoEx(pad.handle, &operation);
            ok = GetOverlappedResult(pad.handle, &operation, &received, TRUE);
        }
        CloseHandle(operation.hEvent);
        if (!ok || received < 6 || reply[0] != 0x21) continue;
        if ((reply[3] & 0x0f) != 0) return false;
        uint16_t offset = static_cast<uint16_t>(reply[4] << 8 | reply[5]);
        if (offset != static_cast<uint16_t>(address + copied)) continue;
        size_t chunk = std::min<size_t>(((reply[3] >> 4) & 15) + 1, received - 6);
        chunk = std::min(chunk, length - copied);
        memcpy(data + copied, reply.data() + 6, chunk);
        copied += chunk;
    }
    return copied == length;
}

static bool open_remote(Pad& pad, const Device& device) {
    pad.handle = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
    if (pad.handle == INVALID_HANDLE_VALUE) return false;
    PHIDP_PREPARSED_DATA data = nullptr;
    HIDP_CAPS caps{};
    if (HidD_GetPreparsedData(pad.handle, &data)) {
        if (HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS) {
            pad.input_size = std::max<USHORT>(caps.InputReportByteLength, 22);
        }
        HidD_FreePreparsedData(data);
    }
    uint8_t identity[6]{};
    if (read_register(pad, 0xa600fa, identity, sizeof(identity)) &&
        identity[2] == 0xa6 && identity[3] == 0x20 && identity[5] == 0x05) {
        if (write_register(pad, 0xa600f0, 0x55)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pad.motion_plus = write_register(pad, 0xa600fe, 0x04);
        }
    } else if (read_register(pad, 0xa400fa, identity, sizeof(identity)) &&
               identity[2] == 0xa4 && identity[3] == 0x20 && identity[5] == 0x05) {
        pad.motion_plus = true;
    }
    if (!write_report(pad, 0x12, 0x04, pad.motion_plus ? 0x35 : 0x31)) {
        CloseHandle(pad.handle);
        pad.handle = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

static void apply_report(Pad& pad, const uint8_t* bytes, DWORD count) {
    if (count < 3 || (bytes[0] != 0x30 && bytes[0] != 0x31 && bytes[0] != 0x35)) return;
    const uint8_t a = bytes[1], b = bytes[2];
    std::lock_guard<std::mutex> lock(pad.mutex);
    pad.buttons1 = (a & 0x01 ? 0x80 : 0) | (a & 0x04 ? 0x40 : 0) |
                   (a & 0x02 ? 0x20 : 0) | (a & 0x08 ? 0x10 : 0) |
                   (a & 0x10 ? 0x08 : 0) | (b & 0x10 ? 0x01 : 0);
    pad.buttons2 = (b & 0x08 ? 0x20 : 0) | (b & 0x04 ? 0x40 : 0) |
                   (b & 0x02 ? 0x80 : 0) | (b & 0x01 ? 0x10 : 0);
    pad.home = (b & 0x80) != 0;
    if (count >= 6 && (bytes[0] == 0x31 || bytes[0] == 0x35)) {
        // ponytail: use nominal calibration first; read EEPROM calibration if hardware tests show drift.
        for (int i = 0; i < 3; ++i) pad.accel[i] = (static_cast<int>(bytes[3 + i]) - 128) / 26.0f;
        pad.motion_timestamp_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    if (pad.motion_plus && count >= 12 && bytes[0] == 0x35 && (bytes[11] & 0x02)) {
        const uint8_t* extension = bytes + 6;
        const int raw[] = {
            extension[2] | (extension[5] & 0xfc) << 6,
            extension[0] | (extension[3] & 0xfc) << 6,
            extension[1] | (extension[4] & 0xfc) << 6,
        };
        const bool slow[] = {bool(extension[3] & 1), bool(extension[3] & 2), bool(extension[4] & 2)};
        if (pad.gyro_samples < 100) {
            for (int i = 0; i < 3; ++i) pad.gyro_zero[i] += raw[i];
            if (++pad.gyro_samples == 100) {
                for (float& zero : pad.gyro_zero) zero /= 100;
                pad.gyro_ready = true;
            }
        }
        for (int i = 0; i < 3; ++i) {
            pad.gyro[i] = pad.gyro_ready ? (raw[i] - pad.gyro_zero[i]) / 13.768f *
                                          (slow[i] ? 1.0f : 2000.0f / 440.0f) : 0.0f;
        }
    }
    pad.connected = true;
    ++pad.sequence;
}

static void read_remote(Pad& pad) {
    std::vector<uint8_t> bytes(pad.input_size);
    while (running) {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) break;
        DWORD count = 0;
        BOOL ok = ReadFile(pad.handle, bytes.data(), pad.input_size, &count, &operation);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(operation.hEvent, 500);
            if (wait == WAIT_TIMEOUT) CancelIoEx(pad.handle, &operation);
            ok = GetOverlappedResult(pad.handle, &operation, &count, TRUE);
            if (wait == WAIT_TIMEOUT) {
                CloseHandle(operation.hEvent);
                continue;
            }
        }
        CloseHandle(operation.hEvent);
        if (!ok) break;
        if (count >= 3 && bytes[0] == 0x20) {
            write_report(pad, 0x12, 0x04, pad.motion_plus ? 0x35 : 0x31);
            continue;
        }
        apply_report(pad, bytes.data(), count);
    }
    std::lock_guard<std::mutex> lock(pad.mutex);
    pad.connected = false;
    pad.buttons1 = pad.buttons2 = pad.home = 0;
    pad.accel[0] = pad.accel[1] = pad.accel[2] = 0;
    pad.gyro[0] = pad.gyro[1] = pad.gyro[2] = 0;
    pad.gyro_ready = false;
    pad.motion_timestamp_us = 0;
    ++pad.sequence;
}

static uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
static void put16(uint8_t* p, uint16_t value) {
    p[0] = value & 255;
    p[1] = value >> 8;
}
static void put32(uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
static void put64(uint8_t* p, uint64_t value) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
static void put_float(uint8_t* p, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    put32(p, bits);
}
static uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
    }
    return ~crc;
}

static std::vector<uint8_t> packet(uint32_t type, size_t payload_size) {
    static const uint32_t session_id = static_cast<uint32_t>(GetTickCount64()) ^ GetCurrentProcessId();
    std::vector<uint8_t> bytes(20 + payload_size);
    memcpy(bytes.data(), "DSUS", 4);
    put16(bytes.data() + 4, 1001);
    put16(bytes.data() + 6, static_cast<uint16_t>(4 + payload_size));
    put32(bytes.data() + 12, session_id);
    put32(bytes.data() + 16, type);
    return bytes;
}
static void send_packet(SOCKET socket, sockaddr_in address, std::vector<uint8_t>& bytes) {
    put32(bytes.data() + 8, crc32(bytes.data(), bytes.size()));
    sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
           reinterpret_cast<sockaddr*>(&address), sizeof(address));
}

static void port_header(uint8_t* p, int slot, bool connected, bool gyro = false) {
    p[0] = static_cast<uint8_t>(slot);
    if (!connected) return;
    p[1] = 2;
    p[2] = gyro ? 2 : 1;
    p[3] = 2;
    p[4] = 0x02;
    p[5] = 'M';
    p[6] = 'B';
    p[7] = 'T';
    p[8] = 0;
    p[9] = static_cast<uint8_t>(slot + 1);
}
static std::vector<uint8_t> port_info(int slot, bool connected, bool gyro = false) {
    auto bytes = packet(0x100001, 12);
    port_header(bytes.data() + 20, slot, connected, gyro);
    return bytes;
}
static std::vector<uint8_t> pad_data(int slot, Pad& pad) {
    auto bytes = packet(0x100002, 80);
    uint8_t* p = bytes.data() + 20;
    std::lock_guard<std::mutex> lock(pad.mutex);
    port_header(p, slot, pad.connected, pad.gyro_ready);
    p[11] = pad.connected;
    put32(p + 12, pad.sequence);
    p[16] = pad.buttons1;
    p[17] = pad.buttons2;
    p[18] = pad.home;
    p[20] = p[21] = p[22] = p[23] = 128;
    for (int i = 0; i < 4; ++i) p[24 + i] = (pad.buttons1 & (0x80 >> i)) ? 255 : 0;
    p[28] = pad.buttons2 & 0x80 ? 255 : 0;
    p[29] = pad.buttons2 & 0x40 ? 255 : 0;
    p[30] = pad.buttons2 & 0x20 ? 255 : 0;
    p[31] = pad.buttons2 & 0x10 ? 255 : 0;
    put64(p + 48, pad.motion_timestamp_us);
    for (int i = 0; i < 3; ++i) put_float(p + 56 + 4 * i, pad.accel[i]);
    for (int i = 0; i < 3; ++i) put_float(p + 68 + 4 * i, pad.gyro[i]);
    return bytes;
}

struct Subscriber {
    sockaddr_in address{};
    std::array<bool, 4> slots{};
    std::chrono::steady_clock::time_point seen;
};

static bool valid_request(uint8_t* bytes, int size) {
    if (size < 20 || memcmp(bytes, "DSUC", 4) || get16(bytes + 4) > 1001 ||
        get16(bytes + 6) + 16 != size) return false;
    uint32_t expected = get32(bytes + 8);
    put32(bytes + 8, 0);
    return expected == crc32(bytes, size);
}

static int serve(uint32_t host, std::vector<std::unique_ptr<Pad>>& pads) {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 1;
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(26760);
    local.sin_addr.s_addr = INADDR_ANY;
    if (socket == INVALID_SOCKET || bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
        if (socket != INVALID_SOCKET) closesocket(socket);
        WSACleanup();
        return 1;
    }
    std::vector<Subscriber> subscribers;
    std::array<std::chrono::steady_clock::time_point, 4> last_rumble{};
    while (running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(socket, &read_set);
        timeval timeout{0, 10000};
        if (select(0, &read_set, nullptr, nullptr, &timeout) > 0) {
            uint8_t request[256]{};
            sockaddr_in peer{};
            int peer_size = sizeof(peer);
            int size = recvfrom(socket, reinterpret_cast<char*>(request), sizeof(request), 0,
                                reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (peer.sin_addr.s_addr == host && valid_request(request, size)) {
                uint32_t type = get32(request + 16);
                if (type == 0x100000) {
                    auto reply = packet(type, 4);
                    put16(reply.data() + 20, 1001);
                    send_packet(socket, peer, reply);
                } else if (type == 0x100001 && size >= 28) {
                    uint32_t count = std::min<uint32_t>(get32(request + 20), 4);
                    for (uint32_t i = 0; i < count && 24 + i < static_cast<uint32_t>(size); ++i) {
                        int slot = request[24 + i];
                        if (slot > 3) continue;
                        bool connected = slot < static_cast<int>(pads.size()) && pads[slot]->connected;
                        auto reply = port_info(slot, connected, connected && pads[slot]->gyro_ready);
                        send_packet(socket, peer, reply);
                    }
                } else if (type == 0x100002 && size >= 28) {
                    auto it = std::find_if(subscribers.begin(), subscribers.end(), [&](const Subscriber& sub) {
                        return sub.address.sin_addr.s_addr == peer.sin_addr.s_addr &&
                               sub.address.sin_port == peer.sin_port;
                    });
                    if (it == subscribers.end()) {
                        subscribers.push_back({});
                        it = subscribers.end() - 1;
                        it->address = peer;
                    }
                    if (request[20] == 0) {
                        for (size_t i = 0; i < pads.size(); ++i) it->slots[i] = true;
                    } else if (request[20] == 1 && request[21] < pads.size()) {
                        it->slots[request[21]] = true;
                    }
                    it->seen = std::chrono::steady_clock::now();
                } else if (type == 0x100100 && size == 22 && request[20] < pads.size()) {
                    int slot = request[20];
                    set_rumble(*pads[slot], request[21] != 0);
                    last_rumble[slot] = std::chrono::steady_clock::now();
                }
            }
        }
        auto now = std::chrono::steady_clock::now();
        for (size_t slot = 0; slot < pads.size(); ++slot) {
            if (pads[slot]->rumble && now - last_rumble[slot] > std::chrono::seconds(2))
                set_rumble(*pads[slot], false);
        }
        subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(), [&](const Subscriber& sub) {
            return now - sub.seen > std::chrono::seconds(5);
        }), subscribers.end());
        for (const auto& sub : subscribers) {
            for (size_t slot = 0; slot < pads.size(); ++slot) {
                if (!sub.slots[slot]) continue;
                auto reply = pad_data(static_cast<int>(slot), *pads[slot]);
                send_packet(socket, sub.address, reply);
            }
        }
    }
    closesocket(socket);
    WSACleanup();
    return 0;
}

#ifndef BTPASS_TEST
enum { ID_HOST = 101, ID_LIST, ID_REFRESH, ID_START, ID_STOP, ID_STATUS };
constexpr UINT WM_SERVER_DONE = WM_APP + 1;

struct App {
    HWND window{}, host{}, list{}, refresh{}, start{}, stop{}, status{};
    std::vector<Device> devices;
    std::vector<std::unique_ptr<Pad>> pads;
    std::thread server;
};
static App app;

static void set_status(const wchar_t* message) { SetWindowTextW(app.status, message); }

static void refresh_devices() {
    app.devices = find_remotes();
    SendMessageW(app.list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < app.devices.size(); ++i) {
        std::wstring text = L"Wii Remote " + std::to_wstring(i + 1) + L": " + app.devices[i].name;
        SendMessageW(app.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    set_status(app.devices.empty() ? L"No connected Wii Remotes. Connect one, then Refresh."
                                   : L"Select up to four Wii Remotes, then press Start.");
}

static void stop_bridge() {
    running = false;
    KillTimer(app.window, 1);
    if (app.server.joinable()) app.server.join();
    for (auto& pad : app.pads) {
        if (pad->reader.joinable()) pad->reader.join();
        set_rumble(*pad, false);
        CloseHandle(pad->handle);
    }
    app.pads.clear();
    EnableWindow(app.host, TRUE);
    EnableWindow(app.list, TRUE);
    EnableWindow(app.refresh, TRUE);
    EnableWindow(app.start, TRUE);
    EnableWindow(app.stop, FALSE);
    set_status(L"Stopped.");
}

static void start_bridge() {
    wchar_t host_text[64]{};
    GetWindowTextW(app.host, host_text, 64);
    in_addr allowed_host{};
    if (InetPtonW(AF_INET, host_text, &allowed_host) != 1) {
        set_status(L"Enter the Moonlight host's IPv4 address.");
        return;
    }
    int selected = static_cast<int>(SendMessageW(app.list, LB_GETSELCOUNT, 0, 0));
    if (selected < 1 || selected > 4) {
        set_status(L"Select between one and four Wii Remotes.");
        return;
    }
    std::vector<int> indices(selected);
    SendMessageW(app.list, LB_GETSELITEMS, selected, reinterpret_cast<LPARAM>(indices.data()));
    for (int index : indices) {
        auto pad = std::make_unique<Pad>();
        if (!open_remote(*pad, app.devices[index])) {
            MessageBoxW(app.window, L"A selected Wii Remote could not be opened. Reconnect it and refresh the list.",
                        L"Moonlight Wii Remote Bridge", MB_OK | MB_ICONERROR);
            for (auto& opened : app.pads) CloseHandle(opened->handle);
            app.pads.clear();
            return;
        }
        app.pads.push_back(std::move(pad));
    }
    running = true;
    for (auto& pad : app.pads) pad->reader = std::thread(read_remote, std::ref(*pad));
    EnableWindow(app.host, FALSE);
    EnableWindow(app.list, FALSE);
    EnableWindow(app.refresh, FALSE);
    EnableWindow(app.start, FALSE);
    EnableWindow(app.stop, TRUE);
    app.server = std::thread([host = allowed_host.s_addr] {
        int result = serve(host, app.pads);
        PostMessageW(app.window, WM_SERVER_DONE, result, 0);
    });
    SetTimer(app.window, 1, 500, nullptr);
    set_status(L"Running on UDP 26760. Configure Dolphin's DSU Client on the host.");
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        app.window = window;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto control = [&](const wchar_t* kind, const wchar_t* label, DWORD style,
                           int x, int y, int width, int height, int id) {
            HWND item = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style,
                                        x, y, width, height, window,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                        GetModuleHandleW(nullptr), nullptr);
            SendMessageW(item, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return item;
        };
        control(L"STATIC", L"Moonlight host IPv4 address", 0, 16, 16, 300, 20, 0);
        app.host = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 16, 40, 330, 25, ID_HOST);
        control(L"STATIC", L"Connected Wii Remotes", 0, 16, 80, 270, 20, 0);
        app.refresh = control(L"BUTTON", L"Refresh", BS_PUSHBUTTON, 355, 77, 95, 26, ID_REFRESH);
        app.list = control(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_EXTENDEDSEL |
                           LBS_NOTIFY, 16, 105, 434, 150, ID_LIST);
        app.start = control(L"BUTTON", L"Start", BS_DEFPUSHBUTTON, 16, 270, 100, 30, ID_START);
        app.stop = control(L"BUTTON", L"Stop", BS_PUSHBUTTON, 125, 270, 100, 30, ID_STOP);
        EnableWindow(app.stop, FALSE);
        app.status = control(L"STATIC", L"", 0, 16, 315, 434, 40, ID_STATUS);
        refresh_devices();
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
            switch (LOWORD(wparam)) {
            case ID_REFRESH: refresh_devices(); return 0;
            case ID_START: start_bridge(); return 0;
            case ID_STOP: stop_bridge(); return 0;
            }
        }
        break;
    case WM_SERVER_DONE:
        if (app.server.joinable()) {
            stop_bridge();
            if (wparam) set_status(L"Could not start UDP 26760. Close other DSU servers and retry.");
        }
        return 0;
    case WM_TIMER:
        if (wparam == 1) {
            int active = 0;
            int calibrating = 0;
            for (const auto& pad : app.pads) active += pad->connected ? 1 : 0;
            for (const auto& pad : app.pads) calibrating += pad->motion_plus && !pad->gyro_ready ? 1 : 0;
            std::wstring text = L"Running: " + std::to_wstring(active) + L"/" +
                                std::to_wstring(app.pads.size()) + L" Wii Remotes sending reports.";
            if (calibrating) text += L" Keep MotionPlus remote(s) still for gyro calibration.";
            set_status(text.c_str());
        }
        return 0;
    case WM_DESTROY:
        stop_bridge();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"MoonlightBtPassWindow";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Moonlight Wii Remote Bridge",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 480, 400, nullptr, nullptr,
                                  instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
#endif
