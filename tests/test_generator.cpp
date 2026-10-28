// Host tests for gen/generator.cpp: the set/pattern/sound/loop state machine (SPEC §6, §9.2, §9.5, §9.19), the vocal
// ring producer (SPEC §9.3) and the loop streams (SPEC §9.19) against the real engine, library, SMF parser, variants and loaders over
// sdcard/lp through PosixFs (override the card root with LP_SDCARD). The harness loop mimics the device loop:
// gen.step(), gen.pumpVocal(), then one 256-sample engine block (~86 steps per second of audio). The card sits
// behind a counting wrapper so the SD budget (SPEC §9.2) is asserted in bytes read per loop iteration, not in
// host wall time (page-cached POSIX reads cannot fail a 20 ms bound).
#include "check.h"
#include "gen/generator.h"
#include "gen/library.h"
#include "gen/variants.h"
#include "gen/theory_tables.hpp"
#include "engine/engine.h"
#include "dsp/ulaw.h"
#include "dsp/presets.h"
#include "io/wav.h"
#include "../firmware/desktop/posix_fs.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace lp;

// ---------------------------------------------------------------------------------------------- harness

static std::string sdRoot() {
  const char* env = getenv("LP_SDCARD");
  if (env && *env) return env;
  struct stat st;
  if (stat("sdcard/lp", &st) == 0) return "sdcard";
  return "/Users/miha/Documents/cardputer/liveplayer/sdcard";
}

// IFileSystem over PosixFs that counts bytes read, opens and directory entries walked (the SD budget meter).
class CountingFile : public IFile {
 public:
  IFile* f = nullptr; uint64_t* bytes = nullptr; uint64_t* reads = nullptr;
  bool valid() const override { return f && f->valid(); }
  size_t read(void* dst, size_t n) override { size_t got = f ? f->read(dst, n) : 0; *bytes += got; if (reads) (*reads)++; return got; }
  size_t write(const void* src, size_t n) override { return f ? f->write(src, n) : 0; }
  bool seek(uint32_t pos) override { return f && f->seek(pos); }
  uint32_t position() const override { return f ? f->position() : 0; }
  uint32_t size() const override { return f ? f->size() : 0; }
  void close() override { if (f) f->close(); f = nullptr; }
};
class CountingFs : public IFileSystem {
 public:
  explicit CountingFs(const char* root) : inner_(root) {}
  uint64_t bytes = 0, opens = 0, walks = 0, walked = 0, reads = 0, failedOpens = 0, failedWavOpens = 0;   // failedWavOpens: stream / kit files (the library's probe of an absent folder's _index.txt fails by design)
  int maxOpen = 0;                                           // most handles in use at once (SPEC §9.19g: never beyond kMaxOpenFiles)
  bool exists(const char* path) override { return inner_.exists(path); }
  bool isDir(const char* path) override { return inner_.isDir(path); }
  IFile* open(const char* path, const char* mode = "r") override {
    opens++;
    IFile* f = inner_.open(path, mode);
    if (!f) { failedOpens++; if (hasExt(path, ".wav")) failedWavOpens++; return nullptr; }
    if (inner_.openCount() > maxOpen) maxOpen = inner_.openCount();
    for (auto& w : pool_) if (!w.f) { w.f = f; w.bytes = &bytes; w.reads = &reads; return &w; }
    f->close(); return nullptr;
  }
  bool mkdir(const char* path) override { return inner_.mkdir(path); }
  bool remove(const char* path) override { return inner_.remove(path); }
  bool walk(const char* path, DirEntryFn fn, void* user) override {
    walks++; Thunk t = {fn, user, &walked};
    return inner_.walk(path, thunk, &t);
  }
  int openCount() const override { return inner_.openCount(); }   // the generator's handle budget asks (SPEC §9.19g)
 private:
  struct Thunk { DirEntryFn fn; void* user; uint64_t* walked; };
  static bool thunk(const char* name, bool isDir, uint32_t size, void* u) { Thunk& t = *static_cast<Thunk*>(u); (*t.walked)++; return t.fn(name, isDir, size, t.user); }
  PosixFs inner_; CountingFile pool_[PosixFs::kMaxOpen];
};

static CountingFs& sd() { static CountingFs fs(sdRoot().c_str()); return fs; }
static int16_t g_loopMem[kLoopSlots][kLoopRingBytes / 2];    // the loop slots' ring memory (the sketch's bootBlock, SPEC §9.19)
static Engine& eng() {
  static Engine* e = nullptr;
  if (!e) { e = new Engine(); ulawInit(); e->init(); for (int k = 0; k < kLoopSlots; k++) e->setLoopRing(k, g_loopMem[k], kLoopRingBytes / 2); }
  return *e;
}
static Library& lib() { static Library l; static bool init = false; if (!init) { l.init(&sd()); init = true; } return l; }
static Generator& gen() {
  static Generator* g = nullptr;                              // ~9 KB: static, like on the device
  if (!g) { g = new Generator(); g->init(&sd(), &eng(), &lib()); }
  return *g;
}

static double g_maxStepMs = 0;
static uint64_t g_maxIterBytes = 0, g_maxIterWalked = 0, g_maxIterOpens = 0;
static int g_maxPumpChunks = 0;
static int g_steps = 0;
static int16_t g_out[kBlock];
static float g_peak = 0;
static void (*g_afterPump)() = nullptr;                      // probe run after the pump, before the block renders
static bool g_fillQueue = false;                             // probe: the command queue is kept full before every step

// One loop iteration: step, pump, one engine block (the device loop order). The card meter covers step + pump.
static void iterate() {
  if (g_fillQueue) { Cmd c; while (eng().cmds().push(c)) {} }
  const uint64_t b0 = sd().bytes, w0 = sd().walked, o0 = sd().opens;
  auto t0 = std::chrono::steady_clock::now();
  gen().step();
  const int pumped = gen().lastPumpChunks();
  auto t1 = std::chrono::steady_clock::now();
  gen().pumpVocal();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count(); if (ms > g_maxStepMs) g_maxStepMs = ms;
  const uint64_t db = sd().bytes - b0, dw = sd().walked - w0, dop = sd().opens - o0;
  if (db > g_maxIterBytes) g_maxIterBytes = db;
  if (db > 12 * 1024 && getenv("LP_DEBUG_ITER")) std::printf("  [iter %d] %llu B, %llu opens, %llu walked, pump %d, state %d, busy %d, progress %d/%d\n", g_steps, (unsigned long long)db, (unsigned long long)dop, (unsigned long long)dw, pumped, (int)gen().stateCode(), (int)gen().busy(), gen().progress(), gen().total());
  if (dw > g_maxIterWalked) g_maxIterWalked = dw;
  if (dop > g_maxIterOpens) g_maxIterOpens = dop;
  if (pumped > g_maxPumpChunks) g_maxPumpChunks = pumped;
  g_steps++;
  if (g_afterPump) g_afterPump();
  eng().render(g_out, kBlock);
  for (int i = 0; i < kBlock; i++) { float a = g_out[i] < 0 ? -(float)g_out[i] : (float)g_out[i]; if (a > g_peak) g_peak = a; }
}
static int runUntilIdle(int maxSteps = 86 * 30) {
  int n = 0;
  while (gen().busy() && n < maxSteps) { iterate(); n++; }
  return n;
}
static void runBlocks(int blocks) { for (int i = 0; i < blocks; i++) iterate(); }
static void drainEvents() { Ev ev; while (eng().events().pop(ev)) {} }
static bool runUntilBar(int maxBlocks) {                     // true when a bar line passed within maxBlocks
  drainEvents();                                             // stale bar events would answer at once
  for (int i = 0; i < maxBlocks; i++) { iterate(); Ev ev; bool bar = false; while (eng().events().pop(ev)) if (ev.type == EvBar) bar = true; if (bar) return true; }
  return false;
}
static void push(CmdType t, int track = 0, int a = 0, int32_t i = 0, float f = 0) { Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f; eng().cmds().push(c); }

// Pops every queued command into `out` and pushes it back unchanged (peek at what the generator sent).
static std::vector<Cmd> peekCmds() {
  std::vector<Cmd> out; Cmd c;
  while (eng().cmds().pop(c)) out.push_back(c);
  for (const Cmd& k : out) eng().cmds().push(k);
  return out;
}

static int32_t vocalAhead(int set) {                          // samples written beyond the engine clock
  VocalRing& r = eng().vocalRing(set);
  return (int32_t)(r.baseSample.load() + r.w.load() - eng().status().sampleClock);
}
static int pendingSwaps(int set) { int n = 0; for (int i = 0; i < kTracksPerSet; i++) n += eng().status().track[set * kTracksPerSet + i].pendingSwap ? 1 : 0; return n; }

// The whole phrase file as int16 mono (reference for the producer).
static std::vector<int16_t> readPhrase(const char* path, WavInfo& wi) {
  std::vector<int16_t> v;
  IFile* f = sd().open(path);
  if (!f) return v;
  if (wavReadHeader(*f, wi)) {
    uint32_t frames = wi.dataBytes / (wi.channels * 2u);
    v.resize(frames);
    uint32_t got = 0;
    while (got < frames) { uint32_t n = wavReadMono(*f, wi, v.data() + got, frames - got); if (!n) break; got += n; }
    v.resize(got);
  }
  f->close();
  return v;
}
static std::vector<int16_t> currentPhrase(int set, WavInfo& wi) {
  const int t = set * kTracksPerSet + Vocal;
  return readPhrase(gen().vocalPath(set, eng().nextVocal(set).present ? (gen().info(t).variant == 1 ? 1 : 0) : 0), wi);
}
static uint32_t loopSamplesOf(uint16_t loopTicks, int bpm) { const uint64_t den = (uint64_t)bpm * kPPQN; return (uint32_t)(((uint64_t)loopTicks * 60 * kSampleRate + den / 2) / den); }
static int16_t lerpRef(const std::vector<int16_t>& file, uint32_t i, uint32_t fr) {
  int16_t a = i < file.size() ? file[i] : 0, b = i + 1 < file.size() ? file[i + 1] : 0;
  return (int16_t)(a + (((int32_t)(b - a) * (int32_t)(fr >> 4)) >> 12));
}
static uint32_t stepFpRef(int bpm, float fileBpm) {           // file samples per output sample (16.16): rate = bpm / fileBpm clamped 0.5..2 (the card's files are 22050 Hz)
  float rate = (float)bpm / fileBpm; if (rate < 0.5f) rate = 0.5f; if (rate > 2.0f) rate = 2.0f;
  return (uint32_t)(rate * 65536.0f + 0.5f);
}
// The engine's bar clock as the producer reads it (SPEC §9.16): the current bar's exact first sample and index, the
// next bar line (exact while the tempo holds) and the nominal bar length at the tempo in force.
static Generator::BarClock clockNow() {
  const EngineStatus& st = eng().status(); Generator::BarClock c;
  c.bpm = st.bpm ? st.bpm : (uint16_t)kDefaultBpm; c.barLen = loopSamplesOf(kTicksPerBar, c.bpm); c.idx0 = st.tick / kTicksPerBar; c.bar0 = st.barSample; c.bar1 = st.nextBarSample;
  if ((int32_t)(c.bar1 - c.bar0) <= 0) c.bar1 = c.bar0 + c.barLen;
  return c;
}
static uint32_t cycleBars0() { const uint16_t lt = gen().vocalLoopTicks(0); return gen().info(Vocal).variant < 2 && lt >= kTicksPerBar ? lt / kTicksPerBar : 1; }
// Iterates until a bar line whose global index ≡ phase (mod C) passes; its exact sample is then status.barSample.
static bool runUntilBarPhase(uint32_t C, uint32_t phase, int maxBlocks) {
  drainEvents();
  for (int i = 0; i < maxBlocks; i++) { iterate(); Ev ev; bool hit = false; while (eng().events().pop(ev)) if (ev.type == EvBar && (ev.tick / kTicksPerBar) % C == phase) hit = true; if (hit) return true; }
  return false;
}

// Reference producer: every sample is placed by the bar clock (SPEC §9.16). Window variants 0 / 1: in cycle bar
// (bar index mod cycleBars) 0 / 1 the phrase at file position (cycleBar × bar + samples into the bar) × rate (plus the
// continuity offset of cycle offCycle after a tempo change), zeros once the file ends, faded out over the last 64
// samples before the bar line that ends the window as out × left / 64, then silence until the cycle's bar; every cycle
// restarts at the file's first sample. Loop variants 2 / 3 (SPEC §9.3): the loop repeats inside the bar (pos = into mod
// loop) and at a wrap after the first pass from `start` the 64-sample crossfade tail + (head − tail) × pos / 64 in the
// generator's integer arithmetic (head = the file's first samples) — only while the file holds a full crossfade of content
// past the loop end (a phrase shorter than that plays its head straight, like a whole loop file). xfade reports the fade /
// crossfade samples. windowBars is the stream's window (SPEC §9.16 amendment: a '<n>bars' file-name token widens the default 2 bars).
static bool tailContent(size_t frames, uint32_t L, uint32_t stepFp) { return ((((uint64_t)L + Generator::kXfade - 1) * stepFp) >> 16) < frames; }
static int16_t reference(const std::vector<int16_t>& file, const Generator::BarClock& c, uint32_t start, uint16_t loopTicks, int variant, float fileBpm, uint32_t S,
                         int64_t offFp = 0, uint32_t offCycle = 0, bool* xfade = nullptr, int windowBars = kVocalWindowBars) {
  const uint32_t stepFp = stepFpRef(c.bpm, fileBpm);
  uint32_t idx, into, barEnd; Generator::barPhase(c, S, idx, into, barEnd);
  if ((variant & 3) < 2) {
    const uint32_t C = loopTicks >= kTicksPerBar ? loopTicks / kTicksPerBar : 1, cb = idx % C;
    const uint32_t W = windowBars > 0 ? (uint32_t)windowBars : (uint32_t)kVocalWindowBars;
    const bool fade = cb == W - 1 && barEnd - S <= (uint32_t)Generator::kXfade;
    if (xfade) *xfade = fade;
    if (cb >= W) return 0;
    int64_t fp = (int64_t)(cb * c.barLen + into) * stepFp; if (idx / C == offCycle) fp += offFp;
    if (fp < 0) return 0;
    int32_t out = lerpRef(file, (uint32_t)(fp >> 16), (uint32_t)(fp & 0xFFFF));
    if (fade) out = out * (int32_t)(barEnd - S) / Generator::kXfade;
    return (int16_t)out;
  }
  const uint32_t L = loopSamplesOf(loopTicks, c.bpm), pos = into % L;
  const uint64_t srcFp = (uint64_t)pos * stepFp; const uint32_t i = (uint32_t)(srcFp >> 16), fr = (uint32_t)(srcFp & 0xFFFF);
  const int32_t rel = (int32_t)(S - start);
  const bool wrap = tailContent(file.size(), L, stepFp) && rel >= (int32_t)L && pos < (uint32_t)Generator::kXfade;
  if (xfade) *xfade = wrap;
  if (!wrap) return lerpRef(file, i, fr);
  const uint64_t tFp = (uint64_t)(L + pos) * stepFp;
  const int32_t tail = lerpRef(file, (uint32_t)(tFp >> 16), (uint32_t)(tFp & 0xFFFF));
  int32_t head = 0;
  if (i + 1 < (uint32_t)Generator::kHeadSamples) head = lerpRef(file, i, fr);
  else if (i < (uint32_t)Generator::kHeadSamples) head = i < file.size() ? file[i] : 0;
  return (int16_t)(tail + ((head - tail) * (int32_t)pos) / Generator::kXfade);
}

// Compares the ring content for `n` samples from absolute sample `from` with the reference under the engine's clock
// now (the loop variants' first pass counted from `start`); the producer's continuity offset is applied.
static int compareRingFrom(int set, uint32_t from, int n, uint32_t start, int* compared, int* xfaded = nullptr) {
  const int t = set * kTracksPerSet + Vocal;
  WavInfo wi; std::vector<int16_t> file = currentPhrase(set, wi);
  if (file.empty()) return -1;
  VocalRing& r = eng().vocalRing(set);
  const uint32_t base = r.baseSample.load(), w = r.w.load();
  const uint16_t loop = gen().vocalLoopTicks(set); const int variant = gen().info(t).variant; const float fbpm = gen().info(t).fileBpm;
  const int window = gen().info(t).windowBars;                // the stream's window: a '<n>bars' pick widens it (SPEC §9.16 amendment)
  const Generator::BarClock c = clockNow();
  uint32_t offCycle = 0; const int64_t offFp = gen().vocalFileOffset(set, &offCycle);
  int bad = 0; *compared = 0; if (xfaded) *xfaded = 0;
  for (int k = 0; k < n; k++) {
    uint32_t S = from + (uint32_t)k;
    if ((int32_t)(S - base) < 0 || (int32_t)(S - (base + w)) >= 0) break;
    bool xf = false;
    const int16_t want = reference(file, c, start, loop, variant, fbpm, S, offFp, offCycle, &xf, window);
    (*compared)++; if (xf && xfaded) (*xfaded)++;
    if (r.buf[(S - base) & (uint32_t)(r.cap - 1)] != want) bad++;
  }
  return bad;
}
// Compares the next `n` samples after the engine clock with the reference for the producer's own start.
static int compareRing(int set, int n, int* compared) { return compareRingFrom(set, eng().status().sampleClock, n, gen().vocalStartSample(set), compared); }
// The next absolute sample at `offset` inside the vocal cycle of set 0 that lies at least `margin` samples ahead of
// the clock: cycle starts are the bar lines whose global index is a multiple of the cycle's bars (SPEC §9.16), the
// offset counted at the nominal bar length (exact at 120 bpm).
static uint32_t nextCycleSample(uint32_t offset, int margin) {
  const Generator::BarClock c = clockNow(); const uint32_t C = cycleBars0();
  const uint32_t from = eng().status().sampleClock + (uint32_t)margin;
  uint32_t S0 = c.bar0 - (c.idx0 % C) * c.barLen + offset;
  while ((int32_t)(S0 - from) < 0) S0 += C * c.barLen;
  return S0;
}
// Iterates until the clock is within `lead` samples of S0 (false when it did not get there in `maxBlocks`).
static bool approach(uint32_t S0, int lead, int maxBlocks) {
  int guard = 0;
  while ((int32_t)(S0 - eng().status().sampleClock) > lead && guard++ < maxBlocks) iterate();
  return (int32_t)(S0 - eng().status().sampleClock) <= lead && (int32_t)(S0 - eng().status().sampleClock) > 0;
}

struct NameFn { std::vector<std::string> names; };
static bool collect(const char* name, bool isDir, uint32_t, void* u) { if (!isDir) static_cast<NameFn*>(u)->names.push_back(name); return true; }
static bool collectDirs(const char* name, bool isDir, uint32_t, void* u) { if (isDir) static_cast<NameFn*>(u)->names.push_back(name); return true; }

// A PCM WAV of zeros at the card path (for the stream validation tests).
// signal: 16-bit mono files get a deterministic ramp per sample instead of silence (so a ring comparison means something).
static bool writeWav(const char* path, int channels, int bits, uint32_t rate, uint32_t frames, bool signal = false) {
  IFile* f = sd().open(path, "w");
  if (!f) return false;
  const uint32_t blockAlign = (uint32_t)channels * (uint32_t)bits / 8, dataBytes = frames * blockAlign;
  std::vector<uint8_t> h;
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) h.push_back((uint8_t)(v >> (8 * i))); };
  auto u16 = [&](uint32_t v) { for (int i = 0; i < 2; i++) h.push_back((uint8_t)(v >> (8 * i))); };
  auto tag = [&](const char* t) { for (int i = 0; i < 4; i++) h.push_back((uint8_t)t[i]); };
  tag("RIFF"); u32(36 + dataBytes); tag("WAVE"); tag("fmt "); u32(16); u16(1); u16((uint32_t)channels); u32(rate); u32(rate * blockAlign); u16(blockAlign); u16((uint32_t)bits); tag("data"); u32(dataBytes);
  bool ok = f->write(h.data(), h.size()) == h.size();
  std::vector<uint8_t> body(dataBytes, 0);
  if (signal && channels == 1 && bits == 16) for (uint32_t i = 0; i < frames; i++) { const int16_t v = (int16_t)(((i * 37u) & 0x7FFFu) - 16384u); body[2 * i] = (uint8_t)v; body[2 * i + 1] = (uint8_t)((uint16_t)v >> 8); }
  ok = ok && f->write(body.data(), body.size()) == body.size();
  f->close();
  return ok;
}

// ---------------------------------------------------------------------------------------------- pure helpers

TEST(bpm_token_scanner_on_real_house_names) {
  NameFn dirs; sd().walk("/lp/packs/House/vocal", collectDirs, &dirs);
  int checked = 0;
  for (const std::string& d : dirs.names) {
    NameFn files; sd().walk(("/lp/packs/House/vocal/" + d).c_str(), collect, &files);
    NameFn shards; sd().walk(("/lp/packs/House/vocal/" + d).c_str(), collectDirs, &shards);   // a key folder may be sharded (sNN, SPEC §9.2)
    for (const std::string& sh : shards.names) sd().walk(("/lp/packs/House/vocal/" + d + "/" + sh).c_str(), collect, &files);
    for (const std::string& n : files.names) {
      // Convention of the import: "<Key>__<n>bpm__…" — the expected value is the number right before "bpm".
      size_t p = n.find("bpm"); if (p == std::string::npos) continue;
      size_t j = p; while (j > 0 && isdigit((unsigned char)n[j - 1])) j--;
      int expect = atoi(n.substr(j, p - j).c_str());
      CHECK_EQ(Generator::parseBpmToken(n.c_str()), expect);
      checked++;
    }
  }
  std::printf("  %d House vocal names carry a parsable BPM token\n", checked);
  CHECK(checked >= 200);
  NameFn tp; sd().walk("/lp/packs/Testpack/vocal/NA", collect, &tp);
  for (const std::string& n : tp.names) CHECK_EQ(Generator::parseBpmToken(n.c_str()), 0);
  CHECK(tp.names.size() >= 3);
}

TEST(bpm_token_scanner_forms_and_limits) {
  CHECK_EQ(Generator::parseBpmToken("Amin__124bpm__x.wav"), 124);
  CHECK_EQ(Generator::parseBpmToken("loop_124_bpm.wav"), 124);
  CHECK_EQ(Generator::parseBpmToken("Vocal 124 BPM dry.wav"), 124);
  CHECK_EQ(Generator::parseBpmToken("vox_bpm_128.wav"), 128);
  CHECK_EQ(Generator::parseBpmToken("vox_bpm128.wav"), 128);
  CHECK_EQ(Generator::parseBpmToken("BPM-90_vox.wav"), 90);
  CHECK_EQ(Generator::parseBpmToken("124 - bpm.wav"), 124);
  CHECK_EQ(Generator::parseBpmToken("59bpm.wav"), 0);           // below 60
  CHECK_EQ(Generator::parseBpmToken("250bpm.wav"), 0);          // above 249
  CHECK_EQ(Generator::parseBpmToken("1240bpm.wav"), 0);         // four digits
  CHECK_EQ(Generator::parseBpmToken("bpm1240.wav"), 0);
  CHECK_EQ(Generator::parseBpmToken("x124bpmy.wav"), 0);        // glued letter after bpm
  CHECK_EQ(Generator::parseBpmToken("alphabet_d.wav"), 0);
  CHECK_EQ(Generator::parseBpmToken("bpm.wav"), 0);
  CHECK_EQ(Generator::parseBpmToken(""), 0);
  CHECK_EQ(Generator::parseBpmToken(nullptr), 0);
  CHECK_EQ(Generator::parseBpmToken("70bpm_and_bpm_200"), 70);  // first valid token wins
}

TEST(natural_beats_largest_power_of_two_inside) {
  // frames at 22050 Hz for a given duration × bpm → beats; largest power of two that fits, 2 % tolerance (SPEC §9.3, REAPER rule)
  auto beats = [](float b, float bpm) { return Generator::naturalBeats((uint32_t)(b * 60.0f / bpm * 22050.0f + 0.5f), 22050, bpm); };
  CHECK_EQ(beats(7.54f, 110), 4);
  CHECK_EQ(beats(7.9f, 110), 8);                               // within the 2 % tolerance below 8
  CHECK_EQ(beats(1.57f, 115), 1);
  CHECK_EQ(beats(11.43f, 110), 8);
  CHECK_EQ(beats(16.5f, 110), 16);
  CHECK_EQ(beats(0.5f, 120), 1);
  CHECK_EQ(beats(100.0f, 120), 16);
  CHECK_EQ(beats(2.83f, 128), 2);
  CHECK_EQ(beats(4.0f, 128), 4);
  CHECK_EQ(Generator::naturalBeats(0, 22050, 120), 1);
  CHECK_EQ(Generator::naturalBeats(22050, 0, 120), 1);
  CHECK_EQ(Generator::naturalBeats(44100, 44100, 120), 2);      // other sample rates count by duration
}

// SPEC §9.16: the clip of variants 1 / 2 is the (2 + pause)-bar cycle, 3 / 4 loop 2 / 1 beats; the pause steps 0 1 2 4.
TEST(vocal_clip_cycle_and_pause_steps) {
  CHECK_EQ((int)Generator::vocalClipTicks(0, 1), 3 * kTicksPerBar);
  CHECK_EQ((int)Generator::vocalClipTicks(1, 0), 2 * kTicksPerBar);
  CHECK_EQ((int)Generator::vocalClipTicks(0, 2), 4 * kTicksPerBar);
  CHECK_EQ((int)Generator::vocalClipTicks(1, 4), 6 * kTicksPerBar);
  CHECK_EQ((int)Generator::vocalClipTicks(0, 3), 6 * kTicksPerBar);   // 3 is not a step: 4
  CHECK_EQ((int)Generator::vocalClipTicks(2, 4), 2 * kTicksPerBeat);  // loops: no pause
  CHECK_EQ((int)Generator::vocalClipTicks(3, 4), kTicksPerBeat);
  CHECK_EQ((int)Generator::vocalClipTicks(4, 1), 3 * kTicksPerBar);   // variant & 3
  CHECK_EQ((int)kVocalWindowTicks, 768); CHECK_EQ((int)kVocalPauseDefault, 1); CHECK_EQ((int)kMaxVocalClipTicks, (16 + kVocalPauseMaxBars) * kTicksPerBar);   // 7680 (SPEC §9.18.1)
  // SPEC §9.16 amendment: a '<n>bars' window. A window the engine's clip cap (kVocalWindowMaxBars, 20 today) cannot hold is
  // never cut: it falls back to the default 2 bars; the pause is kept inside the cap by fitPause / stepPause, and the cap
  // in vocalClipTicks is the last defence for a stored pause that no longer fits. Written against the cap so a raised
  // kMaxVocalClipTicks (config.h) keeps the test true.
  const int maxW = Generator::kVocalWindowMaxBars;
  CHECK_EQ(maxW, (int)kMaxVocalClipTicks / kTicksPerBar); CHECK(maxW >= 6);
  CHECK_EQ((int)Generator::vocalClipTicks(0, 1, 4), 5 * kTicksPerBar);
  CHECK_EQ((int)Generator::vocalClipTicks(0, 0, 1), 1 * kTicksPerBar);
  CHECK_EQ((int)Generator::vocalClipTicks(0, 4, 4), 8 <= maxW ? 8 * kTicksPerBar : (int)kMaxVocalClipTicks);   // a stored pause 4 under a 4-bar window: capped (the pause gives way)
  CHECK_EQ((int)Generator::vocalClipTicks(0, 1, 8), 8 <= maxW ? 9 * kTicksPerBar : 3 * kTicksPerBar);          // an 8-bar window under today's cap: the 2-bar window, whole bars only (never cut mid-verse)
  CHECK_EQ((int)Generator::vocalClipTicks(2, 1, 8), 2 * kTicksPerBeat);               // loop variants: no window
  CHECK_EQ((int)Generator::vocalClipTicks(0, 1, 0), 3 * kTicksPerBar);                // 0 = the default window
  CHECK_EQ((int)Generator::clampWindow(0), kVocalWindowBars); CHECK_EQ((int)Generator::clampWindow(3), 3); CHECK_EQ((int)Generator::clampWindow(maxW), maxW);
  CHECK_EQ((int)Generator::clampWindow(maxW + 1), kVocalWindowBars); CHECK_EQ((int)Generator::clampWindow(99), kVocalWindowBars);   // too long for the cap: the default, not a truncation
  // the pause under a window: a step the cap would swallow is refused (the page never shows a pause that does not play),
  // a step down always works, and a stored pause steps down to the largest that fits
  CHECK_EQ((int)Generator::stepPause(1, +1, 2), 2); CHECK_EQ((int)Generator::stepPause(2, +1, 2), 4);          // 2 + 4 fits every cap ≥ 6
  CHECK_EQ((int)Generator::stepPause(2, +1, maxW - 2), 2); CHECK_EQ((int)Generator::stepPause(1, +1, maxW - 2), 2);   // window maxW − 2: pause 4 would exceed the cap
  CHECK_EQ((int)Generator::stepPause(0, +1, maxW), 0); CHECK_EQ((int)Generator::stepPause(4, -1, maxW), 2); CHECK_EQ((int)Generator::stepPause(1, -1, maxW), 0);
  CHECK_EQ((int)Generator::stepPause(1, 0, maxW), 1);
  CHECK_EQ((int)Generator::fitPause(4, 2), 4); CHECK_EQ((int)Generator::fitPause(4, maxW - 2), 2); CHECK_EQ((int)Generator::fitPause(4, maxW - 3), 2);
  CHECK_EQ((int)Generator::fitPause(2, maxW - 1), 1); CHECK_EQ((int)Generator::fitPause(1, maxW), 0); CHECK_EQ((int)Generator::fitPause(0, maxW), 0); CHECK_EQ((int)Generator::fitPause(3, 2), 4);
  CHECK_EQ(Generator::parseBarsToken("125bpm__RAPM__8bars__Verse_Hook.wav"), 8);
  CHECK_EQ(Generator::parseBarsToken("Amin__120bpm__4_bars__x.wav"), 4);
  CHECK_EQ(Generator::parseBarsToken("rap 16 bars 90bpm.wav"), 16);
  CHECK_EQ(Generator::parseBarsToken("2bar_loop_100bpm.wav"), 2);
  CHECK_EQ(Generator::parseBarsToken("NA__110bpm__GHUAC__Vocal__Adlib_1_Minor_Wet.wav"), 0);
  CHECK_EQ(Generator::parseBarsToken("Fmin__110bpm__Barbara__8barry.wav"), 0);          // letters glued after 'bar' are not a token
  CHECK_EQ(Generator::parseBarsToken("bars__128bpm.wav"), 0);                           // no digits before
  CHECK_EQ(Generator::parseBarsToken(nullptr), 0);
  CHECK_EQ((int)Generator::clampPause(-2), 0); CHECK_EQ((int)Generator::clampPause(0), 0); CHECK_EQ((int)Generator::clampPause(1), 1);
  CHECK_EQ((int)Generator::clampPause(2), 2); CHECK_EQ((int)Generator::clampPause(3), 4); CHECK_EQ((int)Generator::clampPause(9), 4);
  CHECK_EQ((int)Generator::stepPause(0, +1), 1); CHECK_EQ((int)Generator::stepPause(1, +1), 2); CHECK_EQ((int)Generator::stepPause(2, +1), 4);
  CHECK_EQ((int)Generator::stepPause(4, +1), 4); CHECK_EQ((int)Generator::stepPause(4, -1), 2); CHECK_EQ((int)Generator::stepPause(2, -1), 1);
  CHECK_EQ((int)Generator::stepPause(1, -1), 0); CHECK_EQ((int)Generator::stepPause(0, -1), 0); CHECK_EQ((int)Generator::stepPause(3, -1), 2);
  CHECK_EQ((int)Generator::stepPause(1, 0), 1);
}

