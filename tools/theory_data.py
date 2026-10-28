"""Music-theory tables shared by the Mac clip generator (tools/make_clips.py) and the firmware's on-device THEORY SET
GENERATOR (SPEC §9.24): tools/make_theory_tables.py turns the tables below into src/gen/theory_tables.hpp, so the card
clips and the device generate from ONE source of truth. Stdlib only, no side effects on import.

Conventions (the firmware's, core/config.h and gen/key.h): natural-minor scale degrees 0..6 = i ii III iv v VI VII
(MINOR intervals), a chord slot is a scale degree, rhythm templates are 16-step grids of one 4/4 bar, 96 PPQN.

Sections:
  1. make_clips.py tables (kept byte-for-byte as the clip generator used them, so every GEN clip on the card is still
     reproducible from its name): PROGRESSIONS, ROMAN, CHORD_KINDS, CHORD_KIND_W, CHORD_RHYTHMS, LEAD_RHYTHMS,
     LEAD_MOVES, LEAD_HOME.
  2. Theory generator additions (SPEC §9.24): THEORY_PROGRESSIONS (>= 100 minor-key progressions with borrowed
     chords), VOICINGS, the extended rhythm banks (CHORD_RHYTHMS_EXT, BASS_RHYTHMS, LEAD_RHYTHMS_EXT) and the lead's
     scale-degree Markov table. Everything a device needs is a small integer; names are for the tools and the tests.

References for the progression list (facts, not text): the common minor-key progressions of pop / house / EDM as
catalogued in Hooktheory's public theory pages (i-VI-III-VII, i-VII-VI-VII, i-iv-v, the Andalusian i-VII-VI-V, the
circle progression i-iv-VII-III-VI-ii°-V-i), the dorian i-IV vamp of funk / house and harmonic-minor V / vii°.
"""

PPQN = 96
MINOR = [0, 2, 3, 5, 7, 8, 10]
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

# ----------------------------------------------------------------------------------------------- 1. make_clips tables
# Progressions as natural-minor scale degrees (0 = i, 1 = ii(dim), 2 = III, 3 = iv, 4 = v, 5 = VI, 6 = VII), one
# entry per chord slot. "V" is the borrowed major dominant (raised seventh degree), kept rare. Weights follow the
# library's chord-root usage (i ~40 %, VII / VI / iv / v / III 8-12 % each, ii rare) and its top progressions
# (im; im ivm; im vm; im bIII; im bVI; im bVII; bIIImaj7 ...).
PROGRESSIONS = [
    ([0, 0, 0, 0], 8), ([0, 5, 0, 5], 5), ([0, 3, 0, 3], 5), ([0, 4, 0, 4], 4), ([0, 6, 0, 6], 4), ([0, 2, 0, 2], 3),
    ([0, 5, 6, 0], 4), ([0, 6, 5, 0], 3), ([0, 5, 2, 6], 4), ([0, 2, 6, 5], 3), ([0, 5, 3, 0], 3), ([0, 3, 4, 0], 3),
    ([0, 3, 5, 6], 3), ([0, 6, 3, 5], 2), ([0, 2, 5, 6], 2), ([0, 5, 6, 3], 2), ([0, 6, 2, 5], 2), ([0, 3, 2, 6], 2),
    ([0, 3, 4, 5], 2), ([0, 4, 5, 6], 1), ([0, 2, 3, 6], 2), ([0, 0, 5, 5], 2), ([0, 0, 6, 6], 2), ([0, 0, 3, 3], 2),
    ([0, 0, 2, 2], 1), ([5, 6, 0, 0], 2), ([0, 6, 3, 3], 1), ([0, 1, 0, 1], 1), ([0, 5, 1, 4], 1), ([2, 6, 0, 0], 1),
    ([0, 2, 6, 3], 1), ([3, 5, 0, 6], 1), ([0, 5, 3, 6], 1), ([0, 3, 6, 5], 2), ([5, 6, 0, 2], 1),
    ([0, 5, 0, "V"], 1), ([0, 3, "V", 0], 1), ([0, 0, 0, "V"], 1),              # borrowed dominant, rare
]
ROMAN = {0: "i", 1: "ii", 2: "III", 3: "iv", 4: "v", 5: "VI", 6: "VII", "V": "V"}

# Chord kinds as scale-degree stacks above the chord root (diatonic stacking, never semitone templates: every
# pitch class stays in the key, the firmware's acidStabs rule). Per-category weights below.
CHORD_KINDS = {"triad": [0, 2, 4], "7th": [0, 2, 4, 6], "9th": [0, 2, 4, 6, 8], "add9": [0, 2, 4, 8], "6th": [0, 2, 4, 5],
               "sus2": [0, 1, 4], "sus4": [0, 3, 4], "5": [0, 4], "shell": [0, 2, 6], "9no1": [2, 4, 6, 8],
               "dyad": [0, 2], "oct": [0, 7]}                   # two-voice kinds: the library has 15 % two-voice onsets
