#!/usr/bin/env python3
"""Import ldrolez/free-midi-chords (MIT) progressions as Chord clips and derive Lead and Pad clips from them
(docs/RESEARCH_LIBRARY_EXPANSION.md §2.1 option B, §2.2 phase M1).

Source: the "chords" release of https://github.com/ldrolez/free-midi-chords ("All the MIDI files are
licensed under the MIT license", Copyright (c) 2019 Ludovic Drolez), already on disk at
~/Documents/cardputer/_downloads/free-midi-chords. Only `4 Progression/{Major,Minor}` is read: those
progressions stay inside their key; the Modal folder borrows from other scales and is skipped. Every
progression comes as a straight file (one chord per bar, 4 beats) and in four rhythm variants in the
`pop style`, `pop2 style`, `soul style`, `hiphop2 style` subfolders (two beats per chord in pop/pop2/soul,
syncopated bars in hiphop2).

Output (same tree as the card, no shards / indexes — shard_folders.py and write_indexes.py run after the merge):

  <out>/lp/midi/Chord/<Key>/<Key>__Chord__LDZ-<numerals>-<mood>[-<style>][-A|-B]__<hash8>.mid
  <out>/lp/midi/Lead/<Key>/<Key>__Lead__LDZ-<numerals>-<mood>[-A|-B]-<leadstyle>__<hash8>.mid
  <out>/lp/midi/Pad/<Key>/<Key>__Pad__LDZ-<numerals>-<mood>[-A|-B]-pad__<hash8>.mid

  Key       a Minor progression lands in its minor folder ("Amin"), a Major progression in the relative-major
            folder with the bare-root label ("C", SPEC §6); the picker reads both for a project key, so the
            per-key caps below count both folders together and nothing is written twice
  LDZ       marks ldrolez-derived material (research open question 9)
  hash8     the project's hash rule (tools/lp_smf.py): sha1 over the written note bytes, first 8 hex digits

Clip rules (core/config.h): a clip is 2 or 4 bars. A progression of 8 chords (32 beats straight / hiphop2) is
split into two 4-bar halves A and B; a shorter file is extended to the next 2- or 4-bar length by holding its
last chord (at most one bar of extension: the 3-chord files are 5.5 beats in pop style and 12 in hiphop2 — the
author's own advice for odd chord counts is to double the last chord); anything else (24, 36, 40 beats, or
more than a bar short) is skipped and counted. Chord clips drop the bass note below C3 (48): the Bass track
plays roots. Lead and Pad are derived only from the straight files, with the recipes of
tools/prepare_content.py (make_lead: arp_up8 / arp_down8 / arp_updown16 / stab_syncopated / top_melody, one
style per progression; make_pad: the voicing sustained legato, <= 4 notes). Every clip is placed so its median
pitch sits inside the category band of transpose_library.py (Chord 53-69, Lead 48-79, Pad 59-77) and every
note inside 36..88.

Selection per key (both folders together), deterministic by --seed: Chord takes every straight clip first,
then rhythm variants in rounds over the progressions with the style rotating (so the five styles and all
progressions are spread evenly) until --chord-per-key; Lead and Pad take the first --lead-per-key /
--pad-per-key of the shuffled straight clips. A clip whose notes equal a file already in the key's folders
(the library under --existing, files already staged under --out, or an earlier clip of this run) is dropped.
--folder-cap keeps every target folder (existing + staged + these) at or below the cap.

Usage:
  python3 -I tools/import_ldrolez.py --src ~/Documents/cardputer/_downloads/free-midi-chords
      --out sdcard/lp_stage/m1 --existing sdcard/lp/midi
      [--chord-per-key 300] [--lead-per-key 108] [--pad-per-key 108] [--folder-cap 600] [--seed 2026]
      [--modes Major,Minor] [--styles straight,pop,pop2,soul,hiphop2]
      [--dry-run] [--report] [--verify-only] [--keep] [--manifest FILE] [--audition N]

Run after tools/transpose_library.py (which replaces the staged category folders wholesale); this tool only
removes its own earlier LDZ files and counts everything else already staged toward the caps. Stdlib only; run
with -I. Imports tools/lp_smf.py and transpose_library.py (bands, octave placement) from its own folder.
"""
import argparse
import collections
import os
import random
import re
import shutil
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_smf as smf  # noqa: E402
from transpose_library import BANDS, shift_and_fold  # noqa: E402

