// Cheap waveshapers and limiters (SPEC §5). No transcendental call per sample (distMakeup's sqrtf runs once per
// parameter change in the engine, never in a sample loop).
#pragma once
#include <stdint.h>
#include <math.h>
#include "../core/config.h"
namespace lp {
// Soft clip: linear below ±0.5, cubic knee, hard at ±1. ~40 ns.
inline float softClip(float x) {
  if (x > 1.5f) return 1.0f;
  if (x < -1.5f) return -1.0f;
  return x - (x * x * x) * (4.0f / 27.0f);   // x*(1 - x²·4/27): unity at 0, 1.0 at x=1.5
}
// Synth voice saturation: drive 1..8 then soft clip, with makeup so loudness stays roughly constant.
inline float distort(float x, float drive) { return softClip(x * drive) * (1.0f / (0.35f + 0.65f * drive)); }
// Strip distortion (SPEC §9.17a): dist 0..1 → drive 1..32, output = softClip(x × drive) × distMakeup(drive). The
// makeup is 1/sqrt(drive) up to drive 8 (dist 0.226) and plateaus there: below the knee small signals gain sqrt(drive)
// while a fully clipped signal settles at 1/sqrt(8) = 0.354; above it the clipped level stays put and the small-signal
// gain keeps rising (×11.3 at dist 1), so more dist only ever crushes harder and gets louder, never quieter (a pure
// 1/sqrt(drive) made dist 1 2.5 dB quieter than dist 0.3, and the old 1 + 7 × dist formula clipped at 0.18). The
// engine evaluates both once per parameter change (smoothParams) and the per-sample strip loop multiplies only.
constexpr float kDistMakeupKnee = 8.0f;
inline float distDrive(float dist) { return 1.0f + 31.0f * (dist < 0 ? 0 : dist > 1 ? 1 : dist); }
inline float distMakeup(float drive) { return 1.0f / sqrtf(drive < 1.0f ? 1.0f : drive > kDistMakeupKnee ? kDistMakeupKnee : drive); }

struct Limiter {                           // peak limiter: instant-ish attack, exponential release
  float gain = 1.0f, threshold = 0.95f, release = 0.0f;  // release coefficient per sample
  void set(float thr, float releaseMs) { threshold = thr; release = 1.0f - 1.0f / (releaseMs * 0.001f * kSampleRate + 1.0f); }
  inline float process(float x) {
    float a = x < 0 ? -x : x;
    float want = a * gain > threshold ? threshold / (a + 1e-9f) : 1.0f;
    if (want < gain) gain = want; else gain += (1.0f - gain) * (1.0f - release);
    return x * gain;
  }
};
struct Ducker {                            // kick ducking: −4.5 dB, 3 ms attack, 120 ms release
  float env = 0.0f, depth = 0.5957f /*10^(-4.5/20)*/, atk = 0, rel = 0; bool enabled = true;
  void init() { atk = 1.0f / (0.003f * kSampleRate); rel = 1.0f / (0.120f * kSampleRate); }
  void trigger() { target_ = 1.0f; }
  inline float gain() {                    // call once per sample, returns multiplier
    if (target_ > env) { env += atk; if (env > 1.0f) { env = 1.0f; target_ = 0.0f; } }
    else if (env > 0.0f) { env -= rel; if (env < 0.0f) env = 0.0f; }
    return enabled ? 1.0f - env * (1.0f - depth) : 1.0f;
  }
 private:
  float target_ = 0.0f;
};
}
