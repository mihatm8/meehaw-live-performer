// meehaw live performer — set generation state machine and vocal producer (SPEC §6, §9.2, §9.3, §9.4, §9.16).
// Runs on the UI/loop task. Every step() does ONE bounded SD operation (a library prepare chunk, a pick, a file
// open, one 4 KB chunk or one slot stage) so no burst exceeds kMaxSdBurstMs (SPEC §9.2); the audio task never
// waits. pumpVocal() is the ring producer of SPEC §9.3 / §9.19 for the kStreams streams (the two vocal streams and
// the two loop slots, emptiest ring first) and reads the WAVs in 4 KB chunks; the chunks it reads inside step()
// are charged to that step (a step whose pump already read two chunks skips its own card work).
// No heap, no std containers; the only large member is one 4 KB chunk buffer (SPEC §9.13 ≈ 8 KB of UI scratch).
#pragma once
#include "../core/types.h"
#include "../engine/engine.h"
#include "library.h"
#include "key.h"
#include "variants.h"                       // Rng, TheoryPlan (SPEC §9.24)
#include "../io/fs.h"
#include "../io/wav.h"

namespace lp {

// What is loaded on a track (for the screen, SPEC §4 footer, and the set files of SPEC §9.8).
struct TrackGenInfo {                       // fields ordered by alignment: 160 B, no padding (16 of these live in the Generator)
  char file[kMaxNameLen] = "";              // basename of the MIDI or vocal file ("" = nothing loaded)
  char sound[40] = "";                      // preset name, kick/perc basename or vocal "110bpm"
  uint32_t seed = 1;                        // variant seed, new per pattern pick (SPEC §5)
  float fileBpm = 0;                        // Vocal: the file's BPM token (SPEC §9.3)
  uint16_t naturalTicks = kTicksPerBar;     // natural length of the selected variant (whole beats × 96, crops 192 / 48; Vocal: the Reaper loop length)
  uint16_t sourceTicks = kTicksPerBar;      // natural length of the source pattern
  uint16_t presetIndex = 0;                 // synth categories: absolute index into the category's bank (SPEC §9.22: banks of up to Generator::kPresetBagMax presets per family, so 16 bits)
  uint8_t variant = 0;                      // selected variant 0..3
  uint8_t keyRoot = 9;                      // key the pattern was generated with; variants rebuild with it (SPEC §9.9)
  uint8_t userLoop = 0;                     // Fx: a recorded loop that GENERATE / gen pattern keep (SPEC §9.10)
  uint8_t loaded = 0;                       // a pattern has been published for this track
  uint8_t edited = 0;                       // the source was changed in the editor / by the looper: set files write it as an SMF asset (SPEC §9.8)
  int8_t lenStep = 0;                       // k of len = natural × 2^k, reset on variant select / gen / load (SPEC §9.4); always 0 on a Vocal
  uint8_t pause = kVocalPauseDefault;       // Vocal: pause bars of the window + pause cycle, 0 / 1 / 2 / 4 (SPEC §9.16), kept across generates
  uint8_t acid = 0;                         // SPEC §9.17b acid mode (Chord / Lead / Pad / Bass): '303' presets, random knobs, acid patterns; lp.track.N.acid
  uint8_t acidPat = 0;                      // the source is the acid generator's line / stabs built from `seed` and keyRoot (no file; a session regenerates it)
  uint8_t windowBars = kVocalWindowBars;    // Vocal: bars of the window (SPEC §9.16 amendment: a '<n>bars' token in the file name sets it, default 2)
};

class Generator {
 public:
  static constexpr int kChunkSamples = 2048;           // one 4 KB SD read of int16 (SPEC §9.2)
  static constexpr int kHeadSamples = 136;             // phrase head kept per stream for the wrap crossfade (64 × rate 2 + 2)
  static constexpr int kXfade = 64;                    // samples of crossfade at a loop wrap (SPEC §9.3)
  static constexpr int kRefillSamples = 2048;          // vocal ring: refill whenever this many samples are free (SPEC §9.2, half the ring); loop rings: kLoopChunkSamples
  static constexpr int kPumpChunks = 4;                // chunk reads per loop iteration over the four rings (≤ 16 KB, SPEC §9.19)
  static constexpr int kGateLeadMs = 170;              // streamsHealthy(): a consumed ring must hold this much lead before card work: the 150 ms slow-card op of SPEC §9.19 §3 plus the refill reads that follow it — the ring read second drains through two of them (10 ms each in the host model, SPEC §9.19e), so 160 ms covered only the first ring
  static constexpr int kGateLeadSamples = kGateLeadMs * kSampleRate / 1000;   // 3748 (a 4096-sample vocal ring refilled in 2048-sample chunks sits at 3840..4096 right after a refill)
  static constexpr int kStepSkipChunks = 2;            // a step whose pump read this many chunks does no further card work
  static constexpr int kVocalPickAttempts = 6;         // re-picks for a name without a BPM token / an unstreamable file
  static constexpr int kRequestQueue = 4;
  static constexpr int kPrepareStepsMax = 64;          // library warm-up steps per pick before picking cold anyway
  // Card file handles (SPEC §9.19g): kMaxOpenFiles (config.h, 7) = kStreams live stream files + kPendingSwitches new-file stream
  // switches waiting for their bar line + one for the job's own kit / index / SMF file (or a Sets file). A new-file stream
  // open (a phrase, a loop, the alternate phrase, the phrase a Vocal track returns to when its loop mode ends) is made only
  // while one handle stays free after it (streamOpenAllowed); otherwise the path waits for the next step — a pending switch
  // frees its handle at its bar line (finalizeSwitch), so the wait is at most a bar. An open that fails anyway (a handle
  // taken elsewhere, a card hiccup) is retried kOpenRetrySteps times before the path gives up as it did before.
  static constexpr int kPendingSwitches = kMaxOpenFiles - kStreams - 1;   // 2
  static constexpr int kOpenRetrySteps = 4;
  // Preset bag (SPEC §9.22, research §4 "bitmask bag"): a family (the library presets, or the '303' family appended after
  // them) of up to this many presets is drawn without a repeat until it is exhausted; one 256-bit seen mask per track (32 B;
  // 512 bits / 64 B before the 2026-10-16 RAM diet — presets.cpp caps every bank at 255 rows, so a family never exceeds it).
  static constexpr int kPresetBagMax = 256;
  static constexpr int kOpenWaitSteps = 1024;          // a job waits this many steps for a handle at most (≈ 12 s, longer than any bar), then opens anyway

