// Host tests for the synth module: SynthVoice (dsp/synth_voice.cpp) and the preset banks (dsp/presets.cpp).
#include "check.h"
#include <chrono>
#include <cstring>
#include <vector>
#include <set>
#include <string>
#include <type_traits>
#include "dsp/synth_voice.h"
#include "dsp/presets.h"
#include "dsp/wavetables.h"
#include "../tools/preset_loudness.hpp"   // the phrase renderer + loudness metric tools/gen_presets.py bakes the preset trims with

using namespace lp;

namespace {

const Category kSynthCats[] = { Chord, Lead, Pad, Bass, Fx };
constexpr int kSr = kSampleRate;
inline int ms(float m) { return (int)(m * 0.001f * kSr); }

// Renders n samples in blocks of `block` into out (appended). Returns whether the voice is still active.
bool renderInto(SynthVoice& v, std::vector<float>& out, int n, int block = kBlock) {
  bool alive = true;
  while (n > 0) {
    int c = n < block ? n : block; if (c > kBlock) c = kBlock;
    float buf[kBlock]; memset(buf, 0, sizeof(buf));
    alive = v.render(buf, c);
    out.insert(out.end(), buf, buf + c);
    n -= c;
  }
  return alive;
}
float peakOf(const std::vector<float>& s, size_t from = 0, size_t to = (size_t)-1) {
  if (to > s.size()) to = s.size();
  float p = 0; for (size_t i = from; i < to; i++) { float a = std::fabs(s[i]); if (a > p) p = a; } return p;
}
bool finiteAll(const std::vector<float>& s) { for (float x : s) if (!std::isfinite(x)) return false; return true; }
// Fundamental estimate from zero crossings (rising) inside [from, to).
float zcHz(const std::vector<float>& s, size_t from, size_t to) {
  int c = 0; size_t first = 0, last = 0;
  for (size_t i = from + 1; i < to; i++) if (s[i - 1] < 0 && s[i] >= 0) { if (!c) first = i; last = i; c++; }
  if (c < 2) return 0; return (float)(c - 1) * kSr / (float)(last - first);
}
// Brightness proxy: RMS of the first difference relative to the RMS of the signal.
float brightness(const std::vector<float>& s, size_t from, size_t to) {
  double d = 0, a = 0; for (size_t i = from + 1; i < to; i++) { double x = s[i] - s[i - 1]; d += x * x; a += (double)s[i] * s[i]; }
  return a > 0 ? (float)std::sqrt(d / a) : 0.f;
}
SynthPatch sinePatch() {                            // clean sine through an open filter, instant envelope
  SynthPatch p; p.wave1 = WSine; p.wave2 = WSine; p.osc2Level = 0; p.subLevel = 0; p.noiseLevel = 0;
  p.cutoff = 10000.f; p.resonance = 0.f; p.filterEnv = 0.f; p.keyTrack = 0.f;
  p.attack = 0.001f; p.decay = 0.05f; p.sustain = 1.0f; p.release = 0.05f; p.fAttack = 0.001f; p.fDecay = 0.05f;
  p.glide = 0; p.lfoRate = 0; p.lfoToCutoff = 0; p.lfoToPitch = 0; p.drive = 0; p.level = 0.5f; p.polyphony = 4;
  return p;
}
// Largest per-sample jump inside [from, to): a click detector.
float maxStep(const std::vector<float>& s, size_t from, size_t to) {
  if (to > s.size()) to = s.size();
  float m = 0; for (size_t i = from + 1; i < to; i++) m = std::fmax(m, std::fabs(s[i] - s[i - 1])); return m;
}
// Pitch in semitones from zero crossings, relative to midi 0.
float zcSemitones(const std::vector<float>& s, size_t from, size_t to) {
  float hz = zcHz(s, from, to); return hz > 0 ? 69.f + 12.f * std::log2(hz / 440.f) : 0.f;
}

} // namespace

// ---------------------------------------------------------------- presets: bank shape
TEST(preset_banks_have_at_least_eight_named_unique_entries) {
  for (Category c : kSynthCats) {
    CHECK(presetCount(c) >= 8);
    std::set<std::string> names;
    for (int i = 0; i < presetCount(c); i++) {
      const SynthPatch& p = preset(c, i);
      CHECK(p.name[0] != 0);
      names.insert(p.name);
      CHECK(p.polyphony >= 1);
      CHECK(p.wave1 < WaveCount); CHECK(p.wave2 < WaveCount);
      CHECK(p.cutoff >= 20.f && p.cutoff <= 10500.f);
      CHECK(p.resonance >= 0.f && p.resonance <= 0.95f);
      CHECK(p.level > 0.f && p.level <= 1.f);
      CHECK(p.osc2Octave >= -2 && p.osc2Octave <= 2);
    }
    CHECK_EQ((int)names.size(), presetCount(c));
  }
  CHECK_EQ(presetCount(Beat), 0); CHECK_EQ(presetCount(Vocal), 0); CHECK_EQ(presetCount(Percussion), 0);
  CHECK(std::strcmp(preset(Beat, 3).name, glidePatch().name) == 0);   // non-synth categories fall back to the glide patch
  CHECK(std::strcmp(preset(Lead, presetCount(Lead) + 1).name, preset(Lead, 1).name) == 0);  // index wraps
  CHECK(std::strcmp(preset(Lead, -1).name, preset(Lead, presetCount(Lead) - 1).name) == 0);
}

// ---------------------------------------------------------------- presets: the generated bank (tools/gen_presets.py, research §4)
TEST(generated_bank_sizes_keep_hand_rows_first_and_the_303_family_last) {
  // Layout per category: [hand library][128 generated library][hand '303'][16 generated '303'] (Fx: no '303' family).
  // The hand rows keep their indices inside each family: a saved library lp.track.N.preset index keeps its sound, and
  // the hand '303' rows are the first 8 of the acid family, where the sets loader (presetForLayout, a file without
  // lp.banks) puts a pre-§9.22 '303' index (Chord [10, 18), Lead / Pad / Bass [9, 17) → [libraryPresetCount, +8);
  // SPEC §9.22b, tested in run_sets.sh). The first / last hand row of each block is pinned by name; the 10 / 9 / 9 / 9
  // + 8 hand counts are the loader's remap table.
  struct Row { Category c; int handLib, handAcid; const char* first; const char* lastLib; const char* firstAcid; const char* lastAcid; };
  const Row rows[] = {
    { Chord, 10, 8, "PluckWide", "ChordWarm", "303 Stab", "303 Dirty" },
    { Lead, 9, 8, "SawLead", "DigitalPluck", "303 Saw", "303 Snap" },
    { Pad, 9, 8, "WarmPad", "BrassPad", "303 PadStab", "303 Dirty" },
    { Bass, 9, 8, "SubSine", "HouseBass", "303 Saw", "303 Snap" },
    { Fx, 9, 0, "GlideSaw", "GlideScream", nullptr, nullptr },
  };
  int total = 0;
  for (const Row& r : rows) {
    const int lib = libraryPresetCount(r.c);
    CHECK_EQ(lib, r.handLib + 128);
    CHECK_EQ(acidPresetCount(r.c), r.handAcid ? r.handAcid + 16 : 0);
    CHECK_EQ(presetCount(r.c), lib + acidPresetCount(r.c));
    CHECK(presetCount(r.c) <= 255);                                       // TrackGenInfo.presetIndex / lp.track.N.preset are uint8_t
    CHECK(std::strcmp(preset(r.c, 0).name, r.first) == 0);
    CHECK(std::strcmp(preset(r.c, r.handLib - 1).name, r.lastLib) == 0);
    CHECK(std::strncmp(preset(r.c, r.handLib).name, "303", 3) != 0);     // row N of the library family is a generated library preset
    if (r.handAcid) {
      CHECK(std::strcmp(preset(r.c, lib).name, r.firstAcid) == 0);
      CHECK(std::strcmp(preset(r.c, lib + r.handAcid - 1).name, r.lastAcid) == 0);
      CHECK(std::strncmp(preset(r.c, lib + r.handAcid).name, "303 ", 4) == 0);   // the generated '303' rows follow
    }
    CHECK_EQ(lp_qa::handLibrary(r.c), r.handLib); CHECK_EQ(lp_qa::handAcid(r.c), r.handAcid);   // the QA header's reference rows
    total += presetCount(r.c);
  }
  CHECK(total >= 700); CHECK_EQ(total, 782);
  // The hand rows are the former hand-written table, field-exact (the builder assigns every member by name).
  const SynthPatch& pw = preset(Chord, 0);
  CHECK(pw.wave1 == WSaw && pw.wave2 == WSaw && pw.osc2Detune == 0.12f && pw.osc2Level == 0.6f && pw.cutoff == 1800.f && pw.resonance == 0.20f);
  CHECK(pw.filterEnv == 2.5f && pw.keyTrack == 0.4f && pw.attack == 0.003f && pw.decay == 0.30f && pw.sustain == 0.f && pw.release == 0.25f);
  CHECK(pw.fAttack == 0.002f && pw.fDecay == 0.20f && pw.glide == 0.f && pw.level == 0.35f && pw.polyphony == 4);
  const SynthPatch& b303 = preset(Bass, libraryPresetCount(Bass));
  CHECK(b303.wave1 == WSaw && b303.osc2Level == 0.f && b303.cutoff == 500.f && b303.resonance == 0.75f && b303.glide == 0.06f && b303.drive == 0.15f && b303.level == 0.77f && b303.polyphony == 1);
  const SynthPatch& bell = preset(Lead, 3);
  CHECK(bell.osc2Detune == 3.86f && bell.osc2Octave == 1 && bell.keyTrack == 0.8f && bell.decay == 1.00f);
  const SynthPatch& organ = preset(Chord, 4);
  CHECK(organ.subLevel == 0.3f && organ.lfoRate == 5.5f && organ.lfoToPitch == 0.08f && organ.sustain == 1.0f);
  const SynthPatch& hollow = preset(Pad, 5);
  CHECK(hollow.filterMode == 1 && hollow.osc2Octave == 1 && hollow.lfoToCutoff == 0.7f);
  CHECK(std::strcmp(glidePatch().name, "Perform") == 0); CHECK(glidePatch().cutoff == 2200.f && glidePatch().glide == 0.12f);
  // Name padding: the builder zero-fills the whole name field, so the display never reads stale bytes.
  for (Category c : kSynthCats) for (int i = 0; i < presetCount(c); i++) {
    const SynthPatch& p = preset(c, i); size_t n = std::strlen(p.name);
    for (size_t k = n; k < sizeof(p.name); k++) CHECK(p.name[k] == 0);
  }
}

