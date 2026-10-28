// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// SynthVoice: 2 wavetable oscillators + sub + noise, drive, TPT SVF, amp ADSR, filter AD, LFO, glide (SPEC §5).
// Budget ≈ 220 ns/sample on the ESP32-S3 (SPEC §1): nothing transcendental runs per sample. Glide, LFO, the
// filter envelope, the cutoff and the oscillators' table sets (SPEC §9.15) are recomputed once per 32-sample
// modulation block with fastTanPi and a polynomial exp2. The per-sample loop is a kernel specialised on the
// feature mask (osc2 / sub / noise / drive / sat) that keeps every piece of state in registers and writes it back
// once per run; the block after a table switch runs a crossfade kernel instead.
// SAT (SPEC §9.22, research §5 option B) is the fifth kernel bit rather than a per-sample flag: a runtime test
// would cost every voice a compare + branch per sample (the S3 has no branch predictor: ≈ 3 cycles × 256 × 18
// voices ≈ 60 µs per block for nothing), the extra 16 instances cost flash only (measured: see the task report),
// and the sat 0 kernels are the unchanged code, so a clean filter is bit-exact with the pre-§9.22 build.
#include "synth_voice.h"
#include <math.h>
#include <string.h>
#include "wavetables.h"
#include "clip.h"

namespace lp {
namespace {

constexpr int kModBlock = 32;                       // samples between modulation updates
constexpr float kInvModBlock = 1.0f / (float)kModBlock;
constexpr float kInv31 = 1.0f / 2147483648.0f;      // int32 xorshift -> float
constexpr float kGlideLog = 4.6051702f;             // ln(100): the glide covers 99% of the jump in `glide` seconds
constexpr float kGlideDone = 0.01f;                 // semitones left below which the glide snaps to the target
constexpr float kMinGlideS = 0.005f;                // minimum glide time (header contract)
constexpr float kMaxOscHz = 11000.f;
constexpr float kMaxResonance = 0.985f;             // TPT SVF k = 2 − 1.9 × res: 0.985 → Q ≈ 8, no self-oscillation (RESEARCH §2)
constexpr float kDrivePerKnob = 7.0f;               // voice waveshaper: drive 1..8 (clip.h distort), 0 = bypass
// SAT (SPEC §9.22): the band-pass state z1 of the TPT SVF goes through softClip(z1 × g) / g every sample with
// g = kSatMax × sat. Small signals pass with unity slope (the cubic term is (4/27) g² z1³, so the knob fades the
// squelch in continuously from 0), the state is hard-limited at ±1.5 / g (±0.75 at sat 1: a resonant peak that would
// ring to several times the input is squashed to a 303-like compressed squelch). MiniAcid uses tanh(1.3 × band).
constexpr float kSatMax = 2.0f;
// softClip(z × g) / g with the two clamps as selects instead of branches: the band state of a resonant 303 patch
// crosses ±1.5 / g all the time, so the branchy clip.h form mispredicts on the host (+4.5 ns per voice-sample
// measured) and costs a taken branch per sample on the S3; this is 9 FP ops, no branch. Same values: the cubic is
// exactly ±1 at ±1.5, where clip.h's clamps take over.
inline float squelch(float z, float g, float ig) {
  float y = z * g; y = y < -1.5f ? -1.5f : y; y = y > 1.5f ? 1.5f : y;
  return (y - (y * y * y) * (4.0f / 27.0f)) * ig;
}
// ACC (SPEC §9.22): an accented note (velocity >= 100) multiplies the filter envelope amount by 1 + accent and its
// level by 1 + kAccentGain × accent: +3 dB (× 1.4125) at accent 1, × 1.206 (+1.6 dB) at the default 0.5.
constexpr float kAccentGain = 0.41254f;             // 10^(3/20) − 1
constexpr uint8_t kAccentVel = 100;
inline float accentEnvMul(bool acc, float accent) { return acc ? 1.0f + accent : 1.0f; }
inline float accentGain(bool acc, float accent) { return acc ? 1.0f + kAccentGain * accent : 1.0f; }
inline float driveOf(float knob) { return knob > 0.f ? 1.0f + kDrivePerKnob * knob : 0.f; }
inline float driveMakeup(float drive) { return drive > 0.f ? 0.577f + 0.423f / (0.5f + 0.5f * drive * drive) : 1.0f; }   // equal-RMS makeup for a saw through distort() (fit, <= 0.27 dB error for drive 1.5..8); the old 1/(0.35+0.65d) lost 3.9 dB at knob 0.3 and 10.6 dB at 1.0 — the cause of quiet acid tracks (SPEC §9.21)
const SynthPatch kDefaultPatch{};                   // used when no patch was set (const: flash)

// The Arduino core builds with -Os, at which GCC leaves waveSampleN, softClip and Adsr::process as real
// windowed calls (call8) inside the per-sample loop: three calls per sample would eat most of the 53-cycle
// budget. flatten makes the compiler inline every call inside the kernel (GCC and clang).
#if defined(__GNUC__)
#define LP_FLATTEN __attribute__((flatten))
#else
#define LP_FLATTEN
#endif

// 2^x without a libm call. 5th-order Taylor on the fraction (max rel. error 1.7e-4, 0.3 cent) and an exponent
// add for the integer part. Only used for modulation (cutoff, glide/LFO pitch, osc2 ratio), never for audio.
inline float fastExp2(float x) {
  if (x > 60.f) x = 60.f; else if (x < -60.f) x = -60.f;
  int i = (int)x; if ((float)i > x) --i;            // floor
  float f = x - (float)i;
  float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013333f))));
  uint32_t bits; memcpy(&bits, &p, 4); bits += (uint32_t)i << 23; memcpy(&p, &bits, 4);
  return p;
}
inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
// Velocity curve: (vel/127)^2 blended 50/50 with linear (task spec).
inline float velCurve(uint8_t vel) { float v = (float)vel * (1.0f / 127.0f); return 0.5f * v * v + 0.5f * v; }
// Semitone -> Hz for the glide path (440 · 2^((st − 69) / 12)); the settled pitch uses midiToHz exactly.
inline float stToHz(float st) { return 440.0f * fastExp2((st - 69.0f) * (1.0f / 12.0f)); }
// Nearest integer pitch of a fractional one (ties up, negatives floor correctly): every table set is alias-free
// up to half a semitone above its last pitch (wavetables.h), so the rounded pitch picks a safe set.
inline int roundPitch(float st) { int i = (int)(st + 0.5f); if ((float)i > st + 0.5f) --i; return i; }

