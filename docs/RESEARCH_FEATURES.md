# Feature feasibility: acid layer, auto-rap, live mic echo, loop mode

Research synthesis, 2026-10-15; nothing under `liveplayer/` except this file was changed. Sources: the four
research reports (acid-layer, tts-rap, mic-echo, loop-mode) and `docs/SPEC.md` §1/§9/§10; line numbers are the
reports' citations, constants re-checked in `config.h`/`generator.h`.

## 0. Baseline

| Resource | Now (source) |
|---|---|
| Heap, both sets loaded, idle | 20 172 B free / 9 716 B largest (serial `stat`, `liveplayer.ino:79`) |
| Heap, both sets playing | 19–21 KB free / 8–10 KB largest; boot 30.9 / 18.4 KB; 20 KB / 8 KB floor asserted at boot only (SPEC §10.1, §9.13) |
| Render block | 256 samples = 11.6 ms; 5.3–5.4 ms with 16 real tracks, 6.0 ms synthetic worst → ≈ 6 ms headroom (SPEC §10) |
| Per-sample costs | synth voice 850 ns, sample voice 200 ns, strip 475 ns, SVF 133 ns, µ-law encode 250 ns, tanhf 1630 ns (SPEC §1, §10) |
| Flash | 713 KB of 3 MB |
| Fixed buffers | pools 2 × 48 952 B (`config.h:31`), staging 8 820 B, vocal rings 2 × 8 KB (`:37`), echo 10 240 int16 = 20 KB (`:46`), one shared 4 KB chunk window (`generator.h:206`) |
| SD | ≈ 1 MB/s; 4 KB chunk 3–5 ms; 20 ms burst cap; open by name 23–25 ms in a 64-entry folder; handle pool 6 (`sd_fs.h`) |

RAM figures are static or boot-allocated bytes (SPEC §9.13).

## 1. Summary

| Feature | Verdict | RAM | CPU | What must be traded | Effort |
|---|---|---|---|---|---|
| Acid layer (303 mode on SynthVoice) | Yes, cheap | +0.6 KB static (+1 KB with a ladder filter) | 12 voices ≈ 1.4–1.5 ms/block summed over both cores vs ≈ 2.6 ms today: neutral or cheaper | `edit` becomes a hold layer (opens the editor on release); 4 new ParamIds | 3–4 days (+1–2 ladder) |
| Auto-rap on Vocal (pre-rendered word bank) | Yes | 0.05 KB, or a 2 KB boot block if the index is not in flash | ≈ 0.1 % of core 1 per stream; render task unchanged | 22 KB/s SD per stream; a Mac tool and a licensed voice | 4–5 days |
| Live mic → echo (direct full-duplex I2S) | Yes, with caveats | ≈ +1 KB net | ≈ 0.45 % of a core (+50 µs/block); render 5.4 → 5.5 ms | Replaces `M5.Speaker` with an own `i2s_std` driver; a listen chord; speaker-mode howl limits | 1-day probe + 4–5 days |
| Loop mode (125 BPM loops streamed from SD) | Yes, largest job | ≈ 8.6 KB per stream; 5 streams = 42 KB carved from the set's idle 49 KB sample pool | Engine CPU falls (10–20 ns/sample per loop vs 1.7–3.4 µs of synth voices); loop task ≈ 75 SD chunk reads/s | Beat/Perc one-shots in loop sets; handle pool 6 → ≈ 2N+4; 314 MB card rewrite; tempo± detunes tonal loops | 7–10 days |
| Rejected: on-device time-stretch, esp-tts, SAM (shipped), MiniAcid voice/delay port, M5Unified Mic route | No | 24–27 KB/stream, 20 KB, 88 KB, 8.7 KB respectively | 10–13 % of a core/stream (phase vocoder), 3.5–4 µs/sample (MiniAcid voice) | — | — |

## 2. Acid layer (MiniAcid mode)

Design: reuse `SynthVoice` (`src/dsp/synth_voice.h`) with a '303' preset family appended at the end of the
kLead/kChord banks (≤ 16 per bank, `generator.cpp:312`) so saved `lp.track.N.preset` indices stay valid,
picked only in acid mode. Three zero-cost voice fixes: (a) a `SynthPatch` flag "glide only on legato",
appended after `name` (`types.h:79`), because `synth_voice.cpp:186` glides every note of a polyphony-1 patch;
(b) accent → filter env, `fEnvOct_ = p.filterEnv × (vel ≥ 100 ? 1.5 : 1)` (`:209`); (c) resonance clamp 0.95 →
0.985 (`:212`, `svf.h:13`): Q ≈ 5 → ≈ 8, no self-oscillation. A slid 16th must be 36 ticks, not 24: note-offs
precede note-ons on a tick (`engine.cpp:181-187`) and legato needs `noteCount > 0` (`:237`).

