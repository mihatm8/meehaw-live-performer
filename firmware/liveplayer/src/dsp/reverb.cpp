// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// meehaw live performer — mono Schroeder reverb bus (SPEC §5). Port of MiniAcid's DrumReverb
// (miniacid/src/dsp/drum_reverb.cpp, MIT) with every delay doubled so pads get a room, not a drum booth:
//   input one-pole HPF ~180 Hz → predelay 352 → 4 parallel damped combs (652 784 930 1058, ×0.25)
//   → 2 series allpasses (104, 158) → wet gain, accumulated into out.
// All delay memory is the engine's static int16 buffer of kMemory samples (4038 used).
// The int16 network runs at −12 dB (kInGain) with the ×4 makeup folded into kWet: a comb at resonance has a
// steady-state gain of 1/(1 − fb) (7.4× at the default decay, 14.3× at decay 1), so a sustained pad tone at a
// realistic send level would otherwise clip inside the feedback loop. Cost: the truncation floor of the tail
// rises from −87 to −75 dBFS at the return (−84 dBFS after the master trim, SPEC §9.11), still inaudible.
#include "reverb.h"
#include <math.h>
#include <string.h>
#include "../core/config.h"

namespace lp {
namespace {

constexpr float kPi = 3.14159265f;
constexpr int kComb[4] = {652, 784, 930, 1058};               // DrumReverb {326, 392, 465, 529} × 2
constexpr int kAllpass[2] = {104, 158};                        // {52, 79} × 2
constexpr int kPredelay = 352;                                 // 176 × 2 = 16 ms
constexpr int kOffComb[4] = {0, 652, 1436, 2366};
constexpr int kOffAllpass[2] = {3424, 3528};
constexpr int kOffPredelay = 3686;
constexpr int kUsed = kOffPredelay + kPredelay;                // 4038
static_assert(kUsed <= Reverb::kMemory, "reverb delay memory exceeds kMemory");

// Input high-pass ~180 Hz: y = a·(y + x − x1), a = exp(−2π·180/22050).
constexpr float kHpA = 0.95f;
// Input headroom into the int16 network (−12 dB) and its makeup, see the header comment.
constexpr float kInGain = 0.25f;
// Wet return = 2·sin(π/4) × 4: DrumReverb's wet path (×2 makeup, sin(mix·π/2)) at an equal-power 50 % mix,
// times the kInGain makeup. Measured IR energy at the return: 0.6 at decay 0.5 / tone 0.5, 0.9 at decay 1.
constexpr float kWet = 1.4142135f * 4.0f;
// The return is bounded: the allpass output is never stored, so |v1| can reach 1 + k(1 + k) ≈ 2.3 of the
// clamped ±1 network and the raw return ~13; dense full-scale input therefore goes through a hard ±2 clamp.
constexpr float kWetClamp = 2.0f;
// Anything outside this range on the send (NaN, ±Inf, a runaway bus) is replaced by 0 before the HPF, whose
// state would otherwise hold NaN forever (NaN compares false, so flush() cannot catch it).
constexpr float kInLimit = 8.0f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float rd16(const int16_t* m, int i) { return (float)m[i] * (1.0f / 32768.0f); }
inline int16_t q16(float v) {                                  // clamp ±1, truncate toward zero; NaN → 0 (defined, never UB)
  float s = v * 32767.0f;
  if (!(s >= -32768.0f && s <= 32767.0f)) s = s > 0.0f ? 32767.0f : (s < 0.0f ? -32768.0f : 0.0f);
  return (int16_t)(int)s;
}
inline void flush(float& z) { if (z > -1e-9f && z < 1e-9f) z = 0.0f; }

} // namespace

void Reverb::init(int16_t* mem) {
  mem_ = mem;
  if (mem_) memset(mem_, 0, sizeof(int16_t) * (size_t)kMemory);
  for (int i = 0; i < 6; i++) idx_[i] = 0;
  for (int i = 0; i < 4; i++) dampZ_[i] = 0.0f;
  preIdx_ = 0; hpZ_ = hpX_ = 0.0f;
  setDecay(kDefaultDecay); setTone(kDefaultTone);               // SPEC §2: (re)init = effect defaults
}

// decay 0..1 → comb feedback 0.5..0.93; allpass k 0.65..0.75 as in DrumReverb::updateDecay.
void Reverb::setDecay(float d) {
  float t = clampf(d, 0.0f, 1.0f);
  fb_ = 0.5f + 0.43f * t;
  apK_ = 0.65f + 0.10f * t;
}

// tone 0..1 → comb damping cutoff 1.2 kHz .. 9 kHz (log), one-pole coefficient 1 − e^(−2πfc/fs).
void Reverb::setTone(float t) {
  float u = clampf(t, 0.0f, 1.0f);
  float fc = 1200.0f * powf(9000.0f / 1200.0f, u);
  dampA_ = 1.0f - expf(-2.0f * kPi * fc / (float)kSampleRate);
}

void Reverb::process(const float* in, float* out, int n) {
  if (!mem_ || n <= 0) return;
  int16_t* m = mem_;
  const float fb = fb_, dampA = dampA_, k = apK_;
  int c0 = idx_[0], c1 = idx_[1], c2 = idx_[2], c3 = idx_[3], a0 = idx_[4], a1 = idx_[5], p = preIdx_;
  float z0 = dampZ_[0], z1 = dampZ_[1], z2 = dampZ_[2], z3 = dampZ_[3];
  float hpZ = hpZ_, hpX = hpX_;
  for (int i = 0; i < n; i++) {
    float x = in[i];
    if (!(x > -kInLimit && x < kInLimit)) x = 0.0f;                // false for NaN/Inf: sanitize before the HPF
    float hf = kHpA * (hpZ + x - hpX); hpX = x; hpZ = hf;         // input HPF
    float revIn = rd16(m, kOffPredelay + p);                       // predelay
    m[kOffPredelay + p] = q16(hf * kInGain); if (++p == kPredelay) p = 0;   // −12 dB into the int16 network
    // four parallel combs with one-pole damping in the feedback (DrumReverb::process)
    float t0 = rd16(m, kOffComb[0] + c0); z0 += dampA * (t0 - z0); m[kOffComb[0] + c0] = q16(revIn + fb * z0); if (++c0 == kComb[0]) c0 = 0;
    float t1 = rd16(m, kOffComb[1] + c1); z1 += dampA * (t1 - z1); m[kOffComb[1] + c1] = q16(revIn + fb * z1); if (++c1 == kComb[1]) c1 = 0;
    float t2 = rd16(m, kOffComb[2] + c2); z2 += dampA * (t2 - z2); m[kOffComb[2] + c2] = q16(revIn + fb * z2); if (++c2 == kComb[2]) c2 = 0;
    float t3 = rd16(m, kOffComb[3] + c3); z3 += dampA * (t3 - z3); m[kOffComb[3] + c3] = q16(revIn + fb * z3); if (++c3 == kComb[3]) c3 = 0;
    float y = (t0 + t1 + t2 + t3) * 0.25f;
    // two series allpasses: v = −k·y + buf; store y + k·v
    float b0 = rd16(m, kOffAllpass[0] + a0); float v0 = b0 - k * y; m[kOffAllpass[0] + a0] = q16(y + k * v0); if (++a0 == kAllpass[0]) a0 = 0;
    float b1 = rd16(m, kOffAllpass[1] + a1); float v1 = b1 - k * v0; m[kOffAllpass[1] + a1] = q16(v0 + k * v1); if (++a1 == kAllpass[1]) a1 = 0;
    float w = kWet * v1;                                           // makeup folded in; hard-bounded return
    if (w > kWetClamp) w = kWetClamp; else if (w < -kWetClamp) w = -kWetClamp;
    out[i] += w;
  }
  idx_[0] = c0; idx_[1] = c1; idx_[2] = c2; idx_[3] = c3; idx_[4] = a0; idx_[5] = a1; preIdx_ = p;
  flush(z0); flush(z1); flush(z2); flush(z3); flush(hpZ);          // keep decayed states out of denormals
  dampZ_[0] = z0; dampZ_[1] = z1; dampZ_[2] = z2; dampZ_[3] = z3;
  hpZ_ = hpZ; hpX_ = hpX;
}

} // namespace lp