// Constants of one run (≤ 32 samples) and the state carried across runs.
struct RunParams {
  const int16_t* t1; const int16_t* t2; const int16_t* ts;
  int s1, s2, ss;                                   // log2 length of each table (its set, wavetables.h)
  uint32_t i1, i2, is;
  float scale, o2, sub, nl, drv, mk;                // scale = 1 / table peak; o2 / sub are already × scale; mk = drive makeup
  float a1, a2, a3, m0, m1, m2;                     // SVF coefficients and output mix y = m2·v2 + m1·v1 + m0·x
  float gInc;                                       // per-sample gain slew
  float satG, satInvG;                              // SAT kernels only: z1 ← softClip(z1 × satG) × satInvG (satG = kSatMax × sat, satInvG = 1 / satG)
  const int16_t* t1o; const int16_t* t2o; const int16_t* tso;   // crossfade block only: the previous tables ...
  int s1o, s2o, sso; float xfInc;                               // ... their log2 lengths, and the ramp step
};
struct RunState { uint32_t p1, p2, ps, nz; float z1, z2, gain, xf; Adsr env; };

// Per-sample kernel. Everything lives in locals (the ADSR as a register-resident copy so Adsr::process keeps
// exactly the env.h semantics), so the `out` stores cannot force reloads of the state, and the five feature
// tests are compile-time. SAT = false is the pre-§9.22 kernel, instruction for instruction.
template <bool O2, bool SUB, bool NL, bool DRV, bool SAT>
LP_FLATTEN void runKernel(float* out, int n, const RunParams& c, RunState& st) {
  const int16_t* t1 = c.t1; const int16_t* t2 = c.t2; const int16_t* ts = c.ts;
  const int s1 = c.s1, s2 = c.s2, ss = c.ss;
  const uint32_t i1 = c.i1, i2 = c.i2, is = c.is;
  const float scale = c.scale, o2 = c.o2, sub = c.sub, nl = c.nl, drv = c.drv, mk = c.mk;
  const float a1 = c.a1, a2 = c.a2, a3 = c.a3, m0 = c.m0, m1 = c.m1, m2 = c.m2, gInc = c.gInc;
  const float sg = c.satG, sig = c.satInvG;
  uint32_t p1 = st.p1, p2 = st.p2, ps = st.ps, nz = st.nz;
  float z1 = st.z1, z2 = st.z2, g = st.gain;
  Adsr env;                                                   // scalar copies: a struct copy becomes a memcpy call at -Os
  env.stage = st.env.stage; env.level = st.env.level; env.aInc = st.env.aInc; env.dCoef = st.env.dCoef;
  env.sLevel = st.env.sLevel; env.rCoef = st.env.rCoef;
  for (int i = 0; i < n; i++) {
    float s = (float)waveSampleN(t1, p1, s1) * scale; p1 += i1;
    if (O2) { s += (float)waveSampleN(t2, p2, s2) * o2; p2 += i2; }
    if (SUB) { s += (float)waveSampleN(ts, ps, ss) * sub; ps += is; }             // band-limited square, octave down
    if (NL) { nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5; s += nl * ((float)(int32_t)nz * kInv31); }
    if (DRV) s = softClip(s * drv) * mk;                                            // clip.h distort() with its makeup hoisted
    // TPT SVF (svf.h process) with the state in registers; LP/BP/HP is a fixed mix instead of two branches.
    const float v3 = s - z2; const float v1 = a1 * z1 + a2 * v3; const float v2 = z2 + a2 * z1 + a3 * v3;
    z1 = 2.0f * v1 - z1; z2 = 2.0f * v2 - z2;
    if (SAT) z1 = squelch(z1, sg, sig);                                             // the 303 squelch: the band-pass state saturates inside the loop (SPEC §9.22)
    const float y = m2 * v2 + m1 * v1 + m0 * s;
    out[i] += y * (env.process() * g);
    g += gInc;
  }
  st.p1 = p1; st.p2 = p2; st.ps = ps; st.nz = nz; st.z1 = z1; st.z2 = z2; st.gain = g;
  st.env.stage = env.stage; st.env.level = env.level;                             // the coefficients did not change
}

