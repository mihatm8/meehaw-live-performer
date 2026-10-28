#!/bin/sh
# Host tests for gen/sets.cpp (the device-native .lpset writer/loader of SPEC §9.29 and its legacy reader, the session
# files, loop mode persistence, the preset manager, the random-load cycle and the slow-card session write / save of
# SPEC §9.19 §3) against the real engine, dsp, gen and io modules over sdcard/lp (override: LP_SDCARD). Uses gen/generator.cpp when it exists and compiles; otherwise
# tests/stubs_generator.cpp (a minimal Generator with the same public contract) stands in, and says so.
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC"
GEN=tests/stubs_generator.cpp
if [ -f $SRC/gen/generator.cpp ] && clang++ $FLAGS -c $SRC/gen/generator.cpp -o /tmp/lp_test_sets_gen.o 2>/dev/null; then
  GEN=/tmp/lp_test_sets_gen.o; echo "generator: gen/generator.cpp"
else
  echo "generator: tests/stubs_generator.cpp (gen/generator.cpp absent or not compiling)"
fi
clang++ $FLAGS \
  tests/test_sets.cpp $SRC/gen/sets.cpp $GEN \
  $SRC/gen/library.cpp $SRC/gen/key.cpp $SRC/gen/smf.cpp $SRC/gen/variants.cpp \
  $SRC/io/fs.cpp $SRC/io/wav.cpp firmware/desktop/posix_fs.cpp \
  $SRC/engine/engine.cpp $SRC/dsp/ulaw.cpp $SRC/dsp/svf.cpp $SRC/dsp/env.cpp $SRC/dsp/wavetables.cpp \
  $SRC/dsp/synth_voice.cpp $SRC/dsp/sample_voice.cpp $SRC/dsp/echo.cpp $SRC/dsp/reverb.cpp $SRC/dsp/chorus.cpp $SRC/dsp/presets.cpp \
  -o /tmp/lp_test_sets && /tmp/lp_test_sets
