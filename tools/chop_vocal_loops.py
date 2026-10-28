#!/usr/bin/env python3
"""Chop the owned vocal loops into 2- and 4-bar sung phrases at 125 BPM for the Vocal tracks (research
docs/RESEARCH_LIBRARY_EXPANSION.md §3 option D, phase V2).

Every vocal loop of the REAPER loop library whose tempo is within --tolerance of the target (115..135 BPM at 8 %)
is stretched ONCE to exactly 125 BPM with Rubber Band (R3, pitch preserved; tools/make_loops.py's stretch) and cut
on its bar grid into whole N-bar segments, each written as an ordinary vocal phrase (NOT loop content: no LOOP
token, so Library::hasLoopToken accepts it, SPEC §6):

  <out>/lp/packs/<Genre>/vocal/<Key>/<Key>__125bpm__CHP__<n>bars__<slug>_<S>.wav

  Key     the loop's REAPER key label folded as make_loops.key_folder ('Bbmin' -> 'A#min', 'Dmaj7' -> 'D'); NA loops
          are skipped by default (--na-limit) because a keyless tonal phrase in NA would be offered in every key
  CHP     the derived-content token (card-only material, prunable by name)
  <n>bars 4 or 2: the segment is EXACTLY n x 42336 frames (1.92 s per bar), 10 ms fade-out, a 5 ms fade-in when
          the segment does not start at the loop's first bar, peak -1 dBFS, 22050 Hz mono 16-bit
  slug    make_loops.make_slug of the source (pack code + name words, no key / bpm / category tokens; a 'Loop(s)'
          word becomes 'Lp'), _S = the segment's position letter (A = bars 0..n, B = the next n bars, ...)

Per key folder the plan aims at --target files and never exceeds a cap: --max-total (600, the 768-per-folder
budget of SPEC §6 minus headroom) minus what the key folder already holds in every --existing card root (the card
and the staging mirror, counted recursively; the tool's own CHP files are not counted and are removed before a
run unless --keep, so a re-run reproduces the same plan). Long loops first give 4-bar segments; when a key stays below the
target, loops are switched to 2-bar segments (fewest-segments first); when it exceeds the cap, the segments per
loop are limited (--max-per-loop, then lower), picked evenly spaced over the loop, so one 100-bar acapella
cannot fill a folder. Segments whose level is below --min-peak-db (breaths, gaps) are dropped.

Usage:
  python3 -I tools/chop_vocal_loops.py --src ~/Documents/cardputer/House/loops --out sdcard/lp_stage/vocals \
        [--existing sdcard --existing sdcard/lp_stage/vocals] [--genre House] [--bpm 125] [--tolerance 0.08]
        [--target 200] [--max-total 600] [--reserve 150] [--max-per-loop 6] [--na-limit 0] [--min-peak-db -24]
        [--dry-run] [--jobs N] [--manifest FILE]
Stdlib + ffmpeg + rubberband; run with -I. Verifies every output (format, exact length, name, folder, peak, no loop token).
"""
import argparse
import math
import os
import re
import subprocess
import sys
import tempfile
import time
import wave
from array import array
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_loops as ml                     # noqa: E402

RATE = 22050
TOKEN = "CHP"
LOOP_WORD = re.compile(r"(?i)(?<![A-Za-z0-9])loops?(?![A-Za-z0-9])")
OUT_RE = re.compile(r"^(?P<key>[A-G]#?(?:min)?|NA)__(?P<bpm>\d{2,3})bpm__CHP__(?P<bars>\d{1,2})bars__(?P<slug>[A-Za-z0-9_-]+)\.wav$")
LOOP_TOKEN_RE = re.compile(r"(?i)(?<![A-Za-z0-9])loops?(?![A-Za-z0-9])")


def seg_letter(i):
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


def count_existing(roots, genre):
    """key folder -> wav files already there (recursively) over every card root."""
    counts = Counter()
    for root in roots:
        vocal = os.path.join(os.path.abspath(root), "lp", "packs", genre, "vocal")
        if not os.path.isdir(vocal):
            continue
        for key in os.listdir(vocal):
            kp = os.path.join(vocal, key)
            if key.startswith(".") or not os.path.isdir(kp):
                continue
            for dp, dns, fns in os.walk(kp):
                counts[key] += sum(1 for f in fns if f.lower().endswith(".wav") and not f.startswith(".") and "__%s__" % TOKEN not in f)
    return counts


def remove_existing(vocal_root):
    n = 0
    for dp, dns, fns in os.walk(vocal_root):
        for f in fns:
            if "__%s__" % TOKEN in f and f.endswith(".wav"):
                os.remove(os.path.join(dp, f))
                n += 1
    return n


