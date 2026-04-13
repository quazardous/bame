# One-shot Windows dev-env setup for BaMe.
# Checks (and installs via winget if missing) the tools the project expects:
#   - Python 3.10+ (sim scripts, screenshots)
#   - Pillow (screenshots)
#   - gcc (compiles src/bame_core.c to sim/bame_core.dll so Python sim can
#     call the same code that runs on the AVR)
#
# PlatformIO itself isn't handled here — install via `pip install platformio`
# or use the PlatformIO IDE extension.
#
# Usage (from the repo root, PowerShell):
#     .\setup.ps1
# If winget prompts for consent or privilege, accept to install.

$ErrorActionPreference = "Stop"

function Have($cmd) {
    $null = Get-Command $cmd -ErrorAction SilentlyContinue
    $?
}

function WingetInstall($id, $label) {
    Write-Host "Installing $label ..." -ForegroundColor Cyan
    winget install --id $id --accept-source-agreements --accept-package-agreements -e
}

# --- Python ---
if (-not (Have python)) {
    WingetInstall "Python.Python.3.12" "Python 3.12"
    Write-Host "Python installed — reopen this shell for PATH to update, then rerun setup.ps1." -ForegroundColor Yellow
    exit 0
} else {
    $v = (python --version) 2>&1
    Write-Host "Python OK: $v" -ForegroundColor Green
}

# --- Pillow (for screenshots) ---
$pil = (python -c "import PIL; print(PIL.__version__)" 2>$null)
if (-not $pil) {
    Write-Host "Installing Pillow ..." -ForegroundColor Cyan
    python -m pip install --user Pillow
} else {
    Write-Host "Pillow OK: $pil" -ForegroundColor Green
}

# --- gcc (mingw-w64) for core-lib ---
if (-not (Have gcc)) {
    WingetInstall "MSYS2.MSYS2" "MSYS2 (includes gcc via pacman)"
    Write-Host ""
    Write-Host "MSYS2 is installed. To finish setting up gcc:"        -ForegroundColor Yellow
    Write-Host "  1. Open MSYS2 MINGW64 shell (Start menu)"           -ForegroundColor Yellow
    Write-Host "  2. Run: pacman -S --noconfirm mingw-w64-x86_64-gcc" -ForegroundColor Yellow
    Write-Host "  3. Add C:\msys64\mingw64\bin to PATH"               -ForegroundColor Yellow
    Write-Host "  4. Reopen your shell and rerun setup.ps1 to verify" -ForegroundColor Yellow
} else {
    $g = (gcc --version | Select-Object -First 1)
    Write-Host "gcc OK: $g" -ForegroundColor Green
}

Write-Host ""
Write-Host "Setup complete. Next:" -ForegroundColor Green
Write-Host "  make core-lib       # compiles sim/bame_core.dll"
Write-Host "  make build          # compiles firmware for the default env"
Write-Host "  cp Makefile.local.example Makefile.local  # pick your install"
