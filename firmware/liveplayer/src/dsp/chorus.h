// meehaw live performer — per-set chorus bus (SPEC §9.22, research §5.3 "chorus"): one int16 modulated delay line per
// set, two taps a half LFO cycle apart, fed by each track's PChorus send after its strip (like the echo / reverb sends)
// and summed back into the set's mix. ≤ 700 B static per set, no allocation, no transcendental call per sample.
//
// Memory contract (why there is no send buffer): the engine has no spare 1 KB float buffer per set, so the tracks
// accumulate their sends straight into the line and the wet signal of a chunk is read BEFORE the chunk's input is
// written (read-before-write): every tap sits at least kMaxChunk samples back, so a chunk of n <= kMaxChunk samples
// only ever reads what earlier chunks wrote. The engine therefore renders in chunks of at most kMaxChunk samples
// (Engine::render caps the tick chunk; at 125 BPM a tick is 110 samples, so nothing changes at the default tempo).
// Protocol per chunk: beginChunk(mix, n) first (adds the wet signal into mix and clears the chunk's write region),
// then send(x, gain, n) once per track with a send (saturating int16 accumulate). A line nothing was sent to for a
// whole kLen samples is all zero and quiet(): beginChunk then costs two compares.
#pragma once
#include <stdint.h>
#include "../core/config.h"
namespace lp {
class Chorus {
 public:
  static constexpr int kLen = 331;                   // 15 ms at 22050 Hz (662 B of int16)
  static constexpr int kMaxChunk = 128;              // the engine never renders a chunk longer than this (read-before-write)
  static constexpr float kRateHz = 0.5f;             // LFO (research: 0.3–0.8 Hz)
  static constexpr float kCenter = 229.5f;           // mean tap delay, samples (10.4 ms)
  static constexpr float kDepth = 66.0f;             // ± 3 ms: taps sweep 163.5 .. 295.5 samples, always inside [kMaxChunk + 1, kLen − 1]
  static constexpr float kWet = 1.0f;                // wet level per unit send (the two taps are averaged)
  void init();                                       // zero line and state (SPEC §2: a set load resets effects)
  // Adds this chunk's wet signal into mix[0..n) (n <= kMaxChunk; larger is clamped), clears the write region, advances.
  void beginChunk(float* mix, int n);
  // Accumulates x[0..n) × gain into the chunk's write region (n beyond the chunk is ignored). Call after beginChunk.
  void send(const float* x, float gain, int n);
  bool quiet() const { return idle_ >= kLen; }      // nothing sent for a whole line: the line is silent
 private:
  int16_t line_[kLen];
  uint16_t w_ = 0, chunkStart_ = 0, chunkN_ = 0, idle_ = kLen;
  uint32_t lfoPh_ = 0;
};
static_assert(sizeof(Chorus) <= 700, "chorus bus must stay within 700 B per set (SPEC §9.22 memory budget)");
static_assert(Chorus::kCenter - Chorus::kDepth > Chorus::kMaxChunk + 1, "a tap must never read inside the chunk being written");
static_assert(Chorus::kCenter + Chorus::kDepth + 2 < Chorus::kLen, "a tap (and its interpolation neighbour) must stay inside the line");
}
