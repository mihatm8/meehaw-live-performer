// meehaw live performer — UI controller (SPEC §3, §4, §9.10, §9.14, §9.17c, §9.19 loop mode, §9.22 preset manager /
// USB drive / 303 knobs, §9.23 recorder, §9.24 theory generator on SHIFT + load 1 / 2, §9.25 random-load cycle marks):
// consumes key matrix events and IMU readings, drives the pages (splash, main,
// parameter / note / status overlays, editor, perform, synth, preset manager, recorder) and draws on an IGfx. Runs on the
// loop task next to the generator; talks to the engine only through the command queue, the status snapshot and the
// UI-owned source patterns. No heap, no std containers; the only large member is the looper's recording buffer (one
// Pattern, 772 B). Redraws are diffed against shadow copies and throttled to 40 ms.
#pragma once
#include <math.h>                                   // types.h's knob mappings use powf / logf
#include "../core/types.h"
#include "../io/keys.h"
#include "gfx.h"
#include "palette.h"
#include "../engine/engine.h"
#include "../gen/generator.h"
#include "../gen/sets.h"
#include "../gen/variants.h"                        // Rng (set loads draw from it)

namespace lp {

class Recorder;                                     // rec/recorder.h (SPEC §9.23): handed over with setRecorder, used in ui.cpp only

struct ImuReading { float ax = 0, ay = 0, az = 1, gx = 0, gy = 0, gz = 0; bool valid = false; };

// Pages. PgParam / PgNote / PgStatus are overlays over the main page (all main keys stay live, SPEC §9.14);
// PgEditor, PgPerform, PgSynth, PgPresets (SPEC §9.22 preset manager) and PgRecorder (SPEC §9.23) are modal. PgStatus
// is kept for API compatibility: the status line is an overlay flag, page() never returns it. The RENDERING / DONE
// screens of an offline render are overlays over whatever page the replay drives (rendering(), renderDoneShowing()).
enum Page : uint8_t { PgSplash = 0, PgMain, PgParam, PgStatus, PgEditor, PgPerform, PgNote, PgSynth, PgPresets, PgRecorder };

// Full-screen parameter pages (SPEC §4). The first ParamCount coincide with ParamId (PPMaster = the output volume,
// PCutoff..PWave = the synth knobs, which have no page of their own: the synth page shows them).
enum PageParam : uint8_t { PPVolume = PVolume, PPReverb = PReverb, PPEcho = PEcho, PPDist = PDist, PPFilter = PFilter,
                           PPLength = PLength, PPTempo = PTempo, PPKey = PKey, PPMaster = PMaster,
                           PPGenre = ParamCount, PPRampBars, PageParamCount };
static_assert(PageParamCount == ParamCount + 2, "kPageParamColor / pageParamName are sized by PageParamCount");

// Perform-page looper (SPEC §9.10): del arms (count-in to the next bar), records, closes at a bar line rounded
// up to 1/2/4 bars, further presses toggle overdub, a 700 ms hold clears.
enum LooperState : uint8_t { LpIdle = 0, LpArmed, LpRecording, LpClosing, LpLooping, LpOverdub };

// ---- layout (SPEC §4, §9.14) ----
constexpr int kScreenW = 240, kScreenH = 135;
constexpr int kCellSize = 26, kCellGap = 4, kCellPitch = kCellSize + kCellGap;
constexpr int kCellX0 = 2;                          // 8 × 26 + 7 × 4 = 236 px centred on 240 (x = 10 would run off the right edge)
constexpr int kRowY[kSets] = {24, 76};
constexpr int kCellTickY = kCellSize + 2;           // variant ticks under the selected square (SPEC §9.14)
constexpr int kCellBoxH = kCellSize + 6;            // square + tick row, cleared together
constexpr int kLabelDy = 10;                        // category labels sit this far above each row
constexpr int kFooterY = kScreenH - 9;              // 9 px footer line (SPEC §4)
constexpr int kFooterTextX = 2, kFooterGap = 4, kFooterBatMargin = 1;   // footer: left text x, least gap before the battery readout, its right margin
constexpr int kBatteryLowPct = 15;                  // below this (and not charging) the battery readout turns kBatteryLow (SPEC §4)
constexpr int kStatusY = 112, kStatusH = 12;        // status overlay line (between the second row and the footer)
constexpr int kEdCell = 12, kEdX0 = 40, kEdY0 = 14; // editor grid: 16 steps × 12 px
constexpr int kPerfCellW = 22, kPerfCellH = 20, kPerfX0 = 10, kPerfY0 = 14;   // perform grid 10 × 4
// ---- synth page (SPEC §9.17c, §9.22): nine knobs in two rows (CUT RES ENV DEC DRV / WAV SAT ACC CHO), each an arc of 270° ----
// Nine knobs in one row would leave 26 px per knob on the 240 px screen: a 6-character value ("1.5oct", 36 px) would
// overlap its neighbours, so the page uses two rows of 48 px pitch (row 2 holds four knobs, centred).
constexpr int kSynthKnobs = 9, kKnobCols = 5;
constexpr int kKnobPitch = 48, kKnobX0 = 24, kKnobX0Row2 = 48, kKnobR = 11;
constexpr int kKnobCy = 25, kKnobRowPitch = 44;    // row centres y 25 / 69; a row's box spans cy − 12 … cy + 31
constexpr int kKnobBoxDy = -12, kKnobBoxH = 44, kKnobLabelDy = 14, kKnobValueDy = 23;
// ---- preset manager (SPEC §9.22, remapped by §9.24): SHIFT + save 2 (10,3); the TARGET SET is toggled by the track rows ----
constexpr int kPresetRowsVisible = 8, kPresetRowY0 = 14, kPresetRowH = 12;   // rows y 14 … 110, status line at kStatusY, help in the footer
constexpr int kPresetScrollPage = 8;                // a new Sets page (kPresetPage rows) is requested from a multiple of this
constexpr int kPresetLabelMax = 19;                 // lp.label (Sets::PresetRow::label is 20 bytes)
constexpr int kPresetTextX = 8;                     // row text; the random-load cycle dot (SPEC §9.25) sits in the margin before it
constexpr int kPresetDotX = 2, kPresetDotDy = 3, kPresetDotSize = 3;   // the dot: a kPresetDotSize square at (kPresetDotX, row y + kPresetDotDy)
constexpr uint32_t kDeleteConfirmMs = 2000;         // DELETE? PRESS AGAIN window (preset manager and recorder page)
// The header's target arrow: the contract writes "→ SET n"; neither the device's built-in font nor the desktop 5x7 font has
// U+2192, so "->" stands in (as the hyphen of kLoopTempoStatusFmt does for the middle dot).
constexpr const char* kPresetTargetPrefix = "->";
// ---- theory generator (SPEC §9.24): SHIFT + load 1 (11,2) / load 2 (11,3) = Generator::generateTheorySet(set) ----
constexpr const char* kTheoryLabelFmt = "THEORY S%d";   // the job's progress label ("THEORY S1 3/8"), as "GEN S1" is GENERATE's
// ---- recorder page (SPEC §9.23): SHIFT + save 1 ----
constexpr int kRecRowsVisible = 8;
constexpr uint32_t kRenderCancelHoldMs = 1000;      // ` held this long on the RENDERING overlay cancels the render (a replayed tap never does)
constexpr uint32_t kRenderDoneMs = 3000;            // "DONE /lp/wav/rec_NNN.wav" stays this long
// ---- USB drive (SPEC §9.22): SHIFT + gen pattern held ----
constexpr uint32_t kUsbDriveHoldMs = 1000, kUsbDriveStatusMs = 100;   // status "USB DRIVE..." first, the hook (a reboot) kUsbDriveStatusMs later so it reaches the screen
// ---- timing ----
constexpr uint32_t kSplashMs = 1500, kNotePageMs = 600, kStatusMs = 1000, kDrawIntervalMs = 40, kBlinkHalfMs = 250;
constexpr uint32_t kGyroSendMs = 20, kLooperHoldMs = 700, kKickCutSyncMs = 200;
constexpr int kSwapLeadTicks = 8;                   // bar lines are acted on this many ticks early so a publish lands on them (≈ 40 ms at 120 BPM)
// ---- gyro mapping (SPEC §3, §9.11) ----
constexpr float kGyroDeadDeg = 10.0f, kGyroRangeDeg = 45.0f;
constexpr float kGyroSmooth = 0.2835f;              // 1 − e^(−20 ms / 60 ms): one-pole τ 60 ms at 50 Hz
// ---- perform / looper ----
constexpr int kPerfCols = 10, kPerfRows = 4, kPerfOctaveDefault = 3, kLooperMaxBars = 4;
constexpr int kPerfRowOffset[kPerfRows] = {1, 1, 2, 3};   // first grid column of each row (SPEC §9.10)
constexpr float kPerfVelocity = 0.8f;
constexpr int kPerfDrumSlots = 8;                   // Beat: row y0 keys x1..x8 = pitches 36..43 (Percussion: the first four, SPEC §9.17c)
constexpr int kPerfQuantSteps = 4;                  // OFF, 1/16, 1/8, 1/4
constexpr int kPerfPendMax = 8;                     // quantised note-ons waiting for their grid point
constexpr int kRampBarsSteps = 5;                   // 1 2 4 8 16
// ---- tempo drift / octave (SPEC §9.17c) ----
constexpr int kTempoDriftBpm = 10, kTempoDriftBeats = 60;
// ---- master output volume (SPEC §3: SHIFT + kick cut S1 = up, S2 = down; headphones) ----
constexpr int kOutputVolumeDefault = 150, kOutputVolumeStep = 16, kOutputVolumeMax = 255;
constexpr uint32_t kOutputFooterMs = 3000;          // the footer shows "OUT 78%" this long after the last step
// ---- loop mode (SPEC §9.19): SHIFT + edit toggles Generator::setLoopMode on the selected track ----
// The tempo-to-125 status reads "LOOP ON - 125 BPM" (the contract writes a middle dot; neither the device's built-in
// font nor the desktop 5x7 font has that glyph, so a hyphen stands in). Format: kLoopTempoStatusFmt with kLoopBpm.
constexpr const char* kLoopTempoStatusFmt = "LOOP ON - %d BPM";

class Ui {
 public:
  void init(IGfx* gfx, Engine* eng, Generator* gen, Sets* sets);
  void onKey(const KeyEvent& e, uint32_t nowMs);
  void onImu(const ImuReading& r, uint32_t nowMs);
  void tick(uint32_t nowMs);                        // timers, page auto-return, queued presses, gyro hold, looper bar lines, status text
  // The loop (sketch / desktop) steps Generator and Sets itself; tick() only polls them. One owner per SD state
  // machine keeps every loop iteration to one bounded card action (SPEC §9.2).
  void draw(uint32_t nowMs);                        // redraws only what changed, at most every kDrawIntervalMs
  int selectedTrack() const { return selected_; }
  bool shiftDown() const { return shift_; }
  Page page() const { return page_; }
  void showStatus(const char* text, uint32_t ms = kStatusMs);
  // ---- loop mode (SPEC §9.19) ----
  // The main-page square of a loop track blinks while its loop pick is in flight: loop mode is requested and the
  // engine has not adopted the stream yet (pick on the card, then the bar line), or a generate re-picks the loop of a
  // track that already streams. loopUnderruns() is what the footer's "LOOP UNDERRUNS n" counts: the loop slots'
  // Engine::streamUnderruns since loop mode came into use (the baseline moves while no loop is in use); the engine
  // counts samples, so loopUnderrunText writes n in thousands from 10 000 ("13k") and the note always fits the footer.
  bool loopPickPending(int track) const;
  uint32_t loopUnderruns() const;
  static void loopUnderrunText(char* out, int cap, uint32_t n);
  // The status line of a refused SHIFT + edit: "LOOP SLOTS FULL (2)", "NO LOOP: DRUM" (the category's loop folder),
  // "LOOP: BUSY", "LOOP: LOW RAM", "LOOP: NO CARD".
  static void loopRefusalText(Generator::LoopRefusal r, Category c, char* out, int cap);
  // IMU axis mapping (SPEC §3: lp/config.txt imu.* until confirmed on the device). axis 0 = x, 1 = y, 2 = z, sign ±1;
  // the remaining axis is "up". Default pitch = +x (forward tilt positive → HP), roll = +y (left lean negative →
  // reverb). Invalid combinations (same axis) are ignored; while the gyro is on the neutral pose is re-captured.
  void setImuMapping(int pitchAxis, int pitchSign, int rollAxis, int rollSign);
  int imuPitchAxis() const { return imuPitchAxis_; }
  int imuPitchSign() const { return imuPitchSign_; }
  int imuRollAxis() const { return imuRollAxis_; }
  int imuRollSign() const { return imuRollSign_; }
  // Master output volume 0..255 (default kOutputVolumeDefault = 150, 59 %; SPEC §3 wrote 200), the headphone level: while SHIFT is held, kick cut S1
  // raises it by 16 and kick cut S2 lowers it (hold repeats); without SHIFT the keys keep toggling the kick cut.
  // The platform applies it through the hook (Speaker.setVolume on the device, a linear gain on the desktop):
  // called whenever the value changes, once when the hook is installed and once by init() with the current
  // value. setOutputVolume applies a config.txt / serial `vol` value (no page). Every explicit level (this,
  // SHIFT + kick cut) marks the session dirty so the session file carries it (SPEC §9.18) and wins over the
  // lp.volume of a session restored later in the same boot; without one the restored level is applied.
  // Configuration like the IMU mapping: init() does not reset the level.
  void setOutputVolumeHook(void (*fn)(int vol255));
  void setUsbDriveHook(void (*fn)()) { usbDriveHook_ = fn; }   // SPEC §9.22: SHIFT + gen pattern held 1 s reboots into the USB drive firmware (the sketch switches the OTA boot slot); null = "NO DRIVE FIRMWARE"
  void setOutputVolume(int vol255);
  int outputVolume() const { return outVol_; }
  // Battery readout (SPEC §4): the platform reports the level every few seconds (the sketch polls M5.Power, the
  // desktop reports a full one) and the main page's footer shows it right-aligned at the right edge: "87%",
  // "CHG 87%" while charging, "--" when the level is unknown (charging or not); the footer's dim grey,
  // kBatteryLow below kBatteryLowPct unless charging. percent 0..100 (clamped) or −1 unknown; charging
  // 1 / 0 / −1 unknown (the Cardputer's M5Unified reports no charge state, so CHG never shows there).
  // The readout is repainted only when its own text changes; the left footer text is fitted to the room
  // before it by whole segments (never cut to a partial number). Not configuration: init() resets it to unknown.
  void setBattery(int percent, int charging);
  int batteryPercent() const { return batPct_; }
  int batteryCharging() const { return batChg_; }
  // Session recorder (SPEC §9.23): the sketch hands its Recorder over once; null = no recorder (SHIFT + save 1 answers
  // "NO RECORDER"). While Recorder::recording() every key press / release the Ui receives outside the recorder page
  // goes to Recorder::onInput as type 0 / 1 with the engine tick (repeats are not recorded: KeyEvent::Repeat has no
  // event type), every CmdGyro as type 2 (x = filter × 127, y = reverb, extra = echo) and every output volume change
  // as type 3 (extra = level). An offline render replays those events through onKey (Recorder::replaying() while it does):
  // the pages follow the replay exactly as live, except that a replayed save / SHIFT + save 1 / drive hold / preset delete or
  // label writes nothing and reboots nothing, and a replayed ` (held or not) is a track key that never cancels. The RENDERING
  // nn% overlay covers the pages; a LIVE key meanwhile never reaches them (it would change the replayed world): only ` HELD
  // kRenderCancelHoldMs (the cancel) and the physical SHIFT state (put back when the render ends) are taken from live keys.
  void setRecorder(Recorder* rec) { recorder_ = rec; }
  Recorder* recorder() const { return recorder_; }

