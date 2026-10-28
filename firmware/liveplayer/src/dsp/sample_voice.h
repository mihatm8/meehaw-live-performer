// One-shot sample voice reading µ-law bytes from the pool with linear interpolation, 2 ms attack / 12 ms release.
#pragma once
#include <stdint.h>
#include "../core/types.h"
namespace lp {
constexpr int kSampleReleaseSamples = (12 * kSampleRate + 999) / 1000;   // 12 ms fade = 265 output samples
class SampleVoice {
 public:
  void trigger(const uint8_t* data, uint32_t length, float gain, float rate = 1.0f);   // rate 1.0 = original
  // Fade out and stop within fadeSamples output samples (12 ms choke by default). The engine passes
  // kSlotFadeSamples (64) for a slot swap or retrigger (SPEC §9.1). A second call only ever makes the
  // fade steeper, and the fade never runs past the end of the slot's data.
  void release(int fadeSamples = kSampleReleaseSamples);
  bool active() const { return active_; }
  // accumulates n samples into acc
  void render(float* acc, int n);
 private:
  const uint8_t* data_ = nullptr; uint32_t len_ = 0; uint32_t pos_ = 0; uint32_t inc_ = 65536;
  float gain_ = 1.0f, env_ = 0.0f, envInc_ = 0.0f; bool active_ = false, releasing_ = false;
};
}
