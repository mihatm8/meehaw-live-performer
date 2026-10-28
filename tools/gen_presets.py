#!/usr/bin/env python3
"""Generates firmware/liveplayer/src/dsp/presets_gen.hpp: the synth preset banks (SPEC §5, §9.17b; research
docs/RESEARCH_LIBRARY_EXPANSION.md §4 "generated preset bank").

Every bank is one flash-resident `constexpr SynthPatch k<Category>[]` laid out as
    [hand library rows][generated library rows][hand '303' rows][generated '303' rows]
so the hand-written presets keep their rows 0..N-1 of each family and the '303' (acid) family stays appended last
(presets.h: library = [0, libraryPresetCount), acid = [libraryPresetCount, presetCount)). Per category: the hand
library rows + 128 generated library rows, and (Chord, Lead, Pad, Bass) the 8 hand '303' rows + 16 generated '303'
rows; Fx has no acid family. 782 presets in all.

Index compatibility (SPEC §9.22b): a library index saved before this bank still means the same hand row, but the
hand '303' rows moved from right after the hand library (Chord [10, 18), Lead / Pad / Bass [9, 17)) to
[libraryPresetCount, +8), which the pinned presets.h contract cannot avoid once 128 library rows sit before them.
gen/sets.cpp (presetForLayout) maps that old range for a file without the lp.banks line (files written since carry
lp.banks=2). Its table (oldHandLibrary 10 / 9 / 9 / 9, oldHandAcid 8) is the OLD layout, so never reorder or drop
the hand rows here: the hand library rows stay rows 0..N-1 and the hand '303' rows the first 8 of the acid family.

The rows are emitted as calls of a tiny constexpr builder (presetgen::make, in the generated header) that assigns
SynthPatch members BY NAME, so the table does not depend on the member order of core/types.h and compiles with or
without the `sat` / `accent` fields of SPEC §9.22 (detected at compile time; the values are in the table either way:
'303' rows sat 0.5-0.8 / accent 0.5-0.8, everything else 0 / 0 -- the SynthPatch contract keeps both OFF for library
presets so velocity-100 library clips are never accented wholesale; the '303' families opt in). The initialisers are
constant expressions, so
the arrays are constant-initialised into .rodata (flash) exactly like plain aggregates: 0 B of RAM.

Generated rows come from per-family archetype tables with musical ranges (seeded, deterministic: the names and
values never change unless this file does), display names "<Archetype> <Adjective>" / "303 <Adjective>" of at most
12 characters, unique per category. Each generated row carries a baked LEVEL TRIM: tools/gen_presets_trims.json
holds the level that puts its host-rendered loudness (tools/preset_loudness.hpp: loudest 50 ms RMS of a fixed phrase
at 120 BPM) within +-1 dB of its family's target, the median of that family's hand rows (the hand rows are the
reference and are never trimmed). Levels are clamped to [0.05, 1.0] and capped so one voice peaks below 1.45 at
C4 vel 100 and at MIDI 24 / 108 vel 127 (tests/test_synth.cpp bounds); a capped row is reported as an outlier.

Usage (standard library only; run from anywhere):
    python3 -I tools/gen_presets.py            emit the header from the archetypes + the stored trims
    python3 -I tools/gen_presets.py --qa       emit, build + run tools/presets_qa.cpp (clang++), bake the trims
                                               into gen_presets_trims.json, re-emit, re-measure, print the QA
                                               summary (exit 1 when a generated row misses its target by > 1.5 dB)
    python3 -I tools/gen_presets.py --check    like --qa but never writes: verify the committed header
Options: --seed N (default 2026), --out PATH, --trims PATH.
"""
import argparse
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SRC = os.path.join(ROOT, "firmware", "liveplayer", "src")
DEFAULT_OUT = os.path.join(SRC, "dsp", "presets_gen.hpp")
DEFAULT_TRIMS = os.path.join(HERE, "gen_presets_trims.json")

CATEGORIES = ["Chord", "Lead", "Pad", "Bass", "Fx"]
GEN_LIBRARY = 128          # generated library rows per category
GEN_ACID = 16              # generated '303' rows per acid category (Chord, Lead, Pad, Bass)
ACID_CATEGORIES = ("Chord", "Lead", "Pad", "Bass")
NAME_MAX = 12
TOLERANCE_DB = 1.5         # a generated row farther than this from its target is an outlier (the test's bound)
PEAK_CAP = 1.45            # single-voice peaks the tests bound at 1.5
LEVEL_MIN, LEVEL_MAX = 0.05, 1.0

# SynthPatch initialiser order (core/types.h), the hand rows below and presetgen::make use it:
FIELDS = ["w1", "w2", "det", "l2", "oct", "sub", "noise", "fm", "cut", "res", "fenv", "kt",
          "a", "d", "s", "r", "fa", "fd", "gl", "lr", "lc", "lp", "drv", "lvl", "poly", "name", "sat", "acc"]
INT_FIELDS = {"oct", "fm", "poly"}
WAVE_FIELDS = {"w1", "w2"}