// ---------------------------------------------------------------------------------------------- generate set

TEST(generate_set_0_loads_patterns_kit_and_vocal) {
  Generator& g = gen(); Engine& e = eng();
  CHECK_EQ(g.genreIndex(), 0); CHECK(strcmp(g.genreName(), "House") == 0);
  CHECK(!g.busy());
  const uint16_t ack0 = e.status().slotAck;
  CHECK(g.generateSet(0));
  CHECK(g.busy());
  CHECK(g.generateSet(0));                                     // still queued: the duplicate folds into it
  iterate();
  CHECK(!g.generateSet(0));                                    // now running: refused
  int steps = runUntilIdle() + 1;
  std::printf("  set 0 generated in %d steps (%.2f s of audio), max step %.2f ms, max %llu B / iteration\n", steps, steps * (double)kBlock / kSampleRate, g_maxStepMs, (unsigned long long)g_maxIterBytes);
  CHECK(!g.busy());
  CHECK_EQ(g.progress(), 8); CHECK_EQ(g.total(), 8);
  if (g.lastError()[0]) std::printf("  lastError: %s\n", g.lastError());
  CHECK(g.lastError()[0] == 0);
  for (int t = 0; t < kTracksPerSet; t++) {
    CHECK(e.status().track[t].state != PSEmpty);
    CHECK(g.info(t).file[0] != 0);
    CHECK(g.info(t).loaded == 1);
    CHECK_EQ((int)g.info(t).keyRoot, (int)g.key().root);
    if (isSynthCategory(trackCategory(t))) CHECK(g.info(t).sound[0] != 0);
  }
  CHECK_EQ((int)(uint16_t)(e.status().slotAck - ack0), 8);
  for (int s = 0; s < kSampleSlots; s++) { CHECK(e.kit(0).name[s][0] != 0); CHECK(e.kit(0).slot[s].present == 1); CHECK(e.kit(0).slot[s].length > 1000); }
  std::printf("  kit: %s | %s | %s | %s | %s | %s | %s | %s\n", e.kit(0).name[0], e.kit(0).name[1], e.kit(0).name[2], e.kit(0).name[3], e.kit(0).name[4], e.kit(0).name[5], e.kit(0).name[6], e.kit(0).name[7]);
  CHECK_EQ((int)e.status().track[Beat].variant, 0);           // a new Beat starts on Kick only (SPEC §9.17b); active stays Full (SPEC §9.6)
  CHECK_EQ((int)g.info(Beat).variant, 0); CHECK_EQ((int)g.info(Percussion).variant, 3); CHECK_EQ((int)g.info(Chord).variant, 0);
  CHECK(e.activePattern(Beat).count > 0); CHECK(e.activePattern(Percussion).count > 0); CHECK(e.activePattern(Chord).count > 0);
  for (int i = 0; i < e.activePattern(Percussion).count; i++) { int p = e.activePattern(Percussion).notes[i].pitch(); CHECK(p >= 36 && p <= 39); }   // folded (SPEC §9.7)
  CHECK(e.rushPattern(0).lengthTicks == kTicksPerBeat);
  for (int i = 0; i < e.rushPattern(0).count; i++) CHECK(e.rushPattern(0).notes[i].pitch() != 36);
  Pattern rush; snareRush(e.activePattern(Beat), rush);        // the rush loop belongs to the Full pattern that was swapped in
  CHECK(memcmp(&rush, &e.rushPattern(0), sizeof(Pattern)) == 0);
  // vocal: a phrase with a BPM token, natural length a power of two, published to the engine
  CHECK(e.nextVocal(0).present == 1);
  CHECK(g.info(Vocal).fileBpm >= 60 && g.info(Vocal).fileBpm <= 249);
  CHECK_EQ((int)e.nextVocal(0).fileBpm, (int)g.info(Vocal).fileBpm);
  int nb = g.info(Vocal).naturalTicks / kTicksPerBeat; CHECK(nb == 1 || nb == 2 || nb == 4 || nb == 8 || nb == 16);
  CHECK_EQ((int)g.info(Vocal).pause, 1); CHECK_EQ((int)g.info(Vocal).lenStep, 0);   // SPEC §9.16: the clip is the 2-bar window + 1-bar pause
  CHECK_EQ((int)e.status().track[Vocal].lenTicks, 3 * kTicksPerBar);
  CHECK_EQ((int)g.vocalLoopTicks(0), 3 * kTicksPerBar);
  CHECK_EQ((int)e.nextVocal(0).loopTicks, 3 * kTicksPerBar);
  CHECK(g.vocalPath(0, 0)[0] != 0 && g.vocalPath(0, 1)[0] != 0);
  CHECK(strcmp(g.vocalPath(0, 0), g.vocalPath(0, 1)) != 0); // alternate file differs (16 phrases in House/NA)
  std::printf("  vocal: %s (%s, natural %d beats), alt %s\n", g.info(Vocal).file, g.info(Vocal).sound, nb, g.vocalPath(0, 1) + strlen("/lp/packs/House/vocal/NA/"));
  CHECK(e.status().generating[0] == 0);
  { int guard = 0; while ((!g.vocalArmed(0) || g.vocalArmBase(0) != e.status().nextBarSample || vocalAhead(0) < 2048) && guard++ < 40) iterate(); }   // a bar passed idle: the pre-roll is rebuilt for the next bar's phase (SPEC §9.16)
  CHECK(vocalAhead(0) >= 2048);                                // pre-rolled at the next bar while idle
  CHECK(g.vocalArmed(0)); CHECK_EQ(g.vocalArmBase(0), e.status().nextBarSample);
}

TEST(vocal_streams_ahead_without_underruns) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  for (int tr = 0; tr < kTracksPerSet; tr++) if (tr != t) push(CmdTrackStopNow, tr);
  push(CmdTrackToggle, t);
  runBlocks(2);
  CHECK_EQ((int)e.status().track[t].state, (int)PSQueued);
  static int minAhead, playingBlocks, minAfterRender;
  minAhead = 1 << 30; minAfterRender = 1 << 30; playingBlocks = 0; g_peak = 0; drainEvents();
  g_afterPump = [] { if (eng().status().track[Vocal].state == PSPlaying) { playingBlocks++; int a = vocalAhead(0); if (a < minAhead) minAhead = a; } };
  const uint16_t under0 = e.status().vocalUnderruns;
  for (int b = 0; b < kSampleRate * 4 / kBlock; b++) {        // 4 s
    iterate();
    if (e.status().track[t].state == PSPlaying) { int a = vocalAhead(0); if (a < minAfterRender) minAfterRender = a; }
  }
  g_afterPump = nullptr;
  std::printf("  played %d blocks, min lead after pump %d samples (after the block %d), peak %.0f, underruns %u\n", playingBlocks, minAhead, minAfterRender, g_peak, (unsigned)(e.status().vocalUnderruns - under0));
  CHECK(playingBlocks > kSampleRate * 2 / kBlock);
  CHECK(minAhead >= 2048);                                     // SPEC §9.2: refilled whenever 2 KB is free
  CHECK(minAfterRender >= 2048 - kBlock);
  CHECK(minAhead <= eng().vocalRing(0).cap);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  CHECK(g_peak > 200);                                         // the phrase is audible
  int compared = 0; int bad = compareRing(0, 1500, &compared);
  std::printf("  ring vs reference: %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  (void)g;
}

TEST(vocal_varispeed_follows_tempo) {
  Engine& e = eng(); const int t = Vocal;
  const uint16_t under0 = e.status().vocalUnderruns;
  push(CmdSetTempo, 0, 0, 150);
  runBlocks(kSampleRate * 2 / kBlock);                         // old-rate content drains, new content follows
  CHECK_EQ((int)e.status().bpm, 150);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  int compared = 0; int bad = compareRing(0, 1500, &compared);
  std::printf("  at 150 bpm (rate %.2f): %d compared, %d mismatches, underruns %u\n", 150.0f / gen().info(t).fileBpm, compared, bad, (unsigned)(e.status().vocalUnderruns - under0));
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdSetTempo, 0, 0, 120);
  runBlocks(kSampleRate / kBlock);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
}

TEST(vocal_variants_loop_and_alternate_file) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  const uint16_t under0 = e.status().vocalUnderruns;
  std::string primary = g.info(t).file;
  // variant 3: first 2 beats looped (SPEC §5): natural 192, switch at a bar line while playing
  g.rebuildVariant(t, 2);
  CHECK_EQ((int)g.info(t).naturalTicks, 2 * kTicksPerBeat);
  CHECK(g.vocalSwitchPending(0));
  runBlocks(kSampleRate * 3 / kBlock);                         // > one bar at 120 bpm
  CHECK(!g.vocalSwitchPending(0));
  CHECK_EQ((int)g.vocalLoopTicks(0), 2 * kTicksPerBeat);
  CHECK_EQ((int)e.status().track[t].lenTicks, 2 * kTicksPerBeat);
  CHECK_EQ((int)e.status().track[t].variant, 2);
  int compared = 0; int bad = compareRing(0, 1500, &compared);
  std::printf("  2-beat loop: %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 800); CHECK_EQ(bad, 0);
  // variant 2: alternate file (SPEC §5), second pick
  g.rebuildVariant(t, 1);
  CHECK(strcmp(g.info(t).file, primary.c_str()) != 0);
  CHECK_EQ((int)g.info(t).variant, 1);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK(!g.vocalSwitchPending(0));
  CHECK_EQ((int)e.status().track[t].variant, 1);
  CHECK_EQ((int)e.nextVocal(0).fileBpm, (int)g.info(t).fileBpm);
  bad = compareRing(0, 1500, &compared);
  std::printf("  alternate %s: %d compared, %d mismatches\n", g.info(t).file, compared, bad);
  CHECK(compared > 800); CHECK_EQ(bad, 0);
  // back to the full primary phrase: the window + pause cycle again (SPEC §9.16), the natural length untouched
  g.rebuildVariant(t, 0);
  CHECK(strcmp(g.info(t).file, primary.c_str()) == 0);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].variant, 0);
  CHECK_EQ((int)e.status().track[t].lenTicks, 3 * kTicksPerBar);
  CHECK_EQ((int)g.vocalLoopTicks(0), 3 * kTicksPerBar);
  CHECK((int)g.info(t).naturalTicks % kTicksPerBeat == 0 && g.info(t).naturalTicks <= kMaxPatternTicks);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  bad = compareRing(0, 1500, &compared);
  std::printf("  back to the cycle: %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 800); CHECK_EQ(bad, 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// SPEC §9.16: with the track playing, the ring around the window end holds the phrase faded out over the last
// 64 samples of 768 ticks worth of output, then silence for the pause, and the next cycle restarts at the file's
// first sample without a crossfade; the first bar-aligned cycle start after the clock is checked too.
TEST(vocal_window_and_pause_match_reference) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  push(CmdTrackToggle, t);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)g.info(t).pause, 1);
  const int bpm = e.status().bpm; CHECK_EQ(bpm, 120);
  const int window = g.info(t).windowBars;                              // 2 unless the pick carried a '<n>bars' token (SPEC §9.16 amendment)
  const uint32_t L = loopSamplesOf(g.vocalLoopTicks(0), bpm), W = loopSamplesOf((uint16_t)(window * kTicksPerBar), bpm);
  CHECK_EQ((int)L, (window + 1) * 44100); CHECK_EQ((int)W, window * 44100);   // (window + 1) bars / window bars at 120 bpm (2 → 132300 / 88200)
  CHECK_EQ((int)L, (int)loopSamplesOf(Generator::vocalClipTicks(0, 1, window), bpm));
  const uint16_t under0 = e.status().vocalUnderruns;
  WavInfo wi; std::vector<int16_t> file = currentPhrase(0, wi);
  CHECK(!file.empty());
  const float fbpm = g.info(t).fileBpm; const float rate = std::min(2.0f, std::max(0.5f, (float)bpm / fbpm));
  const bool longer = (float)file.size() > (float)W * rate;            // does the phrase outlast the window?
  std::printf("  phrase %s: %zu frames at rate %.2f (%s the %d-bar window)\n", g.info(t).file, file.size(), rate, longer ? "outlasts" : "ends inside", window);
  // window end: 128 samples of the window's tail (64 of them fading) and 64 of silence
  uint32_t S0 = nextCycleSample(W, 1500);
  CHECK(approach(S0, 1200, 4000));
  int compared = 0, xf = 0; int bad = compareRingFrom(0, S0 - 128, 192, g.vocalStartSample(0), &compared, &xf);
  std::printf("  window end at %u: %d compared (%d fading), %d mismatches\n", S0, compared, xf, bad);
  CHECK_EQ(compared, 192); CHECK_EQ(xf, Generator::kXfade); CHECK_EQ(bad, 0);
  VocalRing& r = e.vocalRing(0);
  int nonZero = 0, loud = 0;
  for (int k = 0; k < 64; k++) { if (r.buf[(S0 + (uint32_t)k - r.baseSample.load()) & (uint32_t)(r.cap - 1)] != 0) nonZero++; }
  for (int k = 1; k <= 64; k++) { const int16_t v = r.buf[(S0 - 64 - (uint32_t)k - r.baseSample.load()) & (uint32_t)(r.cap - 1)]; if (v > 300 || v < -300) loud++; }
  std::printf("  after the window: %d/64 non-zero; before the fade: %d/64 above 300\n", nonZero, loud);
  CHECK_EQ(nonZero, 0);
  if (longer) CHECK(loud > 0);                                           // a long phrase is cut by the fade, not silent already
  // the middle of the pause is silence
  S0 = nextCycleSample(W + (L - W) / 2, 1500);
  CHECK(approach(S0, 1200, 4000));
  bad = compareRingFrom(0, S0, 1024, g.vocalStartSample(0), &compared, &xf);
  nonZero = 0;
  for (int k = 0; k < 1024; k++) if (r.buf[(S0 + (uint32_t)k - r.baseSample.load()) & (uint32_t)(r.cap - 1)] != 0) nonZero++;
  CHECK_EQ(compared, 1024); CHECK_EQ(bad, 0); CHECK_EQ(nonZero, 0);
  // cycle start: silence, then the file's first samples (no crossfade), on a bar line
  S0 = nextCycleSample(0, 1500);
  CHECK(approach(S0, 1200, 4000));
  { uint32_t idx, into, end; Generator::barPhase(clockNow(), S0, idx, into, end); CHECK_EQ((int)(idx % cycleBars0()), 0); CHECK_EQ((int)into, 0); }   // a bar line with global index ≡ 0 (mod the cycle's bars)
  bad = compareRingFrom(0, S0 - 64, 256, g.vocalStartSample(0), &compared, &xf);
  std::printf("  cycle start at %u: %d compared, %d mismatches\n", S0, compared, bad);
  CHECK_EQ(compared, 256); CHECK_EQ(xf, 0); CHECK_EQ(bad, 0);
  int head = 0;
  for (int k = 0; k < 64; k++) { const int16_t v = r.buf[(S0 + (uint32_t)k - r.baseSample.load()) & (uint32_t)(r.cap - 1)]; if (v == lerpRef(file, (uint32_t)(((uint64_t)k * (uint32_t)(rate * 65536.0f + 0.5f)) >> 16), (uint32_t)(((uint64_t)k * (uint32_t)(rate * 65536.0f + 0.5f)) & 0xFFFF))) head++; }
  CHECK_EQ(head, 64);
  { drainEvents(); int guard = 0; bool hit = false; uint32_t tk = 0;   // the engine crosses that very sample as a bar line of the cycle (phase lock)
    while (!hit && guard++ < 400) { iterate(); Ev ev; while (e.events().pop(ev)) if (ev.type == EvBar) { hit = true; tk = ev.tick; } }
    CHECK(hit); CHECK_EQ(e.status().barSample, S0); CHECK_EQ((int)(tk % (cycleBars0() * kTicksPerBar)), 0); }
  runBlocks(kSampleRate / kBlock);
  bad = compareRing(0, 1500, &compared); CHECK(compared > 1000); CHECK_EQ(bad, 0);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// SPEC §9.16: len± on a vocal steps the pause 0 → 1 → 2 → 4 (clamped at both ends) and switches the stream to the new
// cycle at a bar line; that switch's CmdSwapPattern carries the clip length (no separate CmdSetLength, which would land
// a bar early), so the engine's len and the streamed cycle change on the same bar; the loop variants keep their 2 / 1
// beats and ignore len±; k (lenStep) stays 0.
TEST(vocal_pause_steps_clamp_and_republish) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  push(CmdTrackToggle, t);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  const uint16_t under0 = e.status().vocalUnderruns;
  CHECK_EQ((int)g.setLengthStep(t, 0), 3 * kTicksPerBar);               // query: 2 + 1 bars
  CHECK(peekCmds().empty());
  auto noLen = [&]() { for (const Cmd& k : peekCmds()) CHECK(!(k.type == CmdSetLength && k.track == t)); };   // nothing outside the switch
  auto switched = [&](int want) {                                        // the switch lands the cycle in the producer and the engine alike
    int guard = 0;
    while ((g.vocalSwitchPending(0) || (int)e.status().track[t].lenTicks != want) && guard++ < 1500) iterate();
    CHECK(!g.vocalSwitchPending(0)); CHECK_EQ((int)e.status().track[t].lenTicks, want); CHECK_EQ((int)g.vocalLoopTicks(0), want);
    CHECK_EQ((int)e.nextVocal(0).loopTicks, want);
  };
  CHECK_EQ((int)g.setLengthStep(t, -1), 2 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 0); noLen();
  CHECK(g.vocalSwitchPending(0));
  runBlocks(1);
  CHECK_EQ((int)g.setLengthStep(t, -1), 2 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 0); noLen();   // clamped at 0
  switched(2 * kTicksPerBar);
  int compared = 0; int bad = compareRing(0, 1500, &compared);
  std::printf("  pause 0 (2-bar cycle): %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // without a pause the window end meets the next cycle start: fade-out, then the clean restart, on a bar line ≡ 0 (mod 2)
  uint32_t S0 = nextCycleSample(0, 1500); CHECK(approach(S0, 1200, 4000));
  { uint32_t idx, into, end; Generator::barPhase(clockNow(), S0, idx, into, end); CHECK_EQ((int)(idx % 2), 0); CHECK_EQ((int)into, 0); }
  int xf = 0; bad = compareRingFrom(0, S0 - 96, 192, g.vocalStartSample(0), &compared, &xf);
  std::printf("  pause 0 cycle edge at %u: %d compared (%d fading), %d mismatches\n", S0, compared, xf, bad);
  CHECK_EQ(compared, 192); CHECK_EQ(xf, Generator::kXfade); CHECK_EQ(bad, 0);
  CHECK_EQ((int)g.setLengthStep(t, +1), 3 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 1); noLen();
  runBlocks(1);
  CHECK_EQ((int)g.setLengthStep(t, +1), 4 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 2); noLen();
  runBlocks(1);
  CHECK_EQ((int)g.setLengthStep(t, +1), 6 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 4); noLen();
  runBlocks(1);
  CHECK_EQ((int)g.setLengthStep(t, +1), 6 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 4); noLen();   // clamped at 4
  CHECK_EQ((int)g.info(t).lenStep, 0);
  switched(6 * kTicksPerBar);                                            // beyond kMaxPatternTicks: allowed for a vocal
  CHECK_EQ((int)e.status().track[t].variant, 0);
  bad = compareRing(0, 1500, &compared);
  std::printf("  pause 4 (6-bar cycle): %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // the engine's len and the streamed cycle change on the same bar: len keeps the old cycle while the switch is pending
  // and flips in the block of the bar line on which the producer finalizes its switch
  CHECK_EQ((int)g.setLengthStep(t, -1), 4 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 2);
  { drainEvents(); int guard = 0; bool changed = false;
    while (!changed && guard++ < 1500) {
      CHECK(g.vocalSwitchPending(0)); CHECK_EQ((int)e.status().track[t].lenTicks, 6 * kTicksPerBar);
      iterate(); Ev ev; bool bar = false; while (e.events().pop(ev)) if (ev.type == EvBar) bar = true;
      if ((int)e.status().track[t].lenTicks != 6 * kTicksPerBar) { changed = true; CHECK(bar); CHECK_EQ((int)e.status().track[t].lenTicks, 4 * kTicksPerBar); }
    }
    CHECK(changed);
    iterate();                                                           // the pump after that bar finalizes the switch
    CHECK(!g.vocalSwitchPending(0)); CHECK_EQ((int)g.vocalLoopTicks(0), 4 * kTicksPerBar); }
  bad = compareRing(0, 1500, &compared); CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // a loop variant has no pause: len± does nothing there (no command, no switch); the pause is kept for the window variants
  g.rebuildVariant(t, 2);
  switched(2 * kTicksPerBeat);
  CHECK_EQ((int)g.setLengthStep(t, -1), 2 * kTicksPerBeat); CHECK_EQ((int)g.info(t).pause, 2);
  CHECK_EQ((int)g.setLengthStep(t, +1), 2 * kTicksPerBeat); CHECK_EQ((int)g.info(t).pause, 2);
  CHECK(peekCmds().empty()); CHECK(!g.vocalSwitchPending(0));
  CHECK_EQ((int)g.setLengthStep(t, 0), 2 * kTicksPerBeat);
  g.rebuildVariant(t, 0);
  switched(4 * kTicksPerBar);
  bad = compareRing(0, 1500, &compared); CHECK(compared > 1000); CHECK_EQ(bad, 0);
  CHECK_EQ((int)g.setLengthStep(t, -1), 3 * kTicksPerBar); CHECK_EQ((int)g.info(t).pause, 1);
  switched(3 * kTicksPerBar);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// The 64-sample crossfade at every wrap after the first pass (SPEC §9.3): a 1-beat loop wraps every 11025
// samples at 120 bpm; three consecutive wraps are compared with the integer reference, crossfade included.
TEST(vocal_wrap_crossfade_matches_reference) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  push(CmdTrackToggle, t);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  g.rebuildVariant(t, 3);                                      // variant 4: first beat looped
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)g.vocalLoopTicks(0), kTicksPerBeat);
  const uint32_t L = loopSamplesOf(kTicksPerBeat, e.status().bpm);
  CHECK_EQ((int)L, 11025);
  const uint16_t under0 = e.status().vocalUnderruns;
  int wraps = 0;
  for (int k = 0; k < 3; k++) {
    const Generator::BarClock c = clockNow();
    uint32_t sc = e.status().sampleClock;
    uint32_t S0 = c.bar0 + ((sc - c.bar0) / L + 1) * L;        // the next wrap after the clock: the loop repeats inside the bar (SPEC §9.16 phase lock)
    int guard = 0;
    while ((int32_t)(S0 - e.status().sampleClock) > 1200 && guard++ < 200) iterate();
    sc = e.status().sampleClock;
    CHECK((int32_t)(S0 - 64 - sc) >= 0);
    int compared = 0, xf = 0; int bad = compareRingFrom(0, S0 - 64, 192, g.vocalStartSample(0), &compared, &xf);
    std::printf("  wrap at %u: %d compared (%d in the crossfade), %d mismatches\n", S0, compared, xf, bad);
    CHECK_EQ(compared, 192); CHECK_EQ(xf, Generator::kXfade); CHECK_EQ(bad, 0);
    wraps++;
    runBlocks(8);
  }
  CHECK_EQ(wraps, 3);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  g.rebuildVariant(t, 0);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].variant, 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// Finding 3 (SPEC §9.3): the engine publishes startSample = the exact first sample of the bar (status.nextBarSample
// uses the same ceil rounding as the render loop), so the pre-roll and everything appended after it agree on one
// start. Bars at 120 bpm fall every 44100 samples (≡ 68 mod 256), so a bar that is not block-aligned is near.
TEST(vocal_start_keeps_the_armed_bar_as_content_start) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal; VocalRing& r = e.vocalRing(0);
  runBlocks(kSampleRate * 2 / kBlock);                         // idle: re-armed after the stop
  CHECK_EQ((int)e.status().track[t].state, (int)PSStopped);
  int guard = 0;
  while ((e.status().nextBarSample % kBlock) == 0 && guard++ < 400) iterate();   // a bar inside a block
  while ((g.vocalArmBase(0) != e.status().nextBarSample || r.w.load() < (uint32_t)r.cap) && guard++ < 800) iterate();
  CHECK(g.vocalArmed(0));
  const uint32_t B = e.status().nextBarSample;
  CHECK_EQ(g.vocalArmBase(0), B); CHECK_EQ(r.baseSample.load(), B); CHECK(B % kBlock != 0);
  CHECK((int32_t)(B - e.status().sampleClock) > 4 * kBlock);
  std::vector<int16_t> snap(r.buf, r.buf + r.cap);             // the pre-roll before anything is consumed
  const Generator::BarClock cPre = clockNow();                 // B is its next bar line
  push(CmdTrackToggle, t);
  guard = 0;
  while (e.status().track[t].state != PSPlaying && guard++ < 400) iterate();
  runBlocks(2);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  const uint32_t ss = e.status().track[t].startSample;
  std::printf("  armed bar %u, engine startSample %u (delta %d), content start %u\n", B, ss, (int)(B - ss), g.vocalStartSample(0));
  CHECK((int32_t)(B - ss) >= 0 && (int32_t)(B - ss) < kBlock);
  CHECK_EQ(g.vocalStartSample(0), B);
  CHECK_EQ(ss, B);                                             // exact bar sample from the engine
  WavInfo wi; std::vector<int16_t> file = currentPhrase(0, wi);
  const uint16_t loop = g.vocalLoopTicks(0); const int variant = g.info(t).variant; const int bpm = e.status().bpm; const float fbpm = g.info(t).fileBpm;
  int badPre = 0;
  for (int k = 0; k < r.cap; k++) if (snap[(size_t)k] != reference(file, cPre, B, loop, variant, fbpm, B + (uint32_t)k)) badPre++;
  (void)bpm;
  runBlocks(kSampleRate / kBlock);                             // well past the pre-roll: appended content
  int compared = 0; const int badApp = compareRingFrom(0, e.status().sampleClock, 1500, B, &compared);
  std::printf("  pre-roll vs the bar clock %d/%d bad, appended %d/%d bad\n", badPre, r.cap, badApp, compared);
  CHECK_EQ(badPre, 0);
  CHECK(compared > 1000); CHECK_EQ(badApp, 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// Finding 6, amended by SPEC §9.16: an idle, loaded vocal is pre-rolled for the next bar line with that bar's phase in
// the cycle. For the window variants the bars differ (window bar 0, window bar 1, pause), so each bar passed unheard
// rebuilds the pre-roll — a pause bar costs no card reads — and the content always matches the bar clock; for the
// loop variants every bar is the same, so only the ring's base moves along: no card reads, never inactive.
TEST(idle_vocal_prerolls_each_bar_with_its_phase) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal; VocalRing& r = e.vocalRing(0);
  runBlocks(kSampleRate * 3 / kBlock);                         // the first re-arm after the stop (handshake + refill)
  CHECK_EQ((int)e.status().track[t].state, (int)PSStopped);
  CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)cycleBars0(), 3);
  auto settled = [&]() { int guard = 0; while ((!g.vocalArmed(0) || g.vocalArmBase(0) != clockNow().bar1 || r.w.load() < (uint32_t)r.cap) && guard++ < 60) iterate(); return guard < 60; };
  CHECK(settled());
  WavInfo wi; std::vector<int16_t> file = currentPhrase(0, wi); CHECK(!file.empty());
  const float fbpm = g.info(t).fileBpm;
  for (int bar = 0; bar < 3; bar++) {
    const uint32_t chunks0 = g.pumpChunkTotal();
    CHECK(runUntilBar(kSampleRate * 3 / kBlock));
    CHECK(settled());
    const Generator::BarClock c = clockNow(); const uint32_t phase = (c.idx0 + 1) % 3;
    const int reads = (int)(g.pumpChunkTotal() - chunks0);
    CHECK_EQ(g.vocalArmBase(0), c.bar1); CHECK_EQ(r.baseSample.load(), c.bar1); CHECK_EQ(r.r.load(), 0u); CHECK_EQ(r.w.load(), (uint32_t)r.cap);
    int bad = 0, nonZero = 0;
    for (int k = 0; k < r.cap; k++) {
      const int16_t v = r.buf[(size_t)k];
      if (v != reference(file, c, c.bar1, g.vocalLoopTicks(0), 0, fbpm, c.bar1 + (uint32_t)k)) bad++;
      if (v != 0) nonZero++;
    }
    std::printf("  armed for bar %u (cycle bar %u): %d chunk reads, pre-roll %d/%d bad, %d non-zero\n", c.idx0 + 1, phase, reads, bad, r.cap, nonZero);
    CHECK_EQ(bad, 0);
    if (phase == 2) { CHECK_EQ(reads, 0); CHECK_EQ(nonZero, 0); }        // the pause: silence, no card
    else { CHECK(reads >= 1 && reads <= 4); CHECK(nonZero > 0); }
  }
  // a loop variant: every bar has the same content, so the pre-roll only slides (Finding 6)
  g.rebuildVariant(t, 3);
  runBlocks(kSampleRate / kBlock);                             // idle: switched at once, re-armed
  CHECK(!g.vocalSwitchPending(0)); CHECK_EQ((int)g.vocalLoopTicks(0), kTicksPerBeat);
  CHECK(settled());
  const uint32_t chunks0 = g.pumpChunkTotal(); const uint64_t bytes0 = sd().bytes; const uint32_t base0 = r.baseSample.load();
  static int inactive; inactive = 0;
  g_afterPump = [] { if (!eng().vocalRing(0).active.load()) inactive++; };
  int bars = 0;
  for (int i = 0; i < 4; i++) if (runUntilBar(kSampleRate * 3 / kBlock)) bars++;
  g_afterPump = nullptr;
  runBlocks(2);
  std::printf("  loop variant, %d bars idle: %u chunk reads, %llu bytes, base %u -> %u (next bar %u)\n", bars, (unsigned)(g.pumpChunkTotal() - chunks0), (unsigned long long)(sd().bytes - bytes0), base0, r.baseSample.load(), e.status().nextBarSample);
  CHECK_EQ(bars, 4);
  CHECK_EQ((int)(g.pumpChunkTotal() - chunks0), 0);
  CHECK_EQ(inactive, 0);
  CHECK(r.baseSample.load() != base0);
  CHECK_EQ(r.baseSample.load(), e.status().nextBarSample); CHECK_EQ(g.vocalArmBase(0), e.status().nextBarSample); CHECK_EQ(g.vocalStartSample(0), e.status().nextBarSample);
  CHECK_EQ(r.r.load(), 0u); CHECK_EQ(r.w.load(), (uint32_t)r.cap);
  // a tempo change invalidates the pre-roll: one refill, then sliding again
  push(CmdSetTempo, 0, 0, 100);
  runBlocks(kSampleRate / kBlock);
  CHECK(g.pumpChunkTotal() > chunks0);
  const uint32_t chunks1 = g.pumpChunkTotal();
  CHECK(runUntilBar(kSampleRate * 4 / kBlock)); runBlocks(2);
  CHECK_EQ((int)(g.pumpChunkTotal() - chunks1), 0);
  CHECK_EQ(r.baseSample.load(), e.status().nextBarSample);
  push(CmdSetTempo, 0, 0, 120);
  runBlocks(kSampleRate / kBlock);
  // a queued start takes the pre-roll: no underrun
  const uint16_t under0 = e.status().vocalUnderruns;
  push(CmdTrackToggle, t);
  CHECK(runUntilBar(kSampleRate * 3 / kBlock));
  runBlocks(kSampleRate / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  int compared = 0; CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  push(CmdTrackStopNow, t);
  runBlocks(4);
  g.rebuildVariant(t, 0);                                      // back to the window + pause cycle
  runBlocks(kSampleRate / kBlock);
  CHECK_EQ((int)g.vocalLoopTicks(0), 3 * kTicksPerBar);
}

