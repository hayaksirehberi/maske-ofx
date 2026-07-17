; Inno Setup script for the Maske OFX plugin (Windows).
;
; Produces a single double-clickable Setup.exe that installs
; HistoryBrush.ofx.bundle into the standard OFX plugin folder
; (C:\Program Files\Common Files\OFX\Plugins) — no folder wrangling for users.
;
; Build:
;   1. Install Inno Setup 6:  https://jrsoftware.org/isdl.php
;   2. Build the Windows plugin first so build\HistoryBrush.ofx.bundle exists
;      (see the repo README — CMake + CUDA path).
;   3. Compile this script:
;        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\windows\Maske.iss
;      or open it in the Inno Setup IDE and press Build.
;   Output: dist\Maske-1.2-Windows-Setup.exe
;
; Optional code signing (recommended for distribution — avoids SmartScreen warnings):
;   add  /DSIGNTOOL  and configure [Setup] SignTool, or sign the output .exe
;   afterwards with your certificate.

#define AppName        "Maske"
#define AppVersion     "1.2"
#define AppPublisher   "Mustafa Ekinci"
#define AppId          "com.mustafaekinci.Maske"
#define BundleName     "HistoryBrush.ofx.bundle"

; Path to the built bundle, relative to this .iss file.
#define BundleSource   "..\..\build\" + BundleName

[Setup]
AppId={{8F2E7A10-4C3B-4E6D-9B1A-4D2E5C7F1A22}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppSupportURL=https://www.instagram.com/must.ekinci
DefaultDirName={commoncf}\OFX\Plugins
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\{#BundleName}\Contents\Win64\HistoryBrush.ofx
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=Maske-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; OFX plugins live under Program Files\Common Files — needs admin.
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

[Messages]
WelcomeLabel2=This will install [name/ver] into the DaVinci Resolve OFX plugin folder.%n%nDaVinci Resolve Studio is required (the free edition does not load third-party OFX plugins).%n%nPlease QUIT DaVinci Resolve before continuing — it only scans for plugins at launch.

[Files]
; Recursively copy the whole .ofx.bundle tree into Common Files\OFX\Plugins.
Source: "{#BundleSource}\*"; DestDir: "{app}\{#BundleName}"; \
    Flags: recursesubdirs createallsubdirs ignoreversion

[Code]
// Warn (but don't hard-block) if Resolve appears to be running, since a locked
// .ofx can't be overwritten.
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  if Exec('cmd.exe', '/C tasklist /FI "IMAGENAME eq Resolve.exe" | find /I "Resolve.exe" >nul',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    if ResultCode = 0 then
      if MsgBox('DaVinci Resolve appears to be running. The plugin file may be locked and '
                + 'the install could fail.'#13#10#13#10'Quit Resolve first, then click Yes to continue.',
                mbConfirmation, MB_YESNO) = IDNO then
        Result := False;
  end;
end;

[Run]
; Nothing to launch; the plugin is loaded by Resolve at next startup.

[UninstallDelete]
Type: filesandordirs; Name: "{app}\{#BundleName}"
