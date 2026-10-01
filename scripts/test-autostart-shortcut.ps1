$ErrorActionPreference = 'Stop'
$folder = (Resolve-Path (Join-Path $PSScriptRoot '..\build\native-windows')).ProviderPath
$folder = [IO.Path]::GetFullPath($folder)
$linkPath = [IO.Path]::Combine($folder, 'Тест автозагрузки.lnk')
try {
    & ([IO.Path]::Combine($folder, 'BigHeadVPNProbe.exe')) --startup-shortcut-fixture $linkPath
    if ($LASTEXITCODE -ne 0) { throw 'Shortcut fixture failed' }
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($linkPath)
    if ($link.TargetPath -ne ([IO.Path]::Combine($folder, 'BigHeadVPNStartup.exe'))) { throw "Incorrect target: $($link.TargetPath)" }
    if ($link.Arguments -ne '') { throw "Incorrect arguments: $($link.Arguments)" }
    if ($link.WindowStyle -ne 1) { throw "Incorrect window style: $($link.WindowStyle)" }
    if ($link.IconLocation -notlike '*BigHeadVPNProbe.exe,0') { throw "Incorrect icon: $($link.IconLocation)" }
    Write-Output 'PASS: Unicode startup shortcut, task target, arguments, windowless launcher, application icon'
} finally {
    Remove-Item $linkPath -ErrorAction SilentlyContinue
}
