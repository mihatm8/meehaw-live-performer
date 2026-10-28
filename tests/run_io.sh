#!/bin/sh
# Host tests for the io module (fs helpers, key matrix, WAV) and the desktop PosixFs.
cd "$(dirname "$0")/.." || exit 1
ULAW=firmware/liveplayer/src/dsp/ulaw.cpp
[ -f "$ULAW" ] || ULAW=tests/stubs_io.cpp
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -Ifirmware/liveplayer/src \
  tests/test_io.cpp \
  firmware/liveplayer/src/io/fs.cpp firmware/liveplayer/src/io/keys.cpp firmware/liveplayer/src/io/wav.cpp \
  firmware/desktop/posix_fs.cpp "$ULAW" \
  -o /tmp/lp_test_io && /tmp/lp_test_io
