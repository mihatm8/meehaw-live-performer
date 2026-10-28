// Built-in synth preset banks per category (SPEC §5) and the glide (perform) patch.
#pragma once
#include "../core/types.h"
namespace lp {
int presetCount(Category c);                       // whole bank: library presets, then the appended '303' family (SPEC §9.17b)
const SynthPatch& preset(Category c, int index);
// SPEC §9.17b acid mode: every bank of Chord / Lead / Pad / Bass ends with a '303' family appended AFTER the library
// presets so saved lp.track.N.preset indices stay valid. Indices [0, libraryPresetCount) are the library family,
// [libraryPresetCount, presetCount) the acid family (acidPresetCount entries, 0 for Fx and the sample categories).
int libraryPresetCount(Category c);
int acidPresetCount(Category c);
inline bool isAcidPreset(Category c, int index) { return index >= libraryPresetCount(c) && index < presetCount(c); }
// Copies preset `index` into dst and applies a deterministic ±8% spread from seed to a few parameters.
void makePatch(Category c, int index, uint32_t seed, SynthPatch& dst);
// The spread itself, applied in place to any patch (what makePatch does after copying the preset): cutoff, decay,
// osc2Detune and resonance each × [0.92, 1.08] from a xorshift32 seeded with `seed`, clamped to the valid ranges.
void applySpread(SynthPatch& p, uint32_t seed);
const SynthPatch& glidePatch();                    // perform-mode default sound
}
