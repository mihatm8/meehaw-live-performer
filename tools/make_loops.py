#!/usr/bin/env python3
"""Prepare loop content for the Live Player's loop mode (docs/LOOPS.md).

Picks loops whose tempo is close to the target BPM from a REAPER-named loop library, time-stretches each one
to exactly the target BPM with its pitch preserved, and writes whole-bar 22050 Hz mono 16-bit WAVs into the
card layout:

  lp/packs/<Genre>/loops/<category>/<Key|NA>/<Key|NA>__125bpm__LOOP__<n>bars__<slug>.wav

  category  one of drum percussion bass chord lead pad fx vocal (the source's drum/combo and drum/solo
            are merged into drum; a Full / Kick / Hats stem hint from the name ends the slug as _full/_kick/_hats)
  Key       REAPER key label folded to the card's folders: minor 'Amin', major 'C' (sharps only, 'Bb' -> 'A#'),
            chord-ish labels ('Amaj9sus4' -> 'A', 'Dmin7' -> 'Dmin'); 'NA' or no key label -> 'NA'
  125bpm    the target tempo (the firmware's BPM token, SPEC §9.3); LOOP tags the file as loop content
  <n>bars   whole bars at the target tempo; the file is exactly n x 4 x 60 / BPM seconds long (1.92 s per bar
            at 125 BPM, 42336 frames), padded with silence or trimmed, 10 ms fade at the end, peak -1 dBFS

Usage:
  python3 -I tools/make_loops.py --src ~/Documents/cardputer/House/loops --out sdcard [--genre House]
      [--bpm 125] [--tolerance 0.08] [--budget-mb 120] [--max-bars 8] [--jobs N]
      [--limit drum=60,percussion=40,bass=30,chord=30,lead=20,pad=15,fx=20,vocal=40]
      [--stretcher auto|rubberband|ffmpeg] [--engine r3|r2] [--scan-only | --dry-run | --verify-only]
      [--keep] [--only CAT[,CAT]] [--tmp DIR] [--manifest FILE]

Then shard and index as for every content folder:
  python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard --quiet

Steps: (1) scan the source, parse key and bpm tokens, print a BPM histogram per category; (2) select loops
within --tolerance of the target (125 first, then 124/126, then by distance; 2- and 4-bar loops first, then
1-bar, then 8-bar; loops longer than --max-bars are dropped), diversified round-robin over keys (drum: over
combo/solo and keys), per-category limits, total size within --budget-mb (longest loops dropped first);
--split-bars N instead cuts loops longer than N bars into whole N-bar segments (_A, _B, ...); (3) stretch with rubberband (brew install rubberband; R3 engine) or ffmpeg's atempo when rubberband is missing
or --stretcher ffmpeg; (4) write; (5) verify every output: duration within 5 ms of bars x bar length, name parses,
peak within 1 dB of -1 dBFS. --verify-only runs step 5 on an existing card.

Stdlib only (ffmpeg and rubberband are external commands); run with -I so nothing is imported from the data.
"""
import argparse
import hashlib
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave
from array import array
from collections import Counter, OrderedDict, defaultdict
from concurrent.futures import ThreadPoolExecutor

RATE = 22050
MAX_NAME = 95                      # write_indexes.py MAX_NAME: longer names cannot be resolved by the firmware
PEAK_DB = -1.0
FADE_MS = 10
WAV_HEADER = 44
CATEGORIES = ("drum", "percussion", "bass", "chord", "lead", "pad", "fx", "vocal")
DEFAULT_LIMITS = OrderedDict([("drum", 60), ("percussion", 40), ("bass", 30), ("chord", 30), ("lead", 20),
                              ("pad", 15), ("fx", 20), ("vocal", 40)])
