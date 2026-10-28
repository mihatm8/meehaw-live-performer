// Host tests for rec/recorder.cpp (SPEC §9.23): the session file layout and header, the write-behind buffer, the session
// list / info / delete, and the OFFLINE RENDER — a scripted live performance (key events through the real Ui, recorded by
// the Recorder) replayed in a second, freshly booted "world" (engine, library, generator, sets, Ui) must push the same
// engine command sequence; two fresh worlds rendering the same session must write byte-identical WAVs. The library content
// comes from sdcard/lp through PosixFs (override: LP_SDCARD); everything the recorder and the session writer create
// (/lp/rec, /lp/wav, /lp/state) is routed to a temporary card directory, so the real card image is never touched.
#include "check.h"
#include "rec/recorder.h"
#include "ui/ui.h"
#include "gen/generator.h"
#include "gen/library.h"
#include "gen/sets.h"
#include "engine/engine.h"
#include "dsp/ulaw.h"
#include "../firmware/desktop/posix_fs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace lp;

// ---------------------------------------------------------------------------------------------- card routing

static std::string sdRoot() {
  const char* env = getenv("LP_SDCARD");
  if (env && *env) return env;
  struct stat st;
  if (stat("sdcard/lp", &st) == 0) return "sdcard";
  return "/Users/miha/Documents/cardputer/liveplayer/sdcard";
}
static std::string g_tmpRoot;                          // the temporary card: /lp/rec, /lp/wav, /lp/state live here

// /lp/rec, /lp/wav and /lp/state go to the temporary root; the library (midi, packs) is read from the real card image.
// Files under /lp/rec count their write() calls and bytes (the write-behind contract: one bounded write per step), since
// the host's stdio buffering hides a still-open file's size from a second reader (the device writes through the VFS).
struct RouteFs;
struct CountFile : IFile {
  IFile* in = nullptr; RouteFs* fs = nullptr; bool rec = false;   // rec: a /lp/rec file (its writes and syncs are counted)
  bool valid() const override { return in && in->valid(); }
  size_t read(void* dst, size_t n) override { return in->read(dst, n); }
  size_t write(const void* src, size_t n) override;
  bool sync() override;
  bool seek(uint32_t pos) override { return in->seek(pos); }
  uint32_t position() const override { return in->position(); }
  uint32_t size() const override { return in->size(); }
  void close() override { if (in) { in->close(); in = nullptr; } }
};
// Every open goes through a CountFile so a deleted World's handles (a Generator never closes its streams: the device
// never deletes one) can be released by closeAll() — otherwise a third or fourth world in one test finds the shared
// 7-handle pools full and its generate waits for a handle it never gets.
struct RouteFs : IFileSystem {
  PosixFs card, tmp; CountFile pool[2 * PosixFs::kMaxOpen];
  int recWrites = 0, recSyncs = 0; size_t recBytes = 0;
  // Read opens of /lp/rec files (sessionInfo, the render's transient chunk reads) are counted; the failOpenRun opens from
  // index failOpenAt on answer nullptr — the device's 7-handle pool full for a few steps (SPEC §9.19g).
  int recOpens = 0, failOpenAt = -1, failOpenRun = 0, failedOpens = 0;
  RouteFs(const char* c, const char* t) : card(c), tmp(t) {}
  static bool routed(const char* p) { return p && (strncmp(p, "/lp/rec", 7) == 0 || strncmp(p, "/lp/wav", 7) == 0 || strncmp(p, "/lp/state", 9) == 0); }
  IFileSystem& of(const char* p) { return routed(p) ? static_cast<IFileSystem&>(tmp) : static_cast<IFileSystem&>(card); }
  bool exists(const char* p) override { return of(p).exists(p); }
  bool isDir(const char* p) override { return of(p).isDir(p); }
  IFile* open(const char* p, const char* mode = "r") override {
    if (strncmp(p, "/lp/rec/", 8) == 0 && (!mode || mode[0] == 'r')) {
      const int k = recOpens++;
      if (failOpenAt >= 0 && k >= failOpenAt && k < failOpenAt + failOpenRun) { failedOpens++; return nullptr; }
    }
    IFile* f = of(p).open(p, mode); if (!f) return nullptr;
    for (CountFile& w : pool) if (!w.in) { w.in = f; w.fs = this; w.rec = strncmp(p, "/lp/rec", 7) == 0; return &w; }
    f->close(); return nullptr;
  }
  bool mkdir(const char* p) override { return of(p).mkdir(p); }
  bool remove(const char* p) override { return of(p).remove(p); }
  bool walk(const char* p, DirEntryFn fn, void* user) override { return of(p).walk(p, fn, user); }
  int openCount() const override { return card.openCount() + tmp.openCount(); }
  void noFailures() { failOpenAt = -1; failOpenRun = 0; }
  void closeAll() { for (CountFile& w : pool) if (w.in) w.close(); }
};
size_t CountFile::write(const void* src, size_t n) { if (rec) { fs->recWrites++; fs->recBytes += n; } return in->write(src, n); }
bool CountFile::sync() { if (rec) fs->recSyncs++; return in->sync(); }
static RouteFs& fs() {
  static RouteFs* f = nullptr;
  if (!f) {
    char tpl[256]; const char* td = getenv("TMPDIR"); snprintf(tpl, sizeof tpl, "%s/lp_rec_test_XXXXXX", td && *td ? td : "/tmp");
    char* d = mkdtemp(tpl); if (!d) { perror("mkdtemp"); exit(2); }
    g_tmpRoot = d; ::mkdir((g_tmpRoot + "/lp").c_str(), 0755);
    f = new RouteFs(sdRoot().c_str(), g_tmpRoot.c_str());
  }
  return *f;
}
static std::string tmpPath(const char* card) { return g_tmpRoot + card; }
static bool tmpExists(const char* card) { struct stat st; return stat(tmpPath(card).c_str(), &st) == 0; }
static std::vector<uint8_t> readTmp(const char* card) {
  std::vector<uint8_t> v; FILE* f = fopen(tmpPath(card).c_str(), "rb"); if (!f) return v;
  uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n); fclose(f); return v;
}
static void clearDir(const char* card) {
  const std::string d = tmpPath(card); DIR* dir = opendir(d.c_str()); if (!dir) return;
  while (dirent* e = readdir(dir)) { if (e->d_name[0] == '.') continue; ::remove((d + "/" + e->d_name).c_str()); }
  closedir(dir);
}
static void resetCard() { fs(); clearDir("/lp/rec"); clearDir("/lp/wav"); clearDir("/lp/state"); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16); }

// ---------------------------------------------------------------------------------------------- a world

struct NullGfx : IGfx {
  int width() const override { return 240; }
  int height() const override { return 135; }
  void fillScreen(Color) override {}
  void fillRect(int, int, int, int, Color) override {}
  void drawRect(int, int, int, int, Color) override {}
  void drawLine(int, int, int, int, Color) override {}
  void drawText(int, int, const char*, int, Color) override {}
  int textWidth(const char* t, int size) override { return 6 * size * (int)strlen(t); }
};

// The sketch's parallel-render hooks, modelled: the "worker" runs the set-2 job inline and remembers which task asked
// (g_hookCaller: 2 = the render task's live blocks, 1 = the loop task's Recorder::renderStep), the join checks that it is
// the task that started the job — what liveplayer.ino's parallelStart / workerTask (g_jobOwner) rely on. Installed by one
// test only (lp::g_parallelStart / g_parallelJoin are null otherwise, as in every other host test).
static int g_hookCaller = 0, g_hookOwner = 0, g_hookStarts = 0, g_hookJoins = 0, g_hookBadJoins = 0, g_hookLoopStarts = 0;
static void stubParallelStart(ParallelFn fn, void* arg) { g_hookStarts++; g_hookOwner = g_hookCaller; if (g_hookCaller == 1) g_hookLoopStarts++; fn(arg); }
static void stubParallelJoin() { g_hookJoins++; if (g_hookOwner != g_hookCaller) g_hookBadJoins++; }

