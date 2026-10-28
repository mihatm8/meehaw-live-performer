// meehaw live performer — SDL2 desktop build (SPEC §7): the same Engine, Library, Generator, Sets and Ui objects
// as liveplayer.ino, with
//   audio     an SDL audio callback (22050 Hz mono S16, 256-sample buffers) that calls Engine::render directly:
//             the engine is lock-free (SPEC §7), so no SDL_LockAudioDevice around the loop task; the Ui's master
//             output volume (SPEC §3 SHIFT + kick cut) is a linear gain vol / 255 on the callback's output;
//   display   the 240x135 UI drawn through SdlGfx and shown at x3 in a 720x405 window;
//   keyboard  SDL scancodes of a US layout mapped to the 4x14 matrix exactly like the Cardputer legend (SPEC §3),
//             fed to KeyMatrix every frame; F12 = BtnA (master play/stop); Escape quits;
//   IMU       arrow keys tilt the simulated accelerometer (up/down pitch ±60°, left/right roll ±60°: beyond the
//             UI's 10° dead band + 45° range, so the gyro effects reach their maximum; neutral when released),
//             reported every 20 ms as ImuReading;
//   card      PosixFs rooted at argv[1] or ../../sdcard (also searched next to the binary).
// --selftest N runs N seconds headless (no window, no audio device: a thread renders blocks at real-time pace)
// with a scripted key sequence whose checks read the engine's published state (selftest.h) and exits 0 when
// every check held, the engine produced sound, no block rendered late and the UI drew something.
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/config.h"
#include "core/types.h"
#include "engine/engine.h"
#include "gen/library.h"
#include "gen/generator.h"
#include "gen/sets.h"
#include "io/keys.h"
#include "ui/ui.h"
#include "dsp/ulaw.h"
#include "posix_fs.h"
#include "sdl_gfx.h"
#include "desktop_input.h"
#include "selftest.h"
#ifdef LP_HAVE_RECORDER                                    // the Makefile defines it when src/rec/recorder.cpp exists (SPEC §9.23)
#include "rec/recorder.h"
#endif

using namespace lp;

namespace {

// ---------------------------------------------------------------------------------------------- objects (as in the sketch)

Engine g_engine;                                           // large static object: pools, patterns, buffers
int16_t g_loopRing[kLoopSlots][kLoopRingBytes / 2];        // loop slot rings (SPEC §9.19): static here, bootBlock on the device
Library g_library;
Generator g_generator;
Sets g_sets;
Ui g_ui;
#ifdef LP_HAVE_RECORDER
Recorder g_recorder;                                       // session recorder (SPEC §9.23): SHIFT + save 1 page, offline WAV render (SHIFT + save 2 = the preset manager, SHIFT + load 1 / 2 = THEORY generate, SPEC §9.24)
#endif
KeyMatrix g_keys;
SdlGfx g_gfx;
PosixFs* g_fs = nullptr;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_offline{false};                        // an offline render owns the engine: the audio callback / pacer render nothing meanwhile (SPEC §9.23)
SDL_AudioDeviceID g_dev = 0;
std::atomic<int> g_outVol{kOutputVolumeDefault};           // master output volume 0..255 from the Ui's hook: the audio callback's gain (the headless pacer has no output)

// ---------------------------------------------------------------------------------------------- audio

constexpr int64_t kBlockNs = (int64_t)kBlock * 1000000000LL / kSampleRate;   // 11 609 977 ns per block
constexpr int kSoundThreshold = 328;                       // |sample| above this counts a block as sounding (−40 dBFS)

struct AudioStats {
  std::atomic<uint32_t> blocks{0}, underruns{0}, late{0}, soundBlocks{0}, firstSoundBlock{0};
  std::atomic<uint32_t> renderMaxUs{0}, renderSumUs{0}, peak{0};
} g_audio;

// One 256-sample block (SPEC §2): render, then account time and level like the sketch's render task.
void renderBlock(int16_t* buf, int n) {
  auto t0 = std::chrono::steady_clock::now();
  g_engine.render(buf, n);
  uint32_t us = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  EngineStatus& st = const_cast<EngineStatus&>(g_engine.status());
  if (us > st.renderMicrosMax) st.renderMicrosMax = us;
  if (us > g_audio.renderMaxUs) g_audio.renderMaxUs = us;
  g_audio.renderSumUs += us;
  if ((int64_t)us * 1000 > kBlockNs * n / kBlock) { g_audio.underruns++; st.underruns++; }   // slower than real time
  int peak = 0;
  for (int i = 0; i < n; i++) { int a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > peak) peak = a; }
  uint32_t b = ++g_audio.blocks;
  if (peak > (int)g_audio.peak) g_audio.peak = (uint32_t)peak;
  if (peak > kSoundThreshold) { if (++g_audio.soundBlocks == 1) g_audio.firstSoundBlock = b; }
}

