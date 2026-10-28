// Host tests for the fx module: dsp/echo.cpp, dsp/reverb.cpp (SPEC §5 buses) and dsp/chorus.cpp (SPEC §9.22 per-set
// chorus bus). The echo line is µ-law uint8_t (SPEC §9.19 static reclaim): full scale reads back as kUlawFs
// (32124 / 32768), every stored magnitude is truncated toward zero by up to one µ-law step (≈ 3–6 % of the value) and
// anything below 8 LSB (2.4e-4) is stored as 0.
#include "check.h"
#include <vector>
#include <chrono>
#include <cstring>
#include "core/config.h"
#include "dsp/echo.h"
#include "dsp/reverb.h"
#include "dsp/chorus.h"
#include "dsp/ulaw.h"

using namespace lp;
namespace {

constexpr int kGuard = 64;                                   // sentinel words after each static buffer
constexpr int16_t kSentinel = 0x5A5A;
constexpr uint8_t kSentinel8 = 0x5A;
uint8_t gEchoMem[kEchoSamples + kGuard];                      // µ-law line (echo.h)
int16_t gRevMem[Reverb::kMemory + kGuard];
constexpr float kUlawFs = 32124.0f / 32768.0f;               // what a full-scale sample decodes to after the µ-law line (0.9803)
constexpr float kUlawStep = 1.0f / 16.0f;                    // the largest relative truncation of one stored sample (one bin at a segment's bottom: 8 / 132 ≈ 6 %)
// What the echo line hands back for a sample stored at amplitude a: the G.711 code, stepped one bin toward zero when its
// midpoint decode would exceed the value (echo.cpp qU). Exact for the first repeat of an impulse of that amplitude.
float stored(float a) {
  int pcm = (int)(a * 32767.0f); const int mag = pcm < 0 ? -pcm : pcm;
  uint8_t c = ulawEncode((int16_t)pcm); int d = ulawDecode(c);
  if ((d < 0 ? -d : d) > mag) { const uint8_t u = (uint8_t)~c; c = (uint8_t)~((u & 0x80) | ((u & 0x7F) - 1)); d = ulawDecode(c); }
  return d / 32768.0f;
}

void guard(int16_t* mem, int n) { for (int i = 0; i < kGuard; i++) mem[n + i] = kSentinel; }
void guard(uint8_t* mem, int n) { for (int i = 0; i < kGuard; i++) mem[n + i] = kSentinel8; }
bool guardIntact(const int16_t* mem, int n) { for (int i = 0; i < kGuard; i++) if (mem[n + i] != kSentinel) return false; return true; }
bool guardIntact(const uint8_t* mem, int n) { for (int i = 0; i < kGuard; i++) if (mem[n + i] != kSentinel8) return false; return true; }

// Run a bus over `in` in kBlock-sized blocks, accumulating into `out` (which may be pre-filled).
template <typename Bus>
void run(Bus& b, const std::vector<float>& in, std::vector<float>& out) {
  int n = (int)in.size();
  for (int i = 0; i < n; i += kBlock) { int m = n - i < kBlock ? n - i : kBlock; b.process(&in[i], &out[i], m); }
}
bool finite(const std::vector<float>& v) { for (float x : v) if (!std::isfinite(x)) return false; return true; }
float peakAbs(const std::vector<float>& v, int from = 0, int to = -1) {
  if (to < 0) to = (int)v.size(); float p = 0; for (int i = from; i < to; i++) p = std::fabs(v[i]) > p ? std::fabs(v[i]) : p; return p;
}
double energy(const std::vector<float>& v, int from, int to) { double e = 0; for (int i = from; i < to; i++) e += (double)v[i] * v[i]; return e; }
double sum(const std::vector<float>& v, int from, int to) { double s = 0; for (int i = from; i < to; i++) s += v[i]; return s; }
int argmaxAbs(const std::vector<float>& v, int from, int to) { int k = from; for (int i = from; i < to; i++) if (std::fabs(v[i]) > std::fabs(v[k])) k = i; return k; }
// First index >= from with a non-zero sample (the echo's first repeat).
int firstNonZero(const std::vector<float>& v, int from) { for (int i = from; i < (int)v.size(); i++) if (v[i] != 0.0f) return i; return -1; }

Echo makeEcho(float bpm, EchoDiv div, float fb, int len = kEchoSamples) {
  Echo e; guard(gEchoMem, len); e.init(gEchoMem, len); e.setTempo(bpm); e.setDivision(div); e.setFeedback(fb);
  std::vector<float> z(kBlock * 4, 0.f), o(kBlock * 4, 0.f); run(e, z, o);   // let any initial tap crossfade settle
  return e;
}
// Delay in samples measured from an impulse: index of the first repeat relative to the impulse.
int measuredDelay(Echo& e) {
  const int N = kEchoSamples + 2 * kBlock; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f;
  run(e, in, out); return firstNonZero(out, 1);
}
// Run silence through until the line is empty (feedback 0: one line length) so measuredDelay sees a clean impulse.
void flushLine(Echo& e) { std::vector<float> z(2 * kEchoSamples, 0.f), o(2 * kEchoSamples, 0.f); run(e, z, o); }
float maxStep(const std::vector<float>& v) { float m = 0; for (size_t i = 1; i < v.size(); i++) { float s = std::fabs(v[i] - v[i - 1]); if (s > m) m = s; } return m; }
std::vector<float> sine(float amp, float hz, int n) { std::vector<float> v(n); for (int i = 0; i < n; i++) v[i] = amp * sinf(2.0f * 3.14159265f * hz * (float)i / (float)kSampleRate); return v; }
float sineSlope(float amp, float hz) { return amp * 2.0f * 3.14159265f * hz / (float)kSampleRate; }   // max per-sample step of that sine
int combClamps(int from, int len) { int c = 0; for (int i = from; i < from + len; i++) if (gRevMem[i] == 32767 || gRevMem[i] == -32768) c++; return c; }
Reverb makeReverb(float decay, float tone) { Reverb r; guard(gRevMem, Reverb::kMemory); r.init(gRevMem); r.setDecay(decay); r.setTone(tone); return r; }

} // namespace

// ---------------------------------------------------------------- echo

TEST(echo_delay_1_8_at_120_bpm) {                            // 0.5 beat × 60/120 × 22050 = 5512.5
  Echo e = makeEcho(120, Div8, 0.0f);
  int d = measuredDelay(e);
  CHECK(d == 5512 || d == 5513);
  CHECK(guardIntact(gEchoMem, kEchoSamples));
}

TEST(echo_delay_other_divisions_at_120) {
  { Echo e = makeEcho(120, Div16, 0.0f); int d = measuredDelay(e); CHECK(d == 2756 || d == 2757); }   // 2756.25
  { Echo e = makeEcho(120, Div8d, 0.0f); int d = measuredDelay(e); CHECK(d == 8268 || d == 8269); }   // 8268.75
  { Echo e = makeEcho(120, Div4, 0.0f); int d = measuredDelay(e); CHECK(d == 5512 || d == 5513); }   // 11025 > the 10240 line → 5512.5 (SPEC §10.1)
}

