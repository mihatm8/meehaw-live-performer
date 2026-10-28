// meehaw live performer — real-time engine (SPEC §5, §9). Owned by the render task; the UI/loop task talks to it
// through the command queue, the "next" buffers (written under beginNextWrite/endNextWrite) and the staging buffer.
#pragma once
#include <atomic>
#include "../core/config.h"
#include "../core/types.h"
#include "../core/rt.h"
#include "../dsp/synth_voice.h"
#include "../dsp/sample_voice.h"
#include "../dsp/echo.h"
#include "../dsp/reverb.h"
#include "../dsp/chorus.h"
#include "../dsp/clip.h"
#include "../dsp/svf.h"

namespace lp {

// Stream ring (SPEC §9.3 vocal streams, §9.19 loop slots): the loop task writes resampled int16 samples that correspond
// to absolute sample indices; the engine reads sequentially. Producer and consumer agree on the mapping through
// baseSample. The vocal rings are a power of two (kVocalRingBytes / 2); a loop slot's ring is whatever the host handed
// in (kLoopRingBytes / 2 = 3072, not a power of two), so a ring index is reduced with wrap(), never with `cap - 1`.
struct VocalRing {
  int16_t* buf = nullptr; int cap = 0;                       // samples (any size ≥ 2; see wrap)
  uint32_t mask = 0;                                         // cap − 1 when cap is a power of two, else 0 (wrap() then divides)
  std::atomic<uint32_t> w{0}, r{0};                          // monotonically increasing sample counters
  std::atomic<uint32_t> baseSample{0};                       // absolute sample index of ring position w==r==0 (set by producer on (re)start)
  std::atomic<uint8_t> active{0};                            // producer: 1 while a phrase is loaded
  int available() const { return (int)(w.load(std::memory_order_acquire) - r.load(std::memory_order_relaxed)); }
  int space() const { return cap - (int)(w.load(std::memory_order_relaxed) - r.load(std::memory_order_acquire)); }
  void reset(uint32_t base) { r.store(0); w.store(0); baseSample.store(base); }
  uint32_t wrap(uint32_t i) const { return mask ? (i & mask) : (cap > 0 ? i % (uint32_t)cap : 0u); }   // ring position of sample counter i
  void setBuffer(int16_t* b, int n) { buf = n >= 2 ? b : nullptr; cap = n >= 2 ? n : 0; mask = (cap > 0 && (cap & (cap - 1)) == 0) ? (uint32_t)cap - 1 : 0; }
};

struct VocalState {                                        // per set, published with CmdVocalLoaded, applied at the bar line
  float gain = 1.0f;
  uint8_t present = 0;
  uint16_t loopTicks = 0;                                  // informational for the UI (producer decides positions)
  float fileBpm = 0;
};

// A parameter ramp (SPEC §9.12, §9.17a). `from` is the interpolation start: the value at the latest CmdRamp of this
// track / param, so a re-target (SHIFT + the other key, hold-ramp reversed, drift re-aimed) continues from where the
// parameter is instead of jumping back. `origin` is the value before the FIRST CmdRamp, kept across re-targets:
// CmdCancelRamps (SHIFT release) snaps there.
struct Ramp { uint8_t active = 0; uint8_t track = 0; uint8_t param = 0; float from = 0, to = 0, origin = 0; uint32_t startTick = 0, ticks = 1; };

// Optional microsecond clock for profiling (set by the platform; nullptr = no profiling).
extern uint32_t (*g_microsHook)();
// Optional second-core worker (SPEC §9.13 CPU): start runs fn(arg) on another core, join waits for it. Both or neither.
typedef void (*ParallelFn)(void*);
extern void (*g_parallelStart)(ParallelFn fn, void* arg);
extern void (*g_parallelJoin)();

class Engine {
 public:
  void init();                                               // wires static buffers, no allocation
  void render(int16_t* out, int n);                          // render task only; n <= kBlock

  // ---- producer side (loop task) ----
  CmdQueue& cmds() { return cmds_; }
  EvQueue& events() { return events_; }
  const EngineStatus& status() const { return status_; }

  // Pattern hand-off (SPEC §9.5): write next, then push CmdSwapPattern (or CmdSwapSet for a whole set).
  bool beginNextWrite(int track) { uint8_t z = 0; return nextLock_[track].compare_exchange_strong(z, 1); }
  Pattern& nextPattern(int track) { return next_[track]; }
  void endNextWrite(int track) { nextLock_[track].store(0, std::memory_order_release); }
  Pattern& sourcePattern(int track) { return source_[track]; }     // UI-owned original, never read by the engine
  Pattern& rushPattern(int set) { return rush_[set]; }             // UI writes at generate time (rush must be off)
  const Pattern& activePattern(int track) const { return active_[track]; }  // read-only peek for the editor display