KEY_DIR_RE = re.compile(r"^(\d\d) - (\w+) Major - (\w+#?) minor$")
STYLES = ["straight", "pop", "pop2", "soul", "hiphop2"]
VARIANTS = STYLES[1:]
LDZ_RE = re.compile(r"^LDZ-")
P = smf.PPQN
BAR = 4 * P
LEAD_STYLES = ["arp_up8", "arp_updown16", "top_melody", "stab_syncopated", "arp_down8"]
SCALE_MAJOR = [0, 2, 4, 5, 7, 9, 11]
SCALE_MINOR = [0, 2, 3, 5, 7, 8, 10]


def root_pc(name):
    """'Db' / 'Bb' -> sharps; 'C#', 'A' unchanged."""
    n = smf.FLAT_TO_SHARP.get(name.upper(), name)
    return smf.NOTE_NAMES.index(n[0].upper() + n[1:])


# ----------------------------------------------------------------------------------------------- source clips

def load_clips(path, stats):
    """One source file -> list of (suffix, notes96, length_ticks) clips of exactly 2 or 4 bars."""
    r = smf.read_smf(path)
    notes = smf.notes_to96(r["notes"], r["ppq"])
    if not notes:
        stats["empty"] += 1
        return []
    raw = max(s + d for s, d, _, _ in notes)
    if raw == 8 * BAR:
        out = []
        for half, tag in ((0, "A"), (1, "B")):
            lo, hi = half * 4 * BAR, (half + 1) * 4 * BAR
            part = [(s - lo, min(s + d, hi) - s, p, v) for s, d, p, v in notes if lo <= s < hi]
            if len({s for s, _, _, _ in part}) < 2:
                stats["skipped: half with one chord"] += 1
                continue
            out.append((tag, sorted(part), 4 * BAR))
        stats["split 8-chord progressions"] += 1
        return out
    target = 2 * BAR if raw <= 2 * BAR else 4 * BAR if raw <= 4 * BAR else None
    if target is None:
        stats[f"skipped: {raw / P:g} beats"] += 1
        return []
    if target - raw > BAR:
        stats[f"skipped: {raw / P:g} beats (more than a bar short of {target // P})"] += 1
        return []
    if target != raw:
        stats["extended to the bar by holding the last chord"] += 1
        notes = [(s, (target - s) if s + d == raw else d, p, v) for s, d, p, v in notes]
    return [("", sorted(notes), target)]


def groups_of(notes):
    """Notes grouped by onset -> (start_beats, dur_beats, sorted pitches, vel), as prepare_content.py."""
    g = collections.defaultdict(list)
    for s, d, p, v in notes:
        g[s].append((d, p, v))
    out = []
    for s in sorted(g):
        out.append((s / P, max(d for d, _, _ in g[s]) / P, sorted(p for _, p, _ in g[s]), g[s][0][2]))
    return out


def to_range(pitch, lo, hi):
    while pitch < lo:
        pitch += 12
    while pitch > hi:
        pitch -= 12
    return pitch


def scale_pitches(root, minor, lo, hi):
    ints = SCALE_MINOR if minor else SCALE_MAJOR
    return [p for p in range(lo, hi + 1) if (p - root) % 12 in ints]