void audioCallback(void*, Uint8* stream, int len) {
  int16_t* out = (int16_t*)stream; const int n = len / 2;
  for (int done = 0; done < n;) { const int k = n - done < kBlock ? n - done : kBlock; renderBlock(out + done, k); done += k; }
  const int vol = g_outVol.load(std::memory_order_relaxed);   // master output volume (SPEC §3): linear gain vol / 255, after the level accounting
  if (vol < kOutputVolumeMax) for (int i = 0; i < n; i++) out[i] = (int16_t)((int32_t)out[i] * vol / kOutputVolumeMax);
}

// Headless pacing (selftest, or no audio device): one block per kBlockNs. A block that renders slower than
// real time is an underrun; a turn that comes more than the sketch's DMA depth (4 blocks, 46 ms) late is
// "late": on the device that stall empties the speaker queue, so the verdict tolerates only kLateBudget of them
// (selftest.h). The pacer runs at a raised priority so loop-task work cannot delay it.
void renderThread() {
  static int16_t buf[kBlock];
  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  auto next = std::chrono::steady_clock::now();
  const auto period = std::chrono::nanoseconds(kBlockNs);
  while (!g_quit.load(std::memory_order_relaxed)) {
    if (!g_offline.load(std::memory_order_relaxed)) renderBlock(buf, kBlock);   // an offline render (SPEC §9.23) renders its own blocks on the loop thread
    next += period;
    auto now = std::chrono::steady_clock::now();
    if (now > next + period * 4) { g_audio.late++; next = now; }
    else std::this_thread::sleep_until(next);
  }
}

// ---------------------------------------------------------------------------------------------- selftest script

