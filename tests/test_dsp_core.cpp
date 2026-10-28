// Host tests for dsp-core: ulaw, svf (fastTanPi), env, wavetables, sample_voice.
#include "check.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "dsp/ulaw.h"
#include "dsp/svf.h"
#include "dsp/env.h"
#include "dsp/wavetables.h"
#include "dsp/sample_voice.h"

using namespace lp;

// ---------------------------------------------------------------- µ-law

TEST(ulaw_known_codes) {
  ulawInit();
  CHECK_EQ(ulawEncode(0), 0xFF);                       // G.711 codes are bit-inverted: silence is 0xFF
  CHECK_EQ(ulawDecode(0xFF), 0);
  CHECK_EQ(ulawDecode(0x7F), 0);                       // negative zero
  CHECK_EQ(ulawDecode(0x80), 32124);                   // largest positive
  CHECK_EQ(ulawDecode(0x00), -32124);
  CHECK_EQ(ulawEncode(32767), 0x80);
  CHECK_EQ(ulawEncode(-32768), 0x00);
  CHECK_EQ(ulawDecode(ulawEncode(-32768)), -32124);    // the int16 minimum must not overflow the encoder
  CHECK_EQ(ulawDecode(ulawEncode(8)), 8);              // exact small values (segment 0, step 8)
  CHECK_EQ(ulawDecode(ulawEncode(-8)), -8);
  CHECK_EQ(ulawDecode(ulawEncode(4)), 8);              // rounds up at the half step
  CHECK_EQ(ulawDecode(ulawEncode(3)), 0);
  for (int u = 0; u < 256; u++) CHECK_EQ(ulawDecode((uint8_t)u), -ulawDecode((uint8_t)(u ^ 0x80)));   // sign symmetry
}

TEST(ulaw_roundtrip_full_range) {
  ulawInit();
  int worstAbs = 0; double worstRatio = 0; int prev = -40000; bool monotonic = true;
  for (int x = -32768; x <= 32767; x++) {
    int y = ulawDecode(ulawEncode((int16_t)x));
    int err = std::abs(y - x);
    double ax = std::abs(x);
    CHECK(err < 0.03 * ax + 200);                      // task criterion: 3% of |x| plus 200
    CHECK(err <= (ax + 132.0) / 32.0 + 1e-9);          // exact G.711 bound: half a segment step
    if (err > worstAbs) worstAbs = err;
    double ratio = err / (0.03 * ax + 200); if (ratio > worstRatio) worstRatio = ratio;
    if (y < prev) monotonic = false; prev = y;
  }
  CHECK(monotonic);                                    // decode(encode(x)) never decreases with x
  CHECK_EQ(worstAbs, 644);                             // at the clipped extremes only
  std::printf("  worst err/(0.03|x|+200) = %.3f, worst abs err = %d\n", worstRatio, worstAbs);
}

// Dynamic initialisation of this TU (like the test registrations) runs before main(); the decode table is
// constant-initialised in ulaw.cpp, so it is already valid here, long before anyone calls ulawInit().
static const int16_t gTableBeforeInit = g_ulawTable[0x80];
static const int16_t gTableBeforeInitNeg = g_ulawTable[0x00];
static const int16_t gTableBeforeInitZero = g_ulawTable[0xFF];

TEST(ulaw_table_valid_without_init) {
  CHECK_EQ(gTableBeforeInit, 32124);
  CHECK_EQ(gTableBeforeInitNeg, -32124);
  CHECK_EQ(gTableBeforeInitZero, 0);
  int16_t copy[256]; for (int i = 0; i < 256; i++) copy[i] = g_ulawTable[i];
  ulawInit();                                          // no-op kept for API compatibility: table unchanged
  for (int i = 0; i < 256; i++) CHECK_EQ(copy[i], g_ulawTable[i]);
  for (int i = 0; i < 256; i++) {                      // G.711 expansion formula, every code
    int u = ~i & 0xFF, e = (u >> 4) & 7, m = u & 0x0F, v = (((m << 3) + 0x84) << e) - 0x84;
    CHECK_EQ(g_ulawTable[i], (int16_t)((u & 0x80) ? -v : v));
  }
  int nonzero = 0; for (int i = 0; i < 256; i++) if (g_ulawTable[i] != 0) nonzero++;
  CHECK_EQ(nonzero, 254);                              // only the two zero codes (0xFF, 0x7F) are silent
}

TEST(ulaw_sine_snr) {                                  // a 1 kHz tone at -6 dBFS survives with > 30 dB SNR
  ulawInit();
  double sig = 0, noise = 0;
  for (int i = 0; i < 22050; i++) {
    float v = 16000.0f * sinf(2.0f * 3.14159265f * 1000.0f * i / 22050.0f);
    int16_t x = (int16_t)lrintf(v);
    int y = ulawDecode(ulawEncode(x));
    sig += (double)x * x; noise += (double)(y - x) * (y - x);
  }
  double snr = 10.0 * std::log10(sig / noise);
  CHECK(snr > 30.0);
  std::printf("  sine SNR %.1f dB\n", snr);
}

// ---------------------------------------------------------------- fastTanPi / Svf

TEST(fast_tan_pi_accuracy) {
  float worst = 0, at = 0;
  for (float x = 0.001f; x <= 0.45f; x += 0.0005f) {
    float ref = tanf(3.14159265f * x);
    float rel = std::fabs(fastTanPi(x) / ref - 1.0f);
    if (rel > worst) { worst = rel; at = x; }
  }
  CHECK(worst < 0.01f);                                // spec: < 1%
  CHECK(worst < 0.001f);                               // measured fit quality (7.4e-5 in double)
  std::printf("  max rel error %.2e at x=%.4f\n", worst, at);
  CHECK_NEAR(fastTanPi(0.25f), 1.0f, 1e-3f);
  CHECK_NEAR(fastTanPi(0.49f), tanf(3.14159265f * 0.49f), 0.01f * tanf(3.14159265f * 0.49f));
  float last = 0;                                      // strictly increasing on the useful range
  for (float x = 0.001f; x <= 0.49f; x += 0.001f) { float v = fastTanPi(x); CHECK(v > last); last = v; }
  CHECK(fastTanPi(0.0f) > 0.0f);                       // clamped, never zero or negative
  CHECK(fastTanPi(-1.0f) > 0.0f);
  CHECK(std::isfinite(fastTanPi(0.5f)) && fastTanPi(0.5f) > 30.0f);   // clamped at 0.49, no pole
  CHECK(std::isfinite(fastTanPi(5.0f)));
  // NaN must take the low clamp (a NaN cutoff would otherwise poison the SVF coefficients for the whole note).
  CHECK(std::isfinite(fastTanPi(NAN)));
  CHECK(fastTanPi(NAN) > 0.0f);
  CHECK_NEAR(fastTanPi(NAN), fastTanPi(0.0f), 1e-9f);
  CHECK(std::isfinite(fastTanPi(INFINITY)) && fastTanPi(INFINITY) > 30.0f);
  CHECK(std::isfinite(fastTanPi(-INFINITY)) && fastTanPi(-INFINITY) > 0.0f);
  CHECK(std::isfinite(fastTanPi(-0.0f)) && fastTanPi(-0.0f) > 0.0f);
}

static float svfSteadyGain(uint8_t mode, float cutoff, float hz) {
  Svf f; f.set(cutoff, 0.2f, mode);
  float peak = 0;
  for (int i = 0; i < 22050; i++) {
    float y = f.process(sinf(2.0f * 3.14159265f * hz * i / 22050.0f));
    if (i > 11025 && std::fabs(y) > peak) peak = std::fabs(y);
  }
  return peak;
}

