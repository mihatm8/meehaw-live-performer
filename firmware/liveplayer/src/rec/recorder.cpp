// meehaw live performer — session recorder (SPEC §9.23; the contract is recorder.h). Files on the card:
//   /lp/rec/rec_<NNN>.lprec   the 64-byte RecHeader + 8-byte little-endian RecEvents, appended kBuf = 32 at a time (one 256 B write)
//   /lp/wav/rec_<NNN>.wav     a 44-byte RIFF / WAVE header (PCM, 22050 Hz, mono, int16) + one 512 B block per renderStep
// The header is written at startRecording from the live state; stopRecording rewrites bytes 48..55 (events, lengthTicks).
// reserved[0..1] = the tracks playing or queued when the recording started (bit per track, little-endian; 0 when the
// transport was stopped), reserved[2..3] = the bar phase (ticks into the bar) of the recording start. The replay restores
// both: the masked tracks are queued for a bar line and the rendered time axis is shifted so that every recorded tick
// falls on the bar phase it had live (bar-quantised launches and swaps then land on the same bars).
// reserved[4..7] = the state the recorded keys act on (review 2026-10-16): [4] the selected track, [5] bit s = kick cut of
// set s, bit 2 = gyro on, [6] rampBars, [7] bit 0 = those three are valid (0 in files written before: nothing restored).
// startRender brings them back before the first block: the selection by a synthesised SHIFT + track (select-only, SPEC §3),
// rampBars by SHIFT + len+ taps, the kick cuts and the gyro by engine commands (the Ui's kick-cut shadows follow the engine
// status within kKickCutSyncMs; its gyro flag is NOT restored — the recorded type-2 events own the gyro FX during a render,
// and a replayed toggle's own command is overridden by the type-2 event recorded right after it). Still not restored:
// variants, len steps, loop mode, acid flags and whatever content was loaded before the recording started.
// Pseudo key (255, a): the master play / stop of the side button (not a matrix key). The sketch records it as a type 0
// event with x = 255, y = a (1 run, 0 stop); the replay pushes CmdMasterPlay a instead of a key event.
// Hold repeats: the KeyMatrix's Repeat events (after kKeyRepeatDelayMs, every kKeyRepeatIntervalMs) are not recorded —
// the replay synthesises them for the keys it holds (kHeld at once), on the replay clock, so tempo± / len± / volume holds
// step as live.
// Generator state: the generator's Rng is private (no accessor), so startRecording draws a fresh seed, re-seeds the
// generator (Generator::seed also restarts every track's preset bag) and restarts the library bags (setKeyRoot(current
// root) → Library::invalidate) — the recording begins from a KNOWN generator state, which the replay re-creates (seed,
// genre, key, bags). The preset sounding on each track (presetIndex) is not recorded: a sound pick avoids the preset
// sounding at pick time, which on the device is the render-time one.
// Sets state (review 2026-10-16): a random LOAD draws over the presets not yet loaded in the current cycle and never the
// set's last loaded name (SPEC §9.26 / §9.8), so both starts also call Sets::resetLoadCycle() — masks and last names
// cleared, as a fresh boot has them — and the replayed load runs its reservoir over the same candidates as the recording
// (the Ui seeds its load Rng from sessionSeed() at both starts). UX cost: REC ON (and a render) restarts the load cycle
// of both sets, the preset manager's dots vanish; a snapshot of the masks in the file would avoid it (not done: 256 B).
// Durability: every flush of the event buffer is followed by IFile::sync() (f_sync on the card: the FIL buffer and the
// directory entry — size, chain — go out, two sector writes at most, a few ms), so a power loss mid-recording leaves a
// file replayable to its last flushed event (the counts stay 0: sessionInfo derives them from the size) instead of a
// 0-byte file with a lost chain; the header write is synced too (a session with no event yet is a valid file).
// Clock: the replay runs the Ui on renderNowMs() (the engine's sample clock since startRender); renderStep ticks the Ui
// itself, so the sketch must not tick it with millis() meanwhile (mixed clocks would misfire the Ui's hold timers).
// Ui dispatch of a replayed (or synthesised) key: uiKey() sets replaying() for the call, and the Ui keeps its live-only
// reactions — arming the ` cancel hold, saves, the drive hold, the preset manager's rewrites — out of such keys (a
// recorded 1.5 s hold of ` is a long track-1 press, not a cancel); the sketch's live keys arrive between steps with
// rendering() true and replaying() false.
// Session file reads during a render are transient (open / seek / read / close per kBuf-event chunk) so the WAV handle is
// the render's only standing one (SPEC §9.19g). The chunk the next events need is read at the top of a step, BEFORE the
// block renders: when the open fails (the 7-handle pool full for a step: four streams + a pending switch + a Sets write +
// the WAV) nothing advances — the streams are pumped and the generator / Sets step so the handle frees — and the read is
// retried next step; kOpenRetrySteps failures in a row end the render WITHOUT a WAV (never a silently shortened one).
#include "recorder.h"
#include "../engine/engine.h"
#include "../gen/generator.h"
#include "../gen/sets.h"
#include "../ui/ui.h"
#include <stdio.h>
#include <string.h>

