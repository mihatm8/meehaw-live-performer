#!/bin/sh
# Wavetable CPU bench on the Cardputer ADV (SPEC §9.15): builds firmware/wtbench against the CURRENT firmware
# sources (src/{core,dsp,engine} are copied next to the sketch, never checked in), uploads it and captures the
# serial output. Usage: tools/bench_wavetables.sh [port]   (default port: first /dev/cu.usbmodem*)
# Pass BUILD_ONLY=1 to compile without a device (prints the table/flash sizes from the ELF).
set -e
cd "$(dirname "$0")/.."
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
FQBN="esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled"
BUILD=/tmp/lp-wtbench-build
SKETCH=firmware/wtbench
rm -rf "$SKETCH/src" && mkdir -p "$SKETCH/src"
cp -R firmware/liveplayer/src/core firmware/liveplayer/src/dsp firmware/liveplayer/src/engine "$SKETCH/src/"
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" "$SKETCH" 2>&1 | grep -E "error|Sketch uses|Global variables|Error|warning: " || true
[ -f "$BUILD/wtbench.ino.elf" ] || { echo "build failed"; exit 1; }
NM=$(ls ~/Library/Arduino15/packages/esp32/tools/esp-x32/*/bin/xtensa-esp32s3-elf-nm 2>/dev/null | head -1)
if [ -n "$NM" ]; then
  echo "wavetable symbols in flash (.rodata):"
  total=0
  "$NM" -S --size-sort "$BUILD/wtbench.ino.elf" | grep -i "kWave\(Sine\|Tri\|Saw\|Square\|Fat\)[0-9]" | while read -r addr size kind name; do
    printf "  %-16s %6d B\n" "$name" "$((0x$size))"
  done
  "$NM" -S --size-sort "$BUILD/wtbench.ino.elf" | grep -i "kWave\(Sine\|Tri\|Saw\|Square\|Fat\)[0-9]" | awk '{ print $2 }' | while read -r size; do total=$((total + 0x$size)); echo "$total"; done | tail -1 | sed 's/^/  total /; s/$/ B/'
fi
[ -n "$BUILD_ONLY" ] && exit 0
[ -n "$PORT" ] || { echo "no device port"; exit 1; }
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILD" "$SKETCH" 2>&1 | tail -3
sleep 1
./tools/.venv/bin/python -I tools/serial_capture.py "$PORT" "${CAPTURE_SECONDS:-90}" --no-reset