// Key presses in matrix coordinates (SPEC §3): generate set 1, start four tracks (+ the vocal), variant switches,
// snare rush, kick cut, a SHIFT + filt+ ramp (released = snap back), a held vol- (one step, then the hold-ramp of
// SPEC §9.17c, frozen on release). Waits are real time. An expect() op waits up to `ms` for the engine to publish
// `v` for `track` (selftest.h checkHolds) and records a failure on timeout: variants land on the bar line (SPEC
// §9.5), rush on the next 1/16 and kick cut at once (§9.6), params() right away (§9.12). Every failed check fails
// the verdict.
enum OpKind : uint8_t { OpTap, OpDown, OpUp, OpWait, OpWaitGen, OpWaitPlaying, OpCheck, OpEnd };
struct Op { OpKind kind; uint8_t x, y; uint32_t ms; uint8_t track; uint8_t what; float v; };
constexpr uint32_t kTapMs = 40;
constexpr uint32_t kBarWaitMs = 4500;                      // > two bars at 120 BPM: a bar-line change must have landed
constexpr Op tap(int x, int y) { return {OpTap, (uint8_t)x, (uint8_t)y, 0, 0, 0, 0.0f}; }
constexpr Op hold(int x, int y) { return {OpDown, (uint8_t)x, (uint8_t)y, 0, 0, 0, 0.0f}; }
constexpr Op release(int x, int y) { return {OpUp, (uint8_t)x, (uint8_t)y, 0, 0, 0, 0.0f}; }
constexpr Op idle(uint32_t ms) { return {OpWait, 0, 0, ms, 0, 0, 0.0f}; }
constexpr Op waitGen(uint32_t ms) { return {OpWaitGen, 0, 0, ms, 0, 0, 0.0f}; }
constexpr Op waitPlaying(int track, uint32_t ms) { return {OpWaitPlaying, 0, 0, ms, (uint8_t)track, 0, 0.0f}; }
constexpr Op expect(CheckId id, int track, float v, uint32_t ms) { return {OpCheck, 0, 0, ms, (uint8_t)track, (uint8_t)id, v}; }
constexpr Op finish() { return {OpEnd, 0, 0, 0, 0, 0, 0.0f}; }
const Op kScript[] = {
  idle(1800),                                               // splash (SPEC §4: 1.5 s)
  tap(13, 2),                                               // GENERATE S1
  waitGen(20000),
  idle(200),
  tap(7, 0), tap(5, 0), tap(3, 0), tap(0, 0),               // Beat, Percussion, Bass, Chord of set 1 (tracks 7 5 3 0)
  tap(6, 0),                                                // Vocal (streams the phrase file, SPEC §9.3)
  waitPlaying(Beat, 5000),
  expect(CkPlaying, Percussion, 1, kBarWaitMs), expect(CkPlaying, Bass, 1, kBarWaitMs),   // the others start on the same
  expect(CkPlaying, Chord, 1, kBarWaitMs), expect(CkPlaying, Vocal, 1, kBarWaitMs),       // (or at worst the next) bar line
  idle(500),
  tap(9, 0),  expect(CkVariant, Beat, 1, kBarWaitMs),       // pat2 .. pat4, pat1 (selected = set 1 Beat), each at a bar line
  tap(10, 0), expect(CkVariant, Beat, 2, kBarWaitMs),
  tap(11, 0), expect(CkVariant, Beat, 3, kBarWaitMs),
  tap(8, 0),  expect(CkVariant, Beat, 0, kBarWaitMs),
  idle(300),
  hold(12, 0), expect(CkRush, Beat, 1, 400), idle(300),     // snare rush while held: engages at the next 1/16 (125 ms)
  release(12, 0), expect(CkRush, Beat, 0, 400),             // ... and releases at the next 1/16
  tap(13, 0), expect(CkKickCut, Beat, 1, 200), idle(300),   // kick cut on (immediate), off
  tap(13, 0), expect(CkKickCut, Beat, 0, 200),
  hold(13, 3), idle(60), tap(7, 0), idle(60),               // SHIFT + Beat: select it again without a launch (the track taps above selected the Vocal, SPEC §9.20) ...
  tap(4, 2),                                                // ... then SHIFT + filt+ : ramp 0 → 1 over rampBars = 4 bars (8 s)
  idle(1500), expect(CkFilterAbove, Beat, 0.05f, 100),      // 1.5 s in: ≈ 0.19
  release(13, 3), expect(CkFilterZero, Beat, 0.001f, 300),  // SHIFT release snaps back to the start value
  idle(300),
  hold(0, 3), idle(700), release(0, 3),                     // vol- held 700 ms: one step 0.8 → 0.75, then the hold-ramp towards 0 at the
  expect(CkVolume, Beat, 0.6625f, 200),                     // SHIFT-ramp speed (4 bars × 0.75 = 6 s): 0.75 − 0.75 × 0.7 / 6 ≈ 0.66 at the release ...
  idle(500), expect(CkVolume, Beat, 0.6625f, 100),          // ... and frozen there (CmdFreezeRamp, SPEC §9.17c)
  finish(),
};

const char* opName(OpKind k) { static const char* n[] = {"tap", "down", "up", "wait", "wait-gen", "wait-playing", "check", "end"}; return n[k]; }
void logOp(uint32_t now, const Op& op) {
  if (op.kind == OpTap || op.kind == OpDown || op.kind == OpUp) { KeyMapping m = mapKey(op.x, op.y); printf("[script] %6.2fs %s (%d,%d) fn=%d a=%d b=%d\n", now / 1000.0, opName(op.kind), op.x, op.y, (int)m.fn, (int)m.a, (int)m.b); }
  else if (op.kind == OpCheck) printf("[script] %6.2fs check %s[%d] %g within %u ms\n", now / 1000.0, checkName(op.what), op.track, op.v, (unsigned)op.ms);
  else printf("[script] %6.2fs %s %u ms\n", now / 1000.0, opName(op.kind), (unsigned)op.ms);
}