// SPEC §9.4 / §9.16: the cycle phase is position = globalTick mod len, not "the bar the track was started on". A vocal
// started on a bar whose global index ≡ 1 (mod 3) begins one bar into its phrase (the engine's posTick says so), falls
// silent on the pause bar and restarts on the next bar ≡ 0; one started on a pause bar is silent until that bar.
TEST(vocal_cycle_phase_follows_the_global_bar) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal; VocalRing& r = e.vocalRing(0);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSStopped); CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)cycleBars0(), 3);
  WavInfo wi; std::vector<int16_t> file = currentPhrase(0, wi); CHECK(!file.empty());
  const float fbpm = g.info(t).fileBpm; const uint32_t stepFp = stepFpRef(120, fbpm);
  auto armFor = [&](uint32_t phase) {                          // a next bar of that phase, far enough ahead, and the pre-roll for it
    int guard = 0;
    while (((clockNow().idx0 + 1) % 3 != phase || (int32_t)(clockNow().bar1 - e.status().sampleClock) < 12 * kBlock) && guard++ < 3000) iterate();
    while ((!g.vocalArmed(0) || g.vocalArmBase(0) != clockNow().bar1 || r.w.load() < (uint32_t)r.cap) && guard++ < 3100) iterate();
    return guard < 3100;
  };
  CHECK(armFor(1));
  Generator::BarClock c = clockNow(); const uint32_t B = c.bar1; CHECK_EQ((int)((c.idx0 + 1) % 3), 1);
  std::vector<int16_t> snap(r.buf, r.buf + r.cap);
  int head = 0, bad = 0;
  for (int k = 0; k < 64; k++) { const uint64_t fp = (uint64_t)(c.barLen + (uint32_t)k) * stepFp; if (snap[(size_t)k] == lerpRef(file, (uint32_t)(fp >> 16), (uint32_t)(fp & 0xFFFF))) head++; }
  for (int k = 0; k < r.cap; k++) if (snap[(size_t)k] != reference(file, c, B, g.vocalLoopTicks(0), 0, fbpm, B + (uint32_t)k)) bad++;
  std::printf("  armed for bar %u (cycle bar 1): %d/64 samples are the phrase one bar in, %d/%d off the bar clock\n", c.idx0 + 1, head, bad, r.cap);
  CHECK_EQ(head, 64); CHECK_EQ(bad, 0);
  const uint16_t under0 = e.status().vocalUnderruns;
  push(CmdTrackToggle, t);
  int guard = 0; while (e.status().track[t].state != PSPlaying && guard++ < 400) iterate();
  runBlocks(2);
  CHECK_EQ(e.status().track[t].startSample, B); CHECK_EQ(g.vocalStartSample(0), B);
  const int pos = e.status().track[t].posTick;
  std::printf("  started on bar %u: posTick %d (one bar into the 3-bar cycle)\n", c.idx0 + 1, pos);
  CHECK(pos >= kTicksPerBar && pos < kTicksPerBar + 8);
  int compared = 0; CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  // the pause bar: silence ahead
  CHECK(runUntilBarPhase(3, 2, kSampleRate * 2 / kBlock)); runBlocks(2);
  CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  { int nz = 0; const uint32_t sc = e.status().sampleClock, base = r.baseSample.load(); for (int k = 0; k < 1500; k++) if (r.buf[(sc + (uint32_t)k - base) & (uint32_t)(r.cap - 1)] != 0) nz++; CHECK_EQ(nz, 0); }
  // the restart on the next bar ≡ 0: silence, then the file's first samples; the engine crosses that sample as a bar line with posTick 0
  uint32_t S0 = nextCycleSample(0, 1500); CHECK(approach(S0, 1200, 4000));
  { uint32_t idx, into, end; Generator::barPhase(clockNow(), S0, idx, into, end); CHECK_EQ((int)(idx % 3), 0); CHECK_EQ((int)into, 0); }
  int xf = 0; bad = compareRingFrom(0, S0 - 64, 256, g.vocalStartSample(0), &compared, &xf);
  CHECK_EQ(compared, 256); CHECK_EQ(xf, 0); CHECK_EQ(bad, 0);
  head = 0; { const uint32_t base = r.baseSample.load(); for (int k = 0; k < 64; k++) { const uint64_t fp = (uint64_t)k * stepFp; if (r.buf[(S0 + (uint32_t)k - base) & (uint32_t)(r.cap - 1)] == lerpRef(file, (uint32_t)(fp >> 16), (uint32_t)(fp & 0xFFFF))) head++; } }
  CHECK_EQ(head, 64);
  { drainEvents(); bool hit = false; uint32_t tk = 0; guard = 0; while (!hit && guard++ < 400) { iterate(); Ev ev; while (e.events().pop(ev)) if (ev.type == EvBar) { hit = true; tk = ev.tick; } }
    CHECK(hit); CHECK_EQ(e.status().barSample, S0); CHECK_EQ((int)(tk % (3 * kTicksPerBar)), 0); }
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  // a restart on a pause bar is silent until the cycle's bar
  push(CmdTrackStopNow, t); runBlocks(kSampleRate / kBlock);
  CHECK(armFor(2));
  c = clockNow(); const uint32_t B2 = c.bar1; CHECK_EQ((int)((c.idx0 + 1) % 3), 2);
  { int nz = 0; for (int k = 0; k < r.cap; k++) if (r.buf[(size_t)k] != 0) nz++; CHECK_EQ(nz, 0); }
  push(CmdTrackToggle, t);
  guard = 0; while (e.status().track[t].state != PSPlaying && guard++ < 400) iterate();
  runBlocks(2);
  CHECK_EQ(e.status().track[t].startSample, B2);
  CHECK(e.status().track[t].posTick >= 2 * kTicksPerBar);
  CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  S0 = nextCycleSample(0, 1500); CHECK(approach(S0, 1200, 4000));
  bad = compareRingFrom(0, S0 - 64, 256, g.vocalStartSample(0), &compared, &xf); CHECK_EQ(compared, 256); CHECK_EQ(bad, 0);
  runBlocks(kSampleRate / kBlock);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// SPEC §9.16: a tempo change while the vocal plays moves only the rate. The cycle stays on the engine's bar lines — the
// window end fades into the bar line ≡ 2 and the next restart is the bar line ≡ 0 the engine publishes (EvBar,
// status.barSample, posTick 0) — the ring written ahead is retimed, and the phrase keeps its file position at the
// retime point (no skip), which the reference reproduces through the published continuity offset.
TEST(vocal_tempo_change_keeps_the_cycle_on_the_bars) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal; VocalRing& r = e.vocalRing(0);
  push(CmdTrackToggle, t);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying); CHECK_EQ((int)e.status().bpm, 120); CHECK_EQ((int)cycleBars0(), 3);
  WavInfo wi; std::vector<int16_t> file = currentPhrase(0, wi); CHECK(!file.empty());
  const float fbpm = g.info(t).fileBpm;
  const uint16_t under0 = e.status().vocalUnderruns;
  CHECK(runUntilBarPhase(3, 0, kSampleRate * 4 / kBlock));    // a cycle start
  const uint32_t cycleStart = e.status().barSample; CHECK_EQ((int)((e.status().tick / kTicksPerBar) % 3), 0);
  runBlocks(kSampleRate / kBlock);                             // 1 s into window bar 0 (44100 samples at 120)
  const Generator::BarClock c120 = clockNow(); CHECK_EQ(c120.bar0, cycleStart); CHECK_EQ((int)c120.barLen, 44100);
  push(CmdSetTempo, 0, 0, 150);
  iterate();                                                   // the engine applies the tempo at this block's start
  CHECK_EQ((int)e.status().bpm, 150);
  const Generator::BarClock c150 = clockNow(); CHECK_EQ(c150.bar0, cycleStart); CHECK_EQ((int)c150.barLen, 35280);
  const uint32_t P = e.status().sampleClock + 4 * kBlock;      // the producer's retime point at its next pump
  iterate();                                                   // that pump: the ring ahead is rewritten under the 150 bpm clock
  uint32_t cyc = 0; const int64_t off = g.vocalFileOffset(0, &cyc);
  const int64_t fileOld = (int64_t)(P - cycleStart) * stepFpRef(120, fbpm), fileNew = (int64_t)(P - cycleStart) * stepFpRef(150, fbpm);
  std::printf("  tempo 120 -> 150 at %u (cycle start %u): continuity offset %lld file samples (expected %lld), cycle %u\n", P, cycleStart, (long long)(off >> 16), (long long)((fileOld - fileNew) >> 16), cyc);
  CHECK_EQ((long long)off, (long long)(fileOld - fileNew)); CHECK_EQ(cyc, c150.idx0 / 3);
  runBlocks(8);
  int compared = 0; int bad = compareRing(0, 1500, &compared);
  std::printf("  after the retime: %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // the window end: faded into the bar line ≡ 2 that the engine publishes at 150 bpm
  int guard = 0; while ((clockNow().idx0 + 1) % 3 != 2 && guard++ < 2000) iterate();
  const uint32_t W = clockNow().bar1; CHECK(approach(W, 1200, 4000));
  int xf = 0; bad = compareRingFrom(0, W - 128, 192, g.vocalStartSample(0), &compared, &xf);
  std::printf("  window end at %u: %d compared (%d fading), %d mismatches\n", W, compared, xf, bad);
  CHECK_EQ(compared, 192); CHECK_EQ(xf, Generator::kXfade); CHECK_EQ(bad, 0);
  { const uint32_t base = r.baseSample.load(); int nz = 0; for (int k = 0; k < 64; k++) if (r.buf[(W + (uint32_t)k - base) & (uint32_t)(r.cap - 1)] != 0) nz++; CHECK_EQ(nz, 0); }
  { drainEvents(); bool hit = false; guard = 0; while (!hit && guard++ < 400) { iterate(); Ev ev; while (e.events().pop(ev)) if (ev.type == EvBar) hit = true; } CHECK(hit); CHECK_EQ(e.status().barSample, W); }
  // the restart: the bar line ≡ 0 — the engine's own sample, not 120-bpm arithmetic from the old start
  guard = 0; while ((clockNow().idx0 + 1) % 3 != 0 && guard++ < 2000) iterate();
  const uint32_t S0 = clockNow().bar1; CHECK(approach(S0, 1200, 4000));
  bad = compareRingFrom(0, S0 - 64, 256, g.vocalStartSample(0), &compared, &xf);
  CHECK_EQ(compared, 256); CHECK_EQ(xf, 0); CHECK_EQ(bad, 0);
  { const uint32_t base = r.baseSample.load(), step = stepFpRef(150, fbpm); int head = 0, nz = 0;
    for (int k = 1; k <= 64; k++) if (r.buf[(S0 - (uint32_t)k - base) & (uint32_t)(r.cap - 1)] != 0) nz++;
    for (int k = 0; k < 64; k++) { const uint64_t fp = (uint64_t)k * step; if (r.buf[(S0 + (uint32_t)k - base) & (uint32_t)(r.cap - 1)] == lerpRef(file, (uint32_t)(fp >> 16), (uint32_t)(fp & 0xFFFF))) head++; }
    CHECK_EQ(nz, 0); CHECK_EQ(head, 64); }
  { drainEvents(); bool hit = false; uint32_t tk = 0; guard = 0; while (!hit && guard++ < 400) { iterate(); Ev ev; while (e.events().pop(ev)) if (ev.type == EvBar) { hit = true; tk = ev.tick; } }
    CHECK(hit); CHECK_EQ(e.status().barSample, S0); CHECK_EQ((int)(tk % (3 * kTicksPerBar)), 0);
    const uint32_t stale = cycleStart + 3 * 44100;             // where start-relative 120-bpm cycle arithmetic would have restarted
    std::printf("  restart at %u = the engine's bar line (120-bpm cycle arithmetic from the old start would say %u, %d samples away)\n", S0, stale, (int)(S0 - stale)); }
  runBlocks(kSampleRate / kBlock);
  CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdSetTempo, 0, 0, 120);
  runBlocks(kSampleRate / kBlock);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// ---------------------------------------------------------------------------------------------- single-track jobs

TEST(generate_pattern_changes_file_keeps_sound) {
  Generator& g = gen();
  std::string file = g.info(Chord).file, sound = g.info(Chord).sound; int preset = g.info(Chord).presetIndex; uint32_t seed = g.info(Chord).seed;
  bool changed = false;
  for (int attempt = 0; attempt < 3 && !changed; attempt++) {   // 37 chord files, history of 8: a repeat is rare
    CHECK(g.generatePattern(Chord));
    runUntilIdle();
    changed = file != g.info(Chord).file;
  }
  CHECK(changed);
  CHECK(sound == g.info(Chord).sound);
  CHECK_EQ(g.info(Chord).presetIndex, preset);
  CHECK(g.info(Chord).seed != seed);
  CHECK_EQ((int)g.info(Chord).lenStep, 0);
  CHECK(g.lastError()[0] == 0);
  runBlocks(kSampleRate * 2 / kBlock);                         // the swap lands at the bar
  CHECK(eng().status().track[Chord].state != PSEmpty);
}

TEST(generate_sound_changes_preset_keeps_file) {
  Generator& g = gen();
  std::string file = g.info(Chord).file; int preset = g.info(Chord).presetIndex;
  CHECK(g.generateSound(Chord));
  runUntilIdle();
  CHECK(file == g.info(Chord).file);
  CHECK(g.info(Chord).presetIndex != preset);                  // shuffle without immediate repeat
  CHECK(g.info(Chord).sound[0] != 0);
  std::vector<Cmd> seen;                                       // no pattern swap was sent for a sound change
  // (the queue is drained by render; check through a second request peeked before rendering)
  CHECK(g.generateSound(Lead));
  gen().step();
  bool patch = false, swap = false;
  for (const Cmd& c : peekCmds()) { if (c.type == CmdSetPatch && c.track == Lead) patch = true; if (c.type == CmdSwapPattern && c.track == Lead) swap = true; }
  CHECK(patch); CHECK(!swap);
  runUntilIdle();
  // a kit sound change reloads the four beat slots
  const uint16_t ack0 = eng().status().slotAck;
  std::string beatFile = g.info(Beat).file;
  CHECK(g.generateSound(Beat));
  runUntilIdle();
  CHECK_EQ((int)(uint16_t)(eng().status().slotAck - ack0), 4);
  CHECK(beatFile == g.info(Beat).file);
}

TEST(rebuild_variant_beat_sends_mask) {
  Generator& g = gen();
  g.rebuildVariant(Beat, 1);
  bool mask = false, swap = false;
  for (const Cmd& c : peekCmds()) {
    if (c.type == CmdBeatMask && c.a == 0) { mask = true; CHECK_EQ(c.i, 0x0D | (1 << 8)); }
    if (c.type == CmdSwapPattern && c.track == Beat) { swap = true; CHECK_EQ((int)c.a, 1); CHECK_EQ(c.i, 0); }
  }
  CHECK(mask); CHECK(swap);
  CHECK_EQ((int)g.info(Beat).variant, 1);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)eng().status().track[Beat].variant, 1);
  g.rebuildVariant(Beat, 3);
  for (const Cmd& c : peekCmds()) if (c.type == CmdBeatMask) CHECK_EQ(c.i, 0x1F | (3 << 8));
  runBlocks(kSampleRate * 3 / kBlock);
  // tonal variants rebuild from the stored seed and key: deterministic
  g.rebuildVariant(Chord, 2);
  Pattern a = eng().nextPattern(Chord);
  CHECK_EQ((int)a.lengthTicks, 2 * kTicksPerBeat);
  g.rebuildVariant(Chord, 2);
  CHECK(memcmp(&a, &eng().nextPattern(Chord), sizeof(Pattern)) == 0);
  g.rebuildVariant(Chord, 0);
  CHECK_EQ((int)eng().nextPattern(Chord).lengthTicks, (int)g.info(Chord).sourceTicks);
  runBlocks(kSampleRate * 3 / kBlock);
}

TEST(set_length_step_clamps) {
  Generator& g = gen();
  TrackGenInfo& inf = g.info(Lead); inf.naturalTicks = kTicksPerBar; inf.lenStep = 0;
  CHECK_EQ((int)g.setLengthStep(Lead, +1), 2 * kTicksPerBar);
  CHECK_EQ((int)g.setLengthStep(Lead, +1), 4 * kTicksPerBar);   // 16 beats
  CHECK_EQ((int)g.setLengthStep(Lead, +1), 4 * kTicksPerBar);   // clamped
  CHECK_EQ((int)inf.lenStep, 2);
  CHECK_EQ((int)g.setLengthStep(Lead, -1), 2 * kTicksPerBar);
  for (int i = 0; i < 4; i++) g.setLengthStep(Lead, -1);
  CHECK_EQ((int)g.setLengthStep(Lead, -1), kTicksPerBeat / 2);  // clamped at half a beat
  CHECK_EQ((int)inf.lenStep, -3);
  std::vector<Cmd> cmds = peekCmds(); int n = 0;
  for (const Cmd& c : cmds) if (c.type == CmdSetLength && c.track == Lead) { n++; CHECK(c.i >= kTicksPerBeat / 2 && c.i <= kMaxPatternTicks); }
  CHECK_EQ(n, 9);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)eng().status().track[Lead].lenTicks, kTicksPerBeat / 2);
  CHECK_EQ((int)g.setLengthStep(Lead, 0), kTicksPerBeat / 2);  // dir 0 = query, nothing sent
  CHECK(peekCmds().empty());
  CHECK_EQ((int)g.setLengthStep(99, 1), 0);
  g.rebuildVariant(Lead, 0);                                   // variant select resets k (SPEC §9.4)
  CHECK_EQ((int)inf.lenStep, 0);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)eng().status().track[Lead].lenTicks, (int)inf.naturalTicks);
}

TEST(fx_user_loop_is_kept_by_pattern_generation_only) {
  Generator& g = gen();
  g.info(Fx).userLoop = 1;
  std::string file = g.info(Fx).file, sound = g.info(Fx).sound; const int preset = g.info(Fx).presetIndex;
  CHECK(g.generatePattern(Fx));
  runUntilIdle();
  CHECK(strcmp(g.lastNote(), "FX kept") == 0);
  CHECK(file == g.info(Fx).file);
  CHECK_EQ((int)g.info(Fx).userLoop, 1);
  CHECK(g.generateSound(Fx));                                  // gen sound still re-rolls (SPEC §9.10)
  runUntilIdle();
  CHECK(g.info(Fx).presetIndex != preset);                     // the preset bag never repeats immediately
  CHECK(sound != g.info(Fx).sound);
  CHECK(file == g.info(Fx).file);
  g.info(Fx).userLoop = 0;
}

TEST(requests_queue_while_busy) {
  Generator& g = gen();
  CHECK(g.generatePattern(Bass));
  CHECK(g.generateSound(Pad));
  CHECK(g.generateSound(Pad));                                 // duplicate request is folded
  CHECK(g.busy());
  runUntilIdle();
  CHECK(!g.busy());
  CHECK(g.lastError()[0] == 0);
}

TEST(testpack_vocal_without_bpm_token_leaves_vocal_empty) {
  Generator& g = gen(); Engine& e = eng();
  g.setGenre(1);
  CHECK(strcmp(g.genreName(), "Testpack") == 0);
  CHECK(g.generateSound(Vocal));
  int steps = runUntilIdle();
  std::printf("  gave up after %d steps, lastError: '%s'\n", steps, g.lastError());
  CHECK(steps <= Generator::kVocalPickAttempts + 4 + Generator::kPrepareStepsMax);   // warm-up, six re-picks (one per step), then give up
  CHECK(strstr(g.lastError(), "bpm") != nullptr);
  CHECK(e.nextVocal(0).present == 0);
  CHECK(g.info(Vocal).file[0] == 0);
  CHECK(!g.vocalSwitchPending(0));
  CHECK(e.vocalRing(0).active.load() == 0);
  const uint16_t under0 = e.status().vocalUnderruns;
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);      // an empty vocal never starves: the ring is parked
  g.setGenre(0);
  CHECK(strcmp(g.genreName(), "House") == 0);
  CHECK(g.generateSound(Vocal));                               // a House phrase again
  runUntilIdle();
  CHECK(g.lastError()[0] == 0);
  CHECK(e.nextVocal(0).present == 1);
}

// Finding 5: a pat press on an empty vocal has nothing to rebuild; it must not be queued and replayed on the
// phrase loaded later (the alternate file would silently replace the just-generated primary).
TEST(variant_press_on_empty_vocal_is_not_replayed) {
  Generator& g = gen(); Engine& e = eng();
  g.setGenre(1);
  CHECK(g.generateSound(Vocal));
  runUntilIdle();
  CHECK(e.nextVocal(0).present == 0); CHECK(g.info(Vocal).file[0] == 0);
  g.rebuildVariant(Vocal, 1);                                  // alternate file: nothing loaded, nothing to retry
  g.rebuildVariant(Vocal, 2);
  runBlocks(8);
  g.setGenre(0);
  CHECK(g.generateSound(Vocal));
  runUntilIdle();
  CHECK(e.nextVocal(0).present == 1);
  std::string primary = g.vocalPath(0, 0); primary = primary.substr(primary.rfind('/') + 1);
  runBlocks(kSampleRate * 3 / kBlock);                         // any replayed press would have switched by now
  CHECK_EQ((int)g.info(Vocal).variant, 0);
  CHECK(primary == g.info(Vocal).file);
  CHECK(!g.vocalSwitchPending(0));
  CHECK_EQ((int)e.status().track[Vocal].variant, 0);
  // the same after a whole-set GENERATE following such a press
  g.setGenre(1); CHECK(g.generateSound(Vocal)); runUntilIdle(); CHECK(e.nextVocal(0).present == 0);
  g.rebuildVariant(Vocal, 1);
  g.setGenre(0); CHECK(g.generateSet(0)); runUntilIdle(); CHECK(g.lastError()[0] == 0);
  primary = g.vocalPath(0, 0); primary = primary.substr(primary.rfind('/') + 1);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)g.info(Vocal).variant, 0);
  CHECK(primary == g.info(Vocal).file);
}

TEST(generate_set_1_and_two_vocals_at_once) {
  Generator& g = gen(); Engine& e = eng();
  const uint16_t ack0 = e.status().slotAck;
  CHECK(g.generateSet(1));
  runUntilIdle();
  CHECK(g.lastError()[0] == 0);
  for (int t = kTracksPerSet; t < kTracks; t++) CHECK(e.status().track[t].state != PSEmpty);
  CHECK_EQ((int)(uint16_t)(e.status().slotAck - ack0), 8);
  CHECK(e.nextVocal(1).present == 1);
  push(CmdTrackToggle, Vocal); push(CmdTrackToggle, kTracksPerSet + Vocal);
  const uint16_t under0 = e.status().vocalUnderruns;
  static int minAhead; minAhead = 1 << 30;
  g_afterPump = [] {
    if (eng().status().track[Vocal].state == PSPlaying && eng().status().track[kTracksPerSet + Vocal].state == PSPlaying) {
      int a0 = vocalAhead(0), a1 = vocalAhead(1); if (a0 < minAhead) minAhead = a0; if (a1 < minAhead) minAhead = a1;
    }
  };
  runBlocks(kSampleRate * 4 / kBlock);
  g_afterPump = nullptr;
  CHECK_EQ((int)e.status().track[Vocal].state, (int)PSPlaying);
  CHECK_EQ((int)e.status().track[kTracksPerSet + Vocal].state, (int)PSPlaying);
  std::printf("  both vocals: min lead %d, underruns %u\n", minAhead, (unsigned)(e.status().vocalUnderruns - under0));
  CHECK(minAhead >= 2048);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  int compared = 0; int bad = compareRing(1, 1500, &compared);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // stop + restart: the idle pre-roll re-arms at the next bar and the restart is clean
  push(CmdTrackToggle, Vocal);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[Vocal].state, (int)PSStopped);
  push(CmdTrackToggle, Vocal);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[Vocal].state, (int)PSPlaying);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  push(CmdTrackStopNow, Vocal); push(CmdTrackStopNow, kTracksPerSet + Vocal);
  runBlocks(4);
}

TEST(generate_set_while_the_vocal_plays_switches_on_a_bar) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  push(CmdTrackToggle, t);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  std::string before = g.info(t).file;
  const uint16_t under0 = e.status().vocalUnderruns;
  const uint32_t start0 = e.status().track[t].startSample;
  CHECK(g.generateSet(0));
  runUntilIdle();
  CHECK(g.lastError()[0] == 0);
  CHECK(!g.vocalSwitchPending(0));
  CHECK(before != g.info(t).file || strcmp(g.vocalPath(0, 0), g.vocalPath(0, 1)) != 0);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);  // play state untouched by the swap (SPEC §9.8)
  CHECK_EQ(e.status().track[t].startSample, start0);        // content keeps following the original start
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  // the new phrase plays its window (2 bars, or a '<n>bars' token's) + the track's pause (SPEC §9.16 / amendment)
  CHECK_EQ((int)e.status().track[t].lenTicks, (int)Generator::vocalClipTicks(g.info(t).variant, g.info(t).pause, g.info(t).windowBars));
  CHECK_EQ((int)g.info(t).pause, (int)Generator::fitPause(1, g.info(t).windowBars));
  std::printf("  new phrase window %d bars, cycle %d bars\n", (int)g.info(t).windowBars, (int)e.status().track[t].lenTicks / kTicksPerBar);
  CHECK_EQ((int)e.nextVocal(0).fileBpm, (int)g.info(t).fileBpm);
  int compared = 0; int bad = compareRing(0, 1500, &compared);
  std::printf("  new phrase %s while playing: %d compared, %d mismatches\n", g.info(t).file, compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  for (int tr = 0; tr < kTracksPerSet; tr++) CHECK(e.status().track[tr].state != PSEmpty);
  push(CmdTrackStopNow, t);
  runBlocks(4);
}

