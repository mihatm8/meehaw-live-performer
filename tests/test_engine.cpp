// Host tests for the engine: clock, swaps, sample staging, events, stress, the SPEC §9.17a knobs / commands, the
// SPEC §9.19 loop mode (loop slot streams, CmdLoopMode / CmdLoopLoaded at the bar, clip caps, the loop Beat kick cut, the rush held on a loop Beat track)
// and the SPEC §9.22 303 character (SAT / ACC knobs, the per-set chorus bus, the chunk cap it needs).
#include "check.h"
#include "engine/engine.h"
#include "dsp/ulaw.h"
#include "dsp/presets.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
using namespace lp;

// Loop slot ring memory (SPEC §9.19): the host hands it to the engine (bootBlock on the device, static arrays here).
static int16_t gLoopMem[kLoopSlots][kLoopRingBytes / 2];
static void bindLoopRings(Engine& e) { for (int k = 0; k < kLoopSlots; k++) e.setLoopRing(k, gLoopMem[k], kLoopRingBytes / 2); }

static Engine* eng() { static Engine* e = nullptr; if (!e) { e = new Engine(); ulawInit(); e->init(); bindLoopRings(*e); } return e; }
static void renderBlocks(Engine& e, int blocks, int16_t* out = nullptr, float* peak = nullptr) {
  static int16_t buf[kBlock];
  for (int b = 0; b < blocks; b++) { e.render(buf, kBlock); if (peak) for (int i = 0; i < kBlock; i++) { float a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > *peak) *peak = a; } if (out) memcpy(out + b * kBlock, buf, sizeof buf); }
}
static void drainEvents(Engine& e, int* bars, int* kicks) { Ev ev; while (e.events().pop(ev)) { if (ev.type == EvBar && bars) (*bars)++; if (ev.type == EvKick && kicks) (*kicks)++; } }

TEST(clock_ticks_at_the_default_bpm) {
  Engine& e = *eng(); Cmd c; c.type = CmdResetAll; e.cmds().push(c);   // reset = the firmware default tempo (kDefaultBpm, 125 since 2026-10-16)
  uint32_t t0 = e.status().tick;
  const int blocks = 22050 * 2 / kBlock;
  renderBlocks(e, blocks);                                   // ~2 s: kDefaultBpm × 96 × 2 / 60 ticks (400 at 125 BPM)
  uint32_t dt = e.status().tick - t0;
  const int want = (int)((int64_t)kDefaultBpm * kPPQN * blocks * kBlock / (60 * kSampleRate));
  CHECK((int)dt >= want - 4 && (int)dt <= want + 4);
  CHECK_EQ((int)e.status().bpm, kDefaultBpm);
}

TEST(pattern_swap_and_events) {
  Engine& e = *eng(); int track = 7;                           // set 1 Beat
  CHECK(e.beginNextWrite(track));
  Pattern& p = e.nextPattern(track); p.clear(kTicksPerBar);
  for (int b = 0; b < 4; b++) p.add(Note::make(b * kTicksPerBeat, 12, 36, 100));
  p.sortByStart(); e.endNextWrite(track);
  Cmd c; c.type = CmdSwapPattern; c.track = track; c.a = 3; c.i = 0; e.cmds().push(c);
  // load a synthetic kick into slot 0 via staging
  uint8_t* st = e.staging(); for (int i = 0; i < 4000; i++) st[i] = ulawEncode((int16_t)(20000.0f * (1.0f - i / 4000.0f) * ((i / 20) % 2 ? 1 : -1)));
  Cmd s; s.type = CmdSlotSwap; s.a = 0; s.i = 0 | (4000 << 4); s.f = 0; e.cmds().push(s);
  uint16_t ack = e.status().slotAck;
  renderBlocks(e, 3);
  CHECK_EQ((int)e.status().slotAck, (int)ack + 1);
  CHECK(e.kit(0).slot[0].present == 1 && e.kit(0).slot[0].length == 4000);
  int bars = 0, kicks = 0; drainEvents(e, &bars, &kicks);
  Cmd tg; tg.type = CmdTrackToggle; tg.track = track; e.cmds().push(tg);
  renderBlocks(e, 1); CHECK_EQ((int)e.status().track[track].state, (int)PSQueued);
  float peak = 0; renderBlocks(e, 22050 * 4 / kBlock, nullptr, &peak);   // 4 s: must pass a bar line, start, and fire kicks
  drainEvents(e, &bars, &kicks);
  CHECK_EQ((int)e.status().track[track].state, (int)PSPlaying);
  CHECK(bars >= 1); CHECK(kicks >= 4);
  CHECK(peak > 500);
  CHECK_EQ((int)e.status().track[track].variant, 3);
}

TEST(kick_cut_and_mask) {
  Engine& e = *eng(); int track = 7; Cmd c; c.type = CmdKickCut; c.a = 0; c.i = 1; e.cmds().push(c);
  int kicks = 0; renderBlocks(e, 22050 * 2 / kBlock); drainEvents(e, nullptr, &kicks); CHECK_EQ(kicks, 0);
  c.i = 0; e.cmds().push(c); renderBlocks(e, 22050 * 2 / kBlock); kicks = 0; drainEvents(e, nullptr, &kicks); CHECK(kicks >= 3);
  Cmd m; m.type = CmdBeatMask; m.a = 0; m.i = 0x0E | (1 << 8); e.cmds().push(m);  // Kick+Hats mask without bit 0 → no kicks after the bar
  renderBlocks(e, 22050 * 3 / kBlock); drainEvents(e, nullptr, nullptr); kicks = 0; renderBlocks(e, 22050 * 2 / kBlock); drainEvents(e, nullptr, &kicks); CHECK_EQ(kicks, 0);
  m.i = 0x1F | (3 << 8); e.cmds().push(m); renderBlocks(e, 22050 * 3 / kBlock); drainEvents(e, nullptr, nullptr);
  (void)track;
}

TEST(synth_track_sounds_and_stops) {
  Engine& e = *eng(); int track = 0;                            // set 1 Chord
  e.nextPatch(track) = preset(Chord, 0); Cmd pc; pc.type = CmdSetPatch; pc.track = track; e.cmds().push(pc);
  CHECK(e.beginNextWrite(track)); Pattern& p = e.nextPattern(track); p.clear(kTicksPerBar);
  p.add(Note::make(0, 48, 60, 100)); p.add(Note::make(0, 48, 64, 100)); p.add(Note::make(0, 48, 67, 100)); p.add(Note::make(192, 48, 62, 90)); p.sortByStart(); e.endNextWrite(track);
  Cmd c; c.type = CmdSwapPattern; c.track = track; c.a = 0; e.cmds().push(c);
  Cmd tg; tg.type = CmdTrackToggle; tg.track = track; e.cmds().push(tg);
  Cmd kc; kc.type = CmdTrackStopNow; kc.track = 7; e.cmds().push(kc);                 // silence the beat for this test
  float peak = 0; renderBlocks(e, 22050 * 3 / kBlock, nullptr, &peak); CHECK(peak > 300);
  uint8_t maxLevel = 0; for (int b = 0; b < 22050 * 2 / kBlock; b++) { renderBlocks(e, 1); if (e.status().track[track].level > maxLevel) maxLevel = e.status().track[track].level; }
  CHECK(maxLevel > 0);
  Cmd st; st.type = CmdTrackStopNow; st.track = track; e.cmds().push(st);
  renderBlocks(e, 22050 / kBlock); peak = 0; renderBlocks(e, 22050 / kBlock, nullptr, &peak); CHECK(peak < 40);
}

TEST(params_ramp_and_cancel) {
  Engine& e = *eng(); Cmd r; r.type = CmdRamp; r.track = 0; r.a = PVolume; r.f = 0.0f; r.i = kTicksPerBar; e.cmds().push(r);
  renderBlocks(e, 22050 * 1 / kBlock);                            // half a bar at 120 BPM (1 bar = 2 s)
  float v = e.params(0).volume; CHECK(v < 0.8f && v > 0.1f);
  Cmd c; c.type = CmdCancelRamps; e.cmds().push(c); renderBlocks(e, 2);
  CHECK_NEAR(e.params(0).volume, 0.8f, 1e-5);
}

TEST(stress_all_tracks_cpu) {
  Engine& e = *eng();
  for (int t = 0; t < kTracks; t++) {
    Category cat = trackCategory(t);
    if (isSynthCategory(cat)) { e.nextPatch(t) = preset(cat, t % presetCount(cat)); Cmd pc; pc.type = CmdSetPatch; pc.track = t; e.cmds().push(pc); }
    CHECK(e.beginNextWrite(t)); Pattern& p = e.nextPattern(t); p.clear(kTicksPerBar);
    if (cat == Beat) { for (int s = 0; s < 16; s++) p.add(Note::make(s * 24, 12, s % 4 == 0 ? 36 : s % 2 ? 38 : 37, 100)); }
    else if (cat == Percussion) { for (int s = 0; s < 16; s++) p.add(Note::make(s * 24, 12, 36 + (s % 4), 90)); }
    else if (cat != Vocal) { for (int s = 0; s < 8; s++) { p.add(Note::make(s * 48, 40, 48 + (s * 5) % 24, 100)); if (cat == Chord || cat == Pad) { p.add(Note::make(s * 48, 40, 52 + (s * 5) % 24, 90)); p.add(Note::make(s * 48, 40, 55 + (s * 5) % 24, 90)); } } }
    p.sortByStart(); e.endNextWrite(t);
    Cmd c; c.type = CmdSwapPattern; c.track = t; e.cmds().push(c);
    Cmd pr; pr.type = CmdSetParam; pr.track = t; pr.a = PEcho; pr.f = 0.3f; e.cmds().push(pr); pr.a = PReverb; pr.f = 0.3f; e.cmds().push(pr); pr.a = PDist; pr.f = 0.4f; e.cmds().push(pr); pr.a = PFilter; pr.f = -0.3f; e.cmds().push(pr);
  }
  for (int set = 0; set < 2; set++) for (int slot = 0; slot < 8; slot++) { uint8_t* st = e.staging(); for (int i = 0; i < 2500; i++) st[i] = ulawEncode((int16_t)(15000.0f * (1.0f - i / 2500.0f) * ((i / (5 + slot)) % 2 ? 1 : -1))); Cmd s; s.type = CmdSlotSwap; s.a = set; s.i = slot | (2500 << 4); e.cmds().push(s); renderBlocks(e, 2); }
  Cmd ps; ps.type = CmdPlaySet; ps.a = 0; e.cmds().push(ps); ps.a = 1; e.cmds().push(ps);
  renderBlocks(e, 22050 * 3 / kBlock); drainEvents(e, nullptr, nullptr);
  int playing = 0; for (int t = 0; t < kTracks; t++) if (e.status().track[t].state == PSPlaying) playing++;
  CHECK(playing >= 14);
  float peak = 0; auto t0 = std::chrono::steady_clock::now(); int blocks = 22050 * 10 / kBlock; renderBlocks(e, blocks, nullptr, &peak);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  16 tracks: %.1f ms host CPU per 10 s of audio (%.2fx realtime), peak %.0f, underruns %u\n", secs * 1000, 10.0 / secs, peak, (unsigned)e.status().underruns);
  CHECK(peak > 1000); CHECK(peak <= 32767);
  // SPEC §9.22 CPU regression: the same 16 tracks with the 303 character on every synth track (SAT 0.8, ACC 1 — the
  // vel-100 notes are accented) and a chorus send on every track (both set buses busy). The squelch is ≈ 9 FP ops per
  // acid voice-sample and the chorus ≈ 30 ops per set-sample + 8 ops per sending track-sample, so the whole stress
  // scene may cost at most half as much again on the host (measured: see the printout; the bound leaves CI noise room).
  for (int t = 0; t < kTracks; t++) {
    Cmd pr; pr.type = CmdSetParam; pr.track = t; pr.a = PChorus; pr.f = isSynthCategory(trackCategory(t)) ? 0.6f : 0.3f; e.cmds().push(pr);
    if (isSynthCategory(trackCategory(t))) { pr.a = PSat; pr.f = 0.8f; e.cmds().push(pr); pr.a = PAccent; pr.f = 1.0f; e.cmds().push(pr); }
  }
  renderBlocks(e, 30); drainEvents(e, nullptr, nullptr);
  float peak2 = 0; t0 = std::chrono::steady_clock::now(); renderBlocks(e, blocks, nullptr, &peak2);
  double secs2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  16 tracks + SAT 0.8 / ACC 1 / chorus: %.1f ms host CPU per 10 s of audio (x%.2f of the clean scene), peak %.0f\n", secs2 * 1000, secs2 / secs, peak2);
  CHECK(peak2 > 1000); CHECK(peak2 <= 32767); CHECK_EQ((int)e.status().underruns, 0);
  CHECK(secs2 < secs * 1.5 + 0.02); CHECK(secs2 < 0.5);
  for (int t = 0; t < kTracks; t++) { Cmd pr; pr.type = CmdSetParam; pr.track = t; pr.a = PChorus; pr.f = 0.0f; e.cmds().push(pr); pr.a = PSat; e.cmds().push(pr); pr.a = PAccent; e.cmds().push(pr); }
  renderBlocks(e, 20); drainEvents(e, nullptr, nullptr);
}