// Crossfade kernel (SPEC §9.15): for the one modulation block after a table switch every oscillator blends from
// its previous table to the new one at the SAME phase (only the partial content changes, never the pitch), so
// the two band-limits never step at the kept phase of a legato, stolen or gliding note. It runs once per switch,
// so the features are runtime tests here instead of sixteen more template instances.
LP_FLATTEN void runKernelXfade(float* out, int n, const RunParams& c, RunState& st) {
  const int16_t* t1 = c.t1; const int16_t* t2 = c.t2; const int16_t* ts = c.ts;
  const int16_t* t1o = c.t1o; const int16_t* t2o = c.t2o; const int16_t* tso = c.tso;
  const int s1 = c.s1, s2 = c.s2, ss = c.ss, s1o = c.s1o, s2o = c.s2o, sso = c.sso;
  const uint32_t i1 = c.i1, i2 = c.i2, is = c.is;
  const float scale = c.scale, o2 = c.o2, sub = c.sub, nl = c.nl, drv = c.drv, mk = c.mk;
  const float a1 = c.a1, a2 = c.a2, a3 = c.a3, m0 = c.m0, m1 = c.m1, m2 = c.m2, gInc = c.gInc, rInc = c.xfInc;
  const float sg = c.satG, sig = c.satInvG;
  const bool o2On = o2 != 0.f, subOn = sub != 0.f, nlOn = nl != 0.f, drvOn = drv > 0.f, satOn = sg > 0.f;
  uint32_t p1 = st.p1, p2 = st.p2, ps = st.ps, nz = st.nz;
  float z1 = st.z1, z2 = st.z2, g = st.gain, r = st.xf;
  Adsr env;
  env.stage = st.env.stage; env.level = st.env.level; env.aInc = st.env.aInc; env.dCoef = st.env.dCoef;
  env.sLevel = st.env.sLevel; env.rCoef = st.env.rCoef;
  for (int i = 0; i < n; i++) {
    float w = (float)waveSampleN(t1o, p1, s1o); w += ((float)waveSampleN(t1, p1, s1) - w) * r; p1 += i1;
    float s = w * scale;
    if (o2On) { w = (float)waveSampleN(t2o, p2, s2o); w += ((float)waveSampleN(t2, p2, s2) - w) * r; s += w * o2; p2 += i2; }
    if (subOn) { w = (float)waveSampleN(tso, ps, sso); w += ((float)waveSampleN(ts, ps, ss) - w) * r; s += w * sub; ps += is; }
    if (nlOn) { nz ^= nz << 13; nz ^= nz >> 17; nz ^= nz << 5; s += nl * ((float)(int32_t)nz * kInv31); }
    if (drvOn) s = softClip(s * drv) * mk;
    r += rInc; if (r > 1.0f) r = 1.0f;
    const float v3 = s - z2; const float v1 = a1 * z1 + a2 * v3; const float v2 = z2 + a2 * z1 + a3 * v3;
    z1 = 2.0f * v1 - z1; z2 = 2.0f * v2 - z2;
    if (satOn) z1 = squelch(z1, sg, sig);
    const float y = m2 * v2 + m1 * v1 + m0 * s;
    out[i] += y * (env.process() * g);
    g += gInc;
  }
  st.p1 = p1; st.p2 = p2; st.ps = ps; st.nz = nz; st.z1 = z1; st.z2 = z2; st.gain = g; st.xf = r;
  st.env.stage = env.stage; st.env.level = env.level;
}

