#Requires -Version 5.1
<#
    Start, stop, or inspect vapord inside the Ubuntu WSL distro.

    Usage: powershell -File scripts/start-wsl-server.ps1 [start|stop|restart|status]

    Game drop folder (library_root) defaults to G:\Vapor\Library. Override with
    -Library or $env:VAPOR_LIBRARY_DIR (Windows or /mnt/... path).
#>
param(
    [ValidateSet('start', 'stop', 'restart', 'status')]
    [string]$Command = 'start',
    [string]$Distro = 'Ubuntu-24.04',
    [string]$Library = 'G:\Vapor\Library'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if ($root -notmatch '^[A-Za-z]:\\') {
    throw "unexpected repo path '$root'"
}
$drive = $root.Substring(0, 1).ToLowerInvariant()
$rest = $root.Substring(2).Replace('\', '/')
$wslRoot = "/mnt/$drive$rest"

function ConvertTo-WslPath([string]$Path) {
    if ($Path -match '^/mnt/') {
        return $Path.TrimEnd('/')
    }
    if ($Path -notmatch '^([A-Za-z]):[\\/](.*)$') {
        throw "library path must be a Windows drive path or /mnt/... (got '$Path')"
    }
    $letter = $Matches[1].ToLowerInvariant()
    $rest = $Matches[2].Replace('\', '/').TrimEnd('/')
    if ($rest) {
        return "/mnt/$letter/$rest"
    }
    return "/mnt/$letter"
}

if ($env:VAPOR_LIBRARY_DIR) {
    $Library = $env:VAPOR_LIBRARY_DIR
}
$libraryWsl = ConvertTo-WslPath $Library

$script = "$wslRoot/scripts/wsl-server.sh"
# Checkout on NTFS may have CRLF; strip CR in a temp copy so bash sees LF.
& wsl -d $Distro -- bash -c "tr -d '\r' < '$script' > /tmp/vapor-wsl-server.sh && chmod +x /tmp/vapor-wsl-server.sh"
& wsl -d $Distro -- bash -c "export VAPOR_ROOT='$wslRoot'; export VAPOR_LIBRARY_DIR='$libraryWsl'; bash /tmp/vapor-wsl-server.sh $Command"
exit $LASTEXITCODE