// ---------------------------------------------------------------------------------- SPEC §9.17a: knobs, commands, perform

// A fresh engine per test below (host only: the firmware allocates once at boot), so nothing a previous test left
// playing leaks into a level or spectrum measurement.
static Engine* freshEngine() { Engine* e = new Engine(); ulawInit(); e->init(); bindLoopRings(*e); Cmd t; t.type = CmdSetTempo; t.i = 120; e->cmds().push(t); static int16_t tmp[kBlock]; e->render(tmp, kBlock); return e; }   // tests below reason at 120 BPM (1 bar = 2 s); the firmware default is kDefaultBpm
static void push(Engine& e, CmdType t, int track = 0, int a = 0, int i = 0, float f = 0.0f) { Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f; CHECK(e.cmds().push(c)); }
static void capture(Engine& e, int blocks, std::vector<float>& out) {
  static int16_t buf[kBlock];
  for (int b = 0; b < blocks; b++) { e.render(buf, kBlock); for (int i = 0; i < kBlock; i++) out.push_back(buf[i] * (1.0f / 32768.0f)); }
}
static float rmsOf(const std::vector<float>& s) { double a = 0; for (float x : s) a += (double)x * x; return s.empty() ? 0.f : (float)std::sqrt(a / s.size()); }
static float peakOf(const std::vector<float>& s) { float p = 0; for (float x : s) { float a = x < 0 ? -x : x; if (a > p) p = a; } return p; }
// High-frequency share: RMS of the first difference over the RMS of the signal (≈ 2 sin(πf/fs) weighted; a sine at
// 130 Hz gives 0.037, a bright saw well above 0.3).
static float hfRatio(const std::vector<float>& s) { std::vector<float> d; d.reserve(s.size()); for (size_t i = 1; i < s.size(); i++) d.push_back(s[i] - s[i - 1]); float r = rmsOf(s); return r > 0 ? rmsOf(d) / r : 0.f; }
static float crestOf(const std::vector<float>& s) { float r = rmsOf(s); return r > 0 ? peakOf(s) / r : 0.f; }
static float velCurveRatio(int velA, int velB) { auto c = [](int v) { float x = v / 127.0f; return 0.5f * x * x + 0.5f * x; }; return c(velA) / c(velB); }   // synth_voice.cpp velCurve
// Goertzel magnitude of one frequency (normalised by the length): a cheap single-bin DFT for the resonance check.
static float goertzel(const std::vector<float>& s, float hz) {
  const double w = 2.0 * 3.14159265358979 * hz / (double)kSampleRate, coef = 2.0 * std::cos(w); double s1 = 0, s2 = 0;
  for (float x : s) { double s0 = x + coef * s1 - s2; s2 = s1; s1 = s0; }
  double p = s1 * s1 + s2 * s2 - coef * s1 * s2; return s.empty() ? 0.f : (float)(std::sqrt(p > 0 ? p : 0) / s.size());
}
// A sustained bright patch with nothing modulating the cutoff but the knobs (no filter env, no LFO, no key tracking).
static SynthPatch sustainedSaw() {
  SynthPatch p; p.wave1 = WSaw; p.osc2Level = 0; p.subLevel = 0; p.noiseLevel = 0; p.filterMode = 0; p.cutoff = 2000.f; p.resonance = 0.1f;
  p.filterEnv = 0; p.keyTrack = 0; p.attack = 0.005f; p.decay = 0.1f; p.sustain = 1.0f; p.release = 0.05f; p.fAttack = 0.002f; p.fDecay = 0.2f;
  p.glide = 0; p.lfoRate = 0; p.drive = 0; p.level = 0.8f; p.polyphony = 4; return p;
}
static void loadSlot(Engine& e, int set, int slot, int len, int period) {
  uint8_t* st = e.staging(); for (int i = 0; i < len; i++) st[i] = ulawEncode((int16_t)(18000.0f * (1.0f - (float)i / len) * ((i / period) % 2 ? 1 : -1)));
  push(e, CmdSlotSwap, 0, set, slot | (len << 4), 0.0f); renderBlocks(e, 2);
  CHECK(e.kit(set).slot[slot].present == 1);
}

TEST(knobs_init_from_patch) {
  Engine& e = *freshEngine(); const int t = 0; const SynthPatch& p = preset(Chord, 0);   // PluckWide: 1800 Hz, res 0.2, env 2.5 oct, fDecay 0.2 s
  e.nextPatch(t) = p; push(e, CmdSetPatch, t); renderBlocks(e, 2);
  const TrackParams& tp = e.params(t); const SynthKnobs& k = e.knobs(t);
  CHECK_NEAR(tp.cutoff, knobFromCutoffHz(p.cutoff), 1e-5); CHECK_NEAR(k.cutoffHz, p.cutoff, p.cutoff * 0.002f);   // round trip through the log mapping
  CHECK_NEAR(tp.resonance, p.resonance / 0.985f, 1e-5); CHECK_NEAR(k.resonance, p.resonance, 1e-4);
  CHECK_NEAR(tp.envAmt, p.filterEnv / 4.0f, 1e-5); CHECK_NEAR(k.filterEnvOct, p.filterEnv, 1e-4);
  CHECK_NEAR(tp.decay, knobFromDecayS(p.fDecay), 1e-5); CHECK_NEAR(k.decayS, p.fDecay, p.fDecay * 0.002f);
  CHECK_NEAR(tp.drive, p.drive, 1e-6); CHECK_NEAR(k.drive, p.drive, 1e-6);
  CHECK_EQ((int)tp.wave, (int)p.wave1); CHECK_EQ((int)k.wave, (int)p.wave1);
  // The knobs follow the next patch at once, even mid-smoothing of a knob edit, and clamp a patch outside the knob range.
  push(e, CmdSetParam, t, PCutoff, 0, 0.0f); renderBlocks(e, 1);
  SynthPatch hi = p; hi.cutoff = 20000.f; hi.fDecay = 5.0f; hi.wave1 = WSquare; e.nextPatch(t) = hi; push(e, CmdSetPatch, t); renderBlocks(e, 1);
  CHECK_NEAR(e.params(t).cutoff, 1.0f, 1e-6); CHECK_NEAR(e.knobs(t).cutoffHz, 12000.f, 12000.f * 0.002f);
  CHECK_NEAR(e.params(t).decay, 1.0f, 1e-6); CHECK_NEAR(e.knobs(t).decayS, 2.0f, 0.004f);
  CHECK_EQ((int)e.knobs(t).wave, (int)WSquare);
  // Every synth track of a fresh engine already carries knobs that describe its (default) patch.
  Engine& f = *freshEngine(); SynthPatch d; CHECK_NEAR(f.knobs(2).cutoffHz, d.cutoff, d.cutoff * 0.002f); CHECK_NEAR(f.params(2).envAmt, d.filterEnv / 4.0f, 1e-6);
  // SPEC §9.22: sat / accent are knobs like the others (patch → knob on CmdSetPatch, clamped; library presets carry 0 / 0),
  // the chorus send is a strip parameter (0 by default, untouched by a patch, smoothed, reset to 0).
  CHECK_NEAR(f.params(2).sat, d.sat, 1e-6); CHECK_NEAR(f.params(2).accent, d.accent, 1e-6); CHECK_NEAR(f.knobs(2).sat, 0.0f, 1e-6); CHECK_NEAR(f.knobs(2).accent, 0.0f, 1e-6); CHECK_NEAR(f.params(2).chorus, 0.0f, 1e-6);
  SynthPatch acid = sustainedSaw(); acid.sat = 0.7f; acid.accent = 0.9f; f.nextPatch(2) = acid; push(f, CmdSetPatch, 2); renderBlocks(f, 1);
  CHECK_NEAR(f.params(2).sat, 0.7f, 1e-6); CHECK_NEAR(f.params(2).accent, 0.9f, 1e-6); CHECK_NEAR(f.knobs(2).sat, 0.7f, 1e-6); CHECK_NEAR(f.knobs(2).accent, 0.9f, 1e-6);
  push(f, CmdSetParam, 2, PSat, 0, 1.5f); push(f, CmdSetParam, 2, PAccent, 0, -0.5f); push(f, CmdSetParam, 2, PChorus, 0, 0.4f); renderBlocks(f, 30);
  CHECK_NEAR(f.params(2).sat, 1.0f, 1e-6); CHECK_NEAR(f.params(2).accent, 0.0f, 1e-6); CHECK_NEAR(f.params(2).chorus, 0.4f, 1e-6);
  CHECK_NEAR(f.knobs(2).sat, 1.0f, 1e-4); CHECK_NEAR(f.knobs(2).accent, 0.0f, 1e-4);
  acid.sat = 2.0f; acid.accent = -1.0f; f.nextPatch(2) = acid; push(f, CmdSetPatch, 2); renderBlocks(f, 1);                 // a patch outside the range is clamped
  CHECK_NEAR(f.params(2).sat, 1.0f, 1e-6); CHECK_NEAR(f.params(2).accent, 0.0f, 1e-6); CHECK_NEAR(f.params(2).chorus, 0.4f, 1e-6);   // the send is not a patch field
  push(f, CmdRamp, 2, PSat, kTicksPerBar, 0.0f); renderBlocks(f, 22050 / kBlock); CHECK(f.params(2).sat < 0.7f && f.params(2).sat > 0.2f);   // ramps know the knob
  push(f, CmdResetTrack, 2); renderBlocks(f, 30);
  CHECK_NEAR(f.params(2).sat, 1.0f, 1e-6); CHECK_NEAR(f.params(2).accent, 0.0f, 1e-6); CHECK_NEAR(f.params(2).chorus, 0.0f, 1e-5);   // reset: knobs from the (clamped) patch, send to the default 0
  delete &f; delete &e;
}

TEST(knob_cutoff_moves_spectrum) {
  Engine& e = *freshEngine(); const int t = 2;                                  // set 1 Pad
  e.nextPatch(t) = sustainedSaw(); push(e, CmdSetPatch, t);
  push(e, CmdSetParam, t, PCutoff, 0, 0.1f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f);   // 132 Hz: the saw at C3 (130.8 Hz) comes out as a sine
  push(e, CmdPerformNote, t, 0, 48, 0.9f);
  renderBlocks(e, 30);                                                          // knob smoothing (≈ 10 blocks) and the attack
  std::vector<float> dark; capture(e, 40, dark); const float rDark = hfRatio(dark);
  CHECK(rmsOf(dark) > 0.002f);
  push(e, CmdSetParam, t, PCutoff, 0, 0.9f); renderBlocks(e, 30);              // 7.3 kHz: the harmonics return
  std::vector<float> bright; capture(e, 40, bright); const float rBright = hfRatio(bright);
  std::printf("  hf ratio: cutoff 0.1 → %.3f, cutoff 0.9 → %.3f\n", rDark, rBright);
  CHECK(rBright > 3.0f * rDark);
  CHECK_NEAR(e.knobs(t).cutoffHz, knobCutoffHz(0.9f), 1.0f);
  // The wave knob reaches the sounding voice as well: a square at the same cutoff has no even harmonics, so its
  // first-difference share drops against the saw (resonance, env amount, decay and drive: knob_res_env_decay_drive_reach_voice).
  push(e, CmdSetParam, t, PWave, 0, (float)WSquare); renderBlocks(e, 30);
  std::vector<float> square; capture(e, 40, square); const float rSquare = hfRatio(square);
  std::printf("  hf ratio: square → %.3f\n", rSquare);
  CHECK(rSquare < rBright); CHECK(rSquare > 2.0f * rDark);
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 22050 / kBlock);
  std::vector<float> tail; capture(e, 20, tail); CHECK(peakOf(tail) < 0.002f);  // the note off reached the voice
  delete &e;
}

TEST(freeze_ramp_holds_value) {
  Engine& e = *freshEngine();
  push(e, CmdRamp, 0, PVolume, kTicksPerBar, 0.0f); renderBlocks(e, 22050 / kBlock);   // half a bar at 120 BPM
  push(e, CmdFreezeRamp, 0, PVolume); renderBlocks(e, 1);
  const float v = e.params(0).volume; CHECK(v < 0.7f && v > 0.2f);
  renderBlocks(e, 22050 / kBlock); CHECK_NEAR(e.params(0).volume, v, 1e-6);           // the ramp no longer moves it
  push(e, CmdCancelRamps); renderBlocks(e, 1); CHECK_NEAR(e.params(0).volume, v, 1e-6);   // and there is nothing left to snap back
  // A knob ramp (PCutoff) ramps and freezes the same way.
  push(e, CmdRamp, 0, PCutoff, kTicksPerBar, 1.0f); renderBlocks(e, 22050 / kBlock);
  push(e, CmdFreezeRamp, 0, PCutoff); renderBlocks(e, 1); const float k = e.params(0).cutoff; CHECK(k > 0.55f && k < 0.95f);
  renderBlocks(e, 10); CHECK_NEAR(e.params(0).cutoff, k, 1e-6); CHECK_NEAR(e.knobs(0).cutoffHz, knobCutoffHz(k), knobCutoffHz(k) * 0.002f);
  delete &e;
}

