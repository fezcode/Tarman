<#
.SYNOPSIS
  Render the Tarman logo -- the one source for every place it appears.

.DESCRIPTION
  The mark: a slate tile carrying a ring of ten segments -- the ten minutes
  Tarman keeps -- fading from the oldest to the newest like a moving window,
  with a live graph inside and an amber dot on its spike: the "pinned moment"
  colour the app uses when you click a graph.

  Every size is drawn natively (not downscaled) so 16 px stays crisp. Outputs:
    resources/tarman.ico     16..256 px PNG frames: exe resource, shortcuts, installer
    resources/logo-1024.png  README / docs
    src/logo_png.h           32, 64 and 256 px PNGs embedded in the app (window
                             icon, title bar, About)

.EXAMPLE
  .\tools\make-logo.ps1
#>
param([string]$Root = (Split-Path $PSScriptRoot -Parent))

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
# The ring is drawn per pixel: GDI+ approximates arcs with Beziers, which leaves
# faint seams where the gradient crosses its quadrant points.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class TarmanRing {
    static double Cl(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    public static void Draw(Bitmap b, double cx, double cy, double R, double w, double start, double sweep,
                            int[] deep, int[] mid, int[] end, double a0, double a1, double gamma) {
        int S = b.Width;
        var d = b.LockBits(new Rectangle(0, 0, S, S), ImageLockMode.ReadWrite, PixelFormat.Format32bppArgb);
        var px = new byte[d.Stride * S];
        Marshal.Copy(d.Scan0, px, 0, px.Length);
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy, r = Math.Sqrt(dx * dx + dy * dy);
            double cov = Cl(w / 2 - Math.Abs(r - R) + 0.5);
            if (cov <= 0) continue;
            double ang = Math.Atan2(dy, dx) * 180 / Math.PI;     // y down: clockwise, like GDI+
            double rel = ang - start; while (rel < 0) rel += 360; while (rel >= 360) rel -= 360;
            double arcpx = Math.PI / 180 * r;
            if (rel > sweep) {
                double de = (rel - sweep) * arcpx, ds = (360 - rel) * arcpx;
                cov *= Cl(0.5 - Math.Min(de, ds));
            } else {
                cov *= Cl(rel * arcpx + 0.5) * Cl((sweep - rel) * arcpx + 0.5);
            }
            if (cov <= 0) continue;
            double t = Cl(rel / sweep);
            int[] c0 = t < 0.6 ? deep : mid, c1 = t < 0.6 ? mid : end;
            double u = t < 0.6 ? t / 0.6 : (t - 0.6) / 0.4;
            double a = (a0 + (a1 - a0) * Math.Pow(t, gamma)) * cov;
            int i = y * d.Stride + x * 4;
            for (int k = 0; k < 3; k++) {
                double c = c0[2 - k] + (c1[2 - k] - c0[2 - k]) * u;   // BGRA in memory
                px[i + k] = (byte)Math.Round(c * a + px[i + k] * (1 - a));
            }
            px[i + 3] = (byte)Math.Round(255 * (a + px[i + 3] / 255.0 * (1 - a)));
        }
        Marshal.Copy(px, 0, d.Scan0, px.Length);
        b.UnlockBits(d);
    }
}
"@

function C([int]$r, [int]$g, [int]$b, [int]$a = 255) { [Drawing.Color]::FromArgb($a, $r, $g, $b) }
function Lerp($a, $b, [double]$t) {
    C ([int]($a.R + ($b.R - $a.R) * $t)) ([int]($a.G + ($b.G - $a.G) * $t)) ([int]($a.B + ($b.B - $a.B) * $t)) ([int]($a.A + ($b.A - $a.A) * $t))
}

$CYAN   = C 0x4C 0xC2 0xFF
$VIOLET = C 0x8B 0x7C 0xFF
$PINK   = C 0xFF 0x6B 0x9A
$AMBER  = C 0xFF 0xB3 0x47
$CYAN_DEEP = C 0x1E 0x6F 0xB8

