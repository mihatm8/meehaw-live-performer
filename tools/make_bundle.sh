#!/bin/sh
# Bundle a card image for sharing: zips a card folder into dist/<name>.zip with a manifest.
#
# Usage: sh tools/make_bundle.sh [--card sdcard/lp] [--out dist] [--name meehaw-card-YYYYMMDD] [--with-state]
#        sh tools/make_bundle.sh --free [--from sdcard/lp] [--card sdcard/lp_free/lp] [--name meehaw-card-free-YYYYMMDD]
#
# Default (the owner's full card): zips sdcard/lp as it is.
# --free: the licence-clean bundle for friends (docs/SHARING.md), built from the stage sdcard/lp_free/lp. Before zipping it
#   (1) with --from DIR, copies the shareable MIDI of that card (every `__GEN-` and `__LDZ-` file, flattening a key
#       folder's shards first so a re-run never leaves files beside shards) and DIR/config.txt into the stage —
#       never the library import, the -TR / -SH / __CHP__ / __LOOP__ derivatives, the vocals or the one-shots;
#       the stage's own content (tools/make_clips.py Beat / Percussion / Bass / Fx, tools/make_synth_kit.py) stays;
#   (2) shards every folder above 64 files (tools/shard_folders.py --max 64);
#   (3) rewrites every _index.txt (tools/write_indexes.py --prune);
#   (4) runs tools/card_qa.py over the stage and stops when it fails;
#   then zips the stage. The zip is named meehaw-card-free-<YYYYMMDD>.zip (tools/make_release.sh picks dist/meehaw-card-free*.zip up).
#
# Left out of the zip: packs/Testpack (the host tests' fixtures), any folder whose name ends in `_stage` (staging
# mirrors; a file name may contain the word), and — unless --with-state — the card's own folders that tools/card_sync.py protects as well: state/
# (the session the device saves, SPEC §9.18), */sets/ and */presets/ (saved sets and presets); .DS_Store files.
# The zip unpacks to a single `lp/` folder: that folder goes to the card root (docs/CARD_IMPORT.md).
# Output: <out>/<name>.zip, <out>/<name>.manifest.txt (file count, MB, sha256 of the zip, per-folder counts) and a
# copy of docs/CARD_IMPORT.md next to them. dist/ is gitignored. Needs zip, shasum, python3 (macOS: all built in).
set -e
cd "$(dirname "$0")/.."
CARD=sdcard/lp; OUT=dist; NAME=""; WITH_STATE=0; FREE=0; FROM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --card) CARD="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --name) NAME="$2"; shift 2 ;;
    --with-state) WITH_STATE=1; shift ;;
    --free) FREE=1; [ "$CARD" = sdcard/lp ] && CARD=sdcard/lp_free/lp; shift ;;
    --from) FROM="$2"; shift 2 ;;
    *) echo "unknown argument: $1"; exit 2 ;;
  esac
done
if [ -z "$NAME" ]; then
  if [ "$FREE" = 1 ]; then NAME="meehaw-card-free-$(date +%Y%m%d)"; else NAME="meehaw-card-$(date +%Y%m%d)"; fi
fi
[ "$FREE" = 1 ] && mkdir -p "$CARD"
[ -d "$CARD" ] || { echo "no card image at $CARD"; exit 1; }
command -v zip >/dev/null || { echo "zip not found"; exit 1; }
mkdir -p "$OUT"
ZIP="$OUT/$NAME.zip"; MANIFEST="$OUT/$NAME.manifest.txt"
rm -f "$ZIP" "$MANIFEST"

PARENT=$(cd "$(dirname "$CARD")" && pwd); BASE=$(basename "$CARD"); ABSZIP="$(pwd)/$ZIP"

if [ "$FREE" = 1 ]; then
  ROOT=$(dirname "$CARD")
  if [ -n "$FROM" ]; then
    [ -d "$FROM/midi" ] || { echo "no card to copy from at $FROM (needs $FROM/midi)"; exit 1; }
    echo "copying the shareable MIDI (GEN, LDZ) and config.txt from $FROM -> $CARD"
    python3 -I - "$FROM" "$CARD" <<'EOF'
import os, re, shutil, sys
src, dst = sys.argv[1], sys.argv[2]
FREE_RE = re.compile(r"__(GEN|LDZ)-")           # the derivation tokens of the shareable material (lp/LICENSES.md)
SHARD_RE = re.compile(r"^s\d\d$")
def flatten(folder):
    """Move a sharded key folder's files back up (shard_folders.py re-shards after the copy)."""
    for sub in sorted(os.listdir(folder)):
        p = os.path.join(folder, sub)
        if os.path.isdir(p) and SHARD_RE.match(sub):
            for f in os.listdir(p):
                if not f.startswith("."):
                    os.replace(os.path.join(p, f), os.path.join(folder, f))
            shutil.rmtree(p)
    idx = os.path.join(folder, "_index.txt")
    if os.path.exists(idx):
        os.remove(idx)
copied = skipped = 0
touched = set()
for dp, dirs, files in os.walk(os.path.join(src, "midi")):
    dirs.sort()
    for f in sorted(files):
        if not f.endswith(".mid") or f.startswith(".") or not FREE_RE.search(f):
            continue
        rel = os.path.relpath(dp, os.path.join(src, "midi")).split(os.sep)
        rel = [p for p in rel if not SHARD_RE.match(p)]             # <Category>/<Key>, shards dropped
        if len(rel) != 2:
            continue
        folder = os.path.join(dst, "midi", *rel)
        if folder not in touched:
            os.makedirs(folder, exist_ok=True)
            flatten(folder)
            touched.add(folder)
        target = os.path.join(folder, f)
        if os.path.exists(target):
            skipped += 1
            continue
        shutil.copyfile(os.path.join(dp, f), target)
        copied += 1
