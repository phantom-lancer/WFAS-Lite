# Remove the per-user WFAS-Lite autostart entry.
#
#   powershell -ExecutionPolicy Bypass -File .\uninstall-windows.ps1
#   powershell -ExecutionPolicy Bypass -File .\uninstall-windows.ps1 -RemoveLogs

[CmdletBinding()]
param(
    [switch]$RemoveLogs
)

$ErrorActionPreference = "Stop"

$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
if (Get-ItemProperty -Path $runKey -Name "WFAS-Lite" -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $runKey -Name "WFAS-Lite" -Force
    Write-Host "Removed HKCU Run entry 'WFAS-Lite'."
} else {
    Write-Host "No HKCU Run entry 'WFAS-Lite' was present."
}

$stateDir = Join-Path $env:LOCALAPPDATA "WFAS-Lite"
foreach ($name in @("wfas-lite-run.cmd", "wfas-lite-run.vbs")) {
    $path = Join-Path $stateDir $name
    if (Test-Path $path) {
        Remove-Item $path -Force
        Write-Host "Removed $path"
    }
}

if ($RemoveLogs) {
    Get-ChildItem -Path $stateDir -Filter "wfas-lite.log*" -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item $_.FullName -Force; Write-Host "Removed $($_.FullName)" }
}

Write-Host "Autostart removed. A currently running client is not stopped."