  void init(IFileSystem* fs, Engine* eng, Library* lib);
  // Re-seeds the Rng AND restarts every track's preset bag (the seen masks): the state a session recording / replay
  // starts from must be the same on both sides (SPEC §9.23), and the bag is part of what the next sound pick draws from.
  // presetIndex / sound (the preset sounding now) are untouched: the pick after a re-seed avoids that one as live did.
  void seed(uint32_t s);

  // Requests (non-blocking, queued up to kRequestQueue; progress via busy()/progress()/total()). false = refused.
  bool generateSet(int set);
  bool generateTheorySet(int set);                     // SPEC §9.24 (SHIFT + load 1 / 2): one coherent set from ONE chord progression of the flash tables (theory_tables.hpp): Pad voicings, Chord stabs, Bass and Lead derived through rhythm templates and motifs, in the project key, no card reads for the melodic tracks; Beat / Percussion / Vocal regenerate from the library inside the same job; queues behind other jobs and swaps at the bar like generateSet (request order decides the plan, so a recorded session and its offline render draw the same set); false = refused (the very job already running, queue full, bad set, or no engine)
  bool theorySet(int set) const;                       // the set's melodic tracks came from the theory generator (main-page footer shows the progression name via info().file = "THEORY <name>"); cleared by a GENERATE / gen pattern / set load / acid line on a melodic track of the set
  int theoryProgression(int set) const;                // theory::kProgs index of the set's progression while theorySet(set), else -1 (tests, footer)
  bool generatePattern(int track);                     // pattern only (all variants), sound unchanged
  bool generateSound(int track);                       // sound only: preset, kit slots or another vocal file
  void rebuildVariant(int track, int variant);         // from source + stored seed/key: CmdSwapPattern / CmdBeatMask / vocal loop
  // len± (SPEC §9.4): clip len = natural × 2^k clamped 0.5..16 beats, CmdSetLength at the bar; returns len. On a Vocal
  // track the pause steps through 0 / 1 / 2 / 4 bars instead (SPEC §9.16): the producer switches to the new cycle at
  // a bar line and that switch's CmdSwapPattern carries the clip length (2 + pause) bars, so the engine's len and the
  // streamed cycle change on the same bar (no separate CmdSetLength); returns the clip length. On the loop variants
  // (3 / 4, no pause) len± does nothing and returns the loop. dir 0 queries.
  uint16_t setLengthStep(int track, int dir);
  // SPEC §9.17b additions
  void transposeOctave(int track, int dir);            // shift the track's source pattern ±12 semitones (clamped 12..115), rebuild, swap at the bar
  bool acidEnabled(int track) const;                   // acid mode flag of a synth track (Chord/Lead/Pad/Bass), saved in set files as lp.track.N.acid
  void setAcid(int track, bool on);                    // toggling re-rolls the sound (303 family on, library preset off); pattern unchanged
  // ---- loop mode (SPEC §9.19) ----
  enum LoopRefusal : uint8_t { LoopOk = 0, LoopSlotsFull, LoopNoContent, LoopBusy, LoopLowRam, LoopNoCard };
  bool setLoopMode(int track, bool on);                // SHIFT + edit: on = pick a loop of the track's category (JobLoop: prepare, pick, open, CmdLoopLoaded + CmdLoopMode at the bar; the Vocal track streams it through its set's vocal stream), off = CmdLoopMode off (the parked pattern / sound return at the bar, no card work); false = refused, see lastLoopRefusal(). ON does no card work itself: it refuses LoopNoContent only when the library already knows (a pack without a loops/ folder, a counted category of 0 files); a cold category is found out by the job, which then ends with loop mode off, lastLoopRefusal() LoopNoContent and lastError "NO LOOP: <FOLDER>" (the UI shows that when the job ends)
  LoopRefusal lastLoopRefusal() const { return loopRefusal_; }
  // Snare rush on a loop Beat track (SPEC §9.19 §1.4 as amended by §9.22): on = the stream switches on the next 1/16 to a
  // crop of HALF the clip length set on the track (len = natural × 2^k, so len / 2, at least 1 beat), looped; off = back
  // to the stored crop and len on the next 1/16; info().variant / lenStep are never touched, so the user's len± survives a
  // rush. The UI calls this instead of CmdSnareRush while loopTrack(beat); a no-op on a track not in loop mode (and the
  // rush ends with loop mode).
  void rushLoop(int track, bool on);
  bool loopRushing(int track) const { return track >= 0 && track < kTracks && ((loopRush_ >> track) & 1) != 0; }
  // A loop slot's ring is being consumed (the transport runs and a slot-bound track plays or is queued to): Sets defers a
  // session write and pauses one in flight meanwhile (SPEC §9.19 §3): a 139 ms slot ring cannot absorb a 150 ms card open,
  // whatever lead the gate asks for. The Vocal track's loop (a 186 ms vocal ring) does not count.
  bool loopSlotStreaming() const;
  bool loopEnabled(int track) const;                   // loop mode requested on this track (the engine applies it at the bar)
  int loopSlot(int track) const;                       // 0..kLoopSlots-1, or -1 (not in loop mode, or the Vocal track's own stream)
  const char* loopFile(int track) const;               // basename of the streamed loop ("" none yet)
  uint8_t loopBars(int track) const;                   // its '<n>bars' token (1..kMaxLoopBars), 0 none
  bool anyLoop() const;                                // a loop slot or a Vocal-track loop is in use (tempo-to-125 rule, SPEC §9.19)
  static const char* loopFolder(Category c);           // "drum" "percussion" "bass" "chord" "lead" "pad" "fx" "vocal"
  void setFreeHeapHook(uint32_t (*fn)()) { freeHeap_ = fn; }   // device: ESP.getFreeHeap (LoopLowRam below kLoopLowRamBytes); host: nullptr = unlimited
  // Additions for sets.cpp (SPEC §9.19 §1.7 persistence, §3 no verification opens):
  const char* loopPath(int track) const;               // absolute path of the streamed loop ("" none yet): the preset asset copy / lp.src.i
  // Restores loop mode on `track` with that exact file (one file open; the slot allocated here, the mode and the loop's
  // variant / len = natural × 2^lenStep published with the stream switch; a set loader's burst waits for it like the
  // vocal's). false = the file cannot be streamed (or no slot): the caller picks a loop of the category instead.
  bool loadLoop(int track, const char* path, int variant = 0, int lenStep = 0);
  // The absolute path of the track's pattern source / a kit slot's one-shot, composed from the folder remembered at pick
  // or load time (no card work; false when unknown or when info().file / the kit name changed since): the session writer
  // writes these without opening anything (SPEC §9.19 §3 b).
  bool sourcePath(int track, char* out, int cap) const;
  bool kitPath(int set, int slot, char* out, int cap) const;
  void noteKitPath(int set, int slot, const char* path);   // a one-shot the set loader put into a slot (its absolute path)
  // Stream-health gate (SPEC §9.19 §3 a): true when every stream the engine is consuming holds at least kGateLeadSamples
  // of lead (a ring too small for that, a loop slot: at least half full), or no stream is consumed. Sets::step() and the
  // generator's own card steps do card work only when it is true; a refused query makes the next pump top every ring up
  // (a chunk per ring per round, emptiest first), so the gate opens on a following iteration instead of waiting for a
  // refill threshold.
  bool streamsHealthy() const;
  void randomizeKnobs(int track);                      // seeded musical knob randomisation (used by every sound generation in acid mode)
  static int knobCmdCount(Category c) { return 8 + ((c == Lead || c == Chord) ? 1 : 0); }   // CmdSetParam per randomisation: CUT RES ENV DEC DRV WAV SAT ACC, + the chorus send on Lead / Chord (SPEC §9.22)
  static bool acidCategory(Category c) { return c == Chord || c == Lead || c == Pad || c == Bass; }
  // The acid pattern of (seed, keyRoot) into the track's source (pure; the session restore rebuilds a line from its seed).
  void buildAcidSource(int track, uint32_t seed, int keyRoot);
  // Every change a session file should hold bumps this counter (jobs finished, variants, len / pause, key, genre, acid,
  // knobs, transposes): Sets polls it and debounces the session write (SPEC §9.18).
  uint32_t changeCount() const { return changes_; }
  bool setGenreByName(const char* name);               // genre by pack folder name (session restore); false when unknown
  void step();                                         // one bounded SD operation (after the vocal pump); call every loop iteration
  void pumpVocal();                                    // vocal ring producer (SPEC §9.3); call every loop before other SD work
  bool busy() const { return state_ != Idle || queueN_ > 0; }
  int progress() const { return done_; }
  int total() const { return total_; }
  const char* lastError() const { return err_; }       // "" when the last job had no error
  const char* lastNote() const { return note_; }       // "FX kept" (SPEC §9.10) or ""
  // Card budget bookkeeping (SPEC §9.2; footer debug, tests): chunk reads of the last pump, in total, and the
  // steps that skipped their own card work because the pump had used the budget.
  int lastPumpChunks() const { return lastPumpChunks_; }
  uint32_t pumpChunkTotal() const { return pumpChunkTotal_; }
  uint32_t stepsSkipped() const { return stepsSkipped_; }
  int stateCode() const { return (int)state_; }         // the job state machine's state (debug / tests)

