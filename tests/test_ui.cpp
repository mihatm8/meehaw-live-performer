// Host tests for ui/ui.cpp (SPEC §3, §4, §9.10, §9.14, §9.19): a recording FakeGfx (IGfx) and the real Engine, Library,
// Generator and Sets over sdcard/lp through PosixFs (override the card root with LP_SDCARD). Key events are fed
// as raw matrix coordinates (SPEC §3 map, physical SHIFT = (13,3)); the harness loop renders 256-sample engine
// blocks (≈ 11.6 ms each) and calls gen.step() / sets.step() / ui.tick() like the sketch's loop().
#include "check.h"
#include "ui/ui.h"
#include "rec/recorder.h"
#include "gen/generator.h"
#include "gen/library.h"
#include "gen/sets.h"
#include "gen/key.h"
#include "gen/smf.h"
#include "engine/engine.h"
#include "dsp/ulaw.h"
#include "../firmware/desktop/posix_fs.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace lp;

#ifdef LP_UI_THEORY_STANDIN
// run_ui.sh compiles the tests with this while src/gen/generator.cpp does not yet define Generator::generateTheorySet /
// theorySet (SPEC §9.24, the generator owner's job): a set generate under the theory name, so the Ui's key, label, blink
// and refusal paths run against the real job machinery; theorySet() answers false. The real definitions take over the
// moment they land (the runner then drops this define and says so).
namespace lp {
bool Generator::generateTheorySet(int set) { return generateSet(set); }
bool Generator::theorySet(int) const { return false; }
}
#endif

// ---------------------------------------------------------------------------------------------- fake display

struct Op { char kind; int x, y, w, h; Color c; };   // S screen, F fillRect, R drawRect, L line (x,y,w,h = x0,y0,x1,y1), T text
struct TextOp { std::string text; int x, y, w, h; Color c; };
class FakeGfx : public IGfx {
 public:
  int fills = 0, rects = 0, lines = 0, texts = 0, screens = 0, presents = 0;
  std::vector<Op> ops; std::vector<std::string> textLog; std::vector<TextOp> textOps;   // this draw's ops
  std::vector<TextOp> screen;                        // texts still visible: survives reset(); a fill or text that covers one erases it
  void reset() { fills = rects = lines = texts = screens = presents = 0; ops.clear(); textLog.clear(); textOps.clear(); }
  int width() const override { return 240; }
  int height() const override { return 135; }
  void fillScreen(Color c) override { screens++; ops.push_back({'S', 0, 0, 240, 135, c}); screen.clear(); }
  void fillRect(int x, int y, int w, int h, Color c) override { fills++; ops.push_back({'F', x, y, w, h, c}); cover(x, y, w, h); }
  void drawRect(int x, int y, int w, int h, Color c) override { rects++; ops.push_back({'R', x, y, w, h, c}); }
  void drawLine(int x0, int y0, int x1, int y1, Color c) override { lines++; ops.push_back({'L', x0, y0, x1, y1, c}); }
  void drawText(int x, int y, const char* t, int size, Color c) override {
    texts++; textLog.push_back(t); const int w = 6 * size * (int)strlen(t), h = 8 * size;
    ops.push_back({'T', x, y, w, h, c}); textOps.push_back({t, x, y, w, h, c}); cover(x, y, w, h); screen.push_back({t, x, y, w, h, c});
  }
  int textWidth(const char* t, int size) override { return 6 * size * (int)strlen(t); }
  void present() override { presents++; }
  bool sawText(const char* sub) const { for (const std::string& s : textLog) if (s.find(sub) != std::string::npos) return true; return false; }
  const char* lastText() const { return textLog.empty() ? "" : textLog.back().c_str(); }
  const TextOp* drawn(const char* exact) const { for (const TextOp& t : textOps) if (t.text == exact) return &t; return nullptr; }     // drawn by this draw
  const TextOp* onScreen(const char* exact) const { for (const TextOp& t : screen) if (t.text == exact) return &t; return nullptr; }   // visible now
  void cover(int x, int y, int w, int h) { for (size_t i = 0; i < screen.size();) { const TextOp& t = screen[i]; if (t.x < x + w && x < t.x + t.w && t.y < y + h && y < t.y + t.h) screen.erase(screen.begin() + i); else i++; } }
  // Which main-page cells (0..15) the fill/rect/line ops touched; −1 for an op outside every cell box.
  std::set<int> cellsTouched() const {
    std::set<int> out;
    for (const Op& o : ops) {
      if (o.kind == 'S') { for (int i = 0; i < kTracks; i++) out.insert(i); continue; }
      if (o.kind == 'T') continue;
      int x0 = o.x, y0 = o.y, x1 = o.kind == 'L' ? o.w : o.x + o.w - 1, y1 = o.kind == 'L' ? o.h : o.y + o.h - 1;
      int hit = -1;
      for (int t = 0; t < kTracks; t++) {
        int cx = kCellX0 + (t % kTracksPerSet) * kCellPitch, cy = kRowY[t / kTracksPerSet];
        if (x0 >= cx && x1 < cx + kCellSize && y0 >= cy && y1 < cy + kCellBoxH) { hit = t; break; }
      }
      out.insert(hit);
    }
    return out;
  }
};

// ---------------------------------------------------------------------------------------------- harness

static std::string sdRoot() {
  const char* env = getenv("LP_SDCARD");
  if (env && *env) return env;
  struct stat st;
  if (stat("sdcard/lp", &st) == 0) return "sdcard";
  return "/Users/miha/Documents/cardputer/liveplayer/sdcard";
}
static PosixFs& sd() { static PosixFs fs(sdRoot().c_str()); return fs; }
static int16_t g_loopRing[kLoopSlots][kLoopRingBytes / 2];   // the loop slots' ring memory (SPEC §9.19): static on the host, bootBlock on the device
static Engine& eng() {
  static Engine* e = nullptr;
  if (!e) { e = new Engine(); ulawInit(); e->init(); for (int k = 0; k < kLoopSlots; k++) e->setLoopRing(k, g_loopRing[k], kLoopRingBytes / 2); Cmd t; t.type = CmdSetTempo; t.i = 120; e->cmds().push(t); static int16_t tmp[kBlock]; e->render(tmp, kBlock); }   // the suite reasons at 120 BPM; the firmware default is kDefaultBpm (125)
  return *e;
}
static Library& lib() { static Library l; static bool init = false; if (!init) { l.init(&sd()); init = true; } return l; }
static Generator& gen() { static Generator* g = nullptr; if (!g) { g = new Generator(); g->init(&sd(), &eng(), &lib()); g->seed(7); } return *g; }
static Sets& sets() { static Sets* s = nullptr; if (!s) { s = new Sets(); s->init(&sd(), &eng(), &gen(), &lib()); } return *s; }
static FakeGfx g_gfx;
static Ui& ui() { static Ui* u = nullptr; if (!u) { u = new Ui(); u->init(&g_gfx, &eng(), &gen(), &sets()); } return *u; }

static uint32_t g_now = 0;                           // the harness clock (ms)
static int16_t g_out[kBlock];
constexpr uint32_t kBlockMs = 12;                    // 256 samples ≈ 11.6 ms

static void key(int x, int y, KeyEvent::Kind kind, uint32_t held = 0) { KeyEvent e; e.x = (uint8_t)x; e.y = (uint8_t)y; e.kind = kind; e.ms = g_now; e.heldMs = held; ui().onKey(e, g_now); }
static void press(int x, int y) { key(x, y, KeyEvent::Press); }
static void release(int x, int y, uint32_t held = 50) { key(x, y, KeyEvent::Release, held); }
static void tap(int x, int y) { press(x, y); release(x, y); }
static void shiftDown() { press(13, 3); }
static void shiftUp() { release(13, 3); }
// One loop iteration: generator + sets step, one engine block, ui.tick; the clock advances one block.
static void iterate() { gen().step(); sets().step(); eng().render(g_out, kBlock); g_now += kBlockMs; ui().tick(g_now); }
static void runBlocks(int n) { for (int i = 0; i < n; i++) iterate(); }
static void runToTick(uint32_t tick) { int guard = 0; while (eng().status().tick < tick && guard++ < 20000) iterate(); }
static uint32_t nextBarTick() { uint32_t t = eng().status().tick; return (t / kTicksPerBar + 1) * kTicksPerBar; }
static void draw() { g_gfx.reset(); ui().draw(g_now); }
static void advanceMs(uint32_t ms) { g_now += ms; ui().tick(g_now); }
static void push(CmdType t, int track = 0, int a = 0, int32_t i = 0, float f = 0) { Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f; eng().cmds().push(c); }

// Pops every queued command into `out` and pushes it back unchanged (peek at what the UI sent).
static std::vector<Cmd> peekCmds() {
  std::vector<Cmd> out; Cmd c;
  while (eng().cmds().pop(c)) out.push_back(c);
  for (const Cmd& k : out) eng().cmds().push(k);
  return out;
}
static int countCmd(const std::vector<Cmd>& v, CmdType t) { int n = 0; for (const Cmd& c : v) if (c.type == t) n++; return n; }
static const Cmd* findCmd(const std::vector<Cmd>& v, CmdType t, int track = -1) { for (const Cmd& c : v) if (c.type == t && (track < 0 || c.track == track)) return &c; return nullptr; }
static void drainCmds() { Cmd c; while (eng().cmds().pop(c)) {} }

// A card file's text through the host root (session files), and a "key=value" line test.
static std::string readCardText(const char* path) {
  const std::string full = sdRoot() + path; FILE* f = fopen(full.c_str(), "rb"); if (!f) return "";
  std::string out; char b[512]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n); fclose(f); return out;
}
static bool hasLine(const std::string& text, const char* line) { const std::string l = std::string(line) + "\n"; return text.compare(0, l.size(), l) == 0 || text.find("\n" + l) != std::string::npos; }
static bool cardExists(const std::string& card) { struct stat st; return stat((sdRoot() + card).c_str(), &st) == 0; }
// The newest complete session file of a set: the writer alternates two slots (session_set<N>.lpset / _b, lp.seq one up
// per file, lp.end closes it) and the restore takes the highest complete lp.seq, so a test reads the same one.
static std::string latestSessionText(int set) {
  std::string best; long bestSeq = -1;
  for (int slot = 0; slot < 2; slot++) {
    char path[64]; Sets::sessionFilePath(path, sizeof path, set, slot);
    const std::string t = readCardText(path);
    const size_t p = t.find("lp.seq="); if (p == std::string::npos || t.find("lp.end") == std::string::npos) continue;
    const long seq = strtol(t.c_str() + p + 7, nullptr, 10);
    if (seq > bestSeq) { bestSeq = seq; best = t; }
  }
  return best;
}
// Lets every card job and a running session write finish so a test can watch the next write start.
static void settleSession() {
  for (int guard = 0; guard < 4000 && (sets().sessionBusy() || sets().busy() || gen().busy()); guard++) iterate();
  runBlocks(200);
  for (int guard = 0; guard < 4000 && sets().sessionBusy(); guard++) iterate();
}
// Engine blocks a test allows a session write to start in: the 5 s debounce after the change plus the 15 s the writer
// keeps between two writes (Sets::kSessionDebounceSamples / kSessionSpacingSamples, SPEC §9.19c) and a 1 s margin.
constexpr int kSessionWaitBlocks = (Sets::kSessionDebounceSamples + Sets::kSessionSpacingSamples) / kBlock + kSampleRate / kBlock;   // ≈ 1808 (21 s)
// Iterates until a session write starts (true) or kSessionWaitBlocks pass, then until it is done. A write already
// running was armed before the change under test (its header may predate it): it is let finish first, and the
// change's own write — re-armed by the dirty flag, 15 s after that one — is the one waited for.
static bool waitSessionWrite() {
  for (int guard = 0; guard < 4000 && sets().sessionBusy(); guard++) iterate();
  bool wrote = false;
  for (int blocks = 0; blocks < kSessionWaitBlocks && !wrote; blocks++) { iterate(); if (sets().sessionBusy()) wrote = true; }
  for (int guard = 0; guard < 4000 && sets().sessionBusy(); guard++) iterate();
  return wrote;
}

// A one-bar test pattern into the UI-owned source and the engine (immediate swap) so the track is PSStopped.
static void loadTestPattern(int track, int pitch, int notes = 4) {
  Pattern& src = eng().sourcePattern(track); src.clear(kTicksPerBar);
  for (int i = 0; i < notes; i++) src.add(Note::make((uint16_t)(i * kTicksPerBeat), 48, (uint8_t)pitch, 100));
  src.sortByStart();
  while (!eng().beginNextWrite(track)) iterate();
  eng().nextPattern(track) = src; eng().endNextWrite(track);
  push(CmdSwapPatternNow, track, 0, 0);
  TrackGenInfo& inf = gen().info(track); inf.loaded = 1; inf.variant = 0; inf.naturalTicks = kTicksPerBar; inf.sourceTicks = kTicksPerBar; inf.keyRoot = gen().key().root;
  runBlocks(1);
}

// ---------------------------------------------------------------------------------------------- loop-mode fallbacks (SPEC §9.19)

// The loop-mode API of generator.h / engine.h is implemented by the generator and engine owners in the same wave. Until
// their strong definitions exist in generator.cpp / engine.cpp, the WEAK ones below model the contract on the host: a
// flag, a slot and a canned 2-bar file name per track, kLoopSlots slots, and the refusals the UI must map (no card, a
// running job, the free-heap hook under kLoopLowRamBytes, a genre without a loops/ folder, both slots taken). The
// linker drops each of them the moment the real symbol is linked, so every test below holds in both worlds;
// loopstub::active() says which one a run is in (the stub's setLoopMode sets it, the real one never does).
namespace lp {
namespace loopstub {
struct Track { uint8_t on = 0; int8_t slot = -1; uint8_t bars = 0; char file[kMaxNameLen] = ""; };
static Track g_t[kTracks]; static uint8_t g_called = 0;
static bool active() { return g_called != 0; }
}
__attribute__((weak)) const char* Generator::loopFolder(Category c) {
  static const char* n[CategoryCount] = {"chord", "lead", "pad", "bass", "fx", "percussion", "vocal", "drum"};
  return n[c < CategoryCount ? c : 0];
}
__attribute__((weak)) bool Generator::setLoopMode(int track, bool on) {
  loopstub::g_called = 1;
  if (track < 0 || track >= kTracks) return false;
  loopstub::Track& t = loopstub::g_t[track];
  loopRefusal_ = LoopOk;
  if (!on) {
    if (!t.on) return true;
    t.on = 0; t.slot = -1; t.bars = 0; t.file[0] = 0;
    pushCmd(CmdLoopMode, track, 0, trackSet(track)); changes_++;
    return true;
  }
  if (t.on) return true;
  if (!fs_) { loopRefusal_ = LoopNoCard; return false; }
  if (busy()) { loopRefusal_ = LoopBusy; return false; }
  if (freeHeap_ && freeHeap_() < (uint32_t)kLoopLowRamBytes) { loopRefusal_ = LoopLowRam; return false; }
  const Category c = trackCategory(track);
  char dir[kMaxPathLen]; snprintf(dir, sizeof dir, "/lp/packs/%s/loops/%s", genreName(), loopFolder(c));
  if (!fs_->isDir(dir)) { loopRefusal_ = LoopNoContent; return false; }
  int slot = -1;
  if (c != Vocal) {
    for (int k = 0; k < kLoopSlots && slot < 0; k++) {
      bool used = false;
      for (int i = 0; i < kTracks; i++) if (loopstub::g_t[i].on && loopstub::g_t[i].slot == k) used = true;
      if (!used) slot = k;
    }
    if (slot < 0) { loopRefusal_ = LoopSlotsFull; return false; }
  }
  t.on = 1; t.slot = (int8_t)slot; t.bars = 2;
  snprintf(t.file, sizeof t.file, "NA__125bpm__LOOP__2bars__STUB_%s.wav", loopFolder(c));
  pushCmd(CmdLoopMode, track, 1, slot >= 0 ? kSets + slot : trackSet(track)); changes_++;
  return true;
}
__attribute__((weak)) bool Generator::loopEnabled(int track) const { return track >= 0 && track < kTracks && loopstub::g_t[track].on != 0; }
__attribute__((weak)) int Generator::loopSlot(int track) const { return loopEnabled(track) ? loopstub::g_t[track].slot : -1; }
__attribute__((weak)) const char* Generator::loopFile(int track) const { return loopEnabled(track) ? loopstub::g_t[track].file : ""; }
__attribute__((weak)) uint8_t Generator::loopBars(int track) const { return loopEnabled(track) ? loopstub::g_t[track].bars : 0; }
__attribute__((weak)) bool Generator::anyLoop() const { for (int i = 0; i < kTracks; i++) if (loopstub::g_t[i].on) return true; return false; }
__attribute__((weak)) void Engine::setLoopRing(int slot, int16_t* mem, int samples) { if (slot < 0 || slot >= kLoopSlots) return; loop_[slot].buf = mem; loop_[slot].cap = samples; }
} // namespace lp

// ---------------------------------------------------------------------------------------------- pure helpers

TEST(pure_helpers) {
  CHECK_EQ(Ui::perfDegree(1, 0), 0); CHECK_EQ(Ui::perfDegree(10, 0), 9); CHECK_EQ(Ui::perfDegree(0, 0), -1); CHECK_EQ(Ui::perfDegree(11, 0), -1);
  CHECK_EQ(Ui::perfDegree(1, 1), 3); CHECK_EQ(Ui::perfDegree(2, 2), 6); CHECK_EQ(Ui::perfDegree(3, 3), 9); CHECK_EQ(Ui::perfDegree(12, 3), 18);
  CHECK_EQ(Ui::perfDegree(2, 3), -1); CHECK_EQ(Ui::perfDegree(13, 3), -1); CHECK_EQ(Ui::perfDegree(5, 4), -1);
  CHECK_EQ(Ui::roundUpBars(0), 1); CHECK_EQ(Ui::roundUpBars(1), 1); CHECK_EQ(Ui::roundUpBars(2), 2); CHECK_EQ(Ui::roundUpBars(3), 4); CHECK_EQ(Ui::roundUpBars(9), 4);
  CHECK_EQ((int)Ui::quantize16(0), 0); CHECK_EQ((int)Ui::quantize16(11), 0); CHECK_EQ((int)Ui::quantize16(12), 24); CHECK_EQ((int)Ui::quantize16(100), 96); CHECK_EQ((int)Ui::quantize16(109), 120);
  char b[8];
  formatBeats(b, sizeof b, 384); CHECK(strcmp(b, "4") == 0);
  formatBeats(b, sizeof b, 48); CHECK(strcmp(b, "0.5") == 0);
  formatBeats(b, sizeof b, 1536); CHECK(strcmp(b, "16") == 0);
  CHECK(strcmp(variantName(Beat, 2), "+SNARE") == 0); CHECK(strcmp(variantName(Percussion, 3), "FULL") == 0); CHECK(strcmp(variantName(Vocal, 1), "ALT FILE") == 0);
  CHECK_EQ((int)categoryColor(Chord), (int)rgb(0xFF, 0x8C, 0x00)); CHECK_EQ((int)categoryColor(Beat), (int)rgb(0xA0, 0xA0, 0xA0));
  CHECK_EQ((int)paramColor(PVolume), (int)rgb(0x39, 0xFF, 0x14)); CHECK_EQ((int)pageParamColor(PPRampBars), (int)rgb(0x1D, 0xE9, 0xB6));
  CHECK_EQ((int)pageParamColor(PPMaster), (int)rgb(0xFF, 0x3B, 0x3B)); CHECK_EQ((int)paramColor(PMaster), (int)rgb(0xFF, 0x3B, 0x3B));
  CHECK_EQ((int)pageParamColor(PPGenre), (int)rgb(0xFF, 0x4F, 0xA3)); CHECK(strcmp(pageParamName(PPMaster), "OUTPUT") == 0); CHECK(strcmp(pageParamName(PPRampBars), "RAMP") == 0);
  for (int y = 0; y < kKeyRows; y++) for (int x = 0; x < kKeyCols; x++) {   // no key maps to the output volume slot: it is SHIFT + kick cut
    const KeyMapping m = mapKey(x, y);
    CHECK(!((m.fn == KParamUp || m.fn == KParamDown) && m.a == PMaster));
  }
  CHECK_EQ((int)scaleColor(rgb(255, 255, 255), 30), (int)rgb(9 << 3, 18 << 2, 9 << 3));   // 31*0.3 = 9, 63*0.3 = 18
  CHECK(kCellX0 + 7 * kCellPitch + kCellSize <= kScreenW);                                // the last square is on screen
  // SPEC §9.17c helpers: the perform quantise grid, the synth page's column → knob map, knob values in real units
  CHECK_EQ(Ui::quantTicks(0), 0); CHECK_EQ(Ui::quantTicks(1), 24); CHECK_EQ(Ui::quantTicks(2), 48); CHECK_EQ(Ui::quantTicks(3), 96); CHECK_EQ(Ui::quantTicks(7), 0);
  CHECK_EQ((int)Ui::synthKnobParam(PVolume), (int)PCutoff); CHECK_EQ((int)Ui::synthKnobParam(PReverb), (int)PResonance); CHECK_EQ((int)Ui::synthKnobParam(PEcho), (int)PEnvAmt);
  CHECK_EQ((int)Ui::synthKnobParam(PDist), (int)PDecay); CHECK_EQ((int)Ui::synthKnobParam(PFilter), (int)PDrive); CHECK_EQ((int)Ui::synthKnobParam(PLength), (int)PWave);
  CHECK_EQ((int)Ui::synthKnobParam(PTempo), (int)PSat); CHECK_EQ((int)Ui::synthKnobParam(PKey), (int)PAccent); CHECK_EQ((int)Ui::synthKnobParam(PMaster), (int)ParamCount);   // SPEC §9.22: tempo± / key± are the SAT / ACC knobs
  char kb[8];
  Ui::knobValueText(PCutoff, 0.0f, kb, sizeof kb); CHECK(strcmp(kb, "80Hz") == 0);
  Ui::knobValueText(PCutoff, 1.0f, kb, sizeof kb); CHECK(strcmp(kb, "12.0k") == 0);
  Ui::knobValueText(PResonance, 0.35f, kb, sizeof kb); CHECK(strcmp(kb, "35%") == 0);
  Ui::knobValueText(PEnvAmt, 0.5f, kb, sizeof kb); CHECK(strcmp(kb, "2.0oct") == 0);
  Ui::knobValueText(PDecay, 0.0f, kb, sizeof kb); CHECK(strcmp(kb, "10ms") == 0);
  Ui::knobValueText(PDecay, 1.0f, kb, sizeof kb); CHECK(strcmp(kb, "2.0s") == 0);
  Ui::knobValueText(PDrive, 1.0f, kb, sizeof kb); CHECK(strcmp(kb, "100%") == 0);
  Ui::knobValueText(PWave, 2.0f, kb, sizeof kb); CHECK(strcmp(kb, "SAW") == 0);
  CHECK(strcmp(Ui::waveName(0), "SINE") == 0); CHECK(strcmp(Ui::waveName(4), "FAT") == 0); CHECK(strcmp(Ui::waveName(9), "SINE") == 0);
  CHECK_EQ((int)PPGenre, (int)ParamCount); CHECK(strcmp(pageParamName(PPGenre), "GENRE") == 0); CHECK(strcmp(pageParamName((PageParam)PCutoff), "CUTOFF") == 0);
  CHECK_EQ((int)paramColor(PCutoff), (int)rgb(0xFF, 0xFF, 0x00)); CHECK_EQ((int)paramColor(PWave), (int)rgb(0xFF, 0xFF, 0xFF));
}

// ---------------------------------------------------------------------------------------------- splash / main

TEST(splash_then_main_after_1500ms) {
  Ui& u = ui();
  g_now = 100; u.tick(g_now);
  CHECK_EQ((int)u.page(), (int)PgSplash);
  draw();
  CHECK(g_gfx.sawText("meehaw")); CHECK(g_gfx.sawText("live performer")); CHECK_EQ(g_gfx.screens, 1);
  advanceMs(1499);                                   // 1499 ms after the first tick: still the splash
  CHECK_EQ((int)u.page(), (int)PgSplash);
  advanceMs(1);                                      // 1500 ms: main (SPEC §4)
  CHECK_EQ((int)u.page(), (int)PgMain);
  draw();
  CHECK_EQ(g_gfx.screens, 1);                        // entering the page: full redraw
  CHECK_EQ((int)g_gfx.cellsTouched().count(-1), 1);  // footer + labels live outside the cells
  std::set<int> cells = g_gfx.cellsTouched(); cells.erase(-1);
  CHECK_EQ((int)cells.size(), 16);                   // 16 squares drawn
  CHECK(g_gfx.sawText("A MIN 120 HOUSE  LEN 4"));    // footer (SPEC §4): key, bpm, genre, selected clip length
  CHECK(g_gfx.sawText("BEAT")); CHECK(g_gfx.sawText("CHD"));
  CHECK_EQ(u.selectedTrack(), (int)Beat);            // default selection: set 1 Beat (SPEC §3)
  CHECK_EQ(g_gfx.presents, 1);
}

TEST(main_page_redraws_only_the_changed_cell) {
  Ui& u = ui();
  advanceMs(40); draw();
  CHECK_EQ((int)g_gfx.ops.size(), 0);                // nothing changed: nothing drawn
  loadTestPattern(2, 60);                            // Pad of set 1 becomes PSStopped
  CHECK_EQ((int)eng().status().track[2].state, (int)PSStopped);
  advanceMs(40); draw();
  CHECK_EQ(g_gfx.screens, 0);
  std::set<int> cells = g_gfx.cellsTouched();
  CHECK_EQ((int)cells.size(), 1); CHECK_EQ((int)cells.count(2), 1);
  CHECK(g_gfx.fills >= 1 && g_gfx.rects == 1);       // clear + outline (stopped = outline, SPEC §4)
  // a draw 10 ms later is skipped (40 ms throttle) even though the blink clock moved
  g_now += 10; g_gfx.reset(); u.draw(g_now); CHECK_EQ((int)g_gfx.ops.size(), 0);
  g_now += 30;
}

// Battery readout (SPEC §4): right-aligned at the footer's right edge (ends within 2 px of it, on the footer line).
static bool atRightEdge(const TextOp* t) { return t && t->x + t->w >= kScreenW - 2 && t->x + t->w <= kScreenW && t->y == kFooterY + 1; }

TEST(footer_battery_readout_bottom_right) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ(u.batteryPercent(), -1); CHECK_EQ(u.batteryCharging(), -1);   // unknown until the platform reports ...
  const TextOp* left = g_gfx.onScreen("A MIN 120 HOUSE  LEN 4"); CHECK(left != nullptr);
  const TextOp* bat = g_gfx.onScreen("--"); CHECK(atRightEdge(bat));     // ... shown as "--" at the right edge since the page opened
  if (bat) CHECK_EQ((int)bat->c, (int)kDimGrey);
  u.setBattery(87, 0); CHECK_EQ(u.batteryPercent(), 87); CHECK_EQ(u.batteryCharging(), 0);
  advanceMs(40); draw();
  CHECK_EQ(g_gfx.screens, 0); CHECK_EQ((int)g_gfx.cellsTouched().size(), 1); CHECK_EQ((int)g_gfx.cellsTouched().count(-1), 1);   // only the footer line was touched
  bat = g_gfx.drawn("87%"); CHECK(atRightEdge(bat)); if (bat) CHECK_EQ((int)bat->c, (int)kDimGrey);
  CHECK(!g_gfx.sawText("A MIN"));                    // the left text was not redrawn ...
  left = g_gfx.onScreen("A MIN 120 HOUSE  LEN 4"); CHECK(left != nullptr);   // ... and is still on screen, left of the readout
  if (left && bat) CHECK(left->x + left->w + kFooterGap <= bat->x);
  CHECK(g_gfx.onScreen("--") == nullptr);           // the old readout was cleared
  u.setBattery(87, 1); advanceMs(40); draw();        // charging
  bat = g_gfx.drawn("CHG 87%"); CHECK(atRightEdge(bat)); if (bat) CHECK_EQ((int)bat->c, (int)kDimGrey);
  CHECK(g_gfx.onScreen("87%") == nullptr); CHECK(g_gfx.onScreen("A MIN 120 HOUSE  LEN 4") != nullptr);
  u.setBattery(12, 0); advanceMs(40); draw();        // low and not charging: red
  bat = g_gfx.drawn("12%"); CHECK(atRightEdge(bat)); if (bat) CHECK_EQ((int)bat->c, (int)kBatteryLow);
  CHECK(g_gfx.onScreen("CHG 87%") == nullptr);
  u.setBattery(12, 1); advanceMs(40); draw();        // low but charging: grey
  bat = g_gfx.drawn("CHG 12%"); CHECK(bat != nullptr); if (bat) CHECK_EQ((int)bat->c, (int)kDimGrey);
  u.setBattery(14, -1); advanceMs(40); draw();       // charge state unknown counts as not charging
  bat = g_gfx.drawn("14%"); CHECK(bat != nullptr); if (bat) CHECK_EQ((int)bat->c, (int)kBatteryLow);
  u.setBattery(15, 0); advanceMs(40); draw();        // 15 %: grey again
  bat = g_gfx.drawn("15%"); CHECK(bat != nullptr); if (bat) CHECK_EQ((int)bat->c, (int)kDimGrey);
  u.setBattery(15, 0); advanceMs(40); draw(); CHECK_EQ((int)g_gfx.ops.size(), 0);   // reported again unchanged: nothing drawn
  advanceMs(40); draw(); CHECK_EQ((int)g_gfx.ops.size(), 0);
  u.setBattery(140, 0); advanceMs(40); draw(); CHECK(g_gfx.drawn("100%") != nullptr); CHECK_EQ(u.batteryPercent(), 100);   // clamped
  u.setBattery(-7, 1); advanceMs(40); draw(); CHECK(atRightEdge(g_gfx.drawn("--"))); CHECK(!g_gfx.sawText("CHG")); CHECK_EQ(u.batteryPercent(), -1);   // level unknown while charging: "--", no prefix
  u.setBattery(-1, -1); advanceMs(40); draw(); CHECK_EQ((int)g_gfx.ops.size(), 0); CHECK(atRightEdge(g_gfx.onScreen("--")));   // same "--": nothing repainted
  // The left text is fitted to the room before the readout by whole segments: next to "CHG 100%" (48 px) it may end
  // at 185 px, so "A MIN 120 HOUSE  LEN 4  OUT 85%" (186 px, after a SHIFT + kick cut S1 step) is shown with single-space
  // separators (174 px) and its whole OUT note instead of losing its last character.
  u.setBattery(100, 1); advanceMs(40); draw(); CHECK(atRightEdge(g_gfx.drawn("CHG 100%")));
  u.setOutputVolume(200);                            // start from 200 so the step lands on 216 = "85%"
  shiftDown(); press(13, 0); release(13, 0); shiftUp();   // output volume 200 → 216: the OUTPUT page, then "  OUT 85%" in the footer for 3 s
  CHECK_EQ(u.outputVolume(), 216); CHECK_EQ((int)u.page(), (int)PgParam);
  advanceMs(1250); CHECK_EQ((int)u.page(), (int)PgMain);
  draw(); CHECK_EQ(g_gfx.screens, 1);
  left = g_gfx.drawn("A MIN 120 HOUSE LEN 4 OUT 85%"); bat = g_gfx.drawn("CHG 100%");
  CHECK(left != nullptr); CHECK(atRightEdge(bat)); CHECK(g_gfx.sawText("OUT 85%")); CHECK(g_gfx.drawn("A MIN 120 HOUSE  LEN 4  OUT 85") == nullptr);
  if (left && bat) CHECK(left->x + left->w + kFooterGap <= bat->x);
  u.setOutputVolume(150);                            // back to the default for the tests that follow
  advanceMs(3000); draw();                           // the OUT note left the footer: the left text is redrawn, the unchanged readout is left alone
  CHECK_EQ(g_gfx.screens, 0); CHECK(g_gfx.drawn("A MIN 120 HOUSE  LEN 4") != nullptr); CHECK(g_gfx.drawn("CHG 100%") == nullptr);
  bat = g_gfx.onScreen("CHG 100%"); CHECK(atRightEdge(bat));   // ... and is still there from before
  bool leftCleared = false, readoutTouched = false;
  for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.y == kFooterY && bat) { if (o.x == 0 && o.x + o.w == bat->x - kFooterGap) leftCleared = true; if (o.x + o.w > bat->x) readoutTouched = true; }
  CHECK(leftCleared); CHECK(!readoutTouched);
  u.setBattery(87, 0); advanceMs(40); draw(); CHECK(atRightEdge(g_gfx.drawn("87%")));   // leave a plain readout for the tests that follow
  drainCmds();
}

