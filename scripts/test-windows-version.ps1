param([string]$ExpectedVersion, [switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$ExpectedVersion) { $ExpectedVersion = [IO.File]::ReadAllText([IO.Path]::Combine($project, 'VERSION')).Trim() }
$inputFolder = if ($BuildOnly) { 'build\native-windows' } else { 'dist\BigHeadVPN-Native' }
$files = @(
    [IO.Path]::Combine($project, $inputFolder, 'BigHeadVPN.exe'),
    [IO.Path]::Combine($project, $inputFolder, 'BigHeadVPNStartup.exe')
)
if (!$BuildOnly) { $files += [IO.Path]::Combine($project, 'dist', "BigHeadVPN-$ExpectedVersion-windows-x64-setup.exe") }
$temp = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'BigHeadVPN-version-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    foreach ($file in $files) {
        $localFile = [IO.Path]::Combine($temp, [IO.Path]::GetFileName($file))
        Copy-Item -LiteralPath $file -Destination $localFile
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($localFile)
        if ($info.FileVersion.Trim() -ne "$ExpectedVersion.0") { throw "Incorrect FileVersion for ${file}: $($info.FileVersion)" }
        if ($info.ProductVersion.Trim() -ne $ExpectedVersion) { throw "Incorrect ProductVersion for ${file}: $($info.ProductVersion)" }
        if ($info.ProductName.Trim() -ne 'Big Head VPN') { throw "Incorrect ProductName for ${file}: $($info.ProductName)" }
        Write-Output "PASS: $([IO.Path]::GetFileName($file)) FileVersion=$($info.FileVersion) ProductVersion=$($info.ProductVersion)"
    }
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}