// Finding 1: GENERATE with the master transport stopped (stop → generate → play is a normal live workflow)
// must finish: the vocal switch commands and the set's swaps go out while stopped and the engine applies them
// on the first bar line after resume.
TEST(generate_set_while_transport_stopped_completes_and_swaps_on_resume) {
  Generator& g = gen(); Engine& e = eng();
  push(CmdMasterPlay, 0, 0);
  runBlocks(2);
  CHECK_EQ((int)e.status().running, 0);
  const uint16_t ack0 = e.status().slotAck;
  const std::string vocalBefore = g.info(kTracksPerSet + Vocal).file;
  CHECK(g.generateSet(1));
  const int steps = runUntilIdle(86 * 30);
  std::printf("  stopped transport: set 1 generated in %d steps, generating flag %d, pending swaps %d\n", steps, (int)e.status().generating[1], pendingSwaps(1));
  CHECK(!g.busy());
  CHECK_EQ(g.progress(), 8);
  CHECK(g.lastError()[0] == 0);
  CHECK_EQ((int)e.status().generating[1], 0);
  CHECK_EQ((int)(uint16_t)(e.status().slotAck - ack0), 8);
  CHECK(!g.setSwapWaiting(1)); CHECK(!g.vocalSwitchPending(1));
  CHECK_EQ(pendingSwaps(1), kTracksPerSet);                  // all eight wait for a bar line
  CHECK(vocalBefore != g.info(kTracksPerSet + Vocal).file || strcmp(g.vocalPath(1, 0), g.vocalPath(1, 1)) != 0);
  CHECK(g.generateSound(kTracksPerSet + Chord));              // the queue is not blocked by the stopped set job
  runUntilIdle(); CHECK(!g.busy());
  CHECK_EQ(pendingSwaps(1), kTracksPerSet);
  runBlocks(kSampleRate * 2 / kBlock);                         // still stopped: nothing applies
  CHECK_EQ(pendingSwaps(1), kTracksPerSet);
  push(CmdMasterPlay, 0, 1);
  drainEvents();
  CHECK(runUntilBar(kSampleRate * 3 / kBlock));
  runBlocks(2);
  CHECK_EQ((int)e.status().running, 1);
  CHECK_EQ(pendingSwaps(1), 0);                               // landed on the first bar after resume
  for (int t = kTracksPerSet; t < kTracks; t++) CHECK(e.status().track[t].state != PSEmpty);
  CHECK(e.nextVocal(1).present == 1);
  CHECK_EQ((int)e.status().track[kTracksPerSet + Beat].variant, 0);   // Kick only after a pattern generation (SPEC §9.17b)
  runBlocks(kSampleRate * 2 / kBlock);
  { int guard = 0; while (!g.vocalArmed(1) && guard++ < 40) iterate(); }   // a bar may just have passed: the pre-roll is rebuilt for the next bar's phase (SPEC §9.16)
  CHECK(g.vocalArmed(1));                                      // idle pre-roll resumed with the transport
}

// Finding 2 (SPEC §9.5, §9.6): with the Beat on a non-Full layer, a whole-set GENERATE must switch the layer
// mask on the same bar line as the patterns: CmdBeatMask travels with CmdSwapSet, never ahead of it.
TEST(generate_set_switches_beat_mask_with_the_set) {
  Generator& g = gen(); Engine& e = eng();
  g.rebuildVariant(Beat, 2);                                   // +Snare; the generate lands on Kick only (variant 0, SPEC §9.17b)
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[Beat].variant, 2);
  static int iterMask, iterSwap, maskWithoutSet; static uint32_t chordHash0; iterMask = iterSwap = -1; maskWithoutSet = 0;
  auto chordHash = [&]() { const Pattern& p = e.activePattern(Chord); uint32_t h = 2166136261u ^ p.count ^ ((uint32_t)p.lengthTicks << 16); for (int i = 0; i < p.count; i++) h = (h ^ p.notes[i].packed) * 16777619u; return h; };
  chordHash0 = chordHash();
  g_afterPump = [] {
    bool mask = false, swapSet = false;
    for (const Cmd& c : peekCmds()) { if (c.type == CmdBeatMask && c.a == 0) mask = true; if (c.type == CmdSwapSet && c.a == 0) swapSet = true; }
    if (mask && !swapSet) maskWithoutSet++;
    const EngineStatus& st = eng().status();
    if (iterMask < 0 && st.track[Beat].variant == 0) iterMask = g_steps;
    // the Chord's active pattern changes at the bar line the set swap lands on (pendingSwap can be set and cleared inside
    // one block when the burst leaves right before a bar line, so the pattern itself is what is watched)
    const Pattern& p = eng().activePattern(Chord); uint32_t h = 2166136261u ^ p.count ^ ((uint32_t)p.lengthTicks << 16); for (int i = 0; i < p.count; i++) h = (h ^ p.notes[i].packed) * 16777619u;
    if (iterSwap < 0 && h != chordHash0) iterSwap = g_steps;
  };
  CHECK(g.generateSet(0));
  runUntilIdle();
  CHECK(g.lastError()[0] == 0);
  runBlocks(kSampleRate * 3 / kBlock);
  g_afterPump = nullptr;
  std::printf("  mask applied at iteration %d, set swap at %d, masks pushed without CmdSwapSet: %d\n", iterMask, iterSwap, maskWithoutSet);
  CHECK_EQ(maskWithoutSet, 0);
  CHECK(iterMask > 0 && iterSwap > 0);
  CHECK_EQ(iterMask, iterSwap);
  CHECK_EQ((int)e.status().track[Beat].variant, 0); CHECK_EQ((int)g.info(Beat).variant, 0);
  Pattern rush; snareRush(e.activePattern(Beat), rush);        // the rush loop was rebuilt from the new Full pattern
  CHECK(memcmp(&rush, &e.rushPattern(0), sizeof(Pattern)) == 0);
}

// Finding 7: a full command queue (the UI, ramps and both producers share it) must not hang a job or split a
// set switch; pushes keep their state and retry. The queue is kept full for 40 loop iterations in the middle
// of a GENERATE.
TEST(command_queue_full_defers_pushes_without_losing_them) {
  Generator& g = gen(); Engine& e = eng();
  const uint16_t ack0 = e.status().slotAck;
  static int partial, maxPending, lastPending; partial = 0; maxPending = 0; lastPending = 0;
  g_afterPump = [] {
    const int p = pendingSwaps(1);
    if (p > maxPending) maxPending = p;
    if (p < lastPending && p != 0) partial++;                  // a decrease that does not clear the set = a split switch
    lastPending = p;
  };
  CHECK(g.generateSet(1));
  runBlocks(5);
  g_fillQueue = true;
  runBlocks(40);
  g_fillQueue = false;
  CHECK(g.busy());
  const int steps = runUntilIdle(86 * 30);
  runBlocks(kSampleRate * 3 / kBlock);
  g_afterPump = nullptr;
  std::printf("  queue full for 40 iterations: job took %d more steps, max pending swaps %d, split switches %d, lastError '%s'\n", steps, maxPending, partial, g.lastError());
  CHECK(!g.busy());
  CHECK_EQ(g.progress(), 8);
  CHECK(strstr(g.lastError(), "queue") == nullptr);
  CHECK_EQ((int)(uint16_t)(e.status().slotAck - ack0), 8);
  CHECK_EQ(maxPending, kTracksPerSet); CHECK_EQ(partial, 0);
  CHECK_EQ(pendingSwaps(1), 0);
  for (int t = kTracksPerSet; t < kTracks; t++) CHECK(e.status().track[t].state != PSEmpty);
  for (int s = 0; s < kSampleSlots; s++) CHECK(e.kit(1).slot[s].present == 1);
  CHECK_EQ((int)e.status().generating[1], 0);
  CHECK(e.nextVocal(1).present == 1);
}

// A content folder that is sharded (SPEC §9.2: only s00, s01, ... subfolders) has its files in the shards:
// the first shard, else the folder itself.
static std::string contentFolder(const std::string& folder) {
  NameFn dirs; sd().walk(folder.c_str(), collectDirs, &dirs);
  std::sort(dirs.names.begin(), dirs.names.end());
  return dirs.names.empty() ? folder : folder + "/" + dirs.names[0];
}

TEST(load_helpers_for_set_files) {
  Generator& g = gen(); Engine& e = eng();
  char name[kMaxNameLen]; char path[kMaxPathLen];
  const std::string chordDir = contentFolder("/lp/midi/Chord/Amin"), percDir = contentFolder("/lp/midi/Percussion/NA");
  CHECK(entryAt(sd(), chordDir.c_str(), ".mid", 0, name, sizeof name));
  joinPath(path, sizeof path, chordDir.c_str(), name);
  CHECK(g.loadMidiToSource(Chord, path));
  CHECK(strcmp(g.info(Chord).file, name) == 0);
  CHECK_EQ((int)g.info(Chord).sourceTicks % kTicksPerBeat, 0);
  CHECK(e.sourcePattern(Chord).count > 0);
  g.info(Chord).seed = 777; g.info(Chord).keyRoot = 9;
  CHECK(g.publishVariant(Chord, 0));
  CHECK_EQ((int)e.nextPattern(Chord).lengthTicks, (int)g.info(Chord).sourceTicks);
  CHECK(e.nextPattern(Chord).count > 0);
  CHECK_EQ((int)g.info(Chord).naturalTicks, (int)g.info(Chord).sourceTicks);
  // Percussion: the fold map is a pure function of the source, recomputed on load (SPEC §9.7); the source
  // stays unfolded. Walks the folder for a file with pitches outside 36..39 (most Groove slices have them).
  bool unfolded = false; int tried = 0;
  for (int ord = 0; ord < 40 && !unfolded; ord++) {
    if (!entryAt(sd(), percDir.c_str(), ".mid", ord, name, sizeof name)) break;
    joinPath(path, sizeof path, percDir.c_str(), name);
    CHECK(g.loadMidiToSource(Percussion, path)); tried++;
    for (int i = 0; i < e.sourcePattern(Percussion).count; i++) { int p = e.sourcePattern(Percussion).notes[i].pitch(); if (p < 36 || p > 39) unfolded = true; }
  }
  std::printf("  percussion source with unfolded pitches after %d files: %s\n", tried, name);
  CHECK(unfolded);
  uint8_t map[128]; percussionFoldMap(e.sourcePattern(Percussion), map);
  CHECK(memcmp(map, g.foldMap(0), 128) == 0);
  CHECK(strcmp(g.info(Percussion).file, name) == 0);
  CHECK(g.publishVariant(Percussion, 1));
  for (int i = 0; i < e.nextPattern(Percussion).count; i++) { int p = e.nextPattern(Percussion).notes[i].pitch(); CHECK(p >= 36 && p <= 39); }
  CHECK(!g.loadMidiToSource(Chord, "/lp/midi/Chord/Amin/nope.mid"));
  CHECK(strstr(g.lastError(), "nope.mid") != nullptr);
  CHECK(!g.loadMidiToSource(99, path));
  runBlocks(kSampleRate * 3 / kBlock);
}

// Finding 10: only what wavReadMono streams (16-bit, 1-2 channels) may become a phrase; anything else would
// load "successfully" and play silence. Rejected files name themselves in lastError.
TEST(unstreamable_wav_is_rejected_as_a_vocal) {
  Generator& g = gen();
  CHECK(sd().mkdir("/lp/_gen_scratch"));
  const char* bad8 = "/lp/_gen_scratch/eight_bit_120bpm.wav";
  const char* bad4 = "/lp/_gen_scratch/quad_120bpm.wav";
  const char* bad24 = "/lp/_gen_scratch/deep_120bpm.wav";
  const char* good = "/lp/_gen_scratch/mono_120bpm.wav";
  const char* nobpm = "/lp/_gen_scratch/mono_untagged.wav";
  CHECK(writeWav(bad8, 1, 8, 22050, 22050)); CHECK(writeWav(bad4, 4, 16, 22050, 22050)); CHECK(writeWav(bad24, 1, 24, 22050, 22050));
  CHECK(writeWav(good, 2, 16, 22050, 44100)); CHECK(writeWav(nobpm, 1, 16, 22050, 22050));
  const int opens = sd().openCount(); const std::string before = g.info(kTracksPerSet + Vocal).file;
  CHECK(!g.loadVocal(1, bad8, 0)); CHECK(strstr(g.lastError(), "bad wav") != nullptr); CHECK(strstr(g.lastError(), "eight_bit") != nullptr);
  CHECK(!g.loadVocal(1, bad4, 0)); CHECK(strstr(g.lastError(), "bad wav") != nullptr);
  CHECK(!g.loadVocal(1, bad24, 0)); CHECK(strstr(g.lastError(), "bad wav") != nullptr);
  CHECK(!g.loadVocal(1, nobpm, 0)); CHECK(strstr(g.lastError(), "bpm") != nullptr);
  CHECK(!g.loadVocal(1, "/lp/_gen_scratch/missing_120bpm.wav", 0)); CHECK(strstr(g.lastError(), "vocal open") != nullptr);
  CHECK_EQ(sd().openCount(), opens);                           // every rejected file is closed again
  CHECK(before == g.info(kTracksPerSet + Vocal).file);         // nothing replaced the phrase
  CHECK(!g.vocalSwitchPending(1));
  CHECK(g.loadVocal(1, good, 0));                              // a 16-bit stereo file streams (mono-ized)
  CHECK_EQ((int)g.info(kTracksPerSet + Vocal).fileBpm, 120); CHECK_EQ((int)g.info(kTracksPerSet + Vocal).naturalTicks, 4 * kTicksPerBeat);   // 2 s at 120 bpm = 4 beats
  CHECK(g.vocalSwitchPending(1));
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK(!g.vocalSwitchPending(1));
  CHECK(g.generateSound(kTracksPerSet + Vocal));                // back to a House phrase for the later tests
  runUntilIdle(); runBlocks(kSampleRate * 3 / kBlock);
  CHECK(g.lastError()[0] == 0);
  for (const char* p : {bad8, bad4, bad24, good, nobpm}) CHECK(sd().remove(p));
  CHECK(sd().remove("/lp/_gen_scratch"));
}

// Finding 4 / 9: the library warm-up the generator drives (Library::prepare*) streams one index chunk per call
// and leaves the pick itself with at most two 4 KB reads; a key change keeps the cached counts (the library's
// invalidate() restarts bags and histories only), so no index is streamed again.
TEST(library_prepare_is_resumable_and_key_change_keeps_counts) {
  Generator& g = gen(); Library& l = lib();
  l.dropCache();
  int calls = 0; uint64_t maxCall = 0;
  while (calls < 400) { const uint64_t b0 = sd().bytes; const bool warm = l.prepareMidi(Beat, g.key()); const uint64_t d = sd().bytes - b0; if (d > maxCall) maxCall = d; calls++; if (warm) break; }
  std::printf("  Beat/NA warm after %d prepare calls, max %llu B per call\n", calls, (unsigned long long)maxCall);
  CHECK(calls >= 8 && calls < 400);                            // the category and NA walks, then one chunk per shard (14 on the sharded card)
  CHECK(maxCall <= 4096 + 512);
  char out[kMaxPathLen]; Rng rng(5);
  uint64_t b0 = sd().bytes; CHECK(l.pickMidi(Beat, g.key(), Beat, rng, out, sizeof out)); CHECK(sd().bytes - b0 <= 2 * 4096 + 512);
  calls = 0; maxCall = 0;
  while (calls < 400) { const uint64_t c0 = sd().bytes; const bool warm = l.prepareSample("House", "kick", g.key().root); const uint64_t d = sd().bytes - c0; if (d > maxCall) maxCall = d; calls++; if (warm) break; }
  std::printf("  keyed kick warm after %d prepare calls, max %llu B per call\n", calls, (unsigned long long)maxCall);
  CHECK(calls >= 2 && calls < 400); CHECK(maxCall <= 4096 + 512);
  b0 = sd().bytes; CHECK(l.pickSample("House", "kick", g.key().root, rng, out, sizeof out)); CHECK(sd().bytes - b0 <= 2 * 4096 + 512);
  calls = 0;
  while (calls < 400) { calls++; if (l.prepareVocal("House", g.key())) break; }
  CHECK(calls < 400);
  calls = 0;
  while (calls < 400) { calls++; if (l.prepareMidi(Percussion, g.key()) && l.prepareSample("House", "snare", -1) && l.prepareSample("House", "oh", -1)) break; }
  CHECK(calls < 400);
  // key change (SPEC §9.9): the key-independent picks (Beat, Percussion, kit, vocal NA) stay warm — no index is
  // streamed again; only the bags and histories restart in the library. On the sharded card (SPEC §9.2) a
  // tree is one of the 16 folder slots whatever its shard count, so Beat (2 slots), Percussion (2), the kick,
  // snare and oh roles (1 each) and the vocal folders (3) prepared above are all still cached.
  const int root0 = g.key().root;
  g.setKeyRoot(4);
  b0 = sd().bytes; const uint64_t o0 = sd().opens;
  CHECK(l.prepareMidi(Percussion, g.key())); CHECK(l.prepareSample("House", "snare", -1)); CHECK(l.prepareSample("House", "oh", -1));
  CHECK(l.prepareMidi(Beat, g.key()));
  CHECK_EQ((int)(sd().bytes - b0), 0); CHECK_EQ((int)(sd().opens - o0), 0);
  CHECK(l.pickMidi(Percussion, g.key(), Percussion, rng, out, sizeof out)); CHECK(sd().bytes - b0 <= 2 * 4096 + 512);   // the pick itself: ordinal lookup only
  b0 = sd().bytes;
  CHECK(l.pickMidi(Beat, g.key(), Beat, rng, out, sizeof out)); CHECK(sd().bytes - b0 <= 2 * 4096 + 512);
  g.setKeyRoot(root0);
  b0 = sd().bytes;
  CHECK(l.prepareMidi(Beat, g.key()));
  CHECK_EQ((int)(sd().bytes - b0), 0);
  // the keyed kick and the vocal folders of the original key (6 + 3 folders) survive a key change away and back
  calls = 0;
  while (calls < 400) { calls++; if (l.prepareSample("House", "kick", g.key().root) && l.prepareVocal("House", g.key())) break; }
  CHECK(calls < 400);
  g.setKeyRoot(4); g.setKeyRoot(root0);
  b0 = sd().bytes;
  CHECK(l.prepareVocal("House", g.key())); CHECK(l.prepareSample("House", "kick", g.key().root));
  CHECK_EQ((int)(sd().bytes - b0), 0);
  CHECK_EQ((int)g.key().root, root0);
}


// ---------------------------------------------------------------------------------------------- SPEC §9.17b

// Does this engine already apply the §9.17b commands (CmdResetTrack, the knob ParamIds)? The engine module lands in
// parallel: the generator's contract is what it PUSHES; the params are checked only where the engine answers.
static bool engineAppliesKnobs() {
  static int known = -1;
  if (known < 0) {
    const float before = eng().params(kTracksPerSet + Pad).cutoff;
    push(CmdSetParam, kTracksPerSet + Pad, PCutoff, 0, 0.3125f); runBlocks(2);
    known = std::fabs(eng().params(kTracksPerSet + Pad).cutoff - 0.3125f) < 1e-5f ? 1 : 0;
    push(CmdSetParam, kTracksPerSet + Pad, PCutoff, 0, before); runBlocks(2);
  }
  return known == 1;
}
static bool engineAppliesReset() {
  static int known = -1;
  if (known < 0) {
    const float before = eng().params(kTracksPerSet + Pad).volume;
    push(CmdSetParam, kTracksPerSet + Pad, PVolume, 0, 0.3f); runBlocks(2);
    push(CmdResetTrack, kTracksPerSet + Pad); runBlocks(2);
    known = std::fabs(eng().params(kTracksPerSet + Pad).volume - 0.8f) < 1e-5f ? 1 : 0;
    push(CmdSetParam, kTracksPerSet + Pad, PVolume, 0, before); runBlocks(2);
  }
  return known == 1;
}
struct Knobs { float cutoff = -1, res = -1, env = -1, decay = -1, drive = -1, sat = -1, accent = -1, chorus = -1; int wave = -1; int n = 0; };
static Knobs knobCmds(const std::vector<Cmd>& cmds, int track) {
  Knobs k;
  for (const Cmd& c : cmds) {
    if (c.type != CmdSetParam || c.track != track) continue;
    k.n++;
    if (c.a == PCutoff) k.cutoff = c.f; else if (c.a == PResonance) k.res = c.f; else if (c.a == PEnvAmt) k.env = c.f;
    else if (c.a == PDecay) k.decay = c.f; else if (c.a == PDrive) k.drive = c.f; else if (c.a == PWave) k.wave = c.i;
    else if (c.a == PSat) k.sat = c.f; else if (c.a == PAccent) k.accent = c.f; else if (c.a == PChorus) k.chorus = c.f;   // SPEC §9.22
  }
  return k;
}
static bool patternInKey(const Pattern& p, int root) { ProjectKey k; k.root = (uint8_t)root; for (int i = 0; i < p.count; i++) if (!k.contains(p.notes[i].pitch())) return false; return true; }

// SPEC §9.17b ranges plus the §9.22 draws (research §4 / §5.3): SAT and ACC 0.3..0.9 on every acid-capable track, the
// chorus send 0..0.4 on Lead / Chord only, drive narrowed to 0..0.5 now that the equal-RMS makeup (§9.21) is in.
TEST(randomize_knobs_ranges_and_determinism) {
  Generator& g = gen();
  g.seed(4242u);
  g.randomizeKnobs(Lead);
  const Knobs a = knobCmds(peekCmds(), Lead);
  CHECK_EQ(a.n, 9); CHECK_EQ(Generator::knobCmdCount(Lead), 9); CHECK_EQ(Generator::knobCmdCount(Chord), 9); CHECK_EQ(Generator::knobCmdCount(Pad), 8); CHECK_EQ(Generator::knobCmdCount(Bass), 8);
  CHECK(a.cutoff >= 0.25f && a.cutoff <= 0.75f); CHECK(a.res >= 0.45f && a.res <= 0.95f); CHECK(a.env >= 0.3f && a.env <= 0.9f);
  CHECK(a.decay >= 0.15f && a.decay <= 0.6f); CHECK(a.drive >= 0.0f && a.drive <= 0.5f); CHECK(a.wave == WSaw || a.wave == WSquare);
  CHECK(a.sat >= 0.3f && a.sat <= 0.9f); CHECK(a.accent >= 0.3f && a.accent <= 0.9f); CHECK(a.chorus >= 0.0f && a.chorus <= 0.4f);
  runBlocks(2);
  if (engineAppliesKnobs()) { CHECK_NEAR(eng().params(Lead).cutoff, a.cutoff, 1e-5); CHECK_NEAR(eng().params(Lead).resonance, a.res, 1e-5); CHECK_EQ((int)eng().params(Lead).wave, a.wave); }
  else std::printf("  (engine does not apply the knob ParamIds yet: pushed values checked only)\n");
  g.seed(4242u);
  g.randomizeKnobs(Lead);                                     // same seed → the same nine values
  const Knobs b = knobCmds(peekCmds(), Lead);
  CHECK_EQ(b.n, 9); CHECK_NEAR(b.cutoff, a.cutoff, 1e-7); CHECK_NEAR(b.res, a.res, 1e-7); CHECK_NEAR(b.env, a.env, 1e-7); CHECK_NEAR(b.decay, a.decay, 1e-7); CHECK_NEAR(b.drive, a.drive, 1e-7); CHECK_EQ(b.wave, a.wave);
  CHECK_NEAR(b.sat, a.sat, 1e-7); CHECK_NEAR(b.accent, a.accent, 1e-7); CHECK_NEAR(b.chorus, a.chorus, 1e-7);
  runBlocks(2);
  // spread over many draws: every knob covers its range, both waves appear; Bass and Pad get no chorus send, Chord does
  float loC = 1, hiC = 0, loR = 1, hiR = 0, loD = 1, hiD = 0, loS = 1, hiS = 0, loA = 1, hiA = 0; int saw = 0, sq = 0;
  for (int i = 0; i < 40; i++) {
    g.randomizeKnobs(Bass); const Knobs k = knobCmds(peekCmds(), Bass); runBlocks(1);
    CHECK_EQ(k.n, 8); CHECK(k.chorus < 0);
    if (k.cutoff < loC) loC = k.cutoff; if (k.cutoff > hiC) hiC = k.cutoff; if (k.res < loR) loR = k.res; if (k.res > hiR) hiR = k.res; if (k.drive < loD) loD = k.drive; if (k.drive > hiD) hiD = k.drive;
    if (k.sat < loS) loS = k.sat; if (k.sat > hiS) hiS = k.sat; if (k.accent < loA) loA = k.accent; if (k.accent > hiA) hiA = k.accent;
    CHECK(k.sat >= 0.3f && k.sat <= 0.9f); CHECK(k.accent >= 0.3f && k.accent <= 0.9f); CHECK(k.drive <= 0.5f);
    if (k.wave == WSaw) saw++; else if (k.wave == WSquare) sq++;
  }
  CHECK(loC < 0.4f && hiC > 0.6f); CHECK(loR < 0.6f && hiR > 0.8f); CHECK(loD < 0.2f && hiD > 0.4f); CHECK(saw >= 8 && sq >= 8);
  CHECK(loS < 0.5f && hiS > 0.7f); CHECK(loA < 0.5f && hiA > 0.7f);
  float loH = 1, hiH = 0;
  for (int i = 0; i < 40; i++) { g.randomizeKnobs(Chord); const Knobs k = knobCmds(peekCmds(), Chord); runBlocks(1); CHECK_EQ(k.n, 9); CHECK(k.chorus >= 0.0f && k.chorus <= 0.4f); if (k.chorus < loH) loH = k.chorus; if (k.chorus > hiH) hiH = k.chorus; }
  CHECK(loH < 0.15f && hiH > 0.25f);
  g.randomizeKnobs(Pad); { const Knobs k = knobCmds(peekCmds(), Pad); CHECK_EQ(k.n, 8); CHECK(k.chorus < 0); } runBlocks(1);
  g.randomizeKnobs(Beat); CHECK(peekCmds().empty());         // not a synth track: nothing
  g.randomizeKnobs(99); CHECK(peekCmds().empty());
}

// SPEC §9.22 (research §4 "bitmask bag"): a family of up to kPresetBagMax presets is drawn without a repeat until it is
// exhausted — one cycle is a permutation of the family — and the next cycle never starts with the preset still sounding.
// The acid toggle switches families and restarts the cycle; a presetIndex written from outside (a set load) is the one
// the next draw avoids. The bag is 64 B per track: the mask alone.
TEST(preset_bag_draws_every_preset_once_per_cycle) {
  Generator& g = gen(); const int t = Lead;
  const int n = libraryPresetCount(Lead), na = acidPresetCount(Lead);
  CHECK(n >= 8); CHECK(na >= 8); CHECK(n + na <= Generator::kPresetBagMax);
  if (g.acidEnabled(t)) { g.setAcid(t, false); runBlocks(4); }
  // acid on (a draw from the '303' family) and off again restarts the library cycle: the roll of the toggle is its first draw
  g.setAcid(t, true); runBlocks(4); CHECK(isAcidPreset(Lead, g.info(t).presetIndex));
  g.setAcid(t, false); runBlocks(4); CHECK(!isAcidPreset(Lead, g.info(t).presetIndex));
  std::vector<int> draws; draws.push_back(g.info(t).presetIndex);
  for (int i = 1; i < 2 * n; i++) { CHECK(g.generateSound(t)); runUntilIdle(); draws.push_back(g.info(t).presetIndex); CHECK(!isAcidPreset(Lead, g.info(t).presetIndex)); }
  for (int cycle = 0; cycle < 2; cycle++) {                   // each cycle a permutation of [0, n)
    std::set<int> seen;
    for (int i = 0; i < n; i++) { const int p = draws[cycle * n + i]; CHECK(p >= 0 && p < n); seen.insert(p); }
    CHECK_EQ((int)seen.size(), n);
  }
  for (size_t i = 1; i < draws.size(); i++) CHECK(draws[i] != draws[i - 1]);   // the cycle boundary: no immediate repeat
  std::printf("  Lead library family of %d: two cycles of %d sound jobs, each a permutation; boundary %d → %d\n", n, n, draws[n - 1], draws[n]);
  // a presetIndex set from outside mid-cycle (what a set load does) is never the next draw
  for (int i = 0; i < 3; i++) {
    const int forced = (g.info(t).presetIndex + 1 + i) % n;
    g.info(t).presetIndex = (uint16_t)forced;
    CHECK(g.generateSound(t)); runUntilIdle();
    CHECK(g.info(t).presetIndex != forced);
  }
  // the acid family: its own cycle of na distinct '303' presets, the toggle's roll being the first
  g.setAcid(t, true); runBlocks(4);
  std::set<int> acid; acid.insert(g.info(t).presetIndex);
  for (int i = 1; i < na; i++) { CHECK(g.generateSound(t)); runUntilIdle(); CHECK(isAcidPreset(Lead, g.info(t).presetIndex)); acid.insert(g.info(t).presetIndex); }
  CHECK_EQ((int)acid.size(), na);
  g.setAcid(t, false); runBlocks(4); CHECK(!g.acidEnabled(t)); CHECK(!isAcidPreset(Lead, g.info(t).presetIndex));
  runBlocks(kSampleRate / kBlock);
  CHECK(g.lastError()[0] == 0);
}

