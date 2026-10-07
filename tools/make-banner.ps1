<#
.SYNOPSIS
  Compose banner-image.png for the README: the logo, name and tagline on the
  left, a crop of the Analysis page on the right.

.DESCRIPTION
  Take the screenshot first, in demo mode so it shows no real machine data:
    .\build\tarman.exe --demo --page analysis --window 60 --size 1600x960 --screenshot docs\screenshots\analysis.png --wait 75
#>
param([string]$Root = (Split-Path $PSScriptRoot -Parent))
Add-Type -AssemblyName System.Drawing

$logo = [Drawing.Image]::FromFile("$Root\resources\logo-1024.png")

$W = 1800; $H = 720
$bmp = New-Object Drawing.Bitmap $W, $H
$g = [Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAliasGridFit'
$g.Clear([Drawing.Color]::FromArgb(0x0B, 0x0E, 0x13))

# faint 40px grid, like the graph backgrounds
$gridPen = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(16, 255, 255, 255)), 1
for ($x = 0; $x -lt $W; $x += 40) { $g.DrawLine($gridPen, $x, 0, $x, $H) }
for ($y = 0; $y -lt $H; $y += 40) { $g.DrawLine($gridPen, 0, $y, $W, $y) }

function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc($x, $y, 2*$r, 2*$r, 180, 90); $p.AddArc($x+$w-2*$r, $y, 2*$r, 2*$r, 270, 90)
    $p.AddArc($x+$w-2*$r, $y+$h-2*$r, 2*$r, 2*$r, 0, 90); $p.AddArc($x, $y+$h-2*$r, 2*$r, 2*$r, 90, 90)
    $p.CloseFigure(); return $p
}

# --- screenshot panel on the right, bleeding off the edge ---
$shot = [Drawing.Image]::FromFile("$Root\docs\screenshots\analysis.png")
$src = New-Object Drawing.Rectangle 232, 34, 1368, 530          # sidebar and status bar cropped away
$scale = 1.0
$dw = [int]($src.Width * $scale); $dh = [int]($src.Height * $scale)
$dx = 860; $dy = [int](($H - $dh) / 2)
for ($s = 18; $s -ge 1; $s -= 3) {                               # soft shadow
    $sp = RoundRect ($dx - $s/2) ($dy + 10 - $s/2 + 6) ($dw + $s) ($dh + $s) (16 + $s/2)
    $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(14, 0, 0, 0))), $sp)
}
$clip = RoundRect $dx $dy $dw $dh 16
$g.SetClip($clip)
$g.DrawImage($shot, (New-Object Drawing.Rectangle $dx, $dy, $dw, $dh), $src, 'Pixel')
$g.ResetClip()
$g.DrawPath((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(0x2A, 0x33, 0x42)), 1.5), $clip)

# --- text column ---
$g.DrawImage($logo, 92, 164, 112, 112)
$white = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(0xE8, 0xEB, 0xF1))
$dim   = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(0x9A, 0xA3, 0xB2))
$faint = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(0x6B, 0x74, 0x84))
$amber = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(0xFF, 0xB3, 0x47))
$title = New-Object Drawing.Font "Segoe UI Semibold", 74, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)
$tag   = New-Object Drawing.Font "Segoe UI", 28, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)
$small = New-Object Drawing.Font "Cascadia Mono", 18, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)
$g.DrawString("Tarman", $title, $white, 222, 166)
$g.DrawString("Task manager and resource monitor", $tag, $dim, 98, 318)
$g.DrawString("for Windows, with a", $tag, $dim, 98, 358)
$g.DrawString("10-minute moving window.", $tag, $amber, 98 + $g.MeasureString("for Windows, with a ", $tag).Width - 8, 358)
$g.DrawString("Processes  /  Performance  /  Analysis", $small, $faint, 100, 448)
$g.DrawString("Network  /  Disk  /  Memory  /  Events", $small, $faint, 100, 478)
$g.DrawString("Startup  /  Services  /  Firmware", $small, $faint, 100, 508)

$bmp.Save("$Root\banner-image.png", [Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose(); $shot.Dispose()
"banner written"
