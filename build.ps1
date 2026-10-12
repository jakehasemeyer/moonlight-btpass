$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
$testExePath = Join-Path $env:TEMP "moonlight-btpass-test-$PID.exe"
try {
    g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ `
        main.cpp -o moonlight-btpass.exe -lsetupapi -lhid -lws2_32
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
    g++ -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type -static -static-libgcc -static-libstdc++ `
        host/host.cpp -o host/moonlight-btpass-host.exe -lws2_32
    if ($LASTEXITCODE -ne 0) { throw 'Host build failed' }
    g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-function -static -static-libgcc -static-libstdc++ `
        test_protocol.cpp -o $testExePath -lsetupapi -lhid -lws2_32
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed' }
    & $testExePath
    if ($LASTEXITCODE -ne 0) { throw 'Protocol tests failed' }
    Write-Host 'Built moonlight-btpass.exe and passed protocol tests.'
} finally {
    Remove-Item -LiteralPath $testExePath -ErrorAction SilentlyContinue
    Pop-Location
}