Generator: seeded `acidLine()`/`acidStabs()` in `variants.cpp` beside `generatedFx` (`:353-374`),
key-constrained by `ProjectKey::degreePitch/snap` (`key.h:13-15`), writing `eng_->sourcePattern(track)` like
the looper (`ui.cpp:778`) so variants and set save work unchanged. Chord gets stabs and accents only.

Control: hold `edit` = momentary ACID layer; columns x0–x3 = cutoff / resonance / env / decay, x4 echo toggle
(PEcho 0 ↔ 0.4), x5 distortion toggle (PDist 0 ↔ 0.6). The knobs are four new `ParamId`s in `TrackParams`
(`types.h:96`, +16 B × 32 = 512 B), smoothed by the existing 10 ms one-pole (SPEC §9.12) and applied in
`updateBlock` every 32 samples (`synth_voice.h:56`); `CmdSetPatch` alone is read only at noteOn
(`synth_voice.cpp:194-215`) and would step on legato notes. SHIFT bar ramps come free via `CmdRamp`
(`engine.cpp:124-131`).

Numbers: a 303 preset ≈ 400–450 ns/sample/voice (anchors 387 ns sine, 832 ns full preset, SPEC §9.15); 4 chord
+ 2 lead per set = 12 voices ≈ 1.4–1.5 ms summed, ≈ 0.7 ms per core (6–7 %), against ≈ 2.6 ms at 850 ns today.
Knobs 1–2 ns/sample/voice. RAM +0.6 KB. Flash ≈ 5 KB, or 15–25 KB with templated ladder kernels. MiniAcid's
code is not portable: `TB303Voice::process` calls tanhf + expf + sinf per sample (≈ 3.5–4 µs; 12 voices ≈ 11
ms of 11.6) and `TempoDelay` is an 88 KB float vector.

Unknown: expf/sinf cost on the S3 (only tanhf measured); the 4-pole ladder (+40–80 ns/sample/voice estimated;
bench in `firmware/wtbench` first); whether the 12 dB/oct SVF at Q ≈ 8 sounds acid enough; set-0 wavetables
carry 40 partials (2.6 kHz at MIDI 36), duller than a 303 saw; gain staging of four resonant stabs (voice
level ≈ 0.35).

## 3. Auto-rap on the Vocal track

Routes: A, on-device SAM: 3.5 KB boot block (or 0 via the staging buffer), ≈ 30 KB flash, < 1 % CPU, but a
1982 robot voice, English only, no licence. B, esp-tts: unusable (Chinese only, 16 kHz, 20 KB runtime RAM =
the whole free heap, 2.9 MB voice partition). C, recommended: pre-rendered word banks.

Design (C): a Mac tool `tools/make_rap_bank.py` renders a word list with TTS, trims it, converts to 22050 mono
µ-law and writes ONE file per genre `/lp/packs/<Genre>/rap/bank.ulaw` (header: magic, hash, N, per word offset
u32, len u16, syllables, stress, CMUdict rhyme class) plus a generated `rap_vocab_<genre>.h`. One file because
an open by name costs 23–25 ms (over the 20 ms cap) while `IFile::seek` (`fs.h:15`) plus one 4 KB read ≈ 4–5
ms is what `Generator::fetch` already does (`generator.cpp:961`). Firmware: a per-set schedule of ≤ 12 slots ×
4 B; a template + rhyme-class line generator (< 100 µs) fills the next 2-bar window at the bar line; a
bank-sourced twin of `produce` (`generator.cpp:926`) writes words through the existing 4 KB `chunk_` into the
existing 8 KB ring; `renderVocal` (`engine.cpp:253`) is unchanged. UI: "gen sound" alternates phrase ↔ bank;
variants unchanged; echo send 0.15 instead of 0.35.

Numbers: RAM 48 B + 0 (index in flash) or a 2 KB boot block (256 words × 8 B); a RAM word kit (8–16 words ×
300 ms µ-law = 53–106 KB) does not fit. CPU ≈ 20–50 ns/sample on core 1 ≈ 0.1 % per stream. SD 5.5 KB per 250
ms word = 22 KB/s per stream, ≤ 8 seek+reads ≈ 40 ms per 2 s bar. Flash ≈ 10 KB. Words measured 343–561 ms
against 250 ms per 1/8 at 120 BPM: render at `-r 240` or allow 2-slot words; 4–6 words per bar, 1/16 only as
stutter.