// The footer's notes are whole segments (SPEC §4): with the gyro on and a plain "87%" readout the line
// "A MIN 120 HOUSE  LEN 4  GYRO  OUT nn%" (222 px) is too wide for the 215 px before the readout, so it is shown
// on single-space separators (204 px) with its whole OUT note; next to "CHG 100%" (185 px) the OUT note is dropped
// altogether and GYRO stays. Never a partial number such as "OUT 8".
static bool outNoteWholeOrAbsent(const char* note) {
  for (const TextOp& t : g_gfx.textOps) if (t.text.find("OUT") != std::string::npos && t.text.find(note) == std::string::npos) return false;
  return true;
}

TEST(footer_notes_are_dropped_whole_never_cut) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ((int)u.page(), (int)PgMain); CHECK(!u.gyroOn());
  u.setBattery(87, 0); advanceMs(40); draw(); CHECK(g_gfx.onScreen("87%") != nullptr);
  press(8, 2); advanceMs(450); CHECK(u.gyroOn()); release(8, 2, 450); drainCmds();   // gyro on (SPEC §3): the GYRO ON note page ...
  advanceMs(700); draw(); CHECK_EQ(g_gfx.screens, 1);                                 // ... then the main page again
  CHECK(g_gfx.drawn("A MIN 120 HOUSE  LEN 4  GYRO") != nullptr);
  const int vol0 = u.outputVolume();
  shiftDown(); press(13, 0); release(13, 0); shiftUp(); CHECK_EQ(u.outputVolume(), vol0 + 16);   // the OUT note for 3 s
  char note[16]; snprintf(note, sizeof note, "OUT %d%%", ((vol0 + 16) * 100 + 127) / 255);
  char full[64]; snprintf(full, sizeof full, "A MIN 120 HOUSE LEN 4 GYRO %s", note);
  advanceMs(1250); draw(); CHECK_EQ(g_gfx.screens, 1);
  const TextOp* left = g_gfx.drawn(full); const TextOp* bat = g_gfx.drawn("87%");
  CHECK(left != nullptr); CHECK(atRightEdge(bat)); CHECK(g_gfx.sawText(note)); CHECK(outNoteWholeOrAbsent(note));
  if (left && bat) CHECK(left->x + left->w + kFooterGap <= bat->x);
  u.setBattery(100, 1); advanceMs(40); draw();      // a wider readout: no room for the OUT note at all, GYRO stays
  left = g_gfx.drawn("A MIN 120 HOUSE  LEN 4  GYRO"); bat = g_gfx.drawn("CHG 100%");
  CHECK(left != nullptr); CHECK(atRightEdge(bat)); CHECK(!g_gfx.sawText("OUT")); CHECK(g_gfx.onScreen(full) == nullptr);
  if (left && bat) CHECK(left->x + left->w + kFooterGap <= bat->x);
  u.setBattery(87, 0); advanceMs(40); draw();       // room again: the whole note is back
  CHECK(g_gfx.drawn(full) != nullptr); CHECK(outNoteWholeOrAbsent(note));
  press(8, 2); advanceMs(450); CHECK(!u.gyroOn()); release(8, 2, 450); drainCmds();   // gyro off
  advanceMs(700); draw(); CHECK(!g_gfx.sawText("GYRO")); CHECK(g_gfx.sawText(note)); CHECK(outNoteWholeOrAbsent(note));
  u.setOutputVolume(vol0); advanceMs(3000); draw(); CHECK(!g_gfx.sawText("OUT")); CHECK(g_gfx.sawText("A MIN 120 HOUSE  LEN 4"));
  CHECK(atRightEdge(g_gfx.onScreen("87%")));
  drainCmds();
}

TEST(fit_text_width_cuts_from_the_end) {
  char s[64]; strcpy(s, "A MIN 120 HOUSE  LEN 4  GYRO  OUT 78%");   // 37 cells of 6 px
  Ui::fitTextWidth(&g_gfx, s, 1000); CHECK(strcmp(s, "A MIN 120 HOUSE  LEN 4  GYRO  OUT 78%") == 0);   // fits: untouched
  Ui::fitTextWidth(&g_gfx, s, 185); CHECK(strcmp(s, "A MIN 120 HOUSE  LEN 4  GYRO") == 0);              // 30 cells, trailing spaces trimmed
  Ui::fitTextWidth(&g_gfx, s, 0); CHECK(s[0] == 0);
  Ui::fitTextWidth(nullptr, s, 10); Ui::fitTextWidth(&g_gfx, nullptr, 10);                              // tolerated
}

TEST(track_tap_queues_second_tap_cancels_then_plays) {
  Ui& u = ui(); drainCmds();
  tap(2, 0);                                         // set 1 Pad key
  std::vector<Cmd> c = peekCmds(); CHECK_EQ(countCmd(c, CmdTrackToggle), 1); CHECK_EQ((int)c[0].track, 2);
  runBlocks(1);
  CHECK_EQ((int)eng().status().track[2].state, (int)PSQueued);
  advanceMs(40); draw();
  std::set<int> cells = g_gfx.cellsTouched(); CHECK_EQ((int)cells.count(2), 1); CHECK_EQ((int)cells.count(7), 1); CHECK_EQ((int)cells.size(), 2);   // the tap also moved the selection off the Beat (SPEC §9.20)
  CHECK_EQ(u.selectedTrack(), 2);
  tap(2, 0);                                         // second tap before the bar line: cancel (SPEC §9.4)
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdCancelPending), 1); CHECK_EQ(countCmd(c, CmdTrackToggle), 0);
  runBlocks(1);
  CHECK_EQ((int)eng().status().track[2].state, (int)PSStopped);
  tap(2, 0); runToTick(nextBarTick() + 2);
  CHECK_EQ((int)eng().status().track[2].state, (int)PSPlaying);
  advanceMs(40); draw();
  cells = g_gfx.cellsTouched(); CHECK_EQ((int)cells.count(2), 1);
  int filled = 0; for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.w == kCellSize && o.h == kCellSize) filled++;
  CHECK_EQ(filled, 1);                               // playing = filled square
  tap(2, 0); runBlocks(1); CHECK_EQ((int)eng().status().track[2].state, (int)PSStopQueued);
  tap(2, 0); runBlocks(1); CHECK_EQ((int)eng().status().track[2].state, (int)PSPlaying);
  push(CmdTrackStopNow, 2); runBlocks(1);
  CHECK_EQ((int)u.page(), (int)PgMain);
  advanceMs(40); draw();                             // settle the stopped square
}

TEST(shift_track_selects_without_launch) {
  Ui& u = ui(); drainCmds();
  shiftDown(); CHECK(u.shiftDown());
  press(1, 0); release(1, 0);
  shiftUp(); CHECK(!u.shiftDown());
  CHECK_EQ(u.selectedTrack(), 1);
  CHECK_EQ((int)peekCmds().size(), 0);               // no launch
  runBlocks(1); CHECK_EQ((int)eng().status().track[1].state, (int)PSEmpty);
  advanceMs(40); draw();
  std::set<int> cells = g_gfx.cellsTouched(); cells.erase(-1);
  CHECK_EQ((int)cells.size(), 2); CHECK_EQ((int)cells.count(1), 1); CHECK_EQ((int)cells.count(2), 1);   // old (the Pad tapped by the previous test, SPEC §9.20) and new selection
  // SHIFT + set 2 track selects across sets
  shiftDown(); tap(3, 1); shiftUp();
  CHECK_EQ(u.selectedTrack(), 11);
  shiftDown(); tap(0, 0); shiftUp();
  CHECK_EQ(u.selectedTrack(), 0);
}

// ---------------------------------------------------------------------------------------------- parameters

// A param key steps once on Press and hold-ramps towards the end of its range while held (SPEC §9.17c): the Press
// sends CmdSetParam + CmdRamp (ticks = rampBars × 384 × remaining / range), Repeat sends nothing, the Release sends
// CmdFreezeRamp. A tap (press + release in one scan) is therefore one plain step.
TEST(param_press_sets_param_and_opens_page) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ(u.selectedTrack(), 0);
  press(0, 2);                                       // vol+: 0.80 → 0.85, then a hold-ramp to 1.0 over 4 × 384 × 0.15 ticks
  std::vector<Cmd> c = peekCmds(); const Cmd* p = findCmd(c, CmdSetParam, 0);
  CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PVolume); CHECK_NEAR(p->f, 0.85f, 1e-5); }
  const Cmd* r = findCmd(c, CmdRamp, 0); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PVolume); CHECK_NEAR(r->f, 1.0f, 1e-6); CHECK_EQ(r->i, (int)(4 * kTicksPerBar * 0.15f + 0.5f)); }
  CHECK_EQ((int)c.size(), 2);
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPVolume);
  key(0, 2, KeyEvent::Repeat, 400);                  // a repeat sends nothing: the ramp is running
  CHECK_EQ((int)peekCmds().size(), 2);
  release(0, 2, 450);                                // the release freezes the ramp where it is
  c = peekCmds(); const Cmd* fz = findCmd(c, CmdFreezeRamp, 0); CHECK(fz != nullptr); if (fz) CHECK_EQ((int)fz->a, (int)PVolume);
  CHECK_EQ((int)c.size(), 3);
  runBlocks(1);                                      // step, ramp and freeze land in the same block: the step alone
  CHECK(eng().params(0).volume >= 0.85f - 1e-5f && eng().params(0).volume < 0.86f);
  advanceMs(40); draw();
  CHECK_EQ(g_gfx.screens, 1); CHECK(g_gfx.sawText("VOLUME")); CHECK(g_gfx.sawText("0.85")); CHECK(g_gfx.sawText("S1 CHD"));
  for (int i = 0; i < 5; i++) { tap(0, 2); runBlocks(1); }
  CHECK_NEAR(eng().params(0).volume, 1.0f, 1e-5);    // clamped
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdRamp), 0); // at the end of the range no ramp is started
  for (int i = 0; i < 4; i++) { tap(0, 3); runBlocks(1); }
  CHECK_NEAR(eng().params(0).volume, 0.80f, 1e-5);   // no float drift on the 0.05 grid
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 0); CHECK(g_gfx.sawText("0.80")); CHECK(!g_gfx.sawText("VOLUME"));   // only the value redrawn
  tap(4, 2); runBlocks(1);                           // filter + (step 0.1)
  CHECK_NEAR(eng().params(0).filter, 0.1f, 1e-5); CHECK_EQ((int)u.paramPage(), (int)PPFilter);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("FILTER")); CHECK(g_gfx.sawText("+0.1"));
  tap(4, 3); runBlocks(1); CHECK_NEAR(eng().params(0).filter, 0.0f, 1e-5);
  tap(1, 2); runBlocks(1); CHECK_NEAR(eng().params(0).reverb, 0.05f, 1e-5);
  tap(2, 2); runBlocks(1); CHECK_NEAR(eng().params(0).echo, 0.05f, 1e-5);
  tap(3, 2); runBlocks(1); CHECK_NEAR(eng().params(0).dist, 0.05f, 1e-5);
  tap(1, 3); tap(2, 3); tap(3, 3); runBlocks(1);
  CHECK_NEAR(eng().params(0).reverb, 0.0f, 1e-5); CHECK_NEAR(eng().params(0).echo, 0.0f, 1e-5); CHECK_NEAR(eng().params(0).dist, 0.0f, 1e-5);
  advanceMs(1199 - kBlockMs); CHECK_EQ((int)u.page(), (int)PgParam);   // 1.2 s after the last key (SPEC §3)
  advanceMs(1); CHECK_EQ((int)u.page(), (int)PgMain);
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 1);        // back on main: full redraw
  drainCmds();
}

// The hold-ramp itself (SPEC §9.17c): the filter at 0 steps to +0.1 and ramps to +1 at the SHIFT-ramp speed (the whole
// −1..+1 range would take rampBars bars: 4 × 384 × 0.9 / 2 = 691 ticks), the page stays while the key is held,
// the release freezes the value where the ramp got and it stays there.
TEST(hold_ramp_runs_until_release_then_freezes) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ(u.selectedTrack(), 0);
  push(CmdSetParam, 0, PFilter, 0, 0.0f); runBlocks(1); drainCmds();
  press(4, 2);
  std::vector<Cmd> c = peekCmds(); const Cmd* p = findCmd(c, CmdSetParam, 0); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PFilter); CHECK_NEAR(p->f, 0.1f, 1e-5); }
  const Cmd* r = findCmd(c, CmdRamp, 0); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PFilter); CHECK_NEAR(r->f, 1.0f, 1e-6); CHECK_EQ(r->i, 691); }
  runBlocks(43);                                     // ≈ 0.5 s held ≈ 96 ticks: 0.1 + 0.9 × 96 / 691 ≈ 0.22
  const float mid = eng().params(0).filter;
  CHECK(mid > 0.15f && mid < 0.5f);
  std::printf("  filter after 0.5 s of the hold-ramp: %.3f\n", mid);
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPFilter);   // the page stays while the hold-ramp runs
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgParam);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("HOLD"));
  release(4, 2, 1900);
  c = peekCmds(); const Cmd* fz = findCmd(c, CmdFreezeRamp, 0); CHECK(fz != nullptr); if (fz) CHECK_EQ((int)fz->a, (int)PFilter);
  runBlocks(2);
  const float frozen = eng().params(0).filter;
  CHECK(frozen >= mid && frozen < mid + 0.1f);
  runBlocks(40); CHECK_NEAR(eng().params(0).filter, frozen, 1e-4);   // frozen: it stays
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);              // ... and the page has let go
  press(4, 3);                                       // the other way: a step down, then a ramp towards −1
  c = peekCmds(); r = findCmd(c, CmdRamp, 0); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PFilter); CHECK_NEAR(r->f, -1.0f, 1e-6); CHECK(r->i > 691); }
  release(4, 3); runBlocks(1);
  push(CmdSetParam, 0, PFilter, 0, 0.0f); runBlocks(1); drainCmds();
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
}

// A modal page opened while a param key is still held freezes its hold-ramp (the key's release lands on the perform /
// editor page, where it is dropped): the value stays where the ramp got instead of running to the end.
TEST(hold_ramp_is_frozen_when_a_modal_page_opens) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ(u.selectedTrack(), 0); CHECK_NEAR(eng().params(0).volume, 0.80f, 1e-5);
  press(0, 2); runBlocks(10);                        // vol+: 0.85, hold-ramp towards 1.0 ...
  tap(9, 3);                                         // ... and the perform page opens while vol+ is held
  CHECK_EQ((int)u.page(), (int)PgPerform);
  std::vector<Cmd> c = peekCmds(); CHECK_EQ(countCmd(c, CmdFreezeRamp), 1);
  const Cmd* fz = findCmd(c, CmdFreezeRamp, 0); CHECK(fz != nullptr); if (fz) CHECK_EQ((int)fz->a, (int)PVolume);
  runBlocks(2); const float frozen = eng().params(0).volume; CHECK(frozen >= 0.85f - 1e-5f && frozen < 0.95f);
  release(0, 2, 200); CHECK(peekCmds().empty());     // the release on the perform page sends nothing (fn is no grid key)
  runBlocks(86); CHECK_NEAR(eng().params(0).volume, frozen, 1e-4);   // the volume stays
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  press(3, 2); runBlocks(5);                         // the editor likewise: dist+ held ...
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgEditor);
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdFreezeRamp), 1); fz = findCmd(c, CmdFreezeRamp, 0); if (fz) CHECK_EQ((int)fz->a, (int)PDist);
  runBlocks(1); release(3, 2, 100); CHECK(peekCmds().empty());      // the editor drops every release
  runBlocks(43); const float d = eng().params(0).dist; CHECK(d >= 0.05f - 1e-5f && d < 0.2f);
  runBlocks(43); CHECK_NEAR(eng().params(0).dist, d, 1e-4);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdSetParam, 0, PVolume, 0, 0.8f); push(CmdSetParam, 0, PDist, 0, 0.0f); runBlocks(1); drainCmds();
}

// One hold-ramp per parameter: vol− pressed while vol+ still holds its ramp freezes that ramp first (the engine would
// otherwise reuse the active slot with the first ramp's start and jump back to it), steps from the value reached and
// ramps down; the first key's release sends nothing, the second's freezes the down ramp.
TEST(second_param_press_freezes_the_running_hold_ramp_first) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ(u.selectedTrack(), 0);
  push(CmdSetParam, 0, PVolume, 0, 0.5f); runBlocks(1); drainCmds();
  press(0, 2);                                       // 0.50 → 0.55, then towards 1.0 over 4 × 384 × 0.45 = 691 ticks
  std::vector<Cmd> c = peekCmds(); CHECK_EQ(countCmd(c, CmdRamp), 1); CHECK_EQ(countCmd(c, CmdFreezeRamp), 0);
  runBlocks(86);                                     // ≈ 1 s ≈ 192 ticks: 0.55 + 0.45 × 192 / 691 ≈ 0.675
  const float mid = eng().params(0).volume; CHECK(mid > 0.62f && mid < 0.73f);
  std::printf("  volume when the second key lands: %.3f\n", mid);
  press(0, 3);                                       // vol− while vol+ is still held
  c = peekCmds(); CHECK_EQ((int)c.size(), 3);
  const float stepped = roundf((mid - 0.05f) / 0.05f) * 0.05f;
  if (c.size() == 3) {
    CHECK_EQ((int)c[0].type, (int)CmdFreezeRamp); CHECK_EQ((int)c[0].a, (int)PVolume); CHECK_EQ((int)c[0].track, 0);
    CHECK_EQ((int)c[1].type, (int)CmdSetParam); CHECK_NEAR(c[1].f, stepped, 1e-5);          // one step down from where the ramp got
    CHECK_EQ((int)c[2].type, (int)CmdRamp); CHECK_EQ((int)c[2].a, (int)PVolume); CHECK_NEAR(c[2].f, 0.0f, 1e-6);
  }
  runBlocks(1);
  CHECK_NEAR(eng().params(0).volume, stepped, 0.01f);   // the down ramp starts there, not at the first ramp's 0.55
  release(0, 2, 1100); CHECK(peekCmds().empty());    // the first key no longer owns a ramp
  runBlocks(43); const float later = eng().params(0).volume; CHECK(later < stepped - 0.02f);   // the down ramp runs on
  release(0, 3, 1600);                               // the second key's release freezes it
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdFreezeRamp), 1); runBlocks(2);
  const float frozen = eng().params(0).volume; runBlocks(40); CHECK_NEAR(eng().params(0).volume, frozen, 1e-4);
  push(CmdSetParam, 0, PVolume, 0, 0.8f); runBlocks(1); drainCmds();
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
}

TEST(shift_vol_ramps_then_release_snaps_back) {
  Ui& u = ui(); drainCmds();
  CHECK_NEAR(eng().params(0).volume, 0.80f, 1e-5);
  shiftDown(); press(0, 2);
  std::vector<Cmd> c = peekCmds(); const Cmd* r = findCmd(c, CmdRamp, 0);
  CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PVolume); CHECK_NEAR(r->f, 1.0f, 1e-6); CHECK_EQ(r->i, 4 * kTicksPerBar); }
  CHECK_EQ(countCmd(c, CmdSetParam), 0);
  key(0, 2, KeyEvent::Repeat, 400);                  // no hold-repeat under SHIFT
  CHECK_EQ((int)peekCmds().size(), 1);
  runBlocks(86);                                     // ≈ 1 s of a 4-bar (8 s) ramp at 120 BPM
  float v = eng().params(0).volume;
  CHECK(v > 0.81f && v < 0.86f);
  std::printf("  volume after 1 s of the ramp: %.3f\n", v);
  advanceMs(2000);                                   // the page stays while its ramp runs under SHIFT
  CHECK_EQ((int)u.page(), (int)PgParam);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("RAMP"));
  release(0, 2, 3000);
  shiftUp();
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdCancelRamps), 1);
  runBlocks(2);
  CHECK_NEAR(eng().params(0).volume, 0.80f, 1e-5);   // snapped back to the value before the ramp
  advanceMs(1250); CHECK_EQ((int)u.page(), (int)PgMain);
  // SHIFT + vol− ramps to the minimum; releasing SHIFT without a param key still cancels
  shiftDown(); press(1, 3); release(1, 3);
  c = peekCmds(); r = findCmd(c, CmdRamp, 0); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PReverb); CHECK_NEAR(r->f, 0.0f, 1e-6); }
  shiftUp(); c = peekCmds(); CHECK_EQ(countCmd(c, CmdCancelRamps), 1);
  runBlocks(1); drainCmds();
}

TEST(tempo_length_key_genre_pages) {
  Ui& u = ui(); drainCmds();
  press(6, 2);                                       // tempo+
  std::vector<Cmd> c = peekCmds(); const Cmd* t = findCmd(c, CmdSetTempo); CHECK(t != nullptr); if (t) CHECK_EQ(t->i, 121);
  key(6, 2, KeyEvent::Repeat, 400);                  // repeat before the engine published 121: still +1 (not +0)
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdSetTempo), 2); CHECK_EQ(c.back().i, 122);
  release(6, 2, 500); runBlocks(1);
  CHECK_EQ((int)eng().status().bpm, 122);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("TEMPO")); CHECK(g_gfx.sawText("122"));
  tap(6, 3); tap(6, 3); runBlocks(1); CHECK_EQ((int)eng().status().bpm, 120);
  shiftDown(); press(6, 2);                          // SHIFT + tempo: drift +10 BPM over 60 beats (SPEC §9.17c)
  c = peekCmds(); const Cmd* r = findCmd(c, CmdRamp); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PTempo); CHECK_NEAR(r->f, 130.0f, 1e-6); CHECK_EQ(r->i, 60 * kPPQN); }
  CHECK_EQ(countCmd(c, CmdSetTempo), 0);
  key(6, 2, KeyEvent::Repeat, 400); CHECK_EQ((int)peekCmds().size(), 1);   // no repeat under SHIFT
  release(6, 2); shiftUp(); c = peekCmds(); CHECK_EQ(countCmd(c, CmdCancelRamps), 0);   // a drift is not a SHIFT ramp: nothing snaps back
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPTempo);
  advanceMs(40); draw(); CHECK(g_gfx.onScreen("TEMPO") != nullptr); CHECK(g_gfx.sawText("DRIFT"));   // the page was already up: only the sub line changed
  runBlocks(300);                                    // ≈ 3.5 s of the 30 s drift: + 1.2 BPM
  CHECK(eng().status().bpm >= 121 && eng().status().bpm <= 122);
  std::printf("  bpm after 3.5 s of the drift: %d\n", (int)eng().status().bpm);
  push(CmdFreezeRamp, 0, PTempo); push(CmdSetTempo, 0, 0, 120); runBlocks(2); CHECK_EQ((int)eng().status().bpm, 120); drainCmds();
  shiftDown(); press(6, 3); c = peekCmds(); r = findCmd(c, CmdRamp); CHECK(r != nullptr); if (r) { CHECK_NEAR(r->f, 110.0f, 1e-6); CHECK_EQ(r->i, 60 * kPPQN); }   // and down
  release(6, 3); shiftUp(); push(CmdFreezeRamp, 0, PTempo); push(CmdSetTempo, 0, 0, 120); runBlocks(2); CHECK_EQ((int)eng().status().bpm, 120); drainCmds();
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
  // key ± walks the circle of fifths (SPEC §2), shows "E MIN"
  tap(7, 2); CHECK_EQ((int)gen().key().root, 4);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("KEY")); CHECK(g_gfx.sawText("E MIN"));
  tap(7, 3); CHECK_EQ((int)gen().key().root, 9);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("A MIN"));
  // len ± on a loaded track: natural × 2 (SPEC §9.4), page shows the new length and from → to
  shiftDown(); tap(2, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 2);
  tap(5, 2);
  c = peekCmds(); const Cmd* l = findCmd(c, CmdSetLength, 2); CHECK(l != nullptr); if (l) CHECK_EQ(l->i, 2 * kTicksPerBar);
  CHECK_EQ((int)u.paramPage(), (int)PPLength);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("LENGTH")); CHECK(g_gfx.sawText("8 BEATS")); CHECK(g_gfx.sawText("4 -> 8"));
  runBlocks(1); drainCmds();
  tap(5, 3); c = peekCmds(); l = findCmd(c, CmdSetLength, 2); CHECK(l != nullptr); if (l) CHECK_EQ(l->i, kTicksPerBar);
  runBlocks(1); drainCmds();
  // SHIFT + len ± cycles rampBars 1 2 4 8 16
  CHECK_EQ(u.rampBars(), 4);
  shiftDown(); tap(5, 2); CHECK_EQ(u.rampBars(), 8); tap(5, 2); CHECK_EQ(u.rampBars(), 16); tap(5, 2); CHECK_EQ(u.rampBars(), 1);
  tap(5, 3); CHECK_EQ(u.rampBars(), 16); tap(5, 3); tap(5, 3); tap(5, 3); CHECK_EQ(u.rampBars(), 2); shiftUp();
  CHECK_EQ((int)u.paramPage(), (int)PPRampBars);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("RAMP")); CHECK(g_gfx.sawText("2 BARS"));
  shiftDown(); tap(5, 2); shiftUp(); CHECK_EQ(u.rampBars(), 4);
  CHECK_EQ((int)peekCmds().size(), 0);               // rampBars presses send nothing
  // genre rotates through the packs (House, Testpack)
  const int g0 = gen().genreIndex();
  tap(9, 2); CHECK(gen().genreIndex() != g0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("GENRE")); CHECK(g_gfx.sawText("TESTPACK"));
  tap(9, 2); CHECK_EQ(gen().genreIndex(), g0);
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
}

// A tempo drift never locks the tempo keys: a plain tempo± freezes the PTempo ramp first (the engine would overwrite
// CmdSetTempo on the next tick) and steps from the tempo reached; SHIFT + the same key again stops the drift where it
// is, SHIFT + the other key freezes it and drifts the other way from there.
TEST(tempo_keys_take_over_from_a_drift) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ((int)eng().status().bpm, 120);
  shiftDown(); tap(6, 2); shiftUp();                 // drift towards 130 over 60 beats
  std::vector<Cmd> c = peekCmds(); CHECK_EQ(countCmd(c, CmdRamp), 1);
  runBlocks(4); advanceMs(40); draw(); CHECK(g_gfx.sawText("DRIFT"));   // the TEMPO page says DRIFT while it runs
  runBlocks(596);                                    // ≈ 7 s of the ≈ 30 s drift: ≈ +2.4 BPM (the page has long returned)
  const int mid = eng().status().bpm; CHECK(mid >= 121 && mid <= 124);
  std::printf("  bpm when tempo- lands on the drift: %d\n", mid);
  CHECK_EQ((int)u.page(), (int)PgMain);
  tap(6, 3);                                         // tempo− without SHIFT: freeze, then one step down from here
  c = peekCmds(); CHECK_EQ((int)c.size(), 2);
  if (c.size() == 2) { CHECK_EQ((int)c[0].type, (int)CmdFreezeRamp); CHECK_EQ((int)c[0].a, (int)PTempo); CHECK_EQ((int)c[1].type, (int)CmdSetTempo); CHECK_EQ(c[1].i, mid - 1); }
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPTempo);
  runBlocks(2); CHECK_EQ((int)eng().status().bpm, mid - 1);
  advanceMs(40); draw(); CHECK(!g_gfx.sawText("DRIFT")); CHECK(g_gfx.onScreen("DRIFT") == nullptr);   // ... no longer drifting
  runBlocks(300); CHECK_EQ((int)eng().status().bpm, mid - 1);   // ... and it stays: the drift is over
  shiftDown(); tap(6, 2); shiftUp(); runBlocks(300); // a new drift up ...
  const int mid2 = eng().status().bpm; CHECK(mid2 > mid - 1);
  shiftDown(); tap(6, 2); shiftUp();                 // ... the same key again stops it where it is
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdFreezeRamp), 1); CHECK_EQ(countCmd(c, CmdRamp), 0);
  runBlocks(300); CHECK_EQ((int)eng().status().bpm, mid2);
  shiftDown(); tap(6, 2); shiftUp(); runBlocks(300); // up again ...
  const int b = eng().status().bpm; CHECK(b > mid2);
  shiftDown(); tap(6, 3); shiftUp();                 // ... turned round: freeze, then a drift towards b − 10 from here
  c = peekCmds(); CHECK_EQ((int)c.size(), 2);
  if (c.size() == 2) { CHECK_EQ((int)c[0].type, (int)CmdFreezeRamp); CHECK_EQ((int)c[1].type, (int)CmdRamp); CHECK_EQ((int)c[1].a, (int)PTempo); CHECK_NEAR(c[1].f, (float)(b - 10), 1e-6); CHECK_EQ(c[1].i, 60 * kPPQN); }
  CHECK_EQ((int)u.page(), (int)PgParam); advanceMs(40); draw(); CHECK(g_gfx.sawText("DRIFT"));
  runBlocks(2); const int turned = eng().status().bpm; CHECK(turned >= b - 1 && turned <= b);
  runBlocks(600); CHECK(eng().status().bpm < turned);
  push(CmdFreezeRamp, 0, PTempo); push(CmdSetTempo, 0, 0, 120); runBlocks(2); CHECK_EQ((int)eng().status().bpm, 120); drainCmds();
  tap(6, 2); tap(6, 3); runBlocks(2); CHECK_EQ((int)eng().status().bpm, 120); drainCmds();   // the UI's drift state is cleared by the plain press
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
}

// SHIFT + key±: the selected track's pattern an octave up / down (Generator::transposeOctave, SPEC §9.17c) with a
// 0.6 s "OCTAVE +1" page; the global key is untouched. Only a synth track with notes transposes: Beat / Percussion /
// Vocal and an empty source get a status line, no page.
TEST(shift_key_transposes_the_selected_track) {
  Ui& u = ui(); drainCmds();
  shiftDown(); tap(2, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 2);   // set 1 Pad, loaded with pitch 60 by an earlier test
  const Pattern& src = eng().sourcePattern(2);
  const int p0 = src.count ? src.notes[0].pitch() : -1;
  CHECK(p0 == 60);
  shiftDown(); tap(7, 2); shiftUp();                 // SHIFT + key+
  CHECK_EQ((int)gen().key().root, 9);                // the key did not move
  CHECK_EQ((int)u.page(), (int)PgNote); advanceMs(40); draw(); CHECK(g_gfx.sawText("OCTAVE +1"));
  if (p0 >= 0 && src.count) CHECK_EQ((int)src.notes[0].pitch(), p0 + 12);
  advanceMs(700); CHECK_EQ((int)u.page(), (int)PgMain); runBlocks(1); drainCmds();
  shiftDown(); tap(7, 3); shiftUp();                 // SHIFT + key−
  CHECK_EQ((int)gen().key().root, 9);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("OCTAVE -1"));
  if (p0 >= 0 && src.count) CHECK_EQ((int)src.notes[0].pitch(), p0);
  advanceMs(700); CHECK_EQ((int)u.page(), (int)PgMain); runBlocks(1); drainCmds();
  tap(7, 2); CHECK_EQ((int)gen().key().root, 4); tap(7, 3); CHECK_EQ((int)gen().key().root, 9);   // without SHIFT the key still cycles
  advanceMs(1300); drainCmds();
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);   // the Beat: nothing to transpose
  const Pattern& beat = eng().sourcePattern(7); const int bp = beat.count ? beat.notes[0].pitch() : -1;
  shiftDown(); tap(7, 2); shiftUp();
  CHECK_EQ((int)u.page(), (int)PgMain); advanceMs(1); CHECK(strcmp(u.statusText(), "OCTAVE: SYNTH TRACKS ONLY") == 0);
  if (bp >= 0 && beat.count) CHECK_EQ((int)beat.notes[0].pitch(), bp);
  CHECK(peekCmds().empty()); CHECK_EQ((int)gen().key().root, 9);
  eng().sourcePattern(11).clear();                   // set 2 Bass without a pattern
  shiftDown(); tap(3, 1); shiftUp(); CHECK_EQ(u.selectedTrack(), 11);
  shiftDown(); tap(7, 3); shiftUp();
  CHECK_EQ((int)u.page(), (int)PgMain); advanceMs(1); CHECK(strcmp(u.statusText(), "OCTAVE: NO PATTERN") == 0);
  shiftDown(); tap(0, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 0);
  advanceMs(1100); drainCmds();
}

