# ═══════════════════════════════════════════════════════════════════
# deploy_release.ps1
# Full Release Pipeline: Build → Qt Deploy → InnoSetup → Installer
# ═══════════════════════════════════════════════════════════════════
#
# Usage:
#   .\deploy_release.ps1
#   .\deploy_release.ps1 -SkipBuild     # Skip CMake (use existing build)
#   .\deploy_release.ps1 -ZipOnly       # Create zip instead of installer
#
# Requirements:
#   - Visual Studio 2022 with C++ workload
#   - Qt6 (windeployqt in PATH)
#   - InnoSetup 6 (ISCC.exe in PATH or default location)
# ═══════════════════════════════════════════════════════════════════

param(
    [switch]$SkipBuild,
    [switch]$ZipOnly
)

$ErrorActionPreference = "Stop"

$ProjectRoot   = $PSScriptRoot
$BuildDir      = Join-Path $ProjectRoot "build"
$OutputDir     = Join-Path $BuildDir "bin\Release"
$DistDir       = Join-Path $ProjectRoot "dist"
$Version       = "1.0.0"
$ExeName       = "VISION_AI.exe"

Write-Host ""
Write-Host "╔═══════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║   VISION AI — Windows Release Builder v2.0   ║" -ForegroundColor Cyan
Write-Host "║   AppData\Local Install • No Admin Required   ║" -ForegroundColor Cyan
Write-Host "╚═══════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# ═══════════════════ Step 1: Build ════════════════════════════════

if (-not $SkipBuild) {
    Write-Host "[1/6] Building VISION AI (Release x64)..." -ForegroundColor Yellow

    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir | Out-Null
    }

    Push-Location $BuildDir
    try {
        cmake -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64 ..
        if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed!" }

        cmake --build . --config Release --parallel
        if ($LASTEXITCODE -ne 0) { throw "CMake build failed!" }
    } finally {
        Pop-Location
    }

    Write-Host "  ✅ Build complete." -ForegroundColor Green
} else {
    Write-Host "[1/6] Skipping build (--SkipBuild)." -ForegroundColor DarkGray
}

# ═══════════════════ Step 2: Verify Binary ════════════════════════

Write-Host "[2/6] Verifying binary..." -ForegroundColor Yellow
$ExePath = Join-Path $OutputDir $ExeName

if (-not (Test-Path $ExePath)) {
    throw "$ExeName not found at $ExePath. Build may have failed."
}

$FileSize = (Get-Item $ExePath).Length / 1MB
Write-Host "  ✅ Found: $ExeName ($([math]::Round($FileSize, 1)) MB)" -ForegroundColor Green

# ═══════════════════ Step 3: Qt Deployment ════════════════════════

Write-Host "[3/6] Deploying Qt6 dependencies (windeployqt)..." -ForegroundColor Yellow

$windeployqt = Get-Command "windeployqt" -ErrorAction SilentlyContinue
if (-not $windeployqt) {
    # Try default Qt6 paths
    $QtPaths = @(
        "C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe",
        "C:\Qt\6.7.0\msvc2022_64\bin\windeployqt.exe",
        "$env:QTDIR\bin\windeployqt.exe"
    )
    foreach ($p in $QtPaths) {
        if (Test-Path $p) {
            $windeployqt = $p
            break
        }
    }
    if (-not $windeployqt) {
        throw "windeployqt not found! Add Qt6 bin to PATH or set QTDIR."
    }
}

& $windeployqt --release --no-translations --no-opengl-sw --compiler-runtime $ExePath
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed!" }

Write-Host "  ✅ Qt dependencies deployed." -ForegroundColor Green

# ═══════════════════ Step 4: Prepare Data ═════════════════════════

Write-Host "[4/6] Preparing data directory..." -ForegroundColor Yellow

# Create empty data dir (models download on first run)
$DataDir = Join-Path $OutputDir "data"
if (-not (Test-Path $DataDir)) {
    New-Item -ItemType Directory -Path $DataDir | Out-Null
    Write-Host "  Created data/ (models download on first run)."
}

# Copy tessdata if available
$TessdataSrc = Join-Path $ProjectRoot "tessdata"
$TessdataDst = Join-Path $OutputDir "tessdata"
if (Test-Path $TessdataSrc) {
    if (-not (Test-Path $TessdataDst)) {
        New-Item -ItemType Directory -Path $TessdataDst | Out-Null
    }
    Copy-Item "$TessdataSrc\*" $TessdataDst -Recurse -Force
    Write-Host "  ✅ Tessdata copied." -ForegroundColor Green
} else {
    Write-Host "  ⚠️ No tessdata/ found (OCR will need manual setup)." -ForegroundColor DarkYellow
}

