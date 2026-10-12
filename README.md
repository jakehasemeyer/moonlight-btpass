# Moonlight Wii Remote Bridge

`moonlight-btpass.exe` is a portable Windows GUI for using Wii Remotes near a Moonlight client with Dolphin running on the Windows host. It supports up to four remotes at once. The client needs no installer, administrator rights, or third-party runtime. Dolphin receives buttons, accelerometer, and MotionPlus gyro through DSU. Rumble uses the separate host helper and a host-side virtual controller driver.

## Setup

1. Connect each Wii Remote to **Windows on the Moonlight client** so it appears as a HID device. This program lists connected Nintendo RVL-CNT-01 and RVL-CNT-01-TR devices (HID vendor ID `057e`, product IDs `0306` and `0330`). Windows can label an RVL-036 Wii Remote Plus as RVL-CNT-01/`0306`; the bridge checks the MotionPlus hardware rather than relying on that label. It does not do Bluetooth pairing itself. If a remote does not appear, connect it first and click **Refresh**.
2. On the client, open `moonlight-btpass.exe`. Enter the **host PC's IPv4 address**, select one to four Wii Remotes (Ctrl-click for multiple), and click **Start**. Windows Firewall may ask to allow the app on private networks. The app listens on UDP port `26760` and accepts DSU requests only from the entered host IP.
3. On the host, find the **client PC's IPv4 address** (for example, with `ipconfig`). In Dolphin, open **Controllers → Alternate Input Sources → DSU Client**, enable it, and add a server named **MoonlightBtPass** pointing to `CLIENT_IP:26760`.
4. Copy the two `.ini` files from `profiles/Wiimote` to the host's Dolphin user folder under `Config/Profiles/Wiimote` (**File → Open User Folder** in Dolphin). Set each Wii Remote slot to **Emulated Wii Remote**, click **Configure**, then load **Moonlight Wii Remote** or **Moonlight Wii Remote Plus**. The Plus profile enables MotionPlus gyro and gyro-based pointing; press **− and + together** to recenter its pointer. The first selected client remote is DSU slot 0, the second is slot 1, and so on. After loading a profile for another slot, change its Device from `DSUClient/0/MoonlightBtPass` to that slot's DSU device.
5. For rumble, install the [ViGEmBus driver](https://github.com/nefarius/ViGEmBus/releases/tag/v1.22.0) **on the host** with administrator rights, if it is not already installed. Copy the `host` folder to the host PC and run `moonlight-btpass-host.exe CLIENT_IP REMOTE_COUNT` there. Keep `ViGEmClient.dll` beside the helper. Start it before opening Dolphin's controller configuration. It prints each remote's XInput device index. In the profile's **Rumble → Motor** field, select **Motor L** from the matching printed `XInput/N/Gamepad` device; the saved default is XInput index 0. Leave the host helper running while playing. No host driver is needed if you do not use rumble.

Hold a MotionPlus remote still for about one second after pressing **Start** so the client can calibrate its gyro. The Plus profile needs a Wii Remote Plus or a Wii Remote with a MotionPlus accessory; the plain profile also works on those devices but does not attach MotionPlus in Dolphin.

| Physical Wii Remote | Dolphin DSU input |
| --- | --- |
| A | Circle |
| B | Cross |
| 1 | Triangle |
| 2 | Square |
| + / − | Options / Share |
| Home | PS |
| D-pad | Pad N / S / E / W |
| Accelerometer | Accel Up / Down / Left / Right / Forward / Backward |

The accelerometer uses nominal Wii Remote calibration. If its direction feels reversed in Dolphin, swap the corresponding positive and negative mapping in the Emulated Wii Remote configuration.

## Scope

This is **not full Bluetooth passthrough**. Dolphin uses an *emulated* Wii Remote that receives the real remote's buttons, accelerometer, and MotionPlus gyro through DSU. The client does not create a Bluetooth adapter or device on the host. Physical IR camera data, Nunchuk or other extensions, speaker, and generic Bluetooth devices are not forwarded. The Plus profile can derive a pointer from gyro, but it cannot provide sensor-bar IR precision and will need recentering. Games that require those missing features will not behave like they do with Dolphin's Real Wii Remote or Bluetooth Passthrough modes.

Windows must already expose each remote as a usable HID device. Pairing varies by adapter and Windows version; a connected device may still fail to provide reports. An RVL-036 Wii Remote Plus was tested on Windows: the bridge detected its already-active MotionPlus register and received gyro reports during movement. The remote button/accelerometer path and DSU input packets were also tested. The host helper created a virtual XInput device, and a test XInput vibration command traveled through the helper, UDP, and the client to the physical Wii Remote; the motor pulse was felt. Dolphin gameplay has not yet been verified. Keep Dolphin closed on the client while the bridge uses the remote; both programs reading the same HID interface reduced the observed report rate sharply. The network protocol has no encryption; use it on a trusted LAN or through a private VPN.

## Build

The release `.exe` files are already in this directory. To rebuild them, install a MinGW-w64 `g++` toolchain and run `powershell -ExecutionPolicy Bypass -File build.ps1`. The script compiles the native Windows GUI and host helper and runs protocol tests. It does not install anything on either PC. The bundled `host/ViGEmClient.dll` comes from [vgamepad's ViGEm client binary](https://github.com/yannbouteiller/vgamepad/tree/main/vgamepad/win/vigem/client/x64); its MIT license is in `host/LICENSE-ViGEmClient.txt`.

## References

- [Dolphin controller setup](https://dolphin-emu.org/docs/guides/configuring-controllers/)
- [Dolphin's DSU protocol implementation](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPProto.h)
- [DSU protocol description](https://github.com/v1993/cemuhook-protocol/blob/master/README.md)
- [Wii Remote HID reports](https://www.wiibrew.org/wiki/Wiimote)