# ------------------------------------------------------------------------------------------------ hand-written rows
# Verbatim from the former presets.cpp tables (numbers as written there, so the emitted literals are identical):
#   w1, w2, osc2Detune(st), osc2Level, osc2Octave, subLevel, noiseLevel, filterMode(0 LP 1 BP 2 HP),
#   cutoff(Hz), resonance, filterEnv(oct), keyTrack, attack, decay, sustain, release, fAttack, fDecay,
#   glide(s), lfoRate(Hz), lfoToCutoff(oct), lfoToPitch(st), drive, level, polyphony, name
# The '303' rows additionally get sat / accent (SPEC §9.22) from the seeded generator (0.5-0.8 each).
HAND = {
  "Chord": {
    "lib": [  # plucks, stabs, organ, e-piano-ish, chord pad. 4 voices stack, so levels stay ~ 0.35
      ("WSaw", "WSaw", "0.12", "0.6", 0, "0.0", "0.0", 0, "1800.", "0.20", "2.5", "0.4", "0.003", "0.30", "0.0", "0.25", "0.002", "0.20", "0.0", "0.0", "0.0", "0.0", "0.0", "0.35", 4, "PluckWide"),
      ("WTri", "WSine", "0.05", "0.5", 1, "0.0", "0.0", 0, "2500.", "0.10", "1.5", "0.5", "0.004", "0.40", "0.0", "0.30", "0.002", "0.15", "0.0", "0.0", "0.0", "0.0", "0.0", "0.40", 4, "PluckSoft"),
      ("WSaw", "WFat", "0.08", "0.7", 0, "0.0", "0.0", 0, "1200.", "0.35", "3.0", "0.5", "0.002", "0.18", "0.15", "0.12", "0.002", "0.12", "0.0", "0.0", "0.0", "0.0", "0.20", "0.35", 4, "StabSaw"),
      ("WFat", "WSquare", "0.10", "0.4", -1, "0.0", "0.0", 0, "2200.", "0.30", "2.0", "0.4", "0.002", "0.22", "0.10", "0.15", "0.002", "0.10", "0.0", "0.0", "0.0", "0.0", "0.15", "0.35", 4, "StabHouse"),
      ("WSine", "WSine", "0.0", "0.6", 1, "0.3", "0.0", 0, "6000.", "0.0", "0.0", "0.3", "0.010", "0.05", "1.0", "0.08", "0.005", "0.05", "0.0", "5.5", "0.0", "0.08", "0.0", "0.35", 4, "Organ"),
      ("WSquare", "WSine", "0.0", "0.5", 1, "0.0", "0.0", 0, "3500.", "0.10", "1.0", "0.3", "0.002", "0.03", "0.9", "0.06", "0.001", "0.02", "0.0", "0.0", "0.0", "0.0", "0.0", "0.30", 4, "OrganClick"),
      ("WSine", "WSine", "0.03", "0.35", 2, "0.0", "0.0", 0, "4000.", "0.10", "2.0", "0.6", "0.002", "0.90", "0.2", "0.35", "0.001", "0.25", "0.0", "0.0", "0.0", "0.0", "0.0", "0.45", 4, "EPiano"),
      ("WTri", "WSine", "0.02", "0.3", 1, "0.0", "0.0", 0, "2800.", "0.05", "1.5", "0.5", "0.003", "1.20", "0.25", "0.40", "0.001", "0.35", "0.0", "0.0", "0.0", "0.0", "0.0", "0.45", 4, "EPianoSoft"),
      ("WFat", "WSaw", "0.18", "0.7", 0, "0.0", "0.0", 0, "1400.", "0.20", "1.0", "0.3", "0.080", "0.50", "0.7", "0.50", "0.050", "0.40", "0.0", "0.3", "0.5", "0.0", "0.0", "0.30", 4, "ChordPad"),
      ("WSaw", "WTri", "0.10", "0.6", -1, "0.0", "0.0", 0, "1000.", "0.15", "1.5", "0.4", "0.020", "0.60", "0.6", "0.40", "0.010", "0.30", "0.0", "0.0", "0.0", "0.0", "0.0", "0.33", 4, "ChordWarm"),
    ],
    "acid": [  # resonant saw / square stabs, polyphony 3-4, decays 0.12-0.25 s, resonance 0.4-0.8, filter env 1.5-3 oct
      ("WSaw", "WSaw", "0.06", "0.5", 0, "0.0", "0.0", 0, "900.", "0.55", "2.5", "0.4", "0.002", "0.18", "0.0", "0.10", "0.001", "0.16", "0.0", "0.0", "0.0", "0.0", "0.20", "0.50", 4, "303 Stab"),
      ("WSquare", "WSquare", "0.04", "0.4", 0, "0.0", "0.0", 0, "800.", "0.60", "2.0", "0.4", "0.002", "0.15", "0.0", "0.10", "0.001", "0.14", "0.0", "0.0", "0.0", "0.0", "0.25", "0.50", 4, "303 SqrStab"),
      ("WSaw", "WSquare", "0.08", "0.5", 0, "0.0", "0.0", 0, "1200.", "0.70", "3.0", "0.5", "0.002", "0.12", "0.0", "0.08", "0.001", "0.10", "0.0", "0.0", "0.0", "0.0", "0.30", "0.49", 3, "303 Snap"),
      ("WSaw", "WSaw", "0.10", "0.6", -1, "0.0", "0.0", 0, "600.", "0.75", "2.5", "0.3", "0.002", "0.22", "0.05", "0.12", "0.001", "0.22", "0.0", "0.0", "0.0", "0.0", "0.15", "0.50", 4, "303 Rubber"),
      ("WSquare", "WSaw", "0.05", "0.5", 0, "0.0", "0.0", 0, "1500.", "0.80", "2.0", "0.5", "0.002", "0.14", "0.0", "0.10", "0.001", "0.12", "0.0", "0.0", "0.0", "0.0", "0.40", "0.46", 3, "303 Bite"),
      ("WSaw", "WSaw", "0.03", "0.3", 1, "0.0", "0.0", 0, "700.", "0.45", "1.5", "0.4", "0.002", "0.25", "0.10", "0.14", "0.001", "0.28", "0.0", "0.0", "0.0", "0.0", "0.10", "0.54", 4, "303 Soft"),
      ("WSquare", "WSquare", "0.07", "0.5", 0, "0.0", "0.0", 0, "1000.", "0.65", "2.8", "0.4", "0.002", "0.16", "0.0", "0.10", "0.001", "0.18", "0.0", "0.0", "0.0", "0.0", "0.35", "0.49", 4, "303 Hollow"),
      ("WSaw", "WSaw", "0.12", "0.6", 0, "0.0", "0.0", 0, "1100.", "0.50", "2.2", "0.4", "0.002", "0.20", "0.0", "0.12", "0.001", "0.20", "0.0", "0.0", "0.0", "0.0", "0.50", "0.46", 3, "303 Dirty"),
    ],
  },
  "Lead": {
    "lib": [  # saw, square, pluck, bell-ish, hoover-ish
      ("WSaw", "WSaw", "0.15", "0.7", 0, "0.0", "0.0", 0, "3000.", "0.25", "2.0", "0.5", "0.005", "0.20", "0.7", "0.15", "0.002", "0.20", "0.0", "5.0", "0.0", "0.10", "0.0", "0.45", 2, "SawLead"),
      ("WSquare", "WSquare", "0.10", "0.5", -1, "0.0", "0.0", 0, "2500.", "0.20", "1.5", "0.4", "0.005", "0.25", "0.7", "0.15", "0.002", "0.20", "0.0", "0.0", "0.0", "0.0", "0.0", "0.45", 2, "SquareLead"),
      ("WSaw", "WTri", "0.05", "0.4", 1, "0.0", "0.0", 0, "1500.", "0.30", "3.5", "0.5", "0.002", "0.25", "0.0", "0.20", "0.001", "0.15", "0.0", "0.0", "0.0", "0.0", "0.0", "0.50", 2, "PluckLead"),
      ("WSine", "WSine", "3.86", "0.6", 1, "0.0", "0.0", 0, "6000.", "0.0", "3.0", "0.8", "0.001", "1.00", "0.0", "0.50", "0.001", "0.40", "0.0", "0.0", "0.0", "0.0", "0.0", "0.50", 2, "Bell"),   # osc2 at x2.5: inharmonic bell partial
      ("WFat", "WSaw", "0.30", "0.8", -1, "0.2", "0.0", 0, "2800.", "0.30", "1.5", "0.4", "0.010", "0.30", "0.8", "0.25", "0.005", "0.20", "0.08", "6.0", "0.0", "0.15", "0.30", "0.40", 2, "Hoover"),
      ("WSquare", "WSaw", "0.40", "0.6", 0, "0.0", "0.0", 0, "3500.", "0.40", "2.5", "0.5", "0.003", "0.30", "0.6", "0.15", "0.002", "0.20", "0.0", "0.0", "0.0", "0.0", "0.40", "0.40", 2, "SyncLead"),
      ("WTri", "WSine", "0.02", "0.3", 1, "0.0", "0.03", 0, "2000.", "0.10", "1.0", "0.5", "0.030", "0.20", "0.8", "0.20", "0.020", "0.20", "0.0", "5.5", "0.0", "0.12", "0.0", "0.50", 2, "FluteLead"),
      ("WSaw", "WSquare", "0.02", "0.3", 0, "0.0", "0.0", 0, "900.", "0.65", "3.5", "0.6", "0.002", "0.30", "0.3", "0.15", "0.001", "0.25", "0.06", "0.0", "0.0", "0.0", "0.30", "0.45", 1, "AcidLead"),
      ("WSquare", "WSine", "0.0", "0.5", 1, "0.0", "0.0", 0, "5000.", "0.15", "2.5", "0.6", "0.001", "0.18", "0.0", "0.15", "0.001", "0.10", "0.0", "0.0", "0.0", "0.0", "0.0", "0.50", 2, "DigitalPluck"),
    ],
    "acid": [  # mono saw / square, LP SVF, resonance 0.6-0.9, filter env 1-3 oct, fDecay 0.1-0.4 s, glide 0.04-0.08 s
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "800.", "0.75", "2.5", "0.4", "0.002", "0.25", "0.30", "0.12", "0.001", "0.22", "0.06", "0.0", "0.0", "0.0", "0.15", "0.61", 1, "303 Saw"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "700.", "0.70", "2.0", "0.4", "0.002", "0.25", "0.30", "0.12", "0.001", "0.20", "0.06", "0.0", "0.0", "0.0", "0.20", "0.61", 1, "303 Square"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "500.", "0.85", "3.0", "0.3", "0.002", "0.30", "0.25", "0.10", "0.001", "0.30", "0.05", "0.0", "0.0", "0.0", "0.25", "0.61", 1, "303 Rubber"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "1000.", "0.65", "1.5", "0.5", "0.002", "0.22", "0.30", "0.12", "0.001", "0.15", "0.08", "0.0", "0.0", "0.0", "0.10", "0.61", 1, "303 Hollow"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "1500.", "0.90", "3.0", "0.5", "0.002", "0.20", "0.25", "0.10", "0.001", "0.12", "0.04", "0.0", "0.0", "0.0", "0.40", "0.55", 1, "303 Screech"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "400.", "0.60", "1.2", "0.3", "0.002", "0.35", "0.40", "0.15", "0.001", "0.40", "0.07", "0.0", "0.0", "0.0", "0.05", "0.61", 1, "303 Dub"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "900.", "0.80", "2.2", "0.4", "0.002", "0.25", "0.30", "0.10", "0.001", "0.18", "0.05", "0.0", "0.0", "0.0", "0.55", "0.55", 1, "303 Nasty"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "1200.", "0.72", "2.8", "0.5", "0.002", "0.18", "0.25", "0.10", "0.001", "0.10", "0.05", "0.0", "0.0", "0.0", "0.30", "0.59", 1, "303 Snap"),
    ],
  },
  "Pad": {
    "lib": [  # slow attack >= 0.3 s, LFO, detuned osc2
      ("WSaw", "WSaw", "0.22", "0.8", 0, "0.0", "0.0", 0, "1200.", "0.15", "1.0", "0.3", "0.40", "0.80", "0.80", "0.80", "0.30", "0.80", "0.0", "0.25", "0.6", "0.0", "0.0", "0.30", 4, "WarmPad"),
      ("WTri", "WSine", "0.15", "0.6", 1, "0.0", "0.0", 0, "3500.", "0.10", "0.5", "0.4", "0.50", "1.00", "0.85", "1.00", "0.30", "0.80", "0.0", "0.40", "0.3", "0.10", "0.0", "0.35", 4, "GlassPad"),
      ("WFat", "WSaw", "0.30", "0.7", 0, "0.0", "0.0", 0, "2200.", "0.10", "0.8", "0.3", "0.35", "0.60", "0.85", "0.70", "0.20", "0.60", "0.0", "5.2", "0.15", "0.08", "0.0", "0.28", 4, "StringPad"),
      ("WSaw", "WSquare", "0.18", "0.5", -1, "0.3", "0.0", 0, "700.", "0.25", "0.8", "0.2", "0.60", "1.00", "0.90", "1.20", "0.40", "1.00", "0.0", "0.15", "0.8", "0.0", "0.0", "0.30", 4, "DarkPad"),
      ("WFat", "WFat", "0.25", "0.8", 0, "0.0", "0.0", 0, "600.", "0.40", "2.5", "0.3", "0.30", "1.00", "0.80", "0.90", "1.50", "2.00", "0.0", "0.12", "1.0", "0.0", "0.0", "0.28", 4, "SweepPad"),
      ("WSquare", "WTri", "0.12", "0.6", 1, "0.0", "0.0", 1, "900.", "0.30", "0.0", "0.3", "0.45", "0.80", "0.80", "0.80", "0.30", "0.80", "0.0", "0.30", "0.7", "0.0", "0.0", "0.35", 4, "HollowPad"),
      ("WSine", "WTri", "0.10", "0.5", 1, "0.0", "0.06", 0, "4000.", "0.05", "0.0", "0.3", "0.80", "1.00", "0.90", "1.50", "0.50", "1.00", "0.0", "0.20", "0.4", "0.05", "0.0", "0.35", 4, "AirPad"),
      ("WSaw", "WSaw", "0.40", "0.9", 0, "0.0", "0.0", 1, "1400.", "0.35", "0.0", "0.3", "0.50", "0.80", "0.85", "1.00", "0.30", "0.80", "0.0", "0.35", "0.5", "0.0", "0.0", "0.30", 4, "ChoirPad"),
      ("WSaw", "WSaw", "0.10", "0.7", 0, "0.0", "0.0", 0, "1500.", "0.20", "2.0", "0.3", "0.30", "0.50", "0.80", "0.50", "0.25", "0.50", "0.0", "0.30", "0.3", "0.0", "0.20", "0.30", 4, "BrassPad"),
    ],
    "acid": [  # stabs: 2-5 ms attack, amp decay <= 0.25 s, low sustain, resonance 0.4-0.8; the pad LFO and detuned osc 2 kept
      ("WSaw", "WSaw", "0.15", "0.6", 0, "0.0", "0.0", 0, "900.", "0.55", "2.5", "0.4", "0.003", "0.22", "0.10", "0.20", "0.001", "0.20", "0.0", "0.20", "0.3", "0.0", "0.15", "0.50", 4, "303 PadStab"),
      ("WSquare", "WSquare", "0.12", "0.5", 0, "0.0", "0.0", 0, "800.", "0.60", "2.0", "0.4", "0.003", "0.20", "0.10", "0.20", "0.001", "0.18", "0.0", "0.25", "0.3", "0.0", "0.20", "0.50", 4, "303 SqrPad"),
      ("WSaw", "WSquare", "0.10", "0.5", -1, "0.0", "0.0", 0, "600.", "0.75", "3.0", "0.3", "0.004", "0.25", "0.15", "0.22", "0.001", "0.25", "0.0", "0.15", "0.4", "0.0", "0.15", "0.50", 4, "303 Rubber"),
      ("WSaw", "WSaw", "0.20", "0.7", 0, "0.0", "0.0", 0, "1200.", "0.70", "2.5", "0.5", "0.002", "0.18", "0.05", "0.18", "0.001", "0.12", "0.0", "0.30", "0.3", "0.0", "0.30", "0.46", 3, "303 Bite"),
      ("WSquare", "WSaw", "0.08", "0.5", 0, "0.0", "0.0", 0, "1000.", "0.80", "2.2", "0.4", "0.003", "0.22", "0.10", "0.20", "0.001", "0.16", "0.0", "0.20", "0.5", "0.0", "0.35", "0.46", 3, "303 Hollow"),
      ("WSaw", "WSaw", "0.18", "0.6", 1, "0.0", "0.0", 0, "700.", "0.45", "1.5", "0.4", "0.005", "0.25", "0.20", "0.25", "0.001", "0.28", "0.0", "0.12", "0.3", "0.0", "0.10", "0.50", 4, "303 Soft"),
      ("WSquare", "WSquare", "0.14", "0.5", 0, "0.0", "0.0", 0, "1400.", "0.65", "2.8", "0.5", "0.002", "0.16", "0.05", "0.16", "0.001", "0.10", "0.0", "0.35", "0.4", "0.0", "0.40", "0.46", 4, "303 Snap"),
      ("WSaw", "WSaw", "0.22", "0.7", 0, "0.0", "0.0", 0, "1100.", "0.50", "2.0", "0.4", "0.003", "0.22", "0.10", "0.20", "0.001", "0.22", "0.0", "0.18", "0.3", "0.0", "0.50", "0.46", 3, "303 Dirty"),
    ],
  },
  "Bass": {
    "lib": [  # sub sine, 303-like (glide 60 ms + resonance), reese detuned saws, fm-ish square + sub
      ("WSine", "WSine", "0.0", "0.0", 0, "0.5", "0.0", 0, "400.", "0.0", "0.5", "0.2", "0.005", "0.30", "0.8", "0.12", "0.002", "0.20", "0.0", "0.0", "0.0", "0.0", "0.0", "0.60", 1, "SubSine"),
      ("WSine", "WTri", "0.0", "0.4", -1, "0.0", "0.0", 0, "300.", "0.10", "0.0", "0.2", "0.004", "0.25", "0.9", "0.10", "0.002", "0.20", "0.0", "0.0", "0.0", "0.0", "0.20", "0.60", 1, "SubDeep"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "600.", "0.60", "4.0", "0.3", "0.002", "0.25", "0.2", "0.10", "0.001", "0.20", "0.06", "0.0", "0.0", "0.0", "0.0", "0.50", 1, "Acid303"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "500.", "0.55", "3.5", "0.3", "0.002", "0.30", "0.15", "0.10", "0.001", "0.25", "0.06", "0.0", "0.0", "0.0", "0.20", "0.50", 1, "AcidSquare"),
      ("WSaw", "WSaw", "0.35", "0.9", 0, "0.3", "0.0", 0, "900.", "0.15", "0.5", "0.2", "0.010", "0.30", "0.9", "0.15", "0.005", "0.30", "0.0", "0.0", "0.0", "0.0", "0.30", "0.45", 1, "Reese"),
      ("WFat", "WSaw", "0.50", "0.8", 0, "0.3", "0.0", 0, "700.", "0.20", "0.3", "0.2", "0.010", "0.40", "0.9", "0.20", "0.005", "0.30", "0.0", "0.0", "0.0", "0.0", "0.35", "0.45", 1, "ReeseWide"),
      ("WSquare", "WSine", "0.0", "0.6", -1, "0.5", "0.0", 0, "1500.", "0.20", "3.0", "0.4", "0.002", "0.18", "0.3", "0.10", "0.001", "0.12", "0.0", "0.0", "0.0", "0.0", "0.0", "0.50", 1, "FmSquare"),
      ("WSquare", "WSquare", "0.02", "0.5", 1, "0.6", "0.0", 0, "2500.", "0.30", "4.0", "0.4", "0.001", "0.15", "0.0", "0.10", "0.001", "0.08", "0.0", "0.0", "0.0", "0.0", "0.0", "0.50", 2, "FmPluck"),
      ("WSaw", "WSquare", "0.05", "0.5", -1, "0.2", "0.0", 0, "800.", "0.30", "2.5", "0.3", "0.003", "0.20", "0.4", "0.12", "0.001", "0.15", "0.0", "0.0", "0.0", "0.0", "0.15", "0.50", 2, "HouseBass"),
    ],
    "acid": [  # mono saw / square, LP SVF, resonance 0.6-0.9, filter env 1-3 oct, fDecay 0.1-0.4 s, glide 0.04-0.08 s
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "500.", "0.75", "2.5", "0.3", "0.002", "0.25", "0.30", "0.10", "0.001", "0.22", "0.06", "0.0", "0.0", "0.0", "0.15", "0.77", 1, "303 Saw"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "450.", "0.70", "2.0", "0.3", "0.002", "0.25", "0.30", "0.10", "0.001", "0.20", "0.06", "0.0", "0.0", "0.0", "0.20", "0.77", 1, "303 Square"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "350.", "0.85", "3.0", "0.3", "0.002", "0.30", "0.25", "0.10", "0.001", "0.30", "0.05", "0.0", "0.0", "0.0", "0.25", "0.77", 1, "303 Rubber"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "600.", "0.65", "1.5", "0.4", "0.002", "0.22", "0.30", "0.12", "0.001", "0.15", "0.08", "0.0", "0.0", "0.0", "0.10", "0.77", 1, "303 Hollow"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "900.", "0.90", "3.0", "0.4", "0.002", "0.20", "0.25", "0.10", "0.001", "0.12", "0.04", "0.0", "0.0", "0.0", "0.40", "0.69", 1, "303 Screech"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "300.", "0.60", "1.2", "0.2", "0.002", "0.35", "0.40", "0.15", "0.001", "0.40", "0.07", "0.0", "0.0", "0.0", "0.05", "0.77", 1, "303 Dub"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "550.", "0.80", "2.2", "0.3", "0.002", "0.25", "0.30", "0.10", "0.001", "0.18", "0.05", "0.0", "0.0", "0.0", "0.55", "0.69", 1, "303 Nasty"),
      ("WSquare", "WSquare", "0.0", "0.0", 0, "0.0", "0.0", 0, "700.", "0.72", "2.8", "0.4", "0.002", "0.18", "0.25", "0.10", "0.001", "0.10", "0.05", "0.0", "0.0", "0.0", "0.30", "0.73", 1, "303 Snap"),
    ],
  },
  "Fx": {
    "lib": [  # perform / glide: lead sounds with glide 120 ms and resonance 0.4-0.7; four glide lanes
      ("WSaw", "WSaw", "0.10", "0.6", 0, "0.0", "0.0", 0, "2200.", "0.45", "2.5", "0.5", "0.005", "0.20", "0.7", "0.20", "0.002", "0.20", "0.12", "0.0", "0.0", "0.0", "0.0", "0.45", 4, "GlideSaw"),
      ("WSquare", "WSquare", "0.08", "0.5", -1, "0.0", "0.0", 0, "1800.", "0.50", "2.0", "0.4", "0.005", "0.25", "0.7", "0.20", "0.002", "0.20", "0.12", "0.0", "0.0", "0.0", "0.0", "0.45", 4, "GlideSquare"),
      ("WSaw", "WSaw", "0.0", "0.0", 0, "0.0", "0.0", 0, "700.", "0.70", "4.0", "0.4", "0.002", "0.30", "0.3", "0.15", "0.001", "0.25", "0.12", "0.0", "0.0", "0.0", "0.25", "0.45", 4, "GlideAcid"),
      ("WFat", "WSaw", "0.30", "0.8", -1, "0.0", "0.0", 0, "2500.", "0.40", "1.5", "0.4", "0.010", "0.30", "0.8", "0.25", "0.005", "0.20", "0.12", "6.0", "0.0", "0.10", "0.30", "0.40", 4, "GlideHoover"),
      ("WSine", "WSine", "0.0", "0.4", 1, "0.0", "0.0", 0, "5000.", "0.40", "1.0", "0.5", "0.005", "0.20", "0.8", "0.25", "0.002", "0.20", "0.12", "5.5", "0.0", "0.15", "0.0", "0.50", 4, "GlideSine"),
      ("WSine", "WSine", "3.86", "0.5", 1, "0.0", "0.0", 0, "6000.", "0.45", "3.0", "0.8", "0.001", "0.80", "0.3", "0.40", "0.001", "0.35", "0.12", "0.0", "0.0", "0.0", "0.0", "0.45", 4, "GlideBell"),
      ("WSaw", "WSquare", "0.20", "0.6", 0, "0.0", "0.05", 1, "1500.", "0.60", "2.0", "0.5", "0.003", "0.25", "0.6", "0.20", "0.002", "0.20", "0.12", "0.0", "0.0", "0.0", "0.40", "0.45", 4, "GlideWasp"),
      ("WTri", "WSine", "0.03", "0.3", 1, "0.0", "0.04", 0, "2500.", "0.40", "1.0", "0.5", "0.030", "0.20", "0.8", "0.25", "0.020", "0.20", "0.12", "5.0", "0.0", "0.12", "0.0", "0.50", 4, "GlideFlute"),
      ("WSaw", "WSaw", "0.15", "0.7", 0, "0.0", "0.0", 0, "3500.", "0.65", "3.0", "0.5", "0.003", "0.30", "0.6", "0.20", "0.002", "0.20", "0.12", "0.0", "0.0", "0.0", "0.50", "0.40", 4, "GlideScream"),
    ],
    "acid": [],
  },
}

