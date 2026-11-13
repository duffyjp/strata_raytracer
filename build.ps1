<#
    build.ps1 - one-command build of the Vulkan ray tracer.

    Uses the CMake + Ninja that ship inside Visual Studio Build Tools, so no SDK install is needed,
    and glslang from tools\glslang (fetched from Khronos' official prebuilt releases).
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Capture,          # build, then render verification stills headlessly
    [int]$CaptureCount = 3,
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

function Invoke-Step([string]$name, [scriptblock]$body) {
    Write-Host "==> $name" -ForegroundColor Cyan
    & $body
    if ($LASTEXITCODE -ne 0) { throw "$name failed with exit code $LASTEXITCODE" }
}

# ---------------------------------------------------------------- locate the toolchain
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "Visual Studio Build Tools not found (vswhere missing)." }

$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $install) { $install = & $vswhere -latest -property installationPath }
if (-not $install) { throw "No Visual Studio / Build Tools installation found." }
Write-Host "compiler: $install"

$cmakeDir = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = (@($cmakeDir, $ninjaDir, $env:PATH) -join ';')

$python = $null
foreach ($cand in 'python', 'python3', 'py') {
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    if ($cmd) { $python = $cmd.Source; break }
}
if (-not $python) { throw "Python is required to compile the shaders." }

# ---------------------------------------------------------------- import the MSVC environment
$vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not next to the compiler." }
$envDump = cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] }
}

# ---------------------------------------------------------------- glslang presence check
$glslang = Join-Path $root 'tools\glslang\bin\glslang.exe'
if (-not (Test-Path $glslang)) {
    Write-Host "glslang missing; downloading the official Khronos prebuilt CLI" -ForegroundColor Yellow
    $url = 'https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip'
    $zip = Join-Path $root 'tools\glslang.zip'
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'tools') | Out-Null
    Invoke-WebRequest $url -OutFile $zip -UseBasicParsing
    Expand-Archive $zip -DestinationPath (Join-Path $root 'tools\glslang') -Force
    Remove-Item $zip -Force
}

# ---------------------------------------------------------------- configure / build
$buildDir = Join-Path $root 'build'
if ($Clean -and (Test-Path $buildDir)) { Remove-Item $buildDir -Recurse -Force }
$buildType = $Config

Write-Host '==> configure' -ForegroundColor Cyan
& cmake -S "$root" -B "$buildDir" -G Ninja "-DCMAKE_BUILD_TYPE=$buildType"
if ($LASTEXITCODE -ne 0) { throw "configure failed with exit code $LASTEXITCODE" }

Write-Host "==> build ($buildType)" -ForegroundColor Cyan
& cmake --build "$buildDir" --target raytracer -j
if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }

# ---------------------------------------------------------------- pak
Write-Host '==> pack assets -> scene.pak' -ForegroundColor Cyan
& $python (Join-Path $root 'tools\make_pak.py') --root $root
if ($LASTEXITCODE -ne 0) { throw "make_pak failed with exit code $LASTEXITCODE" }

$exe = Join-Path $root 'build\bin\raytracer.exe'
if (-not (Test-Path $exe)) { throw "expected $exe to exist after the build" }

$deps = & {
    $dumpbin = Get-ChildItem (Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\dumpbin.exe') -ErrorAction SilentlyContinue
    if ($dumpbin) {
        & $dumpbin.FullName /DEPENDENTS $exe | Select-String -Pattern '^\s+(\S+\.dll)$' |
            ForEach-Object { $_.Line.Trim() }
    }
}
if ($deps) {
    Write-Host ("exe imports: " + ($deps -join ', ')) -ForegroundColor DarkGray
    $bad = $deps | Where-Object { $_ -notmatch '^(KERNEL32|USER32|GDI32|SHELL32|OLE32|OLEAUT32|ADVAPI32|IMM32|WINMM|VCRUNTIME140|MSVCP140|api-ms-win.*|dwmapi|bcrypt|ntdll)' }
    if ($bad) { Write-Host "note: non-system DLL imports: $($bad -join ', ')" -ForegroundColor Yellow }
}

Write-Host ""
Write-Host "built: $exe" -ForegroundColor Green
Write-Host "run:   build\bin\raytracer.exe   (window opens; drag to resize; ESC quits)"

if ($Capture) {
    Write-Host '==> capture verification stills' -ForegroundColor Cyan
    & $exe --capture $CaptureCount --accum 48 --size 960x540 --out captures
    if ($LASTEXITCODE -ne 0) { throw "capture failed with exit code $LASTEXITCODE" }
    Write-Host "stills written to $(Join-Path $root 'captures')" -ForegroundColor Green
}
