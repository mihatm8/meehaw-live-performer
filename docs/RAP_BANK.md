# RAP bank — auto-generated rap vocals for the Vocal tracks

`tools/make_rap.py` renders a bank of rhythmic "dance tune" rap phrases with macOS text-to-speech (`say`) and
writes them into the card as ordinary vocal files (SPEC §6, §9.3, §9.16, §10.1):

```
sdcard/lp/packs/House/vocal/<Key>min/<Key>min__125bpm__RAP<M|F|R>__<n>bars__<slug>.wav
sdcard/lp/packs/House/vocal/NA/NA__125bpm__RAPR__<n>bars__<slug>.wav
```

22050 Hz, mono, 16-bit PCM, peak −1 dBFS like every other vocal file (the plain peak-normalise path of
`tools/import_library.py`). `125bpm` is the BPM token the firmware needs for varispeed; `RAPM`/`RAPF`/`RAPR`
tag male, female and robot voices; `<n>bars` (1, 2 or 8) says the file is exactly that many bars long at 125 BPM
(1.92 s per bar), so a player that honours the bars token can loop the whole file instead of the 2-bar window
+ pause cycle. The slug is the first line's words, at most 24 characters of `[A-Za-z0-9_]`; a `_2` suffix keeps
names unique inside a folder. No name carries a `loop` token (the library would skip it, SPEC §6).

## What is generated (defaults)