TEST(svf_modes_with_fast_tan) {
  CHECK_NEAR(svfSteadyGain(0, 1000.0f, 100.0f), 1.0f, 0.05f);          // LP passes below cutoff
  CHECK(svfSteadyGain(0, 1000.0f, 8000.0f) < 0.05f);                   // and stops 3 octaves up (> 26 dB)
  CHECK(svfSteadyGain(2, 1000.0f, 100.0f) < 0.05f);                    // HP mirror
  CHECK_NEAR(svfSteadyGain(2, 1000.0f, 8000.0f), 1.0f, 0.05f);
  CHECK(svfSteadyGain(1, 1000.0f, 1000.0f) > 0.5f);                    // BP peaks at the cutoff
  CHECK(svfSteadyGain(1, 1000.0f, 100.0f) < 0.3f);
  Svf a, b; a.set(2000.0f, 0.3f, 0); b = a; b.g = fastTanPi(2000.0f / 22050.0f);   // fastTanPi substitutes tanf
  CHECK_NEAR(a.g, b.g, 1e-4f);
  // The synth voice derives g/a1/a2/a3 from fastTanPi every block: a NaN cutoff keeps the filter finite.
  Svf c; c.set(1000.0f, 0.2f, 0); float g = fastTanPi(NAN);
  c.g = g; c.a1 = 1.0f / (1.0f + g * (g + c.k)); c.a2 = g * c.a1; c.a3 = g * c.a2;
  bool fin = true; for (int i = 0; i < 2000; i++) if (!std::isfinite(c.process(0.5f))) fin = false;
  CHECK(fin);
  CHECK(std::isfinite(c.z1) && std::isfinite(c.z2));
}

// ---------------------------------------------------------------- envelopes

TEST(adsr_attack_time) {
  Adsr e; e.set(0.1f, 0.05f, 0.5f, 0.1f); e.gate(true);
  int n = 0; while (e.stage == Adsr::Attack && n < 100000) { e.process(); n++; }
  CHECK(std::abs(n - 2205) <= 2);                      // 0.1 s * 22050 = 2205 samples to reach 1.0
  CHECK_NEAR(e.level, 1.0f, 1e-6f);
  CHECK_EQ((int)e.stage, (int)Adsr::Decay);
  Adsr q; q.set(0.0f, 0.0f, 1.0f, 0.0f); q.gate(true);   // minimum 1 ms: 22.05 samples
  n = 0; while (q.stage == Adsr::Attack && n < 1000) { q.process(); n++; }
  CHECK(n >= 22 && n <= 23);
}

TEST(adsr_decay_sustain_release) {
  Adsr e; e.set(0.001f, 0.05f, 0.5f, 0.1f); e.gate(true);
  while (e.stage == Adsr::Attack) e.process();
  // Decay: distance to sustain shrinks by 60 dB in 0.05 s; the stage ends when it is < 0.001 absolute.
  int n = 0; while (e.stage == Adsr::Decay && n < 100000) { e.process(); n++; }
  int expectDecay = (int)(0.05f * 22050 * std::log(0.002) / std::log(0.001));   // 0.5 -> 0.001
  CHECK(std::abs(n - expectDecay) <= 3);
  CHECK_EQ((int)e.stage, (int)Adsr::Sustain);
  for (int i = 0; i < 1000; i++) CHECK_NEAR(e.process(), 0.5f, 1e-6f);
  // Release from sustain 0.5: level = 0.5 x 0.01^(t / r). Below 0.01 at 0.849 r (within r x 1.2), 0.005 at
  // t = r (1% of the distance), Idle (< 0.001) at 1.349 r.
  e.gate(false); CHECK_EQ((int)e.stage, (int)Adsr::Release);
  n = 0; while (e.level >= 0.01f && n < 100000) { e.process(); n++; }
  CHECK(n < 0.1f * 22050 * 1.2f);
  CHECK(std::abs(n - (int)(2205 * std::log(0.02) / std::log(0.01))) <= 3);    // 1873
  float lvl = e.level; int m = n;
  while (m < 2205) { lvl = e.process(); m++; }       // at t = r
  CHECK_NEAR(lvl, 0.005f, 0.005f * 0.02f);
  while (e.active() && m < 100000) { e.process(); m++; }
  CHECK(std::abs(m - (int)(2205 * std::log(0.002) / std::log(0.01))) <= 3);   // 2976: the 0.001 Idle threshold
  CHECK_EQ(e.level, 0.0f);
  CHECK(!e.active());
}

TEST(adsr_release_reaches_one_percent_at_r) {        // locks in the "1% of the distance in r" reading of the brief
  for (float r : {0.01f, 0.1f, 0.5f, 1.5f}) {
    Adsr e; e.set(0.001f, 0.01f, 1.0f, r); e.gate(true);
    for (int i = 0; i < 100; i++) e.process();
    CHECK_NEAR(e.level, 1.0f, 1e-6f);
    e.gate(false);
    const int rs = (int)(r * 22050.0f + 0.5f);
    for (int i = 0; i < rs; i++) e.process();
    CHECK_NEAR(e.level, 0.01f, 0.01f * 0.03f);        // 1% at t = r, not 0.1% (the -60 dB reading)
    CHECK(e.active());
    int n = rs; while (e.active() && n < 10000000) { e.process(); n++; }
    CHECK_NEAR((float)n / (float)rs, 1.5f, 0.02f);     // freed at 0.001 = -60 dB = 1.5 r from full level
    CHECK(n <= (int)(r * 1.5f * 22050.0f + 0.05f * 22050.0f));   // inside the synth module's 1.5 r + 50 ms bound
    std::printf("  r = %.3f s: 1%% at %d samples, idle at %d (%.3f r)\n", r, rs, n, (float)n / (float)rs);
  }
}

TEST(adsr_release_from_full_within_bound) {
  for (float r : {0.0f, 0.001f, 0.02f, 0.3f, 2.0f}) {
    Adsr e; e.set(0.001f, 0.01f, 1.0f, r); e.gate(true);
    for (int i = 0; i < 2000; i++) e.process();
    CHECK_NEAR(e.level, 1.0f, 1e-6f);
    e.gate(false);
    float rr = r < 0.001f ? 0.001f : r;
    int n = 0; while (e.level >= 0.01f && n < 10000000) { e.process(); n++; }
    CHECK(n < rr * 22050 * 1.2f);
    CHECK(n > rr * 22050 * 0.5f);                      // not absurdly fast either (0.01 is reached at exactly r)
    CHECK(n >= (int)(rr * 22050 * 0.98f));             // ... and never earlier than r
  }
  Adsr e; e.set(0.1f, 0.1f, 0.5f, 0.1f); e.gate(true); e.process(); e.process();
  e.gate(false); CHECK_EQ((int)e.stage, (int)Adsr::Release);   // release from mid attack
  e.kill(); CHECK(!e.active()); CHECK_EQ(e.level, 0.0f);
  e.gate(false); CHECK(!e.active());                   // note-off while idle stays idle
}

TEST(adenv_timing) {
  AdEnv f; f.set(0.01f, 0.1f); f.trigger();
  int n = 0; while (f.rising && n < 10000) { f.process(); n++; }
  CHECK(std::abs(n - 221) <= 1);                       // 0.01 s = 220.5 samples
  CHECK_NEAR(f.level, 1.0f, 1e-6f);
  float v = 1.0f; for (int i = 0; i < 2205; i++) v = f.process();
  CHECK_NEAR(v, 0.001f, 0.001f * 0.02f);               // 60 dB down after d seconds
  AdEnv g; g.set(0.0f, 0.0f); g.trigger();             // 1 ms minimum both ways
  n = 0; while (g.rising && n < 1000) { g.process(); n++; }
  CHECK(n >= 22 && n <= 23);
  for (int i = 0; i < 23; i++) v = g.process();
  CHECK(v < 0.002f);
}

TEST(adenv_decay_floors_at_zero) {                   // no denormals on a held note (desktop build pays for them, SPEC §7)
  AdEnv f; f.set(0.001f, 0.1f); f.trigger();
  float v = 1.0f; for (int i = 0; i < 5 * 22050; i++) v = f.process();
  CHECK_EQ(v, 0.0f);                                 // exactly zero after 5 s, not 2e-43
  CHECK(std::fpclassify(f.level) == FP_ZERO);
  AdEnv g; g.set(0.001f, 0.1f); g.trigger();
  for (int i = 0; i < 23 + 2205; i++) v = g.process();
  CHECK_NEAR(v, 0.001f, 0.001f * 0.03f);             // the floor does not touch the -60 dB point at d
  int n = 0; while (g.level > 0.0f && n < 100000) { g.process(); n++; }
  CHECK(std::abs(n - 2206) <= 2);                    // zero at 1e-6 = -120 dB = 2 d, not before
  AdEnv h; h.set(0.001f, 0.1f); h.trigger(); bool subnormal = false;
  for (int i = 0; i < 5 * 22050; i++) { float x = h.process(); if (x != 0.0f && std::fpclassify(x) != FP_NORMAL) subnormal = true; }
  CHECK(!subnormal);
  h.trigger(); CHECK(h.process() > 0.0f);            // retrigger from the floor rises again
}