// SPEC §9.16: with a Vocal selected, len± steps its pause 0 / 1 / 2 / 4 bars; the length page says "PAUSE n BAR(S)"
// (not "n BEATS") with the cycle it makes, the engine gets the new length through the stream switch (CmdSwapPattern
// with (2 + pause) × 384 ticks on the switch bar, never a CmdSetLength), the footer's LEN readout shows the cycle once
// the bar line applied it, and on a loop variant len± does nothing and the page says NO PAUSE with the loop.
// A card file by folder and basename: the folder itself, or one of its shard subfolders (a content folder may be sharded,
// SPEC §9.2 — the House vocal NA folder was sharded during the 2026-10-16 wave), so the fixture path follows the card.
static std::string locateCardFile(const std::string& folder, const std::string& base) {
  if (cardExists(folder + "/" + base)) return folder + "/" + base;
  std::vector<std::string> kids;
  sd().walk(folder.c_str(), [](const char* name, bool isDir, uint32_t, void* user) -> bool { if (isDir) ((std::vector<std::string>*)user)->push_back(name); return true; }, &kids);
  for (const std::string& k : kids) if (cardExists(folder + "/" + k + "/" + base)) return folder + "/" + k + "/" + base;
  return folder + "/" + base;
}

TEST(vocal_length_page_shows_the_pause) {
  Ui& u = ui(); drainCmds();
  static std::string phrasePath = locateCardFile("/lp/packs/House/vocal/NA", "NA__110bpm__GHUAC__Vocal__Adlib_1_Minor_Wet.wav");   // a phrase: without one there is no cycle to switch
  const char* phrase = phrasePath.c_str();
  CHECK(gen().loadVocal(0, phrase, 0)); gen().setVocalPath(0, 1, phrase);
  { int guard = 0; while (guard++ < 400 && (gen().vocalSwitchPending(0) || eng().status().track[Vocal].lenTicks != 3 * kTicksPerBar)) iterate(); }
  CHECK_EQ((int)eng().status().track[Vocal].lenTicks, 3 * kTicksPerBar); drainCmds();
  shiftDown(); tap(6, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), (int)Vocal);
  CHECK_EQ((int)gen().info(Vocal).pause, 1);
  tap(5, 2);                                         // len+: pause 1 → 2 bars
  std::vector<Cmd> c = peekCmds(); CHECK_EQ(countCmd(c, CmdSetLength), 0); CHECK(gen().vocalSwitchPending(0));
  CHECK_EQ((int)gen().info(Vocal).pause, 2); CHECK_EQ((int)gen().info(Vocal).lenStep, 0);
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPLength);
  advanceMs(40); draw();
  CHECK(g_gfx.sawText("LENGTH")); CHECK(g_gfx.drawn("PAUSE 2 BARS") != nullptr); CHECK(g_gfx.sawText("CYCLE 4 BARS")); CHECK(!g_gfx.sawText("BEATS"));
  const TextOp* big = g_gfx.drawn("PAUSE 2 BARS"); if (big) CHECK_EQ((int)big->c, (int)pageParamColor(PPLength));   // the length colour
  runBlocks(1); drainCmds();
  tap(5, 3); tap(5, 3); tap(5, 3);                   // len−: 2 → 1 → 0 → 0 (clamped)
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdSetLength), 0);
  CHECK_EQ((int)gen().info(Vocal).pause, 0); CHECK_EQ((int)gen().setLengthStep(Vocal, 0), 2 * kTicksPerBar);
  advanceMs(40); draw(); CHECK(g_gfx.drawn("PAUSE 0") != nullptr); CHECK(g_gfx.sawText("CYCLE 2 BARS"));
  runBlocks(1); drainCmds();
  tap(5, 2);                                         // 0 → 1
  advanceMs(40); draw(); CHECK(g_gfx.drawn("PAUSE 1 BAR") != nullptr); CHECK(g_gfx.sawText("CYCLE 3 BARS"));
  tap(5, 2); tap(5, 2); tap(5, 2);                   // 1 → 2 → 4 → 4 (clamped)
  CHECK_EQ((int)gen().info(Vocal).pause, 4); CHECK_EQ((int)gen().setLengthStep(Vocal, 0), 6 * kTicksPerBar);
  advanceMs(40); draw(); CHECK(g_gfx.drawn("PAUSE 4 BARS") != nullptr); CHECK(g_gfx.sawText("CYCLE 6 BARS"));
  runBlocks(1); drainCmds();
  tap(5, 3); tap(5, 3);                              // back to 1
  CHECK_EQ((int)gen().info(Vocal).pause, 1); CHECK_EQ((int)gen().setLengthStep(Vocal, 0), 3 * kTicksPerBar);
  // the switch publishes the length: the vocal's CmdSwapPattern carries the cycle (no CmdSetLength) and the engine
  // applies it on the switch bar (the pump pushes it in the first step, the idle vocal switches at once)
  { int guard = 0; bool seen = false;
    while (guard++ < 400 && (gen().vocalSwitchPending(0) || eng().status().track[Vocal].lenTicks != 3 * kTicksPerBar)) {
      gen().step(); sets().step();
      for (const Cmd& k : peekCmds()) { CHECK(k.type != CmdSetLength || k.track != Vocal); if (k.type == CmdSwapPattern && k.track == Vocal) { seen = true; CHECK_EQ(k.i, 3 * kTicksPerBar); } }
      eng().render(g_out, kBlock); g_now += kBlockMs; ui().tick(g_now);
    }
    CHECK(seen); }
  CHECK_EQ((int)eng().status().track[Vocal].lenTicks, 3 * kTicksPerBar);
  drainCmds();
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
  draw(); CHECK(g_gfx.sawText("A MIN 120 HOUSE  LEN 12"));   // the footer shows the cycle for the selected vocal
  // the editor's vocal line names the pause too
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgEditor);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("VOCAL: no pattern")); CHECK(g_gfx.sawText("PAUSE 1 BAR  CYCLE 12"));
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgMain);
  drainCmds();
  // a loop variant (LOOP 2; the pat keys address the Beat only, SPEC §9.17c, so the generator selects it here) has no
  // pause: len± does nothing and the page says so
  gen().rebuildVariant(Vocal, 2); runBlocks(1); drainCmds();
  CHECK_EQ((int)gen().info(Vocal).variant, 2);
  tap(5, 2);
  CHECK(peekCmds().empty()); CHECK_EQ((int)gen().info(Vocal).pause, 1); CHECK_EQ((int)gen().setLengthStep(Vocal, 0), 2 * kTicksPerBeat);
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPLength);
  advanceMs(40); draw(); CHECK(g_gfx.drawn("NO PAUSE") != nullptr); CHECK(g_gfx.sawText("LOOP 2 BEATS")); CHECK(!g_gfx.sawText("PAUSE 1")); CHECK(!g_gfx.sawText("CYCLE"));
  const TextOp* np = g_gfx.drawn("NO PAUSE"); if (np) CHECK_EQ((int)np->c, (int)pageParamColor(PPLength));
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
  tap(8, 3); advanceMs(40); draw(); CHECK(g_gfx.sawText("LOOP 2 BEATS  NO PAUSE")); CHECK(!g_gfx.sawText("len +/-")); tap(8, 3);
  press(8, 0); release(8, 0); runBlocks(1); drainCmds();   // pat1 goes to the Beat: the Vocal stays on its loop ...
  CHECK_EQ((int)gen().info(Vocal).variant, 2); advanceMs(700);
  gen().rebuildVariant(Vocal, 0); runBlocks(1); drainCmds();   // ... until the generator puts it back on the window + pause cycle
  CHECK_EQ((int)gen().info(Vocal).variant, 0);
  drainCmds();
}

// ---------------------------------------------------------------------------------------------- variants, rush, cut

// pat1..4 of a row ALWAYS address that set's Beat track, never the selected track (SPEC §9.17c).
TEST(variant_key_always_routes_to_beat_of_that_set) {
  Ui& u = ui(); drainCmds();
  shiftDown(); tap(0, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 0);   // selection in set 1
  press(10, 1);                                      // pat3 of set 2 → set 2's Beat (track 15)
  std::vector<Cmd> c = peekCmds(); const Cmd* m = findCmd(c, CmdBeatMask);
  CHECK(m != nullptr); if (m) { CHECK_EQ((int)m->a, 1); CHECK_EQ(m->i & 0xFF, 0x0F); CHECK_EQ((m->i >> 8) & 3, 2); }
  const Cmd* sw = findCmd(c, CmdSwapPattern, 15); CHECK(sw != nullptr); if (sw) CHECK_EQ((int)sw->a, 2);   // Full rebuilt into next (SPEC §9.6)
  CHECK_EQ(countCmd(c, CmdSwapPattern), 1);
  CHECK_EQ((int)u.page(), (int)PgNote);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S2 BEAT 3 +SNARE"));
  release(10, 1); runBlocks(1); drainCmds();
  advanceMs(599 - 40 - kBlockMs); CHECK_EQ((int)u.page(), (int)PgNote);   // 0.6 s page (SPEC §9.14)
  advanceMs(1); CHECK_EQ((int)u.page(), (int)PgMain);
  // the selected track belongs to the set: the press STILL addresses the set's Beat (SPEC §9.17c)
  loadTestPattern(9, 62);                            // set 2 Lead
  shiftDown(); tap(1, 1); shiftUp(); CHECK_EQ(u.selectedTrack(), 9); drainCmds();
  press(9, 1); release(9, 1);                        // pat2 of set 2
  c = peekCmds(); CHECK(findCmd(c, CmdSwapPattern, 9) == nullptr);
  m = findCmd(c, CmdBeatMask); CHECK(m != nullptr); if (m) { CHECK_EQ((int)m->a, 1); CHECK_EQ(m->i & 0xFF, 0x0D); CHECK_EQ((m->i >> 8) & 3, 1); }   // +HATS {36, 38, 39}
  CHECK(findCmd(c, CmdSwapPattern, 15) != nullptr);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S2 BEAT 2 +HATS")); CHECK(!g_gfx.sawText("LED"));
  runBlocks(1); drainCmds();
  press(8, 0); release(8, 0);                        // pat1 of set 1 while set 2 is selected → set 1 Beat
  c = peekCmds(); m = findCmd(c, CmdBeatMask); CHECK(m != nullptr); if (m) { CHECK_EQ((int)m->a, 0); CHECK_EQ(m->i & 0xFF, 0x01); }
  runBlocks(1); drainCmds();
  // while the set generates the press is queued and applied afterwards (SPEC §9.5), to the Beat
  push(CmdSetGenerating, 0, 1, 1); runBlocks(1); CHECK_EQ((int)eng().status().generating[1], 1);
  press(11, 1); release(11, 1);                      // pat4 of set 2
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdSwapPattern), 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("QUEUED"));
  push(CmdSetGenerating, 0, 1, 0); runBlocks(1);
  c = peekCmds(); const Cmd* s = findCmd(c, CmdSwapPattern, 15); CHECK(s != nullptr); if (s) CHECK_EQ((int)s->a, 3);
  CHECK(findCmd(c, CmdSwapPattern, 9) == nullptr);
  m = findCmd(c, CmdBeatMask); CHECK(m != nullptr); if (m) CHECK_EQ((m->i >> 8) & 3, 3);
  runBlocks(1); drainCmds();
  advanceMs(700);
}

TEST(len_press_while_generating_is_queued_until_the_swap) {
  Ui& u = ui(); drainCmds();
  loadTestPattern(9, 62); gen().info(9).lenStep = 0; // set 2 Lead, natural length 1 bar
  shiftDown(); tap(1, 1); shiftUp(); CHECK_EQ(u.selectedTrack(), 9);
  push(CmdSetGenerating, 0, 1, 1); runBlocks(1); CHECK_EQ((int)eng().status().generating[1], 1);
  drainCmds();
  tap(5, 2);                                         // len+ while set 2 generates (SPEC §9.5): nothing reaches the old pattern
  std::vector<Cmd> c = peekCmds(); CHECK_EQ(countCmd(c, CmdSetLength), 0);
  CHECK_EQ((int)gen().info(9).lenStep, 0);
  CHECK_EQ((int)u.page(), (int)PgNote); advanceMs(40); draw(); CHECK(g_gfx.sawText("S2 LEN +1 QUEUED"));
  tap(5, 2); tap(5, 3);                              // +1 −1: still one step queued
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S2 LEN +1 QUEUED"));
  CHECK_EQ(countCmd(peekCmds(), CmdSetLength), 0);
  runBlocks(4); CHECK_EQ(countCmd(peekCmds(), CmdSetLength), 0);   // still generating: still queued
  push(CmdSetGenerating, 0, 1, 0); runBlocks(1);     // the set swapped: the step is applied now, on the new natural length
  c = peekCmds(); const Cmd* l = findCmd(c, CmdSetLength, 9); CHECK(l != nullptr); if (l) CHECK_EQ(l->i, 2 * kTicksPerBar);
  CHECK_EQ(countCmd(c, CmdSetLength), 1);
  CHECK_EQ((int)gen().info(9).lenStep, 1);
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPLength);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("8 BEATS")); CHECK(g_gfx.sawText("4 -> 8"));
  runBlocks(1); drainCmds();
  tap(5, 3); runBlocks(1); drainCmds(); CHECK_EQ((int)gen().info(9).lenStep, 0);   // back to 1 bar
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain);
}

TEST(snare_rush_and_kick_cut) {
  Ui& u = ui(); drainCmds();
  press(12, 0);
  std::vector<Cmd> c = peekCmds(); const Cmd* r = findCmd(c, CmdSnareRush); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, 0); CHECK_EQ(r->i, 1); }
  runBlocks(1); drainCmds();
  release(12, 0, 300);
  c = peekCmds(); r = findCmd(c, CmdSnareRush); CHECK(r != nullptr); if (r) CHECK_EQ(r->i, 0);
  runBlocks(1); drainCmds();
  advanceMs(700); CHECK_EQ((int)u.page(), (int)PgMain);
  tap(13, 1);                                        // kick cut of set 2
  c = peekCmds(); const Cmd* k = findCmd(c, CmdKickCut); CHECK(k != nullptr); if (k) { CHECK_EQ((int)k->a, 1); CHECK_EQ(k->i, 1); }
  CHECK_EQ((int)u.page(), (int)PgNote); advanceMs(40); draw(); CHECK(g_gfx.sawText("S2 KICK CUT"));
  runBlocks(1); CHECK_EQ((int)eng().status().track[15].kickCut, 1);
  advanceMs(700); CHECK_EQ((int)u.page(), (int)PgMain);
  advanceMs(40); draw();
  CHECK_EQ(g_gfx.screens, 1);                        // back from the note page: full redraw ...
  CHECK(g_gfx.lines >= 1);                           // ... with the diagonal strike through set 2's Beat square
  drainCmds();
  tap(13, 1); c = peekCmds(); k = findCmd(c, CmdKickCut); CHECK(k != nullptr); if (k) CHECK_EQ(k->i, 0);
  runBlocks(1); CHECK_EQ((int)eng().status().track[15].kickCut, 0);
  advanceMs(700); drainCmds();
}

// ---------------------------------------------------------------------------------------------- output volume (SPEC §3)

static int g_volHookCalls = 0, g_volHookLast = -1;
static void volHook(int v) { g_volHookCalls++; g_volHookLast = v; }

TEST(shift_kick_cut_steps_the_output_volume) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ((int)u.page(), (int)PgMain);
  CHECK_EQ(u.outputVolume(), 150);                   // default (kOutputVolumeDefault)
  u.setOutputVolumeHook(volHook);
  CHECK_EQ(g_volHookCalls, 1); CHECK_EQ(g_volHookLast, 150);   // installing the hook applies the current level
  u.setOutputVolume(200); CHECK_EQ(g_volHookCalls, 2);          // start the stepping test from 200
  shiftDown(); press(13, 0);                         // SHIFT + kick cut S1: +16
  CHECK_EQ(u.outputVolume(), 216); CHECK_EQ(g_volHookLast, 216); CHECK_EQ(g_volHookCalls, 3);
  CHECK_EQ(countCmd(peekCmds(), CmdKickCut), 0);     // not a kick cut
  CHECK_EQ((int)u.page(), (int)PgParam); CHECK_EQ((int)u.paramPage(), (int)PPMaster);
  advanceMs(40); draw();
  CHECK_EQ(g_gfx.screens, 1); CHECK(g_gfx.sawText("OUTPUT")); CHECK(g_gfx.sawText("85%"));   // 166 / 255
  key(13, 0, KeyEvent::Repeat, 400);                 // hold repeats
  CHECK_EQ(u.outputVolume(), 232); CHECK_EQ(g_volHookLast, 232);
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 0); CHECK(g_gfx.sawText("91%")); CHECK(!g_gfx.sawText("OUTPUT"));   // only the value redrawn
  release(13, 0, 450);
  for (int i = 0; i < 3; i++) tap(13, 0);
  CHECK_EQ(u.outputVolume(), 255); CHECK_EQ(g_volHookLast, 255);   // clamped at 255
  const int calls = g_volHookCalls;
  tap(13, 0); CHECK_EQ(u.outputVolume(), 255); CHECK_EQ(g_volHookCalls, calls);   // no change: the hook is not called
  advanceMs(40); draw(); CHECK(g_gfx.sawText("100%"));
  press(13, 1); CHECK_EQ(u.outputVolume(), 239); CHECK_EQ(g_volHookLast, 239); release(13, 1);   // SHIFT + kick cut S2: −16
  key(13, 1, KeyEvent::Press); key(13, 1, KeyEvent::Repeat, 400); release(13, 1, 450);
  CHECK_EQ(u.outputVolume(), 207);
  for (int i = 0; i < 20; i++) tap(13, 1);
  CHECK_EQ(u.outputVolume(), 0); CHECK_EQ(g_volHookLast, 0);   // clamped at 0
  advanceMs(40); draw(); CHECK(g_gfx.sawText("0%"));
  shiftUp();
  CHECK_EQ(countCmd(peekCmds(), CmdKickCut), 0);     // nothing reached the engine
  runBlocks(1); CHECK_EQ((int)eng().status().track[7].kickCut, 0); CHECK_EQ((int)eng().status().track[15].kickCut, 0);
  // a config value at boot: applied through the hook, clamped, no page
  u.setOutputVolume(200); CHECK_EQ(u.outputVolume(), 200); CHECK_EQ(g_volHookLast, 200);
  u.setOutputVolume(300); CHECK_EQ(u.outputVolume(), 255); u.setOutputVolume(-5); CHECK_EQ(u.outputVolume(), 0); CHECK_EQ(g_volHookLast, 0);
  u.setOutputVolume(200); CHECK_EQ((int)u.paramPage(), (int)PPMaster);
  advanceMs(1300 - kBlockMs); CHECK_EQ((int)u.page(), (int)PgMain);   // the page auto-returns like the others (1.2 s)
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 1); CHECK(g_gfx.sawText("OUT 78%"));   // the footer shows the level for a while
  advanceMs(3000); draw(); CHECK(!g_gfx.sawText("OUT ")); CHECK(g_gfx.sawText("LEN"));   // ... then returns to its usual text
  // without SHIFT the kick cut toggles as before (SPEC §9.6)
  drainCmds();
  tap(13, 0);
  std::vector<Cmd> c = peekCmds(); const Cmd* k = findCmd(c, CmdKickCut); CHECK(k != nullptr); if (k) { CHECK_EQ((int)k->a, 0); CHECK_EQ(k->i, 1); }
  CHECK_EQ(u.outputVolume(), 200); CHECK_EQ((int)u.page(), (int)PgNote);
  runBlocks(1); CHECK_EQ((int)eng().status().track[7].kickCut, 1); drainCmds();
  key(13, 0, KeyEvent::Press); key(13, 0, KeyEvent::Repeat, 400); release(13, 0, 450);   // a held key toggles once
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdKickCut), 1); k = findCmd(c, CmdKickCut); if (k) CHECK_EQ(k->i, 0);
  runBlocks(1); CHECK_EQ((int)eng().status().track[7].kickCut, 0);
  CHECK_EQ(u.outputVolume(), 200); CHECK_EQ(g_volHookLast, 200);
  advanceMs(700); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
  u.setOutputVolumeHook(nullptr);
}

// ---------------------------------------------------------------------------------------------- gyro

TEST(gyro_hold_toggles_and_imu_maps_tilt) {
  Ui& u = ui(); drainCmds();
  CHECK(!u.gyroOn());
  press(8, 2);
  advanceMs(150); CHECK(!u.gyroOn());                // a tap does nothing
  advanceMs(250); CHECK(u.gyroOn());                 // held ≥ 400 ms toggles (SPEC §3)
  std::vector<Cmd> c = peekCmds(); const Cmd* g = findCmd(c, CmdGyro); CHECK(g != nullptr); if (g) CHECK_EQ((int)g->a, 1);
  release(8, 2, 450); drainCmds();
  CHECK_EQ((int)u.page(), (int)PgNote); advanceMs(40); draw(); CHECK(g_gfx.sawText("GYRO ON"));
  advanceMs(700);
  // forward tilt 40° (dead band 10°, full at 45° beyond): filter → (40−10)/45 = 0.667, smoothed
  const float a = 40.0f * 3.14159265f / 180.0f;
  ImuReading r; r.valid = true; r.ax = sinf(a); r.ay = 0; r.az = cosf(a);
  for (int i = 0; i < 25; i++) { g_now += 20; u.onImu(r, g_now); }
  CHECK_NEAR(u.gyroFilter(), 0.667f, 0.01f);
  c = peekCmds(); CHECK(countCmd(c, CmdGyro) >= 20);    // one per 20 ms reading
  g = &c.back(); CHECK_EQ((int)g->type, (int)CmdGyro); CHECK_NEAR(g->f, 0.667f, 0.01f); CHECK_EQ(g->i, 0);
  drainCmds();
  // inside the dead band: back to 0; backward tilt: negative (LP)
  r.ax = sinf(5.0f * 3.14159265f / 180.0f); r.az = cosf(5.0f * 3.14159265f / 180.0f);
  for (int i = 0; i < 40; i++) { g_now += 20; u.onImu(r, g_now); }
  CHECK_NEAR(u.gyroFilter(), 0.0f, 0.01f);
  r.ax = -sinf(a); r.az = cosf(a);
  for (int i = 0; i < 25; i++) { g_now += 20; u.onImu(r, g_now); }
  CHECK_NEAR(u.gyroFilter(), -0.667f, 0.01f);
  // roll left 32.5° → reverb (32.5−10)/45 = 0.5; roll right → echo
  r.ax = 0; r.ay = -sinf(32.5f * 3.14159265f / 180.0f); r.az = cosf(32.5f * 3.14159265f / 180.0f);
  for (int i = 0; i < 40; i++) { g_now += 20; u.onImu(r, g_now); }
  CHECK_NEAR(u.gyroReverb(), 0.5f, 0.01f); CHECK_NEAR(u.gyroEcho(), 0.0f, 0.001f);
  c = peekCmds(); g = &c.back(); CHECK_NEAR(((g->i >> 8) & 0xFF) / 255.0f, 0.5f, 0.01f); CHECK_EQ(g->i & 0xFF, 0);
  drainCmds();
  r.ay = -r.ay;
  for (int i = 0; i < 40; i++) { g_now += 20; u.onImu(r, g_now); }
  CHECK_NEAR(u.gyroEcho(), 0.5f, 0.01f); CHECK_NEAR(u.gyroReverb(), 0.0f, 0.001f);
  c = peekCmds(); g = &c.back(); CHECK_NEAR((g->i & 0xFF) / 255.0f, 0.5f, 0.01f);
  drainCmds();
  // readings at 5 ms spacing send at most every 20 ms
  for (int i = 0; i < 8; i++) { g_now += 5; u.onImu(r, g_now); }
  c = peekCmds(); CHECK(countCmd(c, CmdGyro) <= 2); drainCmds();
  advanceMs(40); draw(); CHECK(g_gfx.sawText("GYRO"));   // footer
  // hold again: off, CmdGyro a = 0, no more sends
  press(8, 2); advanceMs(450); CHECK(!u.gyroOn()); release(8, 2, 450);
  c = peekCmds(); g = findCmd(c, CmdGyro); CHECK(g != nullptr); if (g) CHECK_EQ((int)g->a, 0);
  drainCmds();
  for (int i = 0; i < 5; i++) { g_now += 20; u.onImu(r, g_now); }
  CHECK_EQ((int)peekCmds().size(), 0);
  advanceMs(700); runBlocks(1); drainCmds();
}

TEST(imu_mapping_setter_flips_axes_and_signs) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ(u.imuPitchAxis(), 0); CHECK_EQ(u.imuPitchSign(), 1); CHECK_EQ(u.imuRollAxis(), 1); CHECK_EQ(u.imuRollSign(), 1);
  u.setImuMapping(1, 1, 1, 1);                        // same axis twice: ignored
  u.setImuMapping(3, 1, 0, 1);                        // out of range: ignored
  CHECK_EQ(u.imuPitchAxis(), 0); CHECK_EQ(u.imuRollAxis(), 1);
  const float a = 40.0f * 3.14159265f / 180.0f;
  ImuReading flat; flat.valid = true; flat.ax = 0; flat.ay = 0; flat.az = 1;
  ImuReading tiltX; tiltX.valid = true; tiltX.ax = sinf(a); tiltX.ay = 0; tiltX.az = cosf(a);   // the probe's forward tilt
  u.setImuMapping(0, -1, 1, 1);                       // imu.pitch_axis=-x (SPEC §3)
  u.onImu(flat, g_now);
  press(8, 2); advanceMs(450); CHECK(u.gyroOn()); release(8, 2, 450); drainCmds(); advanceMs(700);
  for (int i = 0; i < 25; i++) { g_now += 20; u.onImu(tiltX, g_now); }
  CHECK_NEAR(u.gyroFilter(), -0.667f, 0.01f);        // sign flipped: the same tilt is now backward → LP
  std::vector<Cmd> c = peekCmds(); CHECK(countCmd(c, CmdGyro) >= 20);
  const Cmd* g = &c.back(); CHECK_EQ((int)g->type, (int)CmdGyro); CHECK_NEAR(g->f, -0.667f, 0.01f); CHECK_EQ(g->i, 0);
  drainCmds();
  u.onImu(flat, g_now);                              // neutral is re-captured from the last reading on a mapping change
  u.setImuMapping(1, 1, 0, -1);                       // pitch on y, roll on -x: the x tilt is now a left lean → reverb
  for (int i = 0; i < 40; i++) { g_now += 20; u.onImu(tiltX, g_now); }
  CHECK_NEAR(u.gyroFilter(), 0.0f, 0.01f); CHECK_NEAR(u.gyroReverb(), 0.667f, 0.01f); CHECK_NEAR(u.gyroEcho(), 0.0f, 0.001f);
  c = peekCmds(); g = &c.back(); CHECK_NEAR(((g->i >> 8) & 0xFF) / 255.0f, 0.667f, 0.01f); CHECK_EQ(g->i & 0xFF, 0); CHECK_NEAR(g->f, 0.0f, 0.01f);
  drainCmds();
  u.onImu(flat, g_now);
  u.setImuMapping(1, 1, 0, 1);                        // roll on +x: a right lean → echo
  for (int i = 0; i < 40; i++) { g_now += 20; u.onImu(tiltX, g_now); }
  CHECK_NEAR(u.gyroEcho(), 0.667f, 0.01f); CHECK_NEAR(u.gyroReverb(), 0.0f, 0.001f);
  drainCmds();
  u.onImu(flat, g_now);
  u.setImuMapping(0, 1, 1, 1);                        // back to the default
  CHECK_EQ(u.imuPitchAxis(), 0); CHECK_EQ(u.imuPitchSign(), 1); CHECK_EQ(u.imuRollAxis(), 1); CHECK_EQ(u.imuRollSign(), 1);
  for (int i = 0; i < 40; i++) { g_now += 20; u.onImu(tiltX, g_now); }
  CHECK_NEAR(u.gyroFilter(), 0.667f, 0.01f);
  drainCmds();
  press(8, 2); advanceMs(450); CHECK(!u.gyroOn()); release(8, 2, 450);
  advanceMs(700); runBlocks(1); drainCmds();
}

// ---------------------------------------------------------------------------------------------- editor

TEST(editor_enter_toggles_notes_in_the_source) {
  Ui& u = ui(); drainCmds();
  eng().sourcePattern(7).clear(); gen().info(7) = TrackGenInfo();   // an unloaded Beat
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);
  tap(8, 3);                                         // edit
  CHECK_EQ((int)u.page(), (int)PgEditor); CHECK_EQ(u.editorTrack(), 7);
  advanceMs(40); draw();
  CHECK(g_gfx.sawText("EDIT S1 BEAT")); CHECK(g_gfx.sawText("KICK")); CHECK(g_gfx.sawText("SNR")); CHECK(g_gfx.sawText("OH"));
  tap(13, 2);                                        // enter at step 0, row KICK
  Pattern& src = eng().sourcePattern(7);
  CHECK_EQ((int)src.count, 1);
  if (src.count) { CHECK_EQ((int)src.notes[0].start(), 0); CHECK_EQ((int)src.notes[0].pitch(), 36); CHECK(src.notes[0].vel() >= 99 && src.notes[0].vel() <= 103); CHECK_EQ((int)src.notes[0].len(), 12); }
  CHECK_EQ((int)gen().info(7).edited, 1);
  std::vector<Cmd> c = peekCmds();
  CHECK(findCmd(c, CmdSwapPattern, 7) != nullptr); CHECK(findCmd(c, CmdBeatMask) != nullptr);   // variants rebuilt (Beat: Full + mask)
  const Cmd* m = findCmd(c, CmdBeatMask); if (m) CHECK_EQ((m->i >> 8) & 3, 3);                  // an unloaded Beat edits as Full
  runBlocks(1); drainCmds();
  for (int i = 0; i < 4; i++) tap(12, 3);            // right ×4
  tap(11, 3);                                        // down → SNARE
  CHECK_EQ(u.editorCursorX(), 4); CHECK_EQ(u.editorCursorY(), 1);
  tap(13, 2);
  CHECK_EQ((int)src.count, 2);
  int idx = -1; for (int i = 0; i < src.count; i++) if (src.notes[i].start() == 96) idx = i;
  CHECK(idx >= 0); if (idx >= 0) CHECK_EQ((int)src.notes[idx].pitch(), 37);
  tap(0, 2);                                         // vol+: velocity +10 of the note under the cursor
  if (idx >= 0) { CHECK(src.notes[idx].vel() >= 109 && src.notes[idx].vel() <= 115); }
  tap(0, 3); tap(0, 3);
  if (idx >= 0) { CHECK(src.notes[idx].vel() >= 90 && src.notes[idx].vel() <= 100); }
  tap(13, 2);                                        // enter again removes it
  CHECK_EQ((int)src.count, 1);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("STEP 5"));
  tap(10, 3); CHECK_EQ(u.editorCursorX(), 3);        // left
  tap(10, 3); tap(10, 3); tap(10, 3); CHECK_EQ(u.editorCursorX(), 0); tap(10, 3); CHECK_EQ(u.editorCursorX(), 15);   // wraps
  tap(11, 2); CHECK_EQ(u.editorCursorY(), 0); tap(11, 2); CHECK_EQ(u.editorCursorY(), 0);
  tap(6, 2); CHECK_EQ(u.editorBar(), 0);             // one bar: tempo+ cannot page further
  tap(0, 0);                                         // ` exits
  CHECK_EQ((int)u.page(), (int)PgMain);
  runBlocks(1); drainCmds();
  // Melodic editor: 7 scale rows of the track's key, bottom = root (A4 = 69 in octave 4), gate via dist±
  eng().sourcePattern(0).clear();
  shiftDown(); tap(0, 0); shiftUp(); tap(8, 3);
  CHECK_EQ((int)u.page(), (int)PgEditor); CHECK_EQ(u.editorTrack(), 0); CHECK_EQ(u.editorOctave(), 4); CHECK_EQ(u.editorGate(), 1);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("EDIT S1 CHD")); CHECK(g_gfx.sawText("A")); CHECK(g_gfx.sawText("G"));
  for (int i = 0; i < 6; i++) tap(11, 3);            // bottom row
  CHECK_EQ(u.editorCursorY(), 6);
  tap(13, 2);
  Pattern& s0 = eng().sourcePattern(0);
  CHECK_EQ((int)s0.count, 1); if (s0.count) { CHECK_EQ((int)s0.notes[0].pitch(), 69); CHECK_EQ((int)s0.notes[0].len(), 24); }
  tap(3, 2); CHECK_EQ(u.editorGate(), 2);            // dist+ → 2 steps
  tap(12, 3); tap(11, 2); tap(13, 2);                // step 1, degree 2 (B4 = 71), gate 2 → 48 ticks
  CHECK_EQ((int)s0.count, 2);
  if (s0.count == 2) { CHECK_EQ((int)s0.notes[1].start(), 24); CHECK_EQ((int)s0.notes[1].pitch(), 71); CHECK_EQ((int)s0.notes[1].len(), 48); }
  tap(5, 2); CHECK_EQ(u.editorOctave(), 5);          // len+ → octave 5
  tap(12, 3); tap(13, 2);
  CHECK_EQ((int)s0.count, 3); if (s0.count == 3) CHECK_EQ((int)s0.notes[2].pitch(), 83);
  CHECK_EQ((int)gen().info(0).edited, 1);
  std::vector<Cmd> c2 = peekCmds(); CHECK(findCmd(c2, CmdSwapPattern, 0) != nullptr);
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgMain);
  runBlocks(1); drainCmds();
  // Percussion rows write a pitch the fold map puts on that voice
  shiftDown(); tap(5, 0); shiftUp(); tap(8, 3);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("EDIT S1 PRC"));
  tap(11, 3); tap(13, 2);                            // row B
  Pattern& s5 = eng().sourcePattern(5);
  CHECK_EQ((int)s5.count, 1); if (s5.count) CHECK_EQ((int)gen().foldMap(0)[s5.notes[0].pitch()], 37);
  tap(8, 3); runBlocks(1); drainCmds();
  // Vocal: no grid
  shiftDown(); tap(6, 0); shiftUp(); tap(8, 3);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("VOCAL: no pattern"));
  tap(13, 2); CHECK_EQ((int)eng().sourcePattern(6).count, 0);
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgMain);
  drainCmds();
}

