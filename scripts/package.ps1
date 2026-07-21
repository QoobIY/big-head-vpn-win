$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "build.ps1")
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not (Test-Path (Join-Path $root "tools\sing-box.exe"))) { & (Join-Path $PSScriptRoot "download-sing-box.ps1") }
$target = Join-Path $root "dist\BigHeadVPN"
if (Test-Path $target) { Remove-Item $target -Recurse -Force }
jpackage --type app-image --name "BigHeadVPN" --input (Join-Path $root "build") --main-jar "big-head-vpn.jar" --main-class app.bighead.vpn.Main --icon (Join-Path $root "packaging\big-head-vpn.ico") --dest (Join-Path $root "dist") --app-version "1.0.0" --vendor "Big Head"
Copy-Item (Join-Path $root "tools") (Join-Path $root "dist\BigHeadVPN\tools") -Recurse -Force
Copy-Item (Join-Path $root "THIRD_PARTY_NOTICES.md") (Join-Path $root "dist\BigHeadVPN\THIRD_PARTY_NOTICES.md") -Force
Write-Host "Packaged dist\BigHeadVPN"