TEST(preset_names_are_short_unique_and_mark_the_303_family) {
  for (Category c : kSynthCats) {
    std::set<std::string> names;
    for (int i = 0; i < presetCount(c); i++) {
      const SynthPatch& p = preset(c, i);
      const size_t n = std::strlen(p.name);
      if (n < 1 || n > 12) std::printf("  %s/%d name '%s' length %zu\n", categoryName(c), i, p.name, n);
      CHECK(n >= 1 && n <= 12);                                           // fits the 12-character sound field of the track rows
      if (!names.insert(p.name).second) std::printf("  %s duplicate name '%s' at %d\n", categoryName(c), p.name, i);
      for (size_t k = 0; k < n; k++) CHECK(p.name[k] >= 32 && p.name[k] < 127);   // printable ASCII (the UI font)
      CHECK((std::strncmp(p.name, "303 ", 4) == 0) == isAcidPreset(c, i));        // "303 " names exactly the acid family (generator: info.sound)
    }
    CHECK_EQ((int)names.size(), presetCount(c));
  }
}

// SPEC §9.22: SynthPatch grows sat / accent (the engine owner's change). The generated table carries them whenever the
// fields exist (the builder detects them); without the fields the check is skipped and says so. Library rows keep both
// OFF (core/types.h: a velocity-100 library clip must never be accented wholesale), the '303' families opt in at 0.5-0.8.
template <class P, class = void> struct PatchHasSatAccent : std::false_type {};
template <class P> struct PatchHasSatAccent<P, std::void_t<decltype(P::sat), decltype(P::accent)>> : std::true_type {};
template <class P> static bool checkSatAccent(const P& p, bool acid) {
  if constexpr (PatchHasSatAccent<P>::value) {
    if (acid) { CHECK(p.sat >= 0.5f && p.sat <= 0.8f); CHECK(p.accent >= 0.5f && p.accent <= 0.8f); }
    else { CHECK(p.sat == 0.f); CHECK(p.accent == 0.f); }
    return true;
  } else { (void)p; (void)acid; return false; }
}

TEST(acid_family_detection_and_303_character) {
  int satChecked = 0;
  for (Category c : kSynthCats) {
    const int lib = libraryPresetCount(c), all = presetCount(c);
    CHECK(!isAcidPreset(c, -1)); CHECK(!isAcidPreset(c, all));
    for (int i = 0; i < all; i++) {
      CHECK(isAcidPreset(c, i) == (i >= lib));
      if (checkSatAccent(preset(c, i), i >= lib)) satChecked++;
    }
    for (int i = lib; i < all; i++) {                                    // the whole '303' family, hand and generated
      const SynthPatch& p = preset(c, i);
      CHECK(p.resonance >= 0.4f && p.resonance <= 0.9f);
      CHECK(p.filterEnv >= 1.0f && p.filterEnv <= 3.0f);
      CHECK(p.wave1 == WSaw || p.wave1 == WSquare);
      CHECK(p.attack <= 0.005f);
      if (c == Lead || c == Bass) {                                      // mono, single oscillator, legato slides
        CHECK_EQ((int)p.polyphony, 1); CHECK(p.osc2Level == 0.f); CHECK(p.glide >= 0.04f && p.glide <= 0.08f);
        CHECK(p.sustain >= 0.25f && p.sustain <= 0.4f);
      } else {                                                           // Chord / Pad: 3-4 voice stabs
        CHECK(p.polyphony >= 3); CHECK(p.decay <= 0.25f); CHECK(p.sustain <= 0.2f);
      }
    }
  }
  CHECK_EQ(acidPresetCount(Fx), 0); CHECK_EQ(acidPresetCount(Beat), 0); CHECK_EQ(libraryPresetCount(Beat), 0);
  if (satChecked) std::printf("  sat / accent checked on %d presets ('303' rows 0.5-0.8, library rows 0 / 0)\n", satChecked);
  else std::printf("  SynthPatch has no sat / accent fields yet (SPEC §9.22, engine owner): values not checked\n");
}

TEST(generated_presets_sit_within_1_5_db_of_their_family_target) {
  // tools/preset_loudness.hpp: loudest 50 ms RMS of a fixed two-bar phrase at 120 BPM, the metric tools/gen_presets.py
  // --qa baked the generated rows' levels with (target = that family's hand-row median, within +-1 dB; the test allows
  // 1.5). The hand rows are the reference and are reported, not bounded. Single-voice peaks stay under the 1.5 the
  // render tests above require.
  int checked = 0; float worst = 0.f; const char* worstName = "";
  for (Category c : kSynthCats) {
    const int lib = libraryPresetCount(c), all = presetCount(c), hl = lp_qa::handLibrary(c), ha = lp_qa::handAcid(c);
    std::vector<lp_qa::Measure> ms((size_t)all);
    std::vector<float> handLib, handAcid;
    for (int i = 0; i < all; i++) {
      ms[(size_t)i] = lp_qa::measure(c, preset(c, i));
      CHECK(ms[(size_t)i].finite);
      CHECK(ms[(size_t)i].peakC4 < 1.5f); CHECK(ms[(size_t)i].peakExtreme < 1.5f);
      CHECK(ms[(size_t)i].loudDb > -40.f);                               // nothing near-silent
      if (i < hl) handLib.push_back(ms[(size_t)i].loudDb); else if (i >= lib && i < lib + ha) handAcid.push_back(ms[(size_t)i].loudDb);
    }
    const float tLib = lp_qa::medianOf(handLib), tAcid = ha ? lp_qa::medianOf(handAcid) : tLib;
    float lo = 1e9f, hi = -1e9f; int n = 0;
    for (int i = 0; i < all; i++) {
      const bool acid = i >= lib, hand = acid ? (i - lib) < ha : i < hl;
      if (hand) continue;
      const float d = ms[(size_t)i].loudDb - (acid ? tAcid : tLib);
      if (std::fabs(d) > 1.5f) std::printf("  OUTLIER %s/%d '%s' %+.2f dB (level %.3f)\n", categoryName(c), i, preset(c, i).name, d, preset(c, i).level);
      CHECK(std::fabs(d) <= 1.5f);
      if (std::fabs(d) > worst) { worst = std::fabs(d); worstName = preset(c, i).name; }
      if (d < lo) lo = d; if (d > hi) hi = d; n++; checked++;
    }
    std::printf("  %-5s targets lib %6.2f dB (hand %6.2f..%6.2f) acid %6.2f: %d generated rows at %+.2f..%+.2f dB\n", categoryName(c), tLib,
                *std::min_element(handLib.begin(), handLib.end()), *std::max_element(handLib.begin(), handLib.end()), tAcid, n, lo, hi);
  }
  std::printf("  %d generated presets checked, worst deviation %.2f dB ('%s')\n", checked, worst, worstName);
  CHECK_EQ(checked, 782 - 78);
}

TEST(preset_category_character) {
  for (int i = 0; i < libraryPresetCount(Pad); i++) {        // the library pads; the appended '303' family is deliberately not pad-like (SPEC §9.17b)
    const SynthPatch& p = preset(Pad, i);
    CHECK(p.attack >= 0.3f);
    CHECK(p.lfoRate > 0.f && (p.lfoToCutoff != 0.f || p.lfoToPitch != 0.f));
    CHECK(p.osc2Level > 0.f && p.osc2Detune != 0.f);
  }
  for (int i = libraryPresetCount(Pad); i < presetCount(Pad); i++) {   // the Pad '303' stabs: fast attack, short amp decay, LFO and detune kept
    const SynthPatch& p = preset(Pad, i);
    CHECK(p.attack >= 0.002f && p.attack <= 0.005f); CHECK(p.decay <= 0.25f); CHECK(p.sustain <= 0.2f);
    CHECK(p.resonance >= 0.4f && p.resonance <= 0.8f); CHECK(p.polyphony >= 3);
    CHECK(p.lfoRate > 0.f && p.osc2Level > 0.f && p.osc2Detune != 0.f);
  }
  for (int i = 0; i < presetCount(Fx); i++) {
    const SynthPatch& p = preset(Fx, i);
    CHECK_NEAR(p.glide, 0.12f, 1e-6);
    CHECK(p.resonance >= 0.4f && p.resonance <= 0.7f);
  }
  int bassGlide = 0, bassSub = 0, bassDetuned = 0;
  for (int i = 0; i < presetCount(Bass); i++) {
    const SynthPatch& p = preset(Bass, i);
    if (p.glide > 0.f && p.resonance >= 0.5f) bassGlide++;
    if (p.subLevel > 0.f) bassSub++;
    if (p.osc2Detune >= 0.3f && p.osc2Level >= 0.8f) bassDetuned++;
  }
  CHECK(bassGlide >= 2); CHECK(bassSub >= 3); CHECK(bassDetuned >= 2);   // 303-like, sub, reese
  int chordPluck = 0, chordSustain = 0;
  for (int i = 0; i < presetCount(Chord); i++) { const SynthPatch& p = preset(Chord, i); if (p.sustain == 0.f) chordPluck++; if (p.sustain >= 0.9f) chordSustain++; }
  CHECK(chordPluck >= 2); CHECK(chordSustain >= 1);                     // plucks and an organ
  CHECK(glidePatch().glide > 0.1f); CHECK(glidePatch().polyphony >= 4);
}

