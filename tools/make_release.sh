#!/bin/sh
# Build a shareable release of the meehaw live performer firmware (docs/FLASHING.md explains how friends flash it).
#
# Compiles the three sketches into separate build dirs under dist/build-<ver>/ and packs dist/release-<ver>/ with:
#   meehaw-player-<ver>-full.bin      ONE image for address 0x0 (esptool merge-bin --chip esp32s3): bootloader @0x0, partition
#                                     table @0x8000, otadata = boot_app0 @0xe000, the PLAYER @0x10000 (ota_0) and the USB DRIVE
#                                     firmware @0x340000 (ota_1). Flash it once and the device has the player AND drive mode.
#   meehaw-player-<ver>-app.bin       the player alone, for updates (write at 0x10000)
#   meehaw-carddrive-<ver>-app.bin    the drive firmware alone (write at 0x340000)
#   meehaw-cardloader-<ver>-app.bin   the serial card loader (write at 0x10000): it REPLACES the player until the player
#                                     app is flashed again (tools/card_sync.py fills the card through it, docs/CARD_IMPORT.md B)
#   SHA256SUMS                        sha256 of every other file in the folder (`shasum -a 256 -c SHA256SUMS`)
#   index.html + manifest.json        the web flasher (tools/webflash; ESP Web Tools): serve the folder over https or
#                                     localhost (`python3 -m http.server` inside it) and flash from Chrome / Edge
#   FLASHING.md, GETTING_STARTED.md, CARD_IMPORT.md, MANUAL.md, KEYMAP.md (each when docs/ has it), the sticker PDFs
#   (sticker-keys-*.pdf), README.txt, LICENSE and CREDITS.md (required: the MIT licence and the third-party notices of what
#   the binaries link — M5Unified / M5GFX / TinyUSB MIT, arduino-esp32 LGPL-2.1, ESP-IDF Apache-2.0, FatFs — must travel
#   with every copy of them), and a free card bundle (meehaw-card-free*.zip + its .manifest.txt) when one is found or given.
#
# Usage: sh tools/make_release.sh [VERSION] [--skip-build] [--card-bundle ZIP] [--out DIR]
#   VERSION         defaults to the VERSION file at the repository root (e.g. 0.9.0 -> release tag v0.9.0)
#   --skip-build    re-pack the binaries an earlier run of the same version compiled (dist/build-<ver>/)
#   --card-bundle   copy that zip (and <name>.manifest.txt beside it, if any) into the release; without the flag the newest
#                   dist/meehaw-card-free*.zip is used when one exists (the SHAREABLE card content only: GEN clips and the
#                   MIT ldrolez chords — never the owner's pack derivatives, docs/RESEARCH_LIBRARY_EXPANSION.md §6)
#   --out DIR       the release folder (default dist/release-<ver>); it is emptied first
# Needs: arduino-cli with the esp32 core 3.3.12 and the M5 libraries (README "Build and flash"), the esptool that core
# ships (v5, hyphenated commands; ESPTOOL=/path overrides), python3, shasum or sha256sum. Nothing is flashed or uploaded:
# tools/flash.sh / docs/FLASHING.md flash, tools/gh_release.py uploads the folder to a draft GitHub release.
set -e
cd "$(dirname "$0")/.."

VER=""; SKIP_BUILD=0; CARD_BUNDLE=""; OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    --card-bundle) CARD_BUNDLE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    -*) echo "unknown option: $1"; exit 2 ;;
    *) [ -z "$VER" ] || { echo "one version only: $1"; exit 2; }; VER="$1"; shift ;;
  esac
done
[ -n "$VER" ] || VER=$(tr -d ' \t\r\n' < VERSION 2>/dev/null || true)
[ -n "$VER" ] || { echo "no version: pass one or write the VERSION file"; exit 2; }
case "$VER" in *[!0-9A-Za-z.+-]*) echo "bad version string: $VER"; exit 2 ;; esac