// One freshly booted device: engine, library, generator (genre House), sets, Ui, recorder. Live blocks mirror the
// sketch's loop() and Recorder::renderStep (render, pump + steps, Ui tick, then the key events of the iteration); the
// command log holds what the engine consumed, drained and re-pushed right before every render.
struct World {
  Engine* eng; Library lib; Generator gen; Sets sets; Ui ui; Recorder rec; NullGfx gfx;
  int16_t ring[kLoopSlots][kLoopRingBytes / 2];
  int16_t out[kBlock];
  std::vector<Cmd> log; std::vector<uint32_t> logTick;
  std::vector<int16_t> audio;                        // every live block rendered while `capture`
  bool capture = false;
  World() {
    static bool ulaw = false; if (!ulaw) { ulawInit(); ulaw = true; }
    eng = new Engine(); eng->init();
    for (int k = 0; k < kLoopSlots; k++) eng->setLoopRing(k, ring[k], kLoopRingBytes / 2);
    lib.init(&fs()); gen.init(&fs(), eng, &lib); gen.setGenreByName("House");
    sets.init(&fs(), eng, &gen, &lib); ui.init(&gfx, eng, &gen, &sets);
    rec.init(&fs(), eng, &gen, &sets, &ui);
    ui.setRecorder(&rec);                              // as the sketch wires it: the Ui records its keys / volume itself and drives the recorder page
  }
  ~World() { delete eng; fs().closeAll(); }           // the generator's streams stay open otherwise (see RouteFs)
  uint32_t nowMs() const { return (uint32_t)((uint64_t)eng->status().sampleClock * 1000u / kSampleRate); }
  uint32_t tick() const { return eng->status().tick; }
  void push(CmdType t, int track = 0, int a = 0, int32_t i = 0, float f = 0) { Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f; eng->cmds().push(c); }
  void drain() { Cmd c; std::vector<Cmd> v; while (eng->cmds().pop(c)) v.push_back(c); for (const Cmd& k : v) { eng->cmds().push(k); log.push_back(k); logTick.push_back(tick()); } }
  std::vector<Cmd> peek() { Cmd c; std::vector<Cmd> v; while (eng->cmds().pop(c)) v.push_back(c); for (const Cmd& k : v) eng->cmds().push(k); return v; }   // the queue as it stands, not logged (drain() logs; a second drain before a render would log the same commands twice)
  void liveBlock() {
    g_hookCaller = 2; drain(); eng->render(out, kBlock);
    if (capture) audio.insert(audio.end(), out, out + kBlock);
    gen.pumpVocal(); gen.step(); sets.step(); ui.tick(nowMs()); rec.step();
  }
  void liveBlocks(int n) { for (int i = 0; i < n; i++) liveBlock(); }
  void key(int x, int y, KeyEvent::Kind kind) {         // a matrix key as the sketch delivers it; the Ui records it while a recording runs (Ui::recordKey)
    KeyEvent e; e.x = (uint8_t)x; e.y = (uint8_t)y; e.kind = kind; e.ms = nowMs(); e.heldMs = 0; ui.onKey(e, nowMs());
  }
  void tap(int x, int y) { key(x, y, KeyEvent::Press); key(x, y, KeyEvent::Release); }
  void uiOnly(int x, int y, KeyEvent::Kind kind) { key(x, y, kind); }   // a key before / outside a recording (nothing records it)
  bool replayBlock() { g_hookCaller = 1; drain(); return rec.renderStep(out); }
  int replayAll() { int n = 0; while (replayBlock() && n < 200000) n++; return n; }
  int firstSetTempo120() { push(CmdSetTempo, 0, 0, 120); liveBlock(); return (int)eng->status().bpm; }   // the suites reason at 120 BPM
};
constexpr int kBlocksPerBar120 = 173;                   // 44 100 samples per bar at 120 BPM ≈ 172.3 blocks
// startRender's command burst: CmdResetAll, MasterPlay 0 / 1, SetTempo, 16 × TrackStopNow, TrackStart per recorded play-mask
// bit, then restoreLive's CmdKickCut × kSets and CmdGyro (header reserved[5]); a fresh world's Ui holds no ramps, so no CmdCancelRamps.
constexpr int kBurstBase = 4 + kTracks + kSets + 1;

static bool sameCmd(const Cmd& a, const Cmd& b) { return a.type == b.type && a.track == b.track && a.a == b.a && a.i == b.i && memcmp(&a.f, &b.f, sizeof a.f) == 0; }
static int countType(const std::vector<Cmd>& v, CmdType t, int a = -1) { int n = 0; for (const Cmd& c : v) if (c.type == t && (a < 0 || c.a == a)) n++; return n; }

// ---------------------------------------------------------------------------------------------- tests

TEST(recorder_fits_its_budget) {
  // 448 B on the device (32-bit pointers; measured 416 B with the Xtensa toolchain after the 2026-10-16 RAM diet, 672 B before);
  // the host carries seven 8-byte pointers. The diet's levers here: the write-behind buffer kBuf 64 → 32 events (512 → 256 B)
  // and the session list as two 64-bit masks + one 8-byte name buffer (no 64 × 8 B name table).
  CHECK(sizeof(Recorder) <= 448 + 7 * (sizeof(void*) - 4));
  CHECK_EQ(Recorder::kBuf, 32); CHECK_EQ(sizeof(RecEvent), 8u); CHECK_EQ(sizeof(RecHeader), 64u);
  printf("  sizeof(Recorder) = %zu B on this host (%zu-byte pointers)\n", sizeof(Recorder), sizeof(void*));
}

