# meehaw live performer — the manual

For the performer. The firmware specification is docs/SPEC.md, every key with its matrix position is docs/KEYMAP.md,
printable key stickers are in docs/stickers/ (`tools/make_stickers.py`), the card is docs/CARD_IMPORT.md and the credits
docs/CREDITS.md. Written 2026-10-16 for the §9.22 / §9.23 build and updated the same evening for §9.25–§9.27: the
**THEORY** generator on SHIFT + load 1 / 2, the preset manager on **SHIFT + save 2** with a target set, the no-repeat
random load, and the RAM diet (boot floor 8 KB, loop mode refused under 6 KB); §10 / §11 updated 2026-10-27 for the
device-native presets (SPEC §9.29: source paths instead of clip copies, nothing exported).

## 1. What it is

A pocket live set on the M5Stack Cardputer ADV (ESP32-S3, 22050 Hz mono, headphone jack and speaker). Two **sets** of
eight **tracks** each — Chord, Lead, Pad, Bass, Fx, Percussion, Vocal, Beat — all sixteen may sound at once. Patterns are
picked from a MIDI library on the microSD card in the global key — or, for the melodic tracks, composed on the device from
one chord progression (the THEORY keys, §9) — and turned into four variants; the melodic tracks are
synthesised (presets with live knobs, a 303 character and a chorus bus); Beat and Percussion play one-shot kits; the
Vocal track streams a phrase from the card; any track can stream a pre-rendered 125 BPM **loop** instead. Everything
launches on the bar. You generate, start, stop, tweak, ramp, tilt, play live on the keyboard, record your moves and
render them to a WAV, and save sets as presets on the card.

The keyboard is a 4 × 14 matrix; the printed legends are only landmarks, so the key names below are the Cardputer's
captions with their matrix position `(x, y)` (x 0–13 left to right, y 0–3 top to bottom). **SHIFT is the space key**
`(13,3)`, held. The side button is **BtnA**.

## 2. First boot and the card