  // ---- introspection (footer, tests) ----
  // preset manager (SPEC §9.22; SHIFT + save 2 since §9.24)
  int presetSet() const { return psSet_; }          // the TARGET SET the page loads into (0 / 1): any key of track row y0 (x 1–7; (0,0) is the ` exit) → 0, of row y1 → 1; kept across opens like the filter
  // SPEC §9.25: rows loaded in the current random-load cycle carry a dot — when Sets offers presetLoadedInCycle (resolved at
  // compile time in ui.cpp so the page builds before the sets owner lands the query and lights up when it does); false = no marks.
  static bool presetCycleMarks();
  int presetCursor() const { return psCursor_; }    // highlighted row (index into the filtered list)
  int presetFilterKey() const { return psFilter_; } // −1 = ALL, else the minor root the list is filtered to
  bool presetAuto() const { return psAuto_ != 0; }  // AUTO: a loaded preset starts its set at the bar
  bool presetRenaming() const { return psRename_ != 0; }
  const char* presetRenameText() const { return psText_; }
  int presetPageFirst() const { return psFirst_; }  // first index of the Sets page the rows come from (−1 none yet)
  // recorder page (SPEC §9.23)
  int recorderCursor() const { return rcCursor_; }
  int recorderSessions() const { return rcCount_; } // Recorder::sessionCount as last listed by the page
  bool renderDoneShowing() const { return rcDoneUntil_ != 0; }
  // USB drive (SPEC §9.22): 0 idle, 1 SHIFT + gen pattern held, 2 "USB DRIVE..." shown (hook pending), 3 hook called
  int usbDriveState() const { return usbState_; }
  bool gyroOn() const { return gyroOn_; }
  int rampBars() const { return rampBars_; }
  PageParam paramPage() const { return paramPage_; }
  LooperState looper() const { return looper_; }
  int perfOctave() const { return perfOctave_; }
  int perfTrack() const { return perfTrack_; }      // the track the perform page plays and the looper records (SPEC §9.17c)
  int perfSet() const { return trackSet(perfTrack_); }
  int perfQuant() const { return perfQuant_; }      // 0 OFF, 1 = 1/16, 2 = 1/8, 3 = 1/4
  bool perfChord() const { return perfChord_; }     // CHORD mode (Chord / Pad tracks): a grid key plays the diatonic triad
  int synthTrack() const { return synthTrack_; }    // the track the synth page edits
  int editorTrack() const { return edTrack_; }
  int editorCursorX() const { return edCursorX_; }
  int editorCursorY() const { return edCursorY_; }
  int editorOctave() const { return edOctave_; }
  int editorGate() const { return edGate_; }
  int editorBar() const { return edPage_; }
  const char* statusText() const { return statusWant(); }        // an explicit message while it lasts, else job progress
  bool statusVisible() const { return statusWant()[0] != 0; }
  const char* progressText() const { return progressOn_ ? progress_ : ""; }
  float gyroFilter() const { return gyroFilter_; }
  float gyroReverb() const { return gyroRev_; }
  float gyroEcho() const { return gyroEcho_; }
  uint16_t loopTicks() const { return loopTicks_; }
  const Pattern& loopPattern() const { return rec_; }
  // Pure helpers (also used by tests)
  static int perfDegree(int x, int y);              // grid key → degree 0..18, −1 when (x, y) is not a grid key
  static int roundUpBars(int bars);                 // 1 / 2 / 4 (SPEC §9.10)
  static uint16_t quantize16(uint32_t ticks);       // nearest 1/16 (24 ticks)
  static int quantTicks(int quant);                 // perform quantise step in ticks: 0 (OFF), 24, 48, 96
  static ParamId synthKnobParam(ParamId column);    // vol→cutoff, rev→resonance, echo→envAmt, dist→decay, filter→drive, len→wave, tempo→sat, key→accent (SPEC §9.22); ParamCount otherwise (CHO sits on the gyro / edit keys)
  static void knobValueText(ParamId knob, float v, char* out, int cap);   // real units: "440Hz" "2.5k" "35%" "1.5oct" "120ms" "1.2s" "SAW"; SAT / ACC / CHO in percent
  static const char* knobLabel(int knob);           // CUT RES ENV DEC DRV WAV SAT ACC CHO (knob 0..kSynthKnobs-1)
  static const char* waveName(int wave);            // SINE TRI SAW SQR FAT
  static void fitTextWidth(IGfx* gfx, char* text, int maxWidth, int size = 1);   // cuts text in place (trailing spaces trimmed) until it is at most maxWidth px wide

