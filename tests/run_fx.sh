#!/bin/sh
# fx module: dsp/echo.cpp + dsp/reverb.cpp + dsp/chorus.cpp host tests (the echo line is µ-law: dsp/ulaw.cpp supplies the codec, SPEC §9.19; the chorus bus is SPEC §9.22).
cd "$(dirname "$0")/.." && clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror=return-type -Ifirmware/liveplayer/src tests/test_fx.cpp firmware/liveplayer/src/dsp/echo.cpp firmware/liveplayer/src/dsp/reverb.cpp firmware/liveplayer/src/dsp/chorus.cpp firmware/liveplayer/src/dsp/ulaw.cpp -o /tmp/lp_test_fx && /tmp/lp_test_fx
