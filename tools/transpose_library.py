#!/usr/bin/env python3
"""Transpose the card's own melodic MIDI clips into the other eleven keys (docs/RESEARCH_LIBRARY_EXPANSION.md
§2.1 option A, §2.3 rules; phase M1).

The device never transposes (SPEC §6: the key is a label, gen/key.cpp accepts a file iff its label's
pitch-class set equals the project key's), so a clip labelled Amin is only ever heard in A minor. This tool
reads every keyed clip of the chosen categories under lp/midi, shifts it into each of the other eleven
natural-minor pitch-class sets and writes the result into the TARGET key's minor folder only (the picker also
reads the relative-major folder, so writing there too would double the clip):

  <out>/lp/midi/<Category>/<Root>min/<Root>min__<Category>__<origname>-TR<orig label>__<hash8>.mid

  TR<orig label>   marks the file as transposed from the clip labelled <orig label> ("TRCmaj", "TRAmin"), so
                   derived material can be pruned or ratio-limited later (research open question 9)
  <hash8>          the project's hash rule, tools/lp_smf.py: sha1 over the written note bytes, first 8 hex digits

Transposition rules (research §2.3):
  1. shift by the nearest interval t in [-6, +5] semitones between the source label's pitch-class set and the
     target key's;
  2. of t-12, t, t+12 (and ±24, ±36 as a last resort for clips whose own register is far outside their
     category, such as a "Chord" that is really a sub-bass line) keep the one whose clip median pitch lands
     inside the category's measured band after any hard-limit folding, then the one with the fewest folded
     notes, then the smallest register move;
  3. bands (median): Chord 53-69, Lead 48-79, Pad 59-77, Bass 38-59, Fx 45-79 (measured p10-p90 of the
     library's Fx medians); hard limits: top <= 88, bottom >= 36 (>= 28 for Bass). A note still outside the
     hard limits after step 2 is folded by octaves (counted and reported; rare).
  Clips chromatic to any single diatonic set (their pitch classes fit no major scale) are skipped and counted:
  no label-based shift can be right for them. Clips the firmware would cut (longer than 16 beats, more than
  192 notes) are written as the firmware plays them (16-beat window, earliest 192 notes), so the card holds
  what the device would have played anyway. A result whose notes equal an existing file of the target key's
  folders (minor, relative major, NA) or an earlier result of this run is dropped (hash de-dupe).

Usage:
  python3 -I tools/transpose_library.py --midi sdcard/lp/midi --out sdcard/lp_stage/m1
      [--categories Chord,Lead,Pad,Fx] [--cap Bass=200] [--folder-cap 600] [--seed 2026]
      [--dry-run] [--report] [--verify-only] [--keep] [--manifest FILE] [--audition N]

  --categories   folders under lp/midi to transpose (default Chord,Lead,Pad,Fx; Bass is already rich, pass it
                 explicitly, with --cap, if wanted)
  --cap CAT=N    at most N transposed files per target key for that category (seeded subset); default Bass=200
  --folder-cap   a target minor folder (existing files + files already staged + these) stays at or below this
                 many files (default 600; the firmware reaches 12 shards x 64 = 768, the rest is headroom for
                 the clip generator and the vocal work)
  --dry-run      plan and report, write nothing
  --report       print the per-category / per-key table (always on with --dry-run)
  --verify-only  re-check an existing output tree (name parses, label = folder, hash, firmware rules, bands)
  --keep         do not delete <out>/lp/midi/<Category> before writing (existing staged files count toward
                 the folder cap and the de-dupe)
  --manifest     TSV of output -> source, shift, medians, folds
  --audition N   print N output paths spread over categories and keys for a listening pass

Run order: this tool first (without --keep it replaces <out>/lp/midi/<Category> wholesale), then
tools/import_ldrolez.py, which counts the staged TR files toward the folder caps and the de-dupe and only
removes its own earlier LDZ files. The output tree has no shards and no _index.txt: shard_folders.py and
write_indexes.py run after the merge. Stdlib only; run with -I so nothing is imported from the card. Imports tools/lp_smf.py from its own folder.
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

# category -> (median low, median high, hard low, hard high)
BANDS = {"Chord": (53, 69, 36, 88), "Lead": (48, 79, 36, 88), "Pad": (59, 77, 36, 88),
         "Bass": (38, 59, 28, 88), "Fx": (45, 79, 36, 88)}
DEFAULT_CATEGORIES = ["Chord", "Lead", "Pad", "Fx"]
DEFAULT_CAPS = {"Bass": 200}
HASH_RE = re.compile(r"^[0-9a-f]{8}$")
TR_RE = re.compile(r"-TR([A-G]#?(?:min|maj)?)$")


class Source:
    __slots__ = ("cat", "path", "rel", "stem", "label", "root", "minor", "minor_root", "notes", "length",
                 "hash", "median", "pcs", "mid", "truncated")


def source_mid(stem):
    """The <name> field of a library stem: everything between the category and the trailing hash, joined
    with '-' when the stem has more than four fields ('Dmin__Chord__BassDerived__Simple-...__99a2d3ed')."""
    fields = stem.split("__")
    if len(fields) >= 4 and HASH_RE.match(fields[-1]):
        mid = fields[2:-1]
    elif len(fields) >= 3:
        mid = fields[2:]
    else:
        mid = fields[1:]
    return smf.safe_token("-".join(mid)) or "clip"


def scan(midi_root, categories, stats):
    """Read every clip of the categories. Returns (sources per category, existing hashes per (cat, folder),
    existing file counts per (cat, folder))."""
    sources = collections.defaultdict(list)
    existing = collections.defaultdict(set)
    counts = collections.Counter()
    for cat in categories:
        cdir = os.path.join(midi_root, cat)
        if not os.path.isdir(cdir):
            print(f"warning: no folder {cdir}", file=sys.stderr)
            continue
        for folder in sorted(os.listdir(cdir)):
            fdir = os.path.join(cdir, folder)
            if folder.startswith(".") or not os.path.isdir(fdir):
                continue
            for dp, dirs, files in os.walk(fdir):
                dirs[:] = sorted(d for d in dirs if not d.startswith("."))
                for f in sorted(files):
                    if not f.lower().endswith(".mid") or f.startswith("."):
                        continue
                    path = os.path.join(dp, f)
                    try:
                        r = smf.read_smf(path)
                    except (ValueError, OSError) as e:
                        stats[cat, "unreadable"] += 1
                        print(f"warning: cannot read {path}: {e}", file=sys.stderr)
                        continue
                    notes96 = smf.notes_to96(r["notes"], r["ppq"])
                    notes, L, truncated = smf.device_view(notes96, smf.to96(r["end"], r["ppq"]))
                    counts[cat, folder] += 1
                    if notes:
                        existing[cat, folder].add(smf.note_hash(notes))
                    stats[cat, "files"] += 1
                    lab = smf.label_of_filename(f)
                    if lab is None:
                        stats[cat, "unparseable label"] += 1
                        continue
                    root, minor, keyless = lab
                    if keyless:
                        stats[cat, "keyless (NA)"] += 1
                        continue
                    if not notes:
                        stats[cat, "empty"] += 1
                        continue
                    if truncated:
                        stats[cat, "cut by the firmware (>16 beats or >192 notes)"] += 1
                    pcs = frozenset(p % 12 for _, _, p, _ in notes)
                    if not smf.diatonic_roots(pcs):
                        stats[cat, "chromatic (fits no diatonic set), skipped"] += 1
                        continue
                    if smf.minor_pcs(smf.minor_root_of(root, minor)) >= pcs:
                        stats[cat, "diatonic to own label"] += 1
                    else:
                        stats[cat, "diatonic to another set than the label"] += 1
                    s = Source()
                    s.cat, s.path, s.rel, s.stem = cat, path, os.path.relpath(path, midi_root), f[:-4]
                    s.label = f.split("__")[0]
                    s.root, s.minor, s.minor_root = root, minor, smf.minor_root_of(root, minor)
                    s.notes, s.length, s.truncated = notes, L, truncated
                    s.hash = smf.note_hash(notes)
                    s.median = smf.median_pitch(notes)
                    s.pcs = pcs
                    s.mid = source_mid(s.stem)
                    sources[cat].append(s)
    return sources, existing, counts


def shift_and_fold(notes, base, band):
    """Research §2.3 steps 2-3. Among base + 12k (k = 0, ±1, ±2, ±3) pick the shift whose result — after folding any
    note still outside the hard limits by octaves — has its median inside the band, then the fewest folded
    notes, then the smallest register move (the base interval on a tie). Returns (notes, shift, folded)."""
    mlo, mhi, hlo, hhi = band
    best = None
    for k in (0, 1, -1, 2, -2, 3, -3):
        c = base + 12 * k
        out, folded = [], 0
        for s, d, p, v in notes:
            q = p + c
            while q < hlo:
                q += 12
            while q > hhi:
                q -= 12
            folded += q != p + c
            out.append((s, d, q, v))
        med = statistics.median([q for _, _, q, _ in out])
        key = (not (mlo <= med <= mhi), folded, abs(c), abs(k))
        if best is None or key < best[0]:
            best = (key, out, c, folded)
    return best[1], best[2], best[3]


def choose_shift(pitches, base, band):
    """The shift shift_and_fold would pick for these pitches (helper for callers that only need the number)."""
    return shift_and_fold([(0, 1, p, 100) for p in pitches], base, band)[1]


def plan(sources, existing, counts, out_root, categories, caps, folder_cap, seed, keep, stats):
    """Decide every output file. Returns a list of dicts (out_rel, src, shift, notes, length, hash, folds,
    median_out, in_key) and the per-(cat, target) counts of what was trimmed."""
    items = []
    trimmed = collections.Counter()
    for cat in categories:
        band = BANDS.get(cat, BANDS["Lead"])
        for T in smf.MINOR_CIRCLE:
            tfolder = smf.folder_label(T, True)
            visible = set()
            for f in smf.key_folders(T):
                visible |= existing.get((cat, f), set())
            planned_hashes = set()
            cands = []
            for src in sources.get(cat, []):
                if src.minor_root == T:
                    continue                                   # already visible in this key
                base = ((T - src.minor_root + 6) % 12) - 6
                notes, shift, folds = shift_and_fold(src.notes, base, band)
                h = smf.note_hash(notes)
                if h in visible or h in planned_hashes:
                    stats[cat, "de-duplicated (same notes already in the target key)"] += 1
                    continue
                planned_hashes.add(h)
                name = smf.fit_name(tfolder, cat, f"{src.mid}-TR{src.label}", h)
                med_out = smf.median_pitch(notes)
                in_key = smf.minor_pcs(T) >= frozenset(p % 12 for _, _, p, _ in notes)
                cands.append({"out_rel": os.path.join("lp", "midi", cat, tfolder, name), "src": src, "shift": shift,
                              "base": base, "notes": notes, "length": src.length, "hash": h, "folds": folds,
                              "median_out": med_out, "in_key": in_key, "cat": cat, "target": tfolder})
            rng = random.Random(f"{seed}:{cat}:{tfolder}")
            cap = caps.get(cat)
            if cap is not None and len(cands) > cap:
                trimmed[cat, tfolder, "cap"] += len(cands) - cap
                cands = sorted(rng.sample(cands, cap), key=lambda c: c["out_rel"])
            staged = 0
            if keep:
                sdir = os.path.join(out_root, "lp", "midi", cat, tfolder)
                staged = len([f for f in os.listdir(sdir) if f.endswith(".mid")]) if os.path.isdir(sdir) else 0
            allowed = max(0, folder_cap - counts.get((cat, tfolder), 0) - staged)
            if len(cands) > allowed:
                trimmed[cat, tfolder, "folder-cap"] += len(cands) - allowed
                cands = sorted(rng.sample(cands, allowed), key=lambda c: c["out_rel"])
            items.extend(cands)
    return items, trimmed


def write_all(items, out_root, categories, keep):
    if not keep:
        for cat in categories:
            shutil.rmtree(os.path.join(out_root, "lp", "midi", cat), ignore_errors=True)
    total = 0
    for it in items:
        total += smf.write_smf0(os.path.join(out_root, it["out_rel"]), it["notes"], it["length"])
    return total


def report(items, sources, existing, counts, categories, stats, trimmed):
    print("\n== sources")
    for cat in categories:
        keys = sorted(k for (c, k) in stats if c == cat)
        print(f"  {cat}: " + "; ".join(f"{k} {stats[cat, k]}" for k in keys))
    print("\n== per category / key (TR = files this tool adds to the key's minor folder; visible = minor + relative major + NA folders)")
    by = collections.Counter((it["cat"], it["target"]) for it in items)
    for cat in categories:
        print(f"\n  {cat:6s} {'key':6s} {'min-folder':>10s} {'visible':>8s} {'+TR':>6s} {'min-after':>10s} {'vis-after':>10s} {'trimmed':>8s}")
        tot = collections.Counter()
        for T in smf.MINOR_CIRCLE:
            tfolder = smf.folder_label(T, True)
            minf = counts.get((cat, tfolder), 0)
            vis = sum(counts.get((cat, f), 0) for f in smf.key_folders(T))
            add = by[cat, tfolder]
            tr = trimmed[cat, tfolder, "cap"] + trimmed[cat, tfolder, "folder-cap"]
            print(f"  {'':6s} {tfolder:6s} {minf:10d} {vis:8d} {add:6d} {minf + add:10d} {vis + add:10d} {tr:8d}")
            tot.update({"minf": minf, "vis": vis, "add": add, "tr": tr})
        print(f"  {'':6s} {'total':6s} {tot['minf']:10d} {tot['vis']:8d} {tot['add']:6d} {tot['minf'] + tot['add']:10d} {tot['vis'] + tot['add']:10d} {tot['tr']:8d}")
    if items:
        shifts = collections.Counter(it["shift"] for it in items)
        moves = collections.Counter(min(24, abs(int(round(it["median_out"] - it["src"].median)))) for it in items)
        print("\n== shifts (semitones): " + ", ".join(f"{k:+d}:{shifts[k]}" for k in sorted(shifts)))
        print("== |median move| (semitones): " + ", ".join(f"{k}:{moves[k]}" for k in sorted(moves)))
        folds = sum(it["folds"] for it in items)
        print(f"== notes folded by an octave to meet the hard limits: {folds} in {sum(1 for it in items if it['folds'])} files")
        print(f"== outputs fully inside the target key's pitch classes: {sum(it['in_key'] for it in items)} of {len(items)}")
        bars = collections.Counter(it["length"] // smf.PPQN for it in items)
        print("== lengths (beats): " + ", ".join(f"{k}:{bars[k]}" for k in sorted(bars)))
    print(f"\n== total files planned: {len(items)}")


def verify(out_root, categories):
    return smf.verify_tree(out_root, categories, BANDS, smf.DERIVED_RE, "stage")


def audition(items, n, seed):
    rng = random.Random(f"{seed}:audition")
    by_cat = collections.defaultdict(list)
    for it in items:
        by_cat[it["cat"]].append(it)
    picks = []
    cats = [c for c in by_cat if by_cat[c]]
    while len(picks) < n and cats:
        for c in list(cats):
            if len(picks) >= n:
                break
            pool = [it for it in by_cat[c] if it["target"] not in {p["target"] for p in picks if p["cat"] == c}] or by_cat[c]
            it = rng.choice(pool)
            by_cat[c].remove(it)
            picks.append(it)
            if not by_cat[c]:
                cats.remove(c)
    return picks


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--midi", required=True, help="the library's lp/midi folder (read only)")
    ap.add_argument("--out", required=True, help="staging root; files go to <out>/lp/midi/<Category>/<Root>min/")
    ap.add_argument("--categories", default=",".join(DEFAULT_CATEGORIES))
    ap.add_argument("--cap", action="append", default=[], help="CAT=N files per target key (default Bass=200)")
    ap.add_argument("--folder-cap", type=int, default=600)
    ap.add_argument("--seed", default="2026")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--verify-only", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--manifest")
    ap.add_argument("--audition", type=int, default=0)
    args = ap.parse_args(argv)
    categories = [c for c in args.categories.split(",") if c]
    if args.verify_only:
        return 1 if verify(args.out, categories) else 0
    caps = dict(DEFAULT_CAPS)
    for c in args.cap:
        k, _, v = c.partition("=")
        caps[k] = int(v)
    stats = collections.Counter()
    sources, existing, counts = scan(args.midi, categories, stats)
    items, trimmed = plan(sources, existing, counts, args.out, categories, caps, args.folder_cap, args.seed, args.keep, stats)
    if args.report or args.dry_run:
        report(items, sources, existing, counts, categories, stats, trimmed)
    if args.manifest:
        with open(args.manifest, "w") as f:
            f.write("output\tsource\tshift\tmedian_src\tmedian_out\tfolded\tbeats\tnotes\tin_target_key\n")
            for it in sorted(items, key=lambda i: i["out_rel"]):
                f.write(f"{it['out_rel']}\t{it['src'].rel}\t{it['shift']:+d}\t{it['src'].median:g}\t{it['median_out']:g}\t"
                        f"{it['folds']}\t{it['length'] // smf.PPQN}\t{len(it['notes'])}\t{int(it['in_key'])}\n")
    if args.audition:
        print("\n== audition")
        for it in audition(items, args.audition, args.seed):
            print(f"  {it['out_rel']}    <- {it['src'].rel} ({it['shift']:+d} st)")
    if args.dry_run:
        print("dry run: nothing written")
        return 0
    total = write_all(items, args.out, categories, args.keep)
    print(f"written: {len(items)} files, {total / 1024:.1f} KB -> {os.path.join(args.out, 'lp', 'midi')}")
    return 1 if verify(args.out, categories) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