TEST(echo_folds_long_divisions_down) {                       // SPEC §5: halve until it fits the line; the line is kEchoSamples = 10240 (SPEC §10.1 bring-up value, 464 ms)
  static_assert(kEchoSamples == 10240, "the fold cases below pin the 10240-sample line");
  { Echo e = makeEcho(40, Div2, 0.0f); int d = measuredDelay(e); CHECK(d == 8268 || d == 8269); }      // 66150 → 33075 → 16537.5 → 8268.75
  { Echo e = makeEcho(120, Div2, 0.0f); int d = measuredDelay(e); CHECK(d == 5512 || d == 5513); }     // 22050 → 11025 → 5512.5
  { Echo e = makeEcho(120, Div4d, 0.0f); int d = measuredDelay(e); CHECK(d == 8268 || d == 8269); }    // 16537.5 → 8268.75
  { Echo e = makeEcho(240, Div2, 0.0f); int d = measuredDelay(e); CHECK(d == 5512 || d == 5513); }     // 11025 > 10240 → 5512.5
  // Cases that only pass with a 10240 line (a 12288 line would leave them unfolded):
  { Echo e = makeEcho(100, Div4, 0.0f); CHECK_EQ(measuredDelay(e), 6615); }                            // 13230 > 10240 → 6615
  { Echo e = makeEcho(108, Div4, 0.0f); CHECK_EQ(measuredDelay(e), 6125); }                            // 12250 > 10240 → 6125
  { Echo e = makeEcho(129, Div4, 0.0f); CHECK_EQ(measuredDelay(e), 5128); }                            // 10255.8 > 10240 → 5127.9
  { Echo e = makeEcho(130, Div4, 0.0f); CHECK_EQ(measuredDelay(e), 10177); }                           // 10176.9 ≤ 10240 stays unfolded (longest possible delay)
  { Echo e = makeEcho(72, Div8d, 0.0f); int d = measuredDelay(e); CHECK(d == 6890 || d == 6891); }     // 13781.25 → 6890.6
  { Echo e = makeEcho(120, Div2, 0.0f, 1000); CHECK_EQ(measuredDelay(e), 689); CHECK(guardIntact(gEchoMem, 1000)); } // tiny line: 22050/32 = 689.06
}

TEST(echo_impulse_repeats_decay_by_feedback) {
  const float fb = 0.45f; const int D = 5513;
  Echo e = makeEcho(120, Div8, fb); e.setDamping(0.4f);
  const int N = D * 7; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f;
  run(e, in, out);
  CHECK(finite(out));
  CHECK_EQ(out[0], 0.0f);                                    // nothing before the first repeat
  CHECK(peakAbs(out, 0, D) == 0.0f);
  CHECK_NEAR(out[D], kUlawFs, 2e-4f);                        // first repeat = input (undamped read) at the µ-law full scale
  CHECK_NEAR(out[D], stored(1.0f), 1e-6f);
  // The damping low-pass preserves DC, so the sum over each repeat window decays by fb — less the µ-law truncation of
  // the pass (every stored sample loses up to one bin toward zero, ≈ 3–6 %, more once the tail nears the 8-LSB floor):
  // the ratio is never above fb and stays within two bins of it. Measured: 0.438 0.440 0.433 0.412 at fb 0.45.
  double prev = sum(out, D - D / 2, D + D / 2);
  CHECK_NEAR(prev, kUlawFs, 1e-3);
  float prevPeak = peakAbs(out, D - D / 2, D + D / 2);
  for (int k = 2; k <= 6; k++) {
    double s = sum(out, k * D - D / 2, k * D + D / 2);
    const double ratio = s / prev;
    CHECK(ratio <= fb + 0.002); CHECK(ratio >= fb * (1.0 - 2.0 * kUlawStep));
    float pk = peakAbs(out, k * D - D / 2, k * D + D / 2);
    CHECK(pk < prevPeak * fb * 1.001f);                     // damping smears, so peaks fall at least as fast as fb
    int at = argmaxAbs(out, k * D - D / 2, k * D + D / 2);
    CHECK(at >= k * D && at <= k * D + 8);                   // repeat k lands at k·D (LP shifts the peak a little)
    std::printf("  repeat %d: window sum ratio %.4f (fb %.2f)\n", k, ratio, fb);
    prev = s; prevPeak = pk;
  }
}

TEST(echo_damping_darkens_repeats) {                         // more damping → peak of the 2nd repeat lower and wider
  const int D = 5513; float peakBright, peakDark;
  // The second repeat's sum is fb × the first (kUlawFs) less the pass's µ-law truncation (within one bin): 0.474 / 0.475 measured.
  const double lo = 0.5 * kUlawFs * (1.0 - kUlawStep), hi = 0.5 * kUlawFs + 1e-3;
  { Echo e = makeEcho(120, Div8, 0.5f); e.setDamping(0.0f); std::vector<float> in(3 * D, 0.f), out(3 * D, 0.f); in[0] = 1.f; run(e, in, out);
    peakBright = peakAbs(out, 2 * D, 2 * D + 100); const double s2 = sum(out, 2 * D - 10, 2 * D + 200); CHECK(s2 >= lo && s2 <= hi); }
  { Echo e = makeEcho(120, Div8, 0.5f); e.setDamping(1.0f); std::vector<float> in(3 * D, 0.f), out(3 * D, 0.f); in[0] = 1.f; run(e, in, out);
    peakDark = peakAbs(out, 2 * D, 2 * D + 100); const double s2 = sum(out, 2 * D - 10, 2 * D + 200); CHECK(s2 >= lo && s2 <= hi); }
  CHECK(peakDark < peakBright * 0.6f);
  CHECK(peakBright > 0.5f * 0.85f);                          // 8 kHz cutoff: first-sample response ≈ 0.9
}

TEST(echo_clamps_before_storing) {                           // over-range input saturates to the µ-law full scale, never wraps
  Echo e = makeEcho(120, Div8, 0.0f);
  const int N = 6000; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 3.0f; in[1] = -5.0f;
  run(e, in, out);
  CHECK(finite(out));
  CHECK(peakAbs(out) <= kUlawFs + 1e-6f);
  CHECK_NEAR(out[5513], kUlawFs, 1e-4f);
  CHECK_NEAR(out[5514], -kUlawFs, 1e-4f);
}