struct Script {
  int pc = 0; bool started = false; uint32_t until = 0; uint8_t phase = 0; uint32_t opStart = 0;
  uint16_t held[kKeyRows] = {0};
  uint32_t genStartedAt = 0, genDoneAt = 0, playingAt = 0; bool genTimeout = false, playTimeout = false;
  uint32_t checksOk = 0, checkFails = 0; char firstFail[64] = "";
  bool finished() const { return kScript[pc].kind == OpEnd; }
  void set(int x, int y, bool down) { if (down) held[y] |= (uint16_t)(1u << x); else held[y] &= (uint16_t)~(1u << x); }
  void mark() { for (int y = 0; y < kKeyRows; y++) for (int x = 0; x < kKeyCols; x++) if ((held[y] >> x) & 1) g_keys.markPressed(x, y); }
  void advance() { pc++; started = false; phase = 0; }
  // OpCheck: the engine's published value for the op, and its outcome (the first failure names the verdict).
  static float value(const Op& op) { return checkValue(g_engine.status(), g_engine.params(op.track), (CheckId)op.what, op.track); }
  void checkDone(const Op& op, uint32_t now, bool ok, float got) {
    printf("[check] %6.2fs %s[%d] %g: %s (got %g after %u ms)\n", now / 1000.0, checkName(op.what), op.track, op.v, ok ? "ok" : "FAILED", got, (unsigned)(now - opStart));
    if (ok) { checksOk++; return; }
    checkFails++;
    if (!firstFail[0]) snprintf(firstFail, sizeof firstFail, "check failed: %s[%d] expected %g got %g", checkName(op.what), op.track, op.v, got);
  }
  void update(uint32_t now) {
    for (int guard = 0; guard < 8 && !finished(); guard++) {
      const Op& op = kScript[pc];
      if (!started) {
        started = true; until = now + op.ms; opStart = now;
        logOp(now, op);
        if (op.kind == OpTap) { set(op.x, op.y, true); until = now + kTapMs; }
        else if (op.kind == OpDown) { set(op.x, op.y, true); advance(); continue; }
        else if (op.kind == OpUp) { set(op.x, op.y, false); advance(); continue; }
        else if (op.kind == OpWaitGen) until = now + 1500;   // phase 0: the UI must start the job within 1.5 s
      }
      bool done = false;
      switch (op.kind) {
        case OpTap: if ((int32_t)(now - until) >= 0) { set(op.x, op.y, false); done = true; } break;
        case OpWait: done = (int32_t)(now - until) >= 0; break;
        case OpWaitGen:
          if (phase == 0) { if (g_generator.busy()) { phase = 1; genStartedAt = now; until = now + op.ms; } else if ((int32_t)(now - until) >= 0) { genTimeout = true; done = true; } }
          else if (!g_generator.busy()) { genDoneAt = now; done = true; } else if ((int32_t)(now - until) >= 0) { genTimeout = true; done = true; }
          break;
        case OpWaitPlaying:
          if (g_engine.status().track[op.track].state == PSPlaying) { playingAt = now; done = true; } else if ((int32_t)(now - until) >= 0) { playTimeout = true; done = true; }
          break;
        case OpCheck: {
          const float got = value(op);
          if (checkHolds((CheckId)op.what, got, op.v)) { checkDone(op, now, true, got); done = true; }
          else if ((int32_t)(now - until) >= 0) { checkDone(op, now, false, got); done = true; }
          break; }
        default: break;
      }
      if (!done) return;
      advance();
    }
  }
} g_script;

// ---------------------------------------------------------------------------------------------- stub fallback driver