BPM_RE = re.compile(r"(?<![0-9])(\d{2,3})[ _-]*bpm(?![a-z])|bpm[ _-]*(\d{2,3})(?![0-9])", re.IGNORECASE)
KEY_RE = re.compile(r"^([A-G])([#b]?)([A-Za-z0-9]*)$")
FLAT = {"Db": "C#", "Eb": "D#", "Gb": "F#", "Ab": "G#", "Bb": "A#"}
CATEGORY_WORDS = {"drum", "drum_combo", "drum_solo", "bass", "chord", "lead", "pad", "fx", "percussion", "vocal"}
HINTS = ("full", "kick", "hats")
OUT_RE = re.compile(r"^(?P<key>[A-G]#?(?:min)?|NA)__(?P<bpm>\d{2,3})bpm__LOOP__(?P<bars>\d{1,2})bars__(?P<slug>[A-Za-z0-9_-]+)\.wav$")


# ------------------------------------------------------------------------------------------------- name parsing

def key_folder(label):
    """REAPER key contract (import_library.py KEY_RE) folded to a card folder. Returns (folder, kind): kind is
    'key' for '<Root>[min|maj]', 'chord' for a chord label ('Amaj9sus4', 'Dmin7'), 'na' for 'NA', 'none' when the
    token is not a key label at all (a bpm token, a word); the last two land in 'NA'."""
    if label.upper() == "NA":
        return "NA", "na"
    m = KEY_RE.match(label)
    if not m:
        return "NA", "none"
    root, acc, quality = m.groups()
    root = FLAT.get(root + acc, root + acc)
    q = quality.lower()
    if q.startswith("min") or q.startswith("dim") or (q.startswith("m") and not q.startswith("maj")):
        mode = "min"
    elif q == "" or q.startswith("maj") or q[0].isdigit() or q.startswith(("sus", "add", "aug", "dom")):
        mode = ""
    else:
        return "NA", "none"
    return root + mode, ("key" if q in ("", "min", "maj") else "chord")


def bpm_token(name):
    """First BPM token of a name as the firmware's parseBpmToken sees it (60..249), 0 when absent."""
    for m in BPM_RE.finditer(name):
        v = int(m.group(1) or m.group(2))
        if 60 <= v <= 249:
            return v
    return 0


def wav_info(path):
    """(channels, rate, bits, frames) from the RIFF header, or None when it cannot be read."""
    try:
        with open(path, "rb") as f:
            head = f.read(12)
            if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
                return None
            fmt = None
            while True:
                h = f.read(8)
                if len(h) < 8:
                    return None
                cid, size = h[:4], struct.unpack("<I", h[4:])[0]
                if cid == b"fmt ":
                    body = f.read(size + (size & 1))
                    fmt = struct.unpack("<HHIIHH", body[:16])
                elif cid == b"data":
                    if fmt is None or fmt[4] == 0:
                        return None
                    return fmt[1], fmt[2], fmt[5], size // fmt[4]
                else:
                    f.seek(size + (size & 1), 1)
    except OSError:
        return None


def ffprobe_duration(path):
    try:
        out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                             capture_output=True, text=True, check=True).stdout.strip()
        return float(out)
    except (subprocess.CalledProcessError, ValueError, OSError):
        return None


def make_slug(stem, drop_first):
    """Descriptive part of the output name: pack code + name words, without the key label, bpm and category
    tokens; a source dedupe hash ('__65e35803') keeps its first four hex digits. Returns (slug, hint)."""
    toks = stem.split("__")
    if drop_first and len(toks) > 1:
        toks = toks[1:]
    out = []
    for t in toks:
        tl = t.lower()
        if bpm_token(t) and len(t) <= 8:
            continue
        if tl in CATEGORY_WORDS:
            if tl.startswith("drum_"):
                out.append(tl.split("_")[1].capitalize())        # Combo / Solo
            continue
        if re.fullmatch(r"[0-9a-f]{8}", tl):
            out.append(tl[:4])
            continue
        out.append(t)
    slug = re.sub(r"[^A-Za-z0-9_-]+", "_", "_".join(out))
    slug = re.sub(r"_+", "_", slug).strip("_")
    hint = None
    for h in HINTS:
        m = re.search(r"(?i)(?:^|_)%s(?=_|$)" % h, slug)
        if m:
            hint = h
            slug = re.sub(r"_+", "_", slug[:m.start()] + "_" + slug[m.end():]).strip("_")
            break
    return slug or "loop", hint