 private:
  // ---- wiring ----
  IGfx* gfx_ = nullptr; Engine* eng_ = nullptr; Generator* gen_ = nullptr; Sets* sets_ = nullptr;
  // ---- page state ----
  Page page_ = PgSplash; uint32_t pageUntil_ = 0; uint32_t lastNow_ = 0; bool started_ = false;
  bool fullRedraw_ = true; bool everDrawn_ = false; uint32_t lastDraw_ = 0;
  int selected_ = Beat; bool shift_ = false; int rampBars_ = kRampBarsDefault;
  uint32_t rampsActive_ = 0;                        // bit per ParamId (18 of them since SPEC §9.22, hence 32 bits): a SHIFT ramp was started and not yet cancelled
  uint32_t holdRamps_ = 0;                          // bit per ParamId: a hold-ramp runs until its key's release freezes it (SPEC §9.17c)
  uint32_t holdDir_ = 0;                            // bit per ParamId: the running hold-ramp goes up (only the key of that direction owns it)
  uint8_t holdTrack_[ParamCount];                   // the track each hold-ramp was started on
  uint32_t driftUntilTick_ = 0;                     // a SHIFT + tempo drift runs until this engine tick (page sub text)
  int8_t driftDir_ = 0;                             // its direction: the same SHIFT + tempo key again stops it, the other turns it round
  PageParam paramPage_ = PPVolume; uint16_t lenFrom_ = 0, lenTo_ = 0;   // '2 → 4 beats'
  char note_[32] = ""; Color noteColor_ = kWhite;   // 0.6 s note page ("S1 BEAT 3 +SNARE")
  char status_[48] = ""; uint32_t statusUntil_ = 0; uint32_t statusArmMs_ = 0; bool statusArmed_ = false;   // explicit message (showStatus)
  char progress_[48] = ""; bool progressOn_ = false;   // job progress ("GEN S1 3/8"); shown only while no explicit message lasts
  char genLabel_[12] = ""; bool genWasBusy_ = false;
  int genTotal_ = 0; int genStaleDone_ = -1, genStaleTotal_ = -1;   // progress()/total() are the previous job's until the queued job starts
  uint8_t setsOp_ = 0; uint8_t setsSet_ = 0;        // 1 save, 2 load in progress
  Rng rng_{0x5EED1234};
  uint8_t queuedVariant_[kSets] = {0, 0}; uint8_t queuedVariantTrack_[kSets] = {0, 0};   // pat presses while generating (SPEC §9.5)
  int8_t queuedLen_[kSets] = {0, 0}; uint8_t queuedLenTrack_[kSets] = {0, 0};            // len± presses while generating: signed step count (SPEC §9.5)
  uint8_t kickCut_[kSets] = {0, 0}; uint32_t kickCutMs_[kSets] = {0, 0};
  int bpmSent_ = 0; uint32_t bpmSentMs_ = 0;
  uint32_t lastBarIndex_ = 0;
  // ---- gyro ----
  bool gyroOn_ = false; bool gyroKeyDown_ = false; bool gyroToggled_ = false; uint32_t gyroHeldSince_ = 0;
  ImuReading lastImu_; float neutralPitch_ = 0, neutralRoll_ = 0;
  uint8_t imuPitchAxis_ = 0, imuRollAxis_ = 1; int8_t imuPitchSign_ = 1, imuRollSign_ = 1;   // SPEC §3 imu.* mapping (configuration, not reset by init)
  float gyroFilter_ = 0, gyroRev_ = 0, gyroEcho_ = 0; uint32_t gyroSentMs_ = 0;
  // ---- master output volume (configuration, not reset by init) ----
  uint8_t outVol_ = kOutputVolumeDefault; void (*outVolHook_)(int) = nullptr; uint32_t outVolFooterUntil_ = 0;
  void (*usbDriveHook_)() = nullptr;
  int16_t sessionVolSeen_ = -1;                     // the lp.volume a session restore handed over (applied unless an explicit level came first, SPEC §9.18)
  bool volExplicit_ = false;                        // a level was set since init (config.txt volume=, serial vol, SHIFT + kick cut): a restored lp.volume yields to it
  // ---- USB drive (SPEC §9.22): SHIFT + gen pattern held kUsbDriveHoldMs ----
  uint8_t usbState_ = 0; bool usbDown_ = false; uint32_t usbArmMs_ = 0;   // 0 idle, 1 armed (key down), 2 status shown (hook pending), 3 hook called
  // ---- session recorder (SPEC §9.23) ----
  Recorder* recorder_ = nullptr;
  int16_t rcCount_ = 0; int16_t rcCursor_ = 0, rcTop_ = 0; bool rcDirty_ = true;   // the recorder page's list
  int16_t rcBars_[kRecRowsVisible]; uint8_t rcWav_ = 0; uint8_t rcInfoN_ = 0;       // visible rows' header data, read one row per tick (−1 = unknown); bit r = a WAV exists
  uint32_t rcDelArmUntil_ = 0; int16_t rcDelArmRow_ = -1;   // DELETE? PRESS AGAIN window (recorder page) and the row it was armed on: a del on another row only re-arms
  bool rcLiveShift_ = false;                        // the physical SHIFT during an offline render (the replay owns shift_ meanwhile; put back when the render ends)
  char rcRenderName_[8] = "";                       // the session being rendered ("rec_007"), for the DONE line
  bool rcWasRendering_ = false, rcCancelled_ = false, rcCancelArmed_ = false;   // a render was seen running (tick): its end is the DONE / cancelled transition; ` down on the overlay (hold = cancel)
  bool rcOverlayDrawn_ = false;                     // the RENDERING overlay has been painted whole once for this render
  bool rcStartKey_ = false;                         // the ent that started a recording is still down: its release is not recorded
  uint32_t rcCancelMs_ = 0; uint32_t rcDoneUntil_ = 0; int8_t rcPctShown_ = -1;
  uint8_t recMarkShown_ = 0;                        // main footer REC marker shadow: 0 none, 1 lit, 2 dark phase
  // ---- preset manager (SPEC §9.22, §9.24 remap) ----
  uint8_t psSet_ = 0; int8_t psFilter_ = -1;        // the target set (track row y0 / y1); −1 = ALL, else the minor root (CURRENT KEY at toggle time)
  int16_t psCursor_ = 0, psTop_ = 0, psFirst_ = -1; // highlighted index, first visible index, first index of the Sets page the rows come from (−1 none)
  uint8_t psOp_ = 0;                                // 0 idle, 1 list request waiting for Sets (busy), 2 list in flight, 3 label write in flight, 4 delete in flight
  uint8_t psAuto_ = 0, psPlayAfter_ = 0, psRename_ = 0; bool psDirty_ = true;
  uint32_t psDelArmUntil_ = 0; int16_t psDelArmRow_ = -1; int8_t psDelArmSet_ = -1, psDelArmFilter_ = -1;   // DELETE? PRESS AGAIN window and the row / set / filter it was armed on: anything else is a fresh first press
  char psText_[kPresetLabelMax + 1] = "";           // the label being typed (RENAME)
  // ---- battery (platform-reported, SPEC §4 footer) ----
  int8_t batPct_ = -1, batChg_ = -1;                // 0..100 / −1 unknown; 1 charging, 0 not, −1 unknown
  // ---- loop mode (SPEC §9.19) ----
  bool anyLoopSeen_ = false;                        // Generator::anyLoop() at the last tick: its false → true edge sends CmdSetTempo kLoopBpm
  uint16_t loopPickMask_ = 0;                       // bit per track: a loop pick this Ui asked for (SHIFT + edit on, a generate on a loop track), cleared when the generator is idle
  uint16_t loopUnderrunBase_[kLoopSlots];           // Engine::streamUnderruns of each loop slot when loop mode came into use: the footer counts from there
  uint8_t loopRushKeys_ = 0;                        // bit per set: the held rush key went to Generator::rushLoop (that set's Beat was in loop mode), so its release goes there too
  // ---- editor ----
  int edTrack_ = Beat; int edCursorX_ = 0, edCursorY_ = 0, edPage_ = 0, edOctave_ = 4, edGate_ = 1;
  bool edDirty_ = true; int edPlayCol_ = -1;
  // ---- synth page (SPEC §9.17c) ----
  int8_t synthTrack_ = 0; bool synthDirty_ = true;
  char synthHeader_[40]; char knobShown_[kSynthKnobs][8]; int8_t knobPctShown_[kSynthKnobs];
  // ---- perform / looper ----
  int8_t perfTrack_ = Fx; int perfOctave_ = kPerfOctaveDefault; bool perfDirty_ = true;
  uint8_t perfQuant_ = 0; bool perfChord_ = false;
  uint8_t perfHeld_[kKeyRows][kKeyCols];           // pitch + 1 of the sounding (or pending) root note per key, 0 = none
  uint8_t perfHeldChord_[kKeyRows][kKeyCols];      // CHORD mode: (third − root) << 4 | (fifth − root), 0 = a single note
  uint16_t perfHeldTick_[kKeyRows][kKeyCols];       // quantised record tick of the note-on
  struct PerfPend { uint8_t x = 0, y = 0, released = 0, used = 0; uint32_t tick = 0; } perfPend_[kPerfPendMax];   // note-ons waiting for their grid point
  uint8_t perfShadow_[kPerfRows][kPerfCols]; int perfBeatShown_ = -1; char perfHeader_[40] = "";
  uint32_t delPressMs_ = 0; bool delDown_ = false;
  LooperState looper_ = LpIdle; uint32_t recStart_ = 0; uint32_t closeTick_ = 0; uint16_t loopTicks_ = 0;
  Pattern rec_;                                     // the loop being recorded / overdubbed (copied to the track's source on close)
  // ---- draw shadows ----
  struct CellLook { uint8_t state = 255, blink = 0, sel = 0, ticks = 0, cut = 0, acid = 0, loop = 0, pick = 0; } cell_[kTracks];   // loop / pick: the 'L' mark and the pick blink (SPEC §9.19)
  char footerShown_[64] = ""; char batShown_[12] = ""; char statusShown_[48] = ""; char valueShown_[24] = ""; char subShown_[40] = ""; int barShown_ = -1;

