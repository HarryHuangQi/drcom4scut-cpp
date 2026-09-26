$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Gxx = 'C:\msys64\ucrt64\bin\g++.exe'
$Windres = 'C:\msys64\ucrt64\bin\windres.exe'
$Output = Join-Path $Root 'bin'

if (-not (Test-Path -LiteralPath $Gxx)) {
    throw 'MSYS2 UCRT64 g++ was not found. Install MSYS2 and the ucrt64 toolchain first.'
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null

& $Gxx -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -static -static-libgcc -static-libstdc++ `
    (Join-Path $PSScriptRoot 'main_windows.cpp') `
    -o (Join-Path $Output 'drcom4scut.exe') `
    -lcrypto -lws2_32 -liphlpapi -lcrypt32 -lbcrypt
if ($LASTEXITCODE -ne 0) { throw 'Authentication core build failed.' }

& $Windres -I $PSScriptRoot `
    (Join-Path $PSScriptRoot 'resources.rc') `
    -O coff -o (Join-Path $PSScriptRoot 'resources.o')
if ($LASTEXITCODE -ne 0) { throw 'Windows resource build failed.' }

& $Gxx -std=c++17 -O2 -Wall -Wextra -Wpedantic `
    -municode -mwindows -static -static-libgcc -static-libstdc++ `
    (Join-Path $PSScriptRoot 'gui_windows.cpp') `
    (Join-Path $PSScriptRoot 'resources.o') `
    -o (Join-Path $Output 'drcom4scut-gui.exe') `
    -lws2_32 -liphlpapi -lshell32 -lcomctl32 -lole32 -luuid
if ($LASTEXITCODE -ne 0) { throw 'GUI build failed.' }

& (Join-Path $Output 'drcom4scut.exe') --help | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Built client did not start.' }

Write-Host "Windows binaries are in $Output"
