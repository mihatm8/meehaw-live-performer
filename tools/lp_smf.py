#!/usr/bin/env python3
"""Shared stdlib SMF helpers for the Live Player content tools (transpose_library.py, import_ldrolez.py).

Reader: any type-0/1 SMF; tracks are merged, note-ons are paired FIFO per (pitch, channel), then per pitch,
as firmware/liveplayer/src/gen/smf.cpp and tests/refmodel_smf.py do; notes come back at the file's own PPQN.
`to96` converts them to kPPQN = 96 (core/config.h) with the firmware's rounding, `device_view` applies the
firmware's pattern rules (16-beat window, 192 notes, whole-beat length with the trailing-silence rule), and
`write_smf0` writes a type-0 file at 96 PPQN with note-offs before note-ons at the same tick and the
end-of-track at the pattern length, so the firmware derives exactly that length.

HASH RULE — the `__<hash8>` suffix of every MIDI file the Mac tools write (SPEC §6 names it, no upstream
tool reproduces the REAPER library's own values):

    hash8 = sha1( b"LPNOTES1" + pack(">H", 96)
                  + b"".join(pack(">IIBB", start, dur, pitch, vel) for every note,
                             sorted by (start, pitch, dur, vel)) ).hexdigest()[:8]

with start and dur in 96-PPQN ticks exactly as written to the file and vel 1..127 as written. Only the notes
count: tempo, track names and the end-of-track time are not hashed, so a re-export that keeps the notes keeps
the hash, and two files with the same notes collide — which is how the tools de-duplicate against the
library and against each other. A file at another PPQN is converted to 96 first (nearest tick, the
firmware's conversion), so the hash of an existing library file is the hash of the notes the device plays.
`note_hash()` below is the reference implementation; `python3 -I tools/transpose_library.py --verify` and
`tools/import_ldrolez.py --verify` recompute it for every file they produced.

Key labels follow gen/key.cpp: text before the first "__"; "Amin" minor, "C" or "Cmaj" major, sharps only on
the card (a flat such as "Bb" is read but never written), "NA" keyless. Folder names are the canonical labels
of SPEC §6: minor "<Root>min", major the bare root "<Root>".
"""
import hashlib
import os
import re
import statistics
import struct

PPQN = 96                      # kPPQN
MAX_TICKS = 16 * PPQN          # kMaxPatternTicks (16 beats)
MAX_NOTES = 192                # kMaxNotes
NOTE_UNIT = 12                 # kNoteLenUnit
MAX_NAME = 95                  # kMaxNameLen - 1
TRAILING_BEATS = 2             # smf.cpp kTrailingBeats

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
FLAT_TO_SHARP = {"DB": "C#", "EB": "D#", "GB": "F#", "AB": "G#", "BB": "A#", "CB": "B", "FB": "E"}
MINOR_CIRCLE = [9, 4, 11, 6, 1, 8, 3, 10, 5, 0, 7, 2]          # key.cpp kMinorCircle: A E B F# C# G# D# A# F C G D
MAJOR_SET = (0, 2, 4, 5, 7, 9, 11)
MINOR_INTERVALS = (0, 2, 3, 5, 7, 8, 10)
# derivation tokens of card-only / imported material (research §6): TR<label> transposed, LDZ ldrolez, GEN generated
DERIVED_RE = re.compile(r"^(?:LDZ|GEN)-|-TR[A-G]#?(?:min|maj)?$")


# ----------------------------------------------------------------------------------------------- reading

def _vlq(b, i):
    v = 0
    for _ in range(8):
        c = b[i]
        i += 1
        v = (v << 7) | (c & 0x7F)
        if c < 0x80:
            return v, i
    raise ValueError("bad variable-length quantity")