using Kernel = void (*)(float*, int, const RunParams&, RunState&);
// Index = o2 | sub << 1 | noise << 2 | drive << 3 | sat << 4 (const: flash).
#define LP_KERNEL_ROW(S) \
  runKernel<0, 0, 0, 0, S>, runKernel<1, 0, 0, 0, S>, runKernel<0, 1, 0, 0, S>, runKernel<1, 1, 0, 0, S>, \
  runKernel<0, 0, 1, 0, S>, runKernel<1, 0, 1, 0, S>, runKernel<0, 1, 1, 0, S>, runKernel<1, 1, 1, 0, S>, \
  runKernel<0, 0, 0, 1, S>, runKernel<1, 0, 0, 1, S>, runKernel<0, 1, 0, 1, S>, runKernel<1, 1, 0, 1, S>, \
  runKernel<0, 0, 1, 1, S>, runKernel<1, 0, 1, 1, S>, runKernel<0, 1, 1, 1, S>, runKernel<1, 1, 1, 1, S>
const Kernel kKernels[32] = { LP_KERNEL_ROW(false), LP_KERNEL_ROW(true) };
#undef LP_KERNEL_ROW
constexpr int kKernelSat = 16;

} // namespace

void SynthVoice::init() {
  // Deterministic noise: voices are initialised in a fixed order (engine boot), so the n-th voice always gets
  // the same seed on the host and on the device; noteOn restarts the generator from it.
  static uint32_t s_initCount = 0;
  ++s_initCount;
  seed_ = s_initCount * 0x9E3779B9u; seed_ ^= seed_ >> 15; seed_ *= 0x2C1B3C6Du; seed_ ^= seed_ >> 12;
  if (seed_ == 0) seed_ = 22222u;                 // xorshift must never sit on zero
  noise_ = seed_;
  // Tables peak below int16 full scale (29000 in wavetables_data.hpp): normalise oscillators and LFO to ±1.0.
  const int16_t* sine = waveTable(WSine);
  const int peak = sine ? (int)sine[kWaveLen / 4] : 0;
  invPeak_ = peak > 0 ? 1.0f / (float)peak : 1.0f / 32768.0f;
  active_ = false; pitch_ = 60; vel_ = 1.0f; age_ = 0;
  ph1_ = ph2_ = phSub_ = lfoPh_ = 0;
  curSt_ = targetSt_ = 69.f; curHz_ = targetHz_ = 440.f; glideCoef_ = 1.0f;
  svf_.reset(); svf_.k = 2.0f - 1.9f * 0.2f; svf_.mode = 0;
  amp_.kill(); fenv_.level = 0; fenv_.rising = false; fAtk32_ = 0.f; fDec32_ = 1.f;
  baseCutoff_ = 2000.f; blockCount_ = 0; primed_ = false;
  tab1_ = tab2_ = tabSub_ = sineTab_ = nullptr; inc1_ = inc2_ = incSub_ = lfoInc_ = 0;
  tab1Old_ = tab2Old_ = tabSubOld_ = nullptr;
  sh1_ = sh2_ = shSub_ = sh1Old_ = sh2Old_ = shSubOld_ = (uint8_t)kWaveShift;
  wave1_ = wave2_ = (uint8_t)WSaw; xfade_ = false; xfPos_ = 0.f; osc2St_ = 0.f; lfoPitchAbs_ = 0.f;
  osc2Ratio_ = 1.f; osc2Lvl_ = subLvl_ = noiseLvl_ = drive_ = 0.f;
  fEnvOct_ = lfoCut_ = lfoPitch_ = 0.f; gain_ = gainTarget_ = gainInc_ = 0.f;
  keyFactor_ = 1.f; knobRes_ = knobDecayS_ = knobDrive_ = -1.f; driveMk_ = 1.f; fAttackS_ = 0.005f;
  accent_ = false;
}