// ---------------------------------------------------------------- wavetables

// Amplitude of harmonic k (peak units) of a single-cycle table of `len` samples.
static float harmonicN(const int16_t* t, int len, int k) {
  double re = 0, im = 0;
  for (int n = 0; n < len; n++) {
    double a = 2.0 * M_PI * k * n / len; re += t[n] * std::cos(a); im += t[n] * std::sin(a);
  }
  return (float)(2.0 * std::sqrt(re * re + im * im) / len);
}
static float harmonic(const int16_t* t, int k) { return harmonicN(t, kWaveLen, k); }   // set-0 tables
// Last integer pitch a set serves (SPEC §9.15; set 3 is alias-free up to kWaveSetTopPitch).
static int setTopPitch(int s) { return s + 1 < kWaveSetCount ? kWaveSetMinPitch[s + 1] - 1 : kWaveSetTopPitch; }

TEST(wavetable_peak_and_dc) {
  // Every table of every set peaks at exactly 29000, carries no DC and ends in a guard sample equal to its first
  // (waveSampleN reads t[i + 1] without a wrap mask).
  for (int s = 0; s < kWaveSetCount; s++) {
    const int len = waveSetLen(s);
    CHECK_EQ(len, 1 << kWaveSetShift[s]);
    for (int w = 0; w < WaveCount; w++) {
      const int16_t* t = waveTableSet((Wave)w, s);
      CHECK(t != nullptr);
      CHECK(t == waveTable((Wave)w, kWaveSetMinPitch[s]));   // the pitch lookup lands on the same table
      if (s == 0) CHECK(t == waveTable((Wave)w));             // the one-argument overload is set 0
      int peak = 0; double sum = 0;
      for (int n = 0; n < len; n++) { int v = std::abs((int)t[n]); if (v > peak) peak = v; sum += t[n]; }
      CHECK_EQ(peak, 29000);
      CHECK(std::fabs(sum / len) < 1.0);
      CHECK_EQ(t[len], t[0]);                                  // guard sample
    }
  }
  CHECK(waveTable(WSine) != waveTable(WSaw));
  CHECK(waveTable((Wave)99) == waveTable(WSine));      // out of range falls back to sine
  CHECK(waveTable((Wave)99, 72) == waveTable(WSine, 72));
  CHECK(waveTable((Wave)WaveCount, 100) == waveTable(WSine, 100));
  CHECK(waveTableSet(WSaw, -3) == waveTable(WSaw));     // set index clamped both ways
  CHECK(waveTableSet(WSaw, 99) == waveTableSet(WSaw, kWaveSetCount - 1));
  CHECK_EQ(waveSetLen(-1), kWaveLen); CHECK_EQ(waveSetLen(99), 1 << kWaveSetShift[kWaveSetCount - 1]);
}

TEST(wavetable_sets_by_pitch) {
  // Boundaries (wavetables.h, SPEC §9.15): set 0 up to MIDI 60, set 1 61..76, set 2 77..93, set 3 from 94, any int.
  CHECK_EQ(kWaveSetCount, 4);
  CHECK_EQ(waveSetForPitch(-5), 0); CHECK_EQ(waveSetForPitch(0), 0); CHECK_EQ(waveSetForPitch(59), 0); CHECK_EQ(waveSetForPitch(60), 0);
  CHECK_EQ(waveSetForPitch(61), 1); CHECK_EQ(waveSetForPitch(72), 1); CHECK_EQ(waveSetForPitch(76), 1);
  CHECK_EQ(waveSetForPitch(77), 2); CHECK_EQ(waveSetForPitch(84), 2); CHECK_EQ(waveSetForPitch(93), 2);
  CHECK_EQ(waveSetForPitch(94), 3); CHECK_EQ(waveSetForPitch(127), 3); CHECK_EQ(waveSetForPitch(200), 3);
  CHECK_EQ(kWaveSetHarmonics[0], 40); CHECK_EQ(kWaveSetHarmonics[1], 16); CHECK_EQ(kWaveSetHarmonics[2], 6); CHECK_EQ(kWaveSetHarmonics[3], 3);
  CHECK_EQ(kWaveSetShift[0], 11); CHECK_EQ(kWaveSetShift[1], 9); CHECK_EQ(kWaveSetShift[2], 8); CHECK_EQ(kWaveSetShift[3], 7);
  CHECK_EQ(kWaveSetTopPitch, 105);
  for (int w = 0; w < WaveCount; w++) {
    CHECK(waveTable((Wave)w, 60) == waveTable((Wave)w)); CHECK(waveTable((Wave)w, -1) == waveTable((Wave)w));
    CHECK(waveTable((Wave)w, 61) != waveTable((Wave)w)); CHECK(waveTable((Wave)w, 76) == waveTable((Wave)w, 61));
    CHECK(waveTable((Wave)w, 77) != waveTable((Wave)w, 76)); CHECK(waveTable((Wave)w, 93) == waveTable((Wave)w, 77));
    CHECK(waveTable((Wave)w, 94) != waveTable((Wave)w, 93)); CHECK(waveTable((Wave)w, 127) == waveTable((Wave)w, 94));
  }
  // Harmonic content of every set: the natural series (saw 1/k, square and triangle odd k, fat 1.4/j odd and
  // 1.5/j even in saw-fundamental units) up to the set's cap H and nothing (< -54 dB) above it up to the
  // table's own Nyquist (len / 2); the sine is one partial in every set.
  size_t flashBytes = 0;
  for (int s = 0; s < kWaveSetCount; s++) {
    const int H = kWaveSetHarmonics[s], len = waveSetLen(s);
    flashBytes += (size_t)WaveCount * (len + 1) * sizeof(int16_t);
    const int16_t* sine = waveTableSet(WSine, s); const int16_t* saw = waveTableSet(WSaw, s); const int16_t* sq = waveTableSet(WSquare, s);
    const int16_t* tri = waveTableSet(WTri, s); const int16_t* fat = waveTableSet(WFat, s);
    const float s1 = harmonicN(sine, len, 1), w1 = harmonicN(saw, len, 1), q1 = harmonicN(sq, len, 1);
    const float t1 = harmonicN(tri, len, 1), f1 = harmonicN(fat, len, 1);
    CHECK_NEAR(s1, 29000.f, 2.f);
    CHECK(w1 > 10000.f && q1 > 10000.f && t1 > 10000.f && f1 > 10000.f);
    const int kMax = len / 2 - 1 < 48 ? len / 2 - 1 : 48;
    for (int k = 1; k <= kMax; k++) {
      const bool in = k <= H, odd = (k % 2) == 1;
      if (k > 1) CHECK(harmonicN(sine, len, k) / s1 < 0.002f);
      CHECK_NEAR(harmonicN(saw, len, k) / w1, in ? 1.0 / k : 0.0, 0.002);
      CHECK_NEAR(harmonicN(sq, len, k) / q1, in && odd ? 1.0 / k : 0.0, 0.002);
      CHECK_NEAR(harmonicN(tri, len, k) / t1, in && odd ? 1.0 / ((double)k * k) : 0.0, 0.002);
      CHECK_NEAR(harmonicN(fat, len, k) / f1, in ? (odd ? 1.4 / k : 1.5 / k) / 1.4 : 0.0, 0.003);
    }
    CHECK(len >= 32 * H);                                // >= 32 samples per period of the top partial (images < -74 dB)
    // Every set is alias-free up to half a semitone above the last integer pitch it serves (the voice rounds a
    // glide pitch to the nearest integer), and one pitch higher it WOULD fold: the boundaries sit at the limits.
    const int top = setTopPitch(s);
    const double topHz = H * midiToHz((float)top + 0.49f), nextHz = H * midiToHz((float)(top + 1));
    std::printf("  set %d: %2d partials, %4d samples, MIDI %3d..%3d; top partial at MIDI %d.49 = %5.0f Hz, at %d = %5.0f Hz\n",
                s, H, len, kWaveSetMinPitch[s], top, top, topHz, top + 1, nextHz);
    CHECK(topHz < 11025.0);
    if (s + 1 < kWaveSetCount) CHECK(nextHz > 11025.0);
  }
  std::printf("  tables: %zu B of const data (flash)\n", flashBytes);
  CHECK_EQ((int)flashBytes, 29480);
  // Shapes of the reduced sets agree where it matters: peak at a quarter cycle for the triangle, a positive
  // first half and mirrored second half for the square, a falling ramp through the middle for the saw.
  for (int s = 1; s < kWaveSetCount; s++) {
    const int len = waveSetLen(s);
    const int16_t* tri = waveTableSet(WTri, s); const int16_t* sq = waveTableSet(WSquare, s); const int16_t* saw = waveTableSet(WSaw, s);
    CHECK_EQ(tri[len / 4], 29000); CHECK_EQ(tri[0], 0); CHECK_EQ(tri[len / 2], 0);
    bool squareOk = sq[len / 4] > 18000; for (int n = 1; n < len / 2; n++) if (sq[n] <= 0 || sq[n] != -sq[n + len / 2]) squareOk = false;
    CHECK(squareOk);
    // (a Gibbs ripple of the short series may rise a few counts per sample; the ramp itself falls by hundreds)
    int maxRise = -100000; for (int n = 3 * len / 8; n < 5 * len / 8; n++) maxRise = std::max(maxRise, saw[n + 1] - saw[n]);
    CHECK(maxRise <= 60);
    CHECK(saw[3 * len / 8] - saw[5 * len / 8] > 10000);
    CHECK(saw[len / 4] > 8000 && saw[3 * len / 4] < -8000 && std::abs(saw[len / 2]) < 60);
  }
}

