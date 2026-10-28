#!/usr/bin/env python3
"""Pitch-shift the card's sung vocal phrases into neighbouring keys (research docs/RESEARCH_LIBRARY_EXPANSION.md §3
option B, phase V1): every keyed, non-TTS vocal file under <card>/lp/packs/<Genre>/vocal/<Root>min/ is shifted by
±1, ±2 and ±3 semitones with Rubber Band 4 (R3 engine, formant preservation: `rubberband -3 -F -p <n>`), which keeps
the tempo and the singer's timbre, and the copy lands in the key folder the shift reaches:

  <out>/lp/packs/<Genre>/vocal/<TargetKey>/<TargetKey>__<bpm>bpm__<orig>-SH<+n>.wav

  TargetKey  the source key's root moved by n semitones, as a minor key folder ('A#min' + 2 -> 'Cmin')
  <bpm>bpm   the source's own BPM token, unchanged: the firmware varispeeds by it (SPEC §9.3) and a pitch shift does
             not change the tempo
  <orig>     the source name without its key label (e.g. 'GHUAC__Vocal__Megan_Adlib_10_Key_Wet', or for a staged shot
             '91UKG__1bars__91V_UKG_Vocal_Chop_Boys' so its bars token is kept)
  -SH<+n>    the derived-content token (SH+2, SH-3): card-only material that can be pruned by name later

Sources: the GHUAC phrases (and any other keyed sung file: not RAP, folder '<Root>min') of the card, plus, with
--also, further roots in the same layout (the staging mirror's imported one-shots). Files whose name carries a
'<n>bars' token keep EXACTLY that many bars (the shift preserves the length; the result is fitted to the sample);
other files are silence-trimmed again (-54 dBFS) after the shift. Peak -1 dBFS, 1 ms attack, 12 ms fade-out,
22050 Hz mono 16-bit, as tools/import_library.py's vocal path.

Verification (printed): every output's format and name, and for --pitch-check N files per shift the measured
median f0 of the source vs the output (YIN, tools/make_rap.py) against the intended semitones.

Usage:
  python3 -I tools/shift_vocals.py --card sdcard --out sdcard/lp_stage/vocals [--also sdcard/lp_stage/vocals]
        [--genre House] [--shifts -3,-2,-1,1,2,3] [--only-keys Amin,G#min] [--dry-run] [--jobs N] [--pitch-check 4]
Stdlib + rubberband (brew install rubberband) + ffmpeg; run with -I.
"""
import argparse
import concurrent.futures as cf
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from array import array
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_loops as ml                     # noqa: E402
import make_rap as mr                       # noqa: E402

RATE = 22050
PC = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}
ROOT_OF = {v: k for k, v in PC.items()}
MINOR_RE = re.compile(r"^(?P<root>[A-G]#?)min$")
RAP_RE = re.compile(r"__RAP[MFR]__")
SH_RE = re.compile(r"-SH[+-]\d\.wav$")
BARS_RE = re.compile(r"(?<![A-Za-z0-9])(\d{1,3})bars?(?![A-Za-z])")


def sources(root, genre, only_keys=None):
    """Keyed sung vocal files of one card root: [(key folder, path)] — minor key folders only, no RAP, no SH copies."""
    vocal = os.path.join(root, "lp", "packs", genre, "vocal")
    out = []
    if not os.path.isdir(vocal):
        return out
    for key in sorted(os.listdir(vocal)):
        if not MINOR_RE.match(key) or (only_keys and key not in only_keys):
            continue
        for dp, dns, fns in os.walk(os.path.join(vocal, key)):
            for f in sorted(fns):
                if not f.lower().endswith(".wav") or f.startswith("."):
                    continue
                if RAP_RE.search(f) or SH_RE.search(f) or not f.startswith(key + "__"):
                    continue
                out.append((key, os.path.join(dp, f)))
    return out


def target_key(key, n):
    root = MINOR_RE.match(key).group("root")
    return ROOT_OF[(PC[root] + n) % 12] + "min"


def out_name(key, src_name, n):
    tkey = target_key(key, n)
    rest = src_name[len(key) + 2: -4]                        # drop '<Key>__' and '.wav'
    name = "%s__%s-SH%+d.wav" % (tkey, rest, n)
    if len(name.encode("utf-8")) > ml.MAX_NAME:
        cut = ml.MAX_NAME - len(name.encode("utf-8"))
        rest = rest[:cut].rstrip("_-")
        name = "%s__%s-SH%+d.wav" % (tkey, rest, n)
    return tkey, name


