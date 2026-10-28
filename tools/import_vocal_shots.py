#!/usr/bin/env python3
"""Import the owned one-shot vocal phrases that tools/import_library.py skipped (no BPM token in the name).

Research docs/RESEARCH_LIBRARY_EXPANSION.md §3 item E: 202 shots under <pack>/one-shots/vocal carry a key label
(or NA) but no tempo, so the firmware could not varispeed them (SPEC §9.3) and the import left them out. This
tool gives each one the card's 125 BPM token and the window token it needs, and pads it to whole bars:

  <out>/lp/packs/<Genre>/vocal/<Key|NA>/<Key|NA>__125bpm__<PACK>__<n>bars__<slug>.wav

  Key    the REAPER key label folded as import_library.key_folder: minor 'Amin', major bare root 'C#', 'Eb' -> 'D#';
         'NA' or no label -> NA
  PACK   the pack code token of the source name (91UKG, 91HHV, ASV, BPV, OSH, ...) so the file stays traceable
  n      whole bars at 125 BPM the padded file spans (1 when the trimmed shot fits a bar, else 2, at most 4);
         the file is EXACTLY n x 42336 frames, so a player that honours the bars token can cycle it
  slug   the rest of the source name, [A-Za-z0-9_-] only, a 'Loop(s)' word spelled 'Lp' (SPEC §6: the vocal picker
         rejects names with a loop token), cut at a word boundary so the whole name stays <= 95 bytes

Audio as import_library.py's vocal path: ffmpeg -> 22050 Hz mono 16-bit, silence-trimmed at -54 dBFS, 1 ms attack,
12 ms fade-out, peak -1 dBFS, then zero-padded to the bar. Stdlib + ffmpeg; run with -I.

Usage:
  python3 -I tools/import_vocal_shots.py --src ~/Documents/cardputer/House/one-shots/vocal \
        --out sdcard/lp_stage/vocals [--genre House] [--bpm 125] [--max-bars 4] [--dry-run] [--jobs N]
"""
import argparse
import os
import re
import sys
import time
from array import array
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_loops as ml                     # noqa: E402  (bpm_token, MAX_NAME)
import make_rap as mr                       # noqa: E402  (ffmpeg_pcm, trim, write_wav, peak_of, db)

RATE = 22050
KEY_RE = re.compile(r"^([A-G](?:#|b)?)(min|maj)?$", re.IGNORECASE)
FLAT = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#"}
LOOP_WORD = re.compile(r"(?i)(?<![A-Za-z0-9])loops?(?![A-Za-z0-9])")


def key_folder(label):
    """import_library.key_folder on the first '__' token: folder name or None when the token is not a key label."""
    if label.upper() == "NA":
        return "NA"
    m = KEY_RE.match(label)
    if not m:
        return None
    root = m.group(1)
    root = root[0].upper() + root[1:]
    root = FLAT.get(root, root)
    return root + ("min" if (m.group(2) or "").lower() == "min" else "")


def clean_slug(text):
    text = LOOP_WORD.sub("Lp", text)
    text = re.sub(r"[^A-Za-z0-9_-]+", "_", text)
    return re.sub(r"_+", "_", text).strip("_")


def plan(src, bpm, max_bars):
    items, skipped = [], Counter()
    for f in sorted(os.listdir(src)):
        if f.startswith(".") or not f.lower().endswith(".wav"):
            continue
        stem = f[:-4]
        if ml.bpm_token(stem):
            skipped["has a bpm token (import_library.py took it)"] += 1
            continue
        toks = stem.split("__")
        key = key_folder(toks[0])
        rest = toks[1:] if key is not None else toks
        key = key or "NA"
        pack = rest[0] if rest else "SHOT"
        words = [t for t in rest[1:] if t.lower() != "vocal"]
        slug = clean_slug("_".join(words)) or "shot"
        items.append(dict(src=os.path.join(src, f), rel=f, key=key, pack=clean_slug(pack) or "SHOT", slug=slug))
    return items, skipped


def name_for(key, bpm, bars, pack, slug, used):
    prefix = "%s__%dbpm__%s__%dbars__" % (key, bpm, pack, bars)
    room = ml.MAX_NAME - len(prefix) - 4
    s = slug
    if len(s) > room:
        cut = s[:room]
        if "_" in cut[room // 2:]:
            cut = cut[: cut.rfind("_")]
        s = cut.rstrip("_-")
    name = prefix + s + ".wav"
    n = 2
    while name in used:
        extra = "_%d" % n
        name = prefix + s[: room - len(extra)].rstrip("_-") + extra + ".wav"
        n += 1
    used.add(name)
    return name


def render(item, bpm, bar_frames, max_bars):
    a = mr.ffmpeg_pcm(item["src"])
    a = mr.trim(a)                                             # -54 dBFS, 2 ms pre-roll (import_library.py)
    if len(a) < RATE * 0.01:
        return None, "silent"
    limit = max_bars * bar_frames
    truncated = len(a) > limit
    if truncated:
        a = a[:limit]
    gain = 10 ** (mr.PEAK_DB / 20) / mr.peak_of(a)
    mr.apply_gain_fades(a, gain, fade_in_ms=1, fade_out_ms=20 if truncated else 12)
    bars = 1
    while bars * bar_frames < len(a):
        bars *= 2
    out = array("h", bytes(2 * bars * bar_frames))
    out[: len(a)] = a
    return out, dict(bars=bars, content_s=len(a) / RATE, truncated=truncated)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True)
    ap.add_argument("--out", required=True, help="card root (holds lp/), e.g. sdcard/lp_stage/vocals")
    ap.add_argument("--genre", default="House")
    ap.add_argument("--bpm", type=int, default=125)
    ap.add_argument("--max-bars", type=int, default=4)
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4)))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    bar_frames = int(round(240.0 / args.bpm * RATE))
    items, skipped = plan(os.path.abspath(args.src), args.bpm, args.max_bars)
    vocal_root = os.path.join(os.path.abspath(args.out), "lp", "packs", args.genre, "vocal")
    print("%d shots without a bpm token; skipped: %s" % (len(items), ", ".join("%s %d" % kv for kv in skipped.items()) or "none"))
    if args.dry_run:
        for it in items:
            print("  %-6s %-6s %s" % (it["key"], it["pack"], it["slug"]))
        return 0
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(lambda it: render(it, args.bpm, bar_frames, args.max_bars), items))
    used = defaultdict(set)
    per_key = defaultdict(lambda: [0, 0, Counter()])
    failed = []
    for it, (pcm, info) in zip(items, results):
        if pcm is None:
            failed.append("%s: %s" % (it["rel"], info))
            continue
        folder = os.path.join(vocal_root, it["key"])
        name = name_for(it["key"], args.bpm, info["bars"], it["pack"], it["slug"], used[folder])
        path = os.path.join(folder, name)
        mr.write_wav(path, pcm)
        pk = per_key[it["key"]]
        pk[0] += 1
        pk[1] += os.path.getsize(path)
        pk[2][info["bars"]] += 1
        if info["truncated"]:
            failed.append("note: %s cut to %d bars" % (it["rel"], args.max_bars))
    for line in failed:
        print("  " + line)
    tn = tb = 0
    for key in sorted(per_key):
        n, b, bars = per_key[key]
        tn += n
        tb += b
        print("  %-6s %3d files %6.2f MB  bars: %s" % (key, n, b / 1048576, "  ".join("%d:%d" % kv for kv in sorted(bars.items()))))
    print("%d files, %.2f MB into %s (%.1f s)" % (tn, tb / 1048576, vocal_root, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