// ---------------------------------------------------------------------------------------------- perform + looper

TEST(perform_notes_and_looper_record_quantised_loop) {
  Ui& u = ui(); drainCmds();
  shiftDown(); tap(4, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 4);   // the perform page plays the selected track (SPEC §9.17c): set 1's Fx
  tap(9, 3);
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfTrack(), 4); CHECK_EQ(u.perfSet(), 0); CHECK_EQ(u.perfOctave(), 3);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S1 FX OCT3")); CHECK(g_gfx.sawText("1")); CHECK(g_gfx.sawText("7"));
  const int root = gen().key().root; ProjectKey k; k.root = (uint8_t)root;
  press(1, 0);                                       // degree 0, octave 3
  std::vector<Cmd> c = peekCmds(); const Cmd* n = findCmd(c, CmdPerformNote, 4);
  CHECK(n != nullptr); if (n) { CHECK_EQ(n->i, k.degreePitch(0, 3)); CHECK_NEAR(n->f, 0.8f, 1e-6); }
  advanceMs(40); draw();
  int heldCells = 0; for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.w == kPerfCellW - 1) heldCells++;
  CHECK_EQ(heldCells, 1);                            // only the held cell was redrawn
  release(1, 0);
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdPerformNote), 2); CHECK_NEAR(c.back().f, 0.0f, 1e-6); CHECK_EQ(c.back().i, k.degreePitch(0, 3));
  drainCmds();
  press(4, 1); c = peekCmds(); n = findCmd(c, CmdPerformNote); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, k.degreePitch(6 % 7, 3));   // row 1 col 3 → degree 6
  release(4, 1); drainCmds();
  press(12, 3); c = peekCmds(); n = findCmd(c, CmdPerformNote); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, k.degreePitch(18 % 7, 3 + 18 / 7));
  release(12, 3); drainCmds();
  tap(11, 0); CHECK_EQ(u.perfOctave(), 2);           // '-' octave down
  press(1, 0); c = peekCmds(); n = findCmd(c, CmdPerformNote); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, k.degreePitch(0, 2)); release(1, 0); drainCmds();
  tap(12, 0); CHECK_EQ(u.perfOctave(), 3);
  shiftDown(); press(1, 0); c = peekCmds(); n = findCmd(c, CmdPerformNote); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, k.degreePitch(0, 3) + 1);   // SHIFT = +1 semitone
  release(1, 0); shiftUp(); c = peekCmds(); CHECK_EQ(c.back().i, k.degreePitch(0, 3) + 1); drainCmds();   // note-off uses the sounding pitch
  // ---- looper: arm, count-in, record one note at ~tick 100 (→ 96), close at the bar, loop = 1 bar
  eng().sourcePattern(4).clear(); gen().info(4).userLoop = 0;
  runToTick(nextBarTick() - 40);                     // well before a bar line
  tap(13, 0);                                        // del: arm
  CHECK_EQ((int)u.looper(), (int)LpArmed);
  const uint32_t bar = nextBarTick();
  runToTick(bar - kSwapLeadTicks + 1);
  CHECK_EQ((int)u.looper(), (int)LpRecording);       // count-in done at the bar line
  runToTick(bar + 100);
  press(1, 0); runBlocks(3); release(1, 0);          // ≈ 7 ticks long → one 1/16 after quantisation
  CHECK_EQ((int)u.loopPattern().count, 1);
  tap(13, 0);                                        // close at the next bar (1 bar recorded)
  CHECK_EQ((int)u.looper(), (int)LpClosing);
  drainCmds();
  // The UI closes the loop on its early bar (kSwapLeadTicks before the line) and the next block consumes what it
  // pushed, so the queue is peeked after every iteration: which iteration closes depends on the bar's phase in the block.
  std::vector<Cmd> seen;
  for (int guard = 0; eng().status().tick < bar + kTicksPerBar - kSwapLeadTicks + 1 && guard < 20000; guard++) { iterate(); for (const Cmd& k : peekCmds()) seen.push_back(k); }
  CHECK_EQ((int)u.looper(), (int)LpLooping);
  CHECK_EQ((int)u.loopTicks(), kTicksPerBar);
  const Pattern& fx = eng().sourcePattern(4);
  CHECK_EQ((int)fx.lengthTicks, kTicksPerBar); CHECK_EQ((int)fx.count, 1);
  if (fx.count) { CHECK_EQ((int)fx.notes[0].start(), 96); CHECK_EQ((int)fx.notes[0].len(), 24); CHECK_EQ((int)fx.notes[0].pitch(), k.degreePitch(0, 3)); CHECK(fx.notes[0].vel() >= 99); }
  CHECK_EQ((int)gen().info(4).userLoop, 1); CHECK_EQ((int)gen().info(4).variant, 0);
  CHECK(findCmd(seen, CmdSwapPattern, 4) != nullptr); CHECK(findCmd(seen, CmdTrackToggle, 4) != nullptr);   // published + started
  runToTick(bar + kTicksPerBar + 4);
  CHECK_EQ((int)eng().status().track[4].state, (int)PSPlaying);
  CHECK_EQ((int)eng().activePattern(4).count, 1); CHECK_EQ((int)eng().status().track[4].lenTicks, kTicksPerBar);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("LOOP")); CHECK(g_gfx.sawText("PLAY"));
  // ---- overdub merges a second note at a wrapped position
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpOverdub);
  const uint32_t t2 = eng().status().tick; runToTick((t2 / kTicksPerBar + 1) * kTicksPerBar + 200);
  press(2, 0); runBlocks(2); release(2, 0);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpLooping);
  CHECK_EQ((int)fx.count, 2);
  int idx = -1; for (int i = 0; i < fx.count; i++) if (fx.notes[i].pitch() == k.degreePitch(1, 3)) idx = i;
  CHECK(idx >= 0); if (idx >= 0) CHECK_EQ((int)fx.notes[idx].start(), 192);
  drainCmds();
  // ---- tab toggles the Fx track; hold del 700 ms clears the loop
  tap(0, 1); c = peekCmds(); CHECK(findCmd(c, CmdTrackToggle, 4) != nullptr); runBlocks(1); CHECK_EQ((int)eng().status().track[4].state, (int)PSStopQueued); drainCmds();
  tap(0, 1); runBlocks(1); CHECK_EQ((int)eng().status().track[4].state, (int)PSPlaying);
  press(13, 0); advanceMs(750);
  CHECK_EQ((int)u.looper(), (int)LpIdle); CHECK_EQ((int)gen().info(4).userLoop, 0); CHECK_EQ((int)fx.count, 0);
  release(13, 0, 760); CHECK_EQ((int)u.looper(), (int)LpIdle);   // the release after a hold does not arm
  advanceMs(40); draw(); CHECK(g_gfx.sawText("LOOP CLEARED"));
  drainCmds();
  press(9, 3);                                       // the 'perform' key is grid degree 15 here (SPEC §9.10 grid y3 x3–12), not an exit
  CHECK_EQ((int)u.page(), (int)PgPerform);
  c = peekCmds(); n = findCmd(c, CmdPerformNote, 4); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, k.degreePitch(15 % 7, 3 + 15 / 7));
  release(9, 3); drainCmds();
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);   // ` is the only exit
  shiftDown(); tap(9, 3); shiftUp();                 // SHIFT + perform → the same track of the other set
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfSet(), 1); CHECK_EQ(u.perfTrack(), 12);
  press(1, 0); c = peekCmds(); n = findCmd(c, CmdPerformNote, 12); CHECK(n != nullptr); release(1, 0);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdTrackStopNow, 4); runBlocks(1); drainCmds();
}

// From the main page: clears set 1's Fx track, opens its perform page, arms the looper before a bar line with the
// requested parity (odd / even bar index) and runs until the count-in ends there. Returns that bar's tick.
static uint32_t armAtBar(bool oddBar) {
  Ui& u = ui();
  CHECK_EQ((int)u.page(), (int)PgMain);
  eng().sourcePattern(4).clear(); gen().info(4) = TrackGenInfo(); gen().info(4).keyRoot = (uint8_t)gen().key().root;
  shiftDown(); tap(4, 0); shiftUp(); tap(9, 3);
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfSet(), 0); CHECK_EQ((int)u.looper(), (int)LpIdle);
  uint32_t bar = nextBarTick();
  if (eng().status().tick + 60 > bar) bar += kTicksPerBar;             // room to arm before the lead window
  if (((bar / kTicksPerBar) & 1u) != (oddBar ? 1u : 0u)) bar += kTicksPerBar;
  runToTick(bar - 40);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpArmed);
  runToTick(bar - kSwapLeadTicks + 1);
  CHECK_EQ((int)u.looper(), (int)LpRecording);
  return bar;
}

TEST(looper_multibar_loop_from_odd_bar_stores_global_phase) {
  Ui& u = ui(); drainCmds();
  ProjectKey k; k.root = (uint8_t)gen().key().root;
  const uint32_t bar = armAtBar(true);               // the take starts on an odd bar: phase 384 inside a 2-bar loop
  CHECK_EQ((int)((bar / kTicksPerBar) & 1u), 1);
  runToTick(bar + 100);
  press(1, 0); runBlocks(3); release(1, 0);          // relative tick ≈ 100 → 96
  CHECK_EQ((int)u.loopPattern().count, 1);
  runToTick(bar + kTicksPerBar + 100);               // close from inside the second bar → 2 bars
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpClosing);
  drainCmds();
  runToTick(bar + 2 * kTicksPerBar - kSwapLeadTicks + 1);
  CHECK_EQ((int)u.looper(), (int)LpLooping); CHECK_EQ((int)u.loopTicks(), 2 * kTicksPerBar);
  const Pattern& fx = eng().sourcePattern(4);
  CHECK_EQ((int)fx.lengthTicks, 2 * kTicksPerBar); CHECK_EQ((int)fx.count, 1);
  const int expect = (int)((bar + 96) % (2 * kTicksPerBar));   // the global phase (SPEC §9.4), not the relative 96
  CHECK_EQ(expect, kTicksPerBar + 96);
  if (fx.count) CHECK_EQ((int)fx.notes[0].start(), expect);
  std::printf("  2-bar take from bar %u: note stored at %d (relative 96)\n", (unsigned)(bar / kTicksPerBar), fx.count ? (int)fx.notes[0].start() : -1);
  // the engine plays it phase-locked (position = globalTick mod 768): the note comes back on odd bars only
  runToTick(bar + 2 * kTicksPerBar + 4);
  CHECK_EQ((int)eng().status().track[4].state, (int)PSPlaying); CHECK_EQ((int)eng().status().track[4].lenTicks, 2 * kTicksPerBar);
  CHECK_EQ((int)eng().activePattern(4).count, 1); if (eng().activePattern(4).count) CHECK_EQ((int)eng().activePattern(4).notes[0].start(), expect);
  CHECK_EQ((int)eng().status().track[4].posTick, (int)(eng().status().tick % (2 * kTicksPerBar)));
  runToTick(bar + 4 * kTicksPerBar + 2);             // an odd bar again (bar + 4): the clip position is in its second half
  CHECK(eng().status().track[4].posTick >= kTicksPerBar);
  // overdub quantises the global phase directly: a note at phase ≈ 200 of an EVEN bar is stored at 192
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpOverdub);
  uint32_t even = nextBarTick(); if ((even / kTicksPerBar) & 1u) even += kTicksPerBar;
  runToTick(even + 200);
  press(2, 0); runBlocks(2); release(2, 0);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpLooping);
  CHECK_EQ((int)fx.count, 2);
  int idx = -1; for (int i = 0; i < fx.count; i++) if (fx.notes[i].pitch() == k.degreePitch(1, 3)) idx = i;
  CHECK(idx >= 0); if (idx >= 0) CHECK_EQ((int)fx.notes[idx].start(), 192);
  press(13, 0); advanceMs(750); release(13, 0, 760); CHECK_EQ((int)u.looper(), (int)LpIdle);   // clear
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdTrackStopNow, 4); runBlocks(1); drainCmds();
}

TEST(looper_close_press_inside_the_lead_window_closes_this_bar) {
  Ui& u = ui(); drainCmds();
  const uint32_t bar = armAtBar(false);
  runToTick(bar + 100);
  press(1, 0); runBlocks(3); release(1, 0);
  CHECK_EQ((int)u.loopPattern().count, 1);
  runToTick(bar + kTicksPerBar - 6);                 // inside the kSwapLeadTicks window: that bar's event already fired
  const uint32_t t = eng().status().tick;
  CHECK(t >= bar + kTicksPerBar - kSwapLeadTicks && t < bar + kTicksPerBar);
  drainCmds();
  tap(13, 0);                                        // "close on the bar": the loop is 1 bar, closed right now
  CHECK_EQ((int)u.looper(), (int)LpLooping);
  CHECK_EQ((int)u.loopTicks(), kTicksPerBar);
  const Pattern& fx = eng().sourcePattern(4);
  CHECK_EQ((int)fx.lengthTicks, kTicksPerBar); CHECK_EQ((int)fx.count, 1);
  if (fx.count) CHECK_EQ((int)fx.notes[0].start(), 96);
  std::vector<Cmd> c = peekCmds(); CHECK(findCmd(c, CmdSwapPattern, 4) != nullptr); CHECK(findCmd(c, CmdTrackToggle, 4) != nullptr);
  runToTick(bar + kTicksPerBar + 4);                 // the publish still lands on that bar line
  CHECK_EQ((int)eng().status().track[4].state, (int)PSPlaying); CHECK_EQ((int)eng().status().track[4].lenTicks, kTicksPerBar);
  CHECK_EQ((int)eng().activePattern(4).count, 1);
  runToTick(bar + 2 * kTicksPerBar + 2);
  CHECK_EQ((int)u.looper(), (int)LpLooping); CHECK_EQ((int)u.loopTicks(), kTicksPerBar);   // no second close a bar later
  press(13, 0); advanceMs(750); release(13, 0, 760); CHECK_EQ((int)u.looper(), (int)LpIdle);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdTrackStopNow, 4); runBlocks(1); drainCmds();
}

TEST(leaving_perform_keeps_an_overdub_and_closes_a_take) {
  Ui& u = ui(); drainCmds();
  ProjectKey k; k.root = (uint8_t)gen().key().root;
  // an existing 1-bar loop on set 1's Fx track, as a closed take leaves it
  Pattern& fx = eng().sourcePattern(4); fx.clear(kTicksPerBar); fx.add(Note::make(0, 24, (uint8_t)k.degreePitch(0, 3), 100));
  TrackGenInfo& inf = gen().info(4); inf = TrackGenInfo(); inf.userLoop = 1; inf.edited = 1; inf.loaded = 1; inf.sourceTicks = kTicksPerBar; inf.naturalTicks = kTicksPerBar; inf.keyRoot = k.root;
  shiftDown(); tap(4, 0); shiftUp(); tap(9, 3);
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ((int)u.looper(), (int)LpLooping); CHECK_EQ((int)u.loopTicks(), kTicksPerBar);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpOverdub);
  runToTick(nextBarTick() + 200);
  press(2, 0); runBlocks(2); release(2, 0);
  CHECK_EQ((int)u.loopPattern().count, 2); CHECK_EQ((int)fx.count, 1);   // merged in the take, not yet published
  tap(0, 0);                                         // leave the page mid-overdub
  CHECK_EQ((int)u.page(), (int)PgMain); CHECK_EQ((int)u.looper(), (int)LpLooping);
  CHECK_EQ((int)fx.count, 2);                        // the overdub was published on the way out
  int idx = -1; for (int i = 0; i < fx.count; i++) if (fx.notes[i].pitch() == k.degreePitch(1, 3)) idx = i;
  CHECK(idx >= 0); if (idx >= 0) CHECK_EQ((int)fx.notes[idx].start(), 192);
  CHECK_EQ((int)gen().info(4).userLoop, 1);
  std::vector<Cmd> c = peekCmds(); CHECK(findCmd(c, CmdSwapPattern, 4) != nullptr);
  runBlocks(1); drainCmds();
  tap(9, 3); CHECK_EQ((int)u.looper(), (int)LpLooping); CHECK_EQ((int)u.loopPattern().count, 2);   // re-entering shows the merged loop
  tap(0, 0);
  // a take with notes closes at its bar line after leaving (tick() keeps dispatching bars outside the page)
  const uint32_t bar = armAtBar(false);
  runToTick(bar + 100); press(1, 0); runBlocks(3); release(1, 0);
  CHECK_EQ((int)u.loopPattern().count, 1);
  tap(0, 0);
  CHECK_EQ((int)u.page(), (int)PgMain); CHECK_EQ((int)u.looper(), (int)LpClosing);
  advanceMs(1); CHECK(strcmp(u.statusText(), "LOOP CLOSING") == 0);
  CHECK_EQ((int)fx.count, 0);
  runToTick(bar + kTicksPerBar - kSwapLeadTicks + 1);
  CHECK_EQ((int)u.looper(), (int)LpLooping); CHECK_EQ((int)u.loopTicks(), kTicksPerBar);
  CHECK_EQ((int)fx.count, 1); CHECK_EQ((int)fx.lengthTicks, kTicksPerBar); if (fx.count) CHECK_EQ((int)fx.notes[0].start(), 96);
  CHECK_EQ((int)gen().info(4).userLoop, 1);
  runBlocks(1); drainCmds();
  // an empty take is dropped, an arm is forgotten
  (void)armAtBar(false);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain); CHECK_EQ((int)u.looper(), (int)LpIdle); CHECK_EQ((int)gen().info(4).userLoop, 0);
  shiftDown(); tap(4, 0); shiftUp(); tap(9, 3); tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpArmed);
  tap(0, 0); CHECK_EQ((int)u.looper(), (int)LpIdle);
  runToTick(nextBarTick() + 2); CHECK_EQ((int)u.looper(), (int)LpIdle);
  push(CmdTrackStopNow, 4); runBlocks(1); drainCmds();
}

// ---------------------------------------------------------------------------------------------- perform for every track (SPEC §9.17c)

// The perform page targets the selected track: a Beat plays its kit slots from row y0 keys x1..x8 (pitches 36..43),
// a Percussion its four voices (x1..x4), a Vocal has nothing to play.
TEST(perform_page_targets_a_beat_track) {
  Ui& u = ui(); drainCmds();
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);
  tap(9, 3);
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfTrack(), 7);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S1 BEAT")); CHECK(g_gfx.drawn("KCK") != nullptr); CHECK(g_gfx.drawn("OH") != nullptr); CHECK(g_gfx.drawn("A4") != nullptr); CHECK(!g_gfx.sawText("OCT"));
  press(1, 0);                                       // slot 0 = kick
  std::vector<Cmd> c = peekCmds(); const Cmd* n = findCmd(c, CmdPerformNote, 7);
  CHECK(n != nullptr); if (n) { CHECK_EQ(n->i, 36); CHECK_NEAR(n->f, 0.8f, 1e-6); }
  CHECK_EQ(countCmd(c, CmdPerformChord), 0);
  advanceMs(40); draw(); int heldCells = 0; for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.w == kPerfCellW - 1 && o.c == categoryColor(Beat)) heldCells++;
  CHECK_EQ(heldCells, 1);
  release(1, 0);
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdPerformNote), 2); CHECK_EQ(c.back().i, 36); CHECK_NEAR(c.back().f, 0.0f, 1e-6); drainCmds();
  press(4, 0); c = peekCmds(); n = findCmd(c, CmdPerformNote, 7); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, 39); release(4, 0); drainCmds();   // open hat
  press(8, 0); c = peekCmds(); n = findCmd(c, CmdPerformNote, 7); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, 43); release(8, 0); drainCmds();   // accent 4
  press(9, 0); CHECK(peekCmds().empty()); release(9, 0);                  // beyond the eight slots
  press(1, 1); CHECK(peekCmds().empty()); release(1, 1);                  // the other rows are no slot rows
  tap(11, 0); CHECK_EQ(u.perfOctave(), 3);                               // octave keys mean nothing on a drum track
  tap(13, 1); advanceMs(1); CHECK(strcmp(u.statusText(), "CHORD: CHORD/PAD ONLY") == 0); CHECK(!u.perfChord());
  tap(0, 1); c = peekCmds(); CHECK(findCmd(c, CmdTrackToggle, 7) != nullptr || findCmd(c, CmdCancelPending, 7) != nullptr);   // tab: the Beat's start/stop
  runBlocks(1); drainCmds(); tap(0, 1); runBlocks(1); drainCmds();         // ... and back
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  // Percussion: four voices A..D on x1..x4, the accent keys do nothing
  shiftDown(); tap(5, 0); shiftUp(); tap(9, 3); CHECK_EQ(u.perfTrack(), 5);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S1 PERCUSSION")); CHECK(g_gfx.drawn("D") != nullptr); CHECK(g_gfx.drawn("A1") == nullptr);
  press(2, 0); c = peekCmds(); n = findCmd(c, CmdPerformNote, 5); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, 37); release(2, 0); drainCmds();
  press(5, 0); CHECK(peekCmds().empty()); release(5, 0);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  // Vocal: nothing to perform
  shiftDown(); tap(6, 0); shiftUp(); tap(9, 3); CHECK_EQ(u.perfTrack(), 6);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("NO PERFORM FOR VOCAL")); CHECK(g_gfx.sawText("S1 VOCAL"));
  press(1, 0); CHECK(peekCmds().empty()); release(1, 0);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpIdle);                     // no looper either
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  advanceMs(1100); drainCmds();
}

// A Percussion slot is recorded as a pitch the track's stored fold map (SPEC §9.7) puts on that voice, so the published
// loop plays the slot that was hit: with a file whose most frequent pitch 37 ranks as voice A (37 → 36), slot B (live
// pitch 37) is stored as the file's voice-B pitch (45) and folds back onto 37 when the loop is rebuilt through the map.
TEST(percussion_perform_records_through_the_fold_map) {
  Ui& u = ui(); drainCmds();
  Pattern file; file.clear(kTicksPerBar);
  for (int i = 0; i < 4; i++) file.add(Note::make((uint16_t)(i * 96), 24, 37, 100));           // 37 × 4 → voice A (36)
  for (int i = 0; i < 3; i++) file.add(Note::make((uint16_t)(i * 96 + 48), 24, 45, 100));      // 45 × 3 → voice B (37)
  for (int i = 0; i < 2; i++) file.add(Note::make((uint16_t)(i * 192 + 24), 24, 50, 100));     // 50 × 2 → voice C (38)
  file.add(Note::make(72, 24, 55, 100));                                                         // 55 × 1 → voice D (39)
  file.sortByStart();
  const char* path = "/lp/state/ui_perc_fold.mid";
  sd().mkdir("/lp/state");
  { IFile* f = sd().open(path, "w"); CHECK(f != nullptr); if (f) { CHECK(smfWrite(*f, file, 120.0f)); f->close(); } }
  CHECK(gen().loadMidiToSource(5, path));
  const uint8_t* map = gen().foldMap(0);
  CHECK_EQ((int)map[37], 36); CHECK_EQ((int)map[45], 37); CHECK_EQ((int)map[50], 38); CHECK_EQ((int)map[55], 39);
  CHECK_EQ((int)gen().info(5).userLoop, 0);
  shiftDown(); tap(5, 0); shiftUp(); tap(9, 3); CHECK_EQ(u.perfTrack(), 5); CHECK_EQ((int)u.looper(), (int)LpIdle);
  press(2, 0); std::vector<Cmd> c = peekCmds(); const Cmd* n = findCmd(c, CmdPerformNote, 5); CHECK(n != nullptr); if (n) CHECK_EQ(n->i, 37);   // slot B sounds voice B live
  release(2, 0); drainCmds();
  runToTick(nextBarTick() - 40); tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpArmed);
  const uint32_t bar = nextBarTick(); runToTick(bar - kSwapLeadTicks + 1); CHECK_EQ((int)u.looper(), (int)LpRecording);
  runToTick(bar + 100); press(2, 0); runBlocks(3); release(2, 0);
  CHECK_EQ((int)u.loopPattern().count, 1); if (u.loopPattern().count) CHECK_EQ((int)u.loopPattern().notes[0].pitch(), 45);   // stored as the file's voice-B pitch
  tap(13, 0); drainCmds();
  runToTick(bar + kTicksPerBar - kSwapLeadTicks + 1); CHECK_EQ((int)u.looper(), (int)LpLooping);
  CHECK_EQ((int)gen().info(5).userLoop, 1);
  runToTick(bar + kTicksPerBar + 4);
  const Pattern& act = eng().activePattern(5);
  CHECK_EQ((int)act.count, 1); if (act.count) CHECK_EQ((int)act.notes[0].pitch(), 37);   // ... and plays voice B, not A
  CHECK_EQ((int)eng().status().track[5].state, (int)PSPlaying);
  press(13, 0); advanceMs(750); release(13, 0, 760); CHECK_EQ((int)u.looper(), (int)LpIdle); CHECK_EQ((int)gen().info(5).userLoop, 0);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdTrackStopNow, 5); runBlocks(1); drainCmds(); sd().remove(path); advanceMs(1100);
}

// CHORD mode (\ on a Chord / Pad track): a grid key plays the diatonic triad degree / +2 / +4 as three CmdPerformChord
// notes, the looper records all three into the selected track (userLoop), the note-offs match the note-ons.
TEST(perform_chord_mode_plays_triads_and_records_them) {
  Ui& u = ui(); drainCmds();
  ProjectKey k; k.root = (uint8_t)gen().key().root;
  eng().sourcePattern(0).clear(); gen().info(0) = TrackGenInfo(); gen().info(0).keyRoot = k.root;
  shiftDown(); tap(0, 0); shiftUp(); tap(9, 3);
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfTrack(), 0); CHECK(!u.perfChord());
  tap(13, 1); CHECK(u.perfChord()); advanceMs(1); CHECK(strcmp(u.statusText(), "CHORD ON") == 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S1 CHORD OCT3 CHD"));
  press(1, 0);                                       // degree 0 → 0, 2, 4
  std::vector<Cmd> c = peekCmds();
  CHECK_EQ(countCmd(c, CmdPerformChord), 3); CHECK_EQ(countCmd(c, CmdPerformNote), 0); CHECK_EQ((int)c.size(), 3);
  if (c.size() == 3) {
    CHECK_EQ(c[0].i, k.degreePitch(0, 3)); CHECK_EQ(c[1].i, k.degreePitch(2, 3)); CHECK_EQ(c[2].i, k.degreePitch(4, 3));
    for (const Cmd& x : c) { CHECK_EQ((int)x.track, 0); CHECK_NEAR(x.f, 0.8f, 1e-6); }
  }
  tap(12, 0); CHECK_EQ(u.perfOctave(), 4);           // an octave change while held must not change the note-offs
  release(1, 0);
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdPerformChord), 6);
  if (c.size() == 6) { CHECK_EQ(c[3].i, k.degreePitch(0, 3)); CHECK_EQ(c[4].i, k.degreePitch(2, 3)); CHECK_EQ(c[5].i, k.degreePitch(4, 3)); for (int i = 3; i < 6; i++) CHECK_NEAR(c[i].f, 0.0f, 1e-6); }
  drainCmds(); tap(11, 0); CHECK_EQ(u.perfOctave(), 3);
  press(4, 1);                                       // degree 6: the triad wraps into the next octave (6, 1', 3')
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdPerformChord), 3);
  if (c.size() == 3) { CHECK_EQ(c[0].i, k.degreePitch(6, 3)); CHECK_EQ(c[1].i, k.degreePitch(1, 4)); CHECK_EQ(c[2].i, k.degreePitch(3, 4)); }
  release(4, 1); drainCmds();
  // the looper records all three notes into the Chord track
  runToTick(nextBarTick() - 40);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpArmed);
  const uint32_t bar = nextBarTick();
  runToTick(bar - kSwapLeadTicks + 1); CHECK_EQ((int)u.looper(), (int)LpRecording);
  runToTick(bar + 100);
  press(1, 0); runBlocks(3); release(1, 0);
  CHECK_EQ((int)u.loopPattern().count, 3);
  tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpClosing); drainCmds();
  std::vector<Cmd> seen;
  for (int guard = 0; eng().status().tick < bar + kTicksPerBar - kSwapLeadTicks + 1 && guard < 20000; guard++) { iterate(); for (const Cmd& x : peekCmds()) seen.push_back(x); }
  CHECK_EQ((int)u.looper(), (int)LpLooping);
  const Pattern& src = eng().sourcePattern(0);
  CHECK_EQ((int)src.count, 3); CHECK_EQ((int)src.lengthTicks, kTicksPerBar);
  bool sawRoot = false, sawThird = false, sawFifth = false;
  for (int i = 0; i < src.count; i++) { CHECK_EQ((int)src.notes[i].start(), 96); const int p = src.notes[i].pitch(); if (p == k.degreePitch(0, 3)) sawRoot = true; if (p == k.degreePitch(2, 3)) sawThird = true; if (p == k.degreePitch(4, 3)) sawFifth = true; }
  CHECK(sawRoot && sawThird && sawFifth);
  CHECK_EQ((int)gen().info(0).userLoop, 1); CHECK_EQ((int)gen().info(0).variant, 0);
  CHECK(findCmd(seen, CmdSwapPattern, 0) != nullptr); CHECK(findCmd(seen, CmdTrackToggle, 0) != nullptr);
  press(13, 0); advanceMs(750); release(13, 0, 760); CHECK_EQ((int)u.looper(), (int)LpIdle); CHECK_EQ((int)gen().info(0).userLoop, 0); CHECK_EQ((int)src.count, 0);
  tap(13, 1); CHECK(!u.perfChord()); advanceMs(1); CHECK(strcmp(u.statusText(), "CHORD OFF") == 0);
  press(1, 0); c = peekCmds(); CHECK_EQ(countCmd(c, CmdPerformNote), 1); CHECK_EQ(countCmd(c, CmdPerformChord), 0); release(1, 0); drainCmds();   // single notes again
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdTrackStopNow, 0); runBlocks(1); drainCmds();
  // a Lead has no CHORD mode
  shiftDown(); tap(1, 0); shiftUp(); tap(9, 3); CHECK_EQ(u.perfTrack(), 1);
  tap(13, 1); CHECK(!u.perfChord()); advanceMs(1); CHECK(strcmp(u.statusText(), "CHORD: CHORD/PAD ONLY") == 0);
  tap(0, 0); advanceMs(1100); drainCmds();
}

