<#
    window_probe.ps1 - capture what the ray tracer actually puts on screen.

    The headless --capture path proves the tracer works, and only this proves the *present* path
    does: it launches the real windowed app, screenshots the desktop while it runs, and saves the
    frame. This is how a black-window-but-correct-title-bar bug was traced to the present pass.

    Usage:  powershell -File tools\window_probe.ps1                     # 60 fps window, 3 s in
            powershell -File tools\window_probe.ps1 -Out shot.png -DelaySeconds 4
            powershell -File tools\window_probe.ps1 -AppArgs @('--scale','0.5')
#>
param(
    [string]$Exe = '',
    [string]$Out = 'screen_probe.png',
    [string[]]$AppArgs = @(),
    [double]$DelaySeconds = 3.0,
    [double]$AutoExit = 8.0
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $root 'build\bin\raytracer.exe' }

Add-Type -AssemblyName System.Drawing, System.Windows.Forms

$auto = @('--autoexit', "$AutoExit")
$proc = Start-Process -FilePath $Exe -ArgumentList ($AppArgs + $auto) -PassThru
Start-Sleep -Seconds $DelaySeconds

$screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($screen.Location, [System.Drawing.Point]::Empty, $screen.Size)
$dest = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$bmp.Save($dest)
$g.Dispose()
$bmp.Dispose()

Wait-Process -Id $proc.Id -Timeout 30 -ErrorAction SilentlyContinue
Write-Host "wrote $dest ($((Get-Item $dest).Length) bytes)"

# Crop to the app window so the interesting pixels are not 1% of the image.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public static class W {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
}
"@
$rect = New-Object RECT
if ($proc.Handle -and [W]::GetWindowRect($proc.Handle, [ref]$rect)) {
    Write-Host "window rect was $($rect.Left),$($rect.Top) - $($rect.Right),$($rect.Bottom)"
}
