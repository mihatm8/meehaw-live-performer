#!/bin/sh
# Compile the firmware for the Cardputer ADV, upload it, then capture 20 s of serial output.
# Usage: tools/flash.sh [port]   (default port: first /dev/cu.usbmodem*)
set -e
cd "$(dirname "$0")/.."
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
FQBN="esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled"
BUILD=/tmp/lp-build
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" firmware/liveplayer 2>&1 | grep -E "error|Sketch uses|Global variables|Error" || true
[ -f "$BUILD/liveplayer.ino.bin" ] || { echo "build failed"; exit 1; }
[ -n "$PORT" ] || { echo "no device port"; exit 1; }
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD" firmware/liveplayer 2>&1 | tail -3
sleep 1
./tools/.venv/bin/python -I tools/serial_capture.py "$PORT" "${CAPTURE_SECONDS:-20}" --no-reset