def bars_of(name):
    m = BARS_RE.search(name)
    return int(m.group(1)) if m else 0


def render(job, tmp):
    """Worker: rubberband pitch shift + post-processing. job = (src, dst, n, bars)."""
    src, dst, n, bars = job
    if os.path.exists(dst):
        return dst, "cached"
    raw = os.path.join(tmp, "%08x_%+d.wav" % (hash(src) & 0xffffffff, n))
    try:
        subprocess.run(["rubberband", "-q", "-3", "-F", "-p", str(n), src, raw], capture_output=True, check=True)
        a = mr.read_wav(raw) if ml.wav_info(raw) and ml.wav_info(raw)[1] == RATE else mr.ffmpeg_pcm(raw)
    except subprocess.CalledProcessError as e:
        return dst, "FAILED " + (e.stderr or b"").decode("utf-8", "replace").strip().splitlines()[-1:][0] if e.stderr else "FAILED rubberband"
    finally:
        try:
            os.remove(raw)
        except OSError:
            pass
    if bars:
        want = bars * mr.BAR
        if len(a) > want:
            a = a[:want]
        gain = 10 ** (mr.PEAK_DB / 20) / mr.peak_of(a)
        mr.apply_gain_fades(a, gain, fade_in_ms=1, fade_out_ms=12)
        if len(a) < want:
            a = a + array("h", bytes(2 * (want - len(a))))
    else:
        a = mr.trim(a)
        if len(a) < RATE * 0.02:
            return dst, "FAILED silent after shift"
        gain = 10 ** (mr.PEAK_DB / 20) / mr.peak_of(a)
        mr.apply_gain_fades(a, gain, fade_in_ms=1, fade_out_ms=12)
    mr.write_wav(dst, a)
    return dst, "ok"


