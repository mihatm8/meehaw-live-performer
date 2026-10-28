// Host-only stand-in for src/dsp/ulaw.cpp (another module) so the io tests link. run_io.sh compiles the
// real file instead as soon as it exists. Standard G.711 µ-law on 16-bit PCM (bias 0x84, clip 32635).
#include "dsp/ulaw.h"

namespace lp {

int16_t g_ulawTable[256];

namespace {
int16_t decode(uint8_t u) {
  u = (uint8_t)~u;
  int sign = u & 0x80, exponent = (u >> 4) & 7, mantissa = u & 0x0F;
  int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
  return (int16_t)(sign ? -sample : sample);
}
}

void ulawInit() { for (int i = 0; i < 256; i++) g_ulawTable[i] = decode((uint8_t)i); }

uint8_t ulawEncode(int16_t pcm) {
  int v = pcm;
  int sign = 0;
  if (v < 0) { v = -v; sign = 0x80; }
  if (v > 32635) v = 32635;
  v += 0x84;
  int exponent = 7;
  for (int mask = 0x4000; (v & mask) == 0 && exponent > 0; mask >>= 1) exponent--;
  int mantissa = (v >> (exponent + 3)) & 0x0F;
  return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

} // namespace lp