TEST(make_patch_spread_is_deterministic_and_within_8_percent) {
  for (Category c : kSynthCats) for (int i = 0; i < presetCount(c); i++) {
    const SynthPatch& base = preset(c, i);
    SynthPatch a, b, d; std::memset(&a, 0, sizeof a); std::memset(&b, 0, sizeof b);
    makePatch(c, i, 1234u, a); makePatch(c, i, 1234u, b); makePatch(c, i, 99u, d);
    CHECK(std::memcmp(&a, &b, sizeof(SynthPatch)) == 0);                  // same seed, same patch
    CHECK(std::strcmp(a.name, base.name) == 0);
    CHECK(a.cutoff >= base.cutoff * 0.919f && a.cutoff <= base.cutoff * 1.081f);
    CHECK(a.decay >= base.decay * 0.919f && a.decay <= base.decay * 1.081f);
    CHECK(a.resonance >= base.resonance * 0.919f && a.resonance <= base.resonance * 1.081f);
    CHECK(std::fabs(a.osc2Detune) >= std::fabs(base.osc2Detune) * 0.919f && std::fabs(a.osc2Detune) <= std::fabs(base.osc2Detune) * 1.081f);
    CHECK(a.resonance <= 0.95f && a.cutoff <= 10500.f);
    // untouched fields are identical
    CHECK(a.attack == base.attack && a.release == base.release && a.level == base.level && a.wave1 == base.wave1);
  }
  // seeds differ → at least some spread differs across the bank
  int differ = 0;
  for (int i = 0; i < presetCount(Lead); i++) { SynthPatch a, b; makePatch(Lead, i, 1u, a); makePatch(Lead, i, 2u, b); if (a.cutoff != b.cutoff) differ++; }
  CHECK(differ >= presetCount(Lead) / 2);
  // seed 0 is valid and still spreads
  SynthPatch z; makePatch(Chord, 0, 0u, z); CHECK(std::isfinite(z.cutoff)); CHECK(z.cutoff > 0.f);
  // spread is clamped: a patch at the ceiling / floor never leaves the valid range, and the clamp really engages
  SynthPatch hi = sinePatch(); hi.cutoff = 10500.f; hi.resonance = 0.95f; hi.decay = 10.f; hi.osc2Detune = 24.f;
  SynthPatch lo = sinePatch(); lo.cutoff = 20.f; lo.resonance = 0.f; lo.decay = 0.001f; lo.osc2Detune = -24.f;
  int clampedHi = 0, clampedLo = 0;
  for (uint32_t seed = 1; seed <= 64; seed++) {
    SynthPatch a = hi; applySpread(a, seed);
    CHECK(a.cutoff <= 10500.f && a.cutoff >= 10500.f * 0.919f);
    CHECK(a.resonance <= 0.95f && a.resonance >= 0.95f * 0.919f);
    CHECK(a.decay <= 10.f && a.decay >= 10.f * 0.919f);
    CHECK(a.osc2Detune <= 24.f && a.osc2Detune >= 24.f * 0.919f);
    if (a.cutoff == 10500.f) clampedHi++;
    SynthPatch b = lo; applySpread(b, seed);
    CHECK(b.cutoff >= 20.f && b.cutoff <= 20.f * 1.081f);
    CHECK(b.resonance == 0.f);
    CHECK(b.decay >= 0.001f && b.decay <= 0.001f * 1.081f);
    CHECK(b.osc2Detune >= -24.f && b.osc2Detune <= -24.f * 0.919f);
    if (b.cutoff == 20.f) clampedLo++;
  }
  CHECK(clampedHi >= 16 && clampedHi <= 48);                                // about half the seeds spread upwards
  CHECK(clampedLo >= 16 && clampedLo <= 48);
  // makePatch is preset + applySpread, deterministic in (seed, index)
  SynthPatch m1, m2; std::memset(&m1, 0, sizeof m1); std::memset(&m2, 0, sizeof m2);   // padding bytes too
  makePatch(Bass, 2, 77u, m1); makePatch(Bass, 2, 77u, m2);
  CHECK(std::memcmp(&m1, &m2, sizeof(SynthPatch)) == 0);
  CHECK(m1.cutoff != preset(Bass, 2).cutoff || m1.decay != preset(Bass, 2).decay);   // something was spread
}

// ---------------------------------------------------------------- presets: every preset renders cleanly
TEST(every_preset_renders_c4_clean_and_releases_in_time) {
  int rendered = 0; float maxPk = 0; const char* loudest = "";
  for (Category c : kSynthCats) for (int i = 0; i < presetCount(c); i++) for (int seedStep = 0; seedStep < 3; seedStep++) {
    SynthPatch p; makePatch(c, i, 1000u + 7u * seedStep, p);
    SynthVoice v; v.init(); v.setPatch(&p);
    v.noteOn(60, 100);
    std::vector<float> out;
    bool heldAlive = renderInto(v, out, kSr / 2);                         // 0.5 s hold
    CHECK(heldAlive || p.sustain == 0.f);                                 // zero-sustain plucks may free themselves
    v.noteOff();
    int limit = (int)((p.release * 1.5f + 0.050f) * kSr);
    size_t holdEnd = out.size();
    bool alive = true; int t = 0;
    while (alive && t < limit) { alive = renderInto(v, out, 64, 64); t += 64; }
    if (alive) std::printf("  %s/%s still active after release limit %d samples\n", categoryName(c), p.name, limit);
    CHECK(!alive);
    CHECK(!v.active());
    if (!finiteAll(out)) std::printf("  %s/%s produced NaN/inf\n", categoryName(c), p.name);
    CHECK(finiteAll(out));
    float pk = peakOf(out);
    if (pk >= 1.5f || pk < 0.01f) std::printf("  %s/%s peak %.3f\n", categoryName(c), p.name, pk);
    CHECK(pk < 1.5f);
    CHECK(pk > 0.01f);                                                     // non-silent
    if (pk > maxPk) { maxPk = pk; loudest = p.name; }
    CHECK(peakOf(out, 0, holdEnd) > 0.01f);                                // sound during the hold, not only the tail
    CHECK(std::fabs(out.back()) < 0.01f);                                 // released to silence
    rendered++;
  }
  std::printf("  rendered %d preset instances, loudest single voice at vel 100: %s peak %.3f\n", rendered, loudest, maxPk);
}

TEST(every_preset_tolerates_extreme_pitch_and_velocity) {
  for (Category c : kSynthCats) for (int i = 0; i < presetCount(c); i++) {
    for (uint8_t pitch : { (uint8_t)24, (uint8_t)108 }) for (uint8_t vel : { (uint8_t)3, (uint8_t)127 }) {
      SynthVoice v; v.init(); v.setPatch(&preset(c, i));
      v.noteOn(pitch, vel);
      std::vector<float> out; renderInto(v, out, kSr / 4); v.noteOff(); renderInto(v, out, kSr / 2);
      CHECK(finiteAll(out));
      CHECK(peakOf(out) < 1.5f);
    }
  }
}

// ---------------------------------------------------------------- voice behaviour
TEST(idle_voice_renders_nothing_and_returns_false) {
  SynthVoice v; v.init();
  float buf[64]; for (float& x : buf) x = 0.25f;
  CHECK(!v.render(buf, 64));
  for (float x : buf) CHECK_NEAR(x, 0.25f, 0.f);
  CHECK(!v.active()); CHECK_EQ((int)v.age(), 0);
  v.noteOff();                                                             // harmless on an idle voice
  CHECK(!v.active());
}

TEST(note_on_without_patch_uses_defaults_and_sounds) {
  SynthVoice v; v.init();
  v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, kSr / 10);
  CHECK(v.active()); CHECK(finiteAll(out)); CHECK(peakOf(out) > 0.05f); CHECK(peakOf(out) < 1.5f);
  CHECK_EQ((int)v.age(), kSr / 10); CHECK_EQ((int)v.pitch(), 60);
}

TEST(pitch_matches_midi_note_via_zero_crossings) {
  SynthPatch p = sinePatch();
  for (int midi : { 36, 48, 60, 69, 84 }) {
    SynthVoice v; v.init(); v.setPatch(&p);
    v.noteOn((uint8_t)midi, 100);
    std::vector<float> out; renderInto(v, out, kSr);
    float hz = zcHz(out, kSr / 10, kSr);
    float want = midiToHz((float)midi);
    CHECK_NEAR(hz, want, want * 0.01f);
  }
}

TEST(velocity_curve_is_half_square_half_linear) {
  SynthPatch p = sinePatch();
  auto peakAt = [&](uint8_t vel) {
    SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(69, vel);
    std::vector<float> out; renderInto(v, out, kSr / 5);
    return peakOf(out, kSr / 10, kSr / 5);
  };
  float p127 = peakAt(127), p64 = peakAt(64), p32 = peakAt(32);
  std::printf("  vel peaks: 127 %.4f 64 %.4f 32 %.4f\n", p127, p64, p32);
  float c64 = 0.5f * (64.f / 127) * (64.f / 127) + 0.5f * (64.f / 127);
  float c32 = 0.5f * (32.f / 127) * (32.f / 127) + 0.5f * (32.f / 127);
  CHECK_NEAR(p64 / p127, c64, 0.02f);
  CHECK_NEAR(p32 / p127, c32, 0.02f);
  CHECK_NEAR(p127, 0.5f, 0.005f);                                           // level × velCurve(127) = 1 × a ±1.0 sine
}

TEST(velocity_zero_is_a_note_off) {
  SynthPatch p = sinePatch();
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(60, 100); std::vector<float> out; renderInto(v, out, 2048);
  CHECK(!v.releasing());
  v.noteOn(60, 0);
  CHECK(v.releasing());
  bool alive = renderInto(v, out, ms(200));
  CHECK(!alive);
}

TEST(release_ends_voice_within_release_time_bound) {
  for (float rel : { 0.02f, 0.1f, 0.4f, 1.0f }) {
    SynthPatch p = sinePatch(); p.release = rel; p.sustain = 1.0f;
    SynthVoice v; v.init(); v.setPatch(&p);
    v.noteOn(60, 127); std::vector<float> out; renderInto(v, out, ms(100));
    v.noteOff(); CHECK(v.releasing());
    int limit = (int)((rel * 1.5f + 0.05f) * kSr), t = 0; bool alive = true;
    while (alive && t < limit) { alive = renderInto(v, out, 32, 32); t += 32; }
    CHECK(!alive);
    CHECK(t >= (int)(rel * 0.5f * kSr));                                  // but not unreasonably early either
  }
}

TEST(zero_sustain_voice_frees_itself_after_decay) {
  SynthPatch p = sinePatch(); p.decay = 0.1f; p.sustain = 0.f; p.release = 2.0f;
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(60, 100); std::vector<float> out;
  bool alive = renderInto(v, out, ms(400));                               // never released
  CHECK(!alive); CHECK(!v.active());
  CHECK(peakOf(out, 0, ms(20)) > 0.3f);
  CHECK(peakOf(out, ms(300), ms(400)) < 1e-3f);
}

TEST(kill_silences_immediately) {
  SynthPatch p = sinePatch();
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, 1024);
  v.kill(); CHECK(!v.active()); CHECK(!v.releasing());
  float buf[64] = {0}; CHECK(!v.render(buf, 64)); CHECK_NEAR(peakOf(std::vector<float>(buf, buf + 64)), 0.f, 0.f);
}

TEST(render_output_is_independent_of_block_size) {
  SynthPatch p = preset(Lead, 0); p.noiseLevel = 0.2f; p.subLevel = 0.3f; p.drive = 0.3f;   // every kernel feature on
  SynthVoice a; a.init(); a.setPatch(&p);
  a.noteOn(62, 90);
  std::vector<float> oa, ob; renderInto(a, oa, 4096, 256);
  a.kill(); a.noteOn(62, 90);                                               // same voice again: same noise seed
  int sizes[] = { 1, 7, 33, 100, 256, 3 }; int k = 0, left = 4096;
  while (left > 0) { int c = sizes[k++ % 6]; if (c > left) c = left; renderInto(a, ob, c, c); left -= c; }
  CHECK_EQ(oa.size(), ob.size());
  float maxDiff = 0; for (size_t i = 0; i < oa.size(); i++) maxDiff = std::fmax(maxDiff, std::fabs(oa[i] - ob[i]));
  CHECK_NEAR(maxDiff, 0.f, 1e-6f);
  CHECK(peakOf(ob) > 0.05f);
}

