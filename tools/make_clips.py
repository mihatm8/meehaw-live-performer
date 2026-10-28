#!/usr/bin/env python3
"""Procedural clip generator for the Live Player library (docs/CLIP_GENERATOR.md): Chord / Lead / Pad (research M2)
and, for the free card (docs/SHARING.md), Bass / Fx per key and the Beat / Percussion drum patterns.

Writes seeded, deterministic 2- and 4-bar clips into the card layout:

  <out>/lp/midi/<Category>/<Key>min/<Key>min__<Category>__GEN-<style>-<seed>__<hash8>.mid   tonal categories
  <out>/lp/midi/<Category>/NA/NA__<Category>__GEN-<style>-<seed>__<hash8>.mid               Beat, Percussion

  Category  Chord, Lead, Pad, Bass, Fx (tonal: a folder per minor key) or Beat, Percussion (keyless: NA, SPEC §9.9)
  Key       the 12 minor labels of SPEC §6 (Amin, A#min, ... sharps only); every pitch is in that natural
            minor scale apart from a rare borrowed leading tone / raised sixth / dominant third (counted and reported)
  GEN       the token that marks generated material so it can be ratio-limited or pruned (research Q9)
  style     the generator's recipe (rhythm template, chord kind, form ...) in [A-Za-z0-9-]
  seed      the draw number; (Category, Key, seed) seeds the RNG, so a clip is reproducible on its own
  hash8     Chord / Lead / Pad: the first 8 hex digits of this tool's original content hash, sha1 over the loop
            length (u16 BE) followed by every note as (start u16, length u16, pitch u8, velocity u8) big-endian in
            canonical order (start, pitch, length, velocity) — kept as it is, the clips on the card carry it.
            Bass / Fx / Beat / Percussion (added 2026-10-27): the project's shared rule of tools/lp_smf.py
            (note_hash: sha1("LPNOTES1" + u16 96 + every note as (start u32, length u32, pitch u8, velocity u8)
            sorted by start, pitch, length, velocity), the rule every other content tool writes.
            Exact duplicates are detected on the full content digest in both cases.

Beat clips follow the firmware's note contract (SPEC §9.6, engine.cpp): 36 kick, 37 snare, 38 closed hat, 39 open
hat, one accent pitch in 40..58 (played on the open-hat slot); the four layers Kick / Kick+Hats / +Snare / Full are
the firmware's (gen/variants.cpp beatLayer). Percussion clips use the four voices 36..39 (the firmware folds any
pitch set onto them). The drum templates live in tools/theory_data.py section 3, the bass templates are the theory
generator's BASS_RHYTHMS.

Every clip obeys the firmware's pattern rules (core/config.h, gen/smf.cpp): type-0 SMF at 96 PPQN, exactly 2 or
4 bars of 4/4 (a note sounds in the last 2 beats so smfParse keeps the whole loop), <= 192 notes, note lengths
in 12-tick units, an onset at tick 0 (the crop variants are never empty), pitches inside the category's band.
The style tables are fitted to the measurements of the existing library (scratchpad research, 2026-10-16;
the numbers are quoted next to each table and in docs/CLIP_GENERATOR.md).

Usage:
  python3 -I tools/make_clips.py --out sdcard/lp_stage/m2 [--keys all|Amin,Emin,...]
          [--counts Lead=150,Pad=120,Chord=60,Bass=60,Fx=20,Beat=240,Percussion=120] [--seed0 1]
          [--max-draw-factor 8] [--no-humanise] [--keep] [--json report.json] [--quiet]
  python3 -I tools/make_clips.py --out sdcard/lp_free --counts Chord=0,Lead=0,Pad=0     # the free card's additions only
  (counts are per key folder; Beat and Percussion have one folder, NA, so their count is the total)
  python3 -I tools/make_clips.py --out sdcard/lp_stage/m2 --check-only [--json report.json]

The QA pass (always run after generating, alone with --check-only) parses every file back with the firmware's
rule model (tests/refmodel_smf.py), checks the name, the folder, the hash, the length, the note cap, the bands
and the key, counts exact duplicates, and prints the style statistics next to the library's measured bands.

Stdlib only; run with -I so nothing is imported from the data folders.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import random
import re
import shutil
import struct
import sys
from collections import Counter, defaultdict

# The theory tables are shared with tools/make_theory_tables.py (the firmware's theory_tables.hpp, SPEC §9.24): one
# source of truth. -I keeps the script directory off sys.path, so it is added here (the tools folder, not the data).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from theory_data import PROGRESSIONS, ROMAN, CHORD_KINDS, CHORD_KIND_W, CHORD_RHYTHMS, LEAD_RHYTHMS, LEAD_MOVES, LEAD_HOME  # noqa: E402
from theory_data import BASS_RHYTHMS, BEAT_STYLES, BEAT_TEMPLATES, BEAT_ACCENTS, BEAT_FILLS, PERC_STYLES, PERC_TEMPLATES  # noqa: E402
import lp_smf  # noqa: E402  (note_hash: the shared hash rule of the categories added for the free card)

PPQN = 96
BAR = 4 * PPQN
UNIT = 12                        # kNoteLenUnit: note lengths are stored in 12-tick units
STEP = PPQN // 4                 # a 16th = 24 ticks
MAX_NOTES = 192                  # kMaxNotes
MAX_TICKS = 16 * PPQN            # kMaxPatternBeats
TRAILING = 2 * PPQN              # smf.cpp kTrailingBeats: a longer silent tail shortens the loop
MAX_NAME = 95                    # write_indexes.py MAX_NAME (kMaxNameLen 96 with the terminator)
TEMPO_BPM = 125
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MINOR = [0, 2, 3, 5, 7, 8, 10]
MINOR_KEYS = [n + "min" for n in ["A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"]]
CATEGORIES = ("Chord", "Lead", "Pad", "Bass", "Fx", "Beat", "Percussion")
KEYLESS = ("Beat", "Percussion")                                 # one folder, NA (SPEC §9.9); the count is the total
LEGACY_HASH = ("Chord", "Lead", "Pad")                           # name hash = this tool's original digest (docstring)
# per key folder: research §2.2 phase M2 (Chord: optional, small); the free card's additions (docs/SHARING.md)
DEFAULT_COUNTS = {"Lead": 150, "Pad": 120, "Chord": 60, "Bass": 60, "Fx": 20, "Beat": 240, "Percussion": 120}
NAME_RE = re.compile(r"^(NA|[A-G]#?min)__(Chord|Lead|Pad|Bass|Fx|Beat|Percussion)__GEN-([A-Za-z0-9-]+)-(\d{4,5})__([0-9a-f]{8})\.mid$")

# Pitch bands, from the library measurement (lowest p10 .. highest p90, and the clip median's p10 .. p90):
# every note must sit inside the first pair, the clip's median pitch inside the second (research §2.1 A /
# §2.3 clamp bands Chord 53-69, Lead 48-79, Pad 59-77). The generators keep their registers well inside.
# Bass / Fx: transpose_library.py's median bands (38-59, 45-79) with the measured hard limits (Bass lowest p10 36,
# highest p90 66; Fx 45..77). Beat: what ensureBeatCore keeps (36..58); Percussion: the four voices and a margin.
BANDS = {"Chord": (36, 79, 53, 69), "Lead": (43, 82, 48, 79), "Pad": (39, 85, 59, 77),
         "Bass": (28, 67, 38, 59), "Fx": (40, 84, 45, 79), "Beat": (36, 58, 36, 58), "Percussion": (36, 47, 36, 47)}

# ----------------------------------------------------------------------------------------------- library reference
# Measured on sdcard/lp/midi (Chord 293, Lead 161, Pad 94 clips, 2026-10-16, scratchpad research/procgen/stats.json),
# through the firmware's length rule. Used only for the parity table the QA prints.
LIBRARY_REF = {
    "Chord": dict(bars={2: 71, 4: 168}, notes_per_bar=(6.0, 16.0, 30.0), onsets_per_bar=(1.5, 5.0, 7.5),
                  pitch_lo=(36, 49, 61), pitch_hi=(60, 68, 79), offgrid_pct=17.1,
                  grid={"1/4": 30.3, "1/8": 28.0, "1/16": 24.1, "off-grid": 17.1},
                  note_len={"<=1/16": 59.4, "1/8": 27.5, "1/4": 7.2, "1/2": 2.5, "1 bar": 2.8, "2 bars": 0.5},
                  voicing={1: 9.6, 2: 15.2, 3: 19.6, 4: 21.9, 5: 22.0, 6: 9.2}, rest_pct=46.9, vel_mean=98,
                  degree={0: 23.8, 2: 7.9, 3: 16.5, 5: 7.5, 7: 20.6, 8: 4.8, 10: 11.4}, nondiatonic_p90=25.0),
    "Lead": dict(bars={1: 12, 2: 28, 4: 98}, notes_per_bar=(2.0, 6.0, 11.0), onsets_per_bar=(2.0, 5.75, 11.0),
                 pitch_lo=(43, 58, 74), pitch_hi=(54, 70, 82), offgrid_pct=19.9,
                 grid={"1/8": 31.4, "1/4": 27.4, "off-grid": 19.9, "1/16": 18.2},
                 note_len={"<=1/16": 66.4, "1/8": 25.8, "1/4": 6.0, "1/2": 1.2},
                 voicing={1: 98.6, 2: 1.3}, rest_pct=43.8, vel_mean=100,
                 degree={0: 44.2, 2: 5.5, 3: 11.6, 5: 8.6, 7: 9.5, 8: 5.9, 10: 8.6}, nondiatonic_p90=25.0),
    "Pad": dict(bars={2: 20, 4: 68}, notes_per_bar=(1.0, 3.5, 12.75), onsets_per_bar=(0.5, 1.5, 3.75),
                pitch_lo=(39, 57, 72), pitch_hi=(67, 75, 84), offgrid_pct=18.4,
                grid={"1/4": 57.6, "1/8": 19.9, "off-grid": 18.4, "1/16": 2.5},
                note_len={"1 bar": 33.8, "1/2": 25.9, "1/8": 13.2, "1/4": 11.2, "2 bars": 10.2, "<=1/16": 4.0},
                voicing={1: 35.2, 2: 8.8, 3: 12.4, 4: 18.4, 5: 17.0, 6: 4.8}, rest_pct=0.0, vel_mean=92,
                degree={0: 23.0, 2: 10.2, 3: 15.8, 5: 10.1, 7: 18.5, 8: 5.6, 10: 12.9}, nondiatonic_p90=15.7),
    # The four categories added for the free card, measured the same way on the library import of sdcard/lp/midi
    # (2026-10-27: Bass 1993, Fx 70, Beat 747, Percussion 296 clips; -TR / GEN / LDZ files left out).
    "Bass": dict(bars={1: 181, 2: 1163, 4: 647}, notes_per_bar=(4.5, 13.0, 36.0), onsets_per_bar=(4.0, 7.0, 16.0),
                 pitch_lo=(36, 48, 52), pitch_hi=(43, 59, 66), offgrid_pct=13.4,
                 grid={"1/16": 29.8, "1/8": 28.0, "1/4": 26.2, "off-grid": 13.4, "1/32": 2.6},
                 note_len={"<=1/16": 64.8, "1/8": 28.1, "1/4": 5.9, "1/2": 0.8, "1 bar": 0.3},
                 voicing={1: 52.7, 3: 47.0}, rest_pct=25.0, vel_mean=89,
                 degree={0: 21.3, 2: 8.7, 3: 22.0, 5: 8.7, 7: 18.2, 8: 7.2, 10: 12.4}, nondiatonic_p90=11.1),
    "Fx": dict(bars={1: 13, 2: 44, 4: 13}, notes_per_bar=(1.5, 6.5, 16.0), onsets_per_bar=(1.5, 6.5, 16.0),
               pitch_lo=(45, 67, 76), pitch_hi=(47, 67, 77), offgrid_pct=9.4,
               grid={"1/4": 36.3, "1/8": 30.5, "1/16": 23.5, "off-grid": 9.4},
               note_len={"<=1/16": 66.9, "1/8": 21.2, "1/4": 8.2, "1 bar": 2.1, "1/2": 1.0},
               voicing={1: 100.0}, rest_pct=23.4, vel_mean=100,
               degree={0: 9.9, 3: 87.7, 5: 0.8}, nondiatonic_p90=0.0),
    "Beat": dict(bars={1: 11, 2: 438, 4: 298}, notes_per_bar=(10.5, 24.0, 39.0), onsets_per_bar=(8.0, 13.75, 20.0),
                 pitch_lo=(36, 36, 36), pitch_hi=(41, 43, 47), offgrid_pct=37.5,
                 grid={"off-grid": 37.5, "1/4": 28.1, "1/8": 23.1, "1/16": 10.8},
                 note_len={"<=1/16": 48.4, "1/8": 40.9, "1/4": 10.1, "1/2": 0.5},
                 voicing={1: 53.1, 2: 27.3, 3: 13.8, 4: 4.5, 5: 1.0}, rest_pct=0.0, vel_mean=80,
                 degree={}, nondiatonic_p90=None),
    "Percussion": dict(bars={1: 6, 2: 103, 4: 187}, notes_per_bar=(4.25, 13.0, 28.5), onsets_per_bar=(4.0, 12.0, 17.0),
                       pitch_lo=(37, 38, 40), pitch_hi=(38, 41, 46), offgrid_pct=46.2,
                       grid={"off-grid": 46.2, "1/8": 20.5, "1/4": 16.7, "1/16": 15.8},
                       note_len={"<=1/16": 75.3, "1/8": 21.5, "1/4": 3.0},
                       voicing={1: 85.6, 2: 9.7, 3: 3.5, 4: 0.8}, rest_pct=6.3, vel_mean=77,
                       degree={}, nondiatonic_p90=None),
}


# ----------------------------------------------------------------------------------------------- small helpers
def pick(rng, items):
    """Weighted choice over [(item, weight), ...]."""
    total = sum(w for _, w in items)
    r = rng.random() * total
    for item, w in items:
        r -= w
        if r <= 0:
            return item
    return items[-1][0]


def seed_rng(cat, key, seed):
    """One RNG per (category, key, draw): sha1 of the triple, so a clip is reproducible from its name alone."""
    digest = hashlib.sha1(("%s|%s|%d" % (cat, key, seed)).encode("ascii")).digest()
    return random.Random(int.from_bytes(digest[:8], "big"))


def degree_pitch(root, deg, octave):
    """Scale degree -> MIDI pitch, the firmware's ProjectKey::degreePitch convention (octave 3 starts at C3 = 48;
    degree 0 of octave 3 is the root in [48, 60); degrees outside 0..6 carry into neighbouring octaves)."""
    carry, d = divmod(deg, 7)
    return 12 * (octave + carry + 1) + root + MINOR[d]


def in_key(root, pitch):
    return (pitch - root) % 12 in MINOR


def round_unit(ticks):
    return max(UNIT, (ticks // UNIT) * UNIT)


def fold_into(pitch, lo, hi):
    """Move a pitch by octaves into [lo, hi]; None when the band is narrower than an octave and nothing fits."""
    while pitch < lo:
        pitch += 12
    while pitch > hi:
        pitch -= 12
    return pitch if lo <= pitch <= hi else None


# ----------------------------------------------------------------------------------------------- harmony tables
# PROGRESSIONS, ROMAN, CHORD_KINDS and CHORD_KIND_W live in tools/theory_data.py (imported above): the same tables
# feed the firmware's theory generator through tools/make_theory_tables.py (SPEC §9.24).


def chord_pcs(root, deg, kind):
    """Pitch classes of the chord on scale degree deg (degree stacking). "V" = major dominant: v with a raised third."""
    if deg == "V":
        pcs = [(root + MINOR[(4 + d) % 7]) % 12 for d in CHORD_KINDS[kind]]
        if len(CHORD_KINDS[kind]) >= 3 and CHORD_KINDS[kind][1] == 2:
            pcs[1] = (pcs[1] + 1) % 12                      # leading tone (harmonic minor)
        return pcs
    return [(root + MINOR[(deg + d) % 7]) % 12 for d in CHORD_KINDS[kind]]


def voicings(pcs, lo, hi, style):
    """Candidate voicings of a pitch-class list inside [lo, hi]: every inversion stacked closed from its lowest
    note, then shaped by the voicing style (closed / open = drop-2 / spread = bass note + upper structure /
    double = root doubled an octave up). Returns sorted pitch lists (duplicates removed)."""
    out = []
    n = len(pcs)
    for r in range(n):
        order = pcs[r:] + pcs[:r]
        for base in range(lo, lo + 12):
            if base % 12 != order[0]:
                continue
            v = [base]
            for pc in order[1:]:
                p = v[-1] + ((pc - v[-1]) % 12 or 12)
                v.append(p)
            if style == "open" and n >= 3:
                v = sorted(v[:-2] + [v[-2] - 12, v[-1]])
            elif style == "spread" and n >= 3:
                v = [v[0]] + [p + 12 if p - v[0] < 7 else p for p in v[1:]]
                v = sorted(v)
            elif style == "double":
                v = sorted(v + [v[0] + 12])
            v = sorted(set(v))
            if v[0] >= lo and v[-1] <= hi:
                out.append(v)
    return out


def pitch_to_degree(root, pitch):
    """Absolute scale-degree number of an in-key pitch (7 per octave); a non-key pitch takes the degree below."""
    rel = pitch - root
    octave, pc = divmod(rel, 12)
    idx = max(i for i, iv in enumerate(MINOR) if iv <= pc)
    return octave * 7 + idx


def degree_to_pitch(root, n):
    octave, idx = divmod(n, 7)
    return root + 12 * octave + MINOR[idx]


def planed(base_degrees, root, diff, lo, hi):
    """Diatonic planing: every voice of the first chord moves by the same number of scale steps, the whole
    chord shifted by octaves into the register."""
    v = sorted(degree_to_pitch(root, n + diff) for n in base_degrees)
    while v[-1] > hi and v[0] - 12 >= lo:
        v = [p - 12 for p in v]
    while v[0] < lo and v[-1] + 12 <= hi:
        v = [p + 12 for p in v]
    return v


def lead_voicing(prev, cands, rng, slack=0.0):
    """The candidate with the least total voice movement from prev (voices matched by rank); without prev, a
    candidate whose middle lies near the register centre. Small random tie-break; with probability slack the
    second-best candidate is taken instead (a player's "lift", and an inversion the firmware's Variation can
    then improve)."""
    if not cands:
        return None
    if not prev:
        mid = sum(sum(c) / len(c) for c in cands) / len(cands)
        return min(cands, key=lambda c: (abs(sum(c) / len(c) - mid) + rng.random() * 3))
    scored = []
    for c in cands:
        score = rng.random() * 2
        for i, p in enumerate(c):
            anchor = prev[min(i, len(prev) - 1)]
            score += abs(p - anchor) + 0.15 * abs(p - sorted(c)[len(c) // 2])
        scored.append((score, c))
    scored.sort(key=lambda sc: sc[0])
    if len(scored) > 1 and slack > 0 and rng.random() < slack:
        return scored[1][1]
    return scored[0][1]


# ----------------------------------------------------------------------------------------------- rhythm tables
# CHORD_RHYTHMS, LEAD_RHYTHMS, LEAD_MOVES and LEAD_HOME: tools/theory_data.py (imported above).

# Humanisation modes: share of clips, then what moves. Shifts are 3..12 ticks (the editor and mutate work on
# 12-tick units; a 12-tick shift lands on the 1/32 grid, every other value is off-grid). The downbeat note never
# moves. Library off-grid onset share: Chord 17 %, Lead 20 %, Pad 18 %.
HUMANISE = {
    "Chord": [("none", 50), ("loose", 28), ("late", 16), ("strum", 6)],
    "Lead": [("none", 36), ("loose", 32), ("late", 32)],
    "Pad": [("none", 50), ("loose", 18), ("late", 12), ("strum", 20)],
    # library off-grid onsets: Bass 13 %, Fx 9 %, Beat 38 %, Percussion 46 % ("late" = the 16th swing)
    "Bass": [("none", 50), ("loose", 20), ("late", 30)],
    "Fx": [("none", 70), ("late", 30)],
    "Beat": [("none", 50), ("late", 40), ("loose", 10)],
    "Percussion": [("none", 40), ("late", 35), ("loose", 25)],
}


# ----------------------------------------------------------------------------------------------- generators
def gen_chord(rng, root, bars):
    """Stabs / comping: a progression x a chord kind x a 16-step rhythm, voiced by least movement inside a
    register, staccato dominated (library note lengths: <= 1/16 59 %, 1/8 28 %), 0.5-4 chord changes per bar
    (p50 2, p90 4), optional dropped root (the Bass track has it), optional pickup fill in the last bar."""
    prog = pick(rng, PROGRESSIONS)
    kind = pick(rng, CHORD_KIND_W["Chord"])
    # voicing: least-movement voice leading (closed / open / spread / doubled root) or "parallel" planing (the
    # same shape moved along the scale, the classic stab move, which also leaves the firmware's Variation slot
    # an inversion to improve)
    vstyle = pick(rng, [("closed", 24), ("open", 14), ("spread", 6), ("double", 14), ("parallel", 12), ("root", 30)])
    mask, _, rname = pick(rng, [((m, w, n), w) for m, w, n in CHORD_RHYTHMS])
    # gate: a fixed stab length (the library's chords are mostly 1/16-ish stabs: <= 1/16 59 %) or a share of
    # the distance to the next onset
    gate = pick(rng, [("stab", 50), (0.4, 18), (0.6, 12), (0.9, 11), (1.0, 9)])
    stab_len = rng.choice([UNIT, 2 * UNIT, 2 * UNIT, 2 * UNIT])
    changes = pick(rng, [(1, 46), (2, 32), (0.5, 14), (4, 8)])
    lo = pick(rng, [(43, 2), (45, 3), (48, 4), (50, 3), (52, 3), (55, 2)])
    hi = min(lo + rng.choice([19, 22, 24, 27]), 78)
    vel_base = rng.choice([88, 94, 100, 106, 110])
    accent = rng.choice([0, 6, 12])
    drop_root = rng.random() < 0.3
    bass = rng.random() < 0.25                                   # the chord root an octave below (library lowest p10 = 36)
    fill = rng.random() < 0.4
    push = rng.random() < 0.3                                    # anticipate the next chord on the last 8th
    steps = bars * 16
    slot_len = int(16 / changes)
    notes = []
    prev = None
    base_degrees = None
    base_deg = None
    onsets = [k for k in range(steps) if mask[k % 16] == "x"]
    if fill:
        last = (bars - 1) * 16
        add = last + rng.choice([14, 15, 13])
        if add not in onsets:
            onsets.append(add)
        if rng.random() < 0.5 and len(onsets) > 2:
            tail = [k for k in onsets if k >= last and k != add]
            if tail:
                onsets.remove(tail[-1])
        onsets.sort()
    for i, k in enumerate(onsets):
        ci = k // slot_len
        if push and (k % slot_len) >= slot_len - 2 and rng.random() < 0.6:
            ci += 1
        deg = prog[ci % len(prog)]
        if vstyle == "parallel" and deg == "V":
            deg = 4                                              # planing stays diatonic
        pcs = chord_pcs(root, deg, kind)
        if drop_root and len(pcs) > 3:
            pcs = pcs[1:]
        if vstyle == "parallel" and base_degrees is not None:
            diff = deg - base_deg
            diff = diff - 7 if diff > 3 else (diff + 7 if diff < -3 else diff)
            v = planed(base_degrees, root, diff, lo, hi)
        elif vstyle == "root":                                   # root position, no voice leading (stab style)
            cands = [c for c in voicings(pcs, lo, hi, "closed") if c[0] % 12 == pcs[0]]
            v = min(cands, key=lambda c: c[0]) if cands else lead_voicing(prev, voicings(pcs, lo, hi, "closed"), rng)
            if v is None:
                continue
        else:
            cands = voicings(pcs, lo, hi, "closed" if vstyle == "parallel" else vstyle)
            v = lead_voicing(prev, cands, rng, slack=0.3)
            if v is None:
                continue
            if vstyle == "parallel":
                base_degrees = [pitch_to_degree(root, p) for p in v]
                base_deg = deg
        prev = v
        nxt = onsets[i + 1] if i + 1 < len(onsets) else steps
        dist = (nxt - k) * STEP
        if gate == "stab":
            length = min(stab_len, dist)
        else:
            length = dist if gate >= 1.0 else round_unit(int(dist * gate))
        strong = k % 4 == 0
        for vi, p in enumerate(v):
            vel = vel_base + (accent if strong else 0) - 3 * vi + rng.randint(-3, 3)
            notes.append([k * STEP, length, p, vel])
        if bass and strong:
            low = fold_into(chord_pcs(root, deg, "5")[0], 36, 47)
            if low is not None and low < v[0]:
                notes.append([k * STEP, length, low, vel_base + accent - 2])
    pi = [i for i, (pr, _) in enumerate(PROGRESSIONS) if pr == prog][0]
    return notes, "%s-%s-%s-p%d" % (rname, kind, vstyle[:4], pi)


def gen_pad(rng, root, bars):
    """Sustained material: one chord per bar (p50) or per 2 bars, voiced by least movement, in seven styles
    (sustain, swell, pulse, suspend, line = monophonic, drone, arp = staggered legato entries); 40 % legato
    overlaps as the library (legato p50 40 %, rests ~0 %)."""
    prog = pick(rng, PROGRESSIONS)
    kind = pick(rng, CHORD_KIND_W["Pad"])
    style = pick(rng, [("sustain", 36), ("swell", 12), ("pulse", 8), ("suspend", 12), ("line", 12), ("drone", 10), ("arp", 10)])
    vstyle = pick(rng, [("closed", 50), ("open", 25), ("spread", 15), ("double", 10)])
    bpc = pick(rng, [(1, 60), (2, 30), (0.5, 10)])             # bars per chord (0.5 = two chords per bar)
    lo = pick(rng, [(48, 2), (52, 3), (55, 4), (57, 3), (60, 3), (64, 1)])
    hi = min(lo + rng.choice([19, 24, 27]), 84)
    vel = rng.choice([86, 92, 98, 104])
    legato = rng.random() < 0.4
    overlap = 2 * UNIT if legato else 0
    push = rng.random() < 0.3                                    # later chords enter an 8th / a quarter early
    gap = rng.random() < 0.2                                     # ... or after a short rest
    bass = rng.random() < 0.2                                    # the chord root low under the pad (library lowest p10 = 39)
    span = int(bpc * BAR)
    n_chords = max(1, int(bars * BAR) // span)
    notes = []
    prev = None
    line_oct = 4
    for ci in range(n_chords):
        deg = prog[ci % len(prog)]
        pcs = chord_pcs(root, deg, kind)
        v = lead_voicing(prev, voicings(pcs, lo, hi, vstyle), rng, slack=0.15)
        if v is None:
            continue
        prev = v
        start = ci * span
        last = ci == n_chords - 1
        ov = 0 if last else overlap
        if style == "sustain":
            early = 0
            if ci > 0 and push and span >= BAR:
                early = rng.choice([PPQN // 2, PPQN])
            late = 0
            if ci > 0 and gap and span >= BAR and rng.random() < 0.5:
                late = rng.choice([PPQN // 2, PPQN])
            if ci > 0 and early and notes:
                for n in notes:                                  # the previous chord ends where this one enters
                    if n[0] + n[1] > start - early and n[0] < start - early:
                        n[1] = max(UNIT, start - early - n[0])
            for vi, p in enumerate(v):
                notes.append([start - early + late, span + early - late + ov, p, vel - 2 * vi])
            if bass:
                low = fold_into(chord_pcs(root, deg, "5")[0], 40, 51)
                if low is not None and low < v[0]:
                    notes.append([start - early + late, span + early - late + ov, low, vel - 4])
        elif style == "swell":
            half = span // 2
            for k in range(2):
                for vi, p in enumerate(v):
                    notes.append([start + k * half, half + (ov if k == 1 else 0), p, vel - 2 * vi - (0 if k == 0 else 6)])
        elif style == "pulse":
            div = rng.choice([PPQN, PPQN // 2])
            for k in range(span // div):
                for vi, p in enumerate(v):
                    notes.append([start + k * div, round_unit(int(div * 0.8)), p, vel - 2 * vi - (0 if k % 2 == 0 else 8)])
        elif style == "suspend":
            top = v[-1]
            for vi, p in enumerate(v[:-1]):
                notes.append([start, span + ov, p, vel - 2 * vi])
            move = rng.choice([1, -1, 2, -2])
            nb = top
            for _ in range(8):                                   # nearest in-key neighbour in the chosen direction
                nb += 1 if move > 0 else -1
                if in_key(root, nb):
                    break
            tail = PPQN if span <= BAR else 2 * PPQN
            notes.append([start, span - tail, top, vel])
            notes.append([start + span - tail, tail + ov, nb if lo <= nb <= hi + 3 else top, vel - 6])
        elif style == "line":
            count = rng.choice([1, 1, 2, 2, 3]) if span >= BAR else 1
            seg = span // count
            tones = [pc for pc in pcs if in_key(root, pc)] or pcs          # a line never takes the borrowed note
            for k in range(count):
                pc = rng.choice(tones if k < count - 1 else [tones[0], tones[min(1, len(tones) - 1)], tones[-1]])
                p = fold_into(pc + 12 * (line_oct + 1), 60, 79)
                if p is None:
                    continue
                notes.append([start + k * seg, seg + (ov if (k < count - 1 or not last) else 0), p, vel - 4 * (k % 2)])
                low = p - 7 if in_key(root, p - 7) else p - 5                 # a fifth (or fourth) below, in key
                if rng.random() < 0.25 and low >= 52 and in_key(root, low):
                    notes.append([start + k * seg, seg, low, vel - 10])
        elif style == "drone":
            if ci == 0:
                hold = min(bars * BAR, 2 * span if n_chords > 1 else span)
                dr = fold_into(pcs[0], lo, lo + 11)              # the chord root; its fifth only when in key
                fifth = dr + 7 if in_key(root, dr + 7) else None
                notes.append([0, hold, dr, vel])
                if rng.random() < 0.6 and fifth is not None:
                    notes.append([0, hold, fifth, vel - 6])
                if hold < bars * BAR:
                    notes.append([hold, bars * BAR - hold, dr, vel])
                    if rng.random() < 0.6 and fifth is not None:
                        notes.append([hold, bars * BAR - hold, fifth, vel - 6])
            tones = [p for p in v if in_key(root, p)] or v                # the colour tone stays diatonic
            colour = tones[-1] if tones[-1] - v[0] >= 10 else tones[-1] + 12
            if colour <= 84:
                notes.append([start, span + ov, colour, vel - 8])
        else:                                                    # arp: staggered entries that pile up legato
            stagger = rng.choice([PPQN, PPQN // 2, PPQN // 2])
            order = list(v) if rng.random() < 0.6 else list(reversed(v))
            for vi, p in enumerate(order):
                s = start + vi * stagger
                if s >= start + span:
                    break
                notes.append([s, start + span - s + ov, p, vel - 2 * vi])
    return notes, "%s-%s-x%s" % (style, kind, str(bpc).replace(".", ""))


def lead_motif(rng, root, tmpl, octave, style, prog):
    """One bar of (step, degree, velocity-offset) from a rhythm template and a style-specific degree walk."""
    onsets = [k for k in range(16) if tmpl[k] == "x"]
    out = []
    deg = pick(rng, [(0, 8), (2, 2), (4, 1), (-3, 1)])
    chord_deg = prog[0] if prog[0] != "V" else 4
    tones = [(chord_deg + x) for x in (0, 2, 4)]
    for i, k in enumerate(onsets):
        if style == "octave":
            if i == 0:
                deg = 0
            else:
                deg = pick(rng, [(0, 5), (7, 3), (-7, 1), (4, 2), (2, 2), (6, 1), (-1, 2), (1, 1)])
        elif style == "arp":
            seq = tones + [t + 7 for t in tones]
            if i == 0:
                direction = rng.choice([1, -1, 2])               # up / down / up-down
            if direction == 2:
                cyc = seq + seq[-2:0:-1]
                deg = cyc[i % len(cyc)]
            else:
                deg = seq[(i if direction == 1 else -i) % len(seq)]
        else:                                                    # riff, callresp
            if i > 0:
                deg += pick(rng, LEAD_MOVES)
                deg = max(-7, min(11, deg))
                if k % 4 != 0 and rng.random() < 0.4:
                    deg = pick(rng, LEAD_HOME)
                if k % 4 == 0 and deg % 7 not in [t % 7 for t in tones] and rng.random() < 0.35:
                    deg += 1 if (deg + 1) % 7 in [t % 7 for t in tones] else -1
        out.append([k, deg, 6 if k % 4 == 0 else 0])
    return out


def vary_bar(rng, motif, op, root):
    """A variation of the motif: sequence (scale step), rotate (rhythm shifted an 8th), ending (resolve to the
    root / a chord tone, longer last note), octave (one note displaced), thin (drop a weak note), pickup (an extra
    16th before a beat), invert (mirror the contour around the first note)."""
    m = [list(n) for n in motif]
    if op == "sequence":
        shift = rng.choice([1, -1, 2, -2])
        for n in m:
            n[1] += shift
    elif op == "rotate":
        for n in m:
            n[0] = (n[0] + 2) % 16
        m.sort()
    elif op == "ending":
        for j, n in enumerate(m):
            if j >= len(m) - 2:
                n[1] = rng.choice([0, 0, 2, 4, -3]) + (7 if rng.random() < 0.2 else 0)
            elif rng.random() < 0.3:
                n[1] += rng.choice([-1, 1])
        if len(m) > 2 and rng.random() < 0.5:
            m.pop()                                              # breathe: rest before the repeat
    elif op == "octave":
        j = rng.randrange(len(m))
        m[j][1] += rng.choice([7, -7])
    elif op == "thin":
        weak = [j for j, n in enumerate(m) if n[0] % 4 != 0]
        if len(m) > 2 and weak:
            m.pop(rng.choice(weak))
    elif op == "pickup":
        beats = [n[0] for n in m if n[0] % 4 == 0 and n[0] > 0]
        if beats:
            b = rng.choice(beats)
            if all(n[0] != b - 1 for n in m):
                tgt = [n for n in m if n[0] == b][0]
                m.append([b - 1, tgt[1] + rng.choice([-1, 1, -2]), -4])
                m.sort()
    elif op == "invert":
        base = m[0][1]
        for n in m[1:]:
            n[1] = base - (n[1] - base)
    return m


def gen_lead(rng, root, bars):
    """Motif and variation: a one-bar motif (rhythm template x degree walk, four styles: riff, octave bounce, arp,
    call/response) laid out as A B / A A' (2 bars) or A A' A B / A B A B' / A A B B' / A B C B' (4 bars),
    monophonic, staccato dominated (<= 1/16 66 %, 1/8 26 %), with strong beats pulled to the hidden harmony."""
    style = pick(rng, [("riff", 43), ("octave", 20), ("arp", 12), ("callresp", 25)])
    tmpl_i = pick(rng, [(i, w) for i, (_, w) in enumerate(LEAD_RHYTHMS)])
    tmpl = LEAD_RHYTHMS[tmpl_i][0]
    if style == "arp" and tmpl.count("x") < 4:
        tmpl = "x.x.x.x.x.x.x.x."
    # register follows the root so the tonic sits near the library's centre (lowest p50 58, highest p50 70):
    # "mid" puts the root in 55..66, "high" an octave up, "low" an octave down
    mid = 4 if root < 7 else 3
    octave = mid + pick(rng, [(0, 60), (1, 25), (-1, 15)])
    # gate: a fixed pluck length (library <= 1/16 66 %) or a share of the distance to the next onset
    gate = pick(rng, [("stab", 42), (0.45, 25), (0.7, 15), (0.95, 10), (1.0, 8)])
    stab_len = rng.choice([UNIT, 2 * UNIT, 2 * UNIT, 3 * UNIT])
    vel = rng.choice([92, 96, 100, 104])
    prog = pick(rng, PROGRESSIONS)
    motif = lead_motif(rng, root, tmpl, octave, style, prog)
    if bars == 2:
        form = pick(rng, [("AB", 60), ("AA'", 40)])
    else:
        form = pick(rng, [("AA'AB", 45), ("ABAB'", 25), ("AABB'", 15), ("ABCB'", 15)])
    if style == "callresp":
        form = "AB" if bars == 2 else pick(rng, [("ABAB'", 60), ("ABCB'", 40)])
    parts = re.findall(r"[A-C]'?", form)
    cache = {"A": motif}
    ops_var = [("sequence", 4), ("rotate", 2), ("octave", 3), ("thin", 3), ("pickup", 3), ("invert", 1)]
    bars_out = []
    borrowed = False
    for bi, part in enumerate(parts):
        if part not in cache:
            if part == "B":
                cache[part] = vary_bar(rng, motif, "ending", root)
                if rng.random() < 0.5:
                    cache[part] = vary_bar(rng, cache[part], pick(rng, ops_var), root)
            elif part == "C":
                cache[part] = vary_bar(rng, vary_bar(rng, motif, "sequence", root), pick(rng, ops_var), root)
            else:                                                # A', B'
                base = cache[part[0]]
                cache[part] = vary_bar(rng, base, pick(rng, ops_var if part == "A'" else ops_var + [("ending", 4)]), root)
        bar = [list(n) for n in cache[part]]
        if bi == 0 and (not bar or bar[0][0] != 0):
            bar.insert(0, [0, 0, 6])                             # the first bar always has a downbeat onset
        chord_deg = prog[bi % len(prog)]
        chord_deg = 4 if chord_deg == "V" else chord_deg
        tones = {(chord_deg + x) % 7 for x in (0, 2, 4)}
        last_bar = bi == len(parts) - 1
        for j, (k, deg, voff) in enumerate(bar):
            if k % 4 == 0 and deg % 7 not in tones and rng.random() < 0.35:
                deg += 1 if (deg + 1) % 7 in tones else -1
            pitch = degree_pitch(root, deg, octave)
            # borrowed leading tone: in the last bar, a b7 right before a final root on a weak step (rare)
            if (last_bar and not borrowed and j == len(bar) - 2 and k % 4 != 0 and deg % 7 == 6
                    and bar[-1][1] % 7 == 0 and rng.random() < 0.35):
                pitch += 1
                borrowed = True
            bars_out.append([bi * BAR + k * STEP, 0, pitch, vel + voff + rng.randint(-3, 3)])
    bars_out.sort(key=lambda n: n[0])
    # monophonic: one note per onset, length = gate x distance to the next onset (never overlapping)
    notes = []
    for i, n in enumerate(bars_out):
        if notes and notes[-1][0] == n[0]:
            continue
        notes.append(n)
    # phrase ends ring: the last note before the half of the clip (and often the final note) is held for up
    # to a quarter. Besides the phrasing, a held note (>= 48 ticks, not the final one) is what the firmware's
    # lead Variation style 3 splits; a clip of nothing but stabs would leave that slot unchanged.
    half = bars * BAR // 2
    ring_half = rng.random() < 0.85
    ring_last = rng.random() < 0.4
    for i, n in enumerate(notes):
        nxt = notes[i + 1][0] if i + 1 < len(notes) else bars * BAR
        dist = nxt - n[0]
        if gate == "stab":
            n[1] = min(stab_len, dist)
        else:
            n[1] = dist if gate >= 1.0 else round_unit(int(dist * gate))
        if style != "arp":
            phrase_end = n[0] < half <= nxt
            if (phrase_end and ring_half) or (i == len(notes) - 1 and ring_last):
                n[1] = max(n[1], min(dist, PPQN))
    gname = "s%d" % (stab_len // UNIT) if gate == "stab" else "g%d" % int(gate * 100)
    return notes, "%s-r%d-o%d-%s" % (style, tmpl_i, octave, gname)


# ----------------------------------------------------------------------------------------------- bass / fx / drums
def gen_bass(rng, root, bars):
    """Bass line: a progression x a BASS_RHYTHMS template (root / octave / fifth / third / approach codes, holds) in
    a register with the root in 40..51 (library clip median p10/p50/p90 = 39/53/59, lowest p10 36, highest p90 66),
    monophonic and staccato dominated (library <= 1/16 65 %, 1/8 28 %), 0.5-2 chord changes per bar, a share of
    303-style slides (a note held into the next), a pickup into the chord that follows in the last two steps."""
    prog = pick(rng, PROGRESSIONS)
    mask, _, rname = pick(rng, [((m, w, n), w) for m, w, n in BASS_RHYTHMS])
    changes = pick(rng, [(1, 55), (2, 20), (0.5, 25)])          # chords per bar
    octave = 2 if 36 + root >= 40 else 3                          # the root in 40..51
    if rng.random() < 0.15:
        octave += 1                                               # the higher register now and then
    gate = pick(rng, [("stab", 45), (0.5, 20), (0.8, 20), (1.0, 15)])
    stab_len = rng.choice([UNIT, 2 * UNIT, 2 * UNIT, 3 * UNIT])
    slide = rng.random() < 0.3
    ghost = rng.random() < 0.35                                   # 16th off-beats quieter
    pickup = rng.random() < 0.4
    vel_base = rng.choice([82, 88, 94, 100])                     # library velocity mean p50 = 89
    # the template's "o" voice: the octave, or the fifth / third instead (library degree shares: root 21 %, b3 22 %,
    # 5th 18 %, b7 12 %); a fifth or third also gives the firmware's lead Variation a passing tone to work with
    colour = pick(rng, [(7, 55), (4, 30), (2, 15)])
    walk = pick(rng, [(0.0, 40), (0.15, 35), (0.3, 25)])        # share of weak-step roots that become colour tones
    steps = bars * 16
    slot_len = int(16 / changes)

    def chord_deg(slot):
        d = prog[slot % len(prog)]
        return (4, True) if d == "V" else (d, False)

    events = []                                                   # [start, hold end or None, pitch, vel]
    for k in range(steps):
        code = mask[k % 16]
        if pickup and k >= steps - 2:
            code = "." if k == steps - 2 else "a"
        if code == "r" and k % 4 != 0 and walk and rng.random() < walk:
            code = rng.choice(["f", "t", "a", "F"])              # fifth, third, approach, fifth below
        if code == "-":
            if events:
                events[-1][1] = (k + 1) * STEP
            continue
        if code == ".":
            continue
        deg, dominant = chord_deg(k // slot_len)
        ndeg, _ = chord_deg((k + 1) // slot_len)
        raise_third = False
        if code in "rR":
            d = deg
        elif code in "oO":
            d = deg + colour
        elif code == "f":
            d = deg + 4
        elif code == "F":
            d = deg - 3
        elif code == "t":
            d = deg + 2
            raise_third = dominant                                # the borrowed dominant's major third
        else:                                                     # "a": approach the next chord's root by a scale step
            d = ndeg + rng.choice([1, -1])
        pitch = degree_pitch(root, d, octave) + (1 if raise_third else 0)
        vel = vel_base + (8 if code in "RO" else 0) + (4 if k % 4 == 0 else 0) - (12 if ghost and k % 2 == 1 else 0)
        events.append([k * STEP, None, pitch, vel + rng.randint(-3, 3)])
    if not events or events[0][0] != 0:
        events.insert(0, [0, None, degree_pitch(root, chord_deg(0)[0], octave), vel_base + 8])
    notes = []
    # phrase ends ring (as gen_lead): the last note of every two bars is held for up to a quarter. Besides the
    # phrasing, a held note (>= 48 ticks) is what the firmware's lead Variation (the Bass track's slot 1) splits or
    # gates; a line of nothing but 12-tick stabs would leave that slot unchanged.
    ring = rng.random() < 0.85
    for i, (s, hold, p, v) in enumerate(events):
        nxt = events[i + 1][0] if i + 1 < len(events) else bars * BAR
        dist = nxt - s
        if hold is not None:
            length = min(hold, nxt) - s
        elif slide and rng.random() < 0.25:
            length = dist                                         # legato into the next note (the engine glides)
        elif gate == "stab":
            length = min(stab_len, dist)
        else:
            length = dist if gate >= 1.0 else round_unit(int(dist * gate))
        if ring and (s // (2 * BAR)) != (nxt // (2 * BAR)) and nxt % (2 * BAR) == 0 or (ring and i == len(events) - 1):
            length = max(length, min(dist, PPQN))
        notes.append([s, max(UNIT, length), p, v])
    pi = [i for i, (pr, _) in enumerate(PROGRESSIONS) if pr == prog][0]
    gname = "s%d" % (stab_len // UNIT) if gate == "stab" else "g%d" % int(gate * 100)
    return notes, "%s-p%d-o%d-%s" % (rname, pi, octave, gname)


def gen_fx(rng, root, bars):
    """Fx phrases on a pool of one to three key pitches (root, fifth, octave in 56..79; library median p10/p50/p90 =
    45/67/77, one voice per onset): stabs on the chord changes, a riser of 16ths with a velocity ramp, impacts held
    for beats, random blips on the 1/16 grid (the firmware's generated_fx density) or an 8th-note sweep. Every
    phrase has a downbeat onset and a hit inside the last two beats, so the firmware keeps the whole loop."""
    style = pick(rng, [("stab", 30), ("riser", 20), ("impact", 15), ("blip", 20), ("sweep", 15)])
    base = 60 + root if root <= 7 else 48 + root                  # 56..67
    fifth, octv = base + 7, base + 12
    pool = pick(rng, [([base], 30), ([base, fifth], 30), ([base, octv], 20), ([base, fifth, octv], 20)])
    vel = rng.choice([90, 100, 110, 118])
    length = bars * BAR
    notes = []
    if style == "stab":
        every = pick(rng, [(BAR, 50), (BAR // 2, 30), (2 * BAR, 20)])
        stab = rng.choice([UNIT, 2 * UNIT, 4 * UNIT, 8 * UNIT])  # library note lengths: <= 1/16 67 %, 1/8 21 %, 1/4 8 %
        for i, s in enumerate(range(0, length, every)):
            notes.append([s, stab, pool[i % len(pool)], vel + rng.randint(-6, 6)])
        notes.append([length - rng.choice([STEP, 2 * STEP, 4 * STEP]), UNIT, pool[0], vel - 12])   # the anticipation
    elif style == "riser":
        div = rng.choice([STEP, STEP, PPQN // 2])
        start = (bars - rng.choice([1, 1, 2])) * BAR if bars == 4 else BAR
        notes.append([0, 2 * UNIT, pool[0], vel])
        n = (length - start) // div
        for i in range(n):
            v = 50 + int(77 * i / max(1, n - 1))
            notes.append([start + i * div, UNIT if div == STEP else 2 * UNIT, pool[-1], v])
    elif style == "impact":
        hold = rng.choice([PPQN, 2 * PPQN, 4 * PPQN])
        notes.append([0, hold, pool[0], 120])
        if rng.random() < 0.6:
            notes.append([length // 2, hold, pool[min(1, len(pool) - 1)], 100])
        if rng.random() < 0.5:
            notes.append([length - PPQN, PPQN, pool[0], 70])
        else:
            notes[-1][1] = length - notes[-1][0]                  # held to the loop end
    elif style == "blip":
        for k in range(bars * 16):
            if k != 0 and rng.random() >= 0.42:
                continue
            notes.append([k * STEP, rng.choice([UNIT, UNIT, 2 * UNIT]), rng.choice(pool), 55 + rng.randint(0, 60)])
        if notes[-1][0] < length - TRAILING:
            notes.append([length - rng.choice([STEP, 2 * STEP]), UNIT, rng.choice(pool), 80])
    else:                                                         # sweep
        div = rng.choice([PPQN // 2, STEP])
        n = length // div
        up = rng.random() < 0.5
        for i in range(n):
            v = 60 + int(60 * (i if up else n - 1 - i) / max(1, n - 1))
            notes.append([i * div, max(UNIT, round_unit(div // 2)), pool[i % len(pool)], v])
    return notes, "%s-p%d" % (style, len(pool))


def gen_beat(rng, root, bars):
    """Drum pattern on the firmware's Beat note contract (36 kick, 37 snare, 38 closed hat, 39 open hat, one accent
    pitch in 40..44): a style (house / techno / garage / breaks) draws one BEAT_TEMPLATES mask per voice, the second
    of every two bars may swap the hat masks, the last bar may carry a fill (BEAT_FILLS), 8th-note hats are accented
    over the 16ths between, quiet ghost kicks and the accent voice complete the Full layer. A closed hat never sits
    on an open hat's step (the engine chokes the open hat). Library: 24 notes per bar p50, 38 % off-grid onsets."""
    style = pick(rng, BEAT_STYLES)
    t = BEAT_TEMPLATES[style]

    def draw(voice):
        i = pick(rng, [(i, w) for i, (_, w) in enumerate(t[voice])])
        return i, t[voice][i][0]

    masks, idx = {}, {}
    for voice in ("kick", "snare", "hh", "oh", "ghost", "acc"):
        idx[voice], masks[voice] = draw(voice)
    alt = dict(masks)
    if rng.random() < 0.5:
        _, alt["hh"] = draw("hh")
        _, alt["oh"] = draw("oh")
    accent = pick(rng, BEAT_ACCENTS)
    crash = rng.random() < 0.2 and style != "techno"              # a second accent pitch on the loop's downbeat
    fill = pick(rng, BEAT_FILLS)
    vel = {"kick": rng.choice([104, 110, 116]), "snare": rng.choice([92, 98, 104]), "hh": rng.choice([60, 68, 76]),
           "oh": rng.choice([72, 80, 88]), "acc": rng.choice([64, 72, 80]), "ghost": rng.choice([46, 54, 62])}
    hat_accent = rng.choice([0, 8, 14])
    notes = []

    def hit(bar, k, pitch, v):
        notes.append([bar * BAR + k * STEP, UNIT, pitch, max(1, min(127, v + rng.randint(-3, 3)))])

    for bar in range(bars):
        m = alt if bar % 2 == 1 else masks
        last = bar == bars - 1
        filling = fill != "none" and last and (bars == 4 or rng.random() < 0.5)
        for k in range(16):
            if m["kick"][k] == "x" and not (filling and fill == "drop-kick" and k >= 12):
                hit(bar, k, 36, vel["kick"] + (6 if k == 0 else 0))
            elif m["ghost"] and m["ghost"][k] == "x":
                hit(bar, k, 36, vel["ghost"])
            if m["snare"] and m["snare"][k] == "x" and not (filling and fill in ("snare16", "snare-roll") and k >= 12):
                hit(bar, k, 37, vel["snare"])
            if m["oh"] and m["oh"][k] == "x":
                hit(bar, k, 39, vel["oh"])
            elif m["hh"][k] == "x":
                hit(bar, k, 38, vel["hh"] + (hat_accent if k % 2 == 0 else 0) + (4 if k % 4 == 0 else 0))
            if m["acc"] and m["acc"][k] == "x":
                hit(bar, k, accent, vel["acc"])
        if crash and bar == 0:
            hit(0, 0, 44 if accent != 44 else 43, vel["acc"] + 10)
        if filling:
            if fill == "snare16":
                for k in range(12, 16):
                    hit(bar, k, 37, vel["snare"] - 24 + 8 * (k - 12))
            elif fill == "snare-roll":
                for k in range(8, 16):
                    hit(bar, k, 37, vel["snare"] - 30 + 4 * (k - 8))
            elif fill == "oh-end":
                hit(bar, 14, 39, vel["oh"])
                hit(bar, 15, 39, vel["oh"] - 6)
            elif fill == "double-kick":
                hit(bar, 11, 36, vel["kick"] - 12)
                hit(bar, 15, 36, vel["kick"] - 8)
            elif fill == "drop-kick":
                hit(bar, 12, 39, vel["oh"])
    return notes, "%s-k%d-h%d-%s" % (style, idx["kick"], idx["hh"], fill)