# ------------------------------------------------------------------------------------------------- name pools
# Adjectives of at most 5 characters (so "<6-char archetype> <adj>" fits 12 and "303 <adj>" always does).
ADJECTIVES = """Soft Warm Hard Thin Fat Dusty Neon Glass Rusty Misty Deep Lush Sharp Silk Wide Tight Dry Wet Cold Hot
Dim Bold Raw Fuzzy Airy Grit Jade Ruby Opal Onyx Mint Lime Plum Rose Gold Iron Zinc Moss Dusk Dawn Noon Rain Snow
Fog Haze Ash Oak Pine Fern Pale Dark Blue Red Ice Fire Sand Clay Wave Tide Reef Coral Pearl Amber Ivory Ebony Slate
Steel Satin Nylon Wool Linen Denim Candy Honey Sugar Spice Mango Lemon Berry Peach Grape Melon Cocoa Mocha Latte
Cream Milk Smoke Steam Spark Flash Glow Shine Gleam Flare Beam Ray Star Moon Sun Sky Cloud Storm Wind Gust Dune Mesa
Ridge Vale Glen Cove Bay Lake River Creek Brook Pond Marsh Delta Isle Atoll Shore Coast Cliff Cave Grove Wood Bark
Leaf Root Seed Bud Bloom Petal Thorn Vine Ivy Sage Basil Thyme Clove Anise Cumin Chili Salt Pixel Byte Volt Watt
Ohm Hertz Pulse Loop Echo Dub Dusk2 Tone Riff Vibe Funk Jazz Soul Punk Folk Wax Vinyl Tape Reel Cell Core Node
Mesh Grid Arc Orb Cube Prism Lens Mirra Halo Nova Comet Orbit Lunar Solar Astro Cosmo Nebul Quark Ion Atom""".split()
ADJECTIVES = [a for a in ADJECTIVES if a != "Dusk2"]
assert all(len(a) <= 5 for a in ADJECTIVES), [a for a in ADJECTIVES if len(a) > 5]
assert len(set(ADJECTIVES)) == len(ADJECTIVES), "duplicate adjective"