def output_name(key, bpm, bars, slug, hint, uid, used, token="LOOP"):
    """'<Key>__<bpm>bpm__LOOP__<n>bars__<slug>[_hint].wav', at most MAX_NAME bytes and unique within `used`
    (a set of names already taken in the destination folder); the slug is cut at a word boundary when too long,
    and a 4-hex digest of the source path is appended on a collision. `token` replaces LOOP for other content
    built from loops (tools/chop_vocal_loops.py writes CHP vocal phrases, which must NOT carry a loop token)."""
    prefix = "%s__%dbpm__%s__%dbars__" % (key, bpm, token, bars)
    suffix = ("_" + hint if hint else "") + ".wav"
    def fit(s, extra=""):
        room = MAX_NAME - len(prefix) - len(suffix) - len(extra)
        if len(s) > room:
            cut = s[:room]
            if "_" in cut[room // 2:]:
                cut = cut[: cut.rfind("_")]
            s = cut.rstrip("_-")
        return prefix + s + extra + suffix
    name = fit(slug)
    if name in used:
        name = fit(slug, "_" + hashlib.md5(uid.encode("utf-8")).hexdigest()[:4])
        n = 2
        while name in used:
            name = fit(slug, "_" + hashlib.md5(("%s#%d" % (uid, n)).encode("utf-8")).hexdigest()[:4])
            n += 1
    used.add(name)
    return name


# -------------------------------------------------------------------------------------------------------- scan

class Loop:
    __slots__ = ("src", "rel", "category", "group", "key", "key_kind", "key_label", "bpm", "duration",
                 "bars", "bars_exact", "slug", "hint", "name", "dst", "status", "detail", "seg", "seg_count", "offset")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


def scan(src, bar_seconds_at):
    """Every .wav under src (folders named 'peaks' and non-media are skipped) as a Loop with its parsed tokens."""
    loops, skipped = [], Counter()
    for dp, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d.lower() != "peaks")
        for f in sorted(files):
            if f.startswith(".") or not f.lower().endswith(".wav"):
                continue
            rel = os.path.relpath(os.path.join(dp, f), src)
            parts = rel.replace(os.sep, "/").split("/")
            category = parts[0].lower()
            if category not in CATEGORIES:
                skipped["unknown category " + parts[0]] += 1
                continue
            group = parts[1].lower() if len(parts) > 2 else ""
            stem = f[:-4]
            label = stem.split("__")[0]
            key, kind = key_folder(label)
            bpm = bpm_token(stem)
            info = wav_info(os.path.join(dp, f))
            duration = info[3] / info[1] if info and info[1] else ffprobe_duration(os.path.join(dp, f))
            if not duration:
                skipped["unreadable"] += 1
                continue
            lp = Loop(src=os.path.join(dp, f), rel=rel, category=category, group=group, key=key, key_kind=kind,
                      key_label=label, bpm=bpm, duration=duration)
            if bpm:
                bars_exact = duration / bar_seconds_at(bpm)
                lp.bars_exact = bars_exact
                lp.bars = max(1, int(round(bars_exact)))
            loops.append(lp)
    return loops, skipped