1. Put the card content in a folder `lp` at the root of the card (docs/CARD_IMPORT.md: card reader, the serial loader,
   or this player's USB drive mode). The player needs `/lp/midi/...`, `/lp/packs/<Genre>/...` and `/lp/config.txt`.
2. Power on. The splash (`meehaw / live performer`) stays 1.5 s, then the main screen. Boot messages: `NO SD CARD`
   (nothing mounted; check the card and the `lp` folder), `LOW RAM nK FREE nK MAX` (the boot floor of 8 KB free / 6 KB
   largest block is not met — see §23), `NO RAM FOR LOOPS` (the loop rings could not be allocated: loop mode will stay
   silent).
3. The last session comes back by itself within about a second (`BUSY` answers to save / load / generate meanwhile):
   every track is loaded as you left it but **nothing plays** until you start it.
4. Press **ent** `(13,2)` = GENERATE set 1. The status line counts `GEN S1 1/8 … 8/8`, the eight squares of the top row
   fill with outlines, and the sounds change slot by slot. Tap a track key to start it at the next bar.

`/lp/config.txt` (key=value, `#` comments): `bpm=125` (the tempo at boot — it wins over the saved session's tempo),
`volume=150` (output level 0–255), `echo.division=2` (0 1/16, 1 1/8, 2 1/8 dotted, 3 1/4, 4 1/4 dotted, 5 1/2),
`duck=1` (kick ducking), `imu.pitch_axis=x` / `imu.roll_axis=y` (gyro axes, §15).

## 3. The main screen

Two rows of eight squares (26 px): the top row is set 1, the bottom row set 2, each square a track in its category
colour with its label above (CHD LED PAD BAS FX PRC VOC BEAT; orange, yellow, purple, brown, pink, green, cyan, grey).

- **Filled** = playing. **Outline** = loaded, stopped. **Dim outline** = empty. **Blinking** = a start or stop is queued
  for the bar line, or a loop pick is on its way.
- The small grey square inside a cell = the **selected track** (the one the parameter keys address). The 1–4 ticks under
  it = its variant.
- `A` top right = acid mode. `L` top left = loop mode. A diagonal strike through a Beat = kick cut.
- The **status line** above the footer shows progress and messages for a second (`GEN S1 3/8`, `THEORY S2 3/8`,
  `SAVED House_Amin_007`, `LOOP ON S1 BEAT`, errors).
- The **footer**: `A MIN 125  HOUSE  LEN 4` = key, tempo, genre, the selected track's clip length in beats; then `GYRO`
  while the gyro FX is on, `OUT 59%` for 3 s after an output-volume step, `LOOP UNDERRUNS n` while a loop stream has
  starved (§19); a blinking red `REC` while a session records; the battery at the right (`87%`, `--` unknown, red below
  15 %).
- Parameter keys open a full-screen **parameter page** for 1.2 s (name, value huge, bar, `S1 BEAT  HOLD` / `RAMP`); the
  main keys stay live under it. Beat layer, rush and kick cut show a 0.6 s **note page** (`S1 BEAT 3 +SNARE`).

## 4. Tracks: start, stop, select

- **Tap a track key** (row y0 for set 1: `` ` `` 1 2 3 4 5 6 7; row y1 for set 2: tab q w e r t y u): the track starts
  or stops at the **next bar** and becomes the **selected** track. A queued stop keeps sounding until the bar line; a
  second tap before the bar cancels the pending change.
- **SHIFT + track key**: select only, nothing starts or stops.
- **BtnA** (side button): master play / stop — everything stops at once, the next press resumes (queued tracks start).
- **SHIFT + j** `(8,2)`: **PLAY ALL / STOP ALL** — if anything plays or is queued, both sets stop at the bar
  (`STOP ALL`); otherwise every loaded track of both sets starts at the bar (`PLAY ALL`).

## 5. Beat layers, rush, kick cut

The keys 8 9 0 - `(8–11,0)` are the **Beat layers of set 1** (i o p [ for set 2), whatever track is selected:
**KICK**, **+HATS**, **+SNARE**, **FULL** (the Beat's own pattern is always the full one; a layer is a mask applied at
the bar line). After a generate the Beat starts on KICK so you build the groove up. Pressed during a generate they are
queued (`S1 PAT 2 QUEUED`) and applied after the swap.

- **= `(12,0)` / ] `(12,1)` = snare rush** (momentary): while held, the Beat plays its full layer cropped to one beat
  with the kick removed, from the next 1/16; let go and it falls back into place. On a Beat in **loop mode** the rush
  crops the loop to **half its current length** (at least one beat) while held.
- **del `(13,0)` / \ `(13,1)` = kick cut** (toggle, at once): the kick (note 36) is muted, the rest of the Beat plays on;
  `S1 KICK CUT` / `S1 KICK ON`. On a loop Beat the whole stream is high-passed instead (the strip filter goes to +0.6).

Kick ducking: every kick of a set dips the other seven tracks of that set by 4.5 dB (not while the kick is cut);
`duck=0` in config.txt turns it off.

## 6. The parameter keys (row y2 up, row y3 down)

| key | parameter | range / step |
|---|---|---|
| fn / ctl `(0,2)/(0,3)` | **VOL** of the selected track | 0 … 1, step 0.05 (default 0.8) |
| sft / opt `(1,2)/(1,3)` | **REVERB** send | 0 … 1, 0.05 |
| a / alt `(2,2)/(2,3)` | **ECHO** send (tempo-synced delay) | 0 … 1, 0.05 (vocals start at 0.35) |
| s / z `(3,2)/(3,3)` | **DIST** (strip distortion) | 0 … 1, 0.05 |
| d / x `(4,2)/(4,3)` | **FILTER** | −1 low-pass … 0 bypass … +1 high-pass, step 0.1 |
| f / c `(5,2)/(5,3)` | **LENGTH** (clip length) | × 2 / ÷ 2, 0.5 … 16 beats; Vocal: pause 0 / 1 / 2 / 4 bars |
| g / v `(6,2)/(6,3)` | **TEMPO** | ± 1 BPM, 40 … 240, hold repeats |
| h / b `(7,2)/(7,3)` | **KEY** | next / previous minor key on the circle of fifths |

**Step, hold-ramp, freeze.** For the five continuous parameters a **press** moves one step; **holding** the key turns
the step into a **hold-ramp** towards the end of the range at the ramp speed (the whole range takes `rampBars` bars,
default 4; the page shows `HOLD`); **releasing** freezes the value where the ramp got. Press the other direction's key to
ramp back from there. Opening a modal page (editor, perform, synth, preset manager, recorder) freezes a ramp you are
still holding.

**SHIFT ramp and snap-back.** **SHIFT + a parameter key** ramps to the end of the range over `rampBars` bars (`RAMP` on
the page) and **releasing SHIFT snaps every such ramp back** to the value it started from — a build that resolves the
moment you let go. **SHIFT + len±** sets `rampBars`: 1 → 2 → 4 → 8 → 16 (RAMP page).

**Length.** The clip cycles every `len` beats, phase-locked to the global clock; longer than the pattern = silence after
it (a 2-beat crop in 8 beats plays once per two bars). On a Vocal the length keys set the **pause** after the phrase
window instead (`PAUSE 1 BAR`, `CYCLE 3 BARS`); the loop variants of a vocal have no pause (`NO PAUSE`). Pressed during
a generate: queued (`S1 LEN +1 QUEUED`).

## 7. Tempo and drift

**tempo±** steps 1 BPM (hold repeats). Vocals and loops are varispeed, so their pitch follows the tempo. **SHIFT +
tempo±** starts a **drift**: ± 10 BPM over 60 beats that keeps going after SHIFT is released (`DRIFT` on the TEMPO page);
the same SHIFT + tempo key again stops it where it is, the other one turns it round, a plain tempo± stops it and steps
from the tempo reached. The default tempo is 125 (`bpm=` in config.txt), where the loops and the rap bank are rendered.

## 8. Key change and octave

**key±** moves the global key one step along the circle of fifths (A E B F# C# G# D# A# F C G D, natural minor); it
affects **future** generation only — what plays keeps its key until you generate again (saved sets remember the key
they were made in). **SHIFT + key±** transposes the **selected track's pattern** one octave up or down (`OCTAVE +1`),
synth tracks with notes only (`OCTAVE: SYNTH TRACKS ONLY`, `OCTAVE: NO PATTERN`).

## 9. Generate: library or theory

Two ways to fill a set, both replacing all eight tracks with one swap at the bar line: **GENERATE** picks patterns and
sounds from the card's MIDI library, **THEORY** composes the melodic tracks on the device from one chord progression.

- **ent `(13,2)` = GENERATE set 1**, **SHIFT + ent = set 2** (library): new patterns and sounds for all eight tracks of
  the set, swapped together at the bar line (`GEN S1 3/8`; stopped tracks adopt silently). The strip parameters of
  regenerated tracks return to their defaults (the Fx track keeps a recorded perform loop and its strip).
- **SHIFT + ; `(11,2)` = THEORY set 1**, **SHIFT + . `(11,3)` = THEORY set 2**: the five melodic tracks — Chord, Lead,
  Pad, Bass, Fx — are written from **one chord progression** drawn from a table of 126 minor-key progressions (four
  chords of a bar, eight of half a bar, two of two bars, and 2-bar loops; with the borrowed major V, the dorian IV, the
  diminished vii°, sus4 and sus2 colours), in the current key and without reading the card. The **Pad** holds the
  voicings (triads, 7ths, add9, 6ths, shell and octave voicings, close or drop-2, the inversion of every chord chosen for
  the smoothest voice leading; sustained, swelling or pulsing), the **Chord** plays the same voicings as stabs, broken
  chords or plucks through one of 47 rhythm templates, the **Bass** a root / fifth / octave line from one of 34 templates
  with approach notes and slides, the **Lead** a motif of chord tones and diatonic passing tones in an A A' B A' form
  over one of 44 templates, the **Fx** hits on the chord changes. Beat, Percussion and Vocal regenerate from the library
  inside the same job, so one key makes a whole new set that hangs together. Status `THEORY S1 3/8`, then `GEN DONE`.
  The result is a normal set: edit it, tweak it, transpose it an octave, put a track in acid mode (an acid track keeps
  its '303' sound and plays the theory line), save it as a preset or let the session keep it — the notes are stored as a
  file, so it comes back exactly. The editor names the progression (`THEORY i-VI-III-VII`). Press again for another
  progression; GENERATE or gen pattern on a melodic track turns the set back into a library set. Pressed while another
  job runs, the THEORY key **queues** like GENERATE (the status shows `THEORY S1 0/8` until its turn; the progression is
  drawn when the job starts). Refusals: `GEN BUSY` for a second press of the **same** set while its theory job is running
  (or when four requests already wait), `SETS BUSY` while a save, load or listing owns the card.
- **' `(12,2)` = gen pattern** of the selected track (all four variants, the sound stays); refused with `LOOP KEPT: HOLD
  DEL CLEARS` on a track holding a perform loop.
- **/ `(12,3)` = gen sound** of the selected track (a new preset, kit or vocal file; the pattern stays). On an acid
  track this also re-rolls the knobs; on a loop-mode track every generate picks a new loop.
- Refusals: `SETS BUSY` (a save / load / restore owns the card), `GEN BUSY` (a job runs). Progress and the generator's
  own note or error come to the status line when the job ends (`GEN DONE`).

Genre: **k `(9,2)`** rotates through the packs on the card (`/lp/packs/*`), `BUSY` while a card job runs.

## 10. Save and load

- **l `(10,2)` = save set 1**, **, `(10,3)` = save set 2**: the set becomes a preset `<Genre>_<Key>_<NNN>.lpset` in the
  genre's `presets/` folder. `SAVE S1 3/8`, then `SAVED House_Amin_007`. The preset is a small text file of `lp.*`
  keys (SPEC §9.29): the key, genre, tempo and label, and per track the sound, variant, length, pause, kit, strip and
  knob values, acid and loop mode, and the **card path** of its pattern, vocal phrase or loop — no clip is copied.
  A track you edited, recorded on the perform page or composed with THEORY is written as its own MIDI file beside
  the preset, `<name>_t<track>.mid` (`House_Amin_007_t4.mid`), so it comes back note for note. A save writes a
  few KB and streams no WAV copy, so the old "save between songs" rule (the vocal starved during the copy) has no
  cause left. Presets saved before 2026-10-27 carry an `assets/` folder of clip copies: they still load, and the
  firmware never deletes that folder. Because a preset names its library files by their card path, it depends on the
  library the card holds: after a re-import that moved a pattern to another shard folder the load finds it again by
  name; a pattern or phrase that is no longer on the card leaves that track as it was (the load still says `LOADED`),
  so re-import the same library before loading old presets.
- **; `(11,2)` = load set 1**, **. `(11,3)` = load set 2**: a random saved preset of the genre **in the current key**
  (`NO SET IN Amin` when there is none), swapped at the bar like a generate; the preset's tempo applies only when nothing
  plays. The preset brings back every sound, pattern, variant, length, pause, kit, strip and knob value, acid and loop
  flag.
- **No repeat until the cycle is through.** A preset loaded into a set — by the load key or from the preset manager —
  is not drawn again for that set until every other preset of the genre in the current key has been loaded into it;
  then a new cycle begins (never with the preset just loaded). Each set has its own cycle; it survives power-off (the
  session file keeps it), a preset you save joins the running cycle, a deleted preset drops out. The cycle belongs to
  one genre: after a genre change the first load, save or preset-manager action in the new genre starts fresh cycles
  there (the old genre's progress is not kept). The preset manager shows a **dot** in front of every preset already
  loaded in the target set's cycle (§11). **Starting a recording or a render restarts the cycle of both sets** (the dots
  vanish, the next random loads draw over every preset again) — that is what makes a random load inside a recording
  replay exactly (§12).

## 11. The preset manager (SHIFT + save 2)

**SHIFT + , `(10,3)`** opens the manager; the same combo or `` ` `` closes it. It lists the current genre's presets newest
first: `007 A  my groove` (number, key, label or file name), eight rows, the cursor row inverted. The header reads
`-> SET 1  ALL  23  AUTO`: the **target set** the next load goes into, the filter, the count (`...` while the card is
read, `LISTING...` in the status line) and the AUTO flag. The page is green while set 1 is the target and cyan for set 2.
A **dot** in front of a row = that preset was already loaded into the target set in its current random-load cycle
(§10); set 1 and set 2 have their own dots.

| key | action |
|---|---|
| 1 … 7 `(1–7,0)` / tab … u `(0–7,1)` | **target SET 1 / SET 2** (any track key of that row, with or without SHIFT): header, colour and dots follow; the list, cursor and filter stay |
| fn / ctl `(0,2)/(0,3)` | cursor up / down (hold repeats); the list is read from the card 16 rows at a time as you scroll |
| h / b `(7,2)/(7,3)` | filter **ALL ↔ current key** (`C# MIN`) |
| ent `(13,2)` | **load** the highlighted preset into the **target set** (`LOAD S2 3/8`, `LOADED <name>`); with **AUTO** on that set starts at the bar once loaded. The load counts for the set's cycle (its dot appears) |
| m `(9,3)` | **AUTO** on / off |
| del `(13,0)` | **delete**: `DELETE? PRESS AGAIN` for 2 s, the second press on the same row removes the file and its `<name>_t<track>.mid` pattern files (`DELETED`; a legacy preset's `assets/` copies stay); a target change in between makes the next del a fresh first press |
| n `(8,3)` | **rename**: type a label (letters from the key legends, SHIFT = capitals, digits, `-`; del = backspace, up to 19 characters), ent writes it (`LABEL SET`), `` ` `` cancels |
| SHIFT + ; / . | nothing here (the THEORY keys are not live on the page) |

The target set, the filter and AUTO stay as you left them while the device is on (target SET 1 after a boot). A set
remembers the label of the preset it was loaded from and writes it into its later saves and into the session, so a
renamed set stays recognisable after a reboot.

## 12. The recorder (SHIFT + save 1)

The recorder captures **what you do**, not audio: every key press and release, the gyro, the output volume and BtnA,
stamped on the engine's tick clock, into `/lp/rec/rec_NNN.lprec` (8 bytes per event, a few KB per session, up to 64
sessions). A session is **rendered offline** into `/lp/wav/rec_NNN.wav` (22050 Hz mono 16-bit) by replaying it through
the same engine: the speaker goes silent, the engine restarts from the session's seed, genre, key and tempo and plays
your moves back as fast as the card allows (about 1–2 × real time).

**SHIFT + l `(10,2)`** opens the RECORDER page: `RECORDER  3 SESSIONS`, rows `rec_007   4 BARS  W` (`W` = a WAV exists).

| key | action |
|---|---|
| fn / ctl | cursor |
| ent `(13,2)` | **start recording** (`REC ON`; the page closes, the main footer blinks `REC`, play as usual) — open the page again and press ent to **stop** (`REC STOPPED`) |
| m `(9,3)` | **render** the highlighted session to WAV: the RENDERING overlay shows `RENDERING rec_007` and the percentage; **hold `` ` `` for 1 s** (or press BtnA) to cancel; `DONE /lp/wav/rec_007.wav` for 3 s at the end |
| del `(13,0)` | **delete** the session and its WAV (`DELETE? PRESS AGAIN`) |
| `` ` `` or SHIFT + l | exit |

What to know:
- Start the recording **before** you load the content you want in it. The replay restarts the generator from a known
  seed and re-plays your generates, loads and tweaks, but what was already playing when the recording began is only
  restarted, not restored — a session recorded from an empty stage renders exactly; one started mid-set renders with
  whatever the engine holds at render time.
- The replay restarts the generator's random state exactly (seed, preset bags, library bags, genre, key) and brings
  back the selected track, the ramp length, the kick cuts and the gyro state as they were when the recording started.
  It does **not** bring back variants, len steps, loop mode, acid flags, content loaded before the recording, or the
  preset sounding on each track at render time (a sound pick avoids the preset that is sounding, so a `gen sound`
  inside a session started mid-set may land on a different preset than live). Patterns, loads, generates, knobs and
  everything else replay exactly as long as the card content is unchanged; a replayed save, preset DELETE / RENAME or
  USB drive hold writes nothing and reboots nothing (a replayed SHIFT + save 2 opens the preset manager as live). A
  THEORY set replays exactly (the progression is drawn from the restarted seed, in request order). A random `load`
  inside a recording picks the same preset on replay when the recording was started from the recorder page (not from
  the serial `rec start`): REC ON and the render both **restart the random-load cycle of both sets** (§10 — the dots
  vanish, the "no repeat" memory is dropped), so the replay draws over the same presets as the recording did, as long as
  the presets of the genre are unchanged; a preset loaded by name from the manager replays exactly in any case.
- During a render the pages follow the replay under the overlay; **your keys do not reach them** — only `` ` `` held
  1 s (the cancel) and the state of SHIFT are taken from the keyboard, and SHIFT is put back as you hold it when the
  render ends. The transport is then stopped and the engine holds the session's end state — BtnA resumes. Recording is
  refused while a render runs (`RENDERING`), rendering while recording (`STOP REC FIRST`); the USB drive switch is
  refused while a render runs and stops a running recording first.
- `REC FAILED`: no card, `/lp/rec` missing, or 64 sessions; `RENDER FAILED`: a generate or save is running, or the file
  is gone. The serial command `rec list|start|stop|render N|cancel|delete N` does the same from the computer.

## 13. Sessions: what is remembered after power-off

The device writes `/lp/state/session_set0.lpset` and `session_set1.lpset` (two alternating copies each) five seconds
after your last change and at least 15 s apart, never while a loop slot streams (it waits for the slot tracks or the
transport to stop). At boot the newest complete file of each set is restored, nothing starts playing. Kept: key,
genre, tempo, every track's sound (preset or kit), pattern (by path; edited, transposed or recorded patterns as files
under `/lp/state`), variant, length, pause, strip and knob values, acid and loop mode, the preset label, the output
volume, and each set's random-load cycle (§10; a THEORY set comes back note for note from its pattern file). Precedence
at boot: **`bpm=` in config.txt wins over the session's tempo**; **`volume=` in config.txt (and the
serial `vol`, and SHIFT + kick cut) wins over the session's volume**; a preset loaded later brings its own tempo when
nothing plays. A session file cut short by a power-off is skipped and the previous copy restores.

## 14. The SYNTH page and the nine knobs

**SHIFT + 8 9 0 - `(8–11,0)`** opens the synth page of set 1's **Chord / Lead / Pad / Bass**; **SHIFT + i o p [** for set
2. Header `S1 LEAD  PluckWide` (`ACID` appended in acid mode); nine knobs drawn as arcs, two rows:

| knob | key up / down | what it is | range |
|---|---|---|---|
| CUT | fn / ctl `(0,2)/(0,3)` | filter cutoff | 80 Hz … 12 kHz |
| RES | sft / opt `(1,2)/(1,3)` | resonance | 0 … 98.5 % |
| ENV | a / alt `(2,2)/(2,3)` | filter envelope amount | 0 … 4 octaves |
| DEC | s / z `(3,2)/(3,3)` | filter envelope decay | 10 ms … 2 s |
| DRV | d / x `(4,2)/(4,3)` | pre-filter drive | 0 … 100 % |
| WAV | f / c `(5,2)/(5,3)` | oscillator wave (stepped) | SINE TRI SAW SQR FAT |
| **SAT** | g / v `(6,2)/(6,3)` | saturation inside the filter loop — the 303 squelch (0 = the clean filter) | 0 … 100 % |
| **ACC** | h / b `(7,2)/(7,3)` | accent depth: a note with velocity ≥ 100 gets its filter envelope × (1 + ACC) and up to +3 dB | 0 … 100 % |
| **CHO** | j `(8,2)` up / n `(8,3)` down | send to the set's chorus bus (a 15 ms modulated line, two taps, 0.5 Hz) | 0 … 100 % |

The knobs behave like the parameter keys: press = step 0.05, hold = ramp, release = freeze, SHIFT = ramp to the end with
snap-back on SHIFT release. **Exit** with the same SHIFT + pat combo or `` ` ``; SHIFT + another pat key switches to that
track. Track and tempo keys are not live on this page (tempo± and key± are SAT and ACC here). A new sound sets the knobs
to its own values (the knobs always describe what you hear); the SAT and ACC of a library preset are 0, those of a '303'
preset 50–80 %.

## 15. Acid mode (SHIFT + genre)

**SHIFT + k `(9,2)`** on a selected Chord / Lead / Pad / Bass toggles **acid mode** (`ACID ON S1 LEAD` / `ACID OFF`; `A`
on the square). On: the sound comes from the **'303' family** of the category (mono saw / square with slides for Lead
and Bass, filter stabs for Chord and Pad), the pattern is **generated on the device** — root-heavy 1/16 lines with
slides, accents and rests (Lead / Bass) or offbeat stabs (Chord / Pad) in the key — and every sound generation
randomises the knobs (CUT, RES, ENV, DEC, DRV, WAV, SAT, ACC, and CHO on Lead / Chord). Off: a library preset and a
library pattern again. Refusals: `ACID: SYNTH TRACKS ONLY`, `ACID: LOOP TRACK`, `SETS BUSY`. Acid mode is kept through
generates and saved with the set.

## 16. Loop mode (SHIFT + edit)

**SHIFT + n `(8,3)`** toggles **loop mode** on the selected track, any category, either set: the track streams a
pre-rendered 125 BPM loop of its category from the card (`/lp/packs/<Genre>/loops/<folder>`, docs/LOOPS.md) instead of
its pattern (`LOOP ON S1 BEAT`, `L` on the square; the square blinks while the loop is being picked). Off (`LOOP OFF
S1 BEAT`) brings the parked pattern and sound back at the bar.

- **Slots**: two loop streams for the whole device, plus the Vocal track of each set, which streams its loop through its
  own vocal stream and takes no slot. A third loop track answers `LOOP SLOTS FULL (2)`.
- **Tempo-125 rule**: the first loop to come into use pulls the tempo to 125 BPM (`LOOP ON - 125 BPM`), where the files
  are rendered — unless a tempo drift is running or the boot restore is bringing back your saved tempo. tempo±
  afterwards varispeeds every loop (pitch follows).
- **Variants**: the Beat layer keys crop a loop Beat (WHOLE, HALF, 2 BEATS, 1 BEAT); len± cycles the loop at natural ×
  2^k (one beat … 8 bars); the rush halves the length while held; the kick cut high-passes the stream.
- Gen pattern, gen sound, GENERATE and THEORY on a loop track **pick a new loop** and keep loop mode. The editor shows a
  read-only loop view (name, bars, variant). The perform page and acid mode refuse a loop track (`PERFORM: LOOP TRACK`,
  `ACID: LOOP TRACK`).
- Refusals: `NO LOOP: DRUM` (the category's folder is empty: drum, percussion, bass, chord, lead, pad, fx, vocal),
  `LOOP: BUSY` (a generate, save, load or restore runs), `LOOP: LOW RAM` (under 6 KB of free heap), `LOOP: NO CARD`.
- Loop mode and the exact file are saved with the set and the session; a missing file at load picks another loop of
  the category.

## 17. The editor (n)

**n `(8,3)`** opens the step editor of the selected track; `` ` `` or n closes it. Drums: four rows (KICK SNR HH OH, or the
Percussion voices A–D) × 16 steps of one bar; melodic tracks: the seven scale degrees of the track's key (bottom = root)
× 16 steps. Cursor with **; . , /** `(11,2) (11,3) (10,3) (12,3)`, **ent** toggles the note under the cursor (edits go to
the track's own pattern; the variants rebuild), **fn / ctl** velocity ± 10, **g / v** bar page, **f / c** octave (melodic;
on a Vocal the pause), **s / z** gate 1–4 steps. The footer shows `STEP 5  VEL 100  12 NOTES` or `OCT 4  GATE 1  VEL 100
A3`; the white bar under the grid is the playhead. `PATTERN FULL` at 192 notes. SHIFT + n inside the editor toggles loop
mode (the page becomes the loop view).

## 18. Perform mode (m)

**m `(9,3)`** opens the perform page of the **selected track** (SHIFT + m: the same track of the other set); `` ` ``
closes it. Synth tracks play their own voices on a grid: rows y0 x1–10, y1 x1–10, y2 x2–11, y3 x3–12, scale degree =
column + 3 × row (every key is in key, rows a fourth apart), default octave 3, **- / =** `(11,0)/(12,0)` octave, SHIFT =
+1 semitone while held. Beat: keys 1–8 of row y0 = kick, snare, hat, open hat and four accents; Percussion: keys 1–4 =
voices A–D. A Vocal has nothing to play (`NO PERFORM FOR VOCAL`). **tab `(0,1)`** starts / stops the track.

- **Quantise [ `(11,1)`**: OFF → 1/16 → 1/8 → 1/4; with the transport running a note waits for the next grid point (a
  key released before the point sounds a short note there); recorded notes sit on the grid (1/16 when OFF).
- **Chord mode \ `(13,1)`** (Chord / Pad tracks): one key plays the diatonic triad (`CHORD ON`); elsewhere `CHORD:
  CHORD/PAD ONLY`.
- **Looper del `(13,0)`**: tap = arm (`ARM`, a one-bar count-in) → `REC` → tap again to close at the next bar line
  rounded to 1 / 2 / 4 bars → `LOOP` (the track starts on that bar if it was stopped); further taps toggle overdub
  (`OVR`); **hold 700 ms clears** (`LOOP CLEARED`). The loop becomes the track's pattern: GENERATE and gen pattern keep
  it (`LOOP KEPT: HOLD DEL CLEARS`) until you clear it; gen sound still changes the sound. Leaving the page publishes an
  overdub and closes a take with notes at its bar line.

## 19. Gyro FX (j) and the imu.* config

**Hold j `(8,2)` for 0.4 s** to toggle the gyro FX (`GYRO ON`, `GYRO` in the footer). The pose at toggle-on is neutral.
Tilt **forward** → master high-pass up to the maximum, **backward** → low-pass; lean **left** → reverb send up, **right**
→ echo send up (both master sends). A ±10° dead band, full effect 45° beyond it, smoothed. Toggle off slews back to
bypass. If forward / backward or left / right come out swapped on your unit, set the axes in config.txt:
`imu.pitch_axis=x|-x|y|-y|z|-z` and `imu.roll_axis=…` (default pitch +x, roll +y). SHIFT + j is PLAY / STOP ALL and never
arms the gyro.

## 20. Output volume (SHIFT + kick cut)

**SHIFT + del `(13,0)`** raises the output level by 16 (of 255), **SHIFT + \ `(13,1)`** lowers it; hold repeats. The
OUTPUT page shows the level in percent and the footer `OUT 59%` for 3 s. Default 150; `volume=` in config.txt and the
serial `vol N` set it too. Every explicit level is written to the session and kept over a restored one.

## 21. USB drive mode (SHIFT + gen pattern held)

Hold **SHIFT + ' `(12,2)` for one second**: `USB DRIVE...` and the device reboots into the drive firmware, which hands
the card to the computer as a removable drive (a short SHIFT + ' is just gen pattern). Copy, merge or back up the `lp`
folder there (docs/CARD_IMPORT.md C). **Eject** the drive on the computer (`EJECTED` on the device), then press **ENTER**
on the device to come back to the player — ENTER before the eject asks for a second press within 3 s; any reset also
returns. The drive firmware must be flashed once with `tools/flash_drive.sh` (`NO DRIVE FIRMWARE` otherwise). The switch
is refused with `RENDERING` while a session renders (finish or cancel the render first), stops a running recording
(the session file is closed properly) and waits up to 3 s for a save, load, listing or session write to finish —
`SETS BUSY` when it does not, and the player simply goes on. Right after a boot the session writer may still be busy
and the first attempt (the key hold or the serial `drive`) can pass without effect — try again a few seconds later. The
serial port gets a new name while the drive is active. Some managed computers block removable drives: the device
enumerates (`M5STACK_CARDPUTER`, a mass-storage interface the system driver claims) but no volume ever appears, or it is
read-only — verified on a Mac under a management policy that disables external disks. Use a card reader or pull files
over the serial port with `tools/card_pull.py PORT /lp/packs/House/presets ./pulled`, which works with the normal
player firmware.

## 22. Battery

The footer's right end shows the charge (`87%`), polled every 5 s and smoothed; `--` while unknown; red below 15 %. The
Cardputer reports no charger state, so `CHG` never shows here and the red stays while a low battery charges.

## 23. Troubleshooting

| message | meaning / what to do |
|---|---|
| `NO SD CARD`, `LOOP: NO CARD`, `NO FS` | the card did not mount: FAT32, folder `lp` at the root, re-seat the card, power-cycle |
| `LOW RAM nK FREE nK MAX` at boot | less than 8 KB free / 6 KB largest block after boot — a one-off boot order issue; power-cycle; if it stays, the build has grown (SPEC §9.27 budget) |
| `NO RAM FOR LOADER` / `NO RAM FOR LOOPS` | the boot pools did not fit; loop mode stays silent (`NO RAM FOR LOOPS`); power-cycle |
| `BUSY`, `SETS BUSY`, `GEN BUSY`, `LOOP: BUSY` | a card job (generate, save, load, the boot restore, a listing) owns the card; wait a second and press again. A THEORY key queues behind a running job like GENERATE; it answers `GEN BUSY` only for a second press of the same set while its job runs, or when four requests already wait |
| `LOOP SLOTS FULL (2)` | both loop streams are in use (the Vocal track needs none); turn loop mode off on another track |
| `NO LOOP: DRUM` (PERCUSSION, BASS, …) | the genre's `loops/<folder>` has nothing for that category; add loops (docs/LOOPS.md) or run `write_indexes.py` |
| `LOOP: LOW RAM` | under 6 KB of free heap; close a loop or reboot |
| `LOOP UNDERRUNS n` in the footer | a loop stream starved since loop mode came on: card stalls (more likely right after writes) or too many card jobs at once; the number is samples (13k ≈ half a second); the counter resets when no loop is in use |
| `NO SET IN Amin` | no saved preset of this genre in the current key; save one, change the key, or use the preset manager with the ALL filter |
| `NO PRESETS`, `NO PRESET`, `BAD NAME`, `OTHER GENRE`, `BAD LABEL`, `OPEN TMP`, `RENAME`, `INDEX` | preset manager: nothing to list / the file vanished / a name not of this genre / a label with other characters than letters, digits, `-` / a card write failed (the preset may already be gone after `INDEX`: it heals on the next listing) |
| `NO SESSIONS`, `REC FAILED`, `RENDER FAILED`, `STOP REC FIRST`, `RENDERING`, `NO RECORDER` | recorder: nothing recorded yet / no card or `/lp/rec` or 64 sessions / a job runs or the file is gone / stop the recording before rendering / a render runs / the sketch has no recorder |
| `NO DRIVE FIRMWARE`, `DRIVE SWITCH FAILED` | run `tools/flash_drive.sh` once; a failed switch leaves the player running |
| `PATTERN FULL` | 192 notes in the pattern |
| `OCTAVE: …`, `ACID: …`, `PERFORM: LOOP TRACK`, `CHORD: CHORD/PAD ONLY`, `NO PERFORM FOR VOCAL`, `LOOP KEPT: HOLD DEL CLEARS` | the key does not apply to this track (see the section above) |
| crackle, gaps | the speaker queue ran dry (`underruns` in the serial `stat`): render too slow — fewer tracks with distortion and both sends, or a tempo above the loops' range; `vocal_underruns` / loop underruns are card starvation: avoid USB transfers and bursts of card jobs (generates, listings) while playing |
| nothing plays after boot | by design: the session is restored stopped; tap the tracks, BtnA, or SHIFT + j (PLAY ALL) |
| the keyboard feels "shifted" | SHIFT is the **space** key; a sticky SHIFT turns taps into selections and ramps — tap space once |

## 24. Quick reference

### Page 1 — the main layer

```
row y0  `  1  2  3  4  5  6  7   8    9    0    -     =     del
        CHD LED PAD BAS FX PRC VOC BEAT KICK +HAT +SNR FULL  RUSH  CUT     set 1  (tap = start/stop + select)
row y1  tab q  w  e  r  t  y  u   i    o    p    [     ]     \
        CHD LED PAD BAS FX PRC VOC BEAT KICK +HAT +SNR FULL  RUSH  CUT     set 2
row y2  fn  sft a  s  d  f  g  h   j    k    l    ;     '     ent
        VOL+ REV+ ECH+ DST+ FLT+ LEN+ TMP+ KEY+ GYRO GENRE SAVE1 LOAD1 GENPAT GEN S1
row y3  ctl opt alt z  x  c  v  b   n    m    ,    .     /     spc
        VOL- REV- ECH- DST- FLT- LEN- TMP- KEY- EDIT PERF SAVE2 LOAD2 GENSND SHIFT
BtnA    master play / stop (cancel a render)
```
- continuous keys: press = step, hold = ramp, release = freeze · len / key: single steps · tempo: hold repeats
- gyro: hold 0.4 s · genre: next pack · save: preset file · load: random preset in the key (no repeat until every one was loaded) · gen pat / snd: selected track
- edit: step editor · perf: play live, looper on del, quantise [, chord \ · Beat layers / rush / cut always act on that row's Beat

### Page 2 — SHIFT (space held) and the pages

```
SHIFT + track      select only            SHIFT + KICK/+HAT/+SNR/FULL   SYNTH page Chord/Lead/Pad/Bass of that set
SHIFT + CUT        output volume (y0 up, y1 down)                SHIFT + VOL..FLT ±   ramp to the end, snap back on release
SHIFT + LEN ±      ramp bars 1 2 4 8 16   SHIFT + TMP ±          drift ±10 BPM / 60 beats (again = stop, other = reverse)
SHIFT + KEY ±      octave ±1              SHIFT + GYRO (j)       PLAY ALL / STOP ALL
SHIFT + GENRE (k)  acid mode              SHIFT + SAVE1 (l)      RECORDER page
SHIFT + LOAD1/2    THEORY set 1/2 (one chord progression → Chord Lead Pad Bass Fx; drums + vocal from the library)
SHIFT + SAVE2 (,)  PRESET MANAGER         SHIFT + GENPAT (') 1 s USB DRIVE (ENTER returns)
SHIFT + ent        GENERATE set 2         SHIFT + EDIT (n)       LOOP MODE on/off (first loop → 125 BPM)
SHIFT + PERF (m)   perform the other set's track
```
```
SYNTH    CUT RES ENV DEC DRV = fn..d / ctl..x   WAV = f/c   SAT = g/v   ACC = h/b   CHO = j up, n down   exit: same combo or `
EDITOR   ; . , / cursor   ent note   fn/ctl vel   g/v bar   f/c octave|pause   s/z gate   n or ` exit   SHIFT+n loop
PERFORM  grid (y0 x1-10 … y3 x3-12) = degrees, rows a fourth apart   -/= octave   tab start/stop   [ quantise   \ chord
         del looper: arm → rec → close (1/2/4 bars) → loop → overdub; hold 700 ms clears     ` exit
PRESETS  row y0 / y1 key → target SET 1 / 2 ("-> SET n", dot = loaded in this cycle)   fn/ctl cursor   h/b filter ALL/key
         ent load into target   m AUTO   del delete (twice)   n rename (ent ok, ` cancel)   SHIFT+, or ` exit
RECORDER fn/ctl cursor   ent REC / STOP   m render WAV   del delete (twice)   ` exit   RENDERING: hold ` 1 s or BtnA cancels
STATUS   footer: KEY BPM GENRE LEN GYRO OUT% LOOP UNDERRUNS | REC | battery      marks: A acid, L loop, strike = kick cut
FILES    /lp/packs/<Genre>/presets/*.lpset (+ <name>_t<i>.mid)  presets · /lp/state  session · /lp/rec  sessions · /lp/wav  renders
```
