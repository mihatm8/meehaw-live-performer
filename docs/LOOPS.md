# Loop content for the loop mode (125 BPM)

Pre-rendered loops for the loop mode (SPEC §9.19; docs/RESEARCH_FEATURES.md §5, route B: stretch on the Mac,
stream on the device). `tools/make_loops.py` picks loops near the target tempo from the REAPER loop library,
stretches each one to exactly 125 BPM with its pitch preserved, and writes whole-bar 22050 Hz mono 16-bit WAVs into
the card layout. The firmware reads this folder in loop mode: SHIFT + edit on the selected track calls
`Generator::setLoopMode`, which maps the track's category to its folder (`Generator::loopFolder`: Beat→drum,
Percussion→percussion, Bass→bass, Chord→chord, Lead→lead, Pad→pad, Fx→fx, Vocal→vocal), warms and picks a file
through `Library::prepareLoop / pickLoop` (shuffle bags, shards and the no-repeat history as every other picker),
opens it as one of the two loop streams (`kLoopSlots`; the Vocal track uses its set's own vocal stream) and hands it
to the engine at the bar line (`CmdLoopLoaded` + `CmdLoopMode`). The stream is varispeed at rate = bpm / 125 like a
vocal; the first loop in use pulls the tempo to 125 (UI status `LOOP ON - 125 BPM`). The names carry everything
the loop mode needs: the key folder, the BPM token, the `LOOP` tag and the `<n>bars` length.

## Layout

```
/lp/packs/<Genre>/loops/<category>/<Key|NA>/<Key|NA>__125bpm__LOOP__<n>bars__<slug>.wav
/lp/packs/<Genre>/loops/<category>/<Key|NA>/sNN/…                 shards when a key folder holds > 64 files
/lp/packs/<Genre>/loops/<category>/<Key|NA>/_index.txt            one per leaf folder (write_indexes.py)
```

- `category` is one of `drum percussion bass chord lead pad fx vocal`. The library's `drum/combo` and
  `drum/solo` are merged into `drum`; a combo stem keeps its role as the last slug word: `_full`, `_kick`,
  `_hats` (the OSH "Tape" kits come as full / kick / hats triples of the same groove, so a loop set can layer them).
- `Key` follows the card's key folders (SPEC §6, same as `vocal/<Key>`): minor keys as `Amin`, major keys as
  the bare root `C`, sharps only (`Bbmin` → `A#min`, `Eb` → `D#`). Chord-ish REAPER labels fold to their root
  and mode (`Amaj9sus4` → `A`, `Dmin7` → `Dmin`, `Ebmin13` → `D#min`, `Gmaj6` → `G`). `NA`, or a name with no
  key label (drum loops start with the bpm token), lands in `NA`. The loop mode addresses key folders by name
  like the vocal picker does (`Library::prepareLoop`: tonal categories — bass, chord, lead, pad, vocal — use the
  key's minor label, its relative major and `NA`; drum, percussion and fx use `NA` plus every key folder present,
  since drum loops carry song keys the firmware ignores), so `loops/<category>` itself is not a shard root and
  `write_indexes.py` covers `loops/*/*`. Nothing found for the category → the UI says `NO LOOP: DRUM`.
- `125bpm` is the firmware's BPM token (`Generator::parseBpmToken`, SPEC §9.3); every file is at the target
  tempo, so varispeed is 1.0 at 125 BPM and tempo± varispeeds the loop like a vocal.
- `LOOP` tags the file as loop content. It is also the "loop" token that `Library::hasLoopToken` rejects for the
  Vocal track (SPEC §6), so a loop can never be mistaken for a vocal phrase even if a picker ever wandered in here.
- `<n>bars` is the length in whole bars at the target tempo. The file is **exactly** n × 4 × 60 / 125 s long:
  1.92 s = 42 336 frames per bar (2 bars = 84 672 frames, 4 bars = 169 344), so a loop track can wrap by frame
  count without reading the header and the bar clock never drifts. `Generator::parseBarsToken` reads the token
  (digits right before `bar`, SPEC §9.17b) and `Generator::loopBars` reports it to the UI (the editor's loop view
  shows `2 BARS`); the clip cycle is `<n>bars` × 384 ticks, and the pattern keys crop it (whole, half, 2 beats,
  1 beat).
- `slug` is the pack code plus the loop's name words (`OSH_Combo_Tape_Bigapple_full`, `91UKG_91V_UKG_Synth_Bass_Bingo`);
  only `[A-Za-z0-9_-]`, cut at a word boundary so the whole name stays ≤ 95 bytes (the firmware's `kMaxNameLen`),
  with a 4-hex digest appended when two sources would collide. A library dedupe hash (`__65e35803`) keeps its
  first four digits. With `--split-bars N` a long loop is cut into whole N-bar segments `…_A`, `…_B`, ….

## Audio

Every output is 22050 Hz, mono, 16-bit PCM, peak −1 dBFS, padded with silence or trimmed to the whole-bar length,
with a 10 ms linear fade at the end of the content (so the wrap never clicks). Stretching is done by
[Rubber Band](https://breakfastquay.com/rubberband/) 4.0 (`brew install rubberband`), R3 engine
(`rubberband -3 --tempo <source>:125`), on a mono float copy of the source; a loop already at 125 BPM is only
resampled. Without rubberband the tool falls back to ffmpeg's `atempo` filter (WSOLA; audibly rougher on tight
hats). `--engine r2` uses the R2 engine with crispness 6 (an option meant for drums) when R3 smears a loop.

## Selection (what the card holds)

Window ±8 % around 125 BPM (115..135), so stretch ratios stay within 0.92..1.09. Pick order per category:
2- and 4-bar loops first, then 1-bar, then 8-bar; inside a length class source tempo 125, then 124/126, then by
distance; loops whose source length is not within 0.06 bar of a whole bar count rank last. Every class is spread
round-robin over keys (drum: over combo/solo first, then keys) so one key cannot take every slot. Loops longer
than `--max-bars` (8) are dropped; when the planned size exceeds `--budget-mb` (120) the longest loops go first.

Per-category limits and the current result of the House library (1542 source WAVs, 2026-10-16):

| category   | in window | limit | written | bars           | MB    |
|------------|-----------|-------|---------|----------------|-------|
| drum       | 124       | 60    | 60 (30 combo = 10 full/kick/hats kits, 30 solo) | 2: 59, 4: 1 | 9.85 |
| percussion | 196       | 40    | 40      | 2: 37, 4: 3    | 6.95  |
| bass       | 35        | 30    | 30      | 2: 15, 4: 10, 8: 5 | 8.88 |
| chord      | 27        | 30    | 27      | 2: 2, 4: 18, 8: 7 | 10.66 |
| lead       | 50        | 20    | 20      | 2: 12, 4: 8    | 4.52  |
| pad        | 3         | 15    | 3       | 8: 3           | 1.94  |
| fx         | 48 (13 > 8 bars) | 20 | 20   | 2: 3, 4: 17    | 5.98  |
| vocal      | 652 (154 > 8 bars) | 40 | 40  | 2: 20, 4: 20   | 9.69  |
| total      |           | 255   | 240     |                | 58.47 |

Source tempos of the written loops: 125 × 167, 120 × 38, 130 × 15, 135 × 11, 124/128 × 3 each, 118/122/126 × 1.
The 15 eight-bar loops (bass, chord, pad) are kept whole: a streaming track's clip may be as long as `kMaxLoopBars`
= 8 bars (`kMaxLoopTicks` = 3072, SPEC §9.19; patterns stay at 16 beats), so they play whole; `--split-bars 4` would
re-render them as A/B halves (255 files, same MB) should shorter cycles be wanted.

## Regenerate

```
brew install rubberband                        # once; the tool falls back to ffmpeg atempo without it
python3 -I tools/make_loops.py --src ~/Documents/cardputer/House/loops --out sdcard --genre House \
        [--bpm 125] [--tolerance 0.08] [--budget-mb 120] [--max-bars 8] [--split-bars 4] \
        [--limit drum=60,percussion=40,bass=30,chord=30,lead=20,pad=15,fx=20,vocal=40] \
        [--scan-only | --dry-run] [--only drum,vocal] [--manifest loops.tsv]
python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard --quiet
python3 -I tools/make_loops.py --out sdcard --verify-only     # duration ±5 ms, name tokens, peak ±1 dB
```

`--scan-only` prints the BPM histogram per category and the key-label folding; `--dry-run` prints the full plan
(source → output name) without writing; `--only` re-renders some categories and leaves the others; `--keep`
does not delete the existing `loops/` folder first. The whole House run (240 loops, rubberband R3, 12 threads)
takes about 6 s on an M-series Mac; the source library is read in place and never modified. Stdlib Python only;
ffmpeg and rubberband are the external commands; run with `-I`.

The tool verifies its own output at the end (and `--verify-only` checks a card later): every WAV is 22050/mono/16,
its frame count is within 5 ms of bars × 42 336, its name parses (`key`, `125bpm` as the firmware reads it,
`bars`), it sits in the folder its key names, and its peak is within 1 dB of −1 dBFS.
