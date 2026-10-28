# Clip generator: procedural Chord / Lead / Pad clips (M2)

`tools/make_clips.py` writes seeded, deterministic 2- and 4-bar MIDI clips in every natural-minor key, the step
"M2 / option D" of docs/RESEARCH_LIBRARY_EXPANSION.md §2.2 (fill Lead and Pad to ≥ 300 per key without a
firmware change). Per key it produces **150 Lead, 120 Pad and 60 Chord** clips; the whole run (12 keys,
3,960 files, 1.03 MB) takes under 2 s and regenerates byte-for-byte. Output goes to a staging mirror of the card
layout; sharding (`shard_folders.py`), indexing (`write_indexes.py`) and the merge into `sdcard/lp` are the
integration step, not this tool's.

```
<out>/lp/midi/<Category>/<Key>min/<Key>min__<Category>__GEN-<style>-<seed>__<hash8>.mid
```

| part | meaning |
|---|---|
| `Category` | `Chord`, `Lead` or `Pad` |
| `Key` | the 12 minor labels of SPEC §6 (`Amin`, `A#min`, … sharps only); the file sits in the folder its label names |
| `GEN` | marks generated material (research Q9) so it can be ratio-limited or pruned without touching library files |
| `style` | the recipe: Chord `<rhythm>-<kind>-<voicing>-p<progression>`, Pad `<style>-<kind>-x<bars per chord>`, Lead `<style>-r<rhythm>-o<octave>-<gate>`; `[A-Za-z0-9-]` only |
| `seed` | the draw number (4 digits); `sha1("<Category>|<Key>|<seed>")` seeds the clip's RNG, so one clip is reproducible from its name alone |
| `hash8` | first 8 hex digits of the content hash: `sha1(u16be loop_ticks + Σ (u16be start, u16be length, u8 pitch, u8 velocity))` over the notes in canonical order (start, pitch, length, velocity). Exact duplicates are detected on the full digest |

