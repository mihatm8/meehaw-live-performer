# Loop mode — design contract for the implementation wave (SPEC §9.19 draft, 2026-10-16)

User request (verbatim): "introduce loop mode at 125 bpm - it can be enabled via shift + edit for the selected
track, let's have the vocal loops here too. normalize the loops first to fit 125 bpm, then copy what you need to
the sd card." Content is on the card (docs/LOOPS.md): `/lp/packs/<Genre>/loops/<category>/<Key|NA>/[sNN/]
<Key|NA>__125bpm__LOOP__<n>bars__<slug>.wav`, 22050 Hz mono int16, exactly n × 42 336 frames, n ∈ {1, 2, 4, 8}.
The API below is already declared in config.h, types.h, engine.h, generator.h, library.h, sets.h (git diff):
implement against it, do not rename it; add private members freely.

## 1. Behaviour
1. **SHIFT + edit** `(8,3)` toggles LOOP MODE on the selected track (any category, either set). Status
   `LOOP ON S1 BEAT` / `LOOP OFF S1 BEAT`; the main page marks loop tracks with an `L` (as acid's `A`). ON picks a
   loop at once (one generator job, `JobLoop`: prepare → pick → open → `CmdLoopLoaded` + `CmdLoopMode on` at the
   bar line); while the pick is in flight the square blinks as during a generate. OFF sends `CmdLoopMode off`: the
   track's parked pattern and patch play again at the bar (nothing is re-read from the card; `source_`, `next_`
   and the patch are left untouched while in loop mode).
2. Category → folder (`Generator::loopFolder`): Beat→drum, Percussion→percussion, Bass→bass, Chord→chord,
   Lead→lead, Pad→pad, Fx→fx, Vocal→vocal. Roots per `Library::prepareLoop`: tonal categories use the key's minor
   label, its relative major label and NA (as `pickVocal`); Beat / Percussion / Fx use NA plus every key folder
   present. Shards (`sNN`), `_index.txt`, shuffle bags and the no-repeat history as every other picker. Nothing
   found → `LoopNoContent`, status `NO LOOP: DRUM`, loop mode stays off.
3. **Tempo**: files are at `kLoopBpm` = 125; the stream varispeeds at rate = bpm / 125 like a vocal (retime on
   tempo change). When `Generator::anyLoop()` goes from false to true and no PTempo drift ramp is active, the UI
   sends `CmdSetTempo 125` (status `LOOP ON · 125 BPM`). Tempo± afterwards varispeeds every loop.
4. **Clip cycle**: natural = `<n>bars` × `kTicksPerBar` (384..3072 ticks; `Generator::parseBarsToken`).
   Pattern keys 1–4 on a loop track: 1 = whole loop, 2 = first half, 3 = first 2 beats, 4 = first beat (wrap
   crossfade `kXfade` = 64 samples as the vocal loop variants). len± = natural × 2^k clamped to [1 beat,
   `kMaxLoopTicks`]; the engine accepts clip lengths up to `kMaxLoopTicks` on a streaming track (patterns keep
   `kMaxPatternTicks`). Kick cut on a loop Beat track = the strip filter raised to +0.6 (high-pass) while the cut is
   on and restored after; snare rush on a loop Beat track = variant 4 momentary (today's rush semantics).
5. **Generates**: gen pattern, gen sound and GENERATE set on a loop track pick a new loop (same job); loop mode is
   retained by generates, as acid mode is. The **Vocal track** in loop mode streams the `loops/vocal` file through
   its set's own vocal stream (stream 0 / 1): a plain loop, no window + pause cycle, variants as above, no slot used.
6. **Refusals**: `PERFORM: LOOP TRACK` (perform page on a loop track), `ACID: LOOP TRACK` (acid toggle), `LOOP
   SLOTS FULL (2)` (`LoopSlotsFull`), `LOOP: LOW RAM` (`LoopLowRam`: free heap below `kLoopLowRamBytes`), `LOOP:
   BUSY` (`LoopBusy`: a generate / save / load is running). The editor page on a loop track shows the loop's name,
   bars and variant (no grid, no edits).
7. **Persistence**: `lp.track.i.loopmode=1` in presets and session files; the loop is the track's clip (clipType
   WAV, the file copied into `presets/assets/` like a vocal — Reaper imports a WAV clip); session files carry the
   absolute path in `lp.src.i`. A restore / load with loopmode=1 restores loop mode with that exact file; a missing
   file → pick a loop of the category instead. Strip and knob values are kept exactly as §9.17b does for every track.

## 2. Streams and memory — measured on the device 2026-10-16 09:20 (build df70a19)
| | |
|---|---|
| static (Arduino "Global variables") | 262 980 B (80 %) |
| heap idle after boot | 25 068 B free, largest 13 300 |
| heap, both sets playing (16 tracks, 2 vocal files open) | 15 100 B free, largest 7 668 |
| heap during a session write (2 more files open) | 10 432 B → **≈ 2.3 KB per open card file** |
| task stacks, min free | render 2 336 of 3 584 B, worker 1 892 of 3 072 B, loop 4 428 of 8 192 B |
| render | 5.4 ms max per 11.6 ms block, 16 tracks |

The loop rings therefore come from **static reclaim, not heap**:
- `kLoopSlots` = 2 loop streams device-wide (`kStreams` = 4: 0 / 1 the sets' vocal streams, 2 / 3 the loop slots),
  each bound to one track of either set; the Vocal track's loop mode uses stream 0 / 1, never a slot.
- Loop ring = **int16, `kLoopRingBytes` = 6 144 B** (3 072 samples = 139 ms), refilled in `kLoopChunkSamples` =
  1 024-sample (2 KB) reads whenever the ring has that much space: worst-case margin 93 ms, equal to the vocal
  ring's worst case (4 096-sample ring, 2 048-sample chunks). Memory is handed in by the host:
  `Engine::setLoopRing(slot, mem, samples)` — liveplayer.ino allocates both rings once in `setup()` with
  `bootBlock` (never freed; SPEC §9.13's "no allocation after setup()" holds), firmware/desktop and the tests use
  static arrays. The boot floor check stays (free ≥ 20 KB, largest ≥ 8 KB).
- **Static reclaim (engine owner), −14.8 KB**: (a) the echo line becomes µ-law `uint8_t[kEchoSamples]` (dsp/echo.*,
  `ulawEncode`/`ulawDecode` of dsp/ulaw.h; the wet path only — truncation toward zero must still let the feedback
  tail reach exact silence; update tests/test_fx.cpp accordingly) −10 240 B; (b) liveplayer.ino's three static
  scratch buffers (`loadConfig` 2 048, `cat` 1 536 and 2 048) become one shared 2 048 B buffer (never live at the
  same time) −3 584 B; (c) task stacks render 3 584 → 3 072, worker 3 072 → 2 560 (≥ 1.2 KB margin over the
  measured high-water marks) −1 024 B. Added: 2 × 6 144 B rings + 2 × 272 B heads + state ≈ +13 KB. Net ≈ −1.8 KB.
- **Heap (generator owner), optional −5 KB**: `Sets` may borrow the engine's `staging_` buffer (8 820 B) for its
  `chunk_` (4 096) and the `PropertiesReader::line_` (1 024) — ONLY if you can show, in code and in a test, that
  Sets never performs card work while the generator is busy or a `CmdSlotSwap` is pending (restore and load drive
  the generator for kits: check the interleaving). If that cannot be shown, skip it and say so in the report.
- Open card files: one per active stream plus one during a switch (`nxt`). `SdFs::kMaxOpen` 6 → 10. `Generator`
  refuses a loop when `freeHeap_()` (device: `ESP.getFreeHeap`) is below `kLoopLowRamBytes`.
- Engine: a streaming track renders its ring (`renderStream`, the `renderVocal` loop) and then the normal strip
  (dist → filter → duck → vol → limiter); its synth / sample voices are released when loop mode turns on and the
  pattern is not stepped while it is on. Per-stream underruns (`streamUnderruns`), shown in the footer as `LOOP
  UNDERRUNS n` when > 0 (UI) and in `[diag]` / `stat` (ino).
- Everything bar-locked exactly like the vocal (SPEC §9.3): start sample = the bar line the clip starts on, file
  frame = (pos in the clip cycle) × rate, `Generator::retime` on tempo change, `pump()` serving all four streams
  emptiest ring first, `kPumpChunks` 3 → 4.

## 3. Session-writer starvation (generator owner, measured bug)
One parameter change → 2 s later the session write runs and the vocal stream loses **≈ 12 500 samples (0.57 s)**
over the write (bars 293–297 s in the log: `vocal_underruns` 16 865 → 28 045, heap dips to 10 432). Fix:
(a) **stream-health gate**: `Generator::streamsHealthy()` = every active stream ring is at least half full (or no
stream is active); `Sets::step()` performs card work only when it is true, otherwise returns and lets the pump
refill first (the pump always runs); expose the same gate to the generator's own file-open steps. (b) The session
write path makes **no verification opens** (no opening a source file to check it exists; write the paths the
generator knows — if it only knows basenames today, remember the picked folder or path). (c) `kSessionDebounce`
5 s and at least 15 s between two session writes; a write still waits while the generator is busy. (d) Host test
with a slow-FS decorator (an `IFileSystem` wrapper advancing the engine's sample clock by 150 ms per open / write
and 20 ms per read): zero vocal underruns through a session write with both vocal streams active, and with two
loop streams active. Also: GENERATE set must keep a recorded (perform) loop on **every** track, not only Fx
(generator.cpp ≈ line 600, `c == Fx && doPat_ && userLoop` — drop the Fx condition; perform records on any track).

## 4. Deliverables
Firmware (engine + ino + echo, generator + library + sets, ui), host tests for every rule above (tests/ runners;
`sh tests/run_all.sh` green), firmware/desktop builds, `arduino-cli compile` for the ADV passes (no upload),
docs: SPEC.md §9.19 (this contract, final numbers), KEYMAP.md (SHIFT + edit row, loop statuses), LOOPS.md
("firmware reads this folder now" paragraph). Device verification is done afterwards by the orchestrator at
output volume 0.