CHORD_KIND_W = {"Chord": [("triad", 18), ("7th", 24), ("9th", 16), ("add9", 9), ("6th", 4), ("sus2", 3), ("sus4", 3),
                          ("5", 8), ("shell", 2), ("9no1", 4), ("dyad", 5), ("oct", 4)],
                "Pad": [("triad", 26), ("7th", 24), ("9th", 16), ("add9", 9), ("sus2", 6), ("6th", 3), ("5", 8), ("shell", 3),
                        ("oct", 5)]}

# 16-step onset masks per bar (x = onset) with weights. Chord: onsets/bar p10/p50/p90 = 1.5 / 5 / 7.5 in the library.
CHORD_RHYTHMS = [
    ("x...............", 5, "whole"), ("x.......x.......", 5, "halves"), ("x...x...x...x...", 7, "quarters"),
    ("x.x.x.x.x.x.x.x.", 4, "8ths"), ("x.x...x...x...x.", 5, "offbeat8"), ("x..x..x...x.x...", 6, "tresillo"),
    ("x..x..x.x..x..x.", 5, "tresillo2"), ("x.x.x.x...x.x.x.", 4, "skank"), ("x.....x...x.....", 4, "sparse"),
    ("x..x....x..x....", 5, "dotted"), ("x...x.x.x...x.x.", 4, "pushpull"), ("x.x...x.x.x...x.", 4, "garage"),
    ("x.....x.........", 3, "stab2"), ("x...x...x.x.....", 3, "stab3"), ("x..x..x...x...x.", 4, "clave"),
    ("x.......x.x.....", 3, "answer"), ("x...x...x...x.x.", 3, "quarters-push"), ("x.x.x...x.x.x...", 3, "gallop"),
    ("x......x......x.", 3, "dotted8"), ("x...x..x..x.x...", 4, "syncop"), ("x..x.x..x..x.x..", 5, "shuffle16"),
    ("x.x.x..x.x.x..x.", 4, "funk"), ("x..x...x..x...x.", 5, "dotted16"), ("x...x..x....x..x", 4, "anticip"),
    ("x..x..x..x..x...", 4, "triplet-feel"), ("x.x..x.x.x.x..x.", 4, "broken"), ("x.x...x.x..x..x.", 4, "bounce"),
]
# Lead motif masks: onsets/bar p10/p50/p90 = 2 / 5.75 / 11; every mask starts on the downbeat (crop variants).
LEAD_RHYTHMS = [
    ("x.x.x.x.x.x.x.x.", 8), ("x..x..x...x.x...", 6), ("x.x.x...x.x.x...", 6), ("x...x.x.x...x.x.", 5),
    ("x.xxx.x.x.xxx.x.", 4), ("x..x.x..x..x.x..", 4), ("x.x..x.x.x.x..x.", 5), ("x...x...x.x.x...", 3),
    ("x.....x.x.....x.", 3), ("xx.xx.xx.xx.xx.x", 3), ("x.x.x.x.........", 3), ("x..x..x.x..x..x.", 5),
    ("x.x.x.xxx.x.x.x.", 3), ("x...x...x...x...", 2), ("x.xx.x.xx.x.x.x.", 3), ("x..x.x.xx..x.x.x", 3),
    ("x.......x.......", 2), ("x...........x...", 1), ("x..x....x..x....", 3), ("x.x.....x.x.....", 3),
    ("x...x.x...x.x...", 3), ("xxxxxxxxxxxxxxxx", 1), ("x.x.x.x.x.x.x...", 4), ("x..x..x...x...x.", 3),
    ("x......x......x.", 2), ("x.x...x...x.x...", 4), ("x...x..x..x.....", 2), ("x.x.xx.x.x.xx.x.", 3),
    ("x.x.x.x.x.x.xx.x", 2), ("x.xx.xx.x.xx.xx.", 2), ("x.x...x.x.x...x.", 4), ("x.x.x.x...x.x...", 4),
    ("x.x...x...x.x.x.", 3), ("x...x.x.x.x...x.", 3),
]
# Scale-degree moves for the lead walk (relative degrees; 7 = an octave). From the library's top-voice intervals:
# repeat 26 %, octave 20 %, +2 st 8 %, +3 st 7 %, +7 st 6 %, -2 st 5 %, -3 st 4 %, fourths/fifths 3 % each.
LEAD_MOVES = [(0, 24), (7, 9), (-7, 9), (1, 10), (-1, 8), (2, 7), (-2, 5), (4, 4), (-4, 3), (3, 3), (-3, 3), (5, 1), (-5, 1)]
LEAD_HOME = [(0, 9), (2, 2), (4, 1), (7, 1), (-3, 1)]          # where weak-beat pulls land (root 44 % in the library)


