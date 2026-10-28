# Library expansion: feasibility study and decision document

Date: 2026-10-16. Status: **research only**. Nothing under `sdcard/`, `firmware/` or `tools/` was changed; this file is the only output. Every number is traced to one of five research reports, tagged **[midi]** (online MIDI sources), **[gen]** (procedural generation), **[voc]** (vocals), **[303]** (MiniAcid / acid level), **[pre]** (synth presets), and to the `file:line` each report cites. Measurements live in the session scratchpad (`.../scratchpad/research/{midi-sources,procgen,vocals,miniacid-303,synth-variety}/`). Anything a report could not confirm is marked **unverified**.

## 0. Decision summary

| Goal | Cheapest path that gets there | Hours | What it costs on the card / device |
|---|---|---|---|
| Acid tracks as loud as the rest | One formula (`driveMakeup`, synth_voice.cpp:30) + 4 `level` numbers | 1–2 h | 0 B RAM, 0 flash, 0 CPU |
| ≥ 300 Chord/key | Transpose own clips (A) + ldrolez MIT chords (B, capped) | 6–9 h | ≈ 11,600 MIDI files, ≈ 5 MB of bytes |
| ≥ 300 Lead and Pad/key | A + B first (Lead ≈ 270, Pad ≈ 200), then the clip generator (D) | +18–27 h | +3,240 files; 0 firmware change |
| Hundreds of vocals/key | Import skipped shots + pitch-shift sung phrases + more TTS voices (V1), then chop the owned vocal loops (V2) | 8–10 h, then 4–6 h | ≈ 1.0 GB at 500/key |
| Varied synths | Generated preset bank (381 → 500 presets), bitmask bag, baked trims | 20–25 h | +46–60 KB flash (+1.4–1.8 %), 0 RAM |
| MiniAcid as the acid synth | 303 *character* inside `SynthVoice` (saturated SVF + accent, SAT/ACC knobs), **not** the MiniAcid code as-is (9.4 ms of the 11.6 ms block, 88 KB delay) | ≈ 8 h | +256 B RAM, +0–6 KB flash, +0.46 ms/block worst case |

Core programme ≈ **65–87 h**. Section 7 has the order and the questions that must be answered first.

---

## 1. Where the library stands

### 1.1 Melodic clips, as the picker sees them (minor folder + relative-major folder + NA)

| Category | per key (min–max) | mean/key | total files | source |
|---|---|---|---|---|
| Chord | 10–45 | 24.4 | 293 | task context; [midi] |
| Lead | 4–28 | 13.4 | 161 | [midi] |
| Pad | 0–18 | 7.8 | 94 | [midi] |
| Fx | 0–14 | — | 76 | task context ([midi] counted 3–17 per key) |
| Bass | 80–358 | — | 2,136 | task context |

Whole MIDI tree: **3,815 files, 1.48 MB, mean 408 B** [midi]. Library sizes p50: Chord 451 B, Lead 225 B, Pad 193 B [gen].

What the clips are like [gen, `analyze_clips.py` over all 548 Chord/Lead/Pad files]: Chord 4-bar 168 / 2-bar 71 / fractional 47 / longer than 16 beats (cut by the firmware) 26; Lead 98 / 28 / 21, 12 one-bar; Pad 68 / 20. Notes per bar p50: Chord 16, Lead 6, Pad 3.5. Pitch lo/hi p50: Chord 49/68, Lead 58/70, Pad 57/75. Polyphony: Chord 3–5 voices 63 %, Lead mono 94 % of clips, Pad single notes 35 %. Off-grid onsets 17–20 % (swing/humanisation the generator must reproduce).

Label quality [midi]: clips fully diatonic to their own key label: **Chord 58 %, Lead 84 %, Pad 73 %, Bass 87 %**; 33 % of Chord clips fit no single diatonic set. A content-based key detector agrees with the label at pitch-class-set level only 60–65 %. Conclusion: on 2–4-bar clips no algorithm can re-label reliably; the vendor label is what we have, and any transposition inherits its ~1/3 mismatch on chords.

### 1.2 Constraints that bound every option

- **Key is a label; the device never transposes.** `gen/key.cpp:94-96` accepts a file iff its label's pitch-class set equals the project key's (Amin ≡ C) [midi]. So "clips per key" means files in `<Root>min/` + the relative major folder + `NA/` (SPEC §6:178-181).
- **Clip ≤ 16 beats** (`core/config.h:13`), longer files cut at `gen/smf.cpp:260`; no time-signature read, so everything plays on a 4/4 grid; ≤ 192 notes; 12-tick length units; 5-bit velocity [gen]. 8-bar sources must be split; 6/8 material is unusable as-is [midi].
- **768 files per leaf folder** = `kTreeMax 12` (`gen/library.h:22`) × `shard_folders.py --max 64`; above that `shard_folders.py` grows the shard to ceil(n/12) and FAT directory opens slow down (23 ms at 64 entries vs 0.8 s at 700, SPEC:396) [gen]. Effective cap per key ≈ 1,536 + NA because two folders are merged [midi].
- **Picker**: bags of `kBagMax 64`, stratified for larger trees; roots are chosen with probability ∝ file count (`library.cpp:725-740`) → growing NA dilutes keyed picks for every key [voc]. Picker RAM is independent of file count (`sizeof(Library)` 14.3 KB, SPEC.md:447) [midi]. Cold pick with 12 shards 0.58 ms, warm 0.035 ms (SPEC §10.3); the 16-slot tree cache evicts between trees [gen].
- **Sync**: 5,757 files / 69 MB in 9 min ≈ **10.7 files/s** (SPEC.md:396) → 10,000 clips ≈ 16 min [midi].
- **Card**: 29.8 GB SDHC (SPEC.md:10). Cluster size **unverified** (card not mounted in the session); at FAT32's default 32 KB each small MIDI costs one cluster: ≈ **32 MB per 1,000 clips** on the card vs ≈ 0.3 MB of bytes [gen]. `diskutil info` on the card settles it.
- **Device budget**: flash 774,055 B (23 %), static RAM 251,628 B (76 %, 76,052 B left), free heap 13.1 KB (SPEC.md:1018-1019, 1071) [303][pre]. Audio block 11.6 ms, today's worst case 5.6–6.0 ms (SPEC §10) [303].

