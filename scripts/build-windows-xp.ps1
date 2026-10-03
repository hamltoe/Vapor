# Configure and build the Windows XP SP3 32-bit client with MinGW-w64 i686.
#
# Visual Studio 2022 cannot produce an XP image (its Universal CRT will not
# load). This script does not touch scripts\build-windows.ps1 or build-windows\.
#
# Needs the msvcrt MinGW toolchain (MSYS2 package mingw-w64-i686-gcc), not the
# UCRT one, plus CMake. Set VAPOR_MINGW32 to the mingw32\bin directory if it
# is not in the usual place.
#
# The GUI links SDL 2.0.22 built from source with the same compiler. SDL 2.32
# does not run on XP.
#
# Usage: scripts\build-windows-xp.ps1 [-BuildType Debug|Release] [-NoGui]
[CmdletBinding()]
param(
    [string]$BuildType = 'Release',
    [switch]$NoGui,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CMakeArgs = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $root 'third_party\sqlite\sqlite3.c'))) {
    throw "third_party is empty; run scripts\vendor-deps.ps1 first"
}

function Find-Mingw32Bin {
    $candidates = @()
    if ($env:VAPOR_MINGW32) { $candidates += $env:VAPOR_MINGW32 }
    $candidates += @(
        'C:\msys64\mingw32\bin',
        'C:\msys32\mingw32\bin'
    )
    foreach ($dir in $candidates) {
        if ($dir -and (Test-Path (Join-Path $dir 'gcc.exe'))) {
            return $dir
        }
    }
    return $null
}

$mingw = Find-Mingw32Bin
if (-not $mingw) {
    throw @"
MinGW-w64 i686 (msvcrt) was not found.
Install the MSYS2 package mingw-w64-i686-gcc, or set VAPOR_MINGW32 to its bin directory.
The Visual Studio toolchain cannot build an XP client.
"@
}
if ($mingw -match 'ucrt') {
    throw "VAPOR_MINGW32 points at a UCRT toolchain. XP needs mingw32 (msvcrt), not ucrt."
}

$gcc = Join-Path $mingw 'gcc.exe'
$machine = & $gcc -dumpmachine
if ($machine -notlike 'i686-*') {
    throw "gcc in $mingw reports '$machine'; XP needs i686-w64-mingw32"
}
$make = Join-Path $mingw 'mingw32-make.exe'
if (-not (Test-Path $make)) {
    throw "mingw32-make.exe is missing next to $gcc"
}

$env:PATH = "$mingw;$env:PATH"
$env:CC = $gcc

function Find-Cmake {
    $cmd = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        if ($vsPath) {
            $bundled = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $bundled) { return $bundled }
        }
    }
    throw "cmake.exe was not found on PATH"
}

$cmake = Find-Cmake
$build = Join-Path $root 'build-windows-xp'
$sdlPrefix = Join-Path $build 'sdl'
$gui = 'ON'

# Match CMakeLists.txt. This GCC defaults to pentium4 (SSE2). An Athlon XP
# 3000+ does not implement SSE2; those instructions fault at startup.
$xpArch = '-march=i686 -mtune=generic -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 -mstackrealign -fno-tree-vectorize -fno-tree-slp-vectorize -fno-tree-loop-vectorize'

function ConvertTo-MsysPath([string]$path) {
    $path = $path -replace '\\', '/'
    if ($path -match '^([A-Za-z]):(.*)$') {
        return ('/' + $Matches[1].ToLower() + $Matches[2])
    }
    return $path
}

