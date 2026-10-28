// Built-in synth preset banks per category and the glide (perform) patch (SPEC §5: 8+ per category, picked by a
// shuffle bag with ±8% parameter spread). The banks live in dsp/presets_gen.hpp, written by tools/gen_presets.py
// (research docs/RESEARCH_LIBRARY_EXPANSION.md §4): per category the hand-written library rows, 128 generated
// library rows, and for Chord / Lead / Pad / Bass the hand-written '303' family followed by 16 generated '303' rows
// (SPEC §9.17b: the acid family is appended AFTER the library presets, and the hand rows stay first inside each
// family, so a saved library lp.track.N.preset index keeps its sound; the '303' rows moved from right after the hand
// library to [libraryPresetCount, +8), and gen/sets.cpp (presetForLayout) remaps that old range for a file without
// the lp.banks line, SPEC §9.22b -- the hand rows must therefore keep their order). The tables are constexpr const
// aggregates built member-by-member (presetgen::make), so they are constant-initialised into flash (.rodata, 0 B
// of RAM) and compile with or without the SPEC §9.22 sat / accent fields of SynthPatch. Each generated row's level
// is a loudness trim baked by the tool (tools/preset_loudness.hpp: every generated row within ±1 dB of its
// family's hand-row median on a fixed phrase); the hand rows are the untouched reference.
// Field order of the rows (presetgen::make, core/types.h):
//   wave1, wave2, osc2Detune(st), osc2Level, osc2Octave, subLevel, noiseLevel, filterMode(0 LP 1 BP 2 HP),
//   cutoff(Hz), resonance, filterEnv(oct), keyTrack, attack, decay, sustain, release, fAttack, fDecay,
//   glide(s), lfoRate(Hz), lfoToCutoff(oct), lfoToPitch(st), drive, level, polyphony, name, sat, accent
#include "presets.h"
#include "presets_gen.hpp"

namespace lp {
namespace {

using namespace presetgen;

// Perform-mode default sound (SPEC §4 perform page): the first Fx preset.
constexpr SynthPatch kGlide = make<SynthPatch>(
  WSaw, WSaw, 0.10f, 0.6f, 0, 0.0f, 0.0f, 0,
  2200.f, 0.45f, 2.5f, 0.5f,
  0.005f, 0.20f, 0.7f, 0.20f, 0.002f, 0.20f,
  0.12f, 0.0f, 0.0f, 0.0f, 0.0f, 0.45f, 4, "Perform", 0.0f, 0.5f);

template <int N> constexpr int countOf(const SynthPatch (&)[N]) { return N; }
static_assert(countOf(kChord) == kChordLibrary + kChordAcid && countOf(kLead) == kLeadLibrary + kLeadAcid &&
              countOf(kPad) == kPadLibrary + kPadAcid && countOf(kBass) == kBassLibrary + kBassAcid &&
              countOf(kFx) == kFxLibrary + kFxAcid, "presets_gen.hpp: every bank is library rows then '303' rows");
static_assert(kChordHandLibrary > 0 && kChordHandAcid > 0 && kLeadHandAcid > 0 && kPadHandAcid > 0 && kBassHandAcid > 0 && kFxAcid == 0,
              "every acid bank keeps its hand library presets first and its '303' family last; Fx has no acid family");
// The generator (TrackGenInfo.presetIndex), the set files (lp.track.N.preset) and the preset bag store a bank index in
// a uint8_t: a bank may not grow past 255 rows.
static_assert(countOf(kChord) <= 255 && countOf(kLead) <= 255 && countOf(kPad) <= 255 && countOf(kBass) <= 255 && countOf(kFx) <= 255,
              "bank indices are uint8_t");
static_assert(kTotalPresets == countOf(kChord) + countOf(kLead) + countOf(kPad) + countOf(kBass) + countOf(kFx), "presets_gen.hpp total");

struct Bank { const SynthPatch* p; int n; int lib; };
Bank bank(Category c) {
  switch (c) {
    case Chord: return { kChord, countOf(kChord), kChordLibrary };
    case Lead: return { kLead, countOf(kLead), kLeadLibrary };
    case Pad: return { kPad, countOf(kPad), kPadLibrary };
    case Bass: return { kBass, countOf(kBass), kBassLibrary };
    case Fx: return { kFx, countOf(kFx), kFxLibrary };
    default: return { nullptr, 0, 0 };
  }
}

// xorshift32 → uniform in [0.92, 1.08] (deterministic ±8% spread). The seed is hashed first: raw xorshift32
// started from a small seed (1, 2, 3 …) yields a tiny first output, which would bias the first parameter.
struct Spread {
  uint32_t x;
  explicit Spread(uint32_t seed) {
    x = seed * 2654435761u; x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13;
    if (x == 0) x = 0x9E3779B9u;                                 // xorshift must never sit on zero
    next();                                                      // one warm-up step
  }
  float next() {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    float u = (float)(x >> 8) * (1.0f / 16777216.0f);          // [0, 1)
    return 1.0f + (2.0f * u - 1.0f) * 0.08f;
  }
};
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

} // namespace

int presetCount(Category c) { return bank(c).n; }
int libraryPresetCount(Category c) { return bank(c).lib; }
int acidPresetCount(Category c) { Bank b = bank(c); return b.n - b.lib; }

const SynthPatch& preset(Category c, int index) {
  Bank b = bank(c);
  if (b.n == 0) return kGlide;
  index %= b.n; if (index < 0) index += b.n;
  return b.p[index];
}

void applySpread(SynthPatch& p, uint32_t seed) {
  Spread rng(seed);
  p.cutoff = clampf(p.cutoff * rng.next(), 20.f, 10500.f);
  p.decay = clampf(p.decay * rng.next(), 0.001f, 10.f);
  p.osc2Detune = clampf(p.osc2Detune * rng.next(), -24.f, 24.f);
  p.resonance = clampf(p.resonance * rng.next(), 0.f, 0.95f);
}

void makePatch(Category c, int index, uint32_t seed, SynthPatch& dst) {
  dst = preset(c, index);
  applySpread(dst, (seed * 2654435761u) ^ ((uint32_t)(index + 1) * 0x85EBCA6Bu));   // seed mixed with the index
}

const SynthPatch& glidePatch() { return kGlide; }

} // namespace lp