| per key folder (12 natural-minor keys A E B F# C# G# D# A# F C G D) | files | bars |
|---|---|---|
| male: 4 one-shots (`1bars`), 3 loops (`2bars`), 1 session (`8bars`) | 8 | 18 |
| female: same | 8 | 18 |
| robot: 2 one-shots, 1 loop | 3 | 4 |
| **per key** | **19** | **40 = 3.4 MB** |
| NA (robot, keyless): 6 one-shots (two of them sung by the novelty voices), 3 loops, 1 session | 10 | 20 |
| **total** | **238** | **500 bars = 42.3 MB** |

Product types, all assembled on a 125 BPM grid of 1/8-note slots (240 ms = 5292 samples, 8 per bar):

* **one-shot** (1 bar): one shouted word (`jump`, `higher`, `tonight`, `everybody`, `fire`, ...) or a 2–3 word
  phrase (`hands up`, `pump it up`, `move your body`, `feel the bass`, ...) starting on the downbeat, rest of the
  bar silent. A lone shout may ring for up to four slots, a sung novelty shout for the whole bar.
* **loop** (2 bars): a rhymed couplet, or one rhymed line twice.
* **session** (8 bars): four rhymed couplets (AABB) from four different rhyme families.

Lines are one bar: an opener phrase (`we own the night`, `put your hands up`, `jump _ jump _` — `_` is a
one-slot rest) plus a tail that ends on a rhyme word (`... tonight`, `... take me higher`, `... shake the ground`).
A one-syllable word takes one slot, a longer word two slots (sixteenth-note syllables, rap flow), a line fills
6–8 slots and ends inside its bar; the last word of a line gets a second slot to breathe when there is room.
The lexicon in the script holds about 300 words with syllable counts, 38 rhyme families, ~230 tails and ~120
openers. The whole plan is deterministic (`--seed`, default `rap125`); `--dry-run` prints every lyric with its
slot positions, and every run writes a lyric sheet to `tools/.rap_cache/lyrics.txt` (file → voice, root, lyrics).

## How the audio is made

1. **Voices** (`VOICES` in the script). Male: `Rocko (English (US))`, `Reed (English (US))`; female: `Karen`,
   `Moira`; robot: `Zarvox`, `Trinoids`, `Fred`; novelty singing voices `Cellos` and `Bells` for two NA
   one-shots. Files alternate between the two voices of a class. The choice came from measuring every
   installed English voice: Karen and Moira hold a word within ~2 semitones with no creak; all male voices drop
   to a pitch floor (~MIDI 36) in the last ~70 ms of every word once the pitch base is above ~45, Rocko and Reed
   the least; Daniel and Samantha creak the most and are not used. `[[pmod]]` (pitch modulation) is ignored
   by all of these voices, punctuation does not change the contour, and the fall is the same whether words
   are rendered alone or in a batch.
2. **Rendering.** Words are rendered once per (voice, pitch): a batch of up to 40 words goes into one `say`
   call as `word. [[slnc 600]] word. ...` (speaking rate `-r 250`, one syllable per 1/8 note), the AIFF is
   converted by ffmpeg to 22050 Hz mono PCM and split on the pauses (−50 dBFS, ≥ 300 ms of quiet; a click in a
   pause is dropped, any other mismatch falls back to one `say` call per word). Each word is silence-trimmed
   (−54 dBFS, as `import_library.py`) and cached under `tools/.rap_cache/words/<voice>/p<pbas>_r<rate>/`.
   The whole bank is ~70 `say` calls (≈ 20 s); a single `say` per word would take 40× longer.
3. **Key.** The key root for a file is the root's MIDI note nearest the voice's centre: male 46.5 → octave 2/3
   (F2..E3, MIDI 41..52), female 58.5 → octave 3/4 (F3..E4, 53..64). The `say` pitch base `[[pbas N]]` is not
   a MIDI note, so each voice is calibrated once (pbas 30..72 in steps of 3, eight words each, core pitch
   measured, cached in `tools/.rap_cache/calib/*.json`) and the inverse of that table picks N. Then every
   rendered word is measured (YIN pitch estimator on 2:1 decimated audio; the word's "core" is the median of
   the voiced frames between 10 % and 60 % of its voiced span, which leaves out the final fall) and shifted to
   the root with ffmpeg `asetrate` + `aresample` (±6 semitones at most; unvoiced words such as `the`, `up`, `it`
   get their group's median correction). Robot voices are left as they are, in key folders and in NA.
4. **Fitting.** A word longer than its slot budget (92 % of slots × 240 ms) is time-compressed with ffmpeg
   `atempo` (never more than 2×; median 1.25×) and whatever is still too long is cut with a 20 ms fade. Words
   are levelled to a loudest-20 ms RMS of −10 dBFS (capped at −1 dBFS peak), faded 3 ms in / 10 ms out.
5. **Assembly.** Words are copied to their slot starts into a buffer of exactly bars × 42336 samples, the file is
   peak-normalised to −1 dBFS and written as a 44-byte-header WAV.

## Listening-free verification (printed at the end of every run, `--verify-only` re-checks)

* duration = bars × 1.92 s (exact to the sample; tolerance ±20 ms), peak = −1.00 dBFS, every name parses
  (key folder, `125bpm`, `<n>bars`, slug charset and length, < 95 bytes);
* grid: energy onsets detected in the output files vs the planned slot starts — 85 % within 30 ms, median
  3.6 ms; the late ones are words that start with a soft consonant (`we`, `the`, `bass`), the placement itself
  is exact. A few files are printed with their planned `bar.slot:word` list and detected onset times;
* pitch: the core pitch of every voiced word in every male/female output file vs the key root — last run:
  male 618 words, 89 % within 1 semitone (94 % within 2), female 1097 words, 89 % within 1 (97 % within 2);
  outliers are syllabic-consonant words like `rhythm` and creaky male word tails.

## Regenerate

```
python3 -I tools/make_rap.py                      # all 12 keys + NA into sdcard/ (≈ 1.5 min with a cold cache)
python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard --quiet
```

Useful switches: `--keys A,E` (subset), `--dry-run` (lyrics only), `--verify-only`, `--out <card root>`,
`--genre`, `--seed`, `--rate`, the per-type counts `--oneshots-mf/--loops-mf/--sessions-mf`,
`--oneshots-r/--loops-r/--sessions-r`, `--na-oneshots/--na-loops/--na-sessions`, `--no-tune` (pbas only),
`--keep-existing` (by default the tool first deletes every `*__RAP?__*bars__*.wav` under the genre's vocal
tree, including ones that sharding moved into `sNN/`). Budget check: 238 files / 42.3 MB; the limit is ≤ 70 MB
and ≤ 40 new files per key folder before sharding (19 here), so a second session per voice (`--sessions-mf 2`,
+16 MB) still fits.

Where the files land: a flat key folder gets them at its root (sharding then splits folders above 64 files,
e.g. Cmin 55 + 19 → `s00`/`s01`); a key folder that is already a shard root of `sNN` subfolders (Emin) gets a
`rap/` subfolder instead, because an indexed folder is a leaf and media next to subfolders would make the
subfolders unreachable (SPEC §10.1). Run `write_indexes.py` afterwards so every new folder has its `_index.txt`.

The cache (`tools/.rap_cache`, ~50 MB: raw words, fitted words, calibration tables, lyric sheet) can be
deleted at any time; a run with a cold cache re-renders everything.

## Voice licence note

The voices are Apple's macOS system voices. Apple licenses them for personal use on the Mac that renders them;
the rendered files are for this player's own card, not for redistribution as a sample pack. Regenerate on your
own machine rather than copying the bank around, and keep the generated set out of any public release of the
repository.
