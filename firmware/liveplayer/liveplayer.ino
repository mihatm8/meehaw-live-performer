// meehaw live performer — Arduino entry point for the M5Stack Cardputer ADV (SPEC §7).
// Render task on core 0 feeds Speaker.playRaw; everything else (keys, IMU, SD, generation, UI) runs in loop() on core 1.
#include <M5Cardputer.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>                  // USB drive mode (SPEC §9.22): the boot slot switch to the drive firmware in ota_1
#include <esp_partition.h>
#include <new>
#include "src/core/config.h"
#include "src/core/types.h"
#include "src/engine/engine.h"
#include "src/gen/library.h"
#include "src/gen/generator.h"
#include "src/gen/sets.h"
#include "src/io/keys.h"
#include "src/ui/ui.h"
#include "src/rec/recorder.h"
#include "src/platform/sd_fs.h"
#include "src/platform/m5_gfx.h"
#include "src/dsp/ulaw.h"
#include "src/dsp/presets.h"

using namespace lp;

static Engine g_engine;                 // large static object: pools, patterns, buffers (SPEC §9.13)
// The loop-task state machines (SPEC §9.2) are allocated ONCE in setup(), before any task runs, and never freed:
// the ESP32-S3 has internal SRAM beyond the linker's dram0_0_seg that only the heap can reach (SPEC §9.13), and
// the static set (engine 222 KB + platform) plus these 45 KB does not fit the segment. Nothing allocates after boot.
static Library* g_library = nullptr;
static Generator* g_generator = nullptr;
static Sets* g_sets = nullptr;
static Ui g_ui;
static Recorder g_recorder;               // session recorder (SPEC §9.23): ≤ 700 B static (672 B measured before the 2026-10-16 review fixes added one seed word), nothing allocated; its files live in /lp/rec and /lp/wav
// OFFLINE RENDER (SPEC §9.23): while g_recorder.rendering() the render task stops calling Engine::render and feeds silence;
// it raises this flag once its last live block is out, and only then does loop() drive Recorder::renderStep (one Engine::render
// per call, on the loop task) — the two tasks never render at the same time. Cleared by the render task when it renders again.
static std::atomic<bool> g_renderParked{false};
static SdFs g_fs;
static M5Gfx g_gfx;
static KeyMatrix g_keys;
static int16_t g_audio[3][kBlock];        // three buffers in sequence: playRaw keeps a pointer, it does not copy (M5Unified Speaker_Class.hpp)
static int16_t* g_loopRing[kLoopSlots] = {nullptr, nullptr};   // loop slot rings (SPEC §9.19): bootBlock in setup(), handed to the engine, never freed
// One scratch buffer for the loop task's text work (SPEC §9.19 static reclaim: loadConfig and the `cat` command used
// separate static buffers; they are never live at the same time — both run on the loop task, one after the other).
static char g_scratch[2048];
static TaskHandle_t g_renderTask = nullptr;
static TaskHandle_t g_workerTask = nullptr;
static TaskHandle_t g_jobOwner = nullptr;   // the task whose parallelStart() is in flight: the worker notifies IT — the render task live, the LOOP task during an offline render (Recorder::renderStep calls Engine::render there; a fixed g_renderTask target would leave parallelJoin waiting for ever on the loop task, SPEC §9.23)
static ParallelFn g_workFn = nullptr; static void* g_workArg = nullptr;
static void workerTask(void*) { for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); if (g_workFn) g_workFn(g_workArg); xTaskNotifyGive(g_jobOwner); } }
static void parallelStart(ParallelFn fn, void* arg) { g_workFn = fn; g_workArg = arg; g_jobOwner = xTaskGetCurrentTaskHandle(); xTaskNotifyGive(g_workerTask); }
static void parallelJoin() { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); }   // on the task that called parallelStart (Engine::render pairs them on one task)
static bool g_sdOk = false;
static int g_demoSlot = -1; static uint16_t g_demoAck = 0;
static uint16_t g_virtualKeys[kKeyRows] = {0, 0, 0, 0};   // serial-injected key presses (bring-up aid)
static uint16_t g_virtualPulse[kKeyRows] = {0, 0, 0, 0};  // released automatically on the next scan

