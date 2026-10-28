// Host-side preset loudness QA, shared by tools/presets_qa.cpp (the measurement pass of tools/gen_presets.py --qa,
// which bakes the per-preset level trims into dsp/presets_gen.hpp) and tests/test_synth.cpp (the spread check:
// every generated preset within +-1.5 dB of its family's target). Header-only, host only (std::vector, double).
//
// One fixed phrase per category at 120 BPM (the host fixtures' tempo; beat 0.5 s, bar 2 s), velocity 100,
// rendered through SynthVoice exactly as the engine would drive it (noteOn / noteOff per note, the voice's own
// glide / steal rules, polyphony-limited voice pool), summed to one mono signal of kPhraseS seconds. Velocity 100 is
// the SPEC §9.22 accent threshold, so a '303' row (accent 0.5-0.8) is measured accented, like its family's hand rows
// that set its target (library rows carry accent 0 and are unaffected); the voice renders the patch's own sat / accent
// (no knobs are set), exactly what CmdSetPatch hands a track. The phrases:
//   Lead, Fx   16 eighths over two bars, A3..B4 riff, gate 80 %           (1 voice for a mono patch, else 2)
//   Bass       the same riff one octave down                              (1 voice for a mono patch, else 2)
//   Chord      4 half-note triads (Am F C G around C4), gate 90 %        (3 voices)
//   Pad        2 whole-note triads, gate 95 %                             (3 voices)
// Loudness = the loudest 50 ms window's RMS (hop 12.5 ms) of that sum, in dBFS. Chosen over the whole-phrase RMS
// because the hand presets of one category (the reference the generator trims to) agree far better under it
// (hand library spread, 2026-10-16: Chord 9 dB vs 19 dB phrase RMS, Lead 5.8 vs 11, Bass 8 vs 11): the hand
// designer balanced plucks and sustained sounds at roughly equal short-term level, not equal energy (a 0.3 s
// pluck would need +16 dB to match an organ's phrase RMS). tools/presets_qa.cpp prints the alternatives. The two
// peaks guard the existing synth tests: peakC4 (one voice, C4, vel 100, 0.5 s + release) and peakExtreme (one
// voice, MIDI 24 / 108, vel 127) must stay below 1.5; the generator caps a trim that would cross 1.45.
#pragma once
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include "dsp/synth_voice.h"
#include "dsp/presets.h"

namespace lp_qa {

// The hand-written rows every family starts with (SPEC §9.17b, gen_presets.py HAND_*): rows [0, kHandLibrary[c])
// of the library family and [libraryPresetCount(c), + kHandAcid(c)) of the '303' family. The generated rows follow
// each hand block; the hand rows are the loudness reference and are never trimmed.
constexpr int kHandLibrary[5] = { 10, 9, 9, 9, 9 };                     // Chord, Lead, Pad, Bass, Fx
inline int handLibrary(lp::Category c) { return (int)c < 5 ? kHandLibrary[(int)c] : 0; }
inline int handAcid(lp::Category c) { return (c == lp::Chord || c == lp::Lead || c == lp::Pad || c == lp::Bass) ? 8 : 0; }

constexpr int kBpm = 120;
constexpr float kBeatS = 60.0f / (float)kBpm;                          // 0.5 s
constexpr float kPhraseS = 2 * 4 * kBeatS + 1.0f;                       // two bars + 1 s tail = 5 s
constexpr float kWindowS = 0.050f, kHopS = 0.0125f;
constexpr float kRefLevel = 1.0f;                                       // levels are linear gains: trims scale exactly

struct NoteEv { float startS, lenS; uint8_t pitch; };

// The phrase of a category (notes sorted by start). Chord notes share a start.
inline std::vector<NoteEv> phraseFor(lp::Category c) {
  std::vector<NoteEv> ev;
  const float e = kBeatS * 0.5f;                                          // an eighth
  if (c == lp::Chord || c == lp::Pad) {
    static const uint8_t tri[4][3] = { {57, 60, 64}, {53, 57, 60}, {60, 64, 67}, {55, 59, 62} };   // Am F C G
    if (c == lp::Chord) { for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) ev.push_back({ i * 2 * kBeatS, 2 * kBeatS * 0.9f, tri[i][k] }); }
    else { for (int i = 0; i < 2; i++) for (int k = 0; k < 3; k++) ev.push_back({ i * 4 * kBeatS, 4 * kBeatS * 0.95f, tri[i * 2][k] }); }
    return ev;
  }
  static const uint8_t riff[16] = { 57, 60, 64, 69, 67, 64, 60, 57, 59, 62, 66, 69, 71, 69, 66, 62 };
  const int tr = c == lp::Bass ? -12 : 0;
  for (int i = 0; i < 16; i++) ev.push_back({ i * e, e * 0.8f, (uint8_t)(riff[i] + tr) });
  return ev;
}
inline int voicesFor(lp::Category c, const lp::SynthPatch& p) {
  int poly = p.polyphony < 1 ? 1 : p.polyphony;
  if (c == lp::Chord || c == lp::Pad) return poly < 3 ? poly : 3;
  return poly < 2 ? poly : 2;
}