def read_smf(src):
    """src: path or bytes. Returns dict(ppq, fmt, notes, end, bpm) with notes as (start, dur, pitch, vel) in the
    file's own ticks (dur >= 1) and `end` the latest end-of-track (or last event) tick. Raises ValueError on a
    file that is not an SMF with a ticks-per-beat division."""
    b = open(src, "rb").read() if isinstance(src, str) else src
    if b[:4] != b"MThd" or len(b) < 14:
        raise ValueError("not an SMF")
    hl = struct.unpack(">I", b[4:8])[0]
    fmt, ntr, div = struct.unpack(">HHH", b[8:14])
    if div == 0 or div & 0x8000:
        raise ValueError("SMPTE or zero division")
    i = 8 + hl
    notes, bpm, end, tracks = [], 0.0, 0, 0
    while i + 8 <= len(b) and tracks < ntr:
        cid = b[i:i + 4]
        tl = struct.unpack(">I", b[i + 4:i + 8])[0]
        i += 8
        tend = min(i + tl, len(b))
        if cid != b"MTrk":
            i = tend
            continue
        tracks += 1
        tm, status, active = 0, 0, []       # active: [pitch, channel, start, vel]
        while i < tend:
            try:
                d, i = _vlq(b, i)
                tm += d
                s = b[i]
                if s < 0x80:
                    if status == 0:
                        break
                    s = status
                else:
                    i += 1
                if s == 0xFF:
                    ty = b[i]; i += 1
                    ln, i = _vlq(b, i)
                    data = b[i:i + ln]; i += ln
                    if ty == 0x2F:
                        end = max(end, tm)
                        break
                    if ty == 0x51 and ln == 3 and bpm == 0 and int.from_bytes(data, "big"):
                        bpm = 60e6 / int.from_bytes(data, "big")
                elif s in (0xF0, 0xF7):
                    ln, i = _vlq(b, i); i += ln
                elif s >= 0xF0:
                    i += {0xF1: 1, 0xF2: 2, 0xF3: 1}.get(s, 0)
                else:
                    status = s
                    kind, ch = s & 0xF0, s & 0x0F
                    d1 = b[i]; i += 1
                    if d1 >= 0x80:
                        break
                    if kind in (0xC0, 0xD0):
                        continue
                    d2 = b[i]; i += 1
                    if d2 >= 0x80:
                        break
                    if kind == 0x90 and d2 > 0:
                        active.append([d1, ch, tm, d2])
                    elif kind in (0x80, 0x90):
                        idx = next((j for j, a in enumerate(active) if a[0] == d1 and a[1] == ch), None)
                        if idx is None:
                            idx = next((j for j, a in enumerate(active) if a[0] == d1), None)
                        if idx is not None:
                            p, _, st, v = active.pop(idx)
                            notes.append((st, max(1, tm - st), p, v))
            except (IndexError, ValueError):
                break
        for p, _, st, v in active:                      # hanging notes end with the track
            notes.append((st, max(1, tm - st), p, v))
        end = max(end, tm)
        i = tend
    if tracks == 0:
        raise ValueError("no track")
    notes.sort()
    return {"ppq": div, "fmt": fmt, "notes": notes, "end": end, "bpm": bpm}