class Plan:
    """Segments of one loop: seg_bars (4 or 2), usable bars, the picked segment indices."""
    __slots__ = ("lp", "usable", "seg_bars", "picks", "dsts")

    def __init__(self, lp, usable):
        self.lp, self.usable, self.seg_bars, self.picks, self.dsts = lp, usable, 4 if usable >= 4 else 2, [], []

    def total(self):
        return self.usable // self.seg_bars

    def pick(self, limit):
        n = self.total()
        if n <= limit:
            self.picks = list(range(n))
        else:                                                   # evenly spaced over the loop, first segment included
            self.picks = sorted({int(round(i * (n - 1) / (limit - 1))) for i in range(limit)}) if limit > 1 else [0]


def plan_key(plans, target, cap, max_per_loop):
    """Choose seg_bars and picks for the loops of one key folder. Returns (files, note)."""
    for p in plans:
        p.seg_bars = 4 if p.usable >= 4 else 2
    def count(limit):
        for p in plans:
            p.pick(limit)
        return sum(len(p.picks) for p in plans)
    limit = max_per_loop
    n = count(limit)
    switched = 0
    if n < target:                                              # fewest segments first: 2-bar doubles them
        for p in sorted(plans, key=lambda p: (p.total(), p.lp.rel)):
            if p.seg_bars == 4:
                p.seg_bars = 2
                switched += 1
                n = count(limit)
                if n >= target:
                    break
    while n > cap and limit > 1:
        limit -= 1
        n = count(limit)
    dropped = 0
    if n > cap:                                                 # still too many single segments: drop loops from the end
        for p in sorted(plans, key=lambda p: p.lp.rel, reverse=True):
            if n <= cap:
                break
            if p.picks:
                dropped += 1
                n -= len(p.picks)
                p.picks = []
    return n, "%d loops, %d switched to 2 bars, <= %d segments per loop%s" % (
        len(plans), switched, limit, ", %d loops dropped" % dropped if dropped else "")


def stretch(lp, target, tmp, engine):
    """Whole loop as 22050 Hz mono float32 at the target tempo (rubberband once per loop)."""
    uid = "%08x" % (hash(lp.rel) & 0xffffffff)
    if lp.bpm == target:
        return ml.decode_f32(["ffmpeg", "-v", "error", "-nostdin", "-i", lp.src, "-ac", "1", "-ar", str(RATE),
                              "-f", "f32le", "-acodec", "pcm_f32le", "-"])
    mono = os.path.join(tmp, uid + ".mono.wav")
    out = os.path.join(tmp, uid + ".rb.wav")
    try:
        ml.run(["ffmpeg", "-v", "error", "-nostdin", "-y", "-i", lp.src, "-ac", "1", "-acodec", "pcm_f32le", mono])
        flags = ["-3"] if engine == "r3" else ["-2", "-c", "6"]
        ml.run(["rubberband", "-q"] + flags + ["--tempo", "%g:%g" % (lp.bpm, target), mono, out])
        return ml.decode_f32(["ffmpeg", "-v", "error", "-nostdin", "-i", out, "-ac", "1", "-ar", str(RATE),
                              "-f", "f32le", "-acodec", "pcm_f32le", "-"])
    finally:
        for p in (mono, out):
            try:
                os.remove(p)
            except OSError:
                pass


