# Cardputer Live Player — layout proposal (v0.1, 2026-10-14)

A standalone port of the REAPER Live Player idea to the M5Stack Cardputer: two sets of eight
tracks (chord, lead, bass, pad, fx, percussion, vocal, drum), all 16 audible at once, patterns
randomly generated from MIDI files on the SD card in the global key, with one-shot samples for
the rhythmic/vocal/fx tracks. Everything below is a proposal to discuss before coding.

## 1. Hardware reality

| Item | Fact | Consequence |
|---|---|---|
| MCU | ESP32-S3FN8, 2 × 240 MHz, both the original Cardputer and the Cardputer ADV | plenty of CPU for ~50 voices at 22 kHz |
| RAM | 512 KB SRAM, **no PSRAM** on either model (your 8 MB firmware backup matches the FN8 part) | roughly 300–330 KB free heap after M5Unified; samples must live in a fixed ~200 KB pool or stream from SD |
| Flash | 8 MB | firmware + a few hundred KB of built-in assets at most |
| Audio out | original: NS4168 I2S amp, mono speaker, shares a pin with the mic; ADV: ES8311 codec + NS4150B + 3.5 mm jack | use M5Unified `Speaker.playRaw` (works on both, as MiniAcid and MicroGroove do) |
| SD | SPI (CS G12, MOSI G14, CLK G40, MISO G39), ~1–2 MB/s realistic | fine for loading one-shots and MIDI at generate time; too slow/risky for 16 concurrent streams |
| Display | 1.14" ST7789V2, 240 × 135 | no full-screen double buffer (64 KB); draw regions with small sprites |
| Keys | 56 keys, 4 rows × 14, plus the side button (BtnA) | two rows of eight letter keys = the two sets |

What the reference projects teach us (all three target the ADV; MiniAcid and MicroGroove run via M5Unified):

- **MicroGroove** (MIT): 22050 Hz mono, 256-sample ping-pong buffers through `Speaker.playRaw`, render task pinned to a core, 192 KB internal-RAM sample pool (`heap_caps_malloc(MALLOC_CAP_INTERNAL)`), ~18 voices, "keep samples short", no fades = clicks.
- **cardputer-studio** (MIT): 16 kHz, 18 voices, drum lanes capped at 0.3 s each, one master delay buffer (16 KB) allocated only while in the synth page, 10–14 ms fades to kill clicks, normalise off because it pops.
- **BKLVA Pocket DAW** (closed): 44.1 kHz streams loops from SD with underrun counters; per-track volume/filter/drive + delay and reverb **sends** to shared master buses; bar-quantised scene launch. The FX topology is exactly what we want.
- **MiniAcid** (MIT, already on your device): audio task on its own core, 22050 Hz/256 samples, 303 voice, 808/909 drum synth, small Schroeder reverb (4 KB), tube distortion, plus a desktop SDL build of the same code for fast iteration. We can reuse the 303 for bass, the drum synth as a fallback kit, the reverb and distortion.

## 2. Keyboard layout

Physical rows of the Cardputer (left to right):

```
 `   1   2   3   4   5   6   7   8   9   0   -   =  del
tab  q   w   e   r   t   y   u   i   o   p   [   ]   \
fn  sft  a   s   d   f   g   h   j   k   l   ;   '  ent
ctl opt alt  z   x   c   v   b   n   m   ,   .   /  spc
```

Proposed meaning (track order as you listed it: CHD LED BAS PAD FX PRC VOC DRM):

```
 `  = help overlay        1..8 = select track 1-8 of the EDIT set     9/0 = key prev/next
                          - / = tempo -1/+1 (hold repeats, shift = ±5)      del = PANIC (stop all now)
tab = toggle EDIT set     q w e r t y u i = SET 1 triggers     o = GENERATE set 1   p = play all set 1   [ = stop all set 1   ] = tap tempo   \ = page
                          a s d f g h j k = SET 2 triggers     l = GENERATE set 2   ; = play all set 2   ' = stop all set 2   enter = confirm/save prompt
ctl/opt/alt modifiers     z x = VOL -/+   c v = DIST -/+   b n = ECHO send -/+   m , = REVERB send -/+   . / = echo time (global)   space = master play/stop
```

Modifiers on a track key (works on both rows):