namespace lp {

static_assert(sizeof(RecEvent) == 8, "RecEvent is the 8-byte card record");
static_assert(sizeof(RecHeader) == 64, "RecHeader is the 64-byte card header");
// 448 B on the device (32-bit pointers; measured 416 B with the Xtensa toolchain after the 2026-10-16 RAM diet's kBuf 64 → 32,
// 672 B before); the host carries 7 pointers of 8 bytes.
static_assert(sizeof(Recorder) <= 448 + 7 * (int)(sizeof(void*) - 4), "Recorder over its 448 B budget (SPEC §9.23, RAM diet 2026-10-16)");

namespace {

constexpr char kMagic[8] = {'L', 'P', 'R', 'E', 'C', '1', 0, 0};
constexpr uint32_t kHeaderBytes = 64, kWavHeaderBytes = 44, kBlockBytes = kBlock * 2;
constexpr uint32_t kOffCounts = 48;                   // header offset of events / lengthTicks
constexpr int kOffSelected = 4, kOffFlags = 5, kOffRampBars = 6, kOffValid = 7;   // reserved[] offsets of the restored Ui state
constexpr uint8_t kFlagGyroOn = 4;                    // reserved[kOffFlags] bits 0..1 = kick cut S1 / S2, bit 2 = gyro on
constexpr int kShiftX = 13, kShiftY = 3;              // physical SHIFT (SPEC §3)

inline void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }
inline int popcount64(uint64_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }
inline uint32_t roundUpBar(uint32_t tick) { return ((tick + kTicksPerBar - 1) / kTicksPerBar) * kTicksPerBar; }   // where CmdMasterPlay 1 puts the tick

void recPath(char* out, int cap, int n) { snprintf(out, (size_t)cap, "%s/rec_%03d.lprec", Recorder::kRecDir, n); }
void wavPath(char* out, int cap, int n) { snprintf(out, (size_t)cap, "%s/rec_%03d.wav", Recorder::kWavDir, n); }

void encodeHeader(const RecHeader& h, uint8_t* b) {
  memset(b, 0, kHeaderBytes);
  memcpy(b, h.magic, 8); put32(b + 8, h.seed); put16(b + 12, h.bpm); b[14] = (uint8_t)h.keyRoot; b[15] = h.set1Autoplay;
  memcpy(b + 16, h.genre, 32); put32(b + kOffCounts, h.events); put32(b + kOffCounts + 4, h.lengthTicks); memcpy(b + 56, h.reserved, 8);
}
bool decodeHeader(const uint8_t* b, RecHeader& h) {
  if (memcmp(b, kMagic, 6) != 0) return false;        // "LPREC1" (the two trailing bytes are padding)
  memcpy(h.magic, b, 8); h.seed = get32(b + 8); h.bpm = get16(b + 12); h.keyRoot = (int8_t)b[14]; h.set1Autoplay = b[15];
  memcpy(h.genre, b + 16, 32); h.genre[31] = 0; h.events = get32(b + kOffCounts); h.lengthTicks = get32(b + kOffCounts + 4); memcpy(h.reserved, b + 56, 8);
  return true;
}
void wavHeader(uint8_t* b, uint32_t dataBytes) {
  memcpy(b, "RIFF", 4); put32(b + 4, 36 + dataBytes); memcpy(b + 8, "WAVEfmt ", 8);
  put32(b + 16, 16); put16(b + 20, 1); put16(b + 22, 1);                      // PCM, mono
  put32(b + 24, kSampleRate); put32(b + 28, kSampleRate * 2); put16(b + 32, 2); put16(b + 34, 16);
  memcpy(b + 36, "data", 4); put32(b + 40, dataBytes);
}

// Directory walk: rec_<NNN>.<ext> → bit NNN. Anything else in the folder is ignored.
struct ScanCtx { uint64_t mask; const char* ext; };
bool scanFn(const char* name, bool isDir, uint32_t, void* user) {
  ScanCtx& c = *static_cast<ScanCtx*>(user);
  if (isDir || strncmp(name, "rec_", 4) != 0) return true;
  int n = 0, digits = 0; const char* p = name + 4;
  while (*p >= '0' && *p <= '9' && digits < 4) { n = n * 10 + (*p - '0'); p++; digits++; }
  if (digits == 3 && n < Recorder::kMaxSessions && strlen(p) == strlen(c.ext) && hasExt(name, c.ext)) c.mask |= 1ull << n;
  return true;
}

// The matrix key bound to (fn, a) in keys.cpp's map, or false (the map is the contract: the key for a track or a param ± is not hard-coded here).
bool findKey(KeyFn fn, int a, int& x, int& y) {
  for (y = 0; y < kKeyRows; y++) for (x = 0; x < kKeyCols; x++) { const KeyMapping m = mapKey(x, y); if (m.fn == fn && m.a == a) return true; }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------- init / listing

void Recorder::init(IFileSystem* fs, Engine* eng, Generator* gen, Sets* sets, Ui* ui) {
  fs_ = fs; eng_ = eng; gen_ = gen; sets_ = sets; ui_ = ui;
  recording_ = rendering_ = replaying_ = false; bufN_ = 0; count_ = 0; startTick_ = 0; rec_ = wav_ = nullptr; written_ = 0; cur_ = -1;
  replayed_ = total_ = lengthTicks_ = 0; base_ = t0_ = chunkStart_ = 0; chunkN_ = first_ = tail_ = openFails_ = 0; tailLeft_ = 0; clock0_ = ms0_ = 0; seed_ = 0;
  for (Held& h : held_) h = Held();
  recMask_ = wavMask_ = 0; sessions_ = -1; name_[0] = 0;
}

void Recorder::scan() {
  ScanCtx a{0, ".lprec"}, b{0, ".wav"};
  if (fs_) { fs_->walk(kRecDir, scanFn, &a); fs_->walk(kWavDir, scanFn, &b); }
  recMask_ = a.mask; wavMask_ = b.mask; sessions_ = (int8_t)popcount64(recMask_);
}

int Recorder::index(int i) {
  if (!fs_ || i < 0) return -1;
  if (sessions_ < 0) scan();
  if (i >= sessions_) return -1;
  for (int n = 0; n < kMaxSessions; n++) if ((recMask_ >> n) & 1) { if (i-- == 0) return n; }
  return -1;
}

int Recorder::sessionCount() { if (!fs_) return 0; if (sessions_ < 0) scan(); return sessions_; }

const char* Recorder::sessionName(int i) {
  const int n = index(i);
  if (n < 0) { name_[0] = 0; return name_; }
  snprintf(name_, sizeof name_, "rec_%03d", n);
  return name_;
}

bool Recorder::sessionInfo(int i, RecHeader& out) {
  const int n = index(i); if (n < 0) return false;
  char p[32]; recPath(p, sizeof p, n);
  IFile* f = fs_->open(p, "r"); if (!f) return false;
  uint8_t b[kHeaderBytes];
  const bool ok = f->read(b, kHeaderBytes) == kHeaderBytes && decodeHeader(b, out);
  if (ok) {
    const uint32_t size = f->size(), inFile = size > kHeaderBytes ? (size - kHeaderBytes) / sizeof(RecEvent) : 0;
    if (out.events == 0 || out.events > inFile) out.events = inFile;   // counts never written (power loss) or a short write: the file is the truth
    if (out.lengthTicks == 0 && out.events) {                          // replayable to its last event
      uint8_t e[8];
      if (f->seek(kHeaderBytes + (out.events - 1) * sizeof(RecEvent)) && f->read(e, sizeof e) == sizeof e) out.lengthTicks = get32(e);
    }
  }
  f->close();
  return ok;
}

bool Recorder::hasWav(int i) { const int n = index(i); return n >= 0 && ((wavMask_ >> n) & 1) != 0; }

bool Recorder::deleteSession(int i) {
  if (recording_ || rendering_) return false;
  const int n = index(i); if (n < 0) return false;
  char p[32]; recPath(p, sizeof p, n);
  const bool ok = fs_->remove(p);
  if ((wavMask_ >> n) & 1) { wavPath(p, sizeof p, n); fs_->remove(p); }
  if (ok) recMask_ &= ~(1ull << n);
  wavMask_ &= ~(1ull << n); sessions_ = (int8_t)popcount64(recMask_);
  return ok;
}

// ---------------------------------------------------------------------------------------------- recording

bool Recorder::startRecording() {
  if (!fs_ || !eng_ || recording_ || rendering_) return false;
  if (!fs_->isDir(kRecDir) && !fs_->mkdir(kRecDir)) return false;
  if (sessions_ < 0) scan();
  int n = -1;
  for (int k = 0; k < kMaxSessions && n < 0; k++) if (!((recMask_ >> k) & 1)) n = k;   // the lowest free number (gaps from deletes are reused)
  if (n < 0) return false;
  const EngineStatus& st = eng_->status();
  uint32_t seed = (st.sampleClock * 2654435761u) ^ (st.tick * 40503u) ^ 0x9E3779B9u ^ ((uint32_t)n << 24);
  if (!seed) seed = 1;
  if (gen_) { gen_->seed(seed); gen_->setKeyRoot((int)gen_->key().root); }   // a known generator state: this seed, fresh preset and library bags (the key is unchanged)
  if (sets_) sets_->resetLoadCycle();                 // a known Sets state: empty load-cycle masks and no "last loaded" names, as at the render start (see the note at the top)
  RecHeader h{};
  memcpy(h.magic, kMagic, 8); h.seed = seed; h.bpm = st.bpm; h.keyRoot = gen_ ? (int8_t)gen_->key().root : (int8_t)9;
  uint16_t mask = 0;
  for (int t = 0; t < kTracks; t++) { const PlayState s = st.track[t].state; if (s == PSPlaying || s == PSQueued) mask |= (uint16_t)(1u << t); }
  if (!st.running) mask = 0;
  h.set1Autoplay = (mask & 0xFF) ? 1 : 0;
  if (gen_) { strncpy(h.genre, gen_->genreName(), sizeof h.genre - 1); h.genre[sizeof h.genre - 1] = 0; }
  put16(h.reserved, mask); put16(h.reserved + 2, (uint16_t)(st.tick % kTicksPerBar));
  if (ui_) {                                          // the Ui / engine state the recorded keys will act on (restored by restoreLive)
    h.reserved[kOffSelected] = (uint8_t)ui_->selectedTrack();
    uint8_t flags = 0;
    for (int s = 0; s < kSets; s++) if (st.track[s * kTracksPerSet + Beat].kickCut) flags |= (uint8_t)(1u << s);
    if (ui_->gyroOn()) flags |= kFlagGyroOn;
    h.reserved[kOffFlags] = flags; h.reserved[kOffRampBars] = (uint8_t)ui_->rampBars(); h.reserved[kOffValid] = 1;
  }
  char p[32]; recPath(p, sizeof p, n);
  IFile* f = fs_->open(p, "w"); if (!f) return false;
  uint8_t b[kHeaderBytes]; encodeHeader(h, b);
  if (f->write(b, kHeaderBytes) != kHeaderBytes) { f->close(); fs_->remove(p); return false; }
  f->sync();                                          // a valid (empty) session is on the card from here on, whatever happens next
  rec_ = f; cur_ = (int8_t)n; bufN_ = 0; count_ = 0; startTick_ = st.tick; recording_ = true; seed_ = seed;
  recMask_ |= 1ull << n; sessions_ = (int8_t)popcount64(recMask_);
  return true;
}

void Recorder::onInput(const RecEvent& e) {
  if (!recording_) return;
  if (bufN_ >= kBuf) flush();                        // a full buffer goes to the card now (the Ui never loses an event)
  if (bufN_ >= kBuf) return;
  RecEvent& r = buf_[bufN_++]; r = e;
  const uint32_t t = eng_ ? eng_->status().tick : 0;   // stamped here: the engine tick the Ui saw when it handled the event
  r.tick = (int32_t)(t - startTick_) >= 0 ? t - startTick_ : 0;
  count_++;
}

// One bounded card operation: the buffered events (≤ 512 B) written, then synced so the bytes AND the file size are on the
// card (see the durability note at the top). A short write leaves a shorter file, which the replay reads as far as it goes.
void Recorder::flush() {
  if (rec_ && bufN_) { rec_->write(buf_, (size_t)bufN_ * sizeof(RecEvent)); rec_->sync(); }
  bufN_ = 0;
}

void Recorder::step() {
  if (!recording_ || !bufN_) return;
  // SPEC §9.19 §3: card work only while every consumed stream holds its lead and no 139 ms slot ring is being consumed
  // (the Sets writer's rule); a full buffer flushes regardless (onInput does the same).
  if (bufN_ < kBuf && gen_ && (!gen_->streamsHealthy() || gen_->loopSlotStreaming())) return;
  flush();
}

void Recorder::stopRecording() {
  if (!recording_) return;
  flush();
  const uint32_t t = eng_ ? eng_->status().tick : startTick_;
  const uint32_t len = (int32_t)(t - startTick_) >= 0 ? t - startTick_ : 0;
  if (rec_) {
    uint8_t b[8]; put32(b, count_); put32(b + 4, len);
    if (rec_->seek(kOffCounts)) rec_->write(b, sizeof b);
    rec_->close(); rec_ = nullptr;
  }
  recording_ = false; cur_ = -1; seed_ = 0;
}

// ---------------------------------------------------------------------------------------------- offline render

bool Recorder::startRender(int i) {
  if (!fs_ || !eng_ || recording_ || rendering_) return false;
  if ((gen_ && gen_->busy()) || (sets_ && sets_->busy())) return false;   // a job owns the card and the engine's next buffers
  const int n = index(i); if (n < 0) return false;
  RecHeader h;
  if (!sessionInfo(i, h)) return false;
  if (!fs_->isDir(kWavDir) && !fs_->mkdir(kWavDir)) return false;
  const uint16_t mask = get16(h.reserved), phase = get16(h.reserved + 2);
  CmdQueue& q = eng_->cmds();
  if (q.size() + 8 + kTracks + popcount64(mask) >= kCmdQueueSize - 1) return false;   // the reset burst (+ restoreLive's kick cuts, gyro and a possible CmdCancelRamps) goes out whole or not at all
  char p[32]; wavPath(p, sizeof p, n);
  IFile* w = fs_->open(p, "w"); if (!w) return false;
  uint8_t wh[kWavHeaderBytes]; wavHeader(wh, 0);
  if (w->write(wh, kWavHeaderBytes) != kWavHeaderBytes) { w->close(); fs_->remove(p); return false; }
  // The generator back to the recorded state (see the note at the top): Rng + preset bags, genre (restarts the library bags), key;
  // Sets back to the state the recording started from: empty load-cycle masks, no "last loaded" names (a replayed random LOAD
  // then draws over the same candidates as live).
  if (gen_) { gen_->seed(h.seed); if (h.genre[0]) gen_->setGenreByName(h.genre); gen_->setKeyRoot(h.keyRoot); }
  if (sets_) sets_->resetLoadCycle();
  // The engine: params and ramps to defaults; the transport restarted (CmdMasterPlay 0 then 1 puts the tick on a bar line with
  // phase 0 at the next block start); every track stopped (1 resumes what 0 stopped); the recorded tempo; the tracks that
  // played at the recording start queued for the bar line after that (onBarLine fires on an incremented tick).
  Cmd c; c.type = CmdResetAll; q.push(c);
  c = Cmd(); c.type = CmdMasterPlay; c.a = 0; q.push(c); c.a = 1; q.push(c);
  c = Cmd(); c.type = CmdSetTempo; c.i = (h.bpm >= kMinBpm && h.bpm <= kMaxBpm) ? h.bpm : kDefaultBpm; q.push(c);
  for (int t = 0; t < kTracks; t++) { c = Cmd(); c.type = CmdTrackStopNow; c.track = (uint8_t)t; q.push(c); }
  for (int t = 0; t < kTracks; t++) if ((mask >> t) & 1) { c = Cmd(); c.type = CmdTrackStart; c.track = (uint8_t)t; q.push(c); }
  const EngineStatus& st = eng_->status();
  t0_ = roundUpBar(st.tick);
  base_ = t0_ + kTicksPerBar + phase;                 // one bar of pre-roll (the masked tracks start at its end), then the recorded bar phase
  wav_ = w; cur_ = (int8_t)n; written_ = 0; replayed_ = 0; total_ = h.events; lengthTicks_ = h.lengthTicks; seed_ = h.seed;
  chunkStart_ = 0; chunkN_ = 0; first_ = 1; tail_ = 0; tailLeft_ = 0; openFails_ = 0;
  clock0_ = st.sampleClock;
  ms0_ = g_microsHook ? g_microsHook() / 1000u : (uint32_t)((uint64_t)st.sampleClock * 1000u / kSampleRate);
  for (Held& hd : held_) hd = Held();
  rendering_ = true;
  restoreLive(h, ms0_);
  return true;
}

// The state the session's keys act on, before the first block (see reserved[4..7] at the top). The page first: a render
// started from the RECORDER page would otherwise replay its events INTO that page (ent = the recording toggle, m = render,
// del = delete) — live, the ent that started the recording closed the page, so the replay must begin on the main page too;
// ` closes it as a user's ` would. Each synthesised key goes through uiKey (replaying() true, never recorded: recording_ is false).
void Recorder::restoreLive(const RecHeader& h, uint32_t ms) {
  if (!ui_ || !eng_) return;
  if (ui_->page() == PgRecorder) { uiKey(0, 0, KeyEvent::Press, ms); uiKey(0, 0, KeyEvent::Release, ms); }
  if (!(h.reserved[kOffValid] & 1)) return;
  int x = 0, y = 0;
  const int sel = h.reserved[kOffSelected];
  if (sel < kTracks && sel != ui_->selectedTrack() && findKey(KTrack, sel, x, y)) {   // SHIFT + track selects without toggling (SPEC §3)
    uiKey(kShiftX, kShiftY, KeyEvent::Press, ms); uiKey((uint8_t)x, (uint8_t)y, KeyEvent::Press, ms); uiKey((uint8_t)x, (uint8_t)y, KeyEvent::Release, ms); uiKey(kShiftX, kShiftY, KeyEvent::Release, ms);
  }
  const int bars = h.reserved[kOffRampBars];
  if (bars >= 1 && findKey(KParamUp, PLength, x, y)) {                             // SHIFT + len+ cycles the ramp length table (kRampBarsSteps entries)
    for (int i = 0; i < kRampBarsSteps && ui_->rampBars() != bars; i++) {
      uiKey(kShiftX, kShiftY, KeyEvent::Press, ms); uiKey((uint8_t)x, (uint8_t)y, KeyEvent::Press, ms); uiKey((uint8_t)x, (uint8_t)y, KeyEvent::Release, ms); uiKey(kShiftX, kShiftY, KeyEvent::Release, ms);
    }
  }
  CmdQueue& q = eng_->cmds(); Cmd c;
  for (int s = 0; s < kSets; s++) { c = Cmd(); c.type = CmdKickCut; c.a = (uint8_t)s; c.i = (h.reserved[kOffFlags] >> s) & 1; q.push(c); }
  c = Cmd(); c.type = CmdGyro; c.a = (h.reserved[kOffFlags] & kFlagGyroOn) ? 1 : 0; c.f = 0; c.i = 0; q.push(c);   // neutral; the recorded type-2 events take over
}

uint32_t Recorder::renderNowMs() const {
  const uint32_t clock = eng_ ? eng_->status().sampleClock : 0;
  return ms0_ + (uint32_t)((uint64_t)(clock - clock0_) * 1000u / kSampleRate);
}

int Recorder::renderPercent() const {
  if (!rendering_ || !eng_) return 0;
  const uint32_t tick = eng_->status().tick, span = lengthTicks_ + kTicksPerBar;
  if ((int32_t)(tick - base_) <= 0) return 0;
  const uint64_t pct = (uint64_t)(tick - base_) * 100u / (span ? span : 1u);
  return pct > 100 ? 100 : (int)pct;
}

bool Recorder::readChunk() {
  if (!fs_ || cur_ < 0 || replayed_ >= total_) return false;
  char p[32]; recPath(p, sizeof p, cur_);
  IFile* f = fs_->open(p, "r"); if (!f) return false;   // transient: one handle, open only for the read (SPEC §9.19g budget); the caller retries
  uint32_t n = total_ - replayed_; if (n > (uint32_t)kBuf) n = kBuf;
  const size_t got = f->seek(kHeaderBytes + replayed_ * sizeof(RecEvent)) ? f->read(buf_, n * sizeof(RecEvent)) : 0;
  f->close();
  chunkStart_ = replayed_; chunkN_ = (uint8_t)(got / sizeof(RecEvent));
  return chunkN_ > 0;
}

void Recorder::holdPress(uint8_t x, uint8_t y, uint32_t ms) {
  for (Held& h : held_) if (h.used && h.x == x && h.y == y) { h.nextMs = ms + kKeyRepeatDelayMs; return; }
  for (Held& h : held_) if (!h.used) { h.used = 1; h.x = x; h.y = y; h.nextMs = ms + kKeyRepeatDelayMs; return; }
}
void Recorder::holdRelease(uint8_t x, uint8_t y) { for (Held& h : held_) if (h.used && h.x == x && h.y == y) h.used = 0; }

// One key to the Ui as a replayed event: replaying() is true for the call (and false again before the sketch's live keys
// arrive between steps), which is how the Ui keeps its live-only reactions — the ` cancel arming, saves, the drive hold,
// the preset manager's rewrites — out of the replay (ui.cpp checks Recorder::replaying()).
void Recorder::uiKey(uint8_t x, uint8_t y, KeyEvent::Kind kind, uint32_t ms) {
  if (!ui_) return;
  KeyEvent k; k.x = x; k.y = y; k.kind = kind; k.ms = ms; k.heldMs = 0;
  replaying_ = true;
  ui_->onKey(k, ms);
  replaying_ = false;
}

// KeyMatrix::endScan's repeat rule on the replay clock: after kKeyRepeatDelayMs, then every kKeyRepeatIntervalMs on schedule,
// one repeat per step at most (a stalled schedule re-arms from now).
void Recorder::repeats(uint32_t ms) {
  if (!ui_) return;
  for (Held& h : held_) {
    if (!h.used || (int32_t)(ms - h.nextMs) < 0) continue;
    h.nextMs += kKeyRepeatIntervalMs;
    if ((int32_t)(h.nextMs - ms) <= 0) h.nextMs = ms + kKeyRepeatIntervalMs;
    uiKey(h.x, h.y, KeyEvent::Repeat, ms);
  }
}

void Recorder::deliver(const RecEvent& e, uint32_t ms) {
  switch (e.type) {
    case 0: case 1: {
      if (e.x == 255) {                                 // pseudo key: the side button's master play / stop
        if (e.type == 0 && eng_) { Cmd c; c.type = CmdMasterPlay; c.a = e.y ? 1 : 0; eng_->cmds().push(c); }
        return;
      }
      if (e.type == 0) holdPress(e.x, e.y, ms); else holdRelease(e.x, e.y);
      uiKey(e.x, e.y, e.type == 0 ? KeyEvent::Press : KeyEvent::Release, ms);
      return;
    }
    case 2:                                             // gyro: filter -128..127 → -1..1, reverb / echo sends 0..255 (the Ui's CmdGyro layout)
      if (eng_) { Cmd c; c.type = CmdGyro; c.a = 1; c.f = (float)(int8_t)e.x / 127.0f; c.i = ((int32_t)e.y << 8) | e.extra; eng_->cmds().push(c); }
      return;
    case 3: if (ui_) ui_->setOutputVolume(e.extra); return;
    default: return;
  }
}

// Every loaded event whose tick has come (the engine tick is the next block's start), in file order. A chunk boundary inside
// the due events needs another read; when that open fails the rest waits for the next step, which reads BEFORE rendering.
bool Recorder::deliverDue(uint32_t tick, uint32_t ms) {
  while (replayed_ < total_ && rendering_) {
    if (replayed_ >= chunkStart_ + chunkN_ && !readChunk()) return false;
    const RecEvent& e = buf_[replayed_ - chunkStart_];
    if ((int32_t)(tick - (base_ + e.tick)) < 0) break;
    deliver(e, ms); replayed_++;
  }
  return true;
}

bool Recorder::renderStep(int16_t* block) {
  if (!rendering_ || !block || !eng_ || !wav_) return false;
  const EngineStatus& st = eng_->status();
  // The chunk the next events live in, before this block renders (see the note at the top): a failed open stalls the step.
  if (replayed_ < total_ && replayed_ >= chunkStart_ + chunkN_) {
    if (!readChunk()) {
      if (++openFails_ >= kOpenRetrySteps) { finishRender(false); return false; }
      if (gen_) { gen_->pumpVocal(); gen_->step(); }   // the streams keep their lead and a pending switch / Sets write frees its handle
      if (sets_) sets_->step();
      return true;
    }
    openFails_ = 0;
    if (!deliverDue(st.tick, renderNowMs())) return true;   // the events a previous step could not load, due at this block's start
    if (!rendering_) return false;
  }
  // The tick this block starts at: the engine's, or t0_ when the reset burst (CmdMasterPlay 1 → t0_) is still queued for this block.
  uint32_t startTick = st.tick;
  if (first_) { first_ = 0; if ((int32_t)(st.tick - t0_) < 0) startTick = t0_; }
  eng_->render(block, kBlock);
  const bool live = !st.running || (int32_t)(startTick - base_) >= 0;   // inside the recorded span (a stopped transport never reaches it by ticks)
  if (live) {
    if (wav_->write(block, kBlockBytes) != kBlockBytes) { finishRender(false); return false; }   // card full / error: no partial WAV is left
    written_ += kBlockBytes;
    if (tail_) tailLeft_ = tailLeft_ > kBlock ? tailLeft_ - kBlock : 0;
  }
  if (gen_) { gen_->pumpVocal(); gen_->step(); }      // loads and generates progress between blocks, as live
  if (sets_) sets_->step();
  const uint32_t ms = renderNowMs();
  if (ui_) ui_->tick(ms);
  if (!rendering_) return false;                      // the Ui cancelled from inside its tick (the ` hold): nothing more to do
  const uint32_t tick = st.tick;
  deliverDue(tick, ms);                               // a failed chunk read here is retried at the top of the next step
  repeats(ms);
  if (!rendering_) return false;
  // Done once the last event is replayed and the recorded length is reached (or a replayed master stop froze the tick),
  // plus one bar of tail at the tempo in force (echo / reverb decay).
  if (!tail_ && replayed_ >= total_ && ((int32_t)(tick - (base_ + lengthTicks_)) >= 0 || !st.running)) {
    tail_ = 1; tailLeft_ = (uint32_t)(((uint64_t)kTicksPerBar * eng_->samplesPerTickFp()) >> 16);
  }
  if (tail_ && tailLeft_ == 0 && live) { finishRender(true); return false; }
  return true;
}

void Recorder::finishRender(bool keepWav) {
  if (wav_) {
    if (keepWav) { uint8_t wh[kWavHeaderBytes]; wavHeader(wh, written_); if (wav_->seek(0)) wav_->write(wh, kWavHeaderBytes); }
    wav_->close(); wav_ = nullptr;
  }
  if (cur_ >= 0) {
    if (keepWav) wavMask_ |= 1ull << cur_;
    else { char p[32]; wavPath(p, sizeof p, cur_); if (fs_) fs_->remove(p); wavMask_ &= ~(1ull << cur_); }
  }
  if (eng_) { Cmd c; c.type = CmdMasterPlay; c.a = 0; eng_->cmds().push(c); }   // the replayed performance does not go on playing live; BtnA resumes it
  for (Held& h : held_) h.used = 0;
  rendering_ = false; replaying_ = false; cur_ = -1; tail_ = 0; first_ = 0; openFails_ = 0; seed_ = 0;
}

void Recorder::cancelRender() { if (rendering_) finishRender(false); }

} // namespace lp