// SPEC §9.19 (a): the line is µ-law and its store truncates toward zero. (1) Every stored amplitude reads back no larger
// than it went in and within one µ-law bin (≤ 1/16 of the value, or 8 LSB near zero) of it. (2) The feedback tail dies
// to exact zero even at the highest feedback the bus allows (0.98) with no damping — the case where G.711's midpoint
// decode would sustain an 8-LSB tail forever (8 × 0.98 × 0.897 → 7 → bin 1 → 8 again). (3) The quantisation never
// exceeds the signal: a sine stays within its amplitude.
TEST(echo_ulaw_store_truncates_toward_zero) {
  const int D = 5513;
  int checked = 0;
  for (float a = 1.0f; a > 1e-4f; a *= 0.83f) {
    Echo e = makeEcho(120, Div8, 0.0f);
    const int N = D + 8; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = a; run(e, in, out);
    const float got = out[D];
    CHECK(got <= a + 1e-7f);                                 // never above the input
    CHECK(got >= a * (1.0f - kUlawStep) - 8.0f / 32768.0f - 1e-7f);   // within one bin (8 LSB at the bottom segment)
    CHECK_NEAR(got, stored(a), 1e-6f);                       // exactly the truncating quantiser's value
    CHECK(got >= 0.0f);
    checked++;
  }
  CHECK(checked > 40);
  { Echo e = makeEcho(120, Div8, 0.0f);                      // a negative impulse: the same toward zero
    const int N = D + 8; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = -0.5f; run(e, in, out);
    CHECK(out[D] >= -0.5f); CHECK(out[D] <= -0.5f * (1.0f - kUlawStep)); CHECK_NEAR(out[D], -stored(0.5f), 1e-6f); }
  { Echo e = makeEcho(120, Div8, 0.98f); e.setDamping(0.0f);   // the worst case for a stuck tail
    const int N = 90 * kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f;
    run(e, in, out);
    CHECK(finite(out));
    int lastNz = -1; for (int i = 0; i < N; i++) if (out[i] != 0.0f) lastNz = i;
    CHECK(lastNz >= 0 && lastNz < N - 2 * D);                // it died inside the run, with two silent periods to spare
    std::vector<float> z(kBlock, 0.f), o(kBlock, 0.f); run(e, z, o);
    CHECK(peakAbs(o) == 0.0f);                               // and nothing is left in the line
    std::printf("  echo tail fb 0.98 damp 0: exact zero after %.1f s (%d repeats)\n", lastNz / (double)kSampleRate, lastNz / D); }
  { Echo e = makeEcho(120, Div8, 0.0f);
    const float A = 0.7f; const int N = kSampleRate; std::vector<float> in = sine(A, 440.0f, N), out(N, 0.f); run(e, in, out);
    CHECK(peakAbs(out) <= A); CHECK(peakAbs(out, N / 2, N) > A * (1.0f - kUlawStep) - 1e-3f); }
}

TEST(echo_tap_change_crossfades_without_click) {
  Echo e = makeEcho(120, Div8, 0.0f);
  const float A = 0.5f, f = 200.0f; const int N = kSampleRate;
  std::vector<float> in = sine(A, f, N), out(N, 0.f);
  run(e, in, out);                                           // one second of steady state at 1/8
  e.setDivision(Div4d); e.setTempo(120);                     // the engine's CmdEchoDivision path: setDivision then setTempo (1/4d = 8268.75 fits the 10240 line unfolded)
  std::vector<float> out2(N, 0.f); run(e, in, out2);
  float sineStep = sineSlope(A, f);                          // 0.0285
  CHECK(maxStep(out2) < sineStep * 1.3f + 0.003f);           // an abrupt tap jump would step up to 2A = 1.0
  CHECK(peakAbs(out2) <= A * 1.01f);
  // After the fade the output is the 1/4d-delayed sine: compare against the input shifted by 8269 (half a sample off: 200 Hz moves 0.007 per sample).
  float err = 0; for (int i = N / 2; i < N; i++) { float want = in[(i + N - 8269) % N]; float d = std::fabs(out2[i] - want); if (d > err) err = d; }
  CHECK(err < 2e-2f);
}

TEST(echo_tempo_changes_during_fade_converge) {              // setTempo during a crossfade is applied once it ends, without a click
  Echo e = makeEcho(120, Div8, 0.0f);
  const float A = 0.5f, f = 1000.0f; const int N = kSampleRate;
  std::vector<float> in = sine(A, f, N), out(N, 0.f);
  run(e, in, out); std::fill(out.begin(), out.end(), 0.f);   // steady state at 5513
  e.setTempo(130); e.process(&in[0], &out[0], kBlock);       // fade 5513 → 5088 starts
  e.setTempo(140); e.process(&in[kBlock], &out[kBlock], kBlock);         // mid-fade: must not be lost, must not move the new tap
  e.setTempo(150); for (int i = 2 * kBlock; i < N; i += kBlock) e.process(&in[i], &out[i], kBlock);
  CHECK(maxStep(out) < sineSlope(A, f) * 1.3f + 0.003f);
  CHECK(peakAbs(out) <= A * 1.01f);
  flushLine(e);
  CHECK_EQ(measuredDelay(e), 4410);                          // 0.5 × 60/150 × 22050 = 4410
}

TEST(echo_retarget_mid_fade_has_no_click) {                 // engine.cpp serviceRamps() calls setTempo every tick during a SHIFT+tempo ramp (SPEC §9.12)
  const float A = 0.5f;
  { // (a) one retarget landing at t ≈ 0.58 of a running fade, 100 Hz tone (slope 0.014/sample, so any tap jump stands out)
    Echo e = makeEcho(120, Div8d, 0.0f);
    const float f = 100.0f; const int N = kSampleRate;
    std::vector<float> in = sine(A, f, N), out(N, 0.f);
    run(e, in, out); std::fill(out.begin(), out.end(), 0.f); // steady state at 8269
    e.setTempo(163); e.process(&in[0], &out[0], kBlock);     // fade 8269 → 6087 starts (256 of 441 samples done)
    e.setTempo(140); e.process(&in[kBlock], &out[kBlock], kBlock);         // unfixed code re-pointed the new tap here: a 0.53 step
    e.setTempo(163); for (int i = 2 * kBlock; i < N; i += kBlock) e.process(&in[i], &out[i], kBlock);
    CHECK(maxStep(out) < sineSlope(A, f) * 1.3f + 0.003f);   // sine slope 0.0142; the old step was 37× that
    CHECK(peakAbs(out) <= A * 1.01f);
    flushLine(e);
    CHECK_EQ(measuredDelay(e), 6087);                        // 0.75 × 60/163 × 22050 = 6087.4: the last tempo wins
  }
  { // (b) a 2 s ramp 120 → 163 BPM with setTempo twice per block (≈ 2 ticks per block), 1 kHz tone
    Echo e = makeEcho(120, Div8d, 0.0f);
    const float f = 1000.0f; const int M = 2 * kSampleRate, H = kBlock / 2;
    std::vector<float> in = sine(A, f, M), out(M, 0.f);
    run(e, in, out); std::fill(out.begin(), out.end(), 0.f);
    for (int i = 0; i < M; i += H) { e.setTempo(120.0f + 43.0f * (float)i / (float)M); e.process(&in[i], &out[i], H); }
    float slope = sineSlope(A, f); int big = 0; for (int i = 1; i < M; i++) if (std::fabs(out[i] - out[i - 1]) > 1.5f * slope) big++;
    CHECK_EQ(big, 0);                                        // unfixed code: 55 such steps, max 0.58 (natural slope 0.14)
    CHECK(maxStep(out) < slope * 1.3f + 0.003f);
    CHECK(peakAbs(out) <= A * 1.01f);
    std::printf("  echo ramp 120→163 with setTempo per half block: max step %.3f vs sine slope %.3f\n", maxStep(out), slope);
    e.setTempo(163); flushLine(e);
    CHECK_EQ(measuredDelay(e), 6087);                        // the delay followed the ramp to its end (0.75 × 60/163 × 22050 = 6087.4)
  }
}

