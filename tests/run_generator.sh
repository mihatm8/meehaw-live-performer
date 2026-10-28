#!/bin/sh
# Host tests for gen/generator.cpp: the generation state machine, the vocal ring producer and the loop streams of
# SPEC §9.19 (loop mode: picks, crops, len steps, the health gate, the remembered pick folders), driven against the
# real engine, library, parser, variants, loaders and PosixFs over sdcard/lp (override: LP_SDCARD). The loop slots'
# ring memory is two static arrays in the test (the sketch's bootBlock).
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC \
  tests/test_generator.cpp \
  $SRC/gen/generator.cpp $SRC/gen/library.cpp $SRC/gen/key.cpp $SRC/gen/smf.cpp $SRC/gen/variants.cpp \
  $SRC/engine/engine.cpp \
  $SRC/dsp/ulaw.cpp $SRC/dsp/svf.cpp $SRC/dsp/env.cpp $SRC/dsp/wavetables.cpp $SRC/dsp/synth_voice.cpp \
  $SRC/dsp/sample_voice.cpp $SRC/dsp/echo.cpp $SRC/dsp/reverb.cpp $SRC/dsp/chorus.cpp $SRC/dsp/presets.cpp \
  $SRC/io/fs.cpp $SRC/io/wav.cpp \
  firmware/desktop/posix_fs.cpp \
  -o /tmp/lp_test_generator && /tmp/lp_test_generator
