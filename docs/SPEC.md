# meehaw live performer — firmware specification v0.2 (2026-10-15)

Supersedes LAYOUT_PROPOSAL.md where they differ. Target: M5Stack Cardputer ADV. Source of truth for
the implementation; every section number is referenced from code comments.

## 1. Measured platform facts (probe sketch, 2026-10-15)

| Item | Value |
|---|---|
| Board | ESP32-S3FN8, board id 24 (CardputerADV), BMI270 IMU (type 6), 29.8 GB SDHC card |
| Heap after begin + display + speaker + SD | 254 KB free, 209 KB largest block (SD.begin costs 30 KB) |
| DRAM total | 327 KB; static globals count against it |
| Sampler voice (int16 or µ-law, linear interp) | 200 ns/sample → 16 voices = 7% of one core at 22050 Hz |
| Synth voice (wavetable osc + TPT SVF + env) | 220 ns/sample → 32 voices = 15.5% |
| SVF alone | 133 ns; int16 delay line 95 ns; µ-law encode 250 ns |
| tanhf | 1630 ns (never per sample); rational clip 436 ns; polynomial clip ≈ 40 ns |
| Display fillRect 24×24 | 250 µs → full 16-square redraw ≈ 4 ms |
| SD | init 47 ms; directory listing of ~100 entries expected ≈ 50 ms |
| IMU at rest, screen up | accel ≈ (0.01, 0.01, +0.99) g; x/y orientation relative to the keyboard still to be confirmed by tilting |
| Keyboard | TCA8418 FIFO pops ONE event per `Keyboard.updateKeyList()`; call it up to 8 times per loop |
| Build | `esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled`, esp32 core 3.3.12, M5Cardputer 1.1.1, M5Unified 0.2.26, M5GFX 0.2.33 |

## 2. Fixed decisions

- Audio 22050 Hz mono int16, 256-sample blocks, two ping-pong buffers, `Speaker.playRaw` on channel 0.
  Render task pinned to core 0, priority 5. M5 speaker task pinned to core 1, priority 3. Arduino
  loop (UI, keyboard, SD loading, generation) on core 1.
- Track order per set (Reaper order): 0 Chord, 1 Lead, 2 Pad, 3 Bass, 4 Fx, 5 Percussion, 6 Vocal, 7 Beat.
  Set 1 = tracks 0–7, Set 2 = tracks 8–15. All 16 may sound at once.
- Melodic tracks (Chord, Lead, Pad, Bass) are synthesised. Fx is the performance (glide) synth. Beat and
  Percussion play four one-shots each from a RAM pool. Vocal streams one phrase file per set from SD.
- Key: natural minor only, 12 roots cycled in circle-of-fifths order A E B F# C# G# D# A# F C G D,
  default A. Key change affects future generation only. Files accepted when their label has the same
  pitch classes as the key (so A minor accepts `Amin__` and `C__`) or is `NA__`. Never transposed.
- Launch quantisation: bar. Tempo 40–240 BPM, default 120, ±1 per press, hold repeats.
- Loading a set resets all effects of that set to defaults. Every track and the master have a limiter.
- Sample rate, block size and the memory map are fixed constants in `src/core/config.h`.

## 3. Keyboard map (physical 4 × 14 matrix, coordinates x 0–13, y 0–3)

All keys are read as raw matrix positions via `Keyboard.keyList()`, so the printed modifier legends
do not apply. "SHIFT" below is the physical space key (13,3).