TEST(tempo_ramp_survives_cancel) {
  Engine& e = *freshEngine();
  push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, 130.0f);                      // tempo drift: +10 BPM over 60 beats
  push(e, CmdRamp, 0, PVolume, kTicksPerBar, 0.0f);                             // a strip ramp alongside
  renderBlocks(e, 40); push(e, CmdCancelRamps); renderBlocks(e, 2);
  CHECK_NEAR(e.params(0).volume, 0.8f, 1e-5);                                   // the strip ramp snapped back ...
  renderBlocks(e, 22050 * 4 / kBlock);                                          // ≈ 8 beats into the drift: +1.3 BPM
  CHECK((int)e.status().bpm >= 121); CHECK((int)e.status().bpm <= 123);        // ... the drift did not, and is still under way
  uint32_t t0 = e.status().tick; while (e.status().tick - t0 < 61 * (uint32_t)kTicksPerBeat) renderBlocks(e, 8);
  CHECK_EQ((int)e.status().bpm, 130);
  push(e, CmdSetTempo, 0, 0, 120); renderBlocks(e, 1); CHECK_EQ((int)e.status().bpm, 120);   // the finished drift released the tempo
  push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, 110.0f); renderBlocks(e, 40); push(e, CmdCancelRamps);
  t0 = e.status().tick; while (e.status().tick - t0 < 61 * (uint32_t)kTicksPerBeat) renderBlocks(e, 8);
  CHECK_EQ((int)e.status().bpm, 110);
  // CmdFreezeRamp on PTempo stops a drift where it is.
  push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, 130.0f); renderBlocks(e, 22050 * 6 / kBlock); push(e, CmdFreezeRamp, 3, PTempo); renderBlocks(e, 1);
  const int held = e.status().bpm; CHECK(held > 110 && held < 130); renderBlocks(e, 22050 * 4 / kBlock); CHECK_EQ((int)e.status().bpm, held);
  delete &e;
}

TEST(reset_track_restores_defaults_and_knobs) {
  Engine& e = *freshEngine(); const int t = 0; const SynthPatch& p = preset(Chord, 0); e.nextPatch(t) = p; push(e, CmdSetPatch, t);
  push(e, CmdSetParam, t, PVolume, 0, 0.3f); push(e, CmdSetParam, t, PDist, 0, 0.7f); push(e, CmdSetParam, t, PCutoff, 0, 0.1f);
  push(e, CmdSetParam, t, PWave, 0, (float)WSquare); push(e, CmdSetParam, t, PResonance, 0, 0.9f); renderBlocks(e, 20);
  CHECK_NEAR(e.params(t).cutoff, 0.1f, 1e-6); CHECK_EQ((int)e.params(t).wave, (int)WSquare); CHECK_NEAR(e.knobs(t).cutoffHz, knobCutoffHz(0.1f), 0.5f);
  push(e, CmdResetTrack, t); renderBlocks(e, 20);
  CHECK_NEAR(e.params(t).volume, 0.8f, 1e-5); CHECK_NEAR(e.params(t).dist, 0.0f, 1e-5);
  CHECK_NEAR(e.params(t).cutoff, knobFromCutoffHz(p.cutoff), 1e-5); CHECK_NEAR(e.params(t).resonance, p.resonance / 0.985f, 1e-5);
  CHECK_EQ((int)e.params(t).wave, (int)p.wave1); CHECK_NEAR(e.knobs(t).cutoffHz, p.cutoff, p.cutoff * 0.002f); CHECK_EQ((int)e.knobs(t).wave, (int)p.wave1);
  // The per-set reset re-derives knobs too and keeps the vocal's default echo send; the other set is untouched.
  push(e, CmdSetParam, t, PCutoff, 0, 0.2f); push(e, CmdSetParam, 8, PCutoff, 0, 0.2f); renderBlocks(e, 2);
  push(e, CmdResetTrackParams, 0, 0); renderBlocks(e, 2);
  CHECK_NEAR(e.params(t).cutoff, knobFromCutoffHz(p.cutoff), 1e-5); CHECK_NEAR(e.params(Vocal).echo, 0.35f, 1e-5); CHECK_NEAR(e.params(8).cutoff, 0.2f, 1e-6);
  // A running knob ramp does not survive the reset (the generator resets on every generate while a key may still be
  // held): the knob sits on the patch value and stays there.
  const float k0 = knobFromCutoffHz(p.cutoff);
  push(e, CmdRamp, t, PCutoff, kTicksPerBar, 1.0f); renderBlocks(e, 10); CHECK(e.params(t).cutoff > k0 + 0.01f);
  push(e, CmdResetTrack, t); renderBlocks(e, 2); CHECK_NEAR(e.params(t).cutoff, k0, 1e-5);
  renderBlocks(e, 20); CHECK_NEAR(e.params(t).cutoff, k0, 1e-5); CHECK_NEAR(e.knobs(t).cutoffHz, p.cutoff, p.cutoff * 0.002f);
  // The set reset drops the set's ramps only: a ramp on the other set keeps running, and a tempo drift is untouched.
  push(e, CmdRamp, 8, PCutoff, kTicksPerBar, 1.0f); push(e, CmdRamp, t, PVolume, kTicksPerBar, 0.0f); push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, 130.0f); renderBlocks(e, 10);
  const float c8 = e.params(8).cutoff; CHECK(e.params(t).volume < 0.79f);
  push(e, CmdResetTrackParams, 0, 0); renderBlocks(e, 10);
  CHECK_NEAR(e.params(t).volume, 0.8f, 1e-5); CHECK(e.params(8).cutoff > c8 + 0.01f);
  renderBlocks(e, 22050 * 4 / kBlock); CHECK((int)e.status().bpm >= 121);
  delete &e;
}

// Finding: a CmdRamp onto an already running ramp of the same track / param used to keep the stale `from`, so the
// parameter jumped back to the original start before ramping to the new target (audible on a live key).
TEST(ramp_retarget_continues_from_current) {
  Engine& e = *freshEngine();
  // Tempo drift 120 → 130 over 60 beats, re-aimed half-way at bpm − 10 (SHIFT + tempo− mid-drift): no jump back to 120.
  push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, 130.0f);
  uint32_t t0 = e.status().tick; while (e.status().tick - t0 < 30 * (uint32_t)kTicksPerBeat) renderBlocks(e, 4);
  const int mid = e.status().bpm; CHECK(mid >= 124 && mid <= 126);
  push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, (float)(mid - 10)); renderBlocks(e, 1);
  CHECK((int)e.status().bpm >= mid - 1 && (int)e.status().bpm <= mid);        // continues from where it was ...
  renderBlocks(e, 22050 * 4 / kBlock);                                          // ≈ 8 beats later: −1.3 BPM
  CHECK((int)e.status().bpm <= mid - 1 && (int)e.status().bpm >= mid - 2);     // ... downwards ...
  t0 = e.status().tick; while (e.status().tick - t0 < 61 * (uint32_t)kTicksPerBeat) renderBlocks(e, 8);
  CHECK_EQ((int)e.status().bpm, mid - 10);                                      // ... and arrives at the new target
  // A hold-ramp vol− (0.8 → 0 over a bar) reversed after 0.3 bar by a vol+ press (step + ramp to max, as the UI sends
  // it): the new ramp starts at the stepped value, not at the stale 0.8, and SHIFT snap-back still returns to 0.8.
  push(e, CmdRamp, 0, PVolume, kTicksPerBar, 0.0f); renderBlocks(e, 22050 * 6 / 10 / kBlock);   // 0.6 s ≈ 0.3 bar → ≈ 0.56
  const float v = e.params(0).volume; CHECK(v > 0.5f && v < 0.65f);
  push(e, CmdSetParam, 0, PVolume, 0, v + 0.05f); push(e, CmdRamp, 0, PVolume, kTicksPerBar, 1.0f); renderBlocks(e, 1);
  CHECK_NEAR(e.params(0).volume, v + 0.05f, 0.01);
  renderBlocks(e, 22050 / kBlock); CHECK(e.params(0).volume > v + 0.2f);       // ramping up from there
  push(e, CmdCancelRamps); renderBlocks(e, 1); CHECK_NEAR(e.params(0).volume, 0.8f, 1e-5);   // origin: the value before the first ramp
  // The same for a knob: cutoff ramp up, re-targeted down after a while, continues from its current value.
  push(e, CmdRamp, 2, PCutoff, kTicksPerBar, 1.0f); renderBlocks(e, 22050 / kBlock); const float k = e.params(2).cutoff; CHECK(k > 0.6f && k < 0.9f);
  push(e, CmdRamp, 2, PCutoff, kTicksPerBar, 0.0f); renderBlocks(e, 1); CHECK_NEAR(e.params(2).cutoff, k, 0.02);
  renderBlocks(e, 10); CHECK(e.params(2).cutoff < k - 0.02f);
  delete &e;
}

// Finding: an active PTempo drift re-pinned bpm_ every block, so CmdSetTempo (tempo±) was dead for up to 60 beats.
TEST(set_tempo_stops_drift) {
  Engine& e = *freshEngine();
  push(e, CmdRamp, 0, PTempo, 60 * kTicksPerBeat, 130.0f); renderBlocks(e, 22050 * 4 / kBlock);   // ≈ 8 beats in: 121.3
  CHECK((int)e.status().bpm >= 121);
  push(e, CmdSetTempo, 0, 0, 100); renderBlocks(e, 1); CHECK_EQ((int)e.status().bpm, 100);
  renderBlocks(e, 22050 * 4 / kBlock); CHECK_EQ((int)e.status().bpm, 100);     // the drift is gone, not just overridden for a block
  // A strip ramp alongside is untouched by CmdSetTempo.
  push(e, CmdRamp, 0, PVolume, kTicksPerBar, 0.0f); renderBlocks(e, 10); const float v = e.params(0).volume;
  push(e, CmdSetTempo, 0, 0, 120); renderBlocks(e, 10); CHECK(e.params(0).volume < v - 0.01f); CHECK_EQ((int)e.status().bpm, 120);
  delete &e;
}

// Finding: only the cutoff and wave knobs were shown to change the rendered sound. Resonance, env amount, filter
// decay and drive each get a short spectral / level check on a sustained note.
TEST(knob_res_env_decay_drive_reach_voice) {
  Engine& e = *freshEngine(); const int t = 2;                                  // set 1 Pad
  e.nextPatch(t) = sustainedSaw(); push(e, CmdSetPatch, t);
  const float f0 = midiToHz(48);                                                // C3, 130.8 Hz
  // Resonance: a saw under a 980 Hz low-pass (knob 0.5); resonance 0.95 (Q ≈ 8) lifts the harmonics around the cutoff
  // (7th / 8th, 915 / 1046 Hz) against the fundamental.
  push(e, CmdSetParam, t, PCutoff, 0, 0.5f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f); push(e, CmdSetParam, t, PResonance, 0, 0.1f);
  push(e, CmdPerformNote, t, 0, 48, 0.9f); renderBlocks(e, 30);
  std::vector<float> resLo; capture(e, 40, resLo);
  push(e, CmdSetParam, t, PResonance, 0, 0.95f); renderBlocks(e, 30);
  std::vector<float> resHi; capture(e, 40, resHi);
  auto nearCutoff = [&](const std::vector<float>& s) { return (goertzel(s, 7 * f0) + goertzel(s, 8 * f0)) / goertzel(s, f0); };
  std::printf("  resonance 0.1 → harmonics 7+8 / fundamental %.3f, resonance 0.95 → %.3f\n", nearCutoff(resLo), nearCutoff(resHi));
  CHECK(nearCutoff(resHi) > 2.5f * nearCutoff(resLo)); CHECK_NEAR(e.knobs(t).resonance, knobResonance(0.95f), 1e-5);
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 20);
  // Env amount: cutoff 359 Hz (knob 0.3), filter decay knob 1.0 (2 s to −60 dB, AdEnv). With envAmt 1.0 (+4 octaves)
  // the first 90 ms of a note are bright, with envAmt 0 they are not. The knobs settle before each note on.
  push(e, CmdSetParam, t, PCutoff, 0, 0.3f); push(e, CmdSetParam, t, PResonance, 0, 0.1f); push(e, CmdSetParam, t, PDecay, 0, 1.0f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f); renderBlocks(e, 30);
  push(e, CmdPerformNote, t, 0, 48, 0.9f); std::vector<float> envOff; capture(e, 8, envOff);
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 20);
  push(e, CmdSetParam, t, PEnvAmt, 0, 1.0f); renderBlocks(e, 30);
  push(e, CmdPerformNote, t, 0, 48, 0.9f); std::vector<float> envOn; capture(e, 8, envOn);
  std::printf("  first 90 ms hf ratio: envAmt 0 → %.3f, envAmt 1 → %.3f\n", hfRatio(envOff), hfRatio(envOn));
  CHECK(hfRatio(envOn) > 2.0f * hfRatio(envOff));
  // Decay: 100..200 ms into the note the 2 s decay still holds the filter well open (≈ +2.4 oct at 150 ms); a 17 ms one
  // (knob 0.1) closed it long ago (at 300 ms the 2 s decay is itself down to +1.4 oct, so the window sits earlier).
  renderBlocks(e, 1); std::vector<float> decLong; capture(e, 8, decLong);                     // blocks 9..17 of the envAmt 1 note
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 20);
  push(e, CmdSetParam, t, PDecay, 0, 0.1f); renderBlocks(e, 30);
  push(e, CmdPerformNote, t, 0, 48, 0.9f); renderBlocks(e, 9); std::vector<float> decShort; capture(e, 8, decShort);
  std::printf("  hf ratio at 100..200 ms: decay 1.0 → %.3f, decay 0.1 → %.3f\n", hfRatio(decLong), hfRatio(decShort));
  CHECK(hfRatio(decLong) > 1.5f * hfRatio(decShort)); CHECK_NEAR(e.knobs(t).decayS, knobDecayS(0.1f), 1e-4);
  // Drive: the voice waveshaper (knob 1.0 → drive 8, pre-filter) flattens the saw while it sounds: the crest factor drops.
  push(e, CmdSetParam, t, PCutoff, 0, 0.9f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f); renderBlocks(e, 30);
  std::vector<float> clean; capture(e, 40, clean);
  push(e, CmdSetParam, t, PDrive, 0, 1.0f); renderBlocks(e, 30); std::vector<float> driven; capture(e, 40, driven);
  std::printf("  crest: drive 0 → %.2f, drive 1 → %.2f\n", crestOf(clean), crestOf(driven));
  CHECK(crestOf(driven) < 0.85f * crestOf(clean)); CHECK(rmsOf(driven) > 0.002f);
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 20);
  delete &e;
}

