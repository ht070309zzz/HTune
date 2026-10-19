; Built by package-release.ps1. Never install or start HTune during packaging.
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef PayloadDir
  #error PayloadDir is required
#endif
#ifndef ReleaseDir
  #error ReleaseDir is required
#endif

[Setup]
AppId={{DBE9C907-8158-4D81-AB87-EBE064B7B91F}
AppName=HTune
AppVersion={#AppVersion}
AppPublisher=ht070309zzz
AppPublisherURL=https://github.com/ht070309zzz/HTune
AppSupportURL=https://github.com/ht070309zzz/HTune/issues
AppUpdatesURL=https://github.com/ht070309zzz/HTune/releases
DefaultDirName={localappdata}\Programs\HTune
DefaultGroupName=HTune
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
AppMutex=Local\MusicStage.NativePrototype.0.1
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\HTune.exe
SetupIconFile={#PayloadDir}\assets\htune.ico
LicenseFile={#PayloadDir}\LICENSE
OutputDir={#ReleaseDir}
OutputBaseFilename=HTune-Setup-{#AppVersion}-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}.0
VersionInfoProductName=HTune
VersionInfoDescription=HTune Setup

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Payload is a new allowlisted directory produced by package-release.ps1.
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\HTune"; Filename: "{app}\HTune.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\HTune"; Filename: "{app}\HTune.exe"; WorkingDir: "{app}"; Tasks: desktopicon

; No automatic launch, process termination, startup registration, or user-data deletion.