// [ cycles the quantise OFF 1/16 1/8 1/4: a live note-on waits for the next grid point — tick() queues it in the render
// block before the point (at most one block of ticks ahead), never after it — a key released before that point sounds
// a short note there, and recorded timestamps sit on the grid.
TEST(perform_quantise_cycles_and_delays_note_ons) {
  Ui& u = ui(); drainCmds();
  eng().sourcePattern(4).clear(); gen().info(4) = TrackGenInfo(); gen().info(4).keyRoot = (uint8_t)gen().key().root;
  shiftDown(); tap(4, 0); shiftUp(); tap(9, 3);
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfTrack(), 4); CHECK_EQ(u.perfQuant(), 0);
  tap(11, 1); CHECK_EQ(u.perfQuant(), 1); advanceMs(1); CHECK(strcmp(u.statusText(), "QUANT 1/16") == 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("Q1/16"));
  tap(11, 1); CHECK_EQ(u.perfQuant(), 2); advanceMs(1); CHECK(strcmp(u.statusText(), "QUANT 1/8") == 0);
  tap(11, 1); CHECK_EQ(u.perfQuant(), 3); advanceMs(1); CHECK(strcmp(u.statusText(), "QUANT 1/4") == 0);
  tap(11, 1); CHECK_EQ(u.perfQuant(), 0); advanceMs(1); CHECK(strcmp(u.statusText(), "QUANT OFF") == 0);
  advanceMs(40); draw(); CHECK(!g_gfx.sawText("Q1/"));
  tap(11, 1); tap(11, 1); tap(11, 1); CHECK_EQ(u.perfQuant(), 3);
  // a note-on between quarter grid points waits for the next one: the command is queued while the grid point is at
  // most one render block of ticks ahead (2 at 120 BPM: 256 samples / 114.8 samples per tick), so the engine crosses
  // the point with the note already in its queue
  const uint32_t blockTicks = (uint32_t)(((uint64_t)kBlock << 16) / eng().samplesPerTickFp());
  CHECK_EQ((int)blockTicks, 2);
  uint32_t bar = nextBarTick(); runToTick(bar + 10);
  const uint32_t t0 = eng().status().tick; CHECK(t0 % 96 != 0);
  press(1, 0); CHECK(peekCmds().empty());
  const uint32_t grid = ((t0 + 95) / 96) * 96;
  uint32_t firedAt = 0;
  while (!firedAt && eng().status().tick < grid + 3) {
    iterate();
    if (!peekCmds().empty()) firedAt = eng().status().tick; else CHECK(grid - eng().status().tick > blockTicks);   // silent while more than a block away
  }
  CHECK(firedAt != 0); CHECK((int32_t)(grid - firedAt) >= 0); CHECK(grid - firedAt <= blockTicks);   // ahead of the grid point, within a block
  std::printf("  quantised note-on queued %u ticks before its grid point\n", (unsigned)(grid - firedAt));
  std::vector<Cmd> c = peekCmds(); const Cmd* n = findCmd(c, CmdPerformNote, 4); CHECK(n != nullptr); if (n) CHECK_NEAR(n->f, 0.8f, 1e-6);
  release(1, 0); c = peekCmds(); CHECK_EQ(countCmd(c, CmdPerformNote), 2); CHECK_NEAR(c.back().f, 0.0f, 1e-6); drainCmds();
  // released before its grid point: on and off arrive together at the grid point
  bar = nextBarTick(); runToTick(bar + 10);
  press(2, 0); release(2, 0); CHECK(peekCmds().empty());
  const uint32_t t1 = eng().status().tick; const uint32_t grid1 = ((t1 + 95) / 96) * 96;
  std::vector<Cmd> seen;
  while (eng().status().tick < grid1 + 3) { iterate(); for (const Cmd& k : peekCmds()) seen.push_back(k); drainCmds(); }
  CHECK_EQ(countCmd(seen, CmdPerformNote), 2); if (seen.size() == 2) { CHECK_NEAR(seen[0].f, 0.8f, 1e-6); CHECK_NEAR(seen[1].f, 0.0f, 1e-6); CHECK_EQ(seen[0].i, seen[1].i); }
  // recorded timestamps snap to the grid: a key pressed 30 ticks into the bar is recorded on the quarter (96)
  runToTick(nextBarTick() - 40); tap(13, 0); CHECK_EQ((int)u.looper(), (int)LpArmed);
  bar = nextBarTick(); runToTick(bar - kSwapLeadTicks + 1); CHECK_EQ((int)u.looper(), (int)LpRecording);
  runToTick(bar + 30); press(1, 0);
  runToTick(bar + 96 + 5); release(1, 0);
  CHECK_EQ((int)u.loopPattern().count, 1); if (u.loopPattern().count) CHECK_EQ((int)u.loopPattern().notes[0].start(), 96);
  tap(13, 0); drainCmds();
  runToTick(bar + kTicksPerBar - kSwapLeadTicks + 1);
  CHECK_EQ((int)u.looper(), (int)LpLooping);
  const Pattern& fx = eng().sourcePattern(4);
  CHECK_EQ((int)fx.count, 1); if (fx.count) CHECK_EQ((int)fx.notes[0].start(), 96);
  press(13, 0); advanceMs(750); release(13, 0, 760); CHECK_EQ((int)u.looper(), (int)LpIdle);
  tap(11, 1); CHECK_EQ(u.perfQuant(), 0);           // back to OFF for the tests that follow
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdTrackStopNow, 4); runBlocks(1); drainCmds();
}

// ---------------------------------------------------------------------------------------------- synth page, acid (SPEC §9.17c)

// SHIFT + pat1..4 of a row opens the synth page of that set's Chord / Lead / Pad / Bass: six knobs, the columns
// vol rev echo dist filt len step cutoff resonance envAmt decay drive wave (hold-ramps included, no overlay pages);
// the same combo, another combo (switches), the edit key or ` leave it.
TEST(synth_page_opens_and_routes_knob_keys) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ((int)u.page(), (int)PgMain);
  shiftDown(); tap(9, 0); shiftUp();                 // SHIFT + pat2 (row y0) = set 1 Lead
  CHECK_EQ((int)u.page(), (int)PgSynth); CHECK_EQ(u.synthTrack(), (int)Lead);
  CHECK(peekCmds().empty());                         // no Beat layer change (SPEC §9.17c)
  advanceMs(40); draw();
  CHECK_EQ(g_gfx.screens, 1); CHECK(g_gfx.sawText("S1 LEAD")); CHECK(!g_gfx.sawText("ACID"));
  for (const char* l : {"CUT", "RES", "ENV", "DEC", "DRV", "WAV"}) CHECK(g_gfx.drawn(l) != nullptr);
  CHECK(g_gfx.lines >= 6 * 19);                      // six arcs of 18 segments + a pointer each
  CHECK(g_gfx.sawText("Hz") || g_gfx.sawText("k")); CHECK(g_gfx.sawText("%"));
  const float cut0 = eng().params(Lead).cutoff;      // the preset's cutoff (knobFromCutoffHz), anywhere in 0..1
  const float cut1 = roundf((cut0 + 0.05f) / 0.05f) * 0.05f;   // a step lands on the 0.05 grid like every parameter
  press(0, 2);                                       // vol+ → cutoff: a step and a hold-ramp towards 1
  std::vector<Cmd> c = peekCmds(); const Cmd* p = findCmd(c, CmdSetParam, Lead);
  CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PCutoff); CHECK_NEAR(p->f, cut1, 1e-5); CHECK(p->f > cut0); }
  std::printf("  Lead cutoff %.3f → %.3f\n", cut0, p ? p->f : -1.0f);
  const Cmd* r = findCmd(c, CmdRamp, Lead); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PCutoff); CHECK_NEAR(r->f, 1.0f, 1e-6); CHECK(r->i > 0); }
  CHECK_EQ((int)u.page(), (int)PgSynth);             // no parameter overlay here
  key(0, 2, KeyEvent::Repeat, 400); CHECK_EQ((int)peekCmds().size(), 2);
  release(0, 2, 450); c = peekCmds(); const Cmd* fz = findCmd(c, CmdFreezeRamp, Lead); CHECK(fz != nullptr); if (fz) CHECK_EQ((int)fz->a, (int)PCutoff);
  runBlocks(1); drainCmds();
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 0); // only the changed knob is repainted
  tap(1, 2); c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PResonance); CHECK(p->f > 0.0f); } runBlocks(1); drainCmds();
  tap(2, 3); c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) CHECK_EQ((int)p->a, (int)PEnvAmt); runBlocks(1); drainCmds();
  tap(3, 2); c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) CHECK_EQ((int)p->a, (int)PDecay); runBlocks(1); drainCmds();
  tap(4, 2); c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PDrive); CHECK_NEAR(p->f, 0.05f, 1e-5); } runBlocks(1); drainCmds();
  const int w0 = eng().params(Lead).wave;
  tap(5, 2); c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PWave); CHECK_EQ((int)(p->f + 0.5f), (w0 + 1) % WaveCount); }
  CHECK_EQ(countCmd(c, CmdRamp), 0);                 // the wave is stepped, never ramped
  key(5, 2, KeyEvent::Repeat, 400); CHECK_EQ((int)peekCmds().size(), 1); runBlocks(1); drainCmds();
  tap(5, 3); c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) CHECK_EQ((int)(p->f + 0.5f), (int)eng().params(Lead).wave == w0 ? (w0 + WaveCount - 1) % WaveCount : w0); runBlocks(1); drainCmds();
  shiftDown(); press(1, 2);                          // SHIFT + rev+ : a SHIFT ramp of the resonance over rampBars, snap back on release
  c = peekCmds(); r = findCmd(c, CmdRamp, Lead); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PResonance); CHECK_NEAR(r->f, 1.0f, 1e-6); CHECK_EQ(r->i, 4 * kTicksPerBar); }
  CHECK_EQ(countCmd(c, CmdSetParam), 0);
  release(1, 2); shiftUp(); c = peekCmds(); CHECK_EQ(countCmd(c, CmdCancelRamps), 1); runBlocks(1); drainCmds();
  tap(0, 0 + 1); CHECK(peekCmds().empty());         // track keys are not live here (tempo / key are the SAT / ACC knobs since SPEC §9.22, see synth_page_sat_acc_cho_knobs)
  CHECK_EQ((int)u.page(), (int)PgSynth);
  shiftDown(); tap(8, 1); shiftUp();                 // another combo switches the page: set 2 Chord
  CHECK_EQ((int)u.page(), (int)PgSynth); CHECK_EQ(u.synthTrack(), 8);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("S2 CHORD"));
  shiftDown(); tap(8, 1); shiftUp(); CHECK_EQ((int)u.page(), (int)PgMain);   // the same combo closes it
  shiftDown(); tap(11, 0); shiftUp(); CHECK_EQ((int)u.page(), (int)PgSynth); CHECK_EQ(u.synthTrack(), (int)Bass);
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgSynth);  // the edit key is CHO− since SPEC §9.22 (it used to close the page) ...
  drainCmds(); tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);   // ... ` closes it
  shiftDown(); tap(10, 0); shiftUp(); CHECK_EQ(u.synthTrack(), (int)Pad);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);   // ` closes it
  CHECK_EQ(countCmd(peekCmds(), CmdBeatMask), 0); CHECK_EQ(countCmd(peekCmds(), CmdSwapPattern), 0);
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 1);
  drainCmds();
}

// SHIFT + genre toggles acid mode of the selected Chord / Lead / Pad / Bass (Generator::setAcid) with "ACID ON S1 LEAD"
// / "ACID OFF"; other tracks get "ACID: SYNTH TRACKS ONLY"; the main page marks acid tracks with an 'A' and the synth
// page header says ACID. The genre never changes.
TEST(shift_genre_toggles_acid_mode) {
  Ui& u = ui(); drainCmds();
  shiftDown(); tap(1, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 1);   // set 1 Lead
  const int g0 = gen().genreIndex();
  advanceMs(40); draw(); CHECK(g_gfx.drawn("A") == nullptr);
  shiftDown(); tap(9, 2); shiftUp();
  CHECK_EQ(gen().genreIndex(), g0); CHECK(gen().acidEnabled(1)); CHECK(!gen().acidEnabled(0));
  CHECK_EQ((int)u.page(), (int)PgMain);
  advanceMs(1); CHECK(strcmp(u.statusText(), "ACID ON S1 LEAD") == 0);
  advanceMs(40); draw(); const TextOp* a = g_gfx.drawn("A"); CHECK(a != nullptr);
  if (a) { const int cx = kCellX0 + 1 * kCellPitch; CHECK(a->x >= cx && a->x < cx + kCellSize); CHECK(a->y >= kRowY[0] && a->y < kRowY[0] + kCellSize); }
  { int guard = 0; while (gen().busy() && guard++ < 86 * 40) iterate(); }   // the real setAcid re-rolls the sound through the card
  CHECK(!gen().busy());
  shiftDown(); tap(9, 0); shiftUp(); CHECK_EQ((int)u.page(), (int)PgSynth);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("ACID"));
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  shiftDown(); tap(9, 2); shiftUp();
  CHECK(!gen().acidEnabled(1)); advanceMs(1); CHECK(strcmp(u.statusText(), "ACID OFF") == 0);
  { int guard = 0; while (gen().busy() && guard++ < 86 * 40) iterate(); }
  advanceMs(1100); advanceMs(40); draw(); CHECK(g_gfx.drawn("A") == nullptr);
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);   // a Beat
  shiftDown(); tap(9, 2); shiftUp();
  CHECK_EQ(gen().genreIndex(), g0); advanceMs(1); CHECK(strcmp(u.statusText(), "ACID: SYNTH TRACKS ONLY") == 0);
  shiftDown(); tap(4, 0); shiftUp(); shiftDown(); tap(9, 2); shiftUp();   // Fx is not an acid track either
  advanceMs(1); CHECK(strcmp(u.statusText(), "ACID: SYNTH TRACKS ONLY") == 0); CHECK(!gen().acidEnabled(4));
  tap(9, 2); CHECK(gen().genreIndex() != g0); tap(9, 2); CHECK_EQ(gen().genreIndex(), g0);   // without SHIFT the genre still rotates
  advanceMs(1300); drainCmds();
}

// Gen pattern on a track holding a recorded loop is refused (the loop stays until hold-del clears it, SPEC §9.17c);
// gen sound still re-rolls the sound and leaves the loop alone.
TEST(gen_pattern_keeps_a_recorded_loop) {
  Ui& u = ui(); drainCmds();
  CHECK(!gen().busy());
  shiftDown(); tap(1, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 1);
  TrackGenInfo& inf = gen().info(1); inf.userLoop = 1; inf.edited = 1;
  tap(12, 2);                                        // gen pattern: refused
  CHECK(!gen().busy()); advanceMs(1); CHECK(strcmp(u.statusText(), "LOOP KEPT: HOLD DEL CLEARS") == 0);
  CHECK_EQ((int)inf.edited, 1); CHECK_EQ((int)inf.userLoop, 1);
  tap(12, 3);                                        // gen sound: runs
  CHECK(gen().busy()); advanceMs(1); CHECK(strncmp(u.statusText(), "GEN SND", 7) == 0);
  int guard = 0; while (gen().busy() && guard++ < 86 * 40) iterate();
  CHECK(!gen().busy()); CHECK_EQ((int)inf.userLoop, 1);
  inf.userLoop = 0; inf.edited = 0;
  advanceMs(1100); drainCmds();
}

// Every performance change marks the session dirty (SPEC §9.18): a UI-only change (a parameter step touches no
// generator change count) arms the Sets debounce, and 5 s later (15 s after the previous write at the earliest, SPEC
// §9.19c) the session write runs (sessionBusy()). The master output level reaches the session file through
// Sets::setSessionVolume.
TEST(changes_mark_the_session_dirty) {
  Ui& u = ui(); drainCmds();
  CHECK_EQ((int)u.page(), (int)PgMain);
  for (int guard = 0; guard < 4000 && (sets().sessionBusy() || sets().busy() || gen().busy()); guard++) iterate();   // let earlier work settle
  runBlocks(200);                                    // a running write of an earlier dirty flag ...
  for (int guard = 0; guard < 4000 && sets().sessionBusy(); guard++) iterate();                                   // ... has finished
  CHECK(!sets().sessionBusy());
  tap(0, 2); tap(0, 3); runBlocks(1); drainCmds();   // vol+ / vol−: a UI-only change
  bool wrote = false; int blocks = 0;
  for (; blocks < kSessionWaitBlocks && !wrote; blocks++) { iterate(); if (sets().sessionBusy()) wrote = true; }
  CHECK(wrote);                                      // the session write started (5 s debounce on the sample clock, or the 15 s spacing after the previous write)
  std::printf("  session write started %d blocks (%.1f s) after the change\n", blocks, blocks * 256.0 / kSampleRate);
  for (int guard = 0; guard < 4000 && sets().sessionBusy(); guard++) iterate();
  CHECK(!sets().sessionBusy());
  u.setOutputVolume(123); CHECK_EQ(u.outputVolume(), 123);   // the level the next session write carries
  u.setOutputVolume(150);
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
}

// An output level set by config / serial (setOutputVolume) or SHIFT + kick cut marks the session dirty: the write that
// follows carries it as lp.volume (a `vol 0` before bed must not leave 150 in the file for the next boot).
TEST(output_volume_changes_write_the_session) {
  Ui& u = ui(); drainCmds();
  settleSession();
  CHECK(!sets().sessionBusy());
  u.setOutputVolume(0); CHECK_EQ(u.outputVolume(), 0);
  CHECK(waitSessionWrite());                         // the write started within the debounce (or the 15 s spacing after the previous write) ...
  std::string t = latestSessionText(0); CHECK(hasLine(t, "lp.volume=0"));   // ... and the newest file holds the level
  shiftDown(); press(13, 0); release(13, 0); shiftUp();   // SHIFT + kick cut S1: 0 → 16, likewise
  CHECK_EQ(u.outputVolume(), 16); CHECK_EQ((int)u.page(), (int)PgParam);
  CHECK(waitSessionWrite()); t = latestSessionText(0); CHECK(hasLine(t, "lp.volume=16"));
  u.setOutputVolume(123);                            // a non-default level for the restore test below
  CHECK(waitSessionWrite()); t = latestSessionText(0); CHECK(hasLine(t, "lp.volume=123"));
  u.setOutputVolume(123); CHECK(!waitSessionWrite()); // the same level again changes nothing: no write
  advanceMs(1300); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
}

// The lp.volume a session restore brings back is applied only when no explicit level (config.txt volume=, serial
// vol, SHIFT + kick cut) was set since boot: `vol 0` sent right after boot stays 0 through the restore, while a fresh
// Ui (the sketch's at boot) takes the restored level.
TEST(explicit_output_volume_wins_over_the_restored_session) {
  Ui& u = ui(); drainCmds();
  settleSession();
  CHECK(hasLine(latestSessionText(0), "lp.volume=123")); CHECK_EQ(u.outputVolume(), 123);
  sets().restoreSession();
  u.setOutputVolume(0);                              // the harness's `vol 0`, before the restore has run
  runBlocks(1); CHECK(sets().sessionBusy());         // the restore owns the card
  int guard = 0; while (sets().sessionBusy() && guard++ < 8000) iterate();
  CHECK(!sets().sessionBusy()); CHECK_EQ(sets().sessionVolume(), 123);   // the file's level was read ...
  runBlocks(2 * kTicksPerBar / 2); CHECK_EQ(u.outputVolume(), 0);         // ... but the explicit level stays
  for (int guard = 0; guard < 4000 && sets().sessionBusy(); guard++) iterate();   // the write the `vol 0` armed may run by now: a restored level is applied between writes only
  CHECK(!sets().sessionBusy());
  static Ui boot; boot.init(&g_gfx, &eng(), &gen(), &sets());            // no explicit level since its init: it applies the restored one
  CHECK_EQ(boot.outputVolume(), kOutputVolumeDefault);
  boot.tick(g_now); CHECK_EQ(boot.outputVolume(), 123);
  CHECK_EQ(u.outputVolume(), 0);
  u.setOutputVolume(150);                            // back to the default for the tests that follow
  CHECK(waitSessionWrite()); CHECK(hasLine(latestSessionText(0), "lp.volume=150"));
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  runBlocks(2); settleSession(); drainCmds(); advanceMs(1300);
}

// ---------------------------------------------------------------------------------------------- status, sets, generation

TEST(status_overlay_and_set_load_message) {
  Ui& u = ui(); drainCmds();
  u.showStatus("NO SD CARD", 3000);
  advanceMs(1); CHECK(u.statusVisible());
  advanceMs(40); draw(); CHECK(g_gfx.sawText("NO SD CARD"));
  int statusOps = 0; for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.y == kStatusY) statusOps++;
  CHECK_EQ(statusOps, 1);
  advanceMs(2950); CHECK(u.statusVisible());         // 2991 ms
  advanceMs(10); CHECK(!u.statusVisible());          // 3001 ms
  advanceMs(40); draw(); CHECK(!g_gfx.sawText("NO SD CARD"));
  statusOps = 0; for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.y == kStatusY) statusOps++;
  CHECK_EQ(statusOps, 1);                            // cleared once
  // load S1: House has no saved sets for A minor → "NO SET IN Amin" (SPEC §9.8)
  tap(11, 2);
  int guard = 0; while (sets().busy() && guard++ < 5000) iterate();
  advanceMs(1);
  CHECK(u.statusVisible());
  std::printf("  load status: %s (ok=%d err='%s')\n", u.statusText(), (int)sets().ok(), sets().lastError());
  if (sets().ok()) { CHECK(strncmp(u.statusText(), "LOADED ", 7) == 0); CHECK(strstr(u.statusText(), sets().lastName()) != nullptr); }
  else CHECK(strcmp(u.statusText(), sets().lastError()) == 0);   // the loader's own error ("NO SET IN Amin", ...), verbatim
  advanceMs(1100); CHECK(!u.statusVisible());
  drainCmds();
}

TEST(generate_set_shows_progress_then_done) {
  Ui& u = ui(); drainCmds();
  gen().info(7).edited = 1;
  tap(13, 2);                                        // GENERATE S1
  CHECK(gen().busy());
  advanceMs(1);
  CHECK(strncmp(u.statusText(), "GEN S1 0/8", 10) == 0);
  CHECK_EQ((int)gen().info(7).edited, 0);            // the pattern will be replaced
  advanceMs(40); draw(); CHECK(g_gfx.sawText("GEN S1 0/8"));
  int steps = 0; bool sawMid = false;
  while (gen().busy() && steps++ < 86 * 40) { iterate(); if (strncmp(u.statusText(), "GEN S1 ", 7) == 0 && u.statusText()[7] >= '1' && u.statusText()[7] <= '7') sawMid = true; }
  CHECK(!gen().busy()); CHECK(sawMid);
  advanceMs(1);
  std::printf("  after generation: %s (%d steps)\n", u.statusText(), steps);
  CHECK(strcmp(u.statusText(), "GEN DONE") == 0 || strcmp(u.statusText(), "FX kept") == 0);
  CHECK(gen().lastError()[0] == 0);
  for (int t = 0; t < kTracksPerSet; t++) CHECK(eng().status().track[t].state != PSEmpty);
  advanceMs(40); draw();
  std::set<int> cells = g_gfx.cellsTouched(); cells.erase(-1);
  CHECK(cells.size() >= 1);                          // the cells that became loaded were redrawn
  advanceMs(1100); CHECK(!u.statusVisible());
  tap(12, 2);                                        // gen pattern for the selected track (Chord) → "GEN PAT 0/1"
  advanceMs(1); CHECK(strncmp(u.statusText(), "GEN PAT", 7) == 0);
  steps = 0; while (gen().busy() && steps++ < 86 * 20) iterate();
  CHECK(!gen().busy());
  drainCmds();
}

TEST(explicit_status_survives_progress_updates) {
  Ui& u = ui(); drainCmds();
  gen().info(4).userLoop = 0;
  advanceMs(1); CHECK(strcmp(u.statusText(), "GEN DONE") == 0);   // the previous job's result is still up (1 s) ...
  tap(13, 2); CHECK(gen().busy());                   // GENERATE S1
  CHECK(strcmp(u.statusText(), "GEN S1 0/8") == 0); // ... a job that starts supersedes it at once
  runBlocks(2); CHECK(gen().busy());
  CHECK(strncmp(u.statusText(), "GEN S1", 6) == 0);
  u.showStatus("PATTERN FULL", 100);                 // raised while the job reports progress every tick
  runBlocks(3); CHECK(gen().busy());                 // 36 ms later the message is still the one shown
  CHECK(strcmp(u.statusText(), "PATTERN FULL") == 0);
  CHECK(strncmp(u.progressText(), "GEN S1", 6) == 0);   // progress kept apart
  advanceMs(40); draw(); CHECK(g_gfx.sawText("PATTERN FULL")); CHECK(!g_gfx.sawText("GEN S1"));
  runBlocks(8); CHECK(gen().busy());                 // expired: progress shows again
  CHECK(strncmp(u.statusText(), "GEN S1", 6) == 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("GEN S1"));
  int guard = 0; while (gen().busy() && guard++ < 86 * 40) iterate();
  CHECK(!gen().busy());
  advanceMs(1); CHECK(strcmp(u.statusText(), "GEN DONE") == 0 || strcmp(u.statusText(), "FX kept") == 0);
  advanceMs(1100); CHECK(!u.statusVisible()); drainCmds();
}

TEST(generate_and_sets_jobs_exclude_each_other) {
  Ui& u = ui(); drainCmds();
  CHECK(!gen().busy()); CHECK(!sets().busy());
  tap(11, 2);                                        // load S1: the directory scan is in flight
  CHECK(sets().busy());
  tap(13, 2);                                        // GENERATE S1 meanwhile: refused, no job queued
  CHECK(!gen().busy());
  tap(12, 2); CHECK(!gen().busy());                  // gen pattern / gen sound likewise
  tap(12, 3); CHECK(!gen().busy());
  advanceMs(1); CHECK(strcmp(u.statusText(), "SETS BUSY") == 0);
  const int g0 = gen().genreIndex();
  tap(9, 2); CHECK_EQ(gen().genreIndex(), g0);      // a genre switch would invalidate the index under the walk
  CHECK_EQ((int)u.page(), (int)PgMain);
  int guard = 0; while (sets().busy() && guard++ < 5000) iterate();
  CHECK(!sets().busy()); CHECK(!gen().busy());
  advanceMs(1100);
  // the other way round: save / load are refused while a generation job runs
  tap(13, 2); CHECK(gen().busy());
  tap(10, 2); CHECK(!sets().busy());                 // save S1
  advanceMs(1); CHECK(strcmp(u.statusText(), "GEN BUSY") == 0);
  tap(11, 2); CHECK(!sets().busy());                 // load S1
  tap(11, 3); CHECK(!sets().busy());                 // load S2
  tap(9, 2); CHECK_EQ(gen().genreIndex(), g0);
  guard = 0; while (gen().busy() && guard++ < 86 * 40) iterate();
  CHECK(!gen().busy()); CHECK(!sets().busy());
  advanceMs(1100); CHECK(!u.statusVisible()); drainCmds();
}

// ---------------------------------------------------------------------------------------------- loop mode (SPEC §9.19)

static_assert(kLoopBpm == 125, "the LOOP ON status below names the loop tempo");
static const char* const kLoopTempoStatus = "LOOP ON - 125 BPM";
static void loopKey() { shiftDown(); tap(8, 3); shiftUp(); }   // SHIFT + edit
static void settleJobs() { int guard = 0; while ((gen().busy() || sets().busy()) && guard++ < 86 * 40) iterate(); }
static void loopsOff() { settleJobs(); for (int t = 0; t < kTracks; t++) if (gen().loopEnabled(t)) gen().setLoopMode(t, false); runBlocks(2); drainCmds(); }
static void setTempo(int bpm) { push(CmdSetTempo, 0, 0, bpm); runBlocks(1); CHECK_EQ((int)eng().status().bpm, bpm); drainCmds(); }
// Iterates until Generator::anyLoop() is true (the stub flips it at once, the real generator when its pick binds the slot).
static bool waitAnyLoop() { int guard = 0; while (!gen().anyLoop() && guard++ < 4000) iterate(); return gen().anyLoop(); }
// The 'L' marker drawn in this draw inside cell `track`'s box (nullptr when none).
static const TextOp* loopMarkIn(int track) {
  const int cx = kCellX0 + (track % kTracksPerSet) * kCellPitch, cy = kRowY[track / kTracksPerSet];
  for (const TextOp& t : g_gfx.textOps) if (t.text == "L" && t.x >= cx && t.x < cx + kCellSize && t.y >= cy && t.y < cy + kCellSize) return &t;
  return nullptr;
}
// The colour this draw painted cell `track`'s square with: its outline (drawRect) or, for a playing track, its fill.
static Color cellPaint(int track) {
  const int cx = kCellX0 + (track % kTracksPerSet) * kCellPitch, cy = kRowY[track / kTracksPerSet];
  Color c = 0;
  for (const Op& o : g_gfx.ops) if ((o.kind == 'R' || o.kind == 'F') && o.x == cx && o.y == cy && o.w == kCellSize && o.h == kCellSize) c = o.c;
  return c;
}

TEST(loop_mode_pure_helpers) {
  char m[32];
  Ui::loopRefusalText(Generator::LoopSlotsFull, Beat, m, sizeof m); CHECK(strcmp(m, "LOOP SLOTS FULL (2)") == 0);
  Ui::loopRefusalText(Generator::LoopNoContent, Beat, m, sizeof m); CHECK(strcmp(m, "NO LOOP: DRUM") == 0);
  Ui::loopRefusalText(Generator::LoopNoContent, Percussion, m, sizeof m); CHECK(strcmp(m, "NO LOOP: PERCUSSION") == 0);
  Ui::loopRefusalText(Generator::LoopNoContent, Vocal, m, sizeof m); CHECK(strcmp(m, "NO LOOP: VOCAL") == 0);
  Ui::loopRefusalText(Generator::LoopNoContent, Fx, m, sizeof m); CHECK(strcmp(m, "NO LOOP: FX") == 0);
  Ui::loopRefusalText(Generator::LoopBusy, Lead, m, sizeof m); CHECK(strcmp(m, "LOOP: BUSY") == 0);
  Ui::loopRefusalText(Generator::LoopLowRam, Lead, m, sizeof m); CHECK(strcmp(m, "LOOP: LOW RAM") == 0);
  Ui::loopRefusalText(Generator::LoopNoCard, Lead, m, sizeof m); CHECK(strcmp(m, "LOOP: NO CARD") == 0);
  CHECK(strcmp(loopVariantName(0), "WHOLE") == 0); CHECK(strcmp(loopVariantName(1), "HALF") == 0);
  CHECK(strcmp(loopVariantName(2), "2 BEATS") == 0); CHECK(strcmp(loopVariantName(3), "1 BEAT") == 0);
  // the contract's category → folder map (the UI's NO LOOP text is built from it)
  CHECK(strcmp(Generator::loopFolder(Beat), "drum") == 0); CHECK(strcmp(Generator::loopFolder(Percussion), "percussion") == 0);
  CHECK(strcmp(Generator::loopFolder(Bass), "bass") == 0); CHECK(strcmp(Generator::loopFolder(Chord), "chord") == 0);
  CHECK(strcmp(Generator::loopFolder(Lead), "lead") == 0); CHECK(strcmp(Generator::loopFolder(Pad), "pad") == 0);
  CHECK(strcmp(Generator::loopFolder(Fx), "fx") == 0); CHECK(strcmp(Generator::loopFolder(Vocal), "vocal") == 0);
  char t[32]; snprintf(t, sizeof t, kLoopTempoStatusFmt, kLoopBpm); CHECK(strcmp(t, kLoopTempoStatus) == 0);
  // the footer's underrun count: samples as they are below 10 000, thousands from there (the two uint16 slot counters)
  char n[8];
  Ui::loopUnderrunText(n, sizeof n, 3); CHECK(strcmp(n, "3") == 0);
  Ui::loopUnderrunText(n, sizeof n, 9999); CHECK(strcmp(n, "9999") == 0);
  Ui::loopUnderrunText(n, sizeof n, 10000); CHECK(strcmp(n, "10k") == 0);
  Ui::loopUnderrunText(n, sizeof n, 13312); CHECK(strcmp(n, "13k") == 0);
  Ui::loopUnderrunText(n, sizeof n, 2u * 65535u); CHECK(strcmp(n, "131k") == 0);
  // the widest line that keeps the note fits beside the widest battery readout at the fake font's 6 px per character
  CHECK_EQ((int)strlen("C# MIN 240 LOOP UNDERRUNS 131k") * 6, 180); CHECK(180 <= 240 - kFooterBatMargin - (int)strlen("CHG 100%") * 6 - kFooterGap - kFooterTextX);
}