Risks: a word's first chunk must be in the 186 ms ring before its onset, so loop-task stalls over ≈ 150 ms (a
set save starved the ring 0.5 s, SPEC §10.1) silence words; bank and header must match (hash check).
Licensing: macOS `say` voices are non-commercial (own card only), Piper lessac research-only; libritts_r (CC
BY 4.0) or espeak-ng output are safe to ship.

Unknown: FAT `f_lseek` cost on a 1.5–2 MB bank (expected ≤ 1 sector read; measure); intelligibility of 4
words/s through the NS4150B 8 Ω 1 W speaker under a beat; SAM ns/sample on the S3.

## 4. Live mic → echo

M5Unified 0.2.26 cannot do it: Mic (`I2S_NUM_0`, `M5Unified.inl:2758-2765`) and Speaker (`I2S_NUM_1`,
`:3105-3116`) are both I2S masters on the same BCLK/WS pins and their ES8311 callbacks overwrite each other's
REG01 (`:1045-1068`, `:1537-1562`); M5Cardputer's own example does `Speaker.end(); Mic.begin();`.

Design: `cfg.internal_spk = cfg.internal_mic = false`; a new `src/platform/audio_io.{h,cpp}` on ESP-IDF
`i2s_std`: one `i2s_new_channel(&c, &tx, &rx)` on `I2S_NUM_1` (`dma_desc_num 3`, `dma_frame_num 256`; the
documented full-duplex form, `i2s_common.h:116-121`), `i2s_channel_init_std_mode` on both at 22050 Hz /
16-bit, `i2s_channel_preload_data(tx, silence)`, enable tx then rx. ES8311 over `In_I2C`: REG01 = 0xBF (all
clocks), PGA REG14 = 0x17 (+21 dB), REG17 = 0xCB, HPF REG1C = 0x64, REG0D = 0x01 written ≥ 30 ms after the
clock runs (`M5Unified.inl:528-556`). The render loop becomes `i2s_channel_read(rx)` → `engine.render(out,
256, micIn)` → `i2s_channel_write(tx)`: RX paces, TX keeps two descriptors of slack, `spk_task` and the
three-buffer protocol of SPEC §10.2 go away; master volume → REG32 via `setOutputVolumeHook`
(`liveplayer.ino:198`). Engine: `setMicInput(ptr, sendEcho, sendDry)`; before `echo_.process`
(`engine.cpp:373`): HPF, clamp at −12 dBFS, add into `echoSend_` and `mix_`, both sends through the 10 ms
one-pole. No new delay line: the 20 KB master echo (`config.h:46`) is the effect.

Numbers: RX DMA 3 KB + 0.3 KB driver + 0.5 KB mic block − 2.3 KB `spk_task` stack − 0.5 KB output buffer ≈ +1
KB net; a dedicated mic delay (11 KB per 250 ms) does not fit. CPU ≈ 200 ns/sample ≈ 50 µs/block ≈ 0.45 % of a
core, plus ≈ 15 µs read/deinterleave; render 5.4 → ≈ 5.5 ms. Flash +4 KB. Dry latency 28–40 ms (comb
filtering: keep dry ≤ −12 dB or 0).

Howl: the mic sits ≈ 2 cm from the speaker; at +21 dB PGA any echo send ≥ ≈ 0.3 oscillates at the echo period.
Rule: sends non-zero only while the key is held; on speaker dry 0, send ≤ 0.3, feedback ≤ 0.45, plus a runaway
detector (mic RMS over threshold 200 ms → mute); headphone mode is a config flag (no jack detection known).
Key: all 56 positions are mapped (SPEC §3): SHIFT chord or displace gyro. Fallback (1 day, zero RAM): hold
listen → `Speaker.end(); Mic.begin()`, record to SD, release → reload as the set's Vocal; a sampler, not live
input.

Unknown: headphone detection on the ADV; whether mono RX DMA takes 2 or 4 B per frame (4 assumed); ES8311 ADC
group delay; whether REG0D must be re-armed after `i2s_channel_disable`; mic SNR (PGA values come from another
ES8311 board); amp mute routing.

## 5. Loop mode

Library: 1542 WAVs (6.0 GB, mostly 24-bit stereo 44.1 kHz, all BPM-tagged); 998 (65 %) lie within ±5 % of 125
BPM, 81 % of drum/percussion/tonal; the non-vocal set is 314 MB at 22050/16-bit.

