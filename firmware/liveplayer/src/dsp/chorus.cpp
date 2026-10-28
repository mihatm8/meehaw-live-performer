// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// meehaw live performer — per-set chorus bus on an int16 delay line (SPEC §9.22). See chorus.h for the contract.
//
// Two taps read the line at delays kCenter ± kDepth × lfo(φ) and kCenter ∓ kDepth × lfo(φ) (a half cycle apart, so
// their mean delay is constant and the pitch wobble of one tap is mirrored by the other: the classic two-voice
// chorus summed to mono). The LFO is a parabolic sine evaluated twice per chunk; each tap's delay ramps linearly
// across the chunk (at 0.5 Hz the LFO moves < 0.02 rad per 128 samples). Reads interpolate linearly between two
// line samples; the store path is a saturating int16 accumulate (NaN stores nothing, ±Inf saturate), so the line
// can never hold anything but a bounded number.
#include "chorus.h"
#include <string.h>

namespace lp {
namespace {

constexpr float kInvFs = 1.0f / 32768.0f;
constexpr float kToInt16 = 32767.0f;
constexpr uint32_t kLfoInc = (uint32_t)(Chorus::kRateHz * 4294967296.0 / (double)kSampleRate + 0.5);   // phase per sample

// Parabolic sine of a 32-bit phase: ±1 (16 t (0.5 − t) peaks at exactly 1 at t = 0.25), zero mean, continuous (max
// error ≈ 6 % of full scale against a true sine, invisible on an LFO).
inline float lfoOf(uint32_t ph) {
  const float t = (float)ph * (1.0f / 4294967296.0f);                                 // 0..1
  return t < 0.5f ? 16.0f * t * (0.5f - t) : -16.0f * (t - 0.5f) * (1.0f - t);
}
inline float delayOf(uint32_t ph) { return Chorus::kCenter + Chorus::kDepth * lfoOf(ph); }

} // namespace

void Chorus::init() {
  memset(line_, 0, sizeof line_);
  w_ = chunkStart_ = chunkN_ = 0; idle_ = (uint16_t)kLen; lfoPh_ = 0;
}

void Chorus::beginChunk(float* mix, int n) {
  if (n > kMaxChunk) n = kMaxChunk;
  if (n <= 0) return;
  chunkStart_ = w_; chunkN_ = (uint16_t)n;
  const uint32_t phEnd = lfoPh_ + kLfoInc * (uint32_t)n;
  if (idle_ < kLen) {                                                                // something was sent within the last line length
    // Two taps, delays ramped across the chunk. Read positions are kept positive by adding kLen before the floor.
    const float inv = 1.0f / (float)n;
    float dA = delayOf(lfoPh_), dB = delayOf(lfoPh_ + 0x80000000u);
    const float dAInc = (delayOf(phEnd) - dA) * inv, dBInc = (delayOf(phEnd + 0x80000000u) - dB) * inv;
    const int16_t* line = line_; const int w = (int)w_;
    for (int i = 0; i < n; i++) {
      const float base = (float)(w + i + kLen);
      float rA = base - dA; int iA = (int)rA; const float fA = rA - (float)iA; if (iA >= kLen) iA -= kLen; int jA = iA + 1; if (jA == kLen) jA = 0;
      float rB = base - dB; int iB = (int)rB; const float fB = rB - (float)iB; if (iB >= kLen) iB -= kLen; int jB = iB + 1; if (jB == kLen) jB = 0;
      const float a = (float)line[iA] + ((float)line[jA] - (float)line[iA]) * fA;
      const float b = (float)line[iB] + ((float)line[jB] - (float)line[iB]) * fB;
      mix[i] += (a + b) * (0.5f * kWet * kInvFs);
      dA += dAInc; dB += dBInc;
    }
    // Clear the region the tracks write this chunk (it held the samples from one line length ago, read above).
    const int first = kLen - w; if (n <= first) memset(line_ + w, 0, sizeof(int16_t) * (size_t)n);
    else { memset(line_ + w, 0, sizeof(int16_t) * (size_t)first); memset(line_, 0, sizeof(int16_t) * (size_t)(n - first)); }
  }
  lfoPh_ = phEnd;
  int w = (int)w_ + n; if (w >= kLen) w -= kLen; w_ = (uint16_t)w;
  const int idle = (int)idle_ + n; idle_ = (uint16_t)(idle > kLen ? kLen : idle);
}

void Chorus::send(const float* x, float gain, int n) {
  if (n > (int)chunkN_) n = chunkN_;
  if (n <= 0 || !(gain > 0.0f)) return;                                               // NaN gain sends nothing
  const float g = gain * kToInt16; int j = (int)chunkStart_;
  for (int i = 0; i < n; i++) {
    float s = (float)line_[j] + x[i] * g;
    if (!(s >= -32768.0f && s <= 32767.0f)) s = s > 0.0f ? 32767.0f : (s < 0.0f ? -32768.0f : (float)line_[j]);   // saturate; NaN keeps the line
    line_[j] = (int16_t)(int)s;
    if (++j == kLen) j = 0;
  }
  idle_ = 0;
}

} // namespace lp
