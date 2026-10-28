// Wavetable CPU bench for the Cardputer ADV (SPEC §9.15, §9.13 "CPU is re-measured with the real synth voice").
// Build and run with tools/bench_wavetables.sh: the script copies src/{core,dsp,engine} of the firmware next to
// this sketch (src/ is NOT checked in), compiles, uploads and captures the serial output. No display, speaker
// or SD: the render loop runs flat out on core 0 and reports ns per voice-sample and the engine's
// renderMicrosMax, so the numbers are the pure DSP cost including the flash cache behaviour of the tables.
//
// Scenarios (each 5 s of audio, best of 3):
//   A  32 voices, pitches 36..59, no osc2 octave shift, no sub  -> only set 0 tables (the former working set)
//   B  32 voices, pitches 36..100 with osc2 +1/-1 octaves and subs -> tables of every set (the new working set)
//   C  1 voice, lean sine / full Fx preset                           -> kernel cost without cache pressure
//   D  the engine with 16 tracks (test_engine stress setup) at 120 BPM -> renderMicrosMax over 10 s
// Results (fill in after a run; the last known figures, 2026-10-15, esp32 core 3.3.12, -Os, 240 MHz):
//   A 522.5 ns/voice-sample   B 584.6 ns/voice-sample (+11.9 % vs A: the cross-set working set)
//   C lean sine 387 ns/sample, full Fx preset 832 ns/sample (SPEC §1 budget 220)
//   D renderMicrosMax 10509 us, mean 9606 us per 256-sample block (budget 11610 us), heap free 122 KB
//   -> the kernel itself, not the table sets, is what sits above the §1 figure: re-run against the previous
//      dsp sources (tools/bench_wavetables.sh with firmware/liveplayer/src swapped) to split the two.
#include <Arduino.h>
#include <string.h>
#include "src/core/config.h"
#include "src/core/types.h"
#include "src/dsp/synth_voice.h"
#include "src/dsp/presets.h"
#include "src/dsp/wavetables.h"
#include "src/dsp/ulaw.h"
#include "src/engine/engine.h"

using namespace lp;

static SynthVoice g_voices[32];
static SynthPatch g_patches[32];
static float g_acc[kBlock];
static Engine g_engine;                       // large static (pools, patterns, buffers), like the firmware
static int16_t g_out[kBlock];

static const Category kCats[5] = { Chord, Pad, Lead, Bass, Fx };

// Renders `seconds` of audio with the 32 voices; returns ns per voice-sample (best of `reps`).
static double benchVoices(bool spanSets, float seconds, int reps) {
  double best = 1e18;
  for (int r = 0; r < reps; r++) {
    for (int v = 0; v < 32; v++) {
      Category cat = kCats[(v / 2) % 5];
      g_patches[v] = preset(cat, v % presetCount(cat));
      g_patches[v].sustain = 1.0f; g_patches[v].release = 0.3f;
      if (!spanSets) { g_patches[v].osc2Octave = 0; g_patches[v].subLevel = 0.f; }
      else { g_patches[v].osc2Octave = (v % 3) - 1; g_patches[v].subLevel = (v & 1) ? 0.4f : 0.f; g_patches[v].osc2Level = 0.5f; }
      g_voices[v].init(); g_voices[v].setPatch(&g_patches[v]);
      int pitch = spanSets ? 36 + (v * 2) % 65 : 36 + (v * 7) % 24;      // 36..100 / 36..59
      g_voices[v].noteOn((uint8_t)pitch, 100);
    }
    const int blocks = (int)(seconds * kSampleRate / kBlock);
    int64_t t0 = esp_timer_get_time();
    volatile float sink = 0;
    for (int b = 0; b < blocks; b++) {
      memset(g_acc, 0, sizeof(g_acc));
      for (int v = 0; v < 32; v++) g_voices[v].render(g_acc, kBlock);
      sink = sink + g_acc[7];
    }
    double ns = (double)(esp_timer_get_time() - t0) * 1000.0 / ((double)blocks * kBlock * 32);
    if (ns < best) best = ns;
  }
  return best;
}

static double benchOne(const SynthPatch& p, float seconds) {
  g_voices[0].init(); g_voices[0].setPatch(&p); g_voices[0].noteOn(52, 100);
  const int blocks = (int)(seconds * kSampleRate / kBlock);
  int64_t t0 = esp_timer_get_time(); volatile float sink = 0;
  for (int b = 0; b < blocks; b++) { memset(g_acc, 0, sizeof(g_acc)); g_voices[0].render(g_acc, kBlock); sink = sink + g_acc[3]; }
  return (double)(esp_timer_get_time() - t0) * 1000.0 / ((double)blocks * kBlock);
}

