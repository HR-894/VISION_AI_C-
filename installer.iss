; ═══════════════════════════════════════════════════════════════════
; VISION AI - InnoSetup Installer Script
; ═══════════════════════════════════════════════════════════════════
;
; Key Design Decisions:
;   ✅ Installs to AppData\Local (no UAC prompt, silent updates)
;   ✅ Binaries only (~30-40MB) — models download on first run
;   ✅ Start Menu + Desktop shortcuts
;   ✅ Proper uninstaller
;   ✅ Visual Studio C++ Runtime bundled
;   ✅ Single-user install (no admin required)
;
; Build: ISCC.exe installer.iss
; ═══════════════════════════════════════════════════════════════════

#define MyAppName "VISION AI"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "HR-894"
#define MyAppURL "https://github.com/HR-894/VISION_AI_C-"
#define MyAppExeName "VISION_AI.exe"
#define MyAppIcon "assets\icon.png"

; Source directory (populated by deploy_release.ps1)
#define SourceDir "build\bin\Release"

[Setup]
; App identity
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases

; ─── THE MAGIC DIRECTIVES ───
; No admin prompt — install to user's AppData\Local
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Vision_AI

; Start Menu
DefaultGroupName={#MyAppName}
AllowNoIcons=yes

; Output
OutputDir=dist
OutputBaseFilename=VisionAI_Setup_v{#MyAppVersion}
SetupIconFile={#MyAppIcon}

; Compression (LZMA2 ultra = smallest installer)
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes

; Visual
WizardStyle=modern
WizardSizePercent=120
DisableWelcomePage=no

; Uninstaller
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

; Architecture
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Misc
DisableProgramGroupPage=yes
DisableDirPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; ─── BINARIES ONLY ───
; Main executable
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Qt6 DLLs (deployed by windeployqt)
Source: "{#SourceDir}\Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Gui.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Widgets.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Network.dll"; DestDir: "{app}"; Flags: ignoreversion

; Qt plugins (windeployqt output)
Source: "{#SourceDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs; Check: DirExists(ExpandConstant('{#SourceDir}\tls'))

; Third-party DLLs (curl, llama, spdlog etc.)
Source: "{#SourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; Visual C++ Runtime (if bundled by windeployqt --compiler-runtime)
Source: "{#SourceDir}\vc_redist*.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

; Data directory (empty — models download on first run)
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

; Tesseract trained data (small, ~4MB)
Source: "{#SourceDir}\tessdata\*"; DestDir: "{app}\tessdata"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

; ─── EXPLICITLY EXCLUDED (models, GGUF, whisper) ───
; These are NOT included — ModelDownloaderWizard handles them on first run

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Install VC++ Runtime if present (silent, no reboot)
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Visual C++ Runtime..."; Flags: waituntilterminated skipifdoesntexist

; Launch app after install
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Clean up user data on uninstall (optional — keep models)
Type: filesandirs; Name: "{app}\data"
Type: filesandirs; Name: "{app}\logs"