TEST(perform_note_any_track) {
  Engine& e = *freshEngine(); int bars = 0, kicks = 0;
  loadSlot(e, 0, 0, 4000, 20); loadSlot(e, 0, 4, 3000, 9); drainEvents(e, &bars, &kicks); kicks = 0;
  // Track 7 (set 1 Beat), pitch 36: the kick slot, a duck trigger and EvKick, all without a running pattern.
  float peak = 0; push(e, CmdPerformNote, 7, 0, 36, 1.0f); renderBlocks(e, 4, nullptr, &peak); drainEvents(e, &bars, &kicks);
  CHECK(peak > 500); CHECK_EQ(kicks, 1); CHECK(e.status().track[7].level > 0); CHECK_EQ((int)e.status().track[5].level, 0);
  renderBlocks(e, 22050 / kBlock);                                              // let it ring out
  // Kick cut is honoured; snare on an empty slot is silent.
  push(e, CmdKickCut, 0, 0, 1); kicks = 0; peak = 0; push(e, CmdPerformNote, 7, 0, 36, 1.0f); renderBlocks(e, 4, nullptr, &peak); drainEvents(e, &bars, &kicks);
  CHECK(peak < 20); CHECK_EQ(kicks, 0); push(e, CmdKickCut, 0, 0, 0);
  // Track 5 (set 1 Percussion), pitch 36: slot 4.
  peak = 0; push(e, CmdPerformNote, 5, 0, 36, 1.0f); renderBlocks(e, 4, nullptr, &peak); CHECK(peak > 500); CHECK(e.status().track[5].level > 0);
  renderBlocks(e, 22050 / kBlock);
  // Track 6 (Vocal) plays nothing.
  peak = 0; push(e, CmdPerformNote, 6, 0, 60, 1.0f); renderBlocks(e, 8, nullptr, &peak); CHECK(peak < 20);
  // Track 0 (set 1 Chord): its own voices with its patch; the Fx track stays silent (the old fixed routing is gone).
  e.nextPatch(0) = sustainedSaw(); push(e, CmdSetPatch, 0);
  peak = 0; push(e, CmdPerformNote, 0, 0, 60, 0.8f); renderBlocks(e, 10, nullptr, &peak);
  CHECK(peak > 300); CHECK(e.status().track[0].level > 0); CHECK_EQ((int)e.status().track[4].level, 0);
  push(e, CmdPerformNote, 0, 0, 60, 0.0f); renderBlocks(e, 22050 / kBlock); peak = 0; renderBlocks(e, 22050 / kBlock, nullptr, &peak); CHECK(peak < 20);
  // CmdPerformChord is handled identically (per chord note), on set 2 as well.
  e.nextPatch(8) = sustainedSaw(); push(e, CmdSetPatch, 8);
  peak = 0; push(e, CmdPerformChord, 8, 0, 60, 0.8f); push(e, CmdPerformChord, 8, 0, 64, 0.8f); push(e, CmdPerformChord, 8, 0, 67, 0.8f); renderBlocks(e, 10, nullptr, &peak);
  CHECK(peak > 300); CHECK(e.status().track[8].level > 0); CHECK_EQ((int)e.status().track[0].level, 0);
  push(e, CmdPerformChord, 8, 0, 60, 0.0f); push(e, CmdPerformChord, 8, 0, 64, 0.0f); push(e, CmdPerformChord, 8, 0, 67, 0.0f);
  renderBlocks(e, 22050 / kBlock); peak = 0; renderBlocks(e, 22050 / kBlock, nullptr, &peak); CHECK(peak < 20);
  // A mono glide patch (Bass) slides legato: the second key moves the sounding voice instead of starting a new one,
  // so releasing the first key leaves the note playing and releasing the second one ends it.
  SynthPatch mono = sustainedSaw(); mono.polyphony = 1; mono.glide = 0.08f; e.nextPatch(3) = mono; push(e, CmdSetPatch, 3);
  push(e, CmdPerformNote, 3, 0, 40, 0.9f); renderBlocks(e, 10); push(e, CmdPerformNote, 3, 0, 47, 0.9f); renderBlocks(e, 10);
  push(e, CmdPerformNote, 3, 0, 40, 0.0f); renderBlocks(e, 22050 / kBlock); peak = 0; renderBlocks(e, 4, nullptr, &peak); CHECK(peak > 300);
  push(e, CmdPerformNote, 3, 0, 47, 0.0f); renderBlocks(e, 22050 / kBlock); peak = 0; renderBlocks(e, 4, nullptr, &peak); CHECK(peak < 20);
  delete &e;
}

TEST(dist_intensity) {
  Engine& e = *freshEngine(); const int t = 2;
  e.nextPatch(t) = sustainedSaw(); push(e, CmdSetPatch, t); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f);
  push(e, CmdPerformNote, t, 0, 48, 0.9f); renderBlocks(e, 30);
  float rms[3], peak[3], crest[3]; const float dists[3] = {0.0f, 0.3f, 1.0f};
  for (int i = 0; i < 3; i++) {
    push(e, CmdSetParam, t, PDist, 0, dists[i]); renderBlocks(e, 30);           // smoothing settles (snaps within 1e-4)
    std::vector<float> s; capture(e, 40, s); rms[i] = rmsOf(s); peak[i] = peakOf(s); crest[i] = peak[i] / rms[i];
    std::printf("  dist %.1f: rms %.4f peak %.4f crest %.2f\n", dists[i], rms[i], peak[i], crest[i]);
  }
  CHECK(rms[2] > 1.5f * rms[0]);                                                // dist 1 is clearly louder than clean ...
  CHECK(rms[2] >= rms[1]);                                                      // ... never quieter than a lower dist (the makeup plateaus at drive 8) ...
  CHECK(crest[2] < crest[1]); CHECK(crest[1] < crest[0]);                        // ... and flatter (more clipped) than 0.3, which is flatter than clean
  CHECK(crest[2] < 1.25f);                                                       // dist 1 is a near square wave (a square has crest 1.0, a saw 1.73)
  // The drive / makeup of the strip follow the §9.17a formula: 1/sqrt(drive) up to drive 8, flat above, so a fully
  // clipped sample lands at 1/sqrt(8) from dist 0.23 on.
  CHECK_NEAR(distDrive(1.0f), 32.0f, 1e-5); CHECK_NEAR(distMakeup(distDrive(1.0f)), 1.0f / std::sqrt(8.0f), 1e-6);
  CHECK_NEAR(distMakeup(4.0f), 0.5f, 1e-6); CHECK_NEAR(distMakeup(8.0f), distMakeup(32.0f), 1e-6); CHECK_NEAR(distMakeup(distDrive(0.0f)), 1.0f, 1e-6);
  // Level continuity at the gate: the stage is bypassed only at exactly 0 (drive 1, makeup 1), so dist 0 → 1e-4 and
  // 0.009 → 0.011 (across the old 0.01 gate, which stepped +1.2 dB there) each move the level by under 0.3 dB; between
  // them the level follows the makeup curve's small-signal gain sqrt(drive) (×1.13 at 0.009, less the soft-clip knee).
  float small[4]; const float lows[4] = {0.0f, 1e-4f, 0.009f, 0.011f};
  for (int i = 0; i < 4; i++) { push(e, CmdSetParam, t, PDist, 0, lows[i]); renderBlocks(e, 30); std::vector<float> s; capture(e, 40, s); small[i] = rmsOf(s); }
  std::printf("  dist 0 / 1e-4 / 0.009 / 0.011: rms %.5f %.5f %.5f %.5f\n", small[0], small[1], small[2], small[3]);
  const float gate0 = small[1] / small[0], gate1 = small[3] / small[2], curve = small[2] / small[0];
  CHECK(gate0 < 1.035f && gate0 > 1.0f / 1.035f); CHECK(gate1 < 1.035f && gate1 > 1.0f / 1.035f);
  CHECK(curve > 0.95f * std::sqrt(distDrive(0.009f)) && curve < 1.02f * std::sqrt(distDrive(0.009f)));
  delete &e;
}

// ---------------------------------------------------------------------------------- SPEC §9.19: loop mode

// A stream producer for the tests (the generator's pump on the device): the ring maps absolute sample indices to
// positions from `base` (VocalRing in engine.h); top() keeps a sine written `ahead` samples past the engine's read
// position so the engine never starves unless the test wants it to.
struct TestStream {
  VocalRing* ring = nullptr; uint32_t base = 0, written = 0; float hz = 1000.0f, amp = 0.5f;
  void start(VocalRing& r, uint32_t b, float f, float a) { ring = &r; base = b; written = 0; hz = f; amp = a; r.active.store(0); r.reset(b); }
  int16_t sample(uint32_t i) const { return (int16_t)(amp * 32767.0f * std::sin(2.0 * 3.14159265358979 * hz * (double)i / kSampleRate)); }
  // Content is placed by the engine's clock (as the generator places it by the bar clock): written `ahead` samples past
  // the later of the engine's read position and its sample clock, resuming at the read position when the engine skipped
  // ahead while the stream was not rendered (generator.cpp fill()). Indices wrap with VocalRing::wrap (3072 is no power of two).
  void top(const Engine& e, int ahead) {
    const uint32_t rd = ring->r.load(); const uint32_t now = e.status().sampleClock - base;
    uint32_t from = rd; if ((int32_t)(now - from) > 0) from = now;              // the positions [from, from + ahead) are ahead of anything the engine still reads
    if ((int32_t)(from - written) > 0) written = from;
    const uint32_t target = from + (uint32_t)(ahead < ring->cap ? ahead : ring->cap);
    while ((int32_t)(target - written) > 0) { ring->buf[ring->wrap(written)] = sample(written); written++; }
    ring->w.store(written, std::memory_order_release); ring->active.store(1, std::memory_order_release);
  }
};
// Render to just past the next bar line (every bar-line command of the test lands there); returns the blocks rendered.
static int renderToNextBar(Engine& e, TestStream* ts = nullptr, float* peak = nullptr) {
  const uint32_t bar0 = e.status().tick / kTicksPerBar; int blocks = 0;
  while (e.status().tick / kTicksPerBar == bar0 && blocks < 400) { if (ts) ts->top(e, kBlock * 6); renderBlocks(e, 1, nullptr, peak); blocks++; }
  CHECK(blocks < 400);
  return blocks;
}
static void renderTopped(Engine& e, TestStream& ts, int blocks, float* peak = nullptr) { for (int b = 0; b < blocks; b++) { ts.top(e, kBlock * 6); renderBlocks(e, 1, nullptr, peak); } }
static int maxLevelOver(Engine& e, int blocks, float* peak = nullptr) { int m = 0; for (int b = 0; b < blocks; b++) { renderBlocks(e, 1, nullptr, peak); if (e.status().track[0].level > m) m = e.status().track[0].level; } return m; }
static void chordPattern(Engine& e, int t) {                                   // four quarter notes, one bar
  CHECK(e.beginNextWrite(t)); Pattern& p = e.nextPattern(t); p.clear(kTicksPerBar);
  for (int b = 0; b < 4; b++) p.add(Note::make(b * kTicksPerBeat, 48, 60 + b, 100));
  p.sortByStart(); e.endNextWrite(t);
}