def gen_percussion(rng, root, bars):
    """Four one-shot voices (pitches 36..39 = A..D, the engine's percussion slots): a style draws one PERC_TEMPLATES
    mask per voice (an empty mask rests the voice), on-beat hits are accented and 16th off-beats softened, the even
    bars may thin the weak hits, the last bar may end in a 16th run on a mid or high voice. A hit always sounds in
    the last two beats so the firmware keeps the whole loop. Library: 13 notes per bar p50, 46 % off-grid."""
    style = pick(rng, PERC_STYLES)
    t = PERC_TEMPLATES[style]
    masks = {v: pick(rng, t[v]) for v in "ABCD"}
    if sum(1 for m in masks.values() if m) < 2:
        masks["A"] = masks["A"] or "x.......x......."
        masks["C"] = masks["C"] or "..x...x...x...x."
    pitch = {"A": 36, "B": 37, "C": 38, "D": 39}
    vel = {"A": rng.choice([92, 100, 108]), "B": rng.choice([84, 92, 100]), "C": rng.choice([64, 72, 80]), "D": rng.choice([76, 84, 92])}
    soften = rng.choice([0, 10, 16])
    thin = rng.random() < 0.4
    fill = rng.random() < 0.5
    notes = []
    for bar in range(bars):
        for v in "ABCD":
            m = masks[v]
            if not m:
                continue
            for k in range(16):
                if m[k] != "x" or (thin and bar % 2 == 1 and k % 4 != 0 and rng.random() < 0.15):
                    continue
                vv = vel[v] + (6 if k % 4 == 0 else 0) - (soften if k % 2 == 1 else 0)
                notes.append([bar * BAR + k * STEP, UNIT, pitch[v], vv + rng.randint(-4, 4)])
        if fill and bar == bars - 1:
            v = rng.choice(["B", "C"])
            for k in range(12, 16):
                notes.append([bar * BAR + k * STEP, UNIT, pitch[v], vel[v] - 20 + 7 * (k - 12)])
    if not any(s == 0 for s, _, _, _ in notes):
        notes.append([0, UNIT, 36, vel["A"] + 6])
    if max(s + l for s, l, _, _ in notes) < bars * BAR - TRAILING:
        notes.append([bars * BAR - 2 * STEP, UNIT, pitch["D" if masks["D"] else "C"], vel["D"]])
    return notes, "%s-%s" % (style, "".join(v for v in "ABCD" if masks[v]))


