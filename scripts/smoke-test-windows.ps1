#Requires -Version 5.1
<#
    Cross-platform end-to-end test: the Windows client against the Linux server.

    vapord and vapor-admin run inside WSL (they are Linux-only); vapor.exe is
    the native Windows build. WSL2 forwards localhost, so the client reaches the
    server on 127.0.0.1 with no extra networking setup.

    Client state is redirected with VAPOR_DATA_DIR so a real install is never
    touched.
#>
param(
    [int]$Port = 8897,
    [string]$Distro = 'Ubuntu-24.04'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$winBin = Join-Path $root 'build-windows\bin'
$script:pass = 0
$script:fail = 0

function Step($text) { Write-Host "`n$text" }
function Ok($text) { Write-Host "  ok    $text"; $script:pass++ }
function Bad($text, $detail) {
    Write-Host "  FAIL  $text" -ForegroundColor Red
    if ($detail) { ($detail -split "`n" | ForEach-Object { "        | $_" }) -join "`n" | Write-Host }
    $script:fail++
}

# Runs vapor.exe and captures both streams; $LASTEXITCODE stays meaningful.
# Stop-on-error has to be relaxed here or anything the CLI writes to stderr
# would abort the run instead of being treated as a test result.
function Vapor {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & (Join-Path $winBin 'vapor.exe') @args 2>&1 | Out-String }
    finally { $ErrorActionPreference = $prev }
}

function Check($desc, $want, [string[]]$cmd) {
    $out = Vapor @cmd
    if ($LASTEXITCODE -eq 0 -and $out -like "*$want*") { Ok $desc } else { Bad $desc $out.Trim() }
}
function CheckFails($desc, [string[]]$cmd) {
    $out = Vapor @cmd
    if ($LASTEXITCODE -ne 0) { Ok $desc } else { Bad $desc "expected failure, got success`n$($out.Trim())" }
}

if (-not (Test-Path (Join-Path $winBin 'vapor.exe'))) {
    throw "missing $winBin\vapor.exe; run scripts\build-windows.ps1 first"
}

# One helper script does all the Linux-side work, which avoids fighting
# PowerShell -> bash quoting for every individual command.
$helper = Join-Path $root 'scripts\.smoke-win-server.sh'
$helperBody = @'
#!/usr/bin/env bash
# Driven by smoke-test-windows.ps1. Packages a Windows build and serves it.
set -uo pipefail
port="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${root}/build-linux/bin"
work="/tmp/vapor-winsmoke"

case "${2:-}" in
start)
    rm -rf "${work}"; mkdir -p "${work}/src/hollow-vale/bin"
    # A batch file is a real Windows executable target and needs no compiler.
    printf '@echo off\r\necho Hollow Vale running\r\necho   data=%%HOLLOW_DATA%%\r\nexit /b 0\r\n' \
        > "${work}/src/hollow-vale/bin/hollowvale.bat"
    echo 'level data' > "${work}/src/hollow-vale/assets.dat"

    "${bin}/vapor-admin" -r "${work}/content" -d "${work}/vapor.db" \
        add "${work}/src/hollow-vale" \
        --id hollow-vale --name "Hollow Vale" --version 1.0.3 \
        --developer "Smoke Works" --description "A test game." \
        --windows-exec 'bin/hollowvale.bat' \
        --env 'HOLLOW_DATA=$INSTALL_DIR/assets.dat' || exit 1

    setsid "${bin}/vapord" --headless -p "${port}" -H 0.0.0.0 \
        -r "${work}/content" -d "${work}/vapor.db" \
        > "${work}/vapord.log" 2>&1 &
    echo $! > "${work}/vapord.pid"
    for _ in $(seq 1 50); do
        grep -q 'listening on' "${work}/vapord.log" 2>/dev/null && break
        sleep 0.1
    done
    grep -q 'listening on' "${work}/vapord.log" || { cat "${work}/vapord.log"; exit 1; }
    echo started
    ;;
stop)
    [ -f "${work}/vapord.pid" ] && kill "$(cat "${work}/vapord.pid")" 2>/dev/null
    rm -rf "${work}"
    echo stopped
    ;;
esac
'@
# LF only: bash rejects CRLF shebangs.
[IO.File]::WriteAllText($helper, ($helperBody -replace "`r`n", "`n"))

$clientData = Join-Path $env:TEMP 'vapor-winsmoke-client'
$library = Join-Path $env:TEMP 'vapor-winsmoke-games'
$env:VAPOR_DATA_DIR = $clientData
$env:VAPOR_PASSWORD = 'smoke-test-password'

function StopServer {
    & wsl -d $Distro -- bash scripts/.smoke-win-server.sh $Port stop *> $null
    Remove-Item $helper, $clientData, $library -Recurse -Force -ErrorAction SilentlyContinue
}

try {
    Step "starting vapord in WSL ($Distro) on port $Port"
    Remove-Item $clientData, $library -Recurse -Force -ErrorAction SilentlyContinue
    Push-Location $root
    $out = & wsl -d $Distro -- bash scripts/.smoke-win-server.sh $Port start 2>&1 | Out-String
    Pop-Location
    if ($out -notmatch 'started') { throw "vapord failed to start in WSL:`n$out" }
    Ok 'vapord started and a Windows build was packaged'

    Step 'configuration and reachability'
    Check 'config server'  'server is now' @('config', 'server', "http://127.0.0.1:$Port")
    Check 'config library' 'library'       @('config', 'library', $library)
    Check 'ping'           'is up'         @('ping')

    Step 'accounts require a live server'
    $null = Vapor @('config', 'server', 'http://127.0.0.1:1')
    CheckFails 'register against a dead URL' @('register', 'winuser')
    CheckFails 'login against a dead URL'    @('login', 'winuser')
    Check 'restore server' 'server is now' @('config', 'server', "http://127.0.0.1:$Port")

    Step 'accounts'
    Check 'register'  'created account' @('register', 'winuser')
    Check 'login'     'signed in as'    @('login', 'winuser')
    Check 'whoami'    'winuser'         @('whoami')

    Step 'catalog'
    Check 'list shows the game' 'Hollow Vale' @('list')
    Check 'info'                'Smoke Works' @('info', 'hollow-vale')

    Step 'install'
    Check 'install'           'installed'   @('install', 'hollow-vale')
    Check 'installed listing' 'hollow-vale' @('installed')
    Check 'verify'            'matches'     @('verify', 'hollow-vale')
    if (Test-Path (Join-Path $library 'hollow-vale\assets.dat')) {
        Ok 'archive contents extracted'
    } else {
        Bad 'archive contents extracted' "missing $library\hollow-vale\assets.dat"
    }

    Step 'launch'
    Check 'launch runs the game' 'Hollow Vale running' @('launch', 'hollow-vale')
    Check 'env expansion' (Join-Path $library 'hollow-vale\assets.dat') @('launch', 'hollow-vale')
    Check 'playtime recorded' 'hollow-vale' @('installed')

    Step 'uninstall'
    Check 'uninstall' 'removed' @('uninstall', 'hollow-vale')
    if (Test-Path (Join-Path $library 'hollow-vale')) {
        Bad 'install directory deleted' 'directory still present'
    } else {
        Ok 'install directory deleted'
    }

    Step 'authorization is enforced'
    Check 'logout' 'signed out' @('logout')
    CheckFails 'catalog requires a session' @('list')
} finally {
    StopServer
}

Write-Host "`n$($script:pass) passed, $($script:fail) failed"
if ($script:fail -gt 0) { exit 1 }