def make_lead(groups, length, root, minor, rng):
    """prepare_content.py make_lead, in beats. Returns (style, notes as (start, dur, pitch, vel) beats)."""
    style = rng.choice(LEAD_STYLES)
    scale = scale_pitches(root, minor, 60, 84)
    out = []
    for i, (s, d, pitches, vel) in enumerate(groups):
        nxt = groups[i + 1][0] if i + 1 < len(groups) else length
        seg = nxt - s
        tones = sorted(set(to_range(p, 60, 79) for p in pitches if p >= 48)) or sorted(set(to_range(p, 60, 79) for p in pitches))
        t, k = 0.0, 0
        while t < seg - 1e-6:
            if style == "arp_up8":
                out.append((s + t, 0.45, tones[k % len(tones)], 96 if k % 4 == 0 else 80)); t += 0.5; k += 1
            elif style == "arp_down8":
                out.append((s + t, 0.45, tones[(-k) % len(tones)], 96 if k % 4 == 0 else 80)); t += 0.5; k += 1
            elif style == "arp_updown16":
                seq = tones + tones[-2:0:-1] if len(tones) > 2 else tones
                out.append((s + t, 0.22, seq[k % len(seq)], 92 if k % 4 == 0 else 74)); t += 0.25; k += 1
            elif style == "stab_syncopated":
                for off in (0.0, 0.75, 1.5, 2.25, 3.0, 3.5):
                    if off < seg:
                        out.append((s + off, 0.3, tones[(k + int(off * 2)) % len(tones)], 100 if off == 0 else 86))
                t = seg; k += 1
            else:                                                       # top_melody
                top = tones[-1]
                out.append((s + t, 1.4, top, 100))
                if i + 1 < len(groups):
                    nxt_tones = sorted(set(to_range(p, 60, 79) for p in groups[i + 1][2] if p >= 48)) or [top]
                    nxt_top = nxt_tones[-1]
                else:
                    nxt_top = top
                pool = [p for p in scale if min(top, nxt_top) <= p <= max(top, nxt_top)] or [top]
                t2 = t + 1.5
                while t2 < seg - 0.4:
                    out.append((s + t2, 0.45, rng.choice(pool), 72)); t2 += 0.5
                t = seg
    return style, [(st, du, p, v) for st, du, p, v in out if st < length - 1e-6]


def make_pad(groups, length):
    out = []
    for i, (s, d, pitches, vel) in enumerate(groups):
        nxt = groups[i + 1][0] if i + 1 < len(groups) else length
        for p in sorted(set(to_range(p, 56, 77) for p in pitches if p >= 48))[:4]:
            out.append((s, nxt - s - 0.05, p, 80))
    return out


def beats_to_ticks(notes, length_ticks):
    out = []
    for s, d, p, v in notes:
        st = int(round(s * P))
        en = max(st + 1, int(round((s + d) * P)))
        if st >= length_ticks:
            continue
        out.append((st, min(en, length_ticks) - st, p, v))
    return sorted(out)


def place_in_band(notes, cat):
    """Octave-shift the whole clip so its median sits in the category band and the notes inside the hard
    limits (shift_and_fold with a zero base interval)."""
    return shift_and_fold(notes, 0, BANDS[cat])[0]


class Prog:
    __slots__ = ("idx", "mode", "root", "minor", "folder", "label", "numerals", "mood", "stem", "clips", "src")


def scan(src, modes, styles, stats):
    """All progressions of the chords pack. Returns dict key_idx -> list of Prog (one per straight file), each
    with .clips[style] = list of (suffix, notes, length)."""
    base = os.path.join(src, "chords")
    progs = collections.defaultdict(list)
    for kd in sorted(os.listdir(base)):
        m = KEY_DIR_RE.match(kd)
        if not m:
            continue
        idx, major_name, minor_name = int(m.group(1)), m.group(2), m.group(3)
        for mode in modes:
            mode_dir = os.path.join(base, kd, "4 Progression", mode)
            if not os.path.isdir(mode_dir):
                continue
            minor = mode == "Minor"
            root = root_pc(minor_name if minor else major_name)
            for f in sorted(os.listdir(mode_dir)):
                if not f.lower().endswith(".mid"):
                    continue
                parts = f[:-4].split(" - ")
                if len(parts) < 3:
                    stats["skipped: unexpected name"] += 1
                    continue
                p = Prog()
                p.idx, p.mode, p.root, p.minor = idx, mode, root, minor
                p.folder = p.label = smf.folder_label(root, minor)
                p.numerals = smf.safe_token("-".join(parts[1].split()))
                words = parts[2].split()
                if words and words[0] == "New":
                    words = words[1:]
                p.mood = smf.safe_token("".join(words))
                p.stem = f[:-4]
                p.src = os.path.relpath(os.path.join(mode_dir, f), src)
                p.clips = {}
                for style in styles:
                    sp = os.path.join(mode_dir, f) if style == "straight" else os.path.join(mode_dir, f"{style} style", f)
                    if not os.path.isfile(sp):
                        if style != "straight":
                            stats["missing rhythm variant"] += 1
                        continue
                    try:
                        p.clips[style] = load_clips(sp, stats)
                    except ValueError as e:
                        stats["unreadable"] += 1
                        print(f"warning: {sp}: {e}", file=sys.stderr)
                if p.clips.get("straight"):
                    progs[idx].append(p)
                    stats[f"{mode} progressions"] += 1
    return progs