# ----------------------------------------------------------------------------------------------- 2. theory generator
# Chord tokens of the theory progressions. A token is a scale degree (0..6) plus an ALTERATION the firmware applies
# to the diatonic stack: 0 none, 1 raised third (the harmonic-minor dominant V on degree 4, the dorian IV on degree 3),
# 2 sus4 (the third replaced by the fourth), 3 sus2 (by the second), 4 raised root (the leading-tone diminished vii°
# on degree 6: G# B D in A minor). The out-of-key pitch classes a token may use are exactly the altered degrees:
# everything else stays in the natural minor, which is what tests/test_theory.cpp asserts.
ALT_NONE, ALT_MAJOR, ALT_SUS4, ALT_SUS2, ALT_RAISED_ROOT = 0, 1, 2, 3, 4
CHORD_TOKENS = {
    "i": (0, ALT_NONE), "iidim": (1, ALT_NONE), "III": (2, ALT_NONE), "iv": (3, ALT_NONE), "v": (4, ALT_NONE),
    "VI": (5, ALT_NONE), "VII": (6, ALT_NONE),
    "V": (4, ALT_MAJOR), "IV": (3, ALT_MAJOR), "viidim": (6, ALT_RAISED_ROOT),
    "isus4": (0, ALT_SUS4), "ivsus4": (3, ALT_SUS4), "vsus4": (4, ALT_SUS4), "VIIsus4": (6, ALT_SUS4),
    "isus2": (0, ALT_SUS2), "ivsus2": (3, ALT_SUS2), "VIsus2": (5, ALT_SUS2), "IIIsus2": (2, ALT_SUS2), "VIIsus2": (6, ALT_SUS2),
}
# The firmware composes the progression name from the chord codes with these numerals (ASCII only: the 5x7 font).
THEORY_ROMAN = ["i", "iidim", "III", "iv", "v", "VI", "VII"]


def chord_code(token):
    """The firmware's one-byte chord code: degree | alteration << 3."""
    deg, alt = CHORD_TOKENS[token]
    return deg | (alt << 3)


def chord_pcs_theory(root, token, stack):
    """Pitch classes of a theory chord: the diatonic `stack` (scale-degree offsets above the chord degree) with the
    token's alteration applied. Offsets are taken modulo the scale (7 = the root an octave up, 8 = the ninth), so an
    octave offset carries the root's alteration (vii° over [0, 2, 4, 7] is G# B D G#, the octave merging into the raised
    root). Mirrors theoryChordPcs() in gen/variants.cpp (the test's reference)."""
    deg, alt = CHORD_TOKENS[token]
    out = []
    for off in stack:
        o = off % 7
        step = o
        if alt == ALT_SUS4 and o == 2:
            step = 3
        elif alt == ALT_SUS2 and o == 2:
            step = 1
        pc = (root + MINOR[(deg + step) % 7]) % 12
        if alt == ALT_MAJOR and o == 2:
            pc = (pc + 1) % 12
        elif alt == ALT_RAISED_ROOT and o == 0:
            pc = (pc + 1) % 12
        if pc not in out:
            out.append(pc)
    return out


def borrowed_pcs(root, token):
    """The out-of-key pitch classes the token is allowed (empty for a diatonic chord)."""
    deg, alt = CHORD_TOKENS[token]
    if alt == ALT_MAJOR:
        return [(root + MINOR[(deg + 2) % 7] + 1) % 12]
    if alt == ALT_RAISED_ROOT:
        return [(root + MINOR[deg] + 1) % 12]
    return []


