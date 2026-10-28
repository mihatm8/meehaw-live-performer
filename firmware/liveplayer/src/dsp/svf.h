// TPT (Zavalishin) state-variable filter, float, per-sample. ~130 ns/sample measured.
#pragma once
#include <math.h>
#include "../core/config.h"
namespace lp {
struct Svf {
  float z1 = 0, z2 = 0, g = 0.1f, k = 1.0f, a1 = 0, a2 = 0, a3 = 0;
  uint8_t mode = 0;                        // 0 LP 1 BP 2 HP
  void set(float cutoffHz, float resonance /*0..0.95*/, uint8_t m) {
    if (cutoffHz < 20.f) cutoffHz = 20.f;
    if (cutoffHz > 10500.f) cutoffHz = 10500.f;
    g = tanf(3.14159265f * cutoffHz / (float)kSampleRate);
    k = 2.0f - 1.9f * (resonance < 0 ? 0 : resonance > 0.95f ? 0.95f : resonance);
    a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2; mode = m;
  }
  inline float process(float x) {
    float v3 = x - z2; float v1 = a1 * z1 + a2 * v3; float v2 = z2 + a2 * z1 + a3 * v3;
    z1 = 2 * v1 - z1; z2 = 2 * v2 - z2;
    return mode == 0 ? v2 : mode == 1 ? v1 : x - k * v1 - v2;
  }
  void reset() { z1 = z2 = 0; }
};
// Fast tan approximation for per-block cutoff modulation (avoid tanf per sample).
float fastTanPi(float x);                  // tan(pi*x) for x in (0, 0.49)
}