#ifdef LP_UI_STUB
// While src/ui/ui.cpp does not exist the Ui ignores keys; this minimal driver performs the actions of the keys
// the selftest script uses (SPEC §3 semantics, engine protocols of SPEC §9) so the selftest stays meaningful.
// It is compiled only with the stub and disappears with the real UI.
struct StubDriver {
  bool shift = false; int selected = 7; bool kickCut[kSets] = {false, false};
  void push(CmdType t, int track = 0, int a = 0, int32_t i = 0, float f = 0) { Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f; g_engine.cmds().push(c); }
  // Snare rush (SPEC §9.6): the engine's rush pattern, or on a Beat in loop mode the generator's 1-beat crop of the stream (SPEC §9.19 §1.4).
  void rush(int set, bool on) { const int beat = set * kTracksPerSet + Beat; if (g_generator.loopEnabled(beat) || g_generator.loopRushing(beat)) g_generator.rushLoop(beat, on); else push(CmdSnareRush, 0, set, on ? 1 : 0); }
  static float paramOf(const TrackParams& p, ParamId id) { return id == PVolume ? p.volume : id == PReverb ? p.reverb : id == PEcho ? p.echo : id == PDist ? p.dist : p.filter; }
  void onKey(const KeyEvent& e) {
    KeyMapping m = mapKey(e.x, e.y);
    if (m.fn == KShift) { if (e.kind == KeyEvent::Press) shift = true; else if (e.kind == KeyEvent::Release) { shift = false; push(CmdCancelRamps); } return; }
    if (e.kind == KeyEvent::Release) { if (m.fn == KSnareRush) rush(m.a, false); return; }
    switch (m.fn) {
      case KTrack: if (shift) selected = m.a; else if (e.kind == KeyEvent::Press) push(CmdTrackToggle, m.a); break;
      case KVariant: if (e.kind == KeyEvent::Press) g_generator.rebuildVariant(trackSet(selected) == m.a ? selected : m.a * kTracksPerSet + Beat, m.b); break;
      case KSnareRush: if (e.kind == KeyEvent::Press) rush(m.a, true); break;
      case KKickCut: if (e.kind == KeyEvent::Press) { kickCut[m.a & 1] = !kickCut[m.a & 1]; push(CmdKickCut, 0, m.a, kickCut[m.a & 1]); } break;
      case KGenSetAll: if (e.kind == KeyEvent::Press) g_generator.generateSet(shift ? 1 : 0); break;
      case KLoadSet: if (shift && e.kind == KeyEvent::Press) g_generator.generateTheorySet(m.a); break;   // SHIFT + load 1 / 2 = THEORY generate of that set (SPEC §9.24); the plain load and SHIFT + save 2 (the preset manager page) need the real Ui
      case KGenPattern: if (e.kind == KeyEvent::Press) g_generator.generatePattern(selected); break;
      case KGenSound: if (e.kind == KeyEvent::Press) g_generator.generateSound(selected); break;
      case KParamUp: case KParamDown: {
        int dir = m.fn == KParamUp ? 1 : -1; ParamId p = (ParamId)m.a;
        if (p == PTempo) push(CmdSetTempo, 0, 0, g_engine.status().bpm + dir);
        else if (p == PLength) { if (e.kind == KeyEvent::Press) g_generator.setLengthStep(selected, dir); }
        else if (p == PKey) {}
        else if (shift) { if (e.kind == KeyEvent::Press) push(CmdRamp, selected, p, kRampBarsDefault * kTicksPerBar, dir > 0 ? 1.0f : (p == PFilter ? -1.0f : 0.0f)); }
        else { float step = p == PFilter ? 0.1f : 0.05f; push(CmdSetParam, selected, p, 0, paramOf(g_engine.params(selected), p) + dir * step); }
        break; }
      default: break;
    }
  }
} g_stub;
#endif

// ---------------------------------------------------------------------------------------------- card root

bool cardOk(const char* root) {
  if (!root || !root[0]) return false;
  char p[PosixFs::kFullMax]; snprintf(p, sizeof p, "%s/lp", root);
  PosixFs probe(root);
  return probe.isDir("/lp");
}

// argv root, ../../sdcard, or sdcard relative to the binary's folder (build/ → desktop → firmware → root).
void resolveCard(const char* arg, char* out, int cap) {
  out[0] = 0;
  if (arg && cardOk(arg)) { snprintf(out, cap, "%s", arg); return; }
  if (cardOk("../../sdcard")) { snprintf(out, cap, "%s", "../../sdcard"); return; }
  char* base = SDL_GetBasePath();
  if (base) {
    const char* rel[] = {"../../../sdcard", "../../sdcard", "../sdcard"};
    for (const char* r : rel) { char p[PosixFs::kFullMax]; snprintf(p, sizeof p, "%s%s", base, r); if (cardOk(p)) { snprintf(out, cap, "%s", p); break; } }
    SDL_free(base);
  }
  if (!out[0] && arg) snprintf(out, cap, "%s", arg);
}

// ---------------------------------------------------------------------------------------------- loop

void logStat(uint32_t now) {
  const EngineStatus& st = g_engine.status();
  printf("[stat] t=%.1fs blocks=%u render_max_us=%u underruns=%u late=%u peak=%.2f gen=%s %d/%d err=\"%s\" vocal_underruns=%u loop_underruns=%u/%u slots=%d/%d\n",
         now / 1000.0, g_audio.blocks.load(), g_audio.renderMaxUs.load(), g_audio.underruns.load(), g_audio.late.load(),
         g_audio.peak.load() / 32767.0, g_generator.busy() ? "busy" : "idle", g_generator.progress(), g_generator.total(),
         g_generator.lastError(), (unsigned)st.vocalUnderruns,
         (unsigned)g_engine.streamUnderruns(kSets), (unsigned)g_engine.streamUnderruns(kSets + 1), g_engine.loopTrack(0), g_engine.loopTrack(1));   // SPEC §9.19 loop slots
  fflush(stdout);
}