// Renders the category's phrase with this patch into a mono sum of kPhraseS seconds.
inline std::vector<float> renderPhrase(lp::Category c, const lp::SynthPatch& p) {
  const int sr = lp::kSampleRate, total = (int)(kPhraseS * sr);
  std::vector<NoteEv> ev = phraseFor(c);
  const int nv = voicesFor(c, p);
  std::vector<lp::SynthVoice> v(nv);
  for (auto& x : v) { x.init(); x.setPatch(&p); }
  struct Held { int voice; int offAt; };
  std::vector<Held> held;
  std::vector<float> out((size_t)total, 0.0f);
  size_t next = 0; int rr = 0;
  for (int pos = 0; pos < total; pos += 32) {
    const int n = total - pos < 32 ? total - pos : 32;
    for (size_t i = 0; i < held.size();) { if (held[i].offAt <= pos) { v[held[i].voice].noteOff(); held.erase(held.begin() + i); } else i++; }
    while (next < ev.size() && (int)(ev[next].startS * sr) <= pos) {
      const int vi = rr++ % nv;
      for (size_t i = 0; i < held.size();) { if (held[i].voice == vi) held.erase(held.begin() + i); else i++; }   // a stolen voice drops its pending off
      v[vi].noteOn(ev[next].pitch, 100);
      held.push_back({ vi, pos + (int)(ev[next].lenS * sr) });
      next++;
    }
    for (auto& x : v) x.render(&out[(size_t)pos], n);
  }
  return out;
}

struct Measure {
  float loudDb = -120;      // the trim metric: loudest 50 ms window RMS, dBFS
  float rmsDb = -120;       // whole-phrase RMS, dBFS (reported only)
  float peak = 0;           // phrase peak
  float peakC4 = 0;         // one voice, C4 vel 100, 0.5 s + release (tests: < 1.5)
  float peakExtreme = 0;    // one voice, MIDI 24 and 108 at vel 127, 0.25 s + 0.5 s (tests: < 1.5)
  bool finite = true;
};

inline float dbOf(double amp) { return amp > 1e-9 ? (float)(20.0 * std::log10(amp)) : -120.0f; }

inline float maxWindowRmsDb(const std::vector<float>& x, float windowS, float hopS) {
  const int w = (int)(windowS * lp::kSampleRate), hop = (int)(hopS * lp::kSampleRate);
  if ((int)x.size() < w) return -120.0f;
  double best = 0;
  for (int s = 0; s + w <= (int)x.size(); s += hop) {
    double e = 0; for (int i = s; i < s + w; i++) e += (double)x[(size_t)i] * x[(size_t)i];
    if (e > best) best = e;
  }
  return dbOf(std::sqrt(best / (double)w));
}

inline float singleVoicePeak(const lp::SynthPatch& p, uint8_t pitch, uint8_t vel, float holdS, float tailS) {
  lp::SynthVoice v; v.init(); v.setPatch(&p); v.noteOn(pitch, vel);
  float pk = 0; float buf[lp::kBlock];
  int hold = (int)(holdS * lp::kSampleRate), tail = (int)(tailS * lp::kSampleRate);
  for (int t = 0; t < hold + tail; t += lp::kBlock) {
    if (t >= hold && t - lp::kBlock < hold) v.noteOff();
    std::memset(buf, 0, sizeof buf);
    bool alive = v.render(buf, lp::kBlock);
    for (float s : buf) { float a = std::fabs(s); if (a > pk) pk = a; }
    if (!alive && t >= hold) break;
  }
  return pk;
}

inline Measure measure(lp::Category c, const lp::SynthPatch& p) {
  Measure m;
  std::vector<float> x = renderPhrase(c, p);
  double e = 0; for (float s : x) { if (!std::isfinite(s)) m.finite = false; e += (double)s * s; float a = std::fabs(s); if (a > m.peak) m.peak = a; }
  m.rmsDb = dbOf(std::sqrt(e / (double)x.size()));
  m.loudDb = maxWindowRmsDb(x, kWindowS, kHopS);
  m.peakC4 = singleVoicePeak(p, 60, 100, 0.5f, p.release * 1.5f + 0.05f);
  m.peakExtreme = std::max(singleVoicePeak(p, 24, 127, 0.25f, 0.5f), singleVoicePeak(p, 108, 127, 0.25f, 0.5f));
  return m;
}

// Family targets: the median loudness of the hand rows of that family (library: rows [0, handLibrary); acid:
// rows [libraryPresetCount, + handAcid)). What gen_presets.py trims the generated rows to.
inline float medianOf(std::vector<float> v) {
  if (v.empty()) return -120.0f;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace lp_qa
