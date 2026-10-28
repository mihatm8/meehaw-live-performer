// meehaw live performer — headless selftest helpers of the desktop build (SPEC §7): the scripted checks against
// the engine's published state, the pass / fail verdict over a run's counters, and the argument / path helpers of
// main.cpp. Pure functions over value types, no SDL (tests/run_link_all.sh compiles every header standalone):
// tests/test_desktop.cpp exercises them directly, main.cpp drives them from the script.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../liveplayer/src/core/config.h"
#include "../liveplayer/src/core/types.h"

namespace lp {

// One scripted assertion: what the engine must publish for a track after a key action. Variants land on the bar
// line (SPEC §9.5), snare rush on the next 1/16 and kick cut at once (§9.6), steps and ramps in params() right
// away (§9.12); the script waits up to a timeout for each (main.cpp OpCheck).
enum CheckId : uint8_t { CkPlaying = 0, CkVariant, CkRush, CkKickCut, CkFilterAbove, CkFilterZero, CkVolume, CheckIdCount };

inline const char* checkName(int id) {
  static const char* n[CheckIdCount] = {"playing", "variant", "rush", "kickcut", "filter>", "filter==0", "volume"};
  return id >= 0 && id < CheckIdCount ? n[id] : "?";
}

// The published value a check compares: state / variant / rush / cut from the status snapshot, filter and volume
// from the track's target params (what the UI reads back, ui.cpp paramValue).
inline float checkValue(const EngineStatus& st, const TrackParams& p, CheckId id, int track) {
  const TrackStatus& ts = st.track[track < 0 ? 0 : track >= kTracks ? kTracks - 1 : track];
  switch (id) {
    case CkPlaying: return ts.state == PSPlaying ? 1.0f : 0.0f;
    case CkVariant: return (float)ts.variant;
    case CkRush: return (float)ts.rush;
    case CkKickCut: return (float)ts.kickCut;
    case CkFilterAbove: case CkFilterZero: return p.filter;
    case CkVolume: return p.volume;
    default: return 0.0f;
  }
}

// CkFilterAbove: got > expect. CkFilterZero: |got| <= expect. CkVolume: within a fifth of the 0.05 step (CmdSetParam
// values sit on the step grid, ui.cpp stepParam, so one repeat more or less is a failure). The rest: exact.
constexpr float kVolumeTol = 0.011f;
inline bool checkHolds(CheckId id, float got, float expect) {
  switch (id) {
    case CkFilterAbove: return got > expect;
    case CkFilterZero: return fabsf(got) <= expect;
    case CkVolume: return fabsf(got - expect) <= kVolumeTol;
    default: return got == expect;
  }
}

// Counters of one headless run (main.cpp fills them from the pacer, the script, the engine and the gfx).
struct SelftestTally {
  uint32_t blocks = 0, underruns = 0, late = 0, soundBlocks = 0;
  uint32_t peak = 0;                                        // max |sample|, int16 units
  uint32_t vocalUnderruns = 0, litPixels = 0, checkFails = 0;
  bool genTimeout = false, playTimeout = false, scriptFinished = false;
  char firstCheckFail[64] = "";
};
constexpr uint32_t kLateBudget = 2;                         // late pacer turns tolerated per run, see selftestVerdict
constexpr float kSoundPeak = 0.05f;                         // the run "produced sound" above this peak (−26 dBFS)
constexpr uint32_t kMinSoundBlocks = kSampleRate / kBlock / 2;   // ... for at least 0.5 s

// nullptr = PASS, else the first failure in order of severity. A late turn (the pacer woke more than 4 blocks,
// 46 ms, after its slot) is what empties the sketch's 4-block speaker queue, so on the device it is an audible
// drop-out; the host scheduler can oversleep that far a couple of times per run under load, hence a small budget
// instead of zero. Render underruns (render() slower than real time) are never tolerated.
inline const char* selftestVerdict(const SelftestTally& t) {
  if (t.blocks == 0) return "no blocks rendered";
  if (t.underruns) return "render underruns";
  if (t.late > kLateBudget) return "late render turns (pacer stalled > 4 blocks)";
  if (t.genTimeout) return "generation did not finish";
  if (t.playTimeout) return "tracks did not start";
  if (t.checkFails) return t.firstCheckFail[0] ? t.firstCheckFail : "scripted check failed";
  if (t.peak < (uint32_t)(kSoundPeak * 32767)) return "no sound";
  if (t.soundBlocks < kMinSoundBlocks) return "sound shorter than 0.5 s";
  if (t.vocalUnderruns) return "vocal ring underruns";
  if (t.litPixels == 0) return "UI drew nothing";
  if (!t.scriptFinished) return "script did not finish (run longer)";
  return nullptr;
}

// --selftest N / --seconds N: a decimal integer 1..86400. "0", "-3", "abc", "12x", "" and a missing value are
// rejected, so a headless run (which has no window or Escape to stop it) can never be endless.
inline bool parseSeconds(const char* s, uint32_t& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  unsigned long v = strtoul(s, &end, 10);
  if (end == s || *end || v < 1 || v > 86400) return false;
  out = (uint32_t)v;
  return true;
}

// Where the selftest saves its last frame: --frame PATH when given, else selftest_frame.bmp next to the binary
// (base = SDL_GetBasePath(), which ends in a separator), else in the working directory.
inline void framePath(const char* base, const char* override, char* out, int cap) {
  if (override && override[0]) { snprintf(out, cap, "%s", override); return; }
  snprintf(out, cap, "%s%s", base ? base : "", "selftest_frame.bmp");
}

} // namespace lp