// SHIFT + edit toggles loop mode on the selected track: the statuses, the 'L' mark, the pick blink, the tempo-to-125
// rule on the first loop in use (not during a drift), the refusals of the perform page and the acid toggle, the
// editor's loop view, the Vocal track's slot-less loop, both slots taken, and the toggle back off.
TEST(shift_edit_toggles_loop_mode_with_tempo_125) {
  Ui& u = ui(); drainCmds(); loopsOff();
  CHECK(!gen().anyLoop()); CHECK_EQ(u.loopUnderruns(), 0u);
  setTempo(120);
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);   // set 1 Beat
  advanceMs(1100); CHECK(!u.statusVisible());
  advanceMs(40); draw(); CHECK(loopMarkIn(7) == nullptr);
  loopKey();
  std::printf("  setLoopMode(S1 Beat): enabled=%d refusal=%d file='%s' bars=%d slot=%d (%s)\n", (int)gen().loopEnabled(7), (int)gen().lastLoopRefusal(),
              gen().loopFile(7), (int)gen().loopBars(7), gen().loopSlot(7), loopstub::active() ? "host stub" : "real generator");
  CHECK_EQ((int)u.page(), (int)PgMain);                                 // SHIFT + edit never opens the editor
  CHECK(gen().loopEnabled(7)); CHECK_EQ((int)gen().lastLoopRefusal(), (int)Generator::LoopOk);
  CHECK(strcmp(u.statusText(), "LOOP ON S1 BEAT") == 0);
  CHECK(u.loopPickPending(7));                                           // requested, the engine streams nothing yet
  advanceMs(40); draw(); CHECK(loopMarkIn(7) != nullptr);               // the 'L' mark, top left of the Beat square
  { const TextOp* l = loopMarkIn(7); if (l) { CHECK(l->x < kCellX0 + 7 * kCellPitch + kCellSize / 2); if (eng().status().track[7].state != PSPlaying) CHECK_EQ((int)l->c, (int)categoryColor(Beat)); } }
  // the pick blinks the square: two draws half a blink apart repaint cell 7 with alternating colours
  if (u.loopPickPending(7)) {
    advanceMs(kBlinkHalfMs); draw(); std::set<int> c1 = g_gfx.cellsTouched(); CHECK_EQ((int)c1.count(7), 1); const Color o1 = cellPaint(7);
    advanceMs(kBlinkHalfMs); draw(); std::set<int> c2 = g_gfx.cellsTouched(); CHECK_EQ((int)c2.count(7), 1); const Color o2 = cellPaint(7);
    CHECK(o1 != o2); CHECK(o1 == categoryColor(Beat) || o2 == categoryColor(Beat));
  }
  // the first loop in use pulls the tempo to 125 (SPEC §9.19 tempo rule) and says so
  CHECK(waitAnyLoop());
  advanceMs(1);
  CHECK(strcmp(u.statusText(), kLoopTempoStatus) == 0);
  runBlocks(1); CHECK_EQ((int)eng().status().bpm, 125);
  CHECK_EQ(u.loopUnderruns(), 0u);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("A MIN 125 HOUSE"));
  // refusals on a loop track: the perform page, and the acid toggle on a synth loop track
  tap(9, 3); CHECK_EQ((int)u.page(), (int)PgMain); CHECK(strcmp(u.statusText(), "PERFORM: LOOP TRACK") == 0);
  shiftDown(); tap(9, 3); shiftUp();                                     // SHIFT + perform targets the same track of set 2 (not a loop track): the page opens
  CHECK_EQ((int)u.page(), (int)PgPerform); CHECK_EQ(u.perfTrack(), 15); tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  shiftDown(); tap(3, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 3);   // set 1 Bass
  loopKey();
  CHECK(gen().loopEnabled(3)); CHECK(strcmp(u.statusText(), "LOOP ON S1 BASS") == 0);
  CHECK(gen().loopSlot(3) >= 0 && gen().loopSlot(3) < kLoopSlots); CHECK(gen().loopSlot(3) != gen().loopSlot(7));
  const bool acid0 = gen().acidEnabled(3);
  shiftDown(); tap(9, 2); shiftUp();
  CHECK(strcmp(u.statusText(), "ACID: LOOP TRACK") == 0); CHECK_EQ((int)gen().acidEnabled(3), (int)acid0);
  // the editor on a loop track shows the loop (name, bars, variant), no grid and no edits
  const int notes0 = eng().sourcePattern(3).count;
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgEditor); CHECK_EQ(u.editorTrack(), 3);
  advanceMs(40); draw();
  CHECK(g_gfx.sawText("EDIT S1 BAS  LOOP MODE")); CHECK(g_gfx.drawn("LOOP") != nullptr); CHECK(g_gfx.sawText("VARIANT"));
  { char v[24]; snprintf(v, sizeof v, "VARIANT %d %s", (gen().info(3).variant & 3) + 1, loopVariantName(gen().info(3).variant & 3)); CHECK(g_gfx.sawText(v)); }
  CHECK(!g_gfx.sawText("OCT ")); CHECK(!g_gfx.sawText("GATE"));         // the melodic grid's footer is not there
  if (gen().loopFile(3)[0]) { char head[24]; strncpy(head, gen().loopFile(3), 20); head[20] = 0; CHECK(g_gfx.sawText(head)); }
  else CHECK(g_gfx.sawText("PICKING"));
  if (gen().loopBars(3)) { char b[16]; snprintf(b, sizeof b, "%d BAR", (int)gen().loopBars(3)); CHECK(g_gfx.sawText(b)); }
  tap(13, 2); tap(0, 2); tap(12, 3); tap(11, 3);                         // enter, vol+, right, down: nothing edits, nothing moves
  CHECK_EQ((int)eng().sourcePattern(3).count, notes0); CHECK_EQ(u.editorCursorX(), 0); CHECK_EQ(u.editorCursorY(), 0);
  loopKey();                                                             // SHIFT + edit inside the editor toggles the same track off ...
  CHECK_EQ((int)u.page(), (int)PgEditor); CHECK(!gen().loopEnabled(3)); CHECK(strcmp(u.statusText(), "LOOP OFF S1 BASS") == 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("EDIT S1 BAS  BAR 1/")); CHECK(g_gfx.sawText("OCT ")); CHECK(g_gfx.sawText("GATE"));   // ... and the grid is back
  tap(8, 3); CHECK_EQ((int)u.page(), (int)PgMain);
  // both slots taken: a third non-vocal track is refused with LOOP SLOTS FULL (2); the Vocal track needs no slot
  shiftDown(); tap(3, 0); shiftUp(); loopKey(); CHECK(gen().loopEnabled(3));
  shiftDown(); tap(0, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 0);
  loopKey();
  CHECK(!gen().loopEnabled(0)); CHECK_EQ((int)gen().lastLoopRefusal(), (int)Generator::LoopSlotsFull);
  CHECK(strcmp(u.statusText(), "LOOP SLOTS FULL (2)") == 0);
  advanceMs(40); draw(); CHECK(loopMarkIn(0) == nullptr); CHECK(loopMarkIn(3) != nullptr); CHECK(loopMarkIn(7) != nullptr);
  shiftDown(); tap(6, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 6);
  loopKey();
  std::printf("  setLoopMode(S1 Vocal): enabled=%d refusal=%d slot=%d\n", (int)gen().loopEnabled(6), (int)gen().lastLoopRefusal(), gen().loopSlot(6));
  CHECK(gen().loopEnabled(6)); CHECK_EQ(gen().loopSlot(6), -1); CHECK(strcmp(u.statusText(), "LOOP ON S1 VOCAL") == 0);
  loopKey(); CHECK(!gen().loopEnabled(6)); CHECK(strcmp(u.statusText(), "LOOP OFF S1 VOCAL") == 0);
  // a generate on a loop track picks a new loop (not refused); the square blinks while the job runs
  gen().info(7).userLoop = 0;
  shiftDown(); tap(7, 0); shiftUp(); tap(12, 2);                        // gen pattern on the Beat loop track
  CHECK(gen().busy()); CHECK(u.loopPickPending(7)); CHECK(gen().loopEnabled(7));
  settleJobs(); CHECK(!gen().busy());
  // off again: the mark goes, the tempo rule waits for the next false → true edge
  loopKey(); CHECK(!gen().loopEnabled(7)); CHECK(strcmp(u.statusText(), "LOOP OFF S1 BEAT") == 0);
  advanceMs(40); draw(); CHECK(loopMarkIn(7) == nullptr);
  shiftDown(); tap(3, 0); shiftUp(); loopKey(); CHECK(!gen().loopEnabled(3));
  runBlocks(2); CHECK(!gen().anyLoop());
  setTempo(120); advanceMs(1100);
  shiftDown(); tap(7, 0); shiftUp(); loopKey(); CHECK(gen().loopEnabled(7));
  CHECK(waitAnyLoop()); advanceMs(1); CHECK(strcmp(u.statusText(), kLoopTempoStatus) == 0);
  runBlocks(1); CHECK_EQ((int)eng().status().bpm, 125);
  loopsOff(); CHECK(!gen().anyLoop());
  // a running tempo drift is left alone: no CmdSetTempo, the plain LOOP ON status stays
  setTempo(120); advanceMs(1100);
  shiftDown(); tap(6, 2); shiftUp(); drainCmds();                        // SHIFT + tempo+: drift towards 130
  shiftDown(); tap(7, 0); shiftUp(); loopKey(); CHECK(gen().loopEnabled(7));
  CHECK(waitAnyLoop()); advanceMs(1);
  CHECK(strcmp(u.statusText(), "LOOP ON S1 BEAT") == 0); CHECK(findCmd(peekCmds(), CmdSetTempo) == nullptr);
  runBlocks(8); CHECK(eng().status().bpm < 125);
  shiftDown(); tap(6, 2); shiftUp();                                     // the same key stops the drift
  loopsOff(); setTempo(120); advanceMs(1300); drainCmds();
}

// The rush key on a Beat in loop mode (SPEC §9.19 §1.4 as amended by §9.22c): the engine never steps a streaming
// track's pattern, so the UI asks the generator instead — Generator::rushLoop crops the stream to HALF the clip length in
// force (len / 2, at least one beat, never past the file) from the next 1/16 while the key is held (no CmdSnareRush) and
// brings the stored crop and a len± made before back on release; info().variant / lenStep never move. The other set's Beat, not in loop mode, still gets CmdSnareRush; a rush that
// began on the engine is released there even when loop mode came on while the key was held.
TEST(rush_key_on_a_loop_beat_track_crops_the_stream) {
  Ui& u = ui(); drainCmds(); loopsOff();
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);   // set 1 Beat
  loopKey(); CHECK(gen().loopEnabled(7)); CHECK(waitAnyLoop()); runBlocks(1); drainCmds();
  const int slot = gen().loopSlot(7); CHECK(slot >= 0);
  if (slot < 0) { loopsOff(); return; }
  tap(7, 0); runToTick(nextBarTick() + 2 * kTicksPerBar);               // the Beat plays: the engine adopts the stream at a bar line
  { int guard = 0; while (guard++ < 86 * 20 && (gen().busy() || !eng().trackStreams(7))) iterate(); }
  CHECK(eng().trackStreams(7));
  const int bars = gen().loopBars(7); const uint16_t nat = (uint16_t)(bars * kTicksPerBar); CHECK(bars >= 1);
  auto settled = [&](int clip, int region) {                            // the slot stream carries that cycle / crop and no switch is pending
    for (int i = 0; i < 600 && (gen().loopSwitchPending(slot) || gen().loopClipTicks(slot) != clip || gen().loopRegionTicks(slot) != region); i++) iterate();
    CHECK(!gen().loopSwitchPending(slot)); CHECK_EQ((int)gen().loopClipTicks(slot), clip); CHECK_EQ((int)gen().loopRegionTicks(slot), region);
  };
  settled(nat, nat);
  advanceMs(1300); drainCmds();
  tap(5, 2); runBlocks(1);                                               // len+ on the loop: k 1 (unless the loop is already kMaxLoopBars long)
  const int k1 = Generator::clampLoopStep(nat, 1); const int len1 = Generator::loopLenOf(nat, k1);
  CHECK_EQ((int)gen().info(7).lenStep, k1); settled(len1, nat);
  advanceMs(1300); drainCmds();
  press(12, 0);                                                          // rush set 1: the generator crops the stream, the engine's rush is never asked
  CHECK(gen().loopRushing(7)); CHECK(findCmd(peekCmds(), CmdSnareRush) == nullptr);
  CHECK_EQ((int)u.page(), (int)PgNote); advanceMs(40); draw(); CHECK(g_gfx.sawText("S1 RUSH"));
  CHECK_EQ((int)gen().info(7).lenStep, k1); CHECK_EQ((int)gen().info(7).variant, 0);   // nothing the UI shows moves
  int rushLen = len1 / 2;                                                // half the clip length set (len+ above doubled it), clamped to one beat … the whole file
  if (rushLen < kTicksPerBeat) rushLen = kTicksPerBeat;
  if (rushLen > nat) rushLen = nat;
  settled(rushLen, rushLen);                                             // looped every half
  auto engineLen = [&](int len) { for (int i = 0; i < 3 * kSampleRate / kBlock && (int)eng().status().track[7].lenTicks != len; i++) iterate(); CHECK_EQ((int)eng().status().track[7].lenTicks, len); };
  engineLen(rushLen);                                                    // the carrier swap lands at the bar: the rush's clip under the stored variant
  CHECK_EQ((int)eng().status().track[7].rush, 0);                        // the engine's pattern rush is never engaged
  release(12, 0, 600);
  CHECK(!gen().loopRushing(7)); CHECK(findCmd(peekCmds(), CmdSnareRush) == nullptr);
  settled(len1, nat);                                                    // the stored crop and len are back
  CHECK_EQ((int)gen().info(7).lenStep, k1);
  engineLen(len1);
  advanceMs(700); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
  press(12, 1);                                                          // set 2's Beat is not a loop track: the engine's rush as before
  { std::vector<Cmd> c = peekCmds(); const Cmd* r = findCmd(c, CmdSnareRush); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, 1); CHECK_EQ(r->i, 1); } }
  CHECK(!gen().loopRushing(15));
  runBlocks(1); drainCmds(); release(12, 1, 300);
  { std::vector<Cmd> c = peekCmds(); const Cmd* r = findCmd(c, CmdSnareRush); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, 1); CHECK_EQ(r->i, 0); } }
  runBlocks(1); drainCmds(); advanceMs(700);
  // a rush begun on the engine keeps its release there although loop mode came on while the key was held
  loopKey(); CHECK(!gen().loopEnabled(7)); runBlocks(2); drainCmds();   // S1 Beat (still selected) out of loop mode
  press(12, 0); { std::vector<Cmd> c = peekCmds(); const Cmd* r = findCmd(c, CmdSnareRush); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, 0); CHECK_EQ(r->i, 1); } }
  runBlocks(1); drainCmds();
  loopKey(); CHECK(gen().loopEnabled(7)); CHECK(!gen().loopRushing(7));
  release(12, 0, 400); { std::vector<Cmd> c = peekCmds(); const Cmd* r = findCmd(c, CmdSnareRush); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, 0); CHECK_EQ(r->i, 0); } }
  CHECK(!gen().loopRushing(7));
  runBlocks(1); drainCmds();
  push(CmdTrackStopNow, 7); runBlocks(1);
  loopsOff(); setTempo(120); advanceMs(1300); drainCmds();
}

// The refusals the generator reports: too little free heap, a card job running (the Sets machine's and the generator's
// own), and a genre without loop content ("NO LOOP: DRUM").
TEST(loop_mode_refusals) {
  Ui& u = ui(); drainCmds(); loopsOff();
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);
  gen().setFreeHeapHook([]() -> uint32_t { return (uint32_t)kLoopLowRamBytes - 1; });
  loopKey();
  CHECK(!gen().loopEnabled(7)); CHECK_EQ((int)gen().lastLoopRefusal(), (int)Generator::LoopLowRam); CHECK(strcmp(u.statusText(), "LOOP: LOW RAM") == 0);
  gen().setFreeHeapHook(nullptr);
  tap(11, 2); CHECK(sets().busy());                                      // load S1 owns the card
  loopKey();
  CHECK(!gen().loopEnabled(7)); CHECK(strcmp(u.statusText(), "LOOP: BUSY") == 0);
  settleJobs(); CHECK(!sets().busy()); advanceMs(1100);
  tap(13, 2); CHECK(gen().busy());                                       // GENERATE S1 runs
  loopKey();
  CHECK(!gen().loopEnabled(7)); CHECK(strcmp(u.statusText(), "LOOP: BUSY") == 0);
  settleJobs(); CHECK(!gen().busy()); advanceMs(1100);
  const int g0 = gen().genreIndex();
  tap(9, 2); CHECK(gen().genreIndex() != g0); CHECK(strcmp(gen().genreName(), "House") != 0);   // Testpack has no loops/ folder
  advanceMs(1300);
  loopKey();
  CHECK(!gen().loopEnabled(7)); CHECK_EQ((int)gen().lastLoopRefusal(), (int)Generator::LoopNoContent); CHECK(strcmp(u.statusText(), "NO LOOP: DRUM") == 0);
  shiftDown(); tap(5, 0); shiftUp(); loopKey(); CHECK(strcmp(u.statusText(), "NO LOOP: PERCUSSION") == 0);
  shiftDown(); tap(6, 0); shiftUp(); loopKey(); CHECK(strcmp(u.statusText(), "NO LOOP: VOCAL") == 0);
  tap(9, 2); CHECK_EQ(gen().genreIndex(), g0);
  CHECK(!gen().anyLoop());
  advanceMs(1300); drainCmds();
}

// The footer's LOOP UNDERRUNS note: the loop slots' Engine::streamUnderruns since loop mode came into use, shown whole,
// ahead of the OUT / GYRO notes and the genre, and reset once no loop is in use. The engine only starves a ring the
// real generator filled (the host stub streams nothing), so the display check follows the count the engine reports.
TEST(footer_loop_underruns_note) {
  Ui& u = ui(); drainCmds(); loopsOff();
  CHECK_EQ(u.loopUnderruns(), 0u);
  shiftDown(); tap(7, 0); shiftUp(); loopKey(); CHECK(gen().loopEnabled(7));
  CHECK(waitAnyLoop()); runBlocks(1); drainCmds();
  const int slot = gen().loopSlot(7); CHECK(slot >= 0);
  tap(7, 0); runToTick(nextBarTick() + 2 * kTicksPerBar);               // the Beat plays through two bar lines (the engine adopts the stream at one of them)
  { int guard = 0; while (guard++ < 86 * 20 && (gen().busy() || !eng().trackStreams(7))) iterate(); }
  std::printf("  streaming=%d slot=%d underruns before starving=%u\n", (int)eng().trackStreams(7), slot, (unsigned)u.loopUnderruns());
  for (int i = 0; i < 60; i++) { eng().render(g_out, kBlock); g_now += kBlockMs; u.tick(g_now); }   // 60 blocks without the producer: a filled ring runs dry
  const uint32_t n = u.loopUnderruns();
  std::printf("  loop underruns after starving: %u (%s)\n", (unsigned)n, loopstub::active() ? "host stub: the engine streams nothing" : "real stream");
  advanceMs(40); draw();
  if (n) {
    char cnt[8]; Ui::loopUnderrunText(cnt, sizeof cnt, n);               // the engine counts samples: thousands from 10 000
    char note[32]; snprintf(note, sizeof note, "LOOP UNDERRUNS %s", cnt);
    CHECK(g_gfx.sawText(note));
    bool whole = true; for (const TextOp& t : g_gfx.textOps) if (t.text.find("LOOP UNDERRUNS") != std::string::npos && t.text.find(note) == std::string::npos) whole = false;
    CHECK(whole);
    CHECK(!g_gfx.sawText("HOUSE"));                                      // the genre makes room for the note
    CHECK(g_gfx.sawText("LEN "));                                        // ... and LEN still fits beside the "--" readout
    u.setBattery(100, 1); advanceMs(40); draw();                         // the widest readout, "CHG 100%": LEN goes before the note does
    CHECK(g_gfx.drawn("CHG 100%") != nullptr); CHECK(g_gfx.sawText(note)); CHECK(!g_gfx.sawText("LEN ")); CHECK(!g_gfx.sawText("HOUSE"));
    { const TextOp* foot = nullptr; for (const TextOp& t : g_gfx.textOps) if (t.text.find(note) != std::string::npos) foot = &t;
      CHECK(foot != nullptr); if (foot) CHECK(foot->x + foot->w + kFooterGap <= g_gfx.drawn("CHG 100%")->x); }   // the line never runs into the readout
    u.setBattery(-1, -1); advanceMs(40); draw();                         // the readout the tests run with
  } else CHECK(!g_gfx.sawText("LOOP UNDERRUNS"));
  push(CmdTrackStopNow, 7); runBlocks(1);
  loopsOff(); advanceMs(1); CHECK(!gen().anyLoop()); CHECK_EQ(u.loopUnderruns(), 0u);   // the baseline follows the counters while no loop is in use
  advanceMs(40); draw(); CHECK(!g_gfx.sawText("LOOP UNDERRUNS"));
  setTempo(120); advanceMs(1300); drainCmds();
}

// Pops every queued command and pushes back all but those of type `t` (room in a queue a test filled on purpose).
static void dropCmds(CmdType t) { std::vector<Cmd> keep; Cmd c; while (eng().cmds().pop(c)) if (c.type != t) keep.push_back(c); for (const Cmd& k : keep) eng().cmds().push(k); }

// The tempo rule's edge (SPEC §9.19d): a full command queue keeps it for the next tick (no "LOOP ON - 125 BPM" until
// CmdSetTempo 125 really went out), and the boot restore is the one Sets job exempt from the rule — a session restored
// with a loop track brings back its own lp.bpm, the restore consumes the edge, and a second loop track pulls nothing
// either. (A user set load is not exempt: Sets applies lp.bpm only while nothing plays, so the rule runs as above.)
TEST(loop_tempo_rule_edge_waits_for_the_queue_and_skips_the_restore) {
  Ui& u = ui(); drainCmds(); loopsOff(); settleSession();
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  runBlocks(2); drainCmds(); setTempo(120);
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7);
  advanceMs(1100); CHECK(!u.statusVisible());
  loopKey(); CHECK(gen().loopEnabled(7)); CHECK(strcmp(u.statusText(), "LOOP ON S1 BEAT") == 0);
  // the pick runs with no UI tick in between; the queue is then filled before the first tick that sees anyLoop()
  { int guard = 0; while (!gen().anyLoop() && guard++ < 4000) { gen().step(); sets().step(); eng().render(g_out, kBlock); g_now += kBlockMs; } }
  CHECK(gen().anyLoop());
  Cmd filler; filler.type = CmdNone; filler.track = 0; filler.a = 0; filler.i = 0; filler.f = 0.0f;
  int filled = 0; while (eng().cmds().push(filler)) filled++;
  CHECK(filled > 0); CHECK(!eng().cmds().push(filler));
  advanceMs(1);                                                          // the tick finds the queue full: nothing sent, nothing claimed
  CHECK(strcmp(u.statusText(), kLoopTempoStatus) != 0); CHECK(findCmd(peekCmds(), CmdSetTempo) == nullptr);
  dropCmds(CmdNone);                                                     // room again: the kept edge fires on the next tick
  advanceMs(1);
  CHECK(strcmp(u.statusText(), kLoopTempoStatus) == 0);
  { const std::vector<Cmd> q = peekCmds(); const Cmd* c = findCmd(q, CmdSetTempo); CHECK(c != nullptr); if (c) CHECK_EQ((int)c->i, kLoopBpm); }
  runBlocks(1); CHECK_EQ((int)eng().status().bpm, 125);
  std::printf("  queue full at the edge: %d fillers, 125 sent on the tick after the drain (%s)\n", filled, loopstub::active() ? "host stub" : "real generator");
  // the boot restore: the session file carries the loop track and the tempo the user chose with it
  setTempo(118);
  CHECK(waitSessionWrite());
  { const std::string s = latestSessionText(0); CHECK(hasLine(s, "lp.track.7.loopmode=1")); CHECK(hasLine(s, "lp.bpm=118")); }
  loopsOff(); CHECK(!gen().anyLoop());                                   // off again, so the restored loop track is a false → true edge
  setTempo(120); advanceMs(1100); drainCmds();
  sets().restoreSession();
  runBlocks(1); CHECK(sets().sessionBusy()); CHECK(sets().busy());      // the restore owns the card
  bool saw125 = false; int guard = 0;
  while (sets().sessionBusy() && guard++ < 8000) { iterate(); if (strcmp(u.statusText(), kLoopTempoStatus) == 0) saw125 = true; }
  CHECK(!sets().sessionBusy());
  std::printf("  restore: loopEnabled(7)=%d anyLoop=%d bpm=%d saw125=%d\n", (int)gen().loopEnabled(7), (int)gen().anyLoop(), (int)eng().status().bpm, (int)saw125);
  CHECK(gen().loopEnabled(7)); CHECK(gen().anyLoop());                  // loop mode came back with the session ...
  CHECK(!saw125); CHECK_EQ((int)eng().status().bpm, 118);               // ... and the file's tempo stands: no pull to 125
  shiftDown(); tap(3, 0); shiftUp(); loopKey(); CHECK(gen().loopEnabled(3));   // the edge was consumed by the restore: a second loop track pulls nothing
  CHECK(waitAnyLoop()); advanceMs(1); CHECK(strcmp(u.statusText(), "LOOP ON S1 BASS") == 0);
  runBlocks(4); CHECK_EQ((int)eng().status().bpm, 118);
  loopsOff(); setTempo(120); advanceMs(1300); drainCmds();
}

// ---------------------------------------------------------------------------------------------- SPEC §9.22 / §9.23 (2026-10-16 wave)

#ifdef LP_UI_NO_RECORDER
// run_ui.sh compiles the tests with this when src/rec/recorder.cpp is absent or does not compile: an in-memory Recorder
// with the header's contract (sessions numbered from 0, events counted, a render that ticks the Ui on its own clock and
// finishes after kStandinBlocks blocks, a cancel that drops the WAV mark). The file checks of the recorder tests are
// skipped in this build; the real recorder.cpp is the one the firmware links.
namespace lp {
namespace {
constexpr int kStandinBlocks = 400;
uint32_t g_standinLen[Recorder::kMaxSessions];
}
void Recorder::init(IFileSystem* fs, Engine* eng, Generator* gen, Sets* sets, Ui* ui) { fs_ = fs; eng_ = eng; gen_ = gen; sets_ = sets; ui_ = ui; recording_ = rendering_ = false; count_ = 0; recMask_ = wavMask_ = 0; sessions_ = 0; cur_ = -1; name_[0] = 0; }
int Recorder::sessionCount() { int n = 0; for (int i = 0; i < kMaxSessions; i++) if ((recMask_ >> i) & 1) n++; sessions_ = (int8_t)n; return n; }
int Recorder::index(int i) { for (int n = 0; n < kMaxSessions; n++) if ((recMask_ >> n) & 1) { if (i-- == 0) return n; } return -1; }
const char* Recorder::sessionName(int i) { const int n = index(i); if (n < 0) name_[0] = 0; else snprintf(name_, sizeof name_, "rec_%03d", n); return name_; }
bool Recorder::sessionInfo(int i, RecHeader& out) { const int n = index(i); if (n < 0) return false; memset(&out, 0, sizeof out); memcpy(out.magic, "LPREC1\0\0", 8); out.lengthTicks = g_standinLen[n]; out.events = 1; return true; }
bool Recorder::deleteSession(int i) { if (recording_ || rendering_) return false; const int n = index(i); if (n < 0) return false; recMask_ &= ~(1ull << n); wavMask_ &= ~(1ull << n); return true; }
bool Recorder::hasWav(int i) { const int n = index(i); return n >= 0 && ((wavMask_ >> n) & 1) != 0; }
bool Recorder::startRecording() { if (recording_ || rendering_) return false; int n = -1; for (int k = 0; k < kMaxSessions && n < 0; k++) if (!((recMask_ >> k) & 1)) n = k; if (n < 0) return false; recMask_ |= 1ull << n; cur_ = (int8_t)n; count_ = 0; startTick_ = eng_->status().tick; recording_ = true; return true; }
void Recorder::stopRecording() { if (!recording_) return; recording_ = false; g_standinLen[cur_] = eng_->status().tick - startTick_; cur_ = -1; }
void Recorder::onInput(const RecEvent& e) { (void)e; if (recording_) count_++; }
void Recorder::step() {}
bool Recorder::startRender(int i) { if (recording_ || rendering_) return false; const int n = index(i); if (n < 0) return false; cur_ = (int8_t)n; replayed_ = 0; total_ = kStandinBlocks; rendering_ = true; clock0_ = eng_->status().sampleClock; ms0_ = 100000; return true; }
uint32_t Recorder::renderNowMs() const { return ms0_ + (uint32_t)((uint64_t)(eng_->status().sampleClock - clock0_) * 1000u / kSampleRate); }
int Recorder::renderPercent() const { return rendering_ ? (int)(replayed_ * 100 / total_) : 0; }
void Recorder::cancelRender() { if (!rendering_) return; rendering_ = false; wavMask_ &= ~(1ull << cur_); cur_ = -1; }
bool Recorder::renderStep(int16_t* block) { if (!rendering_) return false; eng_->render(block, kBlock); replayed_++; if (ui_) ui_->tick(renderNowMs()); if (replayed_ >= total_) { rendering_ = false; wavMask_ |= 1ull << cur_; cur_ = -1; return false; } return true; }
}
#endif

// The recorder's folders (/lp/rec, /lp/wav) live in a temporary card directory, as in tests/run_rec.sh: the sessions a user
// pulled from the device into sdcard/lp/rec (tools/card_pull.py) are never touched, and the cleanup test removes the whole
// temporary card. Everything else the recorder reaches (nothing today) and every other module use the real card image (sd()).
struct RecRouteFs : IFileSystem {
  PosixFs tmp;
  explicit RecRouteFs(const char* t) : tmp(t) {}
  static bool routed(const char* p) { return p && (strncmp(p, Recorder::kRecDir, strlen(Recorder::kRecDir)) == 0 || strncmp(p, Recorder::kWavDir, strlen(Recorder::kWavDir)) == 0); }
  IFileSystem& of(const char* p) { return routed(p) ? static_cast<IFileSystem&>(tmp) : static_cast<IFileSystem&>(sd()); }
  bool exists(const char* p) override { return of(p).exists(p); }
  bool isDir(const char* p) override { return of(p).isDir(p); }
  IFile* open(const char* p, const char* mode = "r") override { return of(p).open(p, mode); }
  bool mkdir(const char* p) override { return of(p).mkdir(p); }
  bool remove(const char* p) override { return of(p).remove(p); }
  bool rename(const char* a, const char* b) override { return routed(a) == routed(b) && of(a).rename(a, b); }
  bool walk(const char* p, DirEntryFn fn, void* user) override { return of(p).walk(p, fn, user); }
  int openCount() const override { return sd().openCount() + tmp.openCount(); }
};
static std::string g_recTmpRoot;                     // the temporary card root; "" until the first recorder test
static bool g_realRecDirExisted = false, g_realWavDirExisted = false; static size_t g_realRecFiles = 0, g_realWavFiles = 0;   // the real card's /lp/rec and /lp/wav as found: the cleanup test checks they are still so
static size_t countCardFiles(const char* dir) {
  std::vector<std::string> names;
  sd().walk(dir, [](const char* name, bool isDir, uint32_t, void* user) -> bool { if (!isDir) ((std::vector<std::string>*)user)->push_back(name); return true; }, &names);
  return names.size();
}
static RecRouteFs& recFs() {
  static RecRouteFs* f = nullptr;
  if (!f) {
    g_realRecDirExisted = cardExists(Recorder::kRecDir); g_realWavDirExisted = cardExists(Recorder::kWavDir);
    g_realRecFiles = countCardFiles(Recorder::kRecDir); g_realWavFiles = countCardFiles(Recorder::kWavDir);
    char tpl[256]; const char* td = getenv("TMPDIR"); snprintf(tpl, sizeof tpl, "%s/lp_ui_test_XXXXXX", td && *td ? td : "/tmp");
    char* d = mkdtemp(tpl); if (!d) { perror("mkdtemp"); exit(2); }
    g_recTmpRoot = d; ::mkdir((g_recTmpRoot + "/lp").c_str(), 0755);
    f = new RecRouteFs(g_recTmpRoot.c_str());
    std::printf("  recorder card: %s (real /lp/rec: %s, %zu file(s); /lp/wav: %s, %zu file(s))\n", g_recTmpRoot.c_str(),
                g_realRecDirExisted ? "exists" : "absent", g_realRecFiles, g_realWavDirExisted ? "exists" : "absent", g_realWavFiles);
  }
  return *f;
}
// The recorder's replay clock (Recorder::renderNowMs) continues from the platform's millis through g_microsHook (the sketch's
// micros()); the harness hands it its own clock so the Ui runs on one time axis across a render.
static Recorder& rec() { static Recorder* r = nullptr; if (!r) { g_microsHook = []() -> uint32_t { return g_now * 1000u; }; r = new Recorder(); r->init(&recFs(), &eng(), &gen(), &sets(), &ui()); } return *r; }
static void writeCardText(const std::string& card, const std::string& text) { FILE* f = fopen((sdRoot() + card).c_str(), "wb"); if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); } }
// A session / WAV file of the recorder's (temporary) card.
static bool recFile(const char* name, const char* ext) { struct stat st; return stat((g_recTmpRoot + (hasExt(ext, ".wav") ? Recorder::kWavDir : Recorder::kRecDir) + "/" + name + ext).c_str(), &st) == 0; }
// Back on the main page: overlays expire (the param / note page timers), modal pages take `.
static void mainPage() {
  for (int i = 0; i < 4 && ui().page() != PgMain; i++) { const Page p = ui().page(); if (p == PgParam || p == PgNote || p == PgSplash) advanceMs(kParamPageMs + 50); else tap(0, 0); }
  CHECK_EQ((int)ui().page(), (int)PgMain);
}

