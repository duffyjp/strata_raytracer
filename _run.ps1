param([string[]]$RunArgs = @())
$exe = Join-Path $PSScriptRoot 'build\bin\raytracer.exe'
$log = Join-Path $PSScriptRoot 'build\bin\raytracer.log'
Get-Process raytracer -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $log -Force -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $exe -ArgumentList $RunArgs -WorkingDirectory (Join-Path $PSScriptRoot 'build\bin') -PassThru -Wait
Write-Host "=== raytracer $($RunArgs -join ' ')  -> exit code $($p.ExitCode)" -ForegroundColor Cyan
exit $p.ExitCode