// Serial bring-up commands: "k X Y" press+release, "kd X Y" hold, "ku X Y" release, "note P V" (perform note on set-1 Fx,
// V 0..100, 0 = off), "tog T", "gen S", "vol N" (master output 0..255, explicit: written to the session, a later restore
// does not override it), "loop T" (toggle loop mode on track T, SPEC §9.19; prints the refusal), "stat" (engine, session,
// the four streams' underruns and loop slot bindings, and per track the acid / loop flags + the six §9.17 knobs). One command per line.
static const char* loopRefusalName(Generator::LoopRefusal r) {
  switch (r) {
    case Generator::LoopOk: return "ok"; case Generator::LoopSlotsFull: return "LOOP SLOTS FULL"; case Generator::LoopNoContent: return "NO LOOP CONTENT";
    case Generator::LoopBusy: return "LOOP: BUSY"; case Generator::LoopLowRam: return "LOOP: LOW RAM"; case Generator::LoopNoCard: return "LOOP: NO CARD";
  }
  return "?";
}
// USB drive mode (SPEC §9.22): the card is exposed to the computer by the SECOND firmware in the spare OTA slot
// (firmware/carddrive, ota_1 at 0x340000, written there by tools/flash_drive.sh; this player stays in ota_0). The hook
// (SHIFT + gen pattern held 1 s, or the serial command `drive`) checks that ota_1 holds an app, stops the audio, lets a
// session write in flight finish (one card operation per step, bounded), points the boot partition at ota_1 and
// restarts. The drive firmware's first act on its own boot is to point the boot partition back at ota_0, so any reset
// (ENTER there, its serial command `player`, the reset button, a power cycle) returns here. Nothing is allocated.
static bool usbDrive() {
  if (g_recorder.rendering()) {                                        // an offline render owns the engine and a half-written WAV: finish or cancel it (BtnA) first
    Serial.println("[drive] refused: a render is running (BtnA cancels it)");
    g_ui.showStatus("RENDERING", 3000);
    return false;
  }
  const esp_partition_t* drive = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  esp_app_desc_t desc;
  if (!drive || esp_ota_get_partition_description(drive, &desc) != ESP_OK   // erased slot: no app descriptor
      || !memcmp(desc.app_elf_sha256, esp_app_get_description()->app_elf_sha256, sizeof desc.app_elf_sha256)) {   // a copy of THIS player in ota_1 is not the drive (every Arduino sketch carries the core's descriptor "arduino-lib-builder", only the ELF hash tells them apart)
    Serial.println("[drive] no drive firmware in ota_1: run tools/flash_drive.sh");
    g_ui.showStatus("NO DRIVE FIRMWARE", 3000);
    return false;
  }
  Serial.printf("[drive] ota_1 holds an app (%s %s): switching\n", desc.project_name, desc.version);
  if (g_recorder.recording()) {                                        // a restart with the .lprec open would lose the buffered events and (FatFs: the size is written at close) leave a header-only file
    g_recorder.stopRecording();
    Serial.printf("[rec] stopped by the drive switch (%u events)\n", (unsigned)g_recorder.recordedEvents());
  }
  if (g_renderTask) vTaskSuspend(g_renderTask);                     // no more blocks rendered or queued
  M5Cardputer.Speaker.stop();
  // Every Sets machine drains first — a user save / label rewrite / delete (busy()) as much as the session write or restore
  // (sessionBusy(), SPEC §9.18): one bounded card operation per step, a restart in the middle would leave a truncated .lpset,
  // a half-written _index.tmp or a rename without its target. Bounded to 3 s; after that the switch is refused and the
  // player goes on (the user retries once the job is through).
  for (uint32_t t0 = millis(); (g_sets->busy() || g_sets->sessionBusy()) && millis() - t0 < 3000;) { g_sets->step(); delay(1); }
  if (g_sets->busy() || g_sets->sessionBusy()) {
    Serial.println("[drive] refused: Sets still busy after 3 s (a save / load / listing / session write did not finish)");
    g_ui.showStatus("SETS BUSY", 3000);
    if (g_renderTask) vTaskResume(g_renderTask);
    return false;
  }
  g_gfx.fillScreen(kBlack); g_gfx.drawText(8, 56, "USB DRIVE...", 2, kWhite);
  const esp_err_t e = esp_ota_set_boot_partition(drive);              // verifies the image, writes otadata (the drive sketch marks itself valid at boot like every Arduino app)
  if (e != ESP_OK) {
    Serial.printf("[drive] esp_ota_set_boot_partition failed: %d\n", (int)e);
    g_ui.showStatus("DRIVE SWITCH FAILED", 3000);
    if (g_renderTask) vTaskResume(g_renderTask);
    return false;
  }
  delay(100);                                                          // the serial line and the screen go out
  esp_restart();
  return true;
}

// `get <path>` (SPEC §9.22): a card file's bytes over the serial port, binary-safe — "G <size>\n" then exactly <size>
// raw bytes, "G -1\n" when the path is missing or a directory. The file is read through the shared scratch buffer
// (2 KB per card read; the chunking is invisible on the wire) and the transfer runs to completion inside the command,
// so no [diag] line can interleave with the bytes (everything that prints runs on this loop task). A preset is a few
// KB (a few tens of ms); stop the transport before pulling a WAV, the vocal streams are not refilled meanwhile.
// HWCDC gives up after ≈ 2 s of host back-pressure (short write): the transfer is then abandoned and the host sees a
// short stream. tools/card_pull.py walks a directory with `ls` and pulls every file (for a computer whose policy
// blocks USB drives); the card loader firmware answers the same `GET`.
static void serialGet(const char* path) {
  IFile* f = g_fs.open(path, "r");
  if (!f) { Serial.print("G -1\n"); return; }
  uint32_t left = f->size();
  Serial.printf("G %u\n", (unsigned)left);
  while (left) {
    const size_t n = f->read(g_scratch, left < sizeof g_scratch ? left : sizeof g_scratch);
    if (!n) break;                                                     // card error: the host reports the size mismatch
    left -= n;
    if (Serial.write((const uint8_t*)g_scratch, n) != n) break;        // the host stopped reading
  }
  f->close();
}