function RingColor([double]$t) {        # cyan -> violet -> pink along the ring
    if ($t -lt 0.5) { return Lerp $CYAN $VIOLET ($t * 2) }
    return Lerp $VIOLET $PINK (($t - 0.5) * 2)
}

function RoundRect([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $p = New-Object Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Render([int]$S) {
    $bmp = New-Object Drawing.Bitmap $S, $S, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'; $g.CompositingQuality = 'HighQuality'
    $g.Clear([Drawing.Color]::Transparent)
    $small = $S -le 24

    # --- tile: deep slate, lit slightly from above ---------------------------
    $inset = $(if ($small) { 0 } else { [Math]::Max(1.0, $S * 0.015) })
    $tile = RoundRect $inset $inset ($S - 2 * $inset) ($S - 2 * $inset) ($S * 0.23)
    $tr = New-Object Drawing.RectangleF 0, ([float]($inset - 1)), $S, ([float]($S - 2 * $inset + 2))
    $tb = New-Object Drawing.Drawing2D.LinearGradientBrush $tr, (C 0x1D 0x26 0x36), (C 0x0A 0x0E 0x16), ([float]90)
    $tb.WrapMode = 'TileFlipXY'
    $g.FillPath($tb, $tile)
    if ($S -ge 32) { $g.DrawPath((New-Object Drawing.Pen (C 0x34 0x40 0x55), ([float][Math]::Max(1, $S / 110))), $tile) }

    # --- the window: a 300 degree arc, tail fading, head bright --------------
    $cx = $S / 2.0; $cy = $S * 0.5
    $w = $S * $(if ($small) { 0.13 } else { 0.092 })
    $R = $S * $(if ($small) { 0.33 } else { 0.315 })
    $rect = New-Object Drawing.RectangleF ([float]($cx - $R)), ([float]($cy - $R)), ([float](2 * $R)), ([float](2 * $R))
    $start = 125.0; $sweep = 290.0             # GDI+ angles: clockwise from +x; gap at the bottom
    [TarmanRing]::Draw($bmp, $cx, $cy, $R, $w, $start, $sweep, @(0x1E, 0x6F, 0xB8), @(0x4C, 0xC2, 0xFF), @(0x8B, 0x7C, 0xFF),
                       $(if ($small) { 0.55 } else { 0.18 }), 1.0, $(if ($small) { 1.0 } else { 1.15 }))

    # --- the pulse ------------------------------------------------------------
    $ri = $R - $w / 2
    $norm = @(@(-0.62, 0.10), @(-0.30, 0.10), @(-0.17, -0.06), @(-0.05, 0.22), @(0.12, -0.46), @(0.28, 0.38), @(0.40, 0.10), @(0.62, 0.10))
    if ($small) { $norm = @(@(-0.62, 0.12), @(-0.12, 0.12), @(0.08, -0.50), @(0.30, 0.42), @(0.42, 0.12), @(0.62, 0.12)) }
    $pts = $norm | ForEach-Object { New-Object Drawing.PointF ([float]($cx + $_[0] * $ri)), ([float]($cy + $_[1] * $ri)) }
    $lw = [Math]::Max(1.25, $S * $(if ($small) { 0.075 } else { 0.047 }))
    if ($S -ge 48) {
        # a soft radial glow behind the pulse
        $gr = $ri * 0.95
        $gpath = New-Object Drawing.Drawing2D.GraphicsPath
        $gpath.AddEllipse([float]($cx - $gr), [float]($cy - $gr), [float](2 * $gr), [float](2 * $gr))
        $pg = New-Object Drawing.Drawing2D.PathGradientBrush $gpath
        $pg.CenterColor = (C 0x3A 0xA8 0xF0 78)
        $pg.SurroundColors = @([Drawing.Color]::FromArgb(0, 0x3A, 0xA8, 0xF0))
        $g.FillPath($pg, $gpath)
    }
    $lp = New-Object Drawing.Pen (C 0xF5 0xF8 0xFC), ([float]$lw)
    $lp.LineJoin = 'Round'; $lp.StartCap = 'Round'; $lp.EndCap = 'Round'
    $g.DrawLines($lp, $pts)

    # --- the head: "now", in Tarman's pinned-moment amber -----------------------
    $ang = ($start + $sweep) * [Math]::PI / 180.0
    $hx = $cx + $R * [Math]::Cos($ang); $hy = $cy + $R * [Math]::Sin($ang)
    $dr = $w * $(if ($small) { 0.62 } else { 0.66 })
    if (-not $small) {
        $cut = $dr + $w * 0.16                  # a ring of tile colour separates the dot from the arc
        $g.FillEllipse((New-Object Drawing.SolidBrush (C 0x10 0x16 0x21)), [float]($hx - $cut), [float]($hy - $cut), [float](2 * $cut), [float](2 * $cut))
    }
    $g.FillEllipse((New-Object Drawing.SolidBrush $AMBER), [float]($hx - $dr), [float]($hy - $dr), [float](2 * $dr), [float](2 * $dr))
    $g.Dispose()
    return $bmp
}

function PngBytes($bmp) {
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    return ,$ms.ToArray()
}

# --- .ico with PNG frames ---------------------------------------------------
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$frames = foreach ($s in $sizes) { $b = Render $s; ,(PngBytes $b); $b.Dispose() }
$ico = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $ico
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = $(if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] })
    $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$frames[$i].Length); $w.Write([UInt32]$off)
    $off += $frames[$i].Length
}
foreach ($f in $frames) { $w.Write($f) }
[IO.File]::WriteAllBytes((Join-Path $Root "resources\tarman.ico"), $ico.ToArray())

