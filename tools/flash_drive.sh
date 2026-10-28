#!/bin/sh
# Build the USB drive firmware (firmware/carddrive, TinyUSB mass storage) and write ONLY its app image into the spare
# OTA slot: ota_1 at 0x340000 of the 8 MB `default_8MB` table. The player at 0x10000 (ota_0), the bootloader, the
# partition table and otadata are not touched, so the device still boots the player; the player jumps to the drive with
# SHIFT + gen pattern held 1 s or the serial command `drive` (SPEC §9.22), and the drive firmware returns to the player
# on ENTER, on its serial command `player`, or on any reset (it re-points the boot slot at ota_0 as soon as it starts).
#
# Usage: tools/flash_drive.sh [port]      (default port: first /dev/cu.usbmodem*; NO_UPLOAD=1 compiles only)
# Run it while the PLAYER is running (its hardware CDC/JTAG port: esptool resets into the ROM loader through it). When
# the drive firmware is running, its TinyUSB CDC port has a different name and needs the 1200 bps-touch path: send
# `player` first (tools/serial_cmd.py PORT player) or press ENTER on the device, then flash.
set -e
cd "$(dirname "$0")/.."
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
FQBN="esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled,USBMode=default"   # USBMode=default = USB-OTG (TinyUSB)
BUILD=/tmp/lp-drive-build
SLOT=0x340000           # ota_1 (app1) in default_8MB.csv
SLOT_SIZE=3342336       # 0x330000
ESPTOOL=$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | sort -V | tail -1)   # the esptool the arduino core ships (v5: hyphenated commands)

arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" firmware/carddrive 2>&1 | grep -E "error|Sketch uses|Global variables|Error" || true
[ -f "$BUILD/carddrive.ino.bin" ] || { echo "build failed"; exit 1; }
SIZE=$(stat -f %z "$BUILD/carddrive.ino.bin" 2>/dev/null || stat -c %s "$BUILD/carddrive.ino.bin")
echo "carddrive.ino.bin: $SIZE bytes (slot ota_1 holds $SLOT_SIZE)"
[ "$SIZE" -le "$SLOT_SIZE" ] || { echo "the image does not fit the OTA slot"; exit 1; }
[ "${NO_UPLOAD:-0}" = 1 ] && exit 0
[ -n "$PORT" ] || { echo "no device port"; exit 1; }
[ -x "$ESPTOOL" ] || { echo "esptool not found under ~/Library/Arduino15/packages/esp32/tools/esptool_py"; exit 1; }

# Only the app image, only at the slot: the player, bootloader, partition table and otadata stay as they are.
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 --before default-reset --after hard-reset \
  write-flash --flash-mode keep --flash-freq keep --flash-size keep "$SLOT" "$BUILD/carddrive.ino.bin"
cat <<EOF
Drive firmware written to ota_1 ($SLOT). The device has restarted into the PLAYER (otadata still names ota_0).
  Enter the drive:   SHIFT + gen pattern held 1 s on the device, or   tools/serial_cmd.py $PORT drive
  Return:            eject the volume on the computer, press ENTER on the device (or send \`player\` on its new
                     serial port, or press reset) — the drive firmware re-points the boot slot at ota_0 at its own start.
  Without a USB drive on the computer: tools/card_pull.py $PORT /lp/packs/House/presets ./pulled
EOF