Longest name in the build: 64 bytes (limit 95). Every file is a type-0 SMF at 96 PPQN with tempo 125 and 4/4 at
tick 0 (the firmware writer's layout), note-offs before note-ons on a tick, end-of-track at the loop length.

## Regenerate

```
python3 -I tools/make_clips.py --out sdcard/lp_stage/m2                 # all 12 keys, the default counts, then QA
python3 -I tools/make_clips.py --out DIR --keys Amin,Emin --counts Lead=150,Pad=120,Chord=60 --json report.json
python3 -I tools/make_clips.py --out DIR --check-only                  # QA only, on an existing tree
```

| flag | default | effect |
|---|---|---|
| `--out DIR` | required | staging root; files go to `DIR/lp/midi/<Category>/<Key>/` (the category/key folders are rebuilt unless `--keep`) |
| `--keys` | `all` | comma-separated minor labels |
| `--counts` | `Lead=150,Pad=120,Chord=60` | per-category target per key; `Chord=0` skips a category |
| `--seed0` | 1 | first draw number; draws continue past rejections and duplicates until the count is met |
| `--max-draw-factor` | 8 | give up after count × factor draws in one folder (never reached: ≤ 1.3 draws per clip) |
| `--no-humanise` | off | keep every onset on the 16th grid |
| `--keep` | off | add to existing folders (their digests are read first, so nothing is duplicated) |
| `--check-only` | off | run only the QA pass over `--out` |
| `--json FILE` | – | write the generation and QA report |
| `--quiet` | off | summary lines only |

Stdlib only, run with `-I`. The QA pass imports `tests/refmodel_smf.py` (the Python model of `gen/smf.cpp`) by
path; without it the firmware-rule parse is skipped with a warning.

Integration (the merge step, not run by this tool): copy `lp/midi/<Category>/<Key>/*.mid` beside the library
files, then `python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard`.
Every key folder gains at most 150 files, far below the 768 (12 shards × 64) cap.

## What every clip obeys (QA pass, `--check-only`)

Checked on every file after writing, against the firmware's rules (core/config.h, gen/smf.cpp, SPEC §6):

- the name parses (`<Key>min__<Category>__GEN-<style>-<seed>__<hash8>.mid`), its key label equals its folder,
  it is ≤ 95 bytes, and `hash8` equals the content hash of the notes read back;
- `tests/refmodel_smf.py` parses it without truncation, keeps every note, and reports exactly 2 or 4 bars
  (768 or 1536 ticks): the generator rejects a clip whose last note ends more than 2 beats before the loop end,
  because `smfParse` would shorten the loop (`kTrailingBeats`);
- 1..192 notes, lengths in 12-tick units, an onset at tick 0 (so crop 2 beats / crop ½ beat are never empty),
  no two notes of one pitch overlapping (an SMF pairs note-offs FIFO per pitch, and the engine steals the voice
  anyway), no two notes with the same start and pitch (the lead Variation would read that as polyphony);
- pitch bands (library lowest p10 .. highest p90, and the clip's median pitch inside the library's median
  p10..p90): Chord notes 36..79 / median 53..69, Lead 43..82 / 48..79, Pad 39..85 / 59..77;
- at most 25 % of the notes outside the natural minor of the label (the library's p90), and exact duplicates
  across the folder are errors.

It then prints style statistics next to the library's measured values (table below) and a sameness measure
(cell Jaccard over (16th step, pitch class) sets on random pairs, near-duplicate pair rate at Jaccard > 0.6,
distinct rhythm skeletons).

Firmware harness (`tools/clip_varcheck.cpp`): parses with the real `smfParseBuffer` and builds the four variants
with the real `buildVariant`, per key folder set:

```
clang++ -std=c++17 -O2 -Ifirmware/liveplayer/src tools/clip_varcheck.cpp firmware/liveplayer/src/gen/key.cpp \
    firmware/liveplayer/src/gen/smf.cpp firmware/liveplayer/src/gen/variants.cpp -o /tmp/lp_clip_varcheck
/tmp/lp_clip_varcheck <dir holding Chord/ Lead/ Pad/ of ONE key> <root 0..11> [-v]
```

## Build of 2026-10-16 (12 keys, defaults)

| | Chord | Lead | Pad |
|---|---|---|---|
| files (per key) | 720 (60) | 1,800 (150) | 1,440 (120) |
| bytes, mean per file | 373,603, 519 B | 384,132, 213 B | 269,945, 187 B |
| draws → written | 831 → 720 | 1,893 → 1,800 | 1,704 → 1,440 |
| exact duplicates (rate per draw) | 0 (0.00 %) | 0 (0.00 %) | 0 (0.00 %) |
| rejected: median out of band / silent tail / no downbeat / empty | 54 / 35 / 14 / 8 | 66 / 27 / 0 / 0 | 138 / 30 / 56 / 40 |
| QA errors | 0 | 0 | 0 |
| firmware harness: parse failures / truncated / empty variants / wrong crop lengths | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| Variation slot unchanged (v1 == v0) — library 27 / 8 / 6 % | 12.2 % | 9.0 % | 3.5 % |
| clips with a borrowed (out-of-key) note | 8 (1.1 %) | 9 (0.5 %) | 12 (0.8 %) |
| cell Jaccard p50 / p90 / p99 (gate: p90 < 0.35) | 0.047 / 0.130 / 0.263 | 0.000 / 0.061 / 0.143 | 0.034 / 0.143 / 0.308 |
| near-duplicate pair rate (gate < 1 %) | 0.02 % | 0.00 % | 0.00 % |
| distinct pitch+rhythm skeletons | 719 of 720 | 1,800 of 1,800 | 1,399 of 1,440 |
| distinct rhythm skeletons (gate ≥ 60 %) | 51 % | 69 % | 21 % (a sustained pad has one onset per chord; by construction) |

Rejections are deterministic and free (the next seed is drawn); "median out of band" means the register draw
landed outside the band, "silent tail" that the firmware would shorten the loop, "no downbeat" a pad whose first
chord was delayed, "empty" a register too narrow for the chord kind. Determinism: regenerating Amin and Dmin into
another folder gives byte-identical files.

Saturation (one key, 3,000 draws per category, `--max-draw-factor 1`): exact duplicates Chord 0, Lead 0,
Pad 30 (1.0 %; the research prototype saturated at 29 %), near-duplicate pairs 0.12 / 0.00 / 0.53 %, 0 QA errors;
written 2,435 / 2,769 / 2,551 (1,995 distinct pad pitch+rhythm skeletons), so the headroom per key is ≈ 2,400 Chord,
2,700 Lead and 2,000 distinct Pad clips before sameness becomes the limit.

### Parity with the library (generated vs measured, same statistics as the research)

Library = sdcard/lp/midi Chord 293 / Lead 161 / Pad 94 clips, through the firmware's length rule
(scratchpad research `procgen/stats.json`). p10 / p50 / p90 unless noted.

| statistic | Chord gen | Chord lib | Lead gen | Lead lib | Pad gen | Pad lib |
|---|---|---|---|---|---|---|
| bars 2 : 4 | 38 : 62 % | 30 : 70 % (1-bar 2 %, > 4 cut) | 26 : 74 % | 20 : 71 % (1-bar 9 %) | 25 : 75 % | 23 : 77 % |
| notes per bar | 8 / 18 / 30 | 6 / 16 / 30 | 3.75 / 5.75 / 9 | 2 / 6 / 11 | 1.5 / 3 / 10 | 1 / 3.5 / 12.75 |
| onsets per bar | 3 / 5 / 7.5 | 1.5 / 5 / 7.5 | 3.75 / 5.75 / 9 | 2 / 5.75 / 11 | 0.5 / 1.5 / 4.5 | 0.5 / 1.5 / 3.75 |
| lowest pitch | 38 / 50 / 56 | 36 / 49 / 61 | 44 / 55 / 67 | 43 / 58 / 74 | 52 / 58 / 64 | 39 / 57 / 72 |
| highest pitch | 62 / 68 / 73 | 60 / 68 / 79 | 62 / 74 / 81 | 54 / 70 / 82 | 67 / 73 / 80 | 67 / 75 / 84 |
| onset grid 1/4 · 1/8 · 1/16 · off-grid % | 38 · 26 · 15 · 20 | 30 · 28 · 24 · 17 | 38 · 26 · 13 · 22 | 27 · 31 · 18 · 20 | 68 · 13 · 0 · 18 | 58 · 20 · 2.5 · 18 |
| note length ≤1/16 · 1/8 · 1/4 · 1/2 · 1 bar · 2 bars % | 68 · 19 · 12 · 2 · 0 · 0 | 59 · 28 · 7 · 2.5 · 3 · 0.5 | 62 · 26 · 11 · 1 · 0 · 0 | 66 · 26 · 6 · 1 · 0.4 · 0.1 | 0 · 23 · 18 · 22 · 29 · 8 | 4 · 13 · 11 · 26 · 34 · 10 |
| voices per onset 1 · 2 · 3 · 4 · 5 · 6 % | 10 · 10 · 32 · 30 · 16 · 2 | 10 · 15 · 20 · 22 · 22 · 9 | 100 | 99 · 1 | 46 · 9 · 16 · 19 · 8 · 1 | 35 · 9 · 12 · 18 · 17 · 5 |
| degree % root · 2 · b3 · 4 · 5 · b6 · b7 | 20 · 10 · 19 · 8 · 18 · 10 · 14 | 24 · 8 · 17 · 8 · 21 · 5 · 11 | 38 · 9 · 16 · 6 · 16 · 6 · 9 | 44 · 6 · 12 · 9 · 10 · 6 · 9 | 23 · 9 · 19 · 8 · 18 · 10 · 13 | 23 · 10 · 16 · 10 · 19 · 6 · 13 |
| rest share p50 | 52 % | 47 % | 39 % | 44 % | 0 % | 0 % |
| velocity mean p50 | 99 | 98 | 101 | 100 | 92 | 92 |
| non-diatonic notes p50 / p90 | 0 / 0 % | 0 / 25 % | 0 / 0 % | 0 / 25 % | 0 / 0 % | 0 / 16 % |

Deliberate differences: no 1-bar clips (the task asks for 2 or 4 bars), no 8-bar sources cut to 16 beats, and
borrowed notes are kept to ≈ 1 % of clips (the library's chromatic share comes largely from mislabelled files,
research §2.3).

**Where the tables live (2026-10-16, SPEC §9.25).** The progression, chord-kind, rhythm and lead-walk tables below are
no longer defined in `make_clips.py`: they sit in `tools/theory_data.py` (section 1, byte-for-byte as the clip
generator used them) next to the on-device THEORY generator's additions (section 2: 126 progressions with borrowed
chords, voicings, the extended rhythm banks, the lead Markov table). `make_clips.py` imports them, and
`tools/make_theory_tables.py` emits the firmware's `gen/theory_tables.hpp` from the same file (`--check` fails when the
header is stale; `tests/run_theory.sh` runs it), so the card's GEN clips and the device generate from one source of
truth. Verified: regenerating the 36 Amin / Emin clips after the move gives files byte-identical to the card's (same
name and hash). The tables printed below document those values.

