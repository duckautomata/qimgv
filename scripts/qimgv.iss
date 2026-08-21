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
ChangesAssociations=yes
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
; Two things happen here, and only one of them is about defaults.
;
; 1. ProgIDs + the Applications key: qimgv shows up in "Open with" with a
;    real name and icon. This changes no existing association.
; 2. Capabilities + RegisteredApplications: this is what makes qimgv
;    appear in Settings > Default apps as an app you can assign types to.
;
; The installer deliberately does not set any default itself. Since Windows 8
; that is not possible for an application to do -- the real association lives
; in a hash-protected UserChoice key, and programs that forge it get reset by
; Windows and flagged by AV. Declaring capabilities is the supported path: it
; makes qimgv assignable, and leaves the choice with the user.

; --- ProgIDs the associations point at -------------------------------------
Root: HKCU; Subkey: "Software\Classes\qimgv.AssocFile.Image"; ValueType: string; ValueName: ""; ValueData: "Image"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\qimgv.AssocFile.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExeName},0"
Root: HKCU; Subkey: "Software\Classes\qimgv.AssocFile.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\qimgv.AssocFile.Video"; ValueType: string; ValueName: ""; ValueData: "Video"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\qimgv.AssocFile.Video\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExeName},0"
Root: HKCU; Subkey: "Software\Classes\qimgv.AssocFile.Video\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""

; --- Advertise in "Open with" without touching any default ------------------
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Applications\{#AppExeName}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExeName}"" ""%1"""

; --- Capabilities: what qimgv offers to handle -----------------------------
Root: HKCU; Subkey: "Software\qimgv"; Flags: uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\qimgv\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#AppName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\qimgv\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Fast, configurable image viewer with optional video support."
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avif"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avifs"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".bmp"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cur"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dds"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".exr"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".gif"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".hdr"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heic"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heif"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ico"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".j2k"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jfif"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jp2"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpe"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jxl"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mng"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pbm"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pcx"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pgm"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ppm"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".psd"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".qoi"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ras"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".sgi"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".svg"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".svgz"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tga"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wbmp"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xbm"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xcf"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xpm"; ValueData: "qimgv.AssocFile.Image"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".3gp"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m2ts"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4v"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mkv"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mov"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp4"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpeg"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpg"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ts"; ValueData: "qimgv.AssocFile.Video"
Root: HKCU; Subkey: "Software\qimgv\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webm"; ValueData: "qimgv.AssocFile.Video"

; --- Tell Windows where those capabilities live ----------------------------
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "{#AppName}"; ValueData: "Software\qimgv\Capabilities"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The cache and thumbnails qimgv generates at runtime, so uninstalling does not
; strand hundreds of megabytes. Settings under %LOCALAPPDATA%\qimgv are left
; alone on purpose, so reinstalling keeps your configuration.
Type: filesandordirs; Name: "{localappdata}\cache\qimgv"
