# Installs the Ghuleh Miner APK to devices listed by adb (wireless or USB).
#
# By default, SKIPS any device that already succeeded on a previous run of
# this script (tracked in push_state_installed.txt next to this script) --
# so re-running it after a partial run only retries the stragglers
# (failed/timed-out/newly-seen devices) instead of reinstalling over
# everyone, which would restart the app on devices that are already mining.
# Use -Force to push to ALL ready devices regardless of prior success (e.g.
# when you've built a genuinely new APK version and want everyone updated).
#
# Also disables Play Protect's install-time verification scan on each
# device first (adb settings put, standard for adb-deploying custom APKs
# across a fleet) so you don't have to tap through the "scan this app"
# prompt by hand. Use -SkipVerifierDisable to leave that alone.
#
# Each adb command runs with a timeout -- a stuck/flaky wireless device
# gets killed and marked TIMED OUT instead of hanging the whole run.
#
# adb install -r kills the running mining service (it does not resume on
# its own). After each successful install, this script sends a broadcast
# to MiningControlReceiver (dev.ghuleh.miner, added 1.1.4-restartswitch)
# which does exactly what the in-app Start Mining button does, using
# whatever config (CIVIC or Verus, wallet/worker) is already saved on that
# phone -- no re-entry needed. Requires the phone to be running
# 1.1.4-restartswitch or newer; older builds simply won't have the
# receiver, and the broadcast is a harmless no-op on them. Use
# -NoAutoRestart to skip this and leave phones stopped (e.g. if you want
# to reopen and eyeball each one by hand instead).
#
# Usage:
#   .\push_to_devices.ps1                    # only new/failed/timed-out devices
#   .\push_to_devices.ps1 -Force              # push to everyone, e.g. a real app update
#   .\push_to_devices.ps1 -ApkPath "C:\path\to\other.apk"
#   .\push_to_devices.ps1 -AdbPath "D:\somewhere\else\adb.exe"
#   .\push_to_devices.ps1 -TimeoutSec 40
#   .\push_to_devices.ps1 -SkipVerifierDisable
#   .\push_to_devices.ps1 -NoAutoRestart

param(
    [string]$ApkPath = "$env:USERPROFILE\Downloads\ghuleh-miner-release.apk",
    [string]$AdbPath = "C:\platform-tools\adb.exe",
    [int]$TimeoutSec = 25,
    [switch]$SkipVerifierDisable,
    [switch]$Force,
    [switch]$NoAutoRestart
)

$StateFile = Join-Path $PSScriptRoot "push_state_installed.txt"

if (-not (Test-Path $ApkPath)) {
    Write-Host "APK not found at: $ApkPath" -ForegroundColor Red
    Write-Host "Pass the right path with: .\push_to_devices.ps1 -ApkPath `"C:\full\path\to\file.apk`"" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $AdbPath)) {
    Write-Host "adb.exe not found at: $AdbPath" -ForegroundColor Red
    Write-Host "Pass the right path with: .\push_to_devices.ps1 -AdbPath `"C:\full\path\to\adb.exe`"" -ForegroundColor Yellow
    exit 1
}

function Invoke-AdbTimeout {
    param(
        [string]$Arguments,
        [int]$TimeoutSec
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $AdbPath
    $psi.Arguments = $Arguments
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()

    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try { $proc.Kill() } catch {}
        return [pscustomobject]@{ TimedOut = $true; Output = "" }
    }

    $out = $proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd()
    return [pscustomobject]@{ TimedOut = $false; Output = $out }
}

$alreadyInstalled = @{}
if (Test-Path $StateFile) {
    Get-Content $StateFile | Where-Object { $_.Trim() -ne "" } | ForEach-Object { $alreadyInstalled[$_.Trim()] = $true }
}

Write-Host "Using APK: $ApkPath"
Write-Host "Using adb: $AdbPath"
Write-Host "Per-command timeout: ${TimeoutSec}s"
if ($Force) {
    Write-Host "Mode: -Force -- pushing to ALL ready devices, including ones already marked installed" -ForegroundColor Yellow
} else {
    Write-Host "Mode: normal -- skipping $($alreadyInstalled.Count) device(s) already marked installed from previous runs"
}
Write-Host "Querying adb devices..."

$rawDevices = & $AdbPath devices
$ready = $rawDevices | Select-String -Pattern "^\S+\s+device$" | ForEach-Object {
    ($_ -split "\s+")[0]
}
$notReady = $rawDevices | Select-String -Pattern "^\S+\s+(unauthorized|offline)$" | ForEach-Object {
    ($_ -split "\s+")[0]
}

if ($notReady.Count -gt 0) {
    Write-Host ""
    Write-Host "$($notReady.Count) device(s) not ready (need on-device approval or reconnect) -- skipping these:" -ForegroundColor Yellow
    $notReady | ForEach-Object { Write-Host "  $_" }
}

if ($ready.Count -eq 0) {
    Write-Host ""
    Write-Host "No installable devices found. Run '$AdbPath devices' to check connections." -ForegroundColor Red
    exit 1
}

