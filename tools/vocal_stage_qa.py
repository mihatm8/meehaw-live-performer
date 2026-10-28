#!/usr/bin/env python3
"""QA and inventory of a vocal staging mirror (research docs/RESEARCH_LIBRARY_EXPANSION.md §3, phases V1 + V2).

Walks <stage>/lp/packs/<Genre>/vocal/<Key>/ and checks every WAV the way the firmware and the card tools read it:
  * name: key label (first '__' token) equals its folder; <= 95 bytes; charset [A-Za-z0-9_#+.-]; a BPM token the
    firmware's parseBpmToken finds (60..249); no 'loop'/'loops' word (Library::hasLoopToken rejects those);
  * audio: RIFF 22050 Hz, mono, 16-bit; peak within 1 dB of -1 dBFS; when the name carries '<n>bars' the file is
    exactly n x 42336 frames (1.92 s per bar at 125 BPM) — the window token must be true;
  * inventory per key folder and method (SHOT = imported one-shots, SH = pitch-shifted copies, RAP = TTS, CHP =
    chopped loops), MB per key and in total, and the combined count with the card the stage will be merged into
    (--card, counted recursively, split into sung GHUAC/other and RAP) against --max-total and the >= --min-sung
    sung-files rule (sung = card non-RAP + SHOT + SH + CHP);
  * a listening list: --listen N files per method, deterministic (--seed), spread over the keys.

Usage: python3 -I tools/vocal_stage_qa.py --stage sdcard/lp_stage/vocals [--card sdcard] [--genre House]
          [--max-total 600] [--min-sung 120] [--listen 10] [--seed qa]
Exit 1 when a check fails. Stdlib only; run with -I.
"""
import argparse
import math
import os
import random
import re
import struct
import sys
import wave
from array import array
from collections import Counter, defaultdict

RATE = 22050
BAR = 42336
MAX_NAME = 95
CHARSET = re.compile(r"^[A-Za-z0-9_#+.-]+$")
KEY_RE = re.compile(r"^(?:[A-G]#?(?:min)?|NA)$")
BPM_RE = re.compile(r"(?<![0-9])(\d{2,3})[ _-]*bpm(?![a-z])|bpm[ _-]*(\d{2,3})(?![0-9])", re.IGNORECASE)
BARS_RE = re.compile(r"(?<![A-Za-z0-9])(\d{1,3})[ _-]*bars?(?![A-Za-z])", re.IGNORECASE)
LOOP_RE = re.compile(r"(?i)(?<![A-Za-z0-9])loops?(?![A-Za-z0-9])")
MINOR_ORDER = ["Amin", "A#min", "Bmin", "Cmin", "C#min", "Dmin", "D#min", "Emin", "Fmin", "F#min", "Gmin", "G#min"]


def method_of(name):
    if re.search(r"__RAP[MFR]__", name):
        return "RAP"
    if re.search(r"-SH[+-]\d\.wav$", name):
        return "SH"
    if "__CHP__" in name:
        return "CHP"
    if "GHUAC" in name:
        return "GHUAC"
    return "SHOT"


def bpm_token(name):
    for m in BPM_RE.finditer(name):
        v = int(m.group(1) or m.group(2))
        if 60 <= v <= 249:
            return v
    return 0


def wav_info(path):
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
                fmt = struct.unpack("<HHIIHH", f.read(size + (size & 1))[:16])
            elif cid == b"data":
                return fmt[1], fmt[2], fmt[5], size // fmt[4], f.tell()
            else:
                f.seek(size + (size & 1), 1)


def peak_db(path):
    with wave.open(path, "rb") as w:
        a = array("h")
        a.frombytes(w.readframes(w.getnframes()))
    pk = max(max(a), -min(a)) / 32768.0 if len(a) else 0.0
    return 20 * math.log10(pk) if pk > 0 else -120.0