Route A (on-device stretch) is rejected: a phase vocoder costs ≈ 24–27 KB and 1.2–1.5 ms per block per stream
(10–13 % of a core; esp-dsp `dsps_fft4r_fc32` 1024 = 64 482 cycles) and smears transients; WSOLA fits the CPU
(0.19–0.8 µs/sample) but needs +7–12 KB per stream. Route B, recommended: pre-render on the Mac (rubberband R3
for ±4–5 %, ffmpeg/soxr to 22050 mono s16, trim to exact bars: 2 bars = 84 672 samples), name
`<Key>__125bpm__<Pack>__<Cat>__<name>.wav` (`parseBpmToken`, `generator.cpp:102`), shard to ≤ 64 files
(`shard_folders.py`, `write_indexes.py`), split 8-bar loops into A/B (clips top at 16 beats, SPEC §9.3), write
the card in a reader (cardloader: 41 min). Firmware: the existing varispeed producer (`produce`,
`generator.cpp:926-956`: rate = bpm/fileBpm clamped 0.5..2, 64-sample wrap crossfade) generalised from 2 vocal
streams to N with one 8 KB ring per loop track; Beat ← drum, Percussion ← percussion, Bass/Chord/Lead ← their
loops, Pad and Fx stay synth; `/lp/packs/<Genre>/loops/` present ⇒ loop mode. Variants as `vocalVariant()`
already does them; beat layers only from the 30 OSH Kick/Hats/Full stem kits.

Numbers: per stream 8 KB ring (`config.h:37`) + 136-sample head (`generator.h:38`) + ≈ 0.3 KB state + 1 handle
≈ 8.6 KB; 5 streams = 42 KB. The only place that fits: a loop-mode set's 48 952 B sample pool (`config.h:31`)
is idle, so carve 5 × 8 KB rings from it (6 miss by 200 B); perc A–D (22 KB per set) or a µ-law echo line (10
KB) give less. SD: 44.1 KB/s per stream; 5 loop + 2 vocal streams = 309 KB/s ≈ 75 × 4 KB chunks/s ≈ 0.25–0.4 s
of SD time per second on the loop task; worst wait with 7 streams plus one 20 ms burst ≈ 55 ms, fine with 8 KB
rings, not 4 KB. Engine CPU falls: a loop track ≈ 10–20 ns/sample in `renderVocal` replaces 1.7–3.4 µs of
synth voices. Flash +5 KB. The handle pool of 6 must grow to ≈ 2N+4, and the shared 4 KB window re-reads
partial chunks when streams alternate at rate ≠ 1: chunk-align refills or a private window per stream (+4 KB
each).

Tempo: rate is re-read per `produce` call; tempo± varispeeds loops (±5 % = ±0.85 st; fine for drums, tonal
loops detune against synth Pad/Fx), or a beat-slice re-trigger (≈ 30 lines, ±25 ms gap/overlap per beat) keeps
pitch. Keys: ≈ 8 tonal loops per key → pre-render ±2 st copies (220 MB) or report "NO LOOP IN Gmin".

Unknown: SD time per chunk with 7 streams (measured with 2); FatFs per-handle RAM; loop-task cost of `produce`
(60–100 ns/sample estimated); audible quality of the ±4.2 % stretch on tight hats (atempo vs R3); how the
beat-slice sounds; the pool carve-out across a set swap (voices must fade as in `CmdSlotSwap`).

## 6. Suggested order

Heap is the constraint, not CPU. All four designs are static or boot-allocated and together cost ≈ 3.6 KB
(acid 0.6 + rap 2 + mic 1; loop rings come from the pool). That passes the boot assertion (30.9 → ≈ 27 KB) but
takes playing headroom from 19–21 KB to ≈ 16–18 KB: restate SPEC §9.13's 20 KB floor as a boot figure, or
first recover 10 KB by µ-law-encoding the echo line (+64 µs/block). CPU: acid removes ≈ 1 ms summed over both
cores, mic adds 0.07 ms, rap and loops add nothing to the render task; the worst case stays ≈ 5.5 ms of 11.6.

1. Acid layer (3–4 days): pure firmware, smallest RAM, CPU-negative, no hardware risk; its per-block knob
   ParamIds and hold-layer key pattern are reused later.
2. Rap bank (4–5 days): 0–2 KB, reuses the vocal ring and chunk path; the Mac tool and word timing can be
   auditioned without the firmware. Settle the voice licence first.
3. Mic → echo (1-day probe, then 4–5 days): the standalone loopback probe can start any time; integrate after
   1–2: it replaces the proven output path, so later audio regressions would be ambiguous.
4. Loop mode (7–10 days plus the card rewrite): N-stream producer, pool carve-out, handle pool, pickers, SD
   contention with save/generate. Last, on a stable engine, after measuring SD time with 7 streams.