### 1.3 Vocals

`sdcard/lp/packs/House/vocal`: **518 WAV, 93.5 MB** = 280 sung pack phrases (279 GHUAC + 1 ASV, 51.2 MB, mean 179 KB) + 238 TTS `RAP*` files (42.3 MB) [voc]. The vocal tree has **no major folders**, so a vocal pick = `<Key>min` + `NA`. Sung files per key: Emin 66, Cmin 55, Bmin 37, C#min 27, A#min 26, Fmin 25, Dmin 22, F#min 6, **Amin / D#min / G#min / Gmin 0**; every key also has 19 RAP; NA 26 (16 GHUAC + 10 RAP). GHUAC durations: mean 4.15 s, 116 files ≤ 3.84 s (2 bars), 156 between 2 and 4 bars, 6 longer. A 2-bar file is 2 × 42,336 × 2 B + 44 = **169,388 B**; 1 bar 84.7 KB; 8 bars 678 KB [voc].

### 1.4 Synths

78 built-in presets (Chord 10+8, Lead 9+8, Pad 9+8, Bass 9+8, Fx 9; `dsp/presets.cpp:16-356`, `kAcidPresets = 8` at :366); `SynthPatch` 120 B; set files persist the preset **index** (`gen/sets.cpp:271, :923`); `PresetBag` is 20 B per track and **caps a family at 16** (`gen/generator.cpp:700` `if (n > 16) n = 16;`) — a bigger bank would silently never play entries ≥ 16 [pre]. The hand bank's loudness spread within one family is 11–21 dB (Chord Organ −9.8 vs StabHouse −30.4 dBFS) [pre].

---

## 2. Melodic clips (Chord, Lead, Pad)

### 2.1 Options

Yield = usable 1–4-bar 4/4 clips **per key as the picker sees them**. Hours are the reports' estimates; "card" is bytes (multiply files by the cluster size for on-card cost, see §1.2).