// One iteration of the sketch's loop(): the vocal rings first (SPEC §9.2: refilled before any other SD work of
// the iteration), then keys → ui.onKey, imu → ui.onImu, gen.step(), sets.step() (save / load and the session
// write / restore of SPEC §9.18: one bounded SD action per iteration, stepped by the loop exactly as the sketch
// does — ui.tick() only polls it), ui.tick(), ui.draw().
void loopOnce(uint32_t now, bool headless, ImuSim& imu, uint32_t& lastImu, bool& f12Was) {
  g_generator.pumpVocal();
  const Uint8* ks = nullptr;
  if (!headless) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) g_quit = true;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g_quit = true;
    }
    ks = SDL_GetKeyboardState(nullptr);
  }
  g_keys.beginScan();
  if (ks) scanSdlKeyboard(ks, g_keys); else { g_script.update(now); g_script.mark(); }
  static KeyEvent evs[16];
  int n = g_keys.endScan(now, evs, 16);
  // As the sketch (SPEC §9.23): during an offline render the Ui lives on the recorder's clock (renderStep ticks it with the
  // rendered time and the replayed keys carry it), so the live keys get that clock too — the ` cancel hold is timed on it —
  // and the IMU is skipped (a render's gyro FX come from the recorded type 2 events, not from the desk the laptop stands on).
#ifdef LP_HAVE_RECORDER
  const bool rendering = g_recorder.rendering();
  const uint32_t uiNow = rendering ? g_recorder.renderNowMs() : now;
#else
  const bool rendering = false; const uint32_t uiNow = now;
#endif
  for (int i = 0; i < n; i++) {
    g_ui.onKey(evs[i], uiNow);
#ifdef LP_UI_STUB
    g_stub.onKey(evs[i]);
#endif
  }
  bool f12 = ks && ks[btnAScancode()];
  if (f12 && !f12Was) { Cmd c; c.type = CmdMasterPlay; c.a = g_engine.status().running ? 0 : 1; g_engine.cmds().push(c); }
  f12Was = f12;
  if (now - lastImu >= 20) {
    static const Uint8 none[SDL_NUM_SCANCODES] = {0};
    ImuReading r = imu.sample(ks ? ks : none, (now - lastImu) / 1000.0f);
    lastImu = now;
    if (!rendering) g_ui.onImu(r, uiNow);
  }
#ifdef LP_HAVE_RECORDER
  // Offline render (SPEC §9.23), as the sketch does it: the live audio stands still (the callback is paused and locked out,
  // the headless pacer skips its blocks), renderStep renders one block, replays the recorded events and ticks the Ui on the
  // replay clock (the Ui must not be ticked with millis() meanwhile); the live keys above still reach the Ui (` held = cancel).
  if (g_recorder.rendering()) {
    if (!g_offline.exchange(true) && g_dev) SDL_PauseAudioDevice(g_dev, 1);
    static int16_t block[kBlock];
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_recorder.renderStep(block);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    g_ui.draw(g_recorder.renderNowMs());
    g_gfx.pump();
    return;
  }
  if (g_offline.exchange(false) && g_dev) SDL_PauseAudioDevice(g_dev, 0);
  g_recorder.step();                                       // the event buffer to the card when it is free (one bounded write)
#endif
  g_generator.step();
  g_sets.step();
  g_ui.tick(now);
  g_ui.draw(now);
  g_gfx.pump();
}

char g_framePath[PosixFs::kFullMax] = "selftest_frame.bmp";   // set in main(): next to the binary, or --frame PATH