# ------------------------------------------------------------------------------------------------- helpers
class R:
  """Seeded draws with musical helpers."""
  def __init__(self, seed):
    self.r = random.Random(seed)
  def u(self, lo, hi, nd=4):
    return round(self.r.uniform(lo, hi), nd)
  def lu(self, lo, hi, nd=0):
    v = math.exp(self.r.uniform(math.log(lo), math.log(hi)))
    return round(v, nd) if nd else float(round(v))
  def pick(self, *xs):
    return self.r.choice(xs)
  def maybe(self, p):
    return self.r.random() < p


def row(**kw):
  """A generated row: defaults for every SynthPatch field, overridden by kw."""
  d = dict(w1="WSaw", w2="WSaw", det=0.0, l2=0.0, oct=0, sub=0.0, noise=0.0, fm=0, cut=2000.0, res=0.2, fenv=0.0, kt=0.3,
           a=0.005, d=0.15, s=0.7, r=0.2, fa=0.005, fd=0.2, gl=0.0, lr=0.0, lc=0.0, lp=0.0, drv=0.0, lvl=0.4, poly=4,
           name="", sat=0.0, acc=0.0)
  for k, v in kw.items():
    assert k in d, k
    d[k] = v
  return d


def hand_row(tup, sat=0.0, acc=0.0):
  d = dict(zip(FIELDS[:26], tup))
  d["sat"], d["acc"] = sat, acc
  d["hand"] = True
  return d


# ------------------------------------------------------------------------------------------------- archetypes
# Each archetype is (label <= 6 chars, count, fn(R) -> row). Counts sum to GEN_LIBRARY per category.
def chord_archetypes():
  def pluck(r):
    return row(w1=r.pick("WSaw", "WTri", "WSquare", "WFat"), w2=r.pick("WSaw", "WSine", "WTri"), det=r.u(0.03, 0.2), l2=r.u(0.35, 0.7),
               oct=r.pick(0, 0, 1, -1), cut=r.lu(1200, 3500), res=r.u(0.05, 0.3), fenv=r.u(1.5, 3.0), kt=r.u(0.3, 0.6),
               a=r.u(0.002, 0.005), d=r.u(0.2, 0.5), s=0.0, r=r.u(0.15, 0.35), fa=r.u(0.001, 0.003), fd=r.u(0.1, 0.3),
               drv=r.pick(0.0, 0.0, 0.05, 0.1), lvl=0.35, poly=4)
  def stab(r):
    return row(w1=r.pick("WSaw", "WFat", "WSquare"), w2=r.pick("WFat", "WSaw", "WSquare"), det=r.u(0.05, 0.15), l2=r.u(0.4, 0.7),
               oct=r.pick(0, 0, -1), cut=r.lu(1000, 2500), res=r.u(0.2, 0.4), fenv=r.u(2.0, 3.5), kt=r.u(0.4, 0.5),
               a=0.002, d=r.u(0.12, 0.25), s=r.u(0.05, 0.2), r=r.u(0.08, 0.15), fa=0.002, fd=r.u(0.08, 0.15),
               drv=r.u(0.1, 0.3), lvl=0.35, poly=4)
  def organ(r):
    lr = r.pick(0.0, 0.0, r.u(4.5, 6.5, 2))
    return row(w1=r.pick("WSine", "WSquare", "WTri"), w2=r.pick("WSine", "WTri"), det=r.pick(0.0, 0.0, 0.02), l2=r.u(0.3, 0.6),
               oct=r.pick(1, 1, 2), sub=r.pick(0.0, r.u(0.1, 0.3)), cut=r.lu(3000, 7000), res=r.u(0.0, 0.1), fenv=r.u(0.0, 0.5), kt=0.3,
               a=r.u(0.005, 0.015), d=r.u(0.03, 0.08), s=r.u(0.9, 1.0), r=r.u(0.05, 0.1), fa=r.u(0.001, 0.005), fd=r.u(0.02, 0.05),
               lr=lr, lp=r.u(0.05, 0.12) if lr else 0.0, lvl=0.3, poly=4)
  def keys(r):
    return row(w1=r.pick("WSine", "WTri"), w2="WSine", det=r.u(0.02, 0.06), l2=r.u(0.25, 0.45), oct=r.pick(1, 2),
               cut=r.lu(2500, 4500), res=r.u(0.05, 0.15), fenv=r.u(1.5, 2.5), kt=r.u(0.5, 0.7),
               a=r.u(0.002, 0.004), d=r.u(0.8, 1.4), s=r.u(0.15, 0.3), r=r.u(0.3, 0.45), fa=0.001, fd=r.u(0.2, 0.4), lvl=0.45, poly=4)
  def chpad(r):
    lr = r.pick(0.0, r.u(0.2, 0.4, 2), r.u(0.2, 0.4, 2))
    return row(w1=r.pick("WSaw", "WFat"), w2=r.pick("WSaw", "WTri"), det=r.u(0.1, 0.25), l2=r.u(0.6, 0.8), oct=r.pick(0, 0, -1),
               cut=r.lu(900, 1600), res=r.u(0.1, 0.25), fenv=r.u(0.8, 1.5), kt=r.u(0.3, 0.4),
               a=r.u(0.02, 0.1), d=r.u(0.4, 0.7), s=r.u(0.55, 0.75), r=r.u(0.35, 0.55), fa=r.u(0.01, 0.06), fd=r.u(0.25, 0.45),
               lr=lr, lc=r.u(0.3, 0.6) if lr else 0.0, lvl=0.3, poly=4)
  def mallet(r):
    return row(w1="WSine", w2="WSine", det=r.pick(3.86, 7.02, 9.69, 14.0), l2=r.u(0.3, 0.6), oct=r.pick(1, 1, 2),
               cut=r.lu(4000, 7000), res=r.u(0.0, 0.1), fenv=r.u(2.0, 3.5), kt=r.u(0.6, 0.8),
               a=r.u(0.001, 0.002), d=r.u(0.6, 1.0), s=r.pick(0.0, 0.0, 0.1), r=r.u(0.3, 0.5), fa=0.001, fd=r.u(0.25, 0.4), lvl=0.4, poly=4)
  return [("Pluck", 22, pluck), ("Stab", 22, stab), ("Organ", 16, organ), ("Keys", 22, keys), ("ChPad", 22, chpad), ("Mallet", 24, mallet)]


def chord_acid(r):
  return row(w1=r.pick("WSaw", "WSquare"), w2=r.pick("WSaw", "WSquare"), det=r.u(0.03, 0.12), l2=r.u(0.3, 0.6), oct=r.pick(0, 0, 0, -1, 1),
             cut=r.lu(600, 1500), res=r.u(0.45, 0.8), fenv=r.u(1.5, 3.0), kt=r.u(0.3, 0.5),
             a=0.002, d=r.u(0.12, 0.25), s=r.pick(0.0, 0.0, 0.05, 0.1), r=r.u(0.08, 0.14), fa=0.001, fd=r.u(0.1, 0.3),
             drv=r.u(0.1, 0.5), lvl=0.5, poly=r.pick(3, 4, 4))