# The stock MSYS2 libmingwex is compiled for Pentium 4. stat() and printf
# in that archive use SSE2, so they fault on an Athlon XP even when Vapor
# itself was built with -mno-sse2. Rebuild the CRT from the same commit the
# installed headers came from.
function Build-XpCrt {
    $prefix = Join-Path $build 'crt'
    $lib = Join-Path $prefix 'lib\libmingwex.a'
    $stampPath = Join-Path $prefix '.xp-crt-stamp'
    $commit = '4564ee4b5063097bf747af3a3f8270a28adff820'
    $stampText = "$commit msvcrt-os $xpArch"
    if ((Test-Path $lib) -and (Test-Path (Join-Path $prefix 'lib\libmingw32.a')) -and (Test-Path $stampPath)) {
        if ((Get-Content $stampPath -Raw).Trim() -eq $stampText) {
            return (Join-Path $prefix 'lib')
        }
    }
    $bash = 'C:\msys64\usr\bin\bash.exe'
    if (-not (Test-Path $bash)) {
        throw 'MSYS2 bash is required to build a no-SSE2 C runtime for the Athlon XP.'
    }
    $srcRoot = Join-Path $build 'mingw-w64-src'
    $configure = Join-Path $srcRoot 'mingw-w64-crt\configure'
    if (-not (Test-Path $configure)) {
        Write-Host 'fetching mingw-w64 CRT sources'
        & git clone --filter=blob:none --sparse https://github.com/mingw-w64/mingw-w64.git $srcRoot
        if ($LASTEXITCODE -ne 0) { throw 'git clone of mingw-w64 failed' }
        Push-Location $srcRoot
        & git sparse-checkout set mingw-w64-crt
        & git checkout $commit
        if ($LASTEXITCODE -ne 0) { throw 'mingw-w64 checkout failed' }
        Pop-Location
    }
    Write-Host 'building i686 C runtime without SSE2'
    $srcM = ConvertTo-MsysPath (Join-Path $srcRoot 'mingw-w64-crt')
    $buildM = ConvertTo-MsysPath (Join-Path $build 'mingw-crt-build')
    $prefixM = ConvertTo-MsysPath $prefix
    $sh = Join-Path $build 'build-crt.sh'
    $script = @"
set -euo pipefail
export PATH="/mingw32/bin:/usr/bin:`$PATH"
export CC=gcc
export CXX=g++
export AR=ar
export RANLIB=ranlib
export DLLTOOL=dlltool
export WINDRES=windres
export CFLAGS="-O2 $xpArch"
export CXXFLAGS="`$CFLAGS"
rm -rf '$buildM'
mkdir -p '$buildM'
cd '$buildM'
# Default libmsvcrt.a in this CRT is the Universal CRT (api-ms-win-crt-*.dll).
# XP only has msvcrt.dll. msvcrt-os is that DLL.
'$srcM/configure' --host=i686-w64-mingw32 --prefix='$prefixM' --enable-lib32 --disable-lib64 --disable-libarm32 --disable-libarm64 --disable-dependency-tracking --with-default-msvcrt=msvcrt
# Git timestamps make automake try to regenerate configure. The generated
# files are already in the tree; MSYS2 does not have autoconf installed.
find '$srcM' \( -name configure -o -name Makefile.in -o -name aclocal.m4 \) -exec touch {} +
make -j`$(nproc)
make install
"@
    [System.IO.File]::WriteAllText($sh, ($script -replace "`r`n", "`n"))
    # Native stdout would otherwise become this function's return value and
    # land in the -L path on the next link line.
    & $bash (ConvertTo-MsysPath $sh) | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'no-SSE2 CRT build failed' }
    if (-not (Test-Path $lib)) { throw "CRT install missing $lib" }
    Set-Content -Path $stampPath -Value $stampText -NoNewline
    return (Join-Path $prefix 'lib')
}

$crtLib = Build-XpCrt
# gcc.exe is a Windows program. It does not understand MSYS /c/ paths, so a
# -L/c/... flag is ignored and the Pentium 4 CRT is linked instead.
$crtLibGcc = ($crtLib -replace '\\', '/')

