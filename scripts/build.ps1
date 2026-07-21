$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$classes = Join-Path $root "build\classes"
if (Test-Path $classes) { Remove-Item $classes -Recurse -Force }
New-Item $classes -ItemType Directory -Force | Out-Null
$sources = Get-ChildItem (Join-Path $root "src\main\java") -Recurse -Filter *.java | ForEach-Object FullName
javac --release 21 -encoding UTF-8 -d $classes $sources
if (Test-Path (Join-Path $root "src\main\resources")) { Copy-Item (Join-Path $root "src\main\resources\*") $classes -Recurse -Force }
jar --create --file (Join-Path $root "build\big-head-vpn.jar") --main-class app.bighead.vpn.Main -C $classes .
Write-Host "Built build\big-head-vpn.jar"
