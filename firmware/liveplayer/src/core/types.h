// meehaw live performer — shared value types (SPEC §2, §5, §6). No heap, no virtuals, trivially copyable.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "config.h"

namespace lp {

enum Category : uint8_t { Chord = 0, Lead, Pad, Bass, Fx, Percussion, Vocal, Beat, CategoryCount };
inline const char* categoryName(Category c) {            // REAPER folder names
  static const char* n[] = {"Chord", "Lead", "Pad", "Bass", "Fx", "Percussion", "Vocal", "Beat"};
  return n[c < CategoryCount ? c : 0];
}
inline const char* categoryShort(Category c) {
  static const char* n[] = {"CHD", "LED", "PAD", "BAS", "FX", "PRC", "VOC", "BEAT"};
  return n[c < CategoryCount ? c : 0];
}
inline Category trackCategory(int track) { return Category(track % kTracksPerSet); }
inline int trackSet(int track) { return track / kTracksPerSet; }
inline bool isSynthCategory(Category c) { return c == Chord || c == Lead || c == Pad || c == Bass || c == Fx; }
inline bool isSampleCategory(Category c) { return c == Beat || c == Percussion; }

// Packed note: start 12 bits (ticks), length 8 bits (units of kNoteLenUnit ticks), pitch 7 bits, velocity 5 bits.
struct Note {
  uint32_t packed = 0;
  static Note make(uint16_t startTick, uint16_t lenTicks, uint8_t pitch, uint8_t vel) {
    Note n;
    uint32_t lu = (lenTicks + kNoteLenUnit - 1) / kNoteLenUnit; if (lu < 1) lu = 1; if (lu > 255) lu = 255;
    uint32_t v = vel >> 2; if (v > 31) v = 31;
    n.packed = (startTick & 0xFFF) | (lu << 12) | ((uint32_t)(pitch & 0x7F) << 20) | (v << 27);
    return n;
  }
  uint16_t start() const { return packed & 0xFFF; }
  uint16_t len() const { return ((packed >> 12) & 0xFF) * kNoteLenUnit; }
  uint8_t pitch() const { return (packed >> 20) & 0x7F; }
  uint8_t vel() const { return (uint8_t)(((packed >> 27) & 0x1F) * 4 + 3); }   // 3..127
  uint16_t end() const { return start() + len(); }
  void setStart(uint16_t t) { packed = (packed & ~0xFFFu) | (t & 0xFFF); }
  void setLen(uint16_t ticks) { uint32_t lu = (ticks + kNoteLenUnit - 1) / kNoteLenUnit; if (lu < 1) lu = 1; if (lu > 255) lu = 255; packed = (packed & ~(0xFFu << 12)) | (lu << 12); }
  void setPitch(uint8_t p) { packed = (packed & ~(0x7Fu << 20)) | ((uint32_t)(p & 0x7F) << 20); }
  void setVel(uint8_t v) { uint32_t q = v >> 2; if (q > 31) q = 31; packed = (packed & ~(0x1Fu << 27)) | (q << 27); }
};

// A pattern: notes sorted by start tick. lengthTicks is the musical loop length (whole beats).
struct Pattern {
  uint16_t count = 0;
  uint16_t lengthTicks = kTicksPerBar;
  Note notes[kMaxNotes];
  void clear(uint16_t len = kTicksPerBar) { count = 0; lengthTicks = len; }
  bool add(const Note& n) { if (count >= kMaxNotes) return false; notes[count++] = n; return true; }
  void sortByStart() {                                       // insertion sort, patterns are small
    for (int i = 1; i < count; i++) { Note k = notes[i]; int j = i - 1; while (j >= 0 && notes[j].start() > k.start()) { notes[j + 1] = notes[j]; j--; } notes[j + 1] = k; }
  }
};

// Synth patch: fully describes a synth voice sound (SPEC §5). Values are plain floats; no pointers.
enum Wave : uint8_t { WSine = 0, WTri, WSaw, WSquare, WFat, WaveCount };
struct SynthPatch {
  uint8_t wave1 = WSaw, wave2 = WSaw;
  float osc2Detune = 0.0f;     // semitones (fractional allowed)
  float osc2Level = 0.0f;      // 0..1
  int8_t osc2Octave = 0;       // -2..2
  float subLevel = 0.0f;       // 0..1 (square one octave down)
  float noiseLevel = 0.0f;     // 0..1
  uint8_t filterMode = 0;      // 0 LP 1 BP 2 HP
  float cutoff = 2000.0f;      // Hz
  float resonance = 0.2f;      // 0..0.95
  float filterEnv = 0.0f;      // octaves added by the filter envelope
  float keyTrack = 0.3f;       // 0..1
  float attack = 0.005f, decay = 0.15f, sustain = 0.7f, release = 0.2f;   // seconds / level
  float fAttack = 0.005f, fDecay = 0.2f;                                     // filter env (AD)
  float glide = 0.0f;          // seconds, 0 = off (bass/fx legato)
  float lfoRate = 0.0f;        // Hz, 0 = off
  float lfoToCutoff = 0.0f;    // octaves
  float lfoToPitch = 0.0f;     // semitones
  float drive = 0.0f;          // 0..1 pre-filter saturation
  float level = 0.8f;          // output gain
  uint8_t polyphony = 4;       // voices this patch may use (clamped by the track's allocation)
  char name[24] = "Init";
  // SPEC §9.22 (303 character): saturation inside the filter loop and accent depth, the initial SAT / ACC knob values
  // of a track that loads this patch (CmdSetPatch, like cutoff / resonance / ...). Appended so aggregate initialisers
  // of the preset banks stay valid; both default to OFF so every library preset plays exactly as before (library clips
  // sit at the MIDI default velocity 100, which would otherwise accent whole tracks): the '303' families, acid mode and
  // the SAT / ACC knobs opt in (the TrackParams default 0.5 is what a knob without a patch value shows).
  float sat = 0.0f;            // 0..1, 0 = today's clean TPT SVF
  float accent = 0.0f;         // 0..1: an accented note (vel >= 100) gets filter env × (1 + accent) and up to +3 dB (0.5 = MiniAcid's character)
};

// Where one one-shot sits in the sample pool (µ-law bytes at kSampleRate).
struct SampleSlot {
  uint32_t offset = 0;         // byte offset in the region
  uint32_t length = 0;         // samples (bytes)
  uint8_t  present = 0;
  uint8_t  choke = 0;          // slot index that chokes this one + 1 (0 = none); oh choked by hh
  int8_t   gainDb = 0;         // trim applied at load
};
struct SampleKit {             // one set's 8 slots inside that set's pool (SPEC §9.1)
  SampleSlot slot[kSampleSlots];
  char name[kSampleSlots][kMaxNameLen]; // basenames for display / set files (library names run to ~55 chars, SPEC §9.8 kit paths)
};

// Per-track performance parameters (live values; defaults restored on load, SPEC §2).
struct TrackParams {
  float volume = 0.8f;         // 0..1
  float dist = 0.0f;           // 0..1 (drive 1..32 into the clipper, SPEC §9.17)
  float filter = 0.0f;         // -1 LP .. 0 bypass .. +1 HP
  float echo = 0.0f;           // send 0..1
  float reverb = 0.0f;         // send 0..1
  // Synth knobs (SPEC §9.17): the live sound of a synth track, normalised 0..1. Initialised from the patch on
  // CmdSetPatch, edited on the synth page or randomised in acid mode; the voice reads them via SynthKnobs.
  float cutoff = 0.5f;         // 0..1 → knobCutoffHz
  float resonance = 0.2f;      // 0..1 → 0..0.985
  float envAmt = 0.25f;        // 0..1 → 0..4 octaves of filter envelope
  float decay = 0.4f;          // 0..1 → knobDecayS (filter env decay)
  float drive = 0.0f;          // 0..1 pre-filter saturation
  float sat = 0.0f;            // 0..1 saturation inside the filter loop (the 303 squelch, SPEC §9.22); 0 = today's clean TPT SVF
  float accent = 0.5f;         // 0..1 accent depth: a velocity >= 100 note multiplies the filter envelope by 1 + accent (MiniAcid: x2 at 0.5)
  float chorus = 0.0f;         // 0..1 send to the set's chorus bus (SPEC §9.22)
  uint8_t wave = WSaw;         // oscillator 1 wave (stepped)
};
// Knob mappings shared by engine and UI (display in real units).
inline float knobCutoffHz(float k) { return 80.0f * powf(150.0f, k < 0 ? 0 : k > 1 ? 1 : k); }        // 80 Hz .. 12 kHz, log
inline float knobFromCutoffHz(float hz) { float k = logf((hz < 80 ? 80 : hz > 12000 ? 12000 : hz) / 80.0f) / logf(150.0f); return k < 0 ? 0 : k > 1 ? 1 : k; }
inline float knobResonance(float k) { return 0.985f * (k < 0 ? 0 : k > 1 ? 1 : k); }
inline float knobEnvOct(float k) { return 4.0f * (k < 0 ? 0 : k > 1 ? 1 : k); }
inline float knobDecayS(float k) { return 0.01f * powf(200.0f, k < 0 ? 0 : k > 1 ? 1 : k); }        // 10 ms .. 2 s, log
inline float knobFromDecayS(float s) { float k = logf((s < 0.01f ? 0.01f : s > 2 ? 2 : s) / 0.01f) / logf(200.0f); return k < 0 ? 0 : k > 1 ? 1 : k; }

// Effective synth parameters a voice reads every modulation block (engine smooths them from TrackParams).
struct SynthKnobs {
  float sat = 0.0f, accent = 0.5f;   // SPEC §9.22 (303 character), derived from TrackParams like the others
  float cutoffHz = 2000.0f;
  float resonance = 0.2f;
  float filterEnvOct = 1.0f;
  float decayS = 0.2f;         // filter envelope decay
  float drive = 0.0f;
  uint8_t wave = WSaw;
};
// PMaster is the master output volume (headphones, 0..255 in the Ui, SPEC §3 SHIFT + kick cut): a page / colour
// slot only — no key maps to it (keys.cpp), the engine ignores it in CmdSetParam / CmdRamp, and it is never a track parameter.
// Per-category defaults restored on init and on set load (SPEC §2): vocals get a default echo send (user request).
inline TrackParams defaultTrackParams(Category c) { TrackParams p; if (c == Vocal) p.echo = 0.35f; return p; }

enum ParamId : uint8_t { PVolume = 0, PReverb, PEcho, PDist, PFilter, PLength, PTempo, PKey, PMaster, PCutoff, PResonance, PEnvAmt, PDecay, PDrive, PWave,
                         PSat, PAccent, PChorus, ParamCount };   // SPEC §9.22: 303 character (filter-loop saturation, accent depth) and the per-set chorus send

// Playback state of a track as seen by the UI.
enum PlayState : uint8_t { PSEmpty = 0, PSStopped, PSQueued, PSPlaying, PSStopQueued };

struct TrackStatus {
  PlayState state = PSEmpty;
  uint8_t variant = 0;         // 0..3
  uint16_t lenTicks = kTicksPerBar;   // current clip length in ticks (0.5..16 beats, SPEC §9.4)
  uint16_t posTick = 0;        // position inside the clip (globalTick mod len)
  uint32_t startSample = 0;    // absolute sample index at which the clip (re)started (vocal producer, SPEC §9.3)
  uint8_t level = 0;           // 0..255 output level meter
  uint8_t kickCut = 0;
  uint8_t pendingSwap = 0;     // a next-pattern swap is waiting for the bar line
  uint8_t rush = 0;
};
struct EngineStatus {
  uint32_t tick = 0;           // global ticks since start
  uint32_t sampleClock = 0;    // absolute samples rendered
  uint32_t nextBarSample = 0;  // sample index of the next bar line (always ahead of sampleClock; exact while the tempo holds)
  uint32_t barSample = 0;      // exact first sample of the bar that contains sampleClock (the latest bar line; the vocal producer's anchor, SPEC §9.16)
  uint16_t bpm = kDefaultBpm;
  uint8_t  running = 0;        // master transport
  uint8_t  bar = 0, beat = 0;  // bar (mod 256) and beat 0..3
  uint8_t  generating[kSets] = {0, 0};
  uint16_t slotAck = 0;        // incremented after each CmdSlotSwap is applied
  uint16_t underruns = 0;
  uint16_t vocalUnderruns = 0;
  uint32_t renderMicrosMax = 0;
  uint32_t profSynth = 0, profSample = 0, profStrip = 0, profBus = 0;   // µs per block (last block), debug profiling
  TrackStatus track[kTracks];
};

// Commands UI -> engine (single producer, single consumer). Small and POD.
enum CmdType : uint8_t {
  CmdNone = 0,
  CmdTrackToggle,      // track: queue start or stop at next bar
  CmdTrackStart, CmdTrackStop,
  CmdTrackStartNow, CmdTrackStopNow,
  CmdStopSet,          // a = set
  CmdPlaySet,          // a = set (start every loaded track of the set at the bar)
  CmdMasterPlay,       // a = 0 stop everything now, 1 run
  CmdSetTempo,         // i = bpm
  CmdSetParam,         // track, a = ParamId, f = value
  CmdSwapPattern,      // track: engine adopts the track's "next" pattern buffer at the next bar; a = variant, i = lengthTicks
  CmdSwapPatternNow,   // same, immediately (used by the editor while stopped)
  CmdSetLength,        // track, i = lengthTicks (power of two beats), applied at bar
  CmdSnareRush,        // a = set, i = 1 on / 0 off
  CmdKickCut,          // a = set, i = 1/0
  CmdSetPatch,         // track: engine copies the track's "next" SynthPatch (immediate)
  CmdSlotSwap,         // a = set, i = slot: copy the staging buffer into the slot (fade any voice), bump slotAck; f = gain
  CmdSwapSet,          // a = set: apply all pending next-pattern swaps of the set on the same bar line
  CmdSetGenerating,    // a = set, i = 1/0: queue pat/len presses while a set generates
  CmdBeatMask,         // a = set, i = pitch mask bits (bit0 = 36 ... bit3 = 39, bit4 = accents) applied at the bar
  CmdVocalLoaded,      // a = set: nextVocal(set) describes the new stream (swap at bar)
  CmdRamp,             // track, a = ParamId, f = target, i = ticks (SPEC §9.12); PTempo ramps are never cancelled by CmdCancelRamps (tempo drift)
  CmdCancelRamps,      // all ramps snap back to their start values
  CmdGyro,             // f = filter (-1..1), i = (reverb<<8 | echo) 0..255 each, a = enabled
  CmdEchoDivision,     // i = division index
  CmdDuck,             // a = enabled
  CmdPerformNote,      // track = ANY track: synth tracks play their own voices, Beat/Percussion trigger slot (pitch-36), Vocal ignored; i = pitch, f = velocity (0 = off)
  CmdResetTrackParams, // a = set
  CmdResetAll,
  CmdCancelPending,    // track: drop a queued start/stop (second tap before the bar line)
  CmdResetTrack,       // track: this track's params back to category defaults (generate / fresh track, SPEC §9.17)
  CmdFreezeRamp,       // track, a = ParamId: stop that ramp where it is (hold-ramp released without SHIFT)
  CmdPerformChord,     // track, i = pitch, f = velocity (0 = off): like CmdPerformNote but the UI sends it per chord note; identical handling
  CmdLoopMode,     // track, a = 1 on / 0 off, i = stream (kSets + slot; the Vocal track passes its own set): at the bar line (SPEC §9.19) — on: its synth / sample voices are released, the pattern is parked (not stepped) and the track renders stream i through its strip; off: the parked pattern and patch play again. Vocal track: on = the stream's content is a loop (no pause cycle), off = back to phrases
  CmdLoopLoaded,   // a = slot (0..kLoopSlots-1): nextLoop(slot) describes the new stream content, applied at the bar line like CmdVocalLoaded
};
struct Cmd {
  CmdType type = CmdNone;
  uint8_t track = 0;
  uint8_t a = 0;
  int32_t i = 0;
  float f = 0.0f;
};

// Echo divisions (SPEC §5)
enum EchoDiv : uint8_t { Div16 = 0, Div8, Div8d, Div4, Div4d, Div2, EchoDivCount };
inline float echoDivBeats(EchoDiv d) { static const float b[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f}; return b[d < EchoDivCount ? d : 0]; }

} // namespace lp
