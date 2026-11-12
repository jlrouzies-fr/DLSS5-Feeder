# Compare-Shots.ps1 -- put a run's captures side by side with their names on them, and say how
# far each one is from the first and from the one before it (mean absolute difference per
# colour channel, in 8-bit steps, over the whole frame).
#
#   powershell -File tools\overlay-lab\Compare-Shots.ps1 -Dir build\ovlab\out\vis-opti -Names A-off,B-on,C-cinematic -Out sheet.png
param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string[]]$Names,
    [string]$Out = 'sheet.png',
    [int]$Columns = 3,
    [int]$CellWidth = 640,
    [string]$Title = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ('OvLab.Diff' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
namespace OvLab { public static class Diff {
    public static double Mad(Bitmap a, Bitmap b) {
        int w = Math.Min(a.Width, b.Width), h = Math.Min(a.Height, b.Height);
        var ra = a.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        var rb = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        byte[] pa = new byte[ra.Stride * h], pb = new byte[rb.Stride * h];
        Marshal.Copy(ra.Scan0, pa, 0, pa.Length); Marshal.Copy(rb.Scan0, pb, 0, pb.Length);
        a.UnlockBits(ra); b.UnlockBits(rb);
        long sum = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w * 3; ++x) sum += Math.Abs(pa[y * ra.Stride + x] - pb[y * rb.Stride + x]);
        return (double)sum / ((double)w * h * 3);
    } } }
'@
}
$bmps = @()
foreach ($n in $Names) { $bmps += [System.Drawing.Bitmap]::FromFile((Join-Path $Dir "$n.png")) }
$cw = $CellWidth; $ch = [int]($cw * $bmps[0].Height / $bmps[0].Width)
$rows = [int][Math]::Ceiling($Names.Count / $Columns); $top = if ($Title -ne '') { 34 } else { 0 }
$sheet = New-Object System.Drawing.Bitmap ($cw * [Math]::Min($Columns, $Names.Count)), ($ch * $rows + $top)
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.InterpolationMode = 'HighQualityBicubic'; $g.Clear([System.Drawing.Color]::FromArgb(16, 17, 21))
$font = New-Object System.Drawing.Font 'Segoe UI', 13, ([System.Drawing.FontStyle]::Bold)
if ($Title -ne '') { $g.DrawString($Title, $font, [System.Drawing.Brushes]::White, 8, 5) }
$back = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200, 0, 0, 0))
for ($i = 0; $i -lt $Names.Count; $i++) {
    $x = ($i % $Columns) * $cw; $y = [int][Math]::Floor($i / $Columns) * $ch + $top
    $g.DrawImage($bmps[$i], $x, $y, $cw, $ch)
    $first = if ($i -gt 0) { [OvLab.Diff]::Mad($bmps[0], $bmps[$i]) } else { 0 }
    $prev  = if ($i -gt 0) { [OvLab.Diff]::Mad($bmps[$i - 1], $bmps[$i]) } else { 0 }
    $label = if ($i -eq 0) { $Names[$i] } else { '{0}   (vs first {1:N1}, vs previous {2:N1})' -f $Names[$i], $first, $prev }
    $size = $g.MeasureString($label, $font)
    $g.FillRectangle($back, $x, $y, $size.Width + 12, $size.Height + 6)
    $g.DrawString($label, $font, [System.Drawing.Brushes]::White, $x + 6, $y + 3)
    '{0,-22} vs first {1,5:N2}   vs previous {2,5:N2}' -f $Names[$i], $first, $prev
}
$g.Dispose()
$dst = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $Dir $Out }
$sheet.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
foreach ($b in $bmps) { $b.Dispose() }
"sheet: $dst"
