#!/bin/sh
# Host test for src/gen/variants.cpp. Uses the real key.cpp when it exists, else the ProjectKey stub.
cd "$(dirname "$0")/.." || exit 1
KEY=firmware/liveplayer/src/gen/key.cpp
[ -f "$KEY" ] || KEY=tests/stubs_variants.cpp
clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -Ifirmware/liveplayer/src \
  tests/test_variants.cpp firmware/liveplayer/src/gen/variants.cpp "$KEY" -o /tmp/lp_test_variants && /tmp/lp_test_variants
