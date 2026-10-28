#!/bin/sh
# Builds and runs the synth module tests. Uses the real dsp-core sources when present, else the stubs.
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
TMP="${TMPDIR:-/tmp}"
DEPS=""; STUBDEFS=""
if [ -f $SRC/dsp/wavetables.cpp ]; then DEPS="$DEPS $SRC/dsp/wavetables.cpp"; else STUBDEFS="$STUBDEFS -DLP_STUB_WAVE"; fi
if [ -f $SRC/dsp/svf.cpp ]; then DEPS="$DEPS $SRC/dsp/svf.cpp"; else STUBDEFS="$STUBDEFS -DLP_STUB_SVF"; fi
if [ -f $SRC/dsp/env.cpp ]; then DEPS="$DEPS $SRC/dsp/env.cpp"; else STUBDEFS="$STUBDEFS -DLP_STUB_ENV"; fi
[ -n "$STUBDEFS" ] && DEPS="$DEPS tests/stubs_synth.cpp"
# The preset banks are generated: the committed dsp/presets_gen.hpp must be what tools/gen_presets.py emits from its
# archetypes and the baked trims (tools/gen_presets_trims.json), or an edit to the tool was not re-run.
if command -v python3 >/dev/null 2>&1; then
  if python3 -I tools/gen_presets.py --out "$TMP/lp_presets_gen_check.hpp" >/dev/null && cmp -s "$TMP/lp_presets_gen_check.hpp" $SRC/dsp/presets_gen.hpp; then
    echo "presets_gen.hpp: in sync with tools/gen_presets.py"
  else
    echo "presets_gen.hpp: OUT OF SYNC with tools/gen_presets.py (re-run: python3 -I tools/gen_presets.py --qa)"; exit 1
  fi
fi
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC $STUBDEFS \
  tests/test_synth.cpp $SRC/dsp/synth_voice.cpp $SRC/dsp/presets.cpp $DEPS -o "$TMP/lp_test_synth" && "$TMP/lp_test_synth"
