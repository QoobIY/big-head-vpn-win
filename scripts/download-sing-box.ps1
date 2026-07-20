$ErrorActionPreference = "Stop"
$release = Invoke-RestMethod "https://api.github.com/repos/SagerNet/sing-box/releases/latest"
$asset = $release.assets | Where-Object { $_.name -match '^sing-box-.*-windows-amd64\.zip$' } | Select-Object -First 1
if (-not $asset) { throw "Windows amd64 archive was not found in the latest sing-box release" }
$temp = Join-Path $env:TEMP $asset.name
Invoke-WebRequest $asset.browser_download_url -OutFile $temp
$unpack = Join-Path $env:TEMP "big-head-vpn-sing-box"
if (Test-Path $unpack) { Remove-Item $unpack -Recurse -Force }
Expand-Archive $temp $unpack
$tools = Join-Path $PSScriptRoot "..\tools"
New-Item $tools -ItemType Directory -Force | Out-Null
$binary = Get-ChildItem $unpack -Recurse -Filter sing-box.exe | Select-Object -First 1
if (-not $binary) { throw "sing-box.exe is missing in the downloaded archive" }
Copy-Item (Join-Path $binary.Directory.FullName "*") $tools -Recurse -Force
Write-Host "Installed sing-box $($release.tag_name) to tools\sing-box.exe"