TEST(engine_size_after_ulaw_echo) {                                            // SPEC §9.19 (a): the int16 echo line (20 480 B) became µ-law (10 240 B)
  std::printf("  sizeof(Engine) = %zu B, echo line %d B (µ-law), loop slot heads %zu B, chorus bus %zu B x %d, SynthVoice %zu B, SynthPatch %zu B\n",
              sizeof(Engine), kEchoSamples, sizeof(VocalRing) * kLoopSlots, sizeof(Chorus), kSets, sizeof(SynthVoice), sizeof(SynthPatch));
  // Host (64-bit) bound: 216 504 B after the µ-law line, + 512 B SPEC §9.22 TrackParams / SynthKnobs (sat, accent, chorus × 32 /
  // sat, accent × 16), + 256 B SynthPatch sat / accent (× 32 patch copies), + 2 × 676 B chorus bus = 218 624 B; the voice
  // state did not grow (the accent flag sits in padding, level × velocity reuses vel_, sat is read from the knobs per run).
  // On the S3 the same additions are 1 864 B on top of 215 436 B (measured with the core's toolchain, 2026-10-16).
  CHECK(sizeof(Engine) <= 218700u);
  CHECK(sizeof(Chorus) <= 700u);
  Engine& e = *freshEngine();
  CHECK_EQ(e.loopRing(0).cap, kLoopRingBytes / 2); CHECK(e.loopRing(1).buf == gLoopMem[1]);
  static_assert((kLoopRingBytes / 2) & (kLoopRingBytes / 2 - 1), "3072 samples is not a power of two: ring indices go through VocalRing::wrap");
  CHECK_EQ((int)e.loopRing(0).mask, 0); CHECK_EQ((int)e.loopRing(0).wrap(kLoopRingBytes / 2 + 5), 5); CHECK_EQ((int)e.loopRing(0).wrap(7000), 7000 % (kLoopRingBytes / 2));
  CHECK_EQ((int)e.vocalRing(0).mask, kVocalRingBytes / 2 - 1); CHECK_EQ((int)e.vocalRing(0).wrap(kVocalRingBytes / 2 + 5), 5);   // the vocal rings keep their mask
  e.setLoopRing(1, gLoopMem[1], 2048); CHECK_EQ(e.loopRing(1).cap, 2048); CHECK_EQ((int)e.loopRing(1).mask, 2047);   // a power of two is masked
  e.setLoopRing(1, nullptr, 0); CHECK_EQ(e.loopRing(1).cap, 0); CHECK(e.loopRing(1).buf == nullptr);
  e.setLoopRing(1, gLoopMem[1], 1); CHECK_EQ(e.loopRing(1).cap, 0);
  e.setLoopRing(1, gLoopMem[1], kLoopRingBytes / 2);
  for (int t = 0; t < kTracks; t++) { CHECK(!e.trackStreams(t)); CHECK_EQ(e.trackStream(t), -1); }
  CHECK_EQ(e.loopTrack(0), -1); CHECK_EQ(e.loopTrack(1), -1); CHECK_EQ((int)e.streamUnderruns(kSets), 0);
  delete &e;
}

TEST(loop_mode_on_off_at_bar) {
  Engine& e = *freshEngine(); const int t = 0, slot = 0, stream = kSets + slot;   // set 1 Chord on loop slot 0
  e.nextPatch(t) = sustainedSaw(); push(e, CmdSetPatch, t); chordPattern(e, t);
  push(e, CmdSwapPattern, t, 2, 0); push(e, CmdTrackToggle, t);
  renderToNextBar(e); renderBlocks(e, 2); CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying); CHECK_EQ((int)e.status().track[t].variant, 2);
  float peak = 0; renderBlocks(e, 20, nullptr, &peak); CHECK(peak > 300);      // the pattern sounds
  // Loop mode on: nothing changes before the bar line; at the bar the stream is bound, the slot taken, the voices released.
  push(e, CmdLoopMode, t, 1, stream); renderBlocks(e, 2);
  CHECK(!e.trackStreams(t)); CHECK_EQ(e.loopTrack(slot), -1);
  renderToNextBar(e); renderBlocks(e, 1);
  CHECK(e.trackStreams(t)); CHECK_EQ(e.trackStream(t), stream); CHECK_EQ(e.loopTrack(slot), t);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);                   // the track keeps running, now as a stream
  // Not stepped: with the ring empty the track falls silent (the saw's 50 ms release) and stays so for two bars.
  renderBlocks(e, 22050 / 2 / kBlock); peak = 0; renderToNextBar(e, nullptr, &peak); renderToNextBar(e, nullptr, &peak);
  CHECK(peak < 20); CHECK_EQ((int)e.status().track[t].level, 0);
  CHECK_EQ((int)e.streamUnderruns(stream), 0);                                 // an inactive ring is not an underrun
  // CmdLoopLoaded: nextLoop(slot) describes the content; adopted at the bar line like CmdVocalLoaded.
  TestStream ts; ts.start(e.loopRing(slot), e.status().sampleClock, 1000.0f, 0.5f); ts.top(e, kBlock * 6);
  e.nextLoop(slot).gain = 1.0f; e.nextLoop(slot).present = 1; e.nextLoop(slot).loopTicks = 4 * kTicksPerBar; push(e, CmdLoopLoaded, 0, slot);
  renderTopped(e, ts, 2); CHECK_EQ((int)e.loopState(slot).present, 0);
  float before = 0;                                                            // silent in every block before the one holding the bar line ...
  for (;;) { const uint32_t b0 = e.status().tick / kTicksPerBar; ts.top(e, kBlock * 6); float pk = 0; renderBlocks(e, 1, nullptr, &pk); if (e.status().tick / kTicksPerBar != b0) { peak = pk; break; } if (pk > before) before = pk; }
  CHECK(before < 20);
  CHECK_EQ((int)e.loopState(slot).present, 1); CHECK_EQ((int)e.loopState(slot).loopTicks, 4 * kTicksPerBar);
  CHECK(peak > 300);                                                           // ... and placed from the bar sample inside that very block (renderStream reads at sampleClock + offset)
  peak = 0; renderTopped(e, ts, 40, &peak);
  CHECK(peak > 1000); CHECK(e.status().track[t].level > 0);                    // the 1 kHz loop plays through the track's strip
  CHECK_EQ((int)e.streamUnderruns(stream), 0);
  std::printf("  loop stream through the Chord strip: peak %.0f, level %u, ring read %u\n", peak, (unsigned)e.status().track[t].level, (unsigned)e.loopRing(slot).r.load());
  // The strip applies: volume 0 silences the stream.
  push(e, CmdSetParam, t, PVolume, 0, 0.0f); renderTopped(e, ts, 20); peak = 0; renderTopped(e, ts, 10, &peak); CHECK(peak < 20);
  push(e, CmdSetParam, t, PVolume, 0, 0.8f); renderTopped(e, ts, 20);
  // Starvation is counted per stream: stop topping up and the read runs past the write frontier.
  const uint16_t u0 = e.streamUnderruns(stream); renderBlocks(e, 8); CHECK((int)(e.streamUnderruns(stream) - u0) >= kBlock);
  CHECK_EQ((int)e.streamUnderruns(kSets + 1), 0); CHECK_EQ((int)e.status().vocalUnderruns, 0);
  ts.top(e, kBlock * 6); renderTopped(e, ts, 4);
  // A swap on a streaming track carries the stream's clip length / variant (up to kMaxLoopTicks) and leaves the parked pattern alone.
  CHECK(e.beginNextWrite(t)); e.nextPattern(t).clear(kTicksPerBar); e.endNextWrite(t);   // an empty carrier, as the vocal's switch writes it
  push(e, CmdSwapPattern, t, 1, kMaxLoopTicks); renderToNextBar(e, &ts); renderTopped(e, ts, 1);
  CHECK_EQ((int)e.status().track[t].lenTicks, kMaxLoopTicks); CHECK_EQ((int)e.status().track[t].variant, 1);
  CHECK_EQ((int)e.activePattern(t).count, 4);                                  // the parked pattern survived the carrier
  push(e, CmdSetLength, t, 0, 2 * kTicksPerBar); renderToNextBar(e, &ts); renderTopped(e, ts, 1); CHECK_EQ((int)e.status().track[t].lenTicks, 2 * kTicksPerBar);
  // Loop off: at the bar the slot is freed, the parked clip comes back (one bar, variant 2) and the pattern plays again.
  push(e, CmdLoopMode, t, 0, stream); renderTopped(e, ts, 2); CHECK(e.trackStreams(t));
  renderToNextBar(e, &ts); renderBlocks(e, 1);
  CHECK(!e.trackStreams(t)); CHECK_EQ(e.trackStream(t), -1); CHECK_EQ(e.loopTrack(slot), -1);
  CHECK_EQ((int)e.status().track[t].lenTicks, kTicksPerBar); CHECK_EQ((int)e.status().track[t].variant, 2);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  const uint16_t u1 = e.streamUnderruns(stream);
  peak = 0; CHECK(maxLevelOver(e, 40, &peak) > 0); CHECK(peak > 300);          // notes fire again (the ring is not topped up ...)
  CHECK_EQ((int)e.streamUnderruns(stream), (int)u1);                           // ... and no longer read)
  delete &e;
}

TEST(loop_slot_rebind_and_empty_track) {
  Engine& e = *freshEngine(); const int a = 1, b = 2, slot = 1, stream = kSets + slot;   // set 1 Lead and Pad
  // An empty track (no pattern ever loaded) can enter loop mode and be started: the stream is its content.
  CHECK_EQ((int)e.status().track[a].state, (int)PSEmpty);
  push(e, CmdLoopMode, a, 1, stream); renderToNextBar(e); renderBlocks(e, 1);
  CHECK(e.trackStreams(a)); CHECK_EQ(e.loopTrack(slot), a); CHECK_EQ((int)e.status().track[a].state, (int)PSStopped);
  push(e, CmdTrackToggle, a); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[a].state, (int)PSPlaying);
  // Another track binding the same slot (never done by the generator) takes it: the first leaves loop mode, back to empty.
  push(e, CmdLoopMode, b, 1, stream); renderToNextBar(e); renderBlocks(e, 1);
  CHECK(!e.trackStreams(a)); CHECK(e.trackStreams(b)); CHECK_EQ(e.loopTrack(slot), b); CHECK_EQ((int)e.status().track[a].state, (int)PSEmpty);
  // Off again on b, and an on / off pair before the bar ends as off.
  push(e, CmdLoopMode, b, 0, 0); renderToNextBar(e); renderBlocks(e, 1); CHECK(!e.trackStreams(b)); CHECK_EQ(e.loopTrack(slot), -1);
  push(e, CmdLoopMode, b, 1, stream); push(e, CmdLoopMode, b, 0, 0); renderToNextBar(e); renderBlocks(e, 1); CHECK(!e.trackStreams(b)); CHECK_EQ(e.loopTrack(slot), -1);
  // A non-Vocal track may not bind a vocal stream (one reader per ring): the command is dropped.
  push(e, CmdLoopMode, b, 1, 0); push(e, CmdLoopMode, b, 1, kStreams); renderToNextBar(e); renderBlocks(e, 1); CHECK(!e.trackStreams(b));
  delete &e;
}

TEST(loop_clip_cap) {
  Engine& e = *freshEngine(); const int t = 3;                                 // set 1 Bass
  chordPattern(e, t); push(e, CmdSwapPattern, t, 0, kMaxLoopTicks); renderToNextBar(e); renderBlocks(e, 1);
  CHECK_EQ((int)e.status().track[t].lenTicks, kMaxPatternTicks);               // a pattern keeps the 16-beat cap
  push(e, CmdSetLength, t, 0, kMaxLoopTicks); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[t].lenTicks, kMaxPatternTicks);
  push(e, CmdLoopMode, t, 1, kSets); renderToNextBar(e); renderBlocks(e, 1); CHECK(e.trackStreams(t));
  push(e, CmdSetLength, t, 0, kMaxLoopTicks); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[t].lenTicks, kMaxLoopTicks);   // a streaming track: 8 bars
  push(e, CmdSetLength, t, 0, kMaxLoopTicks + kTicksPerBar); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[t].lenTicks, kMaxLoopTicks);
  push(e, CmdSwapPattern, t, 3, kTicksPerBeat); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[t].lenTicks, kTicksPerBeat);
  push(e, CmdSwapPattern, t, 0, 0); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[t].lenTicks, kTicksPerBeat);   // 0 keeps the stream's length
  push(e, CmdLoopMode, t, 0, 0); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[t].lenTicks, kMaxPatternTicks);   // the parked clip returns
  // The Vocal track keeps its own cap (a 16-bar window + pause) in both modes.
  CHECK(e.beginNextWrite(Vocal)); e.nextPattern(Vocal).clear(kTicksPerBar); e.endNextWrite(Vocal);
  push(e, CmdSwapPattern, Vocal, 0, kMaxVocalClipTicks); renderToNextBar(e); renderBlocks(e, 1); CHECK_EQ((int)e.status().track[Vocal].lenTicks, kMaxVocalClipTicks);
  delete &e;
}

