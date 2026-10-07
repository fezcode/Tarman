<#
.SYNOPSIS
  Build the Tarman Windows installer (Tarman-Setup-<version>.exe) with Forge.

.DESCRIPTION
  Builds the app (unless -SkipBuild), verifies the version is consistent across
  the sources AND the built exe, makes sure a GUI-subsystem forge.exe exists in
  the sibling ..\Forge project, then validates forge.toml and stamps the payload
  into dist\installer\Tarman-Setup-<version>.exe. Superseded installers in
  dist\installer are pruned once the new one is verified (each is already a
  GitHub release) unless -KeepOldInstallers is passed.

.EXAMPLE
  .\installer.ps1
  .\installer.ps1 -SkipBuild
#>
param(
  [string]$ForgeDir = "..\Forge",
  [switch]$SkipBuild,
  [switch]$KeepOldInstallers
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$forgeRoot   = Resolve-Path $ForgeDir
$forgeGui    = Join-Path $forgeRoot "build\forge.exe"
$forgeSrc    = Join-Path $forgeRoot "cmd\forge"
$uninstaller = Join-Path $forgeRoot "build\uninstall.exe"
$outDir      = Join-Path $PSScriptRoot "dist\installer"

if (-not $SkipBuild) {
    Write-Host "[1/4] Building Tarman..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build.ps1")
    if (-not $?) { throw "build.ps1 failed" }
} else {
    Write-Host "[1/4] Skipping build (-SkipBuild)" -ForegroundColor DarkGray
}

# Checks the built exe too: sources agreeing with each other says nothing about
# the version the installed app will report. With -SkipBuild a stale build would
# otherwise ship under the new number.
Write-Host "`n[2/4] Verifying the version is consistent..." -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "version.ps1") -RequireBuild
if ($LASTEXITCODE -ne 0) {
    throw "Version mismatch. If only build\tarman.exe disagrees, re-run .\build.ps1; otherwise run .\version.ps1 -Set x.y.z"
}
$version = (Select-String -Path (Join-Path $PSScriptRoot "src\version.h") -Pattern '"(\d+\.\d+\.\d+)"').Matches[0].Groups[1].Value

foreach ($f in @("build\tarman.exe", "build\glfw3.dll", "build\LICENSE.txt", "LICENSE.txt", "resources\tarman.ico")) {
    if (-not (Test-Path (Join-Path $PSScriptRoot $f))) { throw "Missing payload file: $f" }
}

Write-Host "`n[3/4] Ensuring a GUI-subsystem forge.exe..." -ForegroundColor Cyan
if (-not (Test-Path $uninstaller)) {
    throw "Missing $uninstaller. Run 'gobake build' in $forgeRoot first."
}
$needBuild = -not (Test-Path $forgeGui)
if (-not $needBuild) {
    $srcLatest = (Get-ChildItem $forgeSrc -Recurse -Filter *.go |
                  Sort-Object LastWriteTime -Descending |
                  Select-Object -First 1).LastWriteTime
    if ((Get-Item $forgeGui).LastWriteTime -lt $srcLatest) { $needBuild = $true }
}
if ($needBuild) {
    Write-Host "  building $forgeGui..." -ForegroundColor DarkGray
    Push-Location $forgeRoot
    try {
        # -H windowsgui: no console window when the installer runs. Not stripped:
        # a symbol-free Go binary that unpacks an exe is the shape Defender's ML
        # classifier scores as a dropper.
        go build -tags "desktop,production" -ldflags "-H windowsgui" -o build\forge.exe ./cmd/forge/
        if ($LASTEXITCODE -ne 0) { throw "go build forge.exe failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
} else {
    Write-Host "  up to date: $forgeGui" -ForegroundColor DarkGray
}

Write-Host "`n[4/4] Building Setup.exe..." -ForegroundColor Cyan

# forge.exe is a GUI-subsystem binary, so $LASTEXITCODE is not propagated
# through PowerShell's call operator. Use Start-Process -Wait -PassThru instead.
function Invoke-Forge {
    param([string[]]$ForgeArgs)
    $p = Start-Process -FilePath $forgeGui -ArgumentList $ForgeArgs -Wait -PassThru -NoNewWindow
    if ($p.ExitCode -ne 0) { throw "forge $($ForgeArgs -join ' ') failed (exit $($p.ExitCode))" }
}

Invoke-Forge @("validate", "forge.toml")
Invoke-Forge @("build", "--out", "`"$outDir`"")

$setup = Join-Path $outDir "Tarman-Setup-$version.exe"
if (-not (Test-Path $setup)) { throw "$setup not found after forge build." }
$item = Get-Item $setup
if ($item.LastWriteTime -lt (Get-Date).AddMinutes(-5)) { throw "$setup is stale; forge did not rebuild it." }

# The PE subsystem must be GUI (2); a console-subsystem setup flashes a terminal.
$bytes    = [System.IO.File]::ReadAllBytes($setup)
$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
$subsys   = [BitConverter]::ToUInt16($bytes, $peOffset + 24 + 68)
$subName  = switch ($subsys) { 2 { "GUI" } 3 { "CONSOLE" } default { "OTHER($subsys)" } }
if ($subsys -ne 2) { throw "Setup subsystem is $subName, not GUI." }

if (-not $KeepOldInstallers) {
    Get-ChildItem $outDir -Filter "Tarman-Setup-*.exe" |
        Where-Object { $_.Name -ne $item.Name } |
        ForEach-Object { Write-Host "  pruning superseded $($_.Name)" -ForegroundColor DarkGray; Remove-Item $_.FullName }
}

Write-Host ""
Write-Host "Done. Output: $setup" -ForegroundColor Green
Write-Host ("  size:      {0} MB" -f [math]::Round($item.Length / 1MB, 1))
Write-Host ("  subsystem: {0}" -f $subName)
