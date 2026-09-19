#Requires -Version 5.1
<#
    Start, stop, or inspect vapord inside the Ubuntu WSL distro.

    Usage: powershell -File scripts/start-wsl-server.ps1 [start|stop|restart|status]
#>
param(
    [ValidateSet('start', 'stop', 'restart', 'status')]
    [string]$Command = 'start',
    [string]$Distro = 'Ubuntu-24.04'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if ($root -notmatch '^[A-Za-z]:\\') {
    throw "unexpected repo path '$root'"
}
$drive = $root.Substring(0, 1).ToLowerInvariant()
$rest = $root.Substring(2).Replace('\', '/')
$wslRoot = "/mnt/$drive$rest"

$script = "$wslRoot/scripts/wsl-server.sh"
# Checkout on NTFS may have CRLF; strip CR in a temp copy so bash sees LF.
& wsl -d $Distro -- bash -c "tr -d '\r' < '$script' > /tmp/vapor-wsl-server.sh && chmod +x /tmp/vapor-wsl-server.sh"
& wsl -d $Distro -- bash -c "export VAPOR_ROOT='$wslRoot'; bash /tmp/vapor-wsl-server.sh $Command"
exit $LASTEXITCODE
