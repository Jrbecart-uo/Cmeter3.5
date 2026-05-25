<#
.SYNOPSIS
  Auto-attach the ESP32-S3 (Cmeter3.5 board) to WSL2 across replugs and reboots.

.DESCRIPTION
  Run ONCE in a Windows **Administrator** PowerShell. It:
    1. binds the device in usbipd (persists; harmless if already bound),
    2. registers a Scheduled Task that runs `usbipd attach --auto-attach` at
       logon, pinned by hardware-id (survives the busid changing).

  After this: replug -> auto-reattaches; reboot/logon -> the task restarts the
  watcher. `--auto-attach` blocks and re-attaches every time the board
  re-enumerates (every flash cycle resets the USB-JTAG), so a single watcher
  covers normal day-to-day use.

.PARAMETER HardwareId
  VID:PID of the board. Default 303a:1001 (ESP32-S3 USB-JTAG). Check with
  `usbipd list` if attach never lands.

.PARAMETER TaskName
  Scheduled Task name. Default "usbipd-esp32".

.PARAMETER Remove
  Unregister the Scheduled Task and exit (does not unbind).

.EXAMPLE
  # Default device, register everything:
  .\usbipd-autoattach.ps1

.EXAMPLE
  # Different board revision:
  .\usbipd-autoattach.ps1 -HardwareId 303a:4001

.EXAMPLE
  # Tear down:
  .\usbipd-autoattach.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$HardwareId   = "303a:1001",
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

# 1. Bind (persists; safe to repeat). Match the device by hardware-id so we
#    don't depend on a busid that changes between ports.
Write-Host "usbipd list:" -ForegroundColor Cyan
usbipd list
$bus = (usbipd list |
    Select-String -Pattern ([regex]::Escape($HardwareId)) |
    ForEach-Object { ($_ -split '\s+')[0] } |
    Select-Object -First 1)

if ($bus) {
    Write-Host "Binding $HardwareId (busid $bus)..." -ForegroundColor Cyan
    # usbipd writes info messages (e.g. "already shared") to stderr; with
    # $ErrorActionPreference=Stop that would terminate the script, so isolate.
    & { $ErrorActionPreference = 'Continue'; usbipd bind --busid $bus 2>&1 | Out-Null }
} else {
    Write-Host "Device $HardwareId not plugged in right now -- binding skipped." -ForegroundColor Yellow
    Write-Host "Plug it in and re-run, or bind manually once it appears." -ForegroundColor Yellow
}

# 2. Scheduled Task: auto-attach watcher at logon, hidden, highest privileges.
# Wake WSL2 first ('usbipd attach --wsl' fails if no distro is running) and
# loop the attach watcher so it survives WSL restarts. Use a PowerShell
# wrapper (cmd.exe /c can't do labels/goto outside a .bat file).
$psBody = @"
while (`$true) {
  & wsl.exe -d $Distribution -- true
  & usbipd.exe attach --wsl $Distribution --hardware-id $HardwareId --auto-attach
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

Write-Host "Task will run as: $RunAsUser" -ForegroundColor Cyan
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -User $RunAsUser -RunLevel Highest -Force | Out-Null

Write-Host "`nRegistered Scheduled Task '$TaskName'." -ForegroundColor Green
Write-Host "Starting it now so you don't have to log out..." -ForegroundColor Cyan
Start-ScheduledTask -TaskName $TaskName

Write-Host "`nDone. Verify in WSL2:  ls /dev/ttyACM0" -ForegroundColor Green
Write-Host "Tear down later with:  .\usbipd-autoattach.ps1 -Remove" -ForegroundColor DarkGray
