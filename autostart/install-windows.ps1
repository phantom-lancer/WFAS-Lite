# Install WFAS-Lite as a per-user Windows 11 autostart entry.
#
#   powershell -ExecutionPolicy Bypass -File .\install-windows.ps1
#   powershell -ExecutionPolicy Bypass -File .\install-windows.ps1 -Server 192.168.1.125
#
# The client is started at sign-in by wscript.exe with a hidden window, so no
# console stays open.  It runs in the interactive user session, which is what
# WASAPI needs.  Logs go to %LOCALAPPDATA%\WFAS-Lite\wfas-lite.log.

[CmdletBinding()]
param(
    [string]$Exe = "",
    [string]$Server = "",
    [int]$LatencyMs = 400,
    [switch]$NoDiscovery,
    [switch]$NoCopy,
    [switch]$StartNow
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir

if ([string]::IsNullOrWhiteSpace($Exe)) {
    $candidates = @(
        (Join-Path $projectDir "build-final\wfas-lite.exe"),
        (Join-Path $projectDir "build-release\wfas-lite.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { $Exe = $candidate; break }
    }
}
if ([string]::IsNullOrWhiteSpace($Exe) -or -not (Test-Path $Exe)) {
    throw "wfas-lite.exe not found. Pass -Exe <path>."
}
$Exe = (Resolve-Path $Exe).Path

$stateDir = Join-Path $env:LOCALAPPDATA "WFAS-Lite"
New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
$cmdPath = Join-Path $stateDir "wfas-lite-run.cmd"
$vbsPath = Join-Path $stateDir "wfas-lite-run.vbs"
$logPath = Join-Path $stateDir "wfas-lite.log"

# Keep a stable copy outside the build tree so deleting/rebuilding the project
# cannot break the login entry.
if (-not $NoCopy) {
    $installedExe = Join-Path $stateDir "wfas-lite.exe"
    Copy-Item -Path $Exe -Destination $installedExe -Force
    $Exe = $installedExe
}

# Discovery is the default (no address).  A saved server is used as fallback by
# the client itself, so passing -Server is only needed to pin one manually.
$arguments = @()
if (-not [string]::IsNullOrWhiteSpace($Server)) {
    $arguments += ('"{0}"' -f $Server)
    $arguments += "--port 9090"
    if (-not $NoDiscovery) { $arguments += "--discover" }
} else {
    if ($NoDiscovery) { throw "-NoDiscovery requires -Server." }
}
$arguments += "--latency-ms $LatencyMs"
$argumentString = ($arguments -join " ")

$cmdContent = @"
@echo off
setlocal
set "LOG=$logPath"
for %%A in ("%LOG%") do if %%~zA GTR 5242880 move /y "%LOG%" "%LOG%.1" >nul 2>&1
"$Exe" $argumentString >> "%LOG%" 2>&1
"@
Set-Content -Path $cmdPath -Value $cmdContent -Encoding ASCII

# WScript.Shell.Run with window style 0 starts the console app hidden.
$vbsContent = @"
Set sh = CreateObject("WScript.Shell")
sh.Run """" & "$cmdPath" & """", 0, False
"@
Set-Content -Path $vbsPath -Value $vbsContent -Encoding ASCII

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
New-Item -Path $runKey -Force | Out-Null
New-ItemProperty -Path $runKey -Name "WFAS-Lite" `
    -Value "wscript.exe `"$vbsPath`"" -PropertyType String -Force | Out-Null

Write-Host "Installed WFAS-Lite autostart for the current user."
Write-Host "  executable : $Exe"
Write-Host "  arguments  : $argumentString"
Write-Host "  launcher   : $cmdPath"
Write-Host "  log        : $logPath"
Write-Host "  registry   : HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WFAS-Lite"
Write-Host ""
Write-Host "It starts at the next sign-in. Use -StartNow to launch it immediately."

if ($StartNow) {
    Start-Process -FilePath "wscript.exe" -ArgumentList "`"$vbsPath`"" | Out-Null
    Write-Host "Started."
}