def folder_counts(root, cats):
    counts, hashes = collections.Counter(), collections.defaultdict(set)
    for cat in cats:
        cdir = os.path.join(root, "lp", "midi", cat) if os.path.isdir(os.path.join(root, "lp", "midi", cat)) else os.path.join(root, cat)
        if not os.path.isdir(cdir):
            continue
        for folder in sorted(os.listdir(cdir)):
            fdir = os.path.join(cdir, folder)
            if not os.path.isdir(fdir) or folder.startswith("."):
                continue
            for dp, dirs, files in os.walk(fdir):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for f in files:
                    if not f.lower().endswith(".mid") or f.startswith("."):
                        continue
                    counts[cat, folder] += 1
                    try:
                        r = smf.read_smf(os.path.join(dp, f))
                        notes, _, _ = smf.device_view(smf.notes_to96(r["notes"], r["ppq"]), smf.to96(r["end"], r["ppq"]))
                        if notes:
                            hashes[cat, folder].add(smf.note_hash(notes))
                    except (ValueError, OSError):
                        pass
    return counts, hashes


def plan(progs, args, existing_counts, existing_hashes, staged_counts, staged_hashes, stats):
    items = []
    per_key = {"Chord": args.chord_per_key, "Lead": args.lead_per_key, "Pad": args.pad_per_key}
    trimmed = collections.Counter()
    for idx in sorted(progs):
        plist = progs[idx]
        minor_root = next((p.root for p in plist if p.minor), None)
        if minor_root is None:
            minor_root = (plist[0].root + 9) % 12
        folders = smf.key_folders(minor_root)
        rng = random.Random(f"{args.seed}:{idx}")
        order = list(plist)
        rng.shuffle(order)
        run_hashes = collections.defaultdict(set)

        def emit(cat, p, suffix, notes, length, mid):
            key = (cat, p.folder)
            seen = existing_hashes.get(key, set()) | staged_hashes.get(key, set())
            for f in folders:                                      # the picker merges the key's folders
                seen = seen | existing_hashes.get((cat, f), set()) | staged_hashes.get((cat, f), set())
            notes = place_in_band(notes, cat)
            h = smf.note_hash(notes)
            if h in seen or h in run_hashes[key] or any(h in run_hashes[(cat, f)] for f in folders):
                stats[f"{cat}: de-duplicated"] += 1
                return False
            have = existing_counts.get(key, 0) + staged_counts.get(key, 0) + len(run_hashes[key])
            if have >= args.folder_cap:
                trimmed[cat, p.folder] += 1
                return False
            run_hashes[key].add(h)
            name = smf.fit_name(p.label, cat, mid, h)
            items.append({"cat": cat, "folder": p.folder, "key_idx": idx, "minor_root": minor_root, "prog": p, "suffix": suffix,
                          "notes": notes, "length": length, "hash": h, "out_rel": os.path.join("lp", "midi", cat, p.folder, name),
                          "mid": mid})
            return True

        # Chord: straight first, then rounds of rhythm variants with the style rotating
        n = 0
        cands = [("straight", p) for p in order]
        for r in range(len(VARIANTS)):
            for i, p in enumerate(order):
                cands.append((VARIANTS[(i + r) % len(VARIANTS)], p))
        for style, p in cands:
            if n >= per_key["Chord"]:
                break
            for suffix, notes, length in p.clips.get(style, []):
                if n >= per_key["Chord"]:
                    break
                chord = [(s, d, q, v) for s, d, q, v in notes if q >= 48] or notes
                mid = f"LDZ-{p.numerals}-{p.mood}" + (f"-{style}" if style != "straight" else "") + (f"-{suffix}" if suffix else "")
                if emit("Chord", p, suffix, chord, length, mid):
                    n += 1
                    stats[f"Chord: {style}"] += 1
        # Lead and Pad from the straight clips
        for cat in ("Lead", "Pad"):
            n = 0
            for p in order:
                if n >= per_key[cat]:
                    break
                for suffix, notes, length in p.clips.get("straight", []):
                    if n >= per_key[cat]:
                        break
                    groups = groups_of(notes)
                    beats = length / P
                    if cat == "Lead":
                        lrng = random.Random(f"{args.seed}:lead:{p.numerals}:{p.mood}:{suffix}")
                        style, lead = make_lead(groups, beats, p.root, p.minor, lrng)
                        derived = beats_to_ticks(lead, length)
                        mid = f"LDZ-{p.numerals}-{p.mood}" + (f"-{suffix}" if suffix else "") + f"-{style.replace('_', '')}"
                    else:
                        derived = beats_to_ticks(make_pad(groups, beats), length)
                        style = "pad"
                        mid = f"LDZ-{p.numerals}-{p.mood}" + (f"-{suffix}" if suffix else "") + "-pad"
                    if not derived:
                        stats[f"{cat}: empty derivation"] += 1
                        continue
                    if emit(cat, p, suffix, derived, length, mid):
                        n += 1
                        stats[f"{cat}: {style}"] += 1
    return items, trimmed


