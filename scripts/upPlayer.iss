; Inno Setup script for upPlayer.
; Compile via scripts\package-setup.ps1 (or: ISCC.exe /DSourceDir=...\package /DOutputDir=...\dist upPlayer.iss)

#ifndef SourceDir
  #define SourceDir "..\build\dist\package"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\dist"
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SetupSuffix
  #define SetupSuffix ""
#endif

#define MyAppName "upPlayer"
#define MyAppPublisher "upPlayer"
#define MyAppURL "https://github.com/moneypi/upPlayer"
#define MyAppExeName "upPlayer.exe"

[Setup]
AppId={{A7C3E1D2-5B84-4F19-9E6A-2D8C4B1F0A73}
AppName={#MyAppName}
AppVersion={#AppVersion}
AppVerName={#MyAppName} {#AppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf64}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=
InfoBeforeFile=
OutputDir={#OutputDir}
OutputBaseFilename=upPlayer-Setup-x64-{#AppVersion}{#SetupSuffix}
SetupIconFile=..\src\upplayer.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
; PotPlayer-style Open with / Default Apps
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register-assoc.ps1"" -Action Register -ExePath ""{app}\{#MyAppExeName}"" -AppName ""{#MyAppName}"""; StatusMsg: "Registering Open with..."; Flags: runhidden waituntilterminated

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\register-assoc.ps1"" -Action Unregister -ExePath ""{app}\{#MyAppExeName}"" -AppName ""{#MyAppName}"""; RunOnceId: "UnregAssoc"; Flags: runhidden waituntilterminated
