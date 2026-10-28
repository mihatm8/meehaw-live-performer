// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// One-shot sample voice (SPEC §5). Reads µ-law bytes from the pool with a 16.16 fixed-point position
// and linear interpolation: the inner loop is the probe's "sampler ulaw lerp" benchmark (200 ns per
// sample, §1) plus one float multiply for gain * envelope. 2 ms linear attack; linear release started
// by release() (12 ms choke by default, 64 samples for slot swaps / retriggers, SPEC §9.1) or
// automatically so the fade ends exactly at the end of the slot.
#include "sample_voice.h"
#include "ulaw.h"
#include "../core/config.h"
#include <math.h>

namespace lp {
namespace {

constexpr float kAttackSamples = 0.002f * (float)kSampleRate;                 // 44.1
constexpr float kReleaseInc = 1.0f / (0.012f * (float)kSampleRate);           // per output sample
constexpr uint32_t kReleaseSamples = (uint32_t)kSampleReleaseSamples;         // 265
constexpr float kPcmToFloat = 1.0f / 32767.0f;                                // int16 full scale = 1.0
constexpr uint32_t kMaxLen = 65535;                                           // 16.16 position fits 32 bits
constexpr float kMinRate = 1.0f / 64.0f, kMaxRate = 64.0f;
// A fade is over once the envelope is below -80 dB (under the µ-law step): with the slope 1/fadeSamples the
// float sum of fadeSamples steps lands within ~1e-5 of zero either way, so the fade length is exact.
constexpr float kEnvEnd = 1e-4f;

inline uint32_t endPosOf(uint32_t len) { return (len - 1) << 16; }           // interpolation reads idx + 1

// Per-sample slope that takes env to zero no slower than minSlope and no later than the slot's end
// (shared by release() and the natural-end fade, so a short slot never ends in a hard cut). The
// samples still readable are those with pos < endPos: ceil((endPos - pos) / inc); the last of them
// is output at env / remaining, one slope step above silence.
inline float fadeSlope(float env, float minSlope, uint32_t endPos, uint32_t pos, uint32_t inc) {
  float remaining = endPos > pos ? ceilf((float)(endPos - pos) / (float)inc) : 1.0f;
  if (remaining < 1.0f) remaining = 1.0f;
  const float toEnd = env / remaining;
  return toEnd > minSlope ? toEnd : minSlope;
}

} // namespace

void SampleVoice::trigger(const uint8_t* data, uint32_t length, float gain, float rate) {
  if (data == nullptr || length < 2) { active_ = false; return; }
  if (length > kMaxLen) length = kMaxLen;
  if (!(rate > kMinRate)) rate = kMinRate;        // also catches NaN
  if (rate > kMaxRate) rate = kMaxRate;
  data_ = data; len_ = length; pos_ = 0; inc_ = (uint32_t)(rate * 65536.0f);
  gain_ = gain; env_ = 0.0f; envInc_ = 1.0f / kAttackSamples;
  active_ = true; releasing_ = false;
}

void SampleVoice::release(int fadeSamples) {
  if (!active_) return;
  if (fadeSamples < 1) fadeSamples = 1;
  // 1/fadeSamples from full level (shorter from a lower one), capped by the samples left in the slot.
  const float slope = fadeSlope(env_, 1.0f / (float)fadeSamples, endPosOf(len_), pos_, inc_);
  if (releasing_ && -envInc_ >= slope) return;     // already fading at least this fast: keep that fade
  releasing_ = true; envInc_ = -slope;
}

void SampleVoice::render(float* acc, int n) {
  if (!active_) return;
  const uint32_t endPos = endPosOf(len_);
  const uint32_t tail = kReleaseSamples * inc_;   // slot positions covered by a 12 ms output fade
  const uint32_t releaseAt = endPos > tail ? endPos - tail : 0;
  const float scale = gain_ * kPcmToFloat;
  const uint8_t* d = data_;
  for (int i = 0; i < n; i++) {
    if (pos_ >= endPos) { active_ = false; return; }
    if (!releasing_ && env_ >= 1.0f && pos_ >= releaseAt) {
      // Natural end: fade over the remaining samples, never slower than the 12 ms slope. Waiting for
      // the attack to finish keeps slots shorter than 14 ms audible (a steeper fade instead of silence).
      releasing_ = true; envInc_ = -fadeSlope(env_, kReleaseInc, endPos, pos_, inc_);
    }
    const uint32_t idx = pos_ >> 16;
    const int32_t fr = (int32_t)(pos_ & 0xFFFF);
    const int32_t s0 = g_ulawTable[d[idx]], s1 = g_ulawTable[d[idx + 1]];
    const int32_t s = s0 + (((s1 - s0) * fr) >> 16);
    acc[i] += (float)s * (scale * env_);
    env_ += envInc_;
    if (releasing_) { if (env_ <= kEnvEnd) { env_ = 0.0f; active_ = false; return; } }
    else if (env_ >= 1.0f) { env_ = 1.0f; envInc_ = 0.0f; }
    pos_ += inc_;
  }
}

} // namespace lp