TEST(wavetable_harmonic_content) {
  const int16_t* sine = waveTable(WSine); const int16_t* tri = waveTable(WTri);
  const int16_t* saw = waveTable(WSaw); const int16_t* sq = waveTable(WSquare); const int16_t* fat = waveTable(WFat);
  float s1 = harmonic(sine, 1);
  CHECK_NEAR(s1, 29000.0f, 2.0f);
  CHECK(harmonic(sine, 2) < 2.0f && harmonic(sine, 3) < 2.0f);
  // Natural amplitudes: saw 1/k all k, square 1/k odd k, triangle 1/k^2 odd k.
  float w1 = harmonic(saw, 1);
  CHECK_NEAR(harmonic(saw, 2) / w1, 0.5f, 0.002f);
  CHECK_NEAR(harmonic(saw, 3) / w1, 1.0f / 3.0f, 0.002f);
  CHECK_NEAR(harmonic(saw, 40) / w1, 1.0f / 40.0f, 0.002f);
  CHECK(harmonic(saw, 41) / w1 < 0.001f);              // band-limited at 40
  CHECK(harmonic(saw, 64) / w1 < 0.001f);
  float q1 = harmonic(sq, 1);
  CHECK(harmonic(sq, 2) / q1 < 0.001f);                // no even harmonics
  CHECK_NEAR(harmonic(sq, 3) / q1, 1.0f / 3.0f, 0.002f);
  CHECK_NEAR(harmonic(sq, 39) / q1, 1.0f / 39.0f, 0.002f);
  CHECK(harmonic(sq, 41) / q1 < 0.001f);
  float t1 = harmonic(tri, 1);
  CHECK(harmonic(tri, 2) / t1 < 0.001f);
  CHECK_NEAR(harmonic(tri, 3) / t1, 1.0f / 9.0f, 0.002f);
  CHECK_NEAR(harmonic(tri, 5) / t1, 1.0f / 25.0f, 0.002f);
  // Ordering: saw has more 2nd harmonic than square (which has none); fat has even harmonics too.
  CHECK(harmonic(saw, 2) / w1 > harmonic(sq, 2) / q1 + 0.4f);
  float f1 = harmonic(fat, 1);
  CHECK(f1 > 10000.0f);
  CHECK(harmonic(fat, 41) / f1 < 0.001f);
  // WFat = saw + 0.4 square + 0.25 saw2 as raw natural series (no per-part normalisation, one final
  // normalisation), saw2 = saw(2 theta) with k <= 20. In units of the saw's fundamental harmonic j is
  // 1/j + 0.4/j (odd j) = 1.4/j, or 1/j + 0.25 x 2/j (even j <= 40) = 1.5/j.
  auto expected = [](int j) { return j > 40 ? 0.0 : (j % 2 ? 1.4 / j : 1.5 / j); };
  for (int j = 1; j <= 41; j++) CHECK_NEAR(harmonic(fat, j) / f1, expected(j) / expected(1), 0.003);
  CHECK_NEAR(harmonic(fat, 2) / f1, 0.75 / 1.4, 0.003);           // 0.536: the brief's mix (per-part normalising gave 0.423)
  CHECK_NEAR(harmonic(fat, 3) / f1, 1.0 / 3.0, 0.003);
  // Against the saw table: odd harmonics identical in relative terms, even ones 1.5 / 1.4 higher. With the square
  // weighted 0.78 (the old per-part normalisation) the even ratio would be 1.5 / 1.78 = 0.84 instead.
  for (int j = 1; j <= 40; j++) CHECK_NEAR((harmonic(fat, j) / f1) / (harmonic(saw, j) / w1), j % 2 ? 1.0 : 1.5 / 1.4, 0.01);
  std::printf("  fat h2/h1 = %.3f, h3/h1 = %.3f\n", harmonic(fat, 2) / f1, harmonic(fat, 3) / f1);
  // Shapes: sine and triangle peak at a quarter cycle, square is flat over the half cycle (Gibbs ripple
  // aside), saw is a falling ramp with its jump at theta = 0 smeared over ~N/80 samples.
  CHECK_EQ(sine[kWaveLen / 4], 29000); CHECK_EQ(sine[0], 0); CHECK_EQ(sine[3 * kWaveLen / 4], -29000);
  CHECK_EQ(tri[kWaveLen / 4], 29000); CHECK_EQ(tri[0], 0); CHECK_EQ(tri[kWaveLen / 2], 0);
  CHECK(std::abs(tri[kWaveLen / 8] - 14500) < 250);   // 40-term series: 0.505 of peak at an eighth cycle
  int flat = 0; for (int n = 0; n < kWaveLen / 2; n++) if (sq[n] > 29000 * 0.8f) flat++;
  CHECK(flat > kWaveLen / 2 * 0.9f);
  CHECK(sq[1] > 0 && sq[kWaveLen - 1] < 0);
  int peakAt = 0, troughAt = 0; for (int n = 0; n < kWaveLen; n++) { if (saw[n] == 29000) peakAt = n; if (saw[n] == -29000) troughAt = n; }
  CHECK(peakAt > 0 && peakAt < 64 && troughAt > kWaveLen - 64);
  bool falling = true; for (int n = kWaveLen / 4; n < 3 * kWaveLen / 4; n++) if (saw[n + 1] > saw[n] + 40) falling = false;   // ripple of the 40-term series is < +10 per step here
  CHECK(falling);
  CHECK(saw[kWaveLen / 4] > 10000 && saw[3 * kWaveLen / 4] < -10000);
  CHECK(std::abs(saw[kWaveLen / 2]) < 60);
}

