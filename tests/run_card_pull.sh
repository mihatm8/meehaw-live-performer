#!/bin/sh
# tools/card_pull.py protocol self-test (SPEC §9.22): both serial dialects (player `ls`/`get`, loader `LS`/`GET`) over an
# in-process fake port — binary content with \r\n and protocol-looking lines inside, an empty file, a missing file,
# [diag] chatter before replies, the same-size skip, and a short (cut) transfer that must be reported, not written.
# Uses tools/.venv's python when present (pyserial is not needed for the self-test), python3 otherwise.
cd "$(dirname "$0")/.." || exit 1
PY=tools/.venv/bin/python; [ -x "$PY" ] || PY=python3
"$PY" -I tools/card_pull.py --selftest
