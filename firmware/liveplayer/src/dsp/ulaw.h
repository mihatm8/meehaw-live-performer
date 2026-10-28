// µ-law 8-bit codec for the sample pool (SPEC §5). Decode is a 256-entry table lookup.
#pragma once
#include <stdint.h>
namespace lp {
void ulawInit();                                   // builds the decode table (call once at boot)
uint8_t ulawEncode(int16_t pcm);
extern int16_t g_ulawTable[256];
inline int16_t ulawDecode(uint8_t u) { return g_ulawTable[u]; }
}