  SynthPatch& nextPatch(int track) { return nextPatch_[track]; }   // fill, then push CmdSetPatch (immediate)
  uint8_t* staging() { return staging_; }                          // fill ≤ kStagingBytes, then push CmdSlotSwap{set, slot}
  const SampleKit& kit(int set) const { return kit_[set]; }
  void setKitName(int set, int slot, const char* name) {           // loop task; display only (inline: gen links without engine.o)
    if (set < 0 || set >= kSets || slot < 0 || slot >= kSampleSlots) return;
    strncpy(kit_[set].name[slot], name ? name : "", sizeof kit_[set].name[slot] - 1);
    kit_[set].name[slot][sizeof kit_[set].name[slot] - 1] = 0;
  }
  VocalRing& vocalRing(int set) { return vocal_[set]; }
  VocalState& nextVocal(int set) { return nextVocal_[set]; }       // fill, then CmdVocalLoaded
  // ---- loop mode (SPEC §9.19): loop slots are streams like the vocal ones; the ring memory comes from the host (bootBlock on the device, static arrays on the host) ----
  void setLoopRing(int slot, int16_t* mem, int samples);        // samples: a power of two (kLoopRingBytes / 2); call before the first render
  VocalRing& loopRing(int slot) { return loop_[slot]; }
  VocalRing& streamRing(int stream) { return stream < kSets ? vocal_[stream] : loop_[stream - kSets]; }
  VocalState& nextLoop(int slot) { return nextLoop_[slot]; }      // fill, then CmdLoopLoaded{a = slot}
  const VocalState& loopState(int slot) const { return loopState_[slot]; }
  int loopTrack(int slot) const { return loopTrack_[slot]; }      // track bound to the slot by CmdLoopMode (applied at the bar), -1 free
  bool trackStreams(int track) const { return trackStream_[track] >= 0; }   // the track renders a stream (loop mode on, incl. the Vocal track's own stream)
  int trackStream(int track) const { return trackStream_[track]; }          // stream index, -1 none
  uint16_t streamUnderruns(int stream) const { return stream < kSets ? vocalUnderruns_[stream] : loopUnderruns_[stream - kSets]; }
  const TrackParams& params(int track) const { return target_[track]; }
  const SynthKnobs& knobs(int track) const { return knobs_[track]; }   // effective (smoothed) synth knobs, display only
  uint32_t samplesPerTickFp() const { return samplesPerTickFp_; }  // 16.16