  // State the UI and set files need
  ProjectKey& key() { return key_; }
  void setKeyRoot(int root);                           // key change: future generation only (SPEC §2), restarts the bags (SPEC §9.9)
  int genreIndex() const { return genre_; }
  void setGenre(int g);
  const char* genreName() const;
  TrackGenInfo& info(int track) { return info_[track < kTracks ? track : 0]; }
  const uint8_t* foldMap(int set) const { return foldMap_[set & 1]; }   // Percussion voice map (SPEC §9.7)

  // Loading helpers (also used by sets.cpp): synchronous and bounded to one file / no SD at all.
  bool loadMidiToSource(int track, const char* path);  // smf parse → engine source, fold map for Percussion
  bool publishVariant(int track, int variant);         // source + seed/key → next, CmdSwapPattern (Beat: Full + rush + mask)
  bool loadVocal(int set, const char* path, int sel = 0, bool force = false);   // opens the file as the set's next phrase; switches at a bar line. false with lastOpenBusy(): no handle now, call again next step (force skips the budget)
  // SPEC §9.19g: a new-file stream open may be made now (fs openCount() + 1 < kMaxOpenFiles: one handle stays free for the
  // job's own file). Sets polls it before loadVocal / loadLoop; a refused call reports lastOpenBusy().
  bool streamOpenAllowed() const;
  bool lastOpenBusy() const { return openErr_ == OpenBusy; }   // the last loadVocal / loadLoop was refused for a handle: nothing changed, call again next step
  const char* vocalPath(int set, int sel) const { return (sel & 1) ? altPath_[set & 1] : stream_[set & 1].path; }
  void setVocalPath(int set, int sel, const char* path);    // the file behind variant sel (0 primary, 1 alternate), no SD
  // Set-wide swap on behalf of the set loader (sets.cpp, SPEC §9.5 / §9.8): the tracks in `mask` (bit i = track i of
  // the set, already built into their next buffers with info().variant / lenStep set by the caller) get their
  // CmdSwapPattern (len = natural × 2^lenStep), the Beat's layer mask and CmdSwapSet in one burst. While the
  // transport runs and a vocal switch is scheduled, the burst waits and goes out in the same breath as the vocal's
  // commands (poll setSwapWaiting()); mask 0 while waiting forces the push. Returns true when pushed now; a full
  // command queue defers the burst to a later step() (it is never split).
  bool requestSetSwap(int set, uint8_t mask);
  bool setSwapWaiting(int set) const;                        // a stream of the set holds the loader's burst for its switch bar
  bool setSwapPending(int set) const { return setSwapWaiting(set) || ((swapDue_ >> (set & 1)) & 1) != 0; }   // the burst has not left yet (vocal bar or queue room)
  // Producer state (footer display, tests): the startSample the ring content follows, the streamed loop, a pending
  // switch, and the idle pre-roll (armed at the bar line armBase, SPEC §9.3).
  uint32_t vocalStartSample(int set) const { return stream_[set & 1].start; }
  uint16_t vocalLoopTicks(int set) const { const VSet& v = stream_[set & 1]; return v.curValid ? v.cur.loopTicks : 0; }
  uint16_t vocalRegionTicks(int set) const { const VSet& v = stream_[set & 1]; return v.curValid ? v.cur.regionTicks : 0; }   // a Vocal-track loop's crop (SPEC §9.19)
  bool vocalStreamsLoop(int set) const { const VSet& v = stream_[set & 1]; return v.curValid && v.cur.mode != 0; }            // the set's vocal stream carries a loop file (not a phrase)
  bool vocalSwitchPending(int set) const { return stream_[set & 1].nxtValid != 0; }
  bool vocalArmed(int set) const { return stream_[set & 1].armed != 0; }
  uint32_t vocalArmBase(int set) const { return stream_[set & 1].armBase; }
  // The bar clock the ring content is placed by (SPEC §9.16): the latest bar line the engine crossed (bar0, exact,
  // index idx0 = tick / 384), the next one (bar1, exact while the tempo holds) and the nominal bar length at the
  // tempo in force. Every sample S of a vocal cycle sits at (barIndex(S) mod cycleBars) bars + its samples into
  // that bar, so the cycle phase is position = globalTick mod len like every clip (SPEC §9.4) whatever bar the
  // track was started on, and a tempo change moves only the rate, never the bars.
  struct BarClock { uint32_t bar0 = 0, bar1 = 0, idx0 = 0, barLen = 1; uint16_t bpm = kDefaultBpm; uint32_t tick0 = 0; };   // tick0: the engine's tick at the pump (a loop-mode crop / len switch lands on a 1/16 of it, SPEC §9.19)
  const BarClock& barClock() const { return clk_; }         // as of the last pump
  static void barPhase(const BarClock& c, uint32_t S, uint32_t& idx, uint32_t& into, uint32_t& barEnd);   // bar index of S, samples into it, its end
  // Continuity offset of the streamed phrase (16.16 file samples) and the cycle index it applies to: at a tempo change
  // the file position at the retime point is carried over so the phrase does not skip (SPEC §9.16); 0 otherwise.
  int64_t vocalFileOffset(int set, uint32_t* cycle) const { const VStream& st = stream_[set & 1].cur; if (cycle) *cycle = st.offCycle; return st.offFp; }
  // The same for a loop slot's stream (tests): what its ring content follows.
  uint32_t loopStartSample(int slot) const { return stream_[kSets + (slot & 1)].start; }
  uint16_t loopClipTicks(int slot) const { const VSet& v = stream_[kSets + (slot & 1)]; return v.curValid ? v.cur.loopTicks : 0; }
  uint16_t loopRegionTicks(int slot) const { const VSet& v = stream_[kSets + (slot & 1)]; return v.curValid ? v.cur.regionTicks : 0; }
  bool loopSwitchPending(int slot) const { return stream_[kSets + (slot & 1)].nxtValid != 0; }
  int64_t loopFileOffset(int slot, uint32_t* cycle) const { const VStream& st = stream_[kSets + (slot & 1)].cur; if (cycle) *cycle = st.offCycle; return st.offFp; }
  // The clip (cycle) of a streamed loop of `bars` bars at crop `variant` (0 whole, 1 first half, 2 first 2 beats, 3 first beat) and
  // len step k: natural = the crop, len = natural × 2^k clamped to [1 beat, kMaxLoopTicks] (SPEC §9.19 §1.4). Pure.
  static uint16_t loopRegionOf(int bars, int variant);
  static int clampLoopStep(uint16_t natural, int k);
  static uint16_t loopLenOf(uint16_t natural, int k);

