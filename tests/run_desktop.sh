#!/bin/sh
# Desktop module (SPEC §7, firmware/desktop): three parts, all against brew's SDL2 (sdl2-config).
#   1. tests/test_desktop.cpp: the SDL IGfx on a headless surface (incl. the present() / pump() flip rate limit),
#      the 5x7 font, the scancode → matrix table through the real KeyMatrix / key map, the arrow-key IMU model
#      (a held arrow saturates the UI's gyro mapping) and the selftest helpers of selftest.h (checks, verdict,
#      SECONDS parsing, frame path).
#   2. make in firmware/desktop (every firmware .cpp + the desktop files; ui_stub.cpp only while src/ui/ui.cpp
#      is absent), then the argument guard: `--selftest 0 / -3 / abc / (missing)`, `--seconds 0` and an unknown
#      option must exit 2 with the usage text before SDL starts, so a headless run can never be endless.
#   3. `liveplayer_desktop --selftest N`, run from the repository root (not from firmware/desktop): a headless
#      scripted session (generate set 1, start five tracks, variants, rush / cut, a SHIFT ramp, hold repeats)
#      whose checks read the engine's published state, for LP_SELFTEST_SECONDS (default 25) against sdcard
#      (override: LP_SDCARD, a card root holding lp/, relative to the repository root). It exits 1 on a render
#      underrun, late render turns, a failed check, vocal ring underruns, silence or a blank UI. The frame it
#      saves must land next to the binary (firmware/desktop/build/selftest_frame.bmp) whatever the cwd.
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC $(sdl2-config --cflags) \
  tests/test_desktop.cpp firmware/desktop/sdl_gfx.cpp firmware/desktop/desktop_input.cpp $SRC/io/keys.cpp $(sdl2-config --libs) \
  -o /tmp/lp_test_desktop && /tmp/lp_test_desktop || exit 1
(cd firmware/desktop && make -s) || exit 1
BIN=firmware/desktop/build/liveplayer_desktop
# Argument guard. perl's alarm survives exec: a regression that starts an endless run is killed after 5 s (rc 142).
for bad in "--selftest 0" "--selftest -3" "--selftest abc" "--selftest 12x" "--selftest" "--seconds 0" "--frame" "--bogus"; do
  perl -e 'alarm 5; exec @ARGV' "$BIN" $bad >/dev/null 2>/tmp/lp_desktop_usage.txt; rc=$?
  if [ "$rc" != 2 ] || ! grep -q '^usage:' /tmp/lp_desktop_usage.txt; then echo "argument guard: '$bad' exited $rc (want 2 + usage): $(head -1 /tmp/lp_desktop_usage.txt)"; exit 1; fi
done
"$BIN" --help | grep -q '^usage:' || { echo "--help must print the usage"; exit 1; }
echo "argument guard: ok (8 bad invocations exit 2 with usage)"
FRAME=firmware/desktop/build/selftest_frame.bmp
rm -f "$FRAME"
"$BIN" --selftest "${LP_SELFTEST_SECONDS:-25}" "${LP_SDCARD:-sdcard}" || exit 1
[ -s "$FRAME" ] || { echo "selftest frame missing: $FRAME (must be saved next to the binary, not in the cwd)"; exit 1; }
echo "selftest frame: $FRAME ($(wc -c < "$FRAME" | tr -d ' ') bytes)"