// See the header for the legato / retrigger / glide contract.
void SynthVoice::noteOn(uint8_t pitch, uint8_t vel, bool legato) {
  if (vel == 0) { noteOff(); return; }              // MIDI convention: velocity 0 is a note off
  const SynthPatch& p = patch_ ? *patch_ : kDefaultPatch;
  const bool cont = legato && active_;              // legato onto a sounding (or releasing) voice
  const bool fresh = !active_;
  pitch_ = pitch; age_ = 0;
  targetSt_ = (float)pitch; targetHz_ = midiToHz(targetSt_);
  // Glide: one-pole on pitch (semitones), 99% of the jump in patch.glide seconds (min 5 ms), stepped per block.
  // Slides only from a pitch that is still sounding (legato / steal) or on a mono patch (303 style): a fresh
  // polyphonic voice must not slide in from whatever it played long ago.
  float glideS = p.glide;
  const bool slide = primed_ && (cont || (glideS > 0.f && (active_ || p.polyphony == 1)));
  if (slide) {
    if (glideS < kMinGlideS) glideS = kMinGlideS;
    glideCoef_ = 1.0f - expf(-kGlideLog * (float)kModBlock / (glideS * (float)kSampleRate));
  } else {
    curSt_ = targetSt_; curHz_ = targetHz_; glideCoef_ = 1.0f;
  }
  primed_ = true;
  // Every parameter is re-read from the patch (the patch may change under a sounding voice at any time).
  // The oscillator tables are NOT fixed here: selectTables picks each one's set from the pitch it plays, once
  // per modulation block (SPEC §9.15), so a glide that crosses a set boundary in either direction switches to
  // the right table on the way (a downward slide starts at the HIGH pitch, where the rich table of the low
  // target would fold), and a legato / stolen note crossfades from the table it was playing.
  // Live knobs (SPEC §9.17a): cutoff, resonance, filter env amount, filter decay, drive, wave 1 and (SPEC §9.22)
  // sat / accent come from the track's SynthKnobs when the engine set them (updateBlock follows them every block);
  // the rest is the patch. The accent itself is decided here, once per note, from the velocity.
  const SynthKnobs* k = knobs_;
  accent_ = vel >= kAccentVel;
  const float accentK = clampf(k ? k->accent : p.accent, 0.f, 1.f);
  const uint8_t w1 = k ? k->wave : p.wave1;
  wave1_ = w1 < WaveCount ? w1 : (uint8_t)WSaw;
  wave2_ = p.wave2 < WaveCount ? p.wave2 : (uint8_t)WSaw;
  osc2St_ = 12.0f * (float)p.osc2Octave + p.osc2Detune;
  lfoPitchAbs_ = p.lfoRate > 0.f ? fabsf(p.lfoToPitch) : 0.f;   // the LFO's pitch excursion, when it runs
  sineTab_ = waveTable(WSine);
  osc2Ratio_ = fastExp2((float)p.osc2Octave + p.osc2Detune * (1.0f / 12.0f));
  osc2Lvl_ = p.osc2Level; subLvl_ = p.subLevel; noiseLvl_ = p.noiseLevel;
  knobDrive_ = k ? k->drive : p.drive;
  drive_ = driveOf(knobDrive_); driveMk_ = driveMakeup(drive_);   // 0 = bypass the waveshaper
  // Filter base: cutoff × 2^(keyTrack × (pitch − 60) / 12); env and LFO are added per block. The key factor is
  // kept so a moving cutoff knob is re-applied per block with one multiply.
  keyFactor_ = fastExp2(p.keyTrack * (float)((int)pitch - 60) * (1.0f / 12.0f));
  baseCutoff_ = (k ? k->cutoffHz : p.cutoff) * keyFactor_;
  fEnvOct_ = (k ? k->filterEnvOct : p.filterEnv) * accentEnvMul(accent_, accentK); lfoCut_ = p.lfoToCutoff; lfoPitch_ = p.lfoToPitch;
  lfoInc_ = p.lfoRate > 0.f ? phaseIncForHz(p.lfoRate) : 0;
  // Resonance and mode directly (svf.h set would spend a tanf on a g that updateBlock overwrites anyway).
  knobRes_ = k ? k->resonance : p.resonance;
  svf_.k = 2.0f - 1.9f * clampf(knobRes_, 0.f, kMaxResonance);
  svf_.mode = p.filterMode <= 2 ? p.filterMode : 0;
  amp_.set(p.attack, p.decay, p.sustain, p.release);
  fAttackS_ = p.fAttack; knobDecayS_ = k ? k->decayS : p.fDecay;
  fenv_.set(fAttackS_, knobDecayS_);
  fAtk32_ = fenv_.aInc * (float)kModBlock;
  { float c = fenv_.dCoef; c *= c; c *= c; c *= c; c *= c; c *= c; fDec32_ = c; }   // dCoef^32
  vel_ = p.level * velCurve(vel);
  gainTarget_ = vel_ * accentGain(accent_, accentK);               // reached over one modulation block
  noise_ = seed_;                                                  // reproducible noise per note
  if (fresh) {                                                     // clean state: nothing sounding, no click to avoid
    ph1_ = ph2_ = phSub_ = lfoPh_ = 0; svf_.reset(); amp_.level = 0; fenv_.level = 0; fenv_.rising = false;
    gain_ = gainTarget_; gainInc_ = 0.f;
    selectTables(); xfade_ = false;                                // phases restart: nothing to crossfade from
  }
  if (!cont) {
    amp_.gate(true); fenv_.trigger();                              // (re)trigger: Attack continues from the current level
  } else if (amp_.stage == Adsr::Release) {
    amp_.gate(true); fenv_.trigger();                              // legato onto a releasing voice keeps the note alive
  } else if (amp_.stage == Adsr::Decay || amp_.stage == Adsr::Sustain) {
    // Keep the envelope running while honouring a changed sustain level without a step: rise linearly when
    // the new sustain is above the current level, otherwise decay towards it (a no-op for an unchanged patch).
    amp_.stage = amp_.level < amp_.sLevel - 0.001f ? Adsr::Attack : Adsr::Decay;
  }
  // Force a modulation update on the next sample (it re-selects the tables of a kept-phase note and starts
  // their crossfade), unless a crossfade is still running inside the current block: cutting it short would
  // step, so that block finishes first (at most 31 samples, 1.4 ms, later).
  if (!(xfade_ && blockCount_ != 0)) blockCount_ = 0;
  active_ = true;
}