// `rec list|start|stop|render N|cancel|delete N` (SPEC §9.23): the session recorder from the serial port, as the RECORDER
// page (SHIFT + save 1) drives it. N is the row index of `rec list` (sessions in number order), not the file number.
static void recCommand(const char* args) {
  while (*args == ' ') args++;
  int n = 0;
  if (!strcmp(args, "list")) {
    const int count = g_recorder.sessionCount();
    Serial.printf("[rec] %d session(s)%s%s\n", count, g_recorder.recording() ? ", recording" : "", g_recorder.rendering() ? ", rendering" : "");
    for (int i = 0; i < count; i++) {
      RecHeader h; const bool ok = g_recorder.sessionInfo(i, h);
      Serial.printf("  %d %s events=%u ticks=%u bpm=%u key=%d genre=%s%s%s\n", i, g_recorder.sessionName(i), ok ? (unsigned)h.events : 0u, ok ? (unsigned)h.lengthTicks : 0u,
                    ok ? (unsigned)h.bpm : 0u, ok ? (int)h.keyRoot : -1, ok ? h.genre : "?", g_recorder.hasWav(i) ? " wav" : "", ok ? "" : " (unreadable)");
    }
  }
  else if (!strcmp(args, "start")) Serial.printf("[rec] start: %s\n", g_recorder.startRecording() ? "recording" : "refused (no card / dir, 64 sessions, or already recording / rendering)");
  else if (!strcmp(args, "stop")) { const bool was = g_recorder.recording(); g_recorder.stopRecording(); Serial.printf("[rec] stop: %s (%u events)\n", was ? "stopped" : "was not recording", (unsigned)g_recorder.recordedEvents()); }
  else if (sscanf(args, "render %d", &n) == 1) Serial.printf("[rec] render %d: %s\n", n, g_recorder.startRender(n) ? "rendering (the speaker is silent meanwhile; BtnA cancels)" : "refused (busy or no such session)");
  else if (!strcmp(args, "cancel")) { const bool was = g_recorder.rendering(); g_recorder.cancelRender(); Serial.printf("[rec] cancel: %s\n", was ? "cancelled, the partial WAV is removed" : "was not rendering"); }
  else if (sscanf(args, "delete %d", &n) == 1) Serial.printf("[rec] delete %d: %s\n", n, g_recorder.deleteSession(n) ? "removed (.lprec and .wav)" : "refused (no such session, or recording / rendering)");
  else Serial.println("[rec] usage: rec list|start|stop|render N|cancel|delete N");
}