GENERATORS = {"Chord": gen_chord, "Pad": gen_pad, "Lead": gen_lead,
              "Bass": gen_bass, "Fx": gen_fx, "Beat": gen_beat, "Percussion": gen_percussion}
BARS = {"Chord": [(2, 35), (4, 65)], "Pad": [(2, 25), (4, 75)], "Lead": [(2, 25), (4, 75)],
        "Bass": [(2, 60), (4, 40)], "Fx": [(2, 65), (4, 35)], "Beat": [(2, 55), (4, 45)], "Percussion": [(2, 35), (4, 65)]}


# ----------------------------------------------------------------------------------------------- humanise / finalize
def humanise(rng, notes, cat, length):
    """Move a share of onsets off the grid by 3..12 ticks (see HUMANISE). Whole chords move together except
    in strum mode; the tick-0 onset never moves; a shifted onset keeps its length but stays inside the loop."""
    mode = pick(rng, HUMANISE[cat])
    if mode == "none":
        return notes, mode
    groups = defaultdict(list)
    for n in notes:
        groups[n[0]].append(n)
    starts = sorted(groups)
    for s in starts:
        if s == 0:
            continue
        grp = groups[s]
        if mode == "loose":
            if rng.random() < 0.4:
                d = rng.randint(3, 8) * rng.choice([1, -1])
                for n in grp:
                    n[0] = max(1, min(length - UNIT, s + d))
        elif mode == "late":                                     # swing: odd 16ths late, some 8th offbeats too
            step = s // STEP
            p = 0.85 if (s % STEP == 0 and step % 2 == 1) else (0.4 if (s % STEP == 0 and step % 4 == 2) else 0.0)
            if rng.random() < p:
                d = rng.randint(4, 12)
                for n in grp:
                    n[0] = min(length - UNIT, s + d)
        elif mode == "strum":
            if len(grp) > 1 and rng.random() < 0.6:
                step = rng.randint(3, 5)
                for vi, n in enumerate(sorted(grp, key=lambda n: n[2])):
                    n[0] = min(length - UNIT, s + vi * step)
    if cat in ("Lead", "Bass"):                                  # keep the line monophonic after the shifts
        notes.sort(key=lambda n: n[0])
        for i in range(len(notes) - 1):
            gap = notes[i + 1][0] - notes[i][0]
            if gap < UNIT:
                notes[i + 1][0] = notes[i][0] + UNIT
                gap = UNIT
            notes[i][1] = min(notes[i][1], (gap // UNIT) * UNIT)
    return notes, mode


def finalize(notes, cat, bars):
    """Clamp to the band, cut at the loop end, canonical order, firmware rules. Returns (notes, None) or
    (None, reason)."""
    length = bars * BAR
    lo, hi = BANDS[cat][0], BANDS[cat][1]
    out = []
    for s, l, p, v in notes:
        if s < 0 or s >= length:
            continue
        p = fold_into(p, lo, hi)
        if p is None:
            continue
        l = max(UNIT, (l // UNIT) * UNIT)
        if s + l > length:
            l = ((length - s) // UNIT) * UNIT
            if l < UNIT:
                continue
        out.append((s, p, l, max(1, min(127, v))))
    # exact duplicates (same start and pitch) would read as polyphony to the firmware's lead variation
    seen = set()
    uniq = []
    for n in sorted(out):
        if (n[0], n[1]) in seen:
            continue
        seen.add((n[0], n[1]))
        uniq.append(n)
    # no overlapping notes of one pitch: an SMF pairs note-offs FIFO per pitch, so overlaps would read back
    # with swapped lengths (and the engine steals the voice at the retrigger anyway)
    by_pitch = defaultdict(list)
    for i, (s, p, l, v) in enumerate(uniq):
        by_pitch[p].append(i)
    drop = set()
    for p, idx in by_pitch.items():
        for a, b in zip(idx, idx[1:]):
            s1, _, l1, v1 = uniq[a]
            s2 = uniq[b][0]
            if s1 + l1 > s2:
                l1 = ((s2 - s1) // UNIT) * UNIT
                if l1 < UNIT:
                    drop.add(a)
                else:
                    uniq[a] = (s1, p, l1, v1)
    out = [(s, l, p, v) for i, (s, p, l, v) in enumerate(uniq) if i not in drop]
    if not out:
        return None, "empty"
    if out[0][0] != 0:
        return None, "no downbeat"
    if len(out) > MAX_NOTES:
        return None, "over 192 notes"
    if max(s + l for s, l, p, v in out) < length - TRAILING:
        return None, "silent tail"
    med = sorted(p for _, _, p, _ in out)[len(out) // 2]
    if not BANDS[cat][2] <= med <= BANDS[cat][3]:
        return None, "median out of band"
    return out, None


# ----------------------------------------------------------------------------------------------- SMF
def canonical(notes):
    return sorted(notes, key=lambda n: (n[0], n[2], n[1], n[3]))


def content_digest(notes, length):
    h = hashlib.sha1(struct.pack(">H", length))
    for s, l, p, v in canonical(notes):
        h.update(struct.pack(">HHBB", s, l, p, v))
    return h.hexdigest()


def clip_hash(cat, notes, length):
    """The name's hash8 (module docstring): Chord / Lead / Pad keep this tool's original digest, the categories added
    for the free card use the project's shared rule (tools/lp_smf.py note_hash)."""
    if cat in LEGACY_HASH:
        return content_digest(notes, length)[:8]
    return lp_smf.note_hash([(s, l, p, v) for s, l, p, v in notes])


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def smf_bytes(notes, length, bpm=TEMPO_BPM):
    """Type-0 SMF, the firmware writer's layout: tempo + 4/4 at tick 0, note-offs before note-ons on the same
    tick, end of track at the loop length."""
    events = []
    for s, l, p, v in notes:
        events.append((s, 1, p, v))
        events.append((min(s + l, length), 0, p, 0))
    events.sort(key=lambda e: (e[0], e[1], e[2]))
    body = bytearray(b"\x00\xff\x51\x03" + struct.pack(">I", int(60e6 / bpm))[1:] + b"\x00\xff\x58\x04\x04\x02\x18\x08")
    last = 0
    for t, on, p, v in events:
        body += vlq(t - last)
        last = t
        body += bytes([0x90, p, v]) if on else bytes([0x80, p, 0x40])
    body += vlq(max(length, last) - last) + b"\xff\x2f\x00"
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQN) + b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def read_smf(data):
    """Exact reader for the files this tool writes (and any single-track type-0/1 file): (ppq, length_ticks,
    notes[(start, len, pitch, vel)]) in source ticks, EOT as the length."""
    if data[:4] != b"MThd":
        raise ValueError("not SMF")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntracks, ppq = struct.unpack(">HHH", data[8:14])
    pos = 8 + hlen
    notes = []
    eot = 0
    for _ in range(ntracks):
        cid = data[pos:pos + 4]
        clen = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        pos += 8
        end = pos + clen
        if cid != b"MTrk":
            pos = end
            continue
        t = 0
        status = None
        active = defaultdict(list)
        p = pos
        while p < end:
            d = 0
            while True:
                b = data[p]
                p += 1
                d = (d << 7) | (b & 0x7F)
                if not b & 0x80:
                    break
            t += d
            b = data[p]
            if b == 0xFF:
                mtype = data[p + 1]
                ln = 0
                p += 2
                while True:
                    c = data[p]
                    p += 1
                    ln = (ln << 7) | (c & 0x7F)
                    if not c & 0x80:
                        break
                if mtype == 0x2F:
                    eot = max(eot, t)
                p += ln
                continue
            if b in (0xF0, 0xF7):
                ln = 0
                p += 1
                while True:
                    c = data[p]
                    p += 1
                    ln = (ln << 7) | (c & 0x7F)
                    if not c & 0x80:
                        break
                p += ln
                continue
            if b & 0x80:
                status = b
                p += 1
            kind = status & 0xF0
            if kind in (0xC0, 0xD0):
                p += 1
                continue
            d1, d2 = data[p], data[p + 1]
            p += 2
            if kind == 0x90 and d2 > 0:
                active[d1].append((t, d2))
            elif kind in (0x80, 0x90) and active[d1]:
                s, v = active[d1].pop(0)
                notes.append((s, max(1, t - s), d1, v))
        pos = end
    return ppq, eot, sorted(notes)


# ----------------------------------------------------------------------------------------------- QA
def load_refmodel():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "tests", "refmodel_smf.py"), os.path.join(os.getcwd(), "tests", "refmodel_smf.py")):
        if os.path.isfile(cand):
            spec = importlib.util.spec_from_file_location("refmodel_smf", cand)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            return mod
    return None


def grid_class(tick):
    if tick % PPQN == 0:
        return "1/4"
    if tick % (PPQN // 2) == 0:
        return "1/8"
    if tick % (PPQN // 4) == 0:
        return "1/16"
    if tick % (PPQN // 8) == 0:
        return "1/32"
    return "off-grid"


def len_class(ticks):
    beats = ticks / PPQN
    for name, lo, hi in (("<=1/16", 0, 0.3), ("1/8", 0.3, 0.6), ("1/4", 0.6, 1.2), ("1/2", 1.2, 2.5), ("1 bar", 2.5, 5),
                         ("2 bars", 5, 9)):
        if lo <= beats < hi:
            return name
    return ">2 bars"


def pct(xs, q):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def key_files(kdir):
    """(name, path) of every .mid in a key folder and its sNN shards (tools/shard_folders.py), sorted by name."""
    out = []
    for dp, dirs, fs in os.walk(kdir):
        dirs[:] = sorted(d for d in dirs if re.match(r"^s\d\d$", d))
        for fn in fs:
            if fn.endswith(".mid") and not fn.startswith("."):
                out.append((fn, os.path.join(dp, fn)))
    return sorted(out)


def qa(out_root, quiet=False):
    """Parse everything back and check the contract. Returns the report dict; prints a summary. Files without the GEN
    token (LDZ, the library) are another tool's and are only counted; shard subfolders are read like the folder."""
    ref = load_refmodel()
    if ref is None:
        print("warning: tests/refmodel_smf.py not found, the firmware-rule parse is skipped", file=sys.stderr)
    midi_root = os.path.join(out_root, "lp", "midi")
    report = {}
    for cat in CATEGORIES:
        cdir = os.path.join(midi_root, cat)
        if not os.path.isdir(cdir):
            continue
        st = defaultdict(list)
        errors = []
        digests = defaultdict(list)
        counters = {k: Counter() for k in ("bars", "grid", "note_len", "voicing", "degree", "humanise")}
        files = 0
        cells = []
        rhythms = []
        per_key = Counter()
        foreign = 0
        for key in sorted(os.listdir(cdir)):
            kdir = os.path.join(cdir, key)
            if not os.path.isdir(kdir):
                continue
            for fn, path in key_files(kdir):
                if "__GEN-" not in fn:                           # LDZ / library files: not this tool's, not checked
                    foreign += 1
                    continue
                files += 1
                per_key[key] += 1
                rel = "%s/%s/%s" % (cat, key, fn)
                m = NAME_RE.match(fn)
                if not m:
                    errors.append("name does not parse: " + rel)
                    continue
                if len(fn.encode("utf-8")) > MAX_NAME:
                    errors.append("name longer than %d bytes: %s" % (MAX_NAME, rel))
                if m.group(1) != key or m.group(2) != cat or (m.group(1) == "NA") != (cat in KEYLESS):
                    errors.append("label/folder mismatch: " + rel)
                root = None if m.group(1) == "NA" else NOTE_NAMES.index(m.group(1)[:-3])
                data = open(path, "rb").read()
                try:
                    ppq, eot, notes = read_smf(data)
                except Exception as e:                            # noqa: BLE001 - report, keep going
                    errors.append("unreadable (%s): %s" % (e, rel))
                    continue
                if ppq != PPQN:
                    errors.append("ppq %d: %s" % (ppq, rel))
                digest = content_digest(notes, eot)
                want = clip_hash(cat, notes, eot)
                if want != m.group(5):
                    errors.append("hash mismatch (%s != %s): %s" % (want, m.group(5), rel))
                digests[digest].append(rel)
                if ref is not None:
                    r = ref.parse(data)
                    if r is None:
                        errors.append("firmware parse failed: " + rel)
                        continue
                    L = r["L"]
                    fw_notes = r["notes"]
                    if r["trunc"]:
                        errors.append("truncated by the firmware rules: " + rel)
                    if len(fw_notes) != len(notes):
                        errors.append("firmware keeps %d of %d notes: %s" % (len(fw_notes), len(notes), rel))
                else:
                    L = eot
                if L not in (2 * BAR, 4 * BAR) or L != eot:
                    errors.append("length %d ticks (EOT %d), not 2 or 4 bars: %s" % (L, eot, rel))
                if not notes:
                    errors.append("no notes: " + rel)
                    continue
                if len(notes) > MAX_NOTES:
                    errors.append("%d notes: %s" % (len(notes), rel))
                if notes[0][0] != 0:
                    errors.append("first onset at %d, not 0: %s" % (notes[0][0], rel))
                lo, hi, mlo, mhi = BANDS[cat]
                ps = [p for _, _, p, _ in notes]
                if min(ps) < lo or max(ps) > hi:
                    errors.append("pitch %d..%d outside band %d..%d: %s" % (min(ps), max(ps), lo, hi, rel))
                med = sorted(ps)[len(ps) // 2]
                if not mlo <= med <= mhi:
                    errors.append("median pitch %d outside %d..%d: %s" % (med, mlo, mhi, rel))
                nond = sum(1 for p in ps if not in_key(root, p)) if root is not None else 0
                if nond > 0.25 * len(ps):                        # library nondiatonic share p90 = 25 %
                    errors.append("%d of %d notes outside %s natural minor: %s" % (nond, len(ps), key, rel))
                bars = L // BAR
                counters["bars"][bars] += 1
                st["notes_per_bar"].append(len(notes) / bars)
                onsets = sorted(set(s for s, _, _, _ in notes))
                st["onsets_per_bar"].append(len(onsets) / bars)
                st["pitch_lo"].append(min(ps))
                st["pitch_hi"].append(max(ps))
                st["median"].append(med)
                st["nondiatonic_pct"].append(100.0 * nond / len(ps))
                st["clip_has_borrowed"].append(1 if nond else 0)
                st["vel_mean"].append(sum(v for _, _, _, v in notes) / len(notes))
                groups = Counter(s for s, _, _, _ in notes)
                for s in onsets:
                    counters["grid"][grid_class(s)] += 1
                    counters["voicing"][min(groups[s], 8)] += 1
                for s, l, p, v in notes:
                    counters["note_len"][len_class(l)] += 1
                    if root is not None:
                        counters["degree"][(p - root) % 12] += 1
                covered = set()
                for s, l, p, v in notes:
                    for k in range(s // STEP, min(bars * 16, (s + l + STEP - 1) // STEP)):
                        covered.add(k)
                st["rest_pct"].append(100.0 * (1 - len(covered) / (bars * 16)))
                cells.append(frozenset((s // STEP, p % 12) for s, _, p, _ in notes))
                rhythms.append(frozenset(s // STEP for s, _, _, _ in notes))
        dups = {d: rels for d, rels in digests.items() if len(rels) > 1}
        for d, rels in dups.items():
            errors.append("exact duplicates (%s): %s" % (d[:8], ", ".join(rels)))
        # sameness over random pairs (deterministic sample)
        rng = random.Random(7)
        cj, rj = [], []
        for _ in range(min(6000, len(cells) * 20)):
            i, j = rng.randrange(len(cells)), rng.randrange(len(cells))
            if i == j:
                continue
            cj.append(len(cells[i] & cells[j]) / max(1, len(cells[i] | cells[j])))
            rj.append(len(rhythms[i] & rhythms[j]) / max(1, len(rhythms[i] | rhythms[j])))
        total_onsets = sum(counters["grid"].values()) or 1
        total_lens = sum(counters["note_len"].values()) or 1
        total_voic = sum(counters["voicing"].values()) or 1
        total_deg = sum(counters["degree"].values()) or 1
        rep = dict(
            files=files, other_tools_files=foreign, per_key=dict(sorted(per_key.items())), errors=errors,
            exact_duplicate_files=sum(len(v) - 1 for v in dups.values()),
            bars=dict(sorted(counters["bars"].items())),
            notes_per_bar_p10_p50_p90=(pct(st["notes_per_bar"], .1), pct(st["notes_per_bar"], .5), pct(st["notes_per_bar"], .9)),
            onsets_per_bar_p10_p50_p90=(pct(st["onsets_per_bar"], .1), pct(st["onsets_per_bar"], .5), pct(st["onsets_per_bar"], .9)),
            pitch_lo_p10_p50_p90=(pct(st["pitch_lo"], .1), pct(st["pitch_lo"], .5), pct(st["pitch_lo"], .9)),
            pitch_hi_p10_p50_p90=(pct(st["pitch_hi"], .1), pct(st["pitch_hi"], .5), pct(st["pitch_hi"], .9)),
            median_p10_p50_p90=(pct(st["median"], .1), pct(st["median"], .5), pct(st["median"], .9)),
            grid_pct={k: round(100 * v / total_onsets, 1) for k, v in counters["grid"].most_common()},
            offgrid_pct=round(100 * counters["grid"]["off-grid"] / total_onsets, 1),
            note_len_pct={k: round(100 * v / total_lens, 1) for k, v in counters["note_len"].most_common()},
            voicing_pct={k: round(100 * v / total_voic, 1) for k, v in sorted(counters["voicing"].items())},
            degree_pct={k: round(100 * v / total_deg, 1) for k, v in sorted(counters["degree"].items())},
            clips_with_borrowed_pct=round(100 * sum(st["clip_has_borrowed"]) / max(1, files), 1),
            nondiatonic_pct_p50_p90=(pct(st["nondiatonic_pct"], .5), pct(st["nondiatonic_pct"], .9)),
            rest_pct_p10_p50_p90=(pct(st["rest_pct"], .1), pct(st["rest_pct"], .5), pct(st["rest_pct"], .9)),
            vel_mean_p50=pct(st["vel_mean"], .5),
            cell_jaccard_p50_p90_p99=(round(pct(cj, .5), 3), round(pct(cj, .9), 3), round(pct(cj, .99), 3)) if cj else None,
            near_dup_pair_rate_pct=round(100 * sum(1 for x in cj if x > 0.6) / max(1, len(cj)), 2),
            rhythm_jaccard_p50_p90=(round(pct(rj, .5), 3), round(pct(rj, .9), 3)) if rj else None,
            distinct_rhythm_skeletons_pct=round(100 * len(set(rhythms)) / max(1, len(rhythms)), 1),
            distinct_cell_skeletons=len(set(cells)),
        )
        report[cat] = rep
        if not quiet:
            print_parity(cat, rep)
    return report


def print_parity(cat, rep):
    lib = LIBRARY_REF[cat]
    print("== %s: %d files, %d errors, %d exact duplicate files%s" % (cat, rep["files"], len(rep["errors"]), rep["exact_duplicate_files"],
          " (%d other tools' files beside them, not checked)" % rep["other_tools_files"] if rep["other_tools_files"] else ""))
    for e in rep["errors"][:30]:
        print("   ERROR " + e)
    if len(rep["errors"]) > 30:
        print("   ... %d more" % (len(rep["errors"]) - 30))
    rows = [
        ("bars", rep["bars"], lib["bars"]),
        ("notes/bar p10/p50/p90", rep["notes_per_bar_p10_p50_p90"], lib["notes_per_bar"]),
        ("onsets/bar p10/p50/p90", rep["onsets_per_bar_p10_p50_p90"], lib["onsets_per_bar"]),
        ("lowest pitch p10/p50/p90", rep["pitch_lo_p10_p50_p90"], lib["pitch_lo"]),
        ("highest pitch p10/p50/p90", rep["pitch_hi_p10_p50_p90"], lib["pitch_hi"]),
        ("off-grid onsets %", rep["offgrid_pct"], lib["offgrid_pct"]),
        ("onset grid %", rep["grid_pct"], lib["grid"]),
        ("note length %", rep["note_len_pct"], lib["note_len"]),
        ("voices per onset %", rep["voicing_pct"], lib["voicing"]),
        ("degree % (st from root)", rep["degree_pct"], lib["degree"]),
        ("rest % p10/p50/p90", rep["rest_pct_p10_p50_p90"], lib["rest_pct"]),
        ("velocity mean p50", rep["vel_mean_p50"], lib["vel_mean"]),
    ]
    for name, got, want in rows:
        print("   %-28s gen %-60s lib %s" % (name, json.dumps(got)[:60], json.dumps(want)[:50]))
    print("   %-28s %s" % ("clips with a borrowed note %", rep["clips_with_borrowed_pct"]))
    print("   %-28s p50/p90/p99 %s  near-dup pairs %.2f %%  distinct rhythm skeletons %.1f %%  distinct pitch+rhythm skeletons %d of %d"
          % ("cell Jaccard", rep["cell_jaccard_p50_p90_p99"], rep["near_dup_pair_rate_pct"], rep["distinct_rhythm_skeletons_pct"],
             rep["distinct_cell_skeletons"], rep["files"]))


# ----------------------------------------------------------------------------------------------- main
def parse_counts(text):
    counts = dict(DEFAULT_COUNTS)
    if text:
        for part in text.split(","):
            cat, n = part.split("=")
            if cat not in CATEGORIES:
                sys.exit("unknown category %r (%s)" % (cat, ", ".join(CATEGORIES)))
            counts[cat] = int(n)
    return counts


def generate(out_root, keys, counts, seed0, max_factor, humanize, keep, quiet):
    gen_report = {}
    for cat in CATEGORIES:
        want = counts.get(cat, 0)
        if want <= 0:
            continue
        cat_rep = {}
        for key in (["NA"] if cat in KEYLESS else keys):
            root = None if key == "NA" else NOTE_NAMES.index(key[:-3])
            folder = os.path.join(out_root, "lp", "midi", cat, key)
            if os.path.isdir(folder) and not keep:
                shutil.rmtree(folder)
            os.makedirs(folder, exist_ok=True)
            seen = set()
            if keep:
                for fn in os.listdir(folder):
                    m = NAME_RE.match(fn)
                    if m:
                        _, eot, notes = read_smf(open(os.path.join(folder, fn), "rb").read())
                        seen.add(content_digest(notes, eot))
            written = 0
            draws = 0
            dups = 0
            rejected = Counter()
            modes = Counter()
            sizes = []
            seed = seed0
            while written < want and draws < want * max_factor:
                this_seed = seed
                seed += 1
                rng = seed_rng(cat, key, this_seed)
                draws += 1
                bars = pick(rng, BARS[cat])
                notes, style = GENERATORS[cat](rng, root, bars)
                mode = "none"
                if humanize:
                    notes, mode = humanise(rng, notes, cat, bars * BAR)
                notes, why = finalize(notes, cat, bars)
                if notes is None:
                    rejected[why] += 1
                    continue
                length = bars * BAR
                digest = content_digest(notes, length)
                if digest in seen:
                    dups += 1
                    continue
                seen.add(digest)
                modes[mode] += 1
                name = "%s__%s__GEN-%s-%04d__%s.mid" % (key, cat, style, this_seed, clip_hash(cat, notes, length))
                if not NAME_RE.match(name) or len(name.encode()) > MAX_NAME:
                    rejected["bad name"] += 1
                    continue
                data = smf_bytes(canonical(notes), length)
                with open(os.path.join(folder, name), "wb") as f:
                    f.write(data)
                sizes.append(len(data))
                written += 1
            cat_rep[key] = dict(written=written, draws=draws, exact_duplicates=dups, duplicate_rate_pct=round(100 * dups / max(1, draws), 1),
                                rejected=dict(rejected), humanise=dict(modes), bytes=sum(sizes),
                                bytes_mean=round(sum(sizes) / max(1, len(sizes))))
            if not quiet:
                print("%-5s %-6s written %4d of %d draws, duplicates %d (%.1f %%), rejected %s, humanise %s, %d B mean"
                      % (cat, key, written, draws, dups, 100 * dups / max(1, draws), dict(rejected) or "-", dict(modes), cat_rep[key]["bytes_mean"]))
        gen_report[cat] = cat_rep
    return gen_report


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="staging root: files go to <out>/lp/midi/<Category>/<Key>/")
    ap.add_argument("--keys", default="all", help="comma-separated minor labels (Amin,Emin,...) or 'all' (default); Beat and Percussion ignore it (NA)")
    ap.add_argument("--counts", help="per-folder targets, e.g. Lead=150,Pad=120,Chord=60,Bass=60,Fx=20,Beat=240,Percussion=120 (the defaults; 0 skips)")
    ap.add_argument("--seed0", type=int, default=1, help="first draw number (default 1)")
    ap.add_argument("--max-draw-factor", type=int, default=8, help="give up after count x this draws per folder (default 8)")
    ap.add_argument("--no-humanise", action="store_true", help="keep every onset on the 16th grid")
    ap.add_argument("--keep", action="store_true", help="add to existing folders instead of regenerating them")
    ap.add_argument("--check-only", action="store_true", help="only run the QA pass over --out")
    ap.add_argument("--json", help="write the generation and QA report here")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    keys = MINOR_KEYS if args.keys == "all" else [k.strip() for k in args.keys.split(",")]
    for k in keys:
        if k not in MINOR_KEYS:
            sys.exit("unknown key %r; use one of %s" % (k, " ".join(MINOR_KEYS)))
    out_root = os.path.abspath(args.out)
    report = {"out": out_root}
    if not args.check_only:
        counts = parse_counts(args.counts)
        report["generation"] = generate(out_root, keys, counts, args.seed0, args.max_draw_factor, not args.no_humanise, args.keep, args.quiet)
    report["qa"] = qa(out_root, args.quiet)
    errors = sum(len(r["errors"]) for r in report["qa"].values())
    files = sum(r["files"] for r in report["qa"].values())
    size = 0
    for dp, _, fs in os.walk(os.path.join(out_root, "lp", "midi")):
        size += sum(os.path.getsize(os.path.join(dp, f)) for f in fs if f.endswith(".mid"))
    report["files"] = files
    report["bytes"] = size
    print("%d files, %.2f MB, %d QA errors" % (files, size / 1e6, errors))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=1, default=str)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