# (chords, beats per chord, weight). A pattern is at most 16 beats (kMaxPatternBeats), so a progression is 4 x 1 bar,
# 8 x 1/2 bar, 2 x 2 bars, 4 x 1/2 bar (a 2-bar loop) or 2 x 1 bar (a 2-bar loop). Weights favour the pop / house
# staples; the borrowed-chord and sus entries are rarer. 126 entries, no duplicates (checked by make_theory_tables.py).
THEORY_PROGRESSIONS = [
    # --- four chords, one bar each -------------------------------------------------------------------------------
    ("i VI III VII", 4, 10), ("i VII VI VII", 4, 8), ("i III VII VI", 4, 6), ("i VI VII i", 4, 6), ("i VII VI V", 4, 5),
    ("i iv VII III", 4, 5), ("i iv v i", 4, 5), ("i iv V i", 4, 4), ("i VI iv VII", 4, 6), ("i VI iv V", 4, 4),
    ("i III iv VI", 4, 4), ("i III VI VII", 4, 5), ("i v VI III", 4, 4), ("i v VI VII", 4, 4), ("i v iv v", 4, 2),
    ("i iv VI V", 4, 4), ("i iv VI VII", 4, 5), ("i VII i VII", 4, 4), ("i VI i VII", 4, 4), ("i iv i v", 4, 3),
    ("i iv i V", 4, 2), ("i III VII iv", 4, 4), ("i VII III VI", 4, 4), ("i v VII iv", 4, 3), ("i iidim V i", 4, 2),
    ("i iidim v i", 4, 2), ("i iv iidim V", 4, 2), ("VI VII i i", 4, 5), ("VI VII i III", 4, 4), ("VI VII v i", 4, 2),
    ("VI iv i V", 4, 2), ("VI iv i VII", 4, 3), ("iv i V i", 4, 2), ("i VII VI iv", 4, 4), ("i VI VII iv", 4, 3),
    ("i iv VII i", 4, 4), ("i VII iv i", 4, 4), ("i III VI iv", 4, 3), ("i VI III iv", 4, 3), ("i III iv VII", 4, 4),
    ("i VI v VII", 4, 3), ("i v III VII", 4, 3), ("i iv III VII", 4, 3), ("i VII VI VI", 4, 3), ("i i VI VI", 4, 3),
    ("i i VII VII", 4, 3), ("i i iv iv", 4, 3), ("i i v v", 4, 2), ("i i III III", 4, 2), ("i i VI VII", 4, 4),
    ("i i iv VII", 4, 3), ("i VI VI VII", 4, 3), ("i iv iv VII", 4, 2), ("III VII i i", 4, 2), ("iv VI i i", 4, 2),
    ("VII VI i i", 4, 2), ("i VI VII III", 4, 3), ("i III VI v", 4, 2), ("i v VI iv", 4, 3), ("i iv VI III", 4, 3),
    ("i VI iv III", 4, 2), ("i VII v VI", 4, 2), ("v VI i VII", 4, 1), ("iv VII i VI", 4, 1), ("iv v i i", 4, 2),
    ("i IV i IV", 4, 3), ("i IV VII i", 4, 2), ("i VII IV i", 4, 2), ("i IV VI VII", 4, 1), ("i VI viidim i", 4, 1),
    ("iv V viidim i", 4, 1), ("i isus4 i VII", 4, 2), ("i isus2 VI VII", 4, 2), ("i VII ivsus4 iv", 4, 1), ("VI VII isus4 i", 4, 2),
    ("i VIsus2 VI VII", 4, 1), ("i iv vsus4 v", 4, 1), ("i III VIIsus4 VII", 4, 1), ("i VI IIIsus2 VII", 4, 1), ("i ivsus2 iv VII", 4, 1),
    # --- eight chords, half a bar each ----------------------------------------------------------------------------
    ("i VI III VII i VI VII VII", 2, 4), ("i VII VI VII i VII VI V", 2, 3), ("i iv i v i iv VII i", 2, 3), ("i VI iv VII i VI iv V", 2, 3),
    ("i v VI III i v VII i", 2, 2), ("i iv VII III VI iidim V i", 2, 3), ("i VII VI v iv III iidim i", 2, 2), ("i VII VI V i VII VI V", 2, 2),
    ("i VI VII i i VI VII i", 2, 3), ("i iv i VII i iv i V", 2, 2), ("i III iv VII i III iv VI", 2, 2), ("VI VII i i VI VII i III", 2, 2),
    ("i i VII VII VI VI VII VII", 2, 3), ("i i iv iv VI VI V V", 2, 2), ("i i VI VI iv iv VII VII", 2, 3), ("i isus4 i VII VI VIsus2 VI VII", 2, 1),
    ("i VII i VII VI VII i i", 2, 2), ("i III VI VII i III iv v", 2, 2), ("i IV i IV i IV VII VII", 2, 1), ("i VI i VII i VI iv V", 2, 2),
    ("i v i v VI VII i i", 2, 2), ("i iv VI VII i iv v i", 2, 2), ("i VII VI VII III VII VI VII", 2, 1), ("i i VI VII i i iv VII", 2, 2),
    ("i VI VII viidim i VI VII V", 2, 1),
    # --- two chords, two bars each --------------------------------------------------------------------------------
    ("i VI", 8, 5), ("i VII", 8, 5), ("i iv", 8, 4), ("i v", 8, 3), ("i III", 8, 3), ("i IV", 8, 2), ("i V", 8, 2),
    ("VI VII", 8, 2), ("iv i", 8, 2), ("i isus4", 8, 2), ("i VIsus2", 8, 1),
    # --- two-bar loops: four chords of half a bar, two chords of one bar -----------------------------------------
    ("i VI III VII", 2, 4), ("i VII VI VII", 2, 3), ("i iv v i", 2, 2), ("i VI VII i", 2, 2), ("i iv VII III", 2, 2),
    ("i VI", 4, 3), ("i VII", 4, 3), ("i iv", 4, 2), ("i v", 4, 1), ("i III", 4, 1),
]

