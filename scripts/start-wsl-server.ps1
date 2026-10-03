#Requires -Version 5.1
<#
    Start, stop, or inspect vapord inside the Ubuntu WSL distro.

    Usage: powershell -File scripts/start-wsl-server.ps1 [start|stop|restart|status|shortcut]

    Game drop folder (library_root) defaults to G:\Vapor\Library. Override with
    -Library or $env:VAPOR_LIBRARY_DIR (Windows or /mnt/... path).

    shortcut drops a Desktop (and Start Menu) shortcut that double-clicks start.
#>
param(
    [ValidateSet('start', 'stop', 'restart', 'status', 'shortcut', 'publish-lan', 'publish-lan-admin')]
    [string]$Command = 'start',
    [string]$Distro = 'Ubuntu-24.04',
    [string]$Library = 'G:\Vapor\Library'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Test-VaporAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# WSL2 answers 127.0.0.1:8777 on this PC only. Other machines on the LAN,
# including the Windows XP client, connect to this PC's Ethernet address.
function Publish-VapordLan {
    param([int]$Port = 8777)

    if (-not (Test-VaporAdmin)) {
        $self = $PSCommandPath
        if (-not $self) { $self = $MyInvocation.MyCommand.Path }
        Write-Host "Opening port $Port on the LAN needs an administrator prompt."
        $proc = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $self, 'publish-lan-admin'
        )
        if (-not $proc -or $proc.ExitCode -ne 0) {
            throw "LAN port $Port was not opened. The XP client cannot reach vapord until that prompt is allowed."
        }
        return
    }

    $addrs = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
            $_.IPAddress -notlike '127.*' -and
            $_.IPAddress -notlike '169.254.*' -and
            $_.InterfaceAlias -notlike 'vEthernet*'
        } | Select-Object -ExpandProperty IPAddress -Unique)

    $shown = netsh interface portproxy show v4tov4 | Out-String
    foreach ($line in ($shown -split '\r?\n')) {
        if ($line -match '^\s*(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+') {
            if ([int]$Matches[2] -eq $Port) {
                & netsh interface portproxy delete v4tov4 "listenaddress=$($Matches[1])" "listenport=$Port" | Out-Null
            }
        }
    }
    if ($addrs.Count -eq 0) {
        throw "no LAN address found to publish port $Port"
    }

    & netsh advfirewall firewall delete rule name="Vapor Server" | Out-Null
    & netsh advfirewall firewall add rule name="Vapor Server" dir=in action=allow protocol=TCP "localport=$Port" remoteip=localsubnet profile=any | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "could not allow inbound TCP $Port from the local subnet"
    }
}

function Get-VapordLanAddresses {
    @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
            $_.IPAddress -notlike '127.*' -and
            $_.IPAddress -notlike '169.254.*' -and
            $_.InterfaceAlias -notlike 'vEthernet*'
        } | Select-Object -ExpandProperty IPAddress -Unique)
}

function Stop-VapordLanForward {
    $pidfile = Join-Path $env:LOCALAPPDATA 'Vapor\lan-forward.pid'
    if (-not (Test-Path $pidfile)) {
        return
    }
    $procId = 0
    [void][int]::TryParse((Get-Content $pidfile -Raw).Trim(), [ref]$procId)
    if ($procId -gt 0) {
        Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
}

# netsh portproxy accepts a LAN connection and then resets it. A small
# relay on this PC copies the bytes to vapord's localhost port instead.
function Start-VapordLanForward {
    param([int]$Port = 8777)

    $pidfile = Join-Path $env:LOCALAPPDATA 'Vapor\lan-forward.pid'
    if (Test-Path $pidfile) {
        $procId = 0
        [void][int]::TryParse((Get-Content $pidfile -Raw).Trim(), [ref]$procId)
        if ($procId -gt 0 -and (Get-Process -Id $procId -ErrorAction SilentlyContinue)) {
            Get-VapordLanAddresses | ForEach-Object { Write-Host "LAN URL: http://${_}:${Port}" }
            return
        }
    }

    $dir = Split-Path $pidfile
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir | Out-Null
    }
    $forward = Join-Path $PSScriptRoot 'lan-forward.ps1'
    $log = Join-Path $dir 'lan-forward.log'
    $proc = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru `
        -RedirectStandardError $log -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $forward, '-Port', "$Port"
    )
    Set-Content -Path $pidfile -Value $proc.Id -NoNewline
    Start-Sleep -Seconds 1
    Get-VapordLanAddresses | ForEach-Object { Write-Host "LAN URL: http://${_}:${Port}" }
}

if ($Command -eq 'publish-lan-admin') {
    Publish-VapordLan -Port 8777
    exit 0
}

if ($Command -eq 'publish-lan') {
    Publish-VapordLan -Port 8777
    Stop-VapordLanForward
    Start-VapordLanForward -Port 8777
    exit 0
}

if ($Command -eq 'shortcut') {
    $cmd = Join-Path $PSScriptRoot 'start-server.cmd'
    if (-not (Test-Path $cmd)) {
        throw "missing $cmd"
    }
    $shell = New-Object -ComObject WScript.Shell
    $places = @(
        (Join-Path $env:USERPROFILE 'Desktop\Vapor Server.lnk'),
        (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Vapor Server.lnk')
    )
    foreach ($path in $places) {
        $dir = Split-Path $path
        if (-not (Test-Path $dir)) {
            New-Item -ItemType Directory -Path $dir | Out-Null
        }
        $lnk = $shell.CreateShortcut($path)
        $lnk.TargetPath = $cmd
        $lnk.WorkingDirectory = $root
        $lnk.WindowStyle = 1
        $lnk.Description = 'Start the Vapor host (vapord in WSL)'
        $lnk.Save()
        Write-Host "shortcut: $path"
    }
    exit 0
}

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
if ($Command -eq 'stop' -or $Command -eq 'restart') {
    Stop-VapordLanForward
}
& wsl -d $Distro -- bash -c "export VAPOR_ROOT='$wslRoot'; export VAPOR_LIBRARY_DIR='$libraryWsl'; bash /tmp/vapor-wsl-server.sh $Command"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
if ($Command -eq 'start' -or $Command -eq 'restart') {
    Publish-VapordLan -Port 8777
    Stop-VapordLanForward
    Start-VapordLanForward -Port 8777
}
exit 0
