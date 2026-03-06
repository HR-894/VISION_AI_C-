# deploy_release.ps1
# Automates building, deploying Qt dependencies, and packaging VISION AI for Windows.

$ErrorActionPreference = "Stop"

$ProjectRoot = Get-Location
$BuildDir = Join-Path $ProjectRoot "build"
$OutputDir = Join-Path $BuildDir "bin\Release"
$ZipName = "VisionAI_v1.0_Windows.zip"
$ZipPath = Join-Path $ProjectRoot $ZipName

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "   VISION AI - Windows Release Builder" -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# Step 1: Build the Project
Write-Host "`n[1/4] Configuring and Building Project (Release)..." -ForegroundColor Yellow

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Set-Location $BuildDir
# Configure CMake
cmake -DCMAKE_BUILD_TYPE=Release -G "Visual Studio 17 2022" -A x64 ..
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed!" }

# Build Project
cmake --build . --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed!" }

Set-Location $ProjectRoot

# Step 2: Qt Deployment
Write-Host "`n[2/4] Deploying Qt Dependencies..." -ForegroundColor Yellow
$ExePath = Join-Path $OutputDir "VISION_AI.exe"

if (-not (Test-Path $ExePath)) {
    throw "VISION_AI.exe not found at $ExePath. Build might have failed."
}

# Run windeployqt
# Note: Ensure windeployqt is in your PATH (e.g., C:\Qt\6.x.x\msvcxxxx_64\bin)
Write-Host "Running windeployqt on $ExePath..."
windeployqt --release --no-translations --no-opengl-sw --compiler-runtime $ExePath
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed! Please ensure it is in your system PATH." }

# Step 3: Prepare Data Folder
Write-Host "`n[3/4] Preparing Data Directory..." -ForegroundColor Yellow
$DataDir = Join-Path $OutputDir "data"
if (-not (Test-Path $DataDir)) {
    New-Item -ItemType Directory -Path $DataDir | Out-Null
    Write-Host "Created missing data directory."
} else {
    Write-Host "Data directory already exists."
}

# Step 4: Zip it up
Write-Host "`n[4/4] Creating Release Archive..." -ForegroundColor Yellow
if (Test-Path $ZipPath) {
    Remove-Item -Path $ZipPath -Force
    Write-Host "Removed previous archive."
}

Write-Host "Compressing $OutputDir into $ZipName..."
# We compress the contents of the Release folder
Compress-Archive -Path "$OutputDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host "`n=========================================" -ForegroundColor Green
Write-Host "   SUCCESS! Release package created at:" -ForegroundColor Green
Write-Host "   $ZipPath" -ForegroundColor Green
Write-Host "=========================================" -ForegroundColor Green
