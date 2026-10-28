// Host-only stand-ins for the dsp-core module (wavetables.cpp, svf.cpp, env.cpp) used by test_synth while those
// files do not exist yet. run_synth.sh defines LP_STUB_WAVE / LP_STUB_SVF / LP_STUB_ENV only for the missing ones,
// so there are never duplicate symbols once the real files land. Tests may use anything (double, libm, ...).
#include <math.h>
#include "dsp/wavetables.h"
#include "dsp/svf.h"
#include "dsp/env.h"

namespace lp {

#ifdef LP_STUB_WAVE
namespace {
int16_t g_tabs[WaveCount][kWaveLen];
struct TableInit {
  TableInit() {
    for (int i = 0; i < kWaveLen; i++) {
      double ph = (double)i / kWaveLen;                     // 0..1
      double sine = sin(2.0 * M_PI * ph);
      double tri = ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
      double saw = 2 * ph - 1;
      double sq = ph < 0.5 ? 1 : -1;
      double fat = 0.6 * saw + 0.4 * (2 * fmod(ph * 1.0 + 0.37, 1.0) - 1);   // saw + offset copy, "fat-ish"
      g_tabs[WSine][i] = (int16_t)(sine * 32000); g_tabs[WTri][i] = (int16_t)(tri * 32000);
      g_tabs[WSaw][i] = (int16_t)(saw * 32000); g_tabs[WSquare][i] = (int16_t)(sq * 32000);
      g_tabs[WFat][i] = (int16_t)(fat * 32000);
    }
  }
} g_init;
}
const int16_t* waveTable(Wave w) { return g_tabs[w < WaveCount ? w : WSaw]; }
const int16_t* waveTable(Wave w, int) { return waveTable(w); }        // the stub has one (naive) set
uint32_t phaseIncForHz(float hz) { return (uint32_t)((double)hz / kSampleRate * 4294967296.0); }
float midiToHz(float midi) { return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f); }
#endif

#ifdef LP_STUB_SVF
float fastTanPi(float x) { return tanf(3.14159265f * x); }
#endif

#ifdef LP_STUB_ENV
// Times are "reach the stage's end threshold in t seconds": release hits the 0.0005 idle point (ln 2000 = 7.6)
// and decay closes its 0.001 gap (ln 1000 = 6.9) within the stated time.
static float coefFor(float seconds, float logRatio) {
  if (seconds < 0.001f) seconds = 0.001f;
  return expf(-logRatio / (seconds * (float)kSampleRate));
}
void Adsr::set(float a, float d, float s, float r) {
  if (a < 0.0005f) a = 0.0005f;
  aInc = 1.0f / (a * (float)kSampleRate);
  dCoef = coefFor(d, 6.9078f);
  sLevel = s < 0 ? 0 : s > 1 ? 1 : s;
  rCoef = coefFor(r, 7.6009f);
}
void AdEnv::set(float a, float d) {
  if (a < 0.0005f) a = 0.0005f;
  aInc = 1.0f / (a * (float)kSampleRate);
  dCoef = coefFor(d, 6.9078f);
}
#endif

} // namespace lp
