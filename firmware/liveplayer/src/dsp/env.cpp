// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// Envelope coefficients (SPEC §5, amp ADSR and filter AD). Attack is a linear increment that reaches
// 1.0 in `a` seconds. Decay is a per-sample exponential coefficient that covers 60 dB (x0.001) of the
// distance to the sustain level in `d` seconds, matching the 0.001 stage-end threshold in env.h.
// Release is a per-sample exponential coefficient that reaches 1% (x0.01, -40 dB) of the level at
// note-off in `r` seconds; env.h frees the voice at 0.001 (-60 dB), i.e. at 1.5 r from full level,
// earlier from a lower sustain. The filter AD decay uses the 60 dB constant like the ADSR decay.
// All times are clamped to >= 1 ms so coefficients stay finite and every stage terminates.
#include "env.h"
#include <math.h>

namespace lp {
namespace {

constexpr float kMinSeconds = 0.001f;
constexpr float kLn1000 = 6.907755f;              // -ln(0.001): 60 dB, decay stages
constexpr float kLn100 = 4.605170f;               // -ln(0.01): 1% of the distance, release stage

inline float clampTime(float s) { return s < kMinSeconds ? kMinSeconds : s; }
inline float attackInc(float a) { return 1.0f / (clampTime(a) * (float)kSampleRate); }
inline float expCoef(float seconds, float ln) { return expf(-ln / (clampTime(seconds) * (float)kSampleRate)); }

} // namespace

void Adsr::set(float a, float d, float s, float r) {
  aInc = attackInc(a);
  dCoef = expCoef(d, kLn1000);
  sLevel = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
  rCoef = expCoef(r, kLn100);
}

void AdEnv::set(float a, float d) {
  aInc = attackInc(a);
  dCoef = expCoef(d, kLn1000);
}

} // namespace lp