def print_histogram(loops, target, lo, hi):
    print("BPM histogram per category (%d files; * = within %.0f..%.0f BPM, the %.0f target's window):"
          % (len(loops), lo, hi, target))
    by_cat = defaultdict(Counter)
    for lp in loops:
        by_cat[lp.category + ("/" + lp.group if lp.group else "")][lp.bpm] += 1
    for cat in sorted(by_cat):
        c = by_cat[cat]
        cells = []
        for bpm in sorted(c):
            mark = "*" if lo <= bpm <= hi else ""
            cells.append("%s%s:%d" % ("none" if bpm == 0 else str(bpm), mark, c[bpm]))
        print("  %-16s %4d  %s" % (cat, sum(c.values()), "  ".join(cells)))
    labels = Counter((lp.key_label, lp.key, lp.key_kind) for lp in loops)
    print("key labels (label -> folder, kind):")
    line = []
    for (label, key, kind), n in sorted(labels.items(), key=lambda kv: (-kv[1], kv[0])):
        if kind == "none" and bpm_token(label):
            continue
        line.append("%s->%s(%s):%d" % (label, key, kind, n))
    for i in range(0, len(line), 6):
        print("  " + "  ".join(line[i:i + 6]))
    none = sum(n for (label, key, kind), n in labels.items() if kind == "none")
    print("  %d files start with a bpm token or a non-key word -> NA" % none)


# ------------------------------------------------------------------------------------------------- selection

def bars_pref(bars):
    if bars in (2, 4):
        return 0
    if bars == 1:
        return 1
    if bars == 8:
        return 2
    return 3


def bpm_pref(bpm, target):
    d = abs(bpm - target)
    return (0 if d == 0 else 1 if d <= 1 else 2, d)


def interleave(items, group_of):
    """Round-robin over groups (each group in name order) so one key or one sub-source cannot take every slot."""
    by = defaultdict(list)
    for it in items:
        by[group_of(it)].append(it)
    for g in by.values():
        g.sort(key=lambda it: it.rel)
    out, i = [], 0
    while len(out) < len(items):
        for g in sorted(by):
            if i < len(by[g]):
                out.append(by[g][i])
        i += 1
    return out


def rank(cands, target):
    """Candidates of one category in pick order: (bars preference, bpm preference, whole-bar fit) buckets,
    each bucket round-robin over keys."""
    buckets = defaultdict(list)
    for lp in cands:
        fit = 0 if abs(lp.bars_exact - lp.bars) <= 0.06 else 1
        buckets[(bars_pref(lp.bars), bpm_pref(lp.bpm, target), fit)].append(lp)
    out = []
    for b in sorted(buckets):
        out.extend(interleave(buckets[b], lambda lp: lp.key))
    return out