TEST(wave_sample_interpolation) {
  const int16_t* t = waveTable(WSaw);
  CHECK_EQ(waveSample(t, 0u), t[0]);
  CHECK_EQ(waveSample(t, 1u << 31), t[kWaveLen / 2]);
  CHECK_EQ(waveSample(t, (uint32_t)(kWaveLen - 1) << (32 - kWaveShift)), t[kWaveLen - 1]);
  const uint32_t step = 1u << (32 - kWaveShift);
  int mid = (t[0] + t[1]) / 2;
  CHECK(std::abs((int)waveSample(t, step / 2) - mid) <= 1);
  int q = t[100] + (t[101] - t[100]) / 4;
  CHECK(std::abs((int)waveSample(t, 100 * step + step / 4) - q) <= 1);
  // Wrap: a phase just below 2^32 interpolates between the last and the first sample.
  int wrapMid = (t[kWaveLen - 1] + t[0]) / 2;
  CHECK(std::abs((int)waveSample(t, (uint32_t)(kWaveLen - 1) * step + step / 2) - wrapMid) <= 1);
  // Sine via the phase accumulator at 1000 Hz: 22050 samples give 1000 cycles, interpolation keeps it clean.
  const int16_t* s = waveTable(WSine); uint32_t ph = 0, inc = phaseIncForHz(1000.0f);
  double err = 0; for (int i = 0; i < 22050; i++) { double ref = 29000.0 * std::sin(2.0 * M_PI * 1000.0 * i / 22050.0); err += std::fabs(waveSample(s, ph) - ref); ph += inc; }
  CHECK(err / 22050 < 12.0);                           // < 0.05% of full scale on average
  // waveSampleN (SPEC §9.15): the same interpolation from a table of 2^shift samples; the guard sample serves
  // the last interval, so a phase just below 2^32 interpolates between the last sample and sample 0.
  for (int set = 0; set < kWaveSetCount; set++) {
    const int sh = kWaveSetShift[set], len = waveSetLen(set); const int16_t* tt = waveTableSet(WSaw, set);
    const uint32_t stp = 1u << (32 - sh);
    CHECK_EQ(waveSampleN(tt, 0u, sh), tt[0]);
    CHECK_EQ(waveSampleN(tt, 1u << 31, sh), tt[len / 2]);
    CHECK_EQ(waveSampleN(tt, (uint32_t)(len - 1) * stp, sh), tt[len - 1]);
    CHECK(std::abs((int)waveSampleN(tt, 7 * stp + stp / 4, sh) - (tt[7] + (tt[8] - tt[7]) / 4)) <= 1);
    CHECK(std::abs((int)waveSampleN(tt, (uint32_t)(len - 1) * stp + stp / 2, sh) - (tt[len - 1] + tt[0]) / 2) <= 1);
    // Against the double-precision reference at 2000 pseudo-random phases: the 8-bit fraction truncates by at
    // most one step of 1/256 of the interval, plus the integer rounding.
    int maxDiff = 0; for (int n = 0; n < len; n++) maxDiff = std::max(maxDiff, std::abs(tt[n + 1] - tt[n]));
    uint32_t rng = 12345u + (uint32_t)set; double maxErr = 0;
    for (int i = 0; i < 2000; i++) {
      rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
      const double pos = rng / 4294967296.0 * len; const int i0 = (int)pos; const double fr = pos - i0;
      const double ref = tt[i0] + (tt[i0 + 1] - tt[i0]) * fr;
      const double e = std::fabs(waveSampleN(tt, rng, sh) - ref); if (e > maxErr) maxErr = e;
    }
    CHECK(maxErr <= maxDiff / 256.0 + 1.0);
  }
  // On a set-0 table waveSampleN is waveSample, bit for bit.
  bool same = true; for (uint32_t p = 0; p < 0xFFFFFF00u; p += 0x01234567u) if (waveSampleN(t, p, kWaveShift) != waveSample(t, p)) same = false;
  CHECK(same);
}

TEST(phase_inc_and_midi) {
  CHECK_EQ(phaseIncForHz(22050.0f / 2048.0f), 1u << 21);           // one table step per sample
  uint32_t a4 = phaseIncForHz(440.0f);
  CHECK_NEAR((double)a4, 440.0 / 22050.0 * 4294967296.0, 64.0);
  CHECK_EQ(phaseIncForHz(0.0f), 0u);
  CHECK_EQ(phaseIncForHz(-5.0f), 0u);
  CHECK_EQ(phaseIncForHz(1e9f), 1u << 31);                          // clamped at Nyquist
  CHECK_EQ(phaseIncForHz(11025.0f), 1u << 31);
  CHECK_NEAR(midiToHz(69.0f), 440.0f, 1e-3f);
  CHECK_NEAR(midiToHz(81.0f), 880.0f, 1e-2f);
  CHECK_NEAR(midiToHz(57.0f), 220.0f, 1e-2f);
  CHECK_NEAR(midiToHz(60.0f), 261.6256f, 1e-2f);
  CHECK_NEAR(midiToHz(69.5f), 452.893f, 2e-2f);
  CHECK_NEAR(midiToHz(0.0f), 8.1758f, 1e-3f);
  CHECK_NEAR(midiToHz(127.0f), 12543.85f, 0.5f);
  CHECK_NEAR(midiToHz(70.0f) / midiToHz(69.0f), 1.0594631f, 1e-5f);
}

// Hann-windowed magnitude of x at an arbitrary frequency (relative units). With 1 s of signal the window's
// leakage from a component >= 70 Hz away is below -90 dB, so partials and their aliases are measured apart.
static double toneLevel(const std::vector<float>& x, double hz) {
  double re = 0, im = 0; const size_t n = x.size();
  for (size_t i = 0; i < n; i++) {
    double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)n), a = 2.0 * M_PI * hz * (double)i / 22050.0;
    re += x[i] * w * std::cos(a); im += x[i] * w * std::sin(a);
  }
  return std::sqrt(re * re + im * im);
}
// The synth's oscillator path (phaseIncForHz + waveSampleN) on the table set for the pitch, or on a given set.
static std::vector<float> renderWave(Wave w, float midi, int n, int set = -1) {
  if (set < 0) set = waveSetForPitch((int)midi);
  const int16_t* t = waveTableSet(w, set); const int sh = kWaveSetShift[set];
  uint32_t ph = 0, inc = phaseIncForHz(midiToHz(midi));
  std::vector<float> out(n); for (int i = 0; i < n; i++) { out[i] = waveSampleN(t, ph, sh) / 29000.0f; ph += inc; }
  return out;
}
// Where a component at hz lands after sampling at 22050 Hz (folded as often as needed).
static double foldHz(double hz) { double r = std::fmod(hz, 22050.0); return r > 11025.0 ? 22050.0 - r : r; }
// Folded energy (re fundamental, dB) of a saw with H partials at a pitch: the partials above Nyquist, 1/k each.
static double predictedFoldDb(int H, int midi) {
  double e = 0, hz = midiToHz((float)midi);
  for (int k = 2; k <= H; k++) if (k * hz > 11025.0) e += 1.0 / ((double)k * k);
  return e > 0 ? 10 * std::log10(e) : -200.0;
}
// Measured folded energy (dB re fundamental) of a rendered saw at f0: the sum over the bins where the partials
// 2..H above Nyquist would land. Bins within 8 Hz of a harmonic m f0 are skipped (their reading would be the
// harmonic's). Returns -200 when no partial folds.
static double measuredFoldDb(const std::vector<float>& x, double f0, int H) {
  const double fund = toneLevel(x, f0); double e = 0; bool any = false;
  for (int k = 2; k <= H; k++) {
    if (k * f0 <= 11025.0) continue;
    const double fa = foldHz(k * f0); bool nearHarmonic = false;
    for (int m = 1; m <= 40; m++) if (std::fabs(fa - m * f0) < 8.0) nearHarmonic = true;
    if (nearHarmonic || fa < 20.0) continue;
    const double lv = toneLevel(x, fa) / fund; e += lv * lv; any = true;
  }
  return any ? 10 * std::log10(e > 1e-30 ? e : 1e-30) : -200.0;
}

