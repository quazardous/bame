# One-shot Windows dev-env setup for BaMe.
# Installs what the project expects: Python, Pillow, gcc (via MSYS2).
#
# Usage (PowerShell, repo root):
#     powershell -ExecutionPolicy Bypass -File .\setup.ps1

$ErrorActionPreference = "Stop"

function Test-Cmd {
    param([string]$name)
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

function Install-Winget {
    param([string]$id, [string]$label)
    Write-Host "Installing $label ..." -ForegroundColor Cyan
    winget install --id $id --accept-source-agreements --accept-package-agreements -e
}

# --- Python ---
if (-not (Test-Cmd "python")) {
    Install-Winget "Python.Python.3.12" "Python 3.12"
    Write-Host "Python installed. Reopen this shell so PATH picks it up, then rerun setup.ps1." -ForegroundColor Yellow
    exit 0
} else {
    $pyver = (python --version) 2>&1
    Write-Host "Python OK: $pyver" -ForegroundColor Green
}

# --- Pillow ---
$pilver = (python -c "import PIL; print(PIL.__version__)" 2>$null)
if (-not $pilver) {
    Write-Host "Installing Pillow ..." -ForegroundColor Cyan
    python -m pip install --user Pillow
} else {
    Write-Host "Pillow OK: $pilver" -ForegroundColor Green
}

# --- gcc (MSYS2 + mingw-w64) ---
$mingwBin = "C:\msys64\mingw64\bin"
$gccExe   = Join-Path $mingwBin "gcc.exe"
$pacman   = "C:\msys64\usr\bin\pacman.exe"

# Install MSYS2 if missing
if (-not (Test-Path $pacman)) {
    Install-Winget "MSYS2.MSYS2" "MSYS2"
}

# Install mingw-w64 gcc via pacman if missing
if (-not (Test-Path $gccExe)) {
    Write-Host "Installing mingw-w64-x86_64-gcc via pacman ..." -ForegroundColor Cyan
    & $pacman -Sy --noconfirm mingw-w64-x86_64-gcc
}

# Add to user PATH if not there
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$mingwBin*") {
    Write-Host "Adding $mingwBin to user PATH ..." -ForegroundColor Cyan
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$mingwBin", "User")
    $env:Path = "$env:Path;$mingwBin"   # so it's usable in this session too
}

# Verify
if (Test-Path $gccExe) {
    $gccver = (& $gccExe --version | Select-Object -First 1)
    Write-Host "gcc OK: $gccver" -ForegroundColor Green
} else {
    Write-Host "gcc install failed - check MSYS2 install manually" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Setup complete." -ForegroundColor Green
Write-Host "Reopen your shell so the new PATH is active everywhere, then:" -ForegroundColor Yellow
Write-Host "  make core-lib       # compile sim/bame_core.dll"
Write-Host "  make build          # compile firmware for default env"
Write-Host "  copy Makefile.local.example Makefile.local  # pick your env"