void SynthVoice::noteOff() { amp_.gate(false); }

// Table set per oscillator from the pitch it plays this block (SPEC §9.15): osc 1 at the glided pitch, osc 2 at
// its octave offset + detune, the sub one octave below, each raised by the LFO's maximum upward excursion so a
// vibrato never flips a set back and forth. A fractional pitch is rounded to the nearest integer (every set is
// alias-free up to half a semitone above its last pitch). When a table changes the previous one is kept for one
// block and the crossfade kernel blends across; a silent oscillator keeps whatever table it has (no needless
// crossfade), and an oscillator that had no table yet blends from the new one (a no-op).
void SynthVoice::selectTables() {
  const float base = curSt_ + lfoPitchAbs_;
  const int set1 = waveSetForPitch(roundPitch(base));
  const int set2 = osc2Lvl_ > 0.f ? waveSetForPitch(roundPitch(base + osc2St_)) : -1;
  const int setS = subLvl_ > 0.f ? waveSetForPitch(roundPitch(base - 12.0f)) : -1;
  const int16_t* n1 = waveTableSet((Wave)wave1_, set1);
  const int16_t* n2 = set2 >= 0 ? waveTableSet((Wave)wave2_, set2) : tab2_;
  const int16_t* ns = setS >= 0 ? waveTableSet(WSquare, setS) : tabSub_;
  if (n1 == tab1_ && n2 == tab2_ && ns == tabSub_) return;
  tab1Old_ = tab1_ ? tab1_ : n1; sh1Old_ = sh1_;
  tab2Old_ = tab2_ ? tab2_ : n2; sh2Old_ = sh2_;
  tabSubOld_ = tabSub_ ? tabSub_ : ns; shSubOld_ = shSub_;
  tab1_ = n1; sh1_ = (uint8_t)kWaveSetShift[set1];
  if (set2 >= 0) { tab2_ = n2; sh2_ = (uint8_t)kWaveSetShift[set2]; }
  if (setS >= 0) { tabSub_ = ns; shSub_ = (uint8_t)kWaveSetShift[setS]; }
  xfade_ = true; xfPos_ = 0.f;
}

