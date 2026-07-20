$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
  Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`""
  exit
}
Set-Location $root
& ".\scripts\build.ps1"
if (-not (Test-Path ".\tools\sing-box.exe")) { & ".\scripts\download-sing-box.ps1" }
javaw -jar ".\build\big-head-vpn.jar"
