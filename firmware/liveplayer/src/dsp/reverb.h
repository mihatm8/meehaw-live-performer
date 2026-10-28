// Schroeder reverb (MiniAcid DrumReverb topology, combs ×2 for a roomier tail), int16 delay memory, mono.
#pragma once
#include <stdint.h>
namespace lp {
class Reverb {
 public:
  static constexpr int kMemory = 4096;              // int16 samples total across combs/allpasses
  static constexpr float kDefaultDecay = 0.85f;     // init() applies these (SPEC §2: a set load resets effects):
  static constexpr float kDefaultTone = 0.6f;       // a ~1 s pad room (RT60), damping ~4 kHz; not a drum booth
  void init(int16_t* mem);                          // static buffer of kMemory int16; resets decay/tone to the defaults
  void setDecay(float d);                           // 0..1
  void setTone(float t);                            // 0..1 damping brightness
  void process(const float* in, float* out, int n); // accumulates wet into out
 private:
  int16_t* mem_ = nullptr; int idx_[6] = {0}; float fb_ = 0.8655f, dampA_ = 0.682f, dampZ_[4] = {0};   // = kDefaultDecay/kDefaultTone
  float hpZ_ = 0, hpX_ = 0;
  int preIdx_ = 0; float apK_ = 0.7f;               // predelay write index, allpass coefficient
};
}