  // Pure helpers (exposed for tests)
  static int parseBpmToken(const char* name);          // '124bpm', 'bpm_124', '124 BPM' … → 60..249, else 0
  static uint8_t naturalBeats(uint32_t frames, uint32_t sampleRate, float bpm);   // SPEC §9.3: largest power of two 1..16 inside (Reaper's lp.track.6.len)
  // SPEC §9.16: the vocal clip length in ticks — variants 0 / 1: the (window + pause) cycle, 2: 2 beats, 3: 1 beat.
  static uint16_t vocalClipTicks(int variant, int pauseBars);   // at the default 2-bar window
  // SPEC §9.16 amendment: the same at a window of windowBars (a '<n>bars' file-name token). The engine clamps a vocal
  // clip at kMaxVocalClipTicks (engine.cpp applySwap, kVocalWindowMaxBars bars): a window that does not fit under it
  // is never truncated — clampWindow falls back to the default 2-bar window (raise kMaxVocalClipTicks in config.h to
  // play longer files whole) — and the pause is kept where window + pause fits (fitPause / stepPause refuse the rest).
  // The cap in vocalClipTicks itself is a last defence for a stored pause that no longer fits.
  static uint16_t vocalClipTicks(int variant, int pauseBars, int windowBars);
  static constexpr int kVocalWindowMaxBars = kMaxVocalClipTicks / kTicksPerBar;
  static int parseBarsToken(const char* name);         // '8bars', '4_bars', '2 bar' (digits right before 'bar', separators allowed) → n, 0 = none
  static uint8_t clampWindow(int bars);                // 0 → kVocalWindowBars; 1..kVocalWindowMaxBars as is; longer → kVocalWindowBars (never cut)
  static uint8_t clampPause(int bars);                 // nearest allowed pause 0 / 1 / 2 / 4 (3+ → 4, < 0 → 0)
  static uint8_t stepPause(int pause, int dir);        // the next allowed pause in direction dir (clamped at 0 and 4)
  static uint8_t stepPause(int pause, int dir, int windowBars);   // the same, refusing a step the engine's cap would swallow (window + pause > max)
  static uint8_t fitPause(int pause, int windowBars);  // the largest allowed pause ≤ pause with window + pause ≤ kVocalWindowMaxBars

