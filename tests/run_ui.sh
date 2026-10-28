#!/bin/sh
# Host tests for ui/ui.cpp (SPEC §3, §4, §9.10, §9.14, §9.19, §9.22, §9.23, §9.24 theory keys / preset manager remap, §9.25
# cycle marks): a recording IGfx and the real engine, generator, sets, library, dsp and io modules over sdcard/lp through
# PosixFs (override: LP_SDCARD). The session recorder (src/rec/recorder.cpp) is linked when it exists and compiles;
# otherwise the test file's in-memory stand-in (-DLP_UI_NO_RECORDER) models the contract and the run says so. Likewise the
# theory generator (SPEC §9.24): while src/gen/generator.cpp does not define Generator::generateTheorySet the test file's
# stand-in (-DLP_UI_THEORY_STANDIN, a set generate under the theory label) fills the two pinned symbols and the run says so.
# The recorder's folders (/lp/rec, /lp/wav) are routed to a temporary card directory (TMPDIR) that the cleanup test removes,
# as tests/run_rec.sh does: sessions pulled from the device into sdcard/lp/rec are never touched. The preset manager tests
# build and remove their own Testpack presets.
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC"
REC=""
if [ -f $SRC/rec/recorder.cpp ] && clang++ $FLAGS -c $SRC/rec/recorder.cpp -o /tmp/lp_test_ui_rec.o 2>/dev/null; then
  REC=/tmp/lp_test_ui_rec.o; echo "recorder: src/rec/recorder.cpp"
else
  FLAGS="$FLAGS -DLP_UI_NO_RECORDER"; echo "recorder: in-file stand-in (src/rec/recorder.cpp absent or not compiling)"
fi
GEN=$SRC/gen/generator.cpp
if clang++ $FLAGS -c $SRC/gen/generator.cpp -o /tmp/lp_test_ui_gen.o 2>/dev/null; then
  GEN=/tmp/lp_test_ui_gen.o
  if nm /tmp/lp_test_ui_gen.o | grep -q 'generateTheorySet'; then echo "theory generator: src/gen/generator.cpp"
  else FLAGS="$FLAGS -DLP_UI_THEORY_STANDIN"; echo "theory generator: in-file stand-in (Generator::generateTheorySet not yet defined in src/gen/generator.cpp)"; fi
fi
clang++ $FLAGS \
  tests/test_ui.cpp \
  $SRC/ui/ui.cpp $SRC/io/keys.cpp $REC \
  $GEN $SRC/gen/sets.cpp $SRC/gen/library.cpp $SRC/gen/key.cpp $SRC/gen/smf.cpp $SRC/gen/variants.cpp \
  $SRC/engine/engine.cpp \
  $SRC/dsp/ulaw.cpp $SRC/dsp/svf.cpp $SRC/dsp/env.cpp $SRC/dsp/wavetables.cpp $SRC/dsp/synth_voice.cpp \
  $SRC/dsp/sample_voice.cpp $SRC/dsp/echo.cpp $SRC/dsp/reverb.cpp $SRC/dsp/chorus.cpp $SRC/dsp/presets.cpp \
  $SRC/io/fs.cpp $SRC/io/wav.cpp \
  firmware/desktop/posix_fs.cpp \
  -o /tmp/lp_test_ui && /tmp/lp_test_ui