  // key handling
  void handleMain(const KeyEvent& e, const KeyMapping& m, uint32_t now);
  void handleEditor(const KeyEvent& e, const KeyMapping& m, uint32_t now);
  void handlePerform(const KeyEvent& e, const KeyMapping& m, uint32_t now);
  void handleSynth(const KeyEvent& e, const KeyMapping& m, uint32_t now);
  void handlePresets(const KeyEvent& e, const KeyMapping& m, uint32_t now);
  void handleRecorder(const KeyEvent& e, const KeyMapping& m, uint32_t now);
  void onShiftRelease(uint32_t now);
  bool modal() const { return page_ == PgEditor || page_ == PgPerform || page_ == PgSynth || page_ == PgPresets || page_ == PgRecorder; }
  void playStopAll(uint32_t now);                   // SHIFT + gyro key (SPEC §9.22): CmdStopSet × 2 when anything plays or is queued, else CmdPlaySet × 2
  void usbDriveKey(KeyEvent::Kind kind, uint32_t now);   // SHIFT + gen pattern: a short press is gen pattern, a kUsbDriveHoldMs hold the USB drive (tick() fires it)
  // session recorder (SPEC §9.23)
  bool recording() const;                           // recorder_ && recorder_->recording()
  bool rendering() const;                           // recorder_ && recorder_->rendering()
  bool replaying() const;                           // recorder_ && recorder_->replaying(): the event in hand is a replayed one (an offline render), not a live key
  void recordKey(const KeyEvent& e);                // type 0 / 1 to Recorder::onInput at the engine tick
  void recordGyro(float filter, int rev, int echo); // type 2
  void recordVolume();                              // type 3
  void openRecorder(uint32_t now);
  void recorderRefresh();                           // re-list the sessions, forget the visible rows' header data
  void recorderMove(int dir);
  void recorderDelete(uint32_t now);
  void recorderRender(uint32_t now);
  void recorderToggle(uint32_t now);                // ent: start (page closes) / stop
  // preset manager (SPEC §9.22; SHIFT + save 2 since §9.24)
  void openPresets(uint32_t now);                   // SHIFT + save 2: opens the page, or closes it when it is up
  void presetTarget(int set);                       // a track-row key on the page: the set ent loads into (header "-> SET n")
  void presetRequestPage();                         // Sets::beginPresetList for the page the cursor needs (retried from tick() while Sets is busy)
  bool presetRowLoaded(int i) const;                // row i of the filtered list is in the Sets page and the page is complete
  const Sets::PresetRow* presetRowAt(int i) const;
  bool presetRowInCycle(const Sets::PresetRow& row) const;   // SPEC §9.25: loaded in the target set's current random-load cycle (false while Sets lacks the query)
  void presetRowText(const Sets::PresetRow& row, char* out, int cap) const;   // "007 A  label-or-file-name"
  void presetMove(int dir);
  void presetLoad(uint32_t now);
  void presetDelete(uint32_t now);
  void presetRenameStart();
  void presetRenameKey(const KeyEvent& e, uint32_t now);
  void presetToggleFilter();
  void toggleTrack(int track);
  void variantPress(int set, int v, uint32_t now);
  void applyVariant(int track, int v, uint32_t now);
  void kickCutPress(int set, uint32_t now);
  void outputVolumeStep(int delta, uint32_t now);   // SHIFT + kick cut: ±16, page, footer note
  void applyOutputVolume();                         // hands the current value to the platform hook
  void applyRestoredVolume(int vol255);             // a restored session's lp.volume (SPEC §9.18): applied, neither explicit nor a change
  void paramKey(ParamId p, int dir, KeyEvent::Kind kind, uint32_t now);
  void continuousKey(int track, ParamId p, int dir, KeyEvent::Kind kind, uint32_t now);   // vol..filter and the knobs: step + hold-ramp / SHIFT ramp / freeze
  void stepParam(int track, ParamId p, int dir, uint32_t now, float* out = nullptr);
  void startRamp(int track, ParamId p, int dir, uint32_t now);
  void startHoldRamp(int track, ParamId p, int dir, float from);
  void freezeHoldRamps();                           // CmdFreezeRamp for every running hold-ramp (a modal page opens or closes)
  void waveStep(int track, int dir);
  void tempoKey(int dir, KeyEvent::Kind kind, uint32_t now);
  void stopDrift();                                 // CmdFreezeRamp PTempo: a tempo± press or a second SHIFT + tempo takes over from a drift
  void octaveKey(int dir, uint32_t now);            // SHIFT + key±: transpose the selected track by an octave (SPEC §9.17c)
  void acidToggle(uint32_t now);                    // SHIFT + genre (SPEC §9.17c)
  void loopToggle(uint32_t now);                    // SHIFT + edit: Generator::setLoopMode on the selected track, its statuses and refusals (SPEC §9.19)
  bool loopTempoRule(uint32_t now);                 // anyLoop() false → true: CmdSetTempo kLoopBpm unless a tempo drift runs or the boot restore owns the card; false = queue full, the edge waits
  bool loopTrack(int track) const { return gen_ && gen_->loopEnabled(track); }   // loop mode requested on the track (the engine applies it at the bar)
  void requestGen(int kind, int arg, uint32_t now);   // 0 set, 1 pattern, 2 sound, 3 theory set (SPEC §9.24)
  void saveSet(int set, uint32_t now);
  void loadSet(int set, uint32_t now);
  void setGyro(bool on, uint32_t now);
  float imuAngle(const ImuReading& r, int axis, int sign) const;   // tilt (rad) of one mapped axis against the remaining "up" axis
  void captureNeutral();
  bool sdBusy() const;                              // a generator or sets state machine owns the card (they must not interleave)
  void queueLength(int dir, uint32_t now);          // len± while the set generates (SPEC §9.5)
  void sessionDirty();                              // every performance change: the session file is rewritten debounced (SPEC §9.18)
  // pages
  void goMain(uint32_t now);
  void openParamPage(PageParam p, uint32_t now);
  void showNote(const char* text, Color c, uint32_t now);
  void noteVariant(int track, int v, uint32_t now);
  void openEditor(uint32_t now);
  void openPerform(int track, uint32_t now);
  void openSynth(int track, uint32_t now);
  void closeModal(uint32_t now);
  // editor helpers
  int edRows() const;
  int edBars() const;
  uint16_t edTick() const { return (uint16_t)((edPage_ * 16 + edCursorX_) * (kPPQN / 4)); }
  int edRowPitch(int row) const;
  int edRowOf(int pitch) const;
  int edNoteAt(uint16_t tick, int pitch) const;
  bool edPitchExact(int pitch, int row) const;
  void edRowLabel(int row, char* out, int cap) const;
  void edToggle(uint32_t now);
  void edVelocity(int dir, uint32_t now);
  void edCommit(uint32_t now);
  int percVoicePitch(int track, int voice) const;   // a pitch the stored fold map puts on voice 36..39 (SPEC §9.7)
  int recPitch(int pitch) const;                    // what the looper stores for a sounding pitch: Percussion slots through the fold map
  // perform / looper
  Category perfCategory() const { return trackCategory(perfTrack_); }
  bool perfMelodic() const { return isSynthCategory(perfCategory()); }
  int perfDrumSlot(int x, int y) const;             // Beat / Percussion: row y0 key → slot 0..7 / 0..3, −1 otherwise
  int perfPitch(int degree) const;
  int perfPitches(int x, int y, int out[3]) const;  // what a grid key plays now: 1 pitch, or the triad in CHORD mode
  int perfHeldPitches(int x, int y, int out[3]) const;   // the pitches a held key is sounding (from the stored root + intervals)
  void perfNoteOn(int x, int y, uint32_t now);
  void perfNoteOff(int x, int y, uint32_t now);
  void perfFire(int x, int y, uint32_t tick, bool releaseNow, uint32_t now);   // the quantised (or immediate) note-on
  void perfReleaseAll(uint32_t now);
  void perfPending(uint32_t now);                   // tick(): fire the note-ons whose grid point has come
  void looperPress(uint32_t now);
  void looperCloseRequest(uint32_t now);           // close at the next bar line rounded to 1/2/4 bars; now when that bar was already dispatched
  void looperClear(uint32_t now);
  void looperBar(uint32_t barTick, uint32_t now);
  void looperClose(uint32_t barTick, uint32_t now);
  void looperRecordOn(int x, int y, uint32_t tick);
  void looperRecordOff(int x, int y, const int* pitches, int n, uint32_t now);
  void addRec(uint16_t start, uint16_t len, int pitch);
  void publishLoop(uint32_t now);
  uint16_t recGrid() const { return (uint16_t)(perfQuant_ ? quantTicks(perfQuant_) : quantTicks(1)); }
  // engine helpers
  bool push(CmdType t, int track = 0, int a = 0, int32_t i = 0, float f = 0.0f);
  float paramValue(int track, ParamId p) const;
  int baseBpm(uint32_t now) const;
  // drawing
  void drawSplash();
  void drawMain(uint32_t now, bool full);
  void drawCell(int track, const CellLook& look);
  void drawFooter(bool force);
  void drawStatusLine(bool force);
  const char* statusWant() const { return statusUntil_ ? status_ : (progressOn_ ? progress_ : ""); }
  void drawParam(bool full);
  void drawNote();
  void drawEditor(bool full);
  void drawPerform(bool full);
  void drawSynth(bool full);
  void drawKnob(int i, int pct, const char* value, Color col);
  void drawPresets(bool full);
  void drawRecorder(bool full);
  void drawRendering(bool full);                    // the RENDERING nn% overlay (an offline render runs)
  void drawRenderDone();                            // "DONE /lp/wav/rec_NNN.wav"
  void drawRecMark(uint32_t now, bool force);       // the main footer's blinking REC while recording
  void textCentered(int y, const char* text, int size, Color c);
  void paramValueText(char* out, int cap, int& barPct) const;
  void footerText(char* out, int cap, int maxWidth) const;   // the left text fitted to maxWidth px: notes dropped whole, never cut to a partial number
  void batteryText(char* out, int cap, Color& col) const;    // "87%" / "CHG 87%" / "--" (level unknown, charging or not) and its colour
};

Color categoryColor(Category c);
Color paramColor(ParamId p);
Color pageParamColor(PageParam p);
const char* pageParamName(PageParam p);
const char* variantName(Category c, int variant);  // SPEC §5 variant rules, for the note page
const char* loopVariantName(int variant);          // SPEC §9.19 pattern keys on a loop track: WHOLE, HALF, 2 BEATS, 1 BEAT
void formatBeats(char* out, int cap, uint16_t ticks);   // 384 → "4", 48 → "0.5"

} // namespace lp