TEST(echo_nan_and_inf_input_never_enter_the_line) {          // q16: NaN → 0, ±Inf → full scale, all defined (no float→int UB)
  const int D = 5513; Echo e = makeEcho(120, Div8, 0.45f);
  const int N = 4 * D; std::vector<float> in(N, 0.f), out(N, 0.f);
  in[0] = NAN; in[1] = INFINITY; in[2] = -INFINITY; in[D + 100] = 0.5f;   // garbage, then a clean impulse
  run(e, in, out);
  CHECK(finite(out));
  CHECK_EQ(out[D], 0.0f);                                    // the NaN sample was stored as 0, not as a full-scale click
  CHECK_NEAR(out[D + 1], kUlawFs, 1e-4f);                    // ±Inf saturate (to the µ-law full scale)
  CHECK_NEAR(out[D + 2], -kUlawFs, 1e-4f);
  CHECK_NEAR(out[2 * D + 100], stored(0.5f), 2e-3f);         // the echo keeps working: the clean impulse repeats (0.488: one bin under 0.5)
  float fbPk = peakAbs(out, 2 * D, 2 * D + 8);               // ... and the saturated pair feeds back (the damping LP partly cancels +1/−1: 0.31)
  CHECK(fbPk > 0.15f && fbPk < 0.5f);
  std::vector<float> z(7 * kSampleRate, 0.f), o(7 * kSampleRate, 0.f); run(e, z, o);
  CHECK(finite(o));
  std::vector<float> z2(kBlock, 0.f), o2(kBlock, 0.f); run(e, z2, o2);
  CHECK(peakAbs(o2) == 0.0f);                                // no stuck state: the line decays to exact zero
}

TEST(echo_silent_after_input_stops_all_divisions) {          // |out| < 1e-4 (−80 dBFS) after a full-scale impulse, every division
  // Arithmetic, not tuning: repeat k peaks at ≤ fb^k (damping 0.4 smears a little extra), so −80 dB takes 9 repeats at the
  // engine default fb 0.45 and 10–11 at fb 0.5. 1/4 and 1/2 (folded) at 120 BPM repeat every 0.5 s; the longest possible
  // period is the full line, 10240 samples = 0.464 s (1/4 at 130 BPM = 10177). Contract: 5 s at fb 0.45 for every division at
  // 120 BPM (5.0 s + damping smear at the longest period), 6 s at fb 0.5 (6.2 s at the longest period); exact zero within
  // three further repeats. The µ-law line (SPEC §9.19) truncates toward zero and stores anything under 8 LSB (2.4e-4) as
  // silence, so exact zero now coincides with the −80 dB point. Measured (int16 line): fb 0.45 @120: 1.13 2.25 3.38 4.50
  // 3.38 4.50 s; fb 0.5: 1.38 2.75 4.13 5.50 4.13 5.50 s; longest period: 5.00 / 6.11 s. µ-law: 1.00 2.00 3.00 2.00 3.00
  // 2.00 / 3.69 s and 1.13 2.25 3.38 2.25 3.38 2.25 / 4.15 s, exact zero at the same sample.
  struct Case { float fb, bpm; EchoDiv div; float boundS; };
  Case cases[] = {
    {0.45f, 120, Div16, 5.0f}, {0.45f, 120, Div8, 5.0f}, {0.45f, 120, Div8d, 5.0f}, {0.45f, 120, Div4, 5.0f}, {0.45f, 120, Div4d, 5.0f}, {0.45f, 120, Div2, 5.0f},
    {0.45f, 130, Div4, 5.1f},
    {0.5f, 120, Div16, 6.0f}, {0.5f, 120, Div8, 6.0f}, {0.5f, 120, Div8d, 6.0f}, {0.5f, 120, Div4, 6.0f}, {0.5f, 120, Div4d, 6.0f}, {0.5f, 120, Div2, 6.0f},
    {0.5f, 130, Div4, 6.2f},
  };
  const char* names[] = {"1/16", "1/8", "1/8d", "1/4", "1/4d", "1/2"};
  for (const Case& c : cases) {
    Echo e = makeEcho(c.bpm, c.div, c.fb);
    int period = measuredDelay(e); flushLine(e);
    const int N = 7 * kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f);
    in[0] = 1.0f;                                            // full-scale impulse: the worst case
    run(e, in, out);
    CHECK(finite(out));
    int last = -1, lastNz = -1; for (int i = 0; i < N; i++) { if (std::fabs(out[i]) >= 1e-4f) last = i; if (out[i] != 0.0f) lastNz = i; }
    CHECK(last >= 0 && last < (int)(c.boundS * kSampleRate));
    CHECK(lastNz <= last + 3 * period + 16);                 // exact zero within three more repeats (the last 1–3 LSB truncate to zero)
    std::vector<float> after(kBlock, 0.f), o2(kBlock, 0.f); run(e, after, o2);
    CHECK(peakAbs(o2) == 0.0f);
    std::printf("  echo tail %s @%.0f fb %.2f (period %d): below 1e-4 after %.2f s, exact zero after %.2f s\n", names[c.div], c.bpm, c.fb, period,
                last / (double)kSampleRate, lastNz / (double)kSampleRate);
  }
}

TEST(echo_accumulates_into_out) {
  Echo e = makeEcho(120, Div8, 0.0f);
  const int N = 6000; std::vector<float> in(N, 0.f), out(N, 0.25f); in[0] = 0.5f;
  run(e, in, out);
  CHECK_EQ(out[0], 0.25f);
  CHECK_NEAR(out[5513], 0.25f + stored(0.5f), 1e-4f);        // the pre-filled 0.25 plus the µ-law repeat of 0.5
}

TEST(echo_null_line_is_safe) {
  Echo e; e.init(nullptr, 0); e.setTempo(120);
  std::vector<float> in(kBlock, 1.f), out(kBlock, 0.f); e.process(in.data(), out.data(), kBlock);
  CHECK(peakAbs(out) == 0.0f);
}

// ---------------------------------------------------------------- reverb

TEST(reverb_memory_fits_and_init_zeroes) {
  for (int i = 0; i < Reverb::kMemory; i++) gRevMem[i] = kSentinel;
  Reverb r = makeReverb(1.0f, 1.0f);
  bool zero = true; for (int i = 0; i < Reverb::kMemory; i++) if (gRevMem[i] != 0) zero = false;
  CHECK(zero);
  std::vector<float> in(kSampleRate, 0.f), out(kSampleRate, 0.f); for (int i = 0; i < kSampleRate; i += 97) in[i] = 0.9f;
  run(r, in, out);
  CHECK(guardIntact(gRevMem, Reverb::kMemory));
}