```
y0: `  1  2  3  4  5  6  7  8  9  0  -  =  del      → set1: CHD LED PAD BAS FX PRC VOC BEAT | pat1 pat2 pat3 pat4 | snare rush | kick cut
y1: tab q  w  e  r  t  y  u  i  o  p  [  ]  \        → set2: same
y2: fn sft a  s  d  f  g  h  j  k  l  ;  '  ent      → vol+ rev+ echo+ dist+ filt+ len+ tempo+ key+ | gyro fx | genre | save S1 | load S1 | gen pattern (sel) | GENERATE S1
y3: ctl opt alt z x  c  v  b  n  m  ,  .  /  spc     → vol- rev- echo- dist- filt- len- tempo- key- | edit | perform | save S2 | load S2 | gen sound (sel) | SHIFT
```

Semantics:
- Track key tap: toggle the track; start/stop at the next bar. SHIFT + track key: select the track
  (no launch). Selection is global (one selected track, default Set 1 Beat).
- pat1–pat4 (row of set N): choose the variant of the selected track if it belongs to set N, else of
  set N's Beat track. Switch applies at the next bar.
- snare rush (momentary, Beat of that set): while held, the Beat track plays variant 4 cropped to
  one beat and looped; release returns to the current variant at the correct position.
- kick cut (toggle, Beat of that set): note 36 of the Beat track is muted; other voices keep playing.
- vol/rev/echo/dist/filt/len/tempo/key ± : act on the selected track (tempo, key are global).
  Press = one step; hold repeats after 350 ms every 80 ms. Each opens its full-screen page for 1.2 s.
  SHIFT + param±: ramp from the current value to max/min over `rampBars` bars (default 4); releasing
  SHIFT snaps back to the value before the ramp. SHIFT + len±: set `rampBars` (1, 2, 4, 8, 16).
- len±: clip length of the selected track, power of two in 1 … 16 beats (beyond the pattern plays
  silence, like Playtime), applied at the next bar.
- GENERATE S1 (enter): all patterns and sounds of Set 1. SHIFT + enter: Set 2. gen pattern (sel):
  new pattern (all four variants) for the selected track, sound unchanged. gen sound (sel): new sound
  (synth preset or drum/percussion kit, or another vocal file), pattern unchanged.
- genre: rotate through `lp/packs/*` folders. save S1/S2: write a set file into the current genre.
  load S1/S2: load a random saved set of the current genre matching set index and key.
- gyro fx (press-and-hold toggle, 400 ms): global; forward tilt on the pitch axis → high-pass up to
  max, backward → low-pass, flat → bypass; left lean → reverb send 0→max, right lean → echo send.
  Neutral is captured at toggle-on. Axis signs and mapping are in `lp/config.txt` (`imu.*`) until
  confirmed on the device.
- edit: pattern editor page for the selected track. perform: glide synth page (Fx track). Both exit
  with the same key or `` ` ``.
- BtnA (side button): master play/stop (stops all tracks at once, restart resumes queued state).
- SHIFT + kick cut: master output volume (the headphone level, 0–255, default 200, `volume=` in `lp/config.txt`):
  set 1's kick cut key = up, set 2's = down, 16 per step, hold repeats, clamped; opens the OUTPUT page (red
  #FF3B3B, value in percent) like the other parameters and the footer shows `OUT 78%` for 3 s (whole, or not at
  all when the line before the battery readout has no room for it, see §4). Without SHIFT the keys keep toggling
  the kick cut.

- SHIFT + edit `(8,3)`: LOOP MODE of the selected track on / off (§9.19, any category, either set). ON picks a
  loop of the track's category from the card's `loops/<folder>` (one generator job; the square blinks while the pick
  is in flight) and streams it at the bar line, status `LOOP ON S1 BEAT`; OFF returns the parked pattern and sound
  at the bar, `LOOP OFF S1 BEAT`. The main page marks loop tracks with an `L` (as acid's `A`); the first loop in use
  pulls the tempo to 125 BPM (`LOOP ON - 125 BPM`) unless a tempo drift runs or the boot restore owns the card (the
  session brings back its own `lp.bpm`; a user set load is not exempt, §9.19d). Refusals: `LOOP SLOTS FULL (2)`,
  `NO LOOP: DRUM`, `LOOP: BUSY`, `LOOP: LOW RAM`. A loop track refuses the perform page (`PERFORM: LOOP TRACK`) and
  the acid toggle (`ACID: LOOP TRACK`); its editor page is a read-only loop view (name, bars, variant).

## 4. Screens (240 × 135, black background, neon palette)

- Splash: "meehaw" large, "live performer" small, 1.5 s, neon green on black.
- Main: two rows of eight squares (26 px, 4 px gap, rows at y 24 and y 76), category colours from
  Reaper (Chord orange #FF8C00, Lead yellow #FFD700, Pad purple #9B59FF, Bass brown #B5651D,
  Fx pink #FF4FA3, Percussion green #2ECC71, Vocal cyan #00E5FF, Beat grey #A0A0A0). Filled =
  playing, outline = loaded/idle, blinking outline = queued, dim outline = empty. Selected track: a
  square of 30% size inside, black on a filled square, white on an outline. A 9 px footer shows key,
  BPM, genre and the gyro state in dim grey.
- Parameter page: whole screen, parameter name small, value huge (font size 4+), bar below, each
  parameter its own neon colour (vol green, reverb cyan, echo magenta, dist orange, filter yellow,
  length violet, tempo white, key blue, genre pink, ramp bars teal). Auto-returns after 1.2 s idle.
- Status overlay: generation progress ("GEN S1 3/8"), save/load results, errors, 1 s.
- Editor page: drums = 4 rows (kick snare hh oh) × 16 steps for the current bar, pages per bar,
  cursor with arrows (keys ; , . / as arrows), enter toggles, velocity via vol±. Melodic = 16 steps
  × 7 scale degrees of the current key (one octave per page, len± changes octave), enter toggles a
  note at the cursor, dist± changes gate. Edits go to the track's original pattern; variants rebuild.
- Perform page: four rows of ten keys form a grid in fourths (row interval 5 semitones), columns
  walk the natural-minor scale degrees of the current key, default octave 3; `-`/`=` octave; key
  presses quantised to the nearest 1/16; SHIFT = momentary chromatic; `del` = record toggle with a
  one-bar count-in, loop closes at the bar line, overdub on next press, hold 700 ms = clear. The
  recorded loop becomes the Fx track's pattern and is started/stopped with the Fx track key.
- Battery readout: the main page footer shows the battery right-aligned at the right edge ("87%", "CHG 87%"
  while charging, "--" when the level is unknown, charging or not) in the footer's dim grey, red #FF3B3B below
  15 % unless charging; the sketch polls M5.Power every 5 s (and once after init), averaging several reads and
  holding a 1-point change until the next poll repeats it, the desktop reports a full one. The readout is
  repainted only when its own text changes (a changed left text leaves it alone). The left text is fitted to the
  room before it by whole segments: the double-space separators shrink to single ones, then the OUT note is
  dropped, then GYRO (a note shows whole or not at all, never as a partial number), and only then is the rest
  cut. On the Cardputer M5Unified reports no charge state (always unknown), so CHG never shows there and the
  low-battery red stays on while the unit is plugged in.

## 5. Audio engine

- Clock: 96 PPQN. `samplesPerTick = 22050 × 60 / (bpm × 96)` as 16.16 fixed point; the render loop
  advances ticks per sample and fires track events on exact ticks. Global tick counter → beat, bar.
- Tracks hold `Pattern` buffers of packed 32-bit notes: start tick 12 bits, length 8 bits in units of
  12 ticks (1/32 note), pitch 7 bits, velocity 5 bits. Max 192 notes per pattern. Per track: `source`
  (as loaded or edited), `active` (current variant) and `next` (pending variant/length change, swapped
  at the bar line). 16 × 3 × 768 B = 37 KB.
- Variants are rebuilt from `source` + the stored seed whenever selected (microseconds), so only
  the active one is resident. Variant rules (ported from pattern_variants.lua):
  Chord: original, chord_variation, crop 2 beats, crop ½ beat. Lead/Bass: original, lead_variation,
  crop 2, crop ½. Pad: original, chord_variation, two voices (lowest+highest), monophonic (highest).
  Fx: original, mutate 0.55, generated 2-beat, generated ½-beat. Beat: Kick {36}; Kick+Hats {36,38,39};
  +Snare {36–39}; Full (+one accent pitch 40–58 folded to voice D). Percussion: Sparse (every 4th hit,
  ×0.55), Light (odd hits, ×0.70), Driving (3 of 4, ×0.85), Full. Snare rush = crop(Full, 1 beat).
  Vocal: full phrase, alternate file, first 2 beats looped, first 1 beat looped.
- Voices: 8 sample voices per set (kick, snare, hh, oh, perc A–D), choke oh by hh; synth voices per
  set: chord 4, pad 4, lead 2, bass 2, fx 4 (glide lanes). Each synth voice: 2 wavetable oscillators
  (sine/tri/saw/square/fat-saw tables, int16; four pitch-ranged sets, §9.15), TPT SVF (LP/BP/HP), amp ADSR, filter
  env, optional LFO, glide. Presets per category in `src/dsp/presets.cpp` (8+ each), picked by a
  shuffle bag with ±8% parameter spread.
- Sample pool: static `uint8_t pool[2][56 KB]`, µ-law 8-bit at 22050 Hz, filled on generate by the
  loader for the inactive set. Caps: kick 400 ms, snare 350, hh 150, oh 400, perc 300 each. Slot
  table per set with offset/length. One-shots are converted µ-law on load (WAV 16-bit from SD).
- Vocal: per set a `VocalStream` with a 2 × 4096-byte ring of int16 filled by the loop task from the
  open WAV (22050 mono); underrun = silence, counted. Retriggers at pattern start; variant 3/4 loop
  the first 2 / 1 beats by re-seeking.
- Per-track strip: volume (0–1, default 0.8), distortion (0–1 → drive 1–8 into polynomial clip),
  filter (bipolar −1 LP … 0 bypass … +1 HP, cutoff 80 Hz–12 kHz log), echo send, reverb send
  (0–1), limiter (peak > 0.95: 1 ms attack, 80 ms release). Kick ducking per set: −4.5 dB, 3 ms
  attack, 120 ms release, triggered by the set's Beat note 36 (not when kick cut), applied to the
  other seven tracks; default on.
- Buses: echo = int16 delay line of 16384 samples (743 ms; divisions 1/16 1/8 1/8d 1/4 1/4d 1/2
  synced to tempo, a division longer than the line folds down by halving), feedback 0.45, one-pole
  damping; reverb = MiniAcid DrumReverb topology with combs scaled ×2 (8 KB) so pads do not sound
  like a drum room; master = gyro filter (SVF) → limiter → int16. Buses are mono.
- Memory map (all static, allocated at boot, never freed): sample pools 2 × 56 KB = 112 KB, echo
  32 KB, reverb 8 KB, patterns 37 KB, vocal rings 16 KB, audio buffers 1 KB, command queue 2 KB,
  UI scratch ≤ 8 KB, wavetables in flash (const, 29 KB, §9.15). Total ≈ 216 KB against 254 KB measured free after
  init, leaving ≈ 35 KB for stacks of the render task (8 KB) and library/runtime needs. If the first
  build shows less headroom the levers are, in order: perc caps 300 → 250 ms, echo 32 → 24 KB,
  vocal rings 16 → 8 KB.

## 6. Generation and library

- SD layout:
  ```
  /lp/midi/<Category>/<Key>/<Key>__<Category>__<name>__<hash>.mid   Reaper names; Beat not drum
  /lp/packs/<Genre>/drum/{kick,snare,clap,hh,oh,cymbal,perc}/*.wav   22050 mono 16-bit, role-balanced
  /lp/packs/<Genre>/vocal/<Key>/*.wav                                full phrases, streamed
  /lp/packs/<Genre>/sets/<name>.lpset + sets/assets/*.mid            saved sets (Reaper-importable)
  /lp/packs/<Genre>/{fx,inst}/…                                      unused by the firmware for now
  /lp/config.txt                                                     key=value settings
  ```
- Library index: per (category, key) folder the entry count is cached on first use; picks are made
  with a shuffle bag over ordinals (history of the last 8 picks per track kind avoids repeats). Walking
  to ordinal N re-lists the folder (≈ 0.5 ms per entry). Key folders for a key: its minor label and the
  relative major label plus NA.
- Beat kit: kick preferring files whose key root equals the global root (`A#__…Kick`), else any;
  snare from snare else clap; hh from hh; oh from oh else hh. Percussion kit: four distinct files
  from perc + clap + cymbal pools. Percussion patterns fold their pitches onto four voices: the four
  most frequent pitches map to A–D, remaining pitches to (pitch − 36) mod 4.
- Vocal: file from the key folders (or NA), name must have no `loop` token; variant rules above.
- Generate set N: for each track in order, pick pattern + sound, load into `next` buffers and the
  inactive pool half; publish; the engine swaps every track of the set at the next bar line. Audio
  never waits on SD. Progress is shown on screen. Genre switch re-indexes packs lazily.
- Saved set file (`.lpset`, Java-properties text, Reaper set_presets.lua contract):
  ```
  schemaVersion=1
  setIndex=0
  keyRoot=9
  keyName=A minor
  genre=House
  track.0.category=CHORD
  track.0.clipType=MIDI
  track.0.clip=assets/Amin__Chord__x.mid      (copied beneath sets/assets/, no ..)
  track.0.slot=0                              (active variant)
  track.0.instrument=preset:Chord/PluckWide   (text)
  track.7.beatLayer=3
  track.7.beatKickMuted=false
  lp.bpm=124                                  (firmware-only keys, ignored by Reaper)
  lp.track.0.seed=…  lp.track.0.len=8  lp.track.5.kit=perc/a.wav;perc/b.wav;…
  ```
  Reaper's `recall_random_set` is currently unreachable in the Lua project; wiring it to a window
  action is a one-line change on that side (documented in docs/REAPER_IMPORT.md).

## 7. Firmware architecture

```
firmware/liveplayer/liveplayer.ino        Arduino entry: init, tasks, main loop
firmware/liveplayer/src/core/              config.h (all constants), types.h (Note, Pattern, Track,
                                            Set, Category), rt.h (SPSC command queue, atomics)
firmware/liveplayer/src/dsp/               ulaw, wavetables, synth_voice, sample_voice, svf, clip,
                                            limiter, ducker, echo, reverb, presets, glide (perform)
firmware/liveplayer/src/engine/            clock, sequencer (events, quantised launch, variants,
                                            length), mixer (strips, buses, master), engine (render)
firmware/liveplayer/src/gen/               key, smf (MIDI parser), variants (pattern transforms),
                                            library (SD index, bags), generator, sets (.lpset), packs
firmware/liveplayer/src/io/                fs.h (IFileSystem: SD and POSIX), wav (loader, streamer),
                                            keys (matrix events), imu
firmware/liveplayer/src/ui/                gfx.h (IGfx: M5GFX and SDL), pages (splash, main, param,
                                            editor, perform, status), palette
firmware/desktop/                          SDL2 build of the same engine with a POSIX fs rooted at
                                            ../../sdcard, keyboard mapped to the 4×14 matrix
tests/                                     host unit tests (clang++), one per module
```
Rules: no heap allocation after `setup()` completes; no `std::string`/`std::vector` growth in the
audio path; the audio task reads only published pointers and atomics; all SD I/O on the loop task;
`Keyboard.updateKeyList()` called 8× per loop; UI redraw only on change, at most every 40 ms.

## 8. Phases

1. Walking skeleton: tasks, clock, 16 tracks with built-in synth drum kit and a test pattern, main
   screen squares, key matrix events, splash. Flash and verify.
2. Library + SMF parser + key filter + generator for Beat/Percussion from SD (µ-law pool).
3. Synth voices + presets for Chord/Lead/Pad/Bass; variants; pattern/sound re-roll; param pages.
4. Vocal streaming, Fx glide synth + perform page + looper, editor page.
5. Mixer strips, buses, limiter, ducking, gyro FX, ramps, clip length.
6. Sets save/load, genres, config, desktop SDL build, docs.

## 9. v0.3 amendments (after the four-lens review, 2026-10-15) — these override §2–§7 where they differ

1. **Sample pool.** One pool per set (`pool[2][49 KB]`, both sets always live) plus a 9 KB staging
   buffer. Caps: kick 400 ms, snare 350, hh 120, oh 350, perc 250. The loader decodes ONE one-shot
   into staging, posts `CmdSlotSwap{set, slot, len}`; at the start of its next block the engine fades
   any voice on that slot (64 samples), copies ≤ 8820 B into the slot, bumps `slotAck`; the loader
   waits for the ack before the next slot. Sounds of a set therefore change slot by slot during the
   ≈ 300 ms before the pattern swap; nothing is ever cut. Two SampleVoice instances per slot so a
   retrigger or choke fades the old instance over 64 samples. Gain per slot is computed offline by
   the import script (Reaper drum_balance targets); the loader only applies a 10 ms fade at the cap.
2. **SD budget.** No SD burst on the loop task may exceed 20 ms: generator, kit loader, set
   save/load and listings are resumable state machines (walks ≤ 32 entries per step, copies in
   4 KB chunks, one file open per step). Between steps the loop scans keys, refills both vocal rings
   (whenever ≥ 2 KB is free, before any other SD work) and redraws. Render task waits with
   `vTaskDelay(1)` when the speaker queue is full, never spins. Bar-line work is pointer/atomic/
   memcpy of one pattern (≤ 772 B) only.
3. **Vocal.** Files must carry a BPM token (`124bpm`, `bpm_124`); others are skipped. Natural clip
   length = the largest power-of-two beats (1..16) that fits inside duration × fileBpm / 60 (2 % tolerance),
   REAPER's rule — it sizes the Reaper-compatible `lp.track.6.len` only: the clip the firmware plays is the
   window + pause cycle of §9.16 (which replaces the continuous loop and the len± ×2^k of this item). Playback is
   varispeed: rate = bpmNow / fileBpm clamped 0.5..2.0 (pitch follows tempo, as Reaper's playrate
   range). The producer (loop task) follows the sequencer: the engine publishes `sampleClock`,
   `barSample` (the exact first sample of the current bar), `nextBarSample` and each track's
   `startSample`; every ring write is placed by that bar clock as §9.16 says (cycle phase =
   global bar mod cycle bars, file position = samples into the window × rate) with linear
   interpolation, a 64-sample crossfade at the loop variants' wraps, and a new file is pre-rolled
   behind the old one; the engine reads the ring sequentially and only counts genuine starvation.
   Variants: 1 full, 2 alternate file, 3 loop 2 beats, 4 loop 1 beat — see §9.16 for what each
   variant streams.
4. **Clip length.** Each variant has a natural length (source; crops 2 / 0.5 beats; vocal 2 / 1;
   rush 1). `len = natural × 2^k`, clamped 0.5..16 beats, k reset on variant select / gen pattern /
   load; len± steps k. Position = globalTick mod len (phase-locked); silence beyond the pattern
   only when k > 0. A second tap before the bar line cancels a pending start/stop. Vocal tracks:
   len± steps the pause of §9.16 instead (no ×2^k).
5. **Pattern ownership.** The engine owns `active[track]`; the UI writes `next[track]` under
   `beginNextWrite/endNextWrite` (an atomic lock) and sends `CmdSwapPattern`. The engine applies
   pending swaps at the bar line, or at the first later tick where the lock is free (phase lock
   makes a few-ms-late swap seamless), by copying `next` into `active`. Voices store note-end
   ticks, never pointers. Generate/load fill all eight `next` buffers and send ONE `CmdSwapSet`
   so the whole set switches on the same bar line; stopped tracks adopt silently. While a set
   generates, its pat/len presses are queued and applied after the swap.
6. **Beat.** Note contract: 36 kick, 37 snare, 38 closed hat (chokes 39), 39 open hat, 40–58
   accents → open-hat slot at velocity ×0.8, exempt from the choke. `ensure_beat_core` runs before
   layers (seeded). `active` is always Full; layers are a pitch mask applied at event time
   (Kick {36}, Kick+Hats {36,38,39}, +Snare {36–39}, Full all), switched atomically at the bar;
   kick cut clears bit 36 immediately. Snare rush = crop(Full, 1 beat) with pitch 36 removed; if
   that has no 37, snares on every 1/16 (vel 100/80/90/80). Engages at the next 1/16 after key-down,
   releases at the next 1/16, position resumes phase-locked; on a stopped Beat track it sounds
   while held.
7. **Percussion.** Voice = rank among the four most frequent pitches (count desc, pitch asc), the
   rest `36 + (pitch & 3)`; the fold map is computed at load and stored; layer ordinals count per
   original pitch. Kick keyed selection also accepts `A_Kick`, `F#-Kick`, `Bb Kick` forms.
8. **Sets.** `/lp/packs/<Genre>/presets/<Genre>_<Key>_<NNN>.lpset` with clips in `presets/assets/`
   (Reaper scans `<pack>/presets` non-recursively). Writer: `key=value`, ASCII, forward slashes,
   `clipType=MIDI|WAV`, `clip=assets/<file>`, Vocal as WAV with its BPM-tagged file copied, no
   `instrument` key (Reaper would mangle it), `slot` = variant, `beatLayer` = variant for Beat,
   `beatKickMuted`, firmware-only `lp.track.N.{preset,kit,len,key,seed,bpm}` and `lp.bpm`. Edited
   or recorded patterns are written with the SMF writer as `assets/<Key>__<Cat>__edit_<NNN>.mid`.
   Load = like generate (one set-wide swap), random among sets whose keyRoot matches, play state
   and selection unchanged, effects reset at the swap; `lp.bpm` applied only when nothing plays;
   no match → "NO SET IN Amin".
9. **Key.** Beat and Percussion ignore the key (their files live under `/NA/` on the card; key
   change resets only tonal bags). Each track stores the key it was generated with; variants are
   rebuilt with the track's key. Key and len are excluded from SHIFT ramps.
10. **Perform page.** Grid keys y0 x1–10, y1 x1–10, y2 x2–11, y3 x3–12; row offset 3 scale degrees
    (every cell in key); ` exits; tab starts/stops the Fx loop; del = looper (one-bar count-in,
    close at the following bar line rounded to 1/2/4 bars, overdub on further presses, hold 700 ms
    clears); SHIFT = +1 semitone; notes sound on key-down, recorded timestamps quantised to the
    nearest 1/16; target = Fx track of the selected track's set; a recorded loop sets `userLoop`
    which GENERATE / gen pattern skip ("FX kept") until cleared; gen sound still re-rolls.
11. **Gain staging.** Strip: voices → distortion → filter → ducker → volume → limiter (instant
    attack, 80 ms release) → dry; echo/reverb sends post-fader. Synth voices ×1/polyphony. Master:
    −9 dB trim → gyro SVF → 64-sample lookahead limiter → saturating int16. Latency 23 + 2.9 ms.
    Gyro drives two master sends (not per-track sends) and the master SVF with a ±10° dead band,
    IMU at 50 Hz one-pole smoothed (τ 60 ms), toggle-off slews to bypass over 50 ms. Echo default
    1/8d (config `echo.division`).
12. **Parameters.** Every continuous parameter is a target smoothed by a one-pole (≈ 10 ms) in the
    engine. Steps: vol/rev/echo/dist 0.05, filter 0.1, len ×2/÷2, tempo 1, key one fifth. Ramps run
    engine-side (`CmdRamp{track, param, target, ticks}`), start on key-down, linear over rampBars at
    the start tempo, several may run at once, SHIFT release snaps all back, no hold-repeat under
    SHIFT. Sequencer orders note-offs before note-ons per tick; a note-on for a sounding pitch on
    the same track steals that voice.
13. **Memory map (revised).** pools 98 KB + staging 9 KB, echo 24 KB (12288 samples), reverb 8 KB,
    patterns active+next 16 × 2 × 772 ≈ 25 KB + source 12 KB, vocal rings 16 KB, DSP state ≈ 8 KB,
    file handles ≈ 4 KB, queues 3 KB ⇒ ≈ 207 KB. Boot asserts free ≥ 20 KB and largest ≥ 8 KB after
    task creation, else shows the numbers in red. CPU is re-measured with the real synth voice.
14. **Screens.** pat, rush and cut open a 0.6 s page ("S1 BEAT 3 +SNARE"); kick cut = diagonal
    strike through the Beat square; variant = 1–4 ticks under the selected square; footer shows
    the selected track's clip length and GYRO when active. Parameter and status pages are overlays
    (all main keys stay live); editor and perform are modal. Editor: Percussion rows A–D via the
    fold map, Fx = melodic grid, Vocal = "no pattern" (len± only), tempo± pages bars, arrows ; , . /.

15. **Wavetables.** The band-limited single-cycle tables of §5 come in four pitch-ranged sets, generated by
    `tools/gen_wavetables.py` into `src/dsp/wavetables_data.hpp` (const, flash), each table followed by one guard
    sample so the oscillator reads `t[i + 1]` without a wrap mask:

    | set | partials | length | serves | top partial at the last pitch |
    |---|---|---|---|---|
    | 0 | ≤ 40 | 2048 | MIDI ≤ 60 | 40 × 261.6 Hz = 10.47 kHz (61 would fold: 11.09 kHz) |
    | 1 | ≤ 16 | 512 | MIDI 61..76 | 16 × 659.3 Hz = 10.55 kHz |
    | 2 | ≤ 6 | 256 | MIDI 77..93 | 6 × 1760 Hz = 10.56 kHz |
    | 3 | ≤ 3 | 128 | MIDI ≥ 94 | alias-free up to MIDI 105 (3 × 3520 Hz) |

    Every set is alias-free up to half a semitone above its last pitch, so a fractional (glide) pitch is rounded
    to the nearest integer before the lookup (`wavetables.h waveSetForPitch`). A set with fewer partials keeps
    ≥ 32 samples per period of its top partial (linear-interpolation images ≤ −74 dB re the fundamental), so the
    20 tables take 29 480 B of flash, a working set that stays resident in the ESP32-S3's 32 KB data cache
    (8-way, 32 B lines) while 32 voices stream through their tables; the former 2048-sample tables for every set
    (52–68 KB) could not. The sine is one partial in every set and is stored per set at the set's length.
    Selection is per oscillator and per 32-sample modulation block (`synth_voice.cpp selectTables`): osc 1 at the
    current (glided) pitch, osc 2 at its octave offset plus detune, the sub one octave below, each raised by the
    LFO's pitch excursion while the LFO runs (so a vibrato never flips a set); a silent oscillator keeps its
    table. A downward slide therefore starts on the set of the HIGH pitch and switches on the way (the set of the
    target pitch would fold until the glide gets there); an upward slide switches to duller sets as it climbs.
    When a table changes on a sounding note (glide crossing, legato or stolen note, patch change) the previous
    table is kept for that one block and a crossfade kernel blends to the new one at the same phase (32 extra
    reads once per switch, nothing per sample otherwise), so the two band-limits never step at the kept phase.
    A note-on arriving while such a crossfade runs lets the block finish (≤ 1.4 ms) before its modulation update.
    Measured: host tests `tests/run_dsp_core.sh` (tables, boundaries, keyboard sweep) and `tests/run_synth.sh`
    (slides, switches); the device CPU figure (§9.13, `renderMicrosMax` with 16 tracks across the sets) is taken
    with `tools/bench_wavetables.sh` (sketch `firmware/wtbench`) and recorded in its header comment. First run
    (2026-10-15): 32 voices 522 ns/voice-sample on set 0 only, 585 ns across the four sets (+12 %); one voice
    387 ns (sine) / 832 ns (full Fx preset); the engine with 16 tracks across the sets 9.6 ms mean / 10.5 ms max
    per 256-sample block against the 11.6 ms budget — well above the 220 ns of §1, to be split between the voice
    kernel and this change by a bench build against the previous dsp sources before §9.13 is re-budgeted.
## 10. Measured on the device (first firmware build, 2026-10-15 evening)

| Item | Measured |
|---|---|
| Flash / static RAM | 713 KB flash (21%), 260 KB static (79%); Library, Generator, Sets allocated once at boot (33 KB) |
| Heap after boot and task creation | 22.8 KB free, 10.2 KB largest block (floor: 20 KB / 8 KB) |
| Render, idle (no tracks) | 1.3 ms per 256-sample block (11.6 ms deadline) |
| Render, synthetic worst case (16 tracks, 18 synth voices, filter + distortion + both sends on every track) | 5.6–6.0 ms with set 2 rendered on core 1; 9.2 ms single-core |
| Per-section (sum over both cores, worst case) | synth voices ≈ 4.9 ms, strips ≈ 2.1 ms, buses ≈ 0.6 ms, sample voices ≈ 0.4 ms |
| Synth voice (full preset: 2 osc, sub, noise, drive, SVF, env) | ≈ 850 ns/sample on the S3 (not the 220 ns of the 1-osc probe) |
| Strip with filter + distortion | ≈ 475 ns/sample/track (FPU-bound; float division is a libgcc call, hoisted out of all per-sample loops) |

Architecture notes from bring-up: the engine renders set 1 on core 0 and set 2 on a worker task on core 1
(`g_parallelStart/g_parallelJoin` hooks in engine.h; inline when the hooks are null, e.g. on the desktop);
per-set accumulators are summed before the buses. The per-track limiter works per 16-sample sub-block
(one division per sub-block). Serial bring-up commands exist in the sketch (`k x y`, `kd/ku`, `note`,
`tog`, `gen`, `play`, `demo`, `stat`) and `tools/serial_cmd.py` drives them.

### 10.1 Bring-up with real card content (2026-10-15, later)

| Item | Measured |
|---|---|
| Heap after boot (render stack 3.5 KB, worker 3 KB, echo 10240 samples) | 30.9 KB free, 18.4 KB largest |
| Heap with both sets generated and all 16 tracks playing | 19–21 KB free, 8–10 KB largest |
| Task stacks, minimum free under load | render 2.3 KB of 3.5, worker 1.9 KB of 3, loop 4 KB of 8 |
| Render, 16 real tracks from the card (two vocals streaming) | 5.3–5.4 ms per block, 0 underruns |
| Generate set (8 picks, 8 slot loads, vocal open) | ≈ 8 s wall time while playing, UI responsive |
| Card sync over USB serial (tools/card_sync.py + firmware/cardloader) | 5,757 files / 69 MB in 9 min after sharding folders to ≤ 64 files (FAT create cost: 23 ms in a 64-entry folder, 0.8 s in a 700-entry one) |

Card layout amendment: any content folder with more than 64 files is sharded into `s00…sNN` subfolders
(`tools/shard_folders.py`), each with its own `_index.txt`; the library caches a root with its shards as ONE
tree in one of its 16 slots (per-shard counts, one bag over the tree, kTreeMax = 12 shards of names shorter
than 8 characters; `shard_folders.py` grows the shard size past 768 files, `write_indexes.py` refuses a card
with a 13th shard and the library reports one through `lastError()` / serial `stat`). A pick therefore holds
2–4 slots and a cold tree costs one prepare step per shard; slots that are cheap to rebuild (flat or absent
folders) are evicted before sharded trees, so the 29 trees of a set generate re-stream no shard index on the
next generate (host measure: 70 prepare steps / 117 KB the first time, 10 steps / 5 KB after). sizeof(Library)
is 12.2 KB (cap 13 KB). Known limitation: saving a set while the vocal track plays copies the vocal WAV
(≈ 275 KB) into `presets/assets/` and starved the vocal ring for ≈ 0.5 s in total (12,800 samples);
save between songs, or reduce the copy chunk size in sets.cpp.

### 10.2 Output buffer bug found by ear (2026-10-15)

`Speaker.playRaw` keeps a POINTER to the caller's block and reports consumption through
`setBufferReleaseCallback` (Speaker_Class.hpp: "two buffers alternately are enough only if the next one is
filled after the previous one is released … without it use three buffers in sequence"). The render task used
two buffers and refilled the second one immediately, so in steady state it overwrote a block the speaker
had not consumed yet; the audible effect depended on the slowly drifting phase between render and DMA:
clean for 15–20 s, then omnipresent crackle and gaps. Fixed with three buffers in sequence. Also fixed:
note-end ticks were 16-bit (note-offs broke after 65536 ticks ≈ 5.7 min at 120 BPM); now 32-bit.

### 9.16 Vocal window and pause (user request, 2026-10-15 evening) — replaces the continuous loop of §9.3

Long phrases ("laaa laaa oooh") play fully and then leave a stretch of near-silence, which reads as a
rolling bleep with gaps. New rule for every vocal track:
- The clip is a cycle of `window + pause` bars. Window = 2 bars: the phrase plays from its start for two
  bars at varispeed (a shorter phrase ends early inside the window; a longer one is faded out at the
  window end over 64 samples). Pause = silence, default 1 bar, chosen per vocal track with len± from
  {0, 1, 2, 4} bars (the parameter page shows "PAUSE 1 BAR"); `lp.track.6.pause` in set files.
- Cycle length = (2 + pause) bars, phase-locked like every clip (a 3-bar cycle is allowed): the
  producer places every sample by the engine's bar clock (`status.barSample` / `nextBarSample` /
  `tick`), so position = globalTick mod len whatever bar the track was started on (a start on cycle
  bar 1 begins one bar into the phrase), window ends and cycle restarts always fall on the engine's
  bar lines, and a tempo change (including a SHIFT ramp) only changes the rate: the ring written
  ahead is retimed under the new clock and the phrase keeps its file position at the retime point.
- len± on a vocal publishes the new cycle through the stream switch itself (the switch's
  `CmdSwapPattern` carries (2 + pause) × 384 ticks), so the engine's clip length and the streamed
  cycle change on the same bar; no separate `CmdSetLength`.
- Variants: 1 window + pause (default), 2 alternate file (same scheme), 3 loop the first 2 beats,
  4 loop the first 1 beat (stutters, no pause: len± does nothing there and the length page shows
  "NO PAUSE" with the loop).
- Default echo send 0.35 on both Vocal tracks (`defaultTrackParams(Vocal)`), restored on load/reset.

### 10.3 Library after sharding (host measurements, 2026-10-15)

Picks weigh a content root and its `sNN` shards (kTreeMax 12, kFolderSlots 16, kCandMax 14): Beat/NA
cold pick 0.58 ms (walks the category root and streams 12 shard indexes), warm 0.035 ms; keyed kick
cold 0.44 ms, warm 0.034 ms; every warm pick is one open and ≤ 2 reads. Preparing a tree costs one SD
op per step (Beat 14 steps, Percussion 7, Bass/A minor 9, keyed kick 11). sizeof(Library) = 14.3 KB.
Known cost: trees evict each other from the 16-slot cache (Beat 14 slots, Bass 10), so a generate
re-streams evicted trees step by step; a per-tree slot design would fix it (see the library report).

### 9.17 Acid layer: synth knobs, perform routing, hold-ramps (2026-10-16)

#### 9.17a Engine: knobs, commands, distortion

- **Knobs.** `TrackParams.cutoff / resonance / envAmt / decay / drive / wave` are the live sound of a synth track,
  normalised 0..1 (`wave` a stepped index), shared with the UI through the mappings in `core/types.h`
  (`knobCutoffHz` 80 Hz..12 kHz log, `knobResonance` 0..0.985, `knobEnvOct` 0..4 octaves, `knobDecayS` 10 ms..2 s log).
  `smoothParams` smooths them per 256-sample block with the strip's one-pole (§9.12) in knob space, snapping once
  within 1e-4, and re-derives `knobs_[track]` (`SynthKnobs`: cutoffHz, resonance, filterEnvOct, decayS, drive, wave)
  only when a field moved: a settled track costs five compares and no `powf`. Every synth voice of a track points at
  its `SynthKnobs` (`setKnobs` at allocation, in `fireNoteOn` and the perform path) and reads them once per 32-sample
  modulation block, never per sample: cutoff base = knob × 2^(keyTrack × (pitch − 60) / 12) (key tracking kept, one
  multiply), env octaves and wave 1 as plain copies (a wave change goes through `selectTables`, i.e. the usual
  one-block crossfade of §9.15), resonance → SVF k only when it changed (clamp 0.985 instead of 0.95: Q ≈ 8, no
  self-oscillation), filter decay → `AdEnv::set` and dCoef^32 only when it changed (one `expf` per change, at most one
  per 8 modulation blocks while a knob moves), drive → waveshaper drive / makeup when it changed (the kernel is
  re-picked per 32-sample run, so drive can switch on under a sounding note). `noteOn` takes its initial values from
  the knobs when they are set. Host stress test (`tests/run_engine.sh`, 16 tracks, 10 s of audio): 49–56 ms before,
  51–57 ms after (three runs each, within run-to-run noise).
- **Knobs always describe the sound.** `CmdSetPatch` writes the patch into both the target and the current knob
  fields (cutoff → `knobFromCutoffHz`, resonance / 0.985, filterEnv / 4, `knobFromDecayS(fDecay)`, drive, wave1) and
  re-derives `knobs_` at once, so a new sound never glides in from the previous patch's knobs; `init` does the same
  with the default patch.
- **Commands.** `CmdSetParam` accepts `PCutoff..PWave` (PWave: f = index, rounded, clamped to the table set).
  `CmdRamp` ramps knob parameters like strip ones (start value from the target). `CmdFreezeRamp{track, a = param}`
  deactivates that ramp and leaves the target where the ramp left it (PTempo matches regardless of track).
  `CmdCancelRamps` snaps every strip / knob ramp back to its start value but skips PTempo: a tempo drift keeps going.
  A PTempo ramp deactivates itself when it arrives (it is not tied to a held key, and a pinned `bpm_` would swallow
  tempo±); strip / knob ramps stay at their end value until freeze / cancel, as §9.12 needs for SHIFT.
  `CmdResetTrack{track}` restores the category defaults of the five strip parameters (smoothed) and re-derives the
  knobs from the track's current patch (immediate); `CmdResetTrackParams{set}` and `CmdResetAll` do the same per
  track (plus the bus defaults as before).
- **Perform on any track.** `CmdPerformNote` and `CmdPerformChord` are handled identically (`Engine::performNote`):
  synth categories play the track's own voices with its patch and knobs (`allocSynth` in the track's range; legato
  glide on a mono glide patch while another performed note of the track is still held; vel 0 releases the voice of
  that pitch); Beat triggers pitch 36 as the kick (kick cut honoured, duck trigger, `EvKick`), 37..39 their slots,
  40..58 the accent (open-hat slot × 0.8); Percussion triggers slot 4 + (pitch − 36) for 36..39; Vocal plays nothing.
  The beat layer mask does not apply to performed notes. The former fixed routing to the set's Fx track is gone:
  the UI names the Fx track itself (`ui.cpp fxTrack()`), the sketch's `note P V` serial command sends track `Fx`.
- **Distortion intensity.** Strip drive = 1 + 31 × dist (was 1 + 7 × dist), output = softClip(x × drive) ×
  1 / sqrt(drive) (`clip.h distDrive / distMakeup`), both evaluated in `smoothParams` when `dist` moves (one `sqrtf`
  per moving track per block) and only multiplied in the sample loop. Measured on a sustained saw (`dist_intensity`
  host test, master output): dist 0 / 0.3 / 1.0 → RMS 0.018 / 0.048 / 0.036, crest 1.74 / 1.45 / 1.10. Dist 1 is a
  near square wave at ≈ 2 × the clean RMS; a fully clipped signal sits at 1 / sqrt(drive), so once the material is
  flat-topped (≈ dist 0.3 for a hot track) more dist crushes harder without getting louder. The synth voice's own
  pre-filter drive (patch / knob, 1..8, `clip.h distort`) is unchanged.

#### 9.17c UI: Beat-only pattern keys, hold-ramps, synth page, acid toggle, perform for every track (docs/KEYMAP.md is the full map)

- **Pattern keys.** pat1–4 of row y0 / y1 ALWAYS address that set's Beat layer (KICK, +HATS, +SNARE, FULL), never the
  selected track (queued while the set generates, §9.5). The variants of the other tracks have no key any more; the
  footer ticks still show the selected track's variant.
- **Hold-ramps.** A continuous parameter key (vol rev echo dist filt on the main page, the five continuous knobs on
  the synth page) without SHIFT: Press = one step (`CmdSetParam`, on the 0.05 / 0.1 grid) immediately followed by
  `CmdRamp{track, param, target = max | min, ticks = rampBars × 384 × remaining / range}` so the ramp runs at the
  SHIFT-ramp speed (the whole range in `rampBars` bars); Repeat = nothing; Release = `CmdFreezeRamp{track, param}`
  (the track the ramp was started on is remembered per parameter). The parameter page stays up while the key is
  held (sub text `HOLD`). With SHIFT today's behaviour stays (ramp to the end over `rampBars`, Repeat ignored,
  SHIFT release = `CmdCancelRamps` snaps every SHIFT ramp back). Length, tempo and key keep single steps (tempo with
  hold repeat). Leaving the synth page freezes any hold-ramp still held there.
- **SHIFT + tempo±** = tempo drift: `CmdRamp{0, PTempo, bpm ± 10, 60 × 96 ticks}`; the Ui does not count it as a
  SHIFT ramp (no `CmdCancelRamps` for it) and the TEMPO page says `DRIFT` while it runs. **SHIFT + key±** =
  `Generator::transposeOctave(selected, ±1)` with a 0.6 s `OCTAVE +1` / `OCTAVE -1` page in the track's colour.
- **SYNTH page** (modal, `PgSynth`): SHIFT + pat1–4 of a row opens it for that set's Chord / Lead / Pad / Bass; the
  same combo, another combo (switches track), the edit key or ` close it. Header `S1 LEAD  <preset>` plus `  ACID` in
  acid mode (`Generator::info(t).sound`, `acidEnabled`). Six knobs in a row (centres x = 20 + 40 i, y 50, r 14): a
  270° arc of 18 line segments lit to the value, a pointer, the label (CUT RES ENV DEC DRV WAV) and the real-unit
  value (`knobCutoffHz` → `440Hz` / `2.5k`, `%`, `knobEnvOct` → `2.0oct`, `knobDecayS` → `120ms` / `1.2s`, `%`,
  wave name), each repainted only when its text or percentage changed. Columns: vol± → PCutoff, rev± → PResonance,
  echo± → PEnvAmt, dist± → PDecay, filt± → PDrive (step 0.05 of the normalised knob, hold-ramps and SHIFT ramps as
  above), len± → PWave stepped (`CmdSetParam PWave f = index`). Tempo / key / track keys are not live there; no
  parameter overlay opens for a knob. `kPageParamColor` / `pageParamName` grew to the 15 ParamIds + genre + ramp
  (PPGenre = ParamCount).
- **Acid toggle.** SHIFT + genre on a selected Chord / Lead / Pad / Bass: `Generator::setAcid(selected, !acidEnabled)`,
  status `ACID ON S1 LEAD` / `ACID OFF` (refused with `SETS BUSY` while a save / load owns the card); any other track
  (Fx included): `ACID: SYNTH TRACKS ONLY`. The main page draws a small `A` in the top right of every acid square.
- **Perform for every track.** The perform page targets the selected track (`perfTrack_`; SHIFT + perform = the same
  track of the other set); header `S1 LEAD OCT3 Q1/16 CHD LOOP  PLAY`. Synth tracks play their own voices through
  `CmdPerformNote{track}`; Beat / Percussion use row y0 keys x1–8 as slots (pitch 36 + slot: Beat 36–43 with 40–43 as
  accents, Percussion 36–39 on x1–4; the recorded Percussion pitch is the one the stored fold map puts on that voice,
  §9.7); a Vocal shows `NO PERFORM FOR VOCAL` (tab and ` only). `[` (11,1) cycles the quantise OFF 1/16 1/8 1/4: with
  the transport running a note-on waits for the next grid point (a pending list of 8; `tick()` fires it, a key released
  before the point sounds a short note there), recorded timestamps snap to that grid (1/16 when OFF). `\` (13,1)
  toggles CHORD mode on Chord / Pad: a grid key plays degree, +2, +4 (`ProjectKey::degreePitch`, SHIFT = +1 semitone
  on all three) as three `CmdPerformChord` notes; the intervals are stored with the held key so the note-offs match
  whatever the octave / mode does meanwhile; the looper records all three. The looper records into the selected
  track (`userLoop` per track; drum tracks publish as the Full layer / density; GENERATE / gen pattern keep any track
  with `userLoop` until hold-del clears it).
- **Session.** Every param / knob / pause / len / variant / kick cut / acid / octave / tempo / key / genre / edit /
  loop change calls `Sets::markSessionDirty()`; the master output level goes through `Sets::setSessionVolume` and a
  restored `lp.volume` is applied once by `tick()` (`Sets::sessionVolume`, applied through the platform hook, no
  page). The desktop build steps `Sets` from its loop like the sketch and calls `restoreSession()` at boot in window
  mode (the scripted selftest starts clean).
- **Budget.** sizeof(Ui) = 1976 B (cap 2.5 KB, the looper's 772 B Pattern included). Tests: `tests/run_ui.sh`
  (36 cases: Beat-only pattern keys, hold-ramp commands and freeze, SHIFT ramp snap-back, tempo drift, octave
  transpose, synth page open / switch / close and knob routing, acid toggle messages and marker, perform on Beat /
  Percussion / Vocal, CHORD mode triads and their recording, quantise cycling and delayed note-ons, session dirty →
  write); `tests/run_desktop.sh` (the selftest's held vol− now expects the hold-ramp value 0.66, frozen on release).

#### 9.17b Generator: reset on generate, Beat on Kick, acid mode, transpose, preset params, vocal bars token

- **Reset on generate.** Every track a job regenerates (GENERATE set: all eight; gen pattern / gen sound: that one;
  the Fx track whose loop is kept still gets its sound) receives `CmdResetTrack{track}` as the first command of its
  part (`Generator::beginTrack`, retried on a full queue like `CmdSetGenerating`), before `CmdSetPatch` and the acid
  knobs, so the category defaults never wipe a randomised sound. A set LOAD does not reset: `sets.cpp` restores the
  file's values (below); a file from before this section gets `CmdResetTrack` per track instead (the old rule).
- **Beat starts on Kick.** After a Beat pattern generation (set job or gen pattern) the published variant is 0 (mask
  Kick only, `CmdBeatMask 0x01`, `track.7.beatLayer=0` in a set saved right after); the active pattern stays Full
  (§9.6), so pat2–4 build up from the kick. Percussion keeps Full (variant 3).
- **Acid mode** (`TrackGenInfo.acid`, `Generator::acidEnabled / setAcid`, Chord / Lead / Pad / Bass only; saved and
  restored as `lp.track.N.acid`). The preset banks of those four categories END with a `'303'` family of 8 presets
  (`presets.cpp`, `libraryPresetCount / acidPresetCount / isAcidPreset`), appended after the library presets so every
  saved `lp.track.N.preset` index stays valid: Lead / Bass mono saw or square, LP SVF, resonance 0.6–0.9, filter env
  1–3 oct, fDecay 0.1–0.4 s, glide 0.04–0.08 s (legato slides); Chord 3–4 voice stabs, resonance 0.4–0.8, decays
  0.12–0.25 s; Pad the same stabs with the pad family's 0.3 s attack, slow LFO and detuned osc 2 kept (the stab comes
  from the filter envelope). The preset bag runs over the track's family ([0, library) or [library, all)) and restarts
  when the family switches. `setAcid(true)` re-rolls the sound at once (no card: pick → `makePatch` into `nextPatch`
  → `CmdSetPatch`, deferred to `step()` while the queue is full) and randomises the knobs; `setAcid(false)` re-rolls
  a library preset. `randomizeKnobs(track)` draws from the generator's Rng (deterministic for `seed()`): cutoff
  0.25–0.75, resonance 0.45–0.95, envAmt 0.3–0.9, decay 0.15–0.6, drive 0–0.6, wave saw | square → six `CmdSetParam`
  (PCutoff..PWave, PWave with f = i = index) pushed only when they all fit, after `CmdSetPatch` so the knob init from
  the patch (§9.17a) is overridden. Every sound generation of an acid track (set job, gen sound) randomises again.
  A pattern job of an acid track picks nothing from the card: `buildAcidSource(track, seed, keyRoot)` writes the acid
  pattern straight into the track's SOURCE (`variants.cpp acidLine` for Lead / Bass: 16 or 32 steps on the 1/16 grid,
  root-heavy with octave jumps, ≈ 30 % slides whose note runs to the next onset + 12 ticks so the note-off follows the
  next note-on (legato glide, §9.12 ordering), ≈ 20 % accents ≥ 100, 10–25 % rests, Bass an octave below Lead;
  `acidStabs` for Chord / Pad: 2–3 note diatonic stabs over the tonic, iv, bVI or bVII on the offbeat 8ths and a
  few offbeat 16ths, 24 or 36 ticks, 1 or 2 bars), every pitch in the key (`ProjectKey::degreePitch / snap`) and
  clamped 12..115. The source then behaves like a loaded file: variants (variant 0 = the line), the editor, len±
  and set files work unchanged; `info.file` is empty and `info.acidPat` = 1 (a preset save writes it as an SMF asset
  like an edit; a session file writes `lp.src.N=acid` and rebuilds it from `lp.track.N.seed` + `.key`). Host
  measure (`tests/run_variants.sh`, 64 seeds): slides 30 %, accents 19 %, rests 16 %, roots 49 %.
- **Transpose.** `transposeOctave(track, ±1)` (SHIFT + key±, §9.17c) shifts every source note ±12 (clamped 12..115)
  on a synth track, marks it edited (set files write the SMF asset, §9.8) and republishes the current variant (swap
  at the bar, retried while the engine holds the lock). Drums and the vocal ignore it.
- **Set files carry the sound.** Presets write `lp.track.N.vol .dist .filter .echo .reverb` for every track and
  `.cutoff .res .env .decay .drive .wave .acid` for the synth tracks (0..1 floats with three decimals, filter −1..1,
  wave an index), read from the engine's targets at save time; a load restores them with `CmdSetParam` right after
  the track's `CmdSetPatch` (`Sets::pushTrackParams`) instead of the former `CmdResetTrackParams` at the swap. Reaper
  keys are untouched. The file text grew past one 4 KB chunk, so the writer now packs whole parts (header, a track's
  Reaper lines, a track's lp lines, kit lines) into ≤ 4 KB per step with the file kept open across steps (§9.2).
- **Vocal bars token (§9.16 amendment).** A vocal file name carrying `<n>bars` / `<n>bar` (digits right before
  `bar`, separators allowed: `125bpm__RAPM__8bars__…`, `4_bars`, `16 bars`; `Generator::parseBarsToken`) plays a
  window of n bars instead of 2, so a rap file plays whole (`VStream.windowBars`, `TrackGenInfo.windowBars`,
  `vocalClipTicks(variant, pause, window)`); the cycle is still window + pause bars, phase-locked as in §9.16, and the
  loop variants are unchanged. The engine clamps a vocal clip at `kMaxVocalClipTicks` (6 bars today, `engine.cpp
  applySwap`), so the generator caps the cycle there with the pause giving way (`clampWindow`, `kVocalWindowMaxBars`):
  a 4-bar window keeps a pause of up to 2 bars, an 8-bar window needs `kMaxVocalClipTicks` raised in `config.h`
  (e.g. (16 + 4) × 384 = 7680; the engine's `r.len` is 16-bit, nothing else depends on it) to play whole.
- **Sizes.** sizeof(Generator) 9752 → 9840 B (+88: `acid`, `acidPat`, `windowBars` per track, the bag's family base,
  `soundDue_`, `changes_`), sizeof(Sets) 10472 → 12160 B (+1688: strip / knob fields of `SetFileTrack`, the eight
  absolute source paths of a session file, the session state); both under the +2 KB allowance and the 12 KB test caps.

### 9.18 Session persistence (2026-10-16)

- **Files.** `/lp/state/session_set0.lpset` and `session_set1.lpset` (the folder is created on the first write) hold
  what a preset holds (schemaVersion, setIndex, `keyRoot` = the GLOBAL key, keyName, `genre`, `track.N.slot`,
  `track.7.beatLayer / beatKickMuted`, `lp.bpm`, `lp.track.N.{preset,seed,len,key,lenk,pause,kit}`, the §9.17b
  params / knobs / `acid`) plus `lp.volume` (the UI's master output level, when it passed one through
  `Sets::setSessionVolume`), `lp.src.N` = the absolute path of the track's pattern or vocal file (`/lp/midi/…`,
  `/lp/packs/<Genre>/presets/assets/…` or `/lp/packs/<Genre>/vocal/…`; `acid` for a generated line, which is rebuilt
  from `lp.track.N.seed` + `.key`), `lp.track.N.edited=1` / `.loop=1` with `lp.src.N=/lp/state/session_s<set>_t<N>.mid`
  for an edited, transposed or recorded (Fx loop) pattern, written with the SMF writer (also when a library source is
  no longer on the card), and `lp.kit.<slot>` = the absolute one-shot paths of all eight slots. No clip lines, no
  asset copies: nothing is ever written into a presets folder and no file is ever deleted.
- **Writer.** `Sets::markSessionDirty()` — called by the UI for its own changes (params, kick cut, volume …) — or any
  bump of `Generator::changeCount()` (jobs finished, variants, len / pause, key, genre, acid, knobs, transposes;
  polled by `Sets::step()`) arms a 2 s debounce on the engine's sample clock (`kSessionDebounceSamples`); the write
  then runs through the preset writer's state machine (`SsMkdir` → `SvTrack` with `SvResolve` probes for the library
  path or `SsEdit` for the SMF → `SvKitPaths` → `SsWrite` ≤ 4 KB per step), one bounded SD action per step (§9.2),
  for set 0 then set 1. It is not `busy()`: a user save, load or generate aborts it and it is re-armed (`abortSession`),
  so no key press is ever refused because the session is being written; `sessionBusy()` reports it. Persistence is
  armed by `restoreSession()` or the first `markSessionDirty()`: a host that calls neither (host tests, the desktop
  stub) never writes `/lp/state`.
- **Restore.** `Sets::restoreSession()` (the sketch calls it once after `init`, SPEC §7 boot) queues the restore, which
  runs in `step()` as soon as the generator is idle: per set, open + parse (4 KB chunks) → key / genre / volume
  (`gen.setKeyRoot`, `setGenreByName`, `sessionVolume()` for the UI) → per track `CmdSetPatch` + the params / knobs
  (`pushTrackParams`) → the pattern from its absolute path (`Generator::loadMidiToSource`, one file) or the acid seed
  (`buildAcidSource`), with `edited` / `userLoop` restored for a session SMF → the Beat / Percussion kits through
  the existing slot machine (absolute path, then the role folders and shards, then a pick of the same role) → the
  vocal (`loadVocal` + pause + variant) → kick cut, `CmdSetTempo` (nothing plays at boot) and the set-wide burst
  (`requestSetSwap`). The swaps land on the next bar line unheard: every restored track is Stopped, nothing starts
  playing. A missing or malformed file (unknown schema, junk) is skipped silently; the other set still restores. The
  restore owns the card like a load (`busy()`), so save / load / generate answer BUSY until it is done (≈ 1 s).
- **Tests.** `tests/run_sets.sh`: debounce timing, yield to a user save, file contents against the live state and the
  card, SMF of the recorded loop, restore round trip of both sets (sounds, params, knobs, acid flag and line, edited
  Fx loop, variants, pause, kits, key, bpm, volume), a malformed and a missing set-1 file, and the presets folder
  listing unchanged by save / load / session; `tests/run_generator.sh`: reset on generate (every `CmdResetTrack`
  observed, params checked where the engine applies it), Beat on Kick, acid sounds / lines / stabs, knob ranges and
  determinism, transposes, the bars token and a 4-bar vocal window on a real stream, `changeCount()`.

#### 9.17a Engine addendum: review fixes (2026-10-16)

- **Ramp re-target continues from the current value.** `CmdRamp` onto an active ramp of the same track / param used
  to keep the stale `from` while resetting `startTick`, so the parameter jumped back to the original start before
  ramping to the new target (a drift 120 → 130 re-aimed with SHIFT + tempo− at 125: 125 → 120 in one block, then
  towards 115; a hold-ramp vol+ reversed by a vol− press: the press's `CmdSetParam` step was overridden by the old
  start). The interpolation now always starts from the parameter's value at the `CmdRamp` (`bpm_` for PTempo, the
  target otherwise, which includes a step sent just before). `Ramp.origin` keeps the value before the FIRST ramp
  across re-targets and `CmdCancelRamps` snaps there, so the SHIFT release still returns to the pre-SHIFT value.
- **tempo± during a drift.** `CmdSetTempo` deactivates every active PTempo ramp before setting the tempo; the drift
  used to re-pin `bpm_` on the next block, so tempo± was dead for up to 60 beats after SHIFT + tempo±. The Ui needs
  no `CmdFreezeRamp` first.
- **Reset drops the track's ramps.** `resetTrackParams` (CmdResetTrack / CmdResetTrackParams / CmdResetAll)
  deactivates the track's strip / knob ramps before writing the defaults: a hold-ramp or SHIFT ramp held while the
  generator resets the track (every generate) no longer re-applies its value one block later. PTempo ramps are
  global and stay; CmdResetTrackParams therefore drops the ramps of that set's tracks only.
- **Distortion makeup plateau.** `distMakeup(drive) = 1 / sqrt(min(drive, 8))` (`clip.h kDistMakeupKnee`). Below
  the knee (dist < 0.226) nothing changed; above it a fully clipped signal stays at 0.354 while the small-signal
  gain keeps rising (×11.3 at dist 1), so more dist is never quieter (pure 1 / sqrt(drive) made dist 1 2.5 dB
  quieter than dist 0.3). Measured (`dist_intensity`, master output, sustained saw): dist 0 / 0.3 / 1.0 → RMS
  0.018 / 0.054 / 0.072 (was 0.018 / 0.048 / 0.036), crest 1.74 / 1.45 / 1.10. Dist 1 is 3.9 × the clean RMS and
  1.3 × dist 0.3; its clipped level is 2 × the old 1 + 7 × dist formula's 0.18. Listening decision still open: the
  knee (drive 8) and the plateau level are one constant each.
- **Dist gate.** The strip's dist stage is bypassed only at exactly dist 0 (was 0.01: a +1.2 dB step every time the
  smoothed dist crossed it — key down from 0, reset, ramp to 0, snap-back). At dist 0 the stage is drive 1 / makeup
  1 and `smoothParams` snaps dist to 0 within 1e-4, so a settled dist 0 still costs nothing. Measured: 0 → 1e-4
  moves the level −0.03 dB (the soft-clip knee at drive 1 on a 0.17-peak saw), 0.009 → 0.011 +0.23 dB (the curve's
  own slope, sqrt(drive)).
- **Knobs on a sounding note (amends §5).** §5's "a patch change cannot affect a sounding note" holds for what the
  patch alone owns (osc 2, sub, noise, amp envelope, LFO, glide, level). The live knobs (cutoff, resonance, filter
  env amount, filter decay, drive, wave 1) follow `knobs_` once per modulation block, and CmdSetPatch / reset write
  them to `current_` at once (no smoothing, unlike CmdSetParam), so on generate or preset change a sounding note's
  filter and drive step to the new patch immediately while the rest of the note keeps the old patch until it ends.
  `synth_voice.h` states this; the alternative (smooth `current_` while voices sound) was rejected because the next
  note would then start on half-glided knobs, which is what "the knobs always describe the sound" rules out.
- **Tests** (`tests/run_engine.sh`, 16 tests): `ramp_retarget_continues_from_current` (drift re-aimed mid-way
  continues from 125, arrives at 115; hold-ramp reversed with a step starts at the stepped value; knob ramp
  re-targeted; CmdCancelRamps snaps to the origin), `set_tempo_stops_drift` (CmdSetTempo during a drift takes
  effect and the drift is gone, a strip ramp alongside keeps running), `reset_track_restores_defaults_and_knobs`
  (a running PCutoff ramp is dropped by CmdResetTrack and the knob stays on the patch; the set reset leaves the other
  set's ramp and a tempo drift alone), `knob_res_env_decay_drive_reach_voice` (resonance 0.95 vs 0.1 at a 980 Hz
  cutoff: Goertzel harmonics 7+8 / fundamental 0.15 → 1.02; envAmt 1 vs 0: first-90-ms hf ratio 0.051 → 0.141;
  decay 1.0 vs 0.1: hf ratio at 100..200 ms 0.104 vs 0.051; drive 1 vs 0: crest 2.00 → 1.19), `dist_intensity`
  (RMS at dist 1 ≥ dist 0.3, the plateau values, level continuity across 0 and the old 0.01 gate). Host stress test
  (16 tracks, 10 s of audio): 50.6 / 56.2 / 56.6 ms before, 47.7 / 48.1 / 49.5 ms after (three runs each; the dist
  gate change costs nothing at dist 0 and the makeup is one `sqrtf` per moving dist per block as before).

#### 9.17c addendum — review fixes (2026-10-16, UI)

- **Hold-ramps.** One hold-ramp per parameter, owned by the key of its direction (`holdDir_`): a press while one runs
  (the other direction's key, or the same key on another track after SHIFT + track) sends `CmdFreezeRamp` first, so
  the engine starts the new ramp from the value reached (an active slot would keep its old `from` and jump back);
  the first key's release then sends nothing, the second's freezes its own ramp. SHIFT + the key during a hold-ramp
  freezes it too before the SHIFT ramp starts (its snap-back returns to that value). Opening the editor, perform or
  synth page, and closing any of them, freezes every running hold-ramp (`freezeHoldRamps`): the held key's release
  lands on a page where it means something else or is dropped.
- **Tempo drift.** A plain tempo± during a drift sends `CmdFreezeRamp{0, PTempo}` first and steps from the tempo
  reached (the engine's PTempo ramp would overwrite `CmdSetTempo` every tick, so the keys looked dead for 30 s);
  SHIFT + the same tempo key stops the drift where it is, SHIFT + the other key freezes it and drifts the other way
  from there (`driftDir_`).
- **Octave.** SHIFT + key± transposes only a synth track (Chord / Lead / Pad / Bass / Fx) with notes — what
  `Generator::transposeOctave` acts on; Beat / Percussion / Vocal get the status `OCTAVE: SYNTH TRACKS ONLY`, an
  empty source `OCTAVE: NO PATTERN`, with no page and no `markSessionDirty()`.
- **Output volume.** `setOutputVolume` (config.txt `volume=`, serial `vol`) and SHIFT + kick cut are explicit: they
  call `markSessionDirty()` (the session file carries the level within 2 s; `vol 0` before bed no longer leaves the
  previous 150 in the file) and set `volExplicit_`, so the `lp.volume` of a session restored later in the same boot is
  read (`Sets::sessionVolume`) but not applied; without an explicit level since boot `tick()` applies the restored one
  through the private restore path (no dirty flag, no page). `vol 0` sent right after boot therefore stays 0 whatever
  the restore brings, and config.txt `volume=` keeps working on a device with a session file.
- **Percussion recording.** The looper stores a Percussion slot as `percVoicePitch(track, 36 + slot)` — the pitch the
  set's stored fold map (§9.7) puts on that voice, as the editor writes it — while `CmdPerformNote` carries 36 + slot
  live; `publishLoop` rebuilds through the stored map (`Generator::rebuildVariant`), so the loop plays the slot that
  was hit (36 + slot itself may rank as another voice in the loaded file's map).
- **Quantise.** `perfPending()` fires a pending note-on once its grid point lies within the next render block
  (`(grid − tick) × samplesPerTickFp ≤ kBlock << 16`, 2 ticks at 120 BPM), so the command is queued before the engine
  crosses the point instead of a block plus the loop period after it; the recorded tick stays the grid point.
- **Gen pattern on a loop.** `requestGen(1, track)` on a track with `userLoop` is refused with `LOOP KEPT: HOLD DEL
  CLEARS` (the generator itself still protects only the Fx loop inside a set job — generator owner, see the open
  issue list).
- **Boot order (integration).** The sketch must call `g_sets->restoreSession()` once after `g_ui.init()` /
  `loadConfig()` for the session to come back (and to arm the writer); with the explicit-level rule above the order of
  `vol 0` and the restore no longer matters.
- **Tests.** `tests/run_ui.sh` adds: the freeze when a modal page opens, a second param press freezing the running
  hold-ramp (no jump to the first ramp's start), tempo± / SHIFT + tempo taking over a drift, octave on a Beat and an
  empty track, Percussion recording through the fold map, the quantise fire-ahead window, gen pattern refused on a
  loop, the session write carrying `lp.volume` after `setOutputVolume` / SHIFT + kick cut, and an explicit level
  surviving the restore while a fresh Ui takes the restored one. sizeof(Ui) = 1992 B (cap 2.5 KB; 43 cases).

### 9.18.1 Generator / sets fix pass (2026-10-16, after the review of §9.17b / §9.18)

- **Vocal bars token under the clip cap (§9.16 amendment, finding 2).** A window the engine cannot hold
  (n > `kVocalWindowMaxBars` = `kMaxVocalClipTicks` / 384, 6 bars today) is never cut mid-verse any more:
  `Generator::clampWindow` falls back to the default 2-bar window for it (the file plays as before the token
  existed). The pause is kept inside the cap: `fitPause(pause, window)` steps a stored pause down to the largest
  that fits when a file opens (`loadVocal`, the set loader), `stepPause(pause, dir, window)` refuses a len± step
  the cap would swallow, so the PAUSE page always shows what plays; the cap inside `vocalClipTicks` is only a last
  defence. Raising `kMaxVocalClipTicks` in `config.h` to (16 + 4) × 384 = 7680 (the engine's `r.len` is 16-bit)
  lets 8- and 16-bar rap files play whole — nothing else depends on the value, and the tests are written against
  `kVocalWindowMaxBars`.
- **Set files write the generator's intent (finding 3).** `Sets::fillTrackFields` writes `info.variant` and
  len = natural × 2^`lenStep` (`lp.track.N.len / .lenk`) instead of the engine's active pattern, which only follows
  at a bar line (never while the transport is stopped): a pat key, Beat layer or len± pressed inside that wait is
  saved as pressed; the kick cut (applied at once) stays the engine's.
- **Session files survive a cut-short write (finding 4).** Every session file ends in `lp.end=1` and carries
  `lp.seq` (one up per file written); each set has two slots, `session_set<N>.lpset` and `session_set<N>_b.lpset`,
  written alternately (the next write takes the slot that is not the newest). The restore probes both slots with a
  light parse (`RsProbe` / `RsProbeRead`: lp.seq, lp.end only) and rebuilds from the newest complete one; a file
  without `lp.end` (power-off or abort mid-write) is skipped silently like a malformed one, so at most the last
  write is lost, never the whole session. `abortSession` gives a file already in `SsWrite` one more chunk (one
  bounded SD action) so a write whose last chunk is pending completes instead of leaving a half file.
- **Pad '303' presets are stabs (finding 5).** The Pad acid family has a 2–5 ms attack and amp decays ≤ 0.25 s (the
  pad family's slow LFO and detuned osc 2 kept): a 24–36 tick `acidStabs` note reaches full level and decays
  instead of swelling. `tests/test_synth.cpp preset_category_character` checks the pad character over
  `libraryPresetCount(Pad)` and the stab character over the '303' family.
- **nextPatch in flight (finding 6).** A `CmdSetPatch` marks the track's `nextPatch` as the engine's until two
  blocks have passed (`Generator::patchFree`, `patchInFlight_` / `patchClock_`): an acid toggle behind a sound
  job's `CmdSetPatch` (or a job behind a toggle) waits for that window (`rollDue_`, `retryDue`; `stepPreset`
  waits in its pick phase) instead of handing the render task a torn patch.
- **Reset only what is regenerated (finding 7).** A pattern-only job pushes `CmdResetTrack` as the track begins; a
  job with a sound part pushes it right before that part (`SoundBegin`), so the reset-to-sound window stays small;
  an Fx track whose loop is kept under a pattern job regenerates nothing and is not reset (its strip and knobs
  survive). The set job still resets all eight (the kept Fx loop gets its sound part).
- **Gone library sources (finding 8).** A pattern whose library file is no longer on the card is written to the
  session as `/lp/state/session_s<set>_t<N>.mid` with `lp.track.N.edited=1`; after a restore the track is an
  edit, so a later preset save writes the SMF asset instead of losing the track as NONE.
- **Card sync (finding 9).** `tools/card_sync.py` never copies `presets/`, `sets/` or `state/` folders from the
  host tree (and never prunes them): a host test run's session files in `sdcard/lp/state` cannot overwrite the
  device's real session.
- **Strip at the swap (finding 10).** A preset load (and the restore) pushes the synth knobs right after the track's
  `CmdSetPatch` (`Sets::pushKnobParams`) and the strip (vol, dist, filter, echo, reverb) once the set has swapped
  (`LdWaitSwap`, `pushStripParams` / `pushAllStrips`, queue room checked), so the old pattern never plays with the
  new mix (§9.8); a file without the §9.17b keys gets `CmdResetTrack` at the swap, as the old effects reset did.
  With the transport stopped or at boot the strip goes out at once (nothing plays).
- **Sizes.** sizeof(Generator) 9840 → 9848 B, sizeof(Sets) 12160 → 12184 B (both within the +2 KB allowance:
  ≤ 11800 / ≤ 12520).
- **Tests.** `run_generator.sh` (40): the vocal ring reference reads the stream's window, the bars-token test covers
  the pause fit / refused step / 8-bar fallback and a ring comparison on a 4-bar window, a deferred acid toggle
  and a job waiting on a patch in flight, the kept Fx loop not reset. `run_sets.sh` (32): intent written while the
  transport is stopped, strip at the swap (and the pre-§9.17b reset), terminator / seq, truncated-slot restore and
  slot alternation, the gone-source edit round trip through a preset save.

### 9.18.2 Integration of the §9.17 / §9.18 pass (2026-10-16)

- **Sketch (`firmware/liveplayer/liveplayer.ino`).** `Sets::init` gets the library (the restore's kit fallback
  picks); after `loadConfig()` the sketch calls `g_sets->restoreSession()` once (SD present only, as the desktop
  main does): the restore runs in `Sets::step()` once the generator is idle, every restored track is Stopped and
  nothing starts playing; it also arms the session writer, so the device now keeps its last session across a reboot.
  The output volume reaches the session through `Ui::applyOutputVolume` → `Sets::setSessionVolume` (init, hook,
  config `volume=`, serial `vol N`, SHIFT + kick cut), so the sketch passes nothing itself; `vol N` and config
  `volume=` are explicit and win over the restored `lp.volume` (§9.17c). A session `lp.bpm` lands after config
  `bpm=` (the restore's `CmdSetTempo` runs later), so the session's tempo wins at boot. `stat` prints
  `session=busy|idle seq=<lp.seq> svol=<restored lp.volume or -1>` on its first line and, per track on one line,
  `acid=` plus the six knobs as 0..1 targets (`cut res env dec drv wave`, `Engine::params`) and the effective
  cutoff in Hz (`Engine::knobs`). Build: 754951 B program (22 %), 262980 B globals (80 %, 64700 B left) — the
  pre-pass sketch measured the same within the printf strings.
- **`kMaxVocalClipTicks` = (16 + 4) × 384 = 7680** (config.h, the §9.18.1 recommendation): a 16-bar `<n>bars`
  window plus the longest pause fits the engine's 16-bit clip length; 8- and 16-bar rap files now play whole
  instead of falling back to the 2-bar window. Only the clamp in `Engine::applySwap` and the generator's
  `kVocalWindowMaxBars` (now 20) read it; `tests/test_generator.cpp` pins the new value.
- **Tests.** `tests/test_library.cpp`: the RAP bank gave House a vocal `Amin` folder and sharded `Cmin` into
  `s00` / `s01`, so the A-minor fallback case now expects picks over `Amin` + `NA` (no relative-major `C` folder:
  NA must be reached) and the cold Cmin pick may walk the sharded root (`walked ≤ kTreeMax + 4`).
  `tests/test_ui.cpp` ends with `cleanup_session_files`, removing the session files its tests wrote into
  `sdcard/lp/state` (as `test_sets.cpp sizes_and_cleanup` does). `sh tests/run_all.sh`: 13 runners green.
- **Pointer for §5.** "A patch change cannot affect a sounding note" is amended by §9.17a's "Knobs on a sounding
  note": the six live knobs step to the new patch at once; the rest of the patch stays fixed at noteOn.

### 9.19 Loop mode at 125 BPM (2026-10-16; design contract docs/W3_LOOPMODE_CONTRACT.md)

User request: "introduce loop mode at 125 bpm - it can be enabled via shift + edit for the selected track, let's have
the vocal loops here too. normalize the loops first to fit 125 bpm, then copy what you need to the sd card." Content
is on the card (docs/LOOPS.md): `/lp/packs/<Genre>/loops/<category>/<Key|NA>/[sNN/]<Key|NA>__125bpm__LOOP__<n>bars__
<slug>.wav`, 22050 Hz mono int16, exactly n × 42 336 frames, n ∈ {1, 2, 4, 8}. The API is declared in config.h,
types.h, engine.h, generator.h, library.h, sets.h (`kLoopSlots`, `kStreams`, `kLoopRingBytes`, `kLoopChunkSamples`,
`kMaxLoopBars / kMaxLoopTicks`, `kLoopBpm`, `kLoopLowRamBytes`; `CmdLoopMode`, `CmdLoopLoaded`; `Engine::setLoopRing /
loopRing / streamRing / nextLoop / loopState / loopTrack / trackStreams / trackStream / streamUnderruns`;
`Generator::setLoopMode / lastLoopRefusal / loopEnabled / loopSlot / loopFile / loopBars / anyLoop / loopFolder /
setFreeHeapHook`; `Library::prepareLoop / pickLoop`; `SetFileTrack.loopMode`).

#### 9.19a Behaviour

1. **SHIFT + edit** `(8,3)` toggles LOOP MODE on the selected track (any category, either set). Status `LOOP ON S1
   BEAT` / `LOOP OFF S1 BEAT`; the main page marks loop tracks with an `L` (top left of the square, as acid's `A`).
   ON picks a loop at once (one generator job, `JobLoop`: prepare → pick → open → `CmdLoopLoaded` + `CmdLoopMode on`
   at the bar line); while the pick is in flight the square blinks as during a generate. OFF sends `CmdLoopMode off`:
   the track's parked pattern and patch play again at the bar (nothing is re-read from the card; `source_`, `next_`
   and the patch are left untouched while in loop mode).
2. Category → folder (`Generator::loopFolder`): Beat→drum, Percussion→percussion, Bass→bass, Chord→chord,
   Lead→lead, Pad→pad, Fx→fx, Vocal→vocal. Roots per `Library::prepareLoop`: tonal categories use the key's minor
   label, its relative major label and NA (as `pickVocal`); Beat / Percussion / Fx use NA plus every key folder
   present. Shards (`sNN`), `_index.txt`, shuffle bags and the no-repeat history as every other picker. Nothing
   found → `LoopNoContent`, status `NO LOOP: DRUM`, loop mode stays off.
3. **Tempo**: files are at `kLoopBpm` = 125; the stream varispeeds at rate = bpm / 125 like a vocal (retime on
   tempo change). When `Generator::anyLoop()` goes from false to true and no PTempo drift ramp is active, the UI
   sends `CmdSetTempo 125` (status `LOOP ON - 125 BPM`). Tempo± afterwards varispeeds every loop. (Implementation
   note, §9.19d: the boot restore is the one other exception — it restores the session's own `lp.bpm` with its loop
   tracks and consumes the edge; a user set load is not exempt.)
4. **Clip cycle**: natural = `<n>bars` × `kTicksPerBar` (384..3072 ticks; `Generator::parseBarsToken`). Pattern
   keys 1–4 on a loop track: 1 = whole loop, 2 = first half, 3 = first 2 beats, 4 = first beat (wrap crossfade
   `kXfade` = 64 samples as the vocal loop variants). len± = natural × 2^k clamped to [1 beat, `kMaxLoopTicks`];
   the engine accepts clip lengths up to `kMaxLoopTicks` on a streaming track (patterns keep `kMaxPatternTicks`).
   Kick cut on a loop Beat track = the strip filter raised to +0.6 (high-pass) while the cut is on and restored
   after (engine-side; the UI keeps sending `CmdKickCut`); snare rush on a loop Beat track = variant 4 momentary.
5. **Generates**: gen pattern, gen sound and GENERATE set on a loop track pick a new loop (same job); loop mode is
   retained by generates, as acid mode is. The **Vocal track** in loop mode streams the `loops/vocal` file through
   its set's own vocal stream (stream 0 / 1): a plain loop, no window + pause cycle, variants as above, no slot used.
6. **Refusals**: `PERFORM: LOOP TRACK` (perform page on a loop track), `ACID: LOOP TRACK` (acid toggle), `LOOP SLOTS
   FULL (2)` (`LoopSlotsFull`), `LOOP: LOW RAM` (`LoopLowRam`: free heap below `kLoopLowRamBytes`), `LOOP: BUSY`
   (`LoopBusy`: a generate / save / load is running), `LOOP: NO CARD` (`LoopNoCard`). The editor page on a loop
   track shows the loop's name, bars and variant (no grid, no edits).
7. **Persistence**: `lp.track.i.loopmode=1` in presets and session files; the loop is the track's clip (clipType
   WAV, the file copied into `presets/assets/` like a vocal — Reaper imports a WAV clip); session files carry the
   absolute path in `lp.src.i`. A restore / load with loopmode=1 restores loop mode with that exact file; a missing
   file → pick a loop of the category instead. Strip and knob values are kept exactly as §9.17b does for every track.

#### 9.19b Streams and memory — measured on the device 2026-10-16 09:20 (build df70a19, before this pass)

| | |
|---|---|
| static (Arduino "Global variables") | 262 980 B (80 %) |
| heap idle after boot | 25 068 B free, largest 13 300 |
| heap, both sets playing (16 tracks, 2 vocal files open) | 15 100 B free, largest 7 668 |
| heap during a session write (2 more files open) | 10 432 B → ≈ 2.3 KB per open card file |
| task stacks, min free | render 2 336 of 3 584 B, worker 1 892 of 3 072 B, loop 4 428 of 8 192 B |
| render | 5.4 ms max per 11.6 ms block, 16 tracks |

The loop rings therefore come from static reclaim, not heap:
- `kLoopSlots` = 2 loop streams device-wide (`kStreams` = 4: 0 / 1 the sets' vocal streams, 2 / 3 the loop slots),
  each bound to one track of either set; the Vocal track's loop mode uses stream 0 / 1, never a slot.
- Loop ring = int16, `kLoopRingBytes` = 6 144 B (3 072 samples = 139 ms), refilled in `kLoopChunkSamples` =
  1 024-sample (2 KB) reads whenever the ring has that much space: worst-case margin 93 ms, equal to the vocal
  ring's worst case (4 096-sample ring, 2 048-sample chunks). Memory is handed in by the host:
  `Engine::setLoopRing(slot, mem, samples)` — liveplayer.ino allocates both rings once in `setup()` with
  `bootBlock` (never freed; §9.13's "no allocation after setup()" holds), firmware/desktop and the tests use static
  arrays. The boot floor check stays (free ≥ 20 KB, largest ≥ 8 KB).
- Static reclaim (engine owner), −14.8 KB: (a) the echo line becomes µ-law `uint8_t[kEchoSamples]` (dsp/echo.*,
  wet path only; the feedback tail must still reach exact silence) −10 240 B; (b) liveplayer.ino's three static
  scratch buffers become one shared 2 048 B buffer −3 584 B; (c) task stacks render 3 584 → 3 072, worker 3 072 →
  2 560 (≥ 1.2 KB margin over the measured high-water marks) −1 024 B. Added: 2 × 6 144 B rings + 2 × 272 B heads +
  state ≈ +13 KB. Net ≈ −1.8 KB.
- Heap (generator owner), optional −5 KB: `Sets` may borrow the engine's `staging_` buffer for its `chunk_` and the
  `PropertiesReader::line_` only if code and a test show that Sets never performs card work while the generator is
  busy or a `CmdSlotSwap` is pending; otherwise skipped and said so in the report.
- Open card files: one per active stream plus one during a switch. `SdFs::kMaxOpen` 6 → 10. `Generator` refuses a
  loop when `freeHeap_()` (device: `ESP.getFreeHeap`) is below `kLoopLowRamBytes`.
- Engine: a streaming track renders its ring (`renderStream`) and then the normal strip (dist → filter → duck → vol →
  limiter); its synth / sample voices are released when loop mode turns on and the pattern is not stepped while it
  is on. Per-stream underruns (`streamUnderruns`) are shown in the footer as `LOOP UNDERRUNS n` when > 0 (UI) and
  in `[diag]` / `stat` (ino).
- Everything bar-locked exactly like the vocal (§9.3): start sample = the bar line the clip starts on, file frame =
  (pos in the clip cycle) × rate, `Generator::retime` on tempo change, `pump()` serving all four streams emptiest
  ring first, `kPumpChunks` 3 → 4.

#### 9.19c Session-writer starvation (generator owner, measured bug)

One parameter change → 2 s later the session write runs and the vocal stream loses ≈ 12 500 samples (0.57 s) over
the write (`vocal_underruns` 16 865 → 28 045, heap dips to 10 432). Fix: (a) stream-health gate
`Generator::streamsHealthy()` = every active stream ring at least half full (or no stream active); `Sets::step()`
performs card work only when it is true (the pump always runs); the generator's own file-open steps use the same
gate. (b) The session write path makes no verification opens (write the paths the generator knows). (c)
`kSessionDebounce` 5 s and at least 15 s between two session writes; a write still waits while the generator is
busy. (d) Host test with a slow-FS decorator (150 ms per open / write, 20 ms per read on the engine's sample
clock): zero vocal underruns through a session write with both vocal streams active, and with two loop streams
active. Also: GENERATE set keeps a recorded (perform) loop on every track, not only Fx.

#### 9.19d UI (this pass: ui.h, ui.cpp, tests/test_ui.cpp, docs/KEYMAP.md, docs/LOOPS.md)

- **SHIFT + edit** → `Ui::loopToggle`: `Generator::setLoopMode(selected, !loopEnabled)`; a Sets job (save / load /
  restore) is refused first with `LOOP: BUSY` (the generator cannot see the Sets machine); a generator refusal is
  mapped by `Ui::loopRefusalText` (`LOOP SLOTS FULL (2)`, `NO LOOP: <FOLDER>` from `Generator::loopFolder`, `LOOP:
  BUSY`, `LOOP: LOW RAM`, `LOOP: NO CARD`). Success: `LOOP ON S1 BEAT` / `LOOP OFF S1 BEAT`, `markSessionDirty()`.
  The same combo works inside the editor (the page switches between the grid and the loop view).
- **Tempo rule**: `tick()` watches `Generator::anyLoop()`; on its false → true edge (`anyLoopSeen_`) and with no
  drift running (`driftUntilTick_`) and the boot restore not owning the card (`Sets::busy() && sessionBusy()` =
  restoring: the session brings back the `lp.bpm` the user had with these very loop tracks), it sends `CmdSetTempo
  kLoopBpm` and shows `LOOP ON - 125 BPM` (`kLoopTempoStatusFmt`). A user set load is NOT exempt: Sets applies the
  file's `lp.bpm` only while nothing plays (§9.8), so the first loop a load brings in while the other set runs pulls
  to 125 like a SHIFT + edit would (when nothing plays, the load's own `CmdSetTempo lp.bpm` follows the UI's 125 in
  the queue and the preset's tempo stands, as at the restore). The contract writes the status with a middle dot
  (`LOOP ON · 125 BPM`); neither the device's built-in font nor the desktop 5x7 font has that glyph (it would draw
  as a CP437 box fragment), so a hyphen stands in — one format string in ui.h. The edge is consumed by the push and
  by the two exceptions alike, so the next false → true edge is needed for another 125; only a full engine command
  queue (`Ui::push` false) leaves it unconsumed — nothing is sent or shown, and the next tick tries again.
- **Main page**: `CellLook.loop` draws the `L` (top left, black on a filled square); `CellLook.pick` =
  `Ui::loopPickPending(track)` = loop mode requested and `!Engine::trackStreams(track)` (the pick on the card or the
  bar line pending), or a generate this Ui started on a loop track while the generator is busy (`loopPickMask_`) —
  the square then blinks at `kBlinkHalfMs` (outline dim ↔ colour; a playing square's fill colour ↔ 50 %).
- **Refusals**: `openPerform` on a loop track → `PERFORM: LOOP TRACK` (SHIFT + perform on the other set's track still
  opens when that one is not a loop track); `acidToggle` on a synth loop track → `ACID: LOOP TRACK`.
- **Editor loop view**: header `EDIT S1 BEAT  LOOP MODE`, `LOOP` large, the file name without `.wav` cut to the
  screen (`fitTextWidth`) or `PICKING ...`, then `2 BARS  VARIANT 1 WHOLE` (`loopBars`, `info().variant`,
  `loopVariantName`: WHOLE HALF 2 BEATS 1 BEAT), footer `SHIFT+EDIT LOOP OFF  NO EDITS`; cursor / enter / velocity /
  octave / gate keys return early; the view redraws when the picked name arrives. The note page of a pat key on a
  loop Beat says `S1 BEAT 2 HALF`.
- **Footer**: `Ui::loopUnderruns()` = Σ over the loop slots of `Engine::streamUnderruns(kSets + k)` minus a baseline
  that follows the counters while no loop is in use; when > 0 the line carries `LOOP UNDERRUNS n`, a whole segment
  that outranks the OUT and GYRO notes, the genre name and the clip length (drop order with the note up: OUT, GYRO,
  genre, LEN; only when the note still does not fit is it dropped and the line built as before), e.g. `A MIN 125
  LEN 8  LOOP UNDERRUNS 3` next to an `87%` readout. The engine counts samples (half a second of a starved ring is
  13 312), so `Ui::loopUnderrunText` writes n in thousands from 10 000 (`13k`; the two uint16 slot counters sum to
  at most `131k`) and the note is never longer than `LOOP UNDERRUNS 131k`. Measured on the 6 px font: the widest
  line that keeps the note, `C# MIN 240 LOOP UNDERRUNS 131k` (30 characters, 180 px), fits beside the widest
  readout, `CHG 100%` (240 − 1 − 48 − 4 − 2 = 185 px), so in practice the note is never dropped; `tests/run_ui.sh`
  checks the real 13 312-sample starvation beside `--` (LEN kept, genre dropped) and `CHG 100%` (LEN dropped too,
  the line ends `kFooterGap` before the readout).
- **Sizes / tests**: sizeof(Ui) 1992 → 2024 B (+32: `anyLoopSeen_`, `loopPickMask_`, the two underrun baselines, two
  CellLook bytes × 16; cap 2.5 KB). `tests/run_ui.sh` against the landed generator / sets / engine (real loop
  streams, 2026-10-16): **49 tests, 0 failed cases, 0 failed checks** in 4.7 s. Loop-mode cases (+6): the refusal
  texts, folder map and `loopUnderrunText`, SHIFT + edit on / off with the statuses, `L` mark and pick blink, the
  tempo rule (fires once per edge, not during a drift), perform and acid refusals, the editor loop view with no
  edits, both slots full, the Vocal track's slot-less loop, a generate on a loop track, low RAM, Sets / generator
  busy, a genre without loops (`NO LOOP: DRUM / PERCUSSION / VOCAL`), the footer note (shown whole beside `--` and
  `CHG 100%` on the real 13 312-sample starvation, absent and re-zeroed otherwise), and the tempo rule's edge (kept
  while the command queue is full and sent on the next tick; a session restored with a loop track and `lp.bpm=118`
  keeps 118 — no `LOOP ON - 125 BPM` during the restore — and a second loop track afterwards pulls nothing). The
  session-write cases wait `kSessionWaitBlocks` ≈ 21 s of engine time for a write (the 5 s debounce plus the 15 s
  spacing of §9.19c), letting a write already running finish first. The test file carries WEAK fallbacks for the
  generator's loop-mode API (and `Engine::setLoopRing`) so it links and models the contract until the generator
  owner's strong definitions land; the linker drops them the moment the real symbols exist, and the tests hold in
  both worlds (the run prints which). The harness hands the engine two static loop rings as the sketch's `bootBlock`
  does.

#### 9.19e Integration of the loop-mode pass (2026-10-16; engine, generator / library / sets and ui parts merged)

- **Rush key on a loop Beat track (the review's open finding).** `Ui::handleMain KSnareRush`: while that set's Beat is
  in loop mode the press calls `Generator::rushLoop(beat, true)` and the release `rushLoop(beat, false)` (the `S1 RUSH`
  note stays); `CmdSnareRush` goes out only for a Beat out of loop mode. The engine never steps a streaming track's
  pattern (`engine.cpp` skips `trackStream_[t] >= 0` before the rush branch — pinned by `test_engine.cpp
  rush_on_a_loop_beat_track_fires_no_samples`: a held rush is silent for two bars after `CmdLoopMode on` and sounds
  again after off), so the command was dead there. The release follows the press's route (`Ui::loopRushKeys_`, one bit
  per set): a rush begun on the engine is released on the engine although loop mode came on while the key was held, and
  the other way round. `rushLoop` streams the 1-beat crop from the next 1/16 and brings the stored crop and `lenStep`
  back on release (`info()` untouched, `changeCount()` unchanged — nothing for the session writer); `loopVariant(track,
  3)` + `loopVariant(track, prev)` would have reset a len±, which is why the generator grew `rushLoop` / `loopRushing`
  instead of the UI driving `loopVariant`. firmware/desktop's stub-only key driver (`LP_UI_STUB`) does the same. Test:
  `tests/test_ui.cpp rush_key_on_a_loop_beat_track_crops_the_stream` — len+ (k 1) survives the rush, the slot stream
  carries clip 96 / region 96 while held and len₁ / natural again after, `track.lenTicks` follows at the bar line,
  `status.rush` stays 0, set 2's Beat still gets `CmdSnareRush`, an engine-begun rush is released there after a loop
  toggle. KEYMAP rows `=` / `]` say so.
- **Cold `NO LOOP`.** A category the library has not counted yet is refused by the job, not by SHIFT + edit
  (`Generator::setLoopMode` does no card work; `loopFail` turns loop mode off with `lastLoopRefusal() == LoopNoContent`
  and `lastError()` = `NO LOOP: <FOLDER>`): the Ui's job-end path (`tick()`: `genWasBusy_` → `showStatus(lastError())`)
  shows it, no UI change was needed. A pack without `loops/` (`Library::genreHasLoops`, one `isDir` per pack in
  `scanGenres` at boot) and a counted empty category are still refused synchronously (the Testpack statuses of
  `run_ui.sh`).
- **API beyond the contract** (contract names untouched): Generator `rushLoop(track, on)`, `loopRushing(track)`,
  `loopSlotStreaming()` (+ private `encodeFolder` / `decodeFolder`; `kSrcDirLen` and `kLoopSyncOps` removed, no other
  file referenced them); Library `genreHasLoops(i)`; Ui `static loopUnderrunText(out, cap, n)` (public) and
  `loopTempoRule` returning `bool` (false = command queue full, the edge is kept). Engine, types.h, config.h: as declared.
- **Session writer vs loop slots (§9.19c amendment).** The host slow-FS decorator runs 150 ms per open / write / exists
  and 10 ms per read, not the contract's 20 ms: a 186 ms vocal ring survives a 150 ms op only when the refill reads that
  follow it drain < 36 ms — two other rings' reads at 10 ms each; 20 ms reads would need a 190+ ms ring. The two 139 ms
  slot rings cannot absorb a 150 ms op at all (review measurement: 10 570 / 10 790 lost samples, ≈ 0.48 s per slot over
  one write), so the session writer never runs while a loop slot is consumed: `Generator::loopSlotStreaming()` (the
  transport runs and a slot-bound track plays or is queued) makes `Sets::pollSession` defer a due write and `Sets::step()`
  pause one in flight with zero card work; it runs once the slot tracks or the transport stop. Measured (`run_sets.sh`):
  both vocals streaming through a write — 8 slow ops, 0 vocal / 0 loop underruns; the paused write — 60 steps with 0
  opens / probes / walks / bytes; resumed under the slow card — 23 slow ops, 0 vocal / 0 loop underruns; the second
  write began 15.00 s after the first completed; the write path makes 0 probes. Trade-off: no session write while a
  slot track plays (the Vocal track's loop on the 186 ms ring does not hold it back); two idle loop files stay open while
  loop mode is on.
- **Wrap audio.** `produce()` crossfades at a wrap only when the file holds a full `kXfade` of content past the cycle
  end: a whole loop file (n × 42 336 frames) repeats its head straight, identical to the first pass; a crop restarts
  straight after its fade-out; the vocal loop variants keep the crossfade. Measured: 128 samples compared across a
  repeat's bar line (64 in the wrap), 0 mismatches, `repeat[0] == file[0]`; a synthetic 1.5-bar `1bars` file takes the
  crossfade path (64 crossfaded, 0 mismatches). Every crop / len cycle of `run_generator.sh` compares 1 500 ring samples
  against the reference with 0 mismatches; a loop slot streams 3 s with min lead 2 304 of 3 072 samples, 0 underruns.
- **Sizes.** sizeof(Engine) 226 672 → 216 504 B (µ-law echo line −10 240 B, 64 B of slot heads; test cap 216 600);
  sizeof(Generator) 13 184 → 12 416 B (one path per stream, 2-byte pick-folder codes; the 136-sample `nxt` heads stay —
  every stream may hold a pending new-file switch at once, so one shared head would be torn); sizeof(Sets) 12 224 B;
  sizeof(Library) 12 240 B; sizeof(Ui) 2 024 → 2 032 B (`loopRushKeys_` with alignment; cap 2 560). `Sets` does not
  borrow `staging_`: load and restore drive the generator's kit slot machine (`CmdSlotSwap` pending, staging live) while
  Sets does its own card steps (`LdSlotLoad` / `LdSlotAck` interleave), so the contract's proof cannot be given — skipped.
  The echo tail still reaches exact silence (`test_fx` 27 / 27, engine `echo_tail_through_engine_reaches_exact_zero`:
  master output exact zero 2.83 s after the stop).
- **Device build** (`arduino-cli compile --fqbn esp32:esp32:m5stack_cardputer:FlashSize=8M,PartitionScheme=default_8MB,
  PSRAM=disabled`, 2026-10-16, no upload, no warnings): **Sketch uses 774 055 bytes (23 %)** of program storage;
  **Global variables use 251 628 bytes (76 %)**, 76 052 B left for locals. Against §10's first build (260 KB static,
  79 %) that is ≈ −14.6 KB; against the exact pre-pass figure of §9.18.2 / §9.19b (262 980 B, 80 %) it is **−11 352 B**
  (program +19 104 B). The contract's reclaim sums to −14 848 B, of which the task stacks (−1 024 B) are FreeRTOS heap,
  not globals; the static-only reclaim is −13 824 B (echo line −10 240, the sketch's scratch buffers −3 584), so the loop
  mode's static additions come to ≈ +2.5 KB (engine slot heads and pending-mode tables, the Ui's +8 B, four more `SdFs`
  handles, the sketch's ring pointers; the rest is per-module static state not re-measured object by object). The two
  6 144 B rings and the Library / Generator / Sets objects are `bootBlock` heap, allocated once in `setup()` and never
  freed. `tests/run_link_all.sh`: 0 GCC warnings on the Xtensa pass (library.cpp's shard warning now `%.23s`, 47 chars
  into the 48-byte `warn_`), largest frame 1 200 B.
- **Host suite.** `sh tests/run_all.sh`: 13 runners, exit 0 — desktop 19 tests + selftest PASS (25 s, render 0
  underruns, vocal 0 underruns, checks 16/16), dsp_core 26, engine 24, fx 27, generator 55, io 34, key_smf 43, library
  58, link_all PASS, sets 38, synth 42, ui 50, variants 32. The card image is left as found (the tests remove their
  `Testpack/loops` fixture and `/lp/state` session files).
- **Still open.** (1) Device heap re-measurement with both sets playing, two slots streaming and a session write in
  flight (`stat` / `[diag]` free / largest) — not done from the host (no upload); if the write case lands under ≈ 8 KB,
  raise `kLoopLowRamBytes` (config.h). Note the writer no longer runs while a slot streams, but two idle loop files stay
  open while loop mode is on. (2) The Ui infers "restoring" from `Sets::busy() && sessionBusy()`; a `Sets::restoring()`
  accessor would be more explicit. (3) `tests/stubs_generator.cpp`, the sets runner's fallback when generator.cpp does
  not compile, compiles again and defines `loopSlotStreaming` / `rushLoop`, but it predates §9.17 (`clampPause`,
  `setKeyRoot`, `setGenreByName`, `vocalClipTicks`, `generateSound` … undefined) and does not link against today's
  `test_sets.cpp`; the runner has used the real generator since §9.17. (4) `Library::genreHasLoops` is computed once at
  boot (`scanGenres`): a `loops/` folder added to the card afterwards is seen after a rescan only. (5) The user-load
  tempo case (a loaded set's loop track pulling to 125 while the other set plays) is covered by the gate's reasoning,
  not by a card fixture with `loopmode=1` tracks.

#### 9.19f Device bring-up of loop mode, the SD layer, and the tempo decisions (2026-10-16, 12:00–13:00)

Decisions by Miha (2026-10-16 morning): the default tempo is **125 BPM** (`kDefaultBpm`, and `bpm=125` in the card's
config.txt — most House loops and the whole rap bank are rendered at 125); **config.txt `bpm=` wins over a restored
session's `lp.bpm`** at boot (`Sets::setConfigBpm`, called from `loadConfig()`; presets still bring their own tempo
when nothing plays); the vocal clip cap of 20 bars (§9.18.2) stays. Cosmetic: the main page's selection square and
the variant ticks of an unfilled cell are drawn in the Beat grey (`categoryColor(Beat)`), not white (§4 amended).
The host suites keep reasoning at 120 BPM: their engine fixtures set the tempo explicitly after `Engine::init`.

**Measured before this pass (loop-mode build, Arduino `File` SD layer):** heap idle with the session restored 12.2 KB,
one loop slot streaming 7.2 KB (largest 3.1 KB), every further loop refused with `LOOP: LOW RAM`; `files=` 2 → 3, so
one open card file cost **≈ 4.9 KB of heap**, not the 2.3 KB of §9.19b. Cause, from the SD library source and the
mount error `esp_vfs_fat_register failed 0x101` (ESP_ERR_NO_MEM) when `max_files` was raised: this core's FatFs has
`FF_MAX_SS = 4096`, so each VFS file slot is a **preallocated FIL of ≈ 4.2 KB** (its own sector buffer) for the life
of the mount, all slots in ONE allocation at mount — 5 slots ≈ 27 KB with the FATFS object, 8 or more slots never
fit a free block of this heap; the Arduino `File` path then added an `fopen` stdio buffer (≈ 4 KB, `st_blksize`)
per open file on top.

**Fix (platform/sd_fs.*):** the SD layer uses POSIX descriptors on the VFS (`open / read / lseek / write / close`,
`opendir / readdir` for walks, `stat` for `exists` / `isDir`) at `/sd/<path>`; an open now costs no heap at run
time. `SdFs::kMaxOpen = 7` is both the handle pool and the mount's `max_files` (4 stream files + 2 pending stream
switches + one kit / index / set file), and `g_fs.begin()` runs **before** the boot pools (Library, Generator,
Sets, loop rings) so the 29 KB preallocation finds a free block. The boot floor is now free ≥ 12 KB, largest ≥ 6 KB.
Directory walks pass size 0 (no caller reads it; a `stat` per entry would double the cold-count cost).

**Measured after (final build of this pass):** `[boot] heap free=13576 largest=7668 sd=1`; with both sets playing,
two loop slots (Beat: a 2-bar drum loop; Bass: an 8-bar loop, clip 3072 ticks) and the Vocal track in loop mode
through its own stream, free heap **13.1 KB, constant** whatever the number of open files (`files=` 2 → 4 → 3);
`streams: underruns=0/0/0/0` throughout, including a parameter tweak that triggered a session write (`sets_max_us`
≤ 1.5 ms per step, one 21 ms op) and a loop OFF; the first loop pulled the tempo to 125 (`anyLoop` rule). The loop
pick itself is the longest loop-task step seen: 152 ms (`gen_max_us`) in one `Generator::step` (the loops folder
listing), inside the 186 ms vocal ring margin and the 139 ms slot margin only because the rings were full — a
candidate for splitting into bounded steps. `[diag]` now prints `files=` (handles in use) and the second's longest
`pumpVocal` / `Generator::step` / `Sets::step` in µs. Static RAM 251 396 B (76 %), program 772 903 B (23 %).

#### 9.19g Loop-pick step accounting and the card handle budget (2026-10-16, afternoon; host and compile only, no upload)

Scope: the two hardening items after §9.19f — the 152 ms `Generator::step()` seen at the loop pick, and the seven-handle
card pool against a set generate that switches three streams at once. Nothing was measured on the device in this pass
(no upload, no audio); every device figure quoted is §9.19f's.

**1. The loop pick's listing was already one card operation per step — pinned, not split.** Read against the code and
measured by two new host tests, `Library::prepareLoop` spends exactly one operation per call (`opsLeft_ = 1`): the
category listing is one walk (≤ 13 entries); NA and every key folder one index chunk (or one walk when unindexed) each —
the key folders are allocated as cold slots in the step the listing lands and counted one per later step, pinned against
the cache's own eviction while the pick runs (`opStart_`); `Generator::stepPrepLoop` makes one `prepareLoop` call per
step and `prepared()` lets a warm pick run in the same step only when no warm-up step was spent; the pick is one open +
≤ 2 reads, the file open one open + the header's small reads (one card sector). No path lists more than one folder in a
step, so there was nothing to split; the 152 ms step was ONE card operation that took 152 ms, not several — a cold Beat
pick costs ≈ 11 such steps either way. Measured (host; `tests/test_library.cpp
prepareLoop_on_a_full_cache_is_one_operation_per_step_for_every_root`, the cache FULL of 16 other trees first, as the
device's is after a set generate): drum prepared in 9 steps (the listing + NA + 7 key folders; worst step 1 open, 1 read
of 1 842 B, 1 walk of 8 entries), percussion 12, fx 4, bass 3 and chord 3 (A minor: minor, major, NA); the pick after
each: 1 open, ≤ 2 reads, 0 walks; `prepareLoop` again: no card work (the run evicted none of its own roots). Through
`Generator::step()` itself, pump included (`tests/test_generator.cpp cold_beat_loop_pick_is_one_card_operation_per_step`,
the same full cache): a cold Beat loop pick is 12 loop iterations, 11 with card work — prepared in 9 steps (8 roots + the
listing), 1 pick, 1 open; the worst step 1 open, 1 walk of 8 entries, 1 842 B of its own reads beyond the pump's (the
open step's 7 header reads are one sector); every step ≤ 1 open, ≤ 1 walk, ≤ 32 entries, ≤ 4 KB of its own. Still
unexplained from the host is the cost of ONE operation on this card: `open` / `opendir` resolve a six-component path
(`/sd/lp/packs/House/loops/drum/NA/…`) through directories of long names on FatFs, and SD cards stall for 100–250 ms now
and then (housekeeping, more so after writes — the session write of that minute); the vocal ring's 186 ms margin is sized
for exactly that, the 139 ms slot rings are not. To settle it on the device the `[diag]` line now carries
`gen_max_state=` next to `gen_max_us`: the `Generator::stateCode()` the second's longest step began in (0 idle / 1 the
job's first step, 21 PrepLoop, 22 LoopPick, 23 LoopOpen, 15 VocalOpen, 9 SlotOpen, 10 SlotChunk, 3 Pick, 4 Load). A
repeated long step at 21 is one listing / index operation, at 22 one index read, at 23 the WAV open; a value that moves
around is card latency. Unchanged on purpose: an absent or unindexed root costs one failed index open plus one walk in
the same step (the pre-existing accounting, asserted as ≤ 1 open and ≤ 1 walk; `tools/write_indexes.py` indexes every
loops folder, so a conforming card never walks a leaf).

**2. The card handle budget (`kMaxOpenFiles` = 7, config.h).** The pool is `SdFs::kMaxOpen` on the device and now
`PosixFs::kMaxOpen` on the host too (was 8), so the desktop build and every host suite run out of handles exactly where
the device would. Budget: one per live stream (`kStreams` 4) + `Generator::kPendingSwitches` = 2 new-file switches
waiting for their bar line + one for the job's own kit / index / SMF file or a Sets file. Rule (`Generator::
streamOpenAllowed`, `IFileSystem::openCount()` — a new virtual, 0 = cannot tell = never refused): a new-file stream open
(a phrase, a loop, the alternate phrase, the phrase a Vocal track returns to) is made only while one handle stays free
after it, i.e. `openCount() + 1 < 7`; with four live streams that is at most two pending switches, the third waits.
Every path waits instead of failing: `stepVocalOpen` / `stepLoopOpen` wait without card work (`kOpenWaitSteps` 1024
≈ 12 s cap, longer than a bar at 40 BPM, then open anyway), `vocalVariant` (a press to the alternate file) answers
`PubBusy` and `step()` retries it, `setLoopMode(off)` on a Vocal track defers its return (`vocalReturn` /
`vocalReturnDue_`: the loop keeps streaming, the `L` mark goes at once, `CmdLoopMode off` rides with the phrase's switch
once it has a handle; `step()` retries it as a card step of its own, also while idle), Sets' load / restore poll
`streamOpenAllowed()` before `loadLoop` / `loadVocal` and the save's asset copy (two handles) waits before its source
open (`kHandleWaitSteps` 1024). An open that fails outright (a handle taken elsewhere, a card hiccup) is `OpenFailed`
and tried again next step up to `kOpenRetrySteps` = 4 per stream before the old path runs (re-pick, `NO LOOP`, an empty
vocal, silence); a file that cannot stream is `OpenBad` at once. A pending switch frees its handle at its bar line
(`finalizeSwitch`, or at once on a stopped / unheard track), so the wait is at most one bar and never deadlocks: the
pump runs every step whatever Sets or the job wait for. Measured (`tests/test_generator.cpp
generate_set_with_three_switching_streams_never_fails_an_open`, PosixFs at 7 handles, both vocal streams and both slots
live): GENERATE S1 with the Vocal and both slot tracks (Beat, Percussion) switching — 347 steps, 84 of them the Vocal's
open waiting for a handle, at most 2 switches pending at once, 7 handles in use at most, 0 failed stream opens, all three
streams on new files, 0 underruns on all four streams, 4 files open afterwards; then loop OFF on the Vocal track with
both slots switching again — deferred (6 handles, the loop still streaming, nothing emptied), the phrase back after 307
steps with its `CmdLoopMode off`, 0 failed opens. The sets suite (39) runs with the budget active (its counting wrapper
answers `openCount()`), the UI suite on PosixFs directly. Sizes: sizeof(Generator) 12 416 → 12 424 B (+8: `openErr_`,
`openFails_[4]`, `openWait_`, `vocalReturnDue_`; cap unchanged), sizeof(Sets) 12 224 → 12 232 B (`wait_`).

**Behaviour changes to know on stage.** (a) A set generate whose Vocal and two loop tracks all switch files finishes a
bar later than before (the third open waits for a bar line); before, its third open failed with the 7-slot pool. (b) A
Vocal track leaving loop mode while two switches are pending keeps playing its loop up to a bar longer; the mode-off
lands with the phrase. (c) A variant press to the alternate phrase during such a wait is honoured a bar later instead
of being dropped. (d) A loop or phrase whose open fails three times in a row is retried, not re-picked, so a card
hiccup no longer swaps the file.

**Build and suite.** `arduino-cli compile` (no upload, no warnings): program 773 951 B (23 %), globals 251 404 B (76 %),
76 276 B left for locals (§9.19f: 772 903 / 251 396). `tests/run_link_all.sh`: Xtensa pass 19 objects, 0 warnings.
`sh tests/run_all.sh`: 14 runners, exit 0 — desktop 19 + selftest PASS, dsp_core 26, engine 24, fx 27, generator 57 (+2),
io 34, key_smf 43, library 59 (+1), link_all PASS, sets 39, synth 42, ui 50, variants 32.

**Open.** (1) Device: read `gen_max_state` next to a long `gen_max_us` to name the slow operation; if it is always 23
(the WAV open) the FatFs name scan of the 35-entry `loops/drum/NA` is the suspect, if it moves between states it is the
card's own latency and only ring depth helps. (2) A user SAVE started inside the bar after two loop re-picks waits up to
that bar before copying (was: `OPEN DST`). (3) The heap re-measurement of §9.19e (1) is still due. (4) The desktop
build's own `main.cpp` needed no change: PosixFs carries the 7-handle pool for it.

### 9.20 Track taps select, stops stay audible until the bar (user request, 2026-10-16 afternoon)

- A tap on a track key (rows 0 and 1) starts or stops the track at the next bar **and selects it**, exactly as
  SHIFT + track key selects; SHIFT + track key keeps selecting without a launch. The parameter pages, the knob keys
  and SHIFT + edit therefore always address the track the performer last touched (Ui::handleMain KTrack; §3 amended).
- Stops are quantised like starts. The engine already moved a playing track to `PSStopQueued` and only to
  `PSStopped` at the bar line, but a stop-queued track was no longer stepped (onTick) and its vocal / loop stream
  was no longer rendered, so one-shots and streams fell silent at the key press while synth notes rang out. Now a
  `PSStopQueued` track is stepped and rendered until the bar line (engine.cpp onTick / renderTrack); the UI's
  blinking square for the pending stop is unchanged. Tests: test_engine `queued_stop_plays_until_the_bar`
  (kicks keep firing after the stop tap and end exactly at the bar line), test_ui `tap_selects_the_track`.

### 9.21 Acid tracks were quieter: drive makeup and 303 levels (2026-10-16 afternoon; research docs/RESEARCH_LIBRARY_EXPANSION.md §5)

Measured on the host (engine at 120 BPM, master RMS over 2 bars, library preset vs the 303 family on the same
pattern): acid Lead −3.6 dB (min −9.2), Bass −4.8, Chord −12.1, Pad −7.2. Two causes:
1. The synth voice's drive makeup `driveMakeup(d) = 1/(0.35 + 0.65·d)` (synth_voice.cpp) under-compensated the
   soft clipper: −3.9 dB RMS at knob 0.3, −7.4 dB at 0.6, −10.6 dB at 1.0 — and acid mode draws DRV 0–0.6 on every
   generation while library presets mostly sit at drive 0. Replaced by an equal-RMS fit for a saw,
   `0.577 + 0.423 / (0.5 + 0.5·d²)` (≤ 0.27 dB error over drive 1.5..8; identity at 0). Library presets that use
   drive (0.15–0.5) gain ≈ 1 dB; equal-RMS is not equal-loudness, so driven sounds read slightly louder.
2. The 303 presets are single-oscillator with short decays, so even at drive 0 they sat 1.8–4.4 dB under their
   category's library mean. Their `level` fields are scaled ×1.23 (Lead), ×1.53 (Bass, Chord), ×1.66 (Pad), which
   brings each 303 family to +0.0 dB of its library reference (level3.txt); peak check with 3-note stabs at
   velocity 127: master peak 0.056 of full scale. The acid Pad family remains a stab sound by pattern design.
Not changed: the strip distortion's own makeup (clip.h, §9.17a plateau) is a different stage. Saved presets keep
their indices. A device listening pass at normal volume is still due (all tests and renders are host-side).

### 9.22 Library expansion wave: 303 character, chorus, generated preset bank, preset manager, PLAY/STOP ALL, USB drive (2026-10-16, afternoon / evening)

Decision document: docs/RESEARCH_LIBRARY_EXPANSION.md (steps 4 and 5 of its §7, the "chorus" option of its §5.3, and the
card-side items of §2–§3 landed by the content wave, docs/CARD_IMPORT.md). Shared API declared up front in `core/types.h`
(`ParamId PSat / PAccent / PChorus`, `TrackParams sat / accent / chorus`, `SynthKnobs sat / accent`), `ui/ui.h`
(`setUsbDriveHook`), `gen/sets.h` (the preset manager) and `rec/recorder.h` (§9.23). Budget for the whole wave (this
section and §9.23): static RAM + boot heap ≤ +3 KB over §9.19g's 251 404 B (chorus 1.4 KB, recorder 0.7 KB, the rest
0.9 KB); the boot floor check in liveplayer.ino moves from 12 KB to **10 KB free / 6 KB largest**. Host-side only: the
device was not connected during the wave, nothing was uploaded or listened to; every device number below is a compile
figure or an instruction-count estimate and says so. The user manual is docs/MANUAL.md, the key map docs/KEYMAP.md
(regenerated), the credits docs/CREDITS.md, the key stickers docs/stickers/ (`tools/make_stickers.py`).

#### 9.22a Engine: SAT, ACC and the per-set chorus bus (research §5 option B)

- **SAT** (`TrackParams.sat`, knob 0..1). The band-pass state `z1` of the TPT SVF passes through `softClip(z1·g)/g` every
  sample with g = 2·sat (cubic depth grows with sat², hard limit ±1.5/g); sat 0 runs the unchanged kernel code. It is a
  fifth kernel template bit (`kKernels` 16 → 32), chosen over a runtime flag because the S3 has no branch predictor (a
  per-sample compare + branch on every voice ≈ 60 µs per block) and flash is plentiful; the squelch itself is branch-free
  (`olt.s` / `movt.s` selects in the Xtensa disassembly, ≈ +12–15 issue slots per acid voice-sample ≈ +0.05–0.07 µs on the
  S3 — an estimate; the research's target was ≤ +0.15 µs). Host: +2.5–5.5 ns per voice-sample (latency-bound SVF chain);
  the 16-track stress scene with SAT 0.8 / ACC 1 / chorus everywhere 54.1 → 63.8 ms per 10 s of audio (×1.18). Measured
  effect: a sine on the cutoff h3/h1 0.0025 → 0.043; a res-0.95 saw (h7+h8)/h1 1.01 → 0.26 with the peak moving to h5+h6,
  fundamental within 1.5 %. Bit-exactness: a self-contained 4 s golden scene (two mono 303 patches, velocities < 100, SAT /
  ACC pinned to 0) hashes to 0x21e37ccc / RMS 768.608 / peak 5129 with the reconstructed pre-wave voice and with the new
  one; sat 1e-4 differs by 1 LSB (the hash is pinned for clang -O2 on this Mac; the test comment says how to re-pin).
- **ACC** (`TrackParams.accent`). `SynthVoice::noteOn` marks a note accented when vel ≥ 100; the accent multiplies the
  filter envelope amount by (1 + accent) and the level by (1 + 0.41254·accent) (+3 dB at 1), both read live from
  `SynthKnobs` once per modulation block: measured ×1.206 at 0.5, ×1.413 at 1, non-accented notes unchanged, a live knob
  follows within one modulation block. `SynthPatch.sat / .accent` (appended after `name[]`) are the knob values a patch
  brings in on `CmdSetPatch`; **both default to 0** (not the 0.5 of the brief): with 0.5 every library preset would accent
  its vel ≥ 100 notes (+1.6 dB, env ×1.5) and library clips mostly sit at the MIDI default velocity 100 — the '303'
  families, acid mode and the knobs opt in. The `TrackParams.accent = 0.5` header default is therefore overwritten by
  `knobsFromPatch` in every path (init, CmdSetPatch, reset).
- **Chorus** (`dsp/chorus.*`, one `Chorus` per set inside Engine): an int16 line of 331 samples (15 ms), two taps 229.5 ±
  66 samples (7.4–13.4 ms) a half LFO cycle apart, 0.5 Hz parabolic-sine LFO ramped per chunk, linear interpolation,
  saturating accumulate on the sends, exact zero and `quiet()` after one line of silence. The engine has no spare 1 KB
  send buffer per set, so tracks accumulate straight into the line and the chunk's wet signal is read before its input
  is written (read-before-write): this needs chunks ≤ 128 samples, hence **`Engine::render` caps every chunk at
  `Chorus::kMaxChunk` = 128** — observable only below 54 BPM or with the transport stopped (two chunks per block instead of
  one; the strip limiter's 16-sample sub-block alignment shifts there, audible only as tiny level differences while the
  limiter engages); a tick is 110 samples at 125 BPM, so nothing changes at the default tempo. `renderTrack` sends
  `buf × chorus` to `chorus_[set]` when the smoothed send is > 0.001 (after the strip, like the echo / reverb sends);
  `renderSet` calls `beginChunk` before its tracks (time counted in `profStrip`). Host cost bus + 8 sends 9.3 ns per
  sample → ≈ 0.1 ms per set per block worst case on the S3 (bound 0.3 ms).
- **Commands.** `CmdSetParam` accepts PSat / PAccent / PChorus (clamped 0..1); `CmdRamp` / `CmdFreezeRamp` work on them;
  `knobsFromPatch` / `copyKnobFields` carry sat + accent (CmdSetPatch, CmdResetTrack*, init), `deriveKnobs` copies them
  into `SynthKnobs`; `smoothParams` smooths `chorus` like the sends and sat / accent like the knobs (re-derive only when
  moved). No public engine signature changed.
- **Memory / flash (S3, core toolchain).** sizeof(Engine) 215 436 → 217 044 B (+1 608 = Chorus 2 × 676 + SynthPatch
  32 × 8 B; the header diff adds another 512 B inside Engine); SynthVoice 296 → 296 B (accent flag in padding, `vel_`
  reused for level × velocity, sat read from the knobs per run). Host sizeof(Engine) 217 016 → 218 624 (test cap 218 700).
  Flash (-Os text): synth_voice.o 15 849 → 28 149 B (+12.3 KB: 16 more kernels at ≈ 770 B; the research's 5–6 KB estimate
  assumed smaller kernels), engine.o +603 B, chorus.o +846 B → ≈ +13.7 KB.
- **Tests.** `run_engine.sh` 25 → 31 (golden bit-exactness, squelch spectra, accent sweep / level, chorus thickening +
  silent at 0 + exact return + set separation, the chunk cap at 40 BPM and stopped, SAT per-voice cost, the CPU regression
  in `stress_all_tracks_cpu`, sat / accent / chorus in `knobs_init_from_patch`); `run_fx.sh` 27 → 33 (chorus budget /
  silence, tap range + sweep, time variation, accumulate / saturate / NaN, quiet, chunk-size independence, timing).
  `run_generator.sh`, `run_sets.sh`, `run_ui.sh` now compile `dsp/chorus.cpp` (engine.cpp needs it to link).

#### 9.22b Presets: the generated bank (research §4)

- **782 presets** (was 73). Per category the hand library rows + 128 generated library rows, and for Chord / Lead / Pad /
  Bass the 8 hand '303' rows + 16 generated '303' rows: Chord 138 + 24, Lead / Pad / Bass 137 + 24, Fx 137 + 0
  (`libraryPresetCount` 138 / 137 / 137 / 137 / 137, `acidPresetCount` 24 / 24 / 24 / 24 / 0, `presetCount` 162 / 161 /
  161 / 161 / 137). Layout per bank `[hand lib][gen lib][hand 303][gen 303]`: hand rows keep rows 0..N−1 of each family and
  the acid family stays appended last, as the pinned `presets.h` contract requires (`presets.h` unchanged).
- **`tools/gen_presets.py`** (stdlib, seeded 2026, run with -I) emits `dsp/presets_gen.hpp` from per-family archetype
  tables with musical ranges: Chord Pluck / Stab / Organ / Keys / ChPad / Mallet; Lead Saw / Square / Sync / Bell / Flute /
  Hoover / Pluck; Pad Warm / Glass / String / Dark / Sweep / Hollow / Air / Choir / Brass (attack ≥ 0.31 s, LFO, detuned
  osc 2); Bass Sub / Reese / Acid / FM / House / Pluck; Fx Glide / Acid / Hoover / Bell / Wasp / Flute / Scream / Wobble
  (glide 0.12, res 0.41–0.69, poly 4); '303' rows inside the §9.17b ranges (Lead / Bass mono, glide 0.04–0.08; Chord / Pad
  3–4 voice stabs; Pad 303 attack 2–5 ms, decay ≤ 0.24, sustain ≤ 0.18, res 0.42–0.78). Names `<Arch> <Adj>` / `303 <Adj>`,
  ≤ 12 chars, unique per category; the hand names are embedded verbatim and emitted hex-exact (verified field by field
  against the old presets.cpp for all 73 hand rows + the glide patch). Rows are calls of a constexpr builder
  `presetgen::make` that assigns `SynthPatch` members by name and detects sat / accent at compile time; the arrays are
  constexpr, constant-initialised into .rodata (.data 0, .bss 0, no .init_array). '303' rows (hand and generated) carry
  sat 0.5–0.8 / accent 0.5–0.8, library rows 0 / 0 (see 9.22a).
- **Loudness QA** (`tools/preset_loudness.hpp`, `tools/presets_qa.cpp`, shared with `test_synth.cpp`): one fixed phrase
  per category at 120 BPM, vel 100, through `SynthVoice` with the voice's own glide / steal rules (Lead / Fx 16 eighths
  A3..B4, Bass −12, Chord 4 half-note triads, Pad 2 whole-note triads); metric = loudest 50 ms window RMS (the hand rows
  agree best under it: Chord 9 dB spread vs 19 dB whole-phrase RMS); targets = per-family hand-row medians (lib / acid
  separately). `--qa` bakes `level × 10^((target − loud) / 20)` clamped 0.05..1.0, capped so single-voice peaks (C4 vel 100;
  MIDI 24 / 108 vel 127) stay < 1.45, re-emits and re-measures: all 704 generated rows within ±0.01 dB, baked levels
  0.19..0.99, max peaks 0.67 (C4) / 1.32 (extreme); hand rows untouched (reported, not bounded). Whole pipeline 5 s.
  `run_synth.sh` also checks that the committed header is byte-identical to the tool's output ("presets_gen.hpp: in sync
  with tools/gen_presets.py", else OUT OF SYNC, exit 1).
- **Flash / RAM.** presets.o .rodata 9 500 → 100 304 B (+88.7 KB; 782 × 128 B), .text 584 → 560 B; RAM +0, no boot work.
  sizeof(SynthPatch) 120 → 128. Tests 42 → 46.
- **Index compatibility.** Hand library indices are unchanged; the hand '303' rows moved from right after the hand
  library (Chord [10, 18), Lead / Pad / Bass [9, 17)) to [`libraryPresetCount`, +8) (Chord [138, 146), the others
  [137, 145)). The pinned `presets.h` contract (library = [0, lib), acid = [lib, all)) cannot keep both ranges in place
  once 128 library rows sit before the '303' family, so the file says which layout its indices mean and the loader
  remaps: every set and session file written since this wave carries `lp.banks=2` (`sets.cpp kBanksLayout`, right after
  `genre=` / `lp.label=`); a file without it predates the move, and `presetForLayout` (applied per track in
  `Sets::loadTrack`, before `makePatch`; idempotent across re-entries) moves a synth track's index that lies in the old
  '303' range onto the same row of the new one. Every other index, and every index of a file with `lp.banks` ≥ 2, passes
  through. The old layout is a frozen table in `sets.cpp` (`oldHandLibrary` 10 / 9 / 9 / 9, `oldHandAcid` 8), not
  presets.h's current counts, so the hand rows of `gen_presets.py` must keep their order (the hand '303' rows the first
  8 of the acid family). A save after such a load writes the new index under `lp.banks=2`: the file is migrated. Test
  `load_remaps_the_303_rows_of_a_file_saved_before_the_generated_bank` (`run_sets.sh`): a file without `lp.banks`
  loads Chord 10 → '303 Stab', Lead 9 → '303 Saw' with `acid=1` kept, Pad 16 → '303 Dirty', Bass 3 and Fx 9 unchanged;
  the save after it writes 138 / 137 / 144 / 3 with `lp.banks=2`; the same text with `lp.banks=2` loads 10 / 9 / 16 as
  the generated library rows they now are.

#### 9.22c Generator: 512-preset bag, knob draws, half-length rush, the preset manager, the stream gate

- **PresetBag** = `uint8_t seen[64]` per track (exactly 64 B, `kPresetBagMax` 512): base / size come from presets.h on
  every pick, the last draw is `info().presetIndex` (a set load's preset is not re-drawn first), a `presetIndex` outside
  the family (acid toggle) restarts the cycle; uniform over the unseen presets, no repeat until the family is exhausted,
  the new cycle avoids the sounding preset. The former `order[16]` cap is gone (the 137-preset Lead family runs two full
  permutations in the test). `TrackGenInfo::presetIndex` uint8_t → uint16_t, fields reordered by alignment (160 B).
- **pushKnobs** (acid mode): CUT RES ENV DEC DRV WAV **SAT ACC** (+ **CHO on Lead / Chord**), `knobCmdCount(c)` = 8 or 9;
  drive narrowed to 0..0.5 (research §4, after the §9.21 makeup), PSat and PAccent 0.3..0.9, PChorus 0..0.4.
- **rushLoop** (loop Beat): streams a crop of **len / 2** (len = natural × 2^k in force), min 1 beat, clamped to the file;
  released back to the stored crop / len (§9.19e's 1-beat crop is replaced; `test_ui rush_key_on_a_loop_beat_track…`
  still expects 96 ticks — UI owner).
- **Preset manager (Sets).** Listing = passes over `_index.txt` chunks (or `kSetPage` directory pages fed straight to
  `considerRow`, so `names_` is never written while the rows live) selecting the 16 best rows strictly below a threshold
  row; page p costs p + 1 passes from scratch, ONE pass when it follows the page listed before (the usual paging); an
  unaligned first = keep rows (first % 16).. + one more pass; then `lp.label` of each row, one open or one 4 KB read per
  step; names without an NNN rank last (keyRoot −1, listed under the "all" filter only — no keyRoot probes). The rows are a
  union with `names_` (16 × 85 B of 2 048; static_assert). `beginLoadNamed` = `beginLoad` + `chosen_` = name → `LdChoose`.
  `deletePreset`: unlink (one op), then `_index.txt` → `_index.tmp` without the line → rename; rows / total / lastName_
  follow. `beginSetLabel`: `<name>` → `<name>.tmp` through a held-line filter (the old `lp.label` dropped wherever it is,
  the new line right after `genre=` or appended; lines split by the 4 KB read are judged whole), then rename over the
  original; the row's label and the loaded set's label follow. A set remembers the label of the preset it was loaded from
  (`loadedLabel_[kSets]`) and writes it on its saves and in its session files (restored at boot). `IFileSystem::rename`
  (new virtual, default false; `PosixFs` ::rename, `SdFs` VFS rename with unlink-then-rename on EEXIST — the complete .tmp
  exists at that moment). File keys: `lp.label=<tag>` (header, after `genre=`), `lp.track.N.sat / .accent / .chorus`
  (three decimals). New `lastError()` texts: BAD NAME, OTHER GENRE, BAD LABEL, NO PRESET, OPEN TMP, RENAME, INDEX.
  `presetListTotal()` is −1 until a listing finished; rows stay valid until a save / load / session write / restore begins.
- **Stream gate.** The slow-card session-write test exposed that `kGateLeadMs` 160 covered the 150 ms op plus ONE refill
  read — the ring read second drains through two 10 ms reads (needs 3 747 samples); the baseline passed by phase luck and
  the longer set files shifted the phase (127 lost samples). `kGateLeadMs` is now **170** (3 748 samples) and the lead rule
  applies to rings of cap ≥ lead + one block; the write test shows 0 underruns at every phase. Cost: with one pump per
  block the host gate is open every other iteration (never closed twice in a row); the device pumps several times per block.
- **Sizes.** sizeof(Sets) 12 232 → 12 432 B (+200: SetFile +116 [label 20, SetFileTrack 168 → 180 × 8], manager
  bookkeeping ≈ +44, loadedLabel_ 40; the rows in the union cost 0 — within the +256 B allowance); sizeof(Generator)
  12 424 → 13 128 B (+704 = the bag). Total +904 B of bootBlock heap against the 0.9 KB (921 B) share; no static. Xtensa
  text of the gen / dsp / io / engine objects 280 884 B, 0 warnings, largest frame 1 200 B unchanged; Sets' host step
  maximum stayed 2.7 ms. Tests: sets +3 (a 37-preset Testpack fixture: paging / follow / unaligned / filter / walk
  fallback / busy refusals; named load, delete, label replace-remove-insert, a > 4 KB file with the old label straddling
  the chunk boundary, append without genre, last-line label, loaded-label carry-over through the session file; Testpack
  left as found), generator +1 (bag cycles) and updated knob / rush / size tests, io +1 (rename).

#### 9.22d UI: PLAY / STOP ALL, the preset manager page, nine knobs, the USB drive hold

- **SHIFT + gyro (8,2) = PLAY ALL / STOP ALL**: any track PSPlaying / PSQueued → `CmdStopSet` for both sets, status
  `STOP ALL`; else `CmdCancelPending` for every PSStopQueued track + `CmdPlaySet` for both, `PLAY ALL`. Plain gyro is the
  hold toggle as before; SHIFT + gyro never arms it (the former "SHIFT + gyro = gyro" mapping is gone).
- **SHIFT + load 1 / load 2 = PRESET MANAGER** of that set (`PgPresets`, modal): asynchronous listing polled through
  `presetListReady()`; rows treated as valid only while ready (Sets keeps them in its `names_` page buffer and a load /
  save overwrites them — the page re-lists after its own load); pages of 16 requested from the 8-row boundary under the
  visible window so scrolling inside two windows costs no card work; delete and label run as Sets machines with
  `LISTING...` / `DELETING...` / `WRITING LABEL...` progress; AUTO = `CmdPlaySet` after a successful load. Keys, header,
  rows, footer and messages: docs/KEYMAP.md "PRESET MANAGER page". RENAME types from the key legends (SHIFT = upper case),
  digits and `-`, ≤ 19 characters (`kPresetLabelMax`), del = backspace, ent = `Sets::beginSetLabel`, ` = cancel.
- **SYNTH page**: nine knobs in two rows (CUT RES ENV DEC DRV / WAV SAT ACC CHO; one row of nine would leave 26 px per
  knob and a 6-character value is 36 px): tempo± (6,2) / (6,3) = SAT, key± (7,2) / (7,3) = ACC, gyro (8,2) = CHO+, edit
  (8,3) = CHO− — all with step 0.05 / hold-ramp / release freeze / SHIFT ramp + snap-back. **Behaviour change: the edit
  key no longer exits the synth page** (exits: the same SHIFT + pat combo or `; §9.17c amended). Footer `VOL-KEY GYR EDT =
  KNOBS  HOLD RAMPS`. The ramp bit masks are 32 bits (`ParamCount` 18 — the old uint16 masks silently aliased PAccent /
  PChorus with PVolume / PReverb). Geometry: `kSynthKnobs` 9, `kKnobCols` 5, `kKnobPitch` 48, `kKnobX0` 24, `kKnobX0Row2`
  48, `kKnobR` 11, `kKnobCy` 25, `kKnobRowPitch` 44. `synthKnobParam` maps PTempo → PSat, PKey → PAccent; `knobValueText`
  shows SAT / ACC / CHO as `nn%`; `pageParamName` has 20 entries (SAT, ACCENT, CHORUS added); palette: `kSatColor`
  orange-red, `kAccentColor` lime, `kChorusColor` sky blue.
- **SHIFT + gen pattern (12,2) held 1 s = USB drive**: status `USB DRIVE...` (5 s), 100 ms later `usbDriveHook_()` (null →
  `NO DRIVE FIRMWARE`); a short SHIFT press is the gen pattern of the selected track, acted on at the release; not fired
  while an offline render runs.
- Fixed on the way: the baseline crash the header change had caused (`pageParamName` had 17 entries for 20 PageParams →
  strcmp on nullptr). The desktop selftest's param checks read the Beat while the script's last track tap had selected the
  Vocal (§9.20): the script re-selects the Beat first, 16 / 16 PASS. The vocal test's hardcoded phrase path broke when
  House/vocal/NA got sharded: it now locates the file through the shard folders.
- **Sizes / tests.** sizeof(Ui) host 2 024 → 2 192 B (cap 2 700), S3 2 148 B ≈ +150 B static (within "the rest 0.9 KB");
  no heap; ui.cpp.o text 34 061 B (-Os). `run_ui.sh` 61 cases (10 new: pure helpers, PLAY / STOP ALL, SAT / ACC / CHO
  knobs incl. the 32-bit mask aliasing, USB hold / short press, preset manager listing + paging + filter + AUTO load with a
  20-file Testpack fixture, delete + rename against the real Sets rewrite, recorder page recording with event counts /
  REC marker, a real offline render of 707 blocks with DONE and the cancel hold (cancels after 88 blocks ≈ 1 s), session
  delete, size budget, cleanup).

#### 9.22e USB drive mode and the serial pull path (firmware/carddrive, liveplayer.ino, tools)

- **A second firmware in the spare OTA slot.** `firmware/carddrive/carddrive.ino` (ota_1 at 0x340000 of the 8 MB
  `default_8MB` table), built with `…,USBMode=default` (USB-OTG / TinyUSB): 566 423 B flash (16 % of the 3 342 336 B slot),
  59 308 B static RAM; esptool image-info valid. TinyUSB MSC over raw 512-byte sectors; **FatFs is never mounted** on the
  device while it runs — `sdcard_init(12, &SPI, 25 MHz)` registers the SPI driver for a drive number, `disk_initialize`
  runs the SD handshake, `sdcard_num_sectors` gives the capacity; SD.begin() / f_mount are never called. `USBMSC g_msc` is
  a GLOBAL because cores/esp32/main.cpp runs `Serial.begin(); USB.begin();` before setup() under CDC-on-boot and
  `tinyusb_enable_interface` refuses once the stack started; the composite device enumerates before setup() with
  `mediaPresent(false)` and the volume appears 1–2 s later once the card is up (hosts poll TEST UNIT READY about once a
  second). Callbacks run on the "usbd" task with buffers up to CFG_TUD_MSC_BUFSIZE = 4096 (8 sectors) through `sd_read_raw`
  / `sd_write_raw`, partial sectors through a 512 B read-modify-write scratch; an out-of-range lba or a card error returns
  −1 (TinyUSB stalls with a sense code; 0 would retry forever). The display is on SPI3_HOST, the card on the Arduino SPI
  object (SPI2), so loop() redraws while usbd moves sectors. **First act in setup(): `esp_ota_set_boot_partition(ota_0)`**,
  so any reset returns to the player; ENTER (13,2) restarts at once after a host eject, otherwise asks for a second press
  within 3 s; serial `player` restarts, `stat` prints `[drive] sectors=… rd=… wr=… err=… ejected=…`. Rollback: the core's
  sdkconfig has `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, so `esp_ota_set_boot_partition` writes state NEW and
  `initArduino()` calls `esp_ota_mark_app_valid_cancel_rollback` on every PENDING_VERIFY boot — both sketches validate
  themselves, the slots never ping-pong. Every Arduino image carries the prebuilt core's descriptor ("arduino-lib-builder"),
  so the player compares the ota_1 ELF SHA-256 with `esp_app_get_description()`'s and refuses a copy of itself. The CDC
  port keeps working but gets a NEW device name (TinyUSB composite vs hardware CDC/JTAG); USB re-enumerates on every switch.
- **Player side** (`liveplayer.ino usbDrive()`): checks ota_1 (partition present, app descriptor readable, SHA differs),
  suspends the render task, `Speaker.stop()`, steps Sets until `!sessionBusy()` (3 s bound), draws `USB DRIVE...`,
  `esp_ota_set_boot_partition` (verifies the image), `esp_restart`; registered via `setUsbDriveHook`; serial `drive` does
  the same. Serial `get <path>` → `G <size>\n` + exactly `<size>` raw bytes (2 KB reads through `g_scratch`, chunking
  invisible on the wire; `G -1` when missing or a directory; runs to completion inside the command, so stop the transport
  before pulling a WAV). The serial line buffer grew 64 → 128 B (+64 B static) so `get /lp/packs/<Genre>/presets/<name>`
  fits. Boot floor 12 → **10 KB** free (largest ≥ 6 KB unchanged). Measured from the sketch object: static +64 B (the line
  buffer; the remaining bss growth is other owners' headers: g_engine +512 B from types.h, g_ui +136 B from ui.h), flash
  ≈ +0.9 KB, heap 0 (`esp_app_desc_t` 256 B on the loop task stack only during the switch).
- **Tools.** `tools/flash_drive.sh [port]` compiles into /tmp/lp-drive-build, checks the image fits and writes ONLY
  0x340000 with the core's esptool 5.3.1 (`NO_UPLOAD=1` compiles only; run it from the PLAYER's hardware-CDC port — the
  drive firmware's TinyUSB CDC port may not take esptool's DTR/RTS reset). `tools/card_pull.py PORT [REMOTE_DIR]
  [LOCAL_DIR]` pulls a folder over the serial port from either firmware (player `ls` / `get`, loader `LS` / `GET`),
  skipping `[diag]` chatter, verifying received == announced (and == listed size with the loader; the player's `ls` prints
  size 0), exit 1 on any failure; `--selftest` runs both dialects in-process and is `tests/run_card_pull.sh` (PASS).
  `firmware/cardloader` gained `GET` (4096 B chunks). docs/CARD_IMPORT.md C describes the user path; managed computers
  that block removable drives fall back to `card_pull.py`.

#### 9.22f Build figures and what is open

- **Whole sketch** (recorder owner's `arduino-cli compile` at the end of the wave, no upload): program **900 259 B
  (26 %)**, globals **254 404 B (77 %)** = +3 000 B over §9.19g's 251 404 — exactly the 3 KB allowance (the recorder's share
  672 B + 1 B flag). Flash grew ≈ +126 KB over §9.19g's 773 951 B: the preset bank +88.7 KB, the SAT kernels +12.3 KB,
  chorus / engine / ui / recorder / drive hook the rest — 0.4 % … 3.8 % of the 3.3 MB app slot each, plenty left. Boot heap
  growth: +904 B (Generator bag + Sets manager) against the 0.9 KB share; free heap stays 13.1 KB minus that, measured only
  on the host. The drive owner's own compile of the whole player was red at the time (the UI's knob constants in flight);
  its sketch TU compiled; the figure above is the later green build.
- **Open items of the wave** (collected from the implementers' reports):
  1. ENGINE: `SynthPatch.accent` defaults to 0.0, not the 0.5 of the brief (reasoned above); flip to 0.5f only with a
     re-pin of the four `test_synth` vel-127 level tests. The S3 cost of SAT is an instruction-count estimate; the host
     numbers do not transfer by the research's 185× ratio. The golden hash is compiler-specific.
  2. PRESETS: `test_engine sat_zero_is_bit_exact…` loads `presetNamed(Lead, "303")` = hand '303 Saw', which now carries sat
     / accent 0.5–0.8, so the pre-§9.22 hash needs the scene's patches pinned to sat = accent = 0 (verified on a scratch copy:
     appending `e.nextPatch(1).sat = 0.f; … nextPatch(3).accent = 0.f;` restores 0x21e37ccc / 768.608 / 5129 and 31 / 31 pass).
     The hand '303' rows' sat / accent are seeded draws (0.5–0.8), not hand-chosen; the '303' rows are measured accented at
     velocity 100. Acid tracks now squelch by default: SAT 1 on a res-0.95 saw costs ≈ 2 dB RMS and ≈ 14 dB on a pure
     resonant sine — the §9.21 level balance deserves a device listening pass. The "medium spread" of `applySpread`
     (research §4) is not implemented (dsp owner).
  3. GENERATOR / SETS: `deletePreset`'s index rewrite failing after the unlink reports `ok()` false with `INDEX` while the
     preset is already gone (the stale index line heals on the next random load). The host slow-card gate now asks for
     170 ms of lead, halving the host gate's duty cycle to one open iteration in two when a client polls every block (device
     impact: a slightly later start of card work while streams are consumed). `run_library.sh` has 5 red cases from the
     concurrent card expansion (Fx A#min / C#, Pad Bmin / D, House/vocal D# and G, Emin re-sharded, more sharded trees than
     the 16-slot cache in `sd_budget_sharded_trees_survive_a_set_generate`) — fixtures that encode card facts, to be re-based
     by whoever lands the content; the cache-capacity one may be a real design question.
  4. UI: `test_ui rush_key_on_a_loop_beat_track_crops_the_stream` still expects the 1-beat crop (96 ticks); the rush is now
     half the clip length (768 for the 2-bar loop at len 1536). `pure_helpers` expects ParamCount 15 (now 18). Not verified
     on the device: the two-row knob layout's legibility and the REC footer blink (FakeGfx geometry assertions only).
  5. DRIVE: nothing verified on the device — MSC enumeration and mount time, raw-sector throughput (single-sector SPI reads
     at 25 MHz), the otadata switch both ways, `esp_ota_set_boot_partition`'s image verification time (≈ 0.6 MB read), the
     ENTER matrix coordinate (13,2) on the ADV keyboard, the new CDC port name. `get` blocks the loop task for the whole
     transfer (fine for presets; stop the transport before pulling a WAV). The player's `ls` reports size 0 for every entry
     (`SdFs::walk` does not stat). If the drive image is flashed anywhere but 0x340000 the hook finds nothing and says
     `NO DRIVE FIRMWARE` (safe).
  6. SUITE: at the end of the wave `tests/run_all.sh` had reds in desktop (selftest), engine, fx, generator, library, sets
     and ui, every one traced by its reporter to another owner's in-flight edit or to the card content being moved (none to
     the recorder or the drive); `run_rec` (11), `run_card_pull`, `run_synth` (46) and `run_link_all` were green. A green
     run of the whole suite on the merged tree is the integration owner's first job; `run_link_all.sh` does not cover
     `src/rec` yet (one line).

### 9.23 Session recorder: record the performance, render it to WAV offline (user request, 2026-10-16)

User request: "introduce recording mode (SHIFT + save 1) — it brings up a special menu that lets me record a session (not
actually wav, but actions taken), delete a session and output to wav on a special folder on card". Budget: 0.7 KB of the
wave's 3 KB (§9.22); the API is `src/rec/recorder.h`, the implementation `src/rec/recorder.cpp`, the page and the event
logging `ui/ui.cpp` (SHIFT + save 1, §9.22d), the sketch wiring `liveplayer.ino`, the desktop `firmware/desktop/main.cpp`
(Makefile: `rec/*.cpp`, `-DLP_HAVE_RECORDER`). The user's instructions: docs/MANUAL.md §12, docs/KEYMAP.md "RECORDER page".

- **What a session is.** Input events on the engine's tick clock, not audio: `/lp/rec/rec_NNN.lprec` = a 64-byte header
  (magic `LPREC1`, seed, bpm, keyRoot, set1Autoplay, genre[32], events, lengthTicks, reserved[8]; explicit little-endian)
  + 8-byte events (`tick, type, x, y, extra`; type 0 key press, 1 key release, 2 gyro — x = (int8) filter × 127, y = reverb
  0..255, extra = echo 0..255 —, 3 output volume with extra = level). Conventions inside the pinned structs: `reserved[0..1]`
  = little-endian mask of the tracks playing / queued at the start (0 when the transport was stopped), `reserved[2..3]` =
  the bar phase (ticks into the bar) of the start; `set1Autoplay` = 1 when any set-1 track was playing; a type-0 event with
  x = 255 is the pseudo key "master play / stop" (y = 1 run / 0 stop) that the sketch records for BtnA and replays as
  `CmdMasterPlay`. Up to 64 sessions (`kMaxSessions`); `onInput` stamps `Engine::status().tick` itself.
- **Recording.** `startRecording` creates the next `rec_NNN.lprec` and writes the header from the live state (a fresh seed
  drawn from sampleClock / tick — the Generator's Rng has no accessor —, then `gen->seed(seed)` and `gen->setKeyRoot(root)`
  so the recording starts from a known generator state; bpm, key, genre, play mask + bar phase); the counts are rewritten
  at `stopRecording` with one 8-byte write at offset 48 (the file stays open in "w" mode for the whole recording —
  `IFileSystem` has no r+ mode — so one of the 7 FatFs handles is taken and `Generator::streamOpenAllowed` leaves room for
  one pending stream switch instead of two). Events are buffered 64 deep (512 B); `step()` flushes with ONE write when
  `Generator::streamsHealthy() && !loopSlotStreaming()` (the Sets writer's rule), a full buffer flushes inline in `onInput`
  so nothing is lost. The Ui records every key press / release it receives (SHIFT included) except the recorder page's own
  keys, the SHIFT + save 1 that opens or closes it and the release of the ent that started the recording; Repeats are not
  recorded (the Recorder synthesises KeyMatrix repeats on replay, 350 ms then 80 ms, on the replay clock, up to 4 held
  keys); every `CmdGyro` as type 2 (gyro off recorded as neutral 0 / 0 / 0, since `RecEvent` has no field for `CmdGyro.a` —
  the replay re-enables the gyro at neutral, audibly equivalent); every output-volume application as type 3; BtnA as the
  pseudo key. Main footer: a blinking red `REC` left of the battery.
- **Session list.** One walk each of `/lp/rec` and `/lp/wav` into two 64-bit masks (names are `rec_NNN`, no name cache);
  `sessionInfo` derives the event count from the file size and `lengthTicks` from the last event when the counts were never
  written (power loss) — the test corrupts a header and replays it. `deleteSession` removes `.lprec` and `.wav`, refused
  while recording / rendering. The RECORDER page reads one row's header per tick (`rec_007  4 BARS  W`).
- **Offline render.** `startRender` refuses while the generator or Sets is busy; opens `/lp/wav/rec_NNN.wav` (44-byte header
  patched on finish), re-seeds, `setGenreByName(header.genre)`, `setKeyRoot(header.key)` (the same post-state as at the
  recording start), pushes `CmdResetAll`, `CmdMasterPlay 0` then 1 (the engine puts the tick on a bar line with sub-tick
  phase 0 at the next block start), `CmdSetTempo`, `CmdTrackStopNow × 16` (MasterPlay 1 would otherwise resume the live
  tracks), `CmdTrackStart` for the recorded play mask; `base_` = that bar + one bar of pre-roll (the queued tracks start at
  its end) + the recorded bar phase, so every recorded tick lands on the bar phase it had live. `renderStep` (called by the
  sketch's loop() once the render task has parked): render ONE block into the caller's buffer (startTick taken as the bar
  line while the burst is still queued), write it once startTick ≥ base_, `pumpVocal` + `gen.step` + `sets.step`,
  `Ui::tick(renderNowMs)`, deliver every event with tick ≤ the engine tick (64-event chunks read with a transient open /
  seek / read / close — only the WAV handle is held during a render), synthesise the repeats, finish when all events are
  replayed and `lengthTicks` reached (or the transport is frozen by a replayed master stop) plus one bar of tail;
  `finishRender` patches the header, sets the WAV mask bit and pushes `CmdMasterPlay 0` (the replayed performance does not
  go on playing; BtnA resumes). `cancelRender` (the Ui's ` held 1 s, BtnA, serial `rec cancel`) removes the partial WAV; a
  cancel from inside `Ui::tick` is honoured mid-step. **`renderNowMs()`** (the one public addition beyond the pinned
  header) is the replay's clock while rendering: the engine sample clock since `startRender`, continued from
  `g_microsHook() / 1000` when the hook is set; `renderStep` ticks the Ui with it and the sketch passes it to `Ui::draw` and
  to the live key events it still delivers, so the Ui runs on ONE clock during a render (hold timers, pages). The
  RENDERING overlay (`RENDERING rec_NNN`, nn %, `HOLD ` TO CANCEL`) covers every page; `DONE /lp/wav/rec_NNN.wav` for 3 s;
  the post-render cleanup runs on the tick AFTER the render ends because `renderStep` still delivers that step's events
  after ticking the Ui.
- **Sketch wiring.** `static lp::Recorder g_recorder` (672 B), `std::atomic<bool> g_renderParked`: the render task parks
  after its last live block and feeds silence from `g_audio[1] / [2]` while `g_audio[0]` belongs to `renderStep`; loop()
  calls `renderStep` only once parked, does not tick the Ui with millis() while rendering (renderStep ticks it), delivers
  live keys and draws with `renderNowMs()`, skips `onImu` while rendering (the recorded type-2 events own the gyro), and
  records / cancels on BtnA. Serial: `rec list|start|stop|render N|cancel|delete N` (N = the row index of `rec list`),
  `[rec] rendering NN%` once a second.
- **Measured.** Device sizeof(Recorder) = 672 B (Xtensa; static_assert ≤ 700, host ≤ 728 with 8-byte pointers);
  recorder.cpp text 5 289 B of flash, largest frame 224 B; boot heap +0. Whole sketch: 900 259 B program, 254 404 B globals
  (§9.22f). Host test (`tests/run_rec.sh`, 11 cases, routed to a mkdtemp card, asserting the real card was never touched):
  a scripted live performance (GENERATE S1, three track taps, a vol+ hold-ramp, tempo+, SHIFT + rev+ ramp with snap-back,
  gyro, output volume, pat2) recorded through the real Ui and replayed in a freshly booted world pushes the identical
  46-command sequence, every command within 4 ticks of its live position; two fresh worlds render byte-identical WAVs; WAV
  header / length checked; render ≈ 1 695 steps for ≈ 9 bars. Not measured on the device: render speed vs real time (design:
  about 1–2 × real time, bounded by the card), heap during a render (one WAV handle + transient .lprec reads), and whether
  the parked task's silence feed keeps the speaker queue healthy.
- **Limitations (documented in recorder.cpp and the manual).** Content loaded before the recording started is not
  restored (the engine keeps what it holds at render time; the play mask only restarts those tracks): a session recorded
  from an empty stage renders exactly. The generator's per-track preset bags have no public reset, so a sound pick inside a
  recording may draw a different preset than live on the device (the host test uses fresh worlds, where it reproduces
  exactly; suggested fix: `Generator::seed()` also resets `presetBag_[]`). Replay reproduces ticks, not sub-tick phase (the
  live recording starts at an arbitrary block boundary, the replay on a bar line): bar-quantised events are identical,
  samples may differ by < 1 tick from a hypothetical live capture. Replayed changes mark the session dirty, so Sets writes
  `/lp/state` during the render and the engine ends in the replayed session's end state (transport stopped); the user's
  pre-render live state is not restored. After a render the Ui falls back from `renderNowMs()` to millis(), which is
  behind: timers armed during the render (statusUntil_, pageUntil_) may linger until a showStatus / page change. The
  recording's open `.lprec` handle reduces the pending-stream-switch room to one. `tests/run_link_all.sh` does not compile
  `src/rec` (the recorder was compiled with the Xtensa toolchain by hand: 0 warnings).

### 9.24 Integration of the 2026-10-16 wave (§9.22 / §9.23): the merged tree, the suite, the builds (2026-10-16, evening)

Host and compile only (the device was not connected; nothing uploaded, nothing played, `sdcard/` content untouched apart
from what the runners write and remove themselves). The pinned API of §9.22 / §9.23 is unchanged by this pass.

- **Suite.** `sh tests/run_all.sh` is green on the merged tree: card_pull, desktop (19 + the 25 s selftest PASS), dsp_core
  26, engine 31, fx 33, generator 59, io 36, key_smf 43, library 59, link_all PASS, rec 17, sets 45, synth 46, ui 65,
  variants 32. `run_rec.sh` is picked up by the `run_*.sh` glob. `run_link_all.sh` now also compiles `src/ui` and
  `src/rec` (headers 33, one more link pass "engine+ui+rec", the ODR sniff over 73 shared inline functions, the Xtensa
  pass 22 objects, 0 warnings, largest frame still smfParse's 1 200 B; target text 328 443 B with the two added).
- **Library tree cache: the replacement policy** (`gen/library.cpp Library::folder`). The set-generate working set of the
  reference card is 29 trees as before, but since the content wave 18 of them are sharded (Chord/Lead/Pad/Bass Amin, Chord/C,
  Bass/C, Fx/Amin, Percussion/NA, Beat/NA, the seven drum roles, vocal Amin and NA; 3 … 13 steps each) and 2 are listing
  folders, in a 16-slot cache. The former policy — LRU among the slots cheap to rebuild while at least 1 + kRootsMax of them
  are cached, else plain LRU — fell back to plain LRU exactly when the sharded trees had filled the cache, and a sequential
  scan over more slots than the cache holds evicts every tree right before it is needed again: the second generate re-streamed
  all 112 steps / 238 KB (`test_library sd_budget_sharded_trees_survive_a_set_generate`, red since the content landed). The
  slot evicted is now the CHEAPEST to rebuild (`rebuildCost`), the least recently used among equals: measured second pass
  **19 steps / 33 KB** (the 11 flat or absent folders and the 2 listing walks one step each, plus the two or three cheapest
  sharded trees that do not fit beside the expensive ones); the pinning of the operation in progress is unchanged.
  `kCheapCost` is gone. On the device this is the difference between ≈ 100 extra 4 KB index reads per GENERATE and a handful.
  The proper fix — more slots (29 × 404 B = +5.2 KB) — does not fit the RAM budget; the cache stays 16.
- **Fixtures re-based to the card** (`tests/test_library.cpp`): `pickMidi_only_NA_when_key_folders_are_missing` and
  `pickMidi_returns_false_without_candidates` moved to the scratch card (every tonal category has every minor and major folder
  now; built under `lp/midi/Lead`, removed afterwards so no later scratch pick weighs them); C minor vocals accept the D# folder
  (8 phrases); E minor vocals: 2 500 picks over Emin (438, sharded, incl. `rap`) + G (1) + NA (174, sharded), every folder
  reached, the share within ±0.10, ≥ 97 % of each sharded tree drawn (437 / 438 and 174 / 174 with the seed — a stratified bag
  cannot promise the last file in a bounded run: a cycle that skips a stratum misses the file its rotation pointed at).
  `test_ui rush_key_on_a_loop_beat_track_crops_the_stream` expects the §9.22c len / 2 crop (clamped to one beat … the file);
  `test_generator bars_token_sets_the_vocal_window_of_a_stream` derives the restored phrase's window from its own `<n>bars`
  token (the card holds `1bars` OSH / RAPF phrases; the suite's seeded picks reach one after the policy change).
- **Two real fixes found by the integration tests.** (1) `Sets::init` now clears `name_` and `lastName_[]`: a Sets that is not
  value-initialised (a test World's member — the sketch's `new (ps) Sets()` zeroes it) started with a stale "last loaded" name
  and skipped that preset ("no immediate repeat", §9.8). (2) `Ui::loadSet` / `presetLoad` no longer stir the clock into the
  load Rng while a session is recorded or rendered (`rng_ = Rng(rng_.next() ^ (recording() || rendering() ? 0 : now))`): the
  Rng is seeded from `Recorder::sessionSeed()` at both starts (§9.23 review), but millis and renderNowMs differ, so a random
  LOAD inside a recording picked another set on replay. `test_rec a_random_load_inside_a_recording_picks_the_same_set_on_replay`
  records a load S1 started from the recorder page and checks the replay's `LOADED <name>` equals the live one (House_Amin_017
  both; before the fixes 017 vs 015). A recording started over serial (`rec start`) still does not re-seed the Ui's Rng.
- **Docs made consistent with ui.cpp / liveplayer.ino.** KEYMAP "RENDERING overlay" and MANUAL §12 said live keys still reach
  the pages during a render; they do not (only ` held and the SHIFT state, §9.22d / §9.23 fixes) — corrected, with the list of
  what a replay restores (seed, bags, genre, key, selection, rampBars, kick cuts, gyro) and what it does not (variants, len
  steps, loop mode, acid flags, pre-recording content, the preset sounding at render time; the "preset bag cannot be rewound"
  caveat is obsolete since `Generator::seed` restarts the bags), the skipped replayed writes, and the drive switch's `RENDERING`
  / `SETS BUSY` refusals (MANUAL §21). `ui.h` / `liveplayer.ino` comments said output volume default 200: `kOutputVolumeDefault`
  is 150 (59 %), as MANUAL / KEYMAP say; §3 above still reads 200 and is superseded by this line.
- **Builds (arduino-cli, esp32 3.3.12, no upload).** Player `…:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled`:
  **Sketch uses 902 719 bytes (27 %)**, **Global variables use 254 412 bytes (77 %)**, 73 268 B left for locals (before the
  library policy change: 902 723; the policy −68 B, the Sets / Ui fixes +64 B; globals unchanged throughout). Drive
  `…,USBMode=default`: **Sketch uses 566 423 bytes (16 %)**, **Global variables use 59 308 bytes (18 %)**. Static RAM delta
  of the player against §9.19f's 251 396 B: **+3 016 B** (+3 008 B against §9.19g's 251 404), inside the wave's 3 KB static
  allowance by 56 B. The same wave added **+904 B of boot heap** (Generator bag +704, Sets manager +200, §9.22c), so static +
  boot heap grew ≈ **3 920 B**, ≈ 0.85 KB over the brief's "3 KB in total" when both are counted; the §9.19f free heap of 13.1 KB
  (13 414 B) would then come down to ≈ 9.5 KB, under the 10 KB boot floor (`LOW RAM 9K FREE …` for 6 s at boot) and under
  `kLoopLowRamBytes` (10 KB: loop mode refused with `LOOP: LOW RAM`). Not measured — the device was never connected during the
  wave — and the first boot must confirm or refute it (`[boot] heap free=`). Levers if it holds: `Recorder::kBuf` 64 → 32
  (−256 B), the Ui's rows / shadows, the recorder's 512 B event buffer shared with `g_scratch`, `kLoopLowRamBytes` 10 → 9 KB
  only with a measured margin.
- **sizeof as the parts reported (host / device).** Engine 218 624 / 217 044 B (+1 608); SynthVoice 296 (unchanged);
  SynthPatch 128 (+8); Sets 12 432 / 12 384 B (+200, within the +256 B allowance); Generator 13 128 / 13 032 B (+704);
  Ui 2 192 B host (≈ 2 148 S3, ≈ +150); Recorder 704 host / 672 device (budget 700); Library 12 240 B (unchanged; FolderIndex
  404 B, 16 slots); TrackGenInfo 160; `g_jobOwner` +4 B static.
- **Card image after the runs.** `sdcard/lp/rec` and `/lp/wav` never existed (the recorder runners route them to mkdtemp cards
  and assert it); Testpack presets removed by their own cleanup; `sdcard/lp/state` is EMPTY — it held `session_set0.lpset` /
  `session_set1.lpset` before this pass, which `test_ui cleanup_session_files` removes on every run (they are what the desktop
  binary's session writer leaves behind; `run_desktop.sh` precedes `run_ui.sh` in `run_all.sh`, so a full run always ends with
  the folder empty). If those two files were wanted (a pulled device state), they are gone; a desktop run recreates the slot files.
- **Still open after this pass** (besides the RAM accounting above): the device-side items of §9.22f / §9.23 (nothing of this
  wave has run on the device: render speed, fsync cost, the parked task's silence feed, MSC enumeration, the knob layout);
  `Recorder::uiKey` sets `replaying()` and keeps `rendering()` true during a replayed call (the Ui's gates handle both forms);
  a session containing a preset DELETE / RENAME replays with the un-shrunk list (cursor-relative loads may pick another row);
  SPEC §3 (volume default 200), §9.17c (edit exits the synth page) and §9.19e (1-beat loop rush) are superseded by §9.22d /
  §9.22c / this section but left in place (append-only file); the sticker key pitch (5.8 mm) is derived, not measured;
  `run_library.sh`'s `index_entries_match_lines_and_files_corpus` and the Testpack fixtures assume no other runner writes the
  card at the same time (sequential runs only).

### 9.25 Theory set generator: SHIFT + load 1 / 2 writes the melodic tracks from one chord progression (user request 2026-10-16 16:55)

User (verbatim): "let's have shift + load 1 and shift + load 2 to generate a midis track based on music theory instead of
the midi library (use a pad chord progression and derive rhythms for chords, lead, bass...) based off of that. Use a
variety of possible patterns (100+) to basically generate a randomized set 1 or set 2. But again, in line with music
theory and regular patterns)." **Numbering:** the wave brief and the code comments call this section §9.24 and the
random-load cycle below §9.25 (`generator.cpp`, `variants.cpp`, `ui.cpp`, `sets.cpp`, the tests); in this file §9.24 is
the wave integration above, so the theory generator is **§9.25**, the no-repeat random load **§9.26**, the RAM diet with
the key remap **§9.27**. The pinned API: `Generator::generateTheorySet(int set)` / `theorySet(int set)`; the preset
manager API of `sets.h` and the Recorder of `rec/recorder.h` are unchanged. Host-side only: nothing was uploaded or played
during the wave; the device figures quoted are the orchestrator's boot measurement before the wave (§9.27).

- **Keys (UI, `ui.cpp handleMain`).** SHIFT + load 1 `(11,2)` → `requestGen(3, 0)` → `generateTheorySet(0)`; SHIFT +
  load 2 `(11,3)` → set 2. Kind 3 shares GENERATE's path: `SETS BUSY` when a save / load / listing owns the card (no job
  queued), `GEN BUSY` when the generator refuses, the edited flags of the set's patterns cleared at the request (a kept
  user loop stays), loop-mode tracks of the set blink while their new pick is in flight, `genTotal_` 8, the label
  `kTheoryLabelFmt` = `"THEORY S%d"` — the status reads `THEORY S1 0/8` until the queued job starts, `THEORY S1 n/8`
  while it runs, then `GEN DONE` or the generator's lastNote / lastError exactly as GENERATE. Replayed in an offline
  render like GENERATE (the recorder's re-seed reproduces the set). The desktop's `LP_UI_STUB` driver has the same
  binding; no serial command was added (`k`-sequences do it: `kd 13 3`, `k 11 2`, `ku 13 3`). The main-page footer does
  not name the progression: the status line carries the job, the editor shows the pattern's name, and
  `Generator::theoryProgression(set)` exists for a later footer segment.
- **Job (`generator.cpp`).** A new set job kind `JobTheory` with the state `TheoryPat`. `generateTheorySet(set)` **queues
  exactly like `generateSet`** (the review of the wave, §9.28: a first version refused while `busy()`): `startJob` draws the
  whole plan from `rng_` the moment the job starts (`theoryPlan`: progression, voicing template, chord / bass / lead rhythm
  templates, styles, melodic seed), and the request order — the same live and in the recorder's slower offline render,
  where a set job spans several times more audio — decides when, so the re-seed replays the set; a `busy()` refusal would
  have depended on that timing, and a render would have dropped the set and desynchronised every later draw. `queuePush`'s
  semantics give the double press its answer without a clock: a repeat for a set already queued coalesces (true, no second
  job), the very `JobTheory(set)` that is running refuses (false, the UI's `GEN BUSY`), as for GENERATE. The five melodic tracks (Chord, Lead, Pad, Bass, Fx — first in the set order) are
  built in the pure `TheoryPat` step (`theoryBuild` into the engine's source pattern, no card: measured 6 loop iterations,
  0 opens, 0 listings), then `Publish` → `swapMask` as in GENERATE; Beat, Percussion and Vocal run the normal library path
  inside the same job; one `CmdSwapSet` at the bar (test: 8 `CmdSwapPattern`, 1 `CmdSwapSet`, `CmdSetGenerating` on / off).
  Each melodic track gets `info().file = "THEORY <name>"`, `edited = 1` (so `sets.cpp`'s `saveTrackBegin` /
  `sessionTrackBegin` write the pattern as an SMF asset / session SMF and a theory set reloads exactly — `sets.cpp`
  untouched), `acidPat = 0`, a new variant seed, `keyRoot`, `lenStep` reset through `buildNext`, the source folder memory
  cleared (`sourcePath()` false → the session writer takes the SMF path). An acid-mode track under a theory job keeps its
  '303' sound and gets the theory line. `theorySet(set)` is set at the job's end (`theoryProg_[set]` = the `kProgs`
  index) and cleared by `startJob` of a JobSet on the set, a JobPattern on a melodic track, `loadMidiToSource` or
  `buildAcidSource` on a melodic track of the set. After a session restore or a preset load the pattern comes back from
  its SMF asset: `info().file` is then the asset name (not `THEORY …`) and `theorySet(set)` is false — by design; the
  notes reload exactly.
- **Music (`variants.cpp`, pure builders; tables `theory_tables.hpp`).** *Progression:* 126 minor-key progressions
  (`kProgs`, weights `kProgW`): 4 chords × 1 bar, 8 × ½ bar, 2 × 2 bars, and 2-bar loops (4 × ½ bar, 2 × 1 bar), with
  the borrowed major V, the dorian IV, the leading-tone vii°, sus4 / sus2 colours. Chord code = degree (0..6, natural
  minor i ii° III iv v VI VII) | alteration << 3 (`Alt`: none, major third, sus4, sus2, raised root); out-of-key pitch
  classes are allowed only inside the span of a chord whose alteration introduces them (`theoryAllowedPcs`), asserted
  over 1000 seeds. The pattern cap `kMaxPatternTicks` (4 bars) bounds 8-chord progressions to half a bar each — the
  brief's "8 chords × 1–2 bars" cannot fit a `Pattern`; 2-bar chords exist only in 2-chord progressions. *Pad:* voicings
  of the progression through 12 templates (triad / 7th / add9 / 6th / shell / octave, close or drop-2), the inversion per
  chord chosen for the least total voice movement inside 59..77 (measured 3.46 voices per chord, mean 1.20 semitones per
  voice, max 8), sustain / swell / pulse styles. *Chord:* the same voicings through 47 rhythm templates (2 bits per step:
  rest / onset / accent / hold) as stabs, broken chords or plucks, optional octave down; dense templates thin to 3 / 2
  voices so onsets never drop (≤ 192 notes). *Bass:* 34 templates (root / accent / octave / fifth / fifth below / approach /
  third / hold), 38..59, mono, fifths taken from the chord (a diminished chord keeps the flat fifth in key), ≈ 18 % slides
  (the note-off 12 ticks after the next note-on = legato glide). *Lead:* a scale-degree Markov walk (7 × 7 table,
  `kLeadStart`) over 44 templates, strong beats snapped to the chord's tones (the chord's borrowed note included), passing
  tones diatonic, 60..84, form A A' B A' (A A' on 2 bars) with six variation ops (sequence, ending, thin, octave, pickup,
  invert), ≈ 23–26 % accents (template downbeats + 8 % random). *Fx:* hits on the chord changes (+ riser / downbeat-middle
  styles). Register bands `kTheoryPadLo/Hi` 59 / 77, `kTheoryBassLo/Hi` 38 / 59, `kTheoryLeadLo/Hi` 60 / 84,
  `kTheoryChordLo/Hi` 36 / 79, `kTheoryFxLo/Hi` 60 / 84. Tunable constants: lead random accents `walkMotif` 0.08f, bass
  slides `buildBass` 0.20f.
- **Single source of truth (tools).** `tools/theory_data.py` holds `make_clips.py`'s PROGRESSIONS / ROMAN / CHORD_KINDS /
  CHORD_KIND_W / CHORD_RHYTHMS / LEAD_RHYTHMS / LEAD_MOVES / LEAD_HOME verbatim plus the theory additions
  (THEORY_PROGRESSIONS, VOICINGS, CHORD_RHYTHMS_EXT, BASS_RHYTHMS, LEAD_RHYTHMS_EXT, LEAD_MARKOV, LEAD_START,
  LEAD_VARIATIONS); `make_clips.py` imports them (adding its own directory to `sys.path`, since `-I` drops it) — verified
  byte-identical: the 36 Amin / Emin clips regenerated after the move match the card's GEN files (same name + hash, `cmp`
  identical). `tools/make_theory_tables.py` emits `gen/theory_tables.hpp` (constexpr → .rodata; `--check` exits 1 when
  stale, run by `tests/run_theory.sh`: "theory_tables.hpp up to date (progressions 126, voicings 12, chord 47, bass 34,
  lead 44)"). docs/CLIP_GENERATOR.md notes the move; docs/CREDITS.md names the fact source of the progression list
  (Hooktheory's public theory pages; names only, nothing copied).
- **Names.** `TrackGenInfo.file = "THEORY <name>"`, `<name>` the progression in ASCII roman numerals joined by `-`
  (`theoryProgName`; the fonts have no superscripts: `iidim`, `viidim`, `isus4`): `THEORY i-VI-III-VII`, `THEORY
  i-VII-VI-V`, `THEORY i-VI-viidim-i`, `THEORY i-isus4-i-VII`, `THEORY i-iv-VII-III-VI-iidim-V-i` (the longest, 25 + 7
  chars). No new `lp.*` file keys, no new `lastError` texts, no command-queue changes.
- **API beyond the pinned contract.** `generator.h`: NEW `int theoryProgression(int set) const` (`theory::kProgs` index
  while `theorySet(set)`, else −1); CHANGED `kPresetBagMax` 512 → **256** (`PresetBag::seen[]` 64 → 32 B, §9.27);
  `generator.h` now includes `variants.h`; private `TheoryPat`, `JobTheory`, `stepTheoryPat()`, `setJob()`,
  `theoryReplaced(int)`, members `TheoryPlan theory_; uint8_t theorySet_; int8_t theoryProg_[kSets]`. `variants.h` NEW:
  `struct TheoryPlan` (52 B, static_assert), `theoryCategory(Category)`, the band constants, `theoryPlan(Rng&, int keyRoot,
  TheoryPlan&)`, `theoryBuild(const TheoryPlan&, Category, Pattern&)`, `theoryProgName(const TheoryPlan&, char*, int)`,
  `theoryLengthTicks`, `theoryChordAt`, `theoryChordCode`, `theoryChordPcs(int root, uint8_t code, const uint8_t* stack,
  int n, uint8_t* pcs)`, `theoryAllowedPcs(int root, uint8_t code)`. `theory_tables.hpp` (generated): `namespace
  lp::theory { enum Alt; kMaxChords 8; struct Prog {n, beatsPerChord, chords[8]}; kProgCount 126, kProgs[], kProgW[];
  struct Voicing {n, open, stack[4]}; kVoicingCount 12; enum Step {StepRest, StepOn, StepAccent, StepHold};
  kChordRhythmCount 47 (uint32, 2 bits / step); kLeadRhythmCount 44; enum BassStep (BassRest … BassThird);
  kBassTplCount 34 (uint64, 4 bits / step); kLeadMarkov[7][7], kLeadStart[7]; enum LeadVar; kLeadVarCount 6;
  kRhythmTotal 125 }`.
- **Memory / flash (measured).** RAM: `TheoryPlan` 52 B + 3 B flags inside Generator (within the ≤ 200 B allowance);
  `PresetBag` 64 → 32 B per track (−512 B); sizeof(Generator) host 13 128 → **12 672 B** (−456 B of bootBlock heap; the
  test pins ≤ 12 672). No new statics (bss unchanged 2 856 B in the Xtensa partial link). Flash (Xtensa -Os): theory tables
  2 293 B .rodata (kProgs 1 260, bass 272, chord 188, lead 176, voicings 72, weights / Markov ≈ 325); theory code ≈ 6.8 KB
  .text (theoryBuild 2 070, buildLead 1 546, theoryPlan 1 274, theoryProgName 347, chordPcs / allowedPcs 510, generator
  wiring ≈ 440). Largest stack frame unchanged (1 200 B, smf.cpp); 0 warnings. The whole-sketch figure is §9.27's.
- **Tests.** NEW `tests/run_theory.sh` + `tests/test_theory.cpp` (7 cases, pure, no card, may run next to the card-bound
  runners): table counts / encodings / no duplicates, the A-minor chord classes (V, IV, vii°, sus, ii°), the names, 1000
  seeds × 5 categories (in key, bands, ≤ 192 notes, an onset at tick 0, valid, 4 variants non-empty, mono bass / lead,
  stabs on voicing tones, **0.0 % duplicate Chord patterns over 1000 sets**, all 126 progressions and every template used,
  determinism per seed, a key change moves pitches not choices), the lead form, the pad / fx / bass structure — measured in
  the run: "pad voices 3.46 per chord, voice leading: mean move 1.20 semitones per voice, max 8; lead accents 25.7 %; bass
  slides 17.8 % of note pairs". `test_generator.cpp` +3 (62): the job end to end (a repeat while queued coalesces, a bad set is
  refused, the running job refuses the double press; `theory_request_queues_behind_a_running_job_in_request_order`:
  GENERATE S1 + THEORY S1 queued while it runs ends as a theory set, THEORY then GENERATE as a library set, THEORY S1 +
  THEORY S2 as two independent theory sets, a queue of three keeps its order), the THEORY label, edited / lenStep / variant / keyRoot, in key, variant 0 == the active pattern after the swap, drums + kit +
  vocal in the same job, determinism across a re-seed, set 2 independent, GENERATE / gen pattern clear `theorySet`; the
  melodic tracks cost no card work and swap as one set); the size test bound 13 128 → ≤ 12 672 (+ `TheoryPlan` 52 B and
  `kPresetBagMax` 256 asserts). `test_ui.cpp` +1 (`shift_load_generates_a_theory_set`: the labels, the drawn status, the
  edited flags, `generating[]`, `GEN DONE`, a queued square's blink under the job, `SETS BUSY` while a load scans, the
  theory key during a running GENERATE leaving page and selection alone); `run_ui.sh` compiled a stand-in while
  `generator.cpp` lacked the job and now reports "theory generator: src/gen/generator.cpp".
- **Open.** The Pattern cap keeps 8-chord progressions at half a bar per chord (above). `theorySet(set)` is false after a
  restore / preset load although the notes are the theory's (by design). The footer does not show the progression (a
  follow-up if wanted: `theoryProgression(set)` / `info().file`). `run_link_all.sh`'s only warning is `variants.cpp`'s
  unused `nextOnsetStep`. Not auditioned on the device (host only).

### 9.26 Random load is a no-repeat cycle; the preset manager marks the loaded presets (user request 2026-10-16 17:00)

User: "if a preset is loaded randomly that it doesn't appear as an option until all other presets have been loaded (went
through the loading cycle)". Implemented in `Sets` (the preset manager API of §9.22c unchanged); the UI draws the dot. The
code comments call this §9.25 (see the numbering note of §9.25).

- **Rule (`sets.cpp`).** Per set, one bit per preset number of the current genre: `uint8_t loaded_[kSets][kLoadedPresets /
  8]` (`kLoadedPresets` 1024 → 128 B per set, 256 B in all), bit = NNN % 1024; a name without an NNN has no bit (always a
  candidate, never marked). `loadRandom` candidates = the matches of today's key rules (set index / current key) whose bit
  is clear; when nothing but the last loaded name is left, a **second listing pass** (`kFlagNewCycle`: index chunks or
  directory pages as before, still one bounded card operation per step, no extra RAM) clears the matches' bits as it offers
  them and draws again — the §9.8 "no immediate repeat" fallback stays, so a new cycle never starts with the preset just
  loaded. `finishLoad` of a random **or named** load (`beginLoadNamed`, the preset manager) sets the bit; a completed save
  clears the new number's bit in both sets (a new preset comes up in the running cycle, and its number may have been a
  deleted preset's); `deletePreset` clears the deleted number's bit in both sets; deleted presets drop out of the listing
  anyway. The masks belong to ONE genre (`loadedGenre_` = `Generator::genreIndex()`): `beginLoad` / `beginSave` /
  `beginPresetOp` sync them, and a load / save / preset op in another genre clears both (the old genre's progress is not
  kept); `presetLoadedInCycle` answers false while the masks belong to another genre than the current one.
- **Persistence.** `lp.loaded.<set>=<256 hex chars>` in each set's **session** file only (never in a preset file), written
  right after `lp.seq` and only when the masks belong to the file's genre; byte i = presets 8i..8i+7, bit (n & 7) = preset
  n; parsed only during a restore (a stray line in a preset file is ignored); a shorter or malformed value fills what it
  can up to the first non-hex character (`"ff0fzz00"` marks presets 0..11). Every bit change marks the session dirty
  while persistence is armed (tests that never arm it write nothing). The restore brings the file's masks back (`Sets::init`
  starts empty).
- **API (`sets.h`).** `static constexpr int Sets::kLoadedPresets = 1024`; `bool presetLoadedInCycle(int set, int nnn)
  const` (false for nnn < 0, a bad set, or another genre's masks); `void resetLoadCycle()` (both masks cleared; the hook
  for the recorder, cf. `Generator::seed` restarting the preset bags; since the review it also clears `lastName_[]`,
  the §9.8 fallback, and marks the session dirty under `setLoadedBit`'s rule — §9.28). `kSetLineMax` 1024 → 768 → 672
  (§9.27 / §9.28). No new `lastError` or status texts.
- **UI (`ui.cpp drawPresets`).** On the preset manager page a `kPresetDotSize` = 3 × 3 px square at x `kPresetDotX` = 2,
  row y + `kPresetDotDy` = 3, in the target set's colour (black on the inverted cursor row), for every row where
  `Sets::presetLoadedInCycle(target, Sets::parsePresetNumber(row.name))` is true (never for an unnumbered row); the row
  text moved from x 4 to `kPresetTextX` = 8. Because the sets owner's query had not landed when the UI work started, the
  call is resolved at compile time through ranked SFINAE overloads in `ui.cpp`'s anonymous namespace ((set, name) /
  (set, NNN) / (name) / none → no dot); the real `presetLoadedInCycle(int set, int nnn)` matched the (set, NNN) rank, so
  the dots are live against the real Sets. `static bool Ui::presetCycleMarks()` exposes the detection (tests / docs).
- **Recorder interaction (closed by the review, §9.28).** On the device a render after live random loads would see
  different masks than the recording did, so a replayed random load could pick another preset of the key.
  `Recorder::startRecording` and `Recorder::startRender` now call `Sets::resetLoadCycle()` beside `Generator::seed`
  (recorder.cpp): both masks, the genre tag AND `lastName_[]` of both sets are cleared, the session is marked dirty while
  persistence is armed (the next session write carries no `lp.loaded` line, which restores to empty masks), so the
  recording and its render start from the same Sets state and the replayed reservoir runs over the same candidates. UX
  cost, documented in MANUAL §10 / §12 and KEYMAP: REC ON (and a render) restarts the load cycle of both sets, the
  manager's dots vanish and the no-repeat names are dropped; the alternative — a 256 B mask snapshot in the `.lprec` —
  was declined for the RAM diet. `test_rec a_random_load_inside_a_recording_picks_the_same_set_on_replay` renders in
  the SAME World after `stopRecording` (the cycle bit clears at `startRender`, the Ui's `LOADED` name equals the live
  pick, `lastName()` equals it and the bit is set again), then in a fresh World that loaded a preset of its own first,
  then the old fresh-World case; `test_sets reset_load_cycle_makes_a_seeded_random_load_repeatable` (same seed → same
  pick after a reset, a different pick without one, the reset alone triggers a session write whose file carries no masks).
- **Tests.** `run_sets.sh` 45 → 47: `random_loads_cycle_through_every_preset_before_repeating` (five fresh
  House_Amin_950..954 copies listed alone: 5 loads distinct, the 6th starts a new cycle and is not the 5th — measured
  "sixth (new cycle) House_Amin_951 after House_Amin_953" —, the dots follow, a named load marks, `deletePreset` of an
  unloaded preset never blocks the cycle, deleting a loaded one clears its bit, a save's number is clear, a genre switch
  hides the dots and a switch back restarts) and `load_cycle_masks_round_trip_through_a_session_write_and_restore` (256 hex
  chars per set, RAM diverges after a load, the restore brings the file's mask back, the short hand-edited value). The
  fixture leaves `_index.txt` as found (`rewriteIndex`) and removes its files. `run_ui.sh` +1
  (`preset_manager_marks_the_random_load_cycle`: a dot on the loaded row, black on the cursor row, the set-2 target has its
  own marks, no dots when the query is absent). `test_sets.cpp` compiles with two `-Wunused-variable` warnings (`s` at
  :452 and :804) — harmless, sets owner.

### 9.27 RAM diet, the library's one-byte shard codes, and the key remap (2026-10-16, evening)

**Device measurement before the wave** (orchestrator, 20:05, the expansion build at commit 5ff0d8c): `[boot] heap
free=8768 largest=3060` — under the 10 KB boot floor (so the check shows `LOW RAM 8K FREE 2K MAX` for 6 s at boot) and under `kLoopLowRamBytes`
(loop mode refused with `LOOP: LOW RAM`); 8 284 B after the session restore. §9.24's prediction (≈ 9.5 KB) was in the
right direction and optimistic. Target of the diet: ≥ 12 KB free at boot with no audio change, by the levers below; the
orchestrator measures on the device after the wave.

- **[library] FolderIndex 404 → 268 B** (`gen/library.h`): `path[96]` → `kFolderPathLen` **56** ("/lp/packs/<genre>/loops/
  percussion/A#min" leaves 22 chars for a genre name; vocal / drum / midi roots 32; a longer path is not cached and never
  picked from), the 12 × 8 B shard names → **12 one-byte codes** (`uint8_t kids[kTreeMax]`), the Bag's five ints → 16-bit
  (`int16_t count_ / pos_ / last_ / cycleLen_`, `uint16_t cycle_`; 148 → 138 B; `cycle()` wraps at 65 535, the public Bag
  API unchanged). sizeof(Library) host 12 240 → **10 064 B** (−2 176), device (Xtensa, by symbol size) 12 240 → **10 060 B**
  (−2 180; bootBlock heap). `kFolderSlots` stays **16** with the cheapest-to-rebuild replacement of §9.24, so the sd budget
  is unchanged — `sd_budget_sharded_trees_survive_a_set_generate`: set generate working set first (cold) pass 112 prepare
  steps / 238 210 B, second (warm) pass 19 steps / 32 968 B, before and after. **Decision left open:** 24 slots × 268 B =
  6 432 B would fit inside the OLD 16 × 404 = 6 464 B (the §9.24 option, a warm generate then re-streams nothing) but
  costs +2 144 B against the diet; 20 slots would be +1 072 B. *Why codes instead of plain `sNN`:* the listing folders
  (categories, `loops/<folder>`) keep KEY folder names in `kids` ("Amin", "A#min", "C", "NA"), and the card has one
  deliberate non-sNN shard, `/lp/packs/House/vocal/Emin/rap` (20 phrases, docs/RAP_BANK.md: a sharded key folder cannot
  take media beside its shards). Code space: `kKidShards` 100 (0..99 = "s00".."s99"), `kKidKey` 100 (+0..11 the minor
  labels, +12..23 the major labels, `kKidNA` 124 = "NA"), `kKidKeys` 25, `kKidNamed` 125 + i = `kNamedShards[i]`
  (`{"rap"}`, `kNamedShardCount` 1), `kKidNone` 255; `int folderKidCode(const char*)` (−1 = no code: the walk drops the
  subfolder), `void folderKidName(uint8_t, char out[kTreeNameLen])`. `kTreeNameLen` stays 8 but now means "a DECODED
  subfolder name + terminator" (`kEntryLen` unchanged). **Behaviour change:** a subfolder of a walked folder is kept only
  when it has a code (sNN, a named shard, or a key label / NA for a listing folder) — before, any name shorter than 8
  chars was kept; others count in `kidsDropped` and, for content folders, in `lastError()` "<tail>: N shards unreachable".
  The real card's walks meet only coded names (verified over midi, loops, drum, vocal). Tests: two scratch fixtures that
  used shard names `s0` / `s1` and `a` / `b` renamed to sNN, the "names up to 7 chars" test became the rap / uncoded-names
  test, a new test round-trips every code (`run_library.sh` 59 → 60; "sizeof(Library) = 10064 B, sizeof(FolderIndex) =
  268 B, sizeof(Bag) = 138 B"). A new tool-made shard name would need an entry in `kNamedShards`; `tools/write_indexes.py`
  (not changed) still warns by name length and should warn about any shard subfolder that is not sNN / a named shard and
  any listing subfolder that is not a key label / NA (those are unreachable regardless of length).
- **[generator] PresetBag 64 → 32 B per track** (`kPresetBagMax` 512 → 256; banks are ≤ 162 per family, §9.22b): −512 B;
  with the theory state (+56 B) sizeof(Generator) 13 128 → **12 672 B** (−456 B of bootBlock heap, §9.25).
- **[recorder] `Recorder::kBuf` 64 → 32** (`rec/recorder.h`): the write-behind buffer 512 → 256 B; a full buffer still
  flushes inline in `onInput` (one write + sync, ≤ 256 B), `step()` does at most one bounded write, and the render reads
  the session file in **32-event chunks** (one transient open / seek / read / close per chunk; a chunk boundary inside a
  step's due events behaves as before, `kOpenRetrySteps` included). The brief's second recorder lever, `names_[64][8]` →
  a directory walk (−512 B), was **already in the tree** before the 8 768 B measurement (§9.23 "Session list": two 64-bit
  masks, no name cache), so the recorder's NEW saving is −256 B only, not the brief's −768 B. Measured sizeof(Recorder):
  host 704 → **448 B**, device (Xtensa 2601, -Os) 672 → **416 B**; `static lp::Recorder g_recorder` is .bss, which on the
  S3 comes out of the same DRAM as the heap → +256 B of boot heap. Budget: static_assert and `recorder_fits_its_budget`
  pin ≤ 448 B device-equivalent (host 448 + 7 × (sizeof(void*) − 4)) and `CHECK_EQ(Recorder::kBuf, 32)`. recorder.cpp text
  6 125 B, largest frame unchanged, 0 warnings; `run_rec.sh` 17 / 17 (`a_full_buffer_of_kBuf_events_…` in terms of kBuf;
  `a_failed_chunk_open_…` with kChunks = ⌈80 / kBuf⌉ = 3). Cost: a dense performance flushes every ≤ 32 events instead of
  64 (up to twice as many, smaller writes / syncs while the stream gate stays closed); the one-op-per-step contract holds.
- **[sets] `kSetLineMax` 1024 → 768** (`PropertiesReader` host 1 048 → 792, −256; the longest kept line is a 4-path kit
  line ≤ 658 B, `lp.loaded.N` 268 B). The §9.26 masks add `loaded_[2][128]` + genre + a counter (+259 B), so sizeof(Sets)
  host 12 432 → 12 432 (unchanged), device 12 384 → **12 388 B** (+4): the sets lever is spent on the cycle. **Review
  (§9.28): the reserve levers were taken** — `kSetLineMax` 768 → **672** (−96 B, `static_assert kSetLineMax >= 4 *
  kMaxPathLen + 19`: the kit line still fits), `SetFile::genre` 96 → **24 B** (`kGenreNameLen`, −72 B), and in the library
  `genres_[8][96]` → `[8][24]` (−576 B; `scanGenres` skips a pack folder of ≥ 24 chars and reports it as "packs: N name(s)
  over 23 chars" — such a pack's loop paths could not have been cached in `kFolderPathLen` 56 anyway). Final sizeof: Sets
  host 12 264 / device **12 220 B** (−164 against 5ff0d8c), Library host 9 488 / device **9 484 B** (−2 752).
- **[sketch] boot floor 10 KB → 8 KB** (`liveplayer.ino`: `freeB < 8 * 1024 || largest < 6 * 1024`, the `LOW RAM %uK FREE
  %uK MAX` text unchanged) and **`kLoopLowRamBytes` 10 KB → 6 KB** (`core/config.h`): an open card file costs no heap since
  the POSIX SD layer; the only runtime heap users are FatFs' LFN scratch, ≈ 1 KB per open.
- **Expected boot heap** (computed from the measured sizeof deltas; the device figure is the orchestrator's to take):
  8 768 + 2 180 (library) + 456 (generator) + 256 (recorder) − 4 (sets) = **≈ 11 656 B (11.4 KB)** — about 0.6 KB short
  of the ≥ 12 KB target, because the recorder's −512 B was already spent and the sets lever paid for the cycle masks. The
  largest free block (3 060 B) should grow by about the library's share, since the bootBlock arena shrinks at its end.
  Both floors are met with margin (8 KB boot, 6 KB loops). Levers left if the measurement confirms the shortfall, each
  with its cost: `kSetLineMax` 768 → 672 (−96 B; still ≥ the 658 B kit line), the recorder's 256 B buffer shared with
  `g_scratch` (−256 B, the write-behind then competes with `cat` / `get`), the Ui's row shadows, `Sets::names_` (16 × 85 B
  of the 2 048 B page union), fewer `kLoadedPresets` (512 → −128 B; preset numbers wrap sooner). **After the review's
  levers (§9.28, Xtensa 2601 -Os against the same probe built from 5ff0d8c):** Library 12 236 → 9 484 (−2 752),
  Generator 13 032 → 12 568 (−464), Sets 12 384 → 12 220 (−164) — bootBlock heap —, Recorder 672 → 416 (−256, .bss):
  8 768 + 2 752 + 464 + 164 + 256 = **≈ 12 404 B (12.1 KB)**, ≈ 116 B over the 12 288 B target on paper; the device
  measurement decides. Next levers if it falls short: `kSetLineMax` 672 → 664 (−8, the floor is 659), `Library::warn_`
  48 → 32 (−16), `kFolderSlots` 16 → 14 (−536 B, at the sd budget's cost), `variants.cpp`'s `gScratch` (772 B .data).
- **Key remap (UI; the user's request of §9.25 and the previous wave's SHIFT + save 1 recorder).**
  - **SHIFT + load 1 `(11,2)` / load 2 `(11,3)` = THEORY generate** set 1 / 2 (§9.25).
  - **SHIFT + save 2 `(10,3)` = PRESET MANAGER** (was SHIFT + load 1 / 2 in §9.22d); the same combo on the page closes
    it; the plain save 2 is unchanged; a replayed SHIFT + save 2 opens the page as live (its delete / label still write
    nothing). **SHIFT + save 1 `(10,2)` = RECORDER** (unchanged). `openPresets(now)` is a plain toggle.
  - **Target set on the page:** any key of track row y0 except `` ` `` — `(1–7, 0)` — sets target SET 1, any key of row
    y1 `(0–7, 1)` target SET 2, with or without SHIFT (`presetTarget(set)`, driven from `handlePresets` by any `KTrack`;
    `(0,0)` is checked first as the exit). A toggle keeps the list, cursor and filter (the rows are the genre's), repaints
    the header and the page colour (green = set 1, cyan = set 2), pushes no engine command; the target is kept across
    opens like the filter and AUTO (default SET 1 at boot; `Ui::presetSet()`). Header `"-> SET n  <ALL | X MIN>  <count |
    ...>[  AUTO]"` — the contract's "→ SET n" is drawn as ASCII `->` (`kPresetTargetPrefix`): neither the device's
    built-in font nor the desktop 5 × 7 font has U+2192 (the `LOOP ON - 125 BPM` hyphen precedent). **ent** loads into
    the target (`Sets::beginLoadNamed(target, …)`, `LOAD Sn k/8`, `LOADED <name>` / the loader's error); with AUTO the
    `CmdPlaySet` follows the target. **del**'s `DELETE? PRESS AGAIN` is bound to the row, the target set and the filter (a
    target toggle inside the 2 s window re-arms). SHIFT + load 1 / 2 are not live on the page (modal: no theory job, no
    set switch); while RENAME types they are swallowed like every non-legend key. Unchanged texts: `DELETED`, `DELETE
    FAILED`, `LISTING...`, `WRITING LABEL...`, `DELETING...`, `LABEL SET`, `LABEL FAILED`, `NO PRESETS`, the footer help.
  - Memory: no new Ui members, sizeof(Ui) 2 192 B (budget 2 700); the new constants are flash strings (`kPresetTextX` 8,
    `kPresetDotX` 2, `kPresetDotDy` 3, `kPresetDotSize` 3, `kPresetTargetPrefix "->"`, `kTheoryLabelFmt "THEORY S%d"`);
    no heap, nothing on the audio thread. Private `openPresets(uint32_t)` replaces `openPresets(int set, uint32_t)`; new
    `presetTarget(int)`, `presetRowInCycle(const Sets::PresetRow&) const`; `requestGen` kind 3.
  - Tests (`run_ui.sh` 65 → 67, all against the real theory job and the real Sets query): `preset_manager_lists_filters_
    and_loads` rewritten for SHIFT + save 2 (the target toggle with / without SHIFT, header text + colours, no engine
    commands, no re-listing, a load into SET 2 with AUTO → `CmdPlaySet a=1`, the target kept across opens, SHIFT + load
    not live on the page); delete / rename, load-failure and delete-confirm tests moved to the new combo (the confirm
    window re-arms on a target toggle); the render test checks that live SHIFT + save 2 / SHIFT + load 1 reach nothing
    during a render. The §9.26 dot test and the §9.25 theory test are the other two new cases.
  - Found on the way (generator stream gate, open): in `test_ui`, with set 1 playing (its Vocal streaming), the preset
    manager's re-listing after a load into set 2 stalled > 3 000 blocks (≈ 35 s) because `Generator::streamsHealthy()`
    stayed false — stream 0 sat at a constant 3 328 samples (< `kGateLeadSamples` 3 748), neither drained nor topped up,
    then flipped into a 3 584 / 3 840 regime where the gate opened every ≈ 25 blocks. Sets' card work (listing, label,
    delete, session write) can therefore wait long while a vocal plays. The test now stops set 1 at the bar before that
    load and keeps a failure-only diagnostic block; the gate / pump behaviour belongs to the generator owner.
- **Docs and stickers (this section's owner).** docs/KEYMAP.md regenerated key by key from the final `ui.cpp` (the SHIFT
  rows for `(11,2)`, `(11,3)`, `(10,3)`, the PRESET MANAGER page with the target rows and the dot, the random-load cycle
  on the load keys, the serial `k`-sequence for a theory set); docs/MANUAL.md §2, §3, §9 ("Generate: library or
  theory"), §10 (the cycle), §11 (SHIFT + save 2, target, dot, `-> SET n`), §12 (replay caveat), §16, §21 (the drive-mode
  facts below), §23, §24; docs/CREDITS.md (the music-theory references); docs/CLIP_GENERATOR.md (the tables moved to
  `tools/theory_data.py`); `tools/make_stickers.py` legends `(11,2)` LOAD1 → SHIFT **THRY1**, `(11,3)` LOAD2 → **THRY2**,
  `(10,3)` SAVE2 → **PRSET**, and docs/stickers/keys-main.svg / .pdf, keys-shift.svg / .pdf regenerated (built-in PDF
  writer: neither cairosvg nor rsvg-convert is installed on this Mac).
- **Device facts of the wave (orchestrator, 20:15; for MANUAL §21 / CARD_IMPORT C).** `tools/flash_drive.sh` wrote the
  drive firmware to ota_1 and the player came back. The serial `drive` printed "[drive] ota_1 holds an app (arduino-lib-
  builder 6671d0b): switching", the device rebooted into the drive firmware and enumerated on the Mac as
  "M5STACK_CARDPUTER" (Espressif 0x303a:0x1001) with a "TinyUSB MSC" interface (class 8 / subclass 6) that macOS matched
  with IOUSBMassStorageDriver — yet no disk appeared in `diskutil list`, because this Mac's device-management policy (external disk mounting disabled) suppresses it: the expected symptom on a managed computer (USB shows the device, no
  volume mounts; use `tools/card_pull.py` or another computer). The first `drive` attempt on a freshly booted player was
  refused silently while the session writer was busy (the 3 s wait) — "try again a few seconds later" is now in the
  manual. The serial `player` on the drive's own port returns to the player. MSC enumeration is thereby verified; the
  mount, throughput and the otadata round trip remain unverified on an unmanaged host (§9.22f item 5).
- **Suite on the merged tree** (`sh tests/run_all.sh`, green, exit 0): card_pull PASS; desktop 19 + the selftest PASS;
  dsp_core 26; engine 31; fx 33; generator **61**; io 36; key_smf 43; library **60**; link_all PASS (33 headers standalone,
  ODR sniff 73 shared inline functions 0 differing, Xtensa 22 objects 0 warnings, target text **340 984 B**, data 1 572 B,
  bss 2 856 B, largest frame 1 200 B smf.cpp); rec 17; sets **47**; synth 46 (presets_gen.hpp in sync); **theory 7** (new);
  ui **67**; variants 32. `run_theory.sh` is pure and may run beside the card-bound runners; every other runner that
  touches `sdcard/lp` runs sequentially (`run_all.sh`). Card image after the run: `sdcard/lp/packs/House/presets` holds the
  19 presets that existed before the wave (left alone), `/lp/state` is empty (run_ui's cleanup, §9.24); `/lp/rec`, `/lp/wav`
  never created (mkdtemp cards). `write_indexes.py --check` warns that `lp/midi/Beat/NA` holds 12 subfolders (the kTreeMax
  limit: a further shard would be unreachable) — pre-existing.
- **sizeof as the parts report (host / device).** Library 10 064 / 10 060 B (FolderIndex 268, Bag 138; was 12 240 / 404);
  Generator 12 672 host (was 13 128; device not re-measured); Sets 12 432 / 12 388 B (PropertiesReader 792, PresetRow 85,
  max step 1.54 ms); Recorder 448 / 416 B (budget 448; was 704 / 672 / 700); Ui 2 192 B (unchanged); Engine 218 624 B
  (unchanged); TheoryPlan 52 B. **Final figures after the review (§9.28, host / device):** Library 9 488 / **9 484**
  (genres_ 8 × 24); Generator 12 672 / **12 568**; Sets 12 264 / **12 220** (PropertiesReader 696 / 688, SetFile 4 088);
  Recorder 448 / **416**; Ui 2 192 / 2 152; FolderIndex 268; TheoryPlan 52.
- **Statements of earlier sections superseded by this wave** (append-only file; the earlier text stays): §3, §9.13, §9.19f,
  §9.22e and §9.24 "boot floor free ≥ 10 KB" → **8 KB** (largest ≥ 6 KB unchanged); §9.19 `kLoopLowRamBytes` 10 KB and
  "`LOOP: LOW RAM` under 10 KB" → **6 KB**; §9.22c `PresetBag seen[64]` / `kPresetBagMax` 512 and sizeof(Generator)
  13 128 → `seen[32]` / 256 / 12 672; §9.22d "SHIFT + load 1 / load 2 = PRESET MANAGER of that set … the other set's
  combo switches the page" → SHIFT + save 2 with the target set; §9.23 "buffered 64 deep (512 B)", "64-event chunks",
  "sizeof(Recorder) = 672 B (static_assert ≤ 700, host ≤ 728)", "`static lp::Recorder g_recorder` (672 B)" → 32 / 256 B /
  32-event chunks / 416 device, 448 host, budget 448; §9.24's sizeof table (Recorder 704 / 672, Library 12 240 with
  FolderIndex 404, Generator 13 128 / 13 032, Sets 12 384 device) → the table above, and its "the proper fix — more slots
  (29 × 404 B = +5.2 KB) — does not fit" → 24 × 268 B would now fit the old footprint but is declined for the diet; §9.24's
  "Levers if it holds" → applied as listed; §10.3 / `library.h`'s "shard names up to 7 chars" → one-byte codes (sNN, a
  named shard, key labels, NA); `sets.h` `kSetLineMax` 1024 → 768 (→ 672 by the review, §9.28); §9.8 / §9.22c random load
  "no immediate repeat" → the §9.26 cycle with that fallback; §9.18's session keys gain `lp.loaded.<set>` (session files
  only).

### 9.28 Integration of the §9.25–§9.27 wave: the review fixes, the merged tree, the suite, the builds (2026-10-16, night)

Host and compile only (nothing uploaded, nothing played, `sdcard/` untouched apart from what the runners write and remove
themselves, no git state changed). The pinned API of §9.25 (`Generator::generateTheorySet(int set)` / `theorySet(int set)`,
the preset manager API of `sets.h`, `rec/recorder.h`) is unchanged in signature; three behaviours of the parts changed
after their review and the sections above were corrected in place where they described the first version (the Job
paragraph of §9.25, the recorder interaction and the API line of §9.26, the sets lever, the expected heap, the sizeof table
and the superseded list of §9.27).

- **Review fix 1 — the theory request queues** (`generator.cpp generateTheorySet`, theory owner). The first version
  returned false while `busy()` (running or queued), unlike `generateSet`; since the plan is drawn in `startJob` and the
  queue runs requests in order whatever the clock does, a live session and its slower offline render draw the same plan
  only if the request is QUEUED — a `busy()` refusal depended on timing and a render would have dropped the set and
  desynchronised every later draw. Now `generateTheorySet` is `queuePush(JobTheory, set)` like GENERATE: a repeat for a
  set already waiting coalesces (true), the very job running refuses (false → the UI's `GEN BUSY`), a full queue (4) or a
  bad set refuses. Tests: the first theory test's expectations, and the new
  `theory_request_queues_behind_a_running_job_in_request_order` (GENERATE S1 + THEORY S1 while it runs → a theory set, 422
  steps; THEORY then GENERATE → a library set; THEORY S1 + THEORY S2 → two independent progressions; three queued requests
  keep their order). Honest limit: the fixture cannot prove "the same set whether the key came one bar earlier or later"
  exactly (the preset bags and the Library's pick history are not resettable); the per-seed replay is the existing
  determinism test. Residual shared with GENERATE: a second press for the SAME set that lands inside the running job's
  span is a no-op live (≈ 0.17 s) but also inside the render's longer span (≈ 2 s of audio); a press between those windows
  is accepted live and refused in the render — exact replay of same-set double presses would need the running-job
  refusal to become a queued repeat for both. `ui.cpp` needed no change (`requestGen` kind 3); `GEN BUSY` now appears only
  for the residual refusals; KEYMAP, MANUAL §9 / §23 corrected.
- **Review fix 2 — vii° octave voicings** (`variants.cpp theoryChordPcs`, `tools/theory_data.py chord_pcs_theory`): stack
  offsets are taken modulo 7 before the alteration tests, so the octave offset 7 of the oct-triad / fifth-oct / 7th-no5
  templates carries the raised root (G# B D G# → {8, 11, 2}) instead of the natural G; the ninth offset 8 maps to degree 1
  and is unaffected. The generated header is unchanged (`make_theory_tables.py --check` up to date). `test_theory` gained
  the vii° / V / i voicing classes and an exhaustive sweep (every voicing template of every chord code stays inside
  `theoryAllowedPcs`); the 1000-seed statistics are unchanged to the printed precision.
- **Review fix 3 — the recorder resets the load cycle** (`sets.cpp resetLoadCycle`, `recorder.cpp startRecording` /
  `startRender`; §9.26 "Recorder interaction" closed). `resetLoadCycle()` clears both masks, the genre tag AND
  `lastName_[]` of both sets and marks the session dirty under `setLoadedBit`'s rule; the recorder calls it beside
  `Generator::seed` at both starts. UX cost: REC ON and a render restart the load cycle of both sets (the manager's dots
  vanish, the no-repeat names are dropped); documented in MANUAL §10 / §12, KEYMAP (RENDERING overlay). Two scratch
  negative builds showed the test catches both halves of the fix (017 live vs 015 render).
- **RAM diet, final levers** (librarysets owner): `genres_[8][96]` → `[8][kGenreNameLen = 24]` (−576 B; `scanGenres` skips
  and reports a pack folder of ≥ 24 chars: "packs: N name(s) over 23 chars"), `kSetLineMax` 768 → **672** (−96 B,
  static_assert ≥ 4 × kMaxPathLen + 19), `SetFile::genre` 96 → 24 B (−72 B). `kFolderSlots` stays 16 (the brief's 24-slot
  idea conflicts with the diet; sd budget unchanged: cold set generate 112 steps / 238 210 B, warm 19 / 32 968 B).
- **sizeof, Xtensa 2601 -Os, one probe object built against this tree and against 5ff0d8c (`git archive`):**
  Library 12 236 → **9 484** (−2 752); Generator 13 032 → **12 568** (−464); Sets 12 384 → **12 220** (−164);
  Recorder 672 → **416** (−256); Ui 2 152 (unchanged); FolderIndex 404 → 268; PropertiesReader 1 040 → 688; SetFile
  4 160 → 4 088; TheoryPlan 52. Host (clang): Generator 12 672, Library 9 488, Sets 12 264, Recorder 448, Ui 2 192.
  **Projected boot heap** = 8 768 + 2 752 + 464 + 164 (bootBlock arena) + 256 (.bss) = **≈ 12 404 B (12.1 KB)**, ≈ 116 B
  over the 12 288 B target on paper — thin; the device measurement (`[boot] heap free=`) decides. The largest free block
  should grow by about the library's share. Nothing else grew RAM: the theory state is 55 B inside Generator, its tables
  are .rodata.
- **Builds (arduino-cli, esp32 3.3.12, no upload).** Player `…:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled`:
  **Sketch uses 913 919 bytes (27 %)**, **Global variables use 254 156 bytes (77 %)**, 73 524 B left for locals. Static
  RAM delta against §9.24's 254 412 B (commit 5ff0d8c): **−256 B** — the recorder's `.bss` lever; the other levers are
  bootBlock heap and show only on the device. Flash +11 200 B (theory tables ≈ 2.3 KB .rodata, theory code ≈ 6.8 KB, the
  cycle / codes / queue wiring). Drive `…,USBMode=default`: **Sketch uses 566 423 bytes (16 %)**, **Global variables use
  59 308 bytes (18 %)** — unchanged from §9.24.
- **Suite on the final tree** (`sh tests/run_all.sh`, sequential, exit 0): card_pull PASS; desktop 19 + the 25 s selftest
  PASS (the desktop binary rebuilt by `run_desktop.sh`); dsp_core 26; engine 31; fx 33; generator **62**; io 36; key_smf
  43; library **61**; link_all PASS (33 headers, 23 objects, ODR 73 shared inline functions 0 differing, Xtensa 22 objects
  **0 warnings**, target text **341 274 B**, data 1 572 B, bss 2 760 B, largest frame 1 200 B smf.cpp); rec 17; sets **48**
  (the two `-Wunused-variable` warnings of `test_sets.cpp` removed); synth 46; theory 7; ui 67; variants 32. One flake
  seen once in the first full run and not reproduced: `test_generator step_budget_and_size` measures the host WALL time of
  every `step()` of the binary against `kMaxSdBurstMs` (20 ms) and saw a 28.11 ms outlier (identical card meters: 39 984
  iterations, 434 skipped, max 16 384 B / 2 opens per iteration); the runner alone measured 1.59 ms, the second full run
  passed. A disk / scheduling stall of the host (the volume is 98 % full), pre-existing check (5ff0d8c); the card-meter
  checks next to it are the contract. `run_theory.sh` is picked up by the `run_*.sh` glob.
- **Docs made consistent with the code** (this section's owner): KEYMAP (the THEORY row's refusals, the RENDERING
  overlay's random-load paragraph), MANUAL §9, §10, §12, §23 (queueing; the cycle restart at REC ON / render),
  CARD_IMPORT C (the drive-mode device facts), SPEC §9.25–§9.27 as listed above. Cross-check of KEYMAP / MANUAL against
  `ui.cpp` (`handleMain`, `handlePresets`, `handleRecorder`, `requestGen`, `openPresets`, `presetTarget`) and
  `liveplayer.ino`'s serial dispatcher: every binding, text and refusal quoted matches; the main-page footer indeed does
  not show the progression (`theoryProgression(set)` exists for a later footer segment).
- **Still open after this pass:** the device measurement of the boot heap (≈ 12.4 KB projected, 116 B of paper margin;
  next levers listed under §9.27 "Expected boot heap"); the sound of the theory sets, the 3 × 3 px dot, the `->` header
  glyphs and the sticker pitch are not auditioned / seen on the device; the generator's stream gate finding (§9.27: Sets'
  card work can wait tens of seconds while a vocal streams) belongs to the generator owner; the 16-slot folder cache still
  re-streams part of a set generate's working set (§9.27 decision); `tools/write_indexes.py` should warn about uncoded
  shard subfolders and about pack folders of ≥ 24 chars (both unreachable now); a reader of the session files should know
  that an empty cycle appears either as an all-zero `lp.loaded.N` or as no line at all (after a reset); the code comments
  cite the theory generator as §9.24 and the cycle as §9.25 (the numbering note of §9.25); `theorySet(set)` is false after
  a restore / preset load although the notes are the theory's (by design).

### 10.5 Device results of the expansion (2026-10-17, merged main d6f7d30, output volume 0 throughout)

- Card sync over serial (firmware/cardloader + tools/card_sync.py): the loader hung twice (no reply to PING after
  ≈ 1 700 and ≈ 1 100 files; cause not found — the FAT create of a new shard folder is suspected); a reflash +
  resume completed the rest: 8 395 files copied, 21 929 already matching, 1 071 MB in 79 min, 2 544 stale copies
  pruned (the merge re-sharded folders in name order), 8 random CRC checks clean, presets / sets / state untouched;
  rec / wav are now protected folders of the sync.
- Final build: `[boot] heap free=12864 largest=7668`, 12 376 B after the session restore (floor 8 KB, loop guard
  6 KB, no LOW RAM); the restored session referenced a vocal file that moved during re-sharding → `vocal open`
  error once, cleared by the next generate (the fallback pick).
- THEORY set (SHIFT + load 1 over serial keys): chord / lead / pad / bass became "THEORY i-VII-VI-V" with new
  bank sounds (Keys Opal, 303 Nasty, 303 Honey, Sub Wax), drums / percussion / vocal regenerated in the same job,
  an Fx track in loop mode kept its loop; heap unchanged.
- PRESET MANAGER (SHIFT + save 2, row-1 key for the set-2 target, backtick exit): opened and closed, no fault.
- RECORDER: `rec start`, keys, `rec stop` (4 events), `rec render 0` → 100 % in ≈ 20 s for 12.9 s of audio,
  /lp/wav/rec_000.wav pulled with tools/card_pull.py in 1.2 s: RIFF 22050 Hz mono 16-bit, 570 412 B, peak 1 095
  — quiet because the serial `play` is not a key event, so only the Pad toggled by a key sounded on replay (a real
  performance uses keys and is captured fully); 768 vocal underrun samples at the render start (stream not primed).
- USB drive mode: enumerates as a TinyUSB MSC (class 8/6) on the Mac, suppressed by this Mac's management policy.

### 9.29 Device-native presets: the REAPER export removed (user request 2026-10-27)

User: "remove any of the reaper / export features". What goes is the REAPER-compatible set export of §6 / §9.8: the
`set_presets.lua` parser contract as the WRITER's contract (`track.N.clipType`, `track.N.clip`, `track.N.slot`,
`track.N.beatLayer`, `track.N.beatKickMuted`), the byte-for-byte copies of library MIDI, vocal WAV and loop WAV files
into `presets/assets/` (the `SvOpenDst` / `SvCopy` states, the `assets/` mkdir), docs/REAPER_IMPORT.md, and every "import
into REAPER" / "copied for REAPER" wording in README, MANUAL, KEYMAP, CARD_IMPORT, GETTING_STARTED and CREDITS (the
"inspired by the REAPER Live Player" credit stays). What stays: presets, the preset manager (§9.22c), sessions (§9.18),
`lp.label`, the random-load cycle (§9.26), the recorder's WAV render (§9.23: a user feature of 2026-10-16, not an export),
and the names — the `.lpset` extension and `<Genre>_<Key>_<NNN>.lpset`. This section overrides §6 (the `sets/assets/*.mid`
line and the saved-set example), §9.8, §9.19 §1.7 and the §10.1 known limitation where they differ; the earlier text is
kept as the record of the format the legacy reader still accepts. Written by the docs owner from the brief
(`w8_reaper_removal_brief.md`); the key spellings and the session-only list below were pinned in place from the sets
result at the integration (§9.30), which holds the measured sizes.

- **The preset file** `/lp/packs/<Genre>/presets/<Genre>_<Key>_<NNN>.lpset` is written in the SESSION format of §9.18:
  Java-properties text, ASCII, forward slashes, `lp.*` keys only — `schemaVersion=1`, `setIndex`, `keyRoot`, `keyName`,
  `genre`, `lp.label` (when set), `lp.banks`, `lp.bpm`, and per track `lp.src.N` = the ABSOLUTE source path of the
  pattern, vocal phrase or loop (`/lp/midi/…`, `/lp/packs/<Genre>/vocal/…`, `/lp/packs/<Genre>/loops/…`; `acid` for a
  generated line, rebuilt from `lp.track.N.seed` + `.key`) plus the `lp.track.N.*` keys a session carries (`preset`,
  `seed`, `len`, `lenk`, `key`, `pause`, `kit`, the §9.17b strip and knobs `vol dist filter echo reverb cutoff res env
  decay drive wave acid`, the §9.22 `sat accent chorus`, `loopmode`) and the variant and the Beat's kick cut as
  `lp.track.N.variant=0..3` and `lp.track.7.kickcut=0|1` (the reader still takes the legacy `track.N.slot`,
  `track.7.beatLayer` and `track.7.beatKickMuted=true`). The writer never emits `clip=`, `clipType`, `track.N.slot`,
  `beatLayer`, `beatKickMuted` or any other REAPER key. `lp.loaded.N` (the cycle masks, §9.26), `lp.volume`, `lp.seq`,
  `lp.kit.N`, `lp.track.N.edited` / `.loop` and `lp.end` stay session-only (a preset's own `_t<N>.mid` is recognised by
  its path on load and flagged edited, so the next save writes its own copy).
- **Pattern files.** An edited, transposed, recorded (perform looper) or THEORY pattern (§9.25) is still written with the
  SMF writer, now as `presets/<name>_t<N>.mid` next to its preset (`House_Amin_007_t4.mid`, `lp.src.N` names it) instead
  of `assets/<Key>__<Cat>__edit_<NNN>.mid`; the session writer keeps `/lp/state/session_s<set>_t<N>.mid`. Nothing else is
  copied: a library pattern, a vocal phrase and a loop are referenced by their card path, as the session does (§9.18).
- **Loading** resolves `lp.src.N` first. A legacy file — a preset saved before this change, `track.N.clip=assets/<basename>`
  with `clipType=MIDI|WAV` — still loads through the existing reader (`sanitizeClip`, the path joined to the preset
  folder), so the presets on the user's cards keep working. `presets/assets/` folders on a card are never deleted or
  pruned by the firmware; tools/card_sync.py's protected list already covers `presets/`.
- **A missing source** (review fix 2026-10-27: the first draft of this section claimed "a pick of the category" for
  every kind of source; what the loader does is this). A loop track picks a loop of its category after the swap
  (§9.19 §1.7); a kit one-shot is looked for under the role folders and their shards and then replaced by a pick of
  the same role (§9.14). A library pattern (`lp.src.N=/lp/midi/<Cat>/<Key>/sNN/<file>`) whose path fails to open —
  the card re-imported and re-sharded since the save, `tools/shard_folders.py` moving files between `sNN` folders —
  is found again by its basename under its key folder and that folder's shards (`LdSrcFind` / `Sets::srcFind`: ≤ 2
  probes of the flat key folders, then per key folder one `kidFn` walk and ≤ 3 probes per step, the writer's
  `resolveStep` search; the file is opened on the step after it is found), so a preset survives a re-shard the way a
  kit slot does, and the generator remembers the path it was found at, so the next save or session write names the
  real one. A pattern that is nowhere under its key folder, a vocal phrase or a preset's own `_t<N>.mid` that is gone,
  and an unsafe legacy clip leave the track as it was (the engine's pattern, the generator's file and seed) while the
  load still ends `LOADED` with the other tracks restored: a preset depends on the library the card holds, so
  re-import the same library before loading old presets (MANUAL §10). A session restore behaves the same (§9.18 had
  this exposure already). No buffer of its own (host `sizeof(Sets)` unchanged at 11 496 B), one bounded card
  operation per step. Host tests: `load_finds_a_pattern_whose_shard_changed_since_the_save`,
  `load_keeps_a_track_whose_pattern_is_gone_from_the_library` (tests/test_sets.cpp).
- **Delete** (preset manager, §9.22c) removes the `.lpset`, its `_t<N>.mid` files and its index line; a legacy preset's
  `assets/` copies are left alone (they may be shared between legacy presets).
- **Save** writes a few KB of text plus the SMF of edited tracks and streams no WAV copy, so the known limitation of
  §10.1 / MANUAL "save between songs" — a save while the vocal played copied ≈ 275 KB into `presets/assets/` and starved
  the vocal ring for ≈ 0.5 s — has no cause left; the brief asks the sets owner for a host test with the slow-FS fixture
  asserting zero vocal underruns during a save. The two-handle wait before an asset copy (§9.19g) goes with the copy.
- **Limits unchanged**: RAM must not grow (free heap ≈ 13.4 KB; Sets may shrink), one bounded card operation per
  `Sets::step()`, the audio thread never blocks. Measured sizes: §9.30 (Sets −772 B on the target, nothing grew).
- **Docs** (this pass): docs/REAPER_IMPORT.md deleted; README, MANUAL §1 / §10 / §11 / §23 / §24, KEYMAP (title, the save,
  THEORY and delete rows), CARD_IMPORT A / B, GETTING_STARTED §5 and CREDITS scrubbed of the compatibility claims and
  describing the device-native files; docs/RESEARCH_LIBRARY_EXPANSION.md, docs/LAYOUT_PROPOSAL.md and
  docs/W3_LOOPMODE_CONTRACT.md kept as dated records; the earlier SPEC sections above are the record (append-only).
  docs/LOOPS.md and the content tools (`import_library.py`, `make_loops.py`, `chop_vocal_loops.py`,
  `import_vocal_shots.py`, `prepare_content.py`, `lp_smf.py`, `make_synth_kit.py`) keep their REAPER mentions: they name
  the source library's file-name convention (`<Key>__<Category>__…`, the key labels) and the balance targets the card
  content was built FROM — an import of content, not an export of sets.

### 9.30 Integration of the §9.29 wave: the merged tree, the suite, the builds (2026-10-27)

Host and compile only (nothing uploaded, nothing played, no git state changed; `sdcard/lp` left as the runners found
it: the 18 legacy House presets with their 16-file `assets/` folder, no Testpack presets folder, no shard folder, no
`_index.txt`). The three parts — the sets code (`gen/sets.h` / `sets.cpp`, `tests/test_sets.cpp`), the docs pass and the
review fix (`LdSrcFind`, the "missing source" bullet of §9.29) — merged without a conflict. The `sets.h` API the UI
and the recorder use is intact (ui 67, rec 17 and link_all pass without a change there); what §9.29 lists is in the
header: `presetSmfPath` added, `deletePreset` also unlinks the SMFs, the copy states and the REAPER clip fields of
`SetFileTrack` gone, `SetFile::src[N]` holds every source (a legacy clip lands there relative, `sourceAbs` resolves
it). §9.29's placeholders are pinned in place above (the key spellings; `lp.track.N.edited` / `.loop` and
`lp.kit.N` are session keys — the draft listed `edited` / `loop` among the preset keys, which the writer and
`save_writes_a_session_format_preset` contradict).

- **What the writer emits** (that test rejects any line that is not `lp.*` / `schemaVersion` / `setIndex` / `keyRoot` /
  `keyName` / `genre`): the header, `lp.banks=2`, `lp.bpm`, per track `lp.track.N.variant`, `lp.track.7.kickcut`,
  `.preset` (synth tracks), `.seed .len .key .lenk`, `.pause` (Vocal), `.kit` (Beat / Percussion), the strip and knobs,
  `.loopmode=1` when on, and `lp.src.N`. Sources: a library file after ONE `exists()` probe (the NO SRC rule of §9.8
  kept), the vocal and a loop from the generator's open streams without a probe, edited / recorded / theory patterns
  and any pattern whose source is a file that may go away (another preset's `_t<N>.mid`, a session SMF) written from
  RAM as `presets/<name>_t<N>.mid` (`SvEdit`, one open + write, shared with the session writer's
  `/lp/state/session_s<set>_t<N>.mid`). No `assets/` mkdir, no copy state; the index probe and the index open / write
  each got a step of their own (they shared one before).
- **sizeof, Xtensa 2601 -Os with the core flags, one probe object against this tree**: Sets 12 220 → **11 448 B
  (−772)**; SetFile 4 088 → **3 320**; SetFileTrack 181 → **84**; PropertiesReader 688, Generator 12 568, Library 9 484
  unchanged. Host (clang): Sets 12 264 → 11 496, SetFile 3 320, PropertiesReader 696, Generator 12 672, Library 9 488.
  The review fix added no member (host 11 496 B before and after it). Expected on the device: ≈ 0.75 KB more free heap
  at boot (Sets lives in the bootBlock arena, §9.27) — not measured.
- **Builds (arduino-cli, esp32 3.3.12, no upload).** Player `…:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled`:
  **Sketch uses 912 803 bytes (27 %)**, **Global variables use 254 156 bytes (77 %)**, 73 524 B left for locals —
  static RAM unchanged against §9.28 (Sets is heap), flash **−1 116 B** (the copy states and the REAPER key writer gone,
  the shard re-find added), 0 compiler warnings. Desktop: `run_desktop.sh` rebuilt the binary (619 312 B), 19 tests and
  the 25 s selftest PASS (underruns 0, late 0, render max 598 µs, vocal underruns 0, 16 / 16 checks).
- **Suite on the final tree** (`sh tests/run_all.sh`, sequential, exit 0): card_pull PASS; desktop 19 + selftest;
  dsp_core 26; engine 31; fx 33; generator 62; io 36; key_smf 43; library 61; link_all PASS (33 headers, 23 objects,
  ODR 73 shared inline functions 0 differing, Xtensa 22 objects **0 warnings**, target text **340 043 B** (§9.28:
  341 274), data 1 572 B, bss 2 760 B, largest frame 1 200 B smf.cpp); rec 17; sets **52** (48 in §9.28: the slow-card
  save, the legacy fixture load + re-save, the delete of the SMFs, `presetSmfPath`, the two shard re-find tests; max
  step 0.41 ms host; per-step maxima walks 1 / entries 19 / opens 1 / probes 3 / read 4 096 B / written 4 103 B);
  synth 46; theory 7; ui 67 (no preset text is pinned there); variants 32. Sets figures of the run: a generated set with
  the vocal streaming saves in **35 steps** (106 before §9.29); under the slow-card model (150 ms per open / write /
  `exists`, 10 ms per read, both vocal streams playing) the save's own card calls lie in 14 steps, at most 2 in one
  step (the index chunk's open + read), **0 vocal underruns** (`slow_card_save_starves_no_vocal_stream`: the host
  evidence for the §10.1 limitation's removal; the 143 slow calls the fixture counted include the pump's own refill
  reads, the 115 steps the stalls it renders inside); the session write is unchanged (54 steps, 2 opens, 0 probes);
  delete 14 steps (6 before: the eight `_t<N>.mid` unlinks).
- **Suite bookkeeping worth knowing**: the card image's 18 House presets are legacy files whose `assets/` copies are
  NOT in the image (only the 16 `*_edit_*.mid` files are); the earlier suite could load them only because its own
  saves used to copy the same library files into `assets/`. The runner now lists only the presets it wrote in
  `_index.txt` (`suitePresets()` / `rewriteIndex()`), reads the user's files only, and exercises the legacy reader on a
  complete Testpack fixture (`legacy_preset_with_asset_copies_still_loads`). A test that loads a random House preset of
  the image would load nothing on the host.
- **REAPER mentions left on purpose** (`grep -rni reaper docs README.md tools firmware/liveplayer/src`):
  docs/CREDITS.md:21 and README.md:94 — the "inspired by" credit the brief keeps, both say nothing is exchanged; SPEC
  §1–§9.28 (lines 28, 96, 171, 174, 190–208, 253, 262–264, 297–299, 589–591, 853) — the append-only record §9.29
  overrides, line 208's pointer to docs/REAPER_IMPORT.md included (the file is deleted, §9.29 says so), and §9.29 / this
  section describe the removal; docs/LOOPS.md:4,27 and tools/import_library.py, make_loops.py, chop_vocal_loops.py,
  import_vocal_shots.py, prepare_content.py, lp_smf.py, make_synth_kit.py — the source library's naming and balance
  conventions the card content is imported FROM; docs/LAYOUT_PROPOSAL.md (dated record, kept per the brief) and
  docs/W3_LOOPMODE_CONTRACT.md:39 (dated design contract; its §1.7 is the text of SPEC line 853, overridden by §9.29);
  firmware comments naming the provenance of rules the firmware keeps — ui/palette.h:1,21 (the category colours, §4),
  core/types.h:11 (the category folder names), gen/library.cpp:125 (the kit key-root rule), gen/smf.cpp:39 (negative
  deltas tolerated) and :311 (the note cut at the loop end), gen/generator.cpp:5 (the track order) and :195 (the 2 %
  loop timing rule), gen/generator.h:25,216,252 (the natural length rule of §9.3), gen/sets.cpp:287 (the legacy
  reader). docs/RESEARCH_LIBRARY_EXPANSION.md, SHARING.md, FLASHING.md, GETTING_STARTED.md, CARD_IMPORT.md, MANUAL.md,
  KEYMAP.md and tools/card_pull.py / card_sync.py carry none.
- **Still open after this pass**: the device measurements (the boot heap after the −772 B; a save while the vocal plays
  on the real card — the "no starvation" claim rests on the host model; a legacy preset with its `assets/` present
  loaded on the device); `Generator::noteSource` leaves a `presets/<name>_t<N>.mid` path at origin 0 (unknown) —
  correct today because `ownedSource` makes every save write its own SMF, a generator owner may add an origin; a set
  loaded from a legacy preset saves its vocal (and MIDI) as absolute `presets/assets/` paths, so legacy `assets/`
  folders must stay on the card (MANUAL §10 / CARD_IMPORT A say so, the firmware never deletes them); `smf.cpp`'s Sink
  writes SMFs in 64-byte runs, which the slow-card host model counts as ≈ 16 writes for a 1 KB pattern (SdFat buffers
  sectors on the device) — pre-existing for the session SMF writer; the stale words "set assets / editor exports"
  (smf.h:11) and "SMF assets" (generator.cpp:39) are comments of files this wave did not touch;
  docs/W3_LOOPMODE_CONTRACT.md:39 keeps its 2026-10-16 wording (a dated record); arduino-cli's "Low memory available"
  note at 77 % static is unchanged.

### 10.6 Release 0.9.0 on the device (2026-10-28, merged ROM dist/release-0.9.0/meehaw-player-0.9.0-full.bin at 0x0)

- Boot: `vol=150` (config.txt volume=150; the device never boots silent any more), heap 13 956 B free after the
  session restore (Sets −772 B from §9.29), no LOW RAM.
- USB drive mode round trip: `drive` → the Mac enumerates "M5STACK_CARDPUTER" with the TinyUSB MSC interface (the
  volume stays hidden by this Mac's management policy); `player` on the drive's port → the player re-enumerated on
  its hardware CDC port within 1 s, no power cycle. Before the fix of carddrive.ino's returnToPlayer (RTC_CNTL_USB_CONF
  PHY selection survives a soft reset) the player booted with a dead USB port twice.
- Save while a vocal streams (device-native preset, no WAV copies): one Sets::step() took 373 ms (`sets_max_us=372857`,
  the preset file's creation / FAT update on this 32 GB card) and every stream ring ran dry (ring −4352 … −5632):
  vocal underruns +10 496 samples ≈ 0.48 s, loop slots as well. The host slow-card model (150 ms per op) passes; this
  card's write latency is beyond any ring (186 ms vocal, 139 ms loop). Open: a card-write quiet strategy (pre-created
  spare preset file written in place, or saves only while nothing streams) — see the follow-up proposal.
- Random load with the key filter leaving one candidate (the preset just saved, F#min) reloads the same state: the
  no-repeat cycle is covered by the host tests, not by this device run.
- The GitHub draft release v0.9.0 holds 19 assets (ROMs, SHA256SUMS, free card bundle, docs, stickers).
