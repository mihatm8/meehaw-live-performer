// Minimal Generator used by the sets tests while gen/generator.cpp is not in the tree (run_sets.sh picks the
// real file when it exists). Implements only the members sets.cpp and test_sets.cpp link against, following
// the engine hand-off protocols of SPEC §9.1/§9.5/§9.6/§9.7: generateSet() as a step()-driven machine (MIDI
// pick → source → publishVariant; kits through the staging buffer with slot acks; one vocal stream opened with
// loadVocal), loadMidiToSource, publishVariant, rebuildVariant, loadVocal, genreName. The vocal ring producer
// is not part of the stub (pumpVocal is a no-op). Stub-only state lives in file statics so that only the
// generator.h members that exist today are touched.
#include "gen/generator.h"
#include "gen/sets.h"
#include "gen/smf.h"
#include "gen/variants.h"
#include "dsp/presets.h"
#include "io/wav.h"
#include <string.h>
#include <stdio.h>

namespace lp {
namespace {

const char* baseName(const char* p) { const char* b = p; for (const char* q = p; *q; q++) if (*q == '/') b = q + 1; return b; }
constexpr uint32_t kCapMs[kSampleSlots] = {kCapKickMs, kCapSnareMs, kCapHhMs, kCapOhMs, kCapPercMs, kCapPercMs, kCapPercMs, kCapPercMs};
constexpr uint8_t kMasks[kVariants] = {0x01, 0x0D, 0x0F, 0x1F};
const char* const kRoles[kSampleSlots][3] = {{"kick", nullptr, nullptr}, {"snare", "clap", nullptr}, {"hh", nullptr, nullptr}, {"oh", "hh", nullptr},
                                            {"perc", "clap", "cymbal"}, {"perc", "clap", "cymbal"}, {"perc", "clap", "cymbal"}, {"perc", "clap", "cymbal"}};

struct KitState { int slot = 0; bool waitingAck = false; uint16_t ack = 0; char path[kMaxPathLen]; } g_kit;
struct VocalFile { IFile* f = nullptr; uint16_t naturalTicks = kTicksPerBar; } g_vocal[kSets];

uint16_t clipLen(uint16_t natural, int lenStep) {
  int len = natural;
  for (int k = 0; k < lenStep; k++) len *= 2;
  for (int k = 0; k > lenStep; k--) len /= 2;
  if (len < kTicksPerBeat / 2) len = kTicksPerBeat / 2;
  if (len > kMaxPatternTicks) len = kMaxPatternTicks;
  return (uint16_t)len;
}

} // namespace

void Generator::init(IFileSystem* fs, Engine* eng, Library* lib) {
  fs_ = fs; eng_ = eng; lib_ = lib; state_ = Idle; genre_ = 0; done_ = total_ = 0; err_[0] = 0; queueN_ = 0;
  for (int t = 0; t < kTracks; t++) info_[t] = TrackGenInfo();
  for (int s = 0; s < kSets; s++) { for (int p = 0; p < 128; p++) foldMap_[s][p] = (uint8_t)(36 + (p & 3)); g_vocal[s] = VocalFile(); }
}

bool Generator::generateSet(int set) {
  if (state_ != Idle || set < 0 || set >= kSets) return false;
  set_ = (uint8_t)set; track_ = 0; done_ = 0; total_ = kTracksPerSet; state_ = TrackBegin; err_[0] = 0;
  g_kit = KitState();
  Cmd c; c.type = CmdSetGenerating; c.a = (uint8_t)set; c.i = 1; eng_->cmds().push(c);
  return true;
}
void Generator::setGenre(int g) { genre_ = g; }
const char* Generator::genreName() const { return (lib_ && genre_ >= 0 && genre_ < lib_->genreCount()) ? lib_->genreName(genre_) : ""; }

// smf parse into the engine source; Percussion keeps the source unfolded and stores its fold map (SPEC §9.7).
bool Generator::loadMidiToSource(int track, const char* path) {
  IFile* f = fs_->open(path, "r");
  if (!f) return false;
  Pattern& src = eng_->sourcePattern(track);
  bool ok = smfParse(*f, src);
  f->close();
  if (!ok) { src.clear(); return false; }
  src.sortByStart();
  if (trackCategory(track) == Percussion) percussionFoldMap(src, foldMap_[trackSet(track)]);
  info_[track].sourceTicks = src.lengthTicks;
  return true;
}

// source + stored seed/key → next buffer, CmdSwapPattern with len = natural × 2^lenStep (SPEC §9.4/§9.5);
// Beat always publishes Full + the rush loop and switches the layer mask (SPEC §9.6).
bool Generator::publishVariant(int track, int variant) {
  if (track < 0 || track >= kTracks) return false;
  if (!eng_->beginNextWrite(track)) return false;
  TrackGenInfo& i = info_[track]; const Pattern& src = eng_->sourcePattern(track);
  const Category cat = trackCategory(track); const int set = trackSet(track);
  ProjectKey k; k.root = (uint8_t)(i.keyRoot % 12);
  Pattern& next = eng_->nextPattern(track);
  variant &= 3;
  if (cat == Beat) { buildVariant(Beat, src, 3, i.seed, k, next); if (!eng_->status().track[track].rush) snareRush(next, eng_->rushPattern(set)); }
  else if (cat == Percussion) buildVariant(Percussion, src, variant, i.seed, k, next, foldMap_[set]);
  else buildVariant(cat, src, variant, i.seed, k, next);
  eng_->endNextWrite(track);
  i.naturalTicks = next.lengthTicks; i.variant = (uint8_t)variant; i.loaded = 1; i.lenStep = 0;   // k reset on publish (SPEC §9.4)
  Cmd c; c.type = eng_->status().running ? CmdSwapPattern : CmdSwapPatternNow; c.track = (uint8_t)track; c.a = (uint8_t)variant; c.i = clipLen(i.naturalTicks, i.lenStep);
  eng_->cmds().push(c);
  if (cat == Beat) { Cmd m; m.type = CmdBeatMask; m.a = (uint8_t)set; m.i = kMasks[variant] | (variant << 8); eng_->cmds().push(m); }
  return true;
}

// Vocal: an empty pattern whose lengthTicks is the natural clip length (variants 3/4: 2 / 1 beats, SPEC §9.3).
void Generator::rebuildVariant(int track, int variant) {
  if (trackCategory(track) != Vocal) { publishVariant(track, variant); return; }
  const int set = trackSet(track); variant &= 3;
  if (!g_vocal[set].f || !eng_->beginNextWrite(track)) return;
  TrackGenInfo& i = info_[track];
  i.naturalTicks = variant >= 2 ? (uint16_t)(variant == 2 ? 2 * kTicksPerBeat : kTicksPerBeat) : g_vocal[set].naturalTicks;
  eng_->nextPattern(track).clear(i.naturalTicks);
  eng_->endNextWrite(track);
  i.variant = (uint8_t)variant; i.loaded = 1; i.lenStep = 0;
  Cmd c; c.type = eng_->status().running ? CmdSwapPattern : CmdSwapPatternNow; c.track = (uint8_t)track; c.a = (uint8_t)variant; c.i = clipLen(i.naturalTicks, i.lenStep);
  eng_->cmds().push(c);
}

// Opens the stream and publishes the VocalState (SPEC §9.3); the producer itself is not part of the stub.
bool Generator::loadVocal(int set, const char* path, int sel) {
  (void)sel;
  VocalFile& v = g_vocal[set];
  if (v.f) { v.f->close(); v.f = nullptr; }
  IFile* f = fs_->open(path, "r");
  if (!f) return false;
  WavInfo wi;
  if (!wavReadHeader(*f, wi)) { f->close(); return false; }
  v.f = f;
  VocalState& nv = eng_->nextVocal(set);
  nv.gain = 1.0f; nv.present = 1; nv.fileBpm = Sets::bpmToken(path);
  v.naturalTicks = Sets::naturalVocalTicks(wi.dataBytes / (wi.channels * 2u), wi.sampleRate, nv.fileBpm);
  nv.loopTicks = v.naturalTicks;
  Cmd c; c.type = CmdVocalLoaded; c.a = (uint8_t)set; eng_->cmds().push(c);
  TrackGenInfo& i = info_[set * kTracksPerSet + Vocal];
  strncpy(i.file, baseName(path), kMaxNameLen - 1); i.file[kMaxNameLen - 1] = 0; i.fileBpm = nv.fileBpm; i.naturalTicks = v.naturalTicks; i.sourceTicks = v.naturalTicks;
  return true;
}
void Generator::pumpVocal() {}
void Generator::setVocalPath(int set, int sel, const char* path) { char* dst = (sel & 1) ? altPath_[set & 1] : stream_[set & 1].path; strncpy(dst, path ? path : "", kMaxPathLen - 1); dst[kMaxPathLen - 1] = 0; }   // VSet::path is one path per stream; the alternate phrase lives in altPath_

// The loader's set-wide burst (SPEC §9.5): every track in the mask gets CmdSwapPattern (len = natural × 2^lenStep),
// the Beat its layer mask, then CmdSwapSet. The stub streams no vocal, so nothing ever waits.
bool Generator::pushSetSwaps(int set) {
  for (int i = 0; i < kTracksPerSet; i++) {
    if (!((swapMask_[set] >> i) & 1)) continue;
    const int t = set * kTracksPerSet + i; const TrackGenInfo& inf = info_[t];
    Cmd c; c.type = CmdSwapPattern; c.track = (uint8_t)t; c.a = inf.variant; c.i = inf.lenStep ? clipLen(inf.naturalTicks, inf.lenStep) : 0; eng_->cmds().push(c);
    if (i == Beat) { Cmd m; m.type = CmdBeatMask; m.a = (uint8_t)set; m.i = kMasks[inf.variant & 3] | ((inf.variant & 3) << 8); eng_->cmds().push(m); }
  }
  swapMask_[set] = 0;
  Cmd s; s.type = CmdSwapSet; s.a = (uint8_t)set; eng_->cmds().push(s);
  return true;
}
bool Generator::requestSetSwap(int set, uint8_t mask) {
  if (set < 0 || set >= kSets) return false;
  swapMask_[set] |= mask; stream_[set].setJobWaiting = 0;
  return pushSetSwaps(set);
}
bool Generator::setSwapWaiting(int set) const { return set >= 0 && set < kSets && stream_[set].setJobWaiting != 0; }
// SPEC §9.19 loop mode and the session writer's remembered paths: not part of the stub (loop mode is always off, paths unknown, the gate open).
bool Generator::setLoopMode(int, bool) { loopRefusal_ = LoopNoCard; return false; }
bool Generator::loopEnabled(int) const { return false; }
int Generator::loopSlot(int) const { return -1; }
const char* Generator::loopFile(int) const { return ""; }
const char* Generator::loopPath(int) const { return ""; }
uint8_t Generator::loopBars(int) const { return 0; }
bool Generator::anyLoop() const { return false; }
const char* Generator::loopFolder(Category c) { return loopFolderName(c); }
bool Generator::loadLoop(int, const char*, int, int) { return false; }
bool Generator::sourcePath(int, char* out, int cap) const { if (out && cap > 0) out[0] = 0; return false; }
bool Generator::kitPath(int, int, char* out, int cap) const { if (out && cap > 0) out[0] = 0; return false; }
void Generator::noteKitPath(int, int, const char*) {}
bool Generator::streamsHealthy() const { return true; }
bool Generator::loopSlotStreaming() const { return false; }   // no loop slot ever streams in the stub: the session writer is never held back
void Generator::rushLoop(int, bool) {}                         // no loop track to crop

// len± (SPEC §9.4): k steps, len = natural × 2^k clamped 0.5..16 beats; CmdSetLength at the bar (vocal: re-published loop).
uint16_t Generator::setLengthStep(int track, int dir) {
  TrackGenInfo& i = info_[track];
  int k = i.lenStep + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
  if (k > 5) k = 5; if (k < -5) k = -5;
  const uint16_t natural = i.naturalTicks ? i.naturalTicks : (uint16_t)kTicksPerBar;
  while (k > 0 && clipLen(natural, k) == clipLen(natural, k - 1)) k--;
  while (k < 0 && clipLen(natural, k) == clipLen(natural, k + 1)) k++;
  i.lenStep = (int8_t)k;
  const uint16_t len = clipLen(natural, k);
  if (trackCategory(track) == Vocal) { int8_t keep = i.lenStep; rebuildVariant(track, i.variant); i.lenStep = keep; }
  Cmd c; c.type = CmdSetLength; c.track = (uint8_t)track; c.i = len; eng_->cmds().push(c);
  return len;
}

void Generator::step() {
  if (state_ == Idle) return;
  if (state_ == TrackBegin) {                                       // one MIDI track per step
    const int t = set_ * kTracksPerSet + track_; const Category cat = trackCategory(t);
    if (cat != Vocal) {
      TrackGenInfo& i = info_[t]; i = TrackGenInfo(); i.keyRoot = key_.root;
      if (lib_->pickMidi(cat, key_, t, rng_, path_, sizeof path_) && loadMidiToSource(t, path_)) {
        strncpy(i.file, baseName(path_), kMaxNameLen - 1); i.seed = rng_.next(); i.presetIndex = isSynthCategory(cat) ? (uint8_t)(t % presetCount(cat)) : 0;
        if (isSynthCategory(cat)) { makePatch(cat, i.presetIndex, i.seed, eng_->nextPatch(t)); Cmd c; c.type = CmdSetPatch; c.track = (uint8_t)t; eng_->cmds().push(c); }
        if (!publishVariant(t, cat == Beat ? 3 : cat == Percussion ? 2 : 0)) return;   // lock held: retry
      }
    }
    track_++; done_++;
    if (track_ >= kTracksPerSet) { state_ = SlotPick; g_kit = KitState(); }
    return;
  }
  if (state_ == SlotPick) {                                         // one slot per step, ack-gated (SPEC §9.1)
    if (g_kit.waitingAck) {
      if (eng_->status().slotAck == g_kit.ack) return;
      eng_->setKitName(set_, g_kit.slot, baseName(g_kit.path));
      g_kit.waitingAck = false; g_kit.slot++;
      if (g_kit.slot >= kSampleSlots) state_ = VocalPick;
      return;
    }
    const int slot = g_kit.slot; bool found = false;
    for (int r = 0; r < 3 && !found && kRoles[slot][r]; r++)
      found = lib_->pickSample(genreName(), kRoles[slot][r], slot == 0 ? (int)key_.root : -1, rng_, g_kit.path, sizeof g_kit.path);
    IFile* f = found ? fs_->open(g_kit.path, "r") : nullptr;
    WavInfo wi; uint32_t len = 0;
    if (f) { if (wavReadHeader(*f, wi)) len = wavLoadUlaw(*f, wi, eng_->staging(), kCapMs[slot] * (uint32_t)kSampleRate / 1000u, 0.0f); f->close(); }
    if (!len) { g_kit.slot++; if (g_kit.slot >= kSampleSlots) state_ = VocalPick; return; }
    g_kit.ack = eng_->status().slotAck;
    Cmd c; c.type = CmdSlotSwap; c.a = set_; c.i = (int32_t)((uint32_t)slot | (len << 4)); c.f = 0; eng_->cmds().push(c);
    g_kit.waitingAck = true;
    return;
  }
  if (state_ == VocalPick) {
    const int t = set_ * kTracksPerSet + Vocal;
    TrackGenInfo& i = info_[t]; i = TrackGenInfo(); i.keyRoot = key_.root;
    if (lib_->pickVocal(genreName(), key_, set_, rng_, path_, sizeof path_) && loadVocal(set_, path_, 0)) { i.seed = rng_.next(); rebuildVariant(t, 0); }
    Cmd s; s.type = CmdSwapSet; s.a = set_; eng_->cmds().push(s);
    s.type = CmdSetGenerating; s.i = 0; eng_->cmds().push(s);
    state_ = Idle;
  }
}

} // namespace lp
