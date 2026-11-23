# Regenerates the WiX UI artwork (installer\dialog.bmp, installer\banner.bmp).
# Sizes follow WixUI: dialog 493x312 (text starts at x=180), banner 493x58 (title on the left).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$D = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

function Rgb($r, $g, $b, $a = 255) { [Drawing.Color]::FromArgb($a, $r, $g, $b) }
$Navy = Rgb 24 35 52; $Deep = Rgb 13 19 30; $Cyan = Rgb 105 217 245
$Top = Rgb 170 234 251; $Left = Rgb 70 164 204; $Right = Rgb 110 207 238

function Draw-Cube([Drawing.Graphics]$g, [float]$cx, [float]$cy, [float]$s) {
    $k = [float]0.866 * $s; $h = $s / 2
    $pt = { param($x, $y) New-Object Drawing.PointF ([float]$x), ([float]$y) }
    $g.FillPolygon((New-Object Drawing.SolidBrush $Top), [Drawing.PointF[]]@((& $pt $cx ($cy - $s)), (& $pt ($cx + $k) ($cy - $h)), (& $pt $cx $cy), (& $pt ($cx - $k) ($cy - $h))))
    $g.FillPolygon((New-Object Drawing.SolidBrush $Left), [Drawing.PointF[]]@((& $pt ($cx - $k) ($cy - $h)), (& $pt $cx $cy), (& $pt $cx ($cy + $s)), (& $pt ($cx - $k) ($cy + $h))))
    $g.FillPolygon((New-Object Drawing.SolidBrush $Right), [Drawing.PointF[]]@((& $pt $cx $cy), (& $pt ($cx + $k) ($cy - $h)), (& $pt ($cx + $k) ($cy + $h)), (& $pt $cx ($cy + $s))))
}

# Faint triangulated mesh, a nod to STL facets.
function Draw-Mesh([Drawing.Graphics]$g, [Drawing.RectangleF]$area, [float]$step, [int]$alpha) {
    $pen = New-Object Drawing.Pen (Rgb 105 217 245 $alpha), 1
    $g.SetClip($area)
    $rowH = $step * 0.866
    for ($row = -1; $row * $rowH -lt $area.Height + $rowH; $row++) {
        $y = $area.Top + $row * $rowH; $shift = if ($row % 2) { $step / 2 } else { 0 }
        $g.DrawLine($pen, $area.Left, $y, $area.Right, $y)
        for ($x = $area.Left - $step + $shift; $x -lt $area.Right + $step; $x += $step) {
            $g.DrawLine($pen, $x, $y, $x + $step / 2, $y + $rowH)
            $g.DrawLine($pen, $x + $step, $y, $x + $step / 2, $y + $rowH)
        }
    }
    $g.ResetClip()
}

function New-Canvas($w, $h) {
    $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = $D
    $g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([Drawing.Color]::White)
    return $bmp, $g
}

$out = $PSScriptRoot

# Dialog: branded side panel, white content area.
$bmp, $g = New-Canvas 493 312
$panel = New-Object Drawing.RectangleF 0, 0, 168, 312
$g.FillRectangle((New-Object Drawing.Drawing2D.LinearGradientBrush $panel, $Navy, $Deep, 90), $panel)
Draw-Mesh $g $panel 34 16
$glow = New-Object Drawing.Drawing2D.GraphicsPath
$glow.AddEllipse(24, 64, 120, 120)
$glowBrush = New-Object Drawing.Drawing2D.PathGradientBrush $glow
$glowBrush.CenterColor = Rgb 105 217 245 70; $glowBrush.SurroundColors = @(Rgb 105 217 245 0)
$g.FillPath($glowBrush, $glow)
$g.FillEllipse((New-Object Drawing.SolidBrush (Rgb 0 0 0 60)), 44, 170, 80, 12)
Draw-Cube $g 84 124 44
$center = New-Object Drawing.StringFormat; $center.Alignment = 'Center'
$g.DrawString('STL Quick View', (New-Object Drawing.Font 'Segoe UI Semibold', 12, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)), (New-Object Drawing.SolidBrush ([Drawing.Color]::White)), (New-Object Drawing.RectangleF 0, 214, 168, 22), $center)
$g.DrawString('3D 模型快速檢視', (New-Object Drawing.Font 'Microsoft JhengHei UI', 11, ([Drawing.FontStyle]::Regular), ([Drawing.GraphicsUnit]::Pixel)), (New-Object Drawing.SolidBrush (Rgb 168 189 210)), (New-Object Drawing.RectangleF 0, 238, 168, 20), $center)
$g.FillRectangle((New-Object Drawing.SolidBrush $Cyan), 168, 0, 3, 312)
$bmp.Save((Join-Path $out 'dialog.bmp'), [Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $bmp.Dispose()

# Banner: white title area, navy badge on the right.
$bmp, $g = New-Canvas 493 58
$badge = New-Object Drawing.Drawing2D.GraphicsPath
$badge.AddPolygon([Drawing.PointF[]]@((New-Object Drawing.PointF 404, 0), (New-Object Drawing.PointF 493, 0), (New-Object Drawing.PointF 493, 58), (New-Object Drawing.PointF 380, 58)))
$g.FillPath((New-Object Drawing.Drawing2D.LinearGradientBrush (New-Object Drawing.RectangleF 380, 0, 113, 58), $Navy, $Deep, 0), $badge)
$g.SetClip($badge); Draw-Mesh $g (New-Object Drawing.RectangleF 380, 0, 113, 58) 22 18; $g.ResetClip()
$g.FillPolygon((New-Object Drawing.SolidBrush $Cyan), [Drawing.PointF[]]@((New-Object Drawing.PointF 401, 0), (New-Object Drawing.PointF 404, 0), (New-Object Drawing.PointF 380, 58), (New-Object Drawing.PointF 377, 58)))
Draw-Cube $g 450 29 17
$bmp.Save((Join-Path $out 'banner.bmp'), [Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $bmp.Dispose()
Write-Host "Wrote dialog.bmp and banner.bmp to $out"