 private:
  enum State : uint8_t { Idle, TrackBegin, PrepPick, Pick, Load, Publish, Preset, PrepSlot, SlotPick, SlotOpen, SlotChunk,
                         SlotPush, SlotWait, PrepVocal, VocalPick, VocalOpen, TrackDone, Finish, WaitSwap, AcidPat, SoundBegin,
                         PrepLoop, LoopPick, LoopOpen, TheoryPat };
  enum JobKind : uint8_t { JobNone = 0, JobSet, JobPattern, JobSound, JobLoop, JobTheory };   // JobTheory (SPEC §9.24): a set job whose melodic patterns come from the theory plan instead of the library
  // Outcome of a publish: only a busy engine lock / full queue is worth a retry (SPEC §9.5); "nothing loaded" is not.
  enum PubResult : uint8_t { PubOk = 0, PubBusy, PubNone };
  // Outcome of the last stream open (SPEC §9.19g): refused for a handle (wait), the open itself failed (retry a few steps),
  // or the file cannot stream (re-pick / give up as before).
  enum OpenErr : uint8_t { OpenOk = 0, OpenBusy, OpenFailed, OpenBad };
  struct Request { uint8_t kind = JobNone; uint8_t arg = 0; };
  // The seen mask of the track's preset family (SPEC §9.17b family, §9.22 bag): bit i = preset base + i has been drawn in the
  // current cycle. Nothing else is stored: the family's base and size come from presets.h on every pick and the last draw is
  // info().presetIndex, so the whole bag is the 32 B of the mask (16 tracks: 512 B; 1 KB before the RAM diet).
  struct PresetBag { uint8_t seen[kPresetBagMax / 8]; };

