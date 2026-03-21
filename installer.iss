; ═══════════════════════════════════════════════════════════════════
; VISION AI - InnoSetup Installer Script (Phase 8)
; ═══════════════════════════════════════════════════════════════════
;
; Key Design Decisions:
;   ✅ Installs to AppData\Local (no UAC prompt, silent updates)
;   ✅ Binaries only (~30-40MB) — models download on first run
;   ✅ Start Menu + Desktop shortcuts
;   ✅ Proper uninstaller with selective cleanup
;   ✅ Visual Studio C++ Runtime bundled
;   ✅ Single-user install (no admin required)
;   ✅ Persistent data/ directory for user memory & vector store
;
; Build: ISCC.exe installer.iss
; ═══════════════════════════════════════════════════════════════════

#define MyAppName "VISION AI"
#define MyAppVersion "3.0.0"
#define MyAppPublisher "HR-894"
#define MyAppURL "https://github.com/HR-894/VISION_AI_C-"
#define MyAppExeName "VISION_AI.exe"
#define MyAppIcon "assets\icon.png"

; Source directory (populated by CMake build + windeployqt)
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
; No admin prompt — install to user's AppData\Local\Programs
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\VisionAI

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
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; ─── MAIN EXECUTABLE ───
Source: "{#SourceDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; ─── Qt6 DLLs (deployed by windeployqt) ───
Source: "{#SourceDir}\Qt6Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Gui.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Widgets.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Network.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\Qt6Concurrent.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; ─── Qt plugins (windeployqt output) ───
Source: "{#SourceDir}\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#SourceDir}\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#SourceDir}\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#SourceDir}\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

; ─── Third-party DLLs (llama, whisper, portaudio, curl, spdlog) ───
Source: "{#SourceDir}\llama.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\ggml.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\ggml-base.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\ggml-cpu.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\ggml-cuda.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\ggml-vulkan.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\whisper.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\portaudio*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\libcurl*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\spdlog*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; OpenCV DLLs
Source: "{#SourceDir}\opencv_core*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\opencv_imgproc*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\opencv_highgui*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Tesseract DLLs
Source: "{#SourceDir}\tesseract*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\leptonica*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Catch-all for any remaining DLLs (zlib, ssl, etc.)
Source: "{#SourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; ─── Visual C++ Runtime ───
Source: "{#SourceDir}\vc_redist*.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

; ─── Persistent data directory (empty — user memory created at runtime) ───
Source: "{#SourceDir}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist onlyifdoesntexist

; ─── Tesseract trained data ───
Source: "{#SourceDir}\tessdata\*"; DestDir: "{app}\tessdata"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

; ─── EXPLICITLY EXCLUDED (models, GGUF, whisper) ───
; These are NOT included — ModelDownloaderWizard handles them on first run

[Dirs]
; Create data directory on install (persists across updates)
Name: "{app}\data"; Flags: uninsneveruninstall
Name: "{app}\data\memory"; Flags: uninsneveruninstall

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
; Clean up logs and cache on uninstall (preserve user data/memory)
Type: filesandirs; Name: "{app}\logs"
Type: filesandirs; Name: "{app}\cache"
; NOTE: {app}\data is NOT deleted (uninsneveruninstall flag) to preserve user's vector memory