TEST(reverb_impulse_energy_decays_monotonically) {           // 50 ms windows after the first 50 ms never grow
  for (float decay : {0.0f, 0.5f, 1.0f}) {
    Reverb r = makeReverb(decay, 0.5f);
    const int N = 3 * kSampleRate, W = kSampleRate / 20; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f;
    run(r, in, out);
    CHECK(finite(out));
    CHECK(peakAbs(out, 0, 352) == 0.0f);                     // predelay 352 samples (16 ms) of silence
    // Checked down to an RMS of 2e-4 (−74 dBFS): the int16 network runs at −12 dB with ×4 makeup, so one LSB is
    // 5.66/32768 = 1.7e-4 at the return and below ~2e-4 the tail is ±1 LSB truncation noise, not a decay.
    const double floor = (double)W * 4e-8; double prev = energy(out, W, 2 * W); int bad = 0, checked = 0;
    for (int w = 2; w < N / W && prev > floor; w++) { double e = energy(out, w * W, (w + 1) * W); if (e > prev) bad++; prev = e; checked++; }
    CHECK_EQ(bad, 0);
    CHECK(checked >= (decay == 0.0f ? 4 : decay == 0.5f ? 7 : 24));    // ≥ 0.2 s / 0.35 s / 1.2 s of tail above −74 dBFS
    std::printf("  decay %.1f: %d windows of 50 ms above -74 dBFS, all decreasing\n", decay, checked);
    CHECK(energy(out, 0, W) < energy(out, W, 2 * W));       // the tail builds inside the first 50 ms (predelay + combs)
  }
}

TEST(reverb_impulse_train_2s_at_decay_1_peak_bounded) {     // the return is hard-bounded to ±2 (kWetClamp); the stated bound is a property of the code
  const float kBound = 2.0f;
  { Reverb r = makeReverb(1.0f, 1.0f);                       // 16ths at 120 BPM, full-scale impulses
    const int N = 2 * kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); for (int i = 0; i < N; i += 2756) in[i] = 1.0f;
    run(r, in, out); CHECK(finite(out)); float p = peakAbs(out); CHECK(p <= kBound); CHECK(p > 0.1f); CHECK(p < 0.5f);   // linear region: far from the clamp
    std::printf("  reverb peak, 16th impulses 2 s @ decay 1: %.3f\n", p); }
  { Reverb r = makeReverb(1.0f, 1.0f);                       // dense train (period 4: 5.5 kHz = fs/4, an exact resonance of combs 652 and 784) hits the clamp
    const int N = 2 * kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); for (int i = 0; i < N; i += 4) in[i] = 1.0f;
    run(r, in, out); CHECK(finite(out)); CHECK(peakAbs(out) <= kBound); }
  { Reverb r = makeReverb(1.0f, 1.0f);                       // every sample = DC, removed by the input high-pass
    const int N = 2 * kSampleRate; std::vector<float> in(N, 1.0f), out(N, 0.f);
    run(r, in, out); CHECK(finite(out)); CHECK(peakAbs(out) <= kBound);
    CHECK(peakAbs(out, N - kSampleRate / 2, N) < peakAbs(out, 0, kSampleRate / 2) * 0.1f); }   // only the step's tail, decaying
  { Reverb r = makeReverb(1.0f, 1.0f);                       // 2 s of dense full-scale noise: the raw return would reach ~3; the clamp holds it
    const int N = 2 * kSampleRate; std::vector<float> in(N), out(N, 0.f);
    uint32_t s = 777; for (int i = 0; i < N; i++) { s = s * 1664525u + 1013904223u; in[i] = ((s >> 8) & 0xFFFF) / 32768.0f - 1.0f; }
    run(r, in, out); CHECK(finite(out)); float p = peakAbs(out); CHECK(p <= kBound); CHECK(p > 1.0f);
    std::printf("  reverb peak, full-scale noise 2 s @ decay 1: %.3f\n", p); }
  { Reverb r = makeReverb(1.0f, 1.0f);                       // the same noise at −12 dB stays in the linear region (no clamp)
    const int N = 2 * kSampleRate; std::vector<float> in(N), out(N, 0.f);
    uint32_t s = 777; for (int i = 0; i < N; i++) { s = s * 1664525u + 1013904223u; in[i] = 0.25f * (((s >> 8) & 0xFFFF) / 32768.0f - 1.0f); }
    run(r, in, out); CHECK(finite(out)); CHECK(peakAbs(out) < kBound * 0.75f); }
  for (float hz : {100.0f, (float)kSampleRate / 652.0f * 10.0f, 1000.0f}) {   // sustained full-scale tones, incl. an exact comb resonance (338 Hz)
    Reverb r = makeReverb(1.0f, 1.0f);
    const int N = 2 * kSampleRate; std::vector<float> in = sine(1.0f, hz, N), out(N, 0.f);
    run(r, in, out); CHECK(finite(out)); float p = peakAbs(out); CHECK(p <= kBound);
    std::printf("  reverb peak, full-scale %.0f Hz tone 2 s @ decay 1: %.3f\n", hz, p);
  }
}

TEST(reverb_recovers_from_nan_and_inf_input) {              // the input HPF state must never go sticky-NaN (hpZ_ = NaN forever, reverb silent until init)
  for (float bad : {NAN, INFINITY, -INFINITY, 100.0f}) {
    Reverb r = makeReverb(0.5f, 0.5f);
    const int N = kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = bad;
    run(r, in, out);
    CHECK(finite(out));
    CHECK(peakAbs(out) == 0.0f);                             // sanitized to 0 before the HPF: no click, nothing stored
    std::vector<float> in2(N, 0.f), out2(N, 0.f); in2[0] = 0.5f; run(r, in2, out2);
    CHECK(finite(out2));
    CHECK(peakAbs(out2) > 0.05f);                            // unfixed code: 0.0000 forever after one NaN/Inf sample
    Reverb fresh = makeReverb(0.5f, 0.5f); std::vector<float> out3(N, 0.f); run(fresh, in2, out3);
    float err = 0; for (int i = 0; i < N; i++) { float d = std::fabs(out2[i] - out3[i]); if (d > err) err = d; }
    CHECK(err == 0.0f);                                      // identical to a fresh instance: the bad sample left no state behind
  }
  { Reverb r = makeReverb(1.0f, 1.0f);                       // garbage mixed into a signal: the rest of the signal still comes through
    const int N = kSampleRate; std::vector<float> in = sine(0.3f, 440.0f, N), out(N, 0.f); for (int i = 0; i < N; i += 1000) in[i] = NAN;
    run(r, in, out); CHECK(finite(out)); CHECK(peakAbs(out, N / 2, N) > 0.1f); }
}