// SPEC §9.23: Generator::seed restarts the preset bags with the Rng, so a session's first sound picks after the re-seed
// (recording and replay both begin with seed()) draw the same presets whatever the bag held before. From one state of the
// sounding preset (presetIndex / sound, which seed() leaves alone) the same seed yields the same pick sequence twice —
// without the restart the second run would avoid the presets the first one marked as seen.
TEST(seed_restarts_the_preset_bags) {
  Generator& g = gen(); const int t = Lead; const int n = libraryPresetCount(Lead);
  if (g.acidEnabled(t)) { g.setAcid(t, false); runBlocks(4); }
  constexpr int kPicks = 6;
  const uint16_t sounding = (uint16_t)(n / 3);
  auto run = [&](std::vector<int>& out) {
    g.seed(0xC0FFEEu); g.info(t).presetIndex = sounding; strcpy(g.info(t).sound, "sounding");
    for (int i = 0; i < kPicks; i++) { CHECK(g.generateSound(t)); runUntilIdle(); out.push_back(g.info(t).presetIndex); }
  };
  std::vector<int> first, second;
  run(first);                                                 // leaves kPicks bits in the Lead bag
  run(second);                                                // seed() cleared them: the very same draws
  CHECK_EQ((int)first.size(), kPicks); CHECK(first == second);
  std::set<int> distinct(first.begin(), first.end());
  CHECK_EQ((int)distinct.size(), kPicks);                     // one cycle: no repeat ...
  for (int p : first) { CHECK(p >= 0 && p < n); CHECK(p != sounding); }   // ... and never the preset sounding at the re-seed
  std::printf("  Lead picks after seed(): %d %d %d %d %d %d, twice\n", first[0], first[1], first[2], first[3], first[4], first[5]);
  // the control: without a re-seed the next kPicks avoid everything drawn so far
  std::vector<int> third;
  for (int i = 0; i < kPicks; i++) { CHECK(g.generateSound(t)); runUntilIdle(); third.push_back(g.info(t).presetIndex); }
  for (int p : third) CHECK(distinct.count(p) == 0);
  runBlocks(kSampleRate / kBlock);
  CHECK(g.lastError()[0] == 0);
}

TEST(acid_mode_rerolls_303_sound_and_generates_lines) {
  Generator& g = gen(); Engine& e = eng();
  const std::string file = g.info(Lead).file; const int libPreset = g.info(Lead).presetIndex;
  CHECK(!g.acidEnabled(Lead));
  CHECK(libraryPresetCount(Lead) >= 8 && acidPresetCount(Lead) >= 8 && presetCount(Lead) == libraryPresetCount(Lead) + acidPresetCount(Lead));
  CHECK(acidPresetCount(Fx) == 0 && acidPresetCount(Beat) == 0);
  g.setAcid(Lead, true);
  CHECK(g.acidEnabled(Lead));
  { std::vector<Cmd> c = peekCmds(); bool patch = false; for (const Cmd& k : c) if (k.type == CmdSetPatch && k.track == Lead) patch = true; CHECK(patch); CHECK_EQ(knobCmds(c, Lead).n, Generator::knobCmdCount(Lead)); }
  CHECK(isAcidPreset(Lead, g.info(Lead).presetIndex)); CHECK(strncmp(g.info(Lead).sound, "303", 3) == 0);
  CHECK(strncmp(e.nextPatch(Lead).name, "303", 3) == 0); CHECK_EQ((int)e.nextPatch(Lead).polyphony, 1); CHECK(e.nextPatch(Lead).glide >= 0.04f && e.nextPatch(Lead).glide <= 0.09f);
  CHECK(file == g.info(Lead).file);                           // the pattern stays
  runBlocks(2);
  // a pattern job in acid mode generates the line from a fresh seed, no card: the source is in key, variant 0 = the line
  CHECK(g.generatePattern(Lead));
  const uint64_t bytes0 = sd().bytes;
  runUntilIdle();
  CHECK(g.lastError()[0] == 0);
  CHECK_EQ((int)g.info(Lead).acidPat, 1); CHECK(g.info(Lead).file[0] == 0); CHECK_EQ((int)g.info(Lead).loaded, 1);
  const Pattern& src = e.sourcePattern(Lead);
  CHECK(src.count >= 8); CHECK(src.lengthTicks == kTicksPerBar || src.lengthTicks == 2 * kTicksPerBar);
  CHECK(patternInKey(src, g.key().root));
  Pattern line; acidLine(g.info(Lead).seed, g.key(), line, 2);   // rebuildable from seed + key (what a session restore does)
  CHECK_EQ((int)line.count, (int)src.count); CHECK_EQ((int)line.lengthTicks, (int)src.lengthTicks);
  for (int i = 0; i < line.count && i < src.count; i++) CHECK_EQ((long long)line.notes[i].packed, (long long)src.notes[i].packed);
  std::printf("  acid line: %d notes, %d ticks, %llu B of card reads for the job\n", (int)src.count, (int)src.lengthTicks, (unsigned long long)(sd().bytes - bytes0));
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK(e.activePattern(Lead).count == src.count);
  g.rebuildVariant(Lead, 2); CHECK_EQ((int)e.nextPattern(Lead).lengthTicks, 2 * kTicksPerBeat); g.rebuildVariant(Lead, 0);
  runBlocks(kSampleRate * 3 / kBlock);
  // every sound generation in acid mode randomises the knobs again, from the 303 family
  const int acidPreset = g.info(Lead).presetIndex;
  CHECK(g.generateSound(Lead)); gen().step();
  { std::vector<Cmd> c = peekCmds(); CHECK_EQ(knobCmds(c, Lead).n, 9); }   // SPEC §9.22: CUT RES ENV DEC DRV WAV SAT ACC + the Lead's chorus send
  runUntilIdle();
  CHECK(isAcidPreset(Lead, g.info(Lead).presetIndex)); CHECK(g.info(Lead).presetIndex != acidPreset);   // bag: no immediate repeat
  // chords get stabs
  g.setAcid(Chord, true); runBlocks(2);
  CHECK(g.generatePattern(Chord)); runUntilIdle();
  const Pattern& stabs = e.sourcePattern(Chord);
  CHECK(stabs.count >= 2); CHECK(patternInKey(stabs, g.key().root));
  for (int i = 0; i < stabs.count; i++) CHECK(stabs.notes[i].start() % kTicksPerBeat != 0);   // offbeats only
  CHECK(e.nextPatch(Chord).polyphony >= 3);
  // off: a library preset again, no knob commands; the pattern stays the acid one until regenerated
  g.setAcid(Lead, false); g.setAcid(Chord, false);
  { std::vector<Cmd> c = peekCmds(); CHECK_EQ(knobCmds(c, Lead).n, 0); CHECK_EQ(knobCmds(c, Chord).n, 0); }
  CHECK(!g.acidEnabled(Lead)); CHECK(!isAcidPreset(Lead, g.info(Lead).presetIndex)); CHECK_EQ((int)g.info(Lead).acidPat, 1);
  runBlocks(2);
  g.setAcid(Beat, true); CHECK(!g.acidEnabled(Beat)); CHECK(peekCmds().empty());   // not an acid category
  CHECK(g.generatePattern(Lead)); runUntilIdle(); CHECK(g.generatePattern(Chord)); runUntilIdle();   // back to library patterns
  CHECK(g.info(Lead).file[0] != 0); CHECK_EQ((int)g.info(Lead).acidPat, 0); CHECK(g.lastError()[0] == 0);
  runBlocks(kSampleRate * 3 / kBlock);
  (void)libPreset;
}

TEST(reset_on_generate_and_beat_starts_on_kick) {
  Generator& g = gen(); Engine& e = eng();
  push(CmdSetParam, Chord, PVolume, 0, 0.3f); push(CmdSetParam, Chord, PEcho, 0, 0.5f); runBlocks(2);
  CHECK_NEAR(e.params(Chord).volume, 0.3f, 1e-5);
  static int resets, resetBeforePatch; resets = 0; resetBeforePatch = 0;
  static bool seenReset; seenReset = false;
  g_afterPump = [] {
    for (const Cmd& c : peekCmds()) {
      if (c.type == CmdResetTrack && c.track == Chord) { seenReset = true; resets++; }
      if (c.type == CmdSetPatch && c.track == Chord && seenReset) resetBeforePatch++;
    }
  };
  CHECK(g.generateSound(Chord));
  runUntilIdle();
  g_afterPump = nullptr;
  CHECK(resets >= 1);                                          // the regenerated track is reset (SPEC §9.17b)
  CHECK(resetBeforePatch >= 1);                                // ... before its new sound: the reset never wipes the acid knobs
  if (engineAppliesReset()) { CHECK_NEAR(e.params(Chord).volume, 0.8f, 1e-5); CHECK_NEAR(e.params(Chord).echo, 0.0f, 1e-5); }
  else std::printf("  (engine does not apply CmdResetTrack yet: the push is checked only)\n");
  // a set job resets all eight tracks; the Beat comes back on Kick only
  g.rebuildVariant(Beat, 3); runBlocks(kSampleRate * 3 / kBlock); CHECK_EQ((int)e.status().track[Beat].variant, 3);
  static int resetTracks; resetTracks = 0;
  g_afterPump = [] { for (const Cmd& c : peekCmds()) if (c.type == CmdResetTrack && c.track < kTracksPerSet) resetTracks |= 1 << c.track; };
  CHECK(g.generateSet(0));
  runUntilIdle();
  g_afterPump = nullptr;
  CHECK_EQ(resetTracks, 0xFF);
  CHECK(g.lastError()[0] == 0);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[Beat].variant, 0); CHECK_EQ((int)g.info(Beat).variant, 0);
  // a Beat pattern job alone too, with the Kick mask
  g.rebuildVariant(Beat, 2); runBlocks(kSampleRate * 3 / kBlock); CHECK_EQ((int)e.status().track[Beat].variant, 2);
  CHECK(g.generatePattern(Beat));
  static int32_t maskSeen; maskSeen = -1;
  g_afterPump = [] { for (const Cmd& c : peekCmds()) if (c.type == CmdBeatMask && c.a == 0) maskSeen = c.i; };
  runUntilIdle();
  g_afterPump = nullptr;
  CHECK_EQ(maskSeen, 0x01 | (0 << 8));
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[Beat].variant, 0); CHECK_EQ((int)g.info(Beat).variant, 0);
  CHECK_EQ((int)g.info(Percussion).variant, 3);               // Percussion keeps Full
}

TEST(transpose_octave_shifts_source_and_republishes) {
  Generator& g = gen(); Engine& e = eng();
  Pattern& src = e.sourcePattern(Bass);
  CHECK(src.count > 0);
  const Pattern before = src;
  const uint32_t changes = g.changeCount();
  g.info(Bass).edited = 0;
  g.transposeOctave(Bass, +1);
  CHECK_EQ((int)g.info(Bass).edited, 1);
  CHECK(g.changeCount() > changes);
  for (int i = 0; i < src.count; i++) { int want = before.notes[i].pitch() + 12; if (want > 115) want = 115; CHECK_EQ((int)src.notes[i].pitch(), want); CHECK_EQ((int)src.notes[i].start(), (int)before.notes[i].start()); }
  bool swap = false; for (const Cmd& c : peekCmds()) if (c.type == CmdSwapPattern && c.track == Bass) { swap = true; CHECK_EQ((int)c.a, (int)g.info(Bass).variant); }
  CHECK(swap);
  for (int i = 0; i < e.nextPattern(Bass).count && i < src.count; i++) CHECK_EQ((int)e.nextPattern(Bass).notes[i].pitch(), (int)src.notes[i].pitch());   // variant 0 = the source
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.activePattern(Bass).notes[0].pitch(), (int)src.notes[0].pitch());
  g.transposeOctave(Bass, -1);
  bool same = src.count == before.count; for (int i = 0; same && i < src.count; i++) same = src.notes[i].packed == before.notes[i].packed || before.notes[i].pitch() > 103;
  CHECK(same);
  // clamps at 12 and 115
  src.notes[0].setPitch(110); g.transposeOctave(Bass, +1); CHECK_EQ((int)src.notes[0].pitch(), 115);
  src.notes[0].setPitch(20); g.transposeOctave(Bass, -1); CHECK_EQ((int)src.notes[0].pitch(), 12);
  src = before; g.info(Bass).edited = 0; g.rebuildVariant(Bass, 0);
  // drums and the vocal have no pitch to shift; dir 0 does nothing
  const Pattern beat = e.sourcePattern(Beat); g.transposeOctave(Beat, +1); CHECK(memcmp(&beat, &e.sourcePattern(Beat), sizeof(Pattern)) == 0);
  g.transposeOctave(Vocal, +1); g.transposeOctave(Chord, 0); g.transposeOctave(-1, 1); g.transposeOctave(kTracks, 1);
  runBlocks(kSampleRate * 3 / kBlock);
}

TEST(change_count_follows_session_relevant_changes) {
  Generator& g = gen();
  uint32_t c = g.changeCount();
  g.rebuildVariant(Chord, 1); CHECK(g.changeCount() > c); c = g.changeCount();
  g.setLengthStep(Chord, +1); CHECK(g.changeCount() > c); c = g.changeCount();
  g.setLengthStep(Chord, 0); CHECK_EQ(g.changeCount(), c);   // a query is not a change
  g.setLengthStep(Chord, -1);
  g.setKeyRoot(g.key().root); CHECK(g.changeCount() > c); c = g.changeCount();
  CHECK(g.generateSound(Pad)); runUntilIdle(); CHECK(g.changeCount() > c); c = g.changeCount();
  g.randomizeKnobs(Pad); CHECK(g.changeCount() > c);
  g.rebuildVariant(Chord, 0);
  runBlocks(kSampleRate * 3 / kBlock);
}

// SPEC §9.16 amendment: a '<n>bars' token in the file name sets the window so a rap file plays whole — within the engine's
// clip cap (kVocalWindowMaxBars): the pause steps down to fit when the file opens, len± refuses a step the cap would
// swallow, and a file longer than the cap keeps the default 2-bar window instead of being cut mid-verse.
TEST(bars_token_sets_the_vocal_window_of_a_stream) {
  Generator& g = gen(); Engine& e = eng(); const int t = kTracksPerSet + Vocal; const int maxW = Generator::kVocalWindowMaxBars;
  CHECK(sd().mkdir("/lp/_gen_scratch"));
  const char* rap = "/lp/_gen_scratch/120bpm__RAPM__4bars__verse.wav";
  const char* rap8 = "/lp/_gen_scratch/120bpm__RAPM__8bars__verse.wav";
  CHECK(writeWav(rap, 1, 16, 22050, 22050 * 8, true));        // 8 s = 4 bars at 120 bpm, a ramp signal
  CHECK(writeWav(rap8, 1, 16, 22050, 22050 * 16, true));      // 16 s = 8 bars
  const std::string keep = g.vocalPath(1, 0);
  const int opens = sd().openCount();
  TrackGenInfo& inf = g.info(t);
  inf.pause = 4;                                              // a pause the cap cannot hold under a 4-bar window (4 + 4 > 6 today)
  CHECK(g.loadVocal(1, rap, 0));
  CHECK_EQ((int)inf.windowBars, 4);
  const int fit = 4 + 4 <= maxW ? 4 : 2;
  CHECK_EQ((int)inf.pause, fit);                              // stepped down when the file opened: what the PAUSE page shows is what plays
  CHECK_EQ((int)g.setLengthStep(t, 0), (4 + fit) * kTicksPerBar);
  runBlocks(kSampleRate * 3 / kBlock);                        // the switch lands on a bar: the engine's clip is the cycle
  CHECK_EQ((int)e.status().track[t].lenTicks, (4 + fit) * kTicksPerBar);
  CHECK_EQ((int)g.vocalLoopTicks(1), (4 + fit) * kTicksPerBar);
  if (fit == 2) {
    CHECK_EQ((int)g.setLengthStep(t, +1), 6 * kTicksPerBar); CHECK_EQ((int)inf.pause, 2);   // refused: pause 4 would exceed the cap
    CHECK(!g.vocalSwitchPending(1));                                                       // nothing was republished
    CHECK_EQ((int)g.setLengthStep(t, -1), 5 * kTicksPerBar); CHECK_EQ((int)inf.pause, 1);
    CHECK_EQ((int)g.setLengthStep(t, +1), 6 * kTicksPerBar); CHECK_EQ((int)inf.pause, 2);
    runBlocks(kSampleRate * 3 / kBlock);
    CHECK_EQ((int)e.status().track[t].lenTicks, 6 * kTicksPerBar);
  } else std::printf("  (kMaxVocalClipTicks raised to %d bars: the 4-bar window keeps pause 4)\n", maxW);
  // the whole file streams: play it and compare the ring with the window reference at 4 bars
  push(CmdTrackToggle, t);
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  int compared = 0; const int bad = compareRing(1, 1500, &compared);
  std::printf("  4-bar window + %d-bar pause while playing: %d compared, %d mismatches\n", (int)inf.pause, compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  push(CmdTrackStopNow, t); runBlocks(4);
  // an 8-bar file: under today's cap the window falls back to 2 bars (never 6 bars cut mid-verse); a raised cap plays it whole
  inf.pause = kVocalPauseDefault;
  CHECK(g.loadVocal(1, rap8, 0));
  CHECK_EQ((int)inf.windowBars, 8 <= maxW ? 8 : kVocalWindowBars);
  CHECK_EQ((int)inf.pause, kVocalPauseDefault);
  CHECK_EQ((int)g.setLengthStep(t, 0), (inf.windowBars + 1) * kTicksPerBar);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)e.status().track[t].lenTicks, (inf.windowBars + 1) * kTicksPerBar);
  // back to the House phrase for the later tests (same stream slot, same open count): its window is its own '<n>bars'
  // token when it carries one within the cap (the card's OSH / RAPF phrases say 1bars), else the default
  inf.pause = kVocalPauseDefault;
  CHECK(g.loadVocal(1, keep.c_str(), 0));
  runBlocks(kSampleRate * 3 / kBlock);
  const int keepBars = Generator::parseBarsToken(keep.c_str() + keep.rfind('/') + 1);
  CHECK_EQ((int)inf.windowBars, keepBars > 0 && keepBars <= maxW ? keepBars : kVocalWindowBars);
  CHECK_EQ(sd().openCount(), opens);
  CHECK(sd().remove(rap)); CHECK(sd().remove(rap8)); CHECK(sd().remove("/lp/_gen_scratch"));
}

// SPEC §9.17b: nextPatch is the engine's until it copied it (two blocks after CmdSetPatch). An acid toggle right behind a
// sound job's CmdSetPatch (or a job right behind a toggle) waits for that window instead of tearing the patch.
TEST(acid_toggle_waits_for_a_patch_in_flight) {
  Generator& g = gen(); Engine& e = eng(); const int t = Lead;
  auto patches = [&]() { int n = 0; for (const Cmd& c : peekCmds()) if (c.type == CmdSetPatch && c.track == t) n++; return n; };
  runBlocks(4); CHECK(!g.acidEnabled(t)); CHECK_EQ(patches(), 0);
  CHECK(g.generateSound(t)); g.step();                        // the job rolls a library preset and pushes its CmdSetPatch; no block rendered yet
  CHECK(!g.busy()); CHECK_EQ(patches(), 1);
  const std::string jobSound = e.nextPatch(t).name; CHECK(strncmp(jobSound.c_str(), "303", 3) != 0);
  g.setAcid(t, true);                                         // toggled while the engine may still be copying nextPatch: the roll is deferred
  CHECK(g.acidEnabled(t));
  CHECK_EQ(patches(), 1); CHECK(jobSound == e.nextPatch(t).name); CHECK_EQ(knobCmds(peekCmds(), t).n, 0);
  g.step(); CHECK_EQ(patches(), 1); CHECK(jobSound == e.nextPatch(t).name);   // still deferred: no block has passed
  runBlocks(2);                                               // the engine drained the command and rendered two blocks
  g.step();                                                   // retryDue performs the roll now
  { std::vector<Cmd> c = peekCmds(); int n = 0; for (const Cmd& k : c) if (k.type == CmdSetPatch && k.track == t) n++; CHECK_EQ(n, 1); CHECK_EQ(knobCmds(c, t).n, Generator::knobCmdCount(Lead)); }
  CHECK(strncmp(e.nextPatch(t).name, "303", 3) == 0); CHECK(isAcidPreset(Lead, g.info(t).presetIndex)); CHECK(strncmp(g.info(t).sound, "303", 3) == 0);
  runBlocks(2);
  // the other way round: a sound job right behind a toggle waits in its preset step until the toggle's patch was copied
  g.setAcid(t, false);                                        // free: pushed at once
  CHECK_EQ(patches(), 1); CHECK(strncmp(e.nextPatch(t).name, "303", 3) != 0);
  const std::string toggled = e.nextPatch(t).name;
  CHECK(g.generateSound(t)); g.step();
  CHECK(g.busy()); CHECK_EQ(patches(), 1); CHECK(toggled == e.nextPatch(t).name);   // waiting, nextPatch untouched
  runBlocks(2); g.step();
  CHECK(!g.busy()); CHECK_EQ(patches(), 1); CHECK(!isAcidPreset(Lead, g.info(t).presetIndex)); CHECK(g.lastError()[0] == 0);
  runBlocks(kSampleRate / kBlock);
}

// SPEC §9.17b / §9.10: a pattern job on an Fx track whose loop is kept regenerates nothing, so it resets nothing (the
// user's strip and knobs on the kept loop survive); a sound job on it still resets right before its new sound.
TEST(kept_fx_loop_under_a_pattern_job_is_not_reset) {
  Generator& g = gen(); Engine& e = eng(); const int t = Fx;
  CHECK(e.sourcePattern(t).count > 0);
  g.info(t).userLoop = 1;
  push(CmdSetParam, t, PVolume, 0, 0.3f); push(CmdSetParam, t, PReverb, 0, 0.6f); runBlocks(2);
  static int resets, resetBeforePatch; static bool seenReset; resets = 0; resetBeforePatch = 0; seenReset = false;
  g_afterPump = [] {
    for (const Cmd& c : peekCmds()) {
      if (c.type == CmdResetTrack && c.track == Fx) { resets++; seenReset = true; }
      if (c.type == CmdSetPatch && c.track == Fx && seenReset) resetBeforePatch++;
    }
  };
  CHECK(g.generatePattern(t)); runUntilIdle();
  CHECK_EQ(resets, 0); CHECK(strcmp(g.lastNote(), "FX kept") == 0); CHECK_EQ((int)g.info(t).userLoop, 1);
  runBlocks(2);
  CHECK_NEAR(e.params(t).volume, 0.3f, 1e-5); CHECK_NEAR(e.params(t).reverb, 0.6f, 1e-5);   // untouched (the engine applies CmdSetParam)
  CHECK(g.generateSound(t)); runUntilIdle();
  g_afterPump = nullptr;
  CHECK(resets >= 1); CHECK(resetBeforePatch >= 1);           // the sound part resets, right before its CmdSetPatch
  if (engineAppliesReset()) CHECK_NEAR(e.params(t).volume, 0.8f, 1e-5);
  g.info(t).userLoop = 0;
  push(CmdSetParam, t, PVolume, 0, 0.8f); push(CmdSetParam, t, PReverb, 0, 0.0f); runBlocks(2);
}

// ---------------------------------------------------------------------------------------------- loop mode (SPEC §9.19)

// Reference producer for a streamed loop (generator.cpp produce, mode 1): cycle = loopTicks placed by the bar phase
// (whole bars) or repeating inside the bar (shorter); content = the first regionTicks of the cycle at file position
// pos × rate (+ the continuity offset of cycle offCycle), silence beyond the crop with a 64-sample fade-out where the
// crop ends inside the cycle, and the loop variants' wrap crossfade (tail + (head − tail) × pos / 64, tail = the file
// past the cycle end) at every wrap after the first pass from `start` when the crop fills the cycle AND the file holds a
// full crossfade of content past it. A whole loop file (exactly n bars, docs/LOOPS.md) has none: its head plays straight
// at every repeat, as on the first pass (continuity: the downbeat of pass two is the downbeat of pass one). `wrapped`
// reports the samples inside a repeat's first kXfade (crossfaded or straight) so a test can prove it compared a wrap.
static int16_t loopReference(const std::vector<int16_t>& file, const Generator::BarClock& c, uint32_t start, uint16_t loopTicks, uint16_t regionTicks,
                             float fileBpm, uint32_t S, int64_t offFp, uint32_t offCycle, bool* xfade, bool* wrapped = nullptr) {
  const uint32_t stepFp = stepFpRef(c.bpm, fileBpm);
  uint32_t idx, into, barEnd; Generator::barPhase(c, S, idx, into, barEnd);
  const bool whole = loopTicks >= kTicksPerBar;
  const uint32_t C = whole ? loopTicks / kTicksPerBar : 1;
  uint32_t L = whole ? C * c.barLen : loopSamplesOf(loopTicks, c.bpm); if (!L) L = 1;
  uint32_t R = loopSamplesOf(regionTicks, c.bpm); if (R > L) R = L;
  const bool full = R == L;
  const uint32_t pos = whole ? (idx % C) * c.barLen + into : into % L;
  const uint32_t cyc = whole ? idx / C : idx;
  if (xfade) *xfade = false;
  if (wrapped) *wrapped = false;
  if (pos >= R) return 0;
  int64_t fp = (int64_t)pos * stepFp; if (cyc == offCycle) fp += offFp; if (fp < 0) fp = 0;
  const uint32_t i = (uint32_t)(fp >> 16), fr = (uint32_t)(fp & 0xFFFF);
  const int32_t rel = (int32_t)(S - start);
  int32_t out;
  const bool atWrap = rel >= (int32_t)L && pos < (uint32_t)Generator::kXfade;
  if (wrapped) *wrapped = atWrap;
  if (atWrap && full && tailContent(file.size(), L, stepFp)) {
    const uint64_t tFp = (uint64_t)(L + pos) * stepFp; const int32_t tail = lerpRef(file, (uint32_t)(tFp >> 16), (uint32_t)(tFp & 0xFFFF));
    int32_t head = 0;
    if (i + 1 < (uint32_t)Generator::kHeadSamples) head = lerpRef(file, i, fr);
    else if (i < (uint32_t)Generator::kHeadSamples) head = i < file.size() ? file[i] : 0;
    out = tail + ((head - tail) * (int32_t)pos) / Generator::kXfade;
    if (xfade) *xfade = true;
  } else out = lerpRef(file, i, fr);
  if (!full) { const uint32_t left = R - pos; if (left <= (uint32_t)Generator::kXfade) { out = out * (int32_t)left / Generator::kXfade; if (xfade) *xfade = true; } }
  return (int16_t)out;
}
// The ring of stream `stream` (a slot: kSets + k; the Vocal track's loop: its set) against loopReference for `n`
// samples from `from` (the engine clock when 0): -1 without a loop file, else the mismatches; compared / xfaded /
// wrapped (samples inside a repeat's first kXfade) report.
static int compareLoopRing(int stream, uint32_t from, int n, int* compared, int* xfaded = nullptr, int* wrapped = nullptr) {
  const bool slot = stream >= kSets;
  const int track = slot ? -1 : stream * kTracksPerSet + Vocal;
  int t = track;
  if (slot) for (int i = 0; i < kTracks && t < 0; i++) if (gen().loopSlot(i) == stream - kSets) t = i;
  if (t < 0) return -1;
  WavInfo wi; std::vector<int16_t> file = readPhrase(gen().loopPath(t), wi);
  if (file.empty()) return -1;
  VocalRing& r = eng().streamRing(stream);
  const uint32_t base = r.baseSample.load(), w = r.w.load();
  const uint16_t loop = slot ? gen().loopClipTicks(stream - kSets) : gen().vocalLoopTicks(stream);
  const uint16_t region = slot ? gen().loopRegionTicks(stream - kSets) : gen().vocalRegionTicks(stream);
  const uint32_t start = slot ? gen().loopStartSample(stream - kSets) : gen().vocalStartSample(stream);
  int bpm = Generator::parseBpmToken(gen().loopFile(t)); if (!bpm) bpm = kLoopBpm;
  uint32_t offCycle = 0; const int64_t offFp = slot ? gen().loopFileOffset(stream - kSets, &offCycle) : gen().vocalFileOffset(stream, &offCycle);
  const Generator::BarClock c = clockNow();
  if (!from) from = eng().status().sampleClock;
  int bad = 0; *compared = 0; if (xfaded) *xfaded = 0; if (wrapped) *wrapped = 0;
  for (int k = 0; k < n; k++) {
    const uint32_t S = from + (uint32_t)k;
    if ((int32_t)(S - base) < 0 || (int32_t)(S - (base + w)) >= 0) break;
    bool xf = false, wr = false;
    const int16_t want = loopReference(file, c, start, loop, region, (float)bpm, S, offFp, offCycle, &xf, &wr);
    (*compared)++; if (xf && xfaded) (*xfaded)++; if (wr && wrapped) (*wrapped)++;
    if (r.buf[r.wrap(S - base)] != want) bad++;
  }
  return bad;
}
// The next cycle start of a slot's streamed loop (a bar line whose global index is a multiple of the cycle's bars) that is
// a repeat (at least one pass after the stream's start) and at least `margin` samples ahead of the clock.
static uint32_t nextLoopCycleStart(int slot, int margin) {
  const Generator::BarClock c = clockNow();
  const uint16_t clip = gen().loopClipTicks(slot); const uint32_t C = clip >= kTicksPerBar ? clip / kTicksPerBar : 1;
  const uint32_t start = gen().loopStartSample(slot), L = C * c.barLen;
  const uint32_t from = eng().status().sampleClock + (uint32_t)margin;
  uint32_t S0 = c.bar0 - (c.idx0 % C) * c.barLen;
  while ((int32_t)(S0 - from) < 0 || (int32_t)(S0 - start) < (int32_t)L) S0 += L;
  return S0;
}
static const char* baseName(const char* path) { const char* b = path; for (const char* q = path; *q; q++) if (*q == '/') b = q + 1; return b; }
static int32_t streamAhead(int stream) { VocalRing& r = eng().streamRing(stream); return (int32_t)(r.baseSample.load() + r.w.load() - eng().status().sampleClock); }
static bool inFolder(const char* path, const std::string& folder) { return std::string(path).compare(0, folder.size(), folder) == 0; }
static void stopAll() { for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t); runBlocks(2); }
static bool startTrack(int t) { push(CmdTrackToggle, t); for (int i = 0; i < 400 && eng().status().track[t].state != PSPlaying; i++) iterate(); return eng().status().track[t].state == PSPlaying; }
// Recorded commands of a kind for a track since the recorder was armed (g_afterPump peeks the queue every iteration).
struct CmdLog { std::vector<Cmd> cmds; };
static CmdLog g_cmdLog;
static void recordCmds() { for (const Cmd& c : peekCmds()) g_cmdLog.cmds.push_back(c); }
static int countCmd(CmdType t, int track = -1, int a = -1) { int n = 0; for (const Cmd& c : g_cmdLog.cmds) if (c.type == t && (track < 0 || c.track == track) && (a < 0 || c.a == a)) n++; return n; }
static const Cmd* findCmd(CmdType t, int track) { for (const Cmd& c : g_cmdLog.cmds) if (c.type == t && c.track == track) return &c; return nullptr; }
// Loop mode off and the slot stream gone (its file closed once the engine passed the bar).
static void loopOffAndSettle(int t) { gen().setLoopMode(t, false); runBlocks(kSampleRate * 2 / kBlock); runUntilIdle(); }