TEST(record_20_events_file_layout_and_header) {
  resetCard();
  World* w = new World();
  CHECK_EQ(w->firstSetTempo120(), 120);
  CHECK_EQ(w->rec.sessionCount(), 0);
  CHECK(!tmpExists("/lp/rec"));
  const uint32_t startTick = w->tick();
  fs().recWrites = 0; fs().recBytes = 0;
  CHECK(w->rec.startRecording()); CHECK(w->rec.recording());
  CHECK(tmpExists("/lp/rec/rec_000.lprec"));
  CHECK_EQ(fs().recWrites, 1); CHECK_EQ(fs().recBytes, 64u);   // the header, one write
  // 20 events of every type; the recorder stamps the engine tick itself (relative to the start) and step() flushes each
  // buffered event with ONE write while no stream is consumed (the gate is open)
  uint32_t expectTick[20];
  for (int i = 0; i < 20; i++) {
    w->liveBlocks(3);
    RecEvent e; e.tick = 12345; e.type = (uint8_t)(i % 4); e.x = (uint8_t)(i % 4 == 2 ? (uint8_t)(int8_t)(-40 + i) : i); e.y = (uint8_t)(i & 3); e.extra = (uint8_t)(100 + i);
    expectTick[i] = w->tick() - startTick;
    const int before = fs().recWrites;
    w->rec.onInput(e);
    CHECK_EQ(fs().recWrites, before);                    // buffered, not written
    w->liveBlock();
    CHECK_EQ(fs().recWrites, before + 1); CHECK_EQ(fs().recBytes, 64u + 8u * (i + 1));
  }
  CHECK_EQ(w->rec.recordedEvents(), 20u);
  w->liveBlocks(5);
  CHECK_EQ(fs().recWrites, 21);                          // nothing buffered: no write
  CHECK_EQ(fs().recSyncs, 21);                           // the header and every flush were synced (the size reaches the card before the next event)
  { IFile* second = fs().open("/lp/rec/rec_000.lprec", "r"); CHECK(second != nullptr);   // a second handle while the recording is still open sees every flushed event
    if (second) { CHECK_EQ(second->size(), 64u + 160u); second->close(); } }
  const uint32_t len = w->tick() - startTick;
  w->rec.stopRecording();
  CHECK(!w->rec.recording());
  CHECK_EQ(fs().recWrites, 22); CHECK_EQ(fs().recBytes, 64u + 160u + 8u);   // the counts: one 8-byte write at offset 48 (closed right after: no extra sync)
  CHECK_EQ(fs().recSyncs, 21);
  std::vector<uint8_t> f = readTmp("/lp/rec/rec_000.lprec");
  CHECK_EQ(f.size(), 64u + 160u);
  if (f.size() == 224) {
    CHECK(memcmp(f.data(), "LPREC1\0\0", 8) == 0);
    CHECK(rd32(&f[8]) != 0);                             // seed
    CHECK_EQ(rd16(&f[12]), 120u);                        // bpm
    CHECK_EQ((int8_t)f[14], 9);                          // A minor
    CHECK_EQ(f[15], 0u);                                 // nothing played in set 1
    CHECK(strcmp((const char*)&f[16], "House") == 0);
    CHECK_EQ(rd16(&f[56]), 0u);                          // play mask
    CHECK_EQ(rd16(&f[58]), startTick % kTicksPerBar);    // bar phase of the start
    CHECK_EQ(rd32(&f[48]), 20u); CHECK_EQ(rd32(&f[52]), len);
    for (int i = 0; i < 20; i++) {
      const uint8_t* e = &f[64 + 8 * i];
      CHECK_EQ(rd32(e), expectTick[i]); CHECK_EQ(e[4], (uint8_t)(i % 4)); CHECK_EQ(e[5], (uint8_t)(i % 4 == 2 ? (uint8_t)(int8_t)(-40 + i) : i)); CHECK_EQ(e[6], (uint8_t)(i & 3)); CHECK_EQ(e[7], (uint8_t)(100 + i));
    }
  }
  CHECK_EQ(w->rec.sessionCount(), 1);
  CHECK(strcmp(w->rec.sessionName(0), "rec_000") == 0);
  CHECK(strcmp(w->rec.sessionName(1), "") == 0);
  RecHeader h; CHECK(w->rec.sessionInfo(0, h));
  CHECK_EQ(h.events, 20u); CHECK_EQ(h.lengthTicks, len); CHECK_EQ(h.bpm, 120u); CHECK(strcmp(h.genre, "House") == 0);
  CHECK_EQ(h.reserved[4], (uint8_t)Beat); CHECK_EQ(h.reserved[5], 0u); CHECK_EQ(h.reserved[6], (uint8_t)kRampBarsDefault); CHECK_EQ(h.reserved[7], 1u);   // the Ui state: default selection, no kick cut, gyro off, 4 bars
  CHECK(!w->rec.sessionInfo(1, h));
  CHECK(!w->rec.hasWav(0));
  // a second world lists the same session from the card
  World* v = new World();
  CHECK_EQ(v->rec.sessionCount(), 1); CHECK(strcmp(v->rec.sessionName(0), "rec_000") == 0);
  delete v; delete w;
}

TEST(a_full_buffer_of_kBuf_events_flushes_inline_without_step_and_the_next_session_takes_the_next_number) {
  resetCard();
  World* w = new World(); w->firstSetTempo120();
  fs().recWrites = 0; fs().recBytes = 0;
  CHECK(w->rec.startRecording());
  CHECK(tmpExists("/lp/rec/rec_000.lprec"));
  RecEvent e; e.tick = 0; e.type = 0; e.x = 1; e.y = 0; e.extra = 0;
  constexpr uint32_t kN = (uint32_t)Recorder::kBuf, kBufBytes = kN * 8u;   // 32 events / 256 B (RAM diet 2026-10-16; 64 / 512 B before)
  for (uint32_t i = 0; i < kN; i++) w->rec.onInput(e);
  CHECK_EQ(fs().recWrites, 1); CHECK_EQ(fs().recBytes, 64u);           // all kBuf buffered (step() was never called)
  w->rec.onInput(e);                                                     // the (kBuf + 1)th: the full buffer goes to the card in ONE kBuf × 8 B write
  CHECK_EQ(fs().recWrites, 2); CHECK_EQ(fs().recBytes, 64u + kBufBytes);
  CHECK_EQ(w->rec.recordedEvents(), kN + 1u);
  w->rec.stopRecording();                                                // the one buffered event, then the counts
  CHECK_EQ(fs().recWrites, 4); CHECK_EQ(fs().recBytes, 64u + kBufBytes + 8u + 8u);
  CHECK_EQ(readTmp("/lp/rec/rec_000.lprec").size(), 64u + (kN + 1u) * 8u);
  { std::vector<uint8_t> f = readTmp("/lp/rec/rec_000.lprec"); CHECK_EQ(rd32(&f[48]), kN + 1u); }
  CHECK(w->rec.startRecording());
  CHECK(tmpExists("/lp/rec/rec_001.lprec"));
  CHECK(!w->rec.startRecording());                                       // already recording
  CHECK(!w->rec.startRender(0));                                         // not while recording
  CHECK(!w->rec.deleteSession(0));                                       // not while recording
  w->rec.stopRecording();
  CHECK_EQ(w->rec.sessionCount(), 2);
  CHECK(strcmp(w->rec.sessionName(1), "rec_001") == 0);
  CHECK(w->rec.deleteSession(0));                                        // the gap is reused by the next recording
  CHECK(!tmpExists("/lp/rec/rec_000.lprec")); CHECK(tmpExists("/lp/rec/rec_001.lprec"));
  CHECK_EQ(w->rec.sessionCount(), 1); CHECK(strcmp(w->rec.sessionName(0), "rec_001") == 0);
  CHECK(w->rec.startRecording()); w->rec.stopRecording();
  CHECK(tmpExists("/lp/rec/rec_000.lprec")); CHECK_EQ(w->rec.sessionCount(), 2);
  CHECK(!w->rec.deleteSession(2)); CHECK(!w->rec.startRender(-1)); CHECK(!w->rec.startRender(2));
  delete w;
}

TEST(sixty_four_sessions_refuse_a_new_recording) {
  resetCard();
  fs().mkdir("/lp/rec");
  for (int n = 0; n < Recorder::kMaxSessions; n++) {
    char p[64]; snprintf(p, sizeof p, "/lp/rec/rec_%03d.lprec", n);
    FILE* f = fopen(tmpPath(p).c_str(), "wb"); uint8_t h[64] = {'L', 'P', 'R', 'E', 'C', '1'}; fwrite(h, 1, 64, f); fclose(f);
  }
  FILE* junk = fopen(tmpPath("/lp/rec/notes.txt").c_str(), "wb"); fputs("x", junk); fclose(junk);   // ignored by the listing
  World* w = new World(); w->firstSetTempo120();
  CHECK_EQ(w->rec.sessionCount(), Recorder::kMaxSessions);
  CHECK(strcmp(w->rec.sessionName(63), "rec_063") == 0);
  CHECK(!w->rec.startRecording());
  CHECK(w->rec.deleteSession(17));
  CHECK(w->rec.startRecording()); w->rec.stopRecording();
  CHECK(tmpExists("/lp/rec/rec_017.lprec"));
  delete w;
}

