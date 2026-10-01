; Version and paths are supplied by scripts/build-windows-installer.ps1.
#ifndef AppVersion
  #error AppVersion must be supplied by the build script
#endif
#ifndef SourceDir
  #error SourceDir must point to the complete portable distribution
#endif
#ifndef OutputDir
  #error OutputDir must point to the distribution directory
#endif
#define AppName "Big Head VPN"
#define AppExe "BigHeadVPN.exe"
#if GetVersionNumbersString(AddBackslash(SourceDir) + AppExe) != AppVersion + ".0"
  #error EXE version does not match VERSION. Rebuild the native application first.
#endif

[Setup]
AppId={{D0A54D4C-2A41-4D26-A8EC-8D638938A91B}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
DefaultDirName={autopf}\BigHeadVPN
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
UsePreviousAppDir=yes
PrivilegesRequired=admin
; WinDivert64.sys requires native x64 Windows, including on newer compilers.
#if VER >= 0x06030000
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
#else
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
#endif
MinVersion=10.0
AppMutex=Local\BigHeadVPNNative
SetupMutex=BigHeadVPNSetup
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
OutputDir={#OutputDir}
OutputBaseFilename=BigHeadVPN-{#AppVersion}-windows-x64-setup
SetupIconFile=big-head-vpn.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription={#AppName} Setup

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\BigHeadVPN.exe"; DestDir: "{app}"; Flags: overwritereadonly
Source: "{#SourceDir}\BigHeadVPNStartup.exe"; DestDir: "{app}"; Flags: overwritereadonly
Source: "{#SourceDir}\msquic.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinDivert.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinDivert64.sys"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\*.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\VERSION"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}"; Tasks: desktopicon

; Autostart stays under the application's button, so updating does not override
; the user's Windows startup switch or create a second startup registration.
[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--remove-autostart"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveOwnedAutostart"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  VersionMS, VersionLS: Cardinal;
begin
  Result := '';
  if GetVersionNumbers(ExpandConstant('{app}\{#AppExe}'), VersionMS, VersionLS) then
  begin
    if (VersionMS > (({#AppMajor} shl 16) or {#AppMinor})) or
       ((VersionMS = (({#AppMajor} shl 16) or {#AppMinor})) and
        (VersionLS > ({#AppPatch} shl 16))) then
      Result := 'A newer version of Big Head VPN is already installed. / Уже установлена более новая версия Big Head VPN.';
  end;
end;
