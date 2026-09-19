# Fetches the vendored third-party sources into third_party/.
# These are deliberately not committed; run this once after cloning.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$tmp = Join-Path $env:TEMP ('vapor-vendor-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function Get-File($url, $dest) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Write-Host "  fetch $(Split-Path -Leaf $dest)"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing -TimeoutSec 120
}

Write-Host 'cJSON (MIT)'
foreach ($f in @('cJSON.c', 'cJSON.h')) {
    Get-File "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/$f" (Join-Path $tp "cjson/$f")
}

Write-Host 'SQLite (public domain)'
$z = Join-Path $tmp 'sqlite.zip'
Get-File 'https://sqlite.org/2025/sqlite-amalgamation-3500400.zip' $z
Expand-Archive -Path $z -DestinationPath $tmp -Force
$src = Join-Path $tmp 'sqlite-amalgamation-3500400'
New-Item -ItemType Directory -Force -Path (Join-Path $tp 'sqlite') | Out-Null
foreach ($f in @('sqlite3.c', 'sqlite3.h', 'sqlite3ext.h')) {
    Copy-Item (Join-Path $src $f) (Join-Path $tp "sqlite/$f") -Force
}

Write-Host 'miniz (MIT)'
$z = Join-Path $tmp 'miniz.zip'
Get-File 'https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip' $z
$mz = Join-Path $tmp 'miniz'
Expand-Archive -Path $z -DestinationPath $mz -Force
New-Item -ItemType Directory -Force -Path (Join-Path $tp 'miniz') | Out-Null
foreach ($f in @('miniz.c', 'miniz.h')) {
    $found = Get-ChildItem -Path $mz -Filter $f -Recurse | Select-Object -First 1
    if (-not $found) { throw "miniz: $f not found in release archive" }
    Copy-Item $found.FullName (Join-Path $tp "miniz/$f") -Force
}

Write-Host 'civetweb (MIT)'
$tgz = Join-Path $tmp 'civetweb.tar.gz'
Get-File 'https://github.com/civetweb/civetweb/archive/refs/tags/v1.16.tar.gz' $tgz
Push-Location $tmp
tar -xzf $tgz
Pop-Location
$cw = Join-Path $tmp 'civetweb-1.16'
New-Item -ItemType Directory -Force -Path (Join-Path $tp 'civetweb') | Out-Null
Copy-Item (Join-Path $cw 'include/civetweb.h') (Join-Path $tp 'civetweb/civetweb.h') -Force
Copy-Item (Join-Path $cw 'src/civetweb.c') (Join-Path $tp 'civetweb/civetweb.c') -Force
# civetweb.c #includes these as translation-unit fragments.
Get-ChildItem -Path (Join-Path $cw 'src') -Filter '*.inl' | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $tp "civetweb/$($_.Name)") -Force
}

Write-Host 'stb_image (public domain)'
Get-File 'https://raw.githubusercontent.com/nothings/stb/master/stb_image.h' (Join-Path $tp 'stb/stb_image.h')

Write-Host 'Nuklear (public domain)'
Get-File 'https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/nuklear.h' (Join-Path $tp 'nuklear/nuklear.h')
Get-File 'https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/demo/sdl_opengl2/nuklear_sdl_gl2.h' (Join-Path $tp 'nuklear/nuklear_sdl_gl2.h')

# Windows has no system package manager for SDL2, so the official MSVC
# development package gets vendored the same way as the single-file libs. On
# Linux this comes from apt instead - see scripts/bootstrap-linux.sh.
Write-Host 'SDL2 (zlib) - Windows development package'
$sdlVer = '2.32.8'
$z = Join-Path $tmp 'sdl2.zip'
Get-File "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVer/SDL2-devel-$sdlVer-VC.zip" $z
Expand-Archive -Path $z -DestinationPath $tmp -Force
$sdlSrc = Join-Path $tmp "SDL2-$sdlVer"
$sdlDst = Join-Path $tp 'sdl2'
Remove-Item -Recurse -Force $sdlDst -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $sdlDst | Out-Null
# Nested under include/SDL2 rather than include/, because Nuklear's SDL backend
# includes <SDL2/SDL.h> the way Linux distributions lay it out. CMake adds both
# include/ and include/SDL2 so plain <SDL.h> keeps working too.
Copy-Item (Join-Path $sdlSrc 'include') (Join-Path $sdlDst 'include\SDL2') -Recurse -Force
Copy-Item (Join-Path $sdlSrc 'lib') $sdlDst -Recurse -Force
Copy-Item (Join-Path $sdlSrc 'README-SDL.txt') $sdlDst -Force -ErrorAction SilentlyContinue

Remove-Item -Recurse -Force $tmp
Write-Host ''
Write-Host "third_party/ populated at $tp"