| # | Option | Source / licence (quoted) | Yield per key Chord / Lead / Pad | Hours | Card | Risks |
|---|---|---|---|---|---|---|
| A | **Transpose the existing clips on the Mac** into the 11 other pitch-class sets (nearest shift in −6..+5 st, octave-fold to keep the median pitch within 6 st, clamp to measured bands Chord 53–69, Lead 48–79, Pad 59–77) | own library packs (BGK, PTS, DHS, MRH, NJH, DPH, Synth-DM …); **licences not stated anywhere** → card only | **293 / 161 / 94** visible per key (new keyed files 247 / 108 / 74; the keyless NA clips already serve every key) | 3 h [midi] – 4–6 h [gen] | 6,576 files ≈ 2.7 MB | inherits label mismatch (Chord 58 % diatonic); same 548 ideas repeated; `tools/prepare_content.py` has `write_midi` (:48), `read_notes` (:72), `transpose_to_range` (:132), needs `pip install mido` (`.venv` missing) |
| B | **ldrolez `free-midi-chords`** via `tools/prepare_content.py build_tonal` (:219-250) | "All the MIDI files are licensed under the MIT license" (README, https://github.com/ldrolez/free-midi-chords); **already on disk** `~/Documents/cardputer/_downloads/free-midi-chords` (release 20260314, 24,936 MIDI, 108 MB) | **540 Chord** (50 Major + 58 Minor progressions × 5 rhythm variants; Modal 82×5 excluded) / **108 Lead / 108 Pad** as coded (derived only from "straight" variants; up to 540 if the `if` is relaxed) | 1.5 h + listening | +2.5 MB; 6–8k files ≈ 12 min sync | generated block voicings, 5 rhythm templates, derived "leads" are arpeggios; `--per-key` default 12 (:501); names lack the `__hash` suffix (firmware ignores it); nobody has auditioned them |
| C | Chordonomicon → render progressions | "cc-by-nc-4.0" (https://huggingface.co/datasets/ailsntua/Chordonomicon); 679,807 progressions, 264 MB CSV | cap-limited: 768 Chord + 768 Pad per folder | 6–8 h | 264 MB download, ≈ 6 MB card | **non-commercial** licence blocks any future sale; scraped from user sites; quality = our voicing/rhythm generator |
| D | **Own clip generator** `tools/make_clips.py` (prototype measured: progressions × degree-stacked voicings × rhythm templates; pads sustained; leads motif + A A' A B) | ours | distinct-before-samey **Chord 800–1,000, Lead 1,500+, Pad 250–350** (pads saturate: 859 exact duplicates in 3,000 draws, 29 %) | **18–27 h** (tables 6–8, integration 3–4, host checks 2–3, docs 1–2, 2–3 listening rounds 6–10) | 311 B/clip mean; 0.2 ms/clip; 10,800 clips ≈ 3.4 MB bytes (≈ 350 MB at 32 KB clusters, **unverified**); sync ≈ 17 min | no off-grid feel (library 17–20 %); Variation slot is a no-op on 22–26 % of staccato clips (library 27 / 8 / 6 %); needs listening rounds; firmware parsed 7,714/7,714 prototype files with 0 failures and 0 empty variants |
| E | Nottingham ABC, 4/4 subset | **no licence statement** on https://abc.sourceforge.net/NMD/; jukedeck MIDI mirror "released under the GNU GPLv3 license" | ~300 lead + ~300 chord 4-bar phrases **total**, transposable to all keys | 4–5 h | small | folk/jig style; 75 % of tunes are 6/8; licence **unverified** |
| F | Lakh MIDI clip extractor | "distributed with a CC-BY 4.0 license" (compilation; underlying pop songs copyrighted); 176,581 files, lmd_full 1.77 GB | est. 150–400 clips / 100 songs (**unverified** estimate) | 15–20 h | 1.77 GB download | copyright of the songs; GM pop style; key labels ~60 % reliable |
| G | PDMX | "Creative Commons Attribution 4.0 International" (https://zenodo.org/records/15571083); 222,856 scores, mid.tar.gz 214 MB | est. 100–300 / 100 scores (**unverified**) | 12–20 h | 2.4 GB download | "public domain" is uploader metadata; classical/choral, little dance |
| H | POP909 | "MIT License" (software licence; 909 copyrighted Chinese pop songs) | est. ~10k melody + ~10k chord windows total | 8–10 h | — | compositions copyrighted; expressive piano needs quantising |
| I | OpenEWLD lead sheets | "containing only public domain scores … You can redistribute this work as you want"; count **unverified** (Zenodo 2.9 MB) | ~400 / 100 lead sheets, transposable | 4–6 h | 2.9 MB | jazz-standard style; count unverified |
| — | **Excluded** | Hooktheory/SheetSage dump (ToS: "no third party may copy, scrape, bulk-download … or redistribute … the TheoryTab database"); BitMidi/FreeMidi (no licence); Cymatics ("personal, non-commercial use only"), Unison ("may not be included … in any musical library or sample library product"), Ghosthack ("not allowed to redistribute or repack"), Function Loops ("You may not resell, re-license, trade, upload, copy, share or distribute the sounds"); Essen (legal status "unclear"); Tegridy/LA-MIDI/MetaMIDI/GigaMIDI (NC / research-only); GiantMIDI-Piano (unquantised); BenLeon2001 chord zips (no named licence, unknown provenance) | 0 | 0 | — |

Ranking by clips gained per hour [midi]: B ≈ 400–700/h, A ≈ 170/h, C ≈ 200/h (NC), D unlimited but 18–27 h, E ≈ 250/h (folk, licence unverified), F/G/H 100–150/h with copyright caveats.

### 2.2 Recommended combination to reach ≥ 300 per key

**Phase M1 — A + B (capped), 6–9 h, no firmware change.**

| Category | A (transpose) | B (ldrolez) | per key after M1 |
|---|---|---|---|
| Chord | 293 | cap B at ≈ 100 progressions × 2 rhythm variants ≈ **200** (so library material stays ≈ 60 % of picks; the picker weighs files uniformly) | **≈ 490** |
| Lead | 161 | 108 arpeggio leads, **only if a 20-clip audition passes** | ≈ 270 |
| Pad | 94 | 108 sustained progressions (a natural pad source) | ≈ 200 |

Folder arithmetic: A places each transposed clip in the folder of its new label (minor or major), so a key's 293 Chord clips are spread over two folders; B writes ≈ 290 minor + 250 major per key at the full 540 [midi]. Even at full B, no leaf exceeds 768; at the capped 200 the Chord folders hold ≈ 250 each (4 shards). Files: 6,576 (A) + 12 × (200 + 108 + 108) = 4,992 (B) ≈ 11,600 → ≈ 5 MB of bytes, ≈ 18 min sync; on a 32 KB-cluster card ≈ 370 MB (**unverified** cluster size).

**Phase M2 — D (clip generator), 18–27 h, closes Lead and Pad.** Generate ≈ 150 Lead and ≈ 120 Pad per key (well inside the measured distinct-before-samey ranges of 1,500+ / 250–350) → **Lead ≈ 420, Pad ≈ 320 per key**; Chord generation optional (Chord is already at ≈ 490). 3,240 files ≈ 1 MB of bytes, ≈ 5 min sync; shard count per key folder stays ≤ 6 of 12 (headroom ≈ 420 more per category per key [gen]).

Cheaper but weaker alternative for M2: option I (OpenEWLD, public domain, 4–6 h) or E (Nottingham, licence unverified) transposed into every key — real melodies, but jazz/folk style and unverified counts.

What M1+M2 does **not** fix: musical variety per idea (A repeats the same 548 ideas in 12 keys — fine on-device, one key at a time), label mismatch (inherited), and the picker's uniform weighting (generated:library ratio for Lead would be ≈ 60:40 → consider a `GEN` token, open question 9).

### 2.3 Transposition rules and QA gates (to adopt before any import)

Transposition [midi]: (1) nearest interval t ∈ [−6, +5]; (2) pick t' ∈ {t−12, t, t+12} keeping the clip's median pitch closest to the original (never > 6 st register move); (3) clamp to the category bands above, hard limits top ≤ 88 (Pad/Lead), bottom ≥ 36 (Chord) / ≥ 28 (Bass). Optionally re-label by content where the diatonic set is unique (possible for 16 / 12 / 22 % of Chord/Lead/Pad clips) and tag the 33 % chromatic chord clips.

Per-batch gate [gen §6]: `tests/refmodel_smf.py parse` + firmware parse (`LP_MIDI_DIR=… tests/run_key_smf.sh`) + a variants check (4 non-empty variants, correct lengths, every pitch `key.contains`, v1 ≠ v0 on ≥ 75 %, onset at tick 0, ≤ 192 notes, exact 1/2/4 bars, name ≤ 95 B, hash de-dupe); stats parity against the library's p10–p90 bands; sameness gate (near-duplicate pair rate < 1 %, p90 cell-Jaccard < 0.35, ≥ 60 % distinct rhythm skeletons per 1,000); blind A/B listening on the desktop build (`firmware/desktop/build/liveplayer_desktop <cardroot>`), accept at median ≥ 3.5 of 5 with no clip ≤ 2 in two consecutive batches; then `write_indexes.py --check`, `card_sync.py --verify N`, and a cold-GENERATE timing against SPEC §10.3.

---

## 3. Vocals

### 3.1 Options

| # | Option | Yield | Hours | Card | Risks / licence |
|---|---|---|---|---|---|
| A | Scale `make_rap.py` counts by CLI flags (e.g. `--oneshots-mf 40 --loops-mf 80 --sessions-mf 10 --oneshots-r 20 --loops-r 30 --sessions-r 5`) | ≈ 315 files/key, ≈ 3,800 bank-wide (dry-run planned in 0.17 s) | ≈ 1 h + ≈ 25 min compute (238 files took ≈ 1.5 min → 0.38 s/file) | 57.6 MB/key ≈ 690 MB | only **63 one-shot texts** → 46 slug collisions and **24 % repeated lines** at 315/key; TTS timbre; 8-bar sessions 678 KB each; Apple voice licence keeps the bank out of a public repo (RAP_BANK.md statement, terms **unverified**) |
| B | **Rubberband-transpose the 280 sung GHUAC phrases** ±1..3 st into neighbour keys (`rubberband -3 -F`, R3 + formant preservation) | up to 1,680 new sung files, **+140/key mean**; fills Amin/D#min/G#min/Gmin (0 today) | ≈ 2 h tooling + ≈ 2 min render (0.03–0.09 s/file) | ≈ 300 MB | **not listened to**: −3 st loses 1.6–4.6 dB above 5 kHz, possible phasiness on reverb tails; measured shifts within 0.04 st (dry) to ±0.5 st (wet, YIN noise); same 280 phrases reappear; pack licence → card only |
| C | Effect variants (ffmpeg robotize / ring-mod / bitcrush / rubberband monotone `--pitchmap`) | 3 chains × 518 ≈ 1,550 files, +130/key | 1–2 h; 0.05 s/file | ≈ 270 MB | same lyrics, only timbre differs; may read as gimmicky; `sox` absent, no librubberband in ffmpeg |
| D | **Chop the owned 652 in-window vocal loops** (`/Users/miha/Documents/cardputer/House/loops/vocal`: 829 loops, 3.9 GB, mean 19 s; 652 within 115–135 BPM) with `make_loops.py` stretch + `--split-bars`, then rename without the `LOOP` token | ≈ 2,600–3,300 sung, keyed, lyric-bearing files, **+220–275/key** (Gmin 97 / Bmin 96 loops heavy; G#min 28) | 4–6 h + ≈ 10 min rubberband | ≈ 500 MB | segments may cut mid-word; pack licences unknown → card only; stretch ratios up to 8 % |
| E | Import the 202 owned one-shots `import_library.py` skipped for lacking a BPM token (:171-180) | +68 keyed (Gmin 15, Bmin 11, D#min 10, Fmin 7) + 134 NA | ≈ 1 h | small | NA growth dilutes keyed picks; some shots < 1 beat |
| F | More `say` voices + lexicon growth | keyable voices 7 → 15 (Ava Premium, Allison/Evan Enhanced, Samantha, Daniel, Tessa, Flo, Sandy, Shelley, Eddy all respond to `pbas`; Siri Aman/Tara **ignore** it); +100 shouts/phrases lifts one-shot texts 63 → ≈ 160 | ≈ 1 h per 2 voices + 2–3 h lexicon | unchanged | male voices creak on word tails (RAP_BANK finding); quality judged by measurement only |
| G | Spoken-word corpora flattened to the root with `rubberband --pitchmap` | unlimited: LJ Speech 13,100 clips (public domain, **22050 Hz mono 16-bit = card-native**, 2.6 GB); LibriSpeech CC BY 4.0 (train-clean-100 6.3 GB) | 3–4 h | downloads need approval | book prose, not dance lines; CC BY needs attribution; NA inflation unless copied per key |
| H | Freesound CC0 harvest via API (`filter=license:"Creative Commons 0" tag:vocal duration:[0 TO 4]`) | pool: "vocal" 7,870, ≤ 4 s 3,397, acapella 432, vocal chop 233; maybe 300–1,000 usable, +25–80/key | 6–10 h | — | almost no key metadata (44 "vocal minor" hits); originals need OAuth2; many are FX |
| I | Looperman vocal loops / acapellas (4,758 / ~5,000, BPM + key labelled) | ≈ 60 files/h manual | hours of clicking | — | licence "non-transferable", acapellas "personal, non-commercial use", redistribution not addressed → never in the repo |
| J | Sung datasets: VocalSet (CC BY 4.0, 10.1 h, 20 singers, 2.1 GB), vocadito (CC BY 4.0, 40 excerpts), NUS-48E (Zenodo mirror CC BY 4.0, official licence **unverified**) | hundreds of vowel tones per key after segmentation | 4–8 h | 2.1 GB download | sustained vowels are pad-like, not phrases |

### 3.2 Recommendation (hundreds per key)

**Phase V1, 8–10 h:** E (1 h) + B (2 h + 1 h listening of the scratch `rb/` renders first) + F (3–5 h) + A at a **moderate** target of ≈ 150 TTS/key (halve the dry-run flags; ≈ 1 h + 15 min) → per key ≈ 20–200 sung + ≈ 150 TTS ≈ **250–350 vocals per key**, every key non-empty.

**Phase V2, 4–6 h:** D → +220–275 sung per key → **≈ 500–600 per key**, still under the 768-per-folder cap (5–8 shards). Card: 500/key × 12 ≈ 6,000 files ≈ **1.0 GB** [voc]; index cost ≈ 16 KB per key at 300/key, one 4 KB `kIndexChunk` per 64-file shard.

Design caveats to settle first: NA is count-weighted against the key folder (E, G and part of A inflate NA); pitch-shifted/effect files need a token (`_t+2`, `RAPX`) so `make_rap.py --keep-existing` and the shard tools treat them correctly; RAP_BANK.md's 70 MB / 40-files-per-key budget must be raised.

---

## 4. Synth variety: the generated preset bank [pre]

**Plan.** `tools/gen_presets.py` emits `src/dsp/presets_gen.hpp` (rows of `const SynthPatch` in initialiser order) included by `presets.cpp`. Archetypes with musical ranges: Chord Pluck/Stab/Keys/Organ/ChdPad; Lead Saw/Pluck/Bell/Hoover/Flute/Digi; Pad Warm/Glass/String/Dark/Sweep/Hollow/Air/Brass; Bass Sub/Acid/Reese/Fm/House; Fx = lead archetypes with glide 0.12 + Wasp; '303' families inside the SPEC §9.17b ranges. Names `"<Arch> <Adj>"`, ≤ 12 chars, unique per category.

**Numbers (prototype, seed 2026).**
- **381 presets** (64 library + 16 acid per family, Fx 64) = **45,720 B flash** (+1.4 % of ≈ 3.34 MB; 500 presets ≈ +60 KB, +1.8 %); generation + render + analysis of all 381 ≈ 3 s on the host.
- Loudness outliers at "trim > ±12 dB": 33/381 (8.7 %) today, every one a drive-heavy 303 stab or Wasp — i.e. the makeup bug (§5.5); **0/381 once the makeup is fixed**. Trim range −7.1..+11.9 dB, baked into `level` (clamp 0.05..1.0). Within-archetype spread: pads 0.9–2.2 dB, leads 1.5–6.6, plucks/stabs 6.7–8.6, subs 8.6 → the trim is what makes hundreds usable.
- Index safety: layout `[hand lib][hand 303][gen lib][gen 303]`, add `uint8_t family` after `polyphony` (fits the padding, `sizeof` stays 120); tool emits per-family index lists so `libraryPresetCount / isAcidPreset` and the bag work on family ordinals; saved `lp.track.N.preset` indices stay valid.
- Bag: replace `order[16]` by a 128-bit `used` mask + n/left/last/base = **20 B, RAM unchanged**; a 256-bit mask costs +16 B × 16 tracks = +256 B of `bootBlock` heap (13.1 KB free). Remove the `n > 16` cap (generator.cpp:700). Zero-RAM alternative: stratified bag over ≤ 16 archetypes, random member inside (2 h, risks same-archetype back-to-back across refills).
- Per-generation spread (468 variants of the 78 presets, 6 draws each): today's ±8 % on 4 fields = mean |ΔL| 0.10 dB, inaudible. **Medium** (cutoff ±0.5 oct, decay/release ±25 %, detune ±0.05 st, res ±0.1, fEnv ±0.5 oct, drive ±0.1, LFO rate ±20 %) = mean 0.99 dB, 3.0 % move > 3 dB, 0 % change brightness class → recommended default. Wide = 19.2 % > 3 dB, 2.4 % brightness changes → too much unless gated by the trim. Narrow acid drive to 0–0.5 once the makeup is fixed.
- CPU: 0 per sample (same 16 kernels; LFO/env per 32-sample block). Hand banks can stay (9.4 KB) or become rows 0..N of the generated table.

**QA.** `tests/run_presets_qa.sh` renders every preset through `SynthVoice` with fixed phrases (Lead/Fx/Bass 16 eighths A3–A4, Chord 4 half-note triads, Pad one chord per bar; 125 BPM, vel 100, track-scaled), measures RMS/peak/crest/centroid/K-weighted momentary, rejects silent (< −60 dBFS), hot (peak > 1.2), trim beyond ±12 dB, centroid < 150 Hz (non-bass) or > 3 kHz, computes the trim to the per-category target (hand-library median momentary: Chord −27.9, Lead −23.3, Pad −23.9, Bass −23.6, Fx −28.9 dB track-scaled, after the makeup fix), and a second pass asserts every preset within ±1 dB. Human pass: a `preset <cat> <idx>` audition command, 3 s each ≈ 20 min per 381. Tests to update: `make_patch_spread_is_deterministic_and_within_8_percent`, bank-count checks (≥ 8 keeps holding), `level ≤ 1`.

**Effort: ≈ 20–25 h** (tool 6–8, firmware 4–6, QA harness 4–5, audition + SPEC/tests 4–6). **Unverified**: all numbers are voice-level host renders at velocity 100 without the strip (volume, limiter 0.95, echo/reverb); no device A/B yet.

---

## 5. MiniAcid as the acid synth [303]

### 5.1 What MiniAcid's TB-303 is

MIT licence (`miniacid/LICENSE`, © 2025 Hector Urtubia). Parameters (`src/dsp/mini_tb303.cpp:186-194`): Cutoff 60–2500 Hz, Resonance 0.05–0.85, EnvAmount 0–2000 Hz, EnvDecay 20–2200 ms, Oscillator {saw, sqr, supr}, FilterType {lp, bp, hp}, MainVolume 0–1; plus Delay and Tube-distortion toggles (MANUAL.md:157-190). **There is no chorus and no "shape"** in this checkout (`grep -rni chorus src/dsp MANUAL.md` → nothing); the web build at miniacid.mrbook.org may differ — **unverified**. Per-sample `float process()` with no block API; `startNote(hz, accent, slide)`: accent only sets the filter env to 2.0, never the level; slide is a fixed Hz one-pole (≈ 209 ms to 99 %); **no amplitude envelope** (a released note rings at full level for 2 × decay, 840 ms at default, then hard-cuts); naive saw/square (alias at 22050 Hz); "supersaw" = 7 naive saws. Filter: Chamberlin SVF with `sinf` per sample, `tanhf(1.3·bp)` per sample (the squelch), states clamped ±50; env decay coefficient `expf` per sample; filter allocated with `std::make_unique` (heap + virtual). Engine: 2 voices, `TempoDelay` = 1 s `std::vector<float>` = **88 KB per voice**.

### 5.2 CPU / RAM / flash

| | Mac (clang -O2, ns/sample) | S3 extrapolation | 8 acid voices × 256 samples |
|---|---|---|---|
| `TB303Voice` as-is (saw/sqr/supersaw) | 25.1 / 21.9 / 21.7 | ≈ **4.6 µs/sample** (ratio 850 ns / 4.6 ns ≈ 185×; cross-check: `tanhf` alone is 1,630 ns on the S3, SPEC §1; `sinf`/`expf` on the S3 **unmeasured**) | ≈ **9.4 ms of the 11.6 ms block** on top of today's 5.6–6.0 ms → **not feasible** |
| `lp::SynthVoice` today | 4.1–5.2 (303 Saw 4.6) | ≈ 0.85 µs | — |
| Hoisted 303 kernel (coefficients + decay per 32 samples, cubic clip instead of `tanhf`, naive saw) | 9.8 | ≈ 0.5–1.8 µs | ≈ 1–3.7 ms/block → feasible |

RAM: `sizeof(TB303Voice)` 472 B on the host (392 B of it `Parameter[7]`) + 24 B heap filter ≈ 322 B on the S3; DSP state proper ≈ 60 B; `sizeof(lp::SynthVoice)` 336 B. The 88 KB `TempoDelay` cannot exist in 13.1 KB of free heap; our echo bus already exists (`config.h:56`). Flash: any variant below adds 2–6 KB to 774 KB (23 %).

### 5.3 Parameter → knob mapping (including "chorus" and "shape")

| MiniAcid | meehaw equivalent | Note |
|---|---|---|
| Cutoff 60–2500 Hz | **CUT** (`types.h:113`, 80 Hz–12 kHz) | existing |
| Resonance 0.05–0.85 (Q ≈ 1.1–4.4) | **RES** (TPT Q 0.5–8, `kMaxResonance` 0.985) | existing |
| EnvAmount 0–2000 Hz | **ENV** in octaves (2000 Hz ≈ +2.3 oct at a 500 Hz base) | existing; Hz vs octave is an equivalent character |
| EnvDecay 20–2200 ms | **DEC** (10 ms–2 s) | existing |
| Oscillator saw / sqr / supr | **WAV** SAW / SQR / FAT | existing |
| FilterType lp / bp / hp | `patch.filterMode` | no knob today |
| TubeDistortion toggle | **DRV** + strip distortion (SPEC §9.17a) | existing |
| TempoDelay | **PEcho** bus | existing; MiniAcid's own delay is impossible (88 KB/voice) |
| MainVolume | volume | existing |
| accent (`startNote` accent → env 2.0) | **new ACC knob**: vel ≥ 100 → filter env ×2 (optionally +3 dB) | `acidLine` already writes 20 % accents at vel 100–127 (`variants.cpp:508-509`) |
| slide (fixed 209 ms Hz glide) | legato = `patch.glide > 0 && polyphony == 1 && note held` (`engine.cpp:440-442`) | existing; `acidLine` overlaps slides by 12 ticks (:510) |
| squelch (`tanhf` inside the filter loop) | **new SAT knob** (0 = today's clean TPT SVF) | the one thing `SynthVoice` lacks |
| **"shape"** (not a MiniAcid parameter) | → the oscillator option (WAV; supersaw ≈ FAT) or the filter type | **unverified** what the user means |
| **"chorus"** (not a MiniAcid parameter) | **new work**: one 10 ms modulated line = 441 int16 = 882 B per track; 8 acid-capable tracks ≈ 7 KB static of the 76 KB headroom — better as one bus per set (2 × ≈ 1 KB) | hours not estimated by any report |

### 5.4 Integration plan (recommendation: 303 *character* inside `SynthVoice`)

**Option B (recommended), ≈ 1 day (8 h).** Add (a) saturation inside the TPT SVF loop and (b) the accent rule as kernel features: one more bit in `kKernels` (16 → 32 instances, `synth_voice.cpp:143-148`, ≈ +5–6 KB flash) or a runtime flag (≈ 0 KB, +1 compare per sample). Cost: +8 B per voice (32 voices = 256 B); +≈ 0.1 µs per acid voice-sample (worst case 18 voices → +0.46 ms of the 11.6 ms block). Two new knobs **SAT** and **ACC** on the SYNTH page's free keys (6,2)/(6,3) and (7,2)/(7,3) (KEYMAP.md:68-82); drawing 8 knobs on the 240 px screen needs `kKnobPitch` 40 → 30, `kKnobX0` 20 → 15, `kKnobR` 14 → 12 (`ui/ui.h:38, 52-54`). Session keys `lp.track.N.*`, KEYMAP and SPEC §9.17 need the two params. Keeps 4-voice Chord/Pad stabs (`acidStabs` writes 2–3-note stabs, `variants.cpp:537-546`) and the existing legato slide; set files unchanged. Risk: saturation inside the loop changes effective resonance near `kMaxResonance` 0.985 — `knob_res_env_decay_drive_reach_voice` must still pass.

**Option C, 2–3 days:** a separate mono `AcidVoice` porting MiniAcid's algorithm (Chamberlin + saturation, Hz env, Hz slide) with hoisted coefficients, band-limited oscillators and an added ADSR, in a union with `SynthVoice` (≈ 80 B state, +4–6 KB flash, 0.5–1.8 µs/sample). Closest to MiniAcid's sound on Lead/Bass, but Chord/Pad would lose polyphony and the Chamberlin form is unstable above fs/6 (3.7 kHz) without the tanh/clamps.

**Option D, rejected:** link `TB303Voice`/effects as-is — 9.4 ms/block, heap + virtual (breaks the engine's no-heap/no-virtual contract), 88 KB delay, aliasing oscillators, 840 ms constant-level tails.

### 5.5 The acid level bug: cause and exact fix

**Cause.** The voice drive makeup `driveMakeup(d) = 1/(0.35 + 0.65·d)` (`dsp/synth_voice.cpp:30`, same formula in `clip.h:15`, stated intent "loudness stays roughly constant", clip.h:14) **over-attenuates**: on a full-scale saw, drive knob 0.3 (d 3.1) → **−3.9 dB** RMS, 0.6 → **−7.4 dB**, 1.0 → **−10.6 dB** [303]; [pre]'s sweep on SawLead agrees: −18.5 → −23.3 → −26.6 → −29.8 dBFS (knob 0 → 0.3 → 0.6 → 1.0), because the filter removes most harmonics the clipper adds (pre-makeup RMS rises only +3.6 dB from d 1 → 8 while the makeup drops 14.9 dB). Acid mode is the only place drive is routinely high: `pushKnobs` draws drive 0–0.6 (`gen/generator.cpp:737-749`, mean 0.34 ≈ −5 dB) and the '303' families carry drive 0.05–0.55 (`presets.cpp:57-90, 131-164, 205-241, 282-315`).

Measured gap vs the library presets (engine at 120 BPM, master RMS over 2 bars, same 16-step line) [303]: Lead with 303 Saw + `pushKnobs` ranges, 16 seeds **−3.6 dB mean (min −9.2)**; the same knobs with drive forced to 0: **−0.1 dB** (cutoff floor raised: no change) → drive is the cause. Bass −4.8 dB, **Chord −12.1 dB**, Pad −7.2 dB (vs library pads on the same stabs; −16 dB vs held chords because the acid Pad family *is* a stab track by pattern design). [pre] (voice-only phrase RMS, 125 BPM): Chord −14 dB, Pad −18, Lead −3.5, Bass −5; track-scaled K-weighted: Chord −13.2, Pad −14.0, Lead −2.6, Bass −2.8. Secondary causes: 303 Lead/Bass sustain 0.25–0.40 vs library 0.6–0.9 (−4.1 dB at 0.30) and stab envelopes (decay ≤ 0.25 s, sustain 0–0.2) [pre]; `acidLine` velocities 64–95 → −1.0 dB [303] (velCurve mean 0.572 vs library medians 91–100 → ≤ 1.5 dB net [pre]). Ruled out: voice-count scale (polyphony 2 vs 1 = −0.3 dB), cutoff (flat).

**Fix (verified on a patched copy; `tests/run_synth.sh` 42 tests and `run_engine.sh` 25 tests pass unchanged), 1–2 h:**

1. `dsp/synth_voice.cpp:30` (and `clip.h:15` for consistency):
   `inline float driveMakeup(float d) { return d > 0.f ? 0.577f + 0.423f / (0.5f + 0.5f * d * d) : 1.0f; }`
   — equal-RMS for a saw, max error 0.27 dB over d 1.5–8, keeps today's −0.8 dB bypass step at knob 0+. (`1/sqrt(d)` would still be 4.7 dB off at d 8; [pre]'s `d^-0.25` is flat within ≈ 1 dB but lands acid Lead/Bass +2.5 dB *above* the library median.) Effect: Lead −3.6 → **−1.9 dB** (min −4.4), Bass −4.3 → −3.8, Chord −10.9 → −7.9, Pad −7.2 → −4.4; library presets that use drive (Hoover, SyncLead, StabSaw, Reese …) gain +1 dB.
2. Residual = the 303 families' lower sustain/decay/cutoff → raise `level` in `presets.cpp`: **Lead 0.50 → 0.62 (×1.23), Bass 0.50 → 0.76 (×1.53), Chord 0.33 → 0.50 (×1.53), Pad 0.30 → 0.50 (×1.66)**; all four land at 0.0 dB vs their library reference. Peak check at level ×1.6, vel-127 stabs: master peak 0.056 FS (limiter 0.95, `kMasterTrim` 0.28 leave ample headroom).
3. Optional: `acidLine` velocity floor 64 → 80 (+1 dB).
4. Add a regression test in `test_engine.cpp`: acid family means within ±1.5 dB of the library means.

Cost: 0 B RAM, 0 flash, 0 CPU. Risk: equal-RMS is not equal-loudness (clipped sounds are brighter → drive reads slightly louder); the library balance shifts +1 dB and wants a listening pass; the Pad acid family stays ≈ 13 dB below held pads unless it gets a sustained variant (open question).

---

## 6. Licensing rules we will follow

1. **May go into the repo / shared card image** (licence name recorded next to the files in a manifest, e.g. `sdcard/lp/LICENSES.md`, with source URL, licence, date):
   - **MIT**: ldrolez free-midi-chords ("All the MIDI files are licensed under the MIT license"); MiniAcid code (MIT) if any of its algorithm is ported — keep the copyright notice.
   - **CC0 / public domain**: Freesound CC0 sounds, LJ Speech ("in the public domain"), OpenEWLD scores ("only public domain scores").
   - **CC BY 4.0 with an attribution file**: PDMX, VocalSet, vocadito, LibriSpeech, Lakh *compilation* — **but** Lakh/POP909 content derives from copyrighted songs, so clips extracted from them stay card-only.
   - **Our own output**: generated clips (D), generated presets, transposition *tooling*.
2. **Card only, never leaves the card or this Mac** (no licence file found anywhere; assumed private use): everything derived from the commercial packs — MIDI packs BGK, PTS, DHS, MRH, NJH, DPH, Synth-DM …, vocal packs GHUAC, 91UKG, PHV, SHV, BPV, ASV, OSH — including transposed (2.1 A), pitch-shifted (3.1 B), effect (3.1 C), chopped (3.1 D) and re-imported (3.1 E) derivatives; Looperman downloads ("non-transferable", acapellas "personal, non-commercial use"); the TTS bank rendered with Apple `say` voices (RAP_BANK.md keeps it out of the public repo; Apple's exact terms **unverified**).
3. **CC BY-NC** (Chordonomicon, SheetSage dump): only with the user's explicit acceptance that it blocks any future sale of cards or firmware; default **excluded**.
4. **Excluded entirely**: Hooktheory/TheoryTab (ToS forbids bulk download and redistribution), BitMidi/FreeMidi (no licence), Cymatics/Unison/Ghosthack/Function Loops (ToU quoted in §2.1), Essen (status "unclear"), Tegridy/LA-MIDI/MetaMIDI/GigaMIDI (NC/research-only).
5. **Downloads** (Lakh 1.77 GB, PDMX 2.4 GB, Chordonomicon 264 MB, LJ Speech 2.6 GB, VocalSet 2.1 GB, LibriSpeech 6.3 GB, Freesound originals via OAuth2) happen only after the user approves each one; none were fetched during this study.

---

## 7. Proposed order of work

| Step | Work | Hours | Result |
|---|---|---|---|
| 0 | User answers the questions below (at least Q1, Q7, Q12, Q15, Q17) | — | unblocks 1–7 |
| 1 | **Acid level fix**: equal-RMS `driveMakeup`, four `level` bumps, optional velocity floor, regression test, device listening pass | 1–2 | acid tracks within ≈ −2 dB (Lead) / 0 dB (after bumps) of the library |
| 2 | **Melodic M1**: audition 20 ldrolez derived clips (1 h); transposition tool + run (3–6 h); `prepare_content.py --with-midi` at the capped counts (1.5 h); shard, index, sync ≈ 18 min | 6–9 | Chord ≈ 490, Lead ≈ 270, Pad ≈ 200 per key |
| 3 | **Vocals V1**: import the 202 skipped shots (1 h); listen to the scratch rubberband renders, then transpose GHUAC ±1..3 st (3 h); 4 new voices + lexicon (3–5 h); `make_rap.py` at ≈ 150/key (1 h) | 8–10 | ≈ 250–350 vocals per key, no empty key |
| 4 | **303 character in `SynthVoice`**: SAT/ACC kernel features, two knobs, UI geometry, session keys, KEYMAP/SPEC §9.17, tests | ≈ 8 | MiniAcid squelch + accent on every acid voice |
| 5 | **Preset bank generator**: `gen_presets.py`, `presets_gen.hpp`, family byte, bitmask bag, cap removal, medium spread, QA harness, audition | 20–25 | 381–500 presets within ±1 dB |
| 6 | **Clip generator (M2)**: `make_clips.py` + QA gates + listening rounds; fill Lead/Pad to ≥ 300 | 18–27 | Lead ≈ 420, Pad ≈ 320 per key |
| 7 | **Vocals V2**: chop the owned vocal loops into 2/4-bar phrases | 4–6 | ≈ 500–600 vocals per key |
| opt | Per-set chorus bus (2 × ≈ 1 KB) for the acid voice — **no report estimated the hours**; Chordonomicon render (6–8 h) only if NC is accepted; OpenEWLD (4–6 h); Freesound CC0 (6–10 h); LJ Speech robot bank (3–4 h) | — | — |

**Core total ≈ 65–87 h.** Steps 1 and 2 are the quick wins (≈ 7–11 h) and change no firmware beyond one formula and four numbers.

### Open questions the user must answer

**Licensing / scope**
1. Is CC BY-NC material (Chordonomicon, SheetSage) acceptable in a private, non-commercial setup, knowing it blocks any future sale? Default here: no.
2. Confirm the commercial MIDI and vocal packs (BGK, PTS, DHS, MRH, NJH, DPH, Synth-DM; GHUAC, 91UKG, PHV, SHV, BPV, ASV, OSH) are private-card-only — no licence file exists for any of them.
3. Which downloads are approved (Lakh 1.77 GB, PDMX 2.4 GB, Chordonomicon 264 MB, LJ Speech 2.6 GB, VocalSet 2.1 GB, LibriSpeech 6.3 GB, Freesound via OAuth2)?

**Melodic clips**
4. Are ldrolez's derived leads/pads musically acceptable? Nobody has heard them; a 20-clip audition precedes any bulk import.
5. Re-label transposed clips by content where the diatonic set is unique (16–22 % of clips)? Tag or drop the 33 % chromatic Chord clips?
6. Is the 4-bar hard cap (`kMaxPatternBeats = 16`) fine for all new material, or should 8-bar sources be split into _A/_B halves?
7. Card filesystem: FAT32 with 32 KB clusters (≈ 350 MB for 10,800 clips) or smaller? `diskutil info` on the card.
8. Where is the upstream `__<hash>` computed (docs/LOOPS.md:46 names it; no tool in either repo produces it)? Is an 8-hex sha1 of the note content acceptable?
9. Should generated/transposed files carry a `GEN`/`TR` token so they can be pruned or ratio-limited? The picker weighs files uniformly (300 generated vs 30 library = 90 % generated picks).
10. Generated clips only into `<Root>min/`, or also the relative-major folder (doubles the pool, mislabels the file)?
11. Add swing/humanisation (±3–12 ticks) to the generator, given the editor and `mutate` work on 12-tick units?
12. Should the SMF parser read time signatures before importing 6/8 or 3/4 material (Nottingham, Essen, PDMX)?

**Vocals**
13. Target 300 or 500 vocals per key (610 MB vs 1.0 GB; both under the 768 cap)?
14. Should NA stay count-weighted against the key folder, or get a per-root cap/weight in the firmware before NA grows?
15. Rubberband ±2/±3 st renders in the scratch `rb/` folder need a listening pass before step 3.
16. Raise RAP_BANK.md's 70 MB / 40-files-per-key budget, and which tokens for pitch-shifted and effect files?

**MiniAcid / presets**
17. What do "chorus" and "shape" mean? Neither exists in the MiniAcid checkout; closest mappings are the oscillator/filter type (shape) and a new per-set chorus bus.
18. Equal-RMS (proposed) or equal-loudness as the drive makeup target; re-level the library presets that gain 1–5 dB?
19. Should the Pad acid family stay stabs (≈ 13 dB below held pads by design) or get a sustained 303 variant?
20. Accent: filter env ×2 only (MiniAcid's behaviour) or also +3 dB?
21. Fix the 303 Lead/Bass residual by `level` (proposed ×1.23 / ×1.53) or by raising sustain from 0.25–0.40?
22. Generated bank: replace the hand presets (rows 0..N) or append with a `family` byte? Per-category loudness target = hand-library median, or pads deliberately a few dB lower? Does `test_synth.cpp:80`'s `level ≤ 1` cap stay?
23. Are more LFO destinations or a second envelope wanted for "varied synths"? The archetype plan stays inside the current voice (one LFO, no PWM/FM).
24. The S3 cost of `sinf`/`expf` is unmeasured (only `tanhf` = 1,630 ns is); it matters only if option 5.4 C/D is reconsidered.