TEST(reverb_combs_have_headroom_for_sustained_tones) {      // −12 dB into the int16 network: a resonant pad tone must not clip inside the feedback loop
  const float f = (float)kSampleRate / 652.0f * 10.0f;      // 338.2 Hz = harmonic 10 of comb 0 (memory [0, 652)): steady-state gain 1/(1 − fb)
  struct Case { float decay, tone, amp; } cases[] = {{Reverb::kDefaultDecay, Reverb::kDefaultTone, 0.3f}, {1.0f, 1.0f, 0.1f}, {0.5f, 0.5f, 0.5f}};
  for (const Case& c : cases) {
    Reverb r = makeReverb(c.decay, c.tone);
    const int N = 2 * kSampleRate; std::vector<float> in = sine(c.amp, f, N), out(N, 0.f);
    int clamped = 0; int mx = 0;
    for (int i = 0; i < N; i += kBlock) { r.process(&in[i], &out[i], kBlock); clamped += combClamps(0, 652); for (int k = 0; k < 652; k++) { int v = std::abs((int)gRevMem[k]); if (v > mx) mx = v; } }
    CHECK_EQ(clamped, 0);                                    // unfixed code: 36 % of comb-0 samples at decay 1 / amp 0.1, 14 % at the default / amp 0.3
    CHECK(mx < 32767 * 3 / 4);                               // and a quarter of headroom to spare
    float pk = peakAbs(out, N / 2, N); double rms = std::sqrt(energy(out, N / 2, N) / (double)(N / 2));
    CHECK(rms / pk > 0.65 && rms / pk < 0.75);               // a clean sine in the tail (a squared-off one falls toward 0.54)
    std::printf("  reverb 338 Hz tone amp %.1f decay %.2f: comb-0 clamps %d, max |comb0| %d/32767, rms/peak %.3f\n", c.amp, c.decay, clamped, mx, rms / pk);
  }
  { Reverb r = makeReverb(1.0f, 1.0f);                       // known limit, informational: full decay and a 0.5 resonance tone still clip (gain 14.3 × 0.25 × 0.5 > 1)
    const int N = 2 * kSampleRate; std::vector<float> in = sine(0.5f, f, N), out(N, 0.f); int clamped = 0;
    for (int i = 0; i < N; i += kBlock) { r.process(&in[i], &out[i], kBlock); clamped += combClamps(0, 652); }
    std::printf("  reverb 338 Hz tone amp 0.5 decay 1: comb-0 clamps %.1f %% (documented limit)\n", 100.0 * clamped / (652.0 * (N / kBlock))); }
}

TEST(reverb_init_defaults_are_a_pad_room) {                 // SPEC §5: combs ×2 "so pads do not sound like a drum room"; the engine never calls setDecay/setTone
  Reverb r; guard(gRevMem, Reverb::kMemory); r.setDecay(0.0f); r.setTone(0.0f); r.init(gRevMem);   // init() resets to the defaults (SPEC §2)
  const int N = 3 * kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f; run(r, in, out);
  Reverb d = makeReverb(Reverb::kDefaultDecay, Reverb::kDefaultTone); std::vector<float> out2(N, 0.f); run(d, in, out2);
  float err = 0; for (int i = 0; i < N; i++) { float e = std::fabs(out[i] - out2[i]); if (e > err) err = e; }
  CHECK(err == 0.0f);
  // RT60 from the 10 ms energy envelope, measured from its peak window: 0.95 s (the old 0.75/0.3 member defaults gave 0.42 s).
  const int W = kSampleRate / 100; double e0 = 0; int w0 = 0;
  for (int w = 0; w < 20; w++) { double e = energy(out, w * W, (w + 1) * W); if (e > e0) { e0 = e; w0 = w; } }
  double t60 = -1; for (int w = w0; w < N / W; w++) if (energy(out, w * W, (w + 1) * W) < e0 * 1e-6) { t60 = (w - w0) * W / (double)kSampleRate; break; }
  CHECK(t60 > 0.7 && t60 < 1.4);
  float ir = (float)energy(out, 0, N); CHECK(ir > 0.5f && ir < 1.2f);   // equal-power-ish return
  // Brighter than the darkest tone: HF ratio of the tail above a tone-0 instance at the same decay.
  Reverb dark = makeReverb(Reverb::kDefaultDecay, 0.0f); std::vector<float> out3(N, 0.f); run(dark, in, out3);
  auto hf = [](const std::vector<float>& v) { double e = 0, dd = 0; for (int i = kSampleRate / 5 + 1; i < kSampleRate; i++) { e += (double)v[i] * v[i]; double df = v[i] - v[i - 1]; dd += df * df; } return dd / e; };
  CHECK(hf(out) > hf(out3) * 1.5);
  std::printf("  reverb defaults (decay %.2f tone %.2f): RT60 %.2f s, IR energy %.3f\n", Reverb::kDefaultDecay, Reverb::kDefaultTone, t60, ir);
}

TEST(reverb_silent_5s_after_input_stops_decay_0_5) {
  Reverb r = makeReverb(0.5f, 0.5f);
  const int N = kSampleRate; std::vector<float> in(N), out(N, 0.f);
  uint32_t s = 12345; for (int i = 0; i < N; i++) { s = s * 1664525u + 1013904223u; in[i] = ((s >> 8) & 0xFFFF) / 32768.0f - 1.0f; }   // 1 s of full-scale noise
  in[N - 1] = 1.0f;
  run(r, in, out);
  CHECK(finite(out));
  const int M = 5 * kSampleRate; std::vector<float> z(M, 0.f), tail(M, 0.f); run(r, z, tail);
  CHECK(finite(tail));
  int last = -1; for (int i = 0; i < M; i++) if (std::fabs(tail[i]) >= 1e-4f) last = i;
  CHECK(last < M - 1);
  std::vector<float> after(kBlock, 0.f), o2(kBlock, 0.f); run(r, after, o2);
  CHECK(peakAbs(o2) < 1e-4f);
  std::printf("  reverb tail (decay 0.5): below 1e-4 after %.2f s\n", last / (double)kSampleRate);
}

TEST(reverb_decay_lengthens_tail) {
  double late[3]; float ir[3];
  float decays[3] = {0.0f, 0.5f, 1.0f};
  for (int k = 0; k < 3; k++) {
    Reverb r = makeReverb(decays[k], 0.5f);
    const int N = 2 * kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f; run(r, in, out);
    late[k] = energy(out, kSampleRate / 2, kSampleRate); ir[k] = (float)energy(out, 0, N);
  }
  CHECK(late[1] > late[0] * 10.0);
  CHECK(late[2] > late[1] * 10.0);
  CHECK(ir[1] > 0.3f && ir[1] < 1.2f);                       // equal-power-ish return: ≈ −2 dB vs the send at mid decay
  CHECK(ir[2] < 4.0f);
  std::printf("  reverb IR energy decay 0/0.5/1 @ tone 0.5: %.3f %.3f %.3f\n", ir[0], ir[1], ir[2]);
}

TEST(reverb_tone_controls_brightness) {                      // tail HF ratio (first-difference energy / energy) rises with tone
  double bright[2];
  float tones[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; k++) {
    Reverb r = makeReverb(1.0f, tones[k]);
    const int N = kSampleRate; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 1.0f; run(r, in, out);
    double e = 0, d = 0; for (int i = kSampleRate / 5 + 1; i < N; i++) { e += (double)out[i] * out[i]; double df = out[i] - out[i - 1]; d += df * df; }
    bright[k] = d / e;
  }
  CHECK(bright[1] > bright[0] * 2.0);
}

