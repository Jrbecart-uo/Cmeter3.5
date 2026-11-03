<#
.SYNOPSIS
  Auto-attach the ESP32-S3 (Cmeter3.5 board) to WSL2 across replugs and reboots.

.DESCRIPTION
  Run ONCE in a Windows **Administrator** PowerShell. It:
    1. binds the device in usbipd (persists; harmless if already bound),
    2. registers a Scheduled Task that, from logon, polls `usbipd state` every
       5s and attaches the board whenever it is plugged in but not attached.

  The board is matched by its USB SERIAL NUMBER (end of its InstanceId),
  re-resolved to the current busid on every poll (the busid changes between
  ports). NEVER by hardware-id: 303a:1001 is every ESP32-S3 in USB-JTAG mode,
  including hidprj's keyboard/mouse board, which must stay on Windows (a
  hardware-id bind/attach steals the PC's keyboard and mouse).

  After this: replug / reflash -> re-attached within ~5s; reboot/logon -> the
  task restarts the watcher.

.PARAMETER Serial
  The board's USB serial: the last part of its InstanceId in `usbipd state`
  (USB\VID_303A&PID_1001\<serial>). Default 1C:DB:D4:79:7F:3C (Cmeter3.5).

.PARAMETER TaskName
  Scheduled Task name. Default "usbipd-esp32".

.PARAMETER Remove
  Unregister the Scheduled Task and exit (does not unbind).

.EXAMPLE
  # Default device, register everything:
  .\usbipd-autoattach.ps1

.EXAMPLE
  # A replacement board:
  .\usbipd-autoattach.ps1 -Serial AA:BB:CC:DD:EE:FF

.EXAMPLE
  # Tear down:
  .\usbipd-autoattach.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$Serial       = "1C:DB:D4:79:7F:3C",
    [string]$TaskName     = "usbipd-esp32",
    [string]$Distribution = "Ubuntu-24.04",
    # User the task RUNS AS. WSL2 distros are per-user; if the admin shell
    # runs as a different account than the one that owns the distro, the
    # task must still execute under the distro-owner. Override on domain
    # machines: -RunAsUser "domain\someoneelse".
    [string]$RunAsUser    = "$env:USERDOMAIN\$env:USERNAME",
    [switch]$Remove
)

$ErrorActionPreference = "Stop"

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this in an Administrator PowerShell (usbipd bind + Scheduled Task need elevation)."
    }
}

Assert-Admin

if ($Remove) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Removed Scheduled Task '$TaskName'." -ForegroundColor Green
    } else {
        Write-Host "No Scheduled Task '$TaskName' found." -ForegroundColor Yellow
    }
    return
}

if (-not (Get-Command usbipd.exe -ErrorAction SilentlyContinue)) {
    throw "usbipd.exe not on PATH. Install: winget install --exact dorssel.usbipd-win"
}

# 1. Bind ONLY our board (persists; safe to repeat), found by its serial.
Write-Host "usbipd list:" -ForegroundColor Cyan
usbipd list
$dev = (usbipd state | ConvertFrom-Json).Devices |
    Where-Object { $_.InstanceId -like "*\$Serial" } | Select-Object -First 1

if ($dev -and $dev.BusId) {
    Write-Host "Binding board $Serial (busid $($dev.BusId))..." -ForegroundColor Cyan
    # usbipd writes info messages (e.g. "already shared") to stderr; with
    # $ErrorActionPreference=Stop that would terminate the script, so isolate.
    & { $ErrorActionPreference = 'Continue'; usbipd bind --busid $dev.BusId 2>&1 | Out-Null }
} elseif ($dev -and $dev.PersistedGuid) {
    Write-Host "Board $Serial not plugged in, but already bound -- OK." -ForegroundColor Yellow
} else {
    Write-Host "Board $Serial not plugged in right now -- binding skipped." -ForegroundColor Yellow
    Write-Host "Plug it in and re-run, or bind manually once it appears." -ForegroundColor Yellow
}

# 2. Scheduled Task: attach watcher at logon, hidden, highest privileges.
# Poll `usbipd state` and attach OUR board (by serial -> current busid) only
# when it is plugged in, shared and not yet attached. Wake WSL2 first
# ('usbipd attach --wsl' fails if no distro is running). No --auto-attach:
# it can only pin a busid (changes per port) or a hardware-id (matches
# other boards). PowerShell wrapper: cmd.exe /c can't loop outside a .bat.
$psBody = @"
while (`$true) {
  try {
    `$d = (usbipd.exe state | ConvertFrom-Json).Devices |
      Where-Object { `$_.InstanceId -like '*\$Serial' } | Select-Object -First 1
    if (`$d -and `$d.BusId -and `$d.PersistedGuid -and -not `$d.ClientIPAddress) {
      & wsl.exe -d $Distribution -- true
      & usbipd.exe attach --wsl $Distribution --busid `$d.BusId
    }
  } catch { }
  Start-Sleep -Seconds 5
}
"@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($psBody))
$action  = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -WindowStyle Hidden -EncodedCommand $encoded"

# No -User filter on the trigger: on AD machines the logon-event principal
# can be reported as SID / UPN / DOMAIN\user variants and the string match
# silently fails. Fire on any logon and let -User on Register-ScheduledTask
# (interactive logon type) restrict who actually runs. 30s delay gives
# wsl.exe time to come up before the first attach.
$trigger = New-ScheduledTaskTrigger -AtLogOn
$trigger.Delay = "PT30S"

# Internal while($true) loop in the action retries every 5s, so a single
# trigger is enough; -MultipleInstances IgnoreNew keeps double-fires safe.
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)

# Replace, don't stack: stop a running old instance (with IgnoreNew a still-
# running old loop would keep the new one from starting) and kill any old
# `usbipd attach --hardware-id ... --auto-attach` watcher it left behind.
Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='usbipd.exe'" |
    Where-Object { $_.CommandLine -match '--hardware-id' } |
    ForEach-Object {
        Write-Host "Stopping old hardware-id watcher (PID $($_.ProcessId))" -ForegroundColor Yellow
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }

Write-Host "Task will run as: $RunAsUser" -ForegroundColor Cyan
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -User $RunAsUser -RunLevel Highest -Force | Out-Null

Write-Host "`nRegistered Scheduled Task '$TaskName'." -ForegroundColor Green
Write-Host "Starting it now so you don't have to log out..." -ForegroundColor Cyan
Start-ScheduledTask -TaskName $TaskName

Write-Host "`nDone. Verify in WSL2:  ls -l /dev/serial/by-id/ | grep $Serial" -ForegroundColor Green
Write-Host "Tear down later with:  .\usbipd-autoattach.ps1 -Remove" -ForegroundColor DarkGray