// A 40-harmonic table is alias-free only up to MIDI 60 (275.6 Hz x 40 = Nyquist at MIDI 60.9): with the former
// single set a saw at C5 folded partials 22..40 back into the band (-16.6 dB of inharmonic energy, 40 f0 landing
// at 1120 Hz). The voice now takes the set of the pitch (waveTable(w, midi), SPEC §9.15). This test pins the
// oscillator path on set 0 (nothing beyond the table's own content), checks the pitch-ranged sets at C5 (set 1),
// D6 (set 2) and C7 (set 3), then renders every boundary of the sets and a sweep of the whole keyboard.
TEST(wavetable_aliasing_above_midi_61) {
  const int n = 22050;
  // MIDI 60 (261.6 Hz) on set 0: all 40 partials sit below Nyquist (40 f0 = 10.47 kHz); nothing between them.
  std::vector<float> c4 = renderWave(WSaw, 60.0f, n, 0); double f0 = midiToHz(60.0f);
  double fund = toneLevel(c4, f0);
  CHECK_NEAR(toneLevel(c4, 2 * f0) / fund, 0.5, 0.01);
  CHECK_NEAR(toneLevel(c4, 40 * f0) / fund, 1.0 / 40, 0.0025);
  double spur = 0; for (double m : {1.5, 5.5, 10.5, 20.5, 30.5, 39.5}) { double lv = toneLevel(c4, m * f0) / fund; if (lv > spur) spur = lv; }
  CHECK(spur < 0.001);                               // < -60 dB between partials (linear interpolation images)
  // MIDI 72 (523.3 Hz) on set 0, the former behaviour: partials 22..40 (11.5..20.9 kHz) fold to 22050 - k f0
  // at their full 1/k amplitude (40 lands at 1120 Hz, 39 at 1643 Hz). The oscillator adds nothing of its own.
  std::vector<float> old = renderWave(WSaw, 72.0f, n, 0); f0 = midiToHz(72.0f);
  fund = toneLevel(old, f0);
  double aliasedOld = 0, predicted = 0;
  for (int k = 22; k <= 40; k++) {
    double lv = toneLevel(old, 22050.0 - k * f0) / fund;
    CHECK_NEAR(lv, 1.0 / k, 0.1 / k);
    aliasedOld += lv * lv; predicted += 1.0 / ((double)k * k);
  }
  const double oldDb = 10 * std::log10(aliasedOld);
  CHECK_NEAR(oldDb, 10 * std::log10(predicted), 0.5);
  CHECK_NEAR(oldDb, predictedFoldDb(40, 72), 0.5);
  CHECK_NEAR(measuredFoldDb(old, f0, 40), oldDb, 0.5);   // the helper used below agrees with the explicit sum
  // The same note on the set the voice now picks (set 1, 16 partials, 16 f0 = 8.4 kHz): partials 2..16 at 1/k,
  // nothing at 17..21 (which would still be below Nyquist) and every former alias bin down at the interpolation
  // floor: the folded energy is at most -40 dB (acceptance), > 25 dB better than the single set.
  std::vector<float> c5 = renderWave(WSaw, 72.0f, n);
  fund = toneLevel(c5, f0);
  for (int k = 2; k <= 16; k++) CHECK_NEAR(toneLevel(c5, k * f0) / fund, 1.0 / k, 0.02 / k);
  for (int k = 17; k <= 21; k++) CHECK(toneLevel(c5, k * f0) / fund < 0.001);
  double aliased = 0, worst = 0, lowBand = 0;
  for (int k = 22; k <= 40; k++) {
    double fa = 22050.0 - k * f0; double lv = toneLevel(c5, fa) / fund;
    aliased += lv * lv; if (lv > worst) worst = lv; if (fa < 2000.0) lowBand += lv * lv;
  }
  const double aliasDb = 10 * std::log10(aliased);
  std::printf("  saw at MIDI 72: folded energy %.1f dB re fundamental with set 1 (worst bin %.1f dB), was %.1f dB with the single set\n",
              aliasDb, 20 * std::log10(worst), oldDb);
  CHECK(aliasDb <= -40.0);
  CHECK(worst < 0.003);                              // every former alias bin below -50 dB
  CHECK(lowBand < 1e-5);                             // the inharmonic tones at 1120 / 1643 Hz are gone (were -31 dB each)
  CHECK(aliasDb < oldDb - 25.0);
  // MIDI 90 (1174.7 Hz) on set 2 (6 partials, 6 f0 = 7.0 kHz): the six partials at 1/k, nothing folded.
  std::vector<float> d6 = renderWave(WSaw, 90.0f, n); f0 = midiToHz(90.0f); fund = toneLevel(d6, f0);
  CHECK_EQ(waveSetForPitch(90), 2);
  for (int k = 2; k <= 6; k++) CHECK_NEAR(toneLevel(d6, k * f0) / fund, 1.0 / k, 0.02 / k);
  for (int k = 7; k <= 9; k++) CHECK(toneLevel(d6, k * f0) / fund < 0.001);
  CHECK(measuredFoldDb(d6, f0, 40) <= -40.0);
  // MIDI 96 (2093 Hz, C7) on set 3 (3 partials, 3 f0 = 6.3 kHz): with 6 partials the sixth would land at
  // 9492 Hz (-15.6 dB); now partials 2 and 3 at 1/k and that bin empty.
  std::vector<float> c7 = renderWave(WSaw, 96.0f, n); f0 = midiToHz(96.0f); fund = toneLevel(c7, f0);
  CHECK_EQ(waveSetForPitch(96), 3);
  CHECK_NEAR(toneLevel(c7, 2 * f0) / fund, 0.5, 0.01); CHECK_NEAR(toneLevel(c7, 3 * f0) / fund, 1.0 / 3, 0.01);
  CHECK(toneLevel(c7, 22050.0 - 6 * f0) / fund < 0.001);
  CHECK_NEAR(20 * std::log10(1.0 / 6), predictedFoldDb(6, 96), 0.01);   // what set 2 would have folded here
  // Set boundaries, rendered (SPEC §9.15): at the first pitch of each set the richer set below folds exactly its
  // top partial (set 0 at 61: partial 40 -> 10963 Hz, -32 dB; set 1 at 77: partial 16 -> 10875 Hz, -24 dB;
  // set 2 at 94: partial 6 -> 10862 Hz, -15.6 dB) within 0.5 dB of the model, while the set the voice picks is
  // at the floor; at the last pitch of each set that set is still clean.
  for (int s = 1; s < kWaveSetCount; s++) {
    const int lo = kWaveSetMinPitch[s];
    const double fLo = midiToHz((float)lo);
    std::vector<float> below = renderWave(WSaw, (float)lo, n, s - 1);
    const double foldBelow = measuredFoldDb(below, fLo, kWaveSetHarmonics[s - 1]);
    CHECK_NEAR(foldBelow, predictedFoldDb(kWaveSetHarmonics[s - 1], lo), 0.5);
    CHECK(foldBelow > -35.0);
    std::vector<float> own = renderWave(WSaw, (float)lo, n);
    const double foldOwn = measuredFoldDb(own, fLo, 40);
    const int top = setTopPitch(s - 1);
    std::vector<float> last = renderWave(WSaw, (float)top, n);
    const double foldLast = measuredFoldDb(last, midiToHz((float)top), 40);
    std::printf("  boundary %d: MIDI %d on set %d folds %.1f dB (model %.1f), on set %d %.1f dB; MIDI %d on set %d %.1f dB\n",
                s, lo, s - 1, foldBelow, predictedFoldDb(kWaveSetHarmonics[s - 1], lo), s, foldOwn, top, s - 1, foldLast);
    CHECK(foldOwn <= -40.0);
    CHECK(foldLast <= -40.0);
    for (int k = 2; k <= kWaveSetHarmonics[s]; k++) CHECK_NEAR(toneLevel(own, k * fLo) / toneLevel(own, fLo), 1.0 / k, 0.02 / k);
  }
  // Whole keyboard, rendered: a saw at every pitch 24..105 on the set the voice picks, 1/4 s each; the folded
  // energy in the bins where a 40-partial table would have landed stays at the interpolation floor (<= -40 dB).
  double worstDb = -200; int worstPitch = -1; double worstSingle = -200;
  for (int m = 24; m <= kWaveSetTopPitch; m++) {
    std::vector<float> x = renderWave(WSaw, (float)m, n / 4);
    const double db = measuredFoldDb(x, midiToHz((float)m), 40);
    if (db > worstDb) { worstDb = db; worstPitch = m; }
    const double single = predictedFoldDb(40, m); if (single > worstSingle) worstSingle = single;
  }
  std::printf("  keyboard MIDI 24..%d: worst folded energy %.1f dB (at MIDI %d); the single 40-partial set reached %.1f dB\n",
              kWaveSetTopPitch, worstDb, worstPitch, worstSingle);
  CHECK(worstDb <= -40.0);
  CHECK(worstSingle > -6.5);                         // the single set folded 38 of 40 partials by MIDI 105 ...
  CHECK_NEAR(predictedFoldDb(40, 108), -4.3, 0.2);   // ... and -4.3 dB at C8 (sum 1/k^2, k = 3..40)
  // The sine table never aliases: at MIDI 108 (4186 Hz) there is only the fundamental.
  std::vector<float> hi = renderWave(WSine, 108.0f, n); f0 = midiToHz(108.0f); fund = toneLevel(hi, f0);
  CHECK(toneLevel(hi, 22050.0 - 2 * f0) / fund < 0.001 && toneLevel(hi, 2 * f0) / fund < 0.001);
}

// ---------------------------------------------------------------- sample voice

static std::vector<uint8_t> encodeSlot(const std::vector<int16_t>& pcm) {
  std::vector<uint8_t> u(pcm.size()); for (size_t i = 0; i < pcm.size(); i++) u[i] = ulawEncode(pcm[i]); return u;
}
// Renders until the voice goes idle (block size blk); returns the output. Stops at a safety limit.
static std::vector<float> renderAll(SampleVoice& v, int blk, int limit = 200000) {
  std::vector<float> out;
  while (v.active() && (int)out.size() < limit) {
    std::vector<float> b(blk, 0.0f); v.render(b.data(), blk);
    out.insert(out.end(), b.begin(), b.end());
  }
  while (!out.empty() && out.back() == 0.0f) out.pop_back();   // trailing zeros of the last block
  return out;
}

