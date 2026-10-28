#!/bin/sh
# Host tests for gen/key.cpp and gen/smf.cpp. Corpus checks read sdcard/lp/midi (override: LP_MIDI_DIR).
cd "$(dirname "$0")/.." && clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type \
  -Ifirmware/liveplayer/src tests/test_key_smf.cpp \
  firmware/liveplayer/src/gen/key.cpp firmware/liveplayer/src/gen/smf.cpp \
  -o /tmp/lp_test_key_smf && /tmp/lp_test_key_smf