def report(items, progs, existing_counts, staged_counts, stats, trimmed):
    print("\n== source: " + "; ".join(f"{k} {stats[k]}" for k in sorted(stats) if ":" not in k))
    for cat in ("Chord", "Lead", "Pad"):
        print(f"== {cat}: " + "; ".join(f"{k.split(': ', 1)[1]} {stats[k]}" for k in sorted(stats) if k.startswith(cat + ": ")))
    by = collections.Counter((it["cat"], it["folder"]) for it in items)
    print("\n== per category / key (library = existing lp/midi; staged = already under --out before this run; +LDZ = this run;"
          " visible = the key's minor + relative-major (+ NA) folders)")
    for cat in ("Chord", "Lead", "Pad"):
        print(f"\n  {cat:6s} {'key':6s} {'min:lib':>8s} {'min:stg':>8s} {'+LDZ':>6s} {'min:after':>10s} | {'maj':>4s} {'maj:lib':>8s} {'maj:stg':>8s} {'+LDZ':>6s} {'maj:after':>10s} | {'vis-after':>9s} {'trimmed':>8s}")
        tot = collections.Counter()
        for T in smf.MINOR_CIRCLE:
            fmin, fmaj, fna = smf.key_folders(T)
            lib_min, stg_min, add_min = existing_counts.get((cat, fmin), 0), staged_counts.get((cat, fmin), 0), by[cat, fmin]
            lib_maj, stg_maj, add_maj = existing_counts.get((cat, fmaj), 0), staged_counts.get((cat, fmaj), 0), by[cat, fmaj]
            na = existing_counts.get((cat, fna), 0) + staged_counts.get((cat, fna), 0)
            vis = lib_min + stg_min + add_min + lib_maj + stg_maj + add_maj + na
            tr = trimmed[cat, fmin] + trimmed[cat, fmaj]
            print(f"  {'':6s} {fmin:6s} {lib_min:8d} {stg_min:8d} {add_min:6d} {lib_min + stg_min + add_min:10d} | {fmaj:>4s} {lib_maj:8d} {stg_maj:8d} {add_maj:6d} {lib_maj + stg_maj + add_maj:10d} | {vis:9d} {tr:8d}")
            tot.update({"add": add_min + add_maj, "vis": vis, "tr": tr})
        print(f"  {'':6s} {'total':6s} {'':8s} {'':8s} {tot['add']:6d} {'':10s} | {'':>4s} {'':8s} {'':8s} {'':6s} {'':10s} | {tot['vis']:9d} {tot['tr']:8d}")
    bars = collections.Counter((it["cat"], it["length"] // BAR) for it in items)
    print("\n== bars: " + ", ".join(f"{c} {b}-bar: {n}" for (c, b), n in sorted(bars.items())))
    print(f"== total files planned: {len(items)}")


def audition(items, n, seed):
    rng = random.Random(f"{seed}:audition")
    want = {"Lead": (n + 2) // 3 + 1, "Pad": (n + 2) // 3, "Chord": n - (n + 2) // 3 - 1 - (n + 2) // 3}
    picks = []
    for cat, k in want.items():
        pool = [it for it in items if it["cat"] == cat]
        rng.shuffle(pool)
        seen = set()
        for it in pool:
            if len([p for p in picks if p["cat"] == cat]) >= k:
                break
            if it["folder"] in seen and len(seen) < 12:
                continue
            seen.add(it["folder"])
            picks.append(it)
    return picks


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--src", required=True, help="free-midi-chords folder (holds chords/ and progressions/)")
    ap.add_argument("--out", required=True, help="staging root; files go to <out>/lp/midi/<Category>/<Key>/")
    ap.add_argument("--existing", help="the library's lp/midi (counts and de-dupe against the real card content)")
    ap.add_argument("--chord-per-key", type=int, default=300)
    ap.add_argument("--lead-per-key", type=int, default=108)
    ap.add_argument("--pad-per-key", type=int, default=108)
    ap.add_argument("--folder-cap", type=int, default=600)
    ap.add_argument("--seed", default="2026")
    ap.add_argument("--modes", default="Major,Minor")
    ap.add_argument("--styles", default=",".join(STYLES))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--keep", action="store_true", help="keep LDZ files already under --out (they count toward the caps and the de-dupe)")
    ap.add_argument("--manifest")
    ap.add_argument("--audition", type=int, default=0)
    args = ap.parse_args(argv)
    cats = ["Chord", "Lead", "Pad"]
    if args.verify_only:
        return 1 if smf.verify_tree(args.out, cats, BANDS, smf.DERIVED_RE, "stage") else 0
    stats = collections.Counter()
    progs = scan(os.path.expanduser(args.src), [m for m in args.modes.split(",") if m], [s for s in args.styles.split(",") if s], stats)
    existing_counts, existing_hashes = folder_counts(args.existing, cats) if args.existing else (collections.Counter(), {})
    # files already staged under --out: this tool's earlier LDZ output is dropped first unless --keep
    staged_counts, staged_hashes = collections.Counter(), collections.defaultdict(set)
    if os.path.isdir(os.path.join(args.out, "lp", "midi")):
        sc, sh = folder_counts(args.out, cats)
        for (cat, folder), cnt in sc.items():
            fdir = os.path.join(args.out, "lp", "midi", cat, folder)
            names = [f for f in os.listdir(fdir) if f.endswith(".mid")]
            own = [f for f in names if LDZ_RE.match(f.split("__")[2] if f.count("__") >= 3 else "")]
            if args.keep:
                staged_counts[cat, folder] = cnt
                staged_hashes[cat, folder] = sh[cat, folder]
            else:
                staged_counts[cat, folder] = cnt - len(own)
                for f in names:
                    if f not in own:
                        try:
                            r = smf.read_smf(os.path.join(fdir, f))
                            notes, _, _ = smf.device_view(smf.notes_to96(r["notes"], r["ppq"]), smf.to96(r["end"], r["ppq"]))
                            staged_hashes[cat, folder].add(smf.note_hash(notes))
                        except (ValueError, OSError):
                            pass
    items, trimmed = plan(progs, args, existing_counts, existing_hashes, staged_counts, staged_hashes, stats)
    if args.report or args.dry_run:
        report(items, progs, existing_counts, staged_counts, stats, trimmed)
    if args.manifest:
        with open(args.manifest, "w") as f:
            f.write("output\tsource\tbars\tnotes\n")
            for it in sorted(items, key=lambda i: i["out_rel"]):
                f.write(f"{it['out_rel']}\t{it['prog'].src}{(' [' + it['suffix'] + ']') if it['suffix'] else ''}\t{it['length'] // BAR}\t{len(it['notes'])}\n")
    if args.audition:
        print("\n== audition")
        for it in audition(items, args.audition, args.seed):
            print(f"  {it['out_rel']}    <- {it['prog'].src}{(' [' + it['suffix'] + ']') if it['suffix'] else ''}")
    if args.dry_run:
        print("dry run: nothing written")
        return 0
    if not args.keep:
        removed = 0
        for cat in cats:
            cdir = os.path.join(args.out, "lp", "midi", cat)
            if not os.path.isdir(cdir):
                continue
            for folder in os.listdir(cdir):
                fdir = os.path.join(cdir, folder)
                if not os.path.isdir(fdir):
                    continue
                for f in os.listdir(fdir):
                    if f.endswith(".mid") and f.count("__") >= 3 and LDZ_RE.match(f.split("__")[2]):
                        os.remove(os.path.join(fdir, f))
                        removed += 1
                if not os.listdir(fdir):
                    os.rmdir(fdir)
        if removed:
            print(f"removed {removed} earlier LDZ files")
    total = 0
    for it in items:
        total += smf.write_smf0(os.path.join(args.out, it["out_rel"]), it["notes"], it["length"])
    print(f"written: {len(items)} files, {total / 1024:.1f} KB -> {os.path.join(args.out, 'lp', 'midi')}")
    return 1 if smf.verify_tree(args.out, cats, BANDS, smf.DERIVED_RE, "stage") else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
