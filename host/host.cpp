#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using client_t = void*;
using target_t = void*;
using notification_t = void(CALLBACK*)(client_t, target_t, uint8_t, uint8_t, uint8_t, void*);

static std::atomic<bool> alive{true};
static BOOL WINAPI stop_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_CLOSE_EVENT) {
        alive = false;
        return TRUE;
    }
    return FALSE;
}
static void CALLBACK on_rumble(client_t, target_t, uint8_t large, uint8_t small, uint8_t, void* data) {
    static_cast<std::atomic<uint8_t>*>(data)->store(std::max(large, small));
}
static void put16(uint8_t* p, uint16_t value) { p[0] = value & 255; p[1] = value >> 8; }
static void put32(uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
static uint32_t crc32(const uint8_t* p, size_t size) {
    uint32_t value = ~0u;
    for (size_t i = 0; i < size; ++i) {
        value ^= p[i];
        for (int bit = 0; bit < 8; ++bit) value = (value >> 1) ^ (0xedb88320u & -(value & 1u));
    }
    return ~value;
}
static void send_rumble(SOCKET socket, const sockaddr_in& client, int slot, uint8_t level) {
    uint8_t packet[22]{};
    memcpy(packet, "DSUC", 4);
    put16(packet + 4, 1001);
    put16(packet + 6, 6);
    put32(packet + 12, GetCurrentProcessId());
    put32(packet + 16, 0x100100);
    packet[20] = static_cast<uint8_t>(slot);
    packet[21] = level;
    put32(packet + 8, crc32(packet, sizeof(packet)));
    sendto(socket, reinterpret_cast<const char*>(packet), sizeof(packet), 0,
           reinterpret_cast<const sockaddr*>(&client), sizeof(client));
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        puts("Usage: moonlight-btpass-host.exe CLIENT_IPV4 [REMOTE_COUNT_1_TO_4]");
        return 1;
    }
    int count = argc == 3 ? std::atoi(argv[2]) : 1;
    if (count < 1 || count > 4) return 1;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(26760);
    if (InetPtonA(AF_INET, argv[1], &destination.sin_addr) != 1) {
        puts("Invalid client IPv4 address.");
        return 1;
    }
    HMODULE library = LoadLibraryW(L"ViGEmClient.dll");
    if (!library) {
        puts("ViGEmClient.dll must be beside this executable.");
        return 1;
    }
    auto alloc = reinterpret_cast<client_t(__cdecl*)()>(GetProcAddress(library, "vigem_alloc"));
    auto connect = reinterpret_cast<uint32_t(__cdecl*)(client_t)>(GetProcAddress(library, "vigem_connect"));
    auto disconnect = reinterpret_cast<void(__cdecl*)(client_t)>(GetProcAddress(library, "vigem_disconnect"));
    auto free_client = reinterpret_cast<void(__cdecl*)(client_t)>(GetProcAddress(library, "vigem_free"));
    auto target_alloc = reinterpret_cast<target_t(__cdecl*)()>(GetProcAddress(library, "vigem_target_x360_alloc"));
    auto target_add = reinterpret_cast<uint32_t(__cdecl*)(client_t, target_t)>(GetProcAddress(library, "vigem_target_add"));
    auto target_remove = reinterpret_cast<uint32_t(__cdecl*)(client_t, target_t)>(GetProcAddress(library, "vigem_target_remove"));
    auto target_free = reinterpret_cast<void(__cdecl*)(target_t)>(GetProcAddress(library, "vigem_target_free"));
    auto notify = reinterpret_cast<uint32_t(__cdecl*)(client_t, target_t, notification_t, void*)>(
        GetProcAddress(library, "vigem_target_x360_register_notification"));
    auto unnotify = reinterpret_cast<void(__cdecl*)(target_t)>(
        GetProcAddress(library, "vigem_target_x360_unregister_notification"));
    auto user_index = reinterpret_cast<uint32_t(__cdecl*)(client_t, target_t, uint32_t*)>(
        GetProcAddress(library, "vigem_target_x360_get_user_index"));
    if (!alloc || !connect || !disconnect || !free_client || !target_alloc || !target_add ||
        !target_remove || !target_free || !notify || !unnotify || !user_index) {
        puts("ViGEmClient.dll is missing required functions.");
        FreeLibrary(library);
        return 1;
    }
    client_t bus = alloc();
    if (!bus || connect(bus) != 0x20000000) {
        puts("ViGEmBus is missing or unavailable. Install the ViGEmBus driver on the host.");
        if (bus) free_client(bus);
        FreeLibrary(library);
        return 1;
    }
    std::array<target_t, 4> targets{};
    std::array<std::atomic<uint8_t>, 4> levels{};
    int connected = 0;
    for (; connected < count; ++connected) {
        targets[connected] = target_alloc();
        if (!targets[connected] || target_add(bus, targets[connected]) != 0x20000000 ||
            notify(bus, targets[connected], on_rumble, &levels[connected]) != 0x20000000) {
            puts("Could not create a virtual rumble controller.");
            break;
        }
        uint32_t index = 0;
        if (user_index(bus, targets[connected], &index) == 0x20000000)
            printf("Remote %d rumble: XInput/%u/Gamepad\n", connected + 1, index);
    }
    if (connected == count) {
        WSADATA winsock{};
        if (!WSAStartup(MAKEWORD(2, 2), &winsock)) {
            SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket != INVALID_SOCKET) {
                SetConsoleCtrlHandler(stop_handler, TRUE);
                puts("Sending rumble to the client. Press Ctrl+C to stop.");
                while (alive) {
                    for (int slot = 0; slot < count; ++slot)
                        send_rumble(socket, destination, slot, levels[slot]);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                for (int slot = 0; slot < count; ++slot) send_rumble(socket, destination, slot, 0);
                closesocket(socket);
            }
            WSACleanup();
        }
    }
    for (int i = 0; i < count; ++i) {
        if (!targets[i]) continue;
        if (i < connected) unnotify(targets[i]);
        target_remove(bus, targets[i]);
        target_free(targets[i]);
    }
    disconnect(bus);
    free_client(bus);
    FreeLibrary(library);
    return connected == count ? 0 : 1;
}