// The scripted performance: GENERATE S1, three track taps, a vol+ hold-ramp, tempo+, a SHIFT + rev+ ramp with its
// snap-back, a gyro command, an output volume, a pattern key. Returns the live log after the recording started.
static void perform(World& a) {
  a.tap(13, 2);                                          // GENERATE S1 (enter)
  a.liveBlocks(4 * kBlocksPerBar120);                    // the set job finishes and swaps at a bar line
  a.tap(0, 0); a.tap(7, 0); a.tap(3, 0);                 // CHD, BEAT, BASS start at the next bar (and BASS is selected)
  a.liveBlocks(kBlocksPerBar120);
  a.key(0, 2, KeyEvent::Press); a.liveBlocks(10); a.key(0, 2, KeyEvent::Release);   // vol+ hold-ramp on the Bass, frozen on release
  a.liveBlocks(20);
  a.tap(6, 2);                                           // tempo+ → 121
  a.liveBlocks(20);
  a.key(13, 3, KeyEvent::Press); a.tap(1, 2); a.liveBlocks(30); a.key(13, 3, KeyEvent::Release);   // SHIFT + rev+ ramp, snapped back on SHIFT release
  a.liveBlocks(20);
  { RecEvent g; g.tick = 0; g.type = 2; g.x = (uint8_t)(int8_t)-64; g.y = 200; g.extra = 50;      // a gyro command (as the Ui records its CmdGyro)
    a.push(CmdGyro, 0, 1, (200 << 8) | 50, (float)(int8_t)g.x / 127.0f); a.rec.onInput(g); }
  a.liveBlocks(20);
  a.ui.setOutputVolume(90);                              // the Ui records the applied level as type 3 itself (Ui::recordVolume)
  a.liveBlocks(20);
  a.tap(9, 0);                                           // pat2 on set 1's Beat
  a.liveBlocks(2 * kBlocksPerBar120);
}

TEST(replay_reproduces_the_live_command_sequence_and_the_wav_is_deterministic) {
  resetCard();
  World* a = new World();
  CHECK_EQ(a->firstSetTempo120(), 120);
  const uint32_t startTick = a->tick();
  const size_t logStart = a->log.size();
  CHECK(a->rec.startRecording());
  perform(*a);
  CHECK(a->rec.recordedEvents() >= 20u);
  a->rec.stopRecording();
  RecHeader h; CHECK(a->rec.sessionInfo(0, h));
  CHECK_EQ(h.events, a->rec.recordedEvents());
  const uint32_t phase = rd16(h.reserved + 2);
  CHECK_EQ(phase, startTick % kTicksPerBar);
  std::vector<Cmd> live(a->log.begin() + (long)logStart, a->log.end());
  std::vector<uint32_t> liveTick(a->logTick.begin() + (long)logStart, a->logTick.end());
  CHECK(countType(live, CmdSwapSet) >= 1);               // the generate landed live
  CHECK(countType(live, CmdTrackToggle) == 3);
  CHECK(countType(live, CmdFreezeRamp) >= 1); CHECK(countType(live, CmdCancelRamps) == 1); CHECK(countType(live, CmdGyro) == 1);
  delete a;

  // A second boot replays the session: the same commands in the same order, each within a few ticks of its live position.
  World* b = new World();
  CHECK_EQ(b->rec.sessionCount(), 1);
  CHECK(!b->rec.rendering());
  CHECK(b->rec.startRender(0)); CHECK(b->rec.rendering());
  CHECK_EQ(b->rec.renderPercent(), 0);
  const int steps = b->replayAll();
  CHECK(!b->rec.rendering());
  CHECK(b->rec.hasWav(0));
  const int burst = kBurstBase;                          // no track played at the start: no CmdTrackStart
  CHECK(b->log.size() >= (size_t)burst);
  if (b->log.size() >= (size_t)burst) {
    CHECK_EQ(b->log[0].type, CmdResetAll); CHECK_EQ(b->log[1].type, CmdMasterPlay); CHECK_EQ(b->log[1].a, 0u); CHECK_EQ(b->log[2].type, CmdMasterPlay); CHECK_EQ(b->log[2].a, 1u);
    CHECK_EQ(b->log[3].type, CmdSetTempo); CHECK_EQ(b->log[3].i, 120);
    CHECK_EQ(b->log[burst - 3].type, CmdKickCut); CHECK_EQ(b->log[burst - 2].type, CmdKickCut); CHECK_EQ(b->log[burst - 1].type, CmdGyro); CHECK_EQ(b->log[burst - 1].a, 0u);   // restoreLive: both kick cuts off, gyro off
  }
  std::vector<Cmd> rep(b->log.begin() + burst, b->log.end());
  std::vector<uint32_t> repTick(b->logTick.begin() + burst, b->logTick.end());
  const uint32_t base = kTicksPerBar + phase;            // the replay's tick base in a fresh engine: one bar of pre-roll, then the recorded phase
  CHECK(rep.size() >= live.size());
  int mismatches = 0, lateBy = 0;
  for (size_t i = 0; i < live.size() && i < rep.size(); i++) {
    if (!sameCmd(live[i], rep[i])) { if (mismatches++ < 5) printf("  cmd %zu differs: live type %d t%d a%d i%d f%g / replay type %d t%d a%d i%d f%g\n", i, live[i].type, live[i].track, live[i].a, live[i].i, live[i].f, rep[i].type, rep[i].track, rep[i].a, rep[i].i, rep[i].f); }
    const int32_t dl = (int32_t)(liveTick[i] - startTick), dr = (int32_t)(repTick[i] - base);
    if (dr - dl > 4 || dl - dr > 4) { if (lateBy++ < 5) printf("  cmd %zu type %d at live tick %d, replay tick %d\n", i, live[i].type, dl, dr); }
  }
  CHECK_EQ(mismatches, 0); CHECK_EQ(lateBy, 0);
  printf("  live %zu commands, replay %zu (+%zu in the tail bar), %d render steps\n", live.size(), rep.size(), rep.size() - live.size(), steps);
  // the WAV: 22050 Hz mono int16, header patched, (lengthTicks + one bar) of blocks, not silent
  std::vector<uint8_t> wav = readTmp("/lp/wav/rec_000.wav");
  CHECK(wav.size() > 44u);
  if (wav.size() > 44) {
    CHECK(memcmp(&wav[0], "RIFF", 4) == 0); CHECK_EQ(rd32(&wav[4]), (uint32_t)wav.size() - 8); CHECK(memcmp(&wav[8], "WAVEfmt ", 8) == 0);
    CHECK_EQ(rd32(&wav[16]), 16u); CHECK_EQ(rd16(&wav[20]), 1u); CHECK_EQ(rd16(&wav[22]), 1u); CHECK_EQ(rd32(&wav[24]), (uint32_t)kSampleRate);
    CHECK_EQ(rd32(&wav[28]), (uint32_t)kSampleRate * 2); CHECK_EQ(rd16(&wav[32]), 2u); CHECK_EQ(rd16(&wav[34]), 16u); CHECK(memcmp(&wav[36], "data", 4) == 0);
    const uint32_t data = rd32(&wav[40]);
    CHECK_EQ(data, (uint32_t)wav.size() - 44); CHECK_EQ(data % 512u, 0u);
    // length: the recorded span at 120 then 121 BPM plus one bar of tail — between the two tempos' sample counts, ± 2 blocks
    const double ticks = (double)h.lengthTicks + kTicksPerBar;
    const double lo = ticks * kSampleRate * 60.0 / (121.0 * kPPQN) - 2 * kBlock, hi = ticks * kSampleRate * 60.0 / (120.0 * kPPQN) + 2 * kBlock;
    CHECK(data / 2 >= lo && data / 2 <= hi);
    int loud = 0; for (size_t i = 44; i + 1 < wav.size(); i += 2) { int16_t s = (int16_t)rd16(&wav[i]); if (s > 300 || s < -300) loud++; }
    CHECK(loud > 1000);
  }
  b->drain(); CHECK_EQ(b->log.back().type, CmdMasterPlay); CHECK_EQ(b->log.back().a, 0u);   // the replayed performance does not go on playing
  b->eng->render(b->out, kBlock); CHECK_EQ(b->eng->status().running, 0u);
  delete b;

  // A third boot renders the same session again: byte-identical.
  World* c = new World();
  CHECK(c->rec.startRender(0)); c->replayAll();
  std::vector<uint8_t> wav2 = readTmp("/lp/wav/rec_000.wav");
  CHECK_EQ(wav2.size(), wav.size());
  CHECK(wav2 == wav);
  delete c;
}