TEST(noise_is_deterministic_per_voice_and_differs_between_voices) {
  SynthPatch p = sinePatch(); p.noiseLevel = 1.0f;
  SynthVoice a, b; a.init(); b.init(); a.setPatch(&p); b.setPatch(&p);
  a.noteOn(60, 100); std::vector<float> o1; renderInto(a, o1, 2048);
  a.noteOff(); renderInto(a, o1, ms(200)); CHECK(!a.active());
  a.noteOn(60, 100); std::vector<float> o2; renderInto(a, o2, 2048);        // second note on the same voice
  float diff = 0; for (size_t i = 0; i < o2.size(); i++) diff = std::fmax(diff, std::fabs(o1[i] - o2[i]));
  CHECK_NEAR(diff, 0.f, 0.f);                                                // bit-identical: reproducible noise
  b.noteOn(60, 100); std::vector<float> o3; renderInto(b, o3, 2048);
  int same = 0; for (size_t i = 0; i < o3.size(); i++) if (o3[i] == o2[i]) same++;
  CHECK(same < (int)o3.size() / 10);                                         // another voice: another sequence
}

TEST(glide_moves_pitch_gradually_over_patch_glide_time) {
  SynthPatch p = sinePatch(); p.glide = 0.1f;
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(48, 100);                                                        // first note: no glide from nowhere
  std::vector<float> out; renderInto(v, out, ms(200));
  float f0 = midiToHz(48), f1 = midiToHz(60);
  CHECK_NEAR(zcHz(out, ms(100), ms(200)), f0, f0 * 0.01f);
  v.noteOn(60, 100, true);
  size_t t0 = out.size(); renderInto(v, out, ms(400));
  float early = zcHz(out, t0, t0 + ms(25));                                // first 25 ms: still near the start
  float mid = zcHz(out, t0 + ms(35), t0 + ms(70));
  float late = zcHz(out, t0 + ms(250), t0 + ms(400));
  std::printf("  glide: %.1f -> early %.1f mid %.1f late %.1f (target %.1f)\n", f0, early, mid, late, f1);
  CHECK(early > f0 * 1.02f && early < f0 + (f1 - f0) * 0.6f);
  CHECK(mid > early && mid < f1 * 0.99f);
  CHECK_NEAR(late, f1, f1 * 0.01f);
  // Windowed frequencies never go backwards
  float prev = 0; bool monotone = true;
  for (int w = 0; w < 8; w++) { float f = zcHz(out, t0 + ms(w * 15.f), t0 + ms(w * 15.f + 30.f)); if (f < prev * 0.995f) monotone = false; prev = f; }
  CHECK(monotone);
  // Faster glide settles sooner: 5 ms minimum
  SynthPatch q = sinePatch(); q.glide = 0.001f;
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(48, 100); std::vector<float> o2; renderInto(w, o2, ms(100));
  w.noteOn(60, 100, true); size_t s0 = o2.size(); renderInto(w, o2, ms(100));
  CHECK_NEAR(zcHz(o2, s0 + ms(30), s0 + ms(100)), f1, f1 * 0.01f);
}

TEST(non_legato_note_with_glide_slides_from_last_pitch_on_a_mono_patch) {
  SynthPatch p = sinePatch(); p.glide = 0.06f; p.release = 0.01f; p.polyphony = 1;   // 303 style: mono
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(48, 100); std::vector<float> out; renderInto(v, out, ms(100)); v.noteOff(); renderInto(v, out, ms(100));
  CHECK(!v.active());
  v.noteOn(60, 100);                                                        // 303 style: slides from the previous note
  size_t t0 = out.size(); renderInto(v, out, ms(300));
  float f0 = midiToHz(48), f1 = midiToHz(60);
  float early = zcHz(out, t0, t0 + ms(20));
  CHECK(early < f1 * 0.9f && early > f0 * 0.95f);
  CHECK_NEAR(zcHz(out, t0 + ms(150), t0 + ms(300)), f1, f1 * 0.01f);
  // Without glide a fresh note snaps immediately
  SynthPatch q = sinePatch(); SynthVoice w; w.init(); w.setPatch(&q);
  w.noteOn(48, 100); std::vector<float> o2; renderInto(w, o2, ms(100)); w.noteOn(60, 100);
  size_t s0 = o2.size(); renderInto(w, o2, ms(100));
  CHECK_NEAR(zcHz(o2, s0 + ms(5), s0 + ms(100)), f1, f1 * 0.015f);
}

TEST(legato_note_on_does_not_retrigger_envelope) {
  SynthPatch p = sinePatch(); p.attack = 0.15f; p.sustain = 1.0f;
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(48, 100); std::vector<float> out; renderInto(v, out, ms(300));
  float held = peakOf(out, ms(250), ms(300));
  CHECK(held > 0.3f);
  v.noteOn(60, 100, true);
  size_t t0 = out.size(); renderInto(v, out, ms(60));
  CHECK(peakOf(out, t0, t0 + ms(10)) > held * 0.8f);                       // no dip at the legato transition
  CHECK(v.active()); CHECK(!v.releasing()); CHECK_EQ((int)v.pitch(), 60);
  // A non-legato retrigger of a fresh voice starts from zero: first 10 ms much quieter than the held level
  SynthVoice w; w.init(); w.setPatch(&p); w.noteOn(60, 100);
  std::vector<float> o2; renderInto(w, o2, ms(10));
  CHECK(peakOf(o2) < held * 0.2f);
  // Legato on an idle voice is just a note on
  SynthVoice u; u.init(); u.setPatch(&p); u.noteOn(60, 100, true);
  std::vector<float> o3; CHECK(renderInto(u, o3, ms(20))); CHECK(u.active()); CHECK(peakOf(o3) > 0.f);
}

TEST(lfo_modulates_pitch) {
  SynthPatch p = sinePatch(); p.lfoRate = 2.0f; p.lfoToPitch = 4.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, kSr);
  float lo = 1e9f, hi = 0;
  for (int w = 0; w < 20; w++) { float f = zcHz(out, (size_t)(w * kSr / 20), (size_t)((w + 1) * kSr / 20)); if (f < lo) lo = f; if (f > hi) hi = f; }
  std::printf("  pitch lfo: %.1f .. %.1f Hz\n", lo, hi);
  CHECK(hi / lo > 1.3f);                                                    // ±4 st = 1.59 ratio, windows smear it
  float base = midiToHz(60); CHECK(lo < base * 0.95f); CHECK(hi > base * 1.05f);
}

TEST(lfo_modulates_cutoff) {
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.cutoff = 300.f; p.lfoRate = 1.0f; p.lfoToCutoff = 3.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 100);
  std::vector<float> out; renderInto(v, out, kSr);
  float lo = 1e9f, hi = 0;
  for (int w = 2; w < 20; w++) { float b = brightness(out, (size_t)(w * kSr / 20), (size_t)((w + 1) * kSr / 20)); if (b < lo) lo = b; if (b > hi) hi = b; }
  std::printf("  cutoff lfo brightness: %.3f .. %.3f\n", lo, hi);
  CHECK(hi / lo > 1.5f);
  SynthPatch q = p; q.lfoToCutoff = 0.f; q.lfoRate = 0.f;                // without LFO the brightness is steady
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(48, 100); std::vector<float> o2; renderInto(w, o2, kSr);
  float lo2 = 1e9f, hi2 = 0;
  for (int k = 2; k < 20; k++) { float b = brightness(o2, (size_t)(k * kSr / 20), (size_t)((k + 1) * kSr / 20)); if (b < lo2) lo2 = b; if (b > hi2) hi2 = b; }
  CHECK(hi2 / lo2 < 1.1f);
}

TEST(filter_envelope_opens_then_closes) {
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.cutoff = 200.f; p.filterEnv = 4.0f; p.fAttack = 0.001f; p.fDecay = 0.4f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 100);
  std::vector<float> out; renderInto(v, out, kSr / 2);
  float early = brightness(out, ms(2), ms(30)), late = brightness(out, ms(400), ms(500));
  std::printf("  filter env brightness early %.3f late %.3f\n", early, late);
  CHECK(early > late * 2.0f);
}

TEST(key_tracking_raises_cutoff_with_pitch) {
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.cutoff = 1500.f; p.keyTrack = 1.0f;
  auto bright = [&](uint8_t midi, float kt) {
    SynthPatch q = p; q.keyTrack = kt; SynthVoice v; v.init(); v.setPatch(&q); v.noteOn(midi, 100);
    std::vector<float> out; renderInto(v, out, kSr / 4); return brightness(out, ms(50), ms(250));
  };
  // Brightness relative to the fundamental: with full tracking a note two octaves up keeps its spectrum shape
  // (the cutoff follows to 6 kHz and passes all six partials of the C6 saw, 1.0..6.3 kHz); without tracking the
  // fixed 1500 Hz filter removes most of them. (A 300 Hz base met the 1.2x bar only through the folded partials
  // of the former 40-harmonic table at C6; the pitch-ranged set is clean, so the cutoff must sit among the partials.)
  float trackedHi = bright(84, 1.0f), fixedHi = bright(84, 0.0f);
  float low = bright(60, 0.0f);
  std::printf("  C6 saw brightness: tracked %.3f fixed %.3f\n", trackedHi, fixedHi);
  CHECK(trackedHi > fixedHi * 1.2f);
  CHECK(low > 0.f);
}

// Hann-windowed magnitude of x at one frequency (relative units); 1 s of signal separates tones >= 70 Hz apart.
static double toneLevel(const std::vector<float>& x, double hz) {
  double re = 0, im = 0; const size_t n = x.size();
  for (size_t i = 0; i < n; i++) {
    double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (double)i / (double)n), a = 2.0 * M_PI * hz * (double)i / kSr;
    re += x[i] * w * std::cos(a); im += x[i] * w * std::sin(a);
  }
  return std::sqrt(re * re + im * im);
}