def select(loops, target, tolerance, limits, max_bars, budget_bytes, bar_frames):
    lo, hi = target * (1 - tolerance), target * (1 + tolerance)
    chosen, notes = [], []
    for cat, limit in limits.items():
        cands = [lp for lp in loops if lp.category == cat and lp.bpm and (lo <= lp.bpm <= hi or lp.bpm == target)]
        too_long = [lp for lp in cands if lp.bars > max_bars]
        cands = [lp for lp in cands if lp.bars <= max_bars]
        groups = sorted({lp.group for lp in cands})
        picked = []
        if len(groups) > 1:                                   # drum: share the limit between combo and solo
            ranked = {g: rank([lp for lp in cands if lp.group == g], target) for g in groups}
            share = {g: min(len(ranked[g]), limit // len(groups)) for g in groups}
            spare = limit - sum(share.values())
            for g in sorted(groups, key=lambda g: -len(ranked[g])):
                extra = min(spare, len(ranked[g]) - share[g])
                share[g] += extra
                spare -= extra
            for g in groups:
                picked.extend(ranked[g][: share[g]])
            picked = interleave(picked, lambda lp: lp.group) if picked else picked
        else:
            picked = rank(cands, target)[:limit]
        notes.append("%-10s %4d in window, %3d longer than %d bars dropped, %3d picked (limit %d)%s"
                     % (cat, len(cands) + len(too_long), len(too_long), max_bars, len(picked), limit,
                        "  [" + ", ".join("%s %d" % (g, sum(1 for lp in picked if lp.group == g)) for g in groups) + "]"
                        if len(groups) > 1 else ""))
        chosen.extend(picked)
    size = lambda lp: WAV_HEADER + lp.bars * bar_frames * 2
    total = sum(size(lp) for lp in chosen)
    dropped = []
    order = {id(lp): i for i, lp in enumerate(chosen)}          # later picks go first within equal lengths
    while total > budget_bytes and chosen:
        victim = max(chosen, key=lambda lp: (lp.bars, order[id(lp)]))
        chosen.remove(victim)
        dropped.append(victim)
        total -= size(victim)
    return chosen, notes, dropped, total


# -------------------------------------------------------------------------------------------------- rendering

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, check=True, **kw)


def decode_f32(cmd):
    data = run(cmd).stdout
    a = array("f")
    a.frombytes(data[: len(data) - len(data) % 4])
    return a


def render(lp, target, bar_frames, tmp, stretcher, engine):
    """Stretch lp to the target tempo, resample to RATE mono, fit to lp.bars whole bars, fade, normalise, write."""
    uid = hashlib.md5(("%s#%s" % (lp.rel, lp.seg)).encode("utf-8")).hexdigest()[:10]   # unique per output file
    try:
        if lp.bpm == target or stretcher == "none":
            pcm = decode_f32(["ffmpeg", "-v", "error", "-nostdin", "-i", lp.src, "-ac", "1", "-ar", str(RATE),
                              "-f", "f32le", "-acodec", "pcm_f32le", "-"])
        elif stretcher == "rubberband":
            mono = os.path.join(tmp, uid + ".mono.wav")
            stretched = os.path.join(tmp, uid + ".rb.wav")
            run(["ffmpeg", "-v", "error", "-nostdin", "-y", "-i", lp.src, "-ac", "1", "-acodec", "pcm_f32le", mono])
            flags = ["-3"] if engine == "r3" else ["-2", "-c", "6"]
            run(["rubberband", "-q"] + flags + ["--tempo", "%g:%g" % (lp.bpm, target), mono, stretched])
            pcm = decode_f32(["ffmpeg", "-v", "error", "-nostdin", "-i", stretched, "-ac", "1", "-ar", str(RATE),
                              "-f", "f32le", "-acodec", "pcm_f32le", "-"])
            for p in (mono, stretched):
                try:
                    os.remove(p)
                except OSError:
                    pass
        else:
            pcm = decode_f32(["ffmpeg", "-v", "error", "-nostdin", "-i", lp.src, "-af", "atempo=%.6f" % (target / lp.bpm),
                              "-ac", "1", "-ar", str(RATE), "-f", "f32le", "-acodec", "pcm_f32le", "-"])
    except subprocess.CalledProcessError as e:
        lp.status = "failed"
        lp.detail = (e.stderr or b"").decode("utf-8", "replace").strip().splitlines()[-1:] or [str(e)]
        lp.detail = lp.detail[0]
        return lp
    target_n = lp.bars * bar_frames
    start = (lp.offset or 0) * bar_frames
    pcm = pcm[start: start + target_n]
    content_n = len(pcm)
    if content_n == 0:
        lp.status = "failed"
        lp.detail = "no audio decoded" if not start else "segment %s starts beyond the content" % chr(65 + lp.seg)
        return lp
    peak = max(max(pcm), -min(pcm))
    if peak < 1e-6:
        lp.status = "failed"
        lp.detail = "silent"
        return lp
    gain = 10 ** (PEAK_DB / 20) / peak
    fade = min(content_n, RATE * FADE_MS // 1000)
    out = array("h", bytes(2 * target_n))
    full = content_n - fade
    for i in range(full):
        v = int(round(pcm[i] * gain * 32767))
        out[i] = -32768 if v < -32768 else 32767 if v > 32767 else v
    for i in range(full, content_n):
        v = int(round(pcm[i] * gain * 32767 * (content_n - i) / fade))
        out[i] = -32768 if v < -32768 else 32767 if v > 32767 else v
    os.makedirs(os.path.dirname(lp.dst), exist_ok=True)
    with wave.open(lp.dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(out.tobytes())
    lp.status = "ok"
    lp.detail = "%s %.3fs -> %d bars%s%s" % ("padded" if content_n < target_n else "trimmed", content_n / RATE, lp.bars,
                                             "" if lp.bpm == target else " (%g->%g)" % (lp.bpm, target),
                                             "" if lp.seg is None else " segment %s/%d" % (chr(65 + lp.seg), lp.seg_count))
    return lp


# ------------------------------------------------------------------------------------------------------ verify

def verify(loops_root, target, bar_frames, tolerance_s=0.005, peak_tol_db=1.0):
    """Check every WAV under loops_root; returns (ok, problems, per-category (count, bytes, bars Counter))."""
    problems = []
    per_cat = defaultdict(lambda: [0, 0, Counter()])
    for dp, dirs, files in os.walk(loops_root):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for f in sorted(files):
            if not f.lower().endswith(".wav"):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, loops_root)
            parts = rel.split(os.sep)
            category = parts[0]
            folder_key = parts[1] if len(parts) > 2 else "?"
            m = OUT_RE.match(f)
            if not m:
                problems.append("%s: name does not parse" % rel)
                continue
            if m.group("key") != folder_key:
                problems.append("%s: key %s but folder %s" % (rel, m.group("key"), folder_key))
            if int(m.group("bpm")) != target:
                problems.append("%s: bpm %s, expected %d" % (rel, m.group("bpm"), target))
            if bpm_token(f) != target:
                problems.append("%s: the firmware's BPM scanner reads %d" % (rel, bpm_token(f)))
            bars = int(m.group("bars"))
            try:
                with wave.open(path, "rb") as w:
                    ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
                    data = w.readframes(n)
            except (wave.Error, EOFError, OSError) as e:
                problems.append("%s: unreadable (%s)" % (rel, e))
                continue
            if (ch, width, rate) != (1, 2, RATE):
                problems.append("%s: format %dch %dbit %dHz" % (rel, ch, width * 8, rate))
            dev = n / RATE - bars * bar_frames / RATE
            if abs(dev) > tolerance_s:
                problems.append("%s: duration off by %.1f ms" % (rel, dev * 1000))
            a = array("h")
            a.frombytes(data)
            peak = max(max(a), -min(a)) / 32768.0 if len(a) else 0
            peak_db = 20 * math.log10(peak) if peak > 0 else -120
            if abs(peak_db - PEAK_DB) > peak_tol_db:
                problems.append("%s: peak %.2f dBFS" % (rel, peak_db))
            per_cat[category][0] += 1
            per_cat[category][1] += os.path.getsize(path)
            per_cat[category][2][bars] += 1
    return not problems, problems, per_cat


def print_totals(per_cat):
    total_n = total_b = 0
    for cat in sorted(per_cat, key=lambda c: CATEGORIES.index(c) if c in CATEGORIES else 99):
        n, b, bars = per_cat[cat]
        total_n += n
        total_b += b
        print("  %-10s %4d loops %7.2f MB  bars: %s" % (cat, n, b / 1048576, "  ".join("%d:%d" % kv for kv in sorted(bars.items()))))
    print("  %-10s %4d loops %7.2f MB" % ("total", total_n, total_b / 1048576))
    return total_n, total_b


# -------------------------------------------------------------------------------------------------------- main

def parse_limits(text):
    limits = OrderedDict(DEFAULT_LIMITS)
    if text:
        for part in text.split(","):
            cat, _, n = part.partition("=")
            cat = cat.strip().lower()
            if cat not in CATEGORIES:
                sys.exit("unknown category in --limit: %s" % cat)
            limits[cat] = int(n)
    return limits


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", help="loop library root (category folders beneath)")
    ap.add_argument("--out", required=True, help="SD card root (the folder that holds lp/)")
    ap.add_argument("--genre", default="House")
    ap.add_argument("--bpm", type=float, default=125)
    ap.add_argument("--tolerance", type=float, default=0.08, help="tempo window around --bpm (fraction, default 0.08)")
    ap.add_argument("--budget-mb", type=float, default=120)
    ap.add_argument("--max-bars", type=int, default=8)
    ap.add_argument("--split-bars", type=int, default=0,
                    help="split loops longer than N bars into whole N-bar segments named _A, _B, ... (0 = keep them whole)")
    ap.add_argument("--limit", help="per-category counts, e.g. drum=60,vocal=40 (defaults: %s)"
                    % ",".join("%s=%d" % kv for kv in DEFAULT_LIMITS.items()))
    ap.add_argument("--only", help="comma-separated categories to process (others untouched)")
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4)))
    ap.add_argument("--stretcher", choices=("auto", "rubberband", "ffmpeg"), default="auto")
    ap.add_argument("--engine", choices=("r3", "r2"), default="r3", help="rubberband engine (r2 uses crispness 6)")
    ap.add_argument("--scan-only", action="store_true", help="print the histogram and exit")
    ap.add_argument("--dry-run", action="store_true", help="select and print the plan, write nothing")
    ap.add_argument("--verify-only", action="store_true", help="check an existing loops folder and exit")
    ap.add_argument("--keep", action="store_true", help="do not delete the existing loops folder first")
    ap.add_argument("--tmp", help="scratch folder for intermediate files (default: a temp dir)")
    ap.add_argument("--manifest", help="write a TSV of source -> output here")
    args = ap.parse_args(argv)

    target = int(args.bpm) if float(args.bpm).is_integer() else args.bpm
    bar_seconds = 240.0 / target
    bar_frames = int(round(bar_seconds * RATE))
    if abs(bar_frames - bar_seconds * RATE) > 1e-6:
        print("note: a bar at %g BPM is %.4f frames, rounded to %d" % (target, bar_seconds * RATE, bar_frames))
    pack = os.path.join(os.path.abspath(args.out), "lp", "packs", args.genre)
    loops_root = os.path.join(pack, "loops")
    t0 = time.time()

    if args.verify_only:
        ok, problems, per_cat = verify(loops_root, target, bar_frames)
        for p in problems:
            print("PROBLEM: " + p)
        print_totals(per_cat)
        print("verify: %s (%d problems, %.1f s)" % ("ok" if ok else "FAILED", len(problems), time.time() - t0))
        return 0 if ok else 1

    if not args.src:
        sys.exit("--src is required")
    src = os.path.abspath(args.src)
    if not os.path.isdir(src):
        sys.exit("no such folder: %s" % src)

    # (1) scan
    loops, skipped = scan(src, lambda bpm: 240.0 / bpm)
    lo, hi = target * (1 - args.tolerance), target * (1 + args.tolerance)
    print_histogram(loops, target, lo, hi)
    if skipped:
        print("skipped: " + ", ".join("%s %d" % kv for kv in skipped.items()))
    print("scan: %d files in %.1f s" % (len(loops), time.time() - t0))
    if args.scan_only:
        return 0

    # (2) select
    limits = parse_limits(args.limit)
    if args.only:
        only = [c.strip().lower() for c in args.only.split(",")]
        limits = OrderedDict((c, n) for c, n in limits.items() if c in only)
    chosen, notes, dropped, total = select(loops, target, args.tolerance, limits, args.max_bars,
                                           int(args.budget_mb * 1048576), bar_frames)
    print("selection (window %.0f..%.0f BPM, %d-bar loops at most):" % (lo, hi, args.max_bars))
    for n in notes:
        print("  " + n)
    if dropped:
        print("  budget %.0f MB: dropped %d long loops (%s)" % (args.budget_mb, len(dropped),
              ", ".join("%s %d bars" % (lp.category, lp.bars) for lp in dropped[:8]) + (" ..." if len(dropped) > 8 else "")))
    print("  %d loops, %.1f MB planned" % (len(chosen), total / 1048576))

    if args.split_bars > 0:
        expanded = []
        for lp in chosen:
            if lp.bars <= args.split_bars:
                expanded.append(lp)
                continue
            k = lp.bars // args.split_bars
            for i in range(k):
                seg = Loop(**{s: getattr(lp, s) for s in Loop.__slots__})
                seg.seg, seg.seg_count, seg.offset, seg.bars = i, k, i * args.split_bars, args.split_bars
                expanded.append(seg)
        print("  --split-bars %d: %d loops -> %d files" % (args.split_bars, len(chosen), len(expanded)))
        chosen = expanded

    # names
    used = defaultdict(set)
    for lp in sorted(chosen, key=lambda lp: (lp.rel, lp.seg or 0)):
        lp.slug, lp.hint = make_slug(os.path.basename(lp.rel)[:-4], lp.key_kind != "none")
        if lp.seg is not None:
            lp.slug += "_" + chr(65 + lp.seg)
        folder = os.path.join(loops_root, lp.category, lp.key)
        lp.name = output_name(lp.key, target, lp.bars, lp.slug, lp.hint, lp.rel, used[folder])
        lp.dst = os.path.join(folder, lp.name)
    if args.dry_run:
        for lp in sorted(chosen, key=lambda lp: (lp.category, lp.key, lp.name)):
            print("  %-10s %-6s %3g bpm %5.2f bars  %s  <- %s" % (lp.category, lp.key, lp.bpm, lp.bars_exact, lp.name, lp.rel))
        print("dry run: nothing written")
        return 0

    # (3) stretcher
    stretcher = args.stretcher
    if stretcher in ("auto", "rubberband"):
        if shutil.which("rubberband"):
            stretcher = "rubberband"
        elif args.stretcher == "rubberband":
            sys.exit("rubberband not found (brew install rubberband) and --stretcher rubberband was asked")
        else:
            print("rubberband not found: falling back to ffmpeg's atempo filter (brew install rubberband for better quality)")
            stretcher = "ffmpeg"
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found")
    print("stretcher: %s%s" % (stretcher, " (%s engine)" % args.engine if stretcher == "rubberband" else ""))

    # (4) write
    if not args.keep:
        if args.only:
            for cat in limits:
                shutil.rmtree(os.path.join(loops_root, cat), ignore_errors=True)
        else:
            shutil.rmtree(loops_root, ignore_errors=True)
    os.makedirs(loops_root, exist_ok=True)
    tmp = args.tmp or tempfile.mkdtemp(prefix="make_loops_")
    os.makedirs(tmp, exist_ok=True)
    t1 = time.time()
    done = Counter()
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for lp in ex.map(lambda lp: render(lp, target, bar_frames, tmp, stretcher, args.engine), chosen):
            done[lp.status] += 1
            if lp.status != "ok":
                print("FAILED %s: %s" % (lp.rel, lp.detail))
            elif done["ok"] % 25 == 0:
                print("  %d/%d written (%.0f s)" % (done["ok"], len(chosen), time.time() - t1), flush=True)
    if not args.tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    print("render: %d written, %d failed in %.1f s" % (done["ok"], done["failed"], time.time() - t1))

    if args.manifest:
        with open(args.manifest, "w", encoding="utf-8") as f:
            f.write("category\tkey\tsource_bpm\tbars\tbars_exact\tsource\toutput\tstatus\tdetail\n")
            for lp in sorted(chosen, key=lambda lp: (lp.category, lp.key, lp.name)):
                f.write("\t".join(str(x) for x in (lp.category, lp.key, lp.bpm, lp.bars, "%.3f" % lp.bars_exact, lp.rel,
                                                    os.path.relpath(lp.dst, loops_root), lp.status, lp.detail)) + "\n")
        print("manifest: " + args.manifest)

    # (5) verify
    ok, problems, per_cat = verify(loops_root, target, bar_frames)
    for p in problems:
        print("PROBLEM: " + p)
    print("verify:")
    print_totals(per_cat)
    print("verify: %s (%d problems); total %.1f s" % ("ok" if ok else "FAILED", len(problems), time.time() - t0))
    print("next: python3 -I tools/shard_folders.py %s --max 64 && python3 -I tools/write_indexes.py %s --quiet"
          % (os.path.join(args.out, "lp"), args.out))
    return 0 if ok and not done["failed"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