def lead_archetypes():
  def saw(r):
    lr = r.pick(0.0, r.u(4.5, 6.0, 2))
    return row(w1=r.pick("WSaw", "WFat"), w2=r.pick("WSaw", "WSquare"), det=r.u(0.05, 0.3), l2=r.u(0.4, 0.8), oct=r.pick(0, 0, -1),
               cut=r.lu(2000, 4500), res=r.u(0.15, 0.35), fenv=r.u(1.5, 2.5), kt=0.5,
               a=r.u(0.003, 0.01), d=r.u(0.15, 0.3), s=r.u(0.6, 0.8), r=r.u(0.12, 0.25), fa=0.002, fd=r.u(0.15, 0.25),
               lr=lr, lp=r.u(0.05, 0.15) if lr else 0.0, drv=r.pick(0.0, 0.0, 0.1, 0.2), lvl=0.45, poly=2)
  def square(r):
    return row(w1=r.pick("WSquare", "WTri"), w2=r.pick("WSquare", "WSine", "WSaw"), det=r.u(0.05, 0.2), l2=r.u(0.3, 0.6), oct=r.pick(0, -1, 1),
               cut=r.lu(1800, 3500), res=r.u(0.1, 0.3), fenv=r.u(1.0, 2.0), kt=r.u(0.4, 0.5),
               a=r.u(0.003, 0.01), d=r.u(0.2, 0.3), s=r.u(0.6, 0.8), r=r.u(0.12, 0.2), fa=0.002, fd=0.2, drv=r.pick(0.0, 0.0, 0.1), lvl=0.45, poly=2)
  def sync(r):
    return row(w1=r.pick("WSquare", "WSaw"), w2=r.pick("WSaw", "WSquare"), det=r.u(0.3, 0.7), l2=r.u(0.5, 0.7), oct=0,
               cut=r.lu(2500, 4500), res=r.u(0.3, 0.45), fenv=r.u(2.0, 3.0), kt=0.5,
               a=0.003, d=0.3, s=r.u(0.5, 0.7), r=0.15, fa=0.002, fd=0.2, drv=r.u(0.3, 0.5), lvl=0.4, poly=2)
  def bell(r):
    return row(w1="WSine", w2="WSine", det=r.pick(3.86, 7.02, 9.69, 14.0, 15.86), l2=r.u(0.4, 0.7), oct=r.pick(1, 1, 2),
               cut=r.lu(5000, 7000), res=0.0, fenv=r.u(2.0, 3.5), kt=r.u(0.7, 0.9),
               a=0.001, d=r.u(0.7, 1.2), s=r.pick(0.0, 0.0, 0.1), r=r.u(0.4, 0.6), fa=0.001, fd=r.u(0.3, 0.45), lvl=0.5, poly=2)
  def flute(r):
    return row(w1=r.pick("WTri", "WSine"), w2="WSine", det=r.u(0.01, 0.04), l2=r.u(0.2, 0.4), oct=1, noise=r.u(0.02, 0.05),
               cut=r.lu(1500, 2500), res=r.u(0.05, 0.15), fenv=r.u(0.8, 1.2), kt=0.5,
               a=r.u(0.02, 0.05), d=0.2, s=r.u(0.75, 0.85), r=r.u(0.15, 0.25), fa=r.u(0.01, 0.03), fd=0.2,
               lr=r.u(4.5, 6.0, 2), lp=r.u(0.08, 0.15), lvl=0.5, poly=2)
  def hoover(r):
    return row(w1="WFat", w2="WSaw", det=r.u(0.2, 0.4), l2=r.u(0.7, 0.9), oct=-1, sub=r.u(0.1, 0.3),
               cut=r.lu(2200, 3200), res=r.u(0.25, 0.35), fenv=r.u(1.0, 2.0), kt=0.4,
               a=r.u(0.008, 0.02), d=0.3, s=r.u(0.7, 0.85), r=r.u(0.2, 0.3), fa=0.005, fd=0.2, gl=r.u(0.05, 0.1),
               lr=r.u(5.0, 6.5, 2), lp=r.u(0.1, 0.2), drv=r.u(0.2, 0.4), lvl=0.4, poly=2)
  def pluck(r):
    return row(w1=r.pick("WSaw", "WSquare", "WSine", "WTri"), w2=r.pick("WTri", "WSine", "WSquare"), det=r.u(0.02, 0.06), l2=r.u(0.3, 0.5), oct=r.pick(1, 1, 0),
               cut=r.lu(1200, 5000), res=r.u(0.1, 0.3), fenv=r.u(2.5, 3.5), kt=r.u(0.5, 0.6),
               a=r.u(0.001, 0.003), d=r.u(0.15, 0.3), s=0.0, r=r.u(0.12, 0.2), fa=0.001, fd=r.u(0.08, 0.15), lvl=0.5, poly=2)
  return [("Saw", 22, saw), ("Square", 18, square), ("Sync", 16, sync), ("Bell", 16, bell), ("Flute", 16, flute), ("Hoover", 16, hoover), ("Pluck", 24, pluck)]


def lead_acid(r):
  w = r.pick("WSaw", "WSquare")
  return row(w1=w, w2=w, det=0.0, l2=0.0, oct=0, cut=r.lu(400, 1500), res=r.u(0.6, 0.9), fenv=r.u(1.0, 3.0), kt=r.u(0.3, 0.5),
             a=0.002, d=r.u(0.18, 0.35), s=r.u(0.25, 0.4), r=r.u(0.1, 0.15), fa=0.001, fd=r.u(0.1, 0.4), gl=r.u(0.04, 0.08),
             drv=r.u(0.05, 0.5), lvl=0.6, poly=1)


def pad_archetypes():
  # every library pad: attack >= 0.3 s, LFO to cutoff or pitch, detuned osc 2 (tests/test_synth.cpp preset_category_character)
  def warm(r):
    return row(w1=r.pick("WSaw", "WFat"), w2="WSaw", det=r.u(0.15, 0.3), l2=r.u(0.6, 0.9), oct=0,
               cut=r.lu(900, 1600), res=r.u(0.1, 0.2), fenv=r.u(0.5, 1.2), kt=0.3,
               a=r.u(0.31, 0.6), d=r.u(0.6, 1.0), s=r.u(0.75, 0.9), r=r.u(0.6, 1.0), fa=r.u(0.2, 0.4), fd=r.u(0.6, 1.0),
               lr=r.u(0.15, 0.35, 2), lc=r.u(0.3, 0.8), lvl=0.3, poly=4)
  def glass(r):
    return row(w1=r.pick("WTri", "WSine"), w2=r.pick("WSine", "WTri"), det=r.u(0.1, 0.2), l2=r.u(0.5, 0.7), oct=1,
               cut=r.lu(3000, 5000), res=r.u(0.05, 0.15), fenv=r.u(0.3, 0.8), kt=0.4,
               a=r.u(0.4, 0.8), d=1.0, s=r.u(0.8, 0.9), r=r.u(0.8, 1.5), fa=0.3, fd=0.8,
               lr=r.u(0.3, 0.5, 2), lc=r.u(0.2, 0.4), lp=r.u(0.05, 0.15), lvl=0.35, poly=4)
  def string(r):
    return row(w1="WFat", w2="WSaw", det=r.u(0.2, 0.4), l2=r.u(0.6, 0.8), oct=r.pick(0, 0, -1),
               cut=r.lu(1800, 2800), res=r.u(0.05, 0.15), fenv=r.u(0.5, 1.0), kt=0.3,
               a=r.u(0.31, 0.5), d=0.6, s=0.85, r=r.u(0.6, 0.8), fa=0.2, fd=0.6,
               lr=r.u(4.5, 6.0, 2), lc=r.u(0.1, 0.2), lp=r.u(0.05, 0.1), lvl=0.28, poly=4)
  def dark(r):
    return row(w1="WSaw", w2="WSquare", det=r.u(0.1, 0.25), l2=r.u(0.4, 0.6), oct=-1, sub=r.u(0.2, 0.4),
               cut=r.lu(500, 900), res=r.u(0.2, 0.35), fenv=r.u(0.5, 1.0), kt=0.2,
               a=r.u(0.5, 0.8), d=1.0, s=0.9, r=r.u(1.0, 1.5), fa=0.4, fd=1.0,
               lr=r.u(0.1, 0.2, 2), lc=r.u(0.6, 1.0), lvl=0.3, poly=4)
  def sweep(r):
    return row(w1=r.pick("WFat", "WSaw"), w2=r.pick("WFat", "WSaw"), det=r.u(0.2, 0.3), l2=r.u(0.7, 0.9), oct=0,
               cut=r.lu(500, 800), res=r.u(0.3, 0.45), fenv=r.u(2.0, 3.0), kt=0.3,
               a=r.u(0.31, 0.5), d=1.0, s=0.8, r=r.u(0.8, 1.0), fa=r.u(1.0, 2.0), fd=r.u(1.5, 2.5),
               lr=r.u(0.1, 0.15, 2), lc=r.u(0.8, 1.2), lvl=0.28, poly=4)
  def hollow(r):
    return row(w1=r.pick("WSquare", "WTri"), w2=r.pick("WTri", "WSquare"), det=r.u(0.1, 0.2), l2=r.u(0.5, 0.7), oct=r.pick(1, 0), fm=r.pick(1, 1, 2),
               cut=r.lu(700, 1200), res=r.u(0.25, 0.35), fenv=0.0, kt=0.3,
               a=r.u(0.4, 0.6), d=0.8, s=0.8, r=0.8, fa=0.3, fd=0.8, lr=r.u(0.25, 0.4, 2), lc=r.u(0.5, 0.8), lvl=0.35, poly=4)
  def air(r):
    return row(w1=r.pick("WSine", "WTri"), w2="WTri", det=r.u(0.08, 0.15), l2=r.u(0.4, 0.6), oct=1, noise=r.u(0.04, 0.08),
               cut=r.lu(3500, 5000), res=0.05, fenv=0.0, kt=0.3,
               a=r.u(0.7, 1.0), d=1.0, s=0.9, r=r.u(1.2, 1.8), fa=0.5, fd=1.0,
               lr=r.u(0.15, 0.3, 2), lc=r.u(0.3, 0.5), lp=r.u(0.03, 0.06), lvl=0.35, poly=4)
  def choir(r):
    return row(w1="WSaw", w2="WSaw", det=r.u(0.3, 0.5), l2=r.u(0.8, 0.95), oct=0, fm=1,
               cut=r.lu(1200, 1600), res=r.u(0.3, 0.4), fenv=0.0, kt=0.3,
               a=r.u(0.4, 0.6), d=0.8, s=0.85, r=r.u(0.9, 1.1), fa=0.3, fd=0.8, lr=r.u(0.3, 0.4, 2), lc=r.u(0.4, 0.6), lvl=0.3, poly=4)
  def brass(r):
    return row(w1="WSaw", w2="WSaw", det=r.u(0.08, 0.15), l2=r.u(0.6, 0.8), oct=0,
               cut=r.lu(1300, 1800), res=r.u(0.15, 0.25), fenv=r.u(1.5, 2.5), kt=0.3,
               a=r.u(0.31, 0.4), d=0.5, s=0.8, r=0.5, fa=r.u(0.2, 0.3), fd=0.5, lr=r.u(0.25, 0.35, 2), lc=r.u(0.2, 0.4),
               drv=r.u(0.15, 0.3), lvl=0.3, poly=4)
  return [("Warm", 20, warm), ("Glass", 16, glass), ("String", 16, string), ("Dark", 16, dark), ("Sweep", 16, sweep),
          ("Hollow", 16, hollow), ("Air", 12, air), ("Choir", 8, choir), ("Brass", 8, brass)]