| Gesture | Action | REAPER equivalent |
|---|---|---|
| tap | start/stop the track at the next bar (toggle, quantised) | pad trigger |
| shift + track | select the track for editing without launching | Shift + row 1/5 |
| opt + track | re-roll only that track's pattern (same key, shuffle bag, no repeat) | Shift + row 4/8 |
| alt + track | re-roll only its sound (synth preset or sample kit), keep the pattern | Shift + row 3/7 |
| fn + track | immediate (unquantised) start/stop | — |
| ctrl + o / ctrl + l | recall a saved set into set 1 / set 2 (key-matched) | buttons 99/103 |
| ctrl + s | save both sets as a patch on the SD card | performance window save |

Bottom-row pairs edit the **selected** track; with shift held they edit the global buses:
shift+z/x master volume, shift+c/v echo feedback, shift+b/n reverb decay, shift+m/, reverb mix.
Number keys 1–8 select within the edit set shown in the header (toggle it with tab), so the
screen's eight columns, the number row and the two trigger rows all line up visually.

The side button (BtnA) doubles space (master play/stop) for one-handed use.

## 3. Screen layout (240 × 135)

```
┌────────────────────────────────────────────────┐
│ 128 BPM  A min   ▶ 05.3   edit:S1   gen:idle   │  header, 12 px
├────────────────────────────────────────────────┤
│ S1 │CHD│LED│BAS│PAD│ FX│PRC│VOC│DRM│            │  set 1 row, 40 px: category colour, state,
│    │▓▓▓│▓░░│   │▓▓░│   │▓▓▓│░░░│▓▓▓│            │  16-step playhead bar, key label of the file
│ S2 │CHD│LED│BAS│PAD│ FX│PRC│VOC│DRM│            │  set 2 row, 40 px
│    │   │   │▓▓▓│   │   │   │   │▓▓░│            │
├────────────────────────────────────────────────┤
│ S1 LED  Amin__VI_III_i_v  arp  Pluck2  1bar    │  selected track: file, variant, sound, length
│ VOL ▮▮▮▮▮▮▯▯ DST ▮▮▯▯▯▯ ECHO ▮▮▮▯▯▯ REV ▮▯▯▯▯▯  │  four parameter bars (bottom-row pairs)
│ ECHO 1/8d fb40  REV 0.6  MASTER ▮▮▮▮▮▮▮▯  ▁▃▅▇  │  global strip + output meter
└────────────────────────────────────────────────┘
```

