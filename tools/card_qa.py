#!/usr/bin/env python3
"""Verify a Live Player card image and print its inventory (SPEC §6, §9.3, §9.16, docs/LOOPS.md).

Usage:
  python3 -I tools/card_qa.py sdcard [--quiet] [--no-tables] [--cluster 32768]

MIDI (lp/midi/<Category>/<Key>[/sNN]/*.mid), every file:
  * name: at least 4 `__` fields, <= 95 bytes; the key label (first field, gen/key.cpp rules: `Amin`, `C`, `Cmaj`,
    `NA`) parses and names the folder it sits in (minor `<Root>min`, major bare root, `NA`);
  * parses as an SMF (tools/lp_smf.py reader = the firmware's pairing rules), converted to 96 PPQN;
  * the file itself is <= 16 beats and holds <= 192 notes (a longer or denser source is "firmware-cut": the
    device plays the first 16 beats / 192 notes, SPEC §1.2 — counted and listed, an error for new material);
  * the device view respects the invariants: whole beats 1..16, <= 192 notes, notes inside the loop.
WAV (lp/packs/<Genre>/**/*.wav), every file: RIFF/WAVE, PCM, 22050 Hz, mono, 16-bit, a data chunk.
  vocal/<Key> and loops/<cat>/<Key>: key label = folder; a BPM token the firmware finds (Generator::parseBpmToken,
  60..249); vocal names carry no `loop`/`loops` word (Library::hasLoopToken), loop names carry `LOOP`; a `<n>bars`
  token means exactly n x 42336 frames (LOOPS.md).
Tables: files per category and key folder; the picker's view per minor key (minor folder + relative major folder,
NA apart); vocals and loops the same way; drum roles; total files, bytes, and the on-card size at --cluster bytes
per cluster (FAT32 default 32 KB; `diskutil info` on the card tells the real one).
Exit 1 when any check fails. Stdlib only; run with -I (imports tools/lp_smf.py from its own folder).
"""
import argparse
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lp_smf as smf  # noqa: E402

RATE, BAR_FRAMES, MAX_NAME = 22050, 42336, 95
CATEGORIES = ["Chord", "Lead", "Pad", "Bass", "Fx", "Beat", "Percussion"]
MINOR_ORDER = ["Amin", "A#min", "Bmin", "Cmin", "C#min", "Dmin", "D#min", "Emin", "Fmin", "F#min", "Gmin", "G#min"]


def is_alpha(c): return c.isascii() and c.isalpha()
def is_digit(c): return c.isascii() and c.isdigit()
def is_sep(c): return c in " _-."


def parse_bpm_token(name):
    """Port of Generator::parseBpmToken: digits before or after `bpm`, 1..3 digits, value 60..249."""
    n, low = len(name), name.lower()
    for i in range(n - 2):
        if low[i:i + 3] != "bpm":
            continue
        j = i
        while j > 0 and is_sep(name[j - 1]):
            j -= 1
        end = j
        while j > 0 and is_digit(name[j - 1]):
            j -= 1
        if end > j and end - j <= 3 and not (i + 3 < n and is_alpha(name[i + 3])):
            v = int(name[j:end])
            if 60 <= v <= 249:
                return v
        a = i + 3
        while a < n and is_sep(name[a]):
            a += 1
        b = a
        while b < n and is_digit(name[b]):
            b += 1
        if b > a and b - a <= 3 and not (b < n and is_alpha(name[b])) and not (i > 0 and is_alpha(name[i - 1])):
            v = int(name[a:b])
            if 60 <= v <= 249:
                return v
    return 0


def parse_bars_token(name):
    """Port of Generator::parseBarsToken: digits right before `bar`/`bars`."""
    n, low = len(name), name.lower()
    for i in range(n - 2):
        if low[i:i + 3] != "bar":
            continue
        after = i + 3
        if after < n and low[after] == "s":
            after += 1
        if after < n and is_alpha(name[after]):
            continue
        j = i
        while j > 0 and is_sep(name[j - 1]):
            j -= 1
        end = j
        while j > 0 and is_digit(name[j - 1]):
            j -= 1
        if end == j or end - j > 3:
            continue
        return int(name[j:end])
    return 0


def has_loop_token(name):
    """Port of Library::hasLoopToken: an alphanumeric word `loop` or `loops`."""
    word = []
    for c in name + " ":
        if c.isascii() and c.isalnum():
            word.append(c.lower())
        else:
            if "".join(word) in ("loop", "loops"):
                return True
            word = []
    return False


def canonical_folder(label):
    """Folder a key label belongs in (None when the label is not a key)."""
    k = smf.parse_label(label)
    if k is None:
        return None
    root, minor, keyless = k
    if keyless:
        return "NA"
    return smf.folder_label(root, minor)