TEST(play_mask_and_phase_restart_the_running_tracks_after_a_bar_of_preroll) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  // a pattern on set 1's Chord track, playing before the recording starts
  CHECK(a->eng->beginNextWrite(Chord)); { Pattern& p = a->eng->nextPattern(Chord); p.clear(kTicksPerBar); p.add(Note::make(0, 96, 60, 100)); p.add(Note::make(192, 96, 64, 100)); } a->eng->endNextWrite(Chord);
  a->push(CmdSwapPatternNow, Chord); a->liveBlock(); a->push(CmdTrackStartNow, Chord); a->liveBlocks(100);
  CHECK_EQ(a->eng->status().track[Chord].state, PSPlaying);
  const uint32_t startTick = a->tick();
  CHECK(a->rec.startRecording());
  a->tap(7, 0); a->liveBlocks(60); a->tap(7, 0); a->liveBlocks(60);
  a->rec.stopRecording();
  RecHeader h; CHECK(a->rec.sessionInfo(0, h));
  CHECK_EQ(h.set1Autoplay, 1u); CHECK_EQ(rd16(h.reserved), 1u << Chord); CHECK_EQ(rd16(h.reserved + 2), startTick % kTicksPerBar);
  delete a;
  World* b = new World();
  CHECK(b->rec.startRender(0));
  b->drain();
  CHECK_EQ(countType(b->log, CmdTrackStart), 1); CHECK_EQ((int)b->log.size(), kBurstBase + 1);
  { const Cmd& ts = b->log[4 + kTracks]; CHECK_EQ(ts.type, CmdTrackStart); CHECK_EQ(ts.track, (uint8_t)Chord); }   // right after the 16 TrackStopNow, before restoreLive's kick cuts / gyro
  CHECK_EQ(b->log.back().type, CmdGyro);
  // pre-roll: the percentage stays 0 for one bar plus the phase (and the first 1 % of the span, ≈ 3 blocks), then climbs;
  // the WAV holds the span + a bar
  int zeroSteps = 0; while (b->rec.renderPercent() == 0 && b->replayBlock() && zeroSteps < 1000) zeroSteps++;
  const int expectPre = (int)(((double)kTicksPerBar + startTick % kTicksPerBar) * kSampleRate * 60.0 / (120.0 * kPPQN) / kBlock);
  const int onePercent = (int)(((double)h.lengthTicks + kTicksPerBar) / 100.0 * kSampleRate * 60.0 / (120.0 * kPPQN) / kBlock) + 1;
  CHECK(zeroSteps >= expectPre - 1 && zeroSteps <= expectPre + onePercent + 1);
  const int rest = b->replayAll();
  CHECK(!b->rec.rendering());
  std::vector<uint8_t> wav = readTmp("/lp/wav/rec_000.wav");
  const double span = ((double)h.lengthTicks + kTicksPerBar) * kSampleRate * 60.0 / (120.0 * kPPQN);
  CHECK(wav.size() > 44u && (wav.size() - 44) / 2 >= span - 2 * kBlock && (wav.size() - 44) / 2 <= span + 2 * kBlock);
  printf("  pre-roll %d steps (expected %d), %d more; wav %zu B\n", zeroSteps, expectPre, rest, wav.size());
  delete b;
}

TEST(cancel_leaves_no_partial_wav_and_a_render_can_start_again) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording()); a->tap(7, 0); a->liveBlocks(kBlocksPerBar120); a->rec.stopRecording();
  World* b = new World();
  CHECK(b->rec.startRender(0)); for (int i = 0; i < 40; i++) CHECK(b->replayBlock());
  CHECK(b->rec.rendering()); CHECK(tmpExists("/lp/wav/rec_000.wav"));
  b->rec.cancelRender();
  CHECK(!b->rec.rendering()); CHECK(!tmpExists("/lp/wav/rec_000.wav")); CHECK(!b->rec.hasWav(0));
  CHECK(!b->rec.renderStep(b->out));
  b->drain(); CHECK_EQ(b->log.back().type, CmdMasterPlay); CHECK_EQ(b->log.back().a, 0u);
  CHECK(b->rec.startRender(0)); b->replayAll();
  CHECK(b->rec.hasWav(0)); CHECK(tmpExists("/lp/wav/rec_000.wav"));
  CHECK(b->rec.deleteSession(0));
  CHECK(!tmpExists("/lp/wav/rec_000.wav")); CHECK(!tmpExists("/lp/rec/rec_000.lprec")); CHECK_EQ(b->rec.sessionCount(), 0);
  delete a; delete b;
}

TEST(a_file_without_counts_is_replayable_to_its_last_event) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording());
  uint32_t lastTick = 0;
  for (int i = 0; i < 5; i++) { a->liveBlocks(30); a->tap(7, 0); lastTick = a->tick(); }
  a->liveBlocks(kBlocksPerBar120);
  a->rec.stopRecording();
  RecHeader h; CHECK(a->rec.sessionInfo(0, h)); CHECK_EQ(h.events, 10u);
  const uint32_t startTick = lastTick - h.lengthTicks + (a->tick() - lastTick);   // reconstruct: lengthTicks = stopTick - startTick
  (void)startTick;
  // power loss before stopRecording: the counts stay 0
  { FILE* f = fopen(tmpPath("/lp/rec/rec_000.lprec").c_str(), "r+b"); uint8_t z[8] = {0}; fseek(f, 48, SEEK_SET); fwrite(z, 1, 8, f); fclose(f); }
  World* b = new World();
  RecHeader t; CHECK(b->rec.sessionInfo(0, t));
  CHECK_EQ(t.events, 10u);
  std::vector<uint8_t> f = readTmp("/lp/rec/rec_000.lprec");
  CHECK_EQ(t.lengthTicks, rd32(&f[64 + 9 * 8]));        // the last event's tick
  CHECK(t.lengthTicks < h.lengthTicks);
  CHECK(b->rec.startRender(0)); b->replayAll();
  CHECK(!b->rec.rendering()); CHECK(b->rec.hasWav(0));
  std::vector<uint8_t> wav = readTmp("/lp/wav/rec_000.wav");
  const double span = ((double)t.lengthTicks + kTicksPerBar) * kSampleRate * 60.0 / (120.0 * kPPQN);
  CHECK(wav.size() > 44u && (wav.size() - 44) / 2 >= span - 2 * kBlock && (wav.size() - 44) / 2 <= span + 2 * kBlock);
  delete a; delete b;
}

