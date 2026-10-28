// One polyphonic synth voice: 2 wavetable oscillators + sub + noise, drive, TPT SVF, ADSR, filter AD, LFO, glide.
// Measured budget ≈ 220 ns/sample. The patch is copied at noteOn (osc 2, sub, noise, envelopes, LFO, glide, level:
// a later patch change reaches only the next note), the live knobs (SPEC §9.17a: cutoff, resonance, filter env,
// filter decay, drive, wave 1; SPEC §9.22: sat, accent) are followed every modulation block, so a knob edit,
// CmdSetPatch or a track reset changes a sounding note's filter and drive at once.
// 303 character (SPEC §9.22, research §5): SAT saturates the band-pass state inside the TPT SVF loop (the MiniAcid
// squelch: softClip(z1 × g) / g with g = kSatMax × sat, a cubic knee whose depth grows with sat², hard at ±1.5 / g;
// sat 0 runs today's clean kernel bit-exactly). ACC: a note with velocity >= 100 is accented: its filter envelope
// amount is × (1 + accent) and its level × (1 + 0.4125 × accent) (+3 dB at accent 1), both following the knob live.
#pragma once
#include "../core/types.h"
#include "svf.h"
#include "env.h"
#include "wavetables.h"
namespace lp {
class SynthVoice {
 public:
  void init();
  void setPatch(const SynthPatch* p) { patch_ = p; }
  // Live knobs (SPEC §9.17): when set, the modulation block takes cutoff / resonance / filter env / decay / drive /
  // wave1 from here instead of the patch (read every 32 samples, never per sample). nullptr = patch only.
  void setKnobs(const SynthKnobs* k) { knobs_ = k; }
  // Every noteOn re-reads ALL parameters from the current patch (level × velocity slews over one 32-sample block,
  // so a retrigger or steal never steps the gain). legato = true on a sounding voice keeps the envelope stage and
  // level (a releasing envelope is re-gated, so the note stays alive), the oscillator phases, the filter state and
  // the current glide position, and glides to the new pitch over patch.glide seconds (min 5 ms). A non-legato
  // noteOn retriggers the envelopes; it slides from the previous pitch only when the voice is still sounding
  // (steal) or the patch is mono (polyphony 1, 303 style); a fresh polyphonic voice starts on pitch.
  void noteOn(uint8_t pitch, uint8_t vel, bool legato = false);
  void noteOff();
  void kill() { amp_.kill(); active_ = false; }
  bool active() const { return active_; }
  uint8_t pitch() const { return pitch_; }
  uint32_t age() const { return age_; }                    // samples since noteOn (voice stealing)
  bool releasing() const { return amp_.stage == Adsr::Release; }
  // Renders n samples, accumulating into acc (float). Returns false when the voice became idle.
  bool render(float* acc, int n);
 private:
  const SynthPatch* patch_ = nullptr; const SynthKnobs* knobs_ = nullptr;
  bool active_ = false; uint8_t pitch_ = 60; uint32_t age_ = 0;
  float vel_ = 1.0f;                                       // patch level × velocity curve of the note: gainTarget_ = vel_ × the accent gain (SPEC §9.22)
  uint32_t ph1_ = 0, ph2_ = 0, phSub_ = 0, lfoPh_ = 0; uint32_t noise_ = 22222;
  float curHz_ = 440.f, targetHz_ = 440.f, glideCoef_ = 0.f;
  Svf svf_; Adsr amp_; AdEnv fenv_;
  float baseCutoff_ = 2000.f; int blockCount_ = 0;
  // Derived from the patch at noteOn (a later patch change cannot affect these on a sounding note, SPEC §5), except
  // what the live knobs own: baseCutoff_, svf_.k, fEnvOct_, the filter AD decay, drive_ / driveMk_ and wave1_ follow
  // knobs_ per modulation block (SPEC §9.17a), and the engine rewrites the knobs at once on CmdSetPatch / reset. The
  // oscillator tables are re-selected once per modulation block from the pitch each oscillator plays
  // (wavetables.h sets, SPEC §9.15): tab*_ / sh*_ are the current table and its log2 length, tab*Old_ / sh*Old_
  // the previous one while a one-block crossfade (xfade_, xfPos_) blends from it after a switch.
  const int16_t* tab1_ = nullptr; const int16_t* tab2_ = nullptr; const int16_t* sineTab_ = nullptr;
  uint32_t inc1_ = 0, inc2_ = 0, incSub_ = 0, lfoInc_ = 0;
  float osc2Ratio_ = 1.f, osc2Lvl_ = 0.f, subLvl_ = 0.f, noiseLvl_ = 0.f, drive_ = 0.f;
  float fEnvOct_ = 0.f, lfoCut_ = 0.f, lfoPitch_ = 0.f, gain_ = 0.f;
  bool primed_ = false;                                    // false until the first note (no glide from nowhere)
  bool accent_ = false;                                    // this note was accented (vel >= 100, SPEC §9.22): env × (1 + accent), level + up to 3 dB
  const int16_t* tabSub_ = nullptr;                        // band-limited square table for the sub oscillator
  float curSt_ = 69.f, targetSt_ = 69.f;                   // glide slews in semitones (linear in pitch, 303 style)
  float invPeak_ = 1.0f / 32768.0f;                        // 1 / wavetable peak: oscillators and LFO span ±1.0
  float gainTarget_ = 0.f, gainInc_ = 0.f;                 // gain_ slews to gainTarget_ over one modulation block
  float fAtk32_ = 0.f, fDec32_ = 1.f;                      // filter AD stepped once per modulation block
  uint32_t seed_ = 22222;                                  // noise seed base: fixed by init order, reused per note
  const int16_t* tab1Old_ = nullptr; const int16_t* tab2Old_ = nullptr; const int16_t* tabSubOld_ = nullptr;
  uint8_t sh1_ = kWaveShift, sh2_ = kWaveShift, shSub_ = kWaveShift;
  uint8_t sh1Old_ = kWaveShift, sh2Old_ = kWaveShift, shSubOld_ = kWaveShift;
  uint8_t wave1_ = WSaw, wave2_ = WSaw;                    // oscillator waves of the sounding note
  bool xfade_ = false; float xfPos_ = 0.f;                 // crossfade running this block, and its position 0..1
  float osc2St_ = 0.f, lfoPitchAbs_ = 0.f;                 // osc 2 offset (st) and the LFO's pitch excursion (st)
  // Live knobs (SPEC §9.17a): keyFactor_ = 2^(keyTrack × (pitch − 60) / 12), so base cutoff = knob cutoff × keyFactor_
  // every block; the knob* copies are the values last applied (resonance → svf_.k, decay → AdEnv coefficients, drive →
  // waveshaper), compared once per modulation block so the expensive re-sets only run when a knob actually moved.
  float keyFactor_ = 1.f, knobRes_ = -1.f, knobDecayS_ = -1.f, knobDrive_ = -1.f;
  float driveMk_ = 1.f;                                    // waveshaper makeup for drive_ (one division per change)
  float fAttackS_ = 0.005f;                                // patch fAttack of the sounding note (AdEnv re-set on a decay change)
  void updateBlock();                                      // every 32 samples: glide, LFO, tables, cutoff
  void selectTables();                                     // table set per oscillator from its current pitch
};
}
