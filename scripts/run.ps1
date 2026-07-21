$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $root
& ".\scripts\build.ps1"
if (-not (Test-Path ".\tools\sing-box.exe")) { & ".\scripts\download-sing-box.ps1" }
javaw -jar ".\build\big-head-vpn.jar"