  // One streamed file as the producer sees it (SPEC §9.3, §9.16; §9.19 mode 1 = a loop).
  struct VStream {
    IFile* f = nullptr; WavInfo wi; uint32_t frames = 0; float bpm = 0;
    uint8_t natural = 4;                     // natural phrase length in beats (power of two, Reaper's loop length)
    uint8_t variant = 0, sel = 0;            // selected variant; file 0 primary / 1 alternate
    uint8_t windowBars = kVocalWindowBars;   // bars of the window (SPEC §9.16 amendment: '<n>bars' token, default 2)
    uint8_t mode = 0;                        // 0 vocal phrase (window + pause, loop variants 2 / 3), 1 streamed loop (SPEC §9.19): cycle = loopTicks, content = the first regionTicks of it
    uint8_t bars = 0;                        // loop: its '<n>bars' token (1..kMaxLoopBars)
    uint16_t naturalTicks = kTicksPerBar;    // natural length of the variant (full phrase, 2 beats, 1 beat; loop: the crop)
    uint16_t loopTicks = kTicksPerBar;       // clip cycle actually streamed: window + pause (variants 0 / 1), the loop (2 / 3), or natural × 2^k (mode 1)
    uint16_t regionTicks = kTicksPerBar;     // loop: the crop (whole, half, 2 beats, 1 beat); the cycle beyond it is silence (len± × 2^k, SPEC §9.4)
    int64_t offFp = 0; uint32_t offCycle = 0;   // file-position continuity offset (16.16) for cycle offCycle after a tempo change
    int16_t head[kHeadSamples];              // first samples of the file for the wrap crossfade of the loops
  };
  struct VSet {                              // per STREAM producer state (0 / 1 the sets' vocal streams, kSets + k loop slot k)
    VStream cur, nxt; uint8_t curValid = 0, nxtValid = 0;
    uint8_t switchState = 0;                 // 0 none, 1 scheduled (switchTick set), 2 commands pushed
    uint16_t switchGrid = kTicksPerBar;      // grid the next switch lands on: the bar (a new file, a vocal cycle), a 1/16 (a loop-mode crop / len change, SPEC §9.19 rush semantics)
    uint32_t switchTick = 0;                 // absolute tick the switch to nxt lands on; switchSample is its sample under the current clock
    uint32_t switchSample = 0;               // first absolute sample streamed from nxt
    uint32_t rewriteFrom = 0, rewriteTo = 0; // ring region already written that must be redone (a switch moved it to nxt, or the tempo changed)
    uint32_t start = 0;                      // startSample the content follows (SPEC §9.3): the loops' first pass is counted from here
    uint32_t engStart = 0; uint8_t playing = 0;   // the engine's startSample adopted at the last (re)start
    uint8_t armed = 0; uint32_t armBase = 0; uint32_t armBar = 0; uint16_t armBpm = 0;   // idle pre-roll: ring position 0 = bar line armBase (bar index armBar)
    uint8_t handshake = 0; uint32_t handshakeClock = 0;             // ring.reset only after the engine skipped a block
    uint8_t setJobWaiting = 0;               // the set job's swaps go out together with this stream's switch commands
    uint8_t modeDue = 0;                     // CmdLoopMode to push with the switch: 1 on, 2 off (SPEC §9.19)
    uint8_t closing = 0; uint32_t closeTick = 0;   // loop slot released: the stream ends (files closed, ring parked) once the engine passed closeTick
    BarClock clk;                            // the clock the ring content was last written with (a new bpm retimes it)
    char path[kMaxPathLen];                  // vocal stream: the primary phrase (the alternate of SPEC §5 variant 2 is altPath_[set]); loop slot: the loop
  };
  struct LoopTrack {                         // what loop mode parks on a track (SPEC §9.19 §1.1): the pattern's variant / len the UI sees again on off
    uint8_t parkedVariant = 0; int8_t parkedLenStep = 0; uint16_t parkedNatural = kTicksPerBar;
  };

