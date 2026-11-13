# flash_probe.ps1 - objective check for a blank/white frame when the renderer changes quality.
#
# Screenshots the desktop repeatedly while forcing a quality change, and reports the mean luminance
# of the window's client area for each grab. A quality change that presents an empty frame shows up
# as a spike toward 255; holding the previous frame keeps the series flat.
#
#   powershell -File tools\flash_probe.ps1                 # maximise mid-run to force a change
#   powershell -File tools\flash_probe.ps1 -Grabs 12
param(
    [int]$Grabs = 10,
    [int]$SettleSeconds = 8,
    [double]$GapMs = 110,
    [string]$Exe = '.\build\bin\raytracer.exe'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing,System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l,t,r,b; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@
$proc = Start-Process -FilePath $Exe -ArgumentList '--autoexit','26' -PassThru
Start-Sleep -Seconds $SettleSeconds   # let the controller settle first
[void][W]::ShowWindow($proc.MainWindowHandle, 3)   # forces a scale + sample change

$rect = New-Object W+R
$screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
$results = @()
for ($i = 0; $i -lt $Grabs; $i++) {
    $bmp = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($screen.Location, [System.Drawing.Point]::Empty, $screen.Size)
    $g.Dispose()
    if ([W]::GetWindowRect($proc.MainWindowHandle, [ref]$rect)) {
        $x0 = [Math]::Max(0, $rect.l + 40); $y0 = [Math]::Max(0, $rect.t + 60)
        $x1 = [Math]::Min($screen.Width - 1, $rect.r - 10); $y1 = [Math]::Min($screen.Height - 1, $rect.b - 10)
    } else { $x0 = 0; $y0 = 0; $x1 = $screen.Width - 1; $y1 = $screen.Height - 1 }
    $sum = 0.0; $n = 0
    for ($y = $y0; $y -lt $y1; $y += 17) {
        for ($x = $x0; $x -lt $x1; $x += 17) {
            $c = $bmp.GetPixel($x, $y)
            $sum += 0.2126 * $c.R + 0.7152 * $c.G + 0.0722 * $c.B; $n++
        }
    }
    $results += [pscustomobject]@{ Grab = $i + 1; MeanLuma = [Math]::Round($sum / [Math]::Max(1, $n), 1) }
    $bmp.Dispose()
    Start-Sleep -Milliseconds $GapMs
}
Wait-Process -Id $proc.Id -Timeout 30 -ErrorAction SilentlyContinue
$results | Format-Table -AutoSize
$max = ($results | Measure-Object MeanLuma -Maximum).Maximum
$min = ($results | Measure-Object MeanLuma -Minimum).Minimum
$base = ($results | Sort-Object MeanLuma | Select-Object -Skip 1 -First ([Math]::Max(1, $Grabs - 2)) | Measure-Object MeanLuma -Average).Average
"peak={0}  floor={1}  typical={2}  swing={3}" -f $max, $min, [Math]::Round($base,1), [Math]::Round($max - $min, 1)
if ($max -gt $base + 45) { "VERDICT: a frame is far brighter than the rest - looks like a blank/white present." }
else { "VERDICT: no blank frame in the series." }