if (-not $NoGui) {
    $sdlHeader = Join-Path $sdlPrefix 'include\SDL2\SDL.h'
    $sdlLib = Join-Path $sdlPrefix 'lib\libSDL2.dll.a'
    $sdlStamp = Join-Path $sdlPrefix '.xp-build-stamp'
    $sdlStampText = "sdl-2.0.22 i686-no-sse static-libgcc crt-msvcrt-os $xpArch"
    $sdlReady = (Test-Path $sdlHeader) -and (Test-Path $sdlLib) -and (Test-Path $sdlStamp)
    if ($sdlReady) {
        $have = (Get-Content $sdlStamp -Raw).Trim()
        if ($have -ne $sdlStampText) { $sdlReady = $false }
    }
    if (-not $sdlReady) {
        Write-Host "building SDL 2.0.22 for XP"
        $sdlVer = '2.0.22'
        $srcParent = Join-Path $build 'sdl-src'
        $src = Join-Path $srcParent "SDL2-$sdlVer"
        $tgz = Join-Path $build "SDL2-$sdlVer.tar.gz"
        New-Item -ItemType Directory -Force -Path $build | Out-Null
        if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) {
            Write-Host "  fetch SDL2-$sdlVer"
            Invoke-WebRequest -Uri "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVer/SDL2-$sdlVer.tar.gz" `
                -OutFile $tgz -UseBasicParsing -TimeoutSec 180
            New-Item -ItemType Directory -Force -Path $srcParent | Out-Null
            tar -xzf $tgz -C $srcParent
        }
        $sdlBuild = Join-Path $build 'sdl-build'
        if (Test-Path $sdlBuild) { Remove-Item -Recurse -Force $sdlBuild }
        $xpLink = "-L$crtLibGcc -Wl,--subsystem,windows:5.01 -Wl,--major-os-version,5,--minor-os-version,1,--major-subsystem-version,5,--minor-subsystem-version,1 -static-libgcc -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic"
        $xpC = "-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -Wno-error=implicit-function-declaration -Wno-error=int-conversion $xpArch"
        # Windows rejects the configure command when these flag strings are
        # passed on the cmake command line.
        $preload = Join-Path $build 'sdl-preload.cmake'
        @"
set(CMAKE_C_FLAGS "$xpC" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG $xpC" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "$xpLink" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "$xpLink" CACHE STRING "" FORCE)
"@ | Set-Content -Path $preload -Encoding ascii
        & $cmake -S $src -B $sdlBuild -G 'MinGW Makefiles' -C $preload `
            "-DCMAKE_MAKE_PROGRAM=$make" `
            "-DCMAKE_BUILD_TYPE=Release" `
            "-DCMAKE_INSTALL_PREFIX=$sdlPrefix" `
            "-DCMAKE_C_COMPILER=$gcc" `
            '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' `
            '-DHAVE_WINDOWS_GAMING_INPUT_H=OFF' `
            '-DSDL_ASSEMBLY=OFF' '-DSDL_SSEMATH=OFF' '-DSDL_MMX=OFF' '-DSDL_3DNOW=OFF' `
            '-DSDL_SSE=OFF' '-DSDL_SSE2=OFF' '-DSDL_SSE3=OFF' `
            '-DSDL_SHARED=ON' '-DSDL_STATIC=OFF'
        if ($LASTEXITCODE -ne 0) { throw "SDL configure failed" }
        & $cmake --build $sdlBuild --parallel
        if ($LASTEXITCODE -ne 0) { throw "SDL build failed" }
        & $cmake --install $sdlBuild
        if ($LASTEXITCODE -ne 0) { throw "SDL install failed" }
        if (-not (Test-Path $sdlHeader)) {
            throw "SDL installed but $sdlHeader is missing"
        }
        Set-Content -Path $sdlStamp -Value $sdlStampText -NoNewline
    }
} else {
    $gui = 'OFF'
}

$configure = @(
    '-S', $root,
    '-B', $build,
    '-G', 'MinGW Makefiles',
    "-DCMAKE_MAKE_PROGRAM=$make",
    "-DCMAKE_C_COMPILER=$gcc",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    '-DVAPOR_BUILD_SERVER=OFF',
    '-DVAPOR_TARGET_XP=ON',
    "-DVAPOR_XP_CRT_LIB=$crtLibGcc",
    "-DVAPOR_BUILD_GUI=$gui"
)
if ($gui -eq 'ON') {
    $configure += "-DVAPOR_XP_SDL_ROOT=$sdlPrefix"
}
if ($CMakeArgs) { $configure += $CMakeArgs }

Write-Host "configuring XP client ($machine)"
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

# The CRT archive is not a cmake input, so a rebuilt libmsvcrt.a would
# otherwise leave the previous UCRT imports in the exes.
$outBin = Join-Path $build 'bin'
foreach ($name in @('vapor.exe', 'vapor-gui.exe', 'vapor-selftest.exe', 'SDL2.dll')) {
    $built = Join-Path $outBin $name
    if (Test-Path $built) { Remove-Item $built -Force }
}

& $cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host ''
Write-Host "XP binaries in ${build}\bin (Windows XP SP3 32-bit, http:// only):"
Get-ChildItem (Join-Path $build 'bin') -Filter *.exe | ForEach-Object { "  $($_.Name)" }
if ($gui -eq 'ON') {
    $dll = Join-Path $sdlPrefix 'bin\SDL2.dll'
    if (-not (Test-Path $dll)) { $dll = Join-Path $sdlPrefix 'lib\SDL2.dll' }
    $outBin = Join-Path $build 'bin'
    if (Test-Path $dll) {
        Copy-Item $dll (Join-Path $outBin 'SDL2.dll') -Force
        Write-Host '  SDL2.dll'
    }
    # If SDL still imports the shared GCC runtime, those DLLs have to sit
    # beside vapor-gui.exe. XP reports a missing one as "failed to initialize".
    $objdump = Join-Path $mingw 'objdump.exe'
    $imports = & $objdump -p (Join-Path $outBin 'SDL2.dll') | Out-String
    foreach ($dep in @('libgcc_s_dw2-1.dll', 'libwinpthread-1.dll')) {
        $dest = Join-Path $outBin $dep
        if ($imports -match [regex]::Escape($dep)) {
            $src = Join-Path $mingw $dep
            if (-not (Test-Path $src)) { throw "missing $src (needed next to vapor-gui.exe)" }
            Copy-Item $src (Join-Path $outBin $dep) -Force
            Write-Host "  $dep"
        } elseif (Test-Path $dest) {
            Remove-Item $dest -Force
        }
    }
}