  // state machine (one card operation per step, SPEC §9.2)
  bool queuePush(uint8_t kind, uint8_t arg);
  void queueDrop(uint8_t kind, uint8_t arg);
  void startJob(const Request& r);
  void dispatch();
  void beginTrack();
  void nextPart();                                     // after the pattern part: sound part or track done
  void stepSoundBegin();                               // SoundBegin: CmdResetTrack right before the sound part (SPEC §9.17b), then preset / slots
  bool pushReset();                                    // the track's CmdResetTrack once per track (resetDone_); false = no queue room (retry)
  void prepared(State next);                         // after a library warm-up: pick now (was warm) or next step
  void stepPrepPick(); void stepPick(); void stepLoad(); void stepPublish(); void stepPreset(); void stepAcidPat();
  void stepTheoryPat();                                // JobTheory: the track's source from the theory plan (pure, no card), then Publish
  bool setJob() const { return job_ == JobSet || job_ == JobTheory; }   // a set-wide job: CmdSetGenerating, the eight swaps held for one CmdSwapSet
  void theoryReplaced(int track);                      // a melodic track's source came from elsewhere: the set is no longer a theory set
  void stepPrepSlot(); void stepSlotPick(); void stepSlotOpen(); void stepSlotChunk(); void stepSlotPush(); void stepSlotWait();
  void enterSlot(int slot); void skipSlot(); const char* slotRole() const;
  void stepPrepVocal(); void stepVocalPick(); void stepVocalOpen();
  void stepPrepLoop(); void stepLoopPick(); void stepLoopOpen();   // JobLoop / a generate on a loop track (SPEC §9.19): prepare → pick → open
  void loopFail(LoopRefusal why);                      // the pick found nothing / the file cannot stream: loop mode off, lastError "NO LOOP: <FOLDER>"
  void trackDone(); void stepFinish(); void stepWaitSwap(); void finishJob();
  bool pushSetSwaps(int set);
  void retryDue();
  PubResult buildNext(int track, int variant, bool deferBeat);
  PubResult publishVariantEx(int track, int variant);
  bool pickPreset(int track, Category c);              // from the track's family: library presets, or the '303' family in acid mode
  bool isAcidTrack(int track) const { return info_[track].acid && acidCategory(trackCategory(track)); }
  bool rollSound(int track);                           // pick + makePatch into nextPatch, then pushSound (deferred to step() while nextPatch is in flight or the queue is full)
  bool pushSound(int track);                           // CmdSetPatch, plus the random knobs of an acid track (SPEC §9.17b); false = no queue room, nothing pushed
  // nextPatch(track) may be written only while no CmdSetPatch of it is in flight: the render task copies the struct
  // when it drains the command (two blocks after the push at the latest), so a second writer in that window would
  // hand it a torn patch. Pushes mark the track (patchInFlight_, patchClock_); rolls and preset picks wait for it.
  bool patchFree(int track);
  void pushKnobs(int track);                           // the knobCmdCount() CmdSetParam of randomizeKnobs (room checked by the caller)
  bool writeRush(int set);
  bool pushCmd(CmdType type, int track = 0, int a = 0, int32_t i = 0, float f = 0.0f);
  bool cmdRoom(int n) const;
  void notePushFail();
  void setErr(const char* what, const char* detail);
  // remembered source folders (SPEC §9.19 §3 b): a pick folder "<label>[/sNN]" (a key folder: "Amin", "C", "NA") or a kit
  // role folder "<role>[/sNN]" is kept as two bytes — the label (root, minor, NA) or the role index, and the shard number —
  // instead of its text; a folder that does not round-trip through the encoding stays unknown (the writer probes as before).
  void noteSource(int track, const char* path);        // loadMidiToSource: where the pattern came from
  static uint32_t nameHash(const char* name);
  static bool encodeFolder(const char* dir, bool kit, uint8_t& label, uint8_t& shard);   // "<label-or-role>[/sNN]" → bytes; false = not representable
  static bool decodeFolder(uint8_t label, uint8_t shard, bool kit, char* out, int cap);   // the inverse ("Amin/s03", "kick")
  bool composePath(uint8_t origin, const char* dir, uint8_t genre, const char* midBase, const char* file, char* out, int cap) const;
  // loop mode (SPEC §9.19)
  int trackStreamIdx(int track) const;                 // the stream a track in loop mode streams: its set (Vocal) or kSets + its slot; -1 none
  int streamTrack(int s) const;                        // the track a stream feeds: the set's Vocal track, or the slot's bound track (-1 free)
  int streamSet(int s) const { const int t = streamTrack(s); return t < 0 ? -1 : trackSet(t); }
  bool openLoop(int track, const char* path, bool force = false);   // the loop file into the track's stream as nxt (mode 1, the track's variant / len step); openErr_ says why not
  bool vocalReturn(int set);                           // the phrase a Vocal track returns to when its loop mode ends: one new-file open, deferred (vocalReturnDue_) while no handle is free
  bool retryVocalReturn();                             // step(): a deferred return whose handle is free now (true = an open was attempted: the step's card operation)
  void loopFields(int track, VStream& ns);             // nxt's variant / region / cycle from the track's info (or the 1-beat rush crop while rushing), and the info's natural / loaded
  PubResult loopVariant(int track, int variant);       // crop change on a streaming loop: same file, new cycle, switch on the next 1/16 (SPEC §9.19 §1.4)
  PubResult loopLen(int track, uint16_t len);          // len± on a streaming loop: same crop, new cycle
  void releaseSlot(int track);                         // loop mode off / a failed pick: the slot stream ends at the next bar (closing), the binding is dropped
  bool anyStreamWaiting(int set) const;
  bool onlyLoopJobs() const;                           // nothing but loop picks running / queued (a second toggle may queue)
  // stream producer (SPEC §9.3, §9.19)
  int pump(int budget);                                // every live stream, emptiest ring first; returns the chunk reads performed
  void pumpStream(int s, int& budget, bool topUp);     // topUp: refill to the top whatever the threshold (after a refused streamsHealthy())
  void fill(int s, VocalRing& ring, int& budget, bool topUp);
  int produce(VStream& st, uint32_t start, uint32_t S, int n, VocalRing& ring, uint32_t base, int& budget);
  bool fetch(VStream& st, uint32_t i, int16_t& a, int16_t& b, int& budget);
  static void resetStream(VStream& st);
  static void resetSet(VSet& v);
  bool openStream(VStream& st, int s, const char* path, int sel, bool loop = false, bool force = false);   // one file for stream s; force: skip the handle budget (a job that waited kOpenWaitSteps)
  void closeStreamFile(VStream& st, const VStream* keepIfShared);
  void setNext(int s, const VStream& ns, uint16_t grid = kTicksPerBar);
  int write(VSet& v, uint32_t S, int n, VocalRing& ring, uint32_t base, int& budget);   // cur / nxt by S against the switch, silence without a stream
  void scheduleSwitch(int s, uint32_t sc, const VocalRing& ring);
  uint32_t barLineOf(uint32_t idx) const;              // sample of bar line idx under clk_ (exact for idx0 / idx0 + 1, nominal beyond)
  uint32_t tickSample(uint32_t tick) const;            // sample of an absolute tick under clk_ (its bar line + the ticks into the bar)
  bool pushSwitchCmds(int s);
  void finalizeSwitch(int s);
  void rebase(int s, uint32_t base, uint32_t bar, uint16_t bpm, uint32_t sc, uint32_t start);
  void retime(int s, VocalRing& ring, uint32_t sc);    // tempo changed: carry the file position, rewrite what was written ahead
  static void carryFilePos(VStream& st, const BarClock& from, const BarClock& to, uint32_t P);
  static uint32_t cycleBarsOf(const VStream& st) { if (st.mode) return st.loopTicks >= kTicksPerBar ? (uint32_t)(st.loopTicks / kTicksPerBar) : 1u; return st.variant < 2 && st.loopTicks >= kTicksPerBar ? (uint32_t)(st.loopTicks / kTicksPerBar) : 1u; }
  static uint32_t stepFpOf(const VStream& st, uint16_t bpm);   // file samples per output sample, 16.16 (rate clamped 0.5..2, SPEC §9.3)
  void streamEmpty(int s);                             // no content for this stream: files closed, ring parked, present = 0 published
  void vocalEmpty(int set) { streamEmpty(set & 1); }
  PubResult vocalVariant(int set, int variant);
  bool streamConsumed(int s) const;                    // the engine reads this ring now (running, the bound track plays, the ring active)

