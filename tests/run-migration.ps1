# Exercise data migration against scratch files inside build/, never real user settings.
[CmdletBinding()]
param([string]$MingwBin = 'D:\Apps\msys64\mingw64\bin')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:PATH = "$MingwBin;$env:PATH"
$testExe = Join-Path $root 'build/migration-test.exe'
New-Item -ItemType Directory -Force (Join-Path $root 'build') | Out-Null
& gcc -std=c11 -O2 -ffunction-sections -fdata-sections (Join-Path $PSScriptRoot 'migration.c') `
    '-Wl,--gc-sections' -o $testExe -lpsapi -lpdh -liphlpapi -lws2_32 -lwtsapi32 -ladvapi32 `
    -lshell32 -lole32 -loleaut32 -luuid -lwbemuuid -ltbs -lwevtapi -ldwmapi -ldbghelp `
    -lversion -luser32 -lgdi32 -lwinmm
if ($LASTEXITCODE -ne 0) { throw "Migration test compilation failed ($LASTEXITCODE)" }
$fixture = Join-Path $root ('build/migration-fixture-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
Push-Location $fixture
try {
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "Migration test failed ($LASTEXITCODE); fixtures: $fixture" }
} finally {
    Pop-Location
}