static void handleSerialLine(char* line) {
  int a = 0, b = 0;
  if (!strncmp(line, "rec", 3) && (line[3] == 0 || line[3] == ' ')) { recCommand(line + 3); return; }   // SPEC §9.23 session recorder
  if (sscanf(line, "k %d %d", &a, &b) == 2) { if (a >= 0 && a < kKeyCols && b >= 0 && b < kKeyRows) g_virtualPulse[b] |= (1u << a); }
  else if (sscanf(line, "kd %d %d", &a, &b) == 2) { if (a >= 0 && a < kKeyCols && b >= 0 && b < kKeyRows) g_virtualKeys[b] |= (1u << a); }
  else if (sscanf(line, "ku %d %d", &a, &b) == 2) { if (a >= 0 && a < kKeyCols && b >= 0 && b < kKeyRows) g_virtualKeys[b] &= ~(1u << a); }
  else if (sscanf(line, "note %d %d", &a, &b) == 2) { Cmd c; c.type = CmdPerformNote; c.track = Fx; c.i = a; c.f = b / 100.0f; g_engine.cmds().push(c); }
  else if (sscanf(line, "tog %d", &a) == 1) { Cmd c; c.type = CmdTrackToggle; c.track = (uint8_t)a; g_engine.cmds().push(c); }
  else if (sscanf(line, "gen %d", &a) == 1) { g_generator->generateSet(a & 1); }
  else if (sscanf(line, "loop %d", &a) == 1) {       // SPEC §9.19: toggle loop mode on a track (as SHIFT + edit does), report a refusal
    if (a < 0 || a >= kTracks) Serial.println("[loop] track 0..15");
    else {
      const bool want = !g_generator->loopEnabled(a);
      const bool ok = g_generator->setLoopMode(a, want);
      if (ok) Serial.printf("[loop] t%d %s (applied at the bar line)\n", a, want ? "ON: picking a loop" : "OFF");
      else Serial.printf("[loop] t%d refused: %s\n", a, loopRefusalName(g_generator->lastLoopRefusal()));
    }
  }
  else if (!strncmp(line, "demo", 4)) {           // synthetic full load: patterns, presets and noise kits on all 16 tracks
    for (int t = 0; t < kTracks; t++) {
      Category cat = trackCategory(t);
      if (isSynthCategory(cat)) { makePatch(cat, t % presetCount(cat), 7u * t + 1, g_engine.nextPatch(t)); Cmd pc; pc.type = CmdSetPatch; pc.track = (uint8_t)t; g_engine.cmds().push(pc); }
      if (!g_engine.beginNextWrite(t)) continue;
      Pattern& p = g_engine.nextPattern(t); p.clear(kTicksPerBar);
      if (cat == Beat) { for (int st = 0; st < 16; st++) p.add(Note::make(st * 24, 12, st % 4 == 0 ? 36 : st % 2 ? 38 : 37, 100)); }
      else if (cat == Percussion) { for (int st = 0; st < 16; st++) p.add(Note::make(st * 24, 12, 36 + (st % 4), 90)); }
      else if (cat != Vocal) { for (int st = 0; st < 8; st++) { p.add(Note::make(st * 48, 40, 48 + (st * 5) % 24, 100)); if (cat == Chord || cat == Pad) { p.add(Note::make(st * 48, 40, 52 + (st * 5) % 24, 90)); p.add(Note::make(st * 48, 40, 55 + (st * 5) % 24, 90)); } } }
      p.sortByStart(); g_engine.endNextWrite(t);
      Cmd c; c.type = CmdSwapPattern; c.track = (uint8_t)t; g_engine.cmds().push(c);
      Cmd pr; pr.type = CmdSetParam; pr.track = (uint8_t)t; pr.a = PEcho; pr.f = 0.3f; g_engine.cmds().push(pr); pr.a = PReverb; pr.f = 0.3f; g_engine.cmds().push(pr); pr.a = PDist; pr.f = 0.4f; g_engine.cmds().push(pr); pr.a = PFilter; pr.f = -0.3f; g_engine.cmds().push(pr);
    }
    g_demoSlot = 0;                                   // kit slots are staged one per loop iteration (ack-driven) in loop()
  }
  else if (!strncmp(line, "play", 4)) { Cmd ps; ps.type = CmdPlaySet; ps.a = 0; g_engine.cmds().push(ps); ps.a = 1; g_engine.cmds().push(ps); }
  else if (!strncmp(line, "ls ", 3)) {              // list a card directory
    IFileSystem& fs = g_fs; struct Ctx { int n; } ctx{0};
    fs.walk(line + 3, [](const char* name, bool isDir, uint32_t size, void* user) -> bool { Serial.printf("  %s %u %s\n", isDir ? "D" : "F", (unsigned)size, name); ((Ctx*)user)->n++; return true; }, &ctx);
    Serial.printf("[ls] %d entries\n", ctx.n);
  }
  else if (!strncmp(line, "cat ", 4)) {             // print the first 2 KB of a card file (the shared scratch buffer)
    IFile* f = g_fs.open(line + 4, "r"); if (!f) { Serial.println("[cat] not found"); }
    else { char* buf = g_scratch; size_t n = f->read(buf, sizeof g_scratch - 1); f->close(); buf[n] = 0; Serial.println(buf); Serial.println("[cat] end"); }
  }
  else if (!strncmp(line, "get ", 4)) { serialGet(line + 4); }     // SPEC §9.22: pull a card file (binary-safe, see serialGet)
  else if (!strcmp(line, "drive")) { usbDrive(); }                   // SPEC §9.22: reboot into the USB drive firmware (as SHIFT + gen pattern held 1 s); usbDrive refuses while a session renders and stops a recording first (SPEC §9.23)
  else if (sscanf(line, "vol %d", &a) == 1) { g_ui.setOutputVolume(a); Serial.printf("[vol] %d\n", g_ui.outputVolume()); }
  else if (!strncmp(line, "stat", 4)) {
    const EngineStatus& st = g_engine.status();
    Serial.printf("[stat] vol=%d tick=%u bar=%u bpm=%u run=%u heap=%u largest=%u render_max_us=%u underruns=%u vocal_underruns=%u gen=%d/%d busy=%d err=%s lib=%s session=%s seq=%u svol=%d\n",
                  g_ui.outputVolume(), (unsigned)st.tick, (unsigned)st.bar, (unsigned)st.bpm, (unsigned)st.running, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)st.renderMicrosMax, (unsigned)st.underruns, (unsigned)st.vocalUnderruns, g_generator->progress(), g_generator->total(), (int)g_generator->busy(), g_generator->lastError(),
                  g_library->lastError(),    // card layout problems the library saw (shards beyond kTreeMax)
                  g_sets->sessionBusy() ? "busy" : "idle", (unsigned)g_sets->sessionSeq(), g_sets->sessionVolume());   // SPEC §9.18: write / restore running, lp.seq of the newest file, restored lp.volume (-1 none)
    // SPEC §9.19: per stream (0 / 1 the sets' vocal streams, 2 / 3 the loop slots) the underrun count and ring fill, the track bound to each slot (-1 free).
    Serial.printf("  streams: underruns=%u/%u/%u/%u ring=%d/%d/%d/%d slots=t%d/t%d anyloop=%d\n",
                  (unsigned)g_engine.streamUnderruns(0), (unsigned)g_engine.streamUnderruns(1), (unsigned)g_engine.streamUnderruns(2), (unsigned)g_engine.streamUnderruns(3),
                  g_engine.streamRing(0).available(), g_engine.streamRing(1).available(), g_engine.streamRing(2).available(), g_engine.streamRing(3).available(),
                  g_engine.loopTrack(0), g_engine.loopTrack(1), (int)g_generator->anyLoop());
    for (int t = 0; t < kTracks; t++) {        // per track: the acid / loop flags, the stream it renders (-1 none) and the six §9.17 knobs (0..1 targets, the effective cutoff in Hz)
      const TrackParams& p = g_engine.params(t); const SynthKnobs& k = g_engine.knobs(t);
      Serial.printf("  t%02d %s state=%d var=%d len=%u lvl=%u acid=%d loop=%d strm=%d cut=%.2f res=%.2f env=%.2f dec=%.2f drv=%.2f wave=%d cutHz=%.0f file=%s sound=%s\n", t, categoryShort(trackCategory(t)), (int)st.track[t].state, (int)st.track[t].variant, (unsigned)st.track[t].lenTicks, (unsigned)st.track[t].level,
                    (int)g_generator->acidEnabled(t), (int)g_generator->loopEnabled(t), g_engine.trackStream(t), p.cutoff, p.resonance, p.envAmt, p.decay, p.drive, (int)p.wave, k.cutoffHz,
                    g_generator->loopEnabled(t) ? g_generator->loopFile(t) : g_generator->info(t).file, g_generator->info(t).sound);
    }
    Serial.printf("  prof(us/block, last): synth=%u sample=%u strip=%u bus=%u\n", (unsigned)st.profSynth, (unsigned)st.profSample, (unsigned)st.profStrip, (unsigned)st.profBus);
    Serial.printf("  stacks free (words): render=%u worker=%u loop=%u\n", (unsigned)uxTaskGetStackHighWaterMark(g_renderTask), (unsigned)uxTaskGetStackHighWaterMark(g_workerTask), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    const_cast<EngineStatus&>(st).renderMicrosMax = 0;
  }
}
static void pollSerial() {
  static char line[128]; static int len = 0;   // 128: `get /lp/packs/<Genre>/presets/<name>` reaches 90+ characters (kSetNameMax 64)
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') { if (len) { line[len] = 0; handleSerialLine(line); } len = 0; }
    else if (len < (int)sizeof line - 1) line[len++] = ch;
  }
}

