$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "build.ps1")
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$target = Join-Path $root "dist\Big Head VPN"
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
jpackage --type app-image --name "Big Head VPN" --input (Join-Path $root "build") --main-jar "big-head-vpn.jar" --main-class app.bighead.vpn.Main --icon (Join-Path $root "packaging\big-head-vpn.ico") --dest (Join-Path $root "dist")
Copy-Item (Join-Path $root "tools") (Join-Path $root "dist\Big Head VPN\tools") -Recurse -Force
Write-Host "Packaged dist\Big Head VPN"