# Pad / chord voicing templates: the diatonic stack above the chord degree (0 root, 2 third, 4 fifth, 6 seventh,
# 8 ninth, 5 sixth, 7 octave) and the voicing shape (close = stacked from the inversion's bass note, open = drop 2:
# the second-highest voice an octave down). The firmware chooses the inversion of every chord for the least total
# voice movement from the previous chord inside the register band (59..77).
VOICINGS = [
    ("triad", [0, 2, 4], "close", 20), ("triad-open", [0, 2, 4], "open", 10), ("7th", [0, 2, 4, 6], "close", 18),
    ("7th-open", [0, 2, 4, 6], "open", 10), ("add9", [0, 2, 4, 8], "close", 10), ("add9-open", [0, 2, 4, 8], "open", 5),
    ("6th", [0, 2, 4, 5], "close", 5), ("shell", [0, 2, 6], "close", 5), ("oct-triad", [0, 2, 4, 7], "close", 5),
    ("fifth-oct", [0, 4, 7], "close", 4), ("7th-no5", [0, 2, 6, 7], "close", 3), ("9th-no1", [2, 4, 6, 8], "close", 3),
]

# Rhythm template alphabet for the theory banks (16 steps of a bar): '.' rest, 'x' onset, 'X' accented onset,
# '-' hold (the previous note is extended through this step). The firmware packs a step into 2 bits.
RHYTHM_CODES = {".": 0, "x": 1, "X": 2, "-": 3}
# Chord stab bank = the clip generator's CHORD_RHYTHMS (accents on the beats) + these (accents, holds, house / garage
# / breaks figures): 47 templates.
CHORD_RHYTHMS_EXT = [
    ("X.x.X.x.X.x.X.x.", 5, "8ths-accent"), ("x...x.x.x.x.x.x.", 6, "offbeat-stabs"), ("x.x.x.x...x.x...", 4, "house-skip"),
    ("X--.x---X--.x---", 4, "gated-long"), ("x-x-x-x-x-x-x-x-", 3, "gated-8ths"), ("X.x.x.X.x.x.X.x.", 4, "3-3-2"),
    ("X..x..X..x..X.x.", 5, "3-3-2-push"), ("x---....x---....", 4, "half-stabs"), ("X.......x.x.X...", 3, "call-answer"),
    ("x.x...x...x.x.x.", 3, "offbeat-fill"), ("x..X..x..X..x..X", 3, "dotted-accents"), ("X...x..x..x.X..x", 3, "latin"),
    ("xxxx.xxx.xxx.xx.", 2, "gated-16"), ("X.x.X.x.X.x.x.xx", 2, "8ths-roll"), ("X---X---X---X---", 4, "sustained-quarters"),
    ("X-------x-------", 4, "two-long"), ("x...X.......X...", 3, "backbeat"), ("X...x.x.X...x.x.", 3, "pushpull-accent"),
    ("X..x..x.X..x..x.", 3, "tresillo-accent"), ("x.X.x.X.x.X.x.X.", 3, "offbeat-accent"),
]
# Bass bank: 'r' root, 'R' accented root, 'o' octave, 'O' accented octave, 'f' fifth, 'F' fifth below, 'a' approach
# note into the next chord's root (a scale step above or below), 't' chord third, '-' hold, '.' rest. 34 templates.
BASS_CODES = {".": 0, "r": 1, "R": 2, "o": 3, "O": 4, "f": 5, "F": 6, "a": 7, "-": 8, "t": 9}
BASS_RHYTHMS = [
    ("R...r...r...r...", 8, "four-floor"), ("R...r...R...r..a", 5, "four-floor-approach"), ("r.o.r.o.r.o.r.o.", 7, "octaves"),
    ("R.o.r.o.R.o.r.oa", 4, "octaves-approach"), ("r.o...o...o...o.", 6, "offbeat-octaves"), ("r.r.r.r.r.r.r.r.", 6, "rolling-8ths"),
    ("R.r.r.r.R.r.r.r.", 4, "rolling-accent"), ("R..r..r...r.r...", 5, "tresillo"), ("R..r..r.R..r..r.", 4, "tresillo-2"),
    ("R..o..r...o.r...", 4, "tresillo-oct"), ("R-------R-------", 5, "half-notes"), ("R---------------", 4, "whole"),
    ("R---r---R---r---", 4, "quarters-long"), ("R.......r.......", 3, "two-hits"), ("R...f...r...f...", 4, "root-fifth"),
    ("R..f..r...f.r...", 3, "root-fifth-sync"), ("r.f.o.f.r.f.o.f.", 3, "arpeggio"), ("R...F...R...F...", 3, "root-fifth-below"),
    ("R.r.o.r.R.r.o.a.", 3, "walk-approach"), ("R..r.r..R..r.r.a", 3, "skip-approach"), ("R.......R..r..a.", 3, "sparse-pickup"),
    ("r.rr.r.rr.r.rr.r", 2, "16th-roll"), ("R-r-R-r-R-r-R-r-", 3, "gated-8ths"), ("R.o.o.o.R.o.o.o.", 2, "octave-bounce"),
    ("R.r...r...r...r.", 4, "offbeat-roots"), ("R..R..R...R.R...", 3, "accent-tresillo"), ("R...r.r.R...r.ra", 3, "house-kick-bass"),
    ("R.t.f.t.R.t.f.t.", 2, "triad-walk"), ("R...t...f...a...", 2, "walking-quarters"), ("R.......f.......", 3, "root-fifth-halves"),
    ("R..r..a.R..r..a.", 2, "approach-each-half"), ("RrrrRrrrRrrrRrrr", 1, "16th-drive"), ("R.-.r.-.R.-.r.-.", 2, "dotted-8ths"),
    ("R...R...R...R...", 2, "four-accent"),
]
# Lead motif bank = LEAD_RHYTHMS (34) + these with accents / holds: 44 templates.
LEAD_RHYTHMS_EXT = [
    ("X.x.x.x.X.x.x.x.", 4, "8ths-accent"), ("X---x.x.X---x.x.", 3, "long-short"), ("X.x...X.x...X.x.", 3, "3-3-2"),
    ("X..x..X..x..X.x.", 3, "3-3-2-push"), ("X---....X---....", 3, "two-longs"), ("X.x.X---x.x.X---", 2, "phrase"),
    ("X.......X.x.x.x.", 2, "answer-run"), ("X.x.x---X.x.x---", 2, "pairs"), ("X.xx.x.xX.xx.x.x", 2, "syncopated-16"),
    ("X.......x-------", 2, "two-notes"),
]
# Scale-degree Markov table of the lead walk: row = the current degree 0..6, column = the next degree's weight (steps
# and chord-tone leaps favoured; b6 pulls to the fifth, b7 to the root). Octave displacement is drawn separately.
LEAD_MARKOV = [
    [18, 10, 14, 6, 14, 6, 12],
    [22, 6, 20, 6, 8, 4, 10],
    [16, 14, 12, 16, 12, 4, 6],
    [10, 6, 18, 8, 20, 8, 4],
    [18, 4, 10, 14, 10, 14, 10],
    [8, 4, 6, 10, 24, 8, 18],
    [24, 6, 6, 4, 12, 14, 12],
]
LEAD_START = [10, 1, 4, 1, 4, 1, 2]                   # the motif's first degree (root, then the third and fifth)
# A' / B variation operations of the lead motif and their weights (SPEC §9.24: A A' B A').
LEAD_VARIATIONS = [("sequence", 4), ("ending", 4), ("thin", 3), ("octave", 3), ("pickup", 2), ("invert", 1)]


