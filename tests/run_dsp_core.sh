#!/bin/sh
# Host test for dsp-core (ulaw, svf, env, wavetables, sample_voice). Run from anywhere.
cd "$(dirname "$0")/.." || exit 1
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -Ifirmware/liveplayer/src \
  tests/test_dsp_core.cpp \
  firmware/liveplayer/src/dsp/ulaw.cpp \
  firmware/liveplayer/src/dsp/svf.cpp \
  firmware/liveplayer/src/dsp/env.cpp \
  firmware/liveplayer/src/dsp/wavetables.cpp \
  firmware/liveplayer/src/dsp/sample_voice.cpp \
  -o /tmp/lp_test_dsp_core && /tmp/lp_test_dsp_core || exit 1
# g_ulawTable must be constant-initialised (valid before any constructor runs, no runtime initialiser).
clang++ -std=c++17 -O2 -Ifirmware/liveplayer/src -c firmware/liveplayer/src/dsp/ulaw.cpp -o /tmp/lp_ulaw_check.o || exit 1
if nm /tmp/lp_ulaw_check.o | grep -q 'GLOBAL__sub_I\|GLOBAL__I_'; then echo "FAIL: g_ulawTable has a runtime initialiser"; exit 1; fi
echo "ulaw decode table: constant-initialised (no static constructor)"
