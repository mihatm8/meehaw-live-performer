// Exponential ADSR (per-sample), and AD envelope. Parameters set in seconds.
#pragma once
#include "../core/config.h"
namespace lp {
struct Adsr {
  enum Stage : uint8_t { Idle = 0, Attack, Decay, Sustain, Release };
  Stage stage = Idle; float level = 0.0f;
  float aInc = 0.01f, dCoef = 0.999f, sLevel = 0.7f, rCoef = 0.999f;
  void set(float a, float d, float s, float r);     // seconds, seconds, 0..1, seconds
  void gate(bool on) { if (on) stage = Attack; else if (stage != Idle) stage = Release; }
  void kill() { stage = Idle; level = 0; }
  bool active() const { return stage != Idle; }
  inline float process() {
    switch (stage) {
      case Attack: level += aInc; if (level >= 1.0f) { level = 1.0f; stage = Decay; } break;
      case Decay: level = sLevel + (level - sLevel) * dCoef; if (level - sLevel < 0.001f) stage = Sustain; break;
      case Sustain: level = sLevel; break;
      // Release reaches 1% at r (env.cpp); the voice is freed at -60 dB, i.e. 1.5 r from full level.
      case Release: level *= rCoef; if (level < 0.001f) { level = 0; stage = Idle; } break;
      default: break;
    }
    return level;
  }
};
struct AdEnv {                              // attack then exponential decay to 0
  float level = 0, aInc = 0.01f, dCoef = 0.999f; bool rising = false;
  void set(float a, float d);
  void trigger() { rising = true; }
  // The decay is floored at 1e-6 (-120 dB): a held note would otherwise drive level into denormals
  // (2e-43 after 5 s), which the ESP32-S3 FPU flushes but an x86 desktop build pays dearly for.
  inline float process() {
    if (rising) { level += aInc; if (level >= 1.0f) { level = 1.0f; rising = false; } }
    else { level *= dCoef; if (level < 1e-6f) level = 0.0f; }
    return level;
  }
};
}