TEST(oscillators_pick_the_table_set_by_pitch) {
  // Aliasing fix (wavetables.h): the former single 40-partial saw folded partials 22..40 at C5 (40 f0 lands at
  // 1120 Hz, -32 dB). noteOn now takes the set of each oscillator's OWN pitch: osc 1 by the note, osc 2 by note
  // + octave offset (+ detune), the sub by note - 12. Measured through the whole voice with an open filter.
  auto renderOne = [](const SynthPatch& p, uint8_t midi) { SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(midi, 100); std::vector<float> o; renderInto(v, o, kSr); return o; };
  const double f72 = midiToHz(72), alias40 = kSr - 40 * f72;                 // 1120 Hz: where partial 40 of C5 folds to
  // osc 1: saw at C5 is on set 1: partial 2 at 1/2, the 1120 Hz alias bin empty (set 0 would put -32 dB there).
  SynthPatch p = sinePatch(); p.wave1 = WSaw;
  std::vector<float> a = renderOne(p, 72); double fund = toneLevel(a, f72);
  CHECK_NEAR(toneLevel(a, 2 * f72) / fund, 0.5, 0.03);
  CHECK(toneLevel(a, alias40) / fund < 0.003);
  // Up to the boundary (MIDI 60, SPEC §9.15) the rich set is kept: a saw at MIDI 59 still has its 30th partial (7.4 kHz).
  std::vector<float> b = renderOne(p, 59); const double f59 = midiToHz(59);
  CHECK_NEAR(toneLevel(b, 30 * f59) / toneLevel(b, f59), 1.0 / 30, 0.01);
  // osc 2 counts its octave offset: a sine note at C3 with a saw osc 2 two octaves up (C5) must not alias either.
  SynthPatch q = sinePatch(); q.wave1 = WSine; q.wave2 = WSaw; q.osc2Level = 1.0f; q.osc2Octave = 2;
  std::vector<float> c = renderOne(q, 48); fund = toneLevel(c, f72);
  CHECK_NEAR(toneLevel(c, 2 * f72) / fund, 0.5, 0.03);                       // osc 2's own 2nd partial is there
  CHECK(toneLevel(c, alias40) / fund < 0.003);                               // set 1 for osc 2 (set 0 by the note: -32 dB)
  // ... and its detune (semitones): +1 octave +7 st from MIDI 53 is C5 again.
  SynthPatch r = q; r.osc2Octave = 1; r.osc2Detune = 7.0f;
  std::vector<float> d = renderOne(r, 53); fund = toneLevel(d, f72);
  CHECK_NEAR(toneLevel(d, 2 * f72) / fund, 0.5, 0.03);
  CHECK(toneLevel(d, alias40) / fund < 0.003);
  // The sub (square one octave down) at C6 plays C5: set 1's square (odd partials <= 15) instead of set 0's
  // (odd <= 39, whose 39th folds to 1643 Hz at -32 dB).
  SynthPatch s = sinePatch(); s.subLevel = 1.0f;
  std::vector<float> e = renderOne(s, 84); fund = toneLevel(e, f72);
  CHECK_NEAR(toneLevel(e, 3 * f72) / fund, 1.0 / 3, 0.02);
  CHECK(toneLevel(e, kSr - 39 * f72) / fund < 0.003);
  std::printf("  C5 alias bin (1120 Hz) re fundamental: osc1 %.1f dB, osc2 +2 oct %.1f dB, sub of C6 (1643 Hz) %.1f dB\n",
              20 * std::log10(toneLevel(a, alias40) / toneLevel(a, f72)), 20 * std::log10(toneLevel(c, alias40) / toneLevel(c, f72)),
              20 * std::log10(toneLevel(e, kSr - 39 * f72) / fund));
}

TEST(sub_oscillator_sits_one_octave_below) {
  // LP at 150 Hz: osc1 (261.6 Hz) is attenuated well below the sub's fundamental at 130.8 Hz.
  SynthPatch p = sinePatch(); p.subLevel = 1.0f; p.cutoff = 150.f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, kSr / 2);
  float f = zcHz(out, ms(50), ms(500));
  CHECK_NEAR(f, midiToHz(48), midiToHz(48) * 0.02f);
  SynthPatch q = p; q.subLevel = 0.f;                                       // same filter without the sub: osc1 remains
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(60, 100); std::vector<float> o2; renderInto(w, o2, kSr / 2);
  CHECK_NEAR(zcHz(o2, ms(50), ms(500)), midiToHz(60), midiToHz(60) * 0.02f);
}

TEST(sub_oscillator_is_band_limited) {
  // A naive sign-bit square at sub level 1.0 would jump by 2.0 in one sample; the wavetable square (40
  // harmonics) spreads its edge over several samples at C1 (sub 32.7 Hz, 674 samples per cycle).
  SynthPatch p = sinePatch(); p.subLevel = 1.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(36, 127);
  std::vector<float> out; renderInto(v, out, kSr / 2);
  float step = maxStep(out, ms(20), ms(500));
  std::printf("  sub max per-sample step %.3f (naive square would be %.3f)\n", step, 2.0f * 0.5f);
  CHECK(step < 0.5f * 0.5f);                                                // well under half a naive edge
  CHECK(peakOf(out, ms(20), ms(500)) > 0.5f);                               // and the sub is clearly there
}

TEST(osc2_octave_and_detune_change_the_sound) {
  SynthPatch p = sinePatch(); p.osc2Level = 0.4f; p.osc2Octave = 1; p.osc2Detune = 0.f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, kSr / 4);
  // sine + sine an octave up keeps the fundamental's rising zero crossings
  CHECK_NEAR(zcHz(out, ms(20), ms(250)), midiToHz(60), midiToHz(60) * 0.02f);
  SynthPatch solo = sinePatch(); SynthVoice vs; vs.init(); vs.setPatch(&solo); vs.noteOn(60, 100); std::vector<float> os; renderInto(vs, os, kSr / 4);
  CHECK(peakOf(out, ms(20), ms(250)) > peakOf(os, ms(20), ms(250)) * 1.1f);   // two oscillators add up
  // detune 50 cents on a unison osc2 produces beating: the envelope of the sum varies
  SynthPatch q = sinePatch(); q.osc2Level = 1.0f; q.osc2Detune = 0.5f;
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(60, 100); std::vector<float> o2; renderInto(w, o2, kSr * 2);
  float pmin = 1e9f, pmax = 0;
  for (int k = 5; k < 100; k++) { float pk = peakOf(o2, (size_t)(k * kSr / 50), (size_t)((k + 1) * kSr / 50)); pmin = std::fmin(pmin, pk); pmax = std::fmax(pmax, pk); }
  CHECK(pmax / pmin > 2.0f);                                                // beat period 1/7.6 Hz ≈ 130 ms
}

TEST(noise_and_drive_are_bounded_and_audible) {
  SynthPatch p = sinePatch(); p.noiseLevel = 1.0f; p.level = 1.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 127);
  std::vector<float> out; renderInto(v, out, kSr / 4);
  CHECK(finiteAll(out)); CHECK(peakOf(out) < 2.5f); CHECK(peakOf(out) > 1.0f);
  // two voices do not produce identical noise
  SynthVoice w; w.init(); w.setPatch(&p); w.noteOn(60, 127); std::vector<float> o2; renderInto(w, o2, kSr / 4);
  int same = 0; for (size_t i = 0; i < out.size(); i++) if (out[i] == o2[i]) same++;
  CHECK(same < (int)out.size() / 10);
  // drive: bounded (soft clip) and different from the clean signal
  SynthPatch d = sinePatch(); d.wave1 = WSaw; d.level = 1.0f; d.drive = 1.0f;
  SynthPatch c = d; c.drive = 0.f;
  SynthVoice vd, vc; vd.init(); vc.init(); vd.setPatch(&d); vc.setPatch(&c); vd.noteOn(60, 127); vc.noteOn(60, 127);
  std::vector<float> od, oc; renderInto(vd, od, kSr / 4); renderInto(vc, oc, kSr / 4);
  CHECK(peakOf(od, ms(20), ms(250)) < 1.1f);
  float diff = 0; for (size_t i = ms(20); i < od.size(); i++) diff = std::fmax(diff, std::fabs(od[i] - oc[i]));
  CHECK(diff > 0.05f);
}

TEST(patch_change_during_note_does_not_affect_the_sounding_voice) {
  SynthPatch p = sinePatch();
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> a; renderInto(v, a, 1024);
  p.level = 0.f; p.cutoff = 20.f; p.wave1 = WSquare;                        // mutate the live patch
  renderInto(v, a, 1024);
  CHECK(peakOf(a, 1024, 2048) > 0.3f);                                      // still the old sound
}

TEST(age_counts_samples_since_note_on_and_resets_on_legato) {
  SynthPatch p = sinePatch();
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, 1000, 100);
  CHECK_EQ((int)v.age(), 1000);
  v.noteOn(62, 100, true); CHECK_EQ((int)v.age(), 0);
  renderInto(v, out, 300, 300); CHECK_EQ((int)v.age(), 300);
}

TEST(cost_per_sample_on_host) {
  SynthPatch full = preset(Fx, 6);                                           // 2 osc + noise + drive + BP + glide
  full.subLevel = 0.5f; full.lfoRate = 3.f; full.lfoToCutoff = 1.f; full.lfoToPitch = 0.1f; full.sustain = 1.f;
  SynthPatch lean = sinePatch();
  auto measure = [&](const SynthPatch& p) {
    SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(52, 100);
    float buf[kBlock]; double best = 1e18; volatile float sink = 0;
    for (int rep = 0; rep < 5; rep++) {
      auto t0 = std::chrono::steady_clock::now();
      for (int b = 0; b < kSr / kBlock; b++) { memset(buf, 0, sizeof(buf)); v.render(buf, kBlock); sink = sink + buf[3]; }
      double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / (double)((kSr / kBlock) * kBlock);
      if (ns < best) best = ns;
    }
    CHECK(v.active());
    return best;
  };
  double nsFull = measure(full), nsLean = measure(lean);
  std::printf("  host cost: full voice %.1f ns/sample, lean sine voice %.1f ns/sample (ESP32-S3 target ~220)\n", nsFull, nsLean);
  CHECK(nsFull < 60.0);                                                     // ~5 ns on an M-series Mac; 60 leaves room for CI
  CHECK(nsLean < 40.0);
  CHECK(nsLean <= nsFull * 1.05);                                           // the feature mask really trims the lean path
}


// ---------------------------------------------------------------- fixed defects
TEST(fresh_poly_voice_with_glide_starts_on_pitch_but_mono_and_steal_slide) {
  // Finding 1: a long-idle polyphonic voice (Fx presets, polyphony 4) must not slide in from a stale pitch.
  SynthPatch p = sinePatch(); p.glide = 0.12f; p.release = 0.01f; p.polyphony = 4;
  float f0 = midiToHz(36), f1 = midiToHz(72);
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(36, 100); std::vector<float> out; renderInto(v, out, ms(100)); v.noteOff(); renderInto(v, out, kSr * 2);
  CHECK(!v.active());                                                       // idle for ~2 s
  v.noteOn(72, 100); size_t t0 = out.size(); renderInto(v, out, ms(100));
  float early = zcHz(out, t0 + ms(2), t0 + ms(20));
  std::printf("  fresh poly voice after 2 s idle: first 20 ms at %.1f Hz (target %.1f, stale %.1f)\n", early, f1, f0);
  CHECK_NEAR(early, f1, f1 * 0.015f);
  // The same patch with polyphony 1 slides (303 style) from the stale pitch.
  SynthPatch m = p; m.polyphony = 1;
  SynthVoice w; w.init(); w.setPatch(&m);
  w.noteOn(36, 100); std::vector<float> o2; renderInto(w, o2, ms(100)); w.noteOff(); renderInto(w, o2, kSr * 2);
  CHECK(!w.active());
  w.noteOn(72, 100); size_t s0 = o2.size(); renderInto(w, o2, ms(400));
  float earlyMono = zcHz(o2, s0 + ms(2), s0 + ms(20));
  CHECK(earlyMono > f0 * 0.95f && earlyMono < f1 * 0.8f);
  CHECK_NEAR(zcHz(o2, s0 + ms(300), s0 + ms(400)), f1, f1 * 0.01f);
  // A non-legato retrigger of a still-sounding poly voice (steal) also slides: the old pitch is audible.
  SynthVoice u; u.init(); u.setPatch(&p);
  u.noteOn(36, 100); std::vector<float> o3; renderInto(u, o3, ms(100));
  u.noteOn(72, 100); size_t r0 = o3.size(); renderInto(u, o3, ms(400));
  float earlySteal = zcHz(o3, r0 + ms(2), r0 + ms(20));
  CHECK(earlySteal > f0 * 0.95f && earlySteal < f1 * 0.8f);
  CHECK_NEAR(zcHz(o3, r0 + ms(300), r0 + ms(400)), f1, f1 * 0.01f);
}

