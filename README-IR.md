# Moonlight Wii Remote Bridge — IR sensor-bar extension

This package adds sensor-bar pointing to https://github.com/jakehasemeyer/moonlight-btpass.
It preserves the original DSU buttons, accelerometer, MotionPlus gyro, multi-remote support, and host rumble helper.

## Build on the Moonlight client PC (Windows)

1. Extract this ZIP to a normal folder.
2. Install MinGW-w64 `g++` if it is not already available on `PATH` (the original project's build also requires it).
3. Run `powershell -ExecutionPolicy Bypass -File .\build-ir.ps1` in the extracted folder.
4. The script downloads the public upstream repository, applies the IR source changes and builds it. The generated `moonlight-btpass-ir-built.zip` contains the updated source, `moonlight-btpass-ir.exe`, the host helper, and the IR Dolphin profiles. Unzip that second ZIP and run the new IR EXE on the client, not the original upstream EXE.
5. The Wiimote's physical IR camera needs a powered sensor bar in view (a Wii/Wii U sensor bar or powered USB IR sensor bar). The bar does not connect to the PC; it only emits IR light.

The build script requires an internet connection for the one-time GitHub source download. It never writes to your GitHub repository.
If `g++` is missing, the script still prepares the patched source in `moonlight-btpass-ir`; after installing MinGW-w64, run `powershell -ExecutionPolicy Bypass -File .\moonlight-btpass-ir\build.ps1` and rename the newly built `moonlight-btpass.exe` to `moonlight-btpass-ir.exe`.

## Dolphin setup on the host PC

Continue following the original repository's README for DSU connection and optional rumble helper.
Copy `profiles/Wiimote/Moonlight Wii Remote IR.ini` or `Moonlight Wii Remote Plus IR.ini` to Dolphin's `Config/Profiles/Wiimote` folder, load the appropriate new IR profile, and select the DSU device for that slot. Use Emulated Wii Remote (not Real Wii Remote).

The new profiles map `Touch X/Y` (from the Wii Remote IR camera) to the emulated Wii cursor. Gyro input remains available in the Plus profile but is not used to synthesize pointing; the cursor works with a standard non-MotionPlus remote as well.

## Implementation

The bridge initializes the Wii Remote IR camera in the correct data format for each remote. It uses report `0x33` (accelerometer + 12-byte extended IR) for non-MotionPlus remotes and report `0x37` (accelerometer + 10-byte basic IR + 6-byte extension) for MotionPlus remotes. The respective decoders convert the detected IR dot positions into DSU touch coordinates.

Only source parsing tests were run in this environment. Windows compilation, Wii Remote hardware, and in-game Dolphin cursor behavior have not been verified. If pointer movement is inverted in your Dolphin build, swap the appropriate `IR/Left`/`IR/Right` or `IR/Up`/`IR/Down` mapping.