// Per modulation block (32 samples): gain slew, LFO, glide, table sets, pitch → phase increments, filter AD,
// cutoff → SVF.
void SynthVoice::updateBlock() {
  xfade_ = false;                                                  // a crossfade of the previous block is complete
  // Live knobs (SPEC §9.17a), read once per block, never per sample: cutoff and env amount are plain copies; wave 1
  // takes effect through selectTables below (a one-block crossfade to the new table, like any table switch);
  // resonance, filter decay and drive re-derive their coefficients only when the value moved (the decay costs an
  // expf in AdEnv::set — the engine smooths knobs per 256-sample block, so that is at most one per 8 blocks).
  if (knobs_) {
    const SynthKnobs& k = *knobs_;
    if (k.wave != wave1_ && k.wave < WaveCount) wave1_ = k.wave;
    baseCutoff_ = k.cutoffHz * keyFactor_;
    fEnvOct_ = k.filterEnvOct * accentEnvMul(accent_, clampf(k.accent, 0.f, 1.f));   // SPEC §9.22: the accent depth follows the knob live (sat: read per run in render)
    if (k.resonance != knobRes_) { knobRes_ = k.resonance; svf_.k = 2.0f - 1.9f * clampf(knobRes_, 0.f, kMaxResonance); }
    if (k.decayS != knobDecayS_) {
      knobDecayS_ = k.decayS; fenv_.set(fAttackS_, knobDecayS_);
      float c = fenv_.dCoef; c *= c; c *= c; c *= c; c *= c; c *= c; fDec32_ = c;   // dCoef^32 (attack unchanged)
    }
    if (k.drive != knobDrive_) { knobDrive_ = k.drive; drive_ = driveOf(knobDrive_); driveMk_ = driveMakeup(drive_); }
  }
  // Gain slew: the previous block finished its ramp; start a new one if the target moved (retrigger / legato, or
  // the accent knob of an accented note: its level follows the knob over one block like the other knobs).
  if (gainInc_ != 0.f) { gain_ = gainTarget_; gainInc_ = 0.f; }
  if (knobs_ && accent_) gainTarget_ = vel_ * accentGain(true, clampf(knobs_->accent, 0.f, 1.f));
  if (gain_ != gainTarget_) gainInc_ = (gainTarget_ - gain_) * kInvModBlock;
  // LFO sine, normalised to ±1.0 so lfoToPitch (st) and lfoToCutoff (oct) are exact.
  float lfo = 0.f;
  if (lfoInc_) { lfo = (float)waveSample(sineTab_, lfoPh_) * invPeak_; lfoPh_ += lfoInc_ * (uint32_t)kModBlock; }
  // Glide: one-pole in semitones (symmetric up/down, 303 convention); snaps to the exact pitch at the end.
  if (curSt_ != targetSt_) {
    curSt_ += (targetSt_ - curSt_) * glideCoef_;
    if (fabsf(targetSt_ - curSt_) < kGlideDone) { curSt_ = targetSt_; curHz_ = targetHz_; }
    else curHz_ = stToHz(curSt_);
  }
  selectTables();                                                  // the sets for the pitches of this block
  float hz = curHz_;
  if (lfoPitch_ != 0.f) hz *= fastExp2(lfoPitch_ * lfo * (1.0f / 12.0f));
  if (hz > kMaxOscHz) hz = kMaxOscHz;
  inc1_ = phaseIncForHz(hz); incSub_ = inc1_ >> 1;                 // sub: one octave below osc 1
  if (osc2Lvl_ > 0.f) { float h2 = hz * osc2Ratio_; inc2_ = phaseIncForHz(h2 > kMaxOscHz ? kMaxOscHz : h2); }
  // Filter AD: its level is only read here, so it advances 32 samples at a time: AdEnv::process (env.h) applied
  // in closed form at block resolution (attack + 32·aInc clamped at 1, decay × dCoef^32, -120 dB floor).
  const float fe = fenv_.level;
  if (fenv_.rising) { fenv_.level += fAtk32_; if (fenv_.level >= 1.0f) { fenv_.level = 1.0f; fenv_.rising = false; } }
  else { fenv_.level *= fDec32_; if (fenv_.level < 1e-6f) fenv_.level = 0.0f; }   // same floor as AdEnv::process
  // cutoff = base × 2^(filterEnv × fenv) × 2^(lfoToCutoff × lfo), via fastTanPi (never tanf per sample).
  const float oct = fEnvOct_ * fe + lfoCut_ * lfo;
  const float fc = clampf(oct != 0.f ? baseCutoff_ * fastExp2(oct) : baseCutoff_, 20.f, 10500.f);
  const float g = fastTanPi(fc * (1.0f / (float)kSampleRate));
  svf_.g = g; svf_.a1 = 1.0f / (1.0f + g * (g + svf_.k)); svf_.a2 = g * svf_.a1; svf_.a3 = g * svf_.a2;
}