def theory_chord_rhythms():
    """The firmware's chord stab bank: the clip generator's masks (accents on the beats) + the extension."""
    out = []
    for mask, w, name in CHORD_RHYTHMS:
        out.append(("".join("X" if (c == "x" and k % 4 == 0) else c for k, c in enumerate(mask)), w, name))
    return out + list(CHORD_RHYTHMS_EXT)


def theory_lead_rhythms():
    out = [("".join("X" if (c == "x" and k == 0) else c for k, c in enumerate(mask)), w, "lead%d" % i)
           for i, (mask, w) in enumerate(LEAD_RHYTHMS)]
    return out + list(LEAD_RHYTHMS_EXT)


def theory_progressions():
    """(tokens, beats per chord, weight) with the tokens split and validated."""
    out = []
    for text, beats, w in THEORY_PROGRESSIONS:
        toks = text.split()
        for t in toks:
            if t not in CHORD_TOKENS:
                raise ValueError("unknown chord token %r in %r" % (t, text))
        if len(toks) * beats > 16:
            raise ValueError("progression %r does not fit 16 beats" % text)
        out.append((toks, beats, w))
    return out


# ----------------------------------------------------------------------------------------------- 3. drum templates
# Beat and Percussion step templates for the clip generator (tools/make_clips.py gen_beat / gen_percussion): 16-step
# masks of one 4/4 bar ('x' onset, '.' rest), weights, written against the firmware's Beat note contract (SPEC §9.6,
# engine.cpp playBeatPitch): 36 kick, 37 snare, 38 closed hat, 39 open hat, 40..58 accents (played on the open-hat
# slot at x0.8; the Full layer keeps ONE accent pitch). The layers Kick / Kick+Hats / +Snare / Full are the firmware's
# (gen/variants.cpp beatLayer), so a template only has to carry every voice. Percussion voices A..D are pitches
# 36..39 (engine.cpp: slot 4 + (pitch - 36)); the four layers thin the hits per voice by ordinal, so a voice with
# many hits layers best. Not emitted into the firmware (make_theory_tables.py names its tables explicitly).
BEAT_PITCH = {"kick": 36, "snare": 37, "hh": 38, "oh": 39, "ghost": 36}
BEAT_ACCENTS = [(40, 4), (41, 3), (42, 3), (43, 2), (44, 1)]   # accent pitch (one per pattern, two rarely) and weight
BEAT_STYLES = [("house", 35), ("techno", 25), ("garage", 20), ("breaks", 20)]
# style -> voice -> [(mask, weight), ...]; "ghost" = quiet extra kicks, "acc" = the accent voice
BEAT_TEMPLATES = {
    "house": {
        "kick": [("x...x...x...x...", 10), ("x...x...x...x..x", 2), ("x...x...x.x.x...", 2), ("x...x..xx...x...", 1)],
        "snare": [("....x.......x...", 10), ("....x.......x..x", 1), ("....x.....x.x...", 1)],
        "hh": [("..x...x...x...x.", 7), ("x.x.x.x.x.x.x.x.", 5), ("xxxxxxxxxxxxxxxx", 3), ("..x.x.x...x.x.x.", 2),
               ("x.xx.x.xx.x.xx.x", 1), (".x.x.x.x.x.x.x.x", 1)],
        "oh": [("..x...x...x...x.", 7), ("......x.......x.", 3), ("..............x.", 2), ("..x.......x.....", 1), ("", 2)],
        "ghost": [("", 6), ("..............x.", 1), (".......x........", 1)],
        "acc": [("", 4), (".......x........", 2), ("x.......x.......", 1), ("...x......x.....", 1), ("x...x...x...x...", 1)],
    },
    "techno": {
        "kick": [("x...x...x...x...", 12), ("x...x...x...x.x.", 1), ("x...x...x...x..x", 1)],
        "snare": [("....x.......x...", 5), ("............x...", 3), ("", 3), ("....x.......x.x.", 1)],
        "hh": [("..x...x...x...x.", 6), ("xxxxxxxxxxxxxxxx", 5), ("x.x.x.x.x.x.x.x.", 3), (".x.x.x.x.x.x.x.x", 2), ("..xx..xx..xx..xx", 1)],
        "oh": [("..x...x...x...x.", 8), ("......x.......x.", 2), ("", 2), ("...x...x...x...x", 1)],
        "ghost": [("", 3), ("..x...x...x...x.", 2), (".x.......x......", 1), ("...x.......x....", 2)],
        "acc": [("", 2), ("x..x..x.x..x..x.", 4), ("x.......x.......", 1), (".x...x...x...x..", 1), ("..x.......x.....", 1)],
    },
    "garage": {
        "kick": [("x......x..x.....", 7), ("x.....x...x.....", 3), ("x......x..x....x", 2), ("x..x......x.....", 2), ("x.........x..x..", 1)],
        "snare": [("....x.......x...", 9), ("....x.......x..x", 2), ("....x......x....", 1)],
        "hh": [("x.xx.x.xx.x.xx.x", 4), ("x.x.x.x.x.x.x.x.", 4), ("..x...x...x...x.", 3), (".x.x.x.x.x.x.x.x", 2), ("x.x.xx.x.x.x.xx.", 1)],
        "oh": [("......x.......x.", 4), ("..x.......x.....", 2), ("...x.......x....", 2), ("", 2)],
        "ghost": [("", 5), (".........x......", 1), ("..........x..x..", 1)],
        "acc": [("", 2), ("...x.....x......", 3), ("..x...x...x...x.", 1), (".......x.......x", 1), ("...x......x..x..", 1)],
    },
    "breaks": {
        "kick": [("x.........x.x...", 5), ("x..x......x.....", 3), ("x.x.......x.x...", 2), ("x.....x...x.....", 2), ("x..x.....x..x...", 1), ("x.........x.....", 1)],
        "snare": [("....x.......x...", 7), ("....x..x....x...", 2), ("....x.....x.x...", 1), ("....x.......x.x.", 1), ("....x......x....", 1)],
        "hh": [("x.x.x.x.x.x.x.x.", 7), ("xxxxxxxxxxxxxxxx", 2), ("x.x.x.x.x.x.xx.x", 1), ("..x...x...x...x.", 2)],
        "oh": [("......x.......x.", 3), ("..............x.", 3), ("..x.......x.....", 1), ("", 2)],
        "ghost": [("", 3), (".......x.x......", 2), ("..x.......x.....", 1), (".....x.......x..", 1)],
        "acc": [("", 2), ("x...............", 3), ("........x.......", 1), (".......x......x.", 1)],
    },
}
# Last-bar fills of a Beat (chosen per pattern; applied to the last bar of a 4-bar loop, or the 2nd bar of a 2-bar loop
# with a lower chance): name -> what gen_beat does.
BEAT_FILLS = [("none", 5), ("snare16", 3), ("drop-kick", 2), ("oh-end", 2), ("double-kick", 2), ("snare-roll", 1)]

