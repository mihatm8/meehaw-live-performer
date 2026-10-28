// meehaw live performer — generator: set / pattern / sound / loop jobs as a resumable state machine (SPEC §6, §9.2,
// §9.5, §9.19) and the stream producer of the vocal and loop rings (SPEC §9.3, §9.19). Runs on the loop task next to
// the UI.
//
// Jobs. A request becomes a job that walks the tracks of a set in Reaper order (Chord, Lead, Pad, Bass, Fx,
// Percussion, Beat, Vocal); every step() performs at most ONE card operation: a library warm-up chunk
// (Library::prepare*, one 4 KB index read or one listing), a library pick (the ordinal lookup), a MIDI file
// (open, parse, close), one 4 KB loader chunk, or a vocal / loop file open. The stream pump runs first in every step
// and its chunk reads are charged to the step: once it read kStepSkipChunks the step does no card work of its
// own (SPEC §9.2 keeps every loop burst under 20 ms). Pure transitions (building a variant, picking a preset,
// bookkeeping) are chained inside the same step so a step never "wastes" a loop iteration. Patterns go to the
// engine's `next` buffers under beginNextWrite/endNextWrite (SPEC §9.5); a set job keeps its eight swaps, the
// Beat's layer mask and the rush loop back and pushes them together with CmdSwapSet so the whole set changes on
// one bar line (SPEC §9.6: layers switch atomically with the pattern), while sounds (presets, slots) replace
// immediately (SPEC §9.1). The Beat's active pattern is always Full; its layers are a pitch mask. Percussion
// keeps the source unfolded and applies the fold map computed at load (SPEC §9.7). Every command push is
// checked: a full queue keeps the state and retries next step, and a set switch is pushed only when the queue
// has room for all of it (never a partial set, SPEC §9.5).
//
// SPEC §9.17b. Every track a job regenerates gets CmdResetTrack (its params back to the category defaults; a set load
// does NOT reset, sets.cpp restores the file's values): a pattern-only job pushes it as the track begins, a job with a
// sound part right before that part (SoundBegin), so the window between the reset and the new sound stays small; a
// track nothing is regenerated on (a kept recorded loop under a pattern job — on ANY track, SPEC §9.19 §3) is not
// reset. A Beat pattern job publishes variant 0 (Kick only) instead of Full. A synth track in acid mode (info.acid,
// Chord / Lead / Pad / Bass) draws its sound from the '303' family at the end of its preset bank and randomises the
// knobs (eight or nine CmdSetParam after CmdSetPatch, SPEC §9.22: CUT RES ENV DEC DRV WAV SAT ACC and the chorus send on
// Lead / Chord) on every sound generation; its pattern job writes an acid line / stabs
// from a fresh seed straight into the track's source (no card), so variants and set files work unchanged (variant 0
// = the line). nextPatch is written only while no CmdSetPatch of that track is in flight (patchFree): the render task
// copies it when it drains the command, so an acid toggle during a sound job (or two quick toggles) waits two blocks
// instead of tearing the patch.
//
// Theory set (SPEC §9.24, SHIFT + load 1 / 2). JobTheory is a set job whose five melodic tracks (Chord, Lead, Pad, Bass,
// Fx) are written from ONE chord progression instead of library picks: startJob draws the whole plan from the Rng
// (theoryPlan: progression, voicing template, rhythm templates, styles, a melodic seed — flash tables in theory_tables.hpp,
// generated from the same tools/theory_data.py as the card's clips) and every melodic track is built in a pure step
// (TheoryPat → theoryBuild into the source, no card) before the Beat / Percussion / Vocal tracks go through the library as
// in GENERATE; the swaps are held for the one CmdSwapSet like every set job. The patterns are labelled "THEORY <name>" and
// marked edited, so set files and sessions write them as SMF assets and a theory set reloads exactly; theorySet(set) is
// true from the job's end until a GENERATE / gen pattern / set load / acid line replaces a melodic track of the set. The
// request queues behind other jobs exactly like generateSet (request order, not the clock, decides when the plan is drawn,
// so a recorded session and its slower offline render draw the same progression).
//
// Loop mode (SPEC §9.19). setLoopMode(track, on) binds the track to a stream — the Vocal track its set's own vocal
// stream (0 / 1), any other track a free loop slot (kSets + k) — and queues JobLoop: Library::prepareLoop (one bounded
// op per step), pickLoop (/lp/packs/<genre>/loops/<folder>/<roots>), one file open into the stream's `nxt`; the pump
// then switches at the next bar and pushes CmdLoopLoaded (a slot) or CmdVocalLoaded (the Vocal) + CmdLoopMode on + the
// carrier CmdSwapPattern{variant, len} that tells the engine the loop's variant and clip length (engine.cpp applySwap:
// on a streaming track the swap touches no pattern). The stream plays the file as a plain loop phase-locked to the bar
// clock: the clip (cycle) is natural × 2^k with natural = the crop of the selected variant (whole, first half, first 2
// beats, first beat) and the cycle beyond the crop is silence (SPEC §9.4); a crop / len change on a streaming loop is
// a same-file switch on the next 1/16 (today's rush semantics); a new pick (gen pattern / gen sound / GENERATE on a
// loop track) switches on the bar and keeps loop mode. Off pushes CmdLoopMode off at once (the engine brings the parked
// clip back at the bar) and ends the slot stream once the engine passed that bar; the Vocal track re-opens its parked
// phrase, which returns with the switch. Nothing of the parked pattern, patch or next buffer is touched meanwhile; what
// the UI sees in info() (variant, lenStep, naturalTicks) describes the loop while it is on and is parked / restored.
// The library picks are remembered by folder (noteSource / noteKitPath): the session writer composes absolute paths
// from them and opens nothing to verify (SPEC §9.19 §3 b). streamsHealthy() is the card gate of SPEC §9.19 §3 a.
//
// Stream producer. The ring maps absolute sample indices to ring positions (baseSample, w, r in engine.h). Every
// sample S is placed by the engine's bar clock (status.barSample / nextBarSample / tick, BarClock in generator.h):
// S lies in global bar b at `into` samples; the clip is a cycle of window + pause bars (SPEC §9.16) and S sits at
// cycle bar (b mod cycleBars), so the cycle phase is position = globalTick mod len like every clip (SPEC §9.4):
// a track started on bar 7 of a 3-bar cycle begins one bar into the window, and a tempo change keeps every window
// end and cycle restart on the engine's bar lines. The phrase plays from its start for the 2-bar window at
// varispeed (file position = (cycleBar × bar + into) × rate; a shorter phrase ends early inside it, a longer one
// is faded out over 64 samples before the bar line that ends the window), then silence for the pause of 0 / 1 /
// 2 / 4 bars, and every cycle starts clean at the file's first sample. The loop variants (first 2 beats / first
// beat) loop that length inside each bar instead, with a 64-sample crossfade at every wrap after the first pass
// and no pause. A streamed loop (mode 1) is the same with cycle = clip and content = the crop; the crossfade needs
// content past the cycle end, so a whole loop file (exactly n bars, docs/LOOPS.md) plays its head straight at every
// repeat, as on the first pass (the tool faded its end). Samples are read with
// linear interpolation through one shared 4 KB window. While a track is idle its ring is pre-rolled for the next bar
// line with that bar's phase; as bars pass unheard the pre-roll slides (one atomic, no reads) when the next bar has
// the same phase (loops: every bar) and is rebuilt otherwise (a pause bar costs no reads), so a queued start never
// starves; while it plays the producer appends from the write frontier. A start that lands inside the armed bar keeps
// the pre-roll's exact bar sample as the content start. A new file (generate, variant, pause, loop pick) is prepared
// as `nxt` and switched at a grid tick: content from `nxt` is written from that tick's sample on (rewriting what was
// already written beyond it) and the switch commands — which carry the new clip length — are pushed so that the
// engine applies them on the bar they belong to; with the transport stopped the switch commands go out at once and
// the engine applies them on the first bar after resume. A tempo change retimes the rings: what was written ahead
// (beyond a few blocks' margin) is redone under the new clock, and the streams carry their file position over at the
// retime point so the material changes speed without skipping (a SHIFT tempo ramp steps the bpm often). The pump
// serves the live streams emptiest ring first (kPumpChunks chunk reads per iteration over all of them): a vocal ring
// refills whenever kRefillSamples are free, a loop ring whenever kLoopChunkSamples are, every ring to the top after a
// refused streamsHealthy() query.
#include "generator.h"
#include "smf.h"
#include "variants.h"
#include "../dsp/presets.h"
#include <string.h>
#include <stdio.h>