TEST(held_key_repeats_are_synthesised_on_replay) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording());
  a->liveBlocks(10);
  a->key(6, 2, KeyEvent::Press); a->liveBlocks(60); a->key(6, 2, KeyEvent::Release);   // tempo+ held ≈ 700 ms: live without a KeyMatrix = one step
  a->liveBlocks(40);
  a->rec.stopRecording();
  CHECK_EQ(a->eng->status().bpm, 121u);
  World* b = new World();
  CHECK(b->rec.startRender(0)); b->replayAll();
  // the replay repeats after 350 ms then every 80 ms (350 430 510 590 670): 4 or 5 repeats on the 11.6 ms step grid
  CHECK(b->eng->status().bpm >= 124u && b->eng->status().bpm <= 126u);
  CHECK(countType(b->log, CmdSetTempo) >= 5);
  printf("  replay bpm %u after a 700 ms tempo+ hold\n", (unsigned)b->eng->status().bpm);
  delete a; delete b;
}

TEST(master_play_pseudo_key_and_a_stopped_transport_still_finish) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording());
  a->liveBlocks(20);
  { RecEvent e; e.tick = 0; e.type = 0; e.x = 255; e.y = 0; e.extra = 0; a->push(CmdMasterPlay, 0, 0); a->rec.onInput(e); }   // the side button: stop
  a->liveBlocks(40);
  { RecEvent e; e.tick = 0; e.type = 0; e.x = 255; e.y = 1; e.extra = 0; a->push(CmdMasterPlay, 0, 1); a->rec.onInput(e); }   // and run again
  a->liveBlocks(40);
  { RecEvent e; e.tick = 0; e.type = 0; e.x = 255; e.y = 0; e.extra = 0; a->push(CmdMasterPlay, 0, 0); a->rec.onInput(e); }   // stopped at the end: the tick freezes
  a->liveBlocks(40);
  a->rec.stopRecording();
  World* b = new World();
  CHECK(b->rec.startRender(0));
  const int steps = b->replayAll();
  CHECK(!b->rec.rendering()); CHECK(b->rec.hasWav(0));
  const int burst = kBurstBase;
  std::vector<Cmd> rep(b->log.begin() + burst, b->log.end());
  CHECK_EQ(countType(rep, CmdMasterPlay, 0), 2); CHECK_EQ(countType(rep, CmdMasterPlay, 1), 1);
  CHECK(steps < 2000);                                   // a frozen tick ends the render after its tail instead of running forever
  delete a; delete b;
}

// The live and replayed command sequences after the recording started (the replay's startRender burst skipped).
static bool sameSequence(const std::vector<Cmd>& live, const std::vector<Cmd>& replay, const char* what) {
  int mismatches = 0;
  for (size_t i = 0; i < live.size() && i < replay.size(); i++)
    if (!sameCmd(live[i], replay[i])) { if (mismatches++ < 5) printf("  %s: cmd %zu differs: live type %d t%d a%d i%d f%g / replay type %d t%d a%d i%d f%g\n", what, i, live[i].type, live[i].track, live[i].a, live[i].i, live[i].f, replay[i].type, replay[i].track, replay[i].a, replay[i].i, replay[i].f); }
  if (replay.size() < live.size()) { printf("  %s: replay has %zu commands, live %zu\n", what, replay.size(), live.size()); return false; }
  return mismatches == 0;
}

// A chunk open that fails (the 7-handle pool full for a step, SPEC §9.19g) stalls the render — nothing renders, nothing
// is dropped or delayed — and the retry next step makes the WAV and the command sequence identical to an undisturbed
// render; an open that keeps failing ends the render WITHOUT a WAV after kOpenRetrySteps steps instead of a shortened one.
TEST(a_failed_chunk_open_is_retried_and_a_persistent_one_ends_the_render_without_a_wav) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording());
  for (int i = 0; i < 40; i++) { a->liveBlocks(9); if (i & 1) a->tap(0, 2); else a->tap(7, 0); }   // 80 events: three kBuf-event chunks (32; two of 64 before the RAM diet)
  const int kChunks = (80 + Recorder::kBuf - 1) / Recorder::kBuf;
  a->liveBlocks(kBlocksPerBar120);
  a->rec.stopRecording();
  CHECK_EQ(a->rec.recordedEvents(), 80u);
  delete a;
  World* b = new World();
  fs().recOpens = 0; fs().noFailures();
  CHECK(b->rec.startRender(0)); const int stepsB = b->replayAll();
  CHECK(!b->rec.rendering()); CHECK(b->rec.hasWav(0));
  CHECK_EQ(fs().recOpens, 1 + kChunks);                   // sessionInfo + one transient open per chunk
  std::vector<Cmd> rep(b->log.begin() + kBurstBase, b->log.end());
  std::vector<uint8_t> wav = readTmp("/lp/wav/rec_000.wav");
  delete b;
  // the second chunk's open fails once: one stalled step, then the same render
  World* c = new World();
  fs().recOpens = 0; fs().failOpenAt = 2; fs().failOpenRun = 1; fs().failedOpens = 0;
  CHECK(c->rec.startRender(0)); const int stepsC = c->replayAll();
  fs().noFailures();
  CHECK(!c->rec.rendering()); CHECK(c->rec.hasWav(0));
  CHECK_EQ(fs().failedOpens, 1); CHECK_EQ(fs().recOpens, 2 + kChunks);   // the failed open counts too
  CHECK(stepsC == stepsB || stepsC == stepsB + 1);       // a failure at the top of a step stalls that step; one inside the delivery loop is retried at the next step's top before its block renders
  std::vector<Cmd> repC(c->log.begin() + kBurstBase, c->log.end());
  CHECK(sameSequence(rep, repC, "one failed open")); CHECK_EQ(repC.size(), rep.size());
  std::vector<uint8_t> wavC = readTmp("/lp/wav/rec_000.wav");
  CHECK(wavC == wav);
  delete c;
  // the second chunk never opens: the render gives up (no WAV, the mask bit clear) within kOpenRetrySteps stalled steps
  World* d = new World();
  fs().recOpens = 0; fs().failOpenAt = 2; fs().failOpenRun = 1 << 20; fs().failedOpens = 0;
  CHECK(d->rec.startRender(0)); const int stepsD = d->replayAll();
  fs().noFailures();
  CHECK(!d->rec.rendering()); CHECK(!d->rec.hasWav(0)); CHECK(!tmpExists("/lp/wav/rec_000.wav"));
  CHECK(fs().failedOpens == 128 || fs().failedOpens == 129);   // Recorder::kOpenRetrySteps stalled steps (+ the first failure inside a delivery loop, which does not count)
  CHECK(stepsD < stepsB);
  d->drain(); CHECK_EQ(d->log.back().type, CmdMasterPlay); CHECK_EQ(d->log.back().a, 0u);
  printf("  undisturbed %d steps, one failed open %d steps, persistent failure gave up after %d steps\n", stepsB, stepsC, stepsD);
  CHECK(d->rec.startRender(0)); d->replayAll(); CHECK(d->rec.hasWav(0));   // and renders fine once the opens work again
  delete d;
}