TEST(sample_voice_plays_slot_once_with_fades) {
  ulawInit();
  std::vector<int16_t> pcm(1000, 16000); std::vector<uint8_t> u = encodeSlot(pcm);
  const float v = ulawDecode(u[0]) / 32767.0f;      // 16124 / 32767
  SampleVoice sv; CHECK(!sv.active());
  sv.trigger(u.data(), (uint32_t)u.size(), 1.0f, 1.0f); CHECK(sv.active());
  std::vector<float> out = renderAll(sv, 64);
  CHECK(!sv.active());
  CHECK(std::abs((int)out.size() - 1000) <= 2);     // whole slot at rate 1 (999 interpolable positions)
  std::printf("  rate 1: %zu samples\n", out.size());
  CHECK_NEAR(out[0], 0.0f, 1e-6f);                  // 2 ms linear attack from zero
  CHECK_NEAR(out[22], v * 22.0f / 44.1f, 0.002f);
  CHECK_NEAR(out[44], v * 44.0f / 44.1f, 0.002f);
  CHECK_NEAR(out[45], v, 1e-5f);                    // attack done at 44.1 samples
  for (int i = 46; i < 700; i++) CHECK_NEAR(out[i], v, 1e-5f);
  CHECK_NEAR(out[866], v * 0.5f, 0.01f);            // 12 ms linear fade ends at the slot end
  CHECK(out.back() < v * 0.02f);
  for (size_t i = 740; i + 1 < out.size(); i++) CHECK(out[i + 1] <= out[i] + 1e-7f);   // monotonic fade
  for (float s : out) CHECK(s >= 0.0f && s <= v + 1e-5f);
}

TEST(sample_voice_rate_and_gain) {
  ulawInit();
  std::vector<int16_t> pcm(1000, -12000); std::vector<uint8_t> u = encodeSlot(pcm);
  const float v = ulawDecode(u[0]) / 32767.0f;
  SampleVoice sv; sv.trigger(u.data(), 1000, 0.5f, 2.0f);
  std::vector<float> out = renderAll(sv, 256);
  CHECK(std::abs((int)out.size() - 500) <= 2);      // half the samples at rate 2
  std::printf("  rate 2: %zu samples\n", out.size());
  for (int i = 46; i < 230; i++) CHECK_NEAR(out[i], v * 0.5f, 1e-5f);   // gain in linear units
  CHECK(std::fabs(out.back()) < std::fabs(v) * 0.02f);
  sv.trigger(u.data(), 1000, 1.0f, 0.5f);           // rate 0.5 doubles the length
  out = renderAll(sv, 256);
  CHECK(std::abs((int)out.size() - 2000) <= 2);
  sv.trigger(u.data(), 1000, 1.0f, 0.0f);           // rate is clamped, never stalls
  out = renderAll(sv, 256, 100000);
  CHECK(!sv.active() && out.size() < 70000u);
}

TEST(sample_voice_release_choke) {
  ulawInit();
  std::vector<int16_t> pcm(5000, 20000); std::vector<uint8_t> u = encodeSlot(pcm);
  const float v = ulawDecode(u[0]) / 32767.0f;
  SampleVoice sv; sv.trigger(u.data(), 5000, 1.0f, 1.0f);
  std::vector<float> a(200, 0.0f); sv.render(a.data(), 200);
  CHECK_NEAR(a[199], v, 1e-5f);
  sv.release();                                     // choke: 12 ms = 265 output samples
  std::vector<float> out = renderAll(sv, 50);
  CHECK(!sv.active());
  CHECK_EQ((int)out.size(), 265);
  std::printf("  choke: %zu samples after release()\n", out.size());
  CHECK_NEAR(out[0], v, v * 0.005f);
  CHECK_NEAR(out[132], v * 0.5f, 0.01f);
  for (size_t i = 0; i + 1 < out.size(); i++) CHECK(out[i + 1] < out[i]);
  sv.release(); CHECK(!sv.active());                // release on an idle voice is harmless
  sv.trigger(u.data(), 5000, 1.0f, 1.0f); sv.release(); sv.release();   // double release: one fade
  out = renderAll(sv, 50);
  CHECK(out.size() <= 2u);                          // env was still 0: fades out at once
  sv.trigger(u.data(), 5000, 1.0f, 1.0f);           // release mid attack keeps going down from there
  a.assign(20, 0.0f); sv.render(a.data(), 20); sv.release();
  out = renderAll(sv, 50);
  CHECK(out.size() < 130u && out.size() > 100u);    // env 20/44.1 = 0.45 → 0 at 1/264.6 per sample
}

TEST(sample_voice_linear_interpolation) {
  ulawInit();
  std::vector<int16_t> pcm(2000); for (int i = 0; i < 2000; i++) pcm[i] = (int16_t)(i * 16 - 16000);
  std::vector<uint8_t> u = encodeSlot(pcm);
  SampleVoice sv; sv.trigger(u.data(), 2000, 1.0f, 0.5f);
  std::vector<float> out = renderAll(sv, 128);
  CHECK(std::abs((int)out.size() - 4000) <= 2);
  for (int k = 100; k < 1500; k++) {                // after the attack, before the fade
    int s0 = ulawDecode(u[k]), s1 = ulawDecode(u[k + 1]);
    int mid = s0 + (((s1 - s0) * 32768) >> 16);     // 16.16 lerp exactly as the probe benchmark
    CHECK_NEAR(out[2 * k] * 32767.0f, (float)s0, 0.01f);
    CHECK_NEAR(out[2 * k + 1] * 32767.0f, (float)mid, 0.01f);
  }
  // Rate 1.25: position 1.25 k, fraction cycles 0 .25 .5 .75.
  sv.trigger(u.data(), 2000, 1.0f, 1.25f);
  out = renderAll(sv, 128);
  for (int i = 100; i < 1000; i++) {
    uint32_t pos = (uint32_t)i * (uint32_t)(1.25f * 65536.0f); int idx = pos >> 16; int fr = pos & 0xFFFF;
    int s0 = ulawDecode(u[idx]), s1 = ulawDecode(u[idx + 1]);
    CHECK_NEAR(out[i] * 32767.0f, (float)(s0 + (((s1 - s0) * fr) >> 16)), 0.01f);
  }
}

TEST(sample_voice_accumulates_and_guards) {
  ulawInit();
  std::vector<int16_t> pcm(3000, 8000); std::vector<uint8_t> u = encodeSlot(pcm);
  const float v = ulawDecode(u[0]) / 32767.0f;
  SampleVoice a, b; a.trigger(u.data(), 3000, 1.0f); b.trigger(u.data(), 3000, 0.25f);
  std::vector<float> acc(256, 0.5f);
  a.render(acc.data(), 256); b.render(acc.data(), 256);  // accumulate: two voices on top of a 0.5 bias
  CHECK_NEAR(acc[200], 0.5f + v + 0.25f * v, 1e-5f);
  std::vector<int16_t> full(3000, 32767); std::vector<uint8_t> uf = encodeSlot(full);
  SampleVoice c; c.trigger(uf.data(), 3000, 1.0f);
  std::vector<float> out = renderAll(c, 256);
  float peak = 0; for (float s : out) if (s > peak) peak = s;
  CHECK(peak <= 1.0f && peak > 0.97f);               // int16 full scale maps to about 1.0
  SampleVoice d; d.trigger(nullptr, 100, 1.0f); CHECK(!d.active());   // bad slot: stays idle
  d.trigger(u.data(), 1, 1.0f); CHECK(!d.active());
  d.trigger(u.data(), 0, 1.0f); CHECK(!d.active());
  std::vector<float> z(16, 0.0f); d.render(z.data(), 16);              // idle render adds nothing
  for (float s : z) CHECK_EQ(s, 0.0f);
  SampleVoice e; e.trigger(u.data(), 200, 1.0f);     // slot shorter than attack + release still sounds
  out = renderAll(e, 16);
  CHECK(out.size() >= 150u && out.size() <= 200u);
  float pk = 0; for (float s : out) if (s > pk) pk = s;
  CHECK_NEAR(pk, v, 1e-5f);
  CHECK(out.back() < v * 0.05f);
  SampleVoice f; f.trigger(u.data(), 3000, 1.0f); f.render(z.data(), 16);
  f.trigger(u.data(), 3000, 1.0f, 1.0f);             // retrigger restarts from the beginning
  z.assign(16, 0.0f); f.render(z.data(), 16);
  CHECK_NEAR(z[0], 0.0f, 1e-6f);
  CHECK(z[15] > z[0]);
}