// Prints the run's numbers, saves the last frame and applies selftestVerdict (selftest.h) to the tally.
int selftestReport(uint32_t seconds) {
  const EngineStatus& st = g_engine.status();
  SelftestTally tally;
  tally.blocks = g_audio.blocks.load(); tally.underruns = g_audio.underruns.load(); tally.late = g_audio.late.load();
  tally.soundBlocks = g_audio.soundBlocks.load(); tally.peak = g_audio.peak.load();
  tally.vocalUnderruns = st.vocalUnderruns; tally.litPixels = g_gfx.litPixels();
  tally.checkFails = g_script.checkFails; snprintf(tally.firstCheckFail, sizeof tally.firstCheckFail, "%s", g_script.firstFail);
  tally.genTimeout = g_script.genTimeout; tally.playTimeout = g_script.playTimeout; tally.scriptFinished = g_script.finished();
  printf("[selftest] %u s: blocks=%u render_avg_us=%.1f render_max_us=%u underruns=%u late=%u (budget %u)\n", seconds, tally.blocks,
         tally.blocks ? (double)g_audio.renderSumUs.load() / tally.blocks : 0.0, g_audio.renderMaxUs.load(), tally.underruns, tally.late, (unsigned)kLateBudget);
  printf("[selftest] peak=%.3f sound_blocks=%u first_sound=%.2fs gen_started=%.2fs gen_done=%.2fs playing=%.2fs\n",
         tally.peak / 32767.0, tally.soundBlocks, g_audio.firstSoundBlock.load() * kBlock / (double)kSampleRate,
         g_script.genStartedAt / 1000.0, g_script.genDoneAt / 1000.0, g_script.playingAt / 1000.0);
  printf("[selftest] gen err=\"%s\" note=\"%s\" genre=%s key=%s bpm=%u vocal_underruns=%u ui_draw_calls=%u lit_px=%u checks=%u/%u tracks:",
         g_generator.lastError(), g_generator.lastNote(), g_generator.genreName(), g_generator.key().rootName(), (unsigned)st.bpm,
         tally.vocalUnderruns, g_gfx.drawCalls(), tally.litPixels, g_script.checksOk, g_script.checksOk + g_script.checkFails);
  for (int t = 0; t < kTracks; t++) {
    const TrackStatus& ts = st.track[t]; if (ts.state == PSEmpty) continue;
    printf(" S%d.%s=%s/v%d", t / kTracksPerSet + 1, categoryShort(trackCategory(t)), ts.state == PSPlaying ? "play" : ts.state == PSStopped ? "stop" : "queued", ts.variant + 1);
  }
  printf("\n");
  if (g_gfx.saveBmp(g_framePath)) printf("[selftest] frame: %s\n", g_framePath);
  else printf("[selftest] WARNING: frame not saved to %s (%s)\n", g_framePath, SDL_GetError());
  const char* fail = selftestVerdict(tally);
  printf("[selftest] %s%s\n", fail ? "FAIL: " : "PASS", fail ? fail : "");
  fflush(stdout);
  return fail ? 1 : 0;
}

