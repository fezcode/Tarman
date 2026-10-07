<#
.SYNOPSIS
    Configure and build Tarman (tarman.exe) with CMake + Ninja using the
    MSYS2 MinGW-w64 toolchain.

.DESCRIPTION
    Wraps the documented build:

        cmake -G Ninja -B build
        cmake --build build

    The MSYS2 mingw64 bin directory is auto-detected and prepended to PATH so
    cmake finds gcc, ninja and the static raylib/glfw libraries even from a
    vanilla PowerShell session. raylib is linked statically, so build\tarman.exe
    runs on its own with no DLLs next to it.

.PARAMETER Config
    CMAKE_BUILD_TYPE. Default: Release.

.PARAMETER Clean
    Delete the build directory before configuring.

.PARAMETER Run
    Launch build\tarman.exe after a successful build.

.PARAMETER MingwBin
    Path to the MSYS2 mingw64 bin directory. Auto-detected when omitted.

.EXAMPLE
    .\build.ps1 -Run
#>

[CmdletBinding()]
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Run,
    [string]$MingwBin
)

$ErrorActionPreference = "Stop"

$root     = $PSScriptRoot
$buildDir = Join-Path $root "build"

# --- locate the MSYS2 MinGW-w64 toolchain --------------------------------
if (-not $MingwBin) {
    $gcc = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gcc) {
        $MingwBin = Split-Path $gcc.Source
    } else {
        $MingwBin = @(
            "D:\Apps\msys64\mingw64\bin",
            "C:\msys64\mingw64\bin",
            "C:\tools\msys64\mingw64\bin"
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
}
if (-not $MingwBin -or -not (Test-Path $MingwBin)) {
    throw "MSYS2 MinGW-w64 toolchain not found. Pass -MingwBin <path to mingw64\bin>."
}
$env:PATH = "$MingwBin;$env:PATH"
foreach ($tool in @("cmake", "ninja", "gcc")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool '$tool' not found under $MingwBin."
    }
}

Write-Host "Toolchain : $MingwBin" -ForegroundColor Cyan
Write-Host "Config    : $Config"   -ForegroundColor Cyan
Write-Host "Build dir : $buildDir" -ForegroundColor Cyan

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

# --- stop instances running from this build --------------------------------
# The linker cannot overwrite a running exe; it fails part-way and leaves the
# old binary behind. Only processes started from this repo's build\ are touched.
$running = Get-Process -Name tarman -ErrorAction SilentlyContinue |
           Where-Object { $_.Path -and $_.Path.StartsWith($buildDir, [StringComparison]::OrdinalIgnoreCase) }
if ($running) {
    Write-Host "Stopping $(@($running).Count) running Tarman instance(s) from $buildDir ..." -ForegroundColor Yellow
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 500
}

# --- configure (only when there is no cache) -------------------------------
if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    Write-Host "Configuring (cmake -G Ninja) ..." -ForegroundColor Green
    & cmake -G Ninja -B $buildDir -S $root "-DCMAKE_BUILD_TYPE=$Config"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
} else {
    Write-Host "Reusing CMake cache (pass -Clean to reconfigure)." -ForegroundColor DarkGray
}

# --- build -------------------------------------------------------------------
Write-Host "Building (ninja) ..." -ForegroundColor Green
& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $buildDir "tarman.exe"
if (-not (Test-Path $exe)) { throw "build reported success but $exe is missing." }
Write-Host "Built: $exe" -ForegroundColor Green

if ($Run) {
    Write-Host "Launching tarman.exe ..." -ForegroundColor Green
    & $exe
}