Cell states: empty (no pattern), loaded (outline), queued (blinking outline, starts next bar),
playing (filled, playhead moving), generating (hatched). Category colours follow the REAPER
version: Chord orange, Lead yellow, Pad purple, Bass brown, Fx pink, Percussion green, Vocal cyan,
Drum grey. Pressing `` ` `` shows a one-page key map; `\` cycles to a second page with the
per-track mixer of all 16 tracks (volume bars only).

## 4. Audio engine

- 22050 Hz, mono, int16, 256-sample blocks (11.6 ms), two ping-pong buffers, `Speaker.playRaw`.
  Audio render task pinned to one core at high priority; keyboard, display and SD loading on the other.
- Clock: 96 PPQN tick counter advanced inside the audio callback (sample-accurate), tempo 60–200 BPM,
  default 120. Bar = 4 beats. Launch quantisation to the bar (setting: bar / beat / off).
- **Melodic tracks (chord, lead, bass, pad) are synthesised**, not sampled: two oscillators
  (saw/square/tri/sine tables), state-variable filter, amp + filter envelopes, LFO for pads.
  Polyphony: chord 4, pad 4, lead 2, bass 1 (MiniAcid 303 voice). "Generate sound" picks one of a
  small preset bank per category (the Serum-preset rotation equivalent). Zero sample RAM.
- **Sample tracks (drum, percussion, vocal, fx)** play one-shots from a fixed RAM pool, no SD
  streaming while playing. Linear interpolation, per-voice 2 ms attack / 12 ms release to avoid clicks.
- Why no pitched one-shots for melodic parts: one 0.5 s instrument sample is 22 KB; four categories
  × two sets would eat the whole pool. Kept as a later option (the `lp/inst` folder exists for it).

Sample pool budget (stored as 8-bit µ-law, decoded through a 256-entry table; halves the bytes at
no audible cost on this speaker):

| Per set | Count × cap | Bytes |
|---|---|---|
| drum kick / snare / closed hat / open hat | 300 / 250 / 120 / 350 ms | 23 KB |
| percussion | 4 × 200 ms | 18 KB |
| vocal chops | 4 × 400 ms | 35 KB |
| fx hits | 4 × 300 ms | 26 KB |
| **set total** | | **~102 KB** |
| both sets (two independent halves, swapped on generate) | | **~205 KB** |

Rest of the RAM plan: echo bus delay line 33 KB (up to 750 ms, tempo-synced divisions 1/16 … 1/4d),
reverb 4–8 KB (MiniAcid Schroeder), patterns 16 tracks × 256 notes × 6 B = 24 KB, MIDI index
≈ 3 B per file (key + ordinal), UI sprites ≤ 10 KB, audio buffers 2 KB, audio stack 8 KB.
Total ≈ 290 KB against ~320 KB free — tight but in line with MicroGroove's proven 192 KB pool.
If it does not fit, the first levers are vocal/fx caps and 11025 Hz storage for vocal chops.

CPU estimate: ~22 synth voices × 60 cycles + ~24 sample voices × 25 cycles + buses, ≈ 60 M
cycles/s at 22050 Hz, about a quarter of one core.

Effects, as requested: per track **volume, distortion** (tanh/tube waveshaper, free), **echo send,
reverb send**; global echo (division, feedback, damping) and reverb (decay, mix) on shared buses;
master soft limiter. Per-track echo/reverb would cost 16 delay lines, hence sends.

## 5. Generation model (ported from the REAPER Lua)

- **Key**: natural minor keys cycled in circle-of-fifths order (A, E, B, F#, C#, G#, D#, A#, F, C, G,
  D), default A minor, changed with 9/0, affects only future generation. A file is accepted when its
  key label has the same pitch classes (so A minor accepts `Amin__` and `C__`, rejects `A__`), or it is
  keyless (`NA__`). **Never transposed.** Drum/percussion ignore key.
- **Pick**: one shuffle bag + history per track and kind (pattern, sound); every file is offered once
  per cycle, never an immediate repeat; key change resets the tonal bags. History stores file
  ordinals so a saved set can recall the exact files.
- **Generate set N** (key `o`/`l`): for each of the eight tracks pick a pattern file, parse it (cap
  256 notes, normalise to 1/2/4 bars), pick a sound (preset or 4 one-shots), load into the inactive
  half of the pool, then swap atomically at the next bar. A playing track keeps playing the old
  pattern until the bar line — same as pressing the fresh-generation button in REAPER while live.
- **Drum contract**: 36 kick, 37 snare, 38 closed hat, 39 open hat; percussion, vocal and fx patterns
  use 36–39 (fx: 48–51) as four voices A–D mapped to the four randomly chosen one-shots.
- **Variants**: v1 keeps one pattern per track plus re-roll (opt + track). v2 adds the four REAPER
  variants per track (original / variation / 2-beat / half-beat; Kick → Full layers for drums;
  sparse → full for percussion) selected with 9/0 when a track is selected. The transformation rules
  are documented in `pattern_variants.lua` and can be ported one by one.
- **ABAC** and WAV loops: out of scope for the Cardputer (RAM).

## 6. SD card layout and test content

```
/lp/<category>/midi/<Key>/<Key>__<name>.mid    Key folder = Amin, C, F#min, A#, ... or NA
/lp/drum/samples/{kick,snare,hh,oh}/*.wav      roles for notes 36..39
/lp/{percussion,vocal,fx}/samples/*.wav        pools, four random picks per generate
/lp/inst/samples/*.wav                         single-note one-shots (optional sampler mode later)
/lp/sets/*.json                                saved sets (key, tempo, per-track file ordinals, sounds, mixer)
/lp/presets/<category>/*.json                  synth presets (optional overrides of the built-in bank)
/lp/README.txt                                 sources and licences
```

Files are 22050 Hz mono 16-bit PCM WAV, peak −1 dBFS, silence-trimmed, faded; names keep the
REAPER `Key__name` contract and sit in a folder per key so key filtering is a directory open, not a
scan of thousands of FAT entries (keep folders under ~500 files). Your existing REAPER library
(`~/Music/Midi/<Category>/Key__name.mid`) can be imported with a small script that sorts files into
key folders; nothing else changes. 32 GB is far more than this needs: the whole test tree is 11 MB.

Test content built by `tools/prepare_content.py` (deterministic, seed 20261014):

| Folder | Files | Source | Licence |
|---|---|---|---|
| chord | 288 (12 per key, 24 keys, 4 bars) | ldrolez/free-midi-chords v0.20260314 | MIT |
| bass / lead / pad | 288 each, **derived** from those progressions (root-note bass styles, arpeggio/top-note leads, sustained pads) | prep script | MIT (derived) |
| drum | 172 two-bar slices, mapped to 36–39 | Magenta Groove MIDI Dataset v1.0.0 | CC BY 4.0 |
| percussion | 47 from Groove toms/cymbals + 24 Euclidean | Groove / generated | CC BY 4.0 / none |
| vocal, fx patterns | 24 + 24 generated trigger patterns | generated | none |
| drum samples | 36 kicks, 27 snares, 39 closed hats, 8 open hats | Sonic Pi (CC0) + Dirt-Samples 808/909 | CC0 / unstated |
| percussion samples | 131 | Sonic Pi perc/tabla/elec + Dirt | CC0 / unstated |
| vocal samples | 57 chops | Dirt-Samples yeah/speech/numbers/mouth… | **unstated, private testing only** |
| fx samples | 81 | Sonic Pi ambi/misc/vinyl + Dirt stab/hoover/space… | CC0 / unstated |
| inst | 23 single notes (Sonic Pi bass_*, tbd_*, elec bells) | Sonic Pi | CC0 |

No open-licensed bass or lead MIDI packs exist on GitHub; the free ones (Gumroad, itch.io, vendor
sites) need an email checkout, so the bass/lead/pad files are derived from the chord progressions
for now. Vocal chops with a clear licence were not found either; Dirt-Samples is fine to test with
but should be replaced before sharing the card.

## 7. Firmware architecture

```
liveplayer/
  src/engine/   clock, track, pattern, synth voices, sample voice, µ-law pool, fx buses, mixer
  src/gen/      sd index, key filter, shuffle bag, midi parser, wav loader, generator, set save/load
  src/ui/       pages, key map, drawing (M5GFX), help
  platform/     cardputer (M5Unified Speaker/SD/Keyboard) and desktop SDL (like MiniAcid) builds
  tools/        prepare_content.py, import_library.py, venv
  sdcard/       the tree above, copy to the card root
```

- Arduino core for ESP32 with M5Cardputer/M5Unified/M5GFX, built with arduino-cli (or PlatformIO);
  neither is installed on this Mac yet (MiniAcid was flashed as a binary). A desktop SDL build of the
  engine and generator (MiniAcid shows it is cheap) lets us test generation, key filtering and the
  synth with the real SD tree on the Mac before touching the device.
- Generation runs on the UI core and never blocks audio; the audio task only reads immutable
  pattern/pool slots and swaps pointers at bar boundaries.

## 8. Phases

1. Skeleton: audio task, clock, 16 tracks, key map, screen rows, one built-in drum kit. Flash it.
2. SD index + MIDI parser + key filter + shuffle bags; generate drums/percussion from the card.
3. µ-law pool, sample tracks for vocal/fx, per-set pool swap at bar line.
4. Synth voices + preset banks for chord/lead/bass/pad; sound re-roll.
5. Mixer: volume/distortion/echo/reverb sends, buses, master limiter, screen parameter bars.
6. Save/recall sets, tap tempo, panic, help page, polish; desktop SDL build along the way.
7. v2: four variants per track, drum layer selection, library import script.

## 9. Open decisions

1. Which Cardputer do you have, original or ADV? Same RAM, but the ADV has a headphone jack and a
   different codec, and all three reference projects only support the ADV.
2. Melodic tracks as synth presets (recommended, zero sample RAM) or pitched one-shots?
3. Vocal and fx as four short chops per set (≤ 400 / 300 ms) triggered by a pattern — OK?
4. One pattern per track with re-roll in v1, with the four REAPER variants in v2?
5. Track order: your list (chord, lead, bass, pad, fx, perc, vocal, drum) or the REAPER order
   (chord, lead, pad, bass, fx, perc, vocal, beat)?
6. Launch quantisation default: bar (recommended) or beat?
7. Keys: minor only, cycled by fifths like REAPER, or major as well?
8. Toolchain: may I install arduino-cli (Homebrew) plus the ESP32 core and M5 libraries, and build a
   desktop SDL version alongside?
