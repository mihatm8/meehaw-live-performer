// Real-time hot path: compile at -O2 on the target (the Arduino core defaults to -Os).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("O2")
#endif
// meehaw live performer — real-time engine implementation (SPEC §5, §9).
#include "engine.h"
#include <math.h>
#include <string.h>

namespace lp {

uint32_t (*g_microsHook)() = nullptr;
void (*g_parallelStart)(ParallelFn, void*) = nullptr;
void (*g_parallelJoin)() = nullptr;

namespace {
// Synth voice ranges per track-in-set: Chord Lead Pad Bass Fx (Perc, Vocal, Beat have none).
constexpr int kVoiceRange[kTracksPerSet][2] = {{0, 4}, {8, 10}, {4, 8}, {10, 12}, {12, 16}, {0, 0}, {0, 0}, {0, 0}};
constexpr float kMasterTrim = 0.28f;             // −11 dB (SPEC §9.11; lowered after the first listening test)
constexpr int kSixteenth = kPPQN / 4;            // 24 ticks
constexpr float kLoopKickCutFilter = 0.6f;       // SPEC §9.19: the kick cut on a loop Beat track is this strip filter value (high-pass) while the cut is on

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int16_t saturate(float x) { x *= 32767.0f; return x > 32767.f ? 32767 : x < -32768.f ? -32768 : (int16_t)x; }
uint32_t slotCapSamples(int slot) {
  static const int caps[kSampleSlots] = {kCapKickMs, kCapSnareMs, kCapHhMs, kCapOhMs, kCapPercMs, kCapPercMs, kCapPercMs, kCapPercMs};
  return (uint32_t)caps[slot] * kSampleRate / 1000;
}
uint32_t slotOffset(int slot) { uint32_t o = 0; for (int i = 0; i < slot; i++) o += slotCapSamples(i); return o; }
uint8_t beatBit(uint8_t pitch) { if (pitch >= 36 && pitch <= 39) return (uint8_t)(1u << (pitch - 36)); if (pitch >= 40 && pitch <= 58) return 0x10; return 0; }
float filterCutoff(float f) { return f < 0 ? 12000.0f * powf(2.0f, f * 7.2f) : 80.0f * powf(2.0f, f * 7.2f); }   // −1 → 80 Hz LP, +1 → 11.7 kHz HP
// Synth knobs from a patch (SPEC §9.17a): the knobs always describe the sound, so CmdSetPatch / reset write the
// patch's values through the inverse mappings of types.h (cutoff and decay log, resonance / 0.985, env / 4 octaves).
void knobsFromPatch(const SynthPatch& p, TrackParams& tp) {
  tp.cutoff = knobFromCutoffHz(p.cutoff);
  tp.resonance = clampf(p.resonance / 0.985f, 0.f, 1.f);
  tp.envAmt = clampf(p.filterEnv * 0.25f, 0.f, 1.f);
  tp.decay = knobFromDecayS(p.fDecay);
  tp.drive = clampf(p.drive, 0.f, 1.f);
  tp.wave = p.wave1 < WaveCount ? p.wave1 : (uint8_t)WSaw;
  tp.sat = clampf(p.sat, 0.f, 1.f); tp.accent = clampf(p.accent, 0.f, 1.f);   // SPEC §9.22: the 303 character of the patch (0 / 0 for library presets)
}
void copyKnobFields(const TrackParams& from, TrackParams& to) {   // the eight knob fields only; the strip (and the chorus send) keeps smoothing
  to.cutoff = from.cutoff; to.resonance = from.resonance; to.envAmt = from.envAmt; to.decay = from.decay; to.drive = from.drive; to.wave = from.wave;
  to.sat = from.sat; to.accent = from.accent;
}
float paramValue(const TrackParams& p, uint8_t id) {               // CmdRamp start value of a track parameter
  switch (id) {
    case PVolume: return p.volume; case PReverb: return p.reverb; case PEcho: return p.echo; case PDist: return p.dist; case PFilter: return p.filter;
    case PCutoff: return p.cutoff; case PResonance: return p.resonance; case PEnvAmt: return p.envAmt; case PDecay: return p.decay; case PDrive: return p.drive;
    case PWave: return (float)p.wave;
    case PSat: return p.sat; case PAccent: return p.accent; case PChorus: return p.chorus;
    default: return 0.0f;
  }
}
// One-pole step towards the target; snaps once within 1e-4 so a settled parameter costs one compare per block and
// never re-derives anything. Returns true when cur moved.
inline bool approach(float& cur, float tg, float c) {
  const float d = tg - cur;
  if (d > 1e-4f || d < -1e-4f) { cur += d * c; return true; }
  if (d != 0.f) { cur = tg; return true; }
  return false;
}
} // namespace

// ---------------------------------------------------------------------------------------------- init

void Engine::init() {
  memset(pendingSwap_, 0, sizeof pendingSwap_); memset(pendingVariant_, 0, sizeof pendingVariant_); memset(pendingLen_, 0, sizeof pendingLen_);
  memset(pendingSetSwap_, 0, sizeof pendingSetSwap_); memset(queuedLen_, 0, sizeof queuedLen_); memset(swapDue_, 0, sizeof swapDue_);
  memset(pendingVocal_, 0, sizeof pendingVocal_); memset(sampleToggle_, 0, sizeof sampleToggle_); memset(sampleAccent_, 0, sizeof sampleAccent_);
  for (int t = 0; t < kTracks; t++) {
    nextLock_[t].store(0); source_[t].clear(); next_[t].clear(); active_[t].clear();
    patch_[t] = SynthPatch(); nextPatch_[t] = SynthPatch(); rt_[t] = TrackRt();
    target_[t] = defaultTrackParams(trackCategory(t)); knobsFromPatch(patch_[t], target_[t]); current_[t] = target_[t]; deriveKnobs(t);
    distDrive_[t] = 1.0f; distMakeup_[t] = 1.0f;
    trackFilter_[t].set(1000.f, 0.25f, 0); limGain_[t] = 1.0f;
    trackStream_[t] = -1; pendingLoopMode_[t] = 0; pendingLoopStream_[t] = -1; parkedLen_[t] = kTicksPerBar; parkedVariant_[t] = 0;   // SPEC §9.19: no track streams
  }
  parkedEmpty_ = 0;
  for (int s = 0; s < kSets; s++) {
    rush_[s].clear(kTicksPerBeat); kit_[s] = SampleKit(); vocalState_[s] = VocalState(); nextVocal_[s] = VocalState();
    vocal_[s].setBuffer(vocalMem_[s], kVocalRingBytes / 2); vocal_[s].reset(0); vocal_[s].active.store(0);
    loopCutApplied_[s] = 0; loopCutSavedFilter_[s] = 0.0f;
    duck_[s].init();
    for (int v = 0; v < kSynthVoicesPerSet; v++) { synth_[s][v].init(); synth_[s][v].setPatch(&patch_[s * kTracksPerSet]); synth_[s][v].setKnobs(&knobs_[s * kTracksPerSet]); synthTrack_[s][v] = 255; }
    for (int k = 0; k < kSampleSlots; k++) { slotGain_[s][k] = 1.0f; kit_[s].slot[k].offset = slotOffset(k); }
    chorus_[s].init();
  }
  for (int k = 0; k < kLoopSlots; k++) {                   // loop slots (SPEC §9.19): the ring memory stays as setLoopRing handed it in (either call order works)
    loop_[k].reset(0); loop_[k].active.store(0); loopState_[k] = VocalState(); nextLoop_[k] = VocalState(); pendingLoop_[k] = 0;
    loopTrack_[k] = -1; loopUnderruns_[k] = 0;
  }
  for (int i = 0; i < kMaxRamps; i++) ramps_[i] = Ramp();
  echo_.init(echoMem_, kEchoSamples); reverb_.init(revMem_); gyroFilter_.set(1000.f, 0.3f, 0);
  for (int i = 0; i < kLookaheadSamples; i++) lookahead_[i] = 0;
  laPos_ = 0; masterGain_ = 1.0f;
  pendingSlotSet_ = -1;
  setTempo((float)kDefaultBpm);
  tick_ = 0; tickAccFp_ = 0; sampleClock_ = 0; running_ = true;
  updateStatus();
}

void Engine::setTempo(float bpm) {
  bpm_ = clampf(bpm, (float)kMinBpm, (float)kMaxBpm);
  float spt = (float)kSampleRate * 60.0f / (bpm_ * (float)kPPQN);
  samplesPerTickFp_ = (uint32_t)(spt * 65536.0f);
  echo_.setTempo(bpm_);
}

// Loop slot ring memory (SPEC §9.19): handed in by the host once at boot (bootBlock on the device, static arrays on the
// host). The ring takes the size as given — kLoopRingBytes / 2 = 3072 samples is not a power of two, so ring indices go
// through VocalRing::wrap (a mask for the power-of-two vocal rings, a division otherwise); the contract's 139 ms ring and
// 93 ms worst-case margin (1024-sample refills) hold. Without memory the slot stays inactive: the producer has nowhere to
// write and the engine renders silence for it, counting nothing.
void Engine::setLoopRing(int slot, int16_t* mem, int samples) {
  if (slot < 0 || slot >= kLoopSlots) return;
  VocalRing& r = loop_[slot];
  r.active.store(0, std::memory_order_release);
  r.setBuffer(mem, samples); r.reset(0);
}

uint16_t Engine::clipCap(int track) const {                 // the clip length a track may hold (SPEC §9.4 / §9.16 / §9.19)
  if (trackCategory(track) == Vocal) return kMaxVocalClipTicks;   // a vocal cycle may span 20 bars: a 16-bar window + pause (SPEC §9.16 / §9.18.1)
  return trackStream_[track] >= 0 ? kMaxLoopTicks : kMaxPatternTicks;   // a streaming track plays a loop of up to kMaxLoopBars bars; patterns stay 16 beats
}

// ---------------------------------------------------------------------------------------------- commands

void Engine::handleCmd(const Cmd& c) {
  int t = c.track < kTracks ? c.track : 0; TrackRt& r = rt_[t]; int set = c.a & 1;
  switch (c.type) {
    case CmdTrackToggle:
      if (r.state == PSStopped || (r.state == PSEmpty && pendingSwap_[t])) r.state = PSQueued; else if (r.state == PSQueued) r.state = PSStopped;
      else if (r.state == PSPlaying) r.state = PSStopQueued; else if (r.state == PSStopQueued) r.state = PSPlaying;
      break;
    case CmdTrackStart: if (r.state == PSStopped || (r.state == PSEmpty && pendingSwap_[t])) r.state = PSQueued; else if (r.state == PSStopQueued) r.state = PSPlaying; break;
    case CmdTrackStop: if (r.state == PSPlaying) r.state = PSStopQueued; else if (r.state == PSQueued) r.state = PSStopped; break;
    case CmdTrackStartNow: if (r.state != PSEmpty) { r.state = PSPlaying; r.startSample = sampleClock_; r.lastPos = 0xFFFF; } break;
    case CmdTrackStopNow: if (r.state != PSEmpty) { r.state = PSStopped; for (int i = 0; i < r.noteCount; i++) fireNoteOff(t, r.noteOnPitch[i]); r.noteCount = 0; } break;
    case CmdCancelPending: if (r.state == PSQueued) r.state = PSStopped; else if (r.state == PSStopQueued) r.state = PSPlaying; break;
    case CmdStopSet: for (int i = 0; i < kTracksPerSet; i++) { TrackRt& x = rt_[set * kTracksPerSet + i]; if (x.state == PSPlaying) x.state = PSStopQueued; else if (x.state == PSQueued) x.state = PSStopped; } break;
    case CmdPlaySet: for (int i = 0; i < kTracksPerSet; i++) { int tt = set * kTracksPerSet + i; TrackRt& x = rt_[tt]; if (x.state == PSStopped || (x.state == PSEmpty && pendingSwap_[tt])) x.state = PSQueued; } break;
    case CmdMasterPlay:
      if (c.a == 0) {
        running_ = false;
        for (int i = 0; i < kTracks; i++) { TrackRt& x = rt_[i]; x.rush = false; x.rushPending = false; x.resume = (x.state == PSPlaying || x.state == PSQueued); if (x.state != PSEmpty) x.state = PSStopped; for (int k = 0; k < x.noteCount; k++) fireNoteOff(i, x.noteOnPitch[k]); x.noteCount = 0; }
      } else if (!running_) {
        running_ = true; tick_ = ((tick_ + kTicksPerBar - 1) / kTicksPerBar) * kTicksPerBar; tickAccFp_ = 0; barSample_ = sampleClock_;   // the bar resumes at this block start
        for (int i = 0; i < kTracks; i++) { TrackRt& x = rt_[i]; if (x.resume) { x.resume = false; x.state = PSPlaying; x.startSample = sampleClock_; x.lastPos = 0xFFFF; } }
      }
      break;
    case CmdSetTempo:                                                     // tempo± wins over a running drift: the PTempo ramp would re-pin bpm_ next block (SPEC §9.17a)
      for (int i = 0; i < kMaxRamps; i++) if (ramps_[i].active && ramps_[i].param == PTempo) ramps_[i].active = 0;
      setTempo((float)c.i);
      break;
    case CmdSetParam: {
      TrackParams& p = target_[t];
      switch (c.a) {
        case PVolume: p.volume = clampf(c.f, 0, 1); break; case PReverb: p.reverb = clampf(c.f, 0, 1); break; case PEcho: p.echo = clampf(c.f, 0, 1); break;
        case PDist: p.dist = clampf(c.f, 0, 1); break; case PFilter: p.filter = clampf(c.f, -1, 1); break;
        // Synth knobs (SPEC §9.17a), normalised 0..1; the wave is an index, stepped (rounded, clamped to the table set).
        case PCutoff: p.cutoff = clampf(c.f, 0, 1); break; case PResonance: p.resonance = clampf(c.f, 0, 1); break; case PEnvAmt: p.envAmt = clampf(c.f, 0, 1); break;
        case PDecay: p.decay = clampf(c.f, 0, 1); break; case PDrive: p.drive = clampf(c.f, 0, 1); break;
        case PWave: { int w = (int)(c.f + 0.5f); p.wave = (uint8_t)(w < 0 ? 0 : w >= WaveCount ? WaveCount - 1 : w); break; }
        // SPEC §9.22: filter-loop saturation and accent depth (knobs), chorus send (strip send, smoothed like echo / reverb).
        case PSat: p.sat = clampf(c.f, 0, 1); break; case PAccent: p.accent = clampf(c.f, 0, 1); break; case PChorus: p.chorus = clampf(c.f, 0, 1); break;
        default: break;
      }
      break; }
    case CmdSwapPattern: pendingSwap_[t] = 1; pendingVariant_[t] = c.a; pendingLen_[t] = (uint16_t)c.i; break;
    case CmdSwapPatternNow: pendingSwap_[t] = 1; pendingVariant_[t] = c.a; pendingLen_[t] = (uint16_t)c.i; applySwap(t); if (pendingSwap_[t]) swapDue_[t] = 1; break;
    case CmdSwapSet: pendingSetSwap_[set] = 1; break;
    case CmdSetLength: queuedLen_[t] = (uint16_t)c.i; break;                 // clamped to clipCap(track) at the bar (kMaxLoopTicks on a streaming track, SPEC §9.19)
    case CmdSetGenerating: status_.generating[set] = (uint8_t)c.i; break;
    case CmdBeatMask: { TrackRt& b = rt_[set * kTracksPerSet + Beat]; b.nextMask = (uint8_t)(c.i & 0x1F); b.nextVariant = (uint8_t)((c.i >> 8) & 3); break; }
    case CmdSnareRush: { TrackRt& b = rt_[set * kTracksPerSet + Beat]; if (c.i) { if (!b.rush) b.rushPending = true; } else { b.rushPending = b.rush; } break; }
    case CmdKickCut: {
      const int beat = set * kTracksPerSet + Beat; const bool on = c.i != 0; rt_[beat].kickCut = on;
      if (trackStream_[beat] >= 0 || loopCutApplied_[set]) setLoopKickCut(set, on);   // SPEC §9.19: on a loop Beat track the cut is the strip filter at +0.6 (the stream has no note 36 to mute)
      break; }
    case CmdSetPatch:                                                     // the knobs follow the new sound at once (target and current: no glide from the old patch)
      patch_[t] = nextPatch_[t]; knobsFromPatch(patch_[t], target_[t]); copyKnobFields(target_[t], current_[t]); deriveKnobs(t);
      break;
    case CmdSlotSwap: {
      int slot = c.i & 0xF; uint32_t len = (uint32_t)c.i >> 4; if (slot >= kSampleSlots) break;
      if (len > slotCapSamples(slot)) len = slotCapSamples(slot);
      sample_[set][slot * 2].release(kSlotFadeSamples); sample_[set][slot * 2 + 1].release(kSlotFadeSamples);   // 64-sample fade before the deferred copy (SPEC §9.1)
      SampleSlot& s = kit_[set].slot[slot]; s.offset = slotOffset(slot); s.length = 0; s.present = 0; s.gainDb = (int8_t)c.f;
      slotGain_[set][slot] = powf(10.0f, c.f / 20.0f);
      pendingSlotSet_ = (int8_t)set; pendingSlotIdx_ = (int8_t)slot; pendingSlotLen_ = len;    // copied at the next block start
      break; }
    case CmdVocalLoaded: pendingVocal_[set] = 1; break;
    case CmdLoopLoaded: if (c.a < kLoopSlots) pendingLoop_[c.a] = 1; break;         // nextLoop(slot) is adopted at the bar line, like CmdVocalLoaded (SPEC §9.19)
    case CmdLoopMode: {                                                              // applied at the bar line (applyLoopMode); the last request before the bar wins
      if (c.a) {
        // The Vocal track always streams its own set's vocal stream (its loop is fed through that ring, no slot); any other
        // track binds a loop slot (kSets + slot): a vocal ring has one reader, the Vocal track, and never gets a second one.
        const int s = trackCategory(t) == Vocal ? t / kTracksPerSet : (int)c.i;
        if (s < 0 || s >= kStreams || (trackCategory(t) != Vocal && s < kSets)) break;
        pendingLoopMode_[t] = 1; pendingLoopStream_[t] = (int8_t)s;
      } else pendingLoopMode_[t] = 2;
      break; }
    case CmdGyro: gyroOn_ = c.a != 0; gyroFilterTarget_ = gyroOn_ ? clampf(c.f, -1, 1) : 0; gyroRev_ = gyroOn_ ? ((c.i >> 8) & 0xFF) / 255.0f : 0; gyroEcho_ = gyroOn_ ? (c.i & 0xFF) / 255.0f : 0; break;
    case CmdEchoDivision: echo_.setDivision((EchoDiv)(c.i % EchoDivCount)); echo_.setTempo(bpm_); break;
    case CmdDuck: for (int s = 0; s < kSets; s++) duck_[s].enabled = c.a != 0; break;
    case CmdPerformNote: case CmdPerformChord: performNote(t, (uint8_t)(c.i < 0 ? 0 : c.i > 127 ? 127 : c.i), c.f); break;
    case CmdRamp: {
      int idx = -1, free = -1;
      for (int i = 0; i < kMaxRamps; i++) { if (ramps_[i].active && ramps_[i].track == t && ramps_[i].param == c.a) idx = i; else if (!ramps_[i].active && free < 0) free = i; }
      // The interpolation always starts from the parameter's value NOW (bpm_ for the tempo, the target otherwise, which a
      // running ramp wrote last block and a CmdSetParam step may just have moved): a re-target never jumps back to
      // the stale start. `origin` (the value before the first ramp) is only taken on a fresh ramp, for the SHIFT snap-back.
      const float now = c.a == PTempo ? bpm_ : paramValue(target_[t], c.a);
      if (idx < 0) { if (free < 0) break; idx = free; ramps_[idx].origin = now; }
      Ramp& rp = ramps_[idx]; rp.active = 1; rp.track = (uint8_t)t; rp.param = c.a; rp.from = now; rp.to = c.f; rp.startTick = tick_; rp.ticks = c.i > 0 ? (uint32_t)c.i : 1;
      break; }
    case CmdCancelRamps:                                                  // SHIFT release: every strip / knob ramp snaps back to the value before its first ramp; a tempo drift keeps going (SPEC §9.17a)
      for (int i = 0; i < kMaxRamps; i++) if (ramps_[i].active && ramps_[i].param != PTempo) { Ramp& rp = ramps_[i]; rp.active = 0; Cmd s; s.type = CmdSetParam; s.track = rp.track; s.a = rp.param; s.f = rp.origin; handleCmd(s); }
      break;
    case CmdFreezeRamp:                                                   // hold-ramp released without SHIFT: the ramp stops, the target stays where the ramp left it
      for (int i = 0; i < kMaxRamps; i++) if (ramps_[i].active && ramps_[i].param == c.a && (c.a == PTempo || ramps_[i].track == t)) ramps_[i].active = 0;
      break;
    case CmdResetTrackParams: for (int i = 0; i < kTracksPerSet; i++) resetTrackParams(set * kTracksPerSet + i); echo_.setFeedback(0.45f); echo_.setDamping(0.4f); reverb_.setDecay(Reverb::kDefaultDecay); reverb_.setTone(Reverb::kDefaultTone); break;
    case CmdResetTrack: resetTrackParams(t); break;
    case CmdResetAll: for (int i = 0; i < kTracks; i++) resetTrackParams(i); for (int i = 0; i < kMaxRamps; i++) ramps_[i].active = 0; break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------- sequencing

// A pattern swap. On a track that streams a loop slot (SPEC §9.19) the swap is a carrier: it sets the stream's variant
// and clip length (i, up to kMaxLoopTicks; 0 keeps the length) and leaves the parked pattern alone — `next_` is not
// read, so the generator need not write it — and the parked pattern, length and variant come back when loop mode
// turns off. A pattern meant for the parked content is therefore swapped while loop mode is off (before the
// CmdLoopMode on of a later bar, or after off). The Vocal track's swaps keep today's meaning in both modes: the empty
// pattern carries the streamed cycle's length (SPEC §9.16), capped at kMaxVocalClipTicks.
void Engine::applySwap(int track) {
  uint8_t z = 0;
  if (!nextLock_[track].compare_exchange_strong(z, 2)) return;          // UI is writing next: retry on a later tick
  TrackRt& r = rt_[track];
  const bool carrier = trackStream_[track] >= 0 && trackCategory(track) != Vocal;
  if (!carrier) active_[track] = next_[track];
  r.variant = pendingVariant_[track];
  uint16_t natural = active_[track].lengthTicks ? active_[track].lengthTicks : kTicksPerBar;
  uint16_t len = pendingLen_[track] ? pendingLen_[track] : (carrier ? r.len : natural);
  if (len < kTicksPerBeat / 2) len = kTicksPerBeat / 2;
  const uint16_t maxLen = clipCap(track);
  if (len > maxLen) len = maxLen;
  r.len = len;
  r.lastPos = 0xFFFF; r.cursor = 0;
  if (r.state == PSEmpty && (active_[track].count || trackCategory(track) == Vocal || carrier)) r.state = PSStopped;
  pendingSwap_[track] = 0; swapDue_[track] = 0;
  nextLock_[track].store(0, std::memory_order_release);
}

// Loop mode on / off (SPEC §9.19), at the bar line. On: the track's clip (length, variant, emptiness) is parked, every
// voice it owns is released (the tails still render through its strip), the pattern is no longer stepped and the track
// renders stream s through its strip from now on; a slot taken from another track (never by the generator, which owns
// the slot allocation) ends that track's loop mode first. Off: the stream is unbound (the slot freed), the parked clip
// comes back and the pattern re-enters phase-locked on this bar (position = globalTick mod len, SPEC §9.4). The Vocal
// track only gains / loses its stream binding: the generator owns what its ring holds and its clip length in both modes.
void Engine::applyLoopMode(int t) {
  const uint8_t mode = pendingLoopMode_[t]; pendingLoopMode_[t] = 0;
  TrackRt& r = rt_[t]; const Category cat = trackCategory(t); const int set = t / kTracksPerSet;
  if (mode == 1) {
    const int s = pendingLoopStream_[t]; if (s < 0 || s >= kStreams) return;
    if (trackStream_[t] == s) return;                                   // already streaming this one
    if (trackStream_[t] < 0) {                                          // entering loop mode: park the clip, release the voices
      parkedLen_[t] = r.len; parkedVariant_[t] = r.variant;
      if (r.state == PSEmpty) parkedEmpty_ |= (uint16_t)(1u << t); else parkedEmpty_ &= (uint16_t)~(1u << t);
      releaseTrackVoices(t);
    } else if (trackStream_[t] >= kSets && loopTrack_[trackStream_[t] - kSets] == t) loopTrack_[trackStream_[t] - kSets] = -1;   // moving to another slot frees the old one
    if (s >= kSets) {
      const int slot = s - kSets; const int prev = loopTrack_[slot];
      if (prev >= 0 && prev != t) { pendingLoopMode_[prev] = 2; applyLoopMode(prev); }   // defensive: the slot's previous track leaves loop mode
      loopTrack_[slot] = (int8_t)t;
    }
    trackStream_[t] = (int8_t)s;
    r.lastPos = 0xFFFF; r.cursor = 0;
    if (r.state == PSEmpty) r.state = PSStopped;                        // the stream is content: the track can be started
    if (cat == Beat && r.kickCut) setLoopKickCut(set, true);
  } else if (mode == 2) {
    const int s = trackStream_[t]; if (s < 0) return;
    if (s >= kSets && loopTrack_[s - kSets] == t) loopTrack_[s - kSets] = -1;
    trackStream_[t] = -1;
    if (cat != Vocal) {
      r.len = parkedLen_[t] ? parkedLen_[t] : kTicksPerBar; r.variant = parkedVariant_[t];
      if (((parkedEmpty_ >> t) & 1) && active_[t].count == 0) r.state = PSEmpty;
    }
    r.lastPos = 0xFFFF; r.cursor = 0;
    if (cat == Beat) setLoopKickCut(set, false);
  }
}

// Every voice the track owns stops sounding: sequenced notes (note-offs, the end-tick list cleared), performed or
// legato synth notes (noteOff → the patch's release), the set's Beat / Percussion sample voices (kSlotFadeSamples fade).
void Engine::releaseTrackVoices(int t) {
  TrackRt& r = rt_[t]; const Category cat = trackCategory(t); const int set = t / kTracksPerSet, tis = t % kTracksPerSet;
  for (int i = 0; i < r.noteCount; i++) fireNoteOff(t, r.noteOnPitch[i]);
  r.noteCount = 0;
  if (isSynthCategory(cat)) { for (int v = kVoiceRange[tis][0]; v < kVoiceRange[tis][1]; v++) if (synthTrack_[set][v] == tis && synth_[set][v].active() && !synth_[set][v].releasing()) synth_[set][v].noteOff(); }
  else if (cat == Beat) { for (int v = 0; v < 8; v++) if (sample_[set][v].active()) sample_[set][v].release(kSlotFadeSamples); }
  else if (cat == Percussion) { for (int v = 8; v < 16; v++) if (sample_[set][v].active()) sample_[set][v].release(kSlotFadeSamples); }
}

// SPEC §9.19 kick cut on a loop Beat track: a drum loop has no note 36 to mute, so the strip filter target goes to
// kLoopKickCutFilter (high-pass, smoothed like any CmdSetParam) while the cut is on and the value it replaced comes
// back when the cut (or loop mode) ends. A filter step or ramp sent while the cut is applied is overwritten by that restore.
void Engine::setLoopKickCut(int set, bool on) {
  TrackParams& p = target_[set * kTracksPerSet + Beat];
  if (on && !loopCutApplied_[set]) { loopCutSavedFilter_[set] = p.filter; p.filter = kLoopKickCutFilter; loopCutApplied_[set] = 1; }
  else if (!on && loopCutApplied_[set]) { p.filter = loopCutSavedFilter_[set]; loopCutApplied_[set] = 0; }
}

void Engine::onBarLine() {
  barSample_ = sampleClock_ + (uint32_t)curOffset_;                        // the bar's exact first sample (SPEC §9.16: the vocal cycle is anchored here)
  for (int s = 0; s < kSets; s++) {
    TrackRt& b = rt_[s * kTracksPerSet + Beat];
    if (b.nextMask != b.mask) { b.mask = b.nextMask; b.variant = b.nextVariant; }
    if (pendingVocal_[s]) { vocalState_[s] = nextVocal_[s]; pendingVocal_[s] = 0; }
    pendingSetSwap_[s] = 0;                                                // a set swap simply means: everything pending swaps on this bar
  }
  for (int k = 0; k < kLoopSlots; k++) if (pendingLoop_[k]) { loopState_[k] = nextLoop_[k]; pendingLoop_[k] = 0; }   // SPEC §9.19, like the vocal
  for (int t = 0; t < kTracks; t++) if (pendingLoopMode_[t]) applyLoopMode(t);   // before the swaps: a swap landing on this bar on a track that starts streaming now carries the stream's clip
  for (int t = 0; t < kTracks; t++) {
    TrackRt& r = rt_[t];
    if (pendingSwap_[t]) { applySwap(t); if (pendingSwap_[t]) swapDue_[t] = 1; }   // locked by the UI right now: apply on a later tick (phase lock keeps it seamless)
    if (queuedLen_[t]) { uint16_t len = queuedLen_[t]; const uint16_t cap = clipCap(t); if (len > cap) len = cap; if (len < kTicksPerBeat / 2) len = kTicksPerBeat / 2; r.len = len; queuedLen_[t] = 0; r.lastPos = 0xFFFF; }
    if (r.state == PSQueued) { r.state = PSPlaying; r.startSample = sampleClock_ + (uint32_t)curOffset_; r.lastPos = 0xFFFF; Ev e; e.type = EvTrackStarted; e.a = (uint8_t)t; e.tick = tick_; events_.push(e); }
    else if (r.state == PSStopQueued) { r.state = PSStopped; for (int i = 0; i < r.noteCount; i++) fireNoteOff(t, r.noteOnPitch[i]); r.noteCount = 0; Ev e; e.type = EvTrackStopped; e.a = (uint8_t)t; e.tick = tick_; events_.push(e); }
  }
  Ev e; e.type = EvBar; e.tick = tick_; events_.push(e);
}

void Engine::onTick() {
  if (tick_ % kTicksPerBar == 0) onBarLine();
  else for (int t = 0; t < kTracks; t++) if (swapDue_[t]) applySwap(t);
  if (tick_ % kTicksPerBeat == 0) { Ev e; e.type = EvBeat; e.tick = tick_; events_.push(e); }
  if (tick_ % kSixteenth == 0) for (int s = 0; s < kSets; s++) { TrackRt& b = rt_[s * kTracksPerSet + Beat]; if (b.rushPending) { b.rushPending = false; b.rush = !b.rush; b.lastPos = 0xFFFF; b.rushCursor = 0; } }
  // note-offs before note-ons (SPEC §9.12)
  for (int t = 0; t < kTracks; t++) {
    TrackRt& r = rt_[t];
    for (int i = 0; i < r.noteCount;) {
      if ((int32_t)(r.noteEndTick[i] - tick_) <= 0) { fireNoteOff(t, r.noteOnPitch[i]); r.noteCount--; r.noteOnPitch[i] = r.noteOnPitch[r.noteCount]; r.noteEndTick[i] = r.noteEndTick[r.noteCount]; r.noteVoice[i] = r.noteVoice[r.noteCount]; }
      else i++;
    }
  }
  for (int t = 0; t < kTracks; t++) {
    TrackRt& r = rt_[t]; Category cat = trackCategory(t); int set = t / kTracksPerSet;
    if (trackStream_[t] >= 0) continue;                                    // loop mode (SPEC §9.19): the parked pattern (and the rush loop) is not stepped; the stream renders instead
    if (cat == Beat && r.rush) {                                           // snare rush loop replaces the pattern while held (SPEC §9.6)
      const Pattern& p = rush_[set]; uint16_t rl = p.lengthTicks ? p.lengthTicks : kTicksPerBeat; uint16_t pos = (uint16_t)(tick_ % rl);
      if (pos == 0) r.rushCursor = 0;
      while (r.rushCursor < p.count && p.notes[r.rushCursor].start() <= pos) { const Note& n = p.notes[r.rushCursor]; if (n.start() == pos && n.pitch() != 36) fireNoteOn(t, n); r.rushCursor++; }
      continue;
    }
    if (!(r.state == PSPlaying || r.state == PSStopQueued) || cat == Vocal) continue;   // a queued stop keeps playing until the bar line (SPEC §9.20: stops are quantised like starts)
    const Pattern& p = active_[t]; uint16_t len = r.len ? r.len : kTicksPerBar; uint16_t pos = (uint16_t)(tick_ % len);
    if (r.lastPos == 0xFFFF || pos < r.lastPos) { r.cursor = 0; while (r.cursor < p.count && p.notes[r.cursor].start() < pos) r.cursor++; }   // phase-locked (re)entry
    r.lastPos = pos;
    while (r.cursor < p.count && p.notes[r.cursor].start() <= pos) { const Note& n = p.notes[r.cursor]; if (n.start() == pos && n.start() < len) fireNoteOn(t, n); r.cursor++; }
  }
}

int Engine::allocSynth(int set, int trackInSet, int maxVoices, uint8_t pitch, bool legato) {
  int lo = kVoiceRange[trackInSet][0], hi = kVoiceRange[trackInSet][1]; if (hi - lo > maxVoices) hi = lo + maxVoices;
  if (hi <= lo) return -1;
  if (legato) { for (int v = lo; v < hi; v++) if (synth_[set][v].active() && synthTrack_[set][v] == trackInSet) return v; }
  for (int v = lo; v < hi; v++) if (synth_[set][v].active() && synthTrack_[set][v] == trackInSet && synth_[set][v].pitch() == pitch) return v;   // steal same pitch
  for (int v = lo; v < hi; v++) if (!synth_[set][v].active()) { synthTrack_[set][v] = (uint8_t)trackInSet; return v; }
  int best = lo; uint32_t bestAge = 0; bool bestRel = false;
  for (int v = lo; v < hi; v++) { bool rel = synth_[set][v].releasing(); uint32_t age = synth_[set][v].age(); if ((rel && !bestRel) || (rel == bestRel && age >= bestAge)) { best = v; bestAge = age; bestRel = rel; } }
  synthTrack_[set][best] = (uint8_t)trackInSet; return best;
}

void Engine::triggerSample(int set, int slot, float vel, bool accent) {
  SampleSlot& s = kit_[set].slot[slot]; if (!s.present || s.length == 0) return;
  int inst = slot * 2 + (sampleToggle_[set][slot] ^= 1);
  SampleVoice& other = sample_[set][slot * 2 + ((inst & 1) ^ 1)]; if (other.active()) other.release(kSlotFadeSamples);   // retrigger crossfade
  sample_[set][inst].trigger(pool_[set] + s.offset, s.length, vel * (accent ? 0.8f : 1.0f) * slotGain_[set][slot], 1.0f);
  sampleAccent_[set][inst] = accent;
  if (slot == 2) for (int i = 6; i < 8; i++) if (sample_[set][i].active() && !sampleAccent_[set][i]) sample_[set][i].release(kSlotFadeSamples);   // closed hat chokes open hat
}

// Beat note contract (SPEC §9.6) for a pitch that passed the layer mask: 36 kick (kick cut, duck, EvKick), 37 snare,
// 38 closed hat, 39 open hat, 40..58 accents → open-hat slot × 0.8 exempt from the choke. Shared by the sequencer
// and the perform keys.
void Engine::playBeatPitch(int set, uint8_t p, float vel) {
  const TrackRt& r = rt_[set * kTracksPerSet + Beat];
  if (p == 36) { if (r.kickCut) return; duck_[set].trigger(); Ev e; e.type = EvKick; e.a = (uint8_t)set; e.tick = tick_; events_.push(e); triggerSample(set, 0, vel, false); }
  else if (p >= 37 && p <= 39) triggerSample(set, p - 36, vel, false);
  else if (p >= 40 && p <= 58) triggerSample(set, 3, vel, true);
}

// CmdPerformNote / CmdPerformChord (SPEC §9.17a): any track plays with its own sound. Synth categories allocate in the
// track's voice range with its patch and knobs (legato glide on a mono glide patch while another of its notes is
// held); Beat triggers the slot of pitch 36..39 or the accent slot for 40..58, honouring kick cut and the duck;
// Percussion triggers slot 4 + (pitch − 36); Vocal has nothing to play. vel 0 is the note off (synth tracks only).
void Engine::performNote(int track, uint8_t pitch, float vel) {
  const Category cat = trackCategory(track); const int set = track / kTracksPerSet; const int tis = track % kTracksPerSet;
  if (cat == Vocal) return;
  if (cat == Beat) { if (vel > 0 && pitch >= 36 && pitch <= 58) playBeatPitch(set, pitch, clampf(vel, 0.f, 1.f)); return; }
  if (cat == Percussion) { if (vel > 0 && pitch >= 36 && pitch <= 39) triggerSample(set, 4 + (pitch - 36), clampf(vel, 0.f, 1.f), false); return; }
  if (vel <= 0) {
    for (int v = kVoiceRange[tis][0]; v < kVoiceRange[tis][1]; v++)
      if (synthTrack_[set][v] == tis && synth_[set][v].active() && synth_[set][v].pitch() == pitch && !synth_[set][v].releasing()) synth_[set][v].noteOff();
    return;
  }
  const SynthPatch& patch = patch_[track]; const int maxV = patch.polyphony < 1 ? 1 : patch.polyphony;
  bool held = false;                                                    // another performed note of this track still sounding (not released)
  for (int v = kVoiceRange[tis][0]; v < kVoiceRange[tis][1]; v++) if (synthTrack_[set][v] == tis && synth_[set][v].active() && !synth_[set][v].releasing()) { held = true; break; }
  const bool legato = patch.glide > 0 && maxV == 1 && held;
  const int v = allocSynth(set, tis, maxV, pitch, legato); if (v < 0) return;
  synth_[set][v].setPatch(&patch); synth_[set][v].setKnobs(&knobs_[track]);
  synth_[set][v].noteOn(pitch, (uint8_t)clampf(vel * 127.f, 1, 127), legato);
}

// Per-track reset (CmdResetTrack / CmdResetTrackParams / CmdResetAll): the five strip parameters go back to the
// category defaults (smoothed), the knobs are re-derived from the track's current patch (at once, like CmdSetPatch).
// The track's own strip / knob ramps are dropped first (a hold-ramp or SHIFT ramp held while the generator resets the
// track would otherwise re-apply its value on the next block); a tempo drift is global and stays.
void Engine::resetTrackParams(int track) {
  for (int i = 0; i < kMaxRamps; i++) if (ramps_[i].active && ramps_[i].track == track && ramps_[i].param != PTempo) ramps_[i].active = 0;
  TrackParams d = defaultTrackParams(trackCategory(track)); knobsFromPatch(patch_[track], d);
  target_[track] = d; copyKnobFields(d, current_[track]); deriveKnobs(track);
  const int set = track / kTracksPerSet;
  if (trackCategory(track) == Beat && loopCutApplied_[set]) { loopCutSavedFilter_[set] = target_[track].filter; target_[track].filter = kLoopKickCutFilter; }   // a loop Beat track's kick cut stays a high-pass through the reset (SPEC §9.19)
}

// knobs_[track] from the smoothed knob fields (mappings shared with the UI in types.h). Called from smoothParams only
// when a field moved, and after CmdSetPatch / reset, so the two powf stay off the steady-state path.
void Engine::deriveKnobs(int track) {
  const TrackParams& cur = current_[track]; SynthKnobs& k = knobs_[track];
  k.cutoffHz = knobCutoffHz(cur.cutoff); k.resonance = knobResonance(cur.resonance); k.filterEnvOct = knobEnvOct(cur.envAmt);
  k.decayS = knobDecayS(cur.decay); k.drive = clampf(cur.drive, 0.f, 1.f); k.wave = cur.wave < WaveCount ? cur.wave : (uint8_t)WSaw;
  k.sat = clampf(cur.sat, 0.f, 1.f); k.accent = clampf(cur.accent, 0.f, 1.f);   // SPEC §9.22: plain copies, the voice derives its constants per modulation block
}

void Engine::fireNoteOn(int track, const Note& n) {
  TrackRt& r = rt_[track]; Category cat = trackCategory(track); int set = track / kTracksPerSet; int tis = track % kTracksPerSet;
  float vel = n.vel() / 127.0f;
  if (cat == Beat) { uint8_t p = n.pitch(); uint8_t bit = beatBit(p); if (!bit || !(r.mask & bit)) return; playBeatPitch(set, p, vel); return; }
  if (cat == Percussion) { uint8_t p = n.pitch(); if (p < 36 || p > 39) return; triggerSample(set, 4 + (p - 36), vel, false); return; }
  if (cat == Vocal) return;
  const SynthPatch& patch = patch_[track]; int maxV = patch.polyphony < 1 ? 1 : patch.polyphony;
  bool legato = patch.glide > 0 && maxV == 1 && r.noteCount > 0;
  int v = allocSynth(set, tis, maxV, n.pitch(), legato); if (v < 0) return;
  synth_[set][v].setPatch(&patch); synth_[set][v].setKnobs(&knobs_[track]); synth_[set][v].noteOn(n.pitch(), n.vel(), legato);
  int slot = -1; for (int i = 0; i < r.noteCount; i++) if (r.noteOnPitch[i] == n.pitch()) slot = i;
  if (slot < 0) { if (r.noteCount >= 8) { r.noteCount--; r.noteOnPitch[0] = r.noteOnPitch[r.noteCount]; r.noteEndTick[0] = r.noteEndTick[r.noteCount]; r.noteVoice[0] = r.noteVoice[r.noteCount]; } slot = r.noteCount++; }
  r.noteOnPitch[slot] = n.pitch(); r.noteEndTick[slot] = tick_ + n.len(); r.noteVoice[slot] = (uint8_t)v;
}

void Engine::fireNoteOff(int track, uint8_t pitch) {
  int set = track / kTracksPerSet; int tis = track % kTracksPerSet;
  for (int v = kVoiceRange[tis][0]; v < kVoiceRange[tis][1]; v++)
    if (synthTrack_[set][v] == tis && synth_[set][v].active() && synth_[set][v].pitch() == pitch && !synth_[set][v].releasing()) synth_[set][v].noteOff();
}

// ---------------------------------------------------------------------------------------------- rendering

// One stream's ring into dst (SPEC §9.3 / §9.19): the ring maps absolute sample indices to positions (baseSample, w, r);
// the engine reads the samples of the chunk it renders — absolute indices sampleClock_ + offset onwards, so a stream
// whose track starts (or whose content arrives) at a bar line inside a block is placed exactly from that bar sample —
// counts a sample the producer has not written yet as an underrun (silence) and publishes the read position for the
// producer. Vocal streams (0 / 1) and loop slots (kSets + k) differ only in which ring, state and counter they use.
void Engine::renderStream(int stream, int n, int offset, float* dst) {
  const bool vocal = stream < kSets; const int k = vocal ? stream : stream - kSets;
  VocalRing& v = vocal ? vocal_[k] : loop_[k]; if (!v.buf || v.cap <= 0 || !v.active.load(std::memory_order_acquire)) return;
  const VocalState& st = vocal ? vocalState_[k] : loopState_[k];
  uint16_t& under = vocal ? vocalUnderruns_[k] : loopUnderruns_[k];
  uint32_t base = v.baseSample.load(std::memory_order_acquire); uint32_t w = v.w.load(std::memory_order_acquire);
  float g = st.gain * (1.0f / 32768.0f); uint32_t S = sampleClock_ + (uint32_t)offset;
  uint32_t p = 0; bool havePos = false;                                       // ring position of idx, wrapped once and then stepped (any cap)
  for (int i = 0; i < n; i++, S++) {
    int32_t idx = (int32_t)(S - base);
    if (idx < 0) continue;
    if (!havePos) { p = v.wrap((uint32_t)idx); havePos = true; } else if (++p == (uint32_t)v.cap) p = 0;
    if ((uint32_t)idx >= w) { under++; continue; }
    dst[i] += v.buf[p] * g;
  }
  if ((int32_t)(S - base) > 0) v.r.store(S - base, std::memory_order_release);
}

void Engine::renderTrack(int track, int n, int offset) {
  int set = track / kTracksPerSet; int tis = track % kTracksPerSet; Category cat = trackCategory(track);
  float* buf = trackBuf_[set]; for (int i = 0; i < n; i++) buf[i] = 0;
  bool any = false; uint32_t pt0 = g_microsHook ? g_microsHook() : 0;
  if (isSynthCategory(cat)) {
    int cnt = kVoiceRange[tis][1] - kVoiceRange[tis][0]; float scale = 1.0f / (float)(cnt > 0 ? cnt : 1);
    for (int v = kVoiceRange[tis][0]; v < kVoiceRange[tis][1]; v++) if (synthTrack_[set][v] == tis && synth_[set][v].active()) { any = true; if (!synth_[set][v].render(buf, n)) synthTrack_[set][v] = 255; }
    if (any) for (int i = 0; i < n; i++) buf[i] *= scale;
  } else if (cat == Beat) { for (int v = 0; v < 8; v++) if (sample_[set][v].active()) { any = true; sample_[set][v].render(buf, n); } }
  else if (cat == Percussion) { for (int v = 8; v < 16; v++) if (sample_[set][v].active()) { any = true; sample_[set][v].render(buf, n); } }
  else if (cat == Vocal) { if ((rt_[track].state == PSPlaying || rt_[track].state == PSStopQueued) && vocalState_[set].present) { renderVocal(set, n, offset, buf); any = true; } }   // SPEC §9.20: audible until the bar
  const int stream = trackStream_[track];                                   // loop mode (SPEC §9.19): the slot's ring renders through this strip (the released voices' tails above still fade through it)
  if (stream >= kSets && (rt_[track].state == PSPlaying || rt_[track].state == PSStopQueued) && loopState_[stream - kSets].present) { renderStream(stream, n, offset, buf); any = true; }   // SPEC §9.20
  TrackRt& r = rt_[track];
  if (g_microsHook) { uint32_t pt1 = g_microsHook(); if (isSynthCategory(cat)) profSynth_[set] += pt1 - pt0; else profSample_[set] += pt1 - pt0; pt0 = pt1; }
  if (!any) { r.level *= 0.9f; return; }
  const TrackParams& p = current_[track];
  const float drive = distDrive_[track], makeup = distMakeup_[track];   // SPEC §9.17a: 1 + 31 × dist and 1/sqrt(min(drive, 8)), derived in smoothParams when dist moves
  // The dist stage is bypassed only at exactly 0 (where drive 1 / makeup 1 make it continuous with the bypass): any
  // higher gate would step the level by the makeup curve's small-signal gain at the threshold every time the smoothed
  // dist crosses it (+1.2 dB at 0.01). smoothParams snaps dist to 0 within 1e-4, so a settled dist 0 costs nothing.
  bool dist = p.dist > 0.0f; bool filt = p.filter > 0.05f || p.filter < -0.05f;
  float vol = p.volume, echo = p.echo, rev = p.reverb, cho = p.chorus; bool duck = cat != Beat; const float* dg = duckBuf_[set] + offset;
  Svf& f = trackFilter_[track];
  float* mix = (set ? mix2_ : mix_) + offset; float* es = (set ? echoSend2_ : echoSend_) + offset; float* rs = (set ? revSend2_ : revSend_) + offset; float peak = 0;
  // Staged strip (SPEC §9.11): shaping in place, then gain + sub-block limiter, then the sums. Separate tight loops are
  // much cheaper on the S3 than one loop full of branches; the limiter divides once per 16 samples instead of per sample.
  if (dist) for (int i = 0; i < n; i++) buf[i] = softClip(buf[i] * drive) * makeup;
  if (filt) for (int i = 0; i < n; i++) buf[i] = f.process(buf[i]);
  float lg = limGain_[track];
  for (int i0 = 0; i0 < n; i0 += 16) {
    int m = n - i0 < 16 ? n - i0 : 16; float pk = 0;
    if (duck) for (int j = 0; j < m; j++) { float x = buf[i0 + j] * dg[i0 + j] * vol; buf[i0 + j] = x; float a = x < 0 ? -x : x; if (a > pk) pk = a; }
    else for (int j = 0; j < m; j++) { float x = buf[i0 + j] * vol; buf[i0 + j] = x; float a = x < 0 ? -x : x; if (a > pk) pk = a; }
    float lgPrev = lg;
    if (pk * lg > 0.95f) lg = 0.95f / pk; else lg += (1.0f - lg) * 0.009f;     // attack within one sub-block (0.7 ms), ≈ 80 ms release
    float step = (lg - lgPrev) * (1.0f / (float)m); float g = lgPrev;           // ramp the gain across the sub-block: no 16-sample zipper steps
    for (int j = 0; j < m; j++) { g += step; buf[i0 + j] *= g; }
    float lp = pk * lg; if (lp > peak) peak = lp;
  }
  limGain_[track] = lg;
  for (int i = 0; i < n; i++) mix[i] += buf[i];
  if (echo > 0.001f) for (int i = 0; i < n; i++) es[i] += buf[i] * echo;
  if (rev > 0.001f) for (int i = 0; i < n; i++) rs[i] += buf[i] * rev;
  if (cho > 0.001f) chorus_[set].send(buf, cho, n);                      // SPEC §9.22: into the set's chorus line (renderSet began the chunk)
  r.level = peak > r.level ? peak : r.level * 0.9f;
  if (g_microsHook) profStrip_[set] += g_microsHook() - pt0;
}

// The set's chorus bus (SPEC §9.22) reads this chunk's wet signal into the set's mix BEFORE the tracks add their sends to
// its line (Chorus::beginChunk, read-before-write: render() keeps every chunk within Chorus::kMaxChunk samples).
void Engine::renderSet(int set, int n, int offset) {
  uint32_t t0 = g_microsHook ? g_microsHook() : 0;
  chorus_[set].beginChunk((set ? mix2_ : mix_) + offset, n);
  if (g_microsHook) profStrip_[set] += g_microsHook() - t0;
  for (int i = 0; i < kTracksPerSet; i++) renderTrack(set * kTracksPerSet + i, n, offset);
}
void Engine::renderSetJob(void* arg) { ChunkJob* j = (ChunkJob*)arg; j->self->renderSet(1, j->n, j->offset); }

void Engine::smoothParams() {
  const float c = 0.6f;                                              // per 256-sample block ≈ 10 ms one-pole (SPEC §9.12)
  for (int t = 0; t < kTracks; t++) {
    TrackParams& cur = current_[t]; const TrackParams& tg = target_[t];
    cur.volume += (tg.volume - cur.volume) * c; cur.echo += (tg.echo - cur.echo) * c; cur.reverb += (tg.reverb - cur.reverb) * c;
    cur.chorus += (tg.chorus - cur.chorus) * c;                       // SPEC §9.22: a strip send like echo / reverb
    if (approach(cur.dist, tg.dist, c)) { distDrive_[t] = distDrive(cur.dist); distMakeup_[t] = distMakeup(distDrive_[t]); }   // one sqrtf per moving dist per block
    if (fabsf(tg.filter - cur.filter) > 1e-4f) { cur.filter += (tg.filter - cur.filter) * c; if (fabsf(cur.filter) > 0.05f) trackFilter_[t].set(filterCutoff(cur.filter), 0.25f, cur.filter < 0 ? 0 : 2); else trackFilter_[t].reset(); }
    // Synth knobs (SPEC §9.17a): smoothed in knob space (log for cutoff / decay), the wave stepped; knobs_ is re-derived
    // only when a field moved, so a settled track costs five compares and no powf.
    bool moved = approach(cur.cutoff, tg.cutoff, c); moved |= approach(cur.resonance, tg.resonance, c); moved |= approach(cur.envAmt, tg.envAmt, c);
    moved |= approach(cur.decay, tg.decay, c); moved |= approach(cur.drive, tg.drive, c);
    moved |= approach(cur.sat, tg.sat, c); moved |= approach(cur.accent, tg.accent, c);   // SPEC §9.22
    if (cur.wave != tg.wave) { cur.wave = tg.wave; moved = true; }
    if (moved) deriveKnobs(t);
  }
  bool moved = fabsf(gyroFilterTarget_ - gyroFilterCur_) > 1e-4f;
  gyroFilterCur_ += (gyroFilterTarget_ - gyroFilterCur_) * 0.3f; gyroRevCur_ += (gyroRev_ - gyroRevCur_) * 0.3f; gyroEchoCur_ += (gyroEcho_ - gyroEchoCur_) * 0.3f;
  if (moved) { if (fabsf(gyroFilterCur_) > 0.05f) gyroFilter_.set(filterCutoff(gyroFilterCur_), 0.3f, gyroFilterCur_ < 0 ? 0 : 2); else gyroFilter_.reset(); }
}

void Engine::serviceRamps() {
  for (int i = 0; i < kMaxRamps; i++) {
    Ramp& rp = ramps_[i]; if (!rp.active) continue;
    float prog = (float)(tick_ - rp.startTick) / (float)rp.ticks; if (prog > 1.0f) prog = 1.0f;
    float v = rp.from + (rp.to - rp.from) * prog;
    // A tempo drift ends when it arrives (it is not tied to a held key, and a pinned bpm_ would swallow tempo±);
    // strip / knob ramps stay active at their end value until CmdFreezeRamp / CmdCancelRamps release them (SPEC §9.12).
    if (rp.param == PTempo) { setTempo(v); if (prog >= 1.0f) rp.active = 0; }
    else { Cmd s; s.type = CmdSetParam; s.track = rp.track; s.a = rp.param; s.f = v; handleCmd(s); }
  }
}

void Engine::updateStatus() {
  status_.profSynth = profSynth_[0] + profSynth_[1]; status_.profSample = profSample_[0] + profSample_[1]; status_.profStrip = profStrip_[0] + profStrip_[1]; status_.profBus = profBus_;
  status_.vocalUnderruns = (uint16_t)(vocalUnderruns_[0] + vocalUnderruns_[1]);
  status_.tick = tick_; status_.sampleClock = sampleClock_; status_.bpm = (uint16_t)(bpm_ + 0.5f); status_.running = running_;
  status_.bar = (uint8_t)((tick_ / kTicksPerBar) & 0xFF); status_.beat = (uint8_t)((tick_ / kTicksPerBeat) % kBeatsPerBar);
  // The next bar line is always ahead of the clock: at a bar tick (onBarLine already ran, even when its sample is this
  // very clock) the next one is a whole bar away. ceil: the first sample of the bar, exactly as the render loop crosses ticks.
  uint32_t ticksToBar = kTicksPerBar - (tick_ % kTicksPerBar);
  uint64_t fp = (uint64_t)ticksToBar * samplesPerTickFp_; fp = fp > tickAccFp_ ? fp - tickAccFp_ : 0;
  status_.nextBarSample = sampleClock_ + (uint32_t)((fp + 65535) >> 16);
  status_.barSample = barSample_;
  for (int t = 0; t < kTracks; t++) {
    TrackStatus& s = status_.track[t]; const TrackRt& r = rt_[t];
    s.state = r.state; s.variant = r.variant; s.lenTicks = r.len; s.posTick = (uint16_t)(tick_ % (r.len ? r.len : 1)); s.startSample = r.startSample;
    int lv = (int)(r.level * 255.0f); s.level = (uint8_t)(lv > 255 ? 255 : lv); s.kickCut = r.kickCut; s.pendingSwap = pendingSwap_[t]; s.rush = r.rush;
  }
}

void Engine::render(int16_t* out, int n) {
  if (n > kBlock) n = kBlock;
  if (pendingSlotSet_ >= 0) {                                         // deferred copy: the slot's voices faded during the previous block
    int set = pendingSlotSet_, slot = pendingSlotIdx_; SampleSlot& s = kit_[set].slot[slot];
    memcpy(pool_[set] + s.offset, staging_, pendingSlotLen_); s.length = pendingSlotLen_; s.present = pendingSlotLen_ > 0; pendingSlotSet_ = -1; status_.slotAck++;
  }
  for (int s = 0; s < kSets; s++) profSynth_[s] = profSample_[s] = profStrip_[s] = 0;
  profBus_ = 0;
  Cmd c; int guard = 0; while (guard++ < 64 && cmds_.pop(c)) handleCmd(c);
  serviceRamps(); smoothParams();
  for (int i = 0; i < n; i++) { mix_[i] = 0; echoSend_[i] = 0; revSend_[i] = 0; wet_[i] = 0; mix2_[i] = 0; echoSend2_[i] = 0; revSend2_[i] = 0; }
  for (int s = 0; s < kSets; s++) for (int i = 0; i < n; i++) duckBuf_[s][i] = duck_[s].gain();
  int offset = 0; curOffset_ = 0;
  while (offset < n) {
    int chunk = n - offset;
    if (running_) { uint32_t remainFp = samplesPerTickFp_ - tickAccFp_; int toTick = (int)((remainFp + 65535) >> 16); if (toTick < 1) toTick = 1; if (toTick < chunk) chunk = toTick; }
    if (chunk > Chorus::kMaxChunk) chunk = Chorus::kMaxChunk;    // SPEC §9.22 chorus read-before-write contract; a tick is 110 samples at 125 BPM, so only tempos below 54 BPM and a stopped transport split a block in two
    if (g_parallelStart && g_parallelJoin) { job_.self = this; job_.n = chunk; job_.offset = offset; g_parallelStart(&Engine::renderSetJob, &job_); renderSet(0, chunk, offset); g_parallelJoin(); }
    else { renderSet(0, chunk, offset); renderSet(1, chunk, offset); }
    offset += chunk; curOffset_ = offset;
    if (running_) { tickAccFp_ += (uint32_t)chunk << 16; while (tickAccFp_ >= samplesPerTickFp_) { tickAccFp_ -= samplesPerTickFp_; tick_++; onTick(); } }
  }
  uint32_t bt0 = g_microsHook ? g_microsHook() : 0;
  for (int i = 0; i < n; i++) { mix_[i] += mix2_[i]; echoSend_[i] += echoSend2_[i]; revSend_[i] += revSend2_[i]; }
  if (gyroEchoCur_ > 0.001f) for (int i = 0; i < n; i++) echoSend_[i] += mix_[i] * gyroEchoCur_;   // gyro master sends (SPEC §9.11)
  if (gyroRevCur_ > 0.001f) for (int i = 0; i < n; i++) revSend_[i] += mix_[i] * gyroRevCur_;
  echo_.process(echoSend_, wet_, n); reverb_.process(revSend_, wet_, n);
  bool gf = fabsf(gyroFilterCur_) > 0.05f;
  for (int i = 0; i < n; i++) {
    float x = (mix_[i] + wet_[i]) * kMasterTrim;
    if (gf) x = gyroFilter_.process(x);
    float a = x < 0 ? -x : x; float want = a > 0.8f ? 0.8f / a : 1.0f;              // lookahead limiter at −1.9 dBFS: gain computed on the incoming sample,
    if (want < masterGain_) masterGain_ = want; else masterGain_ += (1.0f - masterGain_) * 0.0006f;   // applied to the delayed one (64 samples)
    float y = lookahead_[laPos_]; lookahead_[laPos_] = x; laPos_ = (laPos_ + 1) & (kLookaheadSamples - 1);
    out[i] = saturate(y * masterGain_);
  }
  if (g_microsHook) profBus_ = g_microsHook() - bt0;
  sampleClock_ += (uint32_t)n;
  updateStatus();
}

} // namespace lp
