<#
.SYNOPSIS
  Read, set, or bump the Tarman release version everywhere it lives, then
  verify those places agree.

.DESCRIPTION
  src/version.h (TARMAN_VERSION) is the source of truth: the app prints it for
  --version and the About box, and CMake parses it into the exe's VERSIONINFO.
  Two inputs cannot read the header, so this script keeps them in step:
    - forge.toml  [app] version (the wizard strings and registry value read
                  it back as ${app.version})
    - README.md   the Tarman-Setup-x.y.z.exe installer name
  With -RequireBuild it also checks the version stamped into build\tarman.exe,
  so a stale build can't ship under a new number.

.EXAMPLE
  .\version.ps1                  # report every location + consistency
  .\version.ps1 -Bump patch      # 0.1.0 -> 0.1.1 everywhere, then verify
  .\version.ps1 -Set 0.2.0       # set an exact version, then verify
  .\version.ps1 -Bump minor -DryRun
  .\version.ps1 -RequireBuild    # also verify build\tarman.exe
#>
param(
    [string] $Set,
    [ValidateSet('patch', 'minor', 'major')]
    [string] $Bump,
    [switch] $DryRun,
    [switch] $RequireBuild,
    [string] $RepoRoot = $PSScriptRoot
)

$ErrorActionPreference = "Stop"

function Parse-SemVer([string] $text) {
    if ($text -notmatch '^\s*(\d+)\.(\d+)\.(\d+)\s*$') { throw "Not a valid x.y.z version: '$text'" }
    [pscustomobject]@{ Major = [int]$Matches[1]; Minor = [int]$Matches[2]; Patch = [int]$Matches[3] }
}

function Step-SemVer([string] $current, [string] $kind) {
    $v = Parse-SemVer $current
    switch ($kind) {
        'patch' { $v.Patch++ }
        'minor' { $v.Minor++; $v.Patch = 0 }
        'major' { $v.Major++; $v.Minor = 0; $v.Patch = 0 }
    }
    "{0}.{1}.{2}" -f $v.Major, $v.Minor, $v.Patch
}

$utf8 = New-Object System.Text.UTF8Encoding $false
function Read-Text([string] $path) { [System.IO.File]::ReadAllText($path) }
function Write-Text([string] $path, [string] $text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

# Each location: a file, a regex whose group 1 is the version, and whether it
# must exist (README may legitimately not mention an installer yet).
$locations = @(
    @{ Name = "src/version.h";        Path = "src\version.h"; Pattern = '#define\s+TARMAN_VERSION\s+"(\d+\.\d+\.\d+)"'; Required = $true;  All = $false }
    @{ Name = "forge.toml [app]";     Path = "forge.toml";    Pattern = '(?m)^version\s*=\s*"(\d+\.\d+\.\d+)"';           Required = $true;  All = $false }
    @{ Name = "README.md installer";  Path = "README.md";     Pattern = 'Tarman-Setup-(\d+\.\d+\.\d+)\.exe';               Required = $false; All = $true }
)

function Get-Versions {
    foreach ($loc in $locations) {
        $full = Join-Path $RepoRoot $loc.Path
        if (-not (Test-Path $full)) {
            [pscustomobject]@{ Name = $loc.Name; Version = $null; Missing = $true; Required = $loc.Required }
            continue
        }
        $ms = [regex]::Matches((Read-Text $full), $loc.Pattern)
        if ($ms.Count -eq 0) {
            [pscustomobject]@{ Name = $loc.Name; Version = $null; Missing = $true; Required = $loc.Required }
            continue
        }
        foreach ($m in $ms) {
            [pscustomobject]@{ Name = $loc.Name; Version = $m.Groups[1].Value; Missing = $false; Required = $loc.Required }
            if (-not $loc.All) { break }
        }
    }
}

function Set-Everywhere([string] $new) {
    foreach ($loc in $locations) {
        $full = Join-Path $RepoRoot $loc.Path
        if (-not (Test-Path $full)) { continue }
        $text = Read-Text $full
        $count = $(if ($loc.All) { -1 } else { 1 })
        $re = New-Object System.Text.RegularExpressions.Regex $loc.Pattern
        $updated = $re.Replace($text, {
            param($m)
            $g = $m.Groups[1]
            $m.Value.Substring(0, $g.Index - $m.Index) + $new + $m.Value.Substring($g.Index - $m.Index + $g.Length)
        }, $count)
        if ($updated -ne $text) {
            if ($DryRun) { Write-Host "  would update $($loc.Name)" -ForegroundColor DarkGray }
            else { Write-Text $full $updated; Write-Host "  updated $($loc.Name)" -ForegroundColor DarkGray }
        }
    }
}

$current = (Get-Versions | Where-Object { $_.Name -eq "src/version.h" -and -not $_.Missing } | Select-Object -First 1).Version
if (-not $current) { throw "src/version.h has no TARMAN_VERSION" }

$target = $null
if ($Set)  { $target = (Parse-SemVer $Set) | ForEach-Object { "{0}.{1}.{2}" -f $_.Major, $_.Minor, $_.Patch } }
if ($Bump) { $target = Step-SemVer $current $Bump }
if ($target) {
    Write-Host "Version $current -> $target$(if ($DryRun) { ' (dry run)' })" -ForegroundColor Cyan
    Set-Everywhere $target
    if ($DryRun) { exit 0 }
}

$expected = $(if ($target) { $target } else { $current })
$ok = $true
Write-Host "Tarman version: $expected" -ForegroundColor Cyan
foreach ($v in Get-Versions) {
    if ($v.Missing) {
        if ($v.Required) { Write-Host ("  {0,-22} MISSING" -f $v.Name) -ForegroundColor Red; $ok = $false }
        else { Write-Host ("  {0,-22} (not mentioned)" -f $v.Name) -ForegroundColor DarkGray }
        continue
    }
    $good = $v.Version -eq $expected
    if (-not $good) { $ok = $false }
    Write-Host ("  {0,-22} {1}" -f $v.Name, $v.Version) -ForegroundColor $(if ($good) { "Green" } else { "Red" })
}

if ($RequireBuild) {
    $exe = Join-Path $RepoRoot "build\tarman.exe"
    if (-not (Test-Path $exe)) {
        Write-Host ("  {0,-22} MISSING (run .\build.ps1)" -f "build\tarman.exe") -ForegroundColor Red
        $ok = $false
    } else {
        $built = (Get-Item $exe).VersionInfo.ProductVersion
        $good = $built -eq $expected
        if (-not $good) { $ok = $false }
        Write-Host ("  {0,-22} {1}" -f "build\tarman.exe", $built) -ForegroundColor $(if ($good) { "Green" } else { "Red" })
    }
}

if (-not $ok) { Write-Host "Version mismatch." -ForegroundColor Red; exit 1 }
Write-Host "All locations agree." -ForegroundColor Green
exit 0
