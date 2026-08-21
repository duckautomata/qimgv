; ============================================================================
;  Inno Setup script for the qimgv Windows installer.
;
;  Build it from an already-packaged tree:
;      ./scripts/package-windows.sh
;      ISCC /DAppVersion=2.0.0 scripts/qimgv.iss
;
;  Output: build/qimgv-<version>-win64-setup.exe
;
;  Per-user by design (PrivilegesRequired=lowest). It installs under
;  %LOCALAPPDATA%\Programs, so there is no UAC prompt and no admin account
;  needed, which is what most people expect from an image viewer. It also keeps
;  the install directory writable, though qimgv no longer relies on that --
;  see the DirExists note below.
; ============================================================================

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef DistDir
  #define DistDir "..\build\dist"
#endif

#define AppName    "qimgv"
#define AppExeName "qimgv.exe"
#define AppPublisher "duckautomata"
#define AppUrl     "https://github.com/duckautomata/qimgv"

[Setup]
; Never change AppId: it is what lets an upgrade replace the previous install
; instead of piling up a second entry in Apps & Features.
AppId={{8F7A5219-4F85-4668-812F-8651DEE9A844}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}

DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; qimgv is 64-bit only.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

OutputDir=..\build
OutputBaseFilename=qimgv-{#AppVersion}-win64-setup
SetupIconFile=..\qimgv\res\icons\common\logo\app\qimgv.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName} {#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything the packaging script assembled, minus the portable-mode folders.
; Shipping a "conf" directory would be actively harmful: qimgv treats one next
; to the executable as the signal to run portable, so an installed copy would
; write its settings into Program Files / LOCALAPPDATA\Programs instead of the
; per-user config location. See portableMode() in qimgv/settings.cpp.
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; \
    Excludes: "conf,cache,thumbnails"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Deliberately does NOT take over any file association. This only advertises
; qimgv in the "Open with" list, leaving whatever the user already chose as the
; default alone. Windows' own Default Apps UI is the right place to change that.
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}"; \
    ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\shell\open\command"; \
    ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".jpg";  ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".jpeg"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".png";  ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".gif";  ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".webp"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".avif"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".heic"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".jxl";  ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".bmp";  ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".tif";  ValueData: ""
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\SupportedTypes"; \
    ValueType: string; ValueName: ".tiff"; ValueData: ""

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The cache and thumbnails qimgv generates at runtime, so uninstalling does not
; strand hundreds of megabytes. Settings under %LOCALAPPDATA%\qimgv are left
; alone on purpose, so reinstalling keeps your configuration.
Type: filesandordirs; Name: "{localappdata}\cache\qimgv"