FQBN_PLAYER="esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled"
FQBN_DRIVE="$FQBN_PLAYER,USBMode=default"            # USBMode=default = USB-OTG (TinyUSB) for the mass-storage device
CHIP=esp32s3
APP_MAX=3342336                                      # 0x330000: app0 / app1 of default_8MB.csv
OFF_BOOT=0x0; OFF_PARTS=0x8000; OFF_OTADATA=0xe000; OFF_PLAYER=0x10000; OFF_DRIVE=0x340000
BUILD="dist/build-$VER"; [ -n "$OUT" ] || OUT="dist/release-$VER"
PB="$BUILD/player"; DB="$BUILD/drive"; LB="$BUILD/loader"
PLATFORM="$HOME/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12"
[ -n "$ESPTOOL" ] || ESPTOOL=$(ls -d "$HOME"/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | sort -V | tail -1)
[ -x "$ESPTOOL" ] || ESPTOOL=$(command -v esptool || command -v esptool.py || true)
[ -n "$ESPTOOL" ] || { echo "esptool not found (the esp32 core ships one under ~/Library/Arduino15/packages/esp32/tools/esptool_py)"; exit 1; }
command -v arduino-cli >/dev/null || { echo "arduino-cli not found"; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found"; exit 1; }
if command -v shasum >/dev/null; then SHA="shasum -a 256"; elif command -v sha256sum >/dev/null; then SHA=sha256sum; else echo "shasum / sha256sum not found"; exit 1; fi
fsize() { stat -f %z "$1" 2>/dev/null || stat -c %s "$1"; }
started=$(date +%s)

# ---- 1. compile ------------------------------------------------------------------------------------------------------
build() {   # build <name> <sketch dir> <fqbn> <build dir>
  b_log="$BUILD/$1.log"
  echo "== compiling $2 ($1) with $3"
  echo "   log: $b_log"
  mkdir -p "$4"
  if ! arduino-cli compile --fqbn "$3" --build-path "$4" "$2" > "$b_log" 2>&1; then
    grep -E "error|Error" "$b_log" | head -20; echo "compile of $2 failed: see $b_log"; exit 1
  fi
  grep -E "Sketch uses|Global variables" "$b_log" | sed 's/^/   /' || true
}
mkdir -p "$BUILD"
if [ "$SKIP_BUILD" = 0 ]; then
  build player firmware/liveplayer "$FQBN_PLAYER" "$PB"
  build drive  firmware/carddrive  "$FQBN_DRIVE"  "$DB"
  build loader firmware/cardloader "$FQBN_PLAYER" "$LB"
else
  echo "== --skip-build: using the binaries in $BUILD"
fi
BOOTLOADER="$PB/liveplayer.ino.bootloader.bin"; PARTITIONS="$PB/liveplayer.ino.partitions.bin"
PLAYER="$PB/liveplayer.ino.bin"; DRIVE="$DB/carddrive.ino.bin"; LOADER="$LB/cardloader.ino.bin"
OTADATA="$PB/boot_app0.bin"; [ -f "$OTADATA" ] || OTADATA="$PLATFORM/tools/partitions/boot_app0.bin"   # the core copies it into the build dir (platform.txt postobjcopy.5)
for f in "$BOOTLOADER" "$PARTITIONS" "$OTADATA" "$PLAYER" "$DRIVE" "$LOADER"; do [ -f "$f" ] || { echo "missing build output: $f"; exit 1; }; done
for f in "$PLAYER" "$DRIVE" "$LOADER"; do
  s=$(fsize "$f"); [ "$s" -le "$APP_MAX" ] || { echo "$f is $s bytes: does not fit an OTA slot ($APP_MAX)"; exit 1; }
done
# The partition table the player was built with must be the 8 MB two-slot table: the drive image goes to its app1 offset.
python3 -I - "$PARTITIONS" "$OFF_PLAYER" "$OFF_DRIVE" <<'EOF'
import struct, sys
table = open(sys.argv[1], 'rb').read(); want = {int(sys.argv[2], 16): b'app0', int(sys.argv[3], 16): b'app1'}
seen = {}
for i in range(0, len(table), 32):
    e = table[i:i + 32]
    if e[:2] != b'\xaa\x50': break
    _, ptype, sub, off, size = struct.unpack('<HBBII', e[:12]); seen[off] = (e[12:28].rstrip(b'\0'), size)
for off, name in want.items():
    got = seen.get(off)
    if not got or got[0] != name: sys.exit('partition table: expected %s at 0x%x, found %s (build with PartitionScheme=default_8MB)' % (name.decode(), off, got))
    if got[1] != 0x330000: sys.exit('partition %s is 0x%x bytes, expected 0x330000' % (name.decode(), got[1]))
print('   partition table ok: app0 @0x%x, app1 @0x%x, 0x330000 each' % tuple(want))
EOF

# ---- 2. the release folder ---------------------------------------------------------------------------------------------
rm -rf "$OUT"; mkdir -p "$OUT"
FULL="$OUT/meehaw-player-$VER-full.bin"
PLAYER_APP="$OUT/meehaw-player-$VER-app.bin"; DRIVE_APP="$OUT/meehaw-carddrive-$VER-app.bin"; LOADER_APP="$OUT/meehaw-cardloader-$VER-app.bin"
echo "== merging the full image -> $FULL"
# --flash-mode/freq/size keep: the bootloader header keeps what the core built (dio, 80m, 8MB); no --pad-to-size, the file
# ends with the drive image (~4 MB instead of a padded 8 MB) and write-flash at 0x0 leaves everything beyond it untouched.
"$ESPTOOL" --chip "$CHIP" merge-bin -o "$FULL" --flash-mode keep --flash-freq keep --flash-size keep \
  "$OFF_BOOT" "$BOOTLOADER" "$OFF_PARTS" "$PARTITIONS" "$OFF_OTADATA" "$OTADATA" "$OFF_PLAYER" "$PLAYER" "$OFF_DRIVE" "$DRIVE" \
  > "$BUILD/merge.log" 2>&1 || { cat "$BUILD/merge.log"; echo "merge-bin failed"; exit 1; }
# Every part must sit at its offset, byte for byte, and the gaps must be 0xFF (erased flash).
python3 -I - "$FULL" "$OFF_BOOT" "$BOOTLOADER" "$OFF_PARTS" "$PARTITIONS" "$OFF_OTADATA" "$OTADATA" "$OFF_PLAYER" "$PLAYER" "$OFF_DRIVE" "$DRIVE" <<'EOF'
import sys
full = open(sys.argv[1], 'rb').read(); args = sys.argv[2:]; parts = []
for off, path in zip(args[0::2], args[1::2]):
    o = int(off, 16); data = open(path, 'rb').read(); parts.append((o, len(data), path))
    if full[o:o + len(data)] != data: sys.exit('layout check FAILED: %s is not at %s' % (path, off))
end = 0
for o, n, path in sorted(parts):
    if o < end: sys.exit('layout check FAILED: %s overlaps the previous part' % path)
    if full[end:o].strip(b'\xff'): sys.exit('layout check FAILED: the gap before 0x%x is not 0xFF' % o)
    end = o + n
if end != len(full): sys.exit('layout check FAILED: %d bytes after the last part' % (len(full) - end))
print('   layout verified: %d parts, %d bytes, ends at 0x%x' % (len(parts), len(full), len(full)))
EOF
cp "$PLAYER" "$PLAYER_APP"; cp "$DRIVE" "$DRIVE_APP"; cp "$LOADER" "$LOADER_APP"

echo "== docs, stickers, web flasher"
for d in docs/FLASHING.md docs/GETTING_STARTED.md docs/CARD_IMPORT.md docs/MANUAL.md docs/KEYMAP.md; do
  if [ -f "$d" ]; then cp "$d" "$OUT/"; else echo "   (no $d — skipped)"; fi
done
# LICENSE and the credits are not optional: the MIT / LGPL-2.1 / Apache-2.0 / FatFs / TinyUSB notices of what the binaries
# link must accompany every copy of them, GitHub release or AirDropped folder alike (docs/SHARING.md §2).
for d in LICENSE docs/CREDITS.md; do
  [ -f "$d" ] || { echo "$d is missing: a release must carry the licence and the credits"; exit 1; }
  cp "$d" "$OUT/"
done
for p in docs/stickers/*.pdf; do [ -f "$p" ] && cp "$p" "$OUT/sticker-$(basename "$p")"; done
[ -f tools/webflash/index.html ] || { echo "tools/webflash/index.html is missing"; exit 1; }
cp tools/webflash/index.html "$OUT/index.html"
cat > "$OUT/manifest.json" <<EOF
{
  "name": "meehaw live performer",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "meehaw-player-$VER-full.bin", "offset": 0 }
      ]
    }
  ]
}
EOF
if [ -f tools/webflash/manifest.json ] && ! cmp -s "$OUT/manifest.json" tools/webflash/manifest.json; then
  cp "$OUT/manifest.json" tools/webflash/manifest.json; echo "   tools/webflash/manifest.json now names $VER"
fi

# the free card bundle, when there is one (built by the content tools into dist/)
if [ -z "$CARD_BUNDLE" ]; then CARD_BUNDLE=$(ls -t dist/meehaw-card-free*.zip 2>/dev/null | head -1 || true); fi
if [ -n "$CARD_BUNDLE" ]; then
  [ -f "$CARD_BUNDLE" ] || { echo "no such card bundle: $CARD_BUNDLE"; exit 1; }
  echo "== card bundle: $CARD_BUNDLE"
  cp "$CARD_BUNDLE" "$OUT/"
  m="${CARD_BUNDLE%.zip}.manifest.txt"; [ -f "$m" ] && cp "$m" "$OUT/"
else
  echo "== no free card bundle (dist/meehaw-card-free*.zip); the release holds the firmware and the docs only"
fi

cat > "$OUT/README.txt" <<EOF
meehaw live performer $VER — release files (built $(date '+%Y-%m-%d %H:%M %Z'))

Firmware for the M5Stack Cardputer ADV (ESP32-S3, 8 MB flash). How to flash: FLASHING.md (esptool on macOS / Linux /
Windows, or index.html = the web flasher for Chrome / Edge: serve this folder with \`python3 -m http.server\` and open
http://localhost:8000). First steps: GETTING_STARTED.md (when present) and MANUAL.md; every key: KEYMAP.md; the card layout
and the ways to fill a card: CARD_IMPORT.md; printable key stickers: sticker-keys-main.pdf / sticker-keys-shift.pdf.

  meehaw-player-$VER-full.bin       flash at 0x0: the player (ota_0) AND the USB drive firmware (ota_1) with the bootloader,
                                    partition table and otadata. The one file a new device needs.
  meehaw-player-$VER-app.bin        the player alone — an update, flash at 0x10000
  meehaw-carddrive-$VER-app.bin     the USB drive firmware alone — flash at 0x340000
  meehaw-cardloader-$VER-app.bin    the serial card loader — flash at 0x10000; it replaces the player until the player
                                    app is flashed again (CARD_IMPORT.md B)
  SHA256SUMS                        verify: shasum -a 256 -c SHA256SUMS   (Linux: sha256sum -c SHA256SUMS)
  LICENSE                           the MIT licence of the firmware, the tools and the docs (Copyright (c) 2026 Miha Ambroz)
  CREDITS.md                        the third-party code and content and their licences. The binaries link M5Unified, M5GFX and
                                    TinyUSB (MIT), arduino-esp32 (LGPL-2.1), ESP-IDF (Apache-2.0) and FatFs: keep LICENSE and
                                    CREDITS.md with any copy of them.

The player needs card content in /lp on the microSD card (GETTING_STARTED.md / CARD_IMPORT.md); a free card bundle
(meehaw-card-free*.zip), when it ships with this release, holds the shareable part of the library; its own licences are
in lp/LICENSES.md inside the zip.
Repository: https://github.com/mihatm8/meehaw-live-performer — Miha Ambroz (GitHub mihatm8). Licence: MIT (LICENSE).
EOF

echo "== SHA256SUMS"
( cd "$OUT" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | xargs $SHA > SHA256SUMS )

# ---- 3. report -----------------------------------------------------------------------------------------------------------
echo
echo "release $VER in $OUT ($(( $(date +%s) - started )) s):"
total=0
for f in "$OUT"/*; do
  s=$(fsize "$f"); total=$(( total + s ))
  printf "  %-44s %10d bytes  %7.2f MB\n" "$(basename "$f")" "$s" "$(echo "$s" | awk '{ printf "%.2f", $1 / 1048576 }')"
done
printf "  %-44s %10d bytes  %7.2f MB\n" "TOTAL" "$total" "$(echo "$total" | awk '{ printf "%.2f", $1 / 1048576 }')"
echo
echo "app images vs the 0x330000 slot: player $(fsize "$PLAYER") / drive $(fsize "$DRIVE") / loader $(fsize "$LOADER") of $APP_MAX bytes"
echo "next: python3 -I tools/gh_release.py --dry-run     (then without --dry-run: a DRAFT release v$VER with these files)"