TEST(sample_voice_release_on_short_slot_fades_to_end) {   // choke during the attack of a 100-sample (4.5 ms) slot
  ulawInit();
  std::vector<int16_t> pcm(100, 20000); std::vector<uint8_t> u = encodeSlot(pcm);
  const float v = ulawDecode(u[0]) / 32767.0f;
  SampleVoice sv; sv.trigger(u.data(), 100, 1.0f, 1.0f);
  std::vector<float> a(44, 0.0f); sv.render(a.data(), 44);
  CHECK_NEAR(a[43], v * 43.0f / 44.1f, 0.002f);
  sv.release();                                        // the 12 ms slope needs 264 samples; 55 positions remain
  std::vector<float> out = renderAll(sv, 16);
  CHECK(!sv.active());
  CHECK_EQ((int)out.size(), 55);                       // fades over the remaining data instead of a hard cut
  CHECK(out.back() < v * 0.02f);                       // last non-zero output < 2% of level (was 79%)
  CHECK(out.back() > 0.0f);
  for (size_t i = 0; i + 1 < out.size(); i++) CHECK(out[i + 1] < out[i]);
  CHECK_NEAR(out[0], v * 44.0f / 44.1f, 0.002f);
  CHECK_NEAR(out[27], v * 0.5f, v * 0.03f);           // linear: half way at half the remaining length
  std::printf("  100-sample slot, release() at 44: %zu more samples, last = %.1f%% of level\n", out.size(), 100.0f * out.back() / v);
  // At rate 2 the same slot has 30 positions left after 20 outputs; release() still lands on the end.
  sv.trigger(u.data(), 100, 1.0f, 2.0f); a.assign(20, 0.0f); sv.render(a.data(), 20); sv.release();
  out = renderAll(sv, 16);
  CHECK_EQ((int)out.size(), 30);
  CHECK(out.back() < v * 0.02f);
  // A longer slot keeps the plain 12 ms slope (release() from the attack: env 44/44.1 -> 0 at 1/264.6 per sample).
  std::vector<int16_t> pcm2(2000, 20000); std::vector<uint8_t> u2 = encodeSlot(pcm2);
  sv.trigger(u2.data(), 2000, 1.0f, 1.0f); a.assign(44, 0.0f); sv.render(a.data(), 44); sv.release();
  out = renderAll(sv, 16);
  CHECK(std::abs((int)out.size() - 264) <= 1);
}

TEST(sample_voice_release_fade_samples) {            // SPEC §9.1: 64-sample fade for slot swap / retrigger / choke
  ulawInit();
  CHECK_EQ(kSampleReleaseSamples, 265);
  std::vector<int16_t> pcm(5000, 20000); std::vector<uint8_t> u = encodeSlot(pcm);
  const float v = ulawDecode(u[0]) / 32767.0f;
  SampleVoice sv; sv.trigger(u.data(), 5000, 1.0f, 1.0f);
  std::vector<float> a(200, 0.0f); sv.render(a.data(), 200);
  sv.release(kSlotFadeSamples);
  std::vector<float> b(kBlock, 0.0f); sv.render(b.data(), kBlock);   // one engine block
  CHECK(!sv.active());                                 // idle before the next block: the staging copy cannot be heard
  int nz = 0; while (nz < kBlock && b[nz] != 0.0f) nz++;
  CHECK_EQ(nz, 64);                                    // exactly 64 output samples from full level
  CHECK_NEAR(b[0], v, v * 0.01f);
  CHECK_NEAR(b[32], v * 0.5f, 0.005f);
  CHECK(b[63] < v * 0.02f && b[63] > 0.0f);
  for (int i = 0; i + 1 < 64; i++) CHECK(b[i + 1] < b[i]);
  for (int i = 64; i < kBlock; i++) CHECK_EQ(b[i], 0.0f);
  // The default argument is the 12 ms choke.
  sv.trigger(u.data(), 5000, 1.0f, 1.0f); a.assign(200, 0.0f); sv.render(a.data(), 200); sv.release();
  std::vector<float> out = renderAll(sv, 50); CHECK(std::abs((int)out.size() - 265) <= 1);
  // A faster fade requested during a slower one takes over from the current level (0.81 / (1/64) = 52 samples).
  sv.trigger(u.data(), 5000, 1.0f, 1.0f); a.assign(200, 0.0f); sv.render(a.data(), 200);
  sv.release(); a.assign(50, 0.0f); sv.render(a.data(), 50); CHECK(sv.active());
  sv.release(64);
  out = renderAll(sv, 16);
  CHECK(out.size() >= 50u && out.size() <= 53u);
  CHECK_NEAR(out[0], v * (1.0f - 50.0f / 264.6f), 0.01f);
  // A slower request never undoes a faster fade.
  sv.trigger(u.data(), 5000, 1.0f, 1.0f); a.assign(200, 0.0f); sv.render(a.data(), 200);
  sv.release(64); sv.release();
  out = renderAll(sv, 16); CHECK_EQ((int)out.size(), 64);
  // Release during the attack with the short fade: env 20/44.1 = 0.45 -> 0 at 1/64 per sample (30 outputs).
  sv.trigger(u.data(), 5000, 1.0f, 1.0f); a.assign(20, 0.0f); sv.render(a.data(), 20); sv.release(64);
  out = renderAll(sv, 16); CHECK(out.size() >= 29u && out.size() <= 30u);
  // fadeSamples < 1 is clamped to one sample.
  sv.trigger(u.data(), 5000, 1.0f, 1.0f); a.assign(200, 0.0f); sv.render(a.data(), 200); sv.release(0);
  out = renderAll(sv, 16); CHECK(out.size() <= 1u); CHECK(!sv.active());
  // Rate 2 on a 100-sample slot after 20 outputs (position 40): 30 positions left, the 64-sample slope is steeper.
  sv.trigger(u.data(), 100, 1.0f, 2.0f); a.assign(20, 0.0f); sv.render(a.data(), 20); sv.release(64);
  out = renderAll(sv, 16);
  CHECK(out.size() >= 29u && out.size() <= 30u); CHECK(out.back() < v * 0.05f);
  // Two instances per slot: the engine's retrigger pattern (release the other, trigger this one) sums without a step.
  SampleVoice x, y; x.trigger(u.data(), 5000, 1.0f, 1.0f); a.assign(100, 0.0f); x.render(a.data(), 100);
  x.release(kSlotFadeSamples); y.trigger(u.data(), 5000, 1.0f, 1.0f);
  std::vector<float> mix(128, 0.0f); x.render(mix.data(), 128); y.render(mix.data(), 128);
  float lo = 1e9f, hi2 = 0; for (int i = 0; i < 128; i++) { if (mix[i] < lo) lo = mix[i]; if (mix[i] > hi2) hi2 = mix[i]; }
  CHECK(lo > v * 0.98f);                               // the crossfade never dips (new attack is faster than the old fade)
  CHECK(hi2 < v * 1.32f && hi2 > v * 1.25f);           // and sums to at most 1 + 44 (1/44.1 - 1/64) = 1.31 v at sample 44
  CHECK_NEAR(mix[100], v, 1e-5f);                      // one voice left after the fade
  CHECK(!x.active() && y.active());
}

TEST(sample_voice_real_pool_sizes) {                 // the pool region is 40 KB; a full-length slot plays through
  ulawInit();
  std::vector<int16_t> pcm(40 * 1024); for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(20000.0f * sinf(i * 0.05f));
  std::vector<uint8_t> u = encodeSlot(pcm);
  SampleVoice sv; sv.trigger(u.data(), (uint32_t)u.size(), 0.8f, 1.0f);
  std::vector<float> out = renderAll(sv, 256);
  CHECK(std::abs((int)out.size() - (int)u.size()) <= 2);
  double e = 0; for (int i = 100; i < 20000; i++) { float ref = 0.8f * ulawDecode(u[i]) / 32767.0f; e += std::fabs(out[i] - ref); }
  CHECK(e / 19900 < 1e-5);
}

int main() { return lp_test::run(); }