def relative_major(minor_folder):
    root = smf.NOTE_NAMES.index(minor_folder[:-3])
    return smf.NOTE_NAMES[(root + 3) % 12]


def shard_free(rel_parts):
    """Drop a trailing sNN shard folder from a relative folder path."""
    if rel_parts and len(rel_parts[-1]) == 3 and rel_parts[-1][0] == "s" and rel_parts[-1][1:].isdigit():
        return rel_parts[:-1]
    return rel_parts


def read_wav_header(path):
    """(rate, channels, bits, frames) of a RIFF/WAVE PCM file; raises ValueError."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise ValueError("not RIFF/WAVE")
        fmt = None
        data_len = None
        pos = 12
        size = os.path.getsize(path)
        while pos + 8 <= size:
            f.seek(pos)
            ch = f.read(8)
            if len(ch) < 8:
                break
            cid, clen = ch[:4], struct.unpack("<I", ch[4:8])[0]
            if cid == b"fmt ":
                body = f.read(16)
                tag, chans, rate, _, _, bits = struct.unpack("<HHIIHH", body)
                if tag not in (1, 0xFFFE):
                    raise ValueError("not PCM (format tag %d)" % tag)
                fmt = (rate, chans, bits)
            elif cid == b"data":
                data_len = min(clen, size - pos - 8)
                break
            pos += 8 + clen + (clen & 1)
        if fmt is None or data_len is None:
            raise ValueError("missing fmt or data chunk")
        rate, chans, bits = fmt
        frames = data_len // max(1, chans * bits // 8)
        return rate, chans, bits, frames


def check_midi(root, problems, info):
    midi = os.path.join(root, "lp", "midi")
    counts = defaultdict(Counter)       # category -> key folder -> files
    bytes_total = 0
    files = cut_len = cut_notes = parse_fail = 0
    name_bad = 0
    cat_field_mismatch = 0
    lengths = Counter()
    cut_list = []
    for dp, dirs, fs in os.walk(midi):
        dirs.sort()
        rel = os.path.relpath(dp, midi).replace(os.sep, "/").split("/")
        for f in sorted(fs):
            if not f.lower().endswith(".mid") or f.startswith("."):
                continue
            path = os.path.join(dp, f)
            files += 1
            bytes_total += os.path.getsize(path)
            parts = shard_free(rel)
            if len(parts) != 2:
                problems.append("midi file outside <Category>/<Key>: %s" % path)
                continue
            cat, key = parts
            counts[cat][key] += 1
            fields = f[:-4].split("__")
            if len(f.encode("utf-8")) > MAX_NAME:
                problems.append("name longer than %d bytes: %s" % (MAX_NAME, path)); name_bad += 1
            if len(fields) < 4:
                problems.append("name has %d `__` fields (need >= 4): %s" % (len(fields), path)); name_bad += 1
            else:
                folder = canonical_folder(fields[0])
                if folder is None:
                    problems.append("key label does not parse: %s" % path); name_bad += 1
                elif folder != key:
                    problems.append("key label %s (-> %s) in folder %s: %s" % (fields[0], folder, key, path)); name_bad += 1
                if fields[1] != cat:
                    cat_field_mismatch += 1
            try:
                s = smf.read_smf(path)
            except Exception as e:  # noqa: BLE001
                problems.append("MIDI does not parse (%s): %s" % (e, path)); parse_fail += 1
                continue
            notes96 = smf.notes_to96(s["notes"], s["ppq"])
            eot96 = smf.to96(s["end"], s["ppq"])
            raw_len = smf.pattern_length(notes96, eot96) if notes96 else smf.ceil_beats(eot96)
            if raw_len > smf.MAX_TICKS or any(st >= smf.MAX_TICKS for st, _, _, _ in notes96):
                cut_len += 1
                cut_list.append(path)
            if len(notes96) > smf.MAX_NOTES:
                cut_notes += 1
                if path not in cut_list:
                    cut_list.append(path)
            kept, L, _ = smf.device_view(notes96, eot96)
            lengths[L // smf.PPQN] += 1
            if L % smf.PPQN or L < smf.PPQN or L > smf.MAX_TICKS or len(kept) > smf.MAX_NOTES:
                problems.append("device view breaks the invariants (len %d, notes %d): %s" % (L, len(kept), path))
            for st, d, p, v in kept:
                if st >= L or p > 127 or d < 1:
                    problems.append("device view note outside the loop: %s" % path)
                    break
    info["midi"] = dict(files=files, bytes=bytes_total, parse_fail=parse_fail, cut_len=cut_len, cut_notes=cut_notes,
                        name_bad=name_bad, cat_field_mismatch=cat_field_mismatch, lengths=lengths, cut_list=cut_list)
    return counts


def check_wavs(root, problems, info, excluded=()):
    packs = os.path.join(root, "lp", "packs")
    vocal = defaultdict(Counter)        # genre -> key folder -> files
    loops = defaultdict(Counter)        # genre/category -> key -> files
    drums = defaultdict(Counter)        # genre -> role -> files
    other = Counter()                   # genre/kind -> files
    files = bytes_total = fmt_bad = token_bad = 0
    bars = Counter()
    for dp, dirs, fs in os.walk(packs):
        if dp == packs:
            dirs[:] = [d for d in dirs if d not in excluded]
        dirs.sort()
        rel = os.path.relpath(dp, packs).replace(os.sep, "/").split("/")
        for f in sorted(fs):
            if not f.lower().endswith(".wav") or f.startswith("."):
                continue
            path = os.path.join(dp, f)
            files += 1
            bytes_total += os.path.getsize(path)
            parts = shard_free(rel)
            genre = parts[0]
            kind = parts[1] if len(parts) > 1 else "?"
            try:
                rate, chans, bits, frames = read_wav_header(path)
                if (rate, chans, bits) != (RATE, 1, 16):
                    problems.append("WAV is %d Hz / %d ch / %d bit, need 22050 / 1 / 16: %s" % (rate, chans, bits, path)); fmt_bad += 1
            except Exception as e:  # noqa: BLE001
                problems.append("WAV does not parse (%s): %s" % (e, path)); fmt_bad += 1
                frames = None
            if len(f.encode("utf-8")) > MAX_NAME:
                problems.append("name longer than %d bytes: %s" % (MAX_NAME, path))
            if kind == "vocal" and len(parts) >= 3:
                key = parts[2]
                vocal[genre][key] += 1
                label = f.split("__")[0]
                if canonical_folder(label) != key:
                    problems.append("vocal label %s in folder %s: %s" % (label, key, path)); token_bad += 1
                if not parse_bpm_token(f):
                    problems.append("vocal without a BPM token: %s" % path); token_bad += 1
                if has_loop_token(f):
                    problems.append("vocal name carries a loop token: %s" % path); token_bad += 1
                nb = parse_bars_token(f)
                if nb:
                    bars[nb] += 1
                    if frames is not None and frames != nb * BAR_FRAMES:
                        problems.append("%dbars token but %d frames (want %d): %s" % (nb, frames, nb * BAR_FRAMES, path)); token_bad += 1
            elif kind == "loops" and len(parts) >= 4:
                cat, key = parts[2], parts[3]
                loops[genre + "/" + cat][key] += 1
                label = f.split("__")[0]
                if canonical_folder(label) != key:
                    problems.append("loop label %s in folder %s: %s" % (label, key, path)); token_bad += 1
                if not parse_bpm_token(f):
                    problems.append("loop without a BPM token: %s" % path); token_bad += 1
                if not has_loop_token(f):
                    problems.append("loop name without the LOOP token: %s" % path); token_bad += 1
                nb = parse_bars_token(f)
                if not nb:
                    problems.append("loop without a bars token: %s" % path); token_bad += 1
                elif frames is not None and frames != nb * BAR_FRAMES:
                    problems.append("%dbars token but %d frames (want %d): %s" % (nb, frames, nb * BAR_FRAMES, path)); token_bad += 1
            elif kind == "drum" and len(parts) >= 3:
                drums[genre][parts[2]] += 1
            else:
                other[genre + "/" + kind] += 1
    info["wav"] = dict(files=files, bytes=bytes_total, fmt_bad=fmt_bad, token_bad=token_bad, bars=bars)
    return vocal, loops, drums, other


def picker_view(counts):
    """Per minor key: files in the minor folder + the relative major folder; NA apart."""
    rows = []
    for m in MINOR_ORDER:
        maj = relative_major(m)
        rows.append((m, counts.get(m, 0), maj, counts.get(maj, 0), counts.get(m, 0) + counts.get(maj, 0)))
    return rows, counts.get("NA", 0)


def print_table(title, counts_by_cat, order=None):
    cats = order or sorted(counts_by_cat)
    cats = [c for c in cats if c in counts_by_cat] + [c for c in sorted(counts_by_cat) if c not in cats]
    print("\n%s (picker's view: minor + relative major folder; NA apart)" % title)
    head = "%-7s" % "key" + "".join("%12s" % c for c in cats)
    print(head)
    for m in MINOR_ORDER:
        cells = []
        for c in cats:
            rows, _ = picker_view(counts_by_cat[c])
            r = next(x for x in rows if x[0] == m)
            cells.append("%5d+%-5d" % (r[1], r[3]) if r[3] else "%5d      " % r[1])
        print("%-7s" % m + "".join("%12s" % x for x in cells))
    print("%-7s" % "NA" + "".join("%12d" % counts_by_cat[c].get("NA", 0) for c in cats))
    print("%-7s" % "total" + "".join("%12d" % sum(counts_by_cat[c].values()) for c in cats))
    print("%-7s" % "mean" + "".join("%12.1f" % (sum(r[4] for r in picker_view(counts_by_cat[c])[0]) / 12.0) for c in cats))
    print("%-7s" % "min-max" + "".join("%12s" % ("%d-%d" % (min(r[4] for r in picker_view(counts_by_cat[c])[0]), max(r[4] for r in picker_view(counts_by_cat[c])[0]))) for c in cats))
    print("%-7s" % "maxfold" + "".join("%12d" % max(counts_by_cat[c].values(), default=0) for c in cats))


def card_size(root, cluster):
    files = nbytes = on_card = 0
    for dp, _, fs in os.walk(os.path.join(root, "lp")):
        for f in fs:
            if f.startswith("."):
                continue
            s = os.path.getsize(os.path.join(dp, f))
            files += 1
            nbytes += s
            on_card += ((s + cluster - 1) // cluster) * cluster if s else 0
    return files, nbytes, on_card


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("root", help="card root (the folder that holds lp/)")
    ap.add_argument("--quiet", action="store_true", help="print at most 20 problems")
    ap.add_argument("--no-tables", action="store_true")
    ap.add_argument("--cluster", type=int, default=32768, help="FAT cluster size for the on-card estimate (default 32768)")
    ap.add_argument("--exclude-genre", action="append", default=None,
                    help="pack folders left out of the WAV checks and tables (default: Testpack, the host tests' fixtures)")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, "lp")):
        sys.exit("no lp/ under %s" % root)
    excluded = set(args.exclude_genre if args.exclude_genre is not None else ["Testpack"])
    problems, info = [], {}
    midi_counts = check_midi(root, problems, info)
    vocal, loops, drums, other = check_wavs(root, problems, info, excluded)
    if excluded:
        print("(pack folders excluded from the WAV checks: %s)" % ", ".join(sorted(excluded)))
    m, w = info["midi"], info["wav"]
    if not args.no_tables:
        print_table("MIDI files per category and key", midi_counts, CATEGORIES)
        for genre in sorted(vocal):
            print_table("Vocal files per key (%s)" % genre, {"vocal": vocal[genre]})
        if loops:
            print_table("Loop files per category and key", loops)
        for genre in sorted(drums):
            print("\nDrum one-shots (%s): " % genre + ", ".join("%s %d" % kv for kv in sorted(drums[genre].items())))
        if other:
            print("Other WAV folders: " + ", ".join("%s %d" % kv for kv in sorted(other.items())))
    print("\nMIDI: %d files, %.2f MB, lengths (beats): %s" % (m["files"], m["bytes"] / 1048576,
          ", ".join("%d: %d" % kv for kv in sorted(m["lengths"].items()))))
    print("  parse failures %d, firmware-cut sources (> 16 beats) %d, (> 192 notes) %d, name problems %d, category field != folder %d (library convention, e.g. BassDerived)"
          % (m["parse_fail"], m["cut_len"], m["cut_notes"], m["name_bad"], m["cat_field_mismatch"]))
    print("WAV: %d files, %.1f MB, format problems %d, token problems %d, bars tokens: %s"
          % (w["files"], w["bytes"] / 1048576, w["fmt_bad"], w["token_bad"], ", ".join("%d bars: %d" % kv for kv in sorted(w["bars"].items()))))
    files, nbytes, on_card = card_size(root, args.cluster)
    print("CARD: %d files under lp/, %.1f MB of bytes, about %.0f MB on a card with %d-byte clusters"
          % (files, nbytes / 1048576, on_card / 1048576, args.cluster))
    if problems:
        shown = problems[:20] if args.quiet else problems
        print("\n%d problems:" % len(problems))
        for p in shown:
            print("  " + p)
        if len(shown) < len(problems):
            print("  ... %d more" % (len(problems) - len(shown)))
    cut_new = [p for p in m["cut_list"] if "-TR" in os.path.basename(p) or "__GEN-" in os.path.basename(p) or "__LDZ-" in os.path.basename(p)]
    if cut_new:
        print("\n%d NEW (TR/GEN/LDZ) files are firmware-cut — they should be whole:" % len(cut_new))
        for p in cut_new[:20]:
            print("  " + p)
    print("\nQA: %s" % ("ok" if not problems and not cut_new else "FAILED (%d problems, %d cut new files)" % (len(problems), len(cut_new))))
    return 1 if (problems or cut_new) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