cfg = os.path.join(src, "config.txt")
if os.path.isfile(cfg):
    shutil.copyfile(cfg, os.path.join(dst, "config.txt"))
print("  %d files copied, %d already there, %d key folders" % (copied, skipped, len(touched)))
EOF
  fi
  echo "sharding $CARD"
  python3 -I tools/shard_folders.py "$CARD" --max 64
  echo "indexing $ROOT"
  python3 -I tools/write_indexes.py "$ROOT" --prune --quiet
  echo "card QA on $ROOT"
  python3 -I tools/card_qa.py "$ROOT" --quiet --no-tables || { echo "card QA failed: nothing zipped"; exit 1; }
  [ -f "$CARD/LICENSES.md" ] || { echo "$CARD/LICENSES.md is missing: a free bundle must name its licences"; exit 1; }
  if find "$CARD" -name '*.mid' | grep -Ev '__(GEN|LDZ)-' | grep -q .; then
    echo "the stage holds MIDI without a GEN / LDZ token (card-only material?): nothing zipped"; find "$CARD" -name '*.mid' | grep -Ev '__(GEN|LDZ)-' | head; exit 1
  fi
  if [ -d "$CARD/packs" ] && find "$CARD/packs" -path '*/vocal/*' -name '*.wav' | grep -q .; then
    echo "the stage holds vocal files: the free bundle ships none (docs/SHARING.md): nothing zipped"; exit 1
  fi
fi

echo "zipping $CARD -> $ZIP"
# -X: no extra Mac attributes; -r recursive; -q quiet. Run from the parent so the archive root is `lp/`. The -x
# patterns are quoted so zip matches them against the archive paths (an unquoted `*` would be expanded by the shell).
if [ "$WITH_STATE" = 0 ]; then
  ( cd "$PARENT" && zip -r -X -q "$ABSZIP" "$BASE" -x "$BASE/packs/Testpack/*" -x "*_stage/*" -x "*.DS_Store" -x "*.tmp" \
      -x "$BASE/state/*" -x "*/sets/*" -x "*/presets/*" )
else
  ( cd "$PARENT" && zip -r -X -q "$ABSZIP" "$BASE" -x "$BASE/packs/Testpack/*" -x "*_stage/*" -x "*.DS_Store" -x "*.tmp" )
fi

FILES=$(zipinfo -1 "$ZIP" | grep -v '/$' | wc -l | tr -d ' ')
UNPACKED=$(zipinfo -t "$ZIP" | sed -E 's/.* ([0-9]+) bytes uncompressed.*/\1/')
ZIPBYTES=$(stat -f %z "$ZIP" 2>/dev/null || stat -c %s "$ZIP")
SHA=$(shasum -a 256 "$ZIP" | cut -d' ' -f1)
mb() { echo "$1" | awk '{ printf "%.1f", $1 / 1048576 }'; }

{
  if [ "$FREE" = 1 ]; then echo "meehaw free card bundle $NAME"; else echo "meehaw card bundle $NAME"; fi
  echo "built $(date '+%Y-%m-%d %H:%M %Z') from $CARD"
  echo
  echo "zip:          $(basename "$ZIP")"
  echo "sha256:       $SHA"
  echo "zip size:     $ZIPBYTES bytes ($(mb "$ZIPBYTES") MB)"
  echo "files:        $FILES (unpacked $(mb "$UNPACKED") MB of bytes; on a FAT32 card with 32 KB clusters small files round up, expect roughly 1.5-2x)"
  echo "excluded:     packs/Testpack, *_stage/ folders, .DS_Store$( [ "$WITH_STATE" = 0 ] && echo ', state/, */sets/, */presets/ (the card keeps its own)')"
  echo
  echo "files per folder (media only):"
  zipinfo -1 "$ZIP" | grep -Ei '\.(mid|wav)$' | awk -F/ '{
      if ($2 == "midi") k = $2 "/" $3; else if ($2 == "packs" && $4 == "loops") k = $2 "/" $3 "/" $4 "/" $5; else if ($2 == "packs") k = $2 "/" $3 "/" $4; else k = $2;
      n[k]++ } END { for (k in n) printf "  %-36s %6d\n", k, n[k] }' | sort
  echo
  echo "verify the download:   shasum -a 256 $(basename "$ZIP")     (must print the sha256 above)"
  echo "install:               see CARD_IMPORT.md next to this file (unzip, copy the lp folder to the card root)"
  echo
  if [ "$FREE" = 1 ]; then
    echo "licence: lp/LICENSES.md inside the zip. Everything in this bundle may be shared: GEN files are generated by this"
    echo "project's tools (tools/make_clips.py), LDZ files are MIT (ldrolez/free-midi-chords, notice in LICENSES.md), the"
    echo "Starter drum kit is synthesised (tools/make_synth_kit.py). No vocals: render your own with tools/make_rap.py on a Mac"
    echo "(docs/RAP_BANK.md, docs/SHARING.md); the loop folders are empty until you make loops of your own (docs/LOOPS.md)."
  else
    echo "licence: lp/LICENSES.md inside the zip. Material derived from commercial packs (TR, SH, CHP tokens, the"
    echo "library import, the loops) and the Apple-voice RAP bank is for the owner's own card (docs/RESEARCH_LIBRARY_EXPANSION.md §6);"
    echo "LDZ files are MIT (ldrolez/free-midi-chords), GEN files are generated by this project's tools."
  fi
} > "$MANIFEST"
cp docs/CARD_IMPORT.md "$OUT/CARD_IMPORT.md" 2>/dev/null || true
cat "$MANIFEST"