TEST(legato_onto_releasing_voice_keeps_the_note_alive) {
  // Finding 2: the engine may pass legato while the previous note is already releasing.
  SynthPatch p = sinePatch(); p.release = 0.3f; p.glide = 0.0f;
  SynthVoice v; v.init(); v.setPatch(&p);
  v.noteOn(48, 100); std::vector<float> out; renderInto(v, out, ms(100));
  float held = peakOf(out, ms(50), ms(100));
  v.noteOff(); renderInto(v, out, ms(50)); CHECK(v.releasing());
  v.noteOn(60, 100, true);
  CHECK(!v.releasing()); CHECK(v.active()); CHECK_EQ((int)v.pitch(), 60);
  size_t t0 = out.size(); bool alive = renderInto(v, out, ms(600));
  CHECK(alive); CHECK(v.active()); CHECK(!v.releasing());
  CHECK(peakOf(out, t0 + ms(100), t0 + ms(600)) > held * 0.9f);            // back at full level, not fading out
  CHECK(maxStep(out, t0, t0 + ms(10)) < 0.08f);                             // no click at the transition
  CHECK_NEAR(zcHz(out, t0 + ms(200), t0 + ms(600)), midiToHz(60), midiToHz(60) * 0.01f);   // glided to the new pitch
  v.noteOff(); CHECK(v.releasing());
  int t = 0; alive = true; while (alive && t < ms(500)) { alive = renderInto(v, out, 64, 64); t += 64; }
  CHECK(!alive);                                                            // a later release still frees the voice
}

TEST(lfo_pitch_and_cutoff_depth_are_exact) {
  // Finding 3: the LFO is normalised to ±1.0, so lfoToPitch is exact in semitones ...
  SynthPatch p = sinePatch(); p.lfoRate = 0.25f; p.lfoToPitch = 12.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100);
  std::vector<float> out; renderInto(v, out, kSr * 4);
  float base = midiToHz(60);
  float hi = zcHz(out, kSr * 1 - ms(40), kSr * 1 + ms(40));                 // LFO peak at a quarter period (1 s)
  float lo = zcHz(out, kSr * 3 - ms(40), kSr * 3 + ms(40));                 // trough at 3 s
  std::printf("  pitch lfo ±12 st: hi %.1f (want %.1f) lo %.1f (want %.1f)\n", hi, base * 2, lo, base * 0.5f);
  CHECK_NEAR(hi, base * 2.0f, base * 2.0f * 0.015f);
  CHECK_NEAR(lo, base * 0.5f, base * 0.5f * 0.015f);
  // ... and lfoToCutoff is exact in octaves: at the LFO peak, 1 octave of LFO equals a static doubled cutoff.
  SynthPatch q = sinePatch(); q.wave1 = WSaw; q.cutoff = 400.f; q.lfoRate = 0.25f; q.lfoToCutoff = 1.0f;
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(48, 100); std::vector<float> o2; renderInto(w, o2, kSr * 2);
  float bLfo = brightness(o2, kSr - ms(20), kSr + ms(20));
  SynthPatch s = q; s.lfoRate = 0; s.lfoToCutoff = 0; s.cutoff = 800.f;
  SynthVoice u; u.init(); u.setPatch(&s); u.noteOn(48, 100); std::vector<float> o3; renderInto(u, o3, kSr * 2);
  float bStatic = brightness(o3, kSr - ms(20), kSr + ms(20));
  SynthPatch s2 = s; s2.cutoff = 800.f * std::pow(2.0f, 0.885f) / 2.0f;     // what the old ±0.885 LFO would have reached
  SynthVoice u2; u2.init(); u2.setPatch(&s2); u2.noteOn(48, 100); std::vector<float> o4; renderInto(u2, o4, kSr * 2);
  float bOld = brightness(o4, kSr - ms(20), kSr + ms(20));
  std::printf("  cutoff lfo 1 oct at peak: brightness %.4f vs static 2x %.4f (old 0.885 oct would be %.4f)\n", bLfo, bStatic, bOld);
  CHECK_NEAR(bLfo / bStatic, 1.0f, 0.02f);
  CHECK(std::fabs(bLfo - bStatic) < std::fabs(bLfo - bOld));                // closer to exact than to the old depth
}

TEST(retrigger_on_active_voice_slews_gain_without_a_step) {
  // Finding 4: a same-pitch steal from vel 127 to vel 20 used to jump the gain in one sample (step 0.325).
  SynthPatch p = sinePatch();
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 127);
  std::vector<float> out; renderInto(v, out, ms(100));
  float natural = maxStep(out, ms(50), ms(100));                            // a 131 Hz sine's own slope
  float loud = peakOf(out, ms(50), ms(100));
  v.noteOn(48, 20);                                                         // non-legato retrigger, much quieter
  size_t t0 = out.size(); renderInto(v, out, ms(100));
  float step = maxStep(out, t0 - 1, t0 + ms(5));
  float quiet = peakOf(out, t0 + ms(50), t0 + ms(100));
  std::printf("  retrigger 127->20: max step %.4f (natural slope %.4f), level %.3f -> %.3f\n", step, natural, loud, quiet);
  CHECK(step < natural + 0.02f);                                            // gain ramps over 32 samples: no click
  float c20 = 0.5f * (20.f / 127) * (20.f / 127) + 0.5f * (20.f / 127);
  CHECK_NEAR(quiet / loud, c20, 0.01f);                                     // and the new velocity is reached
  CHECK_EQ((int)v.age(), ms(100));                                          // age restarted at the retrigger
}

TEST(glide_is_linear_in_semitones_so_up_and_down_are_symmetric) {
  // Finding 5: a one-pole on Hz made C3->C5 and C5->C3 progress differently; in semitones both are identical.
  SynthPatch p = sinePatch(); p.glide = 0.2f; p.polyphony = 1;
  auto progressAt = [&](uint8_t from, uint8_t to, float msAt) {
    SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(from, 100);
    std::vector<float> out; renderInto(v, out, ms(300));
    v.noteOn(to, 100, true); size_t t0 = out.size(); renderInto(v, out, ms(600));
    float st = zcSemitones(out, t0 + ms(msAt - 5.f), t0 + ms(msAt + 5.f));
    return (st - (float)from) / ((float)to - (float)from);
  };
  float up50 = progressAt(48, 72, 50.f), down50 = progressAt(72, 48, 50.f);
  float up100 = progressAt(48, 72, 100.f), down100 = progressAt(72, 48, 100.f);
  std::printf("  glide progress (semitone fraction) at 50 ms: up %.3f down %.3f, at 100 ms: up %.3f down %.3f\n", up50, down50, up100, down100);
  float want50 = 1.0f - std::exp(-4.6051702f * 0.05f / 0.2f), want100 = 1.0f - std::exp(-4.6051702f * 0.1f / 0.2f);
  CHECK_NEAR(up50, down50, 0.04f);
  CHECK_NEAR(up100, down100, 0.04f);
  CHECK_NEAR(up50, want50, 0.08f);                                          // 99 % of the jump in `glide` seconds
  CHECK_NEAR(up100, want100, 0.08f);
  CHECK(up100 > up50);
  // 5 ms minimum glide: settles within 30 ms
  SynthPatch q = sinePatch(); q.glide = 0.001f; q.polyphony = 1;
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(48, 100); std::vector<float> o2; renderInto(w, o2, ms(100));
  w.noteOn(60, 100); size_t s0 = o2.size(); renderInto(w, o2, ms(100));
  CHECK_NEAR(zcHz(o2, s0 + ms(30), s0 + ms(100)), midiToHz(60), midiToHz(60) * 0.01f);
}

TEST(filter_modes_lp_bp_hp_shape_the_spectrum) {
  // Finding 10: HP (mode 2) was used by no preset and tested nowhere.
  auto bright = [](uint8_t mode, float cutoff) {
    SynthPatch p = sinePatch(); p.wave1 = WSaw; p.cutoff = cutoff; p.filterMode = mode; p.resonance = 0.2f;
    SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 100);
    std::vector<float> out; renderInto(v, out, kSr / 4);
    return brightness(out, ms(50), ms(250));
  };
  float lp = bright(0, 800.f), bp = bright(1, 800.f), hp = bright(2, 800.f), open = bright(0, 10000.f);
  std::printf("  saw brightness: LP %.3f BP %.3f HP %.3f open %.3f\n", lp, bp, hp, open);
  CHECK(hp > open * 1.3f);                                                  // HP removes the fundamental: brighter than the raw saw
  CHECK(open > lp * 1.5f);                                                  // LP darker than the raw saw
  CHECK(bp > lp && bp < hp);                                                // BP in between
  // HP really removes the low end: the low-passed version of the HP output is small relative to its RMS.
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.cutoff = 800.f; p.filterMode = 2; p.level = 1.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 100);
  std::vector<float> out; renderInto(v, out, kSr / 4);
  double lowE = 0, allE = 0; float lpState = 0; const float k = 1.0f - std::exp(-2.0f * 3.14159f * 200.f / kSr);
  for (size_t i = ms(50); i < out.size(); i++) { lpState += k * (out[i] - lpState); lowE += (double)lpState * lpState; allE += (double)out[i] * out[i]; }
  CHECK(lowE < allE * 0.05f);
  CHECK(finiteAll(out)); CHECK(peakOf(out) < 1.5f);
}

