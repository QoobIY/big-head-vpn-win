$ErrorActionPreference = "Stop"
$version = "1.13.14"
$archiveName = "sing-box-$version-windows-amd64.zip"
$baseUrl = "https://github.com/SagerNet/sing-box/releases/download/v$version"
$tempRoot = Join-Path $env:TEMP ("big-head-vpn-sing-box-" + [Guid]::NewGuid())
$archive = Join-Path $tempRoot $archiveName
$checksumFile = Join-Path $tempRoot "checksums.txt"
$unpack = Join-Path $tempRoot "unpack"

try {
    New-Item $tempRoot -ItemType Directory -Force | Out-Null
    Write-Host "Downloading verified sing-box $version..."
    Invoke-WebRequest "$baseUrl/$archiveName" -OutFile $archive
    Invoke-WebRequest "$baseUrl/sing-box-$version-checksums.txt" -OutFile $checksumFile

    $checksums = Get-Content $checksumFile -Raw
    $match = [regex]::Match($checksums, "(?im)^([0-9a-f]{64})\s+\*?$([regex]::Escape($archiveName))$")
    if (-not $match.Success) { throw "Checksum for $archiveName is missing." }
    $actual = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $match.Groups[1].Value.ToLowerInvariant()) { throw "sing-box checksum mismatch." }

    Expand-Archive $archive $unpack
    $binary = Get-ChildItem $unpack -Recurse -Filter sing-box.exe | Select-Object -First 1
    if (-not $binary) { throw "sing-box.exe is missing in the downloaded archive." }
    $tools = Join-Path $PSScriptRoot "..\tools"
    New-Item $tools -ItemType Directory -Force | Out-Null
    Copy-Item (Join-Path $binary.Directory.FullName "*") $tools -Recurse -Force
    Write-Host "Installed verified sing-box $version to tools\sing-box.exe"
} finally {
    Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