// A recorded hold of ` (0,0) — 1.5 s, longer than kRenderCancelHoldMs — is a long track-1 press on replay, never the
// cancel hold: the replay sees it as replaying() with rendering() false (Recorder::uiKey), so the Ui arms nothing.
TEST(a_recorded_hold_of_the_cancel_key_does_not_cancel_the_render) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  const size_t logStart = a->log.size();
  CHECK(a->rec.startRecording());
  a->liveBlocks(10);
  a->key(0, 0, KeyEvent::Press); a->liveBlocks(130); a->key(0, 0, KeyEvent::Release);   // ≈ 1.5 s
  a->liveBlocks(40);
  a->tap(7, 0);
  a->liveBlocks(kBlocksPerBar120);
  a->rec.stopRecording();
  std::vector<Cmd> live(a->log.begin() + (long)logStart, a->log.end());
  CHECK_EQ(countType(live, CmdTrackToggle), 2);
  delete a;
  World* b = new World();
  CHECK(b->rec.startRender(0));
  bool everReplaying = false;
  for (int n = 0; n < 200000 && b->replayBlock(); n++) { if (b->rec.replaying()) everReplaying = true; }   // replaying() is false between steps (the sketch's live keys arrive then)
  CHECK(!everReplaying);
  CHECK(!b->rec.rendering()); CHECK(b->rec.hasWav(0)); CHECK(tmpExists("/lp/wav/rec_000.wav"));
  std::vector<Cmd> rep(b->log.begin() + kBurstBase, b->log.end());
  CHECK(sameSequence(live, rep, "cancel key hold"));
  CHECK_EQ(countType(rep, CmdTrackToggle), 2);
  delete b;
}

// The Ui state the recorded keys act on travels in the header (reserved[4..7]) and is restored before the first block:
// a live world with the BASS selected (SHIFT + track, select-only) and a 2-bar ramp length records vol+ / rev+ / SHIFT +
// echo+ ramps; the replay in a fresh world (Beat selected, 4 bars) must push the same commands on the same track.
TEST(the_selected_track_and_ramp_bars_are_recorded_and_restored) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  a->liveBlocks(200);                                    // past the splash
  a->uiOnly(13, 3, KeyEvent::Press); a->uiOnly(3, 0, KeyEvent::Press); a->uiOnly(3, 0, KeyEvent::Release); a->uiOnly(13, 3, KeyEvent::Release);   // SHIFT + BASS: selected, not toggled
  CHECK_EQ(a->ui.selectedTrack(), (int)Bass);
  a->uiOnly(13, 3, KeyEvent::Press); a->uiOnly(5, 3, KeyEvent::Press); a->uiOnly(5, 3, KeyEvent::Release); a->uiOnly(13, 3, KeyEvent::Release);   // SHIFT + len−: 4 → 2 bars
  CHECK_EQ(a->ui.rampBars(), 2);
  a->uiOnly(13, 1, KeyEvent::Press); a->uiOnly(13, 1, KeyEvent::Release);   // kick cut S2 on
  a->liveBlocks(30);
  CHECK(a->eng->status().track[kTracksPerSet + Beat].kickCut);
  const size_t logStart = a->log.size();
  CHECK(a->rec.startRecording());
  RecHeader h; CHECK(a->rec.sessionInfo(0, h));
  CHECK_EQ(h.reserved[4], (uint8_t)Bass); CHECK_EQ(h.reserved[5], 2u); CHECK_EQ(h.reserved[6], 2u); CHECK_EQ(h.reserved[7], 1u);
  a->liveBlocks(10);
  a->tap(0, 2); a->liveBlocks(20);                       // vol+ on the Bass
  a->tap(1, 2); a->liveBlocks(20);                       // rev+ on the Bass
  a->key(13, 3, KeyEvent::Press); a->tap(2, 2); a->liveBlocks(30); a->key(13, 3, KeyEvent::Release);   // SHIFT + echo+: a 2-bar ramp (its length is CmdRamp.i), snapped back
  a->liveBlocks(kBlocksPerBar120);
  a->rec.stopRecording();
  std::vector<Cmd> live(a->log.begin() + (long)logStart, a->log.end());
  int onBass = 0, onOthers = 0; for (const Cmd& c : live) if (c.type == CmdSetParam || c.type == CmdRamp || c.type == CmdFreezeRamp) { if (c.track == Bass) onBass++; else onOthers++; }
  CHECK(onBass >= 3); CHECK_EQ(onOthers, 0);
  int rampTicks = -1; for (const Cmd& c : live) if (c.type == CmdRamp && c.a == PEcho) rampTicks = c.i;
  CHECK_EQ(rampTicks, 2 * kTicksPerBar);
  delete a;
  World* b = new World();
  CHECK_EQ(b->ui.selectedTrack(), (int)Beat); CHECK_EQ(b->ui.rampBars(), kRampBarsDefault);
  CHECK(b->rec.startRender(0));
  CHECK_EQ(b->ui.selectedTrack(), (int)Bass); CHECK_EQ(b->ui.rampBars(), 2);   // restored before the first block
  int cutS2 = -1; { const std::vector<Cmd> q = b->peek(); CHECK_EQ((int)q.size(), kBurstBase); for (const Cmd& c : q) if (c.type == CmdKickCut && c.a == 1) cutS2 = c.i; }
  CHECK_EQ(cutS2, 1);
  b->replayAll();
  CHECK(!b->rec.rendering()); CHECK(b->rec.hasWav(0));
  CHECK(b->eng->status().track[kTracksPerSet + Beat].kickCut);
  std::vector<Cmd> rep(b->log.begin() + kBurstBase, b->log.end());
  CHECK(sameSequence(live, rep, "selected track"));
  delete b;
  // a header without the valid bit (a file written before the fields existed) restores nothing
  { FILE* f = fopen(tmpPath("/lp/rec/rec_000.lprec").c_str(), "r+b"); uint8_t z[4] = {0}; fseek(f, 60, SEEK_SET); fwrite(z, 1, 4, f); fclose(f); }
  World* c = new World();
  CHECK(c->rec.startRender(0));
  CHECK_EQ(c->ui.selectedTrack(), (int)Beat); CHECK_EQ(c->ui.rampBars(), kRampBarsDefault);
  c->rec.cancelRender();
  delete c;
}

// A render started from the RECORDER page (the m key there, as on the device) must replay onto the main page, where the
// recording started (the ent that started it closed the page): startRender closes the page first, and the result is the
// render a world on the main page produces.
TEST(a_render_started_from_the_recorder_page_replays_onto_the_main_page) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording());
  a->liveBlocks(10); a->tap(13, 2);                      // GENERATE S1 (ent — the recorder page's REC toggle, had the page stayed open)
  a->liveBlocks(3 * kBlocksPerBar120); a->tap(7, 0); a->liveBlocks(kBlocksPerBar120);
  a->rec.stopRecording();
  delete a;
  World* b = new World(); b->liveBlocks(200);
  CHECK(b->rec.startRender(0)); b->replayAll(); CHECK(b->rec.hasWav(0));
  std::vector<Cmd> rep(b->log.begin() + kBurstBase, b->log.end());
  std::vector<uint8_t> wav = readTmp("/lp/wav/rec_000.wav");
  CHECK(countType(rep, CmdSwapSet) >= 1);
  delete b;
  World* c = new World(); c->liveBlocks(200);
  c->uiOnly(13, 3, KeyEvent::Press); c->uiOnly(10, 2, KeyEvent::Press); c->uiOnly(10, 2, KeyEvent::Release); c->uiOnly(13, 3, KeyEvent::Release);   // SHIFT + save 1: the recorder page
  CHECK_EQ((int)c->ui.page(), (int)PgRecorder);
  CHECK(c->rec.startRender(0));
  CHECK_EQ((int)c->ui.page(), (int)PgMain);
  c->replayAll(); CHECK(c->rec.hasWav(0));
  std::vector<Cmd> repC(c->log.begin() + kBurstBase, c->log.end());
  CHECK(sameSequence(rep, repC, "from the recorder page")); CHECK_EQ(repC.size(), rep.size());
  CHECK(readTmp("/lp/wav/rec_000.wav") == wav);
  delete c;
}