TEST(legato_note_re_reads_the_patch) {
  // Finding 8: after a patch change ('gen sound') a legato note must not be a hybrid of old and new sounds.
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.glide = 0.05f; p.polyphony = 1; p.cutoff = 8000.f; p.level = 0.5f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 100);
  std::vector<float> out; renderInto(v, out, ms(300));
  float oldPeak = peakOf(out, ms(200), ms(300)), oldBright = brightness(out, ms(200), ms(300));
  p.level = 0.1f; p.cutoff = 300.f;                                         // new sound: much quieter and darker
  v.noteOn(60, 100, true);
  size_t t0 = out.size(); renderInto(v, out, ms(400));
  float newPeak = peakOf(out, t0 + ms(200), t0 + ms(400)), newBright = brightness(out, t0 + ms(200), t0 + ms(400));
  std::printf("  legato after patch change: peak %.3f -> %.3f, brightness %.3f -> %.3f\n", oldPeak, newPeak, oldBright, newBright);
  CHECK(newPeak < oldPeak * 0.4f);                                          // level 0.5 -> 0.1 applied (filter also darkens)
  CHECK(newBright < oldBright * 0.5f);                                      // cutoff applied
  CHECK(!v.releasing()); CHECK(v.active());
  // A legato note with a changed sustain reaches the new sustain without a step.
  SynthPatch q = sinePatch(); q.polyphony = 1; q.sustain = 1.0f; q.decay = 0.05f;
  SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(48, 127); std::vector<float> o2; renderInto(w, o2, ms(200));
  float natural = maxStep(o2, ms(100), ms(200));
  q.sustain = 0.3f; w.noteOn(48, 127, true); size_t s0 = o2.size(); renderInto(w, o2, ms(300));
  CHECK(maxStep(o2, s0 - 1, s0 + ms(10)) < natural + 0.02f);
  CHECK_NEAR(peakOf(o2, s0 + ms(200), s0 + ms(300)), 0.5f * 0.3f, 0.02f);
  q.sustain = 1.0f; w.noteOn(48, 127, true); size_t s1 = o2.size(); renderInto(w, o2, ms(300));
  CHECK(maxStep(o2, s1 - 1, s1 + ms(10)) < natural + 0.03f);
  CHECK_NEAR(peakOf(o2, s1 + ms(200), s1 + ms(300)), 0.5f, 0.02f);
}

TEST(filter_envelope_is_stepped_per_block_but_matches_a_per_sample_reference) {
  // Finding 6: the filter AD advances once per 32-sample block. Its level must still follow AdEnv::process.
  // 150 Hz base, +5 octaves: 100 ms linear attack to 4800 Hz, then 60 dB of decay over 1 s (env.cpp).
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.cutoff = 150.f; p.filterEnv = 5.0f; p.fAttack = 0.1f; p.fDecay = 1.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(48, 100);
  std::vector<float> out; renderInto(v, out, kSr * 2);
  float b0 = brightness(out, ms(5), ms(25)), b1 = brightness(out, ms(95), ms(135)), b2 = brightness(out, ms(300), ms(400)), b3 = brightness(out, ms(1500), ms(2000));
  std::printf("  filter AD brightness: attack %.3f peak %.3f mid %.3f tail %.3f\n", b0, b1, b2, b3);
  CHECK(b1 > b0 * 1.5f);                                                    // still opening during the attack
  CHECK(b1 > b2 * 1.3f && b2 > b3 * 1.1f);                                  // then decays towards the base cutoff
  // The tail equals the base cutoff: same brightness as a voice without filter envelope.
  SynthPatch q = p; q.filterEnv = 0.f; SynthVoice w; w.init(); w.setPatch(&q); w.noteOn(48, 100);
  std::vector<float> o2; renderInto(w, o2, kSr * 2);
  CHECK_NEAR(b3 / brightness(o2, ms(1500), ms(2000)), 1.0f, 0.03f);
  // A 2 s hold does not produce denormals or NaN (AdEnv floor mirrored per block)
  CHECK(finiteAll(out));
}


// ---------------------------------------------------------------- wavetable sets on glides and switches (SPEC §9.15)

// A saw through a transparent filter (HP at 20 Hz, no resonance), instant envelope, level 0.5.
static SynthPatch flatSawPatch() {
  SynthPatch p = sinePatch(); p.wave1 = WSaw; p.wave2 = WSaw; p.filterMode = 2; p.cutoff = 20.f; p.sustain = 1.0f; return p;
}
// Energy of x[from, to) between the harmonic bands of a down-chirp from f0a to f0b (bands [k f0b - 120, k f0a + 120]
// for k <= H), relative to the total, in dB: the inharmonic (folded) share of a slowly gliding saw.
static double inharmonicDb(const std::vector<float>& x, size_t from, size_t to, double f0a, double f0b, int H) {
  double harm = 0, total = 0;
  for (double f = 2; f < 11025; f += 2) {
    double l = toneLevel(std::vector<float>(x.begin() + from, x.begin() + to), f); l *= l; total += l;
    bool in = false; for (int k = 1; k <= H; k++) if (f >= k * f0b - 120 && f <= k * f0a + 120) in = true;
    if (in) harm += l;
  }
  return 10 * std::log10((total - harm) / total);
}
// The voice's glide law (header contract: one-pole in semitones, 99 % in `glide` seconds, stepped per 32-sample
// block, snapping within 0.01 st): the pitch of block b and the phase (cycles) of every sample, from midiToHz.
struct GlideLaw {
  std::vector<float> st; std::vector<double> phase; std::vector<int> set;
  GlideLaw(float from, float to, float glideS, int blocks) {
    const float coef = 1.0f - expf(-4.6051702f * 32.0f / (glideS * kSr));
    float s = from; uint32_t acc = 0;
    for (int b = 0; b < blocks; b++) {
      s += (to - s) * coef; if (std::fabs(to - s) < 0.01f) s = to;
      st.push_back(s); set.push_back(waveSetForPitch((int)(s + 0.5f)));
      const uint32_t inc = phaseIncForHz(midiToHz(s));
      for (int i = 0; i < 32; i++) { phase.push_back(acc / 4294967296.0); acc += inc; }
    }
  }
  bool switchIn(int b0, int b1) const { for (int b = b0 < 1 ? 1 : b0; b < b1; b++) if (set[b] != set[b - 1]) return true; return b0 == 0; }
};
// A bare oscillator on one fixed table set driven by a glide law (what the voice did before SPEC §9.15 when it
// kept the set of the note's target pitch: a downward slide from 96 to 48 ran set 0 the whole way).
static std::vector<float> oscOnSet(const GlideLaw& law, int set) {
  std::vector<float> out; uint32_t ph = 0; const int16_t* t = waveTableSet(WSaw, set); const int sh = kWaveSetShift[set];
  for (size_t b = 0; b < law.st.size(); b++) { const uint32_t inc = phaseIncForHz(midiToHz(law.st[b])); for (int i = 0; i < 32; i++) { out.push_back(waveSampleN(t, ph, sh) / 29000.0f * 0.5f); ph += inc; } }
  return out;
}
// Solves G a = r (symmetric, n x n) by Gaussian elimination with pivoting.
static std::vector<double> solveNormal(std::vector<double> G, std::vector<double> r, int n) {
  for (int c = 0; c < n; c++) {
    int piv = c; for (int k = c + 1; k < n; k++) if (std::fabs(G[k * n + c]) > std::fabs(G[piv * n + c])) piv = k;
    if (piv != c) { for (int k = 0; k < n; k++) std::swap(G[c * n + k], G[piv * n + k]); std::swap(r[c], r[piv]); }
    const double d = G[c * n + c]; if (std::fabs(d) < 1e-18) continue;
    for (int k = c + 1; k < n; k++) { const double f = G[k * n + c] / d; if (f == 0) continue; for (int j = c; j < n; j++) G[k * n + j] -= f * G[c * n + j]; r[k] -= f * r[c]; }
  }
  std::vector<double> a(n, 0.0);
  for (int c = n - 1; c >= 0; c--) { double s = r[c]; for (int j = c + 1; j < n; j++) s -= G[c * n + j] * a[j]; a[c] = std::fabs(G[c * n + c]) < 1e-18 ? 0 : s / G[c * n + c]; }
  return a;
}
// Folded energy of a gliding saw: the share of x[from, to) NOT explained by a least-squares fit of the harmonics
// 1..K of the known phase law (any amplitude and phase each), in dB re the total. A partial above Nyquist folds
// to a chirp that is not one of these harmonics, so it lands in the residual; K is the count of partials below
// Nyquist at the window's highest pitch.
static double foldedResidualDb(const std::vector<float>& x, const std::vector<double>& phase, size_t from, size_t to, int K) {
  const int n = 2 * K; const size_t len = to - from;
  std::vector<double> B(len * n), G(n * n, 0.0), r(n, 0.0); double total = 0;
  for (size_t i = 0; i < len; i++) { const double p = 2.0 * M_PI * phase[from + i]; for (int k = 1; k <= K; k++) { B[i * n + 2 * (k - 1)] = std::cos(k * p); B[i * n + 2 * (k - 1) + 1] = std::sin(k * p); } }
  for (size_t i = 0; i < len; i++) { const double xi = x[from + i]; total += xi * xi; for (int a = 0; a < n; a++) { r[a] += B[i * n + a] * xi; for (int b = a; b < n; b++) G[a * n + b] += B[i * n + a] * B[i * n + b]; } }
  for (int a = 0; a < n; a++) for (int b = 0; b < a; b++) G[a * n + b] = G[b * n + a];
  const std::vector<double> coef = solveNormal(G, r, n);
  double explained = 0; for (int a = 0; a < n; a++) explained += coef[a] * r[a];
  double res = total - explained; if (res < 1e-30 * total) res = 1e-30 * total;
  return 10 * std::log10(res / total);
}

TEST(slow_slide_down_switches_table_sets_on_the_way) {
  // Finding 1 (SPEC §9.15): a downward slide starts at the HIGH pitch, where the rich table of the low target
  // folds. The voice re-selects every oscillator's set per modulation block from the glided pitch, so a 3 s slide
  // from 96 to 48 on a mono saw runs set 2 near MIDI 90, set 1 near 72 and set 0 at 60: in a 40 ms window at
  // each of those pitches the energy between the harmonic bands (the folded partials) stays at the floor, while
  // a bare set-0 oscillator on the same pitch law folds -11 .. -19 dB of its energy at the first two.
  SynthPatch p = flatSawPatch(); p.glide = 3.0f; p.polyphony = 1;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(96, 100); std::vector<float> out; renderInto(v, out, ms(200));
  v.noteOn(48, 100); const size_t t0 = out.size(); renderInto(v, out, ms(1000));
  CHECK(finiteAll(out));
  const int win = ms(40) / 32;
  const GlideLaw law(96.f, 48.f, 3.0f, ms(1000) / 32 + 1);
  const double expectWorst[3] = {-25.0, -25.0, -200.0};
  int i = 0;
  for (float at : {0.08f, 0.45f, 0.90f}) {
    const int b0 = ms(at * 1000) / 32, b1 = b0 + win;
    const int set = law.set[b0], H = kWaveSetHarmonics[set];
    CHECK(!law.switchIn(b0, b1));                                            // the window sits inside one set
    const double f0a = midiToHz(law.st[b0]), f0b = midiToHz(law.st[b1 - 1]);
    const double voiceDb = inharmonicDb(out, t0 + b0 * 32, t0 + b1 * 32, f0a, f0b, H);
    const GlideLaw sub(law.st[b0], 48.f, 3.0f, win + 1);
    std::vector<float> ref = oscOnSet(sub, 0);
    const double refDb = inharmonicDb(ref, 0, (size_t)win * 32, f0a, f0b, H);
    std::printf("  slide at %.2f s: pitch %.1f..%.1f on set %d: inharmonic energy %.1f dB (bare set-0 oscillator %.1f dB)\n",
                at, law.st[b0], law.st[b1 - 1], set, voiceDb, refDb);
    CHECK_EQ(set, 2 - i);
    CHECK(voiceDb <= -40.0);
    if (i < 2) CHECK(refDb >= expectWorst[i]);                               // the detector sees the old behaviour
    i++;
  }
  CHECK_NEAR(zcHz(out, t0 + ms(950), t0 + ms(1000)), midiToHz(48 + 48 * expf(-4.6051702f * 0.975f / 3.0f)), 8.0f);   // still gliding, on its law
}