$alreadySkipped = @()
if ($Force) {
    $targets = $ready
} else {
    $targets = @()
    foreach ($d in $ready) {
        if ($alreadyInstalled.ContainsKey($d)) {
            $alreadySkipped += $d
        } else {
            $targets += $d
        }
    }
}

if ($alreadySkipped.Count -gt 0) {
    Write-Host ""
    Write-Host "Already installed, skipping (use -Force to reinstall these too):" -ForegroundColor DarkGray
    $alreadySkipped | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
}

if ($targets.Count -eq 0) {
    Write-Host ""
    Write-Host "Nothing to do -- every ready device is already marked installed. Use -Force to reinstall anyway." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host "Installing to $($targets.Count) device(s)..." -ForegroundColor Cyan
Write-Host ""

$successList = @()
$failList = @()
$timeoutList = @()
$verifierFailList = @()
$restartFailList = @()
$i = 0
$quotedApk = '"' + $ApkPath + '"'

foreach ($d in $targets) {
    $i++
    Write-Host "[$i/$($targets.Count)] $d ... " -NoNewline

    if (-not $SkipVerifierDisable) {
        $v1 = Invoke-AdbTimeout -Arguments "-s $d shell settings put global verifier_verify_adb_installs 0" -TimeoutSec $TimeoutSec
        $v2 = Invoke-AdbTimeout -Arguments "-s $d shell settings put global package_verifier_enable 0" -TimeoutSec $TimeoutSec
        if ($v1.TimedOut -or $v2.TimedOut -or $v1.Output -match "Error|error" -or $v2.Output -match "Error|error") {
            $verifierFailList += $d
        }
    }

    $result = Invoke-AdbTimeout -Arguments "-s $d install -r $quotedApk" -TimeoutSec $TimeoutSec

    if ($result.TimedOut) {
        Write-Host "TIMED OUT (skipped)" -ForegroundColor Magenta
        $timeoutList += $d
    } elseif ($result.Output -match "Success") {
        Write-Host "OK" -ForegroundColor Green
        $successList += $d

        if (-not $NoAutoRestart) {
            $restart = Invoke-AdbTimeout -Arguments "-s $d shell am broadcast -a dev.ghuleh.miner.action.START_MINING -n dev.ghuleh.miner/.MiningControlReceiver" -TimeoutSec $TimeoutSec
            if ($restart.TimedOut -or $restart.Output -match "Error") {
                Write-Host "    restart signal failed (older build, or phone unreachable) -- reopen by hand" -ForegroundColor Yellow
                $restartFailList += $d
            }
        }
    } else {
        Write-Host "FAILED" -ForegroundColor Red
        Write-Host "    $($result.Output)" -ForegroundColor DarkGray
        $failList += $d
    }
}

# Record newly-successful devices so the next run skips them.
foreach ($d in $successList) { $alreadyInstalled[$d] = $true }
$alreadyInstalled.Keys | Sort-Object | Set-Content -Path $StateFile

Write-Host ""
Write-Host "======================================" -ForegroundColor Cyan
Write-Host "Done: $($successList.Count)/$($targets.Count) succeeded this run." -ForegroundColor Cyan
Write-Host "Total marked installed overall: $($alreadyInstalled.Count)" -ForegroundColor Cyan
if ($failList.Count -gt 0) {
    Write-Host ""
    Write-Host "Failed on:" -ForegroundColor Yellow
    $failList | ForEach-Object { Write-Host "  $_" }
}
if ($timeoutList.Count -gt 0) {
    Write-Host ""
    Write-Host "Timed out (dead/stuck connection -- rerun later, they may reconnect):" -ForegroundColor Magenta
    $timeoutList | ForEach-Object { Write-Host "  $_" }
}
if ($verifierFailList.Count -gt 0) {
    Write-Host ""
    Write-Host "Couldn't disable the verify prompt on (installed anyway, may have shown the scan dialog):" -ForegroundColor Yellow
    $verifierFailList | ForEach-Object { Write-Host "  $_" }
}
if (-not $NoAutoRestart -and $restartFailList.Count -gt 0) {
    Write-Host ""
    Write-Host "Installed OK but the restart signal didn't go through -- reopen the app and tap Start Mining by hand on:" -ForegroundColor Yellow
    $restartFailList | ForEach-Object { Write-Host "  $_" }
}
if ($notReady.Count -gt 0) {
    Write-Host ""
    Write-Host "Skipped (not ready):" -ForegroundColor Yellow
    $notReady | ForEach-Object { Write-Host "  $_" }
}

Write-Host ""
Write-Host "State saved to: $StateFile" -ForegroundColor DarkGray
Write-Host "(Delete that file, or use -Force, if you ever want a clean full push to everyone.)" -ForegroundColor DarkGray

# To put the scan prompt back on a device later:
#   & "C:\platform-tools\adb.exe" -s <serial> shell settings put global verifier_verify_adb_installs 1
#   & "C:\platform-tools\adb.exe" -s <serial> shell settings put global package_verifier_enable 1