def pad_acid(r):
  # the Pad '303' contract (SPEC §9.17b, test): attack 2-5 ms, amp decay <= 0.25 s, sustain <= 0.2, resonance 0.4-0.8,
  # polyphony >= 3, LFO and detuned osc 2 kept
  return row(w1=r.pick("WSaw", "WSquare"), w2=r.pick("WSaw", "WSquare"), det=r.u(0.08, 0.22), l2=r.u(0.5, 0.7), oct=r.pick(0, 0, 0, -1, 1),
             cut=r.lu(600, 1400), res=r.u(0.42, 0.78), fenv=r.u(1.5, 3.0), kt=r.u(0.3, 0.5),
             a=r.pick(0.002, 0.003, 0.004, 0.005), d=r.u(0.15, 0.24), s=r.pick(0.02, 0.05, 0.1, 0.15, 0.18), r=r.u(0.16, 0.25),
             fa=0.001, fd=r.u(0.1, 0.28), lr=r.u(0.12, 0.35, 2), lc=r.u(0.3, 0.5), drv=r.u(0.1, 0.5), lvl=0.5, poly=r.pick(3, 4, 4))


def bass_archetypes():
  def sub(r):
    l2 = r.pick(0.0, r.u(0.3, 0.5))
    return row(w1=r.pick("WSine", "WTri"), w2=r.pick("WSine", "WTri"), det=0.0, l2=l2, oct=r.pick(-1, 0) if l2 else 0, sub=r.u(0.3, 0.6),
               cut=r.lu(250, 500), res=r.u(0.0, 0.1), fenv=r.u(0.0, 0.5), kt=0.2,
               a=r.u(0.003, 0.008), d=r.u(0.2, 0.35), s=r.u(0.75, 0.9), r=r.u(0.08, 0.15), fa=0.002, fd=0.2,
               drv=r.pick(0.0, 0.0, 0.1, 0.25), lvl=0.6, poly=1)
  def reese(r):
    return row(w1=r.pick("WSaw", "WFat"), w2="WSaw", det=r.u(0.3, 0.6), l2=r.u(0.8, 0.95), oct=0, sub=r.u(0.2, 0.4),
               cut=r.lu(600, 1000), res=r.u(0.1, 0.25), fenv=r.u(0.3, 0.6), kt=0.2,
               a=r.u(0.008, 0.015), d=r.u(0.3, 0.4), s=r.u(0.85, 0.95), r=r.u(0.15, 0.2), fa=0.005, fd=0.3,
               drv=r.u(0.25, 0.4), lvl=0.45, poly=1)
  def acid(r):   # library acid basses (like the hand Acid303 / AcidSquare): mono, glide, resonant; sat 0 (not the '303' family)
    w = r.pick("WSaw", "WSquare")
    return row(w1=w, w2=w, det=0.0, l2=0.0, oct=0, cut=r.lu(400, 800), res=r.u(0.5, 0.7), fenv=r.u(3.0, 4.0), kt=0.3,
               a=0.002, d=r.u(0.2, 0.3), s=r.u(0.15, 0.25), r=0.1, fa=0.001, fd=r.u(0.15, 0.25), gl=r.u(0.05, 0.07),
               drv=r.pick(0.0, 0.1, 0.2, 0.25), lvl=0.5, poly=1)
  def fm(r):
    return row(w1=r.pick("WSquare", "WSine"), w2=r.pick("WSine", "WSquare"), det=r.pick(0.0, 0.02), l2=r.u(0.4, 0.6), oct=r.pick(-1, 1), sub=r.u(0.4, 0.6),
               cut=r.lu(1200, 2600), res=r.u(0.15, 0.3), fenv=r.u(2.5, 4.0), kt=0.4,
               a=r.u(0.001, 0.002), d=r.u(0.12, 0.2), s=r.pick(0.0, 0.1, 0.3), r=r.u(0.08, 0.12), fa=0.001, fd=r.u(0.06, 0.14), lvl=0.5, poly=r.pick(1, 2))
  def house(r):
    return row(w1="WSaw", w2="WSquare", det=r.u(0.03, 0.08), l2=r.u(0.4, 0.6), oct=-1, sub=r.u(0.1, 0.3),
               cut=r.lu(700, 1000), res=r.u(0.2, 0.35), fenv=r.u(2.0, 3.0), kt=0.3,
               a=0.003, d=r.u(0.18, 0.25), s=r.u(0.3, 0.5), r=r.u(0.1, 0.14), fa=0.001, fd=r.u(0.12, 0.18), drv=r.u(0.1, 0.2), lvl=0.5, poly=2)
  def pluck(r):
    return row(w1=r.pick("WSaw", "WTri"), w2=r.pick("WSine", "WSaw"), det=r.u(0.02, 0.05), l2=r.u(0.3, 0.5), oct=r.pick(0, 1), sub=r.u(0.15, 0.3),
               cut=r.lu(600, 1500), res=r.u(0.1, 0.25), fenv=r.u(2.0, 3.0), kt=0.3,
               a=0.002, d=r.u(0.12, 0.2), s=0.0, r=r.u(0.08, 0.12), fa=0.001, fd=r.u(0.08, 0.14), lvl=0.5, poly=2)
  return [("Sub", 24, sub), ("Reese", 24, reese), ("Acid", 24, acid), ("FM", 24, fm), ("House", 20, house), ("Pluck", 12, pluck)]


def bass_acid(r):
  w = r.pick("WSaw", "WSquare")
  return row(w1=w, w2=w, det=0.0, l2=0.0, oct=0, cut=r.lu(300, 900), res=r.u(0.6, 0.9), fenv=r.u(1.0, 3.0), kt=r.u(0.2, 0.4),
             a=0.002, d=r.u(0.18, 0.35), s=r.u(0.25, 0.4), r=r.u(0.1, 0.15), fa=0.001, fd=r.u(0.1, 0.4), gl=r.u(0.04, 0.08),
             drv=r.u(0.05, 0.5), lvl=0.75, poly=1)