# --- big PNG for docs -----------------------------------------------------------
$big = Render 1024
$big.Save((Join-Path $Root "resources\logo-1024.png"), [Drawing.Imaging.ImageFormat]::Png)
$big.Dispose()

# --- embedded PNGs for the app ----------------------------------------------------
$sb = New-Object Text.StringBuilder
[void]$sb.AppendLine("/* Generated by tools/make-logo.ps1 -- do not edit. The Tarman logo as PNG")
[void]$sb.AppendLine(" * files, embedded so the window icon, title bar and About box show exactly")
[void]$sb.AppendLine(" * the mark the exe resource and the installer carry. */")
[void]$sb.AppendLine("#ifndef TARMAN_LOGO_PNG_H")
[void]$sb.AppendLine("#define TARMAN_LOGO_PNG_H")
[void]$sb.AppendLine("")
foreach ($s in @(32, 64, 256)) {
    $b = Render $s; $bytes = PngBytes $b; $b.Dispose()
    [void]$sb.AppendLine("static const unsigned char LOGO_PNG_$s[$($bytes.Length)] = {")
    for ($i = 0; $i -lt $bytes.Length; $i += 20) {
        $chunk = $bytes[$i..([Math]::Min($i + 19, $bytes.Length - 1))] | ForEach-Object { "0x{0:x2}," -f $_ }
        [void]$sb.AppendLine("    " + ($chunk -join " "))
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("")
}
[void]$sb.AppendLine("#endif /* TARMAN_LOGO_PNG_H */")
[IO.File]::WriteAllText((Join-Path $Root "src\logo_png.h"), $sb.ToString(), (New-Object Text.UTF8Encoding $false))

# preview sheet for eyeballing every size side by side
$sheet = New-Object Drawing.Bitmap 760, 300
$sg = [Drawing.Graphics]::FromImage($sheet)
$sg.Clear((C 0x2B 0x2F 0x36)); $sg.FillRectangle((New-Object Drawing.SolidBrush (C 0xF2 0xF3 0xF5)), 0, 150, 760, 150)
$x = 12
foreach ($s in @(16, 20, 24, 32, 48, 64, 128)) {
    $b = Render $s
    $sg.DrawImage($b, $x, [int](75 - $s / 2)); $sg.DrawImage($b, $x, [int](225 - $s / 2))
    $x += $s + 24; $b.Dispose()
}
$sheet.Save((Join-Path $Root "build\logo-sheet.png"), [Drawing.Imaging.ImageFormat]::Png)
"logo written: resources\tarman.ico, resources\logo-1024.png, src\logo_png.h"