def median_midi(path, f0_range=(70, 1000)):
    tr = mr.pitch_track(mr.read_wav(path), *f0_range, thr=0.25, min_db=-40.0)
    if len(tr) < 4:
        return None
    mids = sorted(mr.midi_of(f) for _, f in tr)
    return mids[len(mids) // 2]


def pitch_pair(args):
    src, dst, n = args
    a, b = median_midi(src), median_midi(dst)
    return (src, dst, n, (b - a) if (a is not None and b is not None) else None)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--card", required=True, help="card root with the sung sources (holds lp/)")
    ap.add_argument("--out", required=True, help="staging card root for the shifted copies")
    ap.add_argument("--also", action="append", default=[], help="further card roots whose keyed sung files are shifted too")
    ap.add_argument("--genre", default="House")
    ap.add_argument("--shifts", default="-3,-2,-1,1,2,3")
    ap.add_argument("--only-keys", default=None, help="comma list of TARGET key folders to fill (default: all)")
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4)))
    ap.add_argument("--pitch-check", type=int, default=4, help="files per shift to measure (0 = skip)")
    ap.add_argument("--seed", default="sh")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    shifts = [int(x) for x in args.shifts.split(",") if x.strip()]
    for n in shifts:
        if n == 0 or abs(n) > 3:
            sys.exit("shifts must be within ±1..3 semitones (research §3 option B)")
    only = set(args.only_keys.split(",")) if args.only_keys else None
    srcs = sources(os.path.abspath(args.card), args.genre)
    for extra in args.also:
        srcs += sources(os.path.abspath(extra), args.genre)
    vocal_out = os.path.join(os.path.abspath(args.out), "lp", "packs", args.genre, "vocal")
    jobs, names = [], set()
    per_target = defaultdict(Counter)
    for key, path in srcs:
        for n in shifts:
            tkey, name = out_name(key, os.path.basename(path), n)
            if only and tkey not in only:
                continue
            if name in names:
                print("  duplicate name skipped: " + name)
                continue
            names.add(name)
            dst = os.path.join(vocal_out, tkey, name)
            jobs.append((path, dst, n, bars_of(name)))
            per_target[tkey][key] += 1
    by_pack = Counter(os.path.basename(p).split("__")[1] if "bpm" not in os.path.basename(p).split("__")[1] else os.path.basename(p).split("__")[2] for _, p in srcs)
    print("%d sources (%s) x %d shifts -> %d files" % (len(srcs), ", ".join("%s %d" % kv for kv in by_pack.most_common()), len(shifts), len(jobs)))
    for tkey in sorted(per_target, key=lambda k: PC[MINOR_RE.match(k).group("root")]):
        c = per_target[tkey]
        print("  %-6s %4d  <- %s" % (tkey, sum(c.values()), "  ".join("%s:%d" % kv for kv in sorted(c.items()))))
    if args.dry_run:
        for src, dst, n, bars in jobs[:12]:
            print("  %s  <- %s (%+d)" % (os.path.relpath(dst, vocal_out), os.path.basename(src), n))
        print("dry run: nothing written")
        return 0
    t0 = time.time()
    tmp = tempfile.mkdtemp(prefix="shift_vocals_")
    status = Counter()
    failures = []
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for i, (dst, st) in enumerate(ex.map(lambda j: render(j, tmp), jobs)):
            status[st.split()[0]] += 1
            if st.startswith("FAILED"):
                failures.append("%s: %s" % (os.path.basename(dst), st))
            if (i + 1) % 300 == 0:
                print("  %d/%d (%.0f s)" % (i + 1, len(jobs), time.time() - t0), flush=True)
    try:
        os.rmdir(tmp)
    except OSError:
        pass
    for f in failures[:20]:
        print("  " + f)
    print("render: %s in %.1f s" % (", ".join("%s %d" % kv for kv in status.items()), time.time() - t0))

    # verification: format, name, folder, length of bars-token files; MB per target folder
    problems = 0
    per_key = defaultdict(lambda: [0, 0])
    for src, dst, n, bars in jobs:
        if not os.path.exists(dst):
            continue
        name = os.path.basename(dst)
        info = ml.wav_info(dst)
        folder = os.path.basename(os.path.dirname(dst))
        if not info or info[:3] != (1, RATE, 16):
            problems += 1
            print("  PROBLEM format: " + name)
        if not name.startswith(folder + "__") or len(name.encode()) > ml.MAX_NAME or ml.bpm_token(name) == 0:
            problems += 1
            print("  PROBLEM name/folder/bpm: %s in %s" % (name, folder))
        if bars and info and info[3] != bars * mr.BAR:
            problems += 1
            print("  PROBLEM length %d != %d bars: %s" % (info[3], bars, name))
        per_key[folder][0] += 1
        per_key[folder][1] += os.path.getsize(dst)
    tn = tb = 0
    for k in sorted(per_key, key=lambda k: PC[MINOR_RE.match(k).group("root")]):
        n_, b_ = per_key[k]
        tn += n_
        tb += b_
        print("  %-6s %4d files %7.2f MB" % (k, n_, b_ / 1048576))
    print("verify: %d files, %.1f MB, %d problems" % (tn, tb / 1048576, problems))

    if args.pitch_check:
        rng = random.Random(args.seed)
        picks = []
        for n in shifts:
            cand = [j for j in jobs if j[2] == n and os.path.exists(j[1]) and "Dry" in os.path.basename(j[0])]
            cand = cand or [j for j in jobs if j[2] == n and os.path.exists(j[1])]
            picks += [(j[0], j[1], j[2]) for j in rng.sample(cand, min(args.pitch_check, len(cand)))]
        t1 = time.time()
        devs = defaultdict(list)
        with cf.ProcessPoolExecutor(max_workers=args.jobs) as ex:
            for src, dst, n, d in ex.map(pitch_pair, picks):
                if d is not None:
                    devs[n].append(d - n)
                    print("  pitch %+d: measured %+.2f st  %s" % (n, d, os.path.basename(dst)))
                else:
                    print("  pitch %+d: unvoiced / too short  %s" % (n, os.path.basename(dst)))
        alld = sorted(abs(x) for ds in devs.values() for x in ds)
        if alld:
            print("pitch check: %d files, |measured - intended| median %.2f st, p90 %.2f st, max %.2f st (%.0f s)"
                  % (len(alld), alld[len(alld) // 2], alld[int(len(alld) * 0.9)], alld[-1], time.time() - t1))
    return 1 if problems or failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