TEST(loop_helpers_folder_region_and_len) {
  CHECK(strcmp(Generator::loopFolder(Beat), "drum") == 0); CHECK(strcmp(Generator::loopFolder(Percussion), "percussion") == 0);
  CHECK(strcmp(Generator::loopFolder(Bass), "bass") == 0); CHECK(strcmp(Generator::loopFolder(Chord), "chord") == 0);
  CHECK(strcmp(Generator::loopFolder(Lead), "lead") == 0); CHECK(strcmp(Generator::loopFolder(Pad), "pad") == 0);
  CHECK(strcmp(Generator::loopFolder(Fx), "fx") == 0); CHECK(strcmp(Generator::loopFolder(Vocal), "vocal") == 0);
  // crops: 1 whole, 2 first half, 3 first 2 beats, 4 first beat (SPEC §9.19 §1.4)
  CHECK_EQ((int)Generator::loopRegionOf(2, 0), 2 * kTicksPerBar); CHECK_EQ((int)Generator::loopRegionOf(2, 1), kTicksPerBar);
  CHECK_EQ((int)Generator::loopRegionOf(2, 2), 2 * kTicksPerBeat); CHECK_EQ((int)Generator::loopRegionOf(2, 3), kTicksPerBeat);
  CHECK_EQ((int)Generator::loopRegionOf(1, 1), 2 * kTicksPerBeat); CHECK_EQ((int)Generator::loopRegionOf(8, 0), kMaxLoopTicks);
  CHECK_EQ((int)Generator::loopRegionOf(9, 0), kMaxLoopTicks);     // never beyond kMaxLoopBars
  // len = natural × 2^k in [1 beat, kMaxLoopTicks], k clamped so the clamp never hides a step
  CHECK_EQ(Generator::clampLoopStep(2 * kTicksPerBar, 2), 2); CHECK_EQ((int)Generator::loopLenOf(2 * kTicksPerBar, 2), kMaxLoopTicks);
  CHECK_EQ(Generator::clampLoopStep(2 * kTicksPerBar, 3), 2); CHECK_EQ(Generator::clampLoopStep(kMaxLoopTicks, 1), 0);
  CHECK_EQ(Generator::clampLoopStep(kTicksPerBeat, -1), 0); CHECK_EQ((int)Generator::loopLenOf(kTicksPerBeat, -3), kTicksPerBeat);
  CHECK_EQ(Generator::clampLoopStep(4 * kTicksPerBar, -2), -2); CHECK_EQ((int)Generator::loopLenOf(4 * kTicksPerBar, -2), kTicksPerBar);
  CHECK_EQ(Generator::clampLoopStep(4 * kTicksPerBar, -9), -4);   // down to 1 beat
  // every loop on the card parses: '<n>bars' with n in {1, 2, 4, 8}, the 125bpm token (docs/LOOPS.md)
  NameFn cats; sd().walk("/lp/packs/House/loops", collectDirs, &cats);
  int files = 0;
  for (const std::string& cat : cats.names) {
    NameFn keys; sd().walk(("/lp/packs/House/loops/" + cat).c_str(), collectDirs, &keys);
    for (const std::string& k : keys.names) {
      NameFn fl; sd().walk(("/lp/packs/House/loops/" + cat + "/" + k).c_str(), collect, &fl);
      for (const std::string& n : fl.names) {
        if (n == "_index.txt") continue;
        const int bars = Generator::parseBarsToken(n.c_str());
        CHECK(bars == 1 || bars == 2 || bars == 4 || bars == 8); CHECK_EQ(Generator::parseBpmToken(n.c_str()), kLoopBpm); files++;
      }
    }
  }
  std::printf("  %d loops on the card parse their bars and bpm tokens\n", files);
  CHECK(files >= 200);
}

TEST(loop_mode_on_beat_picks_a_drum_loop_and_streams_it) {
  Generator& g = gen(); Engine& e = eng(); const int t = Beat;
  stopAll();
  CHECK(startTrack(t));
  const int opens = sd().openCount();
  const uint8_t parkedVariant = g.info(t).variant; const int8_t parkedLen = g.info(t).lenStep;
  const std::string parkedFile = g.info(t).file;
  CHECK(!g.loopEnabled(t)); CHECK(!g.anyLoop()); CHECK_EQ(g.loopSlot(t), -1); CHECK(g.loopFile(t)[0] == 0);
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  CHECK(g.setLoopMode(t, true));
  CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopOk);
  CHECK(g.loopEnabled(t)); CHECK(g.anyLoop()); CHECK(g.busy());
  const int slot = g.loopSlot(t); CHECK(slot >= 0 && slot < kLoopSlots);
  CHECK(g.loopFile(t)[0] == 0);                                // nothing picked yet: the square blinks
  CHECK_EQ((int)g.info(t).variant, 0);                         // the loop starts whole (the editor's VARIANT 1)
  const int steps = runUntilIdle();
  std::printf("  loop picked in %d steps: %s (%d bars, slot %d), lastError '%s'\n", steps, g.loopFile(t), (int)g.loopBars(t), slot, g.lastError());
  CHECK(!g.busy()); CHECK(g.loopEnabled(t)); CHECK(g.lastError()[0] == 0);
  CHECK(g.loopFile(t)[0] != 0); CHECK(inFolder(g.loopPath(t), "/lp/packs/House/loops/drum/")); CHECK(sd().exists(g.loopPath(t)));
  CHECK(strcmp(baseName(g.loopPath(t)), g.loopFile(t)) == 0);
  const int bars = g.loopBars(t); CHECK(bars == 1 || bars == 2 || bars == 4 || bars == 8);
  CHECK_EQ((int)g.info(t).naturalTicks, bars * kTicksPerBar); CHECK_EQ((int)g.info(t).lenStep, 0);
  CHECK_EQ((int)g.setLengthStep(t, 0), bars * kTicksPerBar);
  CHECK(g.info(t).file == parkedFile);                        // the parked pattern's file is untouched
  runBlocks(kSampleRate * 3 / kBlock);                        // the switch landed on a bar
  g_afterPump = nullptr;
  // the switch commands: CmdLoopLoaded{slot}, CmdLoopMode{track, on, stream}, the carrier swap {variant 0, len bars × 384}
  CHECK(countCmd(CmdLoopLoaded, -1, slot) >= 1);
  const Cmd* mode = findCmd(CmdLoopMode, t); CHECK(mode != nullptr); if (mode) { CHECK_EQ((int)mode->a, 1); CHECK_EQ((int)mode->i, kSets + slot); }
  const Cmd* swap = findCmd(CmdSwapPattern, t); CHECK(swap != nullptr); if (swap) { CHECK_EQ((int)swap->a, 0); CHECK_EQ((int)swap->i, bars * kTicksPerBar); }
  CHECK_EQ(countCmd(CmdResetTrack, t), 0);                     // nothing of the parked sound is regenerated: no reset
  CHECK(e.nextLoop(slot).present == 1); CHECK_EQ((int)e.nextLoop(slot).loopTicks, bars * kTicksPerBar); CHECK_EQ((int)e.nextLoop(slot).fileBpm, kLoopBpm);
  CHECK(!g.loopSwitchPending(slot)); CHECK_EQ((int)g.loopClipTicks(slot), bars * kTicksPerBar); CHECK_EQ((int)g.loopRegionTicks(slot), bars * kTicksPerBar);
  // the engine streams it: bound at the bar, the clip length is the loop's, the ring stays ahead, no underruns
  CHECK_EQ(e.trackStream(t), kSets + slot); CHECK(e.trackStreams(t)); CHECK_EQ(e.loopTrack(slot), t);
  CHECK_EQ((int)e.status().track[t].lenTicks, bars * kTicksPerBar); CHECK_EQ((int)e.status().track[t].variant, 0);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  const uint16_t under0 = e.streamUnderruns(kSets + slot);
  static int minAhead; minAhead = 1 << 30;
  g_afterPump = [] { for (int i = 0; i < kTracks; i++) if (gen().loopSlot(i) >= 0) { int a = streamAhead(kSets + gen().loopSlot(i)); if (a < minAhead) minAhead = a; } };
  runBlocks(kSampleRate * 3 / kBlock);
  g_afterPump = nullptr;
  std::printf("  3 s streaming: min lead %d samples (ring %d), underruns %u, peak %.0f\n", minAhead, e.loopRing(slot).cap, (unsigned)(e.streamUnderruns(kSets + slot) - under0), g_peak);
  CHECK(minAhead >= kLoopChunkSamples - kBlock);              // refilled whenever kLoopChunkSamples are free (SPEC §9.19 §2)
  CHECK(minAhead <= e.loopRing(slot).cap);
  CHECK_EQ((int)(e.streamUnderruns(kSets + slot) - under0), 0);
  CHECK(g.loopSlotStreaming());                                // a slot ring is consumed: Sets keeps the session write away (SPEC §9.19 §3)
  int compared = 0, xf = 0; const int bad = compareLoopRing(kSets + slot, 0, 1500, &compared, &xf);
  std::printf("  ring vs reference: %d compared (%d crossfading), %d mismatches\n", compared, xf, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  CHECK_EQ(sd().openCount(), opens + 1);                      // one loop file open
  CHECK(g_peak > 100);                                         // the loop is audible through the Beat's strip
  (void)parkedVariant; (void)parkedLen;
}

TEST(loop_variants_crop_and_len_steps_switch_the_stream) {
  Generator& g = gen(); Engine& e = eng(); const int t = Beat;
  CHECK(g.loopEnabled(t)); const int slot = g.loopSlot(t); CHECK(slot >= 0);
  const int bars = g.loopBars(t); const uint16_t under0 = e.streamUnderruns(kSets + slot);
  auto settled = [&](int clip, int region) {                   // the same-file switch lands on a 1/16 and the stream follows it
    for (int i = 0; i < 400 && (g.loopSwitchPending(slot) || g.loopClipTicks(slot) != clip); i++) iterate();
    CHECK(!g.loopSwitchPending(slot)); CHECK_EQ((int)g.loopClipTicks(slot), clip); CHECK_EQ((int)g.loopRegionTicks(slot), region);
    runBlocks(kSampleRate / kBlock);
    int compared = 0, xf = 0; const int bad = compareLoopRing(kSets + slot, 0, 1500, &compared, &xf);
    std::printf("  clip %d region %d: %d compared (%d fading), %d mismatches\n", clip, region, compared, xf, bad);
    CHECK(compared > 1000); CHECK_EQ(bad, 0);
  };
  // variant 4 = the first beat looped (the rush), k reset
  g.rebuildVariant(t, 3);
  CHECK_EQ((int)g.info(t).variant, 3); CHECK_EQ((int)g.info(t).naturalTicks, kTicksPerBeat); CHECK_EQ((int)g.info(t).lenStep, 0);
  CHECK_EQ((int)g.setLengthStep(t, 0), kTicksPerBeat);
  settled(kTicksPerBeat, kTicksPerBeat);
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)e.status().track[t].variant, 3); CHECK_EQ((int)e.status().track[t].lenTicks, kTicksPerBeat);   // the carrier swap at the bar
  // len+ : 2 beats of clip, the first beat of content then silence (SPEC §9.4 silence beyond the pattern)
  CHECK_EQ((int)g.setLengthStep(t, +1), 2 * kTicksPerBeat); CHECK_EQ((int)g.info(t).lenStep, 1);
  settled(2 * kTicksPerBeat, kTicksPerBeat);
  CHECK_EQ((int)g.setLengthStep(t, +1), kTicksPerBar); CHECK_EQ((int)g.setLengthStep(t, +1), 2 * kTicksPerBar);
  settled(2 * kTicksPerBar, kTicksPerBeat);
  // len- back below a beat is refused (the clip never shrinks under 1 beat)
  g.rebuildVariant(t, 3); settled(kTicksPerBeat, kTicksPerBeat);
  CHECK_EQ((int)g.setLengthStep(t, -1), kTicksPerBeat); CHECK_EQ((int)g.info(t).lenStep, 0);
  // variant 2 = the first half, variant 1 = whole, len- on the whole loop crops it to half with the wrap crossfade
  g.rebuildVariant(t, 1); CHECK_EQ((int)g.info(t).naturalTicks, bars * kTicksPerBar / 2);
  settled(bars * kTicksPerBar / 2, bars * kTicksPerBar / 2);
  g.rebuildVariant(t, 0); settled(bars * kTicksPerBar, bars * kTicksPerBar);
  if (bars >= 2) { CHECK_EQ((int)g.setLengthStep(t, -1), bars * kTicksPerBar / 2); settled(bars * kTicksPerBar / 2, bars * kTicksPerBar); }
  // the longest clip is kMaxLoopTicks whatever the loop
  int len = g.setLengthStep(t, 0); for (int i = 0; i < 6; i++) len = g.setLengthStep(t, +1);
  CHECK_EQ(len, kMaxLoopTicks); settled(kMaxLoopTicks, bars * kTicksPerBar);
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)e.status().track[t].lenTicks, kMaxLoopTicks);   // the engine accepts a streaming track's clip up to kMaxLoopTicks
  g.rebuildVariant(t, 0); settled(bars * kTicksPerBar, bars * kTicksPerBar);
  CHECK_EQ((int)(e.streamUnderruns(kSets + slot) - under0), 0);
}

// SPEC §9.19 review: a whole loop file is exactly its cycle long (n × 42 336 frames, docs/LOOPS.md), so the wrap crossfade
// of the vocal loop variants would read its "tail" past the file's end — silence — and fade the downbeat of every repeat in
// from nothing. The head plays straight instead: the ring across a cycle wrap (the bar line − 64 .. + 64) matches the
// reference that models continuity and the repeat's first sample is the file's first sample. A loop whose file is longer
// than its '<n>bars' token (content past the cycle end) keeps the crossfade: xfaded > 0 over the same window.
TEST(loop_wrap_plays_a_whole_file_straight_and_crossfades_only_with_tail_content) {
  Generator& g = gen(); Engine& e = eng(); const int t = Beat;
  CHECK(g.loopEnabled(t)); const int slot = g.loopSlot(t); CHECK(slot >= 0);
  CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)g.loopRegionTicks(slot), (int)g.loopClipTicks(slot));   // the whole loop is the cycle
  WavInfo wi; const std::vector<int16_t> file = readPhrase(g.loopPath(t), wi);
  CHECK_EQ((int)file.size(), (int)g.loopBars(t) * 42336);          // exactly n bars at 125 bpm: nothing past the cycle end
  const uint32_t S0 = nextLoopCycleStart(slot, 4 * kBlock);         // a repeat's first sample (the stream passed its first pass long ago)
  CHECK(approach(S0, 4 * kBlock, 4000));                            // the ring holds the wrap: its lead is ≥ kLoopChunkSamples − kBlock
  int compared = 0, xf = 0, wrapped = 0;
  const int bad = compareLoopRing(kSets + slot, S0 - Generator::kXfade, 2 * Generator::kXfade, &compared, &xf, &wrapped);
  VocalRing& r = e.streamRing(kSets + slot);
  const int16_t atWrap = r.buf[r.wrap(S0 - r.baseSample.load())];
  std::printf("  whole %d-bar loop, wrap at %u: %d compared (%d in the repeat's first 64, %d crossfading), %d mismatches; repeat[0] %d, file[0] %d\n",
              (int)g.loopBars(t), S0, compared, wrapped, xf, bad, (int)atWrap, (int)file[0]);
  CHECK_EQ(compared, 2 * Generator::kXfade); CHECK_EQ(wrapped, Generator::kXfade); CHECK_EQ(xf, 0); CHECK_EQ(bad, 0);
  CHECK_EQ((int)atWrap, (int)file[0]);                                // the downbeat of the repeat equals the first pass, no fade-in
  // a loop longer than its bars token: a 1-bar cycle over 1.5 bars of content → the crossfade has a tail to read
  CHECK(sd().mkdir("/lp/state"));
  const char* longLoop = "/lp/state/gen_test__125bpm__LOOP__1bars__longer_than_its_cycle.wav";
  CHECK(writeWav(longLoop, 1, 16, 22050, 42336 + 42336 / 2, true));
  const int ft = Fx; CHECK(!g.loopEnabled(ft));
  CHECK(g.loadLoop(ft, longLoop, 0, 0)); const int fslot = g.loopSlot(ft); CHECK(fslot >= 0 && fslot != slot);
  for (int i = 0; i < 600 && (g.loopSwitchPending(fslot) || !e.trackStreams(ft)); i++) iterate();   // bound at the bar (the track is stopped)
  CHECK(!g.loopSwitchPending(fslot)); CHECK(e.trackStreams(ft)); CHECK_EQ((int)g.loopClipTicks(fslot), kTicksPerBar); CHECK_EQ((int)g.loopBars(ft), 1);
  CHECK(startTrack(ft));
  runBlocks(kSampleRate * 2 / kBlock);
  const uint32_t S1 = nextLoopCycleStart(fslot, 4 * kBlock);
  CHECK(approach(S1, 4 * kBlock, 4000));
  const int bad1 = compareLoopRing(kSets + fslot, S1 - Generator::kXfade, 2 * Generator::kXfade, &compared, &xf, &wrapped);
  std::printf("  1-bar cycle of a 1.5-bar file, wrap at %u: %d compared, %d crossfading, %d mismatches\n", S1, compared, xf, bad1);
  CHECK_EQ(compared, 2 * Generator::kXfade); CHECK_EQ(xf, Generator::kXfade); CHECK_EQ(bad1, 0);
  push(CmdTrackStopNow, ft); runBlocks(2);
  loopOffAndSettle(ft);
  CHECK(sd().remove(longLoop));
  CHECK(g.loopEnabled(t));                                           // the Beat loop is untouched for the tests that follow
}

// SPEC §9.19 §1.4 as amended by §9.22: snare rush on a loop Beat track = HALF of the clip length set, momentary. rushLoop(on)
// streams a crop of len / 2 (at least 1 beat) from the next 1/16, rushLoop(off) the stored crop and len again; info().variant /
// lenStep never move, so a len± made before the rush survives it (rebuildVariant(3) + rebuildVariant(prev) would reset it)
// and the rush halves THAT length. Nothing of it reaches the session.
TEST(rush_on_a_loop_track_is_momentary_and_keeps_the_len_step) {
  Generator& g = gen(); Engine& e = eng(); const int t = Beat;
  CHECK(g.loopEnabled(t)); const int slot = g.loopSlot(t); CHECK(slot >= 0);
  const int bars = g.loopBars(t); const uint16_t nat = (uint16_t)(bars * kTicksPerBar); const uint16_t under0 = e.streamUnderruns(kSets + slot);
  auto settled = [&](int clip, int region) {
    for (int i = 0; i < 400 && (g.loopSwitchPending(slot) || g.loopClipTicks(slot) != clip); i++) iterate();
    CHECK(!g.loopSwitchPending(slot)); CHECK_EQ((int)g.loopClipTicks(slot), clip); CHECK_EQ((int)g.loopRegionTicks(slot), region);
    runBlocks(kSampleRate / kBlock);
    int compared = 0; const int bad = compareLoopRing(kSets + slot, 0, 1500, &compared);
    std::printf("  clip %d region %d: %d compared, %d mismatches\n", clip, region, compared, bad);
    CHECK(compared > 1000); CHECK_EQ(bad, 0);
  };
  CHECK(!g.loopRushing(t)); CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)g.info(t).lenStep, 0);
  const int k1 = Generator::clampLoopStep(nat, 1); const int len1 = Generator::loopLenOf(nat, k1);   // len+ (k 1 unless the loop is already 8 bars)
  CHECK_EQ((int)g.setLengthStep(t, +1), len1); CHECK_EQ((int)g.info(t).lenStep, k1);
  settled(len1, nat);
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  g.rushLoop(t, true); CHECK(g.loopRushing(t));
  CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)g.info(t).lenStep, k1);   // nothing the UI shows moves
  int half = len1 / 2; if (half < kTicksPerBeat) half = kTicksPerBeat; if (half > nat) half = nat;   // half of the clip length in force (len+ applied), never past the file
  CHECK(half > kTicksPerBeat);                                                  // this loop is longer than 2 beats: the rush is not the old 1-beat crop
  settled(half, half);                                                          // the first half of the clip, looped, from the next 1/16
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)e.status().track[t].lenTicks, half); CHECK_EQ((int)e.status().track[t].variant, 0);   // the carrier swap: the rush's clip under the stored variant
  CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)g.info(t).lenStep, k1); CHECK_EQ((int)g.setLengthStep(t, 0), len1);
  const uint32_t changes = g.changeCount();
  g.rushLoop(t, false); CHECK(!g.loopRushing(t));
  CHECK_EQ(g.changeCount(), changes);                                          // momentary: nothing for the session writer
  settled(len1, nat);                                                          // back to the stored crop and len
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)e.status().track[t].lenTicks, len1);
  g_afterPump = nullptr;
  CHECK_EQ(countCmd(CmdSnareRush), 0);                                         // the stream rushes; the engine's pattern rush is never asked
  CHECK_EQ((int)(e.streamUnderruns(kSets + slot) - under0), 0);
  g.rushLoop(Chord, true); CHECK(!g.loopRushing(Chord));                       // not a loop track: nothing (the UI sends CmdSnareRush there)
  g.rushLoop(t, true); CHECK(g.loopRushing(t)); g.rushLoop(t, true);           // idempotent
  CHECK_EQ((int)g.setLengthStep(t, -1), (int)nat); CHECK_EQ((int)g.info(t).lenStep, 0);   // len− during a rush: stored, applied when the rush ends
  g.rushLoop(t, false); settled(nat, nat);
  // the rush follows the length set: at the natural length it is half the loop, at len− half of that, at the 1-beat crop it stays 1 beat
  g.rushLoop(t, true); settled(nat / 2 < kTicksPerBeat ? kTicksPerBeat : nat / 2, nat / 2 < kTicksPerBeat ? kTicksPerBeat : nat / 2);
  g.rushLoop(t, false); settled(nat, nat);
  const int km = Generator::clampLoopStep(nat, -1); const int lenm = Generator::loopLenOf(nat, km);
  CHECK_EQ((int)g.setLengthStep(t, -1), lenm); settled(lenm, nat);
  g.rushLoop(t, true); { int h = lenm / 2; if (h < kTicksPerBeat) h = kTicksPerBeat; settled(h, h); }
  g.rushLoop(t, false); settled(lenm, nat);
  CHECK_EQ((int)g.setLengthStep(t, +1), (int)nat); settled(nat, nat); CHECK_EQ((int)g.info(t).lenStep, 0);
  g.rebuildVariant(t, 3); settled(kTicksPerBeat, kTicksPerBeat);                // variant 4 = the 1-beat crop
  g.rushLoop(t, true); settled(kTicksPerBeat, kTicksPerBeat);                   // half a beat is below the 1-beat floor: unchanged
  g.rushLoop(t, false); settled(kTicksPerBeat, kTicksPerBeat);
  g.rebuildVariant(t, 0); settled(nat, nat);                                    // back to the whole loop for the tests that follow
  CHECK_EQ((int)(e.streamUnderruns(kSets + slot) - under0), 0);
}

TEST(loop_mode_off_returns_the_parked_pattern_and_frees_the_slot) {
  Generator& g = gen(); Engine& e = eng(); const int t = Beat;
  CHECK(g.loopEnabled(t)); const int slot = g.loopSlot(t); CHECK(slot >= 0);
  const int opens = sd().openCount();
  // the parked pattern: what the track showed before loop mode (variant 0 = Kick, k 0, its natural length)
  const Pattern src = e.sourcePattern(t); const std::string file = g.info(t).file;
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  CHECK(g.setLoopMode(t, false));
  CHECK(!g.loopEnabled(t)); CHECK(!g.anyLoop()); CHECK_EQ(g.loopSlot(t), -1); CHECK(g.loopFile(t)[0] == 0);
  CHECK_EQ((int)g.info(t).variant, 0); CHECK_EQ((int)g.info(t).lenStep, 0); CHECK_EQ((int)g.info(t).naturalTicks, (int)src.lengthTicks);
  CHECK(g.info(t).file == file);
  recordCmds();                                                // the off command is in the queue right away (no card work)
  const Cmd* mode = findCmd(CmdLoopMode, t); CHECK(mode != nullptr); if (mode) CHECK_EQ((int)mode->a, 0);
  CHECK_EQ(sd().openCount(), opens);                           // the stream keeps its file until the engine passed the bar
  runBlocks(kSampleRate * 3 / kBlock);
  g_afterPump = nullptr;
  CHECK_EQ(e.trackStream(t), -1); CHECK_EQ(e.loopTrack(slot), -1);
  CHECK_EQ((int)e.status().track[t].lenTicks, (int)src.lengthTicks);   // the engine brought the parked clip back
  CHECK_EQ((int)e.status().track[t].variant, 0);
  CHECK_EQ(sd().openCount(), opens - 1);                       // the slot stream ended: its file closed
  CHECK(e.nextLoop(slot).present == 0);
  CHECK(!g.loopSlotStreaming());                               // no slot consumed any more: the session writer may run
  CHECK(memcmp(&src, &e.sourcePattern(t), sizeof(Pattern)) == 0);   // source untouched throughout
  // on again later takes the freed slot
  CHECK(g.setLoopMode(t, true)); CHECK_EQ(g.loopSlot(t), slot); runUntilIdle(); CHECK(g.loopEnabled(t));
  loopOffAndSettle(t);
  CHECK_EQ(sd().openCount(), opens - 1);
}

