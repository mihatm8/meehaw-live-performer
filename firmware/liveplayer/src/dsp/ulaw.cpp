// µ-law codec for the sample pool (SPEC §5). Standard G.711: bias 0x84, clip 32635, 8 segments of 16
// steps, in the 16-bit direct form (equal to Sun's g711.c on 14-bit input, but exact for small values).
// The same loop measured 250 ns per encode on the target; decode is one table lookup.
//
// The decode table is a constant initialiser (constexpr expansion of every code), so it is valid from
// the first instruction after reset: a SampleVoice::render, a host test or the desktop build can never
// read a zero-filled table because ulawInit() has not run yet. ulawInit() is kept as a no-op for API
// compatibility (liveplayer.ino and the tests still call it).
#include "ulaw.h"

namespace lp {
namespace {

// G.711 expansion of one (bit-inverted) code: sign bit, 3-bit segment e, 4-bit mantissa m.
constexpr int16_t ulawExpand(int code) {
  const int u = ~code & 0xFF;
  const int sign = u & 0x80, e = (u >> 4) & 7, m = u & 0x0F;
  const int s = (((m << 3) + 0x84) << e) - 0x84;  // segment start + mantissa, bias removed
  return (int16_t)(sign ? -s : s);
}

#define LP_ULAW8(b) ulawExpand(b), ulawExpand(b + 1), ulawExpand(b + 2), ulawExpand(b + 3), \
                    ulawExpand(b + 4), ulawExpand(b + 5), ulawExpand(b + 6), ulawExpand(b + 7)
#define LP_ULAW64(b) LP_ULAW8(b), LP_ULAW8(b + 8), LP_ULAW8(b + 16), LP_ULAW8(b + 24), \
                     LP_ULAW8(b + 32), LP_ULAW8(b + 40), LP_ULAW8(b + 48), LP_ULAW8(b + 56)

} // namespace

// Constant-initialised (no runtime constructor): every element is a constexpr call with a literal argument.
int16_t g_ulawTable[256] = { LP_ULAW64(0), LP_ULAW64(64), LP_ULAW64(128), LP_ULAW64(192) };

#undef LP_ULAW64
#undef LP_ULAW8

static_assert(ulawExpand(0xFF) == 0, "silence code");
static_assert(ulawExpand(0x80) == 32124, "largest positive code");
static_assert(ulawExpand(0x00) == -32124, "largest negative code");

void ulawInit() {}                                // no-op: the table is built at compile time (see above)

uint8_t ulawEncode(int16_t pcm) {
  int v = pcm, sign = 0;                          // int so that -32768 negates without overflow
  if (v < 0) { v = -v; sign = 0x80; }
  if (v > 32635) v = 32635;                       // 32635 + bias = 32767, the largest codable value
  v += 0x84;
  int e = 7;
  for (int mask = 0x4000; (v & mask) == 0 && e > 0; e--, mask >>= 1) {}   // highest set bit 7..14
  int m = (v >> (e + 3)) & 0x0F;
  return (uint8_t)~(sign | (e << 4) | m);         // G.711 stores the code bit-inverted
}

} // namespace lp