bool SynthVoice::render(float* acc, int n) {
  if (!active_) return false;
  if (n <= 0) return true;
  RunParams c;
  c.scale = invPeak_; c.o2 = osc2Lvl_ * invPeak_; c.sub = subLvl_ * invPeak_; c.nl = noiseLvl_;
  c.xfInc = kInvModBlock;
  // Output mix for svf.h modes: 0 LP = v2, 1 BP = v1, 2 HP = x − k·v1 − v2 (k refreshed per run: the resonance knob).
  if (svf_.mode == 0) { c.m0 = 0.f; c.m1 = 0.f; c.m2 = 1.f; }
  else if (svf_.mode == 1) { c.m0 = 0.f; c.m1 = 1.f; c.m2 = 0.f; }
  else { c.m0 = 1.f; c.m1 = -svf_.k; c.m2 = -1.f; }
  const int featMask = (osc2Lvl_ > 0.f ? 1 : 0) | (subLvl_ > 0.f ? 2 : 0) | (noiseLvl_ > 0.f ? 4 : 0);
  RunState st;
  st.p1 = ph1_; st.p2 = ph2_; st.ps = phSub_; st.nz = noise_; st.z1 = svf_.z1; st.z2 = svf_.z2; st.gain = gain_;
  st.env.stage = amp_.stage; st.env.level = amp_.level; st.env.aInc = amp_.aInc; st.env.dCoef = amp_.dCoef;
  st.env.sLevel = amp_.sLevel; st.env.rCoef = amp_.rCoef;
  int done = 0;
  while (done < n) {
    if (blockCount_ == 0) updateBlock();
    int run = kModBlock - blockCount_; if (run > n - done) run = n - done;
    blockCount_ += run; if (blockCount_ >= kModBlock) blockCount_ = 0;
    // The tables may have been re-selected by updateBlock: refreshed per run, like the increments.
    c.t1 = tab1_; c.t2 = tab2_; c.ts = tabSub_; c.s1 = sh1_; c.s2 = sh2_; c.ss = shSub_;
    c.i1 = inc1_; c.i2 = inc2_; c.is = incSub_;
    c.a1 = svf_.a1; c.a2 = svf_.a2; c.a3 = svf_.a3; c.gInc = gainInc_;
    c.drv = drive_; c.mk = driveMk_; if (svf_.mode == 2) c.m1 = -svf_.k;      // the drive / resonance knobs may have moved
    // SAT (SPEC §9.22): the knob (or the patch without knobs) is read per run like the drive; g = kSatMax × sat and
    // its reciprocal are hoisted here (one division, only on a squelching voice; no per-voice state).
    const float sat = clampf(knobs_ ? knobs_->sat : (patch_ ? patch_->sat : 0.f), 0.f, 1.f);
    if (sat > 0.f) { c.satG = kSatMax * sat; c.satInvG = 1.0f / c.satG; } else { c.satG = 0.f; c.satInvG = 0.f; }
    st.gain = gain_; st.xf = xfPos_;
    Kernel k = kKernels[featMask | (drive_ > 0.f ? 8 : 0) | (sat > 0.f ? kKernelSat : 0)];
    if (xfade_) { c.t1o = tab1Old_; c.t2o = tab2Old_; c.tso = tabSubOld_; c.s1o = sh1Old_; c.s2o = sh2Old_; c.sso = shSubOld_; k = runKernelXfade; }
    k(acc + done, run, c, st);
    gain_ = st.gain; xfPos_ = st.xf; done += run;
    // Idle when the ADSR finished its release, or sits on a zero sustain (nothing left to hear).
    if (!st.env.active() || (st.env.stage == Adsr::Sustain && st.env.sLevel < 0.0005f)) {
      ph1_ = st.p1; ph2_ = st.p2; phSub_ = st.ps; noise_ = st.nz; svf_.z1 = st.z1; svf_.z2 = st.z2;
      age_ += (uint32_t)done;
      amp_.kill(); active_ = false;
      return false;
    }
  }
  ph1_ = st.p1; ph2_ = st.p2; phSub_ = st.ps; noise_ = st.nz; svf_.z1 = st.z1; svf_.z2 = st.z2;
  amp_.stage = st.env.stage; amp_.level = st.env.level;
  age_ += (uint32_t)done;
  return true;
}

} // namespace lp