// /lp/config.txt: key=value lines (SPEC §6). Applied once after init; unknown keys are ignored.
static int axisIndex(const char* v, int* sign) { *sign = 1; if (*v == '-') { *sign = -1; v++; } if (*v == 'x' || *v == 'X') return 0; if (*v == 'y' || *v == 'Y') return 1; if (*v == 'z' || *v == 'Z') return 2; return -1; }
static void loadConfig() {
  IFile* f = g_fs.open("/lp/config.txt", "r"); if (!f) return;
  char* buf = g_scratch; size_t n = f->read(buf, sizeof g_scratch - 1); f->close(); buf[n] = 0;   // the shared scratch buffer (SPEC §9.19)
  int pitchAxis = 0, pitchSign = 1, rollAxis = 1, rollSign = 1;
  char* line = buf;
  while (line && *line) {
    char* nl = strchr(line, '\n'); if (nl) *nl = 0;
    char* hash = strchr(line, '#'); if (hash) *hash = 0;
    char* eq = strchr(line, '=');
    if (eq) {
      *eq = 0; char* key = line; char* val = eq + 1;
      while (*key == ' ') key++; while (*val == ' ') val++;
      for (char* e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\r'); e--) e[-1] = 0;
      for (char* e = val + strlen(val); e > val && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\t'); e--) e[-1] = 0;
      if (!strcmp(key, "bpm")) { int b = atoi(val); if (b >= kMinBpm && b <= kMaxBpm) { Cmd c; c.type = CmdSetTempo; c.i = b; g_engine.cmds().push(c); g_sets->setConfigBpm(b); } }   // the config tempo also wins over a restored session (SPEC §9.19f)
      else if (!strcmp(key, "echo.division")) { Cmd c; c.type = CmdEchoDivision; c.i = atoi(val); g_engine.cmds().push(c); }
      else if (!strcmp(key, "duck")) { Cmd c; c.type = CmdDuck; c.a = atoi(val) ? 1 : 0; g_engine.cmds().push(c); }
            else if (!strcmp(key, "volume")) { char* end = nullptr; long v = strtol(val, &end, 10); if (end != val && *end == 0 && v >= 0 && v <= 255) g_ui.setOutputVolume((int)v); }
      else if (!strcmp(key, "imu.pitch_axis")) { int sg; int a = axisIndex(val, &sg); if (a >= 0) { pitchAxis = a; pitchSign = sg; } }
      else if (!strcmp(key, "imu.roll_axis")) { int sg; int a = axisIndex(val, &sg); if (a >= 0) { rollAxis = a; rollSign = sg; } }
      Serial.printf("[config] %s=%s\n", key, val);
    }
    line = nl ? nl + 1 : nullptr;
  }
  g_ui.setImuMapping(pitchAxis, pitchSign, rollAxis, rollSign);
}

static void* bootBlock(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }   // internal RAM only

// Battery readout for the footer (SPEC §4), polled every kBatteryPollMs. On the Cardputer M5Unified's level
// (0..100, negative = unknown) is one ADC read of the battery divider mapped at 8 mV per percent, which wanders
// by a few points under the speaker's and the SD card's load, so a poll averages kBatterySamples reads and a
// 1-point change reaches the Ui only when the next poll repeats it (a larger change at once). The charge state
// goes through as reported (is_charging / is_discharging / charge_unknown → 1 / 0 / −1) — on the Cardputer
// M5Unified has no charger status line and always answers charge_unknown, so "CHG" never shows there and the
// low-battery red stays on while the unit is plugged in below 15 %.
static constexpr uint32_t kBatteryPollMs = 5000;
static constexpr int kBatterySamples = 8;
static int g_batLevel = -1, g_batPending = -1;   // the level last handed to the Ui; a 1-point change seen once, waiting for the next poll
static void pollBattery() {
  int sum = 0, n = 0;
  for (int i = 0; i < kBatterySamples; i++) { const int32_t l = M5.Power.getBatteryLevel(); if (l >= 0) { sum += (int)l; n++; } }
  int level = n ? (sum + n / 2) / n : -1;
  if (level >= 0 && g_batLevel >= 0 && (level == g_batLevel + 1 || level == g_batLevel - 1)) {   // hysteresis against ADC jitter
    if (g_batPending == level) g_batPending = -1;                      // the same step on two polls in a row: accepted
    else { g_batPending = level; level = g_batLevel; }                 // first sighting: the shown level stays
  } else g_batPending = -1;                                            // unchanged, a bigger step or unknown: nothing pending
  g_batLevel = level;
  const m5::Power_Class::is_charging_t chg = M5.Power.isCharging();
  g_ui.setBattery(level, chg == m5::Power_Class::is_charging ? 1 : chg == m5::Power_Class::is_discharging ? 0 : -1);
}

static void renderTask(void*) {
  int which = 0;
  for (;;) {
    if (g_recorder.rendering()) {                  // OFFLINE RENDER (SPEC §9.23): parked — the loop task renders through Recorder::renderStep into g_audio[0];
      g_renderParked.store(true, std::memory_order_release);   // the speaker keeps getting blocks (silence, alternating buffers 1 / 2) so it never runs dry
      int16_t* buf = g_audio[1 + (which & 1)]; memset(buf, 0, sizeof g_audio[0]);
      while (!M5Cardputer.Speaker.playRaw(buf, kBlock, kSampleRate, false, 1, 0)) vTaskDelay(1);
      which = (which + 1) % 3;
      continue;
    }
    g_renderParked.store(false, std::memory_order_release);
    int16_t* buf = g_audio[which];                 // the buffer two blocks back has been consumed once the previous playRaw returned
    uint32_t t0 = micros();
    g_engine.render(buf, kBlock);
    uint32_t dt = micros() - t0;
    if (dt > g_engine.status().renderMicrosMax) const_cast<EngineStatus&>(g_engine.status()).renderMicrosMax = dt;
    if (!M5Cardputer.Speaker.isPlaying(0)) const_cast<EngineStatus&>(g_engine.status()).underruns++;   // queue ran dry before this block: audible gap
    while (!M5Cardputer.Speaker.playRaw(buf, kBlock, kSampleRate, false, 1, 0)) vTaskDelay(1);
    which = (which + 1) % 3;
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = false;
  cfg.fallback_board = m5::board_t::board_M5CardputerADV;
  Serial.begin(115200);
  M5Cardputer.begin(cfg, true);
  g_gfx.begin();

  auto spk = M5Cardputer.Speaker.config();
  spk.sample_rate = kSampleRate; spk.stereo = false; spk.dma_buf_len = kBlock; spk.dma_buf_count = 4;
  spk.task_priority = 3; spk.task_pinned_core = 1;
  M5Cardputer.Speaker.config(spk);
  M5Cardputer.Speaker.begin();                   // the output volume is applied by the Ui's hook below (default kOutputVolumeDefault = 150, config `volume=`, SHIFT + kick cut)
  g_sdOk = g_fs.begin();                         // FIRST: the mount preallocates SdFs::kMaxOpen FatFs FILs (≈ 4.2 KB each) in ONE block, which must fit the largest free block before the boot pools fragment the heap (SPEC §9.19 §2)

  void* pl = bootBlock(sizeof(Library)); void* pg = bootBlock(sizeof(Generator)); void* ps = bootBlock(sizeof(Sets));
  if (!pl || !pg || !ps) {                        // cannot run without them: say so and stop (SPEC §9.13)
    Serial.printf("[boot] out of RAM: free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    g_gfx.drawText(8, 60, "NO RAM FOR LOADER", 2, kRed);
    for (;;) delay(1000);
  }
  g_library = new (pl) Library(); g_generator = new (pg) Generator(); g_sets = new (ps) Sets();   // constructed in place, never freed
  // Loop slot rings (SPEC §9.19): 2 × kLoopRingBytes from the same boot-time pool, paid for by the static reclaim (µ-law
  // echo line, one scratch buffer, trimmed task stacks). A slot without memory stays silent (Engine::setLoopRing).
  bool loopRam = true;
  for (int k = 0; k < kLoopSlots; k++) { g_loopRing[k] = (int16_t*)bootBlock(kLoopRingBytes); if (!g_loopRing[k]) loopRam = false; }

  ulawInit();
  lp::g_microsHook = []() -> uint32_t { return (uint32_t)micros(); };
  g_engine.init();
  for (int k = 0; k < kLoopSlots; k++) if (g_loopRing[k]) g_engine.setLoopRing(k, g_loopRing[k], kLoopRingBytes / 2);
  g_library->init(&g_fs);
  g_generator->init(&g_fs, &g_engine, g_library);
  g_generator->setFreeHeapHook([]() -> uint32_t { return (uint32_t)ESP.getFreeHeap(); });   // SPEC §9.19: loop mode is refused below kLoopLowRamBytes
  g_sets->init(&g_fs, &g_engine, g_generator, g_library);   // the library: the session restore's kit fallback picks (SPEC §9.18)
  g_ui.init(&g_gfx, &g_engine, g_generator, g_sets);
  g_ui.setOutputVolumeHook([](int v) { M5Cardputer.Speaker.setVolume((uint8_t)v); });   // master output volume (SPEC §3): applied now and on every change
  g_ui.setUsbDriveHook([]() { usbDrive(); });                                            // SPEC §9.22: SHIFT + gen pattern held 1 s → the USB drive firmware in ota_1 (refused inside while a session renders, SPEC §9.23)
  g_recorder.init(&g_fs, &g_engine, g_generator, g_sets, &g_ui);                         // SPEC §9.23: the session recorder, after the Ui (it replays through Ui::onKey)
  g_ui.setRecorder(&g_recorder);                                                         // the Ui records every key / gyro / volume event while recording and drives the RECORDER page
  if (!loopRam) { Serial.printf("[boot] no RAM for the loop rings: free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap()); g_ui.showStatus("NO RAM FOR LOOPS", 4000); }
  if (!g_sdOk) g_ui.showStatus("NO SD CARD", 3000);
  else {
    loadConfig();
    g_sets->restoreSession();                    // SPEC §9.18: the last session comes back (runs in Sets::step once the generator is idle, nothing starts playing)
  }                                              // and the writer is armed; config.txt volume= / serial vol are explicit and win over the restored lp.volume (§9.17c)
  pollBattery();                                 // first footer readout now; loop() refreshes it every kBatteryPollMs

  // Stacks (SPEC §9.19): measured high-water marks render 2 336 B used of 3 584, worker 1 892 of 3 072 (2026-10-16); the trim
  // to 3 072 / 2 560 keeps ≥ 1.2 KB of margin over each and gives 1 KB back to the heap (`stat` prints the free words).
  xTaskCreatePinnedToCore(workerTask, "render2", 2560, nullptr, 6, &g_workerTask, 1);   // set 2 voices on core 1 (SPEC §9.13)
  lp::g_parallelStart = parallelStart; lp::g_parallelJoin = parallelJoin;
  xTaskCreatePinnedToCore(renderTask, "render", 3072, nullptr, 5, &g_renderTask, 0);
  uint32_t freeB = ESP.getFreeHeap(), largest = ESP.getMaxAllocHeap();
  Serial.printf("[boot] heap free=%u largest=%u sd=%d\n", (unsigned)freeB, (unsigned)largest, (int)g_sdOk);
  if (freeB < 8 * 1024 || largest < 6 * 1024) {    // SPEC §9.13 / §9.19 / §9.22 / §9.24: free >= 8 KB (10 KB before the 2026-10-16 RAM diet, 12 KB before that wave's 3 KB allowance; opens cost no heap since the POSIX SD layer — the only runtime heap users are FatFs' LFN scratch, ≈ 1 KB per open) and largest >= 6 KB after task creation (7 preallocated FatFs slots = 29 KB live in this heap)
    char m[40]; snprintf(m, sizeof m, "LOW RAM %uK FREE %uK MAX", (unsigned)(freeB / 1024), (unsigned)(largest / 1024));
    g_ui.showStatus(m, 6000);
  }
}

static uint32_t g_maxPumpUs = 0, g_maxGenUs = 0, g_maxSetsUs = 0;   // longest pumpVocal / Generator::step / Sets::step of the current second (SD latency diagnostics)
static int g_maxGenState = 0;                                       // Generator::stateCode() the longest step started in (SPEC §9.19g: which card operation it was)

void loop() {
  uint32_t now = millis();
  { uint32_t u0 = micros(); g_generator->pumpVocal(); uint32_t d = micros() - u0; if (d > g_maxPumpUs) g_maxPumpUs = d; }                // vocal rings first: refilled before any other SD work of this iteration (SPEC §9.2)
  // Keyboard: drain the TCA8418 FIFO (one event per updateKeyList call, SPEC §1), then diff the matrix.
  M5Cardputer.update();
  for (int i = 0; i < 7; i++) M5Cardputer.Keyboard.updateKeyList();
  pollSerial();
  g_keys.beginScan();
  for (auto& p : M5Cardputer.Keyboard.keyList()) g_keys.markPressed(p.x, p.y);
  for (int y = 0; y < kKeyRows; y++) { uint16_t bits = g_virtualKeys[y] | g_virtualPulse[y]; for (int x = 0; x < kKeyCols; x++) if (bits & (1u << x)) g_keys.markPressed(x, y); }
  static bool pulseHeld = false;
  if (pulseHeld) { for (int y = 0; y < kKeyRows; y++) g_virtualPulse[y] = 0; pulseHeld = false; } else { for (int y = 0; y < kKeyRows; y++) if (g_virtualPulse[y]) pulseHeld = true; }
  static KeyEvent evs[16];
  int n = g_keys.endScan(now, evs, 16);
  // OFFLINE RENDER (SPEC §9.23): while a session renders, the Ui lives on the recorder's clock (renderStep ticks it with the
  // rendered time; the replayed keys carry it), so the live keys, the IMU and draw() get the same clock — mixed clocks would
  // misfire the Ui's hold timers. It falls back to millis() when the render ends. Of the LIVE keys only ` (0,0) reaches the
  // Ui meanwhile — its hold is the cancel (KEYMAP "RENDERING: hold ` 1 s"); every other key would land on the page the
  // replay drives and change the rendered audio without a trace. (A release after the render of a key pressed during it
  // is a stray release the Ui ignores.)
  const bool rendering = g_recorder.rendering();
  const uint32_t uiNow = rendering ? g_recorder.renderNowMs() : now;
  for (int i = 0; i < n; i++) { if (rendering && !(evs[i].x == 0 && evs[i].y == 0)) continue; g_ui.onKey(evs[i], uiNow); }
  if (M5Cardputer.BtnA.wasPressed()) {
    if (rendering) { g_recorder.cancelRender(); Serial.println("[rec] render cancelled (BtnA)"); }   // the side button aborts an offline render (the partial WAV is removed)
    else {
      const uint8_t run = g_engine.status().running ? 0 : 1;
      Cmd c; c.type = CmdMasterPlay; c.a = run; g_engine.cmds().push(c);
      if (g_recorder.recording()) { RecEvent e; e.tick = 0; e.type = 0; e.x = 255; e.y = run; e.extra = 0; g_recorder.onInput(e); }   // SPEC §9.23: master play / stop as the pseudo key (255, run)
    }
  }

  static uint32_t lastImu = 0;
  if (now - lastImu >= 20) {
    lastImu = now;
    ImuReading r; r.valid = M5.Imu.getAccel(&r.ax, &r.ay, &r.az); M5.Imu.getGyro(&r.gx, &r.gy, &r.gz);
    if (!rendering) g_ui.onImu(r, uiNow);        // SPEC §9.23: during an offline render the gyro FX come from the recorded type 2 events, not from the device lying on the table
  }
  if (g_demoSlot >= 0 && g_demoSlot < 16 && g_engine.status().slotAck == g_demoAck) {   // demo: stage one noise burst per slot
    int set = g_demoSlot / 8, slot = g_demoSlot % 8; uint8_t* st = g_engine.staging(); uint32_t len = 2500; uint32_t seed = 12345u + g_demoSlot;
    for (uint32_t i = 0; i < len; i++) { seed = seed * 1664525u + 1013904223u; float env = 1.0f - (float)i / len; int16_t v = (int16_t)(((int32_t)(seed >> 16) - 32768) * env * 0.6f); st[i] = ulawEncode(v); }
    Cmd c; c.type = CmdSlotSwap; c.a = (uint8_t)set; c.i = slot | (int32_t)(len << 4); c.f = 0; g_engine.cmds().push(c);
    g_demoAck = g_engine.status().slotAck + 1; g_demoSlot++;
    if (g_demoSlot == 16) g_demoSlot = -1;
  }
  { const int st = g_generator->stateCode(); uint32_t u0 = micros(); g_generator->step(); uint32_t d = micros() - u0; if (d > g_maxGenUs) { g_maxGenUs = d; g_maxGenState = st; } }
  { uint32_t u0 = micros(); g_sets->step(); uint32_t d = micros() - u0; if (d > g_maxSetsUs) g_maxSetsUs = d; }   // step timing: the longest card operations of the second ([diag])                                  // save / load state machine: one bounded SD action per loop (SPEC §9.2)
  g_recorder.step();                                                                                    // SPEC §9.23: the event buffer's one bounded write when the card is free
  static uint32_t lastBat = 0;
  if (now - lastBat >= kBatteryPollMs) { lastBat = now; pollBattery(); }   // footer battery readout (SPEC §4)
  // OFFLINE RENDER (SPEC §9.23): once the render task has parked, one block per loop() — Recorder::renderStep replays the due
  // events through Ui::onKey, renders one engine block into g_audio[0] (the parked task plays silence from buffers 1 / 2),
  // writes it to the WAV, pumps the streams, steps the generator and Sets and ticks the Ui on the render clock. The key scan,
  // IMU and the generator / sets steps above keep running between blocks. Until the task has parked nothing is rendered.
  if (rendering) { if (g_renderParked.load(std::memory_order_acquire)) g_recorder.renderStep(g_audio[0]); }
  else g_ui.tick(now);
  g_ui.draw(uiNow);
  static uint32_t lastLog = 0;
  if (now - lastLog >= 1000) {                        // one diagnostic line per second (render max is reset each time)
    lastLog = now; const EngineStatus& st = g_engine.status();
    if (g_recorder.rendering()) Serial.printf("[rec] rendering %d%%\n", g_recorder.renderPercent());   // SPEC §9.23 offline render progress
    int avail0 = g_engine.vocalRing(0).available(), avail1 = g_engine.vocalRing(1).available();
    int avail2 = g_engine.loopRing(0).available(), avail3 = g_engine.loopRing(1).available();   // SPEC §9.19 loop slots
    Serial.printf("[diag] t=%us render_max_us=%u underruns=%u vocal_underruns=%u loop_underruns=%u/%u ring0=%d ring1=%d ring2=%d ring3=%d slots=t%d/t%d pump=%d skipped=%u heap=%u largest=%u lvl6=%u lvl14=%u files=%d pump_max_us=%u gen_max_us=%u gen_max_state=%d sets_max_us=%u\n",
                  (unsigned)(now / 1000), (unsigned)st.renderMicrosMax, (unsigned)st.underruns, (unsigned)st.vocalUnderruns,
                  (unsigned)g_engine.streamUnderruns(kSets), (unsigned)g_engine.streamUnderruns(kSets + 1), avail0, avail1, avail2, avail3, g_engine.loopTrack(0), g_engine.loopTrack(1),
                  g_generator->lastPumpChunks(), (unsigned)g_generator->stepsSkipped(),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)st.track[6].level, (unsigned)st.track[14].level,
                  g_fs.openCount(), (unsigned)g_maxPumpUs, (unsigned)g_maxGenUs, g_maxGenState, (unsigned)g_maxSetsUs);
    g_maxPumpUs = g_maxGenUs = g_maxSetsUs = 0; g_maxGenState = 0;
    const_cast<EngineStatus&>(st).renderMicrosMax = 0;
  }
  delay(1);
}