def to96(tick, ppq):
    """The firmware's tick conversion (smf.cpp to96): nearest 96-PPQN tick."""
    if tick <= 0:
        return 0
    return (tick * PPQN + ppq // 2) // ppq


def notes_to96(notes, ppq):
    out = []
    for s, d, p, v in notes:
        s96 = to96(s, ppq)
        e96 = to96(s + d, ppq)
        out.append((s96, max(1, e96 - s96), p, v))
    out.sort()
    return out


def ceil_beats(t):
    return max(PPQN, ((t + PPQN - 1) // PPQN) * PPQN)


def pattern_length(notes96, eot96):
    """smf.cpp length rule before the 16-beat cut: whole beats, the end-of-track extends the loop when it
    lies at most kTrailingBeats beyond the last note end."""
    max_end = max((s + d for s, d, _, _ in notes96), default=0)
    if eot96 > max_end and eot96 - max_end <= TRAILING_BEATS * PPQN:
        return ceil_beats(eot96)
    return ceil_beats(max_end)


def device_view(notes96, eot96):
    """What the firmware plays from these notes: (notes, length_ticks, truncated). Notes starting at or after
    the 16-beat window are dropped, ends are clipped to it, the earliest kMaxNotes are kept (stable), ends are
    clipped to the pattern length. Notes come back sorted."""
    truncated = False
    L = pattern_length(notes96, eot96)
    if L > MAX_TICKS:
        L = MAX_TICKS
        truncated = True
    kept = []
    for s, d, p, v in sorted(notes96):
        if s >= MAX_TICKS:
            truncated = True
            continue
        e = min(s + d, L)
        if e <= s:
            e = s + 1
        if s + d > L:
            truncated = truncated or (s + d > MAX_TICKS)
        kept.append((s, e - s, p, v))
    if len(kept) > MAX_NOTES:
        kept = kept[:MAX_NOTES]
        truncated = True
    return kept, L, truncated


# ----------------------------------------------------------------------------------------------- writing

def _vlq_bytes(v):
    out = bytearray([v & 0x7F])
    v >>= 7
    while v:
        out.insert(0, 0x80 | (v & 0x7F))
        v >>= 7
    return bytes(out)


def smf0_bytes(notes96, length_ticks, bpm=None, channel=0):
    """Type-0 SMF at 96 PPQN. notes: (start, dur, pitch, vel). Note-offs sort before note-ons at the same tick;
    the end-of-track sits at length_ticks (never before the last note-off)."""
    events = []
    for s, d, p, v in notes96:
        v = max(1, min(127, int(v)))
        events.append((s, 1, p, v))
        events.append((s + max(1, d), 0, p, 0))
    events.sort(key=lambda e: (e[0], e[1], e[2]))
    tr = bytearray()
    if bpm:
        tr += _vlq_bytes(0) + b"\xff\x51\x03" + int(round(60e6 / bpm)).to_bytes(3, "big")
    t = 0
    for tick, on, p, v in events:
        tr += _vlq_bytes(tick - t)
        tr += bytes([(0x90 if on else 0x80) | (channel & 0x0F), p & 0x7F, v & 0x7F])
        t = tick
    end = max(length_ticks, t)
    tr += _vlq_bytes(end - t) + b"\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN) + b"MTrk" + struct.pack(">I", len(tr)) + bytes(tr)


def write_smf0(path, notes96, length_ticks, bpm=None):
    data = smf0_bytes(notes96, length_ticks, bpm)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return len(data)


# ----------------------------------------------------------------------------------------------- hash

def note_hash(notes96):
    """The project's hash rule (module docstring): 8 hex digits of sha1 over the sorted note bytes."""
    h = hashlib.sha1(b"LPNOTES1" + struct.pack(">H", PPQN))
    for s, p, d, v in sorted((s, p, d, v) for s, d, p, v in notes96):
        h.update(struct.pack(">IIBB", s, d, p & 0x7F, max(1, min(127, int(v)))))
    return h.hexdigest()[:8]


# ----------------------------------------------------------------------------------------------- keys

def parse_label(label):
    """gen/key.cpp parseLibraryLabel. Returns (root_pc, minor, keyless) or None when the label is not a key.
    "NA" -> (None, False, True)."""
    s = label.strip().upper()
    if not s:
        return None
    if s == "NA":
        return (None, False, True)
    root = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}.get(s[0])
    if root is None:
        return None
    rest = s[1:]
    if rest[:1] in ("#", "B"):
        root = (root + (1 if rest[0] == "#" else -1)) % 12
        rest = rest[1:]
    minor = False
    if rest.startswith("MIN"):
        minor, rest = True, rest[3:]
    elif rest.startswith("MAJ"):
        rest = rest[3:]
    if rest and not rest[0].isdigit() and not any(rest.startswith(x) for x in ("ADD", "SUS", "DIM", "AUG", "DOM")):
        return None
    return (root, minor, False)


def label_of_filename(name):
    base = os.path.basename(name)
    sep = base.find("__")
    if sep <= 0:
        return None
    return parse_label(base[:sep])


def minor_root_of(root_pc, minor):
    """Identity of a label's pitch-class set as a natural-minor root (key.cpp accepts: minor at the root or
    major a minor third up)."""
    return root_pc if minor else (root_pc + 9) % 12


def folder_label(root_pc, minor):
    """Canonical card folder / label (SPEC §6): 'Amin' or the bare major root 'C'."""
    return NOTE_NAMES[root_pc % 12] + ("min" if minor else "")


def key_folders(minor_root):
    """The folders the picker reads for a project key: minor label, relative major, NA."""
    return [folder_label(minor_root, True), folder_label((minor_root + 3) % 12, False), "NA"]


def minor_pcs(minor_root):
    return frozenset((minor_root + x) % 12 for x in MINOR_INTERVALS)


def diatonic_roots(pcs):
    """Major-scale roots whose pitch-class set contains every pc in pcs (empty: chromatic, no single set)."""
    return [r for r in range(12) if all((p - r) % 12 in MAJOR_SET for p in pcs)]


def median_pitch(notes):
    return statistics.median([p for _, _, p, _ in notes])


def safe_token(s, keep="#"):
    """Characters allowed inside a name field: letters, digits, '-' and the kept extras; runs of anything
    else become one '-'. Never produces '__' (the field separator) or leading/trailing '-'."""
    out, dash = [], False
    for ch in s:
        if ch.isalnum() or ch in keep:
            out.append(ch)
            dash = False
        elif not dash:
            out.append("-")
            dash = True
    return "".join(out).strip("-")


def fit_name(label, category, mid, hash8, limit=MAX_NAME):
    """'<label>__<category>__<mid>__<hash8>.mid' cut inside `mid` at a '-' so it stays within `limit` bytes."""
    fixed = len(f"{label}__{category}____{hash8}.mid".encode())
    room = limit - fixed
    if len(mid.encode()) > room:
        cut = mid.encode()[:room].decode("utf-8", "ignore")
        if "-" in cut[room // 2:]:
            cut = cut[:cut.rfind("-")]
        mid = cut.strip("-")
    return f"{label}__{category}__{mid}__{hash8}.mid"


# ----------------------------------------------------------------------------------------------- QA

def verify_tree(out_root, categories, bands, token_re, label=""):
    """Re-read every <out_root>/lp/midi/<Category>/<Key>/*.mid and check the contract: four name fields,
    <= 95 bytes, label parses and equals its folder (SPEC §6 canonical form), category field = folder, the
    derivation token (token_re on the name field), 96 PPQN type 0, the firmware would not cut it (16 beats,
    192 notes, whole-beat length, every note inside the loop), the hash field equals note_hash(), every pitch
    inside the category's hard limits and the clip median inside its band. Prints FAIL lines and a summary;
    returns the number of failing files."""
    fails = n = in_key = 0
    for cat in categories:
        cdir = os.path.join(out_root, "lp", "midi", cat)
        band = bands.get(cat, bands["Lead"])
        if not os.path.isdir(cdir):
            continue
        for folder in sorted(os.listdir(cdir)):
            fdir = os.path.join(cdir, folder)
            if not os.path.isdir(fdir):
                continue
            lab = parse_label(folder)
            for f in sorted(os.listdir(fdir)):
                if not f.endswith(".mid"):
                    continue
                n += 1
                problems = []
                fields = f[:-4].split("__")
                if len(fields) != 4:
                    problems.append(f"{len(fields)} fields")
                if len(f.encode()) > MAX_NAME:
                    problems.append("name too long")
                flab = parse_label(fields[0])
                if (lab is None or flab is None or lab[2] or flab != lab or folder_label(*lab[:2]) != folder
                        or fields[0] != folder):
                    problems.append(f"label {fields[0]} vs folder {folder}")
                if len(fields) >= 2 and fields[1] != cat:
                    problems.append(f"category field {fields[1]}")
                if len(fields) >= 3 and not token_re.search(fields[2]):
                    problems.append("no derivation token")
                try:
                    r = read_smf(os.path.join(fdir, f))
                except ValueError as e:
                    print(f"FAIL {cat}/{folder}/{f}: unreadable ({e})")
                    fails += 1
                    continue
                if r["ppq"] != PPQN or r["fmt"] != 0:
                    problems.append(f"ppq {r['ppq']} fmt {r['fmt']}")
                notes = r["notes"]
                _, L, truncated = device_view(notes, r["end"])
                if truncated or len(notes) > MAX_NOTES:
                    problems.append("firmware would cut it")
                if L % PPQN or not PPQN <= L <= MAX_TICKS:
                    problems.append(f"length {L}")
                if any(s >= L or s + d > L for s, d, _, _ in notes):
                    problems.append("note outside the loop")
                if len(fields) == 4 and note_hash(notes) != fields[3]:
                    problems.append(f"hash {note_hash(notes)} != {fields[3]}")
                pitches = [p for _, _, p, _ in notes]
                if not pitches:
                    problems.append("no notes")
                else:
                    if min(pitches) < band[2] or max(pitches) > band[3]:
                        problems.append(f"pitch {min(pitches)}..{max(pitches)} outside {band[2]}..{band[3]}")
                    if not band[0] <= statistics.median(pitches) <= band[1]:
                        problems.append(f"median {statistics.median(pitches)} outside {band[0]}..{band[1]}")
                    if lab and not lab[2] and minor_pcs(minor_root_of(*lab[:2])) >= frozenset(p % 12 for p in pitches):
                        in_key += 1
                if problems:
                    fails += 1
                    print(f"FAIL {cat}/{folder}/{f}: " + "; ".join(problems))
    print(f"verify{(' ' + label) if label else ''}: {n} files, {fails} failures, {in_key} fully inside their label's pitch classes")
    return fails