TEST(loop_refusals_slots_full_low_ram_busy_no_content) {
  Generator& g = gen(); const int opens = sd().openCount();
  // two slots device-wide: a third slot track is refused, the Vocal track needs none
  CHECK(g.setLoopMode(Chord, true)); runUntilIdle(); CHECK(g.loopEnabled(Chord)); CHECK(g.loopFile(Chord)[0] != 0);
  CHECK(g.setLoopMode(kTracksPerSet + Lead, true)); runUntilIdle(); CHECK(g.loopEnabled(kTracksPerSet + Lead));
  CHECK(g.loopSlot(Chord) != g.loopSlot(kTracksPerSet + Lead)); CHECK(g.loopSlot(Chord) >= 0 && g.loopSlot(kTracksPerSet + Lead) >= 0);
  CHECK(!g.setLoopMode(Pad, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopSlotsFull); CHECK(!g.loopEnabled(Pad));
  CHECK(g.setLoopMode(Vocal, true)); CHECK_EQ(g.loopSlot(Vocal), -1); CHECK(g.loopEnabled(Vocal)); runUntilIdle();
  CHECK(inFolder(g.loopPath(Vocal), "/lp/packs/House/loops/vocal/")); CHECK(inFolder(g.loopPath(Chord), "/lp/packs/House/loops/chord/")); CHECK(inFolder(g.loopPath(kTracksPerSet + Lead), "/lp/packs/House/loops/lead/"));
  CHECK(g.setLoopMode(Vocal, false)); runBlocks(kSampleRate * 2 / kBlock);
  CHECK(g.info(Vocal).file[0] != 0);                           // the phrase came back
  loopOffAndSettle(Chord); loopOffAndSettle(kTracksPerSet + Lead);
  CHECK(!g.anyLoop());
  // low RAM: the device hook reports the free heap; below kLoopLowRamBytes loop mode is refused
  static uint32_t heap; heap = kLoopLowRamBytes - 1;
  g.setFreeHeapHook([]() -> uint32_t { return heap; });
  CHECK(!g.setLoopMode(Beat, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopLowRam); CHECK(!g.loopEnabled(Beat));
  heap = kLoopLowRamBytes + 4096;
  CHECK(g.setLoopMode(Beat, true)); g.setFreeHeapHook(nullptr);
  // busy: a generate refuses the toggle; another track's loop pick in flight does not (the toggles queue behind it)
  CHECK(g.busy());
  CHECK(g.setLoopMode(Percussion, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopOk); CHECK(g.loopEnabled(Percussion));
  runUntilIdle(); CHECK(g.loopEnabled(Beat) && g.loopEnabled(Percussion)); CHECK(g.loopFile(Beat)[0] != 0 && g.loopFile(Percussion)[0] != 0);
  CHECK(g.loopSlot(Beat) != g.loopSlot(Percussion));
  loopOffAndSettle(Percussion);
  CHECK(g.generateSet(1)); CHECK(!g.setLoopMode(Chord, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopBusy); runUntilIdle();
  CHECK(!g.loopEnabled(Chord));
  CHECK(g.setLoopMode(Beat, true));                            // already on: a no-op that succeeds
  loopOffAndSettle(Beat);
  // no content: House has no lead loops in C# minor (no C#min, no E, no NA folder) — refused at once once the library has
  // counted the roots (the cold case is the job's: see the next test), with the status text the UI shows, and no card work
  const int keep = g.key().root;
  g.setKeyRoot(1);
  lib().dropCache();
  for (int i = 0; i < 64 && !lib().prepareLoop("House", Lead, g.key()); i++) {}
  CHECK_EQ(lib().loopTotal("House", Lead, g.key()), 0);
  uint64_t ops = sd().opens + sd().walks;
  CHECK(!g.setLoopMode(Lead, true)); CHECK(!g.loopEnabled(Lead)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopNoContent);
  CHECK(strcmp(g.lastError(), "NO LOOP: LEAD") == 0); CHECK_EQ(g.loopSlot(Lead), -1); CHECK(!g.busy());
  CHECK(!g.setLoopMode(Lead, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopNoContent);   // again: the same
  CHECK(strcmp(g.lastError(), "NO LOOP: LEAD") == 0);
  CHECK_EQ(sd().opens + sd().walks, ops);
  g.setGenre(g.genreIndex() + 1);                              // Testpack has no loops folder at all (noted at boot): every category is refused at once, cold or not
  lib().dropCache(); ops = sd().opens + sd().walks;
  CHECK(!g.setLoopMode(Beat, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopNoContent); CHECK(strcmp(g.lastError(), "NO LOOP: DRUM") == 0);
  CHECK(!g.setLoopMode(Vocal, true)); CHECK(strcmp(g.lastError(), "NO LOOP: VOCAL") == 0);
  CHECK_EQ(sd().opens + sd().walks, ops);
  g.setGenre(g.genreIndex() - 1); CHECK(strcmp(g.genreName(), "House") == 0);
  g.setKeyRoot(keep);
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK(!g.anyLoop()); CHECK_EQ(sd().openCount(), opens);
}

// SPEC §9.19 review: setLoopMode(on) spends no card work of its own (SPEC §9.2: the key handler runs outside the stream
// gate). A category the library has not counted yet is accepted — the slot bound, JobLoop queued, the square blinking —
// and the job finds out: nothing to pick → loop mode off again, lastLoopRefusal() LoopNoContent, lastError "NO LOOP:
// <FOLDER>" (what the UI shows when the job ends), the slot free, no file left open. Once the library knows (the roots
// counted, or a pack without a loops/ folder, noted by scanGenres at boot) the refusal is synchronous and costs nothing.
TEST(cold_category_without_loops_is_refused_by_the_job_not_the_handler) {
  Generator& g = gen(); Engine& e = eng();
  stopAll(); runUntilIdle();
  const int keep = g.key().root; const int opens = sd().openCount();
  g.setKeyRoot(1);                                             // C# minor: House has no lead loops under C#min, E or NA
  lib().dropCache();
  CHECK_EQ(lib().loopTotal("House", Lead, g.key()), -1);      // cold: the library cannot tell without card work
  uint64_t ops = sd().opens + sd().walks;
  CHECK(g.setLoopMode(Lead, true));                            // accepted: the job will tell
  CHECK_EQ(sd().opens + sd().walks, ops);                      // the handler touched the card for nothing
  CHECK(g.loopEnabled(Lead)); CHECK(g.busy()); CHECK(g.loopSlot(Lead) >= 0); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopOk);
  const int steps = runUntilIdle();
  std::printf("  cold C# minor lead: the job ended after %d steps with '%s'\n", steps, g.lastError());
  CHECK(!g.loopEnabled(Lead)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopNoContent);
  CHECK(strcmp(g.lastError(), "NO LOOP: LEAD") == 0); CHECK_EQ(g.loopSlot(Lead), -1); CHECK(!g.anyLoop());
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ(e.trackStream(Lead), -1); CHECK_EQ(sd().openCount(), opens);
  ops = sd().opens + sd().walks;                               // warm now: refused at once, again without card work
  CHECK(!g.setLoopMode(Lead, true)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopNoContent); CHECK(strcmp(g.lastError(), "NO LOOP: LEAD") == 0);
  CHECK_EQ(sd().opens + sd().walks, ops); CHECK(!g.busy());
  // a pack with a loops/ folder whose category holds only empty key folders: cold → the job finds out, then the library knows
  const int house = g.genreIndex();
  g.setGenre(house + 1); CHECK(strcmp(g.genreName(), "Testpack") == 0);
  const char* dirs[] = {"/lp/packs/Testpack/loops", "/lp/packs/Testpack/loops/pad", "/lp/packs/Testpack/loops/pad/NA", "/lp/packs/Testpack/loops/pad/Amin"};
  for (const char* d : dirs) CHECK(sd().mkdir(d));
  CHECK(lib().scanGenres() >= 2); CHECK(lib().genreHasLoops(g.genreIndex()));   // (the pack list is scanned at boot; here again by hand)
  g.setKeyRoot(9); lib().dropCache();                          // A minor: the roots Amin, C and NA — two of them there and empty
  CHECK_EQ(lib().loopTotal("Testpack", Pad, g.key()), -1);
  CHECK(g.setLoopMode(Pad, true)); CHECK(g.busy()); CHECK(g.loopEnabled(Pad));
  runUntilIdle();
  CHECK(!g.loopEnabled(Pad)); CHECK_EQ((int)g.lastLoopRefusal(), (int)Generator::LoopNoContent); CHECK(strcmp(g.lastError(), "NO LOOP: PAD") == 0);
  CHECK_EQ(lib().loopTotal("Testpack", Pad, g.key()), 0);
  ops = sd().opens + sd().walks;
  CHECK(!g.setLoopMode(Pad, true)); CHECK_EQ(sd().opens + sd().walks, ops);   // known now: synchronous
  for (int i = 3; i >= 0; i--) CHECK(sd().remove(dirs[i]));
  CHECK(lib().scanGenres() >= 2); CHECK(!lib().genreHasLoops(g.genreIndex()));
  g.setGenre(house); CHECK(strcmp(g.genreName(), "House") == 0);
  g.setKeyRoot(keep);
  runBlocks(kSampleRate / kBlock);
  CHECK(!g.anyLoop()); CHECK_EQ(sd().openCount(), opens);
}

TEST(vocal_track_loop_mode_streams_a_vocal_loop_through_its_own_stream) {
  Generator& g = gen(); Engine& e = eng(); const int t = Vocal;
  stopAll();
  CHECK(g.info(t).file[0] != 0);
  const std::string phrase = g.info(t).file; const uint8_t pause = g.info(t).pause; const int cycle = g.setLengthStep(t, 0);
  CHECK(startTrack(t));
  runBlocks(kSampleRate / kBlock);
  const int opens = sd().openCount(); const uint16_t under0 = e.status().vocalUnderruns;
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  CHECK(g.setLoopMode(t, true)); CHECK_EQ(g.loopSlot(t), -1); CHECK(g.loopEnabled(t)); CHECK(g.anyLoop());
  runUntilIdle();
  CHECK(g.loopFile(t)[0] != 0); CHECK(inFolder(g.loopPath(t), "/lp/packs/House/loops/vocal/"));
  const int bars = g.loopBars(t); CHECK(bars >= 1);
  CHECK_EQ((int)g.info(t).naturalTicks, bars * kTicksPerBar); CHECK_EQ((int)g.setLengthStep(t, 0), bars * kTicksPerBar);
  runBlocks(kSampleRate * 3 / kBlock);
  g_afterPump = nullptr;
  CHECK(countCmd(CmdVocalLoaded, -1, 0) >= 1); CHECK_EQ(countCmd(CmdLoopLoaded), 0);   // its own stream, no slot
  const Cmd* mode = findCmd(CmdLoopMode, t); CHECK(mode != nullptr); if (mode) { CHECK_EQ((int)mode->a, 1); CHECK_EQ((int)mode->i, 0); }
  const Cmd* swap = findCmd(CmdSwapPattern, t); CHECK(swap != nullptr); if (swap) CHECK_EQ((int)swap->i, bars * kTicksPerBar);
  CHECK(g.vocalStreamsLoop(0)); CHECK_EQ((int)g.vocalLoopTicks(0), bars * kTicksPerBar); CHECK_EQ((int)g.vocalRegionTicks(0), bars * kTicksPerBar);
  CHECK_EQ((int)e.status().track[t].lenTicks, bars * kTicksPerBar); CHECK_EQ(e.trackStream(t), 0);
  CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  CHECK_EQ(sd().openCount(), opens);                           // the loop replaced the phrase file (one stream, one file)
  int compared = 0, xf = 0; int bad = compareLoopRing(0, 0, 1500, &compared, &xf);
  std::printf("  vocal loop %s (%d bars): %d compared (%d fading), %d mismatches, underruns %u\n", g.loopFile(t), bars, compared, xf, bad, (unsigned)(e.status().vocalUnderruns - under0));
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  // a crop on the vocal loop too (no pause cycle: plain loop semantics)
  g.rebuildVariant(t, 2);
  for (int i = 0; i < 400 && (g.vocalSwitchPending(0) || g.vocalLoopTicks(0) != 2 * kTicksPerBeat); i++) iterate();
  CHECK_EQ((int)g.vocalLoopTicks(0), 2 * kTicksPerBeat); CHECK_EQ((int)g.vocalRegionTicks(0), 2 * kTicksPerBeat);
  runBlocks(kSampleRate / kBlock);
  bad = compareLoopRing(0, 0, 1500, &compared, &xf); CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // off: the phrase comes back (one open) with its cycle; CmdLoopMode off rides with the switch
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  CHECK(g.setLoopMode(t, false));
  CHECK(!g.loopEnabled(t)); CHECK(!g.anyLoop());
  CHECK(g.info(t).file == phrase); CHECK_EQ((int)g.info(t).pause, (int)pause);
  runBlocks(kSampleRate * 3 / kBlock);
  g_afterPump = nullptr;
  mode = findCmd(CmdLoopMode, t); CHECK(mode != nullptr); if (mode) CHECK_EQ((int)mode->a, 0);
  if (!mode) { std::printf("  recorded:"); for (const Cmd& c : g_cmdLog.cmds) std::printf(" %d/%d", (int)c.type, (int)c.track); std::printf("\n"); }
  CHECK(!g.vocalStreamsLoop(0)); CHECK_EQ((int)g.vocalLoopTicks(0), cycle); CHECK_EQ((int)e.status().track[t].lenTicks, cycle);
  CHECK_EQ(e.trackStream(t), -1);
  bad = compareRing(0, 1500, &compared);
  std::printf("  back to the phrase: %d compared, %d mismatches\n", compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  CHECK_EQ((int)(e.status().vocalUnderruns - under0), 0);
  CHECK_EQ(sd().openCount(), opens);
  push(CmdTrackStopNow, t); runBlocks(4);
}

TEST(generates_on_a_loop_track_repick_and_keep_loop_mode) {
  Generator& g = gen(); Engine& e = eng(); const int t = Beat;
  stopAll(); CHECK(startTrack(t));
  CHECK(g.setLoopMode(t, true)); runUntilIdle(); CHECK(g.loopEnabled(t));
  const int slot = g.loopSlot(t);
  std::set<std::string> seen; seen.insert(g.loopFile(t));
  const std::string chordFile = g.info(Chord).file; const std::string beatPattern = g.info(t).file;
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  CHECK(g.generatePattern(t)); runUntilIdle();                 // gen pattern: another loop, loop mode kept
  CHECK(g.loopEnabled(t)); CHECK(g.lastError()[0] == 0); seen.insert(g.loopFile(t));
  CHECK(g.generateSound(t)); runUntilIdle();                   // gen sound: the same
  CHECK(g.loopEnabled(t)); seen.insert(g.loopFile(t));
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ(countCmd(CmdResetTrack, t), 0);                     // a re-pick regenerates nothing of the parked sound: no reset
  CHECK_EQ(countCmd(CmdSetPatch, t), 0); CHECK_EQ(countCmd(CmdSlotSwap), 0);
  CHECK(g.info(t).file == beatPattern);                       // the parked pattern survives the generates
  // GENERATE set: seven tracks regenerate, the loop track picks a new loop and stays in loop mode
  CHECK(g.generateSet(0)); runUntilIdle();
  g_afterPump = nullptr;
  CHECK(g.lastError()[0] == 0); CHECK(g.loopEnabled(t)); CHECK_EQ(g.loopSlot(t), slot); seen.insert(g.loopFile(t));
  CHECK(g.info(Chord).file != chordFile || g.info(Lead).file[0] != 0);
  CHECK(countCmd(CmdResetTrack, Chord) >= 1); CHECK_EQ(countCmd(CmdResetTrack, t), 0);
  runBlocks(kSampleRate * 3 / kBlock);
  CHECK_EQ(pendingSwaps(0), 0); CHECK_EQ((int)e.status().generating[0], 0);
  CHECK_EQ(e.trackStream(t), kSets + slot); CHECK_EQ((int)e.status().track[t].state, (int)PSPlaying);
  int compared = 0; const int bad = compareLoopRing(kSets + slot, 0, 1500, &compared);
  std::printf("  %zu distinct loops over four picks; after GENERATE: %d compared, %d mismatches\n", seen.size(), compared, bad);
  CHECK(seen.size() >= 2); CHECK(compared > 1000); CHECK_EQ(bad, 0);
  loopOffAndSettle(t);
}

TEST(four_streams_pump_emptiest_first_without_underruns) {
  Generator& g = gen(); Engine& e = eng();
  stopAll();
  CHECK(g.info(Vocal).file[0] != 0); CHECK(g.info(kTracksPerSet + Vocal).file[0] != 0);
  CHECK(startTrack(Vocal)); CHECK(startTrack(kTracksPerSet + Vocal)); CHECK(startTrack(Beat)); CHECK(startTrack(kTracksPerSet + Percussion));
  CHECK(g.setLoopMode(Beat, true)); runUntilIdle(); CHECK(g.setLoopMode(kTracksPerSet + Percussion, true)); runUntilIdle();
  CHECK(g.loopEnabled(Beat) && g.loopEnabled(kTracksPerSet + Percussion));
  runBlocks(kSampleRate * 2 / kBlock);
  for (int k = 0; k < kLoopSlots; k++) CHECK(e.loopTrack(k) >= 0);
  CHECK(g.loopSlotStreaming());
  uint16_t under0[kStreams]; for (int s = 0; s < kStreams; s++) under0[s] = e.streamUnderruns(s);
  static int minLead[kStreams]; for (int s = 0; s < kStreams; s++) minLead[s] = 1 << 30;
  g_afterPump = [] { for (int s = 0; s < kStreams; s++) { const int a = streamAhead(s); if (a < minLead[s]) minLead[s] = a; } };
  const uint32_t chunks0 = g.pumpChunkTotal();
  runBlocks(kSampleRate * 4 / kBlock);
  g_afterPump = nullptr;
  std::printf("  4 streams for 4 s: min lead %d %d %d %d, underruns %u %u %u %u, %u chunk reads (max pump %d)\n", minLead[0], minLead[1], minLead[2], minLead[3],
              (unsigned)(e.streamUnderruns(0) - under0[0]), (unsigned)(e.streamUnderruns(1) - under0[1]), (unsigned)(e.streamUnderruns(2) - under0[2]), (unsigned)(e.streamUnderruns(3) - under0[3]),
              (unsigned)(g.pumpChunkTotal() - chunks0), g_maxPumpChunks);
  for (int s = 0; s < kStreams; s++) {
    CHECK_EQ((int)(e.streamUnderruns(s) - under0[s]), 0);
    CHECK(minLead[s] >= (s < kSets ? Generator::kRefillSamples : kLoopChunkSamples) - kBlock);
  }
  CHECK(g_maxPumpChunks <= Generator::kPumpChunks);
  int compared = 0;
  CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  CHECK_EQ(compareRing(1, 1500, &compared), 0); CHECK(compared > 1000);
  CHECK_EQ(compareLoopRing(kSets, 0, 1500, &compared), 0); CHECK(compared > 1000);
  CHECK_EQ(compareLoopRing(kSets + 1, 0, 1500, &compared), 0); CHECK(compared > 1000);
  // a tempo change retimes every stream: the rings follow the new clock with the carried file position, no underruns
  push(CmdSetTempo, 0, 0, 140);
  runBlocks(kSampleRate * 2 / kBlock);
  CHECK_EQ((int)e.status().bpm, 140);
  for (int s = 0; s < kStreams; s++) CHECK_EQ((int)(e.streamUnderruns(s) - under0[s]), 0);
  CHECK_EQ(compareRing(0, 1500, &compared), 0); CHECK(compared > 1000);
  int bad = compareLoopRing(kSets, 0, 1500, &compared); std::printf("  at 140 bpm: slot 0 %d compared, %d mismatches\n", compared, bad); CHECK_EQ(bad, 0); CHECK(compared > 1000);
  bad = compareLoopRing(kSets + 1, 0, 1500, &compared); CHECK_EQ(bad, 0); CHECK(compared > 1000);
  push(CmdSetTempo, 0, 0, 120);
  runBlocks(kSampleRate / kBlock);
  for (int s = 0; s < kStreams; s++) CHECK_EQ((int)(e.streamUnderruns(s) - under0[s]), 0);
  loopOffAndSettle(Beat); loopOffAndSettle(kTracksPerSet + Percussion);
  stopAll();
  CHECK(!g.loopSlotStreaming());
}

TEST(streams_health_gate_tops_the_rings_up) {
  Generator& g = gen(); Engine& e = eng();
  stopAll();
  CHECK(g.streamsHealthy());                                   // nothing consumed: healthy
  CHECK(startTrack(Vocal)); CHECK(startTrack(Beat));
  CHECK(g.setLoopMode(Beat, true)); runUntilIdle();
  runBlocks(kSampleRate / kBlock);
  const int slot = g.loopSlot(Beat); CHECK(slot >= 0);
  // steady state: both consumed rings hold their lead right after a pump
  int healthy = 0, run = 0, worstRun = 0; const int probes = kSampleRate / kBlock;
  for (int i = 0; i < probes; i++) { iterate(); if (g.streamsHealthy()) { healthy++; run = 0; } else if (++run > worstRun) worstRun = run; }
  std::printf("  steady state (a client polling every iteration): healthy on %d of %d iterations, longest closed run %d\n", healthy, probes, worstRun);
  // The vocal ring drains 256 per block from 4096 and the lead is 3748 (170 ms: the 150 ms op plus the refill reads that
  // follow it, SPEC §9.19e / §9.22): one block of headroom above the lead, so a client polling once per block (this loop:
  // one pump per block) sees the gate open every other iteration; without a client it would pass ~40 % of the time, on the
  // device the loop task pumps several times per block and keeps the ring within a block of full.
  CHECK(healthy >= probes * 45 / 100);
  CHECK(worstRun <= 1);                                        // every refused query tops the rings up on the next pump: the gate is never closed twice in a row
  // a ring that lost its lead (as after a long card stall) closes the gate; the next pump tops it up and the gate opens again
  VocalRing& ring = e.loopRing(slot);
  const uint32_t w = ring.w.load(); ring.r.store(w - 400);      // 400 samples of lead left
  CHECK(!g.streamsHealthy());
  g.pumpVocal(); g.pumpVocal();                                // the pump of the next iteration (a new block's budget)
  iterate();
  CHECK(g.streamsHealthy());
  CHECK(ring.available() >= Generator::kGateLeadSamples || ring.available() >= ring.cap - 2 * kBlock);
  VocalRing& vring = e.vocalRing(0);
  vring.r.store(vring.w.load() - 800);
  CHECK(!g.streamsHealthy());
  iterate();
  CHECK(g.streamsHealthy()); CHECK(vring.available() >= Generator::kGateLeadSamples);
  loopOffAndSettle(Beat); stopAll();
}

TEST(generate_set_keeps_recorded_loops_on_every_track) {
  Generator& g = gen(); Engine& e = eng();
  // SPEC §9.19 §3: a recorded (perform) loop on ANY track survives GENERATE / gen pattern, not only on Fx
  const int keep[] = {Chord, Beat};
  std::string files[2]; Pattern src[2];
  for (int i = 0; i < 2; i++) { const int t = keep[i]; CHECK(e.sourcePattern(t).count > 0); g.info(t).userLoop = 1; files[i] = g.info(t).file; src[i] = e.sourcePattern(t); }
  const std::string lead = g.info(Lead).file;
  CHECK(g.generateSet(0)); runUntilIdle();
  CHECK(g.lastError()[0] == 0);
  for (int i = 0; i < 2; i++) {
    const int t = keep[i];
    CHECK(g.info(t).file == files[i]); CHECK_EQ((int)g.info(t).userLoop, 1);
    CHECK(memcmp(&src[i], &e.sourcePattern(t), sizeof(Pattern)) == 0);
  }
  CHECK(strcmp(g.lastNote(), "BEAT kept") == 0 || strcmp(g.lastNote(), "CHD kept") == 0);
  CHECK(g.info(Lead).file != lead || g.info(Lead).acidPat);  // the others regenerated
  CHECK(g.generatePattern(Beat)); runUntilIdle();
  CHECK(g.info(Beat).file == files[1]); CHECK(strcmp(g.lastNote(), "BEAT kept") == 0);
  for (int i = 0; i < 2; i++) g.info(keep[i]).userLoop = 0;
  runBlocks(kSampleRate * 3 / kBlock);
}

TEST(source_and_kit_paths_are_remembered_for_the_session_writer) {
  Generator& g = gen(); Engine& e = eng();
  char p[kMaxPathLen];
  for (int t = 0; t < kTracksPerSet; t++) {
    if (t == Vocal || g.info(t).acidPat || !g.info(t).file[0]) continue;
    CHECK(g.sourcePath(t, p, sizeof p));
    CHECK(sd().exists(p)); CHECK(strcmp(baseName(p), g.info(t).file) == 0);
    CHECK(strncmp(p, "/lp/midi/", 9) == 0);
  }
  for (int k = 0; k < kSampleSlots; k++) {
    CHECK(g.kitPath(0, k, p, sizeof p));
    CHECK(sd().exists(p)); CHECK(strcmp(baseName(p), e.kit(0).name[k]) == 0);
    CHECK(strncmp(p, "/lp/packs/House/drum/", 21) == 0);
  }
  // a name changed by hand no longer matches what was picked: unknown (the writer probes as before)
  TrackGenInfo& inf = g.info(Chord); const std::string keep = inf.file;
  strcpy(inf.file, "Amin__Chord__gone_from_the_card__00000000.mid");
  CHECK(!g.sourcePath(Chord, p, sizeof p));
  strcpy(inf.file, keep.c_str()); CHECK(g.sourcePath(Chord, p, sizeof p));
  // a source loaded from /lp/state (a session SMF) and a kit one-shot noted by the set loader
  CHECK(sd().mkdir("/lp/state"));
  { char libp[kMaxPathLen]; CHECK(g.sourcePath(Chord, libp, sizeof libp));
    IFile* in = sd().open(libp); IFile* out = sd().open("/lp/state/gen_test_src.mid", "w"); CHECK(in && out);
    if (in && out) { uint8_t buf[2048]; size_t n; while ((n = in->read(buf, sizeof buf)) > 0) out->write(buf, n); }
    if (in) in->close(); if (out) out->close(); }
  const Pattern before = e.sourcePattern(Chord);
  CHECK(g.loadMidiToSource(Chord, "/lp/state/gen_test_src.mid"));
  CHECK(g.sourcePath(Chord, p, sizeof p)); CHECK(strcmp(p, "/lp/state/gen_test_src.mid") == 0);
  g.noteKitPath(0, 2, "/lp/packs/House/drum/hh/s00/Fake_Hat.wav"); e.setKitName(0, 2, "Fake_Hat.wav");
  CHECK(g.kitPath(0, 2, p, sizeof p)); CHECK(strcmp(p, "/lp/packs/House/drum/hh/s00/Fake_Hat.wav") == 0);
  g.noteKitPath(0, 2, "/somewhere/else/Fake_Hat.wav"); CHECK(!g.kitPath(0, 2, p, sizeof p));
  CHECK(sd().remove("/lp/state/gen_test_src.mid"));
  // back to the library file for the later tests
  char libp[kMaxPathLen]; snprintf(libp, sizeof libp, "%s", keep.c_str());
  (void)before;
  CHECK(g.generatePattern(Chord)); runUntilIdle();
  CHECK(g.generateSound(Beat)); runUntilIdle();              // the kit slots noted again
  CHECK(g.kitPath(0, 2, p, sizeof p));
}

TEST(load_loop_restores_an_exact_file_with_variant_and_len) {
  Generator& g = gen(); Engine& e = eng(); const int t = Bass;
  stopAll();
  NameFn files; sd().walk("/lp/packs/House/loops/bass/Amin", collect, &files);
  std::string pick; for (const std::string& n : files.names) if (n != "_index.txt") { pick = "/lp/packs/House/loops/bass/Amin/" + n; break; }
  CHECK(!pick.empty());
  const int bars = Generator::parseBarsToken(pick.c_str()); CHECK(bars >= 1);
  const uint8_t parkedVariant = g.info(t).variant; const int8_t parkedLen = g.info(t).lenStep; const uint16_t parkedNatural = g.info(t).naturalTicks;
  CHECK(!g.loadLoop(t, "/lp/packs/House/loops/bass/Amin/does_not_exist.wav", 0, 0));   // a missing file: nothing changes
  CHECK(!g.loopEnabled(t)); CHECK_EQ((int)g.info(t).variant, (int)parkedVariant);
  CHECK(g.loadLoop(t, pick.c_str(), 2, 1));                   // the set loader's restore: variant 3 (2 beats), k 1
  CHECK(g.loopEnabled(t)); CHECK(g.loopSlot(t) >= 0); CHECK(!g.busy());
  CHECK(std::string(g.loopPath(t)) == pick); CHECK_EQ((int)g.loopBars(t), bars);
  CHECK_EQ((int)g.info(t).variant, 2); CHECK_EQ((int)g.info(t).lenStep, 1); CHECK_EQ((int)g.info(t).naturalTicks, 2 * kTicksPerBeat);
  CHECK_EQ((int)g.setLengthStep(t, 0), kTicksPerBar);
  runBlocks(kSampleRate * 3 / kBlock);                        // the track is stopped: the switch went out at once, the engine bound it at the bar
  const int slot = g.loopSlot(t);
  CHECK_EQ((int)g.loopClipTicks(slot), kTicksPerBar); CHECK_EQ((int)g.loopRegionTicks(slot), 2 * kTicksPerBeat);
  CHECK_EQ(e.trackStream(t), kSets + slot); CHECK_EQ((int)e.status().track[t].lenTicks, kTicksPerBar); CHECK_EQ((int)e.status().track[t].variant, 2);
  CHECK(startTrack(t)); runBlocks(kSampleRate * 2 / kBlock);
  int compared = 0; const int bad = compareLoopRing(kSets + slot, 0, 1500, &compared);
  std::printf("  restored %s: %d compared, %d mismatches\n", g.loopFile(t), compared, bad);
  CHECK(compared > 1000); CHECK_EQ(bad, 0);
  // loadLoop on a track already in loop mode replaces the file and keeps the slot
  CHECK(g.loadLoop(t, pick.c_str(), 0, 0)); CHECK_EQ(g.loopSlot(t), slot); CHECK_EQ((int)g.info(t).variant, 0);
  for (int i = 0; i < 600 && g.loopSwitchPending(slot); i++) iterate();   // the new file switches on the next bar
  CHECK(!g.loopSwitchPending(slot));
  CHECK_EQ((int)g.loopClipTicks(slot), bars * kTicksPerBar);
  loopOffAndSettle(t);
  CHECK_EQ((int)g.info(t).variant, (int)parkedVariant); CHECK_EQ((int)g.info(t).lenStep, (int)parkedLen); CHECK_EQ((int)g.info(t).naturalTicks, (int)parkedNatural);
  stopAll();
}

// ---------------------------------------------------------------------------------------------- SPEC §9.19g

// The generator's job states (Generator::stateCode(), the private State enum in dispatch order): the loop job's three card
// steps, and the vocal's open, as the per-step accounting below tells them apart.
enum { kStIdle = 0, kStTrackBegin = 1, kStVocalOpen = 15, kStPrepLoop = 21, kStLoopPick = 22, kStLoopOpen = 23 };

// SPEC §9.19f measured one Generator::step() of 152 ms on the device for the loop pick; SPEC §9.2 bounds a step to one card
// operation. This is the per-step accounting of a COLD Beat loop pick through Generator::step() itself (pump included), on
// a library cache full of other trees (the device's after a set generate): every step opens at most one file or walks one
// folder, reads at most one index chunk of its own beyond the pump's, and the job takes exactly one step per root (the
// category listing, NA, every key folder), one for the pick and one for the file open.
TEST(cold_beat_loop_pick_is_one_card_operation_per_step) {
  Generator& g = gen(); Library& l = lib(); const int t = Beat;
  stopAll(); runUntilIdle();
  CHECK(!g.loopEnabled(t)); CHECK(!g.anyLoop());
  // a full cache: every MIDI category, the kit roles and the vocal roots (≥ kFolderSlots trees), the loop trees cold
  l.dropCache();
  for (Category c : {Chord, Lead, Pad, Bass, Fx, Percussion, Beat}) for (int i = 0; i < 128 && !l.prepareMidi(c, g.key()); i++) {}
  for (const char* role : {"kick", "snare", "hh", "oh", "perc"}) for (int i = 0; i < 128 && !l.prepareSample("House", role, role[0] == 'k' ? g.key().root : -1); i++) {}
  for (int i = 0; i < 128 && !l.prepareVocal("House", g.key()); i++) {}
  CHECK_EQ(l.loopTotal("House", Beat, g.key()), -1);          // cold
  NameFn keys; sd().walk("/lp/packs/House/loops/drum", collectDirs, &keys);
  int roots = 0; bool na = false;
  for (const std::string& k : keys.names) { roots++; if (k == "NA") na = true; }
  if (!na) roots++;                                            // NA is addressed by name: one (failed) step even when absent
  CHECK(startTrack(t));
  runBlocks(kSampleRate / kBlock);                             // the rings full: the stream gate lets every step work
  CHECK(g.setLoopMode(t, true)); CHECK(g.busy());
  int steps = 0, cardSteps = 0, prepSteps = 0, pickSteps = 0, openSteps = 0;
  uint64_t worstOpens = 0, worstReads = 0, worstWalks = 0, worstWalked = 0, worstOwnBytes = 0;
  while (g.busy() && steps < 86 * 20) {
    const int st = g.stateCode();
    const uint64_t o0 = sd().opens, r0 = sd().reads, w0 = sd().walks, e0 = sd().walked, b0 = sd().bytes; const uint32_t p0 = g.pumpChunkTotal();
    iterate(); steps++;
    const uint64_t dop = sd().opens - o0, dr = sd().reads - r0, dw = sd().walks - w0, de = sd().walked - e0, db = sd().bytes - b0;
    const uint64_t pumpBytes = (uint64_t)(g.pumpChunkTotal() - p0) * 4096;   // the pump's chunk reads (at most the vocal's 4 KB each)
    const uint64_t own = db > pumpBytes ? db - pumpBytes : 0;
    if (dop > worstOpens) worstOpens = dop; if (dr > worstReads) worstReads = dr; if (dw > worstWalks) worstWalks = dw; if (de > worstWalked) worstWalked = de; if (own > worstOwnBytes) worstOwnBytes = own;
    CHECK(dop <= 1); CHECK(dw <= 1); CHECK(dop + dw <= 1); CHECK(de <= 32); CHECK(own <= 4096 + 512);
    if (dop + dw > 0) { cardSteps++; if (st == kStPrepLoop || st == kStIdle || st == kStTrackBegin) prepSteps++; else if (st == kStLoopPick) pickSteps++; else if (st == kStLoopOpen) openSteps++; else CHECK(false); }   // the first prepare step begins the job (Idle → TrackBegin → PrepLoop in one chain)
  }
  std::printf("  cold Beat loop pick through step(): %d steps, %d with card work: prepared in %d steps (%d roots + the listing; worst step %llu opens %llu reads %llu walks / %llu walked, %llu B of its own), pick %d, open %d\n",
              steps, cardSteps, prepSteps, roots, (unsigned long long)worstOpens, (unsigned long long)worstReads, (unsigned long long)worstWalks, (unsigned long long)worstWalked, (unsigned long long)worstOwnBytes, pickSteps, openSteps);
  CHECK(!g.busy()); CHECK(g.loopEnabled(t)); CHECK(g.loopFile(t)[0] != 0); CHECK(g.lastError()[0] == 0);
  CHECK_EQ(prepSteps, 1 + roots);                              // the listing walk, then one index chunk (or walk) per root — never two in one step
  CHECK_EQ(pickSteps, 1); CHECK_EQ(openSteps, 1);
  CHECK_EQ(cardSteps, prepSteps + 2);
  CHECK(worstOpens <= 1); CHECK(worstWalks <= 1); CHECK(worstWalked <= 32);
  CHECK(l.loopTotal("House", Beat, g.key()) > 0);
  runBlocks(kSampleRate * 2 / kBlock);
  loopOffAndSettle(t); stopAll();
  CHECK(!g.anyLoop());
}

// SPEC §9.19g: the card has kMaxOpenFiles (7) handles — one per live stream, two for pending new-file switches, one for the
// job's own kit / index / SMF file. A GENERATE of a set whose Vocal track and two loop-slot tracks all switch files would
// hold 4 + 3 + 1 = 8: the generator opens a third new stream file only once a pending switch has freed its handle at the
// bar line (the job waits without card work), so with the host file system limited to the device's 7 handles no open ever
// fails, every stream switches, nothing underruns and the handles in use never exceed 7. The Vocal track's return from
// loop mode is deferred the same way: its loop keeps streaming until the phrase has a handle.
TEST(generate_set_with_three_switching_streams_never_fails_an_open) {
  Generator& g = gen(); Engine& e = eng();
  stopAll(); runUntilIdle();
  CHECK_EQ(PosixFs::kMaxOpen, kMaxOpenFiles); CHECK_EQ(kMaxOpenFiles, 7); CHECK_EQ(Generator::kPendingSwitches, 2);
  CHECK(g.info(Vocal).file[0] != 0); CHECK(g.info(kTracksPerSet + Vocal).file[0] != 0);
  const int tA = Beat, tB = Percussion;                       // the set order ends Percussion, Beat, Vocal: the Vocal's open finds both slot switches pending
  CHECK(startTrack(Vocal)); CHECK(startTrack(kTracksPerSet + Vocal)); CHECK(startTrack(tA)); CHECK(startTrack(tB));
  CHECK(g.setLoopMode(tA, true)); runUntilIdle(); CHECK(g.setLoopMode(tB, true)); runUntilIdle();
  CHECK(g.loopEnabled(tA) && g.loopEnabled(tB)); CHECK(g.loopSlot(tA) >= 0 && g.loopSlot(tB) >= 0 && g.loopSlot(tA) != g.loopSlot(tB));
  runBlocks(kSampleRate * 3 / kBlock);                         // both loop switches landed: four live streams, four files
  CHECK(!g.loopSwitchPending(0) && !g.loopSwitchPending(1));
  CHECK_EQ(sd().openCount(), kStreams); CHECK(g.streamOpenAllowed());
  const std::string chord0 = g.loopFile(tA), lead0 = g.loopFile(tB), vocal0 = g.info(Vocal).file;
  uint16_t under0[kStreams]; for (int s = 0; s < kStreams; s++) under0[s] = e.streamUnderruns(s);
  sd().failedWavOpens = 0; sd().maxOpen = sd().openCount();
  static int maxPending, waited, maxOpenSeen;
  maxPending = 0; waited = 0; maxOpenSeen = 0;
  g_afterPump = [] {
    const int p = (gen().loopSwitchPending(0) ? 1 : 0) + (gen().loopSwitchPending(1) ? 1 : 0) + (gen().vocalSwitchPending(0) ? 1 : 0) + (gen().vocalSwitchPending(1) ? 1 : 0);
    if (p > maxPending) maxPending = p;
    if ((gen().stateCode() == kStVocalOpen || gen().stateCode() == kStLoopOpen) && !gen().streamOpenAllowed()) waited++;
    if (sd().openCount() > maxOpenSeen) maxOpenSeen = sd().openCount();
  };
  // The five MIDI trees are warmed first: on the expanded card (key folders of up to 12 shards) their cold counting alone
  // would carry the job past the bar line that frees the slots' handles, and the third open would never have to wait.
  for (Category cat : {Chord, Lead, Pad, Bass, Fx}) { int n = 0; while (n < 4000 && !lib().prepareMidi(cat, g.key())) n++; CHECK(n < 4000); }
  CHECK(runUntilBar(400));                                     // right after a bar line: the job reaches the Vocal's open well inside the bar
  CHECK(g.generateSet(0));
  const int steps = runUntilIdle(86 * 60);
  runBlocks(kSampleRate * 3 / kBlock);                         // the last switch (the vocal's, a bar after its open) lands
  g_afterPump = nullptr;
  std::printf("  GENERATE S1 with the Vocal and both loop slots switching: %d steps, %d waited for a handle, at most %d switches pending, %d handles in use at most (pool %d), %llu failed stream opens\n",
              steps, waited, maxPending, sd().maxOpen > maxOpenSeen ? sd().maxOpen : maxOpenSeen, PosixFs::kMaxOpen, (unsigned long long)sd().failedWavOpens);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0);
  CHECK_EQ((int)sd().failedWavOpens, 0);
  CHECK(sd().maxOpen <= kMaxOpenFiles); CHECK(maxOpenSeen <= kMaxOpenFiles);
  CHECK_EQ(maxPending, Generator::kPendingSwitches);           // two switches waited for their bar line at once, never three
  CHECK(waited > 0);                                           // the third open held back without card work
  CHECK(g.loopEnabled(tA) && g.loopEnabled(tB));
  CHECK(std::string(g.loopFile(tA)) != chord0); CHECK(std::string(g.loopFile(tB)) != lead0); CHECK(std::string(g.info(Vocal).file) != vocal0);
  CHECK(!g.loopSwitchPending(0) && !g.loopSwitchPending(1) && !g.vocalSwitchPending(0));
  for (int s = 0; s < kStreams; s++) CHECK_EQ((int)(e.streamUnderruns(s) - under0[s]), 0);
  CHECK_EQ(sd().openCount(), kStreams);
  // the Vocal track's own loop, then its return while both slots switch again: the phrase has no handle, the return waits
  // (loop mode already off for the UI, the loop still streaming, nothing emptied) and lands once a slot switch finalized
  CHECK(g.setLoopMode(Vocal, true)); runUntilIdle(); runBlocks(kSampleRate * 3 / kBlock);
  CHECK(g.vocalStreamsLoop(0)); CHECK_EQ(e.trackStream(Vocal), 0); CHECK_EQ(sd().openCount(), kStreams);
  const std::string phrase = g.vocalPath(0, 0); CHECK(!phrase.empty());
  CHECK(runUntilBar(400));                                     // right after a bar line: both re-picks switch on the same next bar
  CHECK(g.generatePattern(tA)); runUntilIdle(); CHECK(g.generatePattern(tB)); runUntilIdle();
  CHECK(g.loopSwitchPending(0) && g.loopSwitchPending(1)); CHECK_EQ(sd().openCount(), kStreams + Generator::kPendingSwitches); CHECK(!g.streamOpenAllowed());
  sd().failedWavOpens = 0; g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  CHECK(g.setLoopMode(Vocal, false));
  CHECK(!g.loopEnabled(Vocal)); CHECK(g.vocalStreamsLoop(0)); CHECK(!g.vocalSwitchPending(0)); CHECK_EQ(sd().openCount(), kStreams + Generator::kPendingSwitches);   // deferred: the loop stays, no file opened
  CHECK(e.nextVocal(0).present == 1);                          // nothing emptied
  int i = 0; for (; i < 86 * 12 && g.vocalStreamsLoop(0); i++) iterate();
  g_afterPump = nullptr;
  std::printf("  loop off on the Vocal with both slots switching: the phrase came back after %d steps, %llu failed stream opens\n", i, (unsigned long long)sd().failedWavOpens);
  CHECK(!g.vocalStreamsLoop(0)); CHECK(std::string(g.vocalPath(0, 0)) == phrase); CHECK(g.info(Vocal).file == std::string(baseName(phrase.c_str())));
  CHECK_EQ((int)sd().failedWavOpens, 0); CHECK(sd().maxOpen <= kMaxOpenFiles);
  const Cmd* mode = findCmd(CmdLoopMode, Vocal); CHECK(mode != nullptr); if (mode) CHECK_EQ((int)mode->a, 0);
  CHECK_EQ(e.trackStream(Vocal), -1); CHECK(!g.loopSwitchPending(0) && !g.loopSwitchPending(1));
  CHECK_EQ((int)(e.streamUnderruns(0) - under0[0]), 0);
  loopOffAndSettle(tA); loopOffAndSettle(tB); stopAll();
  CHECK(!g.anyLoop()); CHECK_EQ(sd().openCount(), 2);
}

// SPEC §9.24 (SHIFT + load 1 / 2): a THEORY set job replaces the five melodic tracks with patterns derived from ONE
// progression of the flash tables — no card work for them — and runs Beat / Percussion / Vocal through the library inside
// the same job, so one key regenerates the whole set with one CmdSwapSet at the bar. The patterns are labelled
// "THEORY <progression>" and marked edited (set files and sessions write them as SMF assets, so they reload exactly), every
// note is in the key (the chord's borrowed classes allowed only inside its span), the four variants build from the
// generated source, lenStep is reset, the request queues like GENERATE (a repeat for the queued set coalesces, the very job
// running refuses the double press), and the whole set replays from the same seed (the recorder re-seeds, SPEC §9.23). A
// GENERATE afterwards makes the set an ordinary set again.
TEST(theory_set_generates_the_melodic_tracks_from_one_progression) {
  Generator& g = gen(); Engine& e = eng();
  stopAll(); runUntilIdle();
  for (int t = 0; t < kTracks; t++) if (g.acidEnabled(t)) { g.setAcid(t, false); runBlocks(4); }
  CHECK(!g.theorySet(0)); CHECK_EQ(g.theoryProgression(0), -1); CHECK(!g.theorySet(1));
  const uint16_t ack0 = e.status().slotAck;
  const std::string beat0 = g.info(Beat).file, vocal0 = g.info(Vocal).file;
  g.seed(0x7E0A5EEDu);
  CHECK(g.generateTheorySet(0));
  CHECK(g.busy());
  CHECK(g.generateTheorySet(0));                                      // a repeat for the set that is queued coalesces: accepted, no second job (the replay below proves one plan was drawn)
  CHECK(!g.generateTheorySet(2)); CHECK(!g.generateTheorySet(-1));   // a bad set is refused
  { int n = 0; while (g.busy() && g.stateCode() == 0 && n < 400) { iterate(); n++; } }   // until the job starts (state Idle = 0 while it waits in the queue)
  CHECK(g.busy()); CHECK(g.stateCode() != 0); CHECK(!g.generateTheorySet(0));   // the very job running: the double press is a no-op, as for GENERATE
  const int steps = runUntilIdle(86 * 60);
  std::printf("  THEORY S1 in %d steps (%.2f s of audio)\n", steps, steps * (double)kBlock / kSampleRate);
  CHECK(!g.busy()); CHECK_EQ(g.progress(), 8); CHECK_EQ(g.total(), 8);
  if (g.lastError()[0]) std::printf("  lastError: %s\n", g.lastError());
  CHECK(g.lastError()[0] == 0);
  CHECK(g.theorySet(0)); CHECK(!g.theorySet(1));
  const int prog = g.theoryProgression(0);
  CHECK(prog >= 0 && prog < theory::kProgCount);
  TheoryPlan ref; ref.prog = (uint8_t)prog; ref.nChords = theory::kProgs[prog].n; ref.chordSteps = (uint8_t)(theory::kProgs[prog].beatsPerChord * 4); ref.root = g.key().root;
  char name[64]; theoryProgName(ref, name, sizeof name);
  std::printf("  progression %d: %s, %d chords x %d beats, key %s\n", prog, name, ref.nChords, theory::kProgs[prog].beatsPerChord, g.key().rootName());
  ProjectKey key; key.root = g.key().root;
  Pattern v0, vk;
  for (Category c : {Chord, Lead, Pad, Bass, Fx}) {
    const int t = c; const TrackGenInfo& inf = g.info(t);
    CHECK(strncmp(inf.file, "THEORY ", 7) == 0); CHECK(strcmp(inf.file + 7, name) == 0);
    CHECK_EQ((int)inf.edited, 1); CHECK_EQ((int)inf.acidPat, 0); CHECK_EQ((int)inf.userLoop, 0); CHECK_EQ((int)inf.lenStep, 0); CHECK_EQ((int)inf.loaded, 1);
    CHECK_EQ((int)inf.keyRoot, (int)g.key().root); CHECK_EQ((int)inf.variant, 0);
    CHECK(inf.sound[0] != 0);                                  // the sound part ran as in GENERATE
    CHECK(e.status().track[t].state != PSEmpty);
    const Pattern& src = e.sourcePattern(t);
    CHECK(src.count > 0 && src.count <= kMaxNotes); CHECK_EQ((int)src.lengthTicks, (int)theoryLengthTicks(ref)); CHECK_EQ((int)inf.sourceTicks, (int)src.lengthTicks);
    for (int i = 0; i < src.count; i++) {                      // in key: the natural minor plus the sounding chord's borrowed classes
      const uint16_t allowed = theoryAllowedPcs(ref.root, theoryChordCode(ref, theoryChordAt(ref, src.notes[i].start())));
      CHECK((allowed >> (src.notes[i].pitch() % 12)) & 1);
    }
    buildVariant(c, src, 0, inf.seed, key, v0);
    CHECK(memcmp(&v0, &e.activePattern(t), sizeof(Pattern)) == 0);   // variant 0 of the generated source is what plays: the set swap landed
    for (int v = 1; v < kVariants; v++) { buildVariant(c, src, v, inf.seed, key, vk); CHECK(vk.count > 0); }
    char path[kMaxPathLen]; CHECK(!g.sourcePath(t, path, sizeof path));   // no library folder behind it: the session writer takes the SMF path
  }
  for (int i = 0; i < e.sourcePattern(Pad).count; i++) { const int p = e.sourcePattern(Pad).notes[i].pitch(); CHECK(p >= kTheoryPadLo && p <= kTheoryPadHi); }
  for (int i = 0; i < e.sourcePattern(Bass).count; i++) { const int p = e.sourcePattern(Bass).notes[i].pitch(); CHECK(p >= kTheoryBassLo && p <= kTheoryBassHi); }
  for (int i = 0; i < e.sourcePattern(Lead).count; i++) { const int p = e.sourcePattern(Lead).notes[i].pitch(); CHECK(p >= kTheoryLeadLo && p <= kTheoryLeadHi); }
  // the drum and vocal tracks went through the library inside the same job
  CHECK_EQ((int)(uint16_t)(e.status().slotAck - ack0), 8);
  CHECK(g.info(Beat).file[0] != 0 && strncmp(g.info(Beat).file, "THEORY", 6) != 0); CHECK(g.info(Percussion).file[0] != 0); CHECK(g.info(Vocal).file[0] != 0);
  CHECK_EQ((int)g.info(Beat).edited, 0); CHECK_EQ((int)g.info(Beat).variant, 0); CHECK_EQ((int)g.info(Percussion).variant, 3);
  CHECK(e.nextVocal(0).present == 1);
  CHECK(e.status().generating[0] == 0);                        // CmdSetGenerating off after the swap, as GENERATE
  std::printf("  Chord %d notes, Lead %d, Pad %d, Bass %d, Fx %d; Beat %s; vocal %s\n", e.sourcePattern(Chord).count, e.sourcePattern(Lead).count, e.sourcePattern(Pad).count, e.sourcePattern(Bass).count, e.sourcePattern(Fx).count, g.info(Beat).file, g.info(Vocal).file);
  (void)beat0; (void)vocal0;
  // determinism: the same seed replays the same five patterns (and the same progression)
  Pattern first[5]; for (int i = 0; i < 5; i++) first[i] = e.sourcePattern((Category)std::vector<int>{Chord, Lead, Pad, Bass, Fx}[i]);
  g.seed(0x7E0A5EEDu);
  CHECK(g.generateTheorySet(0)); runUntilIdle(86 * 60);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK_EQ(g.theoryProgression(0), prog);
  { int i = 0; for (Category c : {Chord, Lead, Pad, Bass, Fx}) { CHECK(memcmp(&first[i], &e.sourcePattern(c), sizeof(Pattern)) == 0); i++; } }
  // a different seed: another progression or pattern (not the same set twice)
  g.seed(0x5EED0002u);
  CHECK(g.generateTheorySet(0)); runUntilIdle(86 * 60);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0);
  { bool differs = g.theoryProgression(0) != prog; for (Category c : {Chord, Lead, Pad, Bass}) if (memcmp(&first[c == Fx ? 4 : (int)c], &e.sourcePattern(c), sizeof(Pattern)) != 0) differs = true; CHECK(differs); }
  // set 2 independently; set 1 stays a theory set
  CHECK(g.generateTheorySet(1)); runUntilIdle(86 * 60);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK(g.theorySet(1)); CHECK(g.theorySet(0));
  CHECK(strncmp(g.info(kTracksPerSet + Chord).file, "THEORY ", 7) == 0);
  // a plain GENERATE brings the library back and the set is no longer a theory set; gen pattern on a melodic track too
  CHECK(g.generateSet(1)); runUntilIdle(86 * 60);
  CHECK(!g.busy()); CHECK(!g.theorySet(1)); CHECK_EQ(g.theoryProgression(1), -1);
  CHECK(strncmp(g.info(kTracksPerSet + Chord).file, "THEORY ", 7) != 0);   // (edited is the Ui's to clear at the GENERATE request, as for every pattern it replaces, SPEC §9.17c)
  CHECK(g.theorySet(0));
  CHECK(g.generatePattern(Lead)); runUntilIdle();
  CHECK(!g.theorySet(0)); CHECK(strncmp(g.info(Lead).file, "THEORY ", 7) != 0);
  CHECK(strncmp(g.info(Chord).file, "THEORY ", 7) == 0);       // the other theory patterns stay as they are
  runBlocks(kSampleRate / kBlock);
  CHECK(g.lastError()[0] == 0);
  CHECK_EQ(sd().openCount(), 2);
}

// SPEC §9.24 with §9.23 (review fix): a theory request QUEUES behind the job in flight exactly like generateSet — it is never
// refused for a busy generator. Live, loop() steps the generator about every millisecond; in an offline render a set job
// spans many times more audio (the theory job above: 169 steps ≈ 2 s of audio, GENERATE S1 ≈ 350), so a THEORY key one
// bar after a GENERATE (or another THEORY) is accepted live but would have been refused by a busy() gate in the render:
// the WAV would silently lack the set and every later draw would diverge (a refusal draws nothing from the Rng). Queued,
// the request order — the same live and replayed — decides when startJob draws the plan, so the last request wins in both
// orders; the only refusal left is the double press for the very job that is running (a no-op, as for GENERATE), a repeat
// for a set that is still queued coalesces. (An exact "same set from a key one bar earlier or later" comparison is not
// possible in this fixture: the preset bags and the library's pick history make the preceding job's draw count depend on
// what ran before, and neither can be reset between two runs; the per-seed replay of a queued theory job is the same
// startJob path the determinism check of the previous test covers.)
TEST(theory_request_queues_behind_a_running_job_in_request_order) {
  Generator& g = gen(); Engine& e = eng();
  stopAll(); runUntilIdle();
  auto untilRunning = [&]() { int n = 0; while (g.busy() && g.stateCode() == 0 && n < 400) { iterate(); n++; } return g.busy() && g.stateCode() != 0; };
  // GENERATE S1, then THEORY S1 while the generate runs (its set still being picked): accepted, and the set ends as a theory set
  g.seed(0x0DE7E12Au);
  CHECK(g.generateSet(0)); CHECK(untilRunning());
  CHECK(g.generateTheorySet(0));                               // queued behind the running generate, not refused
  CHECK(g.generateTheorySet(0));                               // a repeat for the queued set coalesces
  int steps = runUntilIdle(86 * 120);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK(g.theorySet(0));
  CHECK(strncmp(g.info(Chord).file, "THEORY ", 7) == 0); CHECK(strncmp(g.info(Pad).file, "THEORY ", 7) == 0);
  std::printf("  GENERATE S1 + THEORY S1 queued while it ran: both jobs in %d steps, progression %d\n", steps, g.theoryProgression(0));
  // THEORY S1 then GENERATE S1 while the theory job runs: an ordinary set again, in that order
  CHECK(g.generateTheorySet(0)); CHECK(untilRunning());
  CHECK(!g.generateTheorySet(0));                              // the very job running: the double press is the one refusal left
  CHECK(g.generateSet(0));
  runUntilIdle(86 * 120);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK(!g.theorySet(0)); CHECK_EQ(g.theoryProgression(0), -1);
  CHECK(strncmp(g.info(Chord).file, "THEORY ", 7) != 0);
  // THEORY S1 then THEORY S2 one "bar" later, while S1 still runs: both sets become theory sets (two independent plans)
  CHECK(g.generateTheorySet(0)); CHECK(untilRunning());
  CHECK(g.generateTheorySet(1));                               // the other set queues behind the running theory job
  CHECK(!g.generateTheorySet(0));                              // ... the running one stays the no-op double press
  steps = runUntilIdle(86 * 120);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK(g.theorySet(0)); CHECK(g.theorySet(1));
  CHECK(g.theoryProgression(0) >= 0); CHECK(g.theoryProgression(1) >= 0);
  CHECK(strncmp(g.info(kTracksPerSet + Chord).file, "THEORY ", 7) == 0);
  std::printf("  THEORY S1 + THEORY S2 queued while S1 ran: both in %d steps, progressions %d / %d\n", steps, g.theoryProgression(0), g.theoryProgression(1));
  // a queued THEORY request behind a queued GENERATE of the other set keeps its place too (two requests waiting at once)
  CHECK(g.generateSet(1)); CHECK(untilRunning());
  CHECK(g.generateSet(0)); CHECK(g.generateTheorySet(1));      // queue: GENERATE S1, THEORY S2 (behind the running GENERATE S2)
  runUntilIdle(86 * 180);
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK(!g.theorySet(0)); CHECK(g.theorySet(1));
  runBlocks(kSampleRate / kBlock);
  CHECK(g.lastError()[0] == 0);
  (void)e;
}

// SPEC §9.24: the theory job needs no card for the melodic tracks — the five patterns are built in pure steps before the
// job reaches the drum picks, kit files and the vocal — and the whole set swaps as one burst with CmdSwapSet, playing tracks
// at the bar like GENERATE.
TEST(theory_melodic_tracks_cost_no_card_work_and_swap_as_one_set) {
  Generator& g = gen(); Engine& e = eng();
  stopAll(); runUntilIdle();
  CHECK(startTrack(Chord)); CHECK(startTrack(Bass));             // playing tracks swap at the bar
  g_cmdLog.cmds.clear(); g_afterPump = recordCmds;
  const uint64_t opens0 = sd().opens, walks0 = sd().walks;
  CHECK(g.generateTheorySet(0));
  int melodicIters = 0;
  while (g.busy() && (g.stateCode() == 0 || g.progress() < 5) && melodicIters < 400) { iterate(); melodicIters++; }   // Chord, Lead, Pad, Bass, Fx come first in the set order (progress() holds the previous job's count until startJob)
  CHECK(melodicIters > 0); CHECK_EQ(g.progress(), 5);
  const uint64_t melodicOpens = sd().opens - opens0, melodicWalks = sd().walks - walks0;
  const int steps = runUntilIdle(86 * 60);
  runBlocks(kSampleRate / kBlock);
  g_afterPump = nullptr;
  CHECK(!g.busy()); CHECK(g.lastError()[0] == 0); CHECK(g.theorySet(0));
  std::printf("  five melodic tracks in %d iterations with %llu file opens and %llu listings; the drums and the vocal in %d more; %d CmdSwapPattern, %d CmdSwapSet, %d CmdSetGenerating\n",
              melodicIters, (unsigned long long)melodicOpens, (unsigned long long)melodicWalks, steps, countCmd(CmdSwapPattern), countCmd(CmdSwapSet), countCmd(CmdSetGenerating));
  CHECK(melodicIters <= 24);                                   // pure transitions chained per step: about one track per loop iteration
  CHECK_EQ((int)melodicOpens, 0); CHECK_EQ((int)melodicWalks, 0);
  CHECK_EQ(countCmd(CmdSwapSet, 0, 0), 1);                      // one set-wide swap
  for (Category c : {Chord, Lead, Pad, Bass, Fx}) CHECK(countCmd(CmdSwapPattern, c) >= 1);
  CHECK(countCmd(CmdSetGenerating, 0, 0) >= 2);                 // on at the start, off after the swap
  CHECK_EQ((int)e.status().track[Chord].state, (int)PSPlaying); CHECK_EQ((int)e.status().track[Bass].state, (int)PSPlaying);
  CHECK(e.activePattern(Chord).count > 0 && e.activePattern(Bass).count > 0);
  CHECK(strncmp(g.info(Chord).file, "THEORY ", 7) == 0);
  stopAll(); runBlocks(4);
  CHECK_EQ(sd().openCount(), 2);
}

TEST(step_budget_and_size) {
  std::printf("  %d loop iterations, %u skipped their card work after a %d-chunk pump; max step() %.2f ms (host wall time)\n", g_steps, (unsigned)gen().stepsSkipped(), Generator::kStepSkipChunks, g_maxStepMs);
  std::printf("  card meter per iteration (step + pump): max %llu B read, %llu opens, %llu directory entries walked; max pump %d chunks\n", (unsigned long long)g_maxIterBytes, (unsigned long long)g_maxIterOpens, (unsigned long long)g_maxIterWalked, g_maxPumpChunks);
  std::printf("  sizeof(Generator) = %zu B (chunk %zu, TrackGenInfo %zu x %d)\n", sizeof(Generator), sizeof(int16_t) * Generator::kChunkSamples, sizeof(TrackGenInfo), kTracks);
  // SPEC §9.19: 9848 B before loop mode + two more stream states (cur / nxt with their 272 B heads) + the remembered pick
  // folders + the loop bookkeeping = 13 184 B at the review; the review trims (one path per stream — the vocal streams'
  // alternate phrase lives in altPath_[kSets] —, the pick folders as two bytes instead of 16 chars) take it to 12 416 B
  // (−768). The nxt heads stay: every stream may hold a pending new-file switch at once (a set job opens the vocal and
  // the set's loop tracks together), so one shared head would be torn. The device pays it from bootBlock heap against the
  // engine's −14.8 KB static reclaim and the +12.3 KB rings (W3 contract §2). SPEC §9.22: the preset bag is a 512-bit seen
  // mask per track (64 B, was order[16] + 4 B) for banks of up to kPresetBagMax presets per family: +704 B, 12 424 → 13 128 B
  // (TrackGenInfo repacked to keep the 16-bit presetIndex free of padding; the wave's budget for the generator + Sets is 0.9 KB).
  // SPEC §9.24 / the 2026-10-16 RAM diet: the bag is a 256-bit mask (32 B per track, banks are <= 255 rows): −512 B; the theory
  // generator adds its 52 B plan + 3 B of set flags (its tables are flash): 13 128 → <= 12 672 B.
  CHECK(sizeof(Generator) <= 13128 - 512 + 64);
  CHECK_EQ(sizeof(TheoryPlan), (size_t)52);
  CHECK_EQ(Generator::kPresetBagMax, 256);
  CHECK_EQ(sizeof(TrackGenInfo), (size_t)160);
  CHECK(g_maxStepMs < kMaxSdBurstMs);
  CHECK(g_maxIterBytes <= (uint64_t)(Generator::kPumpChunks + 1) * 4096);   // SPEC §9.2 / §9.19: 4 pump chunks + one operation; 20 ms at the 1 MB/s floor of io/wav.h
  CHECK(g_maxPumpChunks <= Generator::kPumpChunks);
  CHECK(g_maxIterWalked <= 64);                                // shard scans look at ≤ 32 entries (SPEC §9.2)
  CHECK_EQ(Generator::kPumpChunks, 4);
  CHECK_EQ(sd().openCount(), 2);                               // one phrase file per set stays open while loaded; every loop stream was closed by its test
}

int main() { return lp_test::run(); }