def fx_archetypes():
  # every Fx preset: glide 0.12 s, resonance 0.4-0.7, polyphony 4 (four glide lanes; test preset_category_character)
  G = 0.12
  def glide(r):
    return row(w1=r.pick("WSaw", "WSquare", "WFat"), w2=r.pick("WSaw", "WSquare"), det=r.u(0.05, 0.2), l2=r.u(0.4, 0.7), oct=r.pick(0, 0, -1),
               cut=r.lu(1500, 3500), res=r.u(0.41, 0.55), fenv=r.u(1.5, 2.5), kt=r.u(0.4, 0.5),
               a=0.005, d=r.u(0.2, 0.25), s=r.u(0.6, 0.8), r=0.2, fa=0.002, fd=0.2, gl=G, drv=r.pick(0.0, 0.0, 0.15), lvl=0.45, poly=4)
  def acid(r):
    w = r.pick("WSaw", "WSquare")
    return row(w1=w, w2=w, det=0.0, l2=0.0, oct=0, cut=r.lu(500, 900), res=r.u(0.55, 0.69), fenv=r.u(3.0, 4.5), kt=0.4,
               a=0.002, d=0.3, s=r.u(0.3, 0.5), r=0.15, fa=0.001, fd=r.u(0.2, 0.3), gl=G, drv=r.u(0.2, 0.4), lvl=0.45, poly=4)
  def hoover(r):
    return row(w1="WFat", w2="WSaw", det=r.u(0.2, 0.4), l2=r.u(0.7, 0.9), oct=-1,
               cut=r.lu(2000, 3000), res=r.u(0.41, 0.5), fenv=r.u(1.0, 2.0), kt=0.4,
               a=0.01, d=0.3, s=0.8, r=0.25, fa=0.005, fd=0.2, gl=G, lr=r.u(5.0, 6.5, 2), lp=r.u(0.08, 0.15), drv=r.u(0.2, 0.4), lvl=0.4, poly=4)
  def bell(r):
    return row(w1="WSine", w2="WSine", det=r.pick(3.86, 7.02, 9.69, 14.0), l2=r.u(0.4, 0.6), oct=1,
               cut=r.lu(4500, 7000), res=r.u(0.41, 0.5), fenv=r.u(2.5, 3.5), kt=0.8,
               a=0.001, d=r.u(0.6, 1.0), s=r.u(0.2, 0.4), r=r.u(0.3, 0.5), fa=0.001, fd=0.35, gl=G, lvl=0.45, poly=4)
  def wasp(r):
    # nasal: band-pass (or high-pass) near the played fundamentals, noise and drive. The band sits low (350-700 Hz BP
    # with little key tracking and a small filter envelope, 250-600 Hz HP): a band that the envelope or tracking pushes
    # above 1 kHz passes so little of a lead line that the trim would need a level > 1 (the hand GlideWasp, BP 1.5 kHz,
    # is the quietest hand row for that reason).
    fm = r.pick(1, 1, 2)
    return row(w1=r.pick("WSaw", "WSquare"), w2=r.pick("WSquare", "WSaw"), det=r.u(0.1, 0.3), l2=r.u(0.5, 0.7), oct=0, noise=r.u(0.03, 0.08), fm=fm,
               cut=r.lu(350, 700) if fm == 1 else r.lu(250, 480), res=r.u(0.6, 0.69), fenv=r.u(0.3, 0.8) if fm == 1 else r.u(1.0, 2.0),
               kt=r.u(0.3, 0.6) if fm == 1 else 0.5,
               a=0.003, d=0.25, s=0.6, r=0.2, fa=0.002, fd=0.2, gl=G, drv=r.u(0.35, 0.5), lvl=0.45, poly=4)
  def flute(r):
    return row(w1=r.pick("WTri", "WSine"), w2="WSine", det=r.u(0.02, 0.05), l2=r.u(0.2, 0.4), oct=1, noise=r.u(0.02, 0.05),
               cut=r.lu(2000, 3000), res=r.u(0.41, 0.5), fenv=1.0, kt=0.5,
               a=r.u(0.02, 0.04), d=0.2, s=0.8, r=0.25, fa=0.02, fd=0.2, gl=G, lr=r.u(4.5, 6.0, 2), lp=r.u(0.08, 0.15), lvl=0.5, poly=4)
  def scream(r):
    return row(w1="WSaw", w2="WSaw", det=r.u(0.1, 0.25), l2=r.u(0.6, 0.8), oct=0,
               cut=r.lu(2500, 4000), res=r.u(0.6, 0.69), fenv=r.u(2.5, 3.5), kt=0.5,
               a=0.003, d=0.3, s=0.6, r=0.2, fa=0.002, fd=0.2, gl=G, lr=r.u(6.0, 8.0, 2), lp=r.u(0.1, 0.3), drv=r.u(0.4, 0.6), lvl=0.4, poly=4)
  def wobble(r):
    return row(w1=r.pick("WSaw", "WSquare"), w2=r.pick("WSaw", "WSquare"), det=r.u(0.05, 0.15), l2=r.u(0.5, 0.7), oct=r.pick(0, -1), sub=r.pick(0.0, r.u(0.1, 0.3)),
               cut=r.lu(300, 700), res=r.u(0.5, 0.69), fenv=r.u(0.5, 1.5), kt=0.3,
               a=0.005, d=0.3, s=0.8, r=0.2, fa=0.002, fd=0.3, gl=G, lr=r.pick(1.0, 1.5, 2.0, 3.0, 4.0), lc=r.u(1.5, 3.0), drv=r.u(0.1, 0.3), lvl=0.45, poly=4)
  return [("Glide", 24, glide), ("Acid", 16, acid), ("Hoover", 16, hoover), ("Bell", 16, bell), ("Wasp", 16, wasp),
          ("Flute", 12, flute), ("Scream", 12, scream), ("Wobble", 16, wobble)]


ARCHETYPES = {"Chord": chord_archetypes, "Lead": lead_archetypes, "Pad": pad_archetypes, "Bass": bass_archetypes, "Fx": fx_archetypes}
ACID_FN = {"Chord": chord_acid, "Lead": lead_acid, "Pad": pad_acid, "Bass": bass_acid}


# ------------------------------------------------------------------------------------------------- generation
def build_bank(cat, seed):
  """Returns (lib_rows, acid_rows) for one category: hand rows first, then the generated rows."""
  r = R(seed * 1000003 + CATEGORIES.index(cat) * 7919)
  used = set(t[25] for t in HAND[cat]["lib"]) | set(t[25] for t in HAND[cat]["acid"])
  adjectives = list(ADJECTIVES)
  r.r.shuffle(adjectives)
  adj_iter = iter(adjectives)

  def take_name(prefix):
    while True:
      adj = next(adj_iter)
      name = prefix + " " + adj
      if len(name) <= NAME_MAX and name not in used:
        used.add(name)
        return name

  lib = [hand_row(t) for t in HAND[cat]["lib"]]
  acid = [hand_row(t, sat=r.u(0.5, 0.8, 2), acc=r.u(0.5, 0.8, 2)) for t in HAND[cat]["acid"]]
  archetypes = ARCHETYPES[cat]()
  assert sum(n for _, n, _ in archetypes) == GEN_LIBRARY, (cat, sum(n for _, n, _ in archetypes))
  for label, count, fn in archetypes:
    assert len(label) <= 6, label
    for _ in range(count):
      d = fn(r)
      d["name"] = take_name(label)
      d["arch"] = label
      lib.append(d)
  if cat in ACID_CATEGORIES:
    for _ in range(GEN_ACID):
      d = ACID_FN[cat](r)
      d["name"] = take_name("303")
      d["arch"] = "303"
      d["sat"], d["acc"] = r.u(0.5, 0.8, 2), r.u(0.5, 0.8, 2)
      acid.append(d)
  return lib, acid


def validate(cat, lib, acid):
  names = [d["name"] for d in lib + acid]
  assert len(set(names)) == len(names), "duplicate name in %s" % cat
  for d in lib + acid:
    assert 0 < len(d["name"]) <= NAME_MAX, d["name"]
    if d.get("hand"):
      continue
    assert 20 <= d["cut"] <= 9000 and 0 <= d["res"] <= 0.9 and -2 <= d["oct"] <= 2 and 1 <= d["poly"] <= 4, d
    assert 0 <= d["fm"] <= 2 and d["w1"] in ("WSine", "WTri", "WSaw", "WSquare", "WFat") and d["w2"] in ("WSine", "WTri", "WSaw", "WSquare", "WFat")
  for d in acid:
    assert d["name"].startswith("303 "), d["name"]
    assert 0.5 <= d["sat"] <= 0.8 and 0.5 <= d["acc"] <= 0.8
  for d in lib:
    assert not d["name"].startswith("303"), d["name"]
    assert d["sat"] == 0.0 and d["acc"] == 0.0        # library rows: both OFF (SynthPatch contract, core/types.h)


# ------------------------------------------------------------------------------------------------- emission
def ff(v):
  """A float literal: hand tokens verbatim (+f), generated floats with up to 5 decimals, never exponent notation."""
  if isinstance(v, str):
    return v + "f"
  if isinstance(v, int):
    v = float(v)
  s = ("%.5f" % v).rstrip("0")
  if s.endswith("."):
    s += "0"
  return s + "f"


def emit_row(d):
  parts = []
  for f in FIELDS:
    v = d[f]
    if f == "name":
      parts.append('"%s"' % v)
    elif f in INT_FIELDS:
      parts.append(str(int(v)))
    elif f in WAVE_FIELDS:
      parts.append(v)
    else:
      parts.append(ff(v))
  return "  LP_PRESET(" + ", ".join(parts) + "),"


PREAMBLE = '''// GENERATED by tools/gen_presets.py (seed %(seed)d) -- do not edit by hand; edit the tool and re-run
// `python3 -I tools/gen_presets.py --qa` (re-measures and bakes the level trims). Included once, by dsp/presets.cpp.
// Synth preset banks (SPEC §5, §9.17b, research §4): %(total)d presets, one flash-resident constexpr table per category laid
// out as [hand library][generated library][hand '303'][generated '303'] so the hand rows keep their indices inside
// each family and the '303' family stays appended last (presets.h; the pre-§9.22 '303' indices, right after the hand
// library, are remapped by the sets loader for a file without the lp.banks line: SPEC §9.22b, gen/sets.cpp
// presetForLayout -- its table is the old layout, so the hand rows must keep their order). The rows are built by presetgen::make, which
// assigns SynthPatch members by name: the table does not depend on the member order of core/types.h and compiles
// with or without the SPEC §9.22 `sat` / `accent` fields (kHasSatAccent says which; the values are carried either
// way: '303' rows 0.5-0.8 / 0.5-0.8, library rows 0 / 0 -- both OFF, as the SynthPatch contract wants for every
// library preset: a velocity-100 library clip must never be accented wholesale). Every initialiser is a constant
// expression, so the arrays are constant-initialised into .rodata like plain aggregates: 0 B of RAM, no constructors at boot.
// Levels of the generated rows are the baked loudness trims of tools/gen_presets_trims.json (tools/preset_loudness.hpp).
#pragma once
#include "../core/types.h"
namespace lp {
namespace presetgen {

template <class...> using VoidT = void;
template <class T, class = void> struct HasSatAccent { static constexpr bool value = false; };
template <class T> struct HasSatAccent<T, VoidT<decltype(T::sat), decltype(T::accent)>> { static constexpr bool value = true; };
constexpr bool kHasSatAccent = HasSatAccent<SynthPatch>::value;

// Field order of every row (the former positional initialiser order of presets.cpp) plus sat / accent:
//   wave1, wave2, osc2Detune(st), osc2Level, osc2Octave, subLevel, noiseLevel, filterMode(0 LP 1 BP 2 HP),
//   cutoff(Hz), resonance, filterEnv(oct), keyTrack, attack, decay, sustain, release, fAttack, fDecay,
//   glide(s), lfoRate(Hz), lfoToCutoff(oct), lfoToPitch(st), drive, level, polyphony, name, sat, accent
template <class P>
constexpr P make(uint8_t w1, uint8_t w2, float det, float l2, int8_t oct, float sub, float noise, uint8_t fm,
                 float cut, float res, float fenv, float kt, float a, float d, float s, float r, float fa, float fd,
                 float gl, float lr, float lc, float lpitch, float drv, float lvl, uint8_t poly, const char* name,
                 float sat, float accent) {
  P p{};
  p.wave1 = w1; p.wave2 = w2; p.osc2Detune = det; p.osc2Level = l2; p.osc2Octave = oct; p.subLevel = sub; p.noiseLevel = noise;
  p.filterMode = fm; p.cutoff = cut; p.resonance = res; p.filterEnv = fenv; p.keyTrack = kt;
  p.attack = a; p.decay = d; p.sustain = s; p.release = r; p.fAttack = fa; p.fDecay = fd;
  p.glide = gl; p.lfoRate = lr; p.lfoToCutoff = lc; p.lfoToPitch = lpitch; p.drive = drv; p.level = lvl; p.polyphony = poly;
  for (int i = 0; i < (int)sizeof(p.name); i++) p.name[i] = 0;
  for (int i = 0; i + 1 < (int)sizeof(p.name) && name[i]; i++) p.name[i] = name[i];
  if constexpr (HasSatAccent<P>::value) { p.sat = sat; p.accent = accent; } else { (void)sat; (void)accent; }
  return p;
}
#define LP_PRESET(...) ::lp::presetgen::make< ::lp::SynthPatch >(__VA_ARGS__)
'''