// The engine with the 16-track stress setup of tests/test_engine.cpp (synth tracks on presets, drum slots from
// a synthetic one-shot), 10 s of audio; returns renderMicrosMax and the mean block time.
static void benchEngine(uint32_t& maxUs, double& meanUs) {
  Engine& e = g_engine; e.init();
  for (int t = 0; t < kTracks; t++) {
    Category cat = trackCategory(t);
    if (isSynthCategory(cat)) { e.nextPatch(t) = preset(cat, t % presetCount(cat)); Cmd pc; pc.type = CmdSetPatch; pc.track = t; e.cmds().push(pc); }
    while (!e.beginNextWrite(t)) { e.render(g_out, kBlock); }
    Pattern& p = e.nextPattern(t); p.clear(kTicksPerBar);
    if (cat == Beat) { for (int s = 0; s < 16; s++) p.add(Note::make(s * 24, 12, s % 4 == 0 ? 36 : s % 2 ? 38 : 37, 100)); }
    else if (cat == Percussion) { for (int s = 0; s < 16; s++) p.add(Note::make(s * 24, 12, 36 + (s % 4), 90)); }
    else if (cat != Vocal) {
      // Notes spanning the sets: leads 72..95, chords/pads 48..79, bass 36..59, fx 60..100 (plus osc2 octaves / subs).
      for (int s = 0; s < 8; s++) {
        int base = cat == Lead ? 72 : cat == Bass ? 36 : cat == Fx ? 60 : 48; int span = cat == Fx ? 40 : 24;
        p.add(Note::make(s * 48, 40, base + (s * 5) % span, 100));
        if (cat == Chord || cat == Pad) { p.add(Note::make(s * 48, 40, base + 4 + (s * 5) % span, 90)); p.add(Note::make(s * 48, 40, base + 7 + (s * 5) % span, 90)); }
      }
    }
    p.sortByStart(); e.endNextWrite(t);
    Cmd c; c.type = CmdSwapPattern; c.track = t; e.cmds().push(c);
    Cmd pr; pr.type = CmdSetParam; pr.track = t; pr.a = PEcho; pr.f = 0.3f; e.cmds().push(pr); pr.a = PReverb; pr.f = 0.3f; e.cmds().push(pr); pr.a = PDist; pr.f = 0.4f; e.cmds().push(pr); pr.a = PFilter; pr.f = -0.3f; e.cmds().push(pr);
  }
  for (int set = 0; set < 2; set++) for (int slot = 0; slot < 8; slot++) {
    uint8_t* st = e.staging(); for (int i = 0; i < 2500; i++) st[i] = ulawEncode((int16_t)(15000.0f * (1.0f - i / 2500.0f) * ((i / (5 + slot)) % 2 ? 1 : -1)));
    Cmd s; s.type = CmdSlotSwap; s.a = set; s.i = slot | (2500 << 4); e.cmds().push(s); e.render(g_out, kBlock); e.render(g_out, kBlock);
  }
  Cmd ps; ps.type = CmdPlaySet; ps.a = 0; e.cmds().push(ps); ps.a = 1; e.cmds().push(ps);
  for (int b = 0; b < 22050 * 3 / kBlock; b++) e.render(g_out, kBlock);
  const int blocks = 22050 * 10 / kBlock; maxUs = 0; double total = 0;
  for (int b = 0; b < blocks; b++) {
    int64_t t0 = esp_timer_get_time(); e.render(g_out, kBlock); uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if (dt > maxUs) maxUs = dt; total += dt;
  }
  meanUs = total / blocks;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  ulawInit();
  Serial.printf("[bench] wavetables: %d sets, %d B of tables; heap free=%u\n", kWaveSetCount,
                (int)(5 * ((2049 + 513 + 257 + 129) * 2)), (unsigned)ESP.getFreeHeap());
}

void loop() {
  double a = benchVoices(false, 5.0f, 3);
  Serial.printf("[bench] A  32 voices on set 0 only (36..59):      %.1f ns/voice-sample\n", a);
  double b = benchVoices(true, 5.0f, 3);
  Serial.printf("[bench] B  32 voices across sets 0..3 (36..100): %.1f ns/voice-sample  (%+.1f%% vs A)\n", b, (b - a) / a * 100.0);
  SynthPatch lean; lean.wave1 = WSine; lean.wave2 = WSine; lean.osc2Level = 0; lean.subLevel = 0; lean.noiseLevel = 0; lean.cutoff = 10000.f; lean.resonance = 0; lean.sustain = 1.0f;
  SynthPatch full = preset(Fx, 6); full.subLevel = 0.5f; full.lfoRate = 3.f; full.lfoToCutoff = 1.f; full.lfoToPitch = 0.1f; full.sustain = 1.f;
  Serial.printf("[bench] C  one voice: lean sine %.1f ns/sample, full Fx preset %.1f ns/sample (SPEC §1 budget 220)\n", benchOne(lean, 5.0f), benchOne(full, 5.0f));
  uint32_t maxUs = 0; double meanUs = 0; benchEngine(maxUs, meanUs);
  Serial.printf("[bench] D  engine, 16 tracks across the sets: renderMicrosMax %u us, mean %.0f us per 256-sample block (budget 11610 us)\n", (unsigned)maxUs, meanUs);
  Serial.printf("[bench] heap free=%u\n", (unsigned)ESP.getFreeHeap());
  delay(2000);
}
