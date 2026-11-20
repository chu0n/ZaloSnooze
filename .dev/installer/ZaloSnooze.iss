#ifndef AppVersion
  #error AppVersion must be supplied by package.ps1
#endif
#define Root SourcePath + "\..\.."
#ifndef InstallerAppId
  #define InstallerAppId "{{D6126722-6785-4DA8-BF04-45427867FF21}"
#endif
#ifndef InstallerAppName
  #define InstallerAppName "ZaloSnooze"
#endif
#ifndef OutputDirectory
  #define OutputDirectory Root + "\.dev\release"
#endif
[Setup]
AppId={#InstallerAppId}
AppName={#InstallerAppName}
AppVersion={#AppVersion}
AppPublisher=ZaloSnooze contributors
DefaultDirName={autopf}\ZaloSnooze
DefaultGroupName={#InstallerAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
MinVersion=10.0
OutputDir={#OutputDirectory}
OutputBaseFilename=ZaloSnooze-{#AppVersion}-Setup
SetupIconFile={#Root}\docs\assets\logo.ico
UninstallDisplayIcon={app}\ZaloSnooze.exe
UninstallDisplayName=ZaloSnooze
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
WizardImageFile=
WizardSmallImageFile={#Root}\docs\assets\logo-transparent.png
CloseApplications=yes
RestartApplications=no
SetupMutex=ZaloSnooze.Setup
VersionInfoVersion={#AppVersion}.0
LicenseFile={#Root}\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "startup"; Description: "Start Zalo in the tray when I sign in (asks before changing Zalo startup)"; GroupDescription: "Current user only:"; Flags: unchecked

[Files]
Source: "{#Root}\.dev\build\ZaloSnooze.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\.dev\config\installed.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#InstallerAppName}\{#InstallerAppName}"; Filename: "{app}\ZaloSnooze.exe"; Parameters: "--settings"; WorkingDir: "{app}"
Name: "{autodesktop}\{#InstallerAppName}"; Filename: "{app}\ZaloSnooze.exe"; Parameters: "--settings"; WorkingDir: "{app}"; Tasks: desktopicon

[InstallDelete]
Type: files; Name: "{autoprograms}\{#InstallerAppName}\Uninstall {#InstallerAppName}.lnk"

[Run]
Filename: "{app}\ZaloSnooze.exe"; Parameters: "--enable-startup"; Description: "Configure startup for my account"; Flags: runasoriginaluser waituntilterminated; Tasks: startup
Filename: "{app}\ZaloSnooze.exe"; Parameters: "--settings"; Description: "Open ZaloSnooze"; Flags: postinstall nowait skipifsilent runasoriginaluser

[Code]
function InitializeUninstall(): Boolean;
begin
  Result := True;
  if IsAdminInstallMode then
    MsgBox('Before an all-user uninstall, each user must open ZaloSnooze without administrator rights and disable startup. This elevated uninstaller will not replay user-writable startup backups. AppData settings and recovery files are kept.', mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if (CurUninstallStep = usUninstall) and not IsAdminInstallMode then
  begin
    if not Exec(ExpandConstant('{app}\ZaloSnooze.exe'), '--uninstall-user', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      MsgBox('Could not run startup cleanup. Settings and startup backups remain in AppData.', mbError, MB_OK)
    else if ResultCode <> 0 then
      MsgBox('Some startup entries could not be restored. Settings and backups remain in AppData. Reinstall to retry restoration.', mbError, MB_OK);
  end;
end;