TEST(fast_slide_down_folds_nothing_in_the_first_40_ms) {
  // Finding 1, the preset case: glide 0.12 s (Fx / perform presets), 96 -> 48 on a mono saw. Before SPEC §9.15 the
  // voice spent the first 36 ms above MIDI 60 on the 40-partial table (-9.6 dB of folded energy while passing
  // MIDI 90). Now every modulation block runs the set of its own pitch: in each 4-block window without a set
  // switch (a switch crossfades partials in, which the fixed-amplitude fit below cannot model) the energy not
  // explained by the in-band harmonics of the glide law stays below -35 dB, where the same analysis of a bare
  // set-0 oscillator on the same law reads -13 .. -26 dB. The upward slide 48 -> 96 passes the same bound.
  for (int dir = 0; dir < 2; dir++) {
    const uint8_t from = dir ? 48 : 96, to = dir ? 96 : 48;
    SynthPatch p = flatSawPatch(); p.glide = 0.12f; p.polyphony = 1;
    SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(from, 100); std::vector<float> out; renderInto(v, out, ms(200));
    v.noteOn(to, 100); const size_t t0 = out.size(); renderInto(v, out, ms(50));
    CHECK(finiteAll(out));
    const int blocks = ms(40) / 32;
    const GlideLaw law((float)from, (float)to, 0.12f, blocks + 1);
    std::vector<double> phase(t0, 0.0); phase.insert(phase.end(), law.phase.begin(), law.phase.end());
    std::vector<float> ref = oscOnSet(law, 0);
    double worst = -200, worstRef = 0; int windows = 0;
    for (int b0 = 0; b0 + 4 <= blocks; b0 += 4) {
      if (law.switchIn(b0, b0 + 4)) continue;
      const float stMax = std::fmax(law.st[b0], law.st[b0 + 3]);
      int K = (int)(11025.0 / midiToHz(stMax)); if (K > 40) K = 40;
      const double voiceDb = foldedResidualDb(out, phase, t0 + b0 * 32, t0 + (b0 + 4) * 32, K);
      const double refDb = foldedResidualDb(ref, law.phase, (size_t)b0 * 32, (size_t)(b0 + 4) * 32, K);
      std::printf("  %s slide, blocks %2d..%2d (MIDI %5.1f..%5.1f, set %d, %2d partials in band): folded %.1f dB (set-0 oscillator %.1f dB)\n",
                  dir ? "up" : "down", b0, b0 + 3, law.st[b0], law.st[b0 + 3], law.set[b0], K, voiceDb, refDb);
      if (voiceDb > worst) worst = voiceDb; if (refDb < worstRef) worstRef = refDb; windows++;
    }
    CHECK(windows >= 3);
    CHECK(worst <= -35.0);
    if (!dir) CHECK(worstRef >= -30.0);                                     // the single-set behaviour this replaced
    renderInto(v, out, ms(400));                                            // 99 % of the slide is done after 120 ms
    CHECK_NEAR(zcHz(out, out.size() - ms(50), out.size()), midiToHz(to), midiToHz(to) * 0.01f);
  }
}

TEST(table_switch_on_a_sounding_note_crossfades_instead_of_stepping) {
  // Finding 4 (SPEC §9.15): a legato or stolen note keeps its phase, so swapping its table at once steps by the
  // difference of the two band-limits (up to 0.44 of the peak for saw set 0 -> 1). The set is selected from the
  // pitch plus the LFO's excursion, which gives a switch with NOTHING else changing: patch B is patch A with a
  // 1 st pitch LFO at 0.001 Hz (no audible modulation in 100 ms), so a legato re-trigger of C4 with B moves the
  // saw from set 0 to set 1 at the same frequency and phase. The difference against the same run re-triggered
  // with A is exactly the partials 17..40 being removed, ramped over the 32-sample block: over one period of
  // switch phases its RMS in the first 8 samples stays below its steady RMS (worst 0.46, first 4 samples 0.18)
  // and the output gains no step at the switch sample; with the crossfade disabled the same readings are 3.0,
  // 3.7 and a 0.023 extra step (measured on the host).
  SynthPatch a = flatSawPatch(); a.polyphony = 1; a.glide = 0.f;
  SynthPatch b = a; b.lfoToPitch = 1.0f; b.lfoRate = 0.001f;
  double worst8 = 0, worst4 = 0, extraStep = 0, steadyMin = 1e9;
  for (int j = 0; j < 28; j++) {                                            // 28 x 3 samples: one C4 period of phases
    const int lead = ms(100) + j * 3;
    SynthVoice v1; v1.init(); v1.setPatch(&a); v1.noteOn(60, 100); std::vector<float> y1; renderInto(v1, y1, lead);
    SynthVoice v2; v2.init(); v2.setPatch(&a); v2.noteOn(60, 100); std::vector<float> y2; renderInto(v2, y2, lead);
    v1.setPatch(&b); v1.noteOn(60, 100, true); v2.noteOn(60, 100, true);
    renderInto(v1, y1, ms(40)); renderInto(v2, y2, ms(40));
    std::vector<float> d(y1.size()); for (size_t i = 0; i < d.size(); i++) d[i] = y1[i] - y2[i];
    bool sameBefore = true; for (int i = 0; i < lead; i++) if (d[i] != 0.f) sameBefore = false;
    CHECK(sameBefore);                                                      // identical until the switch
    double e8 = 0, e4 = 0, steady = 0;
    for (int i = 0; i < 8; i++) e8 += (double)d[lead + i] * d[lead + i];
    for (int i = 0; i < 4; i++) e4 += (double)d[lead + i] * d[lead + i];
    for (size_t i = lead + ms(5); i < d.size(); i++) steady += (double)d[i] * d[i];
    steady /= (double)(d.size() - lead - ms(5));
    worst8 = std::fmax(worst8, std::sqrt(e8 / 8 / steady)); worst4 = std::fmax(worst4, std::sqrt(e4 / 4 / steady));
    steadyMin = std::fmin(steadyMin, steady);
    extraStep = std::fmax(extraStep, std::fabs(y1[lead] - y1[lead - 1]) - std::fabs(y2[lead] - y2[lead - 1]));
  }
  std::printf("  set 0 -> 1 on a sounding C4 saw: removed partials' RMS in the first 8 / 4 samples %.2f / %.2f of steady (instant switch: 3.0 / 3.7), extra output step %.4f\n",
              worst8, worst4, extraStep);
  CHECK(steadyMin > 1e-6);                                                  // the switch really removed partials 17..40
  CHECK(worst8 < 1.0); CHECK(worst4 < 0.5);
  CHECK(extraStep < 0.005);
  // In the engine's own cases nothing steps either: a stolen C4 chord voice re-triggered at MIDI 61 (set 0 -> 1)
  // through an 1800 Hz LP shows the same largest per-sample step as the control 59 -> 60 (no switch), and a
  // legato slide of the mono saw through every boundary (48 -> 100) stays finite and lands on pitch.
  float stepCross = 0, stepCtrl = 0;
  for (int cross = 0; cross < 2; cross++) for (int j = 0; j < 8; j++) {
    SynthPatch c = sinePatch(); c.wave1 = WSaw; c.cutoff = 1800.f; c.sustain = 1.0f;
    SynthVoice v; v.init(); v.setPatch(&c); v.noteOn(cross ? 60 : 59, 100); std::vector<float> o; renderInto(v, o, ms(100) + j * 7);
    v.noteOn(cross ? 61 : 60, 100); const size_t t0 = o.size(); renderInto(v, o, ms(10));
    (cross ? stepCross : stepCtrl) = std::fmax(cross ? stepCross : stepCtrl, maxStep(o, t0 - 1, t0 + ms(3)));
  }
  std::printf("  stolen chord voice through 1800 Hz LP: largest step 60 -> 61 %.4f, control 59 -> 60 %.4f\n", stepCross, stepCtrl);
  CHECK(stepCross < stepCtrl * 1.1f + 0.002f);
  SynthPatch m = flatSawPatch(); m.polyphony = 1; m.glide = 0.06f;
  SynthVoice w; w.init(); w.setPatch(&m); w.noteOn(48, 100); std::vector<float> o; renderInto(w, o, ms(100));
  w.noteOn(100, 100, true); const size_t t1 = o.size(); renderInto(w, o, ms(300));
  CHECK(finiteAll(o)); CHECK(peakOf(o) < 1.0f);
  CHECK_NEAR(zcHz(o, t1 + ms(250), t1 + ms(300)), midiToHz(100), midiToHz(100) * 0.01f);
}

TEST(vibrato_depth_counts_towards_the_table_set) {
  // A pitch LFO of 1 st on C4 reaches MIDI 61, where set 0's 40th partial would fold (11.09 kHz -> 10.96 kHz):
  // the set is chosen for the excursion's top, so the band above 9.5 kHz (partials 36..40 of C4, absent from
  // set 1) stays empty, and the set never flips with the vibrato (no crossfades: the output is a clean FM saw).
  SynthPatch p = flatSawPatch(); p.lfoToPitch = 1.0f; p.lfoRate = 6.0f;
  SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(60, 100); std::vector<float> out; renderInto(v, out, kSr);
  const std::vector<float> win(out.begin() + ms(100), out.end());
  double hi = 0, fund = 0;
  for (double f = 9500; f < 11000; f += 10) { double l = toneLevel(win, f); hi += l * l; }
  for (double f = 240; f < 290; f += 2) { double l = toneLevel(win, f); fund += l * l; }
  std::printf("  C4 saw with 1 st vibrato: 9.5-11 kHz band %.1f dB re the fundamental's band\n", 10 * std::log10(hi / fund));
  CHECK(10 * std::log10(hi / fund) < -50.0);
  SynthPatch q = p; q.lfoToPitch = 0.f;                                      // without vibrato C4 keeps set 0: partial 38 at 9.9 kHz
  SynthVoice u; u.init(); u.setPatch(&q); u.noteOn(60, 100); std::vector<float> o2; renderInto(u, o2, kSr);
  CHECK_NEAR(toneLevel(o2, 38 * midiToHz(60)) / toneLevel(o2, midiToHz(60)), 1.0 / 38, 0.01);
}


int main() { return lp_test::run(); }
