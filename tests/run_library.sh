#!/bin/sh
# Host tests for gen/library.cpp (SD index, _index.txt streaming, shuffle bags, keyed kicks, shard folders, the loop
# picks of SPEC §9.19 over /lp/packs/<genre>/loops).
# Corpus checks read sdcard/lp (override: LP_SDCARD); the card's _index.txt files are refreshed first
# (idempotent, tools/write_indexes.py) because the index-based picks and the timing bound rely on them.
cd "$(dirname "$0")/.." || exit 1
CARD="${LP_SDCARD:-sdcard}"
[ -d "$CARD/lp" ] && { python3 -I tools/write_indexes.py "$CARD" --quiet || exit 1; }
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type \
  -Ifirmware/liveplayer/src tests/test_library.cpp \
  firmware/liveplayer/src/gen/library.cpp firmware/liveplayer/src/gen/key.cpp firmware/liveplayer/src/io/fs.cpp \
  firmware/desktop/posix_fs.cpp \
  -o /tmp/lp_test_library && /tmp/lp_test_library