// The pure helpers of the wave: names, colours, the knob map, the two-row knob geometry.
TEST(pure_helpers_9_22) {
  CHECK(strcmp(pageParamName((PageParam)PSat), "SAT") == 0); CHECK(strcmp(pageParamName((PageParam)PAccent), "ACCENT") == 0); CHECK(strcmp(pageParamName((PageParam)PChorus), "CHORUS") == 0);
  CHECK(strcmp(pageParamName(PPGenre), "GENRE") == 0); CHECK(strcmp(pageParamName(PPRampBars), "RAMP") == 0); CHECK(strcmp(pageParamName((PageParam)PWave), "WAVE") == 0);
  CHECK_EQ((int)paramColor(PSat), (int)kSatColor); CHECK_EQ((int)paramColor(PAccent), (int)kAccentColor); CHECK_EQ((int)paramColor(PChorus), (int)kChorusColor);
  CHECK_EQ((int)kSatColor, (int)rgb(0xFF, 0x45, 0x00)); CHECK_EQ((int)kAccentColor, (int)rgb(0xC6, 0xFF, 0x00)); CHECK_EQ((int)kChorusColor, (int)rgb(0x4F, 0xC3, 0xFF));
  CHECK_EQ((int)pageParamColor(PPGenre), (int)rgb(0xFF, 0x4F, 0xA3)); CHECK_EQ((int)pageParamColor(PPRampBars), (int)rgb(0x1D, 0xE9, 0xB6));
  CHECK_EQ((int)Ui::synthKnobParam(PTempo), (int)PSat); CHECK_EQ((int)Ui::synthKnobParam(PKey), (int)PAccent); CHECK_EQ((int)Ui::synthKnobParam(PMaster), (int)ParamCount);
  for (int i = 0; i < kSynthKnobs; i++) CHECK_EQ((int)strlen(Ui::knobLabel(i)), 3);
  CHECK(strcmp(Ui::knobLabel(0), "CUT") == 0); CHECK(strcmp(Ui::knobLabel(6), "SAT") == 0); CHECK(strcmp(Ui::knobLabel(7), "ACC") == 0); CHECK(strcmp(Ui::knobLabel(8), "CHO") == 0);
  char b[8]; Ui::knobValueText(PSat, 0.35f, b, sizeof b); CHECK(strcmp(b, "35%") == 0); Ui::knobValueText(PChorus, 1.0f, b, sizeof b); CHECK(strcmp(b, "100%") == 0); Ui::knobValueText(PAccent, 0.5f, b, sizeof b); CHECK(strcmp(b, "50%") == 0);
  CHECK(kKnobX0 - kKnobPitch / 2 >= 0); CHECK(kKnobX0 + (kKnobCols - 1) * kKnobPitch + kKnobPitch / 2 <= kScreenW);   // both knob rows stay on the 240 px screen ...
  CHECK(kKnobX0Row2 - kKnobPitch / 2 >= 0); CHECK(kKnobX0Row2 + (kSynthKnobs - kKnobCols - 1) * kKnobPitch + kKnobPitch / 2 <= kScreenW);
  CHECK(kKnobCy - kKnobR >= 13); CHECK(kKnobCy + kKnobRowPitch + kKnobValueDy + 8 <= kStatusY);                      // ... below the header and above the status line
  CHECK(6 * 6 <= kKnobPitch);                                                                                           // a 6-character value fits under its knob
}

// SHIFT + gyro key (8,2): PLAY ALL / STOP ALL for both sets, quantised by the engine; a pending stop counts as stopped
// (a second press un-queues it); plain j keeps the gyro hold toggle.
TEST(shift_gyro_plays_and_stops_all) {
  Ui& u = ui(); settleJobs(); loopsOff(); mainPage(); drainCmds();
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  runBlocks(1); drainCmds();
  for (int t = 0; t < kTracks; t++) { const PlayState s = eng().status().track[t].state; CHECK(s != PSPlaying && s != PSQueued); }
  const bool gyro0 = u.gyroOn();
  shiftDown(); tap(8, 2); shiftUp();
  std::vector<Cmd> c = peekCmds();
  CHECK_EQ(countCmd(c, CmdPlaySet), 2); CHECK_EQ(countCmd(c, CmdStopSet), 0);
  bool s0 = false, s1 = false; for (const Cmd& k : c) if (k.type == CmdPlaySet) { if (k.a == 0) s0 = true; if (k.a == 1) s1 = true; }
  CHECK(s0 && s1);
  CHECK(strcmp(u.statusText(), "PLAY ALL") == 0);
  advanceMs(600); CHECK_EQ((int)u.gyroOn(), (int)gyro0);              // SHIFT + j never arms the gyro hold
  drainCmds();
  loadTestPattern(Beat, 36); push(CmdTrackStartNow, Beat); runBlocks(1); drainCmds();
  CHECK_EQ((int)eng().status().track[Beat].state, (int)PSPlaying);
  shiftDown(); tap(8, 2); shiftUp(); c = peekCmds();
  CHECK_EQ(countCmd(c, CmdStopSet), 2); CHECK_EQ(countCmd(c, CmdPlaySet), 0); CHECK(strcmp(u.statusText(), "STOP ALL") == 0);
  runBlocks(1); CHECK_EQ((int)eng().status().track[Beat].state, (int)PSStopQueued);   // the stop lands on the bar line (SPEC §9.20)
  drainCmds();
  shiftDown(); tap(8, 2); shiftUp(); c = peekCmds();                   // before the bar: PLAY ALL again, the pending stop is dropped
  CHECK_EQ(countCmd(c, CmdPlaySet), 2); CHECK(findCmd(c, CmdCancelPending, Beat) != nullptr); CHECK(strcmp(u.statusText(), "PLAY ALL") == 0);
  runBlocks(1); CHECK_EQ((int)eng().status().track[Beat].state, (int)PSPlaying);
  drainCmds(); push(CmdTrackStopNow, Beat); runBlocks(1); drainCmds();
  press(8, 2); advanceMs(450); release(8, 2); CHECK_EQ((int)u.gyroOn(), (int)!gyro0);   // plain j: the 400 ms hold toggle (SPEC §3)
  press(8, 2); advanceMs(450); release(8, 2); CHECK_EQ((int)u.gyroOn(), (int)gyro0);
  advanceMs(700); drainCmds();
}

// SYNTH page knobs 7..9 (SPEC §9.22): tempo± → SAT, key± → ACC, gyro / edit keys → CHO, with the step / hold-ramp / freeze
// / SHIFT-ramp semantics of the other knobs; nine labels in two rows; the edit key no longer exits.
TEST(synth_page_sat_acc_cho_knobs) {
  Ui& u = ui(); mainPage(); drainCmds();
  const int root0 = gen().key().root, bpm0 = eng().status().bpm;
  shiftDown(); tap(9, 0); shiftUp(); CHECK_EQ((int)u.page(), (int)PgSynth); CHECK_EQ(u.synthTrack(), (int)Lead);
  advanceMs(40); draw();
  for (const char* l : {"CUT", "RES", "ENV", "DEC", "DRV", "WAV", "SAT", "ACC", "CHO"}) CHECK(g_gfx.drawn(l) != nullptr);
  CHECK(g_gfx.lines >= 9 * 19);                                          // nine arcs of 18 segments + a pointer each
  const TextOp* sat = g_gfx.drawn("SAT"); const TextOp* cut = g_gfx.drawn("CUT"); const TextOp* wav = g_gfx.drawn("WAV");
  CHECK(sat && cut && wav); if (sat && cut && wav) { CHECK(sat->y > cut->y); CHECK_EQ(sat->y, wav->y); CHECK(cut->y < wav->y); }   // two rows: CUT … DRV, then WAV SAT ACC CHO
  for (const TextOp& t : g_gfx.textOps) { CHECK(t.x >= 0 && t.x + t.w <= kScreenW); CHECK(t.y + t.h <= kScreenH); }
  const float sat0 = eng().params(Lead).sat, acc0 = eng().params(Lead).accent, cho0 = eng().params(Lead).chorus;
  press(6, 2);                                                           // tempo+ → SAT: a step and a hold-ramp towards 1, no tempo change
  std::vector<Cmd> c = peekCmds(); const Cmd* p = findCmd(c, CmdSetParam, Lead);
  CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PSat); CHECK_NEAR(p->f, sat0 + 0.05f, 1e-5); }
  const Cmd* r = findCmd(c, CmdRamp, Lead); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PSat); CHECK_NEAR(r->f, 1.0f, 1e-6); CHECK(r->i > 0); }
  CHECK_EQ(countCmd(c, CmdSetTempo), 0);
  key(6, 2, KeyEvent::Repeat, 400); CHECK_EQ((int)peekCmds().size(), 2);
  release(6, 2, 450); c = peekCmds(); const Cmd* fz = findCmd(c, CmdFreezeRamp, Lead); CHECK(fz != nullptr); if (fz) CHECK_EQ((int)fz->a, (int)PSat);
  runBlocks(1); drainCmds(); CHECK_EQ((int)eng().status().bpm, bpm0);
  tap(7, 3);                                                             // key− → ACC down (clamped at 0: library presets carry accent 0), the key is untouched
  const float acc1 = std::max(0.0f, roundf((acc0 - 0.05f) / 0.05f) * 0.05f);
  c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PAccent); CHECK_NEAR(p->f, acc1, 1e-5); }
  std::printf("  Lead accent %.2f → %.2f, sat %.2f, chorus %.2f\n", acc0, p ? p->f : -1.0f, sat0, cho0);
  CHECK_EQ((int)gen().key().root, root0); runBlocks(1); drainCmds();
  tap(8, 2);                                                             // gyro key → CHO up, no gyro command
  c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PChorus); CHECK_NEAR(p->f, cho0 + 0.05f, 1e-5); }
  CHECK_EQ(countCmd(c, CmdGyro), 0); runBlocks(1); drainCmds();
  advanceMs(500); CHECK_EQ((int)u.page(), (int)PgSynth);                 // and no gyro hold toggle either
  tap(8, 3);                                                             // edit key → CHO down, the page stays
  c = peekCmds(); p = findCmd(c, CmdSetParam, Lead); CHECK(p != nullptr); if (p) { CHECK_EQ((int)p->a, (int)PChorus); CHECK_NEAR(p->f, cho0, 1e-5); }
  CHECK_EQ((int)u.page(), (int)PgSynth); runBlocks(1); drainCmds();
  advanceMs(40); draw(); CHECK_EQ(g_gfx.screens, 0);                      // only the changed knobs repaint
  press(7, 2); c = peekCmds(); CHECK(findCmd(c, CmdRamp, Lead) != nullptr); runBlocks(1); drainCmds();   // ACC hold-ramp (bit 16: no alias with PVolume)
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);                       // ` exits and freezes the held ramp — the ACC one only
  c = peekCmds(); CHECK_EQ(countCmd(c, CmdFreezeRamp), 1); fz = findCmd(c, CmdFreezeRamp, Lead); CHECK(fz != nullptr); if (fz) CHECK_EQ((int)fz->a, (int)PAccent);
  release(7, 2); CHECK_EQ((int)peekCmds().size(), 1); drainCmds();      // the release on the main page (key+) sends nothing
  shiftDown(); tap(9, 0); CHECK_EQ((int)u.page(), (int)PgSynth);
  press(6, 2); c = peekCmds(); r = findCmd(c, CmdRamp, Lead); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->a, (int)PSat); CHECK_EQ(r->i, 4 * kTicksPerBar); }   // SHIFT + tempo+: a SHIFT ramp of SAT
  CHECK_EQ(countCmd(c, CmdSetParam), 0);
  release(6, 2); shiftUp(); c = peekCmds(); CHECK_EQ(countCmd(c, CmdCancelRamps), 1); drainCmds();
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
}

static int g_usbHookCalls = 0;
static void usbHook() { g_usbHookCalls++; }

// SHIFT + gen pattern (12,2) held 1 s: "USB DRIVE..." then the hook (or "NO DRIVE FIRMWARE"); a short press is gen pattern.
TEST(usb_drive_hold_and_short_press) {
  Ui& u = ui(); settleJobs(); mainPage(); drainCmds();
  u.setUsbDriveHook(nullptr);
  shiftDown(); press(12, 2); CHECK_EQ(u.usbDriveState(), 1); CHECK(!gen().busy());
  advanceMs(600); CHECK_EQ(u.usbDriveState(), 1); CHECK(!gen().busy()); CHECK(!u.statusVisible());
  advanceMs(kUsbDriveHoldMs - 600 + 5); CHECK_EQ(u.usbDriveState(), 2); CHECK(strcmp(u.statusText(), "USB DRIVE...") == 0);
  advanceMs(kUsbDriveStatusMs + 5); CHECK_EQ(u.usbDriveState(), 3); CHECK(strcmp(u.statusText(), "NO DRIVE FIRMWARE") == 0);
  release(12, 2); shiftUp(); CHECK_EQ(u.usbDriveState(), 0); CHECK(!gen().busy());   // a long hold is never gen pattern
  u.setUsbDriveHook(usbHook);
  shiftDown(); press(12, 2); advanceMs(kUsbDriveHoldMs + 5); CHECK_EQ(g_usbHookCalls, 0); CHECK(strcmp(u.statusText(), "USB DRIVE...") == 0);   // the status first ...
  advanceMs(kUsbDriveStatusMs + 5); CHECK_EQ(g_usbHookCalls, 1);                                                                             // ... the hook kUsbDriveStatusMs later
  advanceMs(500); CHECK_EQ(g_usbHookCalls, 1); release(12, 2); shiftUp(); CHECK_EQ(u.usbDriveState(), 0);
  shiftDown(); press(12, 2); advanceMs(kUsbDriveHoldMs + 5); release(12, 2); shiftUp();   // released during the status window: the hook still fires once
  advanceMs(kUsbDriveStatusMs + 5); CHECK_EQ(g_usbHookCalls, 2); CHECK_EQ(u.usbDriveState(), 0);
  shiftDown(); tap(1, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), (int)Lead);
  shiftDown(); press(12, 2); advanceMs(200); release(12, 2); shiftUp();   // a short SHIFT press: gen pattern of the selected track
  CHECK(gen().busy()); CHECK(strncmp(u.progressText(), "GEN PAT", 7) == 0); CHECK_EQ(g_usbHookCalls, 2);
  settleJobs(); drainCmds();
  tap(12, 2); CHECK(gen().busy());                                       // plain gen pattern as before
  settleJobs(); drainCmds(); u.setUsbDriveHook(nullptr);
}

// ---------------------------------------------------------------------------------------------- theory generator (SPEC §9.24)

// SHIFT + load 1 (11,2) / load 2 (11,3) = Generator::generateTheorySet(0 / 1): the job reports as "THEORY S1 n/8" the way
// GENERATE reports "GEN S1 n/8", the patterns that will be replaced drop their edited flag, the result message follows
// ("GEN DONE" / the generator's note), the squares blink as under GENERATE (a track queued meanwhile alternates its outline
// every kBlinkHalfMs; a loop track's pick blink is requestGen's shared path), and the usual refusals apply: SETS BUSY while a
// load owns the card, no job queued. The plain load keys keep loading. (Under LP_UI_THEORY_STANDIN the job is a set generate
// with the theory label: the Ui paths are the same.)
TEST(shift_load_generates_a_theory_set) {
  Ui& u = ui(); settleJobs(); mainPage(); runToTick(nextBarTick()); runBlocks(2); drainCmds();   // past a bar line: the earlier tests' queued starts / stops have landed
#ifdef LP_UI_THEORY_STANDIN
  std::printf("  theory job: in-file stand-in (a set generate under the theory label)\n");
#else
  std::printf("  theory job: Generator::generateTheorySet of src/gen/generator.cpp\n");
#endif
  gen().info(7).edited = 1; gen().info(2).edited = 1;                    // set 1 Beat and Pad hold "edits" that the theory set replaces
  shiftDown(); tap(11, 2); shiftUp();                                    // SHIFT + load 1 = THEORY S1
  CHECK(gen().busy()); CHECK(!sets().busy());
  advanceMs(1); CHECK(strcmp(u.statusText(), "THEORY S1 0/8") == 0);
  CHECK_EQ((int)gen().info(7).edited, 0); CHECK_EQ((int)gen().info(2).edited, 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("THEORY S1 0/8"));
  const int sel = u.selectedTrack();
  bool blinkChecked = false;
  const PlayState s0 = eng().status().track[2].state;                    // set 1 Pad (a pattern from the generate tests): stopped or playing
  if (s0 == PSStopped || s0 == PSPlaying) {
    tap(2, 0); iterate();                                                // queue its start (or its stop) under the job
    const PlayState s1 = eng().status().track[2].state;
    if (s1 == PSQueued || s1 == PSStopQueued) {
      g_now = (g_now / kBlinkHalfMs + 1) * kBlinkHalfMs + 5; ui().tick(g_now); draw();
      advanceMs(kBlinkHalfMs); draw(); const Color b = cellPaint(2);
      advanceMs(kBlinkHalfMs); draw(); const Color c = cellPaint(2);
      CHECK(b != 0); CHECK(c != 0); CHECK(b != c);                       // the square alternates every kBlinkHalfMs (outline colour <-> dim, or a fill <-> black)
      blinkChecked = true;
      tap(2, 0); iterate();                                              // the second tap before the bar cancels the pending change (SPEC §9.4)
      CHECK_EQ((int)eng().status().track[2].state, (int)s0);
    } else if (s1 != s0) { tap(2, 0); runToTick(nextBarTick()); runBlocks(2); }   // the bar line fell inside that block: toggle back and let it land
  }
  std::printf("  queued-square blink under the job: %s (Pad of set 1 was %s)\n", blinkChecked ? "checked" : "skipped", s0 == PSPlaying ? "playing" : s0 == PSStopped ? "stopped" : "neither stopped nor playing");
  shiftDown(); tap(sel % kTracksPerSet, sel / kTracksPerSet); shiftUp(); CHECK_EQ(u.selectedTrack(), sel);   // the tap selected the Pad: select the previous track back
  int steps = 0; bool sawMid = false, sawGenerating = false;
  while (gen().busy() && steps++ < 86 * 40) {
    iterate();
    if (eng().status().generating[0]) sawGenerating = true;
    if (strncmp(u.statusText(), "THEORY S1 ", 10) == 0 && u.statusText()[10] >= '1' && u.statusText()[10] <= '7') sawMid = true;
  }
  CHECK(!gen().busy()); CHECK(sawMid); CHECK(sawGenerating);
  advanceMs(1);
  std::printf("  after the theory job: \"%s\" (%d steps, note \"%s\")\n", u.statusText(), steps, gen().lastNote());
  CHECK(gen().lastError()[0] == 0);
  CHECK(strcmp(u.statusText(), "GEN DONE") == 0 || (gen().lastNote()[0] && strcmp(u.statusText(), gen().lastNote()) == 0));
  for (int t = 0; t < kTracksPerSet; t++) CHECK(eng().status().track[t].state != PSEmpty);
  advanceMs(1100); CHECK(!u.statusVisible());
  shiftDown(); tap(11, 3); shiftUp(); CHECK(gen().busy());               // SHIFT + load 2 = THEORY S2
  advanceMs(1); CHECK(strcmp(u.statusText(), "THEORY S2 0/8") == 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("THEORY S2 0/8"));
  settleJobs(); CHECK(!gen().busy()); advanceMs(1100); drainCmds();
  tap(11, 2); CHECK(sets().busy());                                      // the plain load S1 still loads (its directory scan is in flight) ...
  shiftDown(); tap(11, 3); shiftUp(); CHECK(!gen().busy());              // ... and the theory key is refused meanwhile: SETS BUSY, no job
  advanceMs(1); CHECK(strcmp(u.statusText(), "SETS BUSY") == 0);
  settleJobs(); advanceMs(1100);
  tap(13, 2); CHECK(gen().busy());                                       // GENERATE S1 runs ...
  shiftDown(); tap(11, 2); shiftUp();                                    // ... SHIFT + load 1 meanwhile: the generator queues or refuses it, the page and selection stay
  CHECK_EQ((int)u.page(), (int)PgMain); CHECK_EQ(u.selectedTrack(), sel);
  settleJobs(); CHECK(!gen().busy()); CHECK(!sets().busy());
  advanceMs(1100); CHECK(!u.statusVisible()); drainCmds();
}

// ---- preset manager fixture: /lp/packs/Testpack/presets with 20 presets (Testpack has no presets folder of its own) ----
static const std::string kTpPresets = "/lp/packs/Testpack/presets";
static std::vector<std::string> g_tpCreated; static bool g_tpDirExisted = false, g_tpAssetsDirExisted = false; static int g_homeGenre = -1;
static std::string tpName(int n, const char* key) { char b[64]; snprintf(b, sizeof b, "Testpack_%s_%03d.lpset", key, n); return b; }
static std::string tpPath(int n, const char* key = "Amin") { return kTpPresets + "/" + tpName(n, key); }
// The files of the fixture folder (or its assets/ subfolder), sorted: a live save inside a recording adds some, and whatever
// it added is tracked in g_tpCreated so the cleanup removes it even when the folder was the user's.
static std::vector<std::string> tpFiles(const char* sub = "") {
  std::vector<std::string> names;
  sd().walk((kTpPresets + sub).c_str(), [](const char* name, bool isDir, uint32_t, void* user) -> bool { if (!isDir) ((std::vector<std::string>*)user)->push_back(name); return true; }, &names);
  std::sort(names.begin(), names.end()); return names;
}
static size_t tpPresetCount() { size_t n = 0; for (const std::string& s : tpFiles()) if (hasExt(s.c_str(), ".lpset")) n++; return n; }
static void trackNewTpFiles(const std::vector<std::string>& before, const std::vector<std::string>& assetsBefore) {
  for (const std::string& n : tpFiles()) if (!std::binary_search(before.begin(), before.end(), n)) g_tpCreated.push_back(kTpPresets + "/" + n);
  for (const std::string& n : tpFiles("/assets")) if (!std::binary_search(assetsBefore.begin(), assetsBefore.end(), n)) g_tpCreated.push_back(kTpPresets + "/assets/" + n);
}
// 020 … 001, newest first: every fourth one in E minor, the rest in A minor; 003 and 020 carry a label.
static void makeTestpackPresets() {
  g_tpDirExisted = cardExists(kTpPresets);
  if (!g_tpDirExisted) sd().mkdir(kTpPresets.c_str());
  for (int n = 1; n <= 20; n++) {
    const bool e = n % 4 == 0;
    std::string text = std::string("schemaVersion=1\nsetIndex=0\nkeyRoot=") + (e ? "4" : "9") + "\nkeyName=" + (e ? "Emin" : "Amin") + "\ngenre=Testpack\n";
    if (n == 3) text += "lp.label=Three\n";
    if (n == 20) text += "lp.label=Latest\n";
    text += "lp.bpm=120\n";
    writeCardText(tpPath(n, e ? "Emin" : "Amin"), text); g_tpCreated.push_back(tpPath(n, e ? "Emin" : "Amin"));
  }
}
static void enterTestpack() { settleJobs(); g_homeGenre = gen().genreIndex(); CHECK(gen().setGenreByName("Testpack")); }
static void leaveTestpack() { settleJobs(); gen().setGenre(g_homeGenre); }
// Iterates until the page's listing is complete and the Ui has taken it (one more tick), or gives up.
static bool waitPresetList(int maxBlocks = 3000) {
  for (int i = 0; i < maxBlocks; i++) { if (sets().presetListReady() && !sets().busy() && ui().presetPageFirst() >= 0) { iterate(); return sets().presetListReady(); } iterate(); }
  return false;
}
// Iterates until the Sets machine is through, then ticks the Ui once WITHOUT rendering (a command the completion pushes,
// e.g. the AUTO CmdPlaySet, must still be in the queue for the test to see).
static void waitSets(int maxBlocks = 4000) { for (int i = 0; i < maxBlocks && sets().busy(); i++) iterate(); g_now += kBlockMs; ui().tick(g_now); }

// SHIFT + save 2 (10,3): the preset manager (SPEC §9.22 as remapped by §9.24) — newest first with labels, the cursor with hold
// repeats and paging through Sets' 16-row pages, the key filter, AUTO, ent = Sets::beginLoadNamed into the TARGET SET (+
// CmdPlaySet of that set with AUTO), the target toggled by the track rows (y0 → SET 1, y1 → SET 2, header "-> SET n"), exits.
TEST(preset_manager_lists_filters_and_loads) {
  Ui& u = ui(); mainPage(); drainCmds();
  enterTestpack(); makeTestpackPresets();
  shiftDown(); tap(10, 3); shiftUp();
  CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 0); CHECK_EQ(u.presetCursor(), 0); CHECK_EQ(u.presetFilterKey(), -1);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 20); CHECK_EQ(u.presetPageFirst(), 0);
  advanceMs(40); draw();
  CHECK(g_gfx.drawn("-> SET 1  ALL  20") != nullptr);
  CHECK(g_gfx.sawText("020 E  Latest")); CHECK(g_gfx.sawText("019 A  Testpack_Amin_019")); CHECK(g_gfx.sawText("016 E  Testpack_Emin_016"));   // newest first, labels instead of file names
  CHECK(g_gfx.sawText("013 A")); CHECK(!g_gfx.sawText("012"));         // eight visible rows: 020 … 013
  const TextOp* top = g_gfx.drawn("020 E  Latest"); CHECK(top != nullptr); if (top) CHECK_EQ((int)top->c, (int)kBlack);   // the cursor row is inverted
  for (int i = 0; i < 7; i++) tap(0, 3);
  CHECK_EQ(u.presetCursor(), 7);
  tap(0, 3); CHECK_EQ(u.presetCursor(), 8);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("012 E")); CHECK(!g_gfx.sawText("020"));   // the window follows the cursor
  for (int i = 0; i < 8; i++) key(0, 3, KeyEvent::Repeat, 400 + i * 80);                 // hold repeats
  CHECK_EQ(u.presetCursor(), 16);                                                          // past Sets' first page: the next one is requested
  CHECK(waitPresetList()); CHECK_EQ(u.presetPageFirst(), 8);                               // from the 8-row boundary under the window (top = 9)
  advanceMs(40); draw(); CHECK(g_gfx.sawText("004 E  Testpack_Emin_004")); CHECK(g_gfx.sawText("011 A")); CHECK(!g_gfx.sawText("003"));   // rows 011 … 004
  for (int i = 0; i < 10; i++) tap(0, 3); CHECK_EQ(u.presetCursor(), 19);                 // clamped at the end
  CHECK(waitPresetList()); advanceMs(40); draw(); CHECK(g_gfx.sawText("001 A  Testpack_Amin_001")); CHECK(g_gfx.sawText("003 A  Three"));
  for (int i = 0; i < 30; i++) { tap(0, 2); if (!sets().presetListReady()) waitPresetList(); }
  CHECK_EQ(u.presetCursor(), 0); CHECK(waitPresetList()); CHECK_EQ(u.presetPageFirst(), 0);   // and back at the top, paged back
  CHECK_EQ((int)gen().key().root, 9);
  tap(7, 2); CHECK_EQ(u.presetFilterKey(), 9); CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 15);   // key+: ALL → A minor (15 of 20)
  advanceMs(40); draw(); CHECK(g_gfx.sawText("-> SET 1  A MIN  15")); CHECK(!g_gfx.sawText(" E  ")); CHECK(g_gfx.sawText("011 A"));
  tap(7, 3); CHECK_EQ(u.presetFilterKey(), -1); CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 20);  // key−: back to ALL
  tap(9, 3); CHECK(u.presetAuto()); advanceMs(40); draw(); CHECK(g_gfx.drawn("-> SET 1  ALL  20  AUTO") != nullptr);
  settleJobs(); drainCmds();
  tap(13, 2);                                                            // ent: load 020 into set 1
  CHECK(sets().busy());
  waitSets();
  // The Ui latched the load's ok() / lastName() in the tick it completed and started the re-listing in the next one (which
  // resets Sets::ok() while it runs), so the proof of the load is the status it reported, not Sets::ok() read now.
  CHECK(strcmp(sets().lastName(), tpName(20, "Emin").c_str()) == 0);
  std::printf("  loaded %s: status \"%s\" (listing in flight: %d)\n", sets().lastName(), u.statusText(), (int)sets().busy());
  std::vector<Cmd> c = peekCmds(); const Cmd* ps = findCmd(c, CmdPlaySet); CHECK(ps != nullptr); if (ps) CHECK_EQ((int)ps->a, 0);   // AUTO: the set starts at the bar
  CHECK(strncmp(u.statusText(), "LOADED", 6) == 0); CHECK(strstr(u.statusText(), tpName(20, "Emin").c_str()) != nullptr);
  drainCmds(); CHECK_EQ((int)u.page(), (int)PgPresets);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 20);      // the rows come back (the load used Sets' page buffer)
  tap(9, 3); CHECK(!u.presetAuto());
  tap(13, 2); waitSets(); drainCmds(); CHECK_EQ(countCmd(peekCmds(), CmdPlaySet), 0);   // without AUTO: no start
  CHECK(waitPresetList());
  // TARGET SET: any key of track row y1 → SET 2 (SHIFT or not), of row y0 → SET 1; the list, cursor and filter stay
  tap(0, 1); CHECK_EQ(u.presetSet(), 1); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetCursor(), 0); CHECK_EQ(u.presetFilterKey(), -1);
  CHECK(sets().presetListReady()); CHECK_EQ(sets().presetListTotal(), 20);   // no re-listing: the rows are the genre's
  advanceMs(40); draw(); CHECK(g_gfx.drawn("-> SET 2  ALL  20") != nullptr); CHECK(g_gfx.sawText("020 E  Latest"));
  const TextOp* hdr2 = g_gfx.drawn("-> SET 2  ALL  20"); if (hdr2) CHECK_EQ((int)hdr2->c, (int)rgb(0x00, 0xE5, 0xFF));   // set 2's colour
  shiftDown(); tap(3, 1); shiftUp(); CHECK_EQ(u.presetSet(), 1);
  tap(7, 0); CHECK_EQ(u.presetSet(), 0); advanceMs(40); draw(); const TextOp* hdr1 = g_gfx.drawn("-> SET 1  ALL  20"); CHECK(hdr1 != nullptr); if (hdr1) CHECK_EQ((int)hdr1->c, (int)kNeonGreen);
  CHECK(peekCmds().empty());                                             // the row keys neither toggle nor select tracks here
  tap(5, 1); CHECK_EQ(u.presetSet(), 1);
  tap(9, 3); CHECK(u.presetAuto());
  // Set 1 (playing since the PLAY ALL test, its Vocal streaming) is stopped at the bar first: Sets' card work waits for
  // Generator::streamsHealthy (SPEC §9.19 §3), and with that stream consumed the re-listing after this load was seen to wait
  // thousands of blocks for the gate (the ring hovering just under kGateLeadSamples) — a generator matter, not the page's.
  push(CmdStopSet, 0, 0); runToTick(nextBarTick()); runBlocks(2); drainCmds();
  CHECK_EQ((int)eng().status().track[6].state, (int)PSStopped);
  tap(13, 2); CHECK(sets().busy()); CHECK(strncmp(u.statusText(), "LOAD S2 ", 8) == 0);   // ent: 020 into SET 2
  waitSets();
  CHECK(strncmp(u.statusText(), "LOADED", 6) == 0);
  c = peekCmds(); ps = findCmd(c, CmdPlaySet); CHECK(ps != nullptr); if (ps) CHECK_EQ((int)ps->a, 1);   // AUTO starts set 2
  drainCmds(); tap(9, 3); CHECK(!u.presetAuto());
  const bool wpl = waitPresetList(); CHECK(wpl);
  if (!wpl) {                                                            // diagnostics for a stalled re-listing (the stream gate, see above)
    std::printf("  DEBUG after set-2 load: busy=%d sessionBusy=%d listReady=%d pageFirst=%d err='%s' genBusy=%d healthy=%d running=%d page=%d status='%s' progress='%s'\n", (int)sets().busy(), (int)sets().sessionBusy(), (int)sets().presetListReady(), u.presetPageFirst(), sets().lastError(), (int)gen().busy(), (int)gen().streamsHealthy(), (int)eng().status().running, (int)u.page(), u.statusText(), u.progressText());
    for (int t = 0; t < kTracks; t++) std::printf("  DEBUG track %d state=%d\n", t, (int)eng().status().track[t].state);
    for (int k = 0; k < kStreams; k++) std::printf("  DEBUG stream %d avail=%d cap=%d active=%d\n", k, (int)eng().streamRing(k).available(), (int)eng().streamRing(k).cap, (int)eng().streamRing(k).active.load());
  }
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgMain);                                 // the page's combo closes it ...
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 1);   // ... and the target is kept across opens, like the filter
  tap(2, 0); CHECK_EQ(u.presetSet(), 0);
  shiftDown(); tap(11, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK(!gen().busy());         // SHIFT + load 1 (the theory key) is not live on the page
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);                                                          // ` closes
  settleJobs(); drainCmds();
  leaveTestpack();
}

