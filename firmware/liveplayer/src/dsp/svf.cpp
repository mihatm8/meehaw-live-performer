// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// Fast tan(pi*x) for per-block SVF cutoff modulation (SPEC §5; tanf costs 1.6 µs on the target, §1).
#include "svf.h"

namespace lp {

// Rational form pi*x*(1 + a x^2 + c x^4) / (1 - 4 x^2): the denominator carries the pole at x = 0.5
// exactly, a and c were fitted numerically (minimax on 0.001..0.45): max relative error 7.4e-5 on
// that range and 3e-5 at x = 0.49. One division and five multiplies.
float fastTanPi(float x) {
  // Both clamps are written so that NaN takes the clamp branch (NaN compares false against anything):
  // a NaN cutoff from a bad patch or an exp2 overflow must not poison the SVF coefficients for the
  // rest of the note (synth_voice writes g/a1/a2/a3 from this value every modulation block).
  if (!(x > 1e-5f)) x = 1e-5f;                    // negative, zero, denormal or NaN -> smallest cutoff
  if (x > 0.49f) x = 0.49f;                       // +inf lands here too
  const float x2 = x * x;
  return 3.14159265f * x * (1.0f - (0.7075f + 0.2f * x2) * x2) / (1.0f - 4.0f * x2);
}

} // namespace lp
