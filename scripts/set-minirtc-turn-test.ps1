# Dot-source this script to set the caller PowerShell environment.
# Example: . .\scripts\set-minirtc-turn-test.ps1 -Family ipv4 -Port 3478
# Running as a separate powershell.exe process will not modify the parent shell.
param(
    [ValidateSet('any', 'ipv4', 'ipv6')]
    [string]$Family = 'any',
    [ValidateSet('any', '3478', '80')]
    [string]$Port = 'any',
    [switch]$Libnice
)
$ErrorActionPreference = 'Stop'
if ($Family -eq 'any') {
    Remove-Item Env:\REDESK_TURN_DIAG_FAMILY -ErrorAction SilentlyContinue
} else {
    $env:REDESK_TURN_DIAG_FAMILY = $Family
}
if ($Port -eq 'any') {
    Remove-Item Env:\REDESK_TURN_DIAG_PORT -ErrorAction SilentlyContinue
} else {
    $env:REDESK_TURN_DIAG_PORT = $Port
}
if ($Libnice) {
    $env:REDESK_TURN_DIAG_ICE_LOG = '1'
    $env:NICE_DEBUG = 'nice,stun'
    $env:G_MESSAGES_DEBUG = 'libnice,libnice-stun'
} else {
    Remove-Item Env:\REDESK_TURN_DIAG_ICE_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:\NICE_DEBUG -ErrorAction SilentlyContinue
    Remove-Item Env:\G_MESSAGES_DEBUG -ErrorAction SilentlyContinue
}
Write-Host "TURN/TCP test environment: Family=$Family Port=$Port Libnice=$($Libnice.IsPresent)" -ForegroundColor Cyan
Write-Host "Launch ReDesk from THIS PowerShell session to inherit these variables."
Write-Host "An already running GUI process or a Windows service will not inherit them."