TEST(loop_beat_kick_cut_is_a_highpass) {
  Engine& e = *freshEngine(); const int beat = 7, slot = 1, stream = kSets + slot;   // set 1 Beat
  push(e, CmdSetParam, beat, PFilter, 0, -0.3f);
  push(e, CmdKickCut, 0, 0, 1); renderBlocks(e, 20);                          // not streaming: the cut leaves the strip alone
  CHECK_NEAR(e.params(beat).filter, -0.3f, 1e-6); CHECK_EQ((int)e.status().track[beat].kickCut, 1);
  push(e, CmdKickCut, 0, 0, 0); renderBlocks(e, 2);
  // A 100 Hz "drum loop" on the Beat track.
  push(e, CmdLoopMode, beat, 1, stream); renderToNextBar(e); renderBlocks(e, 1); CHECK(e.trackStreams(beat));
  TestStream ts; ts.start(e.loopRing(slot), e.status().sampleClock, 100.0f, 0.5f); ts.top(e, kBlock * 6);
  e.nextLoop(slot).gain = 1.0f; e.nextLoop(slot).present = 1; push(e, CmdLoopLoaded, 0, slot); push(e, CmdTrackToggle, beat);
  renderToNextBar(e, &ts); renderTopped(e, ts, 30);
  CHECK_EQ((int)e.status().track[beat].state, (int)PSPlaying);
  std::vector<float> open; for (int b = 0; b < 40; b++) { ts.top(e, kBlock * 6); capture(e, 1, open); }
  CHECK(rmsOf(open) > 0.01f);
  // Kick cut on: the strip filter target goes to +0.6 (a 1.6 kHz high-pass) at once; the 100 Hz loop all but vanishes.
  push(e, CmdKickCut, 0, 0, 1); renderTopped(e, ts, 30);
  CHECK_NEAR(e.params(beat).filter, 0.6f, 1e-6); CHECK_EQ((int)e.status().track[beat].kickCut, 1);
  std::vector<float> cut; for (int b = 0; b < 40; b++) { ts.top(e, kBlock * 6); capture(e, 1, cut); }
  std::printf("  loop Beat 100 Hz: rms %.4f, with the kick cut (filter +0.6) %.4f\n", rmsOf(open), rmsOf(cut));
  CHECK(rmsOf(cut) < 0.1f * rmsOf(open));
  // A reset during the cut keeps the high-pass (the cut is still on) and the cut's end restores the reset's default.
  push(e, CmdResetTrack, beat); renderTopped(e, ts, 2); CHECK_NEAR(e.params(beat).filter, 0.6f, 1e-6);
  push(e, CmdKickCut, 0, 0, 0); renderTopped(e, ts, 30); CHECK_NEAR(e.params(beat).filter, 0.0f, 1e-6); CHECK_EQ((int)e.status().track[beat].kickCut, 0);
  std::vector<float> back; for (int b = 0; b < 40; b++) { ts.top(e, kBlock * 6); capture(e, 1, back); }
  CHECK_EQ((int)e.streamUnderruns(stream), 0);
  CHECK(rmsOf(back) > 0.5f * rmsOf(open));
  // Off restores what the cut replaced; a cut that is on when loop mode starts applies at that bar; loop off restores too.
  push(e, CmdSetParam, beat, PFilter, 0, -0.3f); renderTopped(e, ts, 2);
  push(e, CmdKickCut, 0, 0, 1); renderTopped(e, ts, 2); CHECK_NEAR(e.params(beat).filter, 0.6f, 1e-6);
  push(e, CmdKickCut, 0, 0, 0); renderTopped(e, ts, 2); CHECK_NEAR(e.params(beat).filter, -0.3f, 1e-6);
  push(e, CmdLoopMode, beat, 0, 0); renderToNextBar(e, &ts); renderTopped(e, ts, 1); CHECK(!e.trackStreams(beat));
  push(e, CmdKickCut, 0, 0, 1); renderBlocks(e, 2); CHECK_NEAR(e.params(beat).filter, -0.3f, 1e-6);   // a pattern Beat: the mask handles it
  push(e, CmdLoopMode, beat, 1, stream); renderToNextBar(e); renderBlocks(e, 1); CHECK_NEAR(e.params(beat).filter, 0.6f, 1e-6);
  push(e, CmdLoopMode, beat, 0, 0); renderToNextBar(e); renderBlocks(e, 1); CHECK_NEAR(e.params(beat).filter, -0.3f, 1e-6); CHECK_EQ((int)e.status().track[beat].kickCut, 1);
  // The other set's Beat is untouched throughout.
  CHECK_NEAR(e.params(15).filter, 0.0f, 1e-6);
  delete &e;
}

TEST(vocal_track_loop_mode_uses_its_own_stream) {
  Engine& e = *freshEngine(); const int v = Vocal;                            // set 1 Vocal, stream 0
  // The generator feeds the set's vocal ring (a plain loop in loop mode); the engine renders it exactly as a phrase.
  TestStream ts; ts.start(e.vocalRing(0), e.status().sampleClock, 500.0f, 0.5f); ts.top(e, kBlock * 6);
  e.nextVocal(0).gain = 1.0f; e.nextVocal(0).present = 1; push(e, CmdVocalLoaded, 0, 0);
  CHECK(e.beginNextWrite(v)); e.nextPattern(v).clear(kTicksPerBar); e.endNextWrite(v); push(e, CmdSwapPattern, v, 0, 2 * kTicksPerBar);
  push(e, CmdTrackToggle, v); renderToNextBar(e, &ts); renderTopped(e, ts, 10);
  CHECK_EQ((int)e.status().track[v].state, (int)PSPlaying); CHECK_EQ((int)e.status().track[v].lenTicks, 2 * kTicksPerBar);
  float peak = 0; renderTopped(e, ts, 20, &peak); CHECK(peak > 1000);
  // Loop mode on the Vocal track: it binds its own stream (whatever slot i names), no loop slot is used.
  push(e, CmdLoopMode, v, 1, kSets + 1); renderToNextBar(e, &ts); renderTopped(e, ts, 1);
  CHECK(e.trackStreams(v)); CHECK_EQ(e.trackStream(v), 0); CHECK_EQ(e.loopTrack(0), -1); CHECK_EQ(e.loopTrack(1), -1);
  peak = 0; renderTopped(e, ts, 20, &peak); CHECK(peak > 1000); CHECK_EQ((int)e.status().vocalUnderruns, 0);   // still rendered
  // Its swaps keep their meaning: the empty pattern carries the streamed cycle's length, capped at kMaxVocalClipTicks.
  CHECK(e.beginNextWrite(v)); e.nextPattern(v).clear(kTicksPerBar); e.endNextWrite(v); push(e, CmdSwapPattern, v, 1, 8 * kTicksPerBar);
  renderToNextBar(e, &ts); renderTopped(e, ts, 1); CHECK_EQ((int)e.status().track[v].lenTicks, 8 * kTicksPerBar); CHECK_EQ((int)e.status().track[v].variant, 1);
  // Off: the binding goes; the length stays what the generator last published (it owns the vocal's clip in both modes).
  push(e, CmdLoopMode, v, 0, 0); renderToNextBar(e, &ts); renderTopped(e, ts, 1);
  CHECK(!e.trackStreams(v)); CHECK_EQ(e.trackStream(v), -1); CHECK_EQ((int)e.status().track[v].lenTicks, 8 * kTicksPerBar);
  peak = 0; renderTopped(e, ts, 20, &peak); CHECK(peak > 1000);
  // Set 2's Vocal binds stream 1.
  push(e, CmdLoopMode, kTracksPerSet + Vocal, 1, 1); renderToNextBar(e, &ts); renderTopped(e, ts, 1); CHECK_EQ(e.trackStream(kTracksPerSet + Vocal), 1);
  delete &e;
}

// SPEC §9.20: a queued stop keeps the track audible until the bar line, like a queued start. Four kicks per bar on
// the Beat; the stop tap lands right after a bar line, the three remaining kicks of that bar still fire, and the
// track is silent from the next bar line on.
TEST(queued_stop_plays_until_the_bar) {
  Engine& e = *freshEngine(); const int beat = 7;
  loadSlot(e, 0, 0, 4000, 20);
  CHECK(e.beginNextWrite(beat)); Pattern& p = e.nextPattern(beat); p.clear(kTicksPerBar);
  for (int k = 0; k < 4; k++) p.add(Note::make((uint16_t)(k * kTicksPerBeat), 12, 36, 110));
  p.sortByStart(); e.endNextWrite(beat);
  push(e, CmdSwapPattern, beat, 3, 0); push(e, CmdTrackToggle, beat); renderToNextBar(e); renderBlocks(e, 4);
  CHECK_EQ((int)e.status().track[beat].state, (int)PSPlaying);
  renderToNextBar(e); renderBlocks(e, 2); drainEvents(e, nullptr, nullptr);     // a few ticks into a fresh bar, before the beat-2 kick
  push(e, CmdTrackToggle, beat); renderBlocks(e, 1);
  CHECK_EQ((int)e.status().track[beat].state, (int)PSStopQueued);
  int kicks = 0; renderBlocks(e, 150, nullptr, nullptr); drainEvents(e, nullptr, &kicks);   // 1.74 s: still inside the 2 s bar
  CHECK(kicks >= 2);                                                            // beats 2, 3 (and 4) still fire while the stop is queued
  CHECK_EQ((int)e.status().track[beat].state, (int)PSStopQueued);
  renderToNextBar(e); renderBlocks(e, 2); drainEvents(e, nullptr, nullptr);
  CHECK_EQ((int)e.status().track[beat].state, (int)PSStopped);                 // the stop landed on the bar line
  kicks = 0; renderBlocks(e, 170); drainEvents(e, nullptr, &kicks); CHECK_EQ(kicks, 0);   // and nothing fires afterwards
}

TEST(echo_tail_through_engine_reaches_exact_zero) {                            // SPEC §9.19 (a): the µ-law echo line empties completely
  Engine& e = *freshEngine(); const int beat = 7;
  loadSlot(e, 0, 0, 4000, 20);
  CHECK(e.beginNextWrite(beat)); Pattern& p = e.nextPattern(beat); p.clear(kTicksPerBar); p.add(Note::make(0, 12, 36, 110)); p.sortByStart(); e.endNextWrite(beat);
  push(e, CmdSwapPattern, beat, 3, 0); push(e, CmdSetParam, beat, PEcho, 0, 0.6f); push(e, CmdSetParam, beat, PReverb, 0, 0.0f);
  push(e, CmdTrackToggle, beat); renderToNextBar(e); renderBlocks(e, 10);
  float peak = 0; renderBlocks(e, 10, nullptr, &peak); CHECK(peak > 500);
  push(e, CmdTrackStopNow, beat); renderBlocks(e, 2);
  int16_t buf[kBlock]; int lastNz = -1; const int blocks = 8 * kSampleRate / kBlock;
  for (int b = 0; b < blocks; b++) { e.render(buf, kBlock); for (int i = 0; i < kBlock; i++) if (buf[i] != 0) lastNz = b; }
  CHECK(lastNz >= 0); CHECK(lastNz < blocks - 8);                               // the repeats were there, and the output is exact zero well before 8 s
  bool zero = true; for (int b = 0; b < 4; b++) { e.render(buf, kBlock); for (int i = 0; i < kBlock; i++) if (buf[i] != 0) zero = false; }
  CHECK(zero);
  std::printf("  kick + echo 0.6: master output exact zero %.2f s after the stop\n", lastNz * (double)kBlock / kSampleRate);
  delete &e;
}

