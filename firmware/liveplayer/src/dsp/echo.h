// Tempo-synced mono echo bus on a µ-law uint8_t delay line (SPEC §5, §9.19 static reclaim).
#pragma once
#include <stdint.h>
#include "../core/types.h"
namespace lp {
class Echo {
 public:
  void init(uint8_t* line, int length);              // static buffer of kEchoSamples µ-law bytes (ulaw.h codec)
  void setTempo(float bpm);                          // recomputes delay samples from division
  void setDivision(EchoDiv d) { div_ = d; }
  void setFeedback(float fb) { feedback_ = fb; }
  void setDamping(float d) { damp_ = d; }            // 0..1 one-pole LP amount
  EchoDiv division() const { return div_; }
  // in: send bus (n samples), out: accumulates wet signal into out
  void process(const float* in, float* out, int n);
 private:
  uint8_t* line_ = nullptr; int len_ = 0; int write_ = 0; int delay_ = 11025; int target_ = 11025;
  EchoDiv div_ = Div8d; float feedback_ = 0.45f, damp_ = 0.4f, lp_ = 0.0f; float bpm_ = 120;
  int xfade_ = 0;                                    // samples left in the tap crossfade (0 = idle)
  float lpA_ = 0.0f, lpAFor_ = -1.0f;                // cached damping coefficient and the damp_ it was built for
};
}
