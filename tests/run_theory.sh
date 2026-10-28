#!/bin/sh
# Host tests for the THEORY SET GENERATOR (SPEC §9.24): the flash tables (gen/theory_tables.hpp must match
# tools/theory_data.py, the same tables tools/make_clips.py uses), the harmony helpers and the five melodic builders of
# gen/variants.cpp over 1000 seeds (in key, bands, note cap, variants, determinism, duplicate rate). Pure: no card, no
# engine, so it may run next to the card-bound runners. The generator's JobTheory state machine is covered by
# tests/test_generator.cpp (run_generator.sh).
cd "$(dirname "$0")/.." || exit 1
SRC=firmware/liveplayer/src
python3 -I tools/make_theory_tables.py --check || exit 1
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -I$SRC \
  tests/test_theory.cpp $SRC/gen/variants.cpp $SRC/gen/key.cpp -o /tmp/lp_test_theory && /tmp/lp_test_theory