// A random LOAD inside a recording (load S1: Sets picks one of the genre's saved sets with the Ui's load Rng) must pick the
// same set on replay. Three things make it so: the Ui seeds that Rng from the session's seed when the recording starts from
// the recorder page and again when the render starts, loadSet does not stir the clock in meanwhile (the live millis and the
// replay's renderNowMs differ), and the Recorder resets the Sets load cycle at both starts (Sets::resetLoadCycle: the SPEC
// §9.26 masks and the §9.8 "last loaded" names), so the replayed load runs its reservoir over the SAME candidates as live.
// The device case is the SAME World rendering its own recording: its Sets still holds the live pick as set 1's last name
// with its cycle bit set (review 2026-10-16: without the reset that render loaded another preset — a fresh second World
// proved nothing about it, its masks and names being empty). The loaded name is read from the Ui's "LOADED <name>" status
// (the Sets::lastName() of the finished load) in every case.
TEST(a_random_load_inside_a_recording_picks_the_same_set_on_replay) {
  resetCard();
  auto loadedName = [](Ui& ui, std::string& out) { const char* t = ui.statusText(); if (strncmp(t, "LOADED ", 7) == 0) out = t + 7; };
  auto liveLoad = [&](World& w, std::string& out) {    // load S1 live, until the Ui reports it; the status is gone again a bar later (kStatusMs)
    w.tap(11, 2);
    for (int i = 0; i < 3 * kBlocksPerBar120 && out.empty(); i++) { w.liveBlock(); loadedName(w.ui, out); }
    w.liveBlocks(kBlocksPerBar120);
    CHECK(strncmp(w.ui.statusText(), "LOADED ", 7) != 0);
  };
  World* a = new World(); a->firstSetTempo120(); a->liveBlocks(200);
  a->uiOnly(13, 3, KeyEvent::Press); a->uiOnly(10, 2, KeyEvent::Press); a->uiOnly(10, 2, KeyEvent::Release); a->uiOnly(13, 3, KeyEvent::Release);   // SHIFT + save 1: the recorder page
  CHECK_EQ((int)a->ui.page(), (int)PgRecorder);
  a->tap(13, 2);                                         // ent: REC ON (the page closes, the Ui's load Rng is seeded from the session, the Sets load cycle is reset)
  CHECK(a->rec.recording()); CHECK_EQ((int)a->ui.page(), (int)PgMain);
  a->liveBlocks(10);
  std::string live; liveLoad(*a, live);                  // a random House set in A minor
  a->rec.stopRecording();
  CHECK(!live.empty());
  CHECK(live == a->sets.lastName());
  const int liveNum = Sets::parsePresetNumber(live.c_str());
  CHECK(liveNum >= 0); CHECK(a->sets.presetLoadedInCycle(0, liveNum));   // the live load marked its preset in set 1's cycle (and is set 1's no-repeat name)
  // 1) the device: the same World renders its own recording. startRender resets the cycle (the bit clears), and the replayed
  //    load draws over every A-minor preset again, as the recording did — the same preset, which the replay marks again.
  CHECK(a->rec.startRender(0));
  CHECK(!a->sets.presetLoadedInCycle(0, liveNum));
  std::string same;
  for (int n = 0; n < 200000 && a->replayBlock(); n++) if (same.empty()) loadedName(a->ui, same);
  CHECK(a->rec.hasWav(0));
  printf("  live loaded \"%s\", the same world's render loaded \"%s\"\n", live.c_str(), same.c_str());
  CHECK(!same.empty()); CHECK(same == live);
  CHECK(live == a->sets.lastName()); CHECK(a->sets.presetLoadedInCycle(0, liveNum));
  delete a;
  // 2) a fresh boot that loaded a preset of its own before the render (a last name and a cycle bit the recording never had)
  World* b = new World(); b->firstSetTempo120(); b->liveBlocks(200);
  std::string pre; liveLoad(*b, pre);
  CHECK(!pre.empty()); CHECK(b->sets.presetLoadedInCycle(0, Sets::parsePresetNumber(pre.c_str())));
  CHECK(b->rec.startRender(0));
  CHECK(!b->sets.presetLoadedInCycle(0, Sets::parsePresetNumber(pre.c_str())));
  std::string replay;
  for (int n = 0; n < 200000 && b->replayBlock(); n++) if (replay.empty()) loadedName(b->ui, replay);
  CHECK(b->rec.hasWav(0));
  printf("  a fresh world that had loaded \"%s\" rendered \"%s\"\n", pre.c_str(), replay.c_str());
  CHECK(!replay.empty()); CHECK(replay == live); CHECK(live == b->sets.lastName());
  delete b;
  // 3) a fresh boot with nothing loaded (the case the test covered before the review)
  World* c = new World(); c->liveBlocks(200);
  CHECK(c->rec.startRender(0));
  std::string fresh;
  for (int n = 0; n < 200000 && c->replayBlock(); n++) if (fresh.empty()) loadedName(c->ui, fresh);
  CHECK(c->rec.hasWav(0));
  CHECK(!fresh.empty()); CHECK(fresh == live);
  delete c;
}

// The engine's parallel-render hooks installed (as the sketch does): a render on the "loop task" pairs every start with a
// join on the same caller — the sketch's worker notifies the task that started the job — and the WAV is the one the
// hook-less render writes.
TEST(render_with_parallel_hooks_joins_on_the_caller_and_matches) {
  resetCard();
  World* a = new World(); a->firstSetTempo120();
  CHECK(a->rec.startRecording());
  a->liveBlocks(10); a->tap(7, 0); a->tap(7, 1);        // set 1 Beat, set 2 Beat (both sets render: the set-2 job goes through the hook)
  a->liveBlocks(2 * kBlocksPerBar120);
  a->rec.stopRecording();
  delete a;
  World* b = new World();
  CHECK(b->rec.startRender(0)); b->replayAll(); CHECK(b->rec.hasWav(0));
  std::vector<uint8_t> wav = readTmp("/lp/wav/rec_000.wav");
  delete b;
  g_hookStarts = g_hookJoins = g_hookBadJoins = g_hookLoopStarts = 0;
  lp::g_parallelStart = stubParallelStart; lp::g_parallelJoin = stubParallelJoin;
  World* c = new World();
  c->liveBlocks(5);
  const int liveStarts = g_hookStarts;
  CHECK(liveStarts >= 5); CHECK_EQ(g_hookLoopStarts, 0);
  CHECK(c->rec.startRender(0)); const int steps = c->replayAll();
  lp::g_parallelStart = nullptr; lp::g_parallelJoin = nullptr;
  CHECK(c->rec.hasWav(0));
  CHECK(g_hookLoopStarts >= steps);                       // every rendered block went through the hooks on the loop task (a block may split in two chunks)
  CHECK_EQ(g_hookStarts, g_hookJoins); CHECK_EQ(g_hookBadJoins, 0);
  CHECK(readTmp("/lp/wav/rec_000.wav") == wav);
  printf("  %d hook jobs on the loop task over %d render steps, %d live\n", g_hookLoopStarts, steps, liveStarts);
  delete c;
}

TEST(the_real_card_was_never_touched_and_the_temp_card_goes_away) {
  struct stat st;
  CHECK(stat((sdRoot() + "/lp/rec").c_str(), &st) != 0);
  CHECK(stat((sdRoot() + "/lp/wav").c_str(), &st) != 0);
  resetCard();
  ::rmdir(tmpPath("/lp/rec").c_str()); ::rmdir(tmpPath("/lp/wav").c_str()); ::rmdir(tmpPath("/lp/state").c_str());
  ::rmdir(tmpPath("/lp").c_str()); ::rmdir(g_tmpRoot.c_str());
  CHECK(stat(g_tmpRoot.c_str(), &st) != 0);
}

int main() { return lp_test::run(); }