def render_plan(p, target, bar_frames, tmp, engine, min_peak_db):
    """Worker: stretch the loop, write its picked segments. Returns [(dst, status, detail)]."""
    results = []
    try:
        pcm = stretch(p.lp, target, tmp, engine)
    except subprocess.CalledProcessError as e:
        msg = (e.stderr or b"").decode("utf-8", "replace").strip().splitlines()[-1:] or [str(e)]
        return [(d, "failed", msg[0]) for d, _, _ in p.dsts]
    seg_n = p.seg_bars * bar_frames
    fade_out = RATE * ml.FADE_MS // 1000
    fade_in = RATE * 5 // 1000
    for dst, idx, _ in p.dsts:
        start = idx * seg_n
        seg = pcm[start: start + seg_n]
        if len(seg) < seg_n * 0.98:                             # the stretch came out a little short of the grid
            results.append((dst, "failed", "segment %s beyond the stretched content (%d of %d frames)" % (seg_letter(idx), len(seg), seg_n)))
            continue
        peak = max(max(seg), -min(seg)) if len(seg) else 0.0
        if peak < 10 ** (min_peak_db / 20):
            results.append((dst, "skipped", "peak %.1f dBFS below %.0f" % (20 * math.log10(max(peak, 1e-9)), min_peak_db)))
            continue
        n = len(seg)
        if idx > 0:
            for i in range(min(fade_in, n)):
                seg[i] *= i / fade_in
        for i in range(max(0, n - fade_out), n):
            seg[i] *= (n - i) / fade_out
        peak = max(max(seg), -min(seg))
        gain = 10 ** (ml.PEAK_DB / 20) / peak                   # after the fades: the file peak is exactly -1 dBFS
        out = array("h", bytes(2 * seg_n))
        for i in range(n):
            v = int(round(seg[i] * gain * 32767))
            out[i] = -32768 if v < -32768 else 32767 if v > 32767 else v
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with wave.open(dst, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(out.tobytes())
        results.append((dst, "ok", "%d bars, segment %s of %d" % (p.seg_bars, seg_letter(idx), p.total())))
    return results


def verify(vocal_root, target, bar_frames):
    problems, per_key = [], defaultdict(lambda: [0, 0, Counter()])
    for dp, dns, fns in os.walk(vocal_root):
        for f in sorted(fns):
            if "__%s__" % TOKEN not in f or not f.endswith(".wav"):
                continue
            path = os.path.join(dp, f)
            folder = os.path.relpath(dp, vocal_root).split(os.sep)[0]
            m = OUT_RE.match(f)
            if not m:
                problems.append("%s: name does not parse" % f)
                continue
            if m.group("key") != folder:
                problems.append("%s: key %s in folder %s" % (f, m.group("key"), folder))
            if ml.bpm_token(f) != target:
                problems.append("%s: firmware reads bpm %d" % (f, ml.bpm_token(f)))
            if LOOP_TOKEN_RE.search(f):
                problems.append("%s: carries a loop token" % f)
            if len(f.encode()) > ml.MAX_NAME:
                problems.append("%s: %d bytes" % (f, len(f.encode())))
            info = ml.wav_info(path)
            if not info or info[:3] != (1, RATE, 16):
                problems.append("%s: format %s" % (f, info))
                continue
            if info[3] != int(m.group("bars")) * bar_frames:
                problems.append("%s: %d frames != %d bars" % (f, info[3], int(m.group("bars"))))
            with wave.open(path, "rb") as w:
                a = array("h")
                a.frombytes(w.readframes(w.getnframes()))
            peak = max(max(a), -min(a)) / 32768.0 if len(a) else 0
            pdb = 20 * math.log10(peak) if peak > 0 else -120
            if abs(pdb - ml.PEAK_DB) > 1.0:
                problems.append("%s: peak %.2f dBFS" % (f, pdb))
            pk = per_key[folder]
            pk[0] += 1
            pk[1] += os.path.getsize(path)
            pk[2][int(m.group("bars"))] += 1
    return problems, per_key


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True, help="loop library root (category folders beneath; only 'vocal' is used)")
    ap.add_argument("--out", required=True, help="staging card root (holds lp/)")
    ap.add_argument("--existing", action="append", default=[], help="card roots whose vocal/<Key> file counts fill the cap")
    ap.add_argument("--genre", default="House")
    ap.add_argument("--bpm", type=int, default=125)
    ap.add_argument("--tolerance", type=float, default=0.08)
    ap.add_argument("--target", type=int, default=200, help="files per key folder to aim at")
    ap.add_argument("--max-total", type=int, default=600, help="files a key folder may hold in all (existing + new)")
    ap.add_argument("--max-per-loop", type=int, default=6)
    ap.add_argument("--reserve", type=int, default=0, help="files per key folder still to come from another tool (counted against the cap)")
    ap.add_argument("--na-limit", type=int, default=0, help="files for the NA folder (default 0: keyless loops are skipped)")
    ap.add_argument("--min-peak-db", type=float, default=-24.0, help="segments quieter than this (before normalising) are dropped")
    ap.add_argument("--engine", choices=("r3", "r2"), default="r3")
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4)))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--keep", action="store_true", help="do not delete the earlier CHP files of the output first")
    ap.add_argument("--manifest")
    args = ap.parse_args(argv)
    target = args.bpm
    bar_frames = int(round(240.0 / target * RATE))
    t0 = time.time()
    loops, skipped = ml.scan(os.path.abspath(args.src), lambda bpm: 240.0 / bpm)
    lo, hi = target * (1 - args.tolerance), target * (1 + args.tolerance)
    vocal = [lp for lp in loops if lp.category == "vocal"]
    cands = [lp for lp in vocal if lp.bpm and lo <= lp.bpm <= hi]
    print("scan: %d vocal loops, %d within %.0f..%.0f BPM (%.1f s)" % (len(vocal), len(cands), lo, hi, time.time() - t0))
    existing = count_existing(args.existing, args.genre)
    vocal_root = os.path.join(os.path.abspath(args.out), "lp", "packs", args.genre, "vocal")

    by_key = defaultdict(list)
    too_short = 0
    for lp in cands:
        usable = int(math.floor(lp.bars_exact + 0.06))
        if usable < 2:
            too_short += 1
            continue
        by_key[lp.key].append(Plan(lp, usable))
    print("%d loops shorter than 2 bars skipped" % too_short)
    plans_all = []
    print("plan per key folder (target %d, total cap %d incl. existing):" % (args.target, args.max_total))
    for key in sorted(by_key, key=lambda k: (k == "NA", k)):
        plans = sorted(by_key[key], key=lambda p: p.lp.rel)
        cap = max(0, args.max_total - existing.get(key, 0) - (args.reserve if key.endswith("min") else 0))
        tgt = args.target
        if key == "NA":
            cap = min(cap, args.na_limit)
            tgt = min(tgt, args.na_limit)
        if not key.endswith("min") and key != "NA":            # a major folder: a modest share, it is reached via the relative minor
            tgt = min(tgt, 40)
        n, note = plan_key(plans, tgt, cap, args.max_per_loop)
        print("  %-6s existing %3d%s  cap %3d  -> %3d files  (%s)" % (key, existing.get(key, 0), (" + %d reserved" % args.reserve) if (args.reserve and key.endswith("min")) else "", cap, n, note))
        plans_all += [p for p in plans if p.picks]
    # names
    used = defaultdict(set)
    for p in sorted(plans_all, key=lambda p: p.lp.rel):
        slug, hint = ml.make_slug(os.path.basename(p.lp.rel)[:-4], p.lp.key_kind != "none")
        slug = LOOP_WORD.sub("Lp", slug)
        if hint:
            slug += "_" + hint
        folder = os.path.join(vocal_root, p.lp.key)
        p.dsts = []
        for idx in p.picks:
            name = ml.output_name(p.lp.key, target, p.seg_bars, slug + "_" + seg_letter(idx), None, "%s#%d" % (p.lp.rel, idx), used[folder], token=TOKEN)
            p.dsts.append((os.path.join(folder, name), idx, p.seg_bars))
    total = sum(len(p.dsts) for p in plans_all)
    print("  %d files planned from %d loops, %.1f MB" % (total, len(plans_all), sum(len(p.dsts) * p.seg_bars for p in plans_all) * bar_frames * 2 / 1048576))
    if args.dry_run:
        for p in plans_all[:10]:
            for dst, idx, bars in p.dsts[:3]:
                print("  %s  <- %s (%g bpm, %.1f bars)" % (os.path.relpath(dst, vocal_root), p.lp.rel, p.lp.bpm, p.lp.bars_exact))
        print("dry run: nothing written")
        return 0
    if not ml.shutil.which("rubberband") or not ml.shutil.which("ffmpeg"):
        sys.exit("rubberband and ffmpeg are required")
    if not args.keep and os.path.isdir(vocal_root):
        n_old = remove_existing(vocal_root)
        if n_old:
            print("removed %d earlier CHP files under %s" % (n_old, vocal_root))
    tmp = tempfile.mkdtemp(prefix="chop_vocal_")
    t1 = time.time()
    done = Counter()
    rows = []
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for res in ex.map(lambda p: render_plan(p, target, bar_frames, tmp, args.engine, args.min_peak_db), plans_all):
            for dst, st, detail in res:
                done[st] += 1
                rows.append((dst, st, detail))
                if st != "ok":
                    print("  %s %s: %s" % (st.upper(), os.path.basename(dst), detail))
            if done["ok"] and done["ok"] % 300 < len(res):
                print("  %d written (%.0f s)" % (done["ok"], time.time() - t1), flush=True)
    ml.shutil.rmtree(tmp, ignore_errors=True)
    print("render: %s in %.1f s" % (", ".join("%s %d" % kv for kv in sorted(done.items())), time.time() - t1))
    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as f:
            f.write("output\tstatus\tdetail\n")
            for dst, st, detail in sorted(rows):
                f.write("%s\t%s\t%s\n" % (os.path.relpath(dst, vocal_root), st, detail))
    problems, per_key = verify(vocal_root, target, bar_frames)
    for pr in problems:
        print("PROBLEM: " + pr)
    tn = tb = 0
    for key in sorted(per_key):
        n, b, bars = per_key[key]
        tn += n
        tb += b
        print("  %-6s %4d files %7.2f MB  bars: %s" % (key, n, b / 1048576, "  ".join("%d:%d" % kv for kv in sorted(bars.items()))))
    print("verify: %d CHP files, %.1f MB, %d problems (%.0f s total)" % (tn, tb / 1048576, len(problems), time.time() - t0))
    return 0 if not problems and not done["failed"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