int usage(FILE* to, const char* argv0) {
  fprintf(to, "usage: %s [--selftest SECONDS | --seconds SECONDS] [--frame PATH] [card-root]\n"
              "  SECONDS  a whole number >= 1 (--selftest: headless scripted run; --seconds: quit the window after N s)\n"
              "  --frame  where the selftest saves its last frame (default: selftest_frame.bmp next to the binary)\n", argv0);
  return to == stdout ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
  bool headless = false; uint32_t seconds = 0; const char* cardArg = nullptr; const char* frameArg = nullptr;
  for (int i = 1; i < argc; i++) {                           // argument errors exit 2 before SDL starts: a 0 / junk N must never run forever
    const char* a = argv[i];
    const bool selftest = !strcmp(a, "--selftest");
    if (selftest || !strcmp(a, "--seconds")) {
      if (i + 1 >= argc || !parseSeconds(argv[i + 1], seconds)) { fprintf(stderr, "%s: %s needs a whole number of seconds >= 1\n", argv[0], a); return usage(stderr, argv[0]); }
      headless = headless || selftest; i++;
    }
    else if (!strcmp(a, "--frame")) { if (i + 1 >= argc) { fprintf(stderr, "%s: --frame needs a path\n", argv[0]); return usage(stderr, argv[0]); } frameArg = argv[++i]; }
    else if (!strcmp(a, "--help") || !strcmp(a, "-h")) return usage(stdout, argv[0]);
    else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "%s: unknown option %s\n", argv[0], a); return usage(stderr, argv[0]); }
    else cardArg = a;
  }
  if (SDL_Init(headless ? (SDL_INIT_TIMER | SDL_INIT_EVENTS) : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2;
  }
  { char* base = SDL_GetBasePath(); framePath(base, frameArg, g_framePath, sizeof g_framePath); if (base) SDL_free(base); }
  char root[PosixFs::kRootMax]; resolveCard(cardArg, root, sizeof root);
  g_fs = new PosixFs(root);
  bool sdOk = g_fs->isDir("/lp");
  printf("[boot] card=%s (%s) ui=%s mode=%s\n", g_fs->root(), sdOk ? "ok" : "NO lp/ FOLDER",
#ifdef LP_UI_STUB
         "stub",
#else
         "src/ui/ui.cpp",
#endif
         headless ? "selftest" : "window");

  if (!g_gfx.begin(headless, "meehaw live performer")) fprintf(stderr, "gfx: %s (drawing disabled)\n", SDL_GetError());
  ulawInit();
  g_engine.init();
  for (int k = 0; k < kLoopSlots; k++) g_engine.setLoopRing(k, g_loopRing[k], kLoopRingBytes / 2);   // before the first render (SPEC §9.19)
  g_library.init(g_fs);
  g_generator.init(g_fs, &g_engine, &g_library);
  g_sets.init(g_fs, &g_engine, &g_generator, &g_library);
  g_ui.init(&g_gfx, &g_engine, &g_generator, &g_sets);
  g_ui.setBattery(100, 0);                                 // no battery on the desktop: the footer shows a full, not charging one (SPEC §4)
  g_ui.setOutputVolumeHook([](int v) { g_outVol.store(v, std::memory_order_relaxed); });   // as the sketch's Speaker.setVolume
  // SHIFT + gen pattern held 1 s (SPEC §9.22): the Cardputer reboots into the USB drive firmware; the desktop has no second
  // firmware (the card folder is a plain directory here), so the hook only says so — the key path itself is exercised.
  g_ui.setUsbDriveHook([]() { printf("[drive] USB drive mode requested: no drive firmware on the desktop (the card is %s)\n", g_fs ? g_fs->root() : "?"); fflush(stdout); g_ui.showStatus("NO DRIVE ON DESKTOP", 3000); });
#ifdef LP_HAVE_RECORDER
  g_recorder.init(g_fs, &g_engine, &g_generator, &g_sets, &g_ui);   // SPEC §9.23: SHIFT + save 1 = the recorder page; renders go to <card>/lp/wav
  g_ui.setRecorder(&g_recorder);
#endif
  if (!sdOk) g_ui.showStatus("NO SD CARD", 3000);
  if (headless) for (int i = 0; i < g_library.genreCount(); i++) if (!strcmp(g_library.genreName(i), "House")) g_generator.setGenre(i);   // deterministic pack
  g_generator.seed(headless ? 20261015u : (uint32_t)SDL_GetPerformanceCounter());
  if (!headless && sdOk) g_sets.restoreSession();           // the last session comes back as the sketch does at boot (SPEC §9.18); the scripted selftest starts clean

  SDL_AudioDeviceID dev = 0; std::thread paced;
  if (!headless) {
    SDL_AudioSpec want, have; SDL_zero(want);
    want.freq = kSampleRate; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = kBlock; want.callback = audioCallback;
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev) { printf("[boot] audio %d Hz, %d samples per callback\n", have.freq, have.samples); SDL_PauseAudioDevice(dev, 0); }
    else fprintf(stderr, "audio: %s (rendering silently at real-time pace)\n", SDL_GetError());
  }
  g_dev = dev;                                             // loopOnce pauses / locks it around an offline render (SPEC §9.23)
  if (!dev) paced = std::thread(renderThread);

  ImuSim imu; uint32_t lastImu = 0, lastLog = 0; bool f12Was = false;
  const uint32_t t0 = SDL_GetTicks();
  while (!g_quit.load(std::memory_order_relaxed)) {
    uint32_t now = SDL_GetTicks() - t0;
    loopOnce(now, headless, imu, lastImu, f12Was);
    if (now - lastLog >= 5000) { lastLog = now; logStat(now); }
    if (seconds && now >= seconds * 1000) break;
    SDL_Delay(1);
  }
  int rc = headless ? selftestReport(seconds) : 0;
  g_quit = true;
  if (dev) SDL_CloseAudioDevice(dev);
  if (paced.joinable()) paced.join();
  g_gfx.end();
  delete g_fs; g_fs = nullptr;
  SDL_Quit();
  return rc;
}