def walk(vocal_root):
    for key in sorted(os.listdir(vocal_root)):
        kp = os.path.join(vocal_root, key)
        if key.startswith(".") or not os.path.isdir(kp):
            continue
        for dp, dns, fns in os.walk(kp):
            for f in sorted(fns):
                if f.lower().endswith(".wav") and not f.startswith("."):
                    yield key, os.path.join(dp, f), f


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--stage", required=True)
    ap.add_argument("--card", default=None, help="the card root the stage merges into (its vocal counts are added)")
    ap.add_argument("--genre", default="House")
    ap.add_argument("--max-total", type=int, default=600)
    ap.add_argument("--min-sung", type=int, default=120)
    ap.add_argument("--listen", type=int, default=10)
    ap.add_argument("--seed", default="qa")
    args = ap.parse_args(argv)
    root = os.path.join(os.path.abspath(args.stage), "lp", "packs", args.genre, "vocal")
    problems = []
    inv = defaultdict(Counter)              # key -> method -> files
    mb = defaultdict(float)
    bars_hist = Counter()
    durations = defaultdict(list)
    names_by_method = defaultdict(list)
    n_files = 0
    for key, path, f in walk(root):
        n_files += 1
        m = method_of(f)
        inv[key][m] += 1
        mb[key] += os.path.getsize(path) / 1048576
        names_by_method[m].append((key, f))
        label = f.split("__")[0]
        if label != key or not KEY_RE.match(key):
            problems.append("%s/%s: key label %s does not match its folder" % (key, f, label))
        if len(f.encode("utf-8")) > MAX_NAME:
            problems.append("%s/%s: %d bytes" % (key, f, len(f.encode("utf-8"))))
        if not CHARSET.match(f):
            problems.append("%s/%s: characters outside [A-Za-z0-9_#+.-]" % (key, f))
        if not bpm_token(f):
            problems.append("%s/%s: no BPM token the firmware can read" % (key, f))
        if LOOP_RE.search(f):
            problems.append("%s/%s: loop token (the vocal picker would skip it)" % (key, f))
        info = wav_info(path)
        if not info or info[:3] != (1, RATE, 16):
            problems.append("%s/%s: format %s" % (key, f, info[:3] if info else None))
            continue
        frames = info[3]
        durations[m].append(frames / RATE)
        bm = BARS_RE.search(f)
        if bm:
            bars = int(bm.group(1))
            bars_hist[(m, bars)] += 1
            if frames != bars * BAR:
                problems.append("%s/%s: %d frames, the %dbars token says %d" % (key, f, frames, bars, bars * BAR))
        pk = peak_db(path)
        if abs(pk + 1.0) > 1.0:
            problems.append("%s/%s: peak %.2f dBFS" % (key, f, pk))
    for p in problems[:40]:
        print("PROBLEM: " + p)
    if len(problems) > 40:
        print("... %d problems" % len(problems))

    card = defaultdict(Counter)
    if args.card:
        croot = os.path.join(os.path.abspath(args.card), "lp", "packs", args.genre, "vocal")
        if os.path.isdir(croot):
            for key, path, f in walk(croot):
                card[key]["RAP" if method_of(f) == "RAP" else "sung"] += 1

    methods = ["SHOT", "SH", "RAP", "CHP"]
    keys = [k for k in MINOR_ORDER if k in inv or k in card] + sorted(k for k in set(inv) | set(card) if k not in MINOR_ORDER)
    print("\ninventory (stage files per method; card = what sdcard/lp holds today: sung GHUAC/other + RAP; total = card + stage):")
    print("  %-6s %5s %5s %5s %5s %7s %8s | %9s %8s | %5s %5s %s" % ("key", "SHOT", "SH", "RAP", "CHP", "stage", "MB", "card sung", "card RAP", "total", "sung", ""))
    tot = Counter()
    tot_mb = 0.0
    fails_total = fails_sung = 0
    for k in keys:
        c = inv[k]
        stage_n = sum(c.values())
        card_sung, card_rap = card[k]["sung"], card[k]["RAP"]
        total = stage_n + card_sung + card_rap
        sung = card_sung + c["SHOT"] + c["SH"] + c["CHP"] + c["GHUAC"]
        flag = ""
        if k.endswith("min"):
            if total > args.max_total:
                flag += " OVER %d" % args.max_total
                fails_total += 1
            if sung < args.min_sung:
                flag += " SUNG<%d" % args.min_sung
                fails_sung += 1
        print("  %-6s %5d %5d %5d %5d %7d %8.1f | %9d %8d | %5d %5d%s" % (k, c["SHOT"], c["SH"], c["RAP"], c["CHP"], stage_n, mb[k], card_sung, card_rap, total, sung, flag))
        tot.update(c)
        tot_mb += mb[k]
    print("  %-6s %5d %5d %5d %5d %7d %8.1f" % ("all", tot["SHOT"], tot["SH"], tot["RAP"], tot["CHP"], sum(tot.values()), tot_mb))
    minor_stage = [sum(inv[k].values()) for k in MINOR_ORDER if k in inv]
    if minor_stage:
        print("  minor keys: stage files min %d / mean %.0f / max %d; stage total %d files, %.1f MB (%.2f GB)"
              % (min(minor_stage), sum(minor_stage) / len(minor_stage), max(minor_stage), n_files, tot_mb, tot_mb / 1024))
    print("\ndurations (s) per method: " + "; ".join(
        "%s n=%d min %.2f median %.2f max %.2f" % (m, len(d), min(d), sorted(d)[len(d) // 2], max(d)) for m, d in sorted(durations.items()) if d))
    print("bars tokens: " + ", ".join("%s %dbars: %d" % (m, b, n) for (m, b), n in sorted(bars_hist.items())))
    print("checks: %d files, %d problems; key folders over %d: %d; minor keys under %d sung: %d"
          % (n_files, len(problems), args.max_total, fails_total, args.min_sung, fails_sung))

    if args.listen:
        rng = random.Random(args.seed)
        print("\nlistening list (%d per method, spread over keys):" % args.listen)
        for m in methods:
            items = names_by_method.get(m, [])
            if not items:
                continue
            by_key = defaultdict(list)
            for k, f in items:
                by_key[k].append(f)
            picked = []
            order = sorted(by_key)
            rng.shuffle(order)
            i = 0
            while len(picked) < min(args.listen, len(items)):
                k = order[i % len(order)]
                if by_key[k]:
                    picked.append((k, by_key[k].pop(rng.randrange(len(by_key[k])))))
                i += 1
            print("  " + m)
            for k, f in picked:
                print("    %s/%s" % (k, f))
    return 1 if problems or fails_total or fails_sung else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
