# run_vr.ps1 -- start SteamVR, give it time to come up, then launch vkQuake in VR.
#
# The active OpenXR runtime here is VirtualDesktopXR, which does not strictly
# need SteamVR, but starting it first makes the sequence deterministic and
# matches how the headset is normally brought up.
#
# Usage:   .\run_vr.ps1              (normal)
#          .\run_vr.ps1 -Wait 20     (slower headset bring-up)
#          .\run_vr.ps1 -NoSteamVR   (skip SteamVR entirely)
#          .\run_vr.ps1 -Log         (wipe + tail the console log afterwards)

param(
    [int]    $Wait = 10,
    [switch] $NoSteamVR,
    [switch] $Log,
    [string] $BaseDir = "D:\SteamLibrary\steamapps\common\Quake",
    [string[]] $ExtraArgs = @()
)

$ErrorActionPreference = 'Stop'

$steamvr = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrstartup.exe"
$vkquake = "E:\vkQuake\Windows\VisualStudio\Build-vkQuake\x64\Release\vkQuake.exe"
$logfile = "$env:APPDATA\vkQuake\qconsole.log"

if (-not (Test-Path $vkquake)) { throw "vkQuake not built: $vkquake" }

# never leave a previous instance holding the XR session
Get-Process vkQuake -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

if (-not $NoSteamVR) {
    if (Get-Process vrmonitor -ErrorAction SilentlyContinue) {
        Write-Host "SteamVR already running."
    }
    elseif (Test-Path $steamvr) {
        Write-Host "Starting SteamVR..."
        Start-Process -FilePath $steamvr | Out-Null
        Write-Host "Waiting $Wait seconds for the headset..."
        Start-Sleep -Seconds $Wait
    }
    else {
        Write-Warning "SteamVR not found at $steamvr - continuing without it."
    }
}

if ($Log) { Remove-Item $logfile -ErrorAction SilentlyContinue }

$argList = @('-basedir', "`"$BaseDir`"", '-vr', '-condebug') + $ExtraArgs
Write-Host "Launching vkQuake: $($argList -join ' ')"

# detached, so it does not die with this shell
Start-Process -FilePath $vkquake -ArgumentList $argList -WorkingDirectory (Split-Path $vkquake) | Out-Null

Start-Sleep -Seconds 3
$p = Get-Process vkQuake -ErrorAction SilentlyContinue
if ($p) { Write-Host "vkQuake running (pid $($p.Id))." }
else    { Write-Warning "vkQuake exited immediately - check $logfile" }

if ($Log) {
    Start-Sleep -Seconds 10
    Write-Host "`n--- OpenXR / blit diagnostics ---"
    Get-Content $logfile -ErrorAction SilentlyContinue |
        Select-String -Pattern '^XR:|XR blit rejected|OpenXR:' |
        Select-Object -Last 20
}