POSTAMBLE = '''
#undef LP_PRESET
constexpr int kTotalPresets = %(total)d;
} // namespace presetgen
} // namespace lp
'''


def emit(banks, seed):
  total = sum(len(l) + len(a) for l, a in banks.values())
  out = [PREAMBLE % dict(seed=seed, total=total)]
  for cat in CATEGORIES:
    lib, acid = banks[cat]
    hl = len(HAND[cat]["lib"]); ha = len(HAND[cat]["acid"])
    out.append("\n// ---- %s: %d library rows (%d hand + %d generated), %d '303' rows (%d hand + %d generated) ----" %
               (cat, len(lib), hl, len(lib) - hl, len(acid), ha, len(acid) - ha))
    out.append("constexpr int k%sHandLibrary = %d, k%sLibrary = %d, k%sHandAcid = %d, k%sAcid = %d;" % (cat, hl, cat, len(lib), cat, ha, cat, len(acid)))
    out.append("constexpr SynthPatch k%s[] = {" % cat)
    last_arch = None
    for d in lib + acid:
      arch = "hand" if d.get("hand") else d["arch"]
      if arch != last_arch:
        if arch == "hand":
          out.append("  // hand-written %s rows" % ("'303'" if d["name"].startswith("303") else "library"))
        elif arch == "303":
          out.append("  // generated '303' rows (sat / accent 0.5-0.8)")
        else:
          out.append("  // generated: %s" % arch)
        last_arch = arch
      out.append(emit_row(d))
    out.append("};")
  out.append(POSTAMBLE % dict(total=total))
  return "\n".join(out)


# ------------------------------------------------------------------------------------------------- trims (QA)
def load_trims(path):
  if not os.path.exists(path):
    return {}
  with open(path) as f:
    return json.load(f)


def apply_trims(banks, trims):
  for cat in CATEGORIES:
    lib, acid = banks[cat]
    t = trims.get(cat, {})
    for d in lib + acid:
      if d.get("hand"):
        continue
      if d["name"] in t:
        d["lvl"] = float(t[d["name"]])


def run_qa(out_path):
  """Compiles tools/presets_qa.cpp against the real dsp sources (clang++) and returns its parsed output."""
  tmp = tempfile.mkdtemp(prefix="lp_presets_qa_")
  exe = os.path.join(tmp, "presets_qa")
  dsp = os.path.join(SRC, "dsp")
  cmd = ["clang++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I" + SRC,
         os.path.join(HERE, "presets_qa.cpp")] + [os.path.join(dsp, f) for f in ("synth_voice.cpp", "presets.cpp", "wavetables.cpp", "svf.cpp", "env.cpp")] + ["-o", exe]
  subprocess.run(cmd, check=True)
  res = subprocess.run([exe, "quiet"], check=True, capture_output=True, text=True)
  rows = {}; targets = {}
  for line in res.stdout.splitlines():
    p = line.split()
    if not p:
      continue
    if p[0] == "ROW":
      cat, idx, fam = p[1], int(p[2]), p[3]
      # the name may contain one space ("303 Snap", "Pluck Mint"): numbers are the last 6 fields
      name = " ".join(p[4:-6])
      level, loud, rms, peak, pc4, pext = (float(x) for x in p[-6:])
      rows[(cat, name)] = dict(idx=idx, fam=fam, level=level, loud=loud, rms=rms, peak=peak, peakC4=pc4, peakExtreme=pext)
    elif p[0] == "TARGET":
      targets[(p[1], p[2])] = dict(median=float(p[3]), lo=float(p[4]), hi=float(p[5]))
  return rows, targets


def bake(banks, rows, targets):
  """New levels for the generated rows from one measurement pass (loudness is linear in level)."""
  trims = {}
  capped = []
  for cat in CATEGORIES:
    lib, acid = banks[cat]
    trims[cat] = {}
    for d in lib + acid:
      if d.get("hand"):
        continue
      m = rows[(cat, d["name"])]
      fam = "acid" if d["name"].startswith("303") else "lib"
      target = targets[(cat, fam)]["median"]
      want = m["level"] * 10.0 ** ((target - m["loud"]) / 20.0)
      cap = m["level"] * PEAK_CAP / max(m["peakC4"], m["peakExtreme"], 1e-6)
      level = min(want, LEVEL_MAX, cap)
      level = max(level, LEVEL_MIN)
      if level < want - 1e-6:
        capped.append((cat, d["name"], want, level, "peak" if cap < LEVEL_MAX else "level"))
      trims[cat][d["name"]] = round(level, 4)
  return trims, capped


def report(banks, rows, targets):
  print("QA: loudest 50 ms RMS of the fixed phrase (tools/preset_loudness.hpp), targets = hand-row medians")
  outliers = []
  worst = 0.0
  for cat in CATEGORIES:
    lib, acid = banks[cat]
    for fam, rws in (("lib", lib), ("acid", acid)):
      if not rws:
        continue
      t = targets[(cat, fam)]
      gen = [rows[(cat, d["name"])] for d in rws if not d.get("hand")]
      hand = [rows[(cat, d["name"])] for d in rws if d.get("hand")]
      if not gen:
        continue
      devs = [g["loud"] - t["median"] for g in gen]
      worst = max(worst, max(abs(x) for x in devs))
      print("  %-5s %-4s target %6.2f dB (hand %d rows %6.2f..%6.2f)  generated %3d rows: %+.2f..%+.2f dB, mean |dev| %.2f, levels %.2f..%.2f, max peak c4 %.2f ext %.2f" %
            (cat, fam, t["median"], len(hand), t["lo"], t["hi"], len(gen), min(devs), max(devs), sum(abs(x) for x in devs) / len(devs),
             min(g["level"] for g in gen), max(g["level"] for g in gen), max(g["peakC4"] for g in gen), max(g["peakExtreme"] for g in gen)))
      for d, g in zip([d for d in rws if not d.get("hand")], gen):
        dev = g["loud"] - t["median"]
        if abs(dev) > TOLERANCE_DB:
          outliers.append((cat, d["name"], dev, g["level"]))
  for o in outliers:
    print("  OUTLIER %s '%s' %+.2f dB at level %.3f" % o)
  print("QA: %d generated rows checked, worst |deviation| %.2f dB, %d outliers beyond +-%.1f dB" %
        (sum(1 for c in CATEGORIES for d in banks[c][0] + banks[c][1] if not d.get("hand")), worst, len(outliers), TOLERANCE_DB))
  return outliers


# ------------------------------------------------------------------------------------------------- main
def main():
  ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument("--seed", type=int, default=2026)
  ap.add_argument("--out", default=DEFAULT_OUT)
  ap.add_argument("--trims", default=DEFAULT_TRIMS)
  ap.add_argument("--qa", action="store_true", help="measure, bake the trims, re-emit, verify")
  ap.add_argument("--check", action="store_true", help="measure and verify the emitted header without writing anything")
  args = ap.parse_args()

  banks = {c: build_bank(c, args.seed) for c in CATEGORIES}
  for c in CATEGORIES:
    validate(c, *banks[c])
  trims = load_trims(args.trims)
  apply_trims(banks, trims)
  total = sum(len(l) + len(a) for l, a in banks.values())

  if args.check:
    rows, targets = run_qa(args.out)
    outliers = report(banks, rows, targets)
    return 1 if outliers else 0

  text = emit(banks, args.seed)
  with open(args.out, "w") as f:
    f.write(text)
  print("wrote %s: %d presets (%s)" % (os.path.relpath(args.out, ROOT), total,
        ", ".join("%s %d+%d" % (c, len(banks[c][0]), len(banks[c][1])) for c in CATEGORIES)))
  if not args.qa:
    return 0

  rows, targets = run_qa(args.out)
  new_trims, capped = bake(banks, rows, targets)
  with open(args.trims, "w") as f:
    json.dump(new_trims, f, indent=1, sort_keys=True)
    f.write("\n")
  for cat, name, want, level, why in capped:
    print("  CAPPED %s '%s': wanted level %.3f, baked %.3f (%s cap)" % (cat, name, want, level, why))
  apply_trims(banks, new_trims)
  with open(args.out, "w") as f:
    f.write(emit(banks, args.seed))
  rows, targets = run_qa(args.out)
  outliers = report(banks, rows, targets)
  print("wrote %s and %s" % (os.path.relpath(args.out, ROOT), os.path.relpath(args.trims, ROOT)))
  return 1 if outliers else 0


if __name__ == "__main__":
  sys.exit(main())
