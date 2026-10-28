#!/bin/sh
cd "$(dirname "$0")" && fail=0; for s in run_*.sh; do [ "$s" = run_all.sh ] && continue; echo "== $s"; sh "$s" || fail=1; done; exit $fail
