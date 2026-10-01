param([string]$CompilerPath)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = [IO.File]::ReadAllText([IO.Path]::Combine($project, 'VERSION')).Trim()
if ($version -notmatch '^([0-9]+)\.([0-9]+)\.([0-9]+)$') { throw 'VERSION must contain major.minor.patch' }
$components = $version.Split('.')
foreach ($component in $components) {
    if ([int]$component -gt 65535) { throw 'Windows version components must be <= 65535' }
}
if (!$CompilerPath) {
    $candidates = @(
        [IO.Path]::Combine($project, '.build-tools\inno-setup\app\ISCC.exe'),
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
    )
    $CompilerPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (!$CompilerPath -or !(Test-Path -LiteralPath $CompilerPath)) {
    throw 'Inno Setup compiler not found. Install Inno Setup or pass -CompilerPath to ISCC.exe.'
}
$source = [IO.Path]::Combine($project, 'dist\BigHeadVPN-Native')
$output = [IO.Path]::Combine($project, 'dist')
$script = [IO.Path]::Combine($project, 'packaging\windows-installer.iss')
$buildStage = $null
$finalOutput = $output
try {
    # WSL shares are case-sensitive and Windows version APIs may return empty
    # results for their EXEs. Compile using local temporary inputs in that case.
    if ($source.StartsWith('\\') -or $CompilerPath.StartsWith('\\')) {
        $buildStage = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'BigHeadVPN-installer-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $buildStage | Out-Null
        if ($CompilerPath.StartsWith('\\')) {
            $toolStage = [IO.Path]::Combine($buildStage, 'compiler')
            Copy-Item -LiteralPath ([IO.Path]::GetDirectoryName($CompilerPath)) -Destination $toolStage -Recurse
            $CompilerPath = [IO.Path]::Combine($toolStage, 'ISCC.exe')
        }
        $sourceStage = [IO.Path]::Combine($buildStage, 'payload')
        Copy-Item -LiteralPath $source -Destination $sourceStage -Recurse
        $source = $sourceStage
        Copy-Item -LiteralPath $script -Destination $buildStage
        Copy-Item -LiteralPath ([IO.Path]::Combine($project, 'packaging\big-head-vpn.ico')) -Destination $buildStage
        $script = [IO.Path]::Combine($buildStage, 'windows-installer.iss')
        $output = [IO.Path]::Combine($buildStage, 'output')
    }
    & $CompilerPath "/DAppVersion=$version" "/DAppMajor=$($components[0])" "/DAppMinor=$($components[1])" "/DAppPatch=$($components[2])" "/DSourceDir=$source" "/DOutputDir=$output" $script
    if ($LASTEXITCODE -ne 0) { throw "Installer compiler exited with code $LASTEXITCODE" }
    if ($buildStage) {
        Copy-Item -LiteralPath ([IO.Path]::Combine($output, "BigHeadVPN-$version-windows-x64-setup.exe")) -Destination $finalOutput
    }
} finally {
    if ($buildStage) { Remove-Item -LiteralPath $buildStage -Recurse -Force }
}
$output = $finalOutput
$installer = [IO.Path]::Combine($output, "BigHeadVPN-$version-windows-x64-setup.exe")
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$installer.sha256", "$hash  $([IO.Path]::GetFileName($installer))`n", [Text.Encoding]::ASCII)
Write-Output "Installer: $installer"
Write-Output "Bytes: $((Get-Item -LiteralPath $installer).Length)"