// SPEC §9.19 §1.4: on a streaming Beat track the rush loop is never stepped (the stream keeps playing; the UI turns the
// rush key into a momentary crop through Generator::loopVariant instead, the engine has no path to the generator).
// Pinned here because the UI's rush key relies on it: a CmdSnareRush that still reaches a loop Beat fires no sample over
// the stream, and today's rush returns at the bar when loop mode goes off with the key still held.
TEST(rush_on_a_loop_beat_track_fires_no_samples) {
  Engine& e = *freshEngine(); const int beat = 7, slot = 0, stream = kSets + slot;   // set 1 Beat on loop slot 0
  loadSlot(e, 0, 1, 3000, 15);                                                        // the snare slot (pitch 37)
  Pattern& rp = e.rushPattern(0); rp.clear(kTicksPerBeat); for (int k = 0; k < 4; k++) rp.add(Note::make(k * (kTicksPerBeat / 4), 12, 37, 100)); rp.sortByStart();
  CHECK(e.beginNextWrite(beat)); e.nextPattern(beat).clear(kTicksPerBar); e.endNextWrite(beat);   // an empty beat: only the rush can sound
  push(e, CmdSwapPattern, beat, 3, 0); push(e, CmdTrackToggle, beat); renderToNextBar(e); renderBlocks(e, 2);
  CHECK_EQ((int)e.status().track[beat].state, (int)PSPlaying);
  float peak = 0; renderBlocks(e, 20, nullptr, &peak); CHECK(peak < 20);              // nothing in the pattern
  push(e, CmdSnareRush, 0, 0, 1); renderBlocks(e, 4);                                 // held: 1/16 snares from the next 1/16
  peak = 0; renderBlocks(e, 40, nullptr, &peak); CHECK(peak > 300); CHECK_EQ((int)e.status().track[beat].rush, 1);
  const float rushPeak = peak;
  // Loop mode on at the bar: the rush loop is not stepped while the track streams (no loop loaded: silence), key still held.
  push(e, CmdLoopMode, beat, 1, stream); renderToNextBar(e); renderBlocks(e, 1); CHECK(e.trackStreams(beat));
  renderBlocks(e, kSampleRate / 4 / kBlock);                                          // the released voices' fade and the last snare's tail
  peak = 0; renderToNextBar(e, nullptr, &peak); renderToNextBar(e, nullptr, &peak);
  CHECK(peak < 20); CHECK_EQ((int)e.status().track[beat].level, 0);
  // Loop off with the key still held: today's rush semantics return at the bar.
  push(e, CmdLoopMode, beat, 0, stream); renderToNextBar(e); renderBlocks(e, 1); CHECK(!e.trackStreams(beat));
  peak = 0; renderToNextBar(e, nullptr, &peak); CHECK(peak > 300);
  push(e, CmdSnareRush, 0, 0, 0); renderBlocks(e, kSampleRate / 2 / kBlock);         // released (flips at the next 1/16): the empty beat is silent again
  peak = 0; renderBlocks(e, 40, nullptr, &peak); CHECK(peak < 20);
  std::printf("  rush on S1 Beat: peak %.0f; silent for 2 bars while the track streams; back after loop off\n", rushPeak);
  delete &e;
}

// ---------------------------------------------------------------------------------- SPEC §9.22: 303 character (SAT / ACC) and the chorus bus

static const SynthPatch& presetNamed(Category c, const char* want) { for (int i = 0; i < presetCount(c); i++) if (strstr(preset(c, i).name, want)) return preset(c, i); return preset(c, 0); }
// The two 303 hand presets as they were before the generated bank (presets_gen.hpp may re-trim or re-order rows and now
// carries sat / accent per row, so the scene owns its patches): Bass "Acid303" and Lead "303 Saw" (drive 0.4, res 0.8).
static SynthPatch goldenBass() {
  SynthPatch p; p.wave1 = WSaw; p.wave2 = WSaw; p.osc2Detune = 0; p.osc2Level = 0; p.osc2Octave = 0; p.subLevel = 0; p.noiseLevel = 0; p.filterMode = 0;
  p.cutoff = 600.f; p.resonance = 0.60f; p.filterEnv = 4.0f; p.keyTrack = 0.3f; p.attack = 0.002f; p.decay = 0.25f; p.sustain = 0.2f; p.release = 0.10f; p.fAttack = 0.001f; p.fDecay = 0.20f;
  p.glide = 0.06f; p.lfoRate = 0; p.lfoToCutoff = 0; p.lfoToPitch = 0; p.drive = 0.0f; p.level = 0.50f; p.polyphony = 1; strncpy(p.name, "Acid303", sizeof p.name); p.sat = 0; p.accent = 0; return p;
}
static SynthPatch goldenLead() {
  SynthPatch p; p.wave1 = WSaw; p.wave2 = WSaw; p.osc2Detune = 0; p.osc2Level = 0; p.osc2Octave = 0; p.subLevel = 0; p.noiseLevel = 0; p.filterMode = 0;
  p.cutoff = 800.f; p.resonance = 0.8f; p.filterEnv = 2.5f; p.keyTrack = 0.4f; p.attack = 0.002f; p.decay = 0.25f; p.sustain = 0.30f; p.release = 0.12f; p.fAttack = 0.001f; p.fDecay = 0.22f;
  p.glide = 0.06f; p.lfoRate = 0; p.lfoToCutoff = 0; p.lfoToPitch = 0; p.drive = 0.4f; p.level = 0.61f; p.polyphony = 1; strncpy(p.name, "303 Saw", sizeof p.name); p.sat = 0; p.accent = 0; return p;
}
// A deterministic 4 s performance: 120 BPM, set 1 Bass + Lead above, sixteenth lines at velocities 70..95 (no accent
// either way), resonance 0.9 / env 0.6 / strip dist 0.2, chorus at its default 0; `sat` goes to both tracks' SAT knob
// (0 = the clean kernel) and the ACC knob is pinned to 0 (so a bank that ships accent values cannot change the scene).
static void goldenScene(Engine& e, float sat, std::vector<int16_t>* out, uint32_t& fnv, double& rms, int& peak) {
  Cmd t; t.type = CmdSetTempo; t.i = 120; CHECK(e.cmds().push(t));
  e.nextPatch(3) = goldenBass(); push(e, CmdSetPatch, 3);
  e.nextPatch(1) = goldenLead(); push(e, CmdSetPatch, 1);
  for (int tr : {3, 1}) {
    CHECK(e.beginNextWrite(tr)); Pattern& p = e.nextPattern(tr); p.clear(kTicksPerBar);
    for (int s = 0; s < 16; s++) { uint8_t pitch = (uint8_t)(tr == 3 ? 36 + (s * 7) % 12 : 60 + (s * 5) % 12); uint8_t vel = (uint8_t)(70 + (s * 9) % 26); p.add(Note::make((uint16_t)(s * 24), s % 3 == 0 ? 30 : 20, pitch, vel)); }
    p.sortByStart(); e.endNextWrite(tr);
    push(e, CmdSwapPattern, tr, 0, 0);
    push(e, CmdSetParam, tr, PResonance, 0, 0.9f); push(e, CmdSetParam, tr, PEnvAmt, 0, 0.6f); push(e, CmdSetParam, tr, PDist, 0, 0.2f);
    push(e, CmdSetParam, tr, PSat, 0, sat); push(e, CmdSetParam, tr, PAccent, 0, 0.0f);
    push(e, CmdTrackToggle, tr);
  }
  static int16_t buf[kBlock]; fnv = 2166136261u; double acc = 0; int n = 0; peak = 0;
  for (int b = 0; b < 22050 * 4 / kBlock; b++) { e.render(buf, kBlock); for (int i = 0; i < kBlock; i++) { fnv ^= (uint16_t)buf[i]; fnv *= 16777619u; acc += (double)buf[i] * buf[i]; n++; int a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > peak) peak = a; if (out) out->push_back(buf[i]); } }
  rms = std::sqrt(acc / n);
}
static Engine* bareEngine() { Engine* e = new Engine(); ulawInit(); e->init(); bindLoopRings(*e); return e; }   // no block rendered yet (the golden scene renders first)

// SAT 0 is today's clean TPT SVF: the sat 0 kernels are the pre-§9.22 code, pinned by the FNV-1a hash / RMS / peak of
// the golden scene measured on the build before §9.22 (2026-10-16, clang -O2 on the Mac the tests run on; another
// host compiler may contract float expressions differently — re-pin the three numbers from a build without §9.22).
TEST(sat_zero_is_bit_exact_with_the_pre_922_build) {
  Engine& e = *bareEngine(); uint32_t fnv; double rms; int peak; std::vector<int16_t> clean; goldenScene(e, 0.0f, &clean, fnv, rms, peak);
  std::printf("  golden scene: fnv 0x%08x rms %.3f peak %d (pre-§9.22 build: 0x21e37ccc / 768.608 / 5129)\n", fnv, rms, peak);
  CHECK_EQ(fnv, 0x21e37cccu); CHECK_NEAR(rms, 768.608, 0.002); CHECK_EQ(peak, 5129);
  // The knob fades in continuously: at sat 1e-4 the SAT kernel runs (g = 2e-4, cubic depth (4/27) g² ≈ 6e-9) and the output
  // differs from the clean one by float rounding only (int16 LSBs), never by a step.
  Engine& f = *bareEngine(); std::vector<int16_t> tiny; goldenScene(f, 1e-4f, &tiny, fnv, rms, peak);
  CHECK_EQ(tiny.size(), clean.size()); int maxDiff = 0; for (size_t i = 0; i < clean.size(); i++) { int d = std::abs((int)tiny[i] - (int)clean[i]); if (d > maxDiff) maxDiff = d; }
  std::printf("  sat 1e-4 vs sat 0: max |diff| %d LSB over %zu samples\n", maxDiff, clean.size());
  CHECK(maxDiff <= 2); CHECK_NEAR(e.knobs(3).sat, 0.0f, 1e-6); CHECK_NEAR(f.knobs(3).sat, 1e-4f, 1e-7);
  delete &e; delete &f;
}

// The squelch: a sine sitting on the cutoff (knob 0.1 = 132 Hz for C3, resonance 0.8) is pure through the clean filter and
// gains odd harmonics as sat rises (h3 / h1 0.0025 → 0.043 measured, h5 too); a saw under a 980 Hz / Q ≈ 8 low-pass keeps
// its fundamental but the resonant lift of harmonics 7 + 8 collapses (1.01 → 0.26) and the peak slides down to 5 + 6 (the
// compressed integrator state lowers the effective cutoff), i.e. the 303's compressed, lower squelch. The knob reaches a
// sounding note (sat changes under a held note).
TEST(sat_squelch_adds_odd_harmonics_and_compresses_the_resonance) {
  const float f0 = midiToHz(48);
  Engine& e = *freshEngine(); const int t = 2; SynthPatch sine = sustainedSaw(); sine.wave1 = WSine; e.nextPatch(t) = sine; push(e, CmdSetPatch, t);
  push(e, CmdSetParam, t, PCutoff, 0, 0.1f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f); push(e, CmdSetParam, t, PResonance, 0, 0.8f);
  float h3[3], h5[3], h1[3]; const float sats[3] = {0.0f, 0.5f, 1.0f};
  for (int i = 0; i < 3; i++) {
    push(e, CmdSetParam, t, PSat, 0, sats[i]); push(e, CmdPerformNote, t, 0, 48, 0.7f); renderBlocks(e, 30);
    std::vector<float> s; capture(e, 40, s); h1[i] = goertzel(s, f0); h3[i] = goertzel(s, 3 * f0) / h1[i]; h5[i] = goertzel(s, 5 * f0) / h1[i];
    std::printf("  sine on the cutoff, sat %.1f: h3/h1 %.4f h5/h1 %.4f (h1 %.4f)\n", sats[i], h3[i], h5[i], h1[i]);
    push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 30);
  }
  CHECK(h3[0] < 0.006f); CHECK(h3[1] > 5.0f * h3[0]); CHECK(h3[2] > 8.0f * h3[0]); CHECK(h3[2] > h3[1]); CHECK(h3[2] > 0.02f); CHECK(h5[2] > 3.0f * h5[0]);
  CHECK(h1[2] > 0.1f * h1[0]);                                                   // compressed, not gone
  // The resonance peak of a saw.
  e.nextPatch(t) = sustainedSaw(); push(e, CmdSetPatch, t);
  push(e, CmdSetParam, t, PCutoff, 0, 0.5f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f); push(e, CmdSetParam, t, PResonance, 0, 0.95f);
  float lift78[2], lift56[2], fund[2];
  for (int i = 0; i < 2; i++) {
    push(e, CmdSetParam, t, PSat, 0, i ? 1.0f : 0.0f); push(e, CmdPerformNote, t, 0, 48, 0.7f); renderBlocks(e, 30);
    std::vector<float> s; capture(e, 40, s); fund[i] = goertzel(s, f0);
    lift78[i] = (goertzel(s, 7 * f0) + goertzel(s, 8 * f0)) / fund[i]; lift56[i] = (goertzel(s, 5 * f0) + goertzel(s, 6 * f0)) / fund[i];
    std::printf("  saw, 980 Hz / res 0.95, sat %d: (h7+h8)/h1 %.3f (h5+h6)/h1 %.3f h1 %.5f\n", i, lift78[i], lift56[i], fund[i]);
    push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 30);
  }
  CHECK(lift78[0] > 0.8f); CHECK(lift78[1] < 0.5f * lift78[0]);                 // the lift near the cutoff collapses ...
  CHECK(lift78[0] > lift56[0]); CHECK(lift56[1] > lift78[1]);                    // ... and the peak moves down
  CHECK_NEAR(fund[1] / fund[0], 1.0f, 0.1f);                                     // the fundamental is not what the squelch eats
  // Live: the knob moves under a sounding note (0 → 1: the lift drops at once, no retrigger).
  push(e, CmdSetParam, t, PSat, 0, 0.0f); push(e, CmdPerformNote, t, 0, 48, 0.7f); renderBlocks(e, 30);
  std::vector<float> before; capture(e, 20, before); push(e, CmdSetParam, t, PSat, 0, 1.0f); renderBlocks(e, 30); std::vector<float> after; capture(e, 20, after);
  const float lb = (goertzel(before, 7 * f0) + goertzel(before, 8 * f0)) / goertzel(before, f0), la = (goertzel(after, 7 * f0) + goertzel(after, 8 * f0)) / goertzel(after, f0);
  std::printf("  live sat 0 → 1 on a held note: (h7+h8)/h1 %.3f → %.3f\n", lb, la);
  CHECK(la < 0.5f * lb); CHECK_NEAR(e.knobs(t).sat, 1.0f, 1e-5);
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 20);
  delete &e;
}

