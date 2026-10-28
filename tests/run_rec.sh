#!/bin/sh
# Host tests for rec/recorder.cpp (SPEC §9.23): session files, the write-behind buffer, the list / info / delete API and
# the offline render against the real engine, generator, sets, Ui, library, dsp and io modules over sdcard/lp through
# PosixFs (override: LP_SDCARD). The recorder's own folders (/lp/rec, /lp/wav) and the session writer's /lp/state are
# routed to a temporary card directory (TMPDIR), which the last test removes; the real card image is never written.
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC \
  tests/test_rec.cpp \
  $SRC/rec/recorder.cpp \
  $SRC/ui/ui.cpp $SRC/io/keys.cpp \
  $SRC/gen/generator.cpp $SRC/gen/sets.cpp $SRC/gen/library.cpp $SRC/gen/key.cpp $SRC/gen/smf.cpp $SRC/gen/variants.cpp \
  $SRC/engine/engine.cpp \
  $SRC/dsp/*.cpp \
  $SRC/io/fs.cpp $SRC/io/wav.cpp \
  firmware/desktop/posix_fs.cpp \
  -o /tmp/lp_test_rec && /tmp/lp_test_rec
