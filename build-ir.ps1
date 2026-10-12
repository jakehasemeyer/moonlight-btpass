$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
$root = Join-Path $kit 'moonlight-btpass-ir'
if (-not (Test-Path (Join-Path $root 'main.cpp'))) {
    $archive = Join-Path $kit 'upstream.zip'
    Invoke-WebRequest 'https://github.com/jakehasemeyer/moonlight-btpass/archive/refs/heads/main.zip' -OutFile $archive
    Expand-Archive $archive -DestinationPath $kit -Force
    Move-Item (Join-Path $kit 'moonlight-btpass-main') $root
    Remove-Item $archive
}
$main = Join-Path $root 'main.cpp'
$source = [IO.File]::ReadAllText($main).Replace("`r`n", "`n")
if ($source.Contains('#include "ir_sensor.h"')) { throw 'This source has already been patched.' }
function Patch([string]$before, [string]$after) {
    if (-not $script:source.Contains($before)) { throw "Upstream source changed; missing: $before" }
    $script:source = $script:source.Replace($before, $after)
}
Patch '    std::thread reader;' @'
    std::thread reader;
    bool ir_visible = false;
    uint8_t ir_touch_id = 0;
    uint16_t ir_x = 0, ir_y = 0;
    std::atomic<bool> ir_enabled{false};
'@
Patch 'static bool read_register(Pad& pad' @'
#include "ir_sensor.h"

static bool read_register(Pad& pad
'@
Patch '    if (!write_report(pad, 0x12, 0x04, pad.motion_plus ? 0x35 : 0x31)) {' @'
    pad.ir_enabled = init_ir(pad, pad.motion_plus);
    const uint8_t mode = pad.ir_enabled ? (pad.motion_plus ? 0x37 : 0x33) : (pad.motion_plus ? 0x35 : 0x31);
    if (!write_report(pad, 0x12, 0x04, mode)) {
'@
Patch '(bytes[0] != 0x30 && bytes[0] != 0x31 && bytes[0] != 0x35)' '(bytes[0] != 0x30 && bytes[0] != 0x31 && bytes[0] != 0x33 && bytes[0] != 0x35 && bytes[0] != 0x37)'
Patch '(bytes[0] == 0x31 || bytes[0] == 0x35)' '(bytes[0] == 0x31 || bytes[0] == 0x33 || bytes[0] == 0x35 || bytes[0] == 0x37)'
Patch '    if (pad.motion_plus && count >= 12 && bytes[0] == 0x35 && (bytes[11] & 0x02)) {' @'
    if (pad.ir_enabled && (bytes[0] == 0x33 || bytes[0] == 0x37) &&
        count >= (bytes[0] == 0x37 ? 22u : 18u)) {
        const IrCursor cursor = bytes[0] == 0x37 ? decode_ir_basic(bytes + 6) :
                                decode_ir_extended(bytes + 6);
        if (cursor.visible && !pad.ir_visible) ++pad.ir_touch_id;
        pad.ir_visible = cursor.visible;
        pad.ir_x = cursor.x;
        pad.ir_y = cursor.y;
    }
    const size_t extension_offset = bytes[0] == 0x37 ? 16 : 6;
    if (pad.motion_plus && (bytes[0] == 0x35 || bytes[0] == 0x37) &&
        count >= extension_offset + 6 && (bytes[extension_offset + 5] & 0x02)) {
'@
Patch '        const uint8_t* extension = bytes + 6;' '        const uint8_t* extension = bytes + extension_offset;'
Patch '            write_report(pad, 0x12, 0x04, pad.motion_plus ? 0x35 : 0x31);' @'
            write_report(pad, 0x12, 0x04, pad.ir_enabled ? (pad.motion_plus ? 0x37 : 0x33) :
                                               (pad.motion_plus ? 0x35 : 0x31));
'@
Patch '    pad.gyro_ready = false;' @'
    pad.gyro_ready = false;
    pad.ir_visible = false;
'@
Patch '    put64(p + 48, pad.motion_timestamp_us);' @'
    p[36] = pad.ir_visible ? 1 : 0;
    p[37] = pad.ir_touch_id;
    if (pad.ir_visible) {
        put16(p + 38, pad.ir_x);
        put16(p + 40, pad.ir_y);
    }
    put64(p + 48, pad.motion_timestamp_us);
'@
Patch '            set_status(text.c_str());' @'
            int ir_ready = 0, ir_visible = 0;
            for (const auto& pad : app.pads) {
                std::lock_guard<std::mutex> lock(pad->mutex);
                ir_ready += pad->ir_enabled ? 1 : 0;
                ir_visible += pad->ir_visible ? 1 : 0;
            }
            text += L" IR enabled " + std::to_wstring(ir_ready) + L", pointing " + std::to_wstring(ir_visible) + L".";
            set_status(text.c_str());
'@
[IO.File]::WriteAllText($main, $source, (New-Object System.Text.UTF8Encoding($false)))
Copy-Item (Join-Path $kit 'ir_parse.h') $root
Copy-Item (Join-Path $kit 'ir_sensor.h') $root
Copy-Item (Join-Path $kit 'profiles/Wiimote/*') (Join-Path $root 'profiles/Wiimote') -Force
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Host "Patched source is ready in $root. Install MinGW-w64 g++ and run its build.ps1 to create the EXE."
    exit 0
}
& (Join-Path $root 'build.ps1')
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
Copy-Item (Join-Path $root 'moonlight-btpass.exe') (Join-Path $root 'moonlight-btpass-ir.exe') -Force
$builtZip = Join-Path $kit 'moonlight-btpass-ir-built.zip'
Compress-Archive -Path (Join-Path $root '*') -DestinationPath $builtZip -Force
Write-Host "Created $builtZip"
