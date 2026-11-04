<#
  Build the Portal Evil HUEBR firmware (.bin) for the M5StickC Plus (Windows).

  Usage:  powershell -ExecutionPolicy Bypass -File scripts\build.ps1
  Output: firmware\portal-evil-huebr-m5stickcplus-merged.bin  (flash at 0x0 in M5Burner)

  Requires arduino-cli on PATH: https://arduino.github.io/arduino-cli/latest/installation/
  The ESP32 core and the M5StickCPlus library are installed automatically if missing.
#>

$ErrorActionPreference = "Stop"

$Fqbn      = "esp32:esp32:m5stack_stickc_plus"
$SketchDir = "portal-evil-huebr"
$Sketch    = "$SketchDir/portal-evil-huebr.ino"
$Esp32Url  = "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
$OutDir    = "firmware"

# Run from the repo root (this script lives in scripts\).
Set-Location (Join-Path $PSScriptRoot "..")

Write-Host "==> Checking arduino-cli..."
if (-not (Get-Command arduino-cli -ErrorAction SilentlyContinue)) {
  Write-Host "ERROR: arduino-cli not found on PATH." -ForegroundColor Red
  Write-Host "Install it: https://arduino.github.io/arduino-cli/latest/installation/"
  exit 1
}

Write-Host "==> Installing ESP32 core + M5StickCPlus library (idempotent)..."
arduino-cli core update-index --additional-urls $Esp32Url
arduino-cli core install esp32:esp32 --additional-urls $Esp32Url
arduino-cli lib install M5StickCPlus

Write-Host "==> Compiling $Sketch ..."
arduino-cli compile --fqbn $Fqbn --warnings all --export-binaries $Sketch

$BuildDir = "$SketchDir/build/esp32.esp32.m5stack_stickc_plus"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item "$BuildDir/portal-evil-huebr.ino.merged.bin" "$OutDir/portal-evil-huebr-m5stickcplus-merged.bin" -Force
Copy-Item "$BuildDir/portal-evil-huebr.ino.bin"        "$OutDir/portal-evil-huebr-m5stickcplus-app.bin"    -Force

Write-Host ""
Write-Host "==> Done! Flash this file in M5Burner (write at 0x0):" -ForegroundColor Green
Write-Host "    $OutDir\portal-evil-huebr-m5stickcplus-merged.bin"
Write-Host "    (or the app-only image for offset 0x10000: $OutDir\portal-evil-huebr-m5stickcplus-app.bin)"