# Percussion kits: four voice pools (A low drum, B mid drum / slap, C high / shaker, D bell / rim), masks with
# weights. gen_percussion draws one mask per voice (an empty mask silences a voice: 3-voice patterns exist too).
PERC_STYLES = [("latin", 30), ("afro", 20), ("tribal", 15), ("shaker", 20), ("minimal", 15)]
PERC_TEMPLATES = {
    "latin": {
        "A": [("x..x..x.x..x..x.", 5), ("x.....x...x.....", 3), ("..x.....x.x.....", 2), ("x..x....x..x....", 2)],
        "B": [("..x...x...x...x.", 4), ("....x.......x...", 3), ("..x.x...x.x.x...", 2), (".x...x.x.x...x.x", 2), ("", 1)],
        "C": [("x.x.x.x.x.x.x.x.", 4), ("xxxxxxxxxxxxxxxx", 2), ("x.xx.x.xx.x.xx.x", 2), ("..x...x...x...x.", 2), ("", 1)],
        "D": [("x..x..x...x.x...", 4), ("..x..x..x.x..x..", 2), (".......x.......x", 2), ("x.......x.......", 1), ("", 2)],
    },
    "afro": {
        "A": [("x..x..x..x..x...", 4), ("x.....x.....x...", 3), ("x..x.....x..x...", 2), ("..x.....x.....x.", 1)],
        "B": [("..x..x..x..x..x.", 4), (".x..x..x..x..x..", 2), ("....x.......x...", 2), ("", 1)],
        "C": [("x.xx.x.xx.x.xx.x", 3), ("x.x.x.x.x.x.x.x.", 3), ("x..x..x..x..x..x", 2), ("", 1)],
        "D": [("x..x..x..x..x..x", 3), ("x.x..x.x..x.x..x", 2), ("..x.....x.....x.", 2), ("", 2)],
    },
    "tribal": {
        "A": [("x...x...x...x...", 4), ("x...x.x.x...x.x.", 2), ("x.....x.x.....x.", 2), ("x..x..x...x..x..", 2)],
        "B": [("..x...x...x...x.", 3), ("...x...x...x...x", 2), ("..x.x...x.x.....", 2), ("", 1)],
        "C": [("xxxxxxxxxxxxxxxx", 3), ("x.x.x.x.x.x.x.x.", 3), (".x.x.x.x.x.x.x.x", 2), ("", 1)],
        "D": [("x.......x.......", 2), ("......x.......x.", 2), ("x..x..x.x..x..x.", 2), ("", 2)],
    },
    "shaker": {
        "A": [("x.......x.......", 2), ("x.....x...x.....", 2), ("", 3), ("....x.......x...", 1)],
        "B": [("..x...x...x...x.", 3), ("....x.......x...", 2), ("", 2)],
        "C": [("xxxxxxxxxxxxxxxx", 6), ("x.xx.x.xx.x.xx.x", 3), ("x.x.x.x.x.x.x.x.", 2), ("xx.xx.xx.xx.xx.x", 1)],
        "D": [("", 3), ("..x...x...x...x.", 2), (".......x.......x", 2), ("x..x..x.x..x..x.", 1)],
    },
    "minimal": {
        "A": [("x.......x.......", 3), ("x.....x.........", 2), ("", 2), ("x...........x...", 1)],
        "B": [("....x.......x...", 3), ("..x.......x.....", 2), ("......x.......x.", 2), ("", 1)],
        "C": [("..x...x...x...x.", 4), ("x.x.x.x.x.x.x.x.", 2), (".x.x.x.x.x.x.x.x", 1), ("", 1)],
        "D": [(".......x........", 2), ("...x.......x....", 2), ("x..x..x.x..x..x.", 1), ("", 2)],
    },
}