// del asks "DELETE? PRESS AGAIN" (2 s), the second press unlinks the preset and the list shrinks; n renames inline:
// legend letters (SHIFT = upper case), digits, '-', del = backspace, ent = Sets::beginSetLabel, ` = cancel.
TEST(preset_manager_delete_and_rename) {
  Ui& u = ui(); mainPage(); drainCmds(); enterTestpack();
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 0);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 20);
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); CHECK(cardExists(tpPath(20, "Emin")));
  advanceMs(kDeleteConfirmMs + 50); CHECK(!u.statusVisible());
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); CHECK(cardExists(tpPath(20, "Emin")));   // after the window: asks again
  advanceMs(300); tap(13, 0);                                                                                      // inside it: deletes
  waitSets();
  CHECK(!cardExists(tpPath(20, "Emin"))); CHECK(strcmp(u.statusText(), "DELETED") == 0);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 19); CHECK_EQ(u.presetCursor(), 0);
  advanceMs(40); draw(); CHECK(!g_gfx.sawText("020")); CHECK(g_gfx.drawn("-> SET 1  ALL  19") != nullptr); CHECK(g_gfx.sawText("019 A  Testpack_Amin_019"));
  tap(8, 3); CHECK(u.presetRenaming()); CHECK(strcmp(u.presetRenameText(), "") == 0);   // n: rename 019
  tap(1, 1); tap(2, 1);                                                  // q w
  shiftDown(); tap(2, 2); shiftUp();                                     // SHIFT + a = A
  tap(3, 3); tap(11, 0); tap(1, 0); tap(10, 0);                          // z - 1 0
  CHECK(strcmp(u.presetRenameText(), "qwAz-10") == 0);
  tap(13, 0); CHECK(strcmp(u.presetRenameText(), "qwAz-1") == 0);        // del = backspace
  tap(0, 3); tap(0, 2); shiftDown(); tap(11, 2); tap(10, 3); shiftUp(); tap(12, 0); tap(13, 1);   // keys without a legend letter (vol±, SHIFT + load 1, SHIFT + save 2, =, \) neither type nor act while typing
  CHECK_EQ(u.presetCursor(), 0); CHECK_EQ(u.presetFilterKey(), -1); CHECK(!u.presetAuto()); CHECK(u.presetRenaming()); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 0); CHECK(!gen().busy());
  CHECK(strcmp(u.presetRenameText(), "qwAz-1") == 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("019 A  qwAz-1_"));
  tap(13, 2); CHECK(!u.presetRenaming());                                // ent: Sets::beginSetLabel
  waitSets();
  CHECK(strcmp(u.statusText(), "LABEL SET") == 0);
  CHECK(hasLine(readCardText(tpPath(19).c_str()), "lp.label=qwAz-1"));
  CHECK(waitPresetList()); advanceMs(40); draw(); CHECK(g_gfx.sawText("019 A  qwAz-1"));
  tap(8, 3); CHECK(strcmp(u.presetRenameText(), "qwAz-1") == 0);        // the rename starts from the current label ...
  tap(5, 1); CHECK(strcmp(u.presetRenameText(), "qwAz-1t") == 0);
  tap(0, 0); CHECK(!u.presetRenaming()); CHECK_EQ((int)u.page(), (int)PgPresets);   // ... ` cancels without writing
  settleJobs(); CHECK(hasLine(readCardText(tpPath(19).c_str()), "lp.label=qwAz-1")); CHECK(!cardExists(tpPath(19) + ".tmp"));
  tap(8, 3); for (int i = 0; i < 25; i++) tap(1, 1); CHECK_EQ((int)strlen(u.presetRenameText()), kPresetLabelMax);   // 19 characters at most
  tap(0, 0);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  leaveTestpack();
}

// A load from the page that fails (here: the file behind the cursor row vanished from the card between the listing and the
// ent) is reported with the LOAD's error (OPEN SET) and AUTO starts nothing — the re-listing that follows (the load used
// Sets' page buffer) must not run first: Sets::beginPresetList resets ok() / lastError(), and the completion would then
// read "LOADED <name>" with the listing's result.
TEST(preset_manager_load_failure_reports_the_error) {
  Ui& u = ui(); mainPage(); drainCmds(); enterTestpack();
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 0);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 19);     // 020 went in the test before: rows 019 018 017 …
  tap(0, 3); CHECK_EQ(u.presetCursor(), 1);                               // 018
  if (!u.presetAuto()) tap(9, 3);
  CHECK(u.presetAuto());
  CHECK(cardExists(tpPath(18))); CHECK(sd().remove(tpPath(18).c_str())); CHECK(!cardExists(tpPath(18)));   // gone behind the page's back
  settleJobs(); drainCmds();
  tap(13, 2); CHECK(sets().busy());                                       // ent: the load starts (the row is still listed)
  waitSets();
  std::printf("  failed load: ok=%d err=\"%s\" status=\"%s\"\n", (int)sets().ok(), sets().lastError(), u.statusText());
  CHECK(!sets().ok()); CHECK(strcmp(u.statusText(), "OPEN SET") == 0); CHECK(strncmp(u.statusText(), "LOADED", 6) != 0);
  CHECK_EQ(countCmd(peekCmds(), CmdPlaySet), 0);                         // AUTO: a set that did not load does not start
  CHECK_EQ((int)u.page(), (int)PgPresets);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 18);       // then the rows come back, without 018
  advanceMs(40); draw(); CHECK(!g_gfx.sawText("018")); CHECK(g_gfx.sawText("017 A"));
  tap(9, 3); CHECK(!u.presetAuto());
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
  leaveTestpack();
}

// DELETE? PRESS AGAIN is bound to the row it was armed on, and to the page's target set and filter: a del on another row (the
// cursor moves with hold repeats), after a target toggle or under the other filter inside the 2 s window asks again
// instead of deleting what the cursor now stands on; only the same row again deletes.
TEST(preset_manager_delete_confirm_follows_the_row) {
  Ui& u = ui(); mainPage(); drainCmds(); enterTestpack();
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 0);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 18); CHECK_EQ(u.presetCursor(), 0);   // rows: 019 017 016 …
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0);   // armed on 019
  advanceMs(200); tap(0, 3); CHECK_EQ(u.presetCursor(), 1);                // the cursor moves to 017 inside the window
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0);   // another row: asks again, deletes nothing
  settleJobs(); CHECK(cardExists(tpPath(17))); CHECK(cardExists(tpPath(19)));
  advanceMs(200); tap(4, 1); CHECK_EQ(u.presetSet(), 1); CHECK_EQ(u.presetCursor(), 1);   // the target toggled to SET 2 inside the window ...
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); settleJobs(); CHECK(cardExists(tpPath(17)));   // ... a fresh first press
  advanceMs(200); tap(13, 0);                                              // the same row, set and filter inside the window: 017 goes
  waitSets(); CHECK(strcmp(u.statusText(), "DELETED") == 0); CHECK(!cardExists(tpPath(17))); CHECK(cardExists(tpPath(19)));
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 17);
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0);   // armed on row 1 (now 016) ...
  advanceMs(200); tap(7, 2); CHECK_EQ(u.presetFilterKey(), 9); CHECK(waitPresetList()); CHECK_EQ(u.presetCursor(), 0);   // ... the filter toggled inside the window
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); settleJobs();   // a fresh first press on 019 (A minor)
  CHECK(cardExists(tpPath(19))); CHECK(cardExists(tpPath(16, "Emin"))); CHECK_EQ(sets().presetListTotal(), 13);   // nothing deleted: the 15 A minor presets less 018 (test before) and 017
  tap(7, 3); CHECK_EQ(u.presetFilterKey(), -1); CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 17);
  tap(1, 0); CHECK_EQ(u.presetSet(), 0);                                   // back to SET 1 for the tests that follow
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgMain); drainCmds();
  leaveTestpack();
}

// SPEC §9.25: rows loaded in the target set's current random-load cycle carry a dot (a kPresetDotSize square in the margin
// before the row text, black on the inverted cursor row). The query is Sets::presetLoadedInCycle, the sets owner's: while
// Sets lacks it (Ui::presetCycleMarks() false) the page draws no dot and the test only checks that; once it is there, a
// load from the page marks its row, the other target set has its own marks, and nothing else is dotted.
static int dotOps() { int n = 0; for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.x == kPresetDotX && o.w == kPresetDotSize && o.h == kPresetDotSize) n++; return n; }
static bool dotOnRow(int r, Color c) { for (const Op& o : g_gfx.ops) if (o.kind == 'F' && o.x == kPresetDotX && o.y == kPresetRowY0 + r * kPresetRowH + kPresetDotDy && o.w == kPresetDotSize && o.c == c) return true; return false; }
TEST(preset_manager_marks_the_random_load_cycle) {
  Ui& u = ui(); mainPage(); drainCmds(); enterTestpack();
  std::printf("  Sets::presetLoadedInCycle: %s\n", Ui::presetCycleMarks() ? "available (dots checked)" : "not yet in Sets (no dots drawn; SPEC §9.25 pending on the sets owner)");
  shiftDown(); tap(10, 3); shiftUp(); CHECK_EQ((int)u.page(), (int)PgPresets); CHECK_EQ(u.presetSet(), 0);
  CHECK(waitPresetList()); CHECK_EQ(sets().presetListTotal(), 17); CHECK_EQ(u.presetCursor(), 0);   // rows: 019 016 015 …
  advanceMs(40); draw();
  const TextOp* row0 = g_gfx.drawn("019 A  qwAz-1"); CHECK(row0 != nullptr); if (row0) CHECK_EQ(row0->x, kPresetTextX);   // the text leaves the margin for the dot
  if (!Ui::presetCycleMarks()) { CHECK_EQ(dotOps(), 0); tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain); leaveTestpack(); return; }
  const int before = dotOps();
  if (u.presetAuto()) tap(9, 3);
  settleJobs(); drainCmds();
  tap(13, 2); CHECK(sets().busy()); waitSets(); CHECK(strncmp(u.statusText(), "LOADED", 6) == 0);   // ent: 019 into SET 1 (beginLoadNamed sets its bit)
  CHECK(waitPresetList()); advanceMs(40); draw();
  CHECK(dotOnRow(0, kBlack)); CHECK_EQ(dotOps(), before + 1);             // the cursor row's dot, black on the inverted row
  tap(0, 3); CHECK_EQ(u.presetCursor(), 1); advanceMs(40); draw();
  CHECK(dotOnRow(0, kNeonGreen)); CHECK(!dotOnRow(1, kBlack)); CHECK(!dotOnRow(1, kNeonGreen));   // 019 keeps its dot in the set's colour; 016 has none
  tap(0, 1); CHECK_EQ(u.presetSet(), 1); advanceMs(40); draw();
  CHECK(!dotOnRow(0, rgb(0x00, 0xE5, 0xFF)));                            // SET 2's cycle is its own: 019 was not loaded there
  tap(1, 0); CHECK_EQ(u.presetSet(), 0);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain); settleJobs(); drainCmds();
  leaveTestpack();
}

// SHIFT + save 1: the recorder page. ent starts a recording (the page closes), the performance's key events, gyro commands
// and volume steps reach Recorder::onInput (not the page's own keys, not repeats), the main footer blinks REC, ent on the
// page stops; the list shows the session's bars.
TEST(recorder_page_records_the_performance) {
  Ui& u = ui(); settleJobs(); mainPage(); drainCmds();
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgMain); CHECK(strcmp(u.statusText(), "NO RECORDER") == 0);   // before setRecorder
  u.setRecorder(&rec()); CHECK(u.recorder() == &rec());
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder); CHECK_EQ(u.recorderSessions(), 0);
  advanceMs(40); draw(); CHECK(g_gfx.sawText("RECORDER  0 SESSIONS")); CHECK(g_gfx.sawText("NO SESSIONS")); CHECK(g_gfx.drawn("REC") == nullptr);
  const int vol0 = u.outputVolume();
  tap(13, 2);                                                            // ent: recording starts, the page closes
  CHECK(rec().recording()); CHECK_EQ((int)u.page(), (int)PgMain); CHECK(strcmp(u.statusText(), "REC ON") == 0);
  CHECK_EQ((int)rec().recordedEvents(), 0);                              // the ent itself is not recorded
  tap(7, 0); CHECK_EQ((int)rec().recordedEvents(), 2);                  // a track key: press + release (type 0 / 1)
  shiftDown(); tap(1, 0); shiftUp(); CHECK_EQ((int)rec().recordedEvents(), 6);   // SHIFT is a key too
  key(6, 2, KeyEvent::Repeat, 400); CHECK_EQ((int)rec().recordedEvents(), 6);   // repeats are not recorded (the replay synthesises them)
  press(8, 2); advanceMs(450); release(8, 2); CHECK_EQ((int)rec().recordedEvents(), 9);   // gyro hold: 2 keys + the CmdGyro (type 2)
  press(8, 2); advanceMs(450); release(8, 2); CHECK_EQ((int)rec().recordedEvents(), 12);  // and off again
  shiftDown(); tap(13, 0); shiftUp(); CHECK_EQ((int)rec().recordedEvents(), 17);           // SHIFT + kick cut 1: 4 keys + the volume step (type 3)
  CHECK_EQ(u.outputVolume(), vol0 + kOutputVolumeStep);
  advanceMs(kParamPageMs + 50); CHECK_EQ((int)u.page(), (int)PgMain);
  g_now = (g_now / (2 * kBlinkHalfMs)) * (2 * kBlinkHalfMs) + 2 * kBlinkHalfMs; ui().tick(g_now); draw();   // lit phase of the footer REC
  const TextOp* mark = g_gfx.drawn("REC"); CHECK(mark != nullptr);
  CHECK(u.batteryPercent() < 0);                                         // the readout is "--" (12 px) here: the marker ends kFooterGap before it
  if (mark) { CHECK_EQ(mark->y, kFooterY + 1); CHECK_EQ((int)mark->c, (int)kRed); CHECK(mark->x + mark->w + kFooterGap <= kScreenW - kFooterBatMargin - 2 * 6); }
  advanceMs(kBlinkHalfMs); draw(); CHECK(g_gfx.drawn("REC") == nullptr);   // dark phase: cleared, nothing else repainted
  advanceMs(kBlinkHalfMs); draw(); CHECK(g_gfx.drawn("REC") != nullptr);
  runToTick(eng().status().tick + 2 * kTicksPerBar);                     // two bars of performance
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder);
  const uint32_t n0 = rec().recordedEvents();
  tap(0, 3); CHECK_EQ((int)rec().recordedEvents(), (int)n0);            // keys on the recorder page are not recorded
  advanceMs(40); draw(); CHECK(g_gfx.drawn("REC") != nullptr); CHECK(g_gfx.sawText("RECORDER  1 SESSION"));
  tap(13, 2); CHECK(!rec().recording()); CHECK(strcmp(u.statusText(), "REC STOPPED") == 0); CHECK_EQ((int)u.page(), (int)PgRecorder);
#ifndef LP_UI_NO_RECORDER
  CHECK(recFile("rec_000", ".lprec"));
#endif
  runBlocks(4); advanceMs(40); draw();
  CHECK(g_gfx.sawText("rec_000")); CHECK(g_gfx.sawText("2 BARS") || g_gfx.sawText("3 BARS")); CHECK(!g_gfx.sawText("BARS  W")); CHECK(g_gfx.drawn("REC") == nullptr);
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
  advanceMs(2 * kBlinkHalfMs); draw(); CHECK(g_gfx.drawn("REC") == nullptr);   // the footer marker is gone
  u.setOutputVolume(vol0); drainCmds();
}

// m renders the highlighted session: the RENDERING nn% overlay while Recorder::renderStep runs (it ticks the Ui itself),
// DONE /lp/wav/rec_000.wav for 3 s, the W mark; ` HELD 1 s on the overlay cancels, a tap (as a replayed ` is) does not.
TEST(recorder_renders_a_session_and_the_cancel_hold) {
  Ui& u = ui(); settleJobs(); mainPage(); drainCmds();
  CHECK_EQ(rec().sessionCount(), 1);
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder);
  tap(9, 3); CHECK(rec().rendering());                                    // m
  advanceMs(40); draw(); CHECK(g_gfx.sawText("RENDERING rec_000")); CHECK(g_gfx.sawText("0%")); CHECK(g_gfx.sawText("HOLD ` TO CANCEL"));
  // The render loop stands in for the sketch's loop(): renderStep renders a block and ticks the Ui on the replay clock; the
  // harness clock follows a block per step and the page is drawn with it.
  static int16_t block[kBlock]; int steps = 0, lastPct = -1; bool rose = false, pctDrawn = false;
  while (rec().rendering() && steps++ < 20000) {
    if (!rec().renderStep(block)) break;
    g_now += kBlockMs;
    const int p = rec().renderPercent(); if (p > lastPct) { if (lastPct >= 0) rose = true; lastPct = p; }
    if (steps % 10 == 0) { g_gfx.reset(); u.draw(g_now); char pt[8]; snprintf(pt, sizeof pt, "%d%%", p); if (p > 0 && g_gfx.drawn(pt)) pctDrawn = true; }
  }
  std::printf("  render: %d blocks, last %d%%\n", steps, lastPct);
  CHECK(!rec().rendering()); CHECK(rose); CHECK(pctDrawn); CHECK(steps < 20000);
  if (rec().renderNowMs() > g_now) g_now = rec().renderNowMs();
#ifndef LP_UI_NO_RECORDER
  CHECK(recFile("rec_000", ".wav"));
#endif
  advanceMs(kDrawIntervalMs); CHECK(u.renderDoneShowing()); draw(); CHECK(g_gfx.sawText("DONE")); CHECK(g_gfx.sawText("/lp/wav/rec_000.wav"));
  advanceMs(kRenderDoneMs + 10); CHECK(!u.renderDoneShowing()); CHECK_EQ((int)u.page(), (int)PgMain);
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder);
  runBlocks(3); advanceMs(40); draw(); CHECK(g_gfx.sawText("BARS  W"));  // the W mark
  tap(9, 3); CHECK(rec().rendering());
  press(0, 0); release(0, 0);                                            // a replayed-style tap of ` (down and up inside one loop turn): no cancel
  for (int i = 0; i < 150 && rec().rendering(); i++) { rec().renderStep(block); g_now += kBlockMs; }
  CHECK(rec().rendering());
  press(0, 0);                                                           // a live hold: ≥ 1 s of the render clock cancels
  int held = 0;
  for (; held < 400 && rec().rendering(); held++) { rec().renderStep(block); g_now += kBlockMs; }
  std::printf("  cancel after %d blocks of hold\n", held);
  CHECK(!rec().rendering()); CHECK(held >= 80); release(0, 0);
  if (rec().renderNowMs() > g_now) g_now = rec().renderNowMs();
  advanceMs(1); CHECK(strcmp(u.statusText(), "RENDER CANCELLED") == 0); CHECK(!u.renderDoneShowing()); CHECK_EQ((int)u.page(), (int)PgMain);
#ifndef LP_UI_NO_RECORDER
  CHECK(!recFile("rec_000", ".wav"));                                     // the cancelled render leaves no WAV
#endif
  push(CmdMasterPlay, 0, 1); runBlocks(1); drainCmds();                   // the replay stopped the transport (Recorder::finishRender)
}

// An offline render tells replayed events from live ones (Recorder::replaying()): a recorded ` HELD 1.5 s (a track key held
// live) replays as a hold and must NOT cancel the render (the session renders to DONE and its WAV); a live key meanwhile —
// a track tap, SHIFT + save 2, SHIFT + load 1, a SHIFT + gen pattern hold — never reaches the pages (the queue, the selection, the page and
// the drive state stay as they are); a recorded save S1 writes no preset copy on replay; the replay's SHIFT state (down when
// the recording stopped) does not outlive the render. Recorded in the Testpack genre so the live save lands in the fixture
// folder (tracked for the cleanup).
TEST(render_replays_a_held_backtick_and_ignores_live_keys) {
  Ui& u = ui(); settleJobs(); mainPage(); drainCmds(); enterTestpack();
  CHECK_EQ(rec().sessionCount(), 1);
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder);
  tap(13, 2); CHECK(rec().recording()); CHECK_EQ((int)u.page(), (int)PgMain);
  runBlocks(10);
  press(0, 0); runBlocks(130); release(0, 0, 130 * kBlockMs);           // ` = track 1 of set 1, held ≈ 1.56 s: a cancel-length hold on the key
  runBlocks(10);
  const std::vector<std::string> tp0 = tpFiles(), tpa0 = tpFiles("/assets"); g_tpAssetsDirExisted = cardExists(kTpPresets + "/assets");
  const size_t presets0 = tpPresetCount();
  tap(10, 2); CHECK(sets().busy()); waitSets();                           // save S1 inside the recording: one preset written live
  trackNewTpFiles(tp0, tpa0);
  const size_t presets1 = tpPresetCount(); CHECK_EQ(presets1, presets0 + 1);
  std::printf("  live save: \"%s\" (%zu → %zu preset(s), %zu file(s) tracked)\n", u.statusText(), presets0, presets1, g_tpCreated.size());
  runToTick(eng().status().tick + kTicksPerBar);
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder);   // the SHIFT release lands on the page: the session ends with SHIFT down
  tap(13, 2); CHECK(!rec().recording()); CHECK_EQ(rec().sessionCount(), 2);   // rec_001
  tap(0, 3); CHECK_EQ(u.recorderCursor(), 1);
  settleJobs(); drainCmds();
  tap(9, 3); CHECK(rec().rendering());                                   // m: render rec_001
  static int16_t block[kBlock]; int steps = 0; bool liveChecked = false;
  while (rec().rendering() && steps++ < 20000) {
    if (!rec().renderStep(block)) break;
    g_now += kBlockMs;
    if (steps == 200 && rec().rendering()) {                             // live keys in the middle of the render, between two steps ...
      CHECK(!rec().replaying());
      const size_t before = peekCmds().size(); const int sel = u.selectedTrack(); const Page pg = u.page();
      tap(7, 0);                                                         // ... a track tap pushes nothing and selects nothing
      const bool genBusy = gen().busy();
      shiftDown(); tap(10, 3); tap(11, 2);                               // ... SHIFT + save 2 opens no page, SHIFT + load 1 starts no theory job
      CHECK_EQ((int)gen().busy(), (int)genBusy);
      press(12, 2); CHECK_EQ(u.usbDriveState(), 0); release(12, 2); shiftUp();   // ... SHIFT + gen pattern does not even arm the drive hold
      CHECK_EQ(peekCmds().size(), before); CHECK_EQ(u.selectedTrack(), sel); CHECK_EQ((int)u.page(), (int)pg);
      liveChecked = true;
    }
  }
  std::printf("  render of rec_001: %d blocks, status \"%s\"\n", steps, u.statusText());
  CHECK(liveChecked); CHECK(!rec().rendering()); CHECK(steps < 20000);
  if (rec().renderNowMs() > g_now) g_now = rec().renderNowMs();
  advanceMs(kDrawIntervalMs); CHECK(u.renderDoneShowing()); CHECK(strcmp(u.statusText(), "RENDER CANCELLED") != 0);   // the replayed hold did not cancel
#ifndef LP_UI_NO_RECORDER
  CHECK(recFile("rec_001", ".wav"));
#endif
  CHECK_EQ(tpPresetCount(), presets1);                                   // the replayed save S1 wrote nothing
  CHECK(!u.shiftDown());                                                 // the replay left SHIFT down; the render's end put the physical (released) key back
  advanceMs(kRenderDoneMs + 10); CHECK(!u.renderDoneShowing()); CHECK_EQ((int)u.page(), (int)PgMain);
  push(CmdMasterPlay, 0, 1); runBlocks(1); drainCmds();
  leaveTestpack();
}

// On the recorder page too, DELETE? PRESS AGAIN is bound to the row: a del on another row inside the window asks again;
// the second press on the same row deletes that session (and its WAV).
TEST(recorder_delete_confirm_follows_the_row) {
  Ui& u = ui(); settleJobs(); mainPage(); drainCmds();
  CHECK_EQ(rec().sessionCount(), 2);
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder); CHECK_EQ(u.recorderSessions(), 2); CHECK_EQ(u.recorderCursor(), 0);
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0);   // armed on rec_000
  advanceMs(200); tap(0, 3); CHECK_EQ(u.recorderCursor(), 1);              // the cursor moves to rec_001 inside the window
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); CHECK_EQ(rec().sessionCount(), 2);   // another row: asks again, deletes nothing
#ifndef LP_UI_NO_RECORDER
  CHECK(recFile("rec_001", ".lprec")); CHECK(recFile("rec_001", ".wav")); CHECK(recFile("rec_000", ".lprec"));
#endif
  advanceMs(200); tap(13, 0);                                              // the same row inside the window: rec_001 goes, with its WAV
  CHECK(strcmp(u.statusText(), "DELETED") == 0); CHECK_EQ(rec().sessionCount(), 1); CHECK_EQ(u.recorderSessions(), 1); CHECK_EQ(u.recorderCursor(), 0);
#ifndef LP_UI_NO_RECORDER
  CHECK(!recFile("rec_001", ".lprec")); CHECK(!recFile("rec_001", ".wav")); CHECK(recFile("rec_000", ".lprec"));
#endif
  tap(0, 0); CHECK_EQ((int)u.page(), (int)PgMain);
}

// del on the recorder page: DELETE? PRESS AGAIN, then Recorder::deleteSession; the same SHIFT + save 1 exits.
TEST(recorder_delete_session) {
  Ui& u = ui(); settleJobs(); mainPage(); drainCmds();
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgRecorder); CHECK_EQ(u.recorderSessions(), 1);
  tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); CHECK_EQ(rec().sessionCount(), 1);
  advanceMs(kDeleteConfirmMs + 50); tap(13, 0); CHECK(strcmp(u.statusText(), "DELETE? PRESS AGAIN") == 0); CHECK_EQ(rec().sessionCount(), 1);
  advanceMs(300); tap(13, 0); CHECK(strcmp(u.statusText(), "DELETED") == 0); CHECK_EQ(rec().sessionCount(), 0); CHECK_EQ(u.recorderSessions(), 0);
#ifndef LP_UI_NO_RECORDER
  CHECK(!recFile("rec_000", ".lprec"));
#endif
  advanceMs(40); draw(); CHECK(g_gfx.sawText("NO SESSIONS"));
  shiftDown(); tap(10, 2); shiftUp(); CHECK_EQ((int)u.page(), (int)PgMain);
  u.setRecorder(nullptr);
}

TEST(ui_object_size_budget) {
  std::printf("  sizeof(Ui) = %zu B (Pattern %zu B)\n", sizeof(Ui), sizeof(Pattern));
  CHECK(sizeof(Ui) <= 2700);                         // 2.7 KB (SPEC §9.22 / §9.23 wave; 2.5 KB before), the looper's Pattern included
}

// The preset manager tests built /lp/packs/Testpack/presets (20 files, labels, a .tmp of the label rewrite, the preset and
// assets a recorded save wrote): the files they created are removed, the folder (and its assets/) only when it was not there
// before. The recorder tests wrote only the temporary card (/lp/rec, /lp/wav routed there): it goes away whole, and the real
// card's /lp/rec and /lp/wav are checked to be exactly as found — a run leaves the host card as it found it.
TEST(cleanup_wave_files) {
  settleJobs();
  for (const std::string& p : g_tpCreated) if (cardExists(p)) sd().remove(p.c_str());
  if (!g_tpDirExisted) {
    for (const char* sub : {"/assets", ""}) {
      for (const std::string& n : tpFiles(sub)) sd().remove((kTpPresets + sub + "/" + n).c_str());
      if (sub[0] && !g_tpAssetsDirExisted) sd().remove((kTpPresets + sub).c_str());
    }
    sd().remove(kTpPresets.c_str());
    CHECK(!cardExists(kTpPresets));
  }
  if (!g_recTmpRoot.empty()) {
    size_t removed = 0;
    for (const char* dir : {Recorder::kRecDir, Recorder::kWavDir}) {
      std::vector<std::string> names;
      recFs().tmp.walk(dir, [](const char* name, bool isDir, uint32_t, void* user) -> bool { if (!isDir) ((std::vector<std::string>*)user)->push_back(name); return true; }, &names);
      for (const std::string& n : names) if (recFs().tmp.remove((std::string(dir) + "/" + n).c_str())) removed++;
      recFs().tmp.remove(dir);
    }
    ::rmdir((g_recTmpRoot + "/lp").c_str()); ::rmdir(g_recTmpRoot.c_str());
    struct stat st; CHECK(stat(g_recTmpRoot.c_str(), &st) != 0);
    std::printf("  temporary card %s: %zu file(s) removed, gone\n", g_recTmpRoot.c_str(), removed);
    CHECK_EQ((int)cardExists(Recorder::kRecDir), (int)g_realRecDirExisted); CHECK_EQ((int)cardExists(Recorder::kWavDir), (int)g_realWavDirExisted);
    CHECK_EQ(countCardFiles(Recorder::kRecDir), g_realRecFiles); CHECK_EQ(countCardFiles(Recorder::kWavDir), g_realWavFiles);   // the real card's sessions: untouched
  }
  if (g_homeGenre >= 0) gen().setGenre(g_homeGenre);
}

// The session tests above wrote /lp/state/session_set<N>[_b].lpset and session_s<set>_t<N>.mid into the host card
// image: removed here (as tests/test_sets.cpp sizes_and_cleanup does) so a run leaves sdcard/lp/state as it found it.
// SPEC §9.20: a tap on a track key toggles the track AND selects it; SHIFT + track key still only selects.
TEST(tap_selects_the_track) {
  Ui& u = ui(); drainCmds();
  shiftDown(); tap(7, 0); shiftUp(); CHECK_EQ(u.selectedTrack(), 7); CHECK(peekCmds().empty());   // SHIFT: select only
  tap(2, 0);                                                                                     // set 1 Pad: toggled and selected
  CHECK_EQ(u.selectedTrack(), 2);
  std::vector<Cmd> c = peekCmds(); CHECK(findCmd(c, CmdTrackToggle, 2) != nullptr || findCmd(c, CmdCancelPending, 2) != nullptr);
  drainCmds();
  tap(1, 1);                                                                                     // set 2 Lead
  CHECK_EQ(u.selectedTrack(), 9);
  c = peekCmds(); CHECK(findCmd(c, CmdTrackToggle, 9) != nullptr || findCmd(c, CmdCancelPending, 9) != nullptr);
  drainCmds();
}

TEST(cleanup_session_files) {
  std::vector<std::string> names;
  sd().walk("/lp/state", [](const char* name, bool isDir, uint32_t, void* user) -> bool { if (!isDir) ((std::vector<std::string>*)user)->push_back(name); return true; }, &names);
  int removed = 0;
  for (const std::string& n : names) {
    const bool session = n.compare(0, 11, "session_set") == 0 || (n.compare(0, 9, "session_s") == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".mid") == 0);
    if (session && sd().remove(("/lp/state/" + n).c_str())) removed++;
  }
  std::printf("  removed %d session file(s) from /lp/state\n", removed);
}

int main() { return lp_test::run(); }