 private:
  // ---- shared state ----
  CmdQueue cmds_; EvQueue events_; EngineStatus status_;
  Pattern source_[kTracks], next_[kTracks], active_[kTracks];
  std::atomic<uint8_t> nextLock_[kTracks];
  uint8_t pendingSwap_[kTracks]; uint8_t pendingVariant_[kTracks]; uint16_t pendingLen_[kTracks]; uint8_t pendingSetSwap_[kSets];
  uint16_t queuedLen_[kTracks];                                    // CmdSetLength waiting for the bar
  Pattern rush_[kSets];
  SynthPatch patch_[kTracks], nextPatch_[kTracks];
  SampleKit kit_[kSets];
  uint8_t pool_[kSets][kPoolBytesPerSet];
  uint8_t staging_[kStagingBytes];
  VocalRing vocal_[kSets]; int16_t vocalMem_[kSets][kVocalRingBytes / 2];
  VocalState vocalState_[kSets], nextVocal_[kSets]; uint8_t pendingVocal_[kSets];
  VocalRing loop_[kLoopSlots]; VocalState loopState_[kLoopSlots], nextLoop_[kLoopSlots]; uint8_t pendingLoop_[kLoopSlots] = {0, 0};   // SPEC §9.19 loop slots (ring memory from setLoopRing)
  int8_t loopTrack_[kLoopSlots] = {-1, -1}; int8_t trackStream_[kTracks]; uint16_t loopUnderruns_[kLoopSlots] = {0, 0};
  // CmdLoopMode waits for the bar line (0 none, 1 on, 2 off; the stream it names), and the parked pattern's clip length /
  // variant / emptiness come back on off. The kick cut on a loop Beat track raises the strip filter to kLoopKickCutFilter
  // and restores what it replaced (per set: applied flag + the saved filter target), SPEC §9.19.
  uint8_t pendingLoopMode_[kTracks]; int8_t pendingLoopStream_[kTracks]; uint16_t parkedLen_[kTracks]; uint8_t parkedVariant_[kTracks]; uint16_t parkedEmpty_ = 0;
  uint8_t loopCutApplied_[kSets] = {0, 0}; float loopCutSavedFilter_[kSets] = {0, 0};
  TrackParams target_[kTracks], current_[kTracks];                 // smoothed each block
  SynthKnobs knobs_[kTracks];                                      // derived from current_ knobs each block; voices point here
  float distDrive_[kTracks], distMakeup_[kTracks];                 // strip distortion of current_.dist (SPEC §9.17a), re-derived when it moves
  Ramp ramps_[kMaxRamps];
  // ---- clock ----
  uint32_t tick_ = 0; uint32_t tickAccFp_ = 0; uint32_t samplesPerTickFp_ = 0; float bpm_ = kDefaultBpm;
  uint32_t sampleClock_ = 0; bool running_ = true;
  uint32_t barSample_ = 0;                                   // exact first sample of the latest bar line (published as status.barSample)
  // ---- per track runtime ----
  struct TrackRt {
    PlayState state = PSEmpty; uint16_t len = kTicksPerBar; uint16_t cursor = 0; uint16_t lastPos = 0xFFFF;
    uint8_t variant = 0; uint8_t nextVariant = 3; uint8_t mask = 0x1F; uint8_t nextMask = 0x1F; bool kickCut = false; bool rush = false; bool rushPending = false; bool resume = false;
    uint32_t startSample = 0; float level = 0;
    uint8_t noteOnPitch[8]; uint32_t noteEndTick[8]; uint8_t noteVoice[8]; uint8_t noteCount = 0;   // sounding synth notes (absolute end ticks)
    uint8_t rushCursor = 0;
  } rt_[kTracks];
  // ---- voices ----
  SynthVoice synth_[kSets][kSynthVoicesPerSet]; uint8_t synthTrack_[kSets][kSynthVoicesPerSet];      // owner track (0..7 in set) or 255
  SampleVoice sample_[kSets][kSampleVoicesPerSet];           // two per slot: voice 2*slot and 2*slot+1
  uint8_t sampleToggle_[kSets][kSampleSlots]; bool sampleAccent_[kSets][kSampleVoicesPerSet];
  float slotGain_[kSets][kSampleSlots];
  int8_t pendingSlotSet_ = -1, pendingSlotIdx_ = 0; uint32_t pendingSlotLen_ = 0;
  uint8_t swapDue_[kTracks];
  int curOffset_ = 0;                                       // sample offset inside the current block (exact bar sample for startSample)
  float duckBuf_[kSets][kBlock];
  uint32_t profSynth_[kSets] = {0, 0}, profSample_[kSets] = {0, 0}, profStrip_[kSets] = {0, 0}, profBus_ = 0;
  struct ChunkJob { Engine* self; int n; int offset; } job_;
  static void renderSetJob(void* arg);
  // ---- DSP ----
  float trackBuf_[kSets][kBlock];                            // one track at a time, per rendering core
  float mix_[kBlock], echoSend_[kBlock], revSend_[kBlock], wet_[kBlock];
  float mix2_[kBlock], echoSend2_[kBlock], revSend2_[kBlock]; // set 2 accumulators (rendered on the worker core)
  uint16_t vocalUnderruns_[kSets] = {0, 0};
  uint8_t echoMem_[kEchoSamples]; int16_t revMem_[Reverb::kMemory];      // the echo line is µ-law (SPEC §9.19 static reclaim: −10 240 B)
  Echo echo_; Reverb reverb_; Svf trackFilter_[kTracks]; float limGain_[kTracks]; Ducker duck_[kSets];
  Chorus chorus_[kSets];                                     // per-set chorus bus (SPEC §9.22): the tracks' PChorus sends accumulate into its line, the wet comes back into the set's mix
  Svf gyroFilter_; float gyroFilterTarget_ = 0, gyroFilterCur_ = 0, gyroRev_ = 0, gyroEcho_ = 0, gyroRevCur_ = 0, gyroEchoCur_ = 0; bool gyroOn_ = false;
  float lookahead_[kLookaheadSamples]; int laPos_ = 0; float masterGain_ = 1.0f;
  // ---- helpers ----
  void handleCmd(const Cmd& c);
  void setTempo(float bpm);
  void onBarLine();
  void onTick();
  void applySwap(int track);
  void fireNoteOn(int track, const Note& n);
  void fireNoteOff(int track, uint8_t pitch);
  void triggerSample(int set, int slot, float vel, bool accent);
  void playBeatPitch(int set, uint8_t pitch, float vel);          // Beat note contract (SPEC §9.6) after the layer mask: kick cut, duck, EvKick, slots
  void performNote(int track, uint8_t pitch, float vel);          // CmdPerformNote / CmdPerformChord on any track (SPEC §9.17a)
  void resetTrackParams(int track);                                // category defaults + knobs from the track's patch
  void deriveKnobs(int track);                                     // knobs_[track] from current_[track] (mappings in types.h)
  int allocSynth(int set, int trackInSet, int maxVoices, uint8_t pitch, bool legato);
  void renderTrack(int track, int n, int offset);
  void renderSet(int set, int n, int offset);
  void renderVocal(int set, int n, int offset, float* dst) { renderStream(set, n, offset, dst); }
  void renderStream(int stream, int n, int offset, float* dst);   // a vocal (0 / 1) or loop (kSets + slot) ring into dst for the chunk at sampleClock_ + offset: gain, sequential read, underrun count (SPEC §9.3 / §9.19)
  void applyLoopMode(int track);                                   // the pending CmdLoopMode of a track, at the bar line
  void releaseTrackVoices(int track);                              // synth noteOff / sample fade of every voice the track owns (loop mode on)
  void setLoopKickCut(int set, bool on);                           // loop Beat track: strip filter to +0.6 while the cut is on, restored after
  uint16_t clipCap(int track) const;                               // kMaxVocalClipTicks (Vocal), kMaxLoopTicks (a streaming track), else kMaxPatternTicks
  void smoothParams();
  void serviceRamps();
  void updateStatus();
};

} // namespace lp