// ACC: a note at velocity >= 100 is accented — its filter envelope amount is × (1 + accent) (a brighter sweep) and its
// level × (1 + 0.4125 × accent): +3 dB at accent 1, +1.6 dB at 0.5; a note below 100 is untouched by the knob. The
// knob is live: an accented note that is already sounding follows it (gain slewed over one modulation block).
TEST(accent_changes_sweep_and_level_of_accented_notes_only) {
  Engine& e = *freshEngine(); const int t = 2; e.nextPatch(t) = sustainedSaw(); push(e, CmdSetPatch, t);
  push(e, CmdSetParam, t, PCutoff, 0, 0.3f); push(e, CmdSetParam, t, PResonance, 0, 0.1f); push(e, CmdSetParam, t, PDecay, 0, 0.6f); push(e, CmdSetParam, t, PEnvAmt, 0, 0.4f);
  float headHf[3][2], body[3][2]; const float accs[3] = {0.0f, 0.5f, 1.0f}; const float vels[2] = {0.6f, 0.85f};   // MIDI 76 (plain) and 107 (accented)
  for (int a = 0; a < 3; a++) for (int v = 0; v < 2; v++) {
    push(e, CmdSetParam, t, PAccent, 0, accs[a]); renderBlocks(e, 30);
    push(e, CmdPerformNote, t, 0, 48, vels[v]); std::vector<float> head; capture(e, 8, head); renderBlocks(e, 60); std::vector<float> sus; capture(e, 20, sus);
    headHf[a][v] = hfRatio(head); body[a][v] = rmsOf(sus);
    std::printf("  accent %.1f, vel %d: first 90 ms hf %.3f, sustained rms %.4f\n", accs[a], (int)(vels[v] * 127), headHf[a][v], body[a][v]);
    push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 30);
  }
  for (int a = 1; a < 3; a++) { CHECK_NEAR(body[a][0] / body[0][0], 1.0f, 0.01f); CHECK_NEAR(headHf[a][0] / headHf[0][0], 1.0f, 0.01f); }   // vel 76: no accent, whatever the knob
  CHECK_NEAR(body[1][1] / body[0][1], 1.206f, 0.03f);                            // +1.6 dB at 0.5
  CHECK_NEAR(body[2][1] / body[0][1], 1.4125f, 0.03f);                           // +3 dB at 1
  CHECK(headHf[1][1] > 1.05f * headHf[0][1]); CHECK(headHf[2][1] > 1.15f * headHf[0][1]); CHECK(headHf[2][1] > headHf[1][1]);   // the sweep opens further
  CHECK_NEAR(body[0][1] / body[0][0], velCurveRatio(107, 76), 0.03f);           // accent 0: the plain velocity curve between the two notes
  // Live knob on a sounding accented note.
  push(e, CmdSetParam, t, PAccent, 0, 0.0f); renderBlocks(e, 30); push(e, CmdPerformNote, t, 0, 48, 0.85f); renderBlocks(e, 80);
  std::vector<float> a0; capture(e, 20, a0); push(e, CmdSetParam, t, PAccent, 0, 1.0f); renderBlocks(e, 30); std::vector<float> a1; capture(e, 20, a1);
  std::printf("  live accent 0 → 1 on a sounding accented note: rms x%.3f\n", rmsOf(a1) / rmsOf(a0));
  CHECK_NEAR(rmsOf(a1) / rmsOf(a0), 1.4125f, 0.03f);
  push(e, CmdPerformNote, t, 0, 48, 0.0f); renderBlocks(e, 30);
  delete &e;
}

// The chorus bus (SPEC §9.22): a track's PChorus send adds a modulated, delayed copy of its strip output to the set's
// mix; at send 0 the bus adds nothing and leaves no trace (after the send returns to 0 and the line empties the output is
// bit-identical to an engine that never used it), and the two sets' buses are separate.
TEST(chorus_send_thickens_the_set_and_is_silent_at_zero) {
  Engine& a = *freshEngine(); Engine& b = *freshEngine(); const int t = 2;
  for (Engine* e : {&a, &b}) { e->nextPatch(t) = sustainedSaw(); push(*e, CmdSetPatch, t); push(*e, CmdSetParam, t, PEnvAmt, 0, 0.0f); push(*e, CmdSetParam, t, PCutoff, 0, 0.8f); }
  push(b, CmdSetParam, t, PChorus, 0, 1.0f);
  for (Engine* e : {&a, &b}) { renderBlocks(*e, 30); push(*e, CmdPerformNote, t, 0, 52, 0.7f); renderBlocks(*e, 30); }
  std::vector<float> dry, wet; capture(a, 40, dry); capture(b, 40, wet);
  std::vector<float> diff(dry.size()); for (size_t i = 0; i < dry.size(); i++) diff[i] = wet[i] - dry[i];
  std::printf("  chorus 1.0 on a held saw: dry rms %.4f, (chorus − dry) rms %.4f, chorus rms %.4f\n", rmsOf(dry), rmsOf(diff), rmsOf(wet));
  CHECK(rmsOf(diff) > 0.3f * rmsOf(dry)); CHECK(rmsOf(diff) < 1.5f * rmsOf(dry)); CHECK(rmsOf(wet) > 0.5f * rmsOf(dry));
  // Time-varying: the comb between the dry saw and its two sweeping taps moves, so 50 ms windows of the chorused signal
  // differ in level over an LFO cycle (2 s) while the dry ones do not.
  std::vector<float> dryL, wetL; capture(a, 180, dryL); capture(b, 180, wetL);
  auto spread = [](const std::vector<float>& s) { float lo = 1e9f, hi = 0; const size_t w = 1102; for (size_t i = 0; i + w <= s.size(); i += w) { std::vector<float> win(s.begin() + i, s.begin() + i + w); float r = rmsOf(win); if (r < lo) lo = r; if (r > hi) hi = r; } return hi / lo; };
  std::printf("  50 ms window rms max/min over 2 s: dry %.3f, chorus %.3f\n", spread(dryL), spread(wetL));
  CHECK(spread(dryL) < 1.05f); CHECK(spread(wetL) > 1.08f);
  // Send back to 0: after the smoothing settles (< 0.001 within 8 blocks) and the 331-sample line empties, b equals a exactly.
  push(b, CmdSetParam, t, PChorus, 0, 0.0f); renderBlocks(a, 12); renderBlocks(b, 12);
  std::vector<float> a2, b2; capture(a, 20, a2); capture(b, 20, b2); int same = 0; for (size_t i = 0; i < a2.size(); i++) if (a2[i] == b2[i]) same++;
  CHECK_EQ(same, (int)a2.size());
  // Set 2 has its own bus: a send on set 2's Pad leaves set 1 (both playing) unchanged relative to engine a.
  for (Engine* e : {&a, &b}) { e->nextPatch(kTracksPerSet + t) = sustainedSaw(); push(*e, CmdSetPatch, kTracksPerSet + t); push(*e, CmdSetParam, kTracksPerSet + t, PVolume, 0, 0.0f); push(*e, CmdPerformNote, kTracksPerSet + t, 0, 40, 0.7f); }
  push(b, CmdSetParam, kTracksPerSet + t, PChorus, 0, 1.0f);
  renderBlocks(a, 30); renderBlocks(b, 30); std::vector<float> a3, b3; capture(a, 10, a3); capture(b, 10, b3);
  same = 0; for (size_t i = 0; i < a3.size(); i++) if (a3[i] == b3[i]) same++;
  CHECK_EQ(same, (int)a3.size());                                                // volume 0 on set 2: nothing reaches its bus, set 1 untouched
  for (Engine* e : {&a, &b}) { push(*e, CmdPerformNote, t, 0, 52, 0.0f); push(*e, CmdPerformNote, kTracksPerSet + t, 0, 40, 0.0f); renderBlocks(*e, 20); }
  delete &a; delete &b;
}

// The chorus bus reads before the tracks write, so render() caps every chunk at Chorus::kMaxChunk samples: tempos below
// 54 BPM (a tick above 256 samples) and a stopped transport now render two chunks per block. The clock stays exact and
// a note performed while the transport is stopped is continuous across the chunk seams.
TEST(chunk_cap_keeps_clock_and_stopped_rendering_continuous) {
  static_assert(Chorus::kMaxChunk <= kBlock && Chorus::kMaxChunk >= 64, "the cap splits a block into a few chunks, not into samples");
  Engine& e = *freshEngine(); push(e, CmdSetTempo, 0, 0, 40); renderBlocks(e, 1);
  const uint32_t t0 = e.status().tick; const int blocks = 22050 * 4 / kBlock; renderBlocks(e, blocks);
  const int want = (int)((int64_t)40 * kPPQN * blocks * kBlock / (60 * kSampleRate));                  // 256 ticks at 40 BPM
  CHECK((int)(e.status().tick - t0) >= want - 2 && (int)(e.status().tick - t0) <= want + 2);
  push(e, CmdMasterPlay, 0, 0); renderBlocks(e, 2); CHECK_EQ((int)e.status().running, 0);
  const int t = 2; SynthPatch sine = sustainedSaw(); sine.wave1 = WSine; e.nextPatch(t) = sine; push(e, CmdSetPatch, t); push(e, CmdSetParam, t, PEnvAmt, 0, 0.0f); push(e, CmdSetParam, t, PCutoff, 0, 1.0f);
  push(e, CmdPerformNote, t, 0, 57, 0.7f); renderBlocks(e, 30); std::vector<float> s; capture(e, 40, s);
  const float amp = peakOf(s); float step = 0; for (size_t i = 1; i < s.size(); i++) { float d = std::fabs(s[i] - s[i - 1]); if (d > step) step = d; }
  std::printf("  A3 sine while stopped (chunks of %d): peak %.4f, max step %.4f (sine slope %.4f)\n", Chorus::kMaxChunk, amp, step, amp * 2.0f * 3.14159265f * 220.0f / kSampleRate);
  CHECK(amp > 0.002f); CHECK(step < amp * 2.0f * 3.14159265f * 220.0f / kSampleRate * 1.3f + 1e-4f);
  push(e, CmdPerformNote, t, 0, 57, 0.0f); push(e, CmdMasterPlay, 0, 1); renderBlocks(e, 20);
  delete &e;
}

// Per-voice-sample cost of the squelch on the host (the S3 cannot be measured here): the SAT kernel adds 9 FP ops to the
// SVF recurrence (one multiply, two selects, a cube, a multiply-subtract, a multiply). On an out-of-order host the loop-
// carried chain pays their latency; on the in-order S3 that is ≈ 12 issue slots ≈ 0.05 µs per voice-sample.
TEST(sat_kernel_cost_per_voice_sample) {
  const SynthPatch& p = presetNamed(Lead, "303");
  SynthKnobs k; k.cutoffHz = 800; k.resonance = 0.9f; k.filterEnvOct = 2; k.decayS = 0.3f; k.drive = 0.3f; k.wave = WSaw; k.accent = 0.5f;
  double ns[2];
  for (int i = 0; i < 2; i++) {
    k.sat = i ? 1.0f : 0.0f; SynthVoice v; v.init(); v.setPatch(&p); v.setKnobs(&k); v.noteOn(45, 110);
    float buf[kBlock]; double best = 1e18; volatile float sink = 0;
    for (int rep = 0; rep < 7; rep++) { auto t0 = std::chrono::steady_clock::now(); for (int b = 0; b < 22050 / kBlock; b++) { memset(buf, 0, sizeof buf); v.render(buf, kBlock); sink = sink + buf[3]; } double t = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double)((22050 / kBlock) * kBlock); if (t < best) best = t; }
    CHECK(v.active()); ns[i] = best;
  }
  std::printf("  %s voice: sat 0 %.2f ns/sample, sat 1 %.2f ns/sample on the host (+%.2f; research Mac→S3 ratio 185x → +%.2f µs, instruction count → ≈ +0.05 µs)\n", p.name, ns[0], ns[1], ns[1] - ns[0], (ns[1] - ns[0]) * 0.185);
  CHECK(ns[1] - ns[0] < 12.0);                                                    // generous: a CI box under load
}

int main() { return lp_test::run(); }