Write-Host "  ✅ Data prepared (no models bundled — wizard handles it)." -ForegroundColor Green

# ═══════════════════ Step 5: Count Files & Size ═══════════════════

Write-Host "[5/6] Calculating package size..." -ForegroundColor Yellow

$AllFiles = Get-ChildItem $OutputDir -Recurse -File
$TotalSize = ($AllFiles | Measure-Object -Property Length -Sum).Sum / 1MB
$FileCount = $AllFiles.Count

# Verify NO large model files snuck in
$LargeFiles = $AllFiles | Where-Object { $_.Length -gt 100MB }
if ($LargeFiles) {
    Write-Host "  ⚠️ WARNING: Large files detected (>100MB):" -ForegroundColor Red
    foreach ($f in $LargeFiles) {
        $sizeMB = [math]::Round($f.Length / 1MB, 1)
        Write-Host "    - $($f.Name) ($sizeMB MB)" -ForegroundColor Red
    }
    Write-Host "  These should NOT be in the installer!" -ForegroundColor Red
    $confirm = Read-Host "Continue anyway? (y/N)"
    if ($confirm -ne "y") { throw "Aborted — remove large files first." }
}

Write-Host "  ✅ Package: $FileCount files, $([math]::Round($TotalSize, 1)) MB total." -ForegroundColor Green

# ═══════════════════ Step 6: Package ══════════════════════════════

if ($ZipOnly) {
    # Zip fallback
    Write-Host "[6/6] Creating ZIP archive..." -ForegroundColor Yellow
    $ZipPath = Join-Path $ProjectRoot "VisionAI_v${Version}_Windows.zip"
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    Compress-Archive -Path "$OutputDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal
    Write-Host "  ✅ ZIP: $ZipPath" -ForegroundColor Green
} else {
    # InnoSetup installer
    Write-Host "[6/6] Building InnoSetup installer..." -ForegroundColor Yellow

    if (-not (Test-Path $DistDir)) {
        New-Item -ItemType Directory -Path $DistDir | Out-Null
    }

    # Find ISCC.exe
    $ISCC = Get-Command "ISCC" -ErrorAction SilentlyContinue
    if (-not $ISCC) {
        $ISCCPaths = @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
            "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
        )
        foreach ($p in $ISCCPaths) {
            if (Test-Path $p) {
                $ISCC = $p
                break
            }
        }
        if (-not $ISCC) {
            Write-Host "  ⚠️ ISCC.exe not found. Install Inno Setup 6 from:" -ForegroundColor Red
            Write-Host "     https://jrsoftware.org/isdl.php" -ForegroundColor Red
            Write-Host "  Falling back to ZIP..." -ForegroundColor Yellow
            $ZipPath = Join-Path $ProjectRoot "VisionAI_v${Version}_Windows.zip"
            if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
            Compress-Archive -Path "$OutputDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal
            Write-Host "  ✅ ZIP: $ZipPath" -ForegroundColor Green
            return
        }
    }

    $ISSFile = Join-Path $ProjectRoot "installer.iss"
    if (-not (Test-Path $ISSFile)) {
        throw "installer.iss not found at $ISSFile"
    }

    Write-Host "  Running: ISCC.exe installer.iss"
    & $ISCC $ISSFile
    if ($LASTEXITCODE -ne 0) { throw "InnoSetup compilation failed!" }

    $InstallerPath = Join-Path $DistDir "VisionAI_Setup_v${Version}.exe"
    if (Test-Path $InstallerPath) {
        $InstallerSize = [math]::Round((Get-Item $InstallerPath).Length / 1MB, 1)
        Write-Host "  ✅ Installer: $InstallerPath ($InstallerSize MB)" -ForegroundColor Green
    }
}

# ═══════════════════ Done ═════════════════════════════════════════

Write-Host ""
Write-Host "╔═══════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "║          ✅ RELEASE BUILD COMPLETE!           ║" -ForegroundColor Green
Write-Host "║                                               ║" -ForegroundColor Green
Write-Host "║  Ready to upload to GitHub Releases!          ║" -ForegroundColor Green
Write-Host "║  Install path: AppData\Local\Vision_AI        ║" -ForegroundColor Green
Write-Host "║  Admin required: NO (PrivilegesRequired=lowest)║" -ForegroundColor Green
Write-Host "╚═══════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
