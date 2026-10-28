# Host tests

Each module has `tests/test_<module>.cpp` and `tests/run_<module>.sh`. Tests compile the module's
`.cpp` files from `firmware/liveplayer/src` with clang++ on the Mac (no Arduino), using `check.h`.
Test data: `sdcard/lp` (the real library import). `run_all.sh` runs every `run_*.sh`.
