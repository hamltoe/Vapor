# Configure and build the Windows client with MSVC.
#
# The server is not built here: it needs libsodium and is only ever deployed on
# Linux. The client itself has no third-party dependencies at all, because the
# HTTP transport uses WinHTTP from the Windows SDK.
#
# Usage: scripts\build-windows.ps1 [-BuildType Debug|Release] [extra cmake args]
[CmdletBinding()]
param(
    [string]$BuildType = 'Debug',
    [switch]$NoGui,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $root 'third_party\sqlite\sqlite3.c'))) {
    throw "third_party is empty; run scripts\vendor-deps.ps1 first"
}

# Locate Visual Studio and its bundled CMake/Ninja.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; is Visual Studio installed?" }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                     -property installationPath
if (-not $vsPath) {
    $vsPath = & $vswhere -latest -products * -property installationPath
}
if (-not $vsPath) { throw "no Visual Studio installation with the C++ toolchain found" }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsPath" }

$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $cmake)) { $cmake = 'cmake' }

# Import the MSVC environment into this session so cmake finds cl.exe.
Write-Host "importing MSVC environment from $vsPath"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

# SDL2 is vendored rather than installed, so the GUI can be built by default as
# long as vendor-deps.ps1 has run.
$gui = 'ON'
if ($NoGui -or -not (Test-Path (Join-Path $root 'third_party\sdl2\include\SDL2\SDL.h'))) {
    $gui = 'OFF'
    if (-not $NoGui) {
        Write-Host 'third_party\sdl2 is missing; skipping the GUI (re-run vendor-deps.ps1 to add it)'
    }
}

$build = Join-Path $root 'build-windows'
$args = @(
    '-S', $root,
    '-B', $build,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    '-DVAPOR_BUILD_SERVER=OFF',
    "-DVAPOR_BUILD_GUI=$gui"
)
if (Test-Path $ninja) {
    $args += @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja")
}
$args += $CMakeArgs

& $cmake @args
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host ''
Write-Host "binaries in $build\bin:"
Get-ChildItem (Join-Path $build 'bin') -Filter *.exe | ForEach-Object { "  $($_.Name)" }
