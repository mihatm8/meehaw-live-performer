// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// meehaw live performer — tempo-synced mono echo bus on a µ-law uint8_t delay line (SPEC §5, §9.19).
//
// delay = division beats × 60 / bpm × 22050 samples; a division longer than the line folds down by
// halving until it fits. The read tap crossfades to a new delay over ~20 ms so tempo/division changes
// never click. Feedback goes through a one-pole low-pass (damping): the line stores in + fb × damped,
// the wet output is the undamped read. All memory is the static line handed in by the engine.
//
// The line holds one µ-law byte per sample (SPEC §9.19 static reclaim: 10 240 B instead of 20 480 B of int16;
// the wet path only — the dry signal never passes through it). G.711 µ-law (dsp/ulaw.h) is logarithmic: the
// relative quantisation error is ≈ 3–6 % per sample whatever the level, full scale decodes to 32124 / 32768
// (−0.17 dB) and every magnitude below 8 LSB (2.4e-4) decodes to exact 0. The store path truncates toward
// zero twice (float → int16 → µ-law mantissa), so with feedback < 1 the tail always reaches exact silence,
// as the int16 line did (tests/test_fx.cpp echo_silent_after_input_stops_all_divisions).
#include "echo.h"
#include <math.h>
#include <string.h>
#include "../core/config.h"
#include "ulaw.h"

namespace lp {
namespace {

constexpr float kPi = 3.14159265f;
constexpr int kXfadeSamples = 441;                           // 20 ms at 22050 Hz
constexpr float kInvXfade = 1.0f / (float)kXfadeSamples;
constexpr float kDampHiHz = 8000.0f, kDampLoHz = 1500.0f;   // damping 0 → 8 kHz, 1 → 1.5 kHz (log)

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// µ-law ↔ float. Truncation toward zero on store guarantees the feedback tail reaches exact zero: the float is cut
// to int16 toward zero, and because G.711 decodes a code to the MIDPOINT of its bin (a value in the lower half of a
// bin would read back larger than it went in — 0.5 → 0.5116, and at feedback 0.98 an 8-LSB tail could then sustain
// itself) a code whose decode exceeds the stored magnitude is stepped one bin toward zero, so |read| ≤ |stored| for
// every sample and a tail under feedback < 1 shrinks every pass until it is below 8 LSB (the smallest non-zero bin)
// and is stored as silence. The clamp is written so that NaN takes a defined branch (stored as silence) instead of
// the UB float→int cast; ±Inf saturate. This is what keeps a NaN on the send bus from ever entering the line.
// Reads are one table lookup (ulawDecode); a store is the encode, one lookup and a compare.
inline float rdU(const uint8_t* line, int i) { return (float)ulawDecode(line[i]) * (1.0f / 32768.0f); }
inline uint8_t qU(float v) {
  float s = v * 32767.0f;
  if (!(s >= -32768.0f && s <= 32767.0f)) s = s > 0.0f ? 32767.0f : (s < 0.0f ? -32768.0f : 0.0f);
  const int pcm = (int)s; const int mag = pcm < 0 ? -pcm : pcm;
  uint8_t c = ulawEncode((int16_t)pcm);
  const int d = ulawDecode(c);
  if ((d < 0 ? -d : d) > mag) { const uint8_t u = (uint8_t)~c; c = (uint8_t)~((u & 0x80) | ((u & 0x7F) - 1)); }   // the code's magnitude index is never 0 here (index 0 decodes to 0)
  return c;
}

// Delay in samples for a division at a tempo, folded down to fit a line of len samples (SPEC §5).
int delayFor(float bpm, EchoDiv div, int len) {
  float d = echoDivBeats(div) * (60.0f / bpm) * (float)kSampleRate;
  for (int k = 0; k < 16 && d > (float)len; k++) d *= 0.5f;     // halve the division until it fits
  int di = (int)(d + 0.5f);
  if (di < 1) di = 1;
  if (di > len) di = len;
  return di;
}

// One-pole LP coefficient for the damping amount: cutoff slides log-wise from 8 kHz down to 1.5 kHz.
float dampCoef(float damp) {
  float t = clampf(damp, 0.0f, 1.0f);
  float fc = kDampHiHz * powf(kDampLoHz / kDampHiHz, t);
  return 1.0f - expf(-2.0f * kPi * fc / (float)kSampleRate);
}

} // namespace

void Echo::init(uint8_t* line, int length) {
  line_ = line; len_ = length > 0 ? length : 0; write_ = 0; lp_ = 0.0f; xfade_ = 0;
  if (line_ && len_ > 0) memset(line_, ulawEncode(0), (size_t)len_);                 // µ-law silence is the code 0xFF, not 0
  delay_ = target_ = len_ > 0 ? delayFor(bpm_, div_, len_) : 1;
  lpA_ = dampCoef(damp_); lpAFor_ = damp_;
}

// Only the tempo is stored. process() derives the delay from bpm_/div_ when no crossfade is running, so a
// call that lands mid-fade (SPEC §9.12 tempo ramps call this every tick, CmdEchoDivision right after
// setDivision) can never move the destination tap of a running fade, which would be a step in the output.
void Echo::setTempo(float bpm) {
  bpm_ = clampf(bpm, (float)kMinBpm, (float)kMaxBpm);         // SPEC §2: 40–240
}

void Echo::process(const float* in, float* out, int n) {
  if (!line_ || len_ <= 0 || n <= 0) return;
  if (damp_ != lpAFor_) { lpA_ = dampCoef(damp_); lpAFor_ = damp_; }   // setDamping is inline in the header
  float fb = clampf(feedback_, 0.0f, 0.98f);
  if (lp_ > -1e-9f && lp_ < 1e-9f) lp_ = 0.0f;                        // flush the damping state when idle
  // Pick up a new delay (tempo or division) only when no crossfade is running, so taps never jump. A fade
  // is 441 samples (< 2 blocks), so a ramping tempo is followed in ~20 ms steps, each one crossfaded.
  if (xfade_ == 0) {
    int want = delayFor(bpm_, div_, len_);
    if (want != delay_) { target_ = want; xfade_ = kXfadeSamples; }
  }
  uint8_t* line = line_; const int len = len_;
  int w = write_;
  for (int i = 0; i < n; i++) {
    float x = in[i];
    int ra = w - delay_; if (ra < 0) ra += len;
    float d = rdU(line, ra);
    if (xfade_ > 0) {                                                    // ~20 ms linear crossfade old → new tap
      int rb = w - target_; if (rb < 0) rb += len;
      float t = 1.0f - (float)xfade_ * kInvXfade;
      d += (rdU(line, rb) - d) * t;
      if (--xfade_ == 0) delay_ = target_;
    }
    lp_ += lpA_ * (d - lp_);                                             // one-pole damping in the feedback path
    line[w] = qU(x + fb * lp_);                                          // clamp, int16, then µ-law (truncation toward zero twice)
    if (++w == len) w = 0;
    out[i] += d;                                                         // accumulate the wet signal
  }
  write_ = w;
}

} // namespace lp