## Style tables

All pitch material is built from scale degrees of the natural minor (`MINOR = 0 2 3 5 7 8 10`); chords are
degree stacks (never semitone templates), so every pitch class is in the key unless a table says "borrowed".
Weights are relative. Measured library numbers that a table targets are quoted in brackets.

**Bars.** Chord 2 bars 35, 4 bars 65 [library 2-bar 30 %, 4-bar 70 %]; Lead and Pad 2 bars 25, 4 bars 75.

**Progressions** (one degree per slot, `V` = borrowed major dominant, i.e. v with the leading tone; weights after
the library's chord-root usage, i ≈ 40 %, VII / VI / iv / v / III 8-12 % each, ii rare):

| weight | progressions |
|---|---|
| 8 | i i i i |
| 5 | i VI i VI · i iv i iv |
| 4 | i v i v · i VII i VII · i VI VII i · i VI III VII |
| 3 | i III i III · i VII VI i · i III VII VI · i VI iv i · i iv v i · i iv VI VII |
| 2 | i VII iv VI · i III VI VII · i VI VII iv · i VII III VI · i iv III VII · i iv v VI · i III iv VII · i i VI VI · i i VII VII · i i iv iv · VI VII i i · i iv VII VI |
| 1 | i v VI VII · i i III III · i VII iv iv · i ii i ii · i VI ii v · III VII i i · i III VII iv · iv VI i VII · i VI iv VII · VI VII i III · **i VI i V · i iv V i · i i i V** (borrowed) |

Chord: 0.5 / 1 / 2 / 4 chord changes per bar with weights 14 / 46 / 32 / 8 [chords per bar p50 2, p90 4]; Pad:
1 / 2 / 0.5 bars per chord with weights 60 / 30 / 10 [p50 1 chord per bar]; Lead: the progression is the hidden
harmony, one chord per bar.

**Chord kinds** (degree stacks above the chord root; the two-voice kinds answer the library's 15 % two-voice onsets):

| kind | degrees | Chord weight | Pad weight |
|---|---|---|---|
| triad | 0 2 4 | 18 | 26 |
| 7th | 0 2 4 6 | 24 | 24 |
| 9th | 0 2 4 6 8 | 16 | 16 |
| add9 | 0 2 4 8 | 9 | 9 |
| 6th | 0 2 4 5 | 4 | 3 |
| sus2 / sus4 | 0 1 4 / 0 3 4 | 3 / 3 | 6 / – |
| 5 (power) | 0 4 | 8 | 8 |
| shell | 0 2 6 | 2 | 3 |
| 9 no root | 2 4 6 8 | 4 | – |
| dyad / oct | 0 2 / 0 7 | 5 / 4 | – / 5 |

**Voicing.** Candidates are every inversion stacked closed inside the register, shaped by the style; the pick is
the least total voice movement from the previous chord (voices matched by rank, small random tie-break), with
*slack* (Chord 0.3, Pad 0.15): that often the second-best candidate is taken — a player's lift, and an inversion
the firmware's Variation (`chordVariation`, least movement) can then improve. Styles: Chord closed 24, open
(drop-2) 14, spread (bass note + upper structure) 6, double (root doubled an octave up) 14, parallel 12 (diatonic
planing: the first chord's shape moved along the scale), root 30 (root position, no voice leading); Pad closed
50, open 25, spread 15, double 10. Chord adds the root an octave below (36..47) on strong beats in 25 % of clips
and drops the root of 4+-note chords in 30 % (the Bass track has it); Pad adds a low root (40..51) in 20 %.
Registers: Chord lowest note from {43, 45, 48, 50, 52, 55} (weights 2 3 4 3 3 2), span 19 / 22 / 24 / 27, top ≤ 78;
Pad from {48, 52, 55, 57, 60, 64} (2 3 4 3 3 1), span 19 / 24 / 27, top ≤ 84.

**Chord rhythm** — 16-step onset masks per bar (x = onset) with weights [onsets per bar p10/p50/p90 = 1.5 / 5 / 7.5]:

```
whole        x...............  5    halves      x.......x.......  5    quarters     x...x...x...x...  7
8ths         x.x.x.x.x.x.x.x.  4    offbeat8    x.x...x...x...x.  5    tresillo     x..x..x...x.x...  6
tresillo2    x..x..x.x..x..x.  5    skank       x.x.x.x...x.x.x.  4    sparse       x.....x...x.....  4
dotted       x..x....x..x....  5    pushpull    x...x.x.x...x.x.  4    garage       x.x...x.x.x...x.  4
stab2        x.....x.........  3    stab3       x...x...x.x.....  3    clave        x..x..x...x...x.  4
answer       x.......x.x.....  3    quarters-push x...x...x...x.x. 3   gallop       x.x.x...x.x.x...  3
dotted8      x......x......x.  3    syncop      x...x..x..x.x...  4    shuffle16    x..x.x..x..x.x..  5
funk         x.x.x..x.x.x..x.  4    dotted16    x..x...x..x...x.  5    anticip      x...x..x....x..x  4
triplet-feel x..x..x..x..x...  4    broken      x.x..x.x.x.x..x.  4    bounce       x.x...x.x..x..x.  4
```

Gate: a fixed stab of 12 or 24 ticks (1 : 3) with weight 50, else 0.4 / 0.6 / 0.9 of the distance to the next
onset or legato (weights 18 / 12 / 11 / 9) [note lengths ≤ 1/16 59 %, 1/8 28 %]. Velocity base {88 94 100 106 110},
strong-beat accent {0 6 12}, −3 per voice from the bottom, ±3 jitter [mean 98]. 40 % of clips end with a fill
(a pickup on step 13..15 of the last bar, sometimes dropping the bar's last hit); 30 % push (hits on the last 8th
before a change anticipate the next chord).

**Pad styles** (weights): sustain 36 (one articulation per chord; 30 % of clips let later chords enter an 8th or a
quarter early, 20 % after a short rest), swell 12 (two articulations, the second softer), pulse 8 (quarter or 8th
pulses, gate 0.8), suspend 12 (the top voice moves to its in-key neighbour for the last beat or two), line 12
(monophonic: 1-3 held diatonic chord tones per chord in 60..79, sometimes with an in-key fifth or fourth below)
[library 35 % single-voice onsets, 18 % monophonic clips], drone 10 (the chord root, with its fifth when that is
in key, held 2-4 bars under a diatonic colour tone per chord), arp 10 (voices enter one by one, a quarter or an
8th apart, and pile up legato). 40 % of clips are legato
(notes overlap the next chord by 24 ticks) [legato p50 40 %]. Velocity {86 92 98 104}, −2 per voice [mean 92].

**Lead.** Styles: riff 43 (degree walk), octave 20 (root / octave bounce with b3, 5, b7 colours), arp 12 (chord
tones of the hidden harmony up, down or up-down), call-response 25 (bar 1 asks, bar 2 answers on the root).
Rhythm templates (34, weights 1-8, every one starts on the downbeat) [onsets per bar 2 / 5.75 / 11]:

```
x.x.x.x.x.x.x.x. 8   x..x..x...x.x... 6   x.x.x...x.x.x... 6   x...x.x.x...x.x. 5   x.xxx.x.x.xxx.x. 4
x..x.x..x..x.x.. 4   x.x..x.x.x.x..x. 5   x...x...x.x.x... 3   x.....x.x.....x. 3   xx.xx.xx.xx.xx.x 3
x.x.x.x......... 3   x..x..x.x..x..x. 5   x.x.x.xxx.x.x.x. 3   x...x...x...x... 2   x.xx.x.xx.x.x.x. 3
x..x.x.xx..x.x.x 3   x.......x....... 2   x...........x... 1   x..x....x..x.... 3   x.x.....x.x..... 3
x...x.x...x.x... 3   xxxxxxxxxxxxxxxx 1   x.x.x.x.x.x.x... 4   x..x..x...x...x. 3   x......x......x. 2
x.x...x...x.x... 4   x...x..x..x..... 2   x.x.xx.x.x.xx.x. 3   x.x.x.x.x.x.xx.x 2   x.xx.xx.x.xx.xx. 2
x.x...x.x.x...x. 4   x.x.x.x...x.x... 4   x.x...x...x.x.x. 3   x...x.x.x.x...x. 3
```

Degree walk (relative scale degrees, 7 = octave) [library top-voice intervals: repeat 26 %, octave 20 %, +2 st 8 %,
+3 st 7 %, +7 st 6 %]: 0 ×24, ±7 ×9 each, +1 ×10, −1 ×8, +2 ×7, −2 ×5, +4 ×4, −4 ×3, ±3 ×3 each, ±5 ×1 each. Weak-beat
notes are pulled home with probability 0.4 to {root 9, b3 2, 5 1, octave 1, 5 below 1}; strong-beat notes outside
the bar's chord move one step to a chord tone with probability 0.35 [root 44 %]. First note: root 8, b3 2, 5 1,
5 below 1. Register: the root sits in 55..66 ("mid") in 60 % of clips, an octave higher in 25 %, lower in 15 %
[lowest p50 58, highest p50 70]. Gate: fixed pluck of 12 / 24 / 24 / 36 ticks with weight 42, else 0.45 / 0.7 /
0.95 of the distance or legato (25 / 15 / 10 / 8) [≤ 1/16 66 %, 1/8 26 %]; the last note before the clip's half
rings up to a quarter in 85 % of clips and the final note in 40 % (phrasing, and a held note is what the firmware's
lead Variation style 3 splits). Velocity {92 96 100 104}, +6 on beats, ±3 [mean 100, range 8].

Form: 2 bars `A B` 60 / `A A'` 40; 4 bars `A A' A B` 45 / `A B A B'` 25 / `A A B B'` 15 / `A B C B'` 15 (call-response:
`A B` and `A B A B'` 60 / `A B C B'` 40). Variation operators: sequence (±1 / ±2 scale steps) 4, rotate (rhythm an
8th later) 2, octave (one note displaced) 3, thin (drop a weak note) 3, pickup (a 16th before a beat) 3, invert
(mirror the contour) 1; `B` = ending (last two notes resolve to root / chord tone, often a rest before the repeat)
plus one operator half the time; `C` = sequence plus one operator. Leads are monophonic: one note per onset,
never overlapping the next. Borrowed note: in the last bar, a b7 right before a final root on a weak step is
raised to the leading tone with probability 0.35 (≈ 0.5 % of clips).

**Humanisation** (share of clips; shifts 3..12 ticks, the downbeat never moves, whole chords move together except
in strum) [library off-grid onsets Chord 17 %, Lead 20 %, Pad 18 %]:

| mode | Chord | Lead | Pad | what moves |
|---|---|---|---|---|
| none | 50 | 36 | 50 | – |
| loose | 28 | 32 | 18 | each later onset with probability 0.4 by ±3..8 ticks |
| late (swing) | 16 | 32 | 12 | odd 16ths +4..12 ticks with probability 0.85, 8th offbeats with 0.4 |
| strum | 6 | – | 20 | 60 % of the chords: voices 3..5 ticks apart from the bottom |

Result in the build: off-grid onsets Chord 19.7 %, Lead 21.6 %, Pad 18.0 %. The editor and `mutate` work on
12-tick units; a 12-tick shift lands on the 1/32 grid (1.1-1.2 % of onsets), every other value is off-grid.

## Known limits

- Nothing has been auditioned; the research's listening gate (blind A/B on the desktop build, median ≥ 3.5 / 5)
  is still to run before the clips go on the card. The statistics match the library, the ear has not voted.
- Clips are written only to the minor folder of their key (research Q10); the picker also sees them from the
  relative major's project key, as for every minor-labelled file.
- The picker weighs files uniformly: Lead becomes ≈ 150 generated : 161 library per key, Pad 120 : 94, Chord
  60 : 293 (research Q9). The `GEN` token makes a later ratio limit or prune a name filter.
- The firmware's lead Variation (`leadVariation`) draws its style 1-based (`randomInt(rng, 3)` → 1..3): style 3
  adds no passing tones and only splits a note ≥ 48 ticks that is not the clip's last — a clip of nothing but
  stabs leaves the slot unchanged. The phrase-end "ring" above is the generator's answer; the harness column
  "Variation unchanged" measures it.
- Pads score low on "distinct rhythm skeletons" (21 %) because a sustained pad has one onset per chord; their
  pitch+rhythm skeletons are 1,399 distinct of 1,440 and no near-duplicate pair appears.
