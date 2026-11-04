#!/usr/bin/env bash
#
# Build the Portal Evil HUEBR firmware (.bin) for the M5StickC Plus.
#
# Usage:  ./scripts/build.sh
# Output: firmware/portal-evil-huebr-m5stickcplus-merged.bin  (flash at 0x0 in M5Burner)
#
# Requires arduino-cli on PATH:  https://arduino.github.io/arduino-cli/latest/installation/
# The ESP32 core and the M5StickCPlus library are installed automatically if missing.

set -euo pipefail

FQBN="esp32:esp32:m5stack_stickc_plus"
SKETCH_DIR="portal-evil-huebr"
SKETCH="$SKETCH_DIR/portal-evil-huebr.ino"
ESP32_URL="https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"
OUT_DIR="firmware"

# Run from the repo root (this script lives in scripts/).
cd "$(dirname "$0")/.."

echo "==> Checking arduino-cli..."
if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "ERROR: arduino-cli not found on PATH."
  echo "Install it: https://arduino.github.io/arduino-cli/latest/installation/"
  echo "  macOS/Linux: curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh"
  echo "  Windows:     use scripts/build.ps1 or download the binary from the page above"
  exit 1
fi

echo "==> Installing ESP32 core + M5StickCPlus library (idempotent)..."
arduino-cli core update-index --additional-urls "$ESP32_URL"
arduino-cli core install esp32:esp32 --additional-urls "$ESP32_URL"
arduino-cli lib install M5StickCPlus

echo "==> Compiling $SKETCH ..."
arduino-cli compile --fqbn "$FQBN" --warnings all --export-binaries "$SKETCH"

BUILD_DIR="$SKETCH_DIR/build/esp32.esp32.m5stack_stickc_plus"
mkdir -p "$OUT_DIR"
cp "$BUILD_DIR/portal-evil-huebr.ino.merged.bin" "$OUT_DIR/portal-evil-huebr-m5stickcplus-merged.bin"
cp "$BUILD_DIR/portal-evil-huebr.ino.bin"        "$OUT_DIR/portal-evil-huebr-m5stickcplus-app.bin"

echo ""
echo "==> Done! Flash this file in M5Burner (write at 0x0):"
echo "    $OUT_DIR/portal-evil-huebr-m5stickcplus-merged.bin"
echo "    (or the app-only image for offset 0x10000: $OUT_DIR/portal-evil-huebr-m5stickcplus-app.bin)"