namespace lp {
namespace {

constexpr uint8_t kBeatMask[kVariants] = {0x01, 0x0D, 0x0F, 0x1F};             // SPEC §9.6 layers
constexpr uint8_t kSetOrder[kTracksPerSet] = {Chord, Lead, Pad, Bass, Fx, Percussion, Beat, Vocal};
constexpr int kSwitchMarginBlocks = 4;          // the switch tick must be this far ahead so the commands land first
constexpr int kLateBlocks = 8;                  // push the switch commands anyway when the switch is this close
constexpr int kRetimeMarginBlocks = 4;          // after a tempo change the ring is rewritten from this far ahead of the clock (the engine may read closer)
constexpr int kHandshakeBlocks = 2;             // blocks the engine must complete without the ring before a reset
constexpr int kDispatchChain = 6;               // pure transitions chained per step()
constexpr int kPushFailLimit = 256;             // consecutive refused pushes (~3 s of loop steps) before lastError says so
constexpr int kSixteenth = kPPQN / 4;           // 24 ticks: the grid a loop-mode crop / len switch lands on (SPEC §9.19 §1.4 rush semantics)
// Kit roles (SPEC §6): kick keyed to the root, snare else clap, hh, oh else hh; perc A..D from perc + clap + cymbal.
const char* const kRole[kSampleSlots] = {"kick", "snare", "hh", "oh", "perc", "perc", "clap", "cymbal"};
const char* const kFallback[kSampleSlots] = {nullptr, "clap", nullptr, "hh", nullptr, nullptr, "perc", "perc"};
// The role folders a remembered kit path may name (encodeFolder: one byte instead of the text, SPEC §9.19 §3 b).
const char* const kKitRoles[] = {"kick", "snare", "hh", "oh", "perc", "clap", "cymbal"};
constexpr int kKitRoleCount = 7;
constexpr uint8_t kNoFolder = 0xFF;             // encodeFolder: no label / role, no shard
constexpr int kFolderTextLen = 16;              // "<label>/sNN" as text ("A#min/s254" is 10 chars)

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { char l = lower(c); return l >= 'a' && l <= 'z'; }
inline bool isSep(char c) { return c == '_' || c == '-' || c == ' '; }

const char* baseName(const char* path) {
  const char* b = path;
  for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}
void copyStr(char* dst, int cap, const char* src) {
  if (cap <= 0) return;
  int i = 0;
  for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
  dst[i] = 0;
}
uint32_t slotCapSamples(int slot) {
  static const int caps[kSampleSlots] = {kCapKickMs, kCapSnareMs, kCapHhMs, kCapOhMs, kCapPercMs, kCapPercMs, kCapPercMs, kCapPercMs};
  return (uint32_t)caps[slot & 7] * kSampleRate / 1000;
}
uint16_t bpmOr(uint16_t bpm) { return bpm ? bpm : (uint16_t)kDefaultBpm; }
// ticks → samples at bpm, rounded (SPEC §9.3 loopSamples = loopBeats × 60/bpm × 22050).
uint32_t ticksToSamples(uint32_t ticks, uint16_t bpm) {
  const uint64_t den = (uint64_t)bpmOr(bpm) * kPPQN;
  return (uint32_t)(((uint64_t)ticks * 60 * kSampleRate + den / 2) / den);
}
uint16_t clampLen(uint32_t len) {
  if (len < (uint32_t)(kTicksPerBeat / 2)) len = kTicksPerBeat / 2;
  if (len > (uint32_t)kMaxPatternTicks) len = kMaxPatternTicks;
  return (uint16_t)len;
}
// a + (b − a) × frac/65536 in int32 (12-bit fraction keeps the product inside 31 bits).
inline int32_t lerp16(int16_t a, int16_t b, uint32_t frac) { return a + (((int32_t)(b - a) * (int32_t)(frac >> 4)) >> 12); }
int naturalVariant(Category c) { return (c == Beat || c == Percussion) ? 3 : 0; }
uint32_t lenOf(uint32_t natural, int k) { return k >= 0 ? natural << k : natural >> (-k); }
int popcount8(uint8_t m) { int n = 0; for (; m; m = (uint8_t)(m & (m - 1))) n++; return n; }
int32_t maskCmd(int variant) { variant &= 3; return (int32_t)(kBeatMask[variant] | (variant << 8)); }
// The path of a genre pack ("/lp/packs/<genre>/..."): the genre's name length, 0 when the path is not one.
int packGenreLen(const char* path) {
  if (strncmp(path, "/lp/packs/", 10) != 0) return 0;
  const char* e = strchr(path + 10, '/');
  return e ? (int)(e - (path + 10)) : 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------- pure helpers

// Tolerant BPM token scanner (SPEC §9.3): '124bpm', '124_bpm', '124 BPM', 'bpm_124', 'bpm124', 'BPM-90'.
// The number is 60..249, at most three digits and never glued to further digits; letters right after 'bpm'
// disqualify the digits-before form ("124bpmx"). The first valid token wins; 0 means none.
int Generator::parseBpmToken(const char* name) {
  if (!name) return 0;
  const size_t n = strlen(name);
  for (size_t i = 0; i + 3 <= n; i++) {
    if (lower(name[i]) != 'b' || lower(name[i + 1]) != 'p' || lower(name[i + 2]) != 'm') continue;
    size_t j = i;
    while (j > 0 && isSep(name[j - 1])) j--;
    const size_t end = j;
    while (j > 0 && isDigit(name[j - 1])) j--;
    if (end > j && end - j <= 3 && !(i + 3 < n && isAlpha(name[i + 3]))) {
      int v = 0;
      for (size_t k = j; k < end; k++) v = v * 10 + (name[k] - '0');
      if (v >= 60 && v <= 249) return v;
    }
    size_t a = i + 3;
    while (a < n && isSep(name[a])) a++;
    size_t b = a;
    while (b < n && isDigit(name[b])) b++;
    if (b > a && b - a <= 3) {
      int v = 0;
      for (size_t k = a; k < b; k++) v = v * 10 + (name[k] - '0');
      if (v >= 60 && v <= 249) return v;
    }
  }
  return 0;
}

// SPEC §9.3 (amended after listening): the largest power-of-two beats 1..16 that fits INSIDE the phrase, with a
// 2 % tolerance for files cut a hair short of a power of two — REAPER's audio_timing rule. The loop stays
// continuous (the tail of a 6-beat phrase is trimmed at 4 beats) instead of leaving silence until the next clip.
uint8_t Generator::naturalBeats(uint32_t frames, uint32_t sampleRate, float bpm) {
  if (!frames || !sampleRate || !(bpm > 0)) return 1;
  const float beats = (float)frames / (float)sampleRate * bpm / 60.0f * 1.02f;
  uint8_t p = 1;
  while (p < kMaxPatternBeats && beats >= (float)(p * 2)) p = (uint8_t)(p * 2);
  return p;
}

// SPEC §9.16: variants 0 / 1 play the window + pause cycle, 2 / 3 loop the first 2 beats / 1 beat without a pause.
uint16_t Generator::vocalClipTicks(int variant, int pauseBars) { return vocalClipTicks(variant, pauseBars, kVocalWindowBars); }

// SPEC §9.16 amendment: a window of windowBars bars (a '<n>bars' file name token; 0 = the default). The engine clamps
// a vocal clip at kMaxVocalClipTicks (engine.cpp): the pause is kept within that cap by fitPause / stepPause, and the
// cap here is the last defence for a stored pause that no longer fits (the pause gives way, never the window).
uint16_t Generator::vocalClipTicks(int variant, int pauseBars, int windowBars) {
  variant &= 3;
  if (variant == 2) return (uint16_t)(2 * kTicksPerBeat);
  if (variant == 3) return (uint16_t)kTicksPerBeat;
  int bars = clampWindow(windowBars) + clampPause(pauseBars);
  if (bars > kVocalWindowMaxBars) bars = kVocalWindowMaxBars;
  return (uint16_t)(bars * kTicksPerBar);
}

// A window the engine cannot hold (n > kVocalWindowMaxBars, 20 bars at today's kMaxVocalClipTicks) is never cut
// mid-verse: the file plays its first two bars as before the token existed. Raising kMaxVocalClipTicks in config.h
// (e.g. (16 + 4) × kTicksPerBar) lets 8- and 16-bar files play whole; nothing else here depends on the value.
uint8_t Generator::clampWindow(int bars) {
  if (bars <= 0 || bars > kVocalWindowMaxBars) return (uint8_t)kVocalWindowBars;
  return (uint8_t)bars;
}

uint8_t Generator::fitPause(int pause, int windowBars) {
  uint8_t p = clampPause(pause);
  const int w = clampWindow(windowBars);
  while (p > 0 && w + (int)p > kVocalWindowMaxBars) p = stepPause(p, -1);
  return p;
}

// '<n>bars' / '<n>bar' token (SPEC §9.16 amendment, e.g. 125bpm__RAPM__8bars__...): up to three digits right before
// 'bar' (separators '_', '-', ' ' allowed in between), no letter glued after 'bar' / 'bars'. 0 = no token.
int Generator::parseBarsToken(const char* name) {
  if (!name) return 0;
  const size_t n = strlen(name);
  for (size_t i = 0; i + 3 <= n; i++) {
    if (lower(name[i]) != 'b' || lower(name[i + 1]) != 'a' || lower(name[i + 2]) != 'r') continue;
    size_t after = i + 3;
    if (after < n && lower(name[after]) == 's') after++;
    if (after < n && isAlpha(name[after])) continue;            // "barry", "barbara"
    size_t j = i;
    while (j > 0 && isSep(name[j - 1])) j--;
    const size_t end = j;
    while (j > 0 && isDigit(name[j - 1])) j--;
    if (end == j || end - j > 3) continue;
    int v = 0;
    for (size_t k = j; k < end; k++) v = v * 10 + (name[k] - '0');
    if (v >= 1) return v;
  }
  return 0;
}

uint8_t Generator::clampPause(int bars) {
  if (bars <= 0) return 0;
  if (bars <= 2) return (uint8_t)bars;
  return (uint8_t)kVocalPauseMaxBars;
}

uint8_t Generator::stepPause(int pause, int dir) {
  static const uint8_t steps[] = {0, 1, 2, 4};
  int idx = 0;
  while (idx < 3 && steps[idx] < clampPause(pause)) idx++;
  idx += dir > 0 ? 1 : (dir < 0 ? -1 : 0);
  if (idx < 0) idx = 0;
  if (idx > 3) idx = 3;
  return steps[idx];
}

// A step the engine's clip cap would swallow (the cycle would stay at kVocalWindowMaxBars while the page shows a
// longer pause) is refused: the pause stays, so what the PAUSE page shows is always what plays.
uint8_t Generator::stepPause(int pause, int dir, int windowBars) {
  const uint8_t next = stepPause(pause, dir);
  if (dir > 0 && clampWindow(windowBars) + (int)next > kVocalWindowMaxBars) return clampPause(pause);
  return next;
}

// SPEC §9.19 §1.2: the loop folder of a category under /lp/packs/<genre>/loops/ (docs/LOOPS.md).
const char* Generator::loopFolder(Category c) { return loopFolderName(c); }

// SPEC §9.19 §1.4: the crop of a loop of `bars` bars at variant 0 whole, 1 first half, 2 first 2 beats, 3 first beat.
uint16_t Generator::loopRegionOf(int bars, int variant) {
  if (bars < 1) bars = 1;
  if (bars > kMaxLoopBars) bars = kMaxLoopBars;
  switch (variant & 3) {
    case 1: { uint32_t half = (uint32_t)bars * kTicksPerBar / 2; if (half < (uint32_t)kTicksPerBeat) half = kTicksPerBeat; return (uint16_t)half; }
    case 2: return (uint16_t)(2 * kTicksPerBeat);
    case 3: return (uint16_t)kTicksPerBeat;
    default: return (uint16_t)(bars * kTicksPerBar);
  }
}

// k of len = natural × 2^k on a streaming track, clamped so the clamp to [1 beat, kMaxLoopTicks] never hides a step.
int Generator::clampLoopStep(uint16_t natural, int k) {
  const uint32_t nat = natural ? natural : (uint32_t)kTicksPerBar;
  if (k > 5) k = 5;
  if (k < -5) k = -5;
  while (k > 0 && lenOf(nat, k) > (uint32_t)kMaxLoopTicks) k--;
  while (k < 0 && lenOf(nat, k) < (uint32_t)kTicksPerBeat) k++;
  return k;
}

uint16_t Generator::loopLenOf(uint16_t natural, int k) {
  const uint32_t nat = natural ? natural : (uint32_t)kTicksPerBar;
  uint32_t len = lenOf(nat, clampLoopStep(natural, k));
  if (len < (uint32_t)kTicksPerBeat) len = kTicksPerBeat;
  if (len > (uint32_t)kMaxLoopTicks) len = kMaxLoopTicks;
  return (uint16_t)len;
}

// ---------------------------------------------------------------------------------------------- setup

void Generator::resetStream(VStream& st) {
  st.f = nullptr; st.wi = WavInfo(); st.frames = 0; st.bpm = 0;
  st.natural = 4; st.variant = 0; st.sel = 0; st.windowBars = kVocalWindowBars; st.mode = 0; st.bars = 0;
  st.naturalTicks = kTicksPerBar; st.loopTicks = kTicksPerBar; st.regionTicks = kTicksPerBar;
  st.offFp = 0; st.offCycle = 0;
  memset(st.head, 0, sizeof st.head);
}

void Generator::resetSet(VSet& v) {
  resetStream(v.cur); resetStream(v.nxt);
  v.curValid = v.nxtValid = 0; v.switchState = 0; v.switchGrid = kTicksPerBar; v.switchTick = 0; v.switchSample = 0; v.rewriteFrom = v.rewriteTo = 0; v.start = 0;
  v.engStart = 0; v.playing = 0;
  v.armed = 0; v.armBase = 0; v.armBar = 0; v.armBpm = 0; v.handshake = 0; v.handshakeClock = 0; v.setJobWaiting = 0;
  v.modeDue = 0; v.closing = 0; v.closeTick = 0;
  v.clk = BarClock(); v.clk.bpm = 0;
  v.path[0] = 0;
}

void Generator::init(IFileSystem* fs, Engine* eng, Library* lib) {
  fs_ = fs; eng_ = eng; lib_ = lib;
  genre_ = 0; state_ = Idle; job_ = JobNone; queueN_ = 0; err_[0] = 0; note_[0] = 0;
  done_ = 0; total_ = 0; rushDue_ = 0; variantDue_ = 0; maskDue_ = 0; swapDue_ = 0; pushClock_ = 0; ackTarget_ = 0;
  file_ = nullptr; sd_ = false; pumpTurn_ = 0; slotLen_ = 0; prepSteps_ = 0; pushFail_ = 0; percAlt_ = 0; genFlag_ = 0; resetDone_ = 0;
  soundDue_ = 0; changes_ = 0; rollDue_ = 0; patchInFlight_ = 0; patchClock_ = 0;
  iterChunks_ = 0; lastPumpChunks_ = 0; pumpChunkTotal_ = 0; stepsSkipped_ = 0; pumpClock_ = 0;
  loopRefusal_ = LoopOk; loopOn_ = 0; modeOffDue_ = 0; gateWait_ = 0; loopRush_ = 0;
  openErr_ = OpenOk; openWait_ = 0; vocalReturnDue_ = 0;
  theory_ = TheoryPlan(); theorySet_ = 0; for (int s = 0; s < kSets; s++) theoryProg_[s] = -1;
  for (int s = 0; s < kStreams; s++) openFails_[s] = 0;
  for (int t = 0; t < kTracks; t++) {
    info_[t] = TrackGenInfo(); presetBag_[t] = PresetBag(); variantDueV_[t] = 0; loopTk_[t] = LoopTrack();
    srcOrigin_[t] = 0; srcGenre_[t] = 0; srcHash_[t] = 0; srcLabel_[t] = kNoFolder; srcShard_[t] = kNoFolder;
  }
  for (int s = 0; s < kSets; s++) {
    for (int p = 0; p < 128; p++) foldMap_[s][p] = (uint8_t)(36 + (p & 3));     // map of an empty source (SPEC §9.7)
    swapMask_[s] = 0; vocalLoopPath_[s][0] = 0; altPath_[s][0] = 0;
    for (int k = 0; k < kSampleSlots; k++) { kitOrigin_[s][k] = 0; kitGenre_[s][k] = 0; kitHash_[s][k] = 0; kitRole_[s][k] = kNoFolder; kitShard_[s][k] = kNoFolder; }
  }
  for (int s = 0; s < kStreams; s++) resetSet(stream_[s]);   // field by field: a VSet() temporary would cost 1 KB of stack
  for (int k = 0; k < kLoopSlots; k++) slotTrack_[k] = -1;
  winStream_ = nullptr; winStart_ = winEnd_ = 0; winHavePrev_ = 0;
  clk_ = BarClock();
  path_[0] = 0;
  if (lib_) lib_->scanGenres();
}

// SPEC §9.23: the recorder re-seeds at the start of a recording and of its replay; a draw after it must come from the
// whole family either way, so the per-track preset bags restart with the Rng (presets.h: pickPreset reads the family's
// base and size on every pick, so the mask alone is the bag).
void Generator::seed(uint32_t s) {
  rng_ = Rng(s);
  for (PresetBag& b : presetBag_) b = PresetBag();
}

// SPEC §9.9: a key change restarts the bags (the library keeps its counts, so no index is streamed again);
// loaded tracks keep the key they were generated with.
void Generator::setKeyRoot(int root) {
  key_.root = (uint8_t)(((root % 12) + 12) % 12);
  if (lib_) lib_->invalidate();
  changes_++;
}

void Generator::setGenre(int g) {
  const int n = lib_ ? lib_->genreCount() : 0;
  if (n <= 0) { genre_ = 0; return; }
  genre_ = ((g % n) + n) % n;
  lib_->invalidate();                                        // SPEC §6: packs re-index lazily
  changes_++;
}

bool Generator::setGenreByName(const char* name) {
  if (!lib_ || !name || !name[0]) return false;
  for (int g = 0; g < lib_->genreCount(); g++) if (strcmp(lib_->genreName(g), name) == 0) { setGenre(g); return true; }
  return false;
}

const char* Generator::genreName() const {
  return (lib_ && genre_ < lib_->genreCount()) ? lib_->genreName(genre_) : "";
}

bool Generator::pushCmd(CmdType type, int track, int a, int32_t i, float f) {
  Cmd c; c.type = type; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f;
  return eng_->cmds().push(c);
}

// The loop task is the only producer of the command queue (SPEC §7), so room checked here is room when pushed.
bool Generator::cmdRoom(int n) const { return eng_->cmds().size() + n <= kCmdQueueSize - 1; }

void Generator::notePushFail() {
  if (++pushFail_ >= kPushFailLimit) { pushFail_ = kPushFailLimit; setErr("cmd queue full", ""); }
}

void Generator::setErr(const char* what, const char* detail) {
  if (detail && detail[0]) snprintf(err_, sizeof err_, "%s %s", what, detail);
  else copyStr(err_, sizeof err_, what);
}

// ---------------------------------------------------------------------------------------------- requests

bool Generator::queuePush(uint8_t kind, uint8_t arg) {
  if (state_ != Idle && job_ == kind && jobArg_ == arg) return false;   // that very job is running
  for (int i = 0; i < queueN_; i++) if (queue_[i].kind == kind && queue_[i].arg == arg) return true;
  if (queueN_ >= kRequestQueue) return false;
  queue_[queueN_].kind = kind; queue_[queueN_].arg = arg; queueN_++;
  return true;
}
void Generator::queueDrop(uint8_t kind, uint8_t arg) {
  for (int i = 0; i < queueN_;) {
    if (queue_[i].kind == kind && queue_[i].arg == arg) { for (int j = i + 1; j < queueN_; j++) queue_[j - 1] = queue_[j]; queueN_--; }
    else i++;
  }
}
bool Generator::generateSet(int set) { return (set >= 0 && set < kSets) && queuePush(JobSet, (uint8_t)set); }
// SPEC §9.24: a theory set queues exactly like generateSet. The plan is drawn from the Rng when the job STARTS, and the
// queue runs jobs in request order whatever the clock does, so a recorded session and its offline render (where a set job
// spans many times more audio, SPEC §9.23) draw the same plan; a refusal keyed on busy() would depend on that timing and
// a render would silently drop the set and desynchronise every later draw. A repeated press for the set that is already
// queued coalesces (true, no second job); one for the very job that is running is refused, as for GENERATE.
bool Generator::generateTheorySet(int set) { return (set >= 0 && set < kSets) && eng_ && queuePush(JobTheory, (uint8_t)set); }
bool Generator::theorySet(int set) const { return set >= 0 && set < kSets && ((theorySet_ >> set) & 1) != 0; }
int Generator::theoryProgression(int set) const { return theorySet(set) ? theoryProg_[set] : -1; }
void Generator::theoryReplaced(int track) {
  if (track < 0 || track >= kTracks || !theoryCategory(trackCategory(track))) return;
  theorySet_ &= (uint8_t)~(1u << trackSet(track));
}
bool Generator::generatePattern(int track) { return (track >= 0 && track < kTracks) && queuePush(JobPattern, (uint8_t)track); }
bool Generator::generateSound(int track) { return (track >= 0 && track < kTracks) && queuePush(JobSound, (uint8_t)track); }

// A press that found the engine lock (or the command queue) busy is retried by step(); one that found nothing
// to rebuild (an empty vocal) is dropped, so it cannot fire on a phrase loaded later. On a track in loop mode the
// variant is the loop's crop (SPEC §9.19 §1.4): a same-file switch on the next 1/16, or the pick's crop while it is in flight.
void Generator::rebuildVariant(int track, int variant) {
  if (track < 0 || track >= kTracks) return;
  variant &= 3;
  const uint16_t bit = (uint16_t)(1u << track);
  if (loopEnabled(track)) { loopVariant(track, variant); variantDue_ &= (uint16_t)~bit; changes_++; return; }
  if (publishVariantEx(track, variant) == PubBusy) { variantDue_ |= bit; variantDueV_[track] = (uint8_t)variant; }
  else variantDue_ &= (uint16_t)~bit;
  changes_++;
}

// len± (SPEC §9.4): k steps, len = natural × 2^k clamped to 0.5..16 beats; k is clamped so the clamp never
// hides a step; CmdSetLength applies at the bar. A Vocal track steps its pause instead (SPEC §9.16): the new
// cycle is streamed from a bar line (a stream switch of the same file) and the switch's CmdSwapPattern publishes
// the clip length on that same bar — a separate CmdSetLength would land a bar early. The loop variants (2 / 1
// beats, no pause) ignore len±. A track in loop mode (SPEC §9.19 §1.4): len = crop × 2^k clamped to [1 beat,
// kMaxLoopTicks], streamed as the new cycle from the next 1/16 (the carrier swap carries it to the engine).
uint16_t Generator::setLengthStep(int track, int dir) {
  if (track < 0 || track >= kTracks) return 0;
  TrackGenInfo& inf = info_[track];
  if (loopEnabled(track)) {
    const uint16_t nat = inf.naturalTicks ? inf.naturalTicks : (uint16_t)kTicksPerBar;
    const int k = clampLoopStep(nat, inf.lenStep + (dir > 0 ? 1 : (dir < 0 ? -1 : 0)));
    const uint16_t len = loopLenOf(nat, k);
    if (dir == 0) return len;
    inf.lenStep = (int8_t)k;
    loopLen(track, len);                                     // PubNone while the pick is in flight: it applies the step
    changes_++;
    return len;
  }
  if (trackCategory(track) == Vocal) {
    inf.lenStep = 0;
    if (dir == 0 || inf.variant >= 2) return vocalClipTicks(inf.variant, inf.pause, inf.windowBars);
    const uint8_t pause = stepPause(inf.pause, dir, inf.windowBars);   // a step the engine's cap would swallow is refused
    if (pause == inf.pause) return vocalClipTicks(inf.variant, inf.pause, inf.windowBars);   // clamped: nothing changes
    inf.pause = pause;
    vocalVariant(trackSet(track), inf.variant);             // the producer's cycle and the engine's len follow at the switch bar
    changes_++;
    return vocalClipTicks(inf.variant, inf.pause, inf.windowBars);
  }
  const uint32_t nat = inf.naturalTicks ? inf.naturalTicks : kTicksPerBar;
  int k = inf.lenStep + (dir > 0 ? 1 : (dir < 0 ? -1 : 0));
  if (k > 5) k = 5;
  if (k < -5) k = -5;
  while (k > 0 && lenOf(nat, k) > (uint32_t)kMaxPatternTicks) k--;
  while (k < 0 && lenOf(nat, k) < (uint32_t)(kTicksPerBeat / 2)) k++;
  const uint16_t len = clampLen(lenOf(nat, k));
  if (dir == 0) return len;                                  // query only
  inf.lenStep = (int8_t)k;
  if (!pushCmd(CmdSetLength, track, 0, len)) notePushFail();
  changes_++;
  return len;
}

// ---------------------------------------------------------------------------------------------- loading helpers

bool Generator::loadMidiToSource(int track, const char* path) {
  if (!fs_ || !eng_ || track < 0 || track >= kTracks || !path) return false;
  IFile* f = fs_->open(path);
  if (!f) { setErr("open", baseName(path)); return false; }
  Pattern& src = eng_->sourcePattern(track);
  const bool ok = smfParse(*f, src);
  f->close();
  if (!ok) { src.clear(); setErr("bad midi", baseName(path)); return false; }
  if (trackCategory(track) == Percussion) percussionFoldMap(src, foldMap_[trackSet(track)]);   // SPEC §9.7, stored
  TrackGenInfo& inf = info_[track];
  copyStr(inf.file, sizeof inf.file, baseName(path));
  inf.sourceTicks = src.lengthTicks;
  inf.userLoop = 0; inf.acidPat = 0;
  noteSource(track, path);
  theoryReplaced(track);                                     // a file (a set load, a library pick) replaced a theory pattern (SPEC §9.24)
  return true;
}

// SPEC §9.17b: the acid generator's pattern for (seed, keyRoot) becomes the track's source (Lead / Bass: a line,
// Chord / Pad: stabs; Bass an octave below Lead). Pure. The session restore rebuilds a line from its saved seed.
void Generator::buildAcidSource(int track, uint32_t seed, int keyRoot) {
  if (!eng_ || track < 0 || track >= kTracks) return;
  const Category c = trackCategory(track);
  if (!acidCategory(c)) return;
  TrackGenInfo& inf = info_[track];
  Pattern& src = eng_->sourcePattern(track);
  ProjectKey k; k.root = (uint8_t)(((keyRoot % 12) + 12) % 12);
  if (c == Lead || c == Bass) acidLine(seed, k, src, c == Bass ? 1 : 2);
  else acidStabs(seed, k, src, 3);
  inf.seed = seed; inf.keyRoot = k.root;
  inf.file[0] = 0; inf.acidPat = 1; inf.userLoop = 0; inf.edited = 0;
  inf.sourceTicks = src.lengthTicks;
  srcOrigin_[track] = 0;
  theoryReplaced(track);
}

// ---- remembered pick folders (SPEC §9.19 §3 b): the session writer composes absolute paths, probing nothing ----

uint32_t Generator::nameHash(const char* name) {
  uint32_t h = 2166136261u;
  for (; name && *name; name++) { h ^= (uint8_t)*name; h *= 16777619u; }
  return h;
}

// "<label-or-role>[/sNN]" → two bytes: a key folder as root (bits 0..3) | minor (bit 4) | NA (bit 5), a kit role folder as
// its index in kKitRoles; the shard as the NN of "sNN" (0..254); kNoFolder = none. False (both kNoFolder) when the folder
// has another shape or does not read back identically through decodeFolder ("Cmaj" would come back as "C", "s5" as "s05").
bool Generator::encodeFolder(const char* dir, bool kit, uint8_t& label, uint8_t& shard) {
  label = shard = kNoFolder;
  if (!dir || !dir[0]) return false;
  const char* slash = strchr(dir, '/');
  const size_t hl = slash ? (size_t)(slash - dir) : strlen(dir);
  char head[kFolderTextLen];
  if (hl == 0 || hl >= sizeof head) return false;
  memcpy(head, dir, hl); head[hl] = 0;
  uint8_t sh = kNoFolder;
  if (slash) {
    const char* s = slash + 1;
    if (s[0] != 's' || !isDigit(s[1])) return false;
    int n = 0;
    const char* p = s + 1;
    for (; isDigit(*p); p++) { n = n * 10 + (*p - '0'); if (n > 254) return false; }
    if (*p) return false;
    sh = (uint8_t)n;
  }
  uint8_t lb = kNoFolder;
  if (kit) {
    for (int i = 0; i < kKitRoleCount && lb == kNoFolder; i++) if (strcmp(head, kKitRoles[i]) == 0) lb = (uint8_t)i;
    if (lb == kNoFolder) return false;
  } else {
    const LibraryKey k = parseLibraryLabel(head);
    if (!k.valid) return false;
    lb = k.keyless ? (uint8_t)0x20 : (uint8_t)((k.root & 15) | (k.minor ? 0x10 : 0));
  }
  char back[kFolderTextLen];
  if (!decodeFolder(lb, sh, kit, back, sizeof back) || strcmp(back, dir) != 0) return false;
  label = lb; shard = sh;
  return true;
}

bool Generator::decodeFolder(uint8_t label, uint8_t shard, bool kit, char* out, int cap) {
  if (!out || cap <= 0) return false;
  out[0] = 0;
  if (label == kNoFolder) return false;
  int n;
  if (kit) { if (label >= kKitRoleCount) return false; n = snprintf(out, (size_t)cap, "%s", kKitRoles[label]); }
  else if (label & 0x20) n = snprintf(out, (size_t)cap, "NA");
  else n = snprintf(out, (size_t)cap, "%s%s", noteName(label & 15), (label & 0x10) ? "min" : "");
  if (n <= 0 || n >= cap) { out[0] = 0; return false; }
  if (shard != kNoFolder) {
    const int m = snprintf(out + n, (size_t)(cap - n), "/s%02u", (unsigned)shard);
    if (m <= 0 || m >= cap - n) { out[0] = 0; return false; }
  }
  return true;
}

// Where a pattern came from: the library key folder (and shard) under /lp/midi/<Category>/, this genre's
// presets/assets, or /lp/state. Anything else stays unknown (the writer then probes as before).
void Generator::noteSource(int track, const char* path) {
  uint8_t origin = 0, genre = 0, label = kNoFolder, shard = kNoFolder;
  const char* base = baseName(path);
  char mid[48]; snprintf(mid, sizeof mid, "/lp/midi/%s/", categoryName(trackCategory(track)));
  const size_t ml = strlen(mid);
  if (strncmp(path, mid, ml) == 0 && base > path + ml) {
    const int n = (int)(base - (path + ml)) - 1;             // "<label>[/sNN]" without the trailing slash
    char dir[kFolderTextLen];
    if (n > 0 && n < kFolderTextLen) { memcpy(dir, path + ml, (size_t)n); dir[n] = 0; if (encodeFolder(dir, false, label, shard)) origin = 1; }
  } else if (strncmp(path, "/lp/state/", 10) == 0 && base == path + 10) {
    origin = 3;
  } else if (const int gl = packGenreLen(path)) {
    const char* rest = path + 10 + gl;
    if (strncmp(rest, "/presets/assets/", 16) == 0 && base == rest + 16 && lib_) {
      for (int g = 0; g < lib_->genreCount(); g++) {
        const char* gn = lib_->genreName(g);
        if ((int)strlen(gn) == gl && strncmp(gn, path + 10, (size_t)gl) == 0) { origin = 2; genre = (uint8_t)g; break; }
      }
    }
  }
  srcOrigin_[track] = origin; srcGenre_[track] = genre; srcHash_[track] = nameHash(base);
  srcLabel_[track] = label; srcShard_[track] = shard;
}

// A one-shot under /lp/packs/<genre>/drum/<role>[/sNN]/<name>: role folder (and shard) + the genre; else unknown.
void Generator::noteKitPath(int set, int slot, const char* path) {
  if (set < 0 || set >= kSets || slot < 0 || slot >= kSampleSlots || !path) return;
  uint8_t origin = 0, genre = 0, role = kNoFolder, shard = kNoFolder;
  const char* base = baseName(path);
  if (const int gl = packGenreLen(path)) {
    const char* rest = path + 10 + gl;
    if (strncmp(rest, "/drum/", 6) == 0 && base > rest + 6 && lib_) {
      const int n = (int)(base - (rest + 6)) - 1;
      char dir[kFolderTextLen];
      for (int g = 0; g < lib_->genreCount() && n > 0 && n < kFolderTextLen; g++) {
        const char* gn = lib_->genreName(g);
        if ((int)strlen(gn) == gl && strncmp(gn, path + 10, (size_t)gl) == 0) {
          memcpy(dir, rest + 6, (size_t)n); dir[n] = 0;
          if (encodeFolder(dir, true, role, shard)) { origin = 1; genre = (uint8_t)g; }
          break;
        }
      }
    }
  }
  kitOrigin_[set][slot] = origin; kitGenre_[set][slot] = genre; kitHash_[set][slot] = nameHash(base);
  kitRole_[set][slot] = role; kitShard_[set][slot] = shard;
}

bool Generator::composePath(uint8_t origin, const char* dir, uint8_t genre, const char* midBase, const char* file, char* out, int cap) const {
  const char* gname = (lib_ && genre < lib_->genreCount()) ? lib_->genreName(genre) : nullptr;
  int n;
  switch (origin) {
    case 1: n = snprintf(out, (size_t)cap, "%s/%s/%s", midBase, dir, file); break;
    case 2: if (!gname) return false; n = snprintf(out, (size_t)cap, "/lp/packs/%s/presets/assets/%s", gname, file); break;
    case 3: n = snprintf(out, (size_t)cap, "/lp/state/%s", file); break;
    default: return false;
  }
  if (n <= 0 || n >= cap) { out[0] = 0; return false; }
  return true;
}

bool Generator::sourcePath(int track, char* out, int cap) const {
  if (!out || cap <= 0) return false;
  out[0] = 0;
  if (track < 0 || track >= kTracks) return false;
  const TrackGenInfo& inf = info_[track];
  if (!inf.file[0] || inf.acidPat || !srcOrigin_[track] || nameHash(inf.file) != srcHash_[track]) return false;
  char mid[32]; snprintf(mid, sizeof mid, "/lp/midi/%s", categoryName(trackCategory(track)));
  char dir[kFolderTextLen] = "";
  if (srcOrigin_[track] == 1 && !decodeFolder(srcLabel_[track], srcShard_[track], false, dir, sizeof dir)) return false;
  return composePath(srcOrigin_[track], dir, srcGenre_[track], mid, inf.file, out, cap);
}

bool Generator::kitPath(int set, int slot, char* out, int cap) const {
  if (!out || cap <= 0) return false;
  out[0] = 0;
  if (!eng_ || set < 0 || set >= kSets || slot < 0 || slot >= kSampleSlots || kitOrigin_[set][slot] != 1) return false;
  const char* name = eng_->kit(set).name[slot];
  if (!name[0] || nameHash(name) != kitHash_[set][slot]) return false;
  const char* gname = (lib_ && kitGenre_[set][slot] < lib_->genreCount()) ? lib_->genreName(kitGenre_[set][slot]) : nullptr;
  if (!gname) return false;
  char base[kMaxPathLen]; snprintf(base, sizeof base, "/lp/packs/%s/drum", gname);
  char dir[kFolderTextLen];
  if (!decodeFolder(kitRole_[set][slot], kitShard_[set][slot], true, dir, sizeof dir)) return false;
  return composePath(1, dir, kitGenre_[set][slot], base, name, out, cap);
}

// source + seed/key → next (SPEC §9.5); Beat always builds Full (SPEC §9.6). Its rush loop and layer mask
// go out at once for a single-track publish, or with the set swap (deferBeat) so mask and pattern land on
// the same bar line.
Generator::PubResult Generator::buildNext(int track, int variant, bool deferBeat) {
  const Category c = trackCategory(track); const int set = trackSet(track); TrackGenInfo& inf = info_[track];
  if (c == Vocal) return PubNone;
  if (!eng_->beginNextWrite(track)) return PubBusy;
  Pattern& out = eng_->nextPattern(track);
  ProjectKey k; k.root = inf.keyRoot;
  buildVariant(c, eng_->sourcePattern(track), c == Beat ? 3 : variant, inf.seed, k, out, c == Percussion ? foldMap_[set] : nullptr);
  eng_->endNextWrite(track);
  inf.naturalTicks = out.lengthTicks; inf.variant = (uint8_t)variant; inf.lenStep = 0; inf.loaded = 1;
  if (c == Beat) {
    const uint8_t bit = (uint8_t)(1u << set);
    if (deferBeat) { maskDue_ |= bit; return PubOk; }
    if (!writeRush(set)) rushDue_ |= bit;
    if (!pushCmd(CmdBeatMask, 0, set, maskCmd(variant))) { notePushFail(); return PubBusy; }
  }
  return PubOk;
}

Generator::PubResult Generator::publishVariantEx(int track, int variant) {
  if (!eng_ || track < 0 || track >= kTracks) return PubNone;
  variant &= 3;
  if (trackCategory(track) == Vocal) return vocalVariant(trackSet(track), variant);
  const PubResult r = buildNext(track, variant, false);
  if (r != PubOk) return r;
  if (!pushCmd(CmdSwapPattern, track, variant, 0)) { notePushFail(); return PubBusy; }
  pushFail_ = 0;
  return PubOk;
}

bool Generator::publishVariant(int track, int variant) { return publishVariantEx(track, variant) == PubOk; }

// Snare rush = crop(Full, 1 beat) of the Beat's next (always Full) pattern; only while rush is off (engine.h).
bool Generator::writeRush(int set) {
  const int beat = set * kTracksPerSet + Beat;
  if (eng_->status().track[beat].rush) return false;
  snareRush(eng_->nextPattern(beat), eng_->rushPattern(set));
  return true;
}

// The bag runs over the track's family (SPEC §9.17b): the library presets, or the '303' family appended after them in
// acid mode. SPEC §9.22 (research §4): a 512-bit seen mask per track instead of a 16-entry order, so a generated bank of
// up to kPresetBagMax presets per family is drawn uniformly over the presets not yet heard in this cycle, none repeats
// until the family is exhausted, then the mask clears and the next cycle begins with any preset but the one sounding.
// The family's base and size come from presets.h on every pick and the last draw is the track's presetIndex (a set load
// writes that too, so the preset a load brought in is not re-drawn first), so the bag is the mask alone; a presetIndex
// outside the family (the acid toggle switched families, nothing picked yet) restarts the cycle. presetIndex is absolute.
namespace {
inline bool bagSeen(const uint8_t* m, int i) { return ((m[i >> 3] >> (i & 7)) & 1) != 0; }
inline void bagMark(uint8_t* m, int i) { m[i >> 3] = (uint8_t)(m[i >> 3] | (1u << (i & 7))); }
}
bool Generator::pickPreset(int track, Category c) {
  PresetBag& bg = presetBag_[track];
  const bool acid = isAcidTrack(track) && acidPresetCount(c) > 0;
  const int base = acid ? libraryPresetCount(c) : 0;
  int n = acid ? acidPresetCount(c) : libraryPresetCount(c); if (n > kPresetBagMax) n = kPresetBagMax;
  if (n <= 0) return false;
  TrackGenInfo& inf = info_[track];
  const int cur = (int)inf.presetIndex - base;
  int last = (cur >= 0 && cur < n) ? cur : -1;              // the preset sounding now, when it belongs to this family
  if (last < 0) bg = PresetBag();                            // another family: the cycle restarts
  int seen = 0;
  for (int i = 0; i < n; i++) if (bagSeen(bg.seen, i)) seen++;
  if (seen == 0 && !inf.sound[0]) last = -1;                 // nothing was ever picked here (the engine plays its init patch): no draw to avoid
  if (seen >= n) { bg = PresetBag(); seen = 0; }             // exhausted: a new cycle
  bool skipLast = last >= 0 && n > 1 && !bagSeen(bg.seen, last);   // no immediate repeat across the refill (SPEC §5)
  int cand = n - seen - (skipLast ? 1 : 0);
  if (cand <= 0) { bg = PresetBag(); seen = 0; skipLast = last >= 0 && n > 1; cand = n - (skipLast ? 1 : 0); }   // only the sounding preset was left: new cycle without it
  int k = rng_.range(0, cand - 1), pick = -1;
  for (int i = 0; i < n && pick < 0; i++) {
    if (bagSeen(bg.seen, i) || (skipLast && i == last)) continue;
    if (k-- == 0) pick = i;
  }
  if (pick < 0) pick = 0;                                    // unreachable: cand counted the candidates above
  bagMark(bg.seen, pick);
  inf.presetIndex = (uint16_t)(base + pick);
  return true;
}

// ---------------------------------------------------------------------------------------------- acid mode (SPEC §9.17b)

bool Generator::acidEnabled(int track) const { return track >= 0 && track < kTracks && info_[track].acid != 0 && acidCategory(trackCategory(track)); }

// Toggling re-rolls the sound (no card): the '303' family on, a library preset off. The pattern stays. The roll goes
// out at once when the track's nextPatch is free, else from retryDue two blocks later (see patchFree).
void Generator::setAcid(int track, bool on) {
  if (!eng_ || track < 0 || track >= kTracks || !acidCategory(trackCategory(track))) return;
  info_[track].acid = on ? 1 : 0;
  rollSound(track);
  changes_++;
}

// A CmdSetPatch pushed at patchClock_ has been drained once the engine rendered two blocks past it (the queue is
// drained at the start of every block; the status clock is the block's end). One clock for all tracks: a push on
// another track extends the window by two blocks at most, which costs nothing.
bool Generator::patchFree(int track) {
  if (patchInFlight_ && (int32_t)(eng_->status().sampleClock - patchClock_) >= 2 * kBlock) patchInFlight_ = 0;
  return !((patchInFlight_ >> track) & 1);
}

// Seeded musical ranges on the normalised knobs (types.h mappings; SPEC §9.17b, §9.22 with research §4 / §5.3): cutoff
// 0.25..0.75, resonance 0.45..0.95, envAmt 0.3..0.9, decay 0.15..0.6, drive 0..0.5 (narrowed from 0.6 now that the
// equal-RMS makeup of §9.21 is in, as the research asks), wave saw or square, the 303 character SAT 0.3..0.9 and ACC
// 0.3..0.9 on every acid track, and the chorus send 0..0.4 on Lead / Chord only (Pad and Bass stay dry: the bus is per
// set and a chorused bass smears the low end). knobCmdCount() CmdSetParam, pushed only when they all fit.
void Generator::pushKnobs(int track) {
  const Category c = trackCategory(track);
  const float cutoff = 0.25f + 0.50f * rng_.unit();
  const float res = 0.45f + 0.50f * rng_.unit();
  const float env = 0.30f + 0.60f * rng_.unit();
  const float decay = 0.15f + 0.45f * rng_.unit();
  const float drive = 0.50f * rng_.unit();
  const int wave = (rng_.next() & 1) ? WSquare : WSaw;
  const float sat = 0.30f + 0.60f * rng_.unit();
  const float accent = 0.30f + 0.60f * rng_.unit();
  pushCmd(CmdSetParam, track, PCutoff, 0, cutoff);
  pushCmd(CmdSetParam, track, PResonance, 0, res);
  pushCmd(CmdSetParam, track, PEnvAmt, 0, env);
  pushCmd(CmdSetParam, track, PDecay, 0, decay);
  pushCmd(CmdSetParam, track, PDrive, 0, drive);
  pushCmd(CmdSetParam, track, PWave, wave, (float)wave);
  pushCmd(CmdSetParam, track, PSat, 0, sat);
  pushCmd(CmdSetParam, track, PAccent, 0, accent);
  if (c == Lead || c == Chord) pushCmd(CmdSetParam, track, PChorus, 0, 0.40f * rng_.unit());
}

void Generator::randomizeKnobs(int track) {
  if (!eng_ || track < 0 || track >= kTracks || !isSynthCategory(trackCategory(track))) return;
  if (!cmdRoom(knobCmdCount(trackCategory(track)))) { notePushFail(); return; }
  pushKnobs(track);
  changes_++;
}

bool Generator::pushSound(int track) {
  const bool acid = isAcidTrack(track);
  if (!cmdRoom(1 + (acid ? knobCmdCount(trackCategory(track)) : 0))) return false;
  pushCmd(CmdSetPatch, track);
  patchInFlight_ |= (uint16_t)(1u << track); patchClock_ = eng_->status().sampleClock;   // nextPatch is the engine's until it copied it
  if (acid) pushKnobs(track);                                // after CmdSetPatch: the knob init from the patch is overridden
  return true;
}

// The roll waits (rollDue_) while nextPatch is in flight or a job is on this very track (its stepPreset owns the
// struct until its own CmdSetPatch is out); retryDue performs it as soon as both are clear.
bool Generator::rollSound(int track) {
  const uint16_t bit = (uint16_t)(1u << track);
  if (!patchFree(track) || (state_ != Idle && track_ == track)) { rollDue_ |= bit; return true; }
  rollDue_ &= (uint16_t)~bit;
  const Category c = trackCategory(track);
  if (!pickPreset(track, c)) return false;
  SynthPatch& p = eng_->nextPatch(track);
  makePatch(c, info_[track].presetIndex, rng_.next(), p);
  copyStr(info_[track].sound, sizeof info_[track].sound, p.name);
  if (pushSound(track)) { soundDue_ &= (uint16_t)~bit; return true; }
  soundDue_ |= bit; notePushFail();                          // nextPatch holds the sound: pushed by a later step()
  return false;
}

// Every source note ±12 (clamped 12..115); the current variant is republished (swap at the bar, retried when the
// engine holds the lock) and the track counts as edited (set files write it as an SMF asset, SPEC §9.8).
void Generator::transposeOctave(int track, int dir) {
  if (!eng_ || track < 0 || track >= kTracks || dir == 0 || !isSynthCategory(trackCategory(track))) return;
  Pattern& src = eng_->sourcePattern(track);
  if (src.count == 0) return;
  const int d = dir > 0 ? 12 : -12;
  for (int i = 0; i < src.count; i++) {
    int p = src.notes[i].pitch() + d;
    if (p < 12) p = 12;
    if (p > 115) p = 115;
    src.notes[i].setPitch((uint8_t)p);
  }
  TrackGenInfo& inf = info_[track];
  inf.edited = 1;
  if (loopEnabled(track)) { changes_++; return; }            // the parked pattern changed; it is republished when loop mode ends (the engine swaps it then)
  rebuildVariant(track, inf.variant);                        // bumps changes_
}

// ---------------------------------------------------------------------------------------------- job state machine

void Generator::step() {
  if (!eng_ || !fs_ || !lib_) return;
  iterChunks_ = (uint8_t)pump(kPumpChunks);                  // SPEC §9.2: stream refills before any other SD work
  retryDue();
  if (state_ == Idle) {
    if (queueN_ == 0) {                                      // no job: a deferred return of a Vocal track to its phrase (SPEC §9.19g) is still a card step of its own, under the same gates
      if (vocalReturnDue_ && iterChunks_ < kStepSkipChunks && streamsHealthy() && retryVocalReturn()) iterChunks_++;
      return;
    }
    const Request r = queue_[0];
    for (int i = 1; i < queueN_; i++) queue_[i - 1] = queue_[i];
    queueN_--;
    startJob(r);
  }
  if (iterChunks_ >= kStepSkipChunks) { stepsSkipped_++; return; }   // the pump spent this step's card budget (SPEC §9.2)
  if (!streamsHealthy()) { stepsSkipped_++; return; }         // SPEC §9.19 §3 a: every card step opens something (an index, a file): it waits for the rings too; the refused query tops them up next pump
  sd_ = false;
  if (vocalReturnDue_ && retryVocalReturn()) { iterChunks_++; return; }   // SPEC §9.19g: a Vocal track's deferred return to its phrase (one open) is this step's card operation
  for (int guard = 0; guard < kDispatchChain && state_ != Idle && !sd_; guard++) {
    const State before = state_;
    dispatch();
    if (state_ == before) break;                             // waiting (ack, bar line, lock, queue room, the stream gate) or a chunk loop
  }
  if (sd_) iterChunks_++;                                    // the step's own card operation counts against the pump that follows it
}
// (stepsSkipped_ counts both: a step skipped for the pump's budget and one skipped for the stream gate.)

void Generator::dispatch() {
  switch (state_) {
    case TrackBegin: beginTrack(); break;
    case PrepPick: stepPrepPick(); break;
    case Pick: stepPick(); break;
    case Load: stepLoad(); break;
    case Publish: stepPublish(); break;
    case Preset: stepPreset(); break;
    case AcidPat: stepAcidPat(); break;
    case TheoryPat: stepTheoryPat(); break;
    case SoundBegin: stepSoundBegin(); break;
    case PrepSlot: stepPrepSlot(); break;
    case SlotPick: stepSlotPick(); break;
    case SlotOpen: stepSlotOpen(); break;
    case SlotChunk: stepSlotChunk(); break;
    case SlotPush: stepSlotPush(); break;
    case SlotWait: stepSlotWait(); break;
    case PrepVocal: stepPrepVocal(); break;
    case VocalPick: stepVocalPick(); break;
    case VocalOpen: stepVocalOpen(); break;
    case PrepLoop: stepPrepLoop(); break;
    case LoopPick: stepLoopPick(); break;
    case LoopOpen: stepLoopOpen(); break;
    case TrackDone: trackDone(); break;
    case Finish: stepFinish(); break;
    case WaitSwap: stepWaitSwap(); break;
    default: break;
  }
}

// Rush rebuilds, variant publishes that found the next lock busy (engine mid-copy), a loader's set swap that
// found the command queue full and a CmdLoopMode off that did are retried here.
void Generator::retryDue() {
  for (int s = 0; s < kSets; s++) {
    if ((rushDue_ >> s) & 1) { if (writeRush(s)) rushDue_ &= (uint8_t)~(1u << s); }
    if ((swapDue_ >> s) & 1) { if (pushSetSwaps(s)) swapDue_ &= (uint8_t)~(1u << s); }
  }
  if (modeOffDue_) for (int t = 0; t < kTracks; t++) if ((modeOffDue_ >> t) & 1) { if (pushCmd(CmdLoopMode, t, 0, 0)) modeOffDue_ &= (uint16_t)~(1u << t); }
  if (soundDue_) for (int t = 0; t < kTracks; t++) if ((soundDue_ >> t) & 1) { if (pushSound(t)) soundDue_ &= (uint16_t)~(1u << t); }
  if (rollDue_) for (int t = 0; t < kTracks; t++) if ((rollDue_ >> t) & 1) { if (patchFree(t) && !(state_ != Idle && track_ == t)) rollSound(t); }   // a deferred acid toggle (SPEC §9.17b)
  if (!variantDue_) return;
  for (int t = 0; t < kTracks; t++) if ((variantDue_ >> t) & 1) { if (publishVariantEx(t, variantDueV_[t]) != PubBusy) variantDue_ &= (uint16_t)~(1u << t); }
}

void Generator::startJob(const Request& r) {
  job_ = r.kind; jobArg_ = r.arg; err_[0] = 0; note_[0] = 0; done_ = 0; pushFail_ = 0; resetDone_ = 0; openWait_ = 0;
  if (setJob()) {
    set_ = r.arg; trackIdx_ = 0; total_ = kTracksPerSet; genFlag_ = 0;
    swapMask_[set_] = 0; maskDue_ &= (uint8_t)~(1u << set_);
    for (int s = 0; s < kStreams; s++) if (streamSet(s) == set_) stream_[s].setJobWaiting = 0;
    variantDue_ &= (uint16_t)~(1u << (set_ * kTracksPerSet + Vocal));   // a stale press on an empty vocal must not fire on the new phrase
    theorySet_ &= (uint8_t)~(1u << set_);                      // the melodic tracks are being replaced either way; a theory job sets the bit again when it finishes
    if (job_ == JobTheory) theoryPlan(rng_, key_.root, theory_);   // SPEC §9.24: progression, voicings, templates — every draw now, so the recorder's re-seed replays the same set
  } else {
    set_ = (uint8_t)trackSet(r.arg); total_ = 1;
    if (job_ == JobPattern) theoryReplaced(r.arg);
  }
  state_ = TrackBegin;
}

void Generator::beginTrack() {
  openWait_ = 0;                                             // the handle wait of SPEC §9.19g is per track
  if (job_ == JobLoop) {                                     // SPEC §9.19: the pick of a track that just entered loop mode
    track_ = jobArg_; doPat_ = 0; doSnd_ = 0;
    if (!loopEnabled(track_)) { state_ = TrackDone; return; }   // switched off before the pick ran
    prepSteps_ = 0; attempt_ = 0; state_ = PrepLoop;
    return;
  }
  if (setJob()) {
    if (!genFlag_) {                                         // SPEC §9.5: pat/len presses queue until the swap
      if (!pushCmd(CmdSetGenerating, 0, set_, 1)) { notePushFail(); return; }
      genFlag_ = 1;
    }
    if (trackIdx_ >= kTracksPerSet) { state_ = Finish; return; }
    track_ = (uint8_t)(set_ * kTracksPerSet + kSetOrder[trackIdx_]); doPat_ = 1; doSnd_ = 1;
  } else {
    track_ = jobArg_; doPat_ = job_ == JobPattern; doSnd_ = job_ == JobSound;
  }
  const Category c = trackCategory(track_);
  if (doPat_ && info_[track_].userLoop) { doPat_ = 0; snprintf(note_, sizeof note_, "%s kept", categoryShort(c)); }   // SPEC §9.10 / §9.19 §3: a recorded loop on ANY track survives a pattern job
  if (!doPat_ && !doSnd_) { state_ = TrackDone; return; }   // nothing regenerated (a kept loop under a pattern job): no reset either
  if (loopEnabled(track_)) {                                 // SPEC §9.19 §1.5: gen pattern / gen sound / GENERATE on a loop track pick a new loop; loop mode and the parked sound stay (no reset)
    info_[track_].lenStep = 0;                               // a new loop starts at its natural length (SPEC §9.4: k reset on gen)
    prepSteps_ = 0; attempt_ = 0; state_ = PrepLoop;
    return;
  }
  prepSteps_ = 0;
  if (c == Vocal) {                                          // pattern or sound both mean another phrase file
    if (!pushReset()) return;                                // SPEC §9.17b: the regenerated track starts from the category defaults
    phase_ = 0; attempt_ = 0; state_ = PrepVocal;
    return;
  }
  if (doPat_) {
    if (!doSnd_ && !pushReset()) return;                     // pattern only: the reset is this track's first command; with a sound part it waits for SoundBegin
    if (job_ == JobTheory && theoryCategory(c)) state_ = TheoryPat;   // SPEC §9.24: the melodic tracks come from the plan (an acid-mode track too: its '303' sound stays, the line is the theory's)
    else state_ = isAcidTrack(track_) ? AcidPat : PrepPick;   // acid mode: the line is generated, not picked
    return;
  }
  nextPart();
}

// SPEC §9.24: the track's source from the theory plan (pure, no card), labelled "THEORY <progression>" and marked edited
// so set files and sessions write it as an SMF asset (SPEC §9.8 / §9.18) and a theory set reloads exactly; the variant
// seed is new per track as a library pick's is. The folder memory is cleared: the session writer must not compose a path.
void Generator::stepTheoryPat() {
  const Category c = trackCategory(track_);
  Pattern& src = eng_->sourcePattern(track_);
  theoryBuild(theory_, c, src);
  TrackGenInfo& inf = info_[track_];
  char name[48]; theoryProgName(theory_, name, sizeof name);
  snprintf(inf.file, sizeof inf.file, "THEORY %s", name);
  inf.seed = rng_.next(); inf.keyRoot = key_.root;
  inf.sourceTicks = src.lengthTicks;
  inf.userLoop = 0; inf.acidPat = 0; inf.edited = 1;
  srcOrigin_[track_] = 0; srcGenre_[track_] = 0; srcHash_[track_] = nameHash(inf.file); srcLabel_[track_] = kNoFolder; srcShard_[track_] = kNoFolder;
  state_ = Publish;
}

bool Generator::pushReset() {
  if (resetDone_) return true;
  if (!pushCmd(CmdResetTrack, track_)) { notePushFail(); return false; }
  resetDone_ = 1;
  return true;
}

// SPEC §9.17b: the acid line / stabs from a fresh seed in the global key, straight into the source (no card).
void Generator::stepAcidPat() {
  buildAcidSource(track_, rng_.next(), key_.root);
  state_ = Publish;
}

void Generator::nextPart() {
  if (!doSnd_) { state_ = TrackDone; return; }
  state_ = SoundBegin;                                       // pure transition: chained inside the same step()
}

// SPEC §9.17b: the reset lands right before the new sound (retried on a full queue like CmdSetGenerating), then the
// sound part proper: a preset for the synth categories, the kit slots for Beat / Percussion.
void Generator::stepSoundBegin() {
  if (!pushReset()) return;
  const Category c = trackCategory(track_);
  if (isSynthCategory(c)) { phase_ = 0; state_ = Preset; }
  else if (c == Beat) { slotEnd_ = 4; enterSlot(0); }
  else if (c == Percussion) { slotEnd_ = kSampleSlots; enterSlot(4); }
  else state_ = TrackDone;
}

// Library warm-up (SPEC §9.2): one index chunk / listing per step until the pick is cached; a pick that stays
// cold after kPrepareStepsMax steps (a card without indexes) is made anyway and finishes the counting itself.
// A pick that was warm from the start is made in this same step; one that needed warm-up steps waits for the
// next step (the step that streamed the last chunk does not also pick).
void Generator::prepared(State next) {
  if (prepSteps_) sd_ = true;
  prepSteps_ = 0; state_ = next;
}

void Generator::stepPrepPick() {
  if (lib_->prepareMidi(trackCategory(track_), key_) || ++prepSteps_ > kPrepareStepsMax) { prepared(Pick); return; }
  sd_ = true;
}

void Generator::stepPick() {
  sd_ = true;
  const Category c = trackCategory(track_);
  if (!lib_->pickMidi(c, key_, track_, rng_, path_, sizeof path_)) {
    char lab[8], maj[8]; key_.labels(lab, maj);
    snprintf(err_, sizeof err_, "no %s in %s", categoryShort(c), isSampleCategory(c) ? "NA" : lab);
    nextPart();
    return;
  }
  state_ = Load;
}

void Generator::stepLoad() {
  sd_ = true;
  if (!loadMidiToSource(track_, path_)) { nextPart(); return; }
  TrackGenInfo& inf = info_[track_];
  inf.seed = rng_.next(); inf.keyRoot = key_.root;           // new seed per pick (SPEC §5), key remembered (SPEC §9.9)
  state_ = Publish;
}

// Builds the variant into next; a set job keeps the swap (and the Beat's mask) for the set-wide push, a single
// job swaps now. A busy lock or a full queue keeps the state for the next step. A new Beat pattern starts on
// variant 0 (Kick only, SPEC §9.17b); the other categories keep their natural variant (set job) or the selected one.
void Generator::stepPublish() {
  const Category c = trackCategory(track_);
  const int v = c == Beat ? 0 : (setJob() ? naturalVariant(c) : info_[track_].variant);
  const PubResult r = buildNext(track_, v, setJob());
  if (r == PubBusy) return;
  if (r == PubOk) {
    if (setJob()) swapMask_[set_] |= (uint8_t)(1u << (track_ % kTracksPerSet));
    else if (!pushCmd(CmdSwapPattern, track_, v, 0)) { notePushFail(); return; }
  }
  pushFail_ = 0;
  nextPart();
}

void Generator::stepPreset() {
  const Category c = trackCategory(track_); TrackGenInfo& inf = info_[track_];
  if (phase_ == 0) {                                         // pick once; the push may have to wait for queue room
    if (!patchFree(track_)) return;                          // an earlier CmdSetPatch (an acid toggle) is still being copied: next step
    if (!pickPreset(track_, c)) { state_ = TrackDone; return; }
    SynthPatch& p = eng_->nextPatch(track_);
    makePatch(c, inf.presetIndex, rng_.next(), p);
    copyStr(inf.sound, sizeof inf.sound, p.name);
    phase_ = 1;
  }
  if (!pushSound(track_)) { notePushFail(); return; }       // CmdSetPatch (+ the acid knobs, SPEC §9.17b) once the queue has room
  pushFail_ = 0; phase_ = 0;
  state_ = TrackDone;
}

// Kit slots: attempt_ 0 = primary role, 1 = fallback; half the perc C/D slots stay percussion (decided once per slot).
void Generator::enterSlot(int slot) {
  slot_ = (uint8_t)slot; attempt_ = 0; prepSteps_ = 0;
  percAlt_ = (uint8_t)((slot_ >= 6 && (rng_.next() & 1)) ? 1 : 0);
  state_ = PrepSlot;
}

const char* Generator::slotRole() const {
  if (attempt_) return kFallback[slot_ & 7];
  return percAlt_ ? "perc" : kRole[slot_ & 7];
}

void Generator::stepPrepSlot() {
  const char* role = slotRole();
  if (!role || lib_->prepareSample(genreName(), role, slot_ == 0 ? key_.root : -1) || ++prepSteps_ > kPrepareStepsMax) { prepared(SlotPick); return; }
  sd_ = true;
}

void Generator::stepSlotPick() {
  sd_ = true;
  const char* role = slotRole();
  const char* avoid = slot_ > 4 ? eng_->kit(set_).name[slot_ - 1] : nullptr;   // distinct perc files
  const int keyRoot = slot_ == 0 ? key_.root : -1;
  if (role && lib_->pickSample(genreName(), role, keyRoot, rng_, path_, sizeof path_, avoid)) { attempt_ = 0; state_ = SlotOpen; return; }
  if (attempt_ == 0 && kFallback[slot_ & 7]) { attempt_ = 1; prepSteps_ = 0; state_ = PrepSlot; return; }
  attempt_ = 0;
  setErr("no", kRole[slot_ & 7]);
  skipSlot();
}

void Generator::skipSlot() {
  if (slot_ + 1 < slotEnd_) enterSlot(slot_ + 1); else state_ = TrackDone;
}

void Generator::stepSlotOpen() {
  sd_ = true;
  file_ = fs_->open(path_);
  if (!file_) { setErr("open", baseName(path_)); skipSlot(); return; }
  if (!wavReadHeader(*file_, wav_) || !loader_.begin(*file_, wav_, eng_->staging(), slotCapSamples(slot_), 0.0f)) {
    file_->close(); file_ = nullptr; setErr("bad wav", baseName(path_)); skipSlot(); return;
  }
  state_ = SlotChunk;
}

void Generator::stepSlotChunk() {
  sd_ = true;
  if (loader_.step(2 * kLoaderChunkBytes)) return;           // 4 KB per step (SPEC §9.2)
  slotLen_ = loader_.written();
  file_->close(); file_ = nullptr;
  state_ = SlotPush;
  stepSlotPush();                                            // same step: the push costs no card work
}

// CmdSlotSwap (SPEC §9.1): a full queue keeps the state and retries; staging stays untouched until the ack.
void Generator::stepSlotPush() {
  ackTarget_ = (uint16_t)(eng_->status().slotAck + 1);
  if (!pushCmd(CmdSlotSwap, 0, set_, (int32_t)((uint32_t)slot_ | (slotLen_ << 4)), 0.0f)) { notePushFail(); return; }   // files are pre-balanced: 0 dB
  pushFail_ = 0;
  state_ = SlotWait;
}

void Generator::stepSlotWait() {
  if ((int16_t)(eng_->status().slotAck - ackTarget_) < 0) return;   // staging is in use until the engine copied it
  const char* b = baseName(path_);
  eng_->setKitName(set_, slot_, b);
  noteKitPath(set_, slot_, path_);                           // the session writer composes the kit paths from this (SPEC §9.19 §3 b)
  if (slot_ == 0 || slot_ == 4) copyStr(info_[track_].sound, sizeof info_[track_].sound, b);
  skipSlot();
}

void Generator::stepPrepVocal() {
  if (lib_->prepareVocal(genreName(), key_) || ++prepSteps_ > kPrepareStepsMax) { prepared(VocalPick); return; }
  sd_ = true;
}

// Primary phrase, then the alternate (SPEC §5 variant 2); names without a BPM token are re-picked up to
// kVocalPickAttempts times, one pick per step (SPEC §9.3 "others are skipped").
void Generator::stepVocalPick() {
  sd_ = true;
  VSet& v = stream_[set_];
  const bool ok = lib_->pickVocal(genreName(), key_, set_, rng_, path_, sizeof path_);
  const int bpm = ok ? parseBpmToken(baseName(path_)) : 0;
  const bool dup = phase_ == 1 && strcmp(path_, v.path) == 0;
  if (ok && bpm && !dup) {
    copyStr(phase_ == 0 ? v.path : altPath_[set_], kMaxPathLen, path_);
    if (phase_ == 0) phase_ = 1; else state_ = VocalOpen;
    return;
  }
  if (!ok) attempt_ = kVocalPickAttempts;                    // no folder at all: do not spend six steps
  if (++attempt_ < kVocalPickAttempts) return;
  attempt_ = 0;
  if (phase_ == 0) { setErr("vocal: no bpm", ok ? "" : genreName()); vocalEmpty(set_); state_ = TrackDone; return; }
  copyStr(altPath_[set_], kMaxPathLen, v.path);              // no alternate: variant 2 plays the same phrase
  state_ = VocalOpen;
}

// Opens the primary phrase. A file the streamer cannot play (not 16-bit mono/stereo, no frames) is re-picked like a name
// without a BPM token; lastError names it ("bad wav ..."). No free handle (SPEC §9.19g): the step waits, no card work; an
// open that failed anyway is tried again next step (openStream bounds that) before the file counts as bad.
void Generator::stepVocalOpen() {
  VSet& v = stream_[set_];
  if (!streamOpenAllowed() && ++openWait_ <= kOpenWaitSteps) return;   // a pending switch frees its handle at the next bar line
  sd_ = true;
  if (!loadVocal(set_, v.path, 0, openWait_ > kOpenWaitSteps)) {
    if (openErr_ == OpenFailed || openErr_ == OpenBusy) return;   // the same file again next step
    phase_ = 0;
    if (++attempt_ < kVocalPickAttempts) { state_ = VocalPick; return; }
    attempt_ = 0; vocalEmpty(set_); state_ = TrackDone; return;
  }
  openWait_ = 0; attempt_ = 0;
  if (setJob()) v.setJobWaiting = 1;                         // the pump pushes the set swaps with the vocal commands
  state_ = TrackDone;
}

// ---- loop job (SPEC §9.19): prepare → pick → open; the switch itself is the pump's ----

void Generator::stepPrepLoop() {
  if (!loopEnabled(track_)) { state_ = TrackDone; return; }
  if (lib_->prepareLoop(genreName(), trackCategory(track_), key_) || ++prepSteps_ > kPrepareStepsMax) { prepared(LoopPick); return; }
  sd_ = true;
}

void Generator::stepLoopPick() {
  sd_ = true;
  if (!loopEnabled(track_)) { state_ = TrackDone; return; }
  if (!lib_->pickLoop(genreName(), trackCategory(track_), key_, track_, rng_, path_, sizeof path_)) { loopFail(LoopNoContent); return; }
  state_ = LoopOpen;
}

// One file open. A file that cannot stream is re-picked up to kVocalPickAttempts times. No free handle (SPEC §9.19g): the
// step waits without card work; an open that failed anyway is tried again next step (openStream bounds that) before the
// file counts as unstreamable.
void Generator::stepLoopOpen() {
  if (!loopEnabled(track_)) { state_ = TrackDone; return; }
  if (!streamOpenAllowed() && ++openWait_ <= kOpenWaitSteps) return;   // a pending switch frees its handle at the next bar line
  sd_ = true;
  if (!openLoop(track_, path_, openWait_ > kOpenWaitSteps)) {
    if (openErr_ == OpenFailed || openErr_ == OpenBusy) return;   // the same file again next step
    if (++attempt_ < kVocalPickAttempts) { state_ = LoopPick; return; }
    attempt_ = 0; loopFail(LoopNoContent); return;
  }
  openWait_ = 0; attempt_ = 0; loopRefusal_ = LoopOk;
  if (setJob()) { const int s = trackStreamIdx(track_); if (s >= 0) stream_[s].setJobWaiting = 1; }   // the set swaps go with this stream's switch
  state_ = TrackDone;
}

// Nothing to pick (or nothing streamable): loop mode stays / goes off, lastError says "NO LOOP: <FOLDER>" (the UI's status).
void Generator::loopFail(LoopRefusal why) {
  loopRefusal_ = why;
  const Category c = trackCategory(track_);
  char f[16]; int i = 0; for (const char* p = loopFolderName(c); *p && i < 15; p++) f[i++] = upper(*p); f[i] = 0;
  snprintf(err_, sizeof err_, "NO LOOP: %s", f);
  if (loopEnabled(track_)) setLoopMode(track_, false);
  loopRefusal_ = why;                                        // setLoopMode(off) reports LoopOk; the refusal is what the UI asks for
  state_ = TrackDone;
}

void Generator::trackDone() {
  done_++; resetDone_ = 0;
  if (setJob()) { trackIdx_++; state_ = TrackBegin; return; }
  finishJob();
}

// Only loop picks are running or queued: another SHIFT + edit may queue behind them (SPEC §9.19 §1.6 BUSY is a generate / save / load).
bool Generator::onlyLoopJobs() const {
  if (state_ != Idle && job_ != JobLoop) return false;
  for (int i = 0; i < queueN_; i++) if (queue_[i].kind != JobLoop) return false;
  return true;
}

bool Generator::anyStreamWaiting(int set) const {
  for (int s = 0; s < kStreams; s++) if (streamSet(s) == set && stream_[s].setJobWaiting) return true;
  return false;
}
bool Generator::setSwapWaiting(int set) const { return set >= 0 && set < kSets && anyStreamWaiting(set); }

void Generator::stepFinish() {
  if (anyStreamWaiting(set_)) return;                        // a stream switch is scheduled: swaps go with it
  if ((swapMask_[set_] || ((maskDue_ >> set_) & 1)) && !pushSetSwaps(set_)) { notePushFail(); return; }   // queue full: retry
  pushFail_ = 0;
  state_ = WaitSwap;
}

// All eight swaps, the Beat's layer mask (with its rush loop) and CmdSwapSet together (SPEC §9.5 one set-wide
// swap, §9.6 layers switch with the pattern). Pushed only when the queue has room for the whole burst, so a
// set is never half switched. The length is natural × 2^lenStep (0 = natural): the generator's own jobs reset
// lenStep, a set loader restores it.
bool Generator::pushSetSwaps(int set) {
  const uint8_t bit = (uint8_t)(1u << set); const int beat = set * kTracksPerSet + Beat;
  const bool mask = ((swapMask_[set] >> Beat) & 1) || ((maskDue_ >> set) & 1);
  if (!cmdRoom(popcount8(swapMask_[set]) + 1 + (mask ? 1 : 0))) return false;
  if ((maskDue_ >> set) & 1) { if (!writeRush(set)) rushDue_ |= bit; }   // rush = crop(new Full, 1 beat), SPEC §9.6
  for (int i = 0; i < kTracksPerSet; i++) {
    if (!((swapMask_[set] >> i) & 1)) continue;
    const int t = set * kTracksPerSet + i; const TrackGenInfo& inf = info_[t];
    const uint32_t nat = inf.naturalTicks ? inf.naturalTicks : kTicksPerBar;
    pushCmd(CmdSwapPattern, t, inf.variant, inf.lenStep ? (int32_t)clampLen(lenOf(nat, inf.lenStep)) : 0);
  }
  if (mask) pushCmd(CmdBeatMask, 0, set, maskCmd(info_[beat].variant));   // lands with the swap (SPEC §9.6)
  swapMask_[set] = 0; maskDue_ &= (uint8_t)~bit;
  pushCmd(CmdSwapSet, 0, set);
  pushClock_ = eng_->status().sampleClock;
  return true;
}

// Loader entry to the same burst (sets.cpp): queued behind a scheduled stream switch of the set (the vocal's, or a
// loop track's) while the transport runs so the switch commands and the other swaps go out together; pushed now
// otherwise (a stopped transport never pumps, the swaps then apply on resume like the generator's own). A full
// queue defers the whole burst to a later step().
bool Generator::requestSetSwap(int set, uint8_t mask) {
  if (!eng_ || set < 0 || set >= kSets) return false;
  swapMask_[set] |= mask;
  bool wait = false;
  if (mask && eng_->status().running) for (int s = 0; s < kStreams; s++) if (streamSet(s) == set && stream_[s].nxtValid) { stream_[s].setJobWaiting = 1; wait = true; }
  if (wait) return false;
  for (int s = 0; s < kStreams; s++) if (streamSet(s) == set) stream_[s].setJobWaiting = 0;
  if (!pushSetSwaps(set)) { swapDue_ |= (uint8_t)(1u << set); notePushFail(); return false; }
  return true;
}

void Generator::setVocalPath(int set, int sel, const char* path) {
  if (set < 0 || set >= kSets || !path) return;
  copyStr((sel & 1) ? altPath_[set] : stream_[set].path, kMaxPathLen, path);
}

void Generator::stepWaitSwap() {
  const EngineStatus& st = eng_->status();
  if (st.running) {
    if ((int32_t)(st.sampleClock - pushClock_) < 2 * kBlock) return;   // commands not yet seen by the engine
    for (int i = 0; i < kTracksPerSet; i++) if (st.track[set_ * kTracksPerSet + i].pendingSwap) return;
  }                                                          // transport stopped: the swaps apply on resume
  if (!pushCmd(CmdSetGenerating, 0, set_, 0)) { notePushFail(); return; }
  finishJob();
}

void Generator::finishJob() {
  if (job_ == JobTheory) { theorySet_ |= (uint8_t)(1u << set_); theoryProg_[set_] = (int8_t)theory_.prog; }   // SPEC §9.24: the set is a theory set from the swap on
  state_ = Idle; job_ = JobNone; changes_++;
}

// ---------------------------------------------------------------------------------------------- loop mode (SPEC §9.19)

bool Generator::loopEnabled(int track) const { return track >= 0 && track < kTracks && ((loopOn_ >> track) & 1) != 0; }
bool Generator::anyLoop() const { return loopOn_ != 0; }

// The stream a track in loop mode streams: the Vocal track its set's own, any other track the slot bound to it
// (a slot that is closing counts as free: its old track is leaving).
int Generator::trackStreamIdx(int track) const {
  if (track < 0 || track >= kTracks || !loopEnabled(track)) return -1;
  if (trackCategory(track) == Vocal) return trackSet(track);
  for (int k = 0; k < kLoopSlots; k++) if (slotTrack_[k] == track && !stream_[kSets + k].closing) return kSets + k;
  return -1;
}
int Generator::streamTrack(int s) const {
  if (s < 0 || s >= kStreams) return -1;
  if (s < kSets) return s * kTracksPerSet + Vocal;
  return slotTrack_[s - kSets];
}
int Generator::loopSlot(int track) const {
  const int s = trackStreamIdx(track);
  return s >= kSets ? s - kSets : -1;
}
const char* Generator::loopPath(int track) const {
  if (!loopEnabled(track)) return "";
  if (trackCategory(track) == Vocal) return vocalLoopPath_[trackSet(track)];
  const int s = trackStreamIdx(track);
  return s >= kSets ? stream_[s].path : "";
}
const char* Generator::loopFile(int track) const { return baseName(loopPath(track)); }
uint8_t Generator::loopBars(int track) const {
  const int s = trackStreamIdx(track);
  if (s < 0) return 0;
  const VSet& v = stream_[s];
  const VStream* st = v.nxtValid ? &v.nxt : (v.curValid ? &v.cur : nullptr);
  return st && st->mode ? st->bars : 0;
}

// SHIFT + edit (SPEC §9.19 §1.1). On: the refusals in order (no card / library, a job running or queued, no free
// slot (the Vocal needs none), low heap, a library that already knows the category has no loops: a pack without a
// loops/ folder (noted at boot) or a counted category of 0 files — never any card work here, SPEC §9.2: a cold category
// is found out by the job, which fails with lastError "NO LOOP: <FOLDER>"), then the slot is bound, the pattern's variant /
// len parked and JobLoop queued. Off: the parked clip comes back — a slot track pushes CmdLoopMode off now (the engine
// restores length and variant at the bar, the stream ends once the engine passed it), the Vocal track re-opens its
// parked phrase (one file open), which returns with the stream switch together with CmdLoopMode off; when its loop never
// arrived (the pick failed, or is still only pending) the phrase is still streaming and nothing is re-opened. No pattern,
// patch or next buffer is touched either way.
bool Generator::setLoopMode(int track, bool on) {
  if (track < 0 || track >= kTracks) { loopRefusal_ = LoopNoCard; return false; }
  const uint16_t bit = (uint16_t)(1u << track); const Category c = trackCategory(track); const int set = trackSet(track);
  TrackGenInfo& inf = info_[track]; LoopTrack& lt = loopTk_[track];
  if (!on) {
    if (!(loopOn_ & bit)) { loopRefusal_ = LoopOk; return true; }
    loopOn_ &= (uint16_t)~bit; loopRush_ &= (uint16_t)~bit;
    queueDrop(JobLoop, track);
    inf.variant = lt.parkedVariant; inf.lenStep = lt.parkedLenStep; inf.naturalTicks = lt.parkedNatural;
    if (c == Vocal) {
      VSet& v = stream_[set];
      vocalLoopPath_[set][0] = 0;
      const bool loopCur = v.curValid && v.cur.mode == 1, loopNxt = v.nxtValid && v.nxt.mode == 1;
      if (!loopCur && !loopNxt) {                            // the loop never arrived: the phrase (or silence) still streams, mode off at once, no re-open
        v.modeDue = 0;
        if (!pushCmd(CmdLoopMode, track, 0, set)) modeOffDue_ |= bit;
      } else vocalReturn(set);                                // the phrase comes back, CmdLoopMode off with its switch (deferred while no handle is free, SPEC §9.19g)
    } else {
      if (!pushCmd(CmdLoopMode, track, 0, 0)) modeOffDue_ |= bit;
      releaseSlot(track);
    }
    loopRefusal_ = LoopOk; changes_++;
    return true;
  }
  if (loopOn_ & bit) { loopRefusal_ = LoopOk; return true; }
  if (!fs_ || !lib_ || !eng_ || !genreName()[0]) { loopRefusal_ = LoopNoCard; return false; }
  if (busy() && !onlyLoopJobs()) { loopRefusal_ = LoopBusy; return false; }   // a generate owns the card; another track's loop pick in flight does not (the toggles queue)
  int s = set;
  if (c != Vocal) {
    s = -1;
    bool anyRing = false;
    for (int k = 0; k < kLoopSlots && s < 0; k++) {
      if (eng_->loopRing(k).cap <= 0 || !eng_->loopRing(k).buf) continue;   // no ring memory handed in (setLoopRing): unusable
      anyRing = true;
      if (slotTrack_[k] < 0 || stream_[kSets + k].closing) s = kSets + k;
    }
    if (s < 0) { loopRefusal_ = anyRing ? LoopSlotsFull : LoopLowRam; return false; }
  }
  if (freeHeap_ && freeHeap_() < (uint32_t)kLoopLowRamBytes) { loopRefusal_ = LoopLowRam; return false; }
  if (!lib_->genreHasLoops(genre_) || lib_->loopTotal(genreName(), c, key_) == 0) {   // the library knows without card work: nothing to pick (a cold category finds out in the job)
    char f[16]; int i = 0; for (const char* p = loopFolderName(c); *p && i < 15; p++) f[i++] = upper(*p); f[i] = 0;
    snprintf(err_, sizeof err_, "NO LOOP: %s", f);
    loopRefusal_ = LoopNoContent; return false;
  }
  if (c != Vocal) { slotTrack_[s - kSets] = (int8_t)track; stream_[s].closing = 0; stream_[s].path[0] = 0; }
  else { vocalLoopPath_[set][0] = 0; vocalReturnDue_ &= (uint8_t)~(1u << set); }   // a return still waiting for a handle is void: the loop stays
  stream_[s].modeDue = 1;
  lt.parkedVariant = inf.variant; lt.parkedLenStep = inf.lenStep; lt.parkedNatural = inf.naturalTicks;
  inf.variant = 0; inf.lenStep = 0; inf.naturalTicks = kTicksPerBar;   // the loop starts whole; its natural follows the pick
  loopOn_ |= bit; loopRefusal_ = LoopOk; err_[0] = 0;
  queuePush(JobLoop, (uint8_t)track);
  changes_++;
  return true;
}

// The slot bound to `track` is released: a stream with content keeps playing for the track until the engine passed the
// next bar (its CmdLoopMode off lands there), then ends; one without content ends now.
void Generator::releaseSlot(int track) {
  for (int k = 0; k < kLoopSlots; k++) {
    if (slotTrack_[k] != track) continue;
    VSet& v = stream_[kSets + k];
    v.modeDue = 0; v.setJobWaiting = 0;
    if (!v.curValid || !eng_->status().running) { streamEmpty(kSets + k); slotTrack_[k] = -1; v.closing = 0; continue; }
    if (v.nxtValid) { closeStreamFile(v.nxt, &v.cur); v.nxtValid = 0; v.switchState = 0; v.rewriteFrom = v.rewriteTo = 0; }   // a pending pick / switch is void
    uint32_t idx = clk_.idx0 + 1;
    if ((int32_t)(barLineOf(idx) - (eng_->status().sampleClock + kSwitchMarginBlocks * kBlock)) < 0) idx++;
    v.closing = 1; v.closeTick = idx * kTicksPerBar;
  }
}

// Restores loop mode with one exact file (sets.cpp, SPEC §9.19 §1.7): the slot is bound here, the pick skipped.
bool Generator::loadLoop(int track, const char* path, int variant, int lenStep) {
  openErr_ = OpenBad;                                        // until openLoop says otherwise (lastOpenBusy() for Sets, SPEC §9.19g)
  if (!fs_ || !eng_ || !lib_ || track < 0 || track >= kTracks || !path || !path[0]) return false;
  const uint16_t bit = (uint16_t)(1u << track); const Category c = trackCategory(track); const int set = trackSet(track);
  TrackGenInfo& inf = info_[track]; LoopTrack& lt = loopTk_[track];
  const bool wasOn = (loopOn_ & bit) != 0;
  int s = c == Vocal ? set : trackStreamIdx(track);
  if (s < 0) {
    for (int k = 0; k < kLoopSlots && s < 0; k++) {
      if (eng_->loopRing(k).cap <= 0 || !eng_->loopRing(k).buf) continue;
      if (slotTrack_[k] < 0 || stream_[kSets + k].closing) s = kSets + k;
    }
    if (s < 0) { loopRefusal_ = LoopSlotsFull; return false; }
  }
  if (!wasOn) {
    lt.parkedVariant = inf.variant; lt.parkedLenStep = inf.lenStep; lt.parkedNatural = inf.naturalTicks;
    if (c != Vocal) { slotTrack_[s - kSets] = (int8_t)track; stream_[s].closing = 0; }
    stream_[s].modeDue = 1;
    loopOn_ |= bit;
  }
  loopRush_ &= (uint16_t)~bit;                               // a load means the stored crop / len, not a rush in progress
  inf.variant = (uint8_t)(variant & 3); inf.lenStep = (int8_t)lenStep;
  if (!openLoop(track, path)) {
    if (!wasOn) {
      loopOn_ &= (uint16_t)~bit; stream_[s].modeDue = 0;
      inf.variant = lt.parkedVariant; inf.lenStep = lt.parkedLenStep; inf.naturalTicks = lt.parkedNatural;
      if (c != Vocal && !stream_[s].curValid) slotTrack_[s - kSets] = -1;
    }
    return false;
  }
  queueDrop(JobLoop, track);                                 // a pending pick for this track is void
  loopRefusal_ = LoopOk; changes_++;
  return true;
}

// The loop file into the track's stream as `nxt` (one open; the pump switches at the next bar). openErr_ says why not:
// no handle free (OpenBusy, SPEC §9.19g), the open failed (OpenFailed: again next step), unstreamable (OpenBad).
bool Generator::openLoop(int track, const char* path, bool force) {
  const int s = trackStreamIdx(track);
  if (s < 0) { openErr_ = OpenBad; return false; }
  VStream ns;
  if (!openStream(ns, s, path, 0, true, force)) return false;
  setNext(s, ns);
  VSet& v = stream_[s];
  if (s < kSets) { copyStr(vocalLoopPath_[s], kMaxPathLen, path); vocalReturnDue_ &= (uint8_t)~(1u << s); }
  else copyStr(v.path, kMaxPathLen, path);
  loopFields(track, v.nxt);
  variantDue_ &= (uint16_t)~(1u << track);
  return true;
}

// nxt's crop and cycle from the track's info (variant, lenStep): natural = the crop, len = natural × 2^k. While the
// track is rushing (rushLoop) the stream carries a crop of HALF the clip length set (len / 2, at least 1 beat, never past
// the file) looped every half instead (SPEC §9.19 §1.4 as amended by §9.22) — info() keeps the stored crop and len step,
// which come back when the rush ends.
void Generator::loopFields(int track, VStream& ns) {
  TrackGenInfo& inf = info_[track];
  ns.variant = (uint8_t)(inf.variant & 3);
  const uint16_t region = loopRegionOf(ns.bars, ns.variant);
  const int k = clampLoopStep(region, inf.lenStep);
  inf.lenStep = (int8_t)k;
  inf.naturalTicks = region; inf.loaded = 1;
  const uint16_t len = loopLenOf(region, k);
  if ((loopRush_ >> track) & 1) {                            // the rush crop: half of the clip length in force, momentary
    uint16_t half = (uint16_t)(len / 2);
    const uint16_t whole = loopRegionOf(ns.bars, 0);
    if (half < (uint16_t)kTicksPerBeat) half = (uint16_t)kTicksPerBeat;
    if (half > whole) half = whole;                          // a len± beyond the file streams silence there: the rush loops the file instead
    ns.regionTicks = ns.naturalTicks = ns.loopTicks = half;
    return;
  }
  ns.regionTicks = ns.naturalTicks = region;
  ns.loopTicks = len;
}

// Snare rush on a loop track (SPEC §9.19 §1.4 / §9.22, today's rush semantics on a stream): the same file as nxt with half
// the clip length as its crop and cycle, switched on the next 1/16; the release switches back to the stored crop / len the
// same way. Nothing in info() moves, nothing is persisted (changes_ untouched); while the pick is still in flight the flag
// alone is kept and openLoop applies it.
void Generator::rushLoop(int track, bool on) {
  if (track < 0 || track >= kTracks) return;
  const uint16_t bit = (uint16_t)(1u << track);
  if (!loopEnabled(track)) { loopRush_ &= (uint16_t)~bit; return; }
  if (on == (((loopRush_ >> track) & 1) != 0)) return;
  if (on) loopRush_ |= bit; else loopRush_ &= (uint16_t)~bit;
  loopLen(track, 0);
}

// A slot-bound track plays (or is queued to) while the transport runs: its 139 ms ring is consumed and no card operation
// of 150 ms can be hidden behind it, so Sets keeps the session write away (SPEC §9.19 §3). A slot the engine still renders
// after the generator released it (closing, until the bar) counts too.
bool Generator::loopSlotStreaming() const {
  if (!eng_) return false;
  const EngineStatus& es = eng_->status();
  if (!es.running) return false;
  for (int k = 0; k < kLoopSlots; k++) {
    int t = slotTrack_[k];
    if (t < 0) t = eng_->loopTrack(k);
    if (t < 0 || t >= kTracks) continue;
    const PlayState ps = es.track[t].state;
    if (ps == PSPlaying || ps == PSStopQueued || ps == PSQueued) return true;
  }
  return false;
}

// Crop / len change on a streaming loop (SPEC §9.19 §1.4): the same file as `nxt` with the new cycle, switched on the
// next 1/16 (rush semantics); PubNone while nothing streams yet (the pick applies the track's info when it opens).
Generator::PubResult Generator::loopVariant(int track, int variant) {
  TrackGenInfo& inf = info_[track];
  inf.variant = (uint8_t)(variant & 3); inf.lenStep = 0;    // k reset on variant select (SPEC §9.4)
  const int s = trackStreamIdx(track);
  if (s < 0) return PubNone;
  VSet& v = stream_[s];
  const VStream* basis = v.nxtValid ? &v.nxt : (v.curValid ? &v.cur : nullptr);
  if (!basis || basis->mode != 1) return PubNone;
  if (basis != &v.nxt) setNext(s, *basis, kSixteenth);       // same file (handle shared with cur), new cycle
  loopFields(track, v.nxt);
  v.switchState = 0; v.rewriteFrom = v.rewriteTo = 0;        // (re)schedule at the next pump
  return PubOk;
}

Generator::PubResult Generator::loopLen(int track, uint16_t len) {
  (void)len;                                                 // the cycle follows info().lenStep in loopFields
  const int s = trackStreamIdx(track);
  if (s < 0) return PubNone;
  VSet& v = stream_[s];
  const VStream* basis = v.nxtValid ? &v.nxt : (v.curValid ? &v.cur : nullptr);
  if (!basis || basis->mode != 1) return PubNone;
  if (basis != &v.nxt) setNext(s, *basis, kSixteenth);
  loopFields(track, v.nxt);
  v.switchState = 0; v.rewriteFrom = v.rewriteTo = 0;
  return PubOk;
}

// The engine reads this ring now: the transport runs, the bound track plays and the ring is active.
bool Generator::streamConsumed(int s) const {
  const VSet& v = stream_[s];
  if (!v.curValid) return false;
  const EngineStatus& es = eng_->status();
  if (!es.running) return false;
  const int t = streamTrack(s);
  if (t < 0) return false;
  const PlayState ps = es.track[t].state;
  if (!(ps == PSPlaying || ps == PSStopQueued)) return false;
  const VocalRing& ring = eng_->streamRing(s);
  return ring.cap > 0 && ring.active.load(std::memory_order_acquire);
}

// SPEC §9.19 §3 a: every consumed ring that can hold kGateLeadSamples holds that much lead (the vocal rings: a card
// operation of up to kGateLeadMs then starves nothing); a smaller ring (a loop slot, 139 ms) is at least half full —
// the contract's wording — since no lead of its own could cover such an operation and asking for "nearly full" from
// three or four rings at once can never be met when every read drains the others. A refusal asks the next pump to top
// every ring up, a chunk at a time, emptiest first.
bool Generator::streamsHealthy() const {
  if (!eng_) return true;
  bool ok = true;
  for (int s = 0; s < kStreams && ok; s++) {
    if (!streamConsumed(s)) continue;
    const VocalRing& ring = eng_->streamRing(s);
    const int need = ring.cap >= kGateLeadSamples + kBlock ? kGateLeadSamples : ring.cap / 2;   // a ring that can hold the lead plus the block being rendered uses it (the 4096-sample vocal rings); a smaller one (a loop slot) is half full
    if (ring.available() < need) ok = false;
  }
  if (!ok) gateWait_ = 1;
  return ok;
}

// ---------------------------------------------------------------------------------------------- streams

// SPEC §9.19g: a new-file stream open takes a handle only while another stays free for the job's own kit / index / SMF file
// (or a Sets file). fs openCount() is what the pool holds now: the streams' files, a file in flight, Sets' files; a file
// system that cannot count answers 0 and nothing is ever refused.
bool Generator::streamOpenAllowed() const {
  return !fs_ || fs_->openCount() + 1 < kMaxOpenFiles;
}

// One file for stream s: open, header, head read. Refused without card work while no handle is free (OpenBusy, unless
// forced); an open that fails is OpenFailed for the caller to try again next step, OpenBad after kOpenRetrySteps failures
// in a row on this stream or when the file cannot stream (only what wavReadMono streams is accepted: 16-bit, 1 or 2
// channels, io/wav.h — anything else would load "successfully" and play silence). A loop (SPEC §9.19) takes its bars from
// the '<n>bars' token (else from its length at the file's tempo) and its tempo from the token, else kLoopBpm.
bool Generator::openStream(VStream& st, int s, const char* path, int sel, bool loop, bool force) {
  st.f = nullptr;
  if (!force && !streamOpenAllowed()) { openErr_ = OpenBusy; return false; }
  IFile* f = fs_->open(path);
  if (!f) {
    setErr(loop ? "loop open" : "vocal open", baseName(path));
    const bool again = s >= 0 && s < kStreams && ++openFails_[s] < kOpenRetrySteps;
    if (!again && s >= 0 && s < kStreams) openFails_[s] = 0;
    openErr_ = again ? OpenFailed : OpenBad;
    return false;
  }
  if (s >= 0 && s < kStreams) openFails_[s] = 0;
  openErr_ = OpenBad;
  if (!wavReadHeader(*f, st.wi) || st.wi.bits != 16 || (st.wi.channels != 1 && st.wi.channels != 2) || !st.wi.sampleRate) {
    f->close(); setErr("bad wav", baseName(path)); return false;
  }
  st.frames = st.wi.dataBytes / (st.wi.channels * 2u);
  st.bpm = (float)parseBpmToken(baseName(path));
  if (loop && !(st.bpm > 0)) st.bpm = (float)kLoopBpm;
  if (!(st.bpm > 0)) { f->close(); setErr("vocal: no bpm", baseName(path)); return false; }
  if (!st.frames) { f->close(); setErr("empty wav", baseName(path)); return false; }
  uint32_t n = wavReadMono(*f, st.wi, st.head, kHeadSamples);   // header leaves the position at the data
  for (; n < (uint32_t)kHeadSamples; n++) st.head[n] = 0;
  st.natural = naturalBeats(st.frames, st.wi.sampleRate, st.bpm);
  st.sel = (uint8_t)(sel & 1); st.variant = 0;
  st.windowBars = clampWindow(parseBarsToken(baseName(path)));   // SPEC §9.16 amendment: a '<n>bars' rap file plays whole
  st.naturalTicks = (uint16_t)(st.natural * kTicksPerBeat);
  st.loopTicks = vocalClipTicks(0, kVocalPauseDefault, st.windowBars);   // the track's own pause is applied by loadVocal / vocalVariant
  st.mode = 0; st.bars = 0; st.regionTicks = st.naturalTicks;
  if (loop) {
    int bars = parseBarsToken(baseName(path));
    if (bars <= 0) bars = (int)((float)st.frames / (float)st.wi.sampleRate * st.bpm / 60.0f / (float)kBeatsPerBar + 0.5f);
    if (bars < 1) bars = 1;
    if (bars > kMaxLoopBars) bars = kMaxLoopBars;
    st.mode = 1; st.bars = (uint8_t)bars;
    st.regionTicks = st.naturalTicks = st.loopTicks = (uint16_t)(bars * kTicksPerBar);
  }
  st.f = f; openErr_ = OpenOk;
  return true;
}

void Generator::closeStreamFile(VStream& st, const VStream* keepIfShared) {
  if (st.f && !(keepIfShared && keepIfShared->f == st.f)) st.f->close();
  st.f = nullptr;
  if (winStream_ == &st) winStream_ = nullptr;
}

void Generator::setNext(int s, const VStream& ns, uint16_t grid) {
  VSet& v = stream_[s];
  if (v.nxtValid) closeStreamFile(v.nxt, &v.cur);            // a switch not yet taken is replaced
  if (winStream_ == &v.nxt) winStream_ = nullptr;
  v.nxt = ns; v.nxtValid = 1; v.switchState = 0; v.rewriteFrom = v.rewriteTo = 0; v.switchGrid = grid ? grid : (uint16_t)kTicksPerBar;
  v.nxt.offFp = 0; v.nxt.offCycle = 0;                       // a new cycle starts without a carried file position
}

// Opens `path` as the set's next phrase (one file: open, header, head read). The producer switches at a bar. A Vocal
// track in loop mode leaves it with this phrase (SPEC §9.19: a set load brings a phrase; CmdLoopMode off rides along).
bool Generator::loadVocal(int set, const char* path, int sel, bool force) {
  openErr_ = OpenBad;
  if (!fs_ || !eng_ || set < 0 || set >= kSets || !path) return false;
  VStream ns;
  if (!openStream(ns, set, path, sel, false, force)) return false;   // lastError set by openStream; openErr_ / lastOpenBusy() say why
  setNext(set, ns);
  VSet& v = stream_[set];
  copyStr((sel & 1) ? altPath_[set] : v.path, kMaxPathLen, path);
  const int t = set * kTracksPerSet + Vocal;
  TrackGenInfo& inf = info_[t];
  const bool returning = ((vocalReturnDue_ >> set) & 1) != 0;   // a deferred return (SPEC §9.19g): the loop still streams, this phrase ends it
  if (loopEnabled(t) || returning) {                         // back to phrases: the loop's variant / len step out of the way, the mode off with the switch
    loopOn_ &= (uint16_t)~(1u << t); vocalLoopPath_[set][0] = 0; v.modeDue = 2; queueDrop(JobLoop, t);
    inf.lenStep = loopTk_[t].parkedLenStep; vocalReturnDue_ &= (uint8_t)~(1u << set);
  }
  copyStr(inf.file, sizeof inf.file, baseName(path));
  snprintf(inf.sound, sizeof inf.sound, "%dbpm", (int)ns.bpm);
  inf.fileBpm = ns.bpm; inf.variant = (uint8_t)(sel ? 1 : 0); inf.naturalTicks = ns.naturalTicks; inf.sourceTicks = ns.naturalTicks;
  inf.lenStep = 0; inf.loaded = 1; inf.keyRoot = key_.root; inf.windowBars = ns.windowBars;
  inf.pause = fitPause(inf.pause, ns.windowBars);            // a pause the engine's cap would swallow under this window steps down (what the page shows plays)
  if (v.nxtValid) { v.nxt.variant = inf.variant; v.nxt.loopTicks = vocalClipTicks(inf.variant, inf.pause, ns.windowBars); }   // the track's cycle (SPEC §9.16)
  variantDue_ &= (uint16_t)~(1u << t);                       // a press queued before this phrase existed is void
  return true;
}

// Variant / cycle change for the vocal (SPEC §5: full, alternate file, 2 beats, 1 beat; SPEC §9.16 window + pause
// for the first two, the track's pause). Opens the other file when the selection changes; a pause change comes
// here with the same variant. PubNone when no phrase streams (nothing to retry); the engine never blocks a vocal publish.
Generator::PubResult Generator::vocalVariant(int set, int variant) {
  VSet& v = stream_[set];
  TrackGenInfo& inf = info_[set * kTracksPerSet + Vocal];
  const VStream* basis = v.nxtValid ? &v.nxt : (v.curValid ? &v.cur : nullptr);
  if (!basis || basis->mode) return PubNone;                 // nothing streams, or the stream is a loop (SPEC §9.19: its variants are the crops)
  const int sel = variant == 1 ? 1 : (variant == 0 ? 0 : basis->sel);
  const char* other = sel ? altPath_[set] : v.path;
  if (sel != basis->sel && other[0]) {
    if (!loadVocal(set, other, sel)) return (openErr_ == OpenBusy || openErr_ == OpenFailed) ? PubBusy : PubNone;   // SPEC §9.19g: no handle now / the open failed: step() retries the press
  } else if (basis != &v.nxt) {
    setNext(set, *basis);                                    // same file (handle shared with cur), new cycle
  }
  VStream& ns = v.nxt;
  ns.variant = (uint8_t)variant;
  ns.naturalTicks = (uint16_t)(variant == 2 ? 2 * kTicksPerBeat : variant == 3 ? kTicksPerBeat : ns.natural * kTicksPerBeat);
  ns.loopTicks = vocalClipTicks(variant, inf.pause, ns.windowBars);
  inf.lenStep = 0;
  inf.variant = (uint8_t)variant; inf.naturalTicks = ns.naturalTicks; inf.fileBpm = ns.bpm; inf.windowBars = ns.windowBars;
  copyStr(inf.file, sizeof inf.file, baseName(ns.sel ? altPath_[set] : v.path));
  v.switchState = 0; v.rewriteFrom = v.rewriteTo = 0;        // (re)schedule at the next pump
  return PubOk;
}

// The phrase a Vocal track returns to when its loop mode ends (SPEC §9.19 §1.1): one new-file open, CmdLoopMode off riding
// with its switch, the parked loop variant of the phrase restored. No handle free now (SPEC §9.19g) or the open failed: the
// loop keeps streaming and step() calls again (vocalReturnDue_), the handle frees at the next bar line. A phrase that stays
// unopenable (kOpenRetrySteps failed opens) or none at all: silence, mode off at once, as before.
bool Generator::vocalReturn(int set) {
  VSet& v = stream_[set]; const int track = set * kTracksPerSet + Vocal; TrackGenInfo& inf = info_[track];
  const uint8_t bit = (uint8_t)(1u << set);
  const int sel = inf.variant == 1 ? 1 : 0;
  const uint8_t parked = inf.variant;
  const char* phrase = sel ? altPath_[set] : v.path;
  if (phrase[0] && loadVocal(set, phrase, sel)) {           // loadVocal clears the due bit and sets modeDue 2
    if (parked >= 2) vocalVariant(set, parked);              // the loop variants of the phrase come back too (same file: no open)
    v.modeDue = 2;
    return true;
  }
  if (phrase[0] && (openErr_ == OpenBusy || openErr_ == OpenFailed)) { vocalReturnDue_ |= bit; return false; }
  vocalReturnDue_ &= (uint8_t)~bit;
  streamEmpty(set);                                          // no phrase to return to: silence, mode off at once
  if (!pushCmd(CmdLoopMode, track, 0, set)) modeOffDue_ |= (uint16_t)(1u << track);
  return false;
}

// step(): one deferred return whose handle is free now; true when an open was attempted (the step's card operation).
bool Generator::retryVocalReturn() {
  for (int s = 0; s < kSets; s++) {
    if (!((vocalReturnDue_ >> s) & 1)) continue;
    if (!streamOpenAllowed()) continue;                      // still no handle: nothing to try, no card work
    vocalReturn(s);
    return true;
  }
  return false;
}

// No content for this stream: files closed, ring parked, present = 0 published (SPEC §9.3 "others are skipped";
// a released loop slot, SPEC §9.19).
void Generator::streamEmpty(int s) {
  if (s < 0 || s >= kStreams) return;
  VSet& v = stream_[s];
  eng_->streamRing(s).active.store(0, std::memory_order_release);
  closeStreamFile(v.nxt, &v.cur); closeStreamFile(v.cur, nullptr);
  v.curValid = v.nxtValid = 0; v.switchState = 0; v.armed = 0; v.handshake = 0; v.rewriteFrom = v.rewriteTo = 0; v.setJobWaiting = 0;
  v.playing = 0; v.closing = 0; v.modeDue = 0; v.switchGrid = kTicksPerBar;
  if (s < kSets) {
    VocalState& vs = eng_->nextVocal(s); vs = VocalState(); vs.present = 0;
    if (!pushCmd(CmdVocalLoaded, 0, s)) notePushFail();
    const int t = s * kTracksPerSet + Vocal;
    if ((vocalReturnDue_ >> s) & 1) {                        // SPEC §9.19g: the loop it was to leave is gone with the stream: mode off at once
      vocalReturnDue_ &= (uint8_t)~(1u << s);
      if (!pushCmd(CmdLoopMode, t, 0, s)) modeOffDue_ |= (uint16_t)(1u << t);
    }
    TrackGenInfo& inf = info_[t];
    inf.file[0] = 0; inf.sound[0] = 0; inf.fileBpm = 0; inf.loaded = 0;
    variantDue_ &= (uint16_t)~(1u << t);
  } else {
    const int k = s - kSets;
    VocalState& ls = eng_->nextLoop(k); ls = VocalState(); ls.present = 0;
    pushCmd(CmdLoopLoaded, 0, k);                            // room permitting: an unbound slot renders nothing anyway
  }
}

// ---------------------------------------------------------------------------------------------- stream producer

// Standalone pump (the loop calls it after step(), which already pumped): continues the block's budget (pump
// chunks + one for the step's card operation) so one loop iteration never reads more than ≈ kPumpChunks × 4 KB
// plus the operation; a new block starts a new budget.
void Generator::pumpVocal() {
  if (!eng_) return;
  if (eng_->status().sampleClock != pumpClock_) iterChunks_ = 0;
  if (iterChunks_ < kPumpChunks) iterChunks_ = (uint8_t)(iterChunks_ + pump(kPumpChunks - iterChunks_));
}

// Every live stream, emptiest ring first (SPEC §9.19: a loop ring holds 139 ms, a vocal ring 186 ms; the one closest
// to running dry gets the budget first), with the turn rotating among equals. A ring that already ran dry comes last:
// it is starving whatever we do for the next read, while a ring with a little lead left can still be saved by it
// (every read on a slow card drains every ring meanwhile).
int Generator::pump(int budget) {
  const int start = budget;
  const EngineStatus& es = eng_->status();                   // the bar clock everything written in this pump is placed by
  clk_.bpm = bpmOr(es.bpm); clk_.barLen = ticksToSamples(kTicksPerBar, clk_.bpm); if (!clk_.barLen) clk_.barLen = 1;
  clk_.idx0 = es.tick / kTicksPerBar; clk_.bar0 = es.barSample; clk_.bar1 = es.nextBarSample; clk_.tick0 = es.tick;
  if ((int32_t)(clk_.bar1 - clk_.bar0) <= 0) clk_.bar1 = clk_.bar0 + clk_.barLen;
  const bool topUp = gateWait_ != 0; gateWait_ = 0;
  int order[kStreams], lead[kStreams], n = 0;
  for (int i = 0; i < kStreams; i++) {
    const int s = (i + pumpTurn_) % kStreams;
    const VSet& v = stream_[s];
    if (!(v.curValid || v.nxtValid || v.handshake || v.closing)) continue;
    const VocalRing& ring = eng_->streamRing(s);
    if (ring.cap <= 0 || !ring.buf) continue;                // no memory for this ring (desktop without setLoopRing): nothing to serve
    int l = ring.active.load(std::memory_order_acquire) ? ring.available() : ring.cap;
    if (l <= 0) l = 1 << 30;                                // already starving: served after the rings that can still be saved
    int j = n++;
    while (j > 0 && lead[j - 1] > l) { order[j] = order[j - 1]; lead[j] = lead[j - 1]; j--; }
    order[j] = s; lead[j] = l;
  }
  pumpTurn_ = (uint8_t)((pumpTurn_ + 1) % kStreams);
  bool live[kStreams] = {false, false, false, false};
  for (int i = 0; i < n; i++) {
    const int s = order[i];
    VSet& v = stream_[s];
    if (v.closing && (!es.running || (int32_t)(clk_.tick0 - v.closeTick) >= 0)) {   // the engine passed the bar its CmdLoopMode off landed on
      streamEmpty(s);
      if (s >= kSets) slotTrack_[s - kSets] = -1;
      continue;
    }
    pumpStream(s, budget, false);
    live[s] = v.curValid || v.nxtValid;
  }
  // Top-up after a refused streamsHealthy(): the rings are brought to the top in rounds of ONE chunk read each,
  // emptiest first, so no ring waits through another ring's two reads on a slow card (every read drains every ring).
  if (topUp) {
    bool done[kStreams] = {false, false, false, false};
    while (budget > 0) {
      int s = -1, lead = 0;
      for (int i = 0; i < kStreams; i++) {
        if (!live[i] || done[i]) continue;
        VocalRing& ring = eng_->streamRing(i);
        if (ring.space() < kBlock) { done[i] = true; continue; }
        int l = ring.available();
        if (l <= 0) l = 1 << 30;                            // already starving: after the rings a read can still save
        if (s < 0 || l < lead) { s = i; lead = l; }
      }
      if (s < 0) break;
      int one = 1;
      fill(s, eng_->streamRing(s), one, true);
      if (one == 1) done[s] = true;                          // nothing had to be read: full now, or waiting for a switch sample
      else budget--;
    }
  }
  const int used = start - budget;
  lastPumpChunks_ = used; pumpChunkTotal_ += (uint32_t)used; pumpClock_ = es.sampleClock;
  return used;
}

// Bar index of absolute sample S, its samples into that bar and the bar's end: exact for the bar the engine is in
// and the next one, the nominal bar length beyond (the ring never reaches further than one bar ahead).
void Generator::barPhase(const BarClock& c, uint32_t S, uint32_t& idx, uint32_t& into, uint32_t& barEnd) {
  const uint32_t L = c.barLen ? c.barLen : 1;
  if ((int32_t)(S - c.bar1) >= 0) { const uint32_t d = S - c.bar1, k = d / L; idx = c.idx0 + 1 + k; into = d - k * L; barEnd = c.bar1 + (k + 1) * L; }
  else if ((int32_t)(S - c.bar0) >= 0) { idx = c.idx0; into = S - c.bar0; barEnd = c.bar1; }
  else { const uint32_t d = c.bar0 - S, k = (d + L - 1) / L; idx = c.idx0 - k; into = S - (c.bar0 - k * L); barEnd = c.bar0 - (k - 1) * L; }
}

uint32_t Generator::barLineOf(uint32_t idx) const {
  const BarClock& c = clk_;
  if ((int32_t)(idx - c.idx0) <= 0) return c.bar0 - (c.idx0 - idx) * c.barLen;
  return c.bar1 + (idx - c.idx0 - 1) * c.barLen;
}

// The sample of an absolute tick: its bar line plus the ticks into the bar at the tempo in force.
uint32_t Generator::tickSample(uint32_t tick) const {
  return barLineOf(tick / kTicksPerBar) + ticksToSamples(tick % kTicksPerBar, clk_.bpm);
}

uint32_t Generator::stepFpOf(const VStream& st, uint16_t bpm) {
  float rate = st.bpm > 0 ? (float)bpmOr(bpm) / st.bpm : 1.0f;
  if (rate < 0.5f) rate = 0.5f;
  if (rate > 2.0f) rate = 2.0f;
  return (uint32_t)(rate * (float)st.wi.sampleRate / (float)kSampleRate * 65536.0f + 0.5f);
}

// Re-bases the ring at `base` (ring position 0 = absolute sample `base`, which lies in bar `bar`). While the engine
// may be reading, the ring is first deactivated and the reset waits until the engine has completed
// kHandshakeBlocks blocks. The content is rebuilt from scratch: no carried file position.
void Generator::rebase(int s, uint32_t base, uint32_t bar, uint16_t bpm, uint32_t sc, uint32_t start) {
  VSet& v = stream_[s]; VocalRing& ring = eng_->streamRing(s);
  v.armBase = base; v.armBar = bar; v.armBpm = bpm; v.start = start; v.armed = 0;
  v.cur.offFp = 0; v.cur.offCycle = 0; v.nxt.offFp = 0; v.nxt.offCycle = 0;
  if (!ring.active.load(std::memory_order_acquire)) { ring.reset(base); v.armed = 1; v.handshake = 0; return; }
  ring.active.store(0, std::memory_order_release);
  v.handshake = 1; v.handshakeClock = sc;
}

// The switch lands on the next grid tick (the bar for a new file or a vocal cycle, a 1/16 for a loop-mode crop / len
// change) at least kSwitchMarginBlocks ahead (the commands must reach the engine first). It is remembered as a tick:
// its sample follows the clock, so it is exact by the time content near it is written (the ring is less than a bar deep).
void Generator::scheduleSwitch(int s, uint32_t sc, const VocalRing& ring) {
  VSet& v = stream_[s];
  const uint32_t grid = v.switchGrid ? v.switchGrid : (uint32_t)kTicksPerBar;
  uint32_t tick = (clk_.tick0 / grid + 1) * grid;
  uint32_t S = tickSample(tick);
  while ((int32_t)(S - (sc + kSwitchMarginBlocks * kBlock)) < 0) { tick += grid; S = tickSample(tick); }
  v.switchTick = tick; v.switchSample = S; v.switchState = 1;
  const uint32_t frontier = ring.baseSample.load(std::memory_order_acquire) + ring.w.load(std::memory_order_acquire);
  if (ring.active.load(std::memory_order_acquire) && (int32_t)(frontier - S) > 0) { v.rewriteFrom = S; v.rewriteTo = frontier; }
  else { v.rewriteFrom = v.rewriteTo = 0; }
}

// The switch commands, applied by the engine on the bar the switch belongs to: a vocal stream pushes CmdVocalLoaded + the
// Vocal track's (empty) pattern swap carrying the cycle (SPEC §9.16); a loop slot CmdLoopLoaded + the carrier swap with
// the loop's variant and clip (SPEC §9.19); CmdLoopMode on / off when due; a waiting set job sends its other swaps, the
// Beat mask and CmdSwapSet in the same breath (SPEC §9.5). Nothing is pushed unless the queue holds all of it.
bool Generator::pushSwitchCmds(int s) {
  VSet& v = stream_[s]; const VStream& ns = v.nxt;
  const int t = streamTrack(s);
  if (t < 0) { v.switchState = 2; return true; }             // an orphan stream: nothing to tell the engine
  const int set = trackSet(t);
  const bool vocalStream = s < kSets;
  const int need = 2 + (v.modeDue ? 1 : 0) + (v.setJobWaiting ? popcount8(swapMask_[set]) + 2 : 0);
  if (!cmdRoom(need)) { notePushFail(); return false; }
  if (vocalStream) {
    if (!eng_->beginNextWrite(t)) return false;
    eng_->nextPattern(t).clear(ns.loopTicks);                // the clip is the cycle (SPEC §9.16), the empty pattern just carries its length
    eng_->endNextWrite(t);
    VocalState& vs = eng_->nextVocal(set);
    vs.gain = 1.0f; vs.present = 1; vs.fileBpm = ns.bpm; vs.loopTicks = ns.loopTicks;
    pushCmd(CmdVocalLoaded, 0, set);
  } else {
    const int slot = s - kSets;
    VocalState& ls = eng_->nextLoop(slot);
    ls.gain = 1.0f; ls.present = 1; ls.fileBpm = ns.bpm; ls.loopTicks = ns.loopTicks;
    pushCmd(CmdLoopLoaded, 0, slot);
  }
  if (v.modeDue) { pushCmd(CmdLoopMode, t, v.modeDue == 1 ? 1 : 0, s); v.modeDue = 0; }   // before the swap: the engine applies the mode first on that bar anyway
  pushCmd(CmdSwapPattern, t, ns.variant, (int32_t)ns.loopTicks);   // vocal: the cycle; loop slot: the carrier (engine.cpp applySwap)
  pushClock_ = eng_->status().sampleClock;
  if (v.setJobWaiting) {
    for (int o = 0; o < kStreams; o++) if (streamSet(o) == set) stream_[o].setJobWaiting = 0;
    pushSetSwaps(set);
  }
  pushFail_ = 0;
  v.switchState = 2;
  return true;
}

void Generator::finalizeSwitch(int s) {
  VSet& v = stream_[s];
  const bool shared = v.cur.f == v.nxt.f;
  const bool keepWin = winStream_ == &v.nxt || (winStream_ == &v.cur && shared);
  if (v.curValid && !shared) closeStreamFile(v.cur, nullptr);
  v.cur = v.nxt; v.curValid = 1;
  v.nxt.f = nullptr; v.nxtValid = 0; v.switchState = 0; v.rewriteFrom = v.rewriteTo = 0; v.armed = 0; v.switchGrid = kTicksPerBar;
  if (keepWin) winStream_ = &v.cur; else if (winStream_ == &v.cur) winStream_ = nullptr;
}

// SPEC §9.16: at a tempo change the material keeps its file position at P — the sample from which the ring is
// rewritten under the new clock — so the varispeed changes speed without a skip; the bars keep their phase. Only
// a window bar of the same cycle under both clocks can carry (a loop: the same cycle, inside the crop under both);
// the pause or another cycle starts clean.
void Generator::carryFilePos(VStream& st, const BarClock& from, const BarClock& to, uint32_t P) {
  const uint32_t C = cycleBarsOf(st);
  uint32_t iO, intoO, endO, iN, intoN, endN;
  barPhase(from, P, iO, intoO, endO); barPhase(to, P, iN, intoN, endN);
  if (st.mode) {                                             // a streamed loop (SPEC §9.19)
    const bool whole = st.loopTicks >= kTicksPerBar;         // whole-bar cycles are placed by the bar phase, sub-bar loops repeat inside the bar
    const uint32_t cycO = whole ? iO / C : iO, cycN = whole ? iN / C : iN;
    uint32_t posO, posN;
    if (whole) { posO = iO % C * from.barLen + intoO; posN = iN % C * to.barLen + intoN; }
    else { const uint32_t lO = ticksToSamples(st.loopTicks, from.bpm), lN = ticksToSamples(st.loopTicks, to.bpm); posO = intoO % (lO ? lO : 1); posN = intoN % (lN ? lN : 1); }
    const uint32_t regO = ticksToSamples(st.regionTicks, from.bpm), regN = ticksToSamples(st.regionTicks, to.bpm);
    if (cycO != cycN || posO >= regO || posN >= regN) { st.offFp = 0; st.offCycle = cycN; return; }
    const int64_t fileO = (int64_t)posO * stepFpOf(st, from.bpm) + (cycO == st.offCycle ? st.offFp : 0);
    const int64_t fileN = (int64_t)posN * stepFpOf(st, to.bpm);
    st.offFp = fileO - fileN; st.offCycle = cycN;
    return;
  }
  const uint32_t W = st.windowBars ? st.windowBars : (uint32_t)kVocalWindowBars;
  const bool carry = st.variant < 2 && iO % C < W && iN % C < W && iO / C == iN / C;
  if (!carry) { st.offFp = 0; st.offCycle = iN / C; return; }
  const int64_t fileO = (int64_t)(iO % C * from.barLen + intoO) * stepFpOf(st, from.bpm) + (iO / C == st.offCycle ? st.offFp : 0);
  const int64_t fileN = (int64_t)(iN % C * to.barLen + intoN) * stepFpOf(st, to.bpm);
  st.offFp = fileO - fileN; st.offCycle = iN / C;
}

// The tempo changed since the ring was last written: everything written ahead beyond a safety margin is redone
// under the new clock (bar lines moved, the rate changed) and the stream that continues at the retime point
// keeps its file position. Content inside the margin stays as written (the engine may be reading it).
void Generator::retime(int s, VocalRing& ring, uint32_t sc) {
  VSet& v = stream_[s];
  const uint32_t F = ring.baseSample.load(std::memory_order_acquire) + ring.w.load(std::memory_order_acquire);
  uint32_t P = sc + kRetimeMarginBlocks * kBlock;
  if ((int32_t)(P - F) > 0) P = F;
  VStream* st = v.curValid ? &v.cur : nullptr;
  if (v.nxtValid && v.switchState >= 1 && (int32_t)(P - v.switchSample) >= 0) st = &v.nxt;
  if (st) carryFilePos(*st, v.clk, clk_, P);
  if ((int32_t)(F - P) > 0) {
    if ((int32_t)(v.rewriteTo - v.rewriteFrom) > 0 && (int32_t)(v.rewriteFrom - P) < 0) P = v.rewriteFrom;   // a pending rewrite starts earlier: one region
    v.rewriteFrom = P; v.rewriteTo = F;
  }
}

void Generator::pumpStream(int s, int& budget, bool topUp) {
  VSet& v = stream_[s]; VocalRing& ring = eng_->streamRing(s);
  const EngineStatus& es = eng_->status();
  const int t = streamTrack(s);
  if (t < 0) { if (v.curValid || v.nxtValid) streamEmpty(s); return; }   // an orphan stream (its slot was unbound): nothing to feed
  const uint32_t sc = es.sampleClock, nextBar = clk_.bar1, nextIdx = clk_.idx0 + 1;
  if (v.handshake) {
    if ((int32_t)(sc - v.handshakeClock) < kHandshakeBlocks * kBlock) return;
    ring.reset(v.armBase); v.handshake = 0; v.armed = 1;
  }
  if (!v.curValid && !v.nxtValid) return;
  const PlayState ps = es.track[t].state;
  const bool playing = es.running && (ps == PSPlaying || ps == PSStopQueued);
  if (!playing) {
    v.playing = 0;
    if (v.nxtValid) {                                        // nothing is heard: switch now, state follows at the bar (or on resume)
      if (v.switchState < 2 && !pushSwitchCmds(s)) return;
      finalizeSwitch(s);
    }
    if (!v.curValid || !es.running) return;                  // transport stopped: no bar line to pre-roll for
    if (!v.armed || v.armBpm != es.bpm) {                    // pre-roll at the next bar line (SPEC §9.3), once per file / tempo
      rebase(s, nextBar, nextIdx, es.bpm, sc, nextBar);
      if (v.handshake) return;
    } else if (v.armBase != nextBar) {
      // The bar passed unheard. A pure pre-roll (nothing consumed, written for armBase) whose bar has the same
      // phase in the cycle as the next bar only moves its base: one atomic, no reads (loops: every bar,
      // SPEC §9.3). Another phase (window bar 1, the pause, the restart) is rebuilt for its bar (SPEC §9.16).
      const uint32_t C = cycleBarsOf(v.cur);
      if (ring.r.load(std::memory_order_acquire) == 0 && v.start == v.armBase && v.armBar % C == nextIdx % C) {
        ring.baseSample.store(nextBar, std::memory_order_release); v.armBase = nextBar; v.armBar = nextIdx; v.start = nextBar;
      } else {
        rebase(s, nextBar, nextIdx, es.bpm, sc, nextBar);
        if (v.handshake) return;
      }
    }
  } else {
    const uint32_t ss = es.track[t].startSample;
    if (!v.playing || ss != v.engStart) {                    // a (re)start
      v.playing = 1; v.engStart = ss;
      const int32_t d = (int32_t)(v.armBase - ss);
      const bool preroll = v.armed || v.handshake;
      if (preroll && d >= 0 && d < kBlock) v.start = v.armBase;   // start at the armed bar: the pre-roll holds the bar's exact sample (SPEC §9.3)
      else if (preroll) {                                    // a start elsewhere (not at the armed bar): rebuild from its sample, in its bar
        uint32_t idx, into, end; barPhase(clk_, ss, idx, into, end);
        rebase(s, ss, idx, es.bpm, sc, ss);
        if (v.handshake) return;
      }
      else v.start = ss;                                     // no pre-roll (resume): append from the engine's read position
    }
    v.armed = 0;
    if (v.nxtValid) {
      if (v.switchState == 0) {
        scheduleSwitch(s, sc, ring);
        if (!ring.active.load(std::memory_order_acquire)) rebase(s, v.switchSample, v.switchTick / kTicksPerBar, es.bpm, sc, v.start);   // first file: content from the switch on
      } else {
        v.switchSample = tickSample(v.switchTick);           // follows the clock: exact once the switch is inside the next bar
      }
      if (v.switchState == 1 && ((int32_t)(nextIdx * kTicksPerBar - v.switchTick) >= 0 || (int32_t)(sc + kLateBlocks * kBlock - v.switchSample) >= 0)) pushSwitchCmds(s);
      if (v.switchState == 2 && (int32_t)(clk_.tick0 - v.switchTick) >= 0) finalizeSwitch(s);
    }
    if (v.clk.bpm && v.clk.bpm != clk_.bpm && ring.active.load(std::memory_order_acquire)) retime(s, ring, sc);
  }
  fill(s, ring, budget, topUp);
}

// Content for [S, S + n): cur before the switch sample, nxt from it on, silence without a stream. Returns the
// count written (fewer when the chunk budget ran out).
int Generator::write(VSet& v, uint32_t S, int n, VocalRing& ring, uint32_t base, int& budget) {
  VStream* src = v.curValid ? &v.cur : nullptr;
  if (v.nxtValid && v.switchState >= 1) {
    if ((int32_t)(S - v.switchSample) >= 0) src = &v.nxt;
    else { const uint32_t lim = v.switchSample - S; if ((uint32_t)n > lim) n = (int)lim; }
  }
  if (src) return produce(*src, v.start, S, n, ring, base, budget);
  uint32_t p = ring.wrap(S - base);                          // ring position, wrapped once and stepped (any cap: a loop ring is 3072 samples)
  for (int k = 0; k < n; k++) { ring.buf[p] = 0; if (++p == (uint32_t)ring.cap) p = 0; }
  return n;
}

// Appends from the write frontier (or from the engine's read position after an underrun) whenever the stream's
// refill threshold is free (kRefillSamples for a vocal ring, kLoopChunkSamples for a loop ring, one block for a
// top-up after a refused streamsHealthy()); content beyond a scheduled switch comes from nxt (pre-roll, SPEC §9.3).
// A rewrite region (a switch moved it to nxt, or the tempo changed) is redone first.
void Generator::fill(int s, VocalRing& ring, int& budget, bool topUp) {
  VSet& v = stream_[s];
  const uint32_t base = ring.baseSample.load(std::memory_order_acquire);
  const uint32_t r = ring.r.load(std::memory_order_acquire);
  uint32_t w = ring.w.load(std::memory_order_relaxed);
  v.clk = clk_;
  if ((int32_t)(v.rewriteFrom - (base + r)) < 0) v.rewriteFrom = base + r;   // behind the engine: already heard
  // Every write() below may spend ONE card read: what that read brought is in the ring (and, for the append, published)
  // before the next read stalls the loop task, so a ring with a little lead left survives the stall (SPEC §9.19 §3).
  while ((int32_t)(v.rewriteTo - v.rewriteFrom) > 0) {
    int n = (int)(v.rewriteTo - v.rewriteFrom); if (n > kChunkSamples) n = kChunkSamples;
    const int want = n;
    int one = budget > 0 ? 1 : 0;
    const int got = write(v, v.rewriteFrom, n, ring, base, one);
    if (budget > 0 && one == 0) budget--;
    v.rewriteFrom += (uint32_t)got;
    if (got < want && (budget <= 0 || got == 0)) return;
  }
  if ((int32_t)(r - w) > 0) w = r;                           // the engine skipped ahead: resume where it reads
  int space = ring.cap - (int)(w - r);
  const int threshold = topUp ? kBlock : (s < kSets ? kRefillSamples : kLoopChunkSamples);
  if (space < threshold) return;
  while (space > 0) {
    const int n = space;
    int one = budget > 0 ? 1 : 0;
    const int got = write(v, base + w, n, ring, base, one);
    if (budget > 0 && one == 0) budget--;
    w += (uint32_t)got; space -= got;
    ring.w.store(w, std::memory_order_release);              // published per read
    if (got < n && (budget <= 0 || got == 0)) break;         // chunk budget spent: next pump continues (a switch sample only shortened this write)
  }
  if (w > 0 && !ring.active.load(std::memory_order_relaxed)) ring.active.store(1, std::memory_order_release);
}

// Writes output samples for absolute indices [S, S + n) of stream `st` into the ring, placed by the bar clock
// (SPEC §9.16): varispeed with linear interpolation (SPEC §9.3 rate = bpmNow / fileBpm clamped 0.5..2.0).
// Window variants: in cycle bar 0 / 1 the phrase from its first sample, file position = (cycleBar × bar + into) ×
// rate (+ the continuity offset after a tempo change) — silence once the file ends, a 64-sample fade-out before
// the bar line that ends the window — then silence for the pause bars; every cycle restarts clean on its bar.
// Loop variants: the loop repeats inside each bar (pos = into mod loop), with a 64-sample crossfade from the tail
// into the head at every wrap after the first pass from `start`. A streamed loop (mode 1, SPEC §9.19): cycle =
// loopTicks placed by the bar phase (whole bars) or repeating inside the bar (shorter), file position = pos × rate
// while pos is inside the crop (regionTicks), silence beyond it with a 64-sample fade-out at the crop's end when
// the cycle is longer, and the wrap crossfade of the loop variants when the crop fills the cycle AND the file holds a
// full crossfade of content past the cycle end (a crop of a longer file). A whole loop file (exactly n bars, its end
// faded by the tool, docs/LOOPS.md) has nothing there: its head plays straight at every repeat, exactly as on the first
// pass — a crossfade from "the file past its end" would fade the downbeat in from silence (SPEC §9.19 review). A crop
// shorter than the cycle restarts from the silence its fade-out left and needs no crossfade either. Returns the count
// written; fewer when a chunk read would exceed `budget` (silence costs no reads).
int Generator::produce(VStream& st, uint32_t start, uint32_t S, int n, VocalRing& ring, uint32_t base, int& budget) {
  const BarClock& c = clk_;
  const bool window = !st.mode && st.variant < 2;
  const uint32_t W = st.windowBars ? st.windowBars : (uint32_t)kVocalWindowBars;   // SPEC §9.16 amendment
  const uint32_t C = cycleBarsOf(st);
  const bool wholeBars = st.mode && st.loopTicks >= kTicksPerBar;
  uint32_t loopSamples = wholeBars ? C * c.barLen : ticksToSamples(st.loopTicks, c.bpm); if (!loopSamples) loopSamples = 1;
  uint32_t regionSamples = st.mode ? ticksToSamples(st.regionTicks, c.bpm) : loopSamples; if (regionSamples > loopSamples) regionSamples = loopSamples;
  const bool fullRegion = regionSamples == loopSamples;
  const uint32_t stepFp = stepFpOf(st, c.bpm);
  const bool xfadeWrap = fullRegion && ((((uint64_t)loopSamples + kXfade - 1) * stepFp) >> 16) < st.frames;   // the last tail sample of a wrap lies inside the file
  uint32_t p = ring.wrap(S - base);                          // ring position of S, stepped per sample (VocalRing::wrap: any cap)
  uint32_t idx, into, barEnd;
  barPhase(c, S, idx, into, barEnd);
  uint32_t cycleBar = idx % C;
  int32_t rel = (int32_t)(S - start);
  for (int k = 0; k < n; k++, rel++, into++, p = (p + 1 == (uint32_t)ring.cap ? 0u : p + 1)) {
    const uint32_t Sk = S + (uint32_t)k;
    if (Sk == barEnd) { idx++; into = 0; barEnd += c.barLen; cycleBar = idx % C; }
    int16_t a, b; int32_t out;
    if (window) {
      if (cycleBar >= W) out = 0;                                           // the pause
      else {
        int64_t fp = (int64_t)(cycleBar * c.barLen + into) * stepFp;
        if (idx / C == st.offCycle) fp += st.offFp;
        if (fp < 0) out = 0;
        else {
          if (!fetch(st, (uint32_t)(fp >> 16), a, b, budget)) return k;
          out = lerp16(a, b, (uint32_t)(fp & 0xFFFF));
        }
        if (cycleBar == W - 1) {
          const uint32_t left = barEnd - Sk;
          if (left <= (uint32_t)kXfade) out = out * (int32_t)left / kXfade;   // fade-out at the window end
        }
      }
    } else {
      const uint32_t pos = wholeBars ? cycleBar * c.barLen + into : into % loopSamples;
      const uint32_t cycleIdx = wholeBars ? idx / C : idx;
      if (st.mode && pos >= regionSamples) { out = 0; }                      // the cycle beyond the crop (SPEC §9.4 silence beyond the pattern)
      else {
        int64_t srcFp = (int64_t)pos * stepFp;
        if (st.mode && cycleIdx == st.offCycle) srcFp += st.offFp;
        if (srcFp < 0) srcFp = 0;
        const uint32_t i = (uint32_t)(srcFp >> 16), fr = (uint32_t)(srcFp & 0xFFFF);
        if (xfadeWrap && rel >= (int32_t)loopSamples && pos < (uint32_t)kXfade) {   // wrap after the first pass: the tail continues past the cycle end under the head's fade-in
          const uint64_t tFp = (uint64_t)(loopSamples + pos) * stepFp;
          if (!fetch(st, (uint32_t)(tFp >> 16), a, b, budget)) return k;
          const int32_t tail = lerp16(a, b, (uint32_t)(tFp & 0xFFFF));
          int32_t head = 0;
          if (i + 1 < (uint32_t)kHeadSamples) head = lerp16(st.head[i], st.head[i + 1], fr);
          else if (i < (uint32_t)kHeadSamples) head = st.head[i];
          out = tail + ((head - tail) * (int32_t)pos) / kXfade;
        } else {                                                            // the first pass, a whole file's repeat (the head straight) or a crop's restart after its fade-out
          if (!fetch(st, i, a, b, budget)) return k;
          out = lerp16(a, b, fr);
        }
        if (st.mode && !fullRegion) {
          const uint32_t left = regionSamples - pos;
          if (left <= (uint32_t)kXfade) out = out * (int32_t)left / kXfade;   // fade-out where the crop ends into silence
        }
      }
    }
    ring.buf[p] = (int16_t)out;
  }
  return n;
}

// s[i] and s[i + 1] of the stream through the shared window (2048 mono / 1024 stereo frames = one 4 KB read);
// beyond the data both are 0. A window reload costs one chunk of `budget`; a sequential advance carries the
// last sample over as winPrev_.
bool Generator::fetch(VStream& st, uint32_t i, int16_t& a, int16_t& b, int& budget) {
  if (!st.f || i >= st.frames) { a = b = 0; return true; }
  for (int pass = 0; pass < 2; pass++) {
    if (winStream_ == &st && winEnd_ > winStart_) {
      if (i >= winStart_ && i + 1 < winEnd_) { a = chunk_[i - winStart_]; b = chunk_[i + 1 - winStart_]; return true; }
      if (i >= winStart_ && i < winEnd_ && i + 1 >= st.frames) { a = chunk_[i - winStart_]; b = 0; return true; }
      if (winHavePrev_ && i + 1 == winStart_) { a = winPrev_; b = chunk_[0]; return true; }
    }
    if (pass == 1) break;
    if (budget <= 0) return false;
    budget--;
    uint32_t ws;
    if (winStream_ == &st && winEnd_ > winStart_ && i + 1 == winEnd_) { winPrev_ = chunk_[winEnd_ - 1 - winStart_]; winHavePrev_ = 1; ws = winEnd_; }
    else { ws = i; winHavePrev_ = 0; }
    uint32_t n = (uint32_t)kChunkSamples / (st.wi.channels ? st.wi.channels : 1);   // one 4 KB read whatever the channel count
    if (ws + n > st.frames) n = st.frames - ws;
    uint32_t got = 0;
    if (st.f->seek(st.wi.dataOffset + ws * (st.wi.channels * 2u))) got = wavReadMono(*st.f, st.wi, chunk_, n);
    winStream_ = &st; winStart_ = ws; winEnd_ = ws + got;
    if (!got) { winStream_ = nullptr; a = b = 0; return true; }   // card error: silence, no retry storm
  }
  a = b = 0;
  return true;
}

} // namespace lp