TEST(reverb_input_highpass_rejects_dc) {
  Reverb r = makeReverb(0.5f, 0.5f);
  const int N = 2 * kSampleRate; std::vector<float> in(N, 0.5f), out(N, 0.f); run(r, in, out);
  CHECK(peakAbs(out, N / 2, N) < 1e-3f);                     // only the step transient gets through
  CHECK(peakAbs(out, 0, N / 4) > 0.01f);
}

TEST(reverb_accumulates_into_out) {
  Reverb r = makeReverb(0.5f, 0.5f);
  const int N = 4096; std::vector<float> in(N, 0.f), out(N, 0.3f); in[0] = 1.0f; run(r, in, out);
  CHECK_EQ(out[0], 0.3f);
  CHECK_EQ(out[351], 0.3f);
  bool changed = false; for (int i = 352; i < N; i++) if (out[i] != 0.3f) changed = true;
  CHECK(changed);
}

TEST(reverb_null_memory_is_safe) {
  Reverb r; r.init(nullptr); r.setDecay(1.0f);
  std::vector<float> in(kBlock, 1.f), out(kBlock, 0.f); r.process(in.data(), out.data(), kBlock);
  CHECK(peakAbs(out) == 0.0f);
}

TEST(fx_host_timing_report) {                                // informational: host ns/sample (ESP32-S3 is ~10× slower)
  Reverb r = makeReverb(0.5f, 0.5f); Echo e = makeEcho(120, Div8d, 0.45f);
  std::vector<float> in(kBlock, 0.1f), out(kBlock, 0.f);
  auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 4000; i++) r.process(in.data(), out.data(), kBlock);
  auto t1 = std::chrono::steady_clock::now(); for (int i = 0; i < 4000; i++) e.process(in.data(), out.data(), kBlock);
  auto t2 = std::chrono::steady_clock::now();
  // Chorus: one set with all 8 tracks sending, in the engine's 128-sample chunks (beginChunk + 8 sends per chunk).
  Chorus c; c.init(); std::vector<float> cin(Chorus::kMaxChunk, 0.1f), cout(Chorus::kMaxChunk, 0.f);
  auto t3 = std::chrono::steady_clock::now(); for (int i = 0; i < 8000; i++) { c.beginChunk(cout.data(), Chorus::kMaxChunk); for (int k = 0; k < 8; k++) c.send(cin.data(), 0.5f, Chorus::kMaxChunk); }
  auto t4 = std::chrono::steady_clock::now();
  std::printf("  host ns/sample: reverb %.1f, echo %.1f, chorus (bus + 8 sends) %.1f\n", std::chrono::duration<double, std::nano>(t1 - t0).count() / (4000.0 * kBlock),
              std::chrono::duration<double, std::nano>(t2 - t1).count() / (4000.0 * kBlock), std::chrono::duration<double, std::nano>(t4 - t3).count() / (8000.0 * Chorus::kMaxChunk));
  CHECK(finite(out)); CHECK(finite(cout));
}

// ---------------------------------------------------------------- chorus (SPEC §9.22)

namespace {
// Runs the chorus in chunks of `chunk` samples over `in` (the one send, gain g), accumulating the wet into `out`
// (pre-filled by the caller), the way renderSet does: beginChunk first, then the send into the same chunk.
void runChorus(Chorus& c, const std::vector<float>& in, std::vector<float>& out, float g, int chunk) {
  const int n = (int)in.size();
  for (int i = 0; i < n; i += chunk) { int m = n - i < chunk ? n - i : chunk; c.beginChunk(&out[i], m); if (g != 0.0f) c.send(&in[i], g, m); }
}
} // namespace

TEST(chorus_fits_its_budget_and_adds_nothing_without_a_send) {
  std::printf("  sizeof(Chorus) = %zu B (line %d x int16), taps %.1f ± %.1f samples, chunk cap %d\n", sizeof(Chorus), Chorus::kLen, Chorus::kCenter, Chorus::kDepth, Chorus::kMaxChunk);
  CHECK(sizeof(Chorus) <= 700u);
  Chorus c; c.init(); CHECK(c.quiet());
  std::vector<float> in(4 * kSampleRate, 0.3f), out(4 * kSampleRate, 0.25f);   // a send of gain 0 and a non-zero mix: nothing may change
  runChorus(c, in, out, 0.0f, Chorus::kMaxChunk);
  bool same = true; for (float x : out) if (x != 0.25f) same = false;
  CHECK(same); CHECK(c.quiet());
  // A send with a NaN gain, or a chunk of 0 / negative length, is ignored as well.
  c.beginChunk(out.data(), Chorus::kMaxChunk); c.send(in.data(), NAN, Chorus::kMaxChunk); c.beginChunk(out.data(), 0); c.beginChunk(out.data(), -5);
  for (int i = 0; i < 20; i++) c.beginChunk(out.data(), Chorus::kMaxChunk);
  same = true; for (float x : out) if (x != 0.25f) same = false;
  CHECK(same); CHECK(c.quiet());
}

TEST(chorus_taps_stay_in_range_and_sweep) {                   // impulses every 2000 samples over 2 LFO cycles (4 s at 0.5 Hz)
  Chorus c; c.init();
  const int N = 4 * kSampleRate, P = 2000; std::vector<float> in(N, 0.f), out(N, 0.f); for (int i = 0; i < N; i += P) in[i] = 1.0f;
  runChorus(c, in, out, 1.0f, 100);                            // a chunk size the engine would use at 125 BPM (110-sample ticks)
  CHECK(finite(out));
  const int lo = (int)(Chorus::kCenter - Chorus::kDepth) - 1, hi = (int)(Chorus::kCenter + Chorus::kDepth) + 2;
  CHECK(lo > Chorus::kMaxChunk); CHECK(hi < Chorus::kLen);
  int minSpread = 1 << 30, maxSpread = 0, bad = 0, checked = 0; double minSum = 1e9, maxSum = 0;
  for (int T = 0; T + hi + 2 < N; T += P) {
    double sum = 0; for (int i = T + 1; i < T + P && i < N; i++) sum += out[i];   // the two taps carry 0.5 each, interpolation preserves the sum
    for (int i = T + 1; i < T + lo; i++) if (out[i] != 0.0f) bad++;             // nothing before the earliest tap ...
    for (int i = T + hi + 1; i < T + P && i < N; i++) if (out[i] != 0.0f) bad++;  // ... nothing after the latest
    int p1 = argmaxAbs(out, T + lo, T + hi + 1); float v1 = std::fabs(out[p1]);
    int p2 = -1; float v2 = 0; for (int i = T + lo; i <= T + hi; i++) if (std::abs(i - p1) > 3 && std::fabs(out[i]) > v2) { v2 = std::fabs(out[i]); p2 = i; }
    int spread = (p2 >= 0 && v2 > 0.25f * v1) ? std::abs(p2 - p1) : 0;           // coincident taps: one peak
    if (spread < minSpread) minSpread = spread; if (spread > maxSpread) maxSpread = spread;
    if (sum < minSum) minSum = sum; if (sum > maxSum) maxSum = sum; checked++;
  }
  std::printf("  %d impulses: tap spread %d .. %d samples (depth %.0f), response sum %.4f .. %.4f, samples outside [%d, %d]: %d\n", checked, minSpread, maxSpread, 2 * Chorus::kDepth, minSum, maxSum, lo, hi, bad);
  CHECK(checked >= 40); CHECK_EQ(bad, 0);
  CHECK(minSpread < 20); CHECK(maxSpread > (int)(1.5f * Chorus::kDepth));       // the taps meet and separate: a sweeping delay
  CHECK(minSum > 0.99); CHECK(maxSum < 1.01);                                   // unity wet per unit send (int16 rounding aside)
}

