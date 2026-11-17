; Compile only from build.bat --installer after windeployqt succeeds.
#ifndef DistDir
  #error DistDir is required
#endif
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif
#ifndef NeedsVCRuntime
  #error NeedsVCRuntime is required
#endif
[Setup]
; Stable upstream installer identity, independent of downstream distributions.
AppId={{752B3E5A-F81B-52B2-B78D-C00D485A4CFB}
AppName=Write (Omawrite)
AppVersion={#AppVersion}
AppPublisher=Omacom and contributors
AppPublisherURL=https://github.com/omacom/omawrite
DefaultDirName={localappdata}\Programs\Omawrite
DefaultGroupName=Omawrite
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=Write-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\omawrite.exe
CloseApplications=yes

[Code]
function InitializeSetup(): Boolean;
var
  Installed: Cardinal;
begin
  Result := True;
// /D preprocessor definitions are strings, including /DNeedsVCRuntime=1.
#if NeedsVCRuntime == "1"
  Result := RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and (Installed = 1);
  if not Result then
    MsgBox('Install the latest Microsoft Visual C++ 2015-2022 x64 Redistributable, then run this installer again:' + #13#10 + 'https://aka.ms/vs/17/release/vc_redist.x64.exe', mbError, MB_OK);
#endif
end;

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Write"; Filename: "{app}\omawrite.exe"
Name: "{autodesktop}\Omawrite"; Filename: "{app}\omawrite.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\omawrite.exe"; Description: "Launch Write"; Flags: nowait postinstall skipifsilent

[Messages]
WelcomeLabel2=This installs Write for your account.%n%nMSVC builds require the Microsoft Visual C++ 2015-2022 x64 Redistributable. Mermaid additionally requires separately installed Node.js, Mermaid CLI and its browser. This unsigned installer does not install those prerequisites.%n%nSee the installed notices folder for third-party licenses. Packaging does not establish redistribution clearance.
