// Band-limited single-cycle wavetables, int16, stored in flash (const). The tables come in four pitch-ranged
// sets with decreasing harmonic counts and lengths (SPEC §9.15; tools/gen_wavetables.py): a partial above
// Nyquist at the played pitch would fold back into the band, so the synth picks each oscillator's set from its
// current pitch once per modulation block (synth_voice.cpp selectTables) and crossfades over a switch.
//   set 0: <= 40 partials, 2048 samples, MIDI <= 60   (alias-free up to MIDI 60: 40 x 261.6 Hz = 10.47 kHz)
//   set 1: <= 16 partials,  512 samples, MIDI 61..76  (16 x 659.3 Hz = 10.55 kHz)
//   set 2: <=  6 partials,  256 samples, MIDI 77..93  ( 6 x 1760 Hz = 10.56 kHz)
//   set 3: <=  3 partials,  128 samples, MIDI >= 94   (alias-free up to MIDI 105)
// Every table is followed by one guard sample (= sample 0) so waveSampleN reads t[i + 1] without a wrap mask.
// The sets together are 29 KB, small enough to stay resident in the ESP32-S3's 32 KB data cache.
#pragma once
#include <stdint.h>
#include "../core/types.h"
namespace lp {
constexpr int kWaveLen = 2048;             // length of the set-0 tables (the one-argument overloads below)
constexpr int kWaveShift = 11;             // log2(kWaveLen)
constexpr int kWaveSetCount = 4;           // table sets by pitch range
constexpr int kWaveSetHarmonics[kWaveSetCount] = {40, 16, 6, 3};   // highest partial present in each set
constexpr int kWaveSetMinPitch[kWaveSetCount] = {0, 61, 77, 94};   // first MIDI pitch each set serves
constexpr int kWaveSetShift[kWaveSetCount] = {11, 9, 8, 7};        // log2 of each set's table length
constexpr int kWaveSetTopPitch = 105;      // set 3 is alias-free up to here (3 x 3520 Hz = 10.56 kHz); above it
                                           // partial 3 (-9.5 dB) folds near Nyquist (osc 2 two octaves above a C7 lead)
// Set serving an integer pitch: the richest set whose top partial stays below Nyquist at that pitch (any int
// accepted; never out of range). Each set is alias-free up to half a semitone above its last pitch, so a
// fractional (glide) pitch may be rounded to the nearest integer before the lookup.
inline int waveSetForPitch(int midiPitch) {
  int s = 0;
  while (s + 1 < kWaveSetCount && midiPitch >= kWaveSetMinPitch[s + 1]) s++;
  return s;
}
inline int waveSetLen(int set) { return 1 << kWaveSetShift[set < 0 ? 0 : set >= kWaveSetCount ? kWaveSetCount - 1 : set]; }
const int16_t* waveTable(Wave w);          // set 0 (2048 samples; WFat = detuned saw stack baked in)
const int16_t* waveTable(Wave w, int midiPitch);   // the set whose partials stay below Nyquist at that pitch
const int16_t* waveTableSet(Wave w, int set);      // a given set (clamped to 0..kWaveSetCount-1)
// Phase accumulator helpers: phase is 32-bit; a table of 2^shift samples is indexed by phase >> (32 - shift).
// waveSample: a 2048-sample table (set 0, the LFO sine); the wrap is masked.
inline int16_t waveSample(const int16_t* t, uint32_t phase) {
  uint32_t i = phase >> (32 - kWaveShift); uint32_t fr = (phase >> (32 - kWaveShift - 8)) & 0xFF;
  int32_t a = t[i], b = t[(i + 1) & (kWaveLen - 1)];
  return (int16_t)(a + (((b - a) * (int32_t)fr) >> 8));
}
// waveSampleN: a table of 2^shift samples plus its guard sample (any set; shift = kWaveSetShift[set]). Same
// linear interpolation with an 8-bit fraction; no mask (the guard sample serves the last interval).
inline int16_t waveSampleN(const int16_t* t, uint32_t phase, int shift) {
  uint32_t i = phase >> (32 - shift); uint32_t fr = (phase << shift) >> 24;
  int32_t a = t[i], b = t[i + 1];
  return (int16_t)(a + (((b - a) * (int32_t)fr) >> 8));
}
uint32_t phaseIncForHz(float hz);          // (hz / kSampleRate) * 2^32
float midiToHz(float midi);
}