TEST(chorus_sine_is_bounded_and_time_varying) {               // a 2 kHz tone: the two sweeping taps comb the sum, so its level breathes
  Chorus c; c.init();
  const int N = 4 * kSampleRate; std::vector<float> in = sine(0.5f, 2000.0f, N), out(N, 0.f);
  runChorus(c, in, out, 1.0f, 110);
  CHECK(finite(out)); CHECK(peakAbs(out) <= 0.5f + 1e-3f);
  const int W = kSampleRate / 50; float lo = 1e9f, hi = 0;                      // 20 ms windows after the line filled
  for (int i = Chorus::kLen; i + W <= N; i += W) { float r = (float)std::sqrt(energy(out, i, i + W) / W); if (r < lo) lo = r; if (r > hi) hi = r; }
  std::printf("  2 kHz through the chorus: 20 ms window rms %.4f .. %.4f (a fixed delay would be flat at 0.354)\n", lo, hi);
  CHECK(hi > 1.5f * lo); CHECK(hi > 0.25f);
  // Against a fixed delay at the centre the output is far off at times (the taps are up to ±3 ms away), never a copy.
  float err = 0; for (int i = Chorus::kLen; i < N; i++) { float want = in[i - (int)Chorus::kCenter]; float d = std::fabs(out[i] - want); if (d > err) err = d; }
  CHECK(err > 0.2f);
}

TEST(chorus_accumulates_sends_and_saturates) {
  { Chorus c; c.init(); const int N = 2000; std::vector<float> in(N, 0.f), a(N, 0.f), b(N, 0.f); in[0] = 0.5f;
    for (int i = 0; i < N; i += 100) { c.beginChunk(&a[i], 100); c.send(&in[i], 1.0f, 100); }
    Chorus d; d.init(); for (int i = 0; i < N; i += 100) { d.beginChunk(&b[i], 100); d.send(&in[i], 0.6f, 100); d.send(&in[i], 0.4f, 100); }   // two tracks adding up to the same send
    float err = 0; for (int i = 0; i < N; i++) err = std::fmax(err, std::fabs(a[i] - b[i]));
    CHECK(err <= 2.0f / 32768.0f); CHECK_NEAR(sum(a, 0, N), 0.5, 0.005); }        // a sweeping tap stretches an impulse by up to ±0.5 %
  { Chorus c; c.init(); const int N = 2000; std::vector<float> in(N, 0.f), out(N, 0.f); in[0] = 10.0f; in[2] = NAN; in[3] = INFINITY;   // garbage saturates to full scale, NaN stores nothing
    for (int i = 0; i < N; i += 100) { c.beginChunk(&out[i], 100); c.send(&in[i], 1.0f, 100); }
    CHECK(finite(out)); CHECK(peakAbs(out) <= 1.0f + 1e-4f); CHECK(peakAbs(out) > 0.4f);     // two saturated impulses (10 and +Inf), each a full-scale pair of taps
    CHECK_NEAR(sum(out, 0, N), 2.0 * 32767.0 / 32768.0, 0.02); }
  { Chorus c; c.init(); const int N = 2000; std::vector<float> in(N, 1.0f), out(N, 0.f);    // nine full-scale sends: the int16 line clips, the output stays bounded
    for (int i = 0; i < N; i += 100) { c.beginChunk(&out[i], 100); for (int k = 0; k < 9; k++) c.send(&in[i], 1.0f, 100); }
    CHECK(finite(out)); CHECK(peakAbs(out) <= 1.0f + 1e-4f); }
}

TEST(chorus_goes_quiet_one_line_after_the_last_send_and_outputs_exact_zero) {
  Chorus c; c.init();
  const int N = 3000; std::vector<float> in = sine(0.5f, 440.0f, N), out(N, 0.f);
  runChorus(c, in, out, 0.8f, Chorus::kMaxChunk); CHECK(!c.quiet());
  int chunks = 0; std::vector<float> z(Chorus::kMaxChunk, 0.f), o(Chorus::kMaxChunk, 0.f);
  while (!c.quiet() && chunks < 100) { c.beginChunk(o.data(), Chorus::kMaxChunk); chunks++; }
  CHECK(c.quiet()); CHECK(chunks <= (Chorus::kLen + Chorus::kMaxChunk - 1) / Chorus::kMaxChunk + 1);
  std::fill(o.begin(), o.end(), 0.f); for (int i = 0; i < 20; i++) c.beginChunk(o.data(), Chorus::kMaxChunk);
  CHECK(peakAbs(o) == 0.0f);
  std::printf("  quiet after %d empty chunks of %d (line %d)\n", chunks, Chorus::kMaxChunk, Chorus::kLen);
  // A new send wakes it up and the earlier content is gone (the line was zeroed on the way): only the new impulse comes back.
  std::vector<float> imp(Chorus::kMaxChunk, 0.f), w(2000, 0.f); imp[0] = 0.5f;
  c.beginChunk(&w[0], Chorus::kMaxChunk); c.send(imp.data(), 1.0f, Chorus::kMaxChunk); CHECK(!c.quiet());
  for (int i = Chorus::kMaxChunk; i + Chorus::kMaxChunk <= 2000; i += Chorus::kMaxChunk) c.beginChunk(&w[i], Chorus::kMaxChunk);
  CHECK(finite(w)); CHECK_NEAR(sum(w, 0, 2000), 0.5, 0.005); CHECK(peakAbs(w, 0, 160) == 0.0f);
}

TEST(chorus_output_does_not_depend_on_the_chunk_size) {    // the engine's chunks vary with tempo and tick phase: 128 vs 37 samples agree
  const int N = 2 * kSampleRate; std::vector<float> in = sine(0.5f, 700.0f, N), a(N, 0.f), b(N, 0.f);
  Chorus ca; ca.init(); runChorus(ca, in, a, 1.0f, Chorus::kMaxChunk);
  Chorus cb; cb.init(); runChorus(cb, in, b, 1.0f, 37);
  float err = 0; for (int i = 0; i < N; i++) err = std::fmax(err, std::fabs(a[i] - b[i]));
  std::printf("  chunk 128 vs 37: max difference %.5f (the delay ramp is piecewise linear per chunk)\n", err);
  CHECK(err < 3e-3f); CHECK(peakAbs(a) > 0.2f);
  Chorus cc; cc.init(); std::vector<float> d(N, 0.f); runChorus(cc, in, d, 1.0f, 300);   // over the cap: clamped to kMaxChunk, memory-safe, still bounded
  CHECK(finite(d)); CHECK(peakAbs(d) <= 0.5f + 1e-3f);
}

int main() { return lp_test::run(); }
