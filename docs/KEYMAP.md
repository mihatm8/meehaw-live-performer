# meehaw live performer — key map (SPEC §3, §4, §9.10, §9.14, §9.17c, §9.19, §9.20, §9.22, §9.23, §9.25–§9.27, §9.29)

Regenerated 2026-10-16 (evening, the §9.25–§9.27 wave) from `firmware/liveplayer/src/ui/ui.cpp` (`handleMain`,
`handleSynth`, `handleEditor`, `handlePerform`, `handlePresets`, `handleRecorder`) and `src/io/keys.cpp` (`kKeyMap`).
The manual is docs/MANUAL.md; printable key stickers come from `tools/make_stickers.py` (docs/stickers/). What changed
in this wave: **SHIFT + load 1 / 2 = THEORY generate** of set 1 / 2 (new), **SHIFT + save 2 = PRESET MANAGER** (moved
from SHIFT + load 1 / 2) with a **target set** chosen on the page, and the random **load** keys cycle through every
preset before repeating one (SPEC §9.26).

All keys are read as raw matrix positions `(x, y)`, x 0–13 left to right, y 0–3 top to bottom; the printed legends on
the Cardputer are only landmarks. "SHIFT" is the physical **space** key `(13,3)`, held. The physical rows:

```
y0: `  1  2  3  4  5  6  7  8  9  0  -  =  del
y1: tab q  w  e  r  t  y  u  i  o  p  [  ]  \
y2: fn sft a  s  d  f  g  h  j  k  l  ;  '  ent
y3: ctl opt alt z x  c  v  b  n  m  ,  .  /  spc
```

Side button BtnA = master play / stop (everything stops at once, a restart resumes the queued state); while a session
renders (SPEC §9.23) BtnA cancels the render. Track order per set: 0 Chord, 1 Lead, 2 Pad, 3 Bass, 4 Fx, 5 Percussion,
6 Vocal, 7 Beat; set 1 = tracks 0–7 (row y0), set 2 = tracks 8–15 (row y1). "Selected" = the one global selection
(default set 1 Beat); a track tap selects (SPEC §9.20). Hold repeats (350 ms, then every 80 ms) exist only where a row
says so.

## Base layer (main page; the parameter / note / status overlays keep every main key live)

| Key | (x, y) | Action |
|---|---|---|
| `` ` `` 1 2 3 4 5 6 7 | (0–7, 0) | Set 1 track CHD LED PAD BAS FX PRC VOC BEAT: **start / stop at the next bar and select the track**; a second tap before the bar line cancels the pending start / stop; a queued stop keeps sounding until the bar line |
| 8 9 0 - | (8–11, 0) | **Set 1 Beat** layer pat1–pat4 (KICK, +HATS, +SNARE, FULL) at the next bar — always the Beat of that row's set, whatever is selected (note page `S1 BEAT 2 +HATS`); queued while the set generates (`S1 PAT 2 QUEUED`). On a Beat in **loop mode** the four keys crop the loop instead: 1 = the whole loop, 2 = its first half, 3 = the first 2 beats, 4 = the first beat (`S1 BEAT 2 HALF`) |
| = | (12, 0) | **Snare rush set 1** (momentary): while held the Beat plays its Full layer cropped to one beat with the kick removed, from the next 1/16 (note `S1 RUSH`); release returns to the layer at the right position. On a Beat in **loop mode** the stream is cropped to **half its current clip length** (len / 2, at least one beat, never past the file; `Generator::rushLoop`, no `CmdSnareRush`) while held, the stored crop and length come back on release (SPEC §9.22) |
| del | (13, 0) | **Kick cut set 1** (toggle, at once): note 36 of the Beat muted (`S1 KICK CUT` / `S1 KICK ON`, a diagonal strike on the square); on a loop Beat the strip filter goes to +0.6 (high-pass) while the cut is on |
| tab q w e r t y u | (0–7, 1) | Set 2 track start / stop and select (as row y0) |
| i o p [ | (8–11, 1) | **Set 2 Beat** layer pat1–pat4 |
| ] | (12, 1) | Snare rush set 2 (as set 1, loop mode included) |
| \ | (13, 1) | Kick cut set 2 |
| fn sft a s d | (0–4, 2) | **vol+ rev+ echo+ dist+ filt+** of the selected track: **press = one step** (0.05; filter 0.1), **held = hold-ramp** towards the maximum at the SHIFT-ramp speed (the whole range in `rampBars` bars; the parameter page shows the live value and `HOLD`), **release = freeze** where the ramp got. One hold-ramp per parameter: a press while one runs (the other direction's key, or the same key on another track after a track tap / SHIFT + track) freezes it first and ramps on from the value reached. Opening the editor, perform, synth, preset or recorder page freezes every running hold-ramp |
| ctl opt alt z x | (0–4, 3) | **vol− rev− echo− dist− filt−**: the same towards the minimum (filter −1 = low-pass, 0 = bypass, +1 = high-pass) |
| f / c | (5, 2) / (5, 3) | **len+ / len−**: clip length × 2 / ÷ 2 (0.5 … 16 beats; LENGTH page `S1 CHD  2 -> 4`); on a Vocal the pause after the window (0 / 1 / 2 / 4 bars, `PAUSE 1 BAR`; `NO PAUSE` on the loop variants); single steps, no hold repeat; queued while the set generates (`S1 LEN +1 QUEUED`) |
| g / v | (6, 2) / (6, 3) | **tempo+ / tempo−**: ± 1 BPM (40 … 240), hold repeats; a plain press during a SHIFT + tempo drift stops the drift and steps from the tempo it reached |
| h / b | (7, 2) / (7, 3) | **key+ / key−**: next / previous minor key on the circle of fifths (A E B F# C# G# D# A# F C G D; future generation only), single steps |
| j | (8, 2) | **Gyro FX**: press-and-hold 400 ms toggles (`GYRO ON` / `GYRO OFF`; tilt → filter, lean → reverb / echo sends, neutral captured at toggle-on) |
| k | (9, 2) | **Genre**: next pack (GENRE page); refused with `BUSY` while a generator or Sets job owns the card |
| l / ; | (10, 2) / (11, 2) | **Save set 1** (a preset `<Genre>_<Key>_<NNN>.lpset` in the genre's `presets/`: a text file of `lp.*` keys that names its sources by card path, with `<name>_t<i>.mid` beside it for edited / recorded / THEORY tracks, SPEC §9.29) / **load** a random saved set 1 of the genre in the current key (`SAVED …` / `LOADED …`, `NO SET IN Amin`, `SETS BUSY`, `GEN BUSY`). The random load is a **no-repeat cycle** (SPEC §9.26): a preset loaded into that set — randomly or from the preset manager — is not drawn again until every other matching preset of the genre has been loaded into it; then a new cycle starts (never with the preset just loaded). The cycle is per set and per genre, survives power-off (`lp.loaded.<set>` in the session file), a newly saved preset joins the current cycle, a deleted one drops out |
| ' | (12, 2) | **Gen pattern**: a new pattern (all variants) for the selected track, sound unchanged (`GEN PAT 0/1`); refused with `LOOP KEPT: HOLD DEL CLEARS` on a track holding a recorded perform loop; on a loop-mode track it picks a new loop. On a melodic track of a THEORY set it turns that set back into a library set (SPEC §9.25) |
| ent | (13, 2) | **GENERATE set 1** from the library: all patterns and sounds of the eight tracks, one set-wide swap at the bar (`GEN S1 3/8`, then `GEN DONE` or the generator's note / error) |
| n | (8, 3) | **Editor page** of the selected track (on a loop-mode track the read-only loop view) |
| m | (9, 3) | **Perform page** of the selected track; `PERFORM: LOOP TRACK` on a loop-mode track |
| , / . | (10, 3) / (11, 3) | Save set 2 / load set 2 (the load cycles as set 1's does, with its own mask) |
| / | (12, 3) | **Gen sound**: a new sound (synth preset, drum / percussion kit or vocal file) for the selected track, pattern unchanged (`GEN SND`) |
| spc | (13, 3) | **SHIFT** (held). Releasing SHIFT snaps every SHIFT ramp back to its value before the ramp |

## SHIFT layer (space held)

| Key | (x, y) | Action |
|---|---|---|
| SHIFT + track key | (0–7, 0) / (0–7, 1) | Select that track without starting or stopping it |
| SHIFT + 8 9 0 - | (8–11, 0) | **SYNTH page** of set 1's Chord / Lead / Pad / Bass (pat1 = Chord … pat4 = Bass) |
| SHIFT + i o p [ | (8–11, 1) | SYNTH page of set 2's Chord / Lead / Pad / Bass |
| SHIFT + = / ] | (12, 0) / (12, 1) | Snare rush (as without SHIFT) |
| SHIFT + del | (13, 0) | **Master output volume +16** (headphone / speaker level 0 … 255, default 150; hold repeats; OUTPUT page in percent, `OUT 59%` in the footer for 3 s). Explicit: written to the session file and kept over the `lp.volume` of a session restored later in the same boot |
| SHIFT + \ | (13, 1) | Master output volume −16 |
| SHIFT + vol / rev / echo / dist / filt ± | (0–4, 2) / (0–4, 3) | **Ramp** to the maximum / minimum over `rampBars` bars (no hold repeat; page shows `RAMP`); releasing SHIFT snaps every such ramp back to the value before it. A hold-ramp running on that parameter is frozen first |
| SHIFT + len ± | (5, 2) / (5, 3) | `rampBars` 1 → 2 → 4 → 8 → 16 (RAMP page) |
| SHIFT + tempo ± | (6, 2) / (6, 3) | **Tempo drift**: ± 10 BPM over 60 beats, keeps going after SHIFT is released (TEMPO page `DRIFT`); the same key again stops it where it is, the other key turns it round from the current tempo; a plain tempo± also stops it |
| SHIFT + key ± | (7, 2) / (7, 3) | **Octave**: the selected track's pattern one octave up / down (0.6 s page `OCTAVE +1` / `OCTAVE -1`; the key is untouched). Synth tracks (Chord, Lead, Pad, Bass, Fx) with notes only: `OCTAVE: SYNTH TRACKS ONLY`, `OCTAVE: NO PATTERN` |
| SHIFT + j | (8, 2) | **PLAY ALL / STOP ALL** (SPEC §9.22): anything playing or queued → both sets stop at the bar (`STOP ALL`); otherwise every loaded track of both sets starts at the bar (`PLAY ALL`, pending stops are un-queued). Never arms the gyro |
| SHIFT + k | (9, 2) | **Acid mode** of the selected Chord / Lead / Pad / Bass track: `ACID ON S1 LEAD` / `ACID OFF`, the sound re-rolls ('303' family on, library preset off) and the SAT / ACC knobs follow; the main page marks acid tracks with an `A`. Other tracks: `ACID: SYNTH TRACKS ONLY`; a loop-mode synth track: `ACID: LOOP TRACK`; `SETS BUSY` during a save / load |
| SHIFT + l | (10, 2) | **RECORDER page** (SPEC §9.23; `NO RECORDER` without one). The same combo on the page closes it |
| SHIFT + , | (10, 3) | **PRESET MANAGER page** (SPEC §9.22, moved here by §9.27; was SHIFT + load 1 / 2). The same combo on the page closes it. The plain save 2 is unchanged. Replayed in an offline render the page opens as live (its delete / label still write nothing) |
| SHIFT + ; / . | (11, 2) / (11, 3) | **THEORY generate set 1 / set 2** (SPEC §9.25, `Generator::generateTheorySet`): the five melodic tracks (Chord, Lead, Pad, Bass, Fx) are written on the device from ONE chord progression of the flash tables — Pad voicings, Chord stabs, a Bass line and a Lead motif derived from it, in the current key, no card reads — while Beat, Percussion and Vocal regenerate from the library inside the same job; one set-wide swap at the bar as GENERATE. Status `THEORY S1 n/8` (`THEORY S1 0/8` until the queued job starts), then `GEN DONE` or the generator's note / error; patterns to be replaced drop their edited flag (a kept user loop stays), loop-mode tracks of the set blink while their new pick is in flight. Refusals as GENERATE: `SETS BUSY` (a save / load / listing owns the card), `GEN BUSY` only when the generator refuses — a second press for the **same** set while its theory job is running (a repeat for a set already waiting coalesces silently), or four requests already waiting. Otherwise the request **queues** behind the running job exactly like GENERATE: the progression is drawn when the job starts, in request order — the same live and in the recorder's slower offline render, so the re-seed replays the set. The patterns carry the label `THEORY <progression>` (`THEORY i-VI-III-VII`, shown by the editor's file name) and are marked edited, so presets (`presets/<name>_t<i>.mid`) and sessions (`/lp/state/session_s<set>_t<i>.mid`) store them as SMF files and reload exactly. Replayed in an offline render like GENERATE |
| SHIFT + ' | (12, 2) | Held **1 s**: **USB DRIVE** mode — status `USB DRIVE...`, then the device reboots into the drive firmware (SPEC §9.22; `NO DRIVE FIRMWARE` when `ota_1` is empty, not while a render runs). A short SHIFT + ' is the plain gen pattern of the selected track, acted on at the release |
| SHIFT + / | (12, 3) | Gen sound (as without SHIFT) |
| SHIFT + ent | (13, 2) | **GENERATE set 2** from the library |
| SHIFT + n | (8, 3) | **Loop mode** of the selected track on / off (SPEC §9.19, any category, either set): ON asks the generator for a loop of the track's category from the card's `loops/<folder>` (the square blinks while the pick is in flight; the Vocal track streams it through its set's own vocal stream, no slot used), `LOOP ON S1 BEAT`, the main page marks the track with an `L`; OFF returns the parked pattern and sound at the bar, `LOOP OFF S1 BEAT`. The first loop to come into use pulls the tempo to 125 BPM (`LOOP ON - 125 BPM`) unless a tempo drift runs or the boot restore owns the card; tempo± afterwards varispeeds every loop. Refusals: `LOOP SLOTS FULL (2)`, `NO LOOP: DRUM` (the category's folder: drum, percussion, bass, chord, lead, pad, fx, vocal), `LOOP: BUSY`, `LOOP: LOW RAM` (free heap under `kLoopLowRamBytes` = 6 KB since §9.27), `LOOP: NO CARD`. Gen pattern / gen sound / GENERATE / THEORY on a loop track pick a new loop and keep loop mode. Works inside the editor too |
| SHIFT + m | (9, 3) | Perform page for the same track of the **other** set |

## SYNTH page (modal; SHIFT + pat1–4)

Header `S1 LEAD  PluckWide` (`  ACID` appended in acid mode). Nine knobs in two rows — CUT RES ENV DEC DRV / WAV SAT ACC
CHO — drawn as 270° arcs with their real-unit values: CUT (Hz / k), RES (%), ENV (oct), DEC (ms / s), DRV (%), WAV
(SINE TRI SAW SQR FAT), SAT (%), ACC (%), CHO (%). Footer `VOL-KEY GYR EDT = KNOBS  HOLD RAMPS`. Every change marks the
session dirty.

| Key | (x, y) | Action |
|---|---|---|
| fn / ctl | (0, 2) / (0, 3) | **CUT ±** (cutoff 80 Hz … 12 kHz, log): step 0.05 of the knob range, hold-ramp, release freezes; SHIFT = ramp to the end, snap back on SHIFT release |
| sft / opt | (1, 2) / (1, 3) | **RES ±** (resonance 0 … 98.5 %), same behaviour |
| a / alt | (2, 2) / (2, 3) | **ENV ±** (filter envelope amount 0 … 4 oct) |
| s / z | (3, 2) / (3, 3) | **DEC ±** (filter envelope decay 10 ms … 2 s) |
| d / x | (4, 2) / (4, 3) | **DRV ±** (pre-filter drive 0 … 100 %) |
| f / c | (5, 2) / (5, 3) | **WAV** next / previous (SINE TRI SAW SQR FAT; stepped, no ramp) |
| g / v | (6, 2) / (6, 3) | **SAT ±** (SPEC §9.22: saturation inside the filter loop, the 303 squelch, 0 … 100 %), same behaviour as CUT |
| h / b | (7, 2) / (7, 3) | **ACC ±** (accent depth 0 … 100 %: a note with velocity ≥ 100 gets filter envelope × (1 + ACC) and up to +3 dB) |
| j | (8, 2) | **CHO +** (send to the set's chorus bus, 0 … 100 %), same behaviour |
| n | (8, 3) | **CHO −** (the edit key no longer exits the synth page) |
| SHIFT + the same pat key | (8–11, 0) / (8–11, 1) | Exit; SHIFT + another pat key switches the page to that set's track |
| `` ` `` | (0, 0) | Exit |
| tempo / key / track / other keys | — | Not live on this page (tempo± and key± are SAT and ACC here) |

## EDITOR page (modal; `n`)

| Key | (x, y) | Action |
|---|---|---|
| `` ` `` / n | (0, 0) / (8, 3) | Exit |
| SHIFT + n | (8, 3) | Loop mode of the track on / off (as on the main page); the page switches between the grid and the loop view |
| ; / . | (11, 2) / (11, 3) | Cursor up / down (rows: Beat KICK SNR HH OH; Percussion A–D; melodic the 7 scale degrees, bottom = root); hold repeats |
| , / / | (10, 3) / (12, 3) | Cursor left / right (16 steps of the shown bar, wraps); hold repeats |
| ent | (13, 2) | Toggle the note under the cursor in the track's source (variants rebuild; `PATTERN FULL` at 192 notes) |
| fn / ctl | (0, 2) / (0, 3) | Velocity of the note under the cursor ± 10 |
| g / v | (6, 2) / (6, 3) | Bar page + / − |
| f / c | (5, 2) / (5, 3) | Melodic tracks: shown octave + / − (1 … 7); Vocal: pause bars (as len±) |
| s / z | (3, 2) / (3, 3) | Melodic tracks: note gate 1 … 4 steps |
| other keys | — | Ignored |

Header `EDIT S1 BEAT  BAR 1/2`, footer `STEP 5  VEL 100  12 NOTES` (drums) or `OCT 4  GATE 1  VEL 100  A3` (melodic); a
Vocal shows `VOCAL: no pattern` with its pause / cycle. On a track in loop mode (SPEC §9.19) the editor is a read-only loop
view: header `EDIT S1 BEAT  LOOP MODE`, the loop's file name (without `.wav`) or `PICKING ...`, then `2 BARS  VARIANT 1
WHOLE` (the crop names WHOLE, HALF, 2 BEATS, 1 BEAT), footer `SHIFT+EDIT LOOP OFF  NO EDITS`; cursor, enter, velocity,
octave and gate keys do nothing there. A melodic track of a THEORY set edits like any other (its source is the generated
line; an edit keeps the `THEORY <progression>` label).

## PERFORM page (modal; `m`, targets the selected track)

Header `S1 LEAD OCT3` + ` Q1/16` (quantise on) + ` CHD` (CHORD mode) + the looper state (`ARM REC CLOSE LOOP OVR`) +
`  PLAY`. Synth tracks (Chord, Lead, Pad, Bass, Fx) play their own voices; Beat and Percussion play the kit slots; a Vocal
shows `NO PERFORM FOR VOCAL` (only tab and `` ` `` work). A track in loop mode does not open the page: `PERFORM: LOOP TRACK`.
Footer `TAB PLAY  DEL LOOP  [ QNT  \ CHD  A MIN`.

| Key | (x, y) | Action |
|---|---|---|
| `` ` `` | (0, 0) | Exit (an overdub is published, a take with notes closes at its bar line, an arm is dropped) |
| tab | (0, 1) | Start / stop the perform track at the next bar |
| - / = | (11, 0) / (12, 0) | Octave − / + (1 … 6; melodic tracks) |
| del | (13, 0) | Looper: tap = arm (one-bar count-in) → record → close at the next bar line rounded to 1 / 2 / 4 bars → loop; further taps toggle overdub; hold 700 ms clears (`LOOP CLEARED`). The loop becomes the track's pattern (`userLoop`: GENERATE / gen pattern keep it until cleared) |
| [ | (11, 1) | Quantise: OFF → 1/16 → 1/8 → 1/4 → OFF (`QUANT 1/16`); live note-ons wait for the next grid point, a key released before the point sounds a short note there, recorded timestamps sit on the grid (1/16 when OFF) |
| \ | (13, 1) | CHORD mode (Chord / Pad tracks): a grid key plays the diatonic triad degree, +2, +4 as three notes (`CHORD ON`); the looper records all three. Other tracks: `CHORD: CHORD/PAD ONLY` |
| grid, melodic tracks | y0 x1–10, y1 x1–10, y2 x2–11, y3 x3–12 | Scale degree = column + 3 × row (every cell in key, rows a fourth apart), default octave 3; SHIFT = +1 semitone (momentary) |
| slots, Beat | (1–8, 0) | KCK SNR HH OH A1 A2 A3 A4 = pitches 36 … 43 (40–43 are accents on the open-hat slot) |
| slots, Percussion | (1–4, 0) | Voices A B C D = pitches 36 … 39 live; the looper stores the pitch the track's fold map puts on that voice |
| other keys | — | Ignored |

## PRESET MANAGER page (modal; SHIFT + save 2, SPEC §9.22 / §9.26 / §9.27)

Lists the current genre's saved presets (`presets/*.lpset`), newest (highest NNN) first, hand-named files last. Header
`-> SET 1  ALL  23  AUTO`: the **target set** the next load goes into (the arrow is ASCII `->`: the fonts have no →),
the filter (`ALL`, or `C# MIN` with the key filter on), the count (`...` while the listing runs) and the AUTO flag. The
page is drawn in the target set's colour (green = set 1, cyan = set 2). Eight rows `NNN K  label-or-file-name`
(`007 A  my groove`, `012 -- Testpack_Amin_012`; the cursor row inverted, `...` while a row is still being read); a
**3 × 3 px dot in the left margin** marks a preset **loaded into the target set in its current random-load cycle** (SPEC §9.26; black on
the cursor row; set 1 and set 2 keep separate marks; a row without an NNN never has one). `NO PRESETS` when the genre
has none. Footer `ENT LOAD  M AUTO  N NAME  DEL  KEY FILT`, while renaming `TYPE LABEL  ENT OK  ` CANCEL`. Progress
texts `LISTING...`, `WRITING LABEL...`, `DELETING...`; the load shows `LOAD S2 3/8` and `LOADED <name>`. The target set,
the filter and AUTO are kept across opens (target SET 1 at boot).

| Key | (x, y) | Action |
|---|---|---|
| fn / ctl | (0, 2) / (0, 3) | Cursor up / down (hold repeats); the window scrolls and a new page of 16 rows is read from the card when the cursor leaves the loaded one |
| 1 2 3 4 5 6 7 | (1–7, 0) | **Target SET 1** (any track key of row y0 except `` ` ``, with or without SHIFT): header `-> SET 1`, green; the list, cursor and filter stay (the rows are the genre's), the dots switch to set 1's cycle; nothing is sent to the engine |
| tab q w e r t y u | (0–7, 1) | **Target SET 2**: header `-> SET 2`, cyan |
| h / b | (7, 2) / (7, 3) | Filter ALL ↔ the current key's minor root (names without a key label are listed under ALL only); the list restarts at the top |
| ent | (13, 2) | Load the highlighted preset into the **target set** (the file's own set index is ignored; `LOAD S2 k/8`); the load marks the preset in that set's cycle; with AUTO on the target set starts at the bar once the load completed (`PLAY`). Refusals `SETS BUSY`, `GEN BUSY`, `BAD NAME`, `OTHER GENRE`, `NO PRESET`, `LOAD FAILED` |
| m | (9, 3) | AUTO on / off (`AUTO` in the header) |
| del | (13, 0) | `DELETE? PRESS AGAIN` for 2 s; the second press — on the same row, target set and filter (a target toggle inside the window makes the next del a fresh first press) — deletes the preset file, its `_t<i>.mid` pattern files and its index line (`DELETED`, else `DELETE FAILED` / `INDEX`); a legacy preset's `assets/` copies are left alone (the firmware never deletes that folder); the number's cycle bits are cleared in both sets |
| n | (8, 3) | RENAME: edit the label (`lp.label`, up to 19 characters) in place — letters from the legends (y1 x1–10 `qwertyuiop`, y2 x2–10 `asdfghjkl`, y3 x3–9 `zxcvbnm`; SHIFT = upper case), digits (y0 x1–10), `-` (11, 0), del = backspace, ent = write it (`LABEL SET`, else `BAD LABEL` / `OPEN TMP` / `RENAME` / `LABEL FAILED`), `` ` `` = cancel. While typing every other key (SHIFT + save 2 and SHIFT + load 1 / 2 included) is swallowed. A set remembers the label of the preset it was loaded from and writes it into its later saves and its session file |
| SHIFT + , | (10, 3) | The page's own combo closes it |
| `` ` `` | (0, 0) | Exit (outside RENAME) |
| SHIFT + ; / . | (11, 2) / (11, 3) | **Not live here** (the page is modal: no theory job, no set switch) |
| other keys | — | Ignored |

## RECORDER page (modal; SHIFT + save 1, SPEC §9.23)

Header `RECORDER  3 SESSIONS` (+ red `REC` top right while recording), rows `rec_007   4 BARS  W` (bars from the session
header, `--` until read; `W` = a WAV exists), `NO SESSIONS`. Footer `ENT REC  M WAV  DEL  ` EXIT` (`ENT STOP …` while
recording).

| Key | (x, y) | Action |
|---|---|---|
| fn / ctl | (0, 2) / (0, 3) | Cursor up / down (hold repeats) |
| ent | (13, 2) | Start a recording (`REC ON`; the page closes and the main footer blinks a red `REC`), or stop the running one (`REC STOPPED`). `REC FAILED` (no card, `/lp/rec` missing, 64 sessions), `RENDERING` while a render runs |
| m | (9, 3) | Render the highlighted session to `/lp/wav/rec_NNN.wav` (offline, the speaker is silent): the RENDERING overlay. `NO SESSIONS`, `STOP REC FIRST`, `RENDER FAILED` (busy generator / Sets, missing file) |
| del | (13, 0) | `DELETE? PRESS AGAIN` for 2 s; the second press removes the session and its WAV (`DELETED` / `DELETE FAILED`; refused while recording or rendering) |
| SHIFT + l / `` ` `` | (10, 2) / (0, 0) | Exit |
| other keys | — | Ignored |

**RENDERING overlay** (covers every page while a render runs): `RENDERING rec_007`, the percentage huge with a bar,
footer `HOLD ` TO CANCEL`. `` ` `` **held 1 s** (or BtnA) cancels (`RENDER CANCELLED`, the partial WAV is removed); a tap
of `` ` `` is a replayed track key and never cancels. When the render ends: `DONE` + `/lp/wav/rec_007.wav` for 3 s, then
the main page. During a render a **live key never reaches the pages** (it would change the replayed world and the WAV
would no longer match the recording): only `` ` `` held (the cancel) and the physical SHIFT state count, and that SHIFT
state is put back when the render ends — the replay's own SHIFT, held keys and running hold-ramps are dropped / frozen
then. A replayed save, SHIFT + save 1, USB drive hold, preset DELETE or RENAME writes nothing and reboots nothing (every
render would otherwise add a preset copy or unlink one); loads, generates (THEORY included), params, acid, loop mode and
pages (SHIFT + save 2 included) replay as live. What is recorded while recording: every key press and release (SHIFT
included) except the recorder page's own keys, every gyro command and output-volume change, and BtnA's master play /
stop. **REC ON and a render both restart the random-load cycle** (`Sets::resetLoadCycle()` beside the generator's re-seed,
SPEC §9.26 / §9.28): the masks of both sets and the "no immediate repeat" names are cleared, the preset manager's dots
vanish and the next random loads start a fresh cycle in each set — the price of an exact replay: a random load inside a
recording draws over the same candidates in the render and lands on the same preset (a named load from the preset
manager replays exactly in any case).

## Main page marks and footer

`A` top right of a square = acid mode, `L` top left = loop mode (a filled square shows them in black); a diagonal strike
= kick cut; the small grey square = the selected track, the 1–4 ticks under it = its variant; a square blinks while a
start or stop is queued or a loop pick is in flight. Footer, left: `A MIN 125  HOUSE  LEN 4` plus the notes `GYRO`,
`OUT 59%` (3 s after a SHIFT + kick cut step) and `LOOP UNDERRUNS n` (a loop slot starved since loop mode came into use;
in thousands from 10 000, `13k`), each shown whole or dropped (drop order with the loop note up: OUT, GYRO, genre, LEN).
Right: a blinking red `REC` while a session records and the battery (`87%`, `CHG 87%`, `--`; red below 15 %). The
status line above the footer shows job progress (`GEN S1 3/8`, `THEORY S2 3/8`, `SAVE S1 2/8`, `LOAD S2 3/8`,
`LISTING...`) and the messages quoted in this file. A THEORY set is not marked on the main page; the progression shows
as the pattern's file name in the editor (`THEORY i-VI-III-VII`) and is available to the firmware as
`Generator::theoryProgression(set)`.

## Serial commands — player (USB, one per line; `tools/serial_cmd.py PORT "stat"`)

| Command | Action |
|---|---|
| `k X Y` | Press and release matrix key (x, y) on the next scan (`kd 13 3`, `k 11 2`, `ku 13 3` = SHIFT + load 1 = a THEORY set 1; there is no dedicated theory command) |
| `kd X Y` / `ku X Y` | Hold / release matrix key (x, y) (e.g. `kd 13 3` holds SHIFT) |
| `note P V` | Perform note pitch P, velocity V 0–100 (0 = off) on set 1's Fx track |
| `tog T` | Toggle track T (0–15) at the next bar |
| `gen S` | Generate set S (0 / 1) from the library (`Generator::generateSet`) |
| `loop T` | Toggle loop mode on track T (as SHIFT + edit); prints the refusal |
| `play` | Start every loaded track of both sets at the bar |
| `demo` | Synthetic full load: patterns, presets and noise kits on all 16 tracks |
| `vol N` | Master output volume 0–255 (explicit: written to the session file, kept through a session restore; `vol 0` for a silent test session) |
| `stat` | Status line (heap, render max, underruns, session state), the four streams, per-track state / acid / loop / knobs, render profile, stacks |
| `ls DIR` / `cat FILE` | List a card directory (sizes print as 0) / print the first 2 KB of a card file |
| `get FILE` | Pull a card file: `G <size>` then exactly `<size>` raw bytes (`G -1` when missing); used by `tools/card_pull.py`. Stop the transport before pulling a WAV (the streams are not refilled meanwhile) |
| `drive` | Reboot into the USB drive firmware (as SHIFT + gen pattern held 1 s); `[drive] no drive firmware in ota_1` when none; refused silently while the session writer is busy right after boot — try again a few seconds later |
| `rec list` / `rec start` / `rec stop` / `rec render N` / `rec cancel` / `rec delete N` | The session recorder from the serial port; N = the row index of `rec list` |

## Serial commands — card loader and card drive firmware

`firmware/cardloader` (fills the card over USB, docs/CARD_IMPORT.md B): `PING` → `PONG`, `LS DIR`, `PUT`, `CRC`, `RM`,
and `GET FILE` → `G <size>` + the bytes in 4096-byte chunks (`G -1` when missing).

`firmware/carddrive` (USB drive mode, docs/CARD_IMPORT.md C): the screen says `USB DRIVE` with the card size, `mounted rd N
wr N` / `EJECTED rd N wr N`; **ENTER** `(13, 2)` returns to the player once the computer has ejected the drive (before the
eject it asks for a second press within 3 s); serial `player` → restart into the player, `stat` → `[drive] sectors=…
rd=… wr=… err=… ejected=…`. Any reset returns to the player.