  IFileSystem* fs_ = nullptr; Engine* eng_ = nullptr; Library* lib_ = nullptr;
  ProjectKey key_; int genre_ = 0;
  Rng rng_{12345};
  TrackGenInfo info_[kTracks];
  PresetBag presetBag_[kTracks];
  TheoryPlan theory_;                                  // SPEC §9.24: the running / last theory job's plan (52 B; the tables are flash)
  uint8_t theorySet_ = 0; int8_t theoryProg_[kSets] = {-1, -1};   // bit s: set s is a theory set; its progression index
  uint8_t foldMap_[kSets][128];
  Request queue_[kRequestQueue]; uint8_t queueN_ = 0;
  // current job
  State state_ = Idle; uint8_t job_ = JobNone, jobArg_ = 0; uint8_t set_ = 0, track_ = 0, trackIdx_ = 0;
  uint8_t doPat_ = 0, doSnd_ = 0; int done_ = 0, total_ = 0; bool sd_ = false;
  uint8_t slot_ = 0, slotEnd_ = 0, attempt_ = 0, phase_ = 0, percAlt_ = 0, genFlag_ = 0, resetDone_ = 0;
  uint32_t slotLen_ = 0; uint16_t prepSteps_ = 0; uint16_t pushFail_ = 0;
  uint8_t swapMask_[kSets] = {0, 0}; uint8_t maskDue_ = 0; uint8_t swapDue_ = 0;   // set swaps held back / the Beat mask + rush that go with them / a loader burst waiting for queue room
  uint8_t rushDue_ = 0; uint16_t variantDue_ = 0; uint8_t variantDueV_[kTracks];
  uint16_t soundDue_ = 0; uint32_t changes_ = 0;      // a re-rolled sound waiting for queue room (SPEC §9.17b); the session change counter (SPEC §9.18)
  uint16_t rollDue_ = 0, patchInFlight_ = 0; uint32_t patchClock_ = 0;   // acid re-rolls waiting for nextPatch; CmdSetPatch pushed at patchClock_ (see patchFree)
  uint32_t pushClock_ = 0; uint16_t ackTarget_ = 0;
  char path_[kMaxPathLen]; char err_[48] = ""; char note_[16] = "";
  IFile* file_ = nullptr; WavInfo wav_; UlawLoader loader_;
  // stream producer (SPEC §9.3 / §9.19)
  VSet stream_[kStreams]; uint8_t pumpTurn_ = 0; BarClock clk_;
  LoopRefusal loopRefusal_ = LoopOk; uint32_t (*freeHeap_)() = nullptr;   // SPEC §9.19
  uint16_t loopOn_ = 0; int8_t slotTrack_[kLoopSlots] = {-1, -1}; LoopTrack loopTk_[kTracks]; uint16_t modeOffDue_ = 0;   // loop mode requested per track; slot → track; parked clip; CmdLoopMode off waiting for queue room
  uint16_t loopRush_ = 0;                              // rushLoop: the half-length crop is streamed on these loop tracks (momentary, never persisted)
  uint8_t openErr_ = OpenOk; uint8_t openFails_[kStreams]; uint16_t openWait_ = 0;   // SPEC §9.19g: the last stream open's outcome, failed opens per stream in a row, steps the job waited for a handle
  uint8_t vocalReturnDue_ = 0;                         // bit s: set s's Vocal track left loop mode and its phrase still waits for a handle (step() retries, the loop streams meanwhile)
  char vocalLoopPath_[kSets][kMaxPathLen];             // the Vocal track's loop file (its stream's path / altPath_ keep the phrases it returns to)
  char altPath_[kSets][kMaxPathLen];                   // the vocal streams' alternate phrase (SPEC §5 variant 2); the loop slots have none
  mutable uint8_t gateWait_ = 0;                       // streamsHealthy() said no: the next pump tops every ring up
  // remembered source folders (SPEC §9.19 §3 b): origin 0 unknown, 1 the library (/lp/midi/<Cat>/<label>[/sNN], /lp/packs/<genre>/drum/<role>[/sNN]), 2 <genre>/presets/assets, 3 /lp/state
  uint8_t srcOrigin_[kTracks]; uint8_t srcGenre_[kTracks]; uint32_t srcHash_[kTracks]; uint8_t srcLabel_[kTracks]; uint8_t srcShard_[kTracks];
  uint8_t kitOrigin_[kSets][kSampleSlots]; uint8_t kitGenre_[kSets][kSampleSlots]; uint32_t kitHash_[kSets][kSampleSlots]; uint8_t kitRole_[kSets][kSampleSlots]; uint8_t kitShard_[kSets][kSampleSlots];
  uint8_t iterChunks_ = 0; int lastPumpChunks_ = 0; uint32_t pumpChunkTotal_ = 0, stepsSkipped_ = 0, pumpClock_ = 0;
  VStream* winStream_ = nullptr; uint32_t winStart_ = 0, winEnd_ = 0; int16_t winPrev_ = 0; uint8_t winHavePrev_ = 0;
  int16_t chunk_[kChunkSamples];
};

} // namespace lp
