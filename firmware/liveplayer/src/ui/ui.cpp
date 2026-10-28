// meehaw live performer — UI controller implementation (SPEC §3 key map, §4 screens, §9.10 perform page,
// §9.14 overlays, §9.17c hold-ramps / synth page / acid / perform for every track, §9.19 loop mode, §9.22 preset
// manager / PLAY-STOP ALL / USB drive / SAT ACC CHO knobs, §9.23 session recorder, §9.24 theory generator on SHIFT +
// load 1 / 2 with the preset manager moved to SHIFT + save 2, §9.25 random-load cycle marks). Everything here runs on the loop
// task: key events arrive from KeyMatrix, IMU readings at 50 Hz, tick() runs the timers and draw() diffs the screen
// against shadow copies (SPEC §7: redraw only on change, at most every 40 ms). The engine is driven with commands
// only; the UI owns the source patterns. Every performance change marks the session dirty (SPEC §9.18).
#include "ui.h"
#include "../gen/key.h"
#include "../rec/recorder.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace lp {
namespace {

constexpr int kSixteenth = kPPQN / 4;                                   // 24 ticks
constexpr int kRampBarsTable[kRampBarsSteps] = {1, 2, 4, 8, 16};
constexpr uint8_t kVelDefault = 100;
constexpr int kMinorCircle[12] = {9, 4, 11, 6, 1, 8, 3, 10, 5, 0, 7, 2};  // A E B F# C# G# D# A# F C G D (SPEC §2)
constexpr float kRadToDeg = 57.29578f;
constexpr float kDegToRad = 0.017453292f;
const char* const kKnobLabel[kSynthKnobs] = {"CUT", "RES", "ENV", "DEC", "DRV", "WAV", "SAT", "ACC", "CHO"};
// RENAME text entry (SPEC §9.22 preset manager): the letters and digits of the key legends by matrix row; '-' is (11,0).
const char* const kLegendRow1 = "\0qwertyuiop\0\0\0";                   // y1: tab q w e r t y u i o p [ ] \ (x 1..10)
const char* const kLegendRow2 = "\0\0asdfghjkl\0\0\0";                  // y2: fn sft a s d f g h j k l ; ' ent (x 2..10)
const char* const kLegendRow3 = "\0\0\0zxcvbnm\0\0\0\0";                // y3: ctl opt alt z x c v b n m , . / spc (x 3..9)
const char* const kLegendRow0 = "\0" "1234567890" "\0\0\0";             // y0: ` 1 2 3 4 5 6 7 8 9 0 - = del (x 1..10)
char legendChar(int x, int y, bool upper) {
  if (x < 0 || x >= kKeyCols) return 0;
  const char* row = y == 0 ? kLegendRow0 : y == 1 ? kLegendRow1 : y == 2 ? kLegendRow2 : y == 3 ? kLegendRow3 : nullptr;
  if (!row) return 0;
  if (y == 0 && x == 11) return '-';
  const char c = row[x];
  return (upper && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}
// SPEC §9.25 random-load cycle marks: the preset manager asks Sets whether a row's preset was loaded in the target set's
// current no-repeat cycle. The query (Sets::presetLoadedInCycle) belongs to the sets owner and is resolved here at compile
// time, so this file builds before it lands and the dots light up when it does: (set, name), then (set, NNN), then (name);
// without any of them cycleLoaded() answers "unknown" (-1) and the page draws no dot. Rank2 → Rank0 orders the overloads.
struct CycleRank0 {}; struct CycleRank1 : CycleRank0 {}; struct CycleRank2 : CycleRank1 {}; struct CycleRank3 : CycleRank2 {};
template <class S> auto cycleLoaded(const S* s, int set, const char* name, CycleRank3) -> decltype(s->presetLoadedInCycle(set, name), int()) { return s->presetLoadedInCycle(set, name) ? 1 : 0; }
template <class S> auto cycleLoaded(const S* s, int set, const char* name, CycleRank2) -> decltype(s->presetLoadedInCycle(set, S::parsePresetNumber(name)), int()) { const int n = S::parsePresetNumber(name); return n >= 0 && s->presetLoadedInCycle(set, n) ? 1 : 0; }
template <class S> auto cycleLoaded(const S* s, int set, const char* name, CycleRank1) -> decltype(s->presetLoadedInCycle(name), int()) { (void)set; return s->presetLoadedInCycle(name) ? 1 : 0; }
template <class S> int cycleLoaded(const S*, int, const char*, CycleRank0) { return -1; }
template <class S> constexpr auto cycleAvailable(const S* s, CycleRank3) -> decltype(s->presetLoadedInCycle(0, (const char*)nullptr), bool()) { return true; }
template <class S> constexpr auto cycleAvailable(const S* s, CycleRank2) -> decltype(s->presetLoadedInCycle(0, 0), bool()) { return true; }
template <class S> constexpr auto cycleAvailable(const S* s, CycleRank1) -> decltype(s->presetLoadedInCycle((const char*)nullptr), bool()) { return true; }
template <class S> constexpr bool cycleAvailable(const S*, CycleRank0) { return false; }
const char* const kQuantName[kPerfQuantSteps] = {"OFF", "1/16", "1/8", "1/4"};
const char* const kBeatSlotName[kPerfDrumSlots] = {"KCK", "SNR", "HH", "OH", "A1", "A2", "A3", "A4"};
const char* const kPercSlotName[4] = {"A", "B", "C", "D"};

void copyStr(char* dst, int cap, const char* src) { if (cap <= 0) return; strncpy(dst, src ? src : "", (size_t)cap - 1); dst[cap - 1] = 0; }
void upperStr(char* dst, int cap, const char* src) {
  int i = 0;
  for (; src && src[i] && i < cap - 1; i++) { const char c = src[i]; dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
  dst[i] = 0;
}
void appendStr(char* dst, int cap, const char* s) { const int n = (int)strlen(dst); if (n >= cap - 1) return; strncpy(dst + n, s, (size_t)(cap - 1 - n)); dst[cap - 1] = 0; }
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// "0.65" / "+0.3" without printf's %f (float formatting is optional in the target's newlib).
void fmtFixed(char* out, int cap, float v, int decimals, bool sign) {
  const int scale = decimals == 1 ? 10 : 100;
  const int c = (int)(fabsf(v) * (float)scale + 0.5f);
  const char* s = (v < 0 && c > 0) ? "-" : (sign ? "+" : "");
  if (decimals == 1) snprintf(out, (size_t)cap, "%s%d.%d", s, c / 10, c % 10);
  else snprintf(out, (size_t)cap, "%s%d.%02d", s, c / 100, c % 100);
}
float stepOf(ParamId p) { return p == PFilter ? 0.1f : 0.05f; }        // SPEC §9.12 steps (knobs step 0.05 of their 0..1 range)
float minOf(ParamId p) { return p == PFilter ? -1.0f : 0.0f; }
bool isContinuous(ParamId p) { return p < PLength || (p >= PCutoff && p <= PDrive) || (p >= PSat && p <= PChorus); }   // the wave is stepped
static_assert(ParamCount <= 32, "the ramp bit masks are 32 bits wide");
uint32_t paramBit(int p) { return 1u << (p & 31); }
int rampIndex(int bars) { for (int i = 0; i < kRampBarsSteps; i++) if (kRampBarsTable[i] == bars) return i; return 2; }
int circleIndex(int root) { for (int i = 0; i < 12; i++) if (kMinorCircle[i] == ((root % 12) + 12) % 12) return i; return 0; }
bool isKnobTrack(Category c) { return c == Chord || c == Lead || c == Pad || c == Bass; }   // the synth page / acid tracks (SPEC §9.17)
void catUpper(Category c, char* out, int cap) { upperStr(out, cap, categoryName(c)); }
// Beyond a ±dead band, `range` degrees of tilt reach full scale (SPEC §3 gyro mapping).
float mapTilt(float deg) {
  const float a = fabsf(deg);
  if (a <= kGyroDeadDeg) return 0.0f;
  float v = (a - kGyroDeadDeg) / kGyroRangeDeg;
  if (v > 1.0f) v = 1.0f;
  return deg < 0 ? -v : v;
}
float wrapDeg(float d) { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
int volumePercent(int vol255) { return (vol255 * 100 + kOutputVolumeMax / 2) / kOutputVolumeMax; }   // 200 → 78, 255 → 100
uint16_t quantTo(uint32_t ticks, uint16_t grid) { return (uint16_t)(((ticks + grid / 2) / grid) * grid); }   // nearest grid point

} // namespace

// ---------------------------------------------------------------------------------------------- palette / names

Color categoryColor(Category c) { return kCategoryColor[c < CategoryCount ? c : 0]; }
Color pageParamColor(PageParam p) { return kPageParamColor[p < PageParamCount ? p : 0]; }
Color paramColor(ParamId p) { return pageParamColor((PageParam)(p < ParamCount ? p : 0)); }

const char* pageParamName(PageParam p) {
  static const char* const n[PageParamCount] = {"VOLUME", "REVERB", "ECHO", "DIST", "FILTER", "LENGTH", "TEMPO", "KEY", "OUTPUT",
                                                "CUTOFF", "RESO", "ENV", "DECAY", "DRIVE", "WAVE", "SAT", "ACCENT", "CHORUS", "GENRE", "RAMP"};
  static_assert(PageParamCount == 20, "pageParamName lists every PageParam (18 ParamIds + genre + ramp bars)");
  return n[p < PageParamCount ? p : 0];
}

// Variant rules of SPEC §5 (Chord/Lead/Bass crops, Pad voicing, Fx mutate/generated, Beat layers, Percussion density, Vocal loops).
const char* variantName(Category c, int variant) {
  static const char* n[CategoryCount][kVariants] = {
    {"ORIGINAL", "VARIATION", "CROP 2", "CROP 1/2"},     // Chord
    {"ORIGINAL", "VARIATION", "CROP 2", "CROP 1/2"},     // Lead
    {"ORIGINAL", "VARIATION", "2 VOICES", "MONO"},       // Pad
    {"ORIGINAL", "VARIATION", "CROP 2", "CROP 1/2"},     // Bass
    {"ORIGINAL", "MUTATE", "GEN 2", "GEN 1/2"},          // Fx
    {"SPARSE", "LIGHT", "DRIVING", "FULL"},              // Percussion
    {"FULL", "ALT FILE", "LOOP 2", "LOOP 1"},            // Vocal
    {"KICK", "+HATS", "+SNARE", "FULL"}};                // Beat (SPEC §9.6 layers)
  return n[c < CategoryCount ? c : 0][variant & 3];
}

// Pattern keys on a loop track (SPEC §9.19): 1 = the whole loop, 2 = its first half, 3 = the first 2 beats, 4 = the first beat.
const char* loopVariantName(int variant) { static const char* n[kVariants] = {"WHOLE", "HALF", "2 BEATS", "1 BEAT"}; return n[variant & 3]; }

void formatBeats(char* out, int cap, uint16_t ticks) {
  const int whole = ticks / kTicksPerBeat, rem = ticks % kTicksPerBeat;
  if (rem == 0) snprintf(out, (size_t)cap, "%d", whole);
  else if (rem == kTicksPerBeat / 2) snprintf(out, (size_t)cap, "%d.5", whole);
  else snprintf(out, (size_t)cap, "%d.%02d", whole, rem * 100 / kTicksPerBeat);
}

// ---------------------------------------------------------------------------------------------- pure helpers

int Ui::perfDegree(int x, int y) {                                      // SPEC §9.10 grid: degree = col + 3 × row
  if (y < 0 || y >= kPerfRows) return -1;
  const int col = x - kPerfRowOffset[y];
  if (col < 0 || col >= kPerfCols) return -1;
  return col + 3 * y;
}
int Ui::roundUpBars(int bars) { return bars <= 1 ? 1 : (bars <= 2 ? 2 : 4); }
uint16_t Ui::quantize16(uint32_t ticks) { return (uint16_t)(((ticks + kSixteenth / 2) / kSixteenth) * kSixteenth); }
int Ui::quantTicks(int quant) { static const int t[kPerfQuantSteps] = {0, kSixteenth, kSixteenth * 2, kSixteenth * 4}; return t[quant >= 0 && quant < kPerfQuantSteps ? quant : 0]; }

// The synth page maps the parameter columns onto the knobs (SPEC §9.17c, §9.22: tempo± → SAT, key± → ACC; CHO has no
// parameter column — it sits on the gyro / edit keys (8,2) / (8,3), handled by position in handleSynth).
ParamId Ui::synthKnobParam(ParamId column) {
  switch (column) {
    case PVolume: return PCutoff; case PReverb: return PResonance; case PEcho: return PEnvAmt;
    case PDist: return PDecay; case PFilter: return PDrive; case PLength: return PWave;
    case PTempo: return PSat; case PKey: return PAccent;
    default: return ParamCount;
  }
}

const char* Ui::waveName(int wave) { static const char* n[WaveCount] = {"SINE", "TRI", "SAW", "SQR", "FAT"}; return n[wave >= 0 && wave < WaveCount ? wave : 0]; }
const char* Ui::knobLabel(int knob) { return kKnobLabel[knob >= 0 && knob < kSynthKnobs ? knob : 0]; }

// Knob values in real units (types.h mappings), at most 6 characters so they fit under a 48 px knob.
void Ui::knobValueText(ParamId knob, float v, char* out, int cap) {
  switch (knob) {
    case PCutoff: { const int hz = (int)(knobCutoffHz(v) + 0.5f); if (hz < 1000) snprintf(out, (size_t)cap, "%dHz", hz); else snprintf(out, (size_t)cap, "%d.%dk", hz / 1000, (hz % 1000) / 100); break; }
    case PResonance: case PDrive: case PSat: case PAccent: case PChorus: snprintf(out, (size_t)cap, "%d%%", (int)(clampf(v, 0, 1) * 100.0f + 0.5f)); break;
    case PEnvAmt: { const int t = (int)(knobEnvOct(v) * 10.0f + 0.5f); snprintf(out, (size_t)cap, "%d.%doct", t / 10, t % 10); break; }
    case PDecay: { const int ms = (int)(knobDecayS(v) * 1000.0f + 0.5f); if (ms < 1000) snprintf(out, (size_t)cap, "%dms", ms); else snprintf(out, (size_t)cap, "%d.%ds", ms / 1000, (ms % 1000) / 100); break; }
    case PWave: copyStr(out, cap, waveName((int)(v + 0.5f))); break;
    default: out[0] = 0; break;
  }
}

// A refused SHIFT + edit (SPEC §9.19): the generator's reason as a status line; NO LOOP names the category's loop folder.
void Ui::loopRefusalText(Generator::LoopRefusal r, Category c, char* out, int cap) {
  switch (r) {
    case Generator::LoopSlotsFull: snprintf(out, (size_t)cap, "LOOP SLOTS FULL (%d)", kLoopSlots); break;
    case Generator::LoopNoContent: { char f[16]; upperStr(f, sizeof f, Generator::loopFolder(c)); snprintf(out, (size_t)cap, "NO LOOP: %s", f); break; }
    case Generator::LoopBusy: copyStr(out, cap, "LOOP: BUSY"); break;
    case Generator::LoopLowRam: copyStr(out, cap, "LOOP: LOW RAM"); break;
    case Generator::LoopNoCard: copyStr(out, cap, "LOOP: NO CARD"); break;
    default: copyStr(out, cap, "LOOP REFUSED"); break;
  }
}

// ---------------------------------------------------------------------------------------------- init / wiring

void Ui::init(IGfx* gfx, Engine* eng, Generator* gen, Sets* sets) {
  gfx_ = gfx; eng_ = eng; gen_ = gen; sets_ = sets;
  page_ = PgSplash; pageUntil_ = 0; lastNow_ = 0; started_ = false; fullRedraw_ = true; everDrawn_ = false; lastDraw_ = 0;
  selected_ = Beat; shift_ = false; rampBars_ = kRampBarsDefault; rampsActive_ = 0; holdRamps_ = 0; holdDir_ = 0; driftUntilTick_ = 0; driftDir_ = 0; paramPage_ = PPVolume; lenFrom_ = lenTo_ = 0;
  memset(holdTrack_, 0, sizeof holdTrack_);
  note_[0] = 0; noteColor_ = kWhite; status_[0] = 0; statusUntil_ = 0; statusArmMs_ = 0; statusArmed_ = false;
  genLabel_[0] = 0; genWasBusy_ = false; genTotal_ = 0; genStaleDone_ = genStaleTotal_ = -1; setsOp_ = 0; setsSet_ = 0;
  progress_[0] = 0; progressOn_ = false;
  for (int s = 0; s < kSets; s++) { queuedVariant_[s] = 0; queuedVariantTrack_[s] = 0; queuedLen_[s] = 0; queuedLenTrack_[s] = 0; kickCut_[s] = 0; kickCutMs_[s] = 0; }
  bpmSent_ = 0; bpmSentMs_ = 0; lastBarIndex_ = 0;
  gyroOn_ = false; gyroKeyDown_ = false; gyroToggled_ = false; gyroHeldSince_ = 0; lastImu_ = ImuReading();
  neutralPitch_ = neutralRoll_ = 0; gyroFilter_ = gyroRev_ = gyroEcho_ = 0; gyroSentMs_ = 0;
  edTrack_ = Beat; edCursorX_ = edCursorY_ = 0; edPage_ = 0; edOctave_ = 4; edGate_ = 1; edDirty_ = true; edPlayCol_ = -1;
  synthTrack_ = 0; synthDirty_ = true; synthHeader_[0] = 0; memset(knobShown_, 0, sizeof knobShown_); memset(knobPctShown_, -1, sizeof knobPctShown_);
  perfTrack_ = Fx; perfOctave_ = kPerfOctaveDefault; perfDirty_ = true; perfQuant_ = 0; perfChord_ = false;
  memset(perfHeld_, 0, sizeof perfHeld_); memset(perfHeldChord_, 0, sizeof perfHeldChord_); memset(perfHeldTick_, 0xFF, sizeof perfHeldTick_); memset(perfShadow_, 0xFF, sizeof perfShadow_);
  for (int i = 0; i < kPerfPendMax; i++) perfPend_[i] = PerfPend();
  perfBeatShown_ = -1; perfHeader_[0] = 0; delPressMs_ = 0; delDown_ = false;
  looper_ = LpIdle; recStart_ = 0; closeTick_ = 0; loopTicks_ = 0; rec_.clear();
  for (int t = 0; t < kTracks; t++) cell_[t] = CellLook();
  footerShown_[0] = batShown_[0] = statusShown_[0] = valueShown_[0] = subShown_[0] = 0; barShown_ = -1;
  outVolFooterUntil_ = 0; sessionVolSeen_ = -1; volExplicit_ = false; applyOutputVolume();   // the platform applies the current (default / config) level
  batPct_ = batChg_ = -1;                                                 // unknown until the platform reports (SPEC §4)
  anyLoopSeen_ = gen_ ? gen_->anyLoop() : false; loopPickMask_ = 0; loopRushKeys_ = 0;   // loop mode (SPEC §9.19): the tempo rule fires on the next false → true edge
  for (int k = 0; k < kLoopSlots; k++) loopUnderrunBase_[k] = eng_ ? eng_->streamUnderruns(kSets + k) : 0;
  usbState_ = 0; usbDown_ = false; usbArmMs_ = 0;                        // SPEC §9.22 USB drive hold (the hook itself is configuration, like the volume hook)
  rcCount_ = 0; rcCursor_ = rcTop_ = 0; rcDirty_ = true; memset(rcBars_, -1, sizeof rcBars_); rcWav_ = 0; rcInfoN_ = 0;   // SPEC §9.23 recorder page (recorder_ stays: wiring)
  rcDelArmUntil_ = 0; rcRenderName_[0] = 0; rcWasRendering_ = rcOverlayDrawn_ = rcCancelled_ = rcCancelArmed_ = rcStartKey_ = false; rcCancelMs_ = 0; rcDoneUntil_ = 0; rcPctShown_ = -1; recMarkShown_ = 0;
  psSet_ = 0; psFilter_ = -1; psCursor_ = psTop_ = 0; psFirst_ = -1; psOp_ = 0; psAuto_ = psPlayAfter_ = psRename_ = 0; psDirty_ = true; psDelArmUntil_ = 0; psText_[0] = 0;   // SPEC §9.22 preset manager
}

// ---------------------------------------------------------------------------------------------- battery (SPEC §4)

// Platform report; the footer picks the change up on the next draw (its text is diffed against a shadow).
void Ui::setBattery(int percent, int charging) {
  batPct_ = (int8_t)(percent < 0 ? -1 : clampi(percent, 0, 100));
  batChg_ = (int8_t)(charging < 0 ? -1 : (charging ? 1 : 0));
}

// "87%", "CHG 87%" while charging, "--" while the level is unknown (charging or not: with no level to show
// the prefix would only widen the readout); kBatteryLow below kBatteryLowPct when not known to be charging,
// else the footer's dim grey.
void Ui::batteryText(char* out, int cap, Color& col) const {
  if (batPct_ < 0) copyStr(out, cap, "--");
  else snprintf(out, (size_t)cap, "%s%d%%", batChg_ == 1 ? "CHG " : "", (int)batPct_);
  col = (batPct_ >= 0 && batPct_ < kBatteryLowPct && batChg_ != 1) ? kBatteryLow : kDimGrey;
}

// Cuts text from the end until gfx draws it within maxWidth px; a cut text loses its trailing spaces too.
void Ui::fitTextWidth(IGfx* gfx, char* text, int maxWidth, int size) {
  if (!gfx || !text) return;
  int n = (int)strlen(text); const int n0 = n;
  while (n > 0 && gfx->textWidth(text, size) > maxWidth) text[--n] = 0;
  if (n < n0) while (n > 0 && text[n - 1] == ' ') text[--n] = 0;
}

// ---------------------------------------------------------------------------------------------- output volume (SPEC §3)

void Ui::setOutputVolumeHook(void (*fn)(int vol255)) { outVolHook_ = fn; applyOutputVolume(); }
// The platform hook gets the level; the session file carries it as lp.volume (SPEC §9.18, Sets::setSessionVolume); a
// running session recording logs it as a type 3 event (SPEC §9.23).
void Ui::applyOutputVolume() { if (outVolHook_) outVolHook_((int)outVol_); if (sets_) sets_->setSessionVolume((int)outVol_); recordVolume(); }

// config.txt `volume=` / serial `vol N`: applied, no page. Explicit, like a SHIFT + kick cut step: the session file
// follows it (SPEC §9.18) and a session restored later in this boot does not override it (`vol 0` right after boot
// stays 0 while the restore brings back lp.volume).
void Ui::setOutputVolume(int vol255) {
  volExplicit_ = true;
  const int v = clampi(vol255, 0, kOutputVolumeMax);
  if (v == (int)outVol_) return;
  outVol_ = (uint8_t)v; applyOutputVolume(); sessionDirty();
}

// The restore path (tick()): the level the session file held, applied unless an explicit level came first since boot.
void Ui::applyRestoredVolume(int vol255) {
  const int v = clampi(vol255, 0, kOutputVolumeMax);
  if (v == (int)outVol_) return;
  outVol_ = (uint8_t)v; applyOutputVolume();
}

// SHIFT + kick cut S1 / S2 (press and hold-repeat): ±16, clamped; the hook only runs when the value changed.
void Ui::outputVolumeStep(int delta, uint32_t now) {
  volExplicit_ = true;
  const int v = clampi((int)outVol_ + delta, 0, kOutputVolumeMax);
  if (v != (int)outVol_) { outVol_ = (uint8_t)v; applyOutputVolume(); sessionDirty(); }
  outVolFooterUntil_ = now + kOutputFooterMs;
  openParamPage(PPMaster, now);
}

bool Ui::push(CmdType t, int track, int a, int32_t i, float f) {
  if (!eng_) return false;
  Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f;
  return eng_->cmds().push(c);
}

float Ui::paramValue(int track, ParamId p) const {
  const TrackParams& tp = eng_->params(track);
  switch (p) {
    case PVolume: return tp.volume; case PReverb: return tp.reverb; case PEcho: return tp.echo; case PDist: return tp.dist; case PFilter: return tp.filter;
    case PCutoff: return tp.cutoff; case PResonance: return tp.resonance; case PEnvAmt: return tp.envAmt; case PDecay: return tp.decay; case PDrive: return tp.drive;
    case PSat: return tp.sat; case PAccent: return tp.accent; case PChorus: return tp.chorus;   // SPEC §9.22
    case PWave: return (float)tp.wave;
    default: return 0.0f;
  }
}

// Hold-repeat steps faster than the engine publishes its status: the last value sent wins for 300 ms.
int Ui::baseBpm(uint32_t now) const {
  if (bpmSent_ && now - bpmSentMs_ < 300) return bpmSent_;
  return eng_->status().bpm;
}

void Ui::showStatus(const char* text, uint32_t ms) {
  copyStr(status_, sizeof status_, text);
  statusArmMs_ = ms;
  if (started_) statusUntil_ = lastNow_ + ms; else statusArmed_ = true;   // before the first tick: timed from it
}

void Ui::sessionDirty() { if (sets_) sets_->markSessionDirty(); }       // SPEC §9.18: the session file follows every change, debounced

// ---------------------------------------------------------------------------------------------- keys

// Every event is dispatched to the page it lands on and, while a session recording runs (SPEC §9.23), logged afterwards
// as it was received — except the recorder page's own keys and the SHIFT + save 1 that opens or closes it (a replay
// must never reopen the page or restart a recording; `before` / page_ catch both ends of that press). An offline render
// replays the log through this very function (Recorder::replaying() while it does): the pages follow the replayed events
// exactly as live, except that none of them writes the card or reboots (save, SHIFT + save 1, the drive hook, a preset
// delete / label — see handleMain, tick, presetDelete, presetRenameKey) and none of them can cancel the render: a recorded `
// hold is a track key held, as it was live. A LIVE key during a render (the sketch keeps delivering them) never reaches the
// pages — it would change the replayed world and the WAV would no longer match the recording: only ` HELD
// kRenderCancelHoldMs (the cancel, resolved in tick()) and the physical SHIFT state (put back when the render ends) count.
void Ui::onKey(const KeyEvent& e, uint32_t now) {
  lastNow_ = now;
  const KeyMapping m = mapKey(e.x, e.y);
  if (rendering() && !replaying()) {                                     // a live key while an offline render runs
    if (e.x == 0 && e.y == 0) {                                           // the cancel hold, resolved in tick()
      if (e.kind == KeyEvent::Press) { rcCancelArmed_ = true; rcCancelMs_ = 0; }
      else if (e.kind == KeyEvent::Release) rcCancelArmed_ = false;
    } else if (m.fn == KShift) {
      if (e.kind == KeyEvent::Press) rcLiveShift_ = true; else if (e.kind == KeyEvent::Release) rcLiveShift_ = false;
    }
    return;
  }
  const Page before = page_;
  if (m.fn == KShift) {                                                   // physical SHIFT = space (SPEC §3)
    if (e.kind == KeyEvent::Press) shift_ = true;
    else if (e.kind == KeyEvent::Release) { shift_ = false; onShiftRelease(now); }
  } else if (m.fn == KNone) return;
  else if (page_ == PgEditor) handleEditor(e, m, now);
  else if (page_ == PgPerform) handlePerform(e, m, now);
  else if (page_ == PgSynth) handleSynth(e, m, now);
  else if (page_ == PgPresets) handlePresets(e, m, now);
  else if (page_ == PgRecorder) handleRecorder(e, m, now);
  else handleMain(e, m, now);
  if (rcStartKey_ && e.kind == KeyEvent::Release && e.x == 13 && e.y == 2) { rcStartKey_ = false; return; }   // the release of the ent that started the recording
  if (before != PgRecorder && page_ != PgRecorder && e.kind != KeyEvent::Repeat) recordKey(e);
}

// SHIFT release snaps every SHIFT ramp back (SPEC §9.12); the parameter page lingers so the snap is visible. A
// tempo drift (SPEC §9.17c) is not a SHIFT ramp: the engine keeps PTempo ramps through CmdCancelRamps.
void Ui::onShiftRelease(uint32_t now) {
  if (!rampsActive_) return;
  push(CmdCancelRamps);
  rampsActive_ = 0;
  if (page_ == PgParam) pageUntil_ = now + kParamPageMs;
}

void Ui::handleMain(const KeyEvent& e, const KeyMapping& m, uint32_t now) {
  const bool press = e.kind == KeyEvent::Press, release = e.kind == KeyEvent::Release;
  switch (m.fn) {
    case KTrack:
      if (!press) break;
      selected_ = m.a; if (!shift_) toggleTrack(m.a);                     // a tap toggles AND selects the track; SHIFT + track only selects (SPEC §3, §9.20)
      break;
    case KVariant:                                                        // pat1..4 = that set's Beat layer; SHIFT + pat = the synth page of Chord/Lead/Pad/Bass (SPEC §9.17c)
      if (press) { if (shift_) openSynth(m.a * kTracksPerSet + m.b, now); else variantPress(m.a, m.b, now); }
      break;
    case KSnareRush: {                                                    // momentary (SPEC §9.6); a Beat in loop mode rushes its stream instead (SPEC §9.19 §1.4)
      // The engine never steps a streaming track's pattern, so CmdSnareRush would be dead there: Generator::rushLoop crops the
      // loop to its first beat from the next 1/16 while the key is held and brings the stored crop / len back on release
      // (info() untouched, nothing persisted). The release follows the press's route (loopRushKeys_), whatever loop mode did meanwhile.
      const int beat = m.a * kTracksPerSet + Beat; const uint8_t bit = (uint8_t)(1u << m.a);
      if (press) {
        if (loopTrack(beat)) { gen_->rushLoop(beat, true); loopRushKeys_ |= bit; } else { push(CmdSnareRush, 0, m.a, 1); loopRushKeys_ &= (uint8_t)~bit; }
        char t[16]; snprintf(t, sizeof t, "S%d RUSH", m.a + 1); showNote(t, categoryColor(Beat), now);
      } else if (release) {
        if (loopRushKeys_ & bit) { if (gen_) gen_->rushLoop(beat, false); loopRushKeys_ &= (uint8_t)~bit; }
        else push(CmdSnareRush, 0, m.a, 0);
      }
      break;
    }
    case KKickCut:                                                        // SHIFT + cut = output volume: S1 up, S2 down (SPEC §3)
      if (shift_) { if (press || e.kind == KeyEvent::Repeat) outputVolumeStep(m.a == 0 ? kOutputVolumeStep : -kOutputVolumeStep, now); }
      else if (press) kickCutPress(m.a, now);
      break;
    case KParamUp: paramKey((ParamId)m.a, +1, e.kind, now); break;
    case KParamDown: paramKey((ParamId)m.a, -1, e.kind, now); break;
    case KGyro:                                                           // press-and-hold toggle, resolved in tick(); SHIFT + gyro = PLAY ALL / STOP ALL (SPEC §9.22)
      if (press && shift_) { playStopAll(now); break; }
      if (press) { gyroKeyDown_ = true; gyroToggled_ = false; gyroHeldSince_ = now; }
      else if (release) gyroKeyDown_ = false;
      break;
    case KGenre:                                                          // setGenre() invalidates the library index: not under a running job
      if (!press || !gen_) break;
      if (shift_) { acidToggle(now); break; }                             // SHIFT + genre = acid mode of the selected track (SPEC §9.17c)
      if (sdBusy()) showStatus("BUSY"); else { gen_->setGenre(gen_->genreIndex() + 1); openParamPage(PPGenre, now); sessionDirty(); }
      break;
    case KSaveSet:                                                        // SHIFT + save 1 = the recorder page (SPEC §9.23), SHIFT + save 2 = the preset manager (SPEC §9.22 as remapped by §9.24)
      if (!press) break;
      if (shift_ && m.a == 1) { openPresets(now); break; }                // followed on replay like every page (a replayed delete / label on it writes nothing)
      if (replaying()) break;                                             // replayed, a save writes nothing (every render would add a preset copy) and SHIFT + save 1 opens no page
      if (shift_ && m.a == 0) openRecorder(now); else saveSet(m.a, now);
      break;
    case KLoadSet: if (press) { if (shift_) requestGen(3, m.a, now); else loadSet(m.a, now); } break;        // SHIFT + load 1 / 2 = THEORY generate of that set (SPEC §9.24)
    case KGenPattern:                                                     // SHIFT + gen pattern held 1 s = USB drive (SPEC §9.22); a short SHIFT press stays gen pattern
      if (shift_ || usbDown_) usbDriveKey(e.kind, now); else if (press) requestGen(1, selected_, now);
      break;
    case KGenSound: if (press) requestGen(2, selected_, now); break;
    case KGenSetAll: if (press) requestGen(0, shift_ ? 1 : 0, now); break;
    case KEdit: if (press) { if (shift_) loopToggle(now); else openEditor(now); } break;   // SHIFT + edit = loop mode of the selected track (SPEC §9.19)
    case KPerform: if (press) openPerform(shift_ ? (selected_ ^ kTracksPerSet) : selected_, now); break;   // SHIFT: the same track of the other set
    default: break;
  }
}

// Tap = start/stop at the bar; a second tap before the bar line cancels the pending change (SPEC §9.4).
void Ui::toggleTrack(int track) {
  if (track < 0 || track >= kTracks) return;
  const PlayState s = eng_->status().track[track].state;
  if (s == PSQueued || s == PSStopQueued) push(CmdCancelPending, track);
  else push(CmdTrackToggle, track);
}

// pat1..4 of set N ALWAYS act on N's Beat track, whatever is selected (SPEC §9.17c); queued while the set
// generates (SPEC §9.5).
void Ui::variantPress(int set, int v, uint32_t now) {
  set &= 1; v &= 3;
  const int target = set * kTracksPerSet + Beat;
  if (eng_->status().generating[set]) {
    queuedVariant_[set] = (uint8_t)(v + 1); queuedVariantTrack_[set] = (uint8_t)target;
    char t[24]; snprintf(t, sizeof t, "S%d PAT %d QUEUED", set + 1, v + 1);
    showNote(t, categoryColor(Beat), now);
    return;
  }
  applyVariant(target, v, now);
}

void Ui::applyVariant(int track, int v, uint32_t now) {
  if (gen_) gen_->rebuildVariant(track, v);                               // Beat: mask; others: next + CmdSwapPattern; Vocal: loop mode
  noteVariant(track, v, now);
  sessionDirty();
}

void Ui::noteVariant(int track, int v, uint32_t now) {
  const Category c = trackCategory(track);
  const char* name = loopTrack(track) ? loopVariantName(v) : variantName(c, v);   // a loop track's keys crop the loop (SPEC §9.19)
  char t[32]; snprintf(t, sizeof t, "S%d %s %d %s", trackSet(track) + 1, categoryShort(c), v + 1, name);
  showNote(t, categoryColor(c), now);
}

void Ui::kickCutPress(int set, uint32_t now) {
  set &= 1;
  kickCut_[set] ^= 1; kickCutMs_[set] = now;
  push(CmdKickCut, 0, set, kickCut_[set]);
  char t[20]; snprintf(t, sizeof t, "S%d KICK %s", set + 1, kickCut_[set] ? "CUT" : "ON");
  showNote(t, categoryColor(Beat), now);
  sessionDirty();
}

// vol/rev/echo/dist/filt/len/tempo/key ± (SPEC §3, §9.12, §9.17c): a continuous parameter steps once on Press and
// hold-ramps while held (release freezes it), SHIFT ramps to the end (snap back on SHIFT release); len/tempo/key keep
// single steps with hold repeats; SHIFT + len± sets rampBars, SHIFT + tempo± drifts the tempo, SHIFT + key± transposes.
void Ui::paramKey(ParamId p, int dir, KeyEvent::Kind kind, uint32_t now) {
  if (!gen_) return;
  if (isContinuous(p)) { continuousKey(selected_, p, dir, kind, now); return; }
  if (kind == KeyEvent::Release) return;
  const bool repeat = kind == KeyEvent::Repeat;
  if (p == PLength) {
    if (repeat) return;
    if (shift_) { rampBars_ = kRampBarsTable[(rampIndex(rampBars_) + dir + kRampBarsSteps) % kRampBarsSteps]; openParamPage(PPRampBars, now); }
    else if (eng_->status().generating[trackSet(selected_)]) queueLength(dir, now);
    else { lenFrom_ = gen_->setLengthStep(selected_, 0); lenTo_ = gen_->setLengthStep(selected_, dir); openParamPage(PPLength, now); sessionDirty(); }
    return;
  }
  if (p == PKey) {
    if (repeat) return;
    if (shift_) octaveKey(dir, now);
    else { gen_->setKeyRoot(cycleMinorRoot(gen_->key().root, dir)); openParamPage(PPKey, now); sessionDirty(); }
    return;
  }
  if (p == PTempo) tempoKey(dir, kind, now);
}

// A continuous parameter (vol..filter, or a synth knob on the synth page) of `track`:
//   Press         one step, then a hold-ramp towards the end of the range at the SHIFT-ramp speed (the full range
//                 takes rampBars bars: ticks = rampBars × 384 × remaining / range), SPEC §9.17c
//   Repeat        nothing: the ramp is running
//   Release       CmdFreezeRamp: the value stays where the ramp got
//   SHIFT + Press CmdRamp to the end over rampBars bars; SHIFT release snaps back (SPEC §9.12), no repeat.
// One hold-ramp per parameter: a press while one runs (the other direction's key, or the same key on another track
// after SHIFT + track) freezes it first, so the engine starts the new ramp from the current value instead of reusing
// the slot with the old start (engine CmdRamp keeps `from` on an active slot). The ramp belongs to the key of its
// direction (holdDir_): the first key's release, whose ramp was taken over, sends nothing.
void Ui::continuousKey(int track, ParamId p, int dir, KeyEvent::Kind kind, uint32_t now) {
  const uint32_t bit = paramBit(p);                                        // 32 bits: PAccent / PChorus are bits 16 / 17 (SPEC §9.22)
  const bool running = (holdRamps_ & bit) != 0, up = (holdDir_ & bit) != 0;
  if (kind == KeyEvent::Release) {
    if (running && up == (dir > 0)) { push(CmdFreezeRamp, holdTrack_[p], p); holdRamps_ &= ~bit; }
    return;
  }
  if (kind == KeyEvent::Repeat) return;
  if (running) { push(CmdFreezeRamp, holdTrack_[p], p); holdRamps_ &= ~bit; }
  if (shift_) { startRamp(track, p, dir, now); return; }
  float v = 0;
  stepParam(track, p, dir, now, &v);
  startHoldRamp(track, p, dir, v);
}

// SPEC §9.5: "while a set generates, its pat/len presses are queued and applied after the swap". A step sent now
// would resize the OLD pattern for its last bar and be reset by the swap; so the signed count waits in tick().
void Ui::queueLength(int dir, uint32_t now) {
  const int set = trackSet(selected_);
  if (queuedLenTrack_[set] != selected_) queuedLen_[set] = 0;
  queuedLenTrack_[set] = (uint8_t)selected_;
  queuedLen_[set] = (int8_t)clampi(queuedLen_[set] + dir, -5, 5);
  char t[28]; snprintf(t, sizeof t, "S%d LEN %+d QUEUED", set + 1, queuedLen_[set]);
  showNote(t, pageParamColor(PPLength), now);
}

void Ui::stepParam(int track, ParamId p, int dir, uint32_t now, float* out) {
  const float step = stepOf(p);
  float v = paramValue(track, p) + (float)dir * step;
  v = roundf(v / step) * step;                                            // stay on the step grid (no float drift)
  v = clampf(v, minOf(p), 1.0f);
  push(CmdSetParam, track, p, 0, v);
  if (p < PLength) openParamPage((PageParam)p, now);                      // the knobs show on the synth page itself
  if (out) *out = v;
  sessionDirty();
}

void Ui::startRamp(int track, ParamId p, int dir, uint32_t now) {
  push(CmdRamp, track, p, rampBars_ * kTicksPerBar, dir > 0 ? 1.0f : minOf(p));
  rampsActive_ |= paramBit(p);
  if (p < PLength) openParamPage((PageParam)p, now);
}

// The hold-ramp runs at the SHIFT-ramp speed from the stepped value `from` to the end of the range (SPEC §9.17c);
// nothing is sent when the step already reached the end.
void Ui::startHoldRamp(int track, ParamId p, int dir, float from) {
  const float target = dir > 0 ? 1.0f : minOf(p);
  const float remaining = fabsf(target - from), range = 1.0f - minOf(p);
  const int ticks = (int)((float)(rampBars_ * kTicksPerBar) * remaining / range + 0.5f);
  if (ticks < 1) return;
  push(CmdRamp, track, p, ticks, target);
  holdRamps_ |= paramBit(p); holdTrack_[p] = (uint8_t)track;
  if (dir > 0) holdDir_ |= paramBit(p); else holdDir_ &= ~paramBit(p);
}

// Every running hold-ramp stops where it is: a modal page opening or closing takes the held key's release to a page
// where the key means something else (or nothing), so the freeze happens here instead (SPEC §9.17c).
void Ui::freezeHoldRamps() {
  for (int p = 0; p < ParamCount; p++) if (holdRamps_ & paramBit(p)) push(CmdFreezeRamp, holdTrack_[p], p);
  holdRamps_ = 0;
}

// len± on the synth page: the oscillator wave, stepped through SINE TRI SAW SQR FAT (SPEC §9.17c).
void Ui::waveStep(int track, int dir) {
  const int w = ((int)(paramValue(track, PWave) + 0.5f) + dir + WaveCount) % WaveCount;
  push(CmdSetParam, track, PWave, 0, (float)w);
  sessionDirty();
}

// tempo± steps 1 BPM (hold repeats); SHIFT + tempo± drifts ±10 BPM over 60 beats and survives the SHIFT release
// (SPEC §9.17c). A running drift never locks the keys: a plain tempo± freezes it and steps from the tempo it reached
// (the engine's PTempo ramp would otherwise overwrite CmdSetTempo on the next tick), the same SHIFT + tempo key again
// stops it where it is, the other one freezes it and drifts the other way from there.
void Ui::tempoKey(int dir, KeyEvent::Kind kind, uint32_t now) {
  const bool drifting = driftUntilTick_ && (int32_t)(eng_->status().tick - driftUntilTick_) < 0;
  if (shift_) {
    if (kind == KeyEvent::Repeat) return;
    const int was = drifting ? driftDir_ : 0;
    if (drifting) stopDrift();
    if (was != dir) {
      const int target = clampi((int)eng_->status().bpm + dir * kTempoDriftBpm, kMinBpm, kMaxBpm);
      push(CmdRamp, 0, PTempo, kTempoDriftBeats * kPPQN, (float)target);
      driftUntilTick_ = eng_->status().tick + (uint32_t)(kTempoDriftBeats * kPPQN); driftDir_ = (int8_t)dir;
    }
  } else {
    if (drifting) stopDrift();
    const int bpm = clampi(baseBpm(now) + dir, kMinBpm, kMaxBpm);
    push(CmdSetTempo, 0, 0, bpm);
    bpmSent_ = bpm; bpmSentMs_ = now;
  }
  openParamPage(PPTempo, now);
  sessionDirty();
}

void Ui::stopDrift() { push(CmdFreezeRamp, 0, PTempo); driftUntilTick_ = 0; driftDir_ = 0; }

// SHIFT + key±: the selected track's pattern an octave up / down (SPEC §9.17c), a 0.6 s "OCTAVE +1" page. Only a
// synth track with notes has anything to transpose (Generator::transposeOctave does nothing otherwise): the others get
// a status line, no page and no session rewrite.
void Ui::octaveKey(int dir, uint32_t now) {
  if (!gen_) return;
  if (!isSynthCategory(trackCategory(selected_))) { showStatus("OCTAVE: SYNTH TRACKS ONLY"); return; }
  if (eng_->sourcePattern(selected_).count == 0) { showStatus("OCTAVE: NO PATTERN"); return; }
  gen_->transposeOctave(selected_, dir);
  char t[16]; snprintf(t, sizeof t, "OCTAVE %+d", dir);
  showNote(t, categoryColor(trackCategory(selected_)), now);
  sessionDirty();
}

// SHIFT + genre: acid mode of the selected Chord/Lead/Pad/Bass track (SPEC §9.17c); the generator re-rolls the
// sound (303 family on / library preset off), so it is refused while the sets state machine owns the card.
void Ui::acidToggle(uint32_t now) {
  (void)now;
  if (!gen_) return;
  const Category c = trackCategory(selected_);
  if (!isKnobTrack(c)) { showStatus("ACID: SYNTH TRACKS ONLY"); return; }
  if (loopTrack(selected_)) { showStatus("ACID: LOOP TRACK"); return; }   // the track streams a loop (SPEC §9.19)
  if (sets_ && sets_->busy()) { showStatus("SETS BUSY"); return; }
  const bool on = !gen_->acidEnabled(selected_);
  gen_->setAcid(selected_, on);
  if (on) { char cat[12]; catUpper(c, cat, sizeof cat); char t[32]; snprintf(t, sizeof t, "ACID ON S%d %s", trackSet(selected_) + 1, cat); showStatus(t); }
  else showStatus("ACID OFF");
  sessionDirty();
}

// SHIFT + edit (SPEC §9.19): loop mode of the selected track. ON asks the generator for a loop of the track's category
// (one JobLoop: the pick, the file, CmdLoopLoaded + CmdLoopMode at the bar line; the Vocal track streams it through its
// set's own vocal stream), OFF sends the parked pattern and sound back at the bar. Statuses "LOOP ON S1 BEAT" / "LOOP
// OFF S1 BEAT"; a refusal shows the generator's reason (loopRefusalText). A save / load / restore owns the card too and
// the generator cannot see the Sets machine, so that case is refused here with the same "LOOP: BUSY".
void Ui::loopToggle(uint32_t now) {
  (void)now;
  if (!gen_) return;
  const int t = selected_; const Category c = trackCategory(t);
  const bool on = !gen_->loopEnabled(t);
  if (sets_ && sets_->busy()) { showStatus("LOOP: BUSY"); return; }
  if (!gen_->setLoopMode(t, on)) { char m[32]; loopRefusalText(gen_->lastLoopRefusal(), c, m, sizeof m); showStatus(m); return; }
  if (on) loopPickMask_ |= (uint16_t)(1u << t);                          // the square blinks while the pick is in flight
  char cat[12]; catUpper(c, cat, sizeof cat);
  char m[40]; snprintf(m, sizeof m, "LOOP %s S%d %s", on ? "ON" : "OFF", trackSet(t) + 1, cat);   // ≤ 22 characters ("LOOP OFF S1 PERCUSSION"); 40 keeps gcc's truncation check quiet
  showStatus(m);
  edDirty_ = true;                                                        // the editor's loop view follows a toggle made inside it
  sessionDirty();
}

// Loops are rendered at kLoopBpm (docs/LOOPS.md) and varispeed like a vocal: the first loop to come into use pulls
// the tempo to 125 ("LOOP ON - 125 BPM") so it plays at rate 1; tempo± afterwards varispeeds every loop (contract
// §1.3). A running tempo drift is left alone, and so is the boot restore (Sets busy() together with sessionBusy():
// the session brings back the tempo the user had with these very loops): the edge is then consumed without a tempo
// change. A user set load is not exempt — Sets applies the file's lp.bpm only while nothing plays, so the first loop a
// load brings in while the other set runs must still pull to 125. Returns false only when the engine's command queue
// was full: nothing was sent or shown, and the caller keeps the edge for the next tick.
bool Ui::loopTempoRule(uint32_t now) {
  const bool drifting = driftUntilTick_ && (int32_t)(eng_->status().tick - driftUntilTick_) < 0;
  const bool restoring = sets_ && sets_->busy() && sets_->sessionBusy();
  if (drifting || restoring) return true;
  if (!push(CmdSetTempo, 0, 0, kLoopBpm)) return false;
  bpmSent_ = kLoopBpm; bpmSentMs_ = now;
  char m[32]; snprintf(m, sizeof m, kLoopTempoStatusFmt, kLoopBpm);
  showStatus(m);
  sessionDirty();
  return true;
}

// Loop mode requested and the engine not yet streaming the track (the pick is on the card, or the bar line has not
// come), or a generator job this Ui started re-picks the loop of a track that already streams.
bool Ui::loopPickPending(int track) const {
  if (!gen_ || !eng_ || track < 0 || track >= kTracks || !gen_->loopEnabled(track)) return false;
  if (!eng_->trackStreams(track)) return true;
  return gen_->busy() && ((loopPickMask_ >> track) & 1) != 0;
}

// The loop slots' underruns since loop mode came into use (the baseline follows the counters while no loop is in use).
uint32_t Ui::loopUnderruns() const {
  if (!eng_) return 0;
  uint32_t n = 0;
  for (int k = 0; k < kLoopSlots; k++) n += (uint16_t)(eng_->streamUnderruns(kSets + k) - loopUnderrunBase_[k]);
  return n;
}

// kind 0 = set (arg = set), 1 = pattern (arg = track), 2 = sound (arg = track), 3 = THEORY set (arg = set, SPEC §9.24:
// Generator::generateTheorySet — one chord progression for the melodic tracks, the library path for the drums and the
// vocal, the same bar-line swap and the same BUSY refusals as a set generate; its progress reads "THEORY S1 n/8"). A
// pattern that will be replaced drops its edited flag (a kept user loop keeps it, SPEC §9.10 / §9.17c). Gen pattern on a
// track holding a recorded loop is refused here (the loop stays until hold-del clears it, SPEC §9.17c); gen sound leaves
// the pattern alone.
void Ui::requestGen(int kind, int arg, uint32_t now) {
  (void)now;
  if (!gen_) return;
  if (kind == 1 && gen_->info(arg).userLoop) { showStatus("LOOP KEPT: HOLD DEL CLEARS"); return; }
  if (sets_ && sets_->busy()) { showStatus("SETS BUSY"); return; }        // one card state machine at a time (SPEC §9.2): both would share staging / slotAck
  const bool wholeSet = kind == 0 || kind == 3;
  if (wholeSet) arg &= 1;
  const bool ok = kind == 0 ? gen_->generateSet(arg) : kind == 3 ? gen_->generateTheorySet(arg) : kind == 1 ? gen_->generatePattern(arg) : gen_->generateSound(arg);
  if (!ok) { showStatus("GEN BUSY"); return; }
  if (wholeSet) {
    snprintf(genLabel_, sizeof genLabel_, kind == 3 ? kTheoryLabelFmt : "GEN S%d", arg + 1);
    for (int i = 0; i < kTracksPerSet; i++) { TrackGenInfo& inf = gen_->info(arg * kTracksPerSet + i); if (!inf.userLoop) inf.edited = 0; }
  } else if (kind == 1) {
    copyStr(genLabel_, sizeof genLabel_, "GEN PAT");
    TrackGenInfo& inf = gen_->info(arg); if (!inf.userLoop) inf.edited = 0;
  } else copyStr(genLabel_, sizeof genLabel_, "GEN SND");
  if (wholeSet) { for (int i = 0; i < kTracksPerSet; i++) { const int t = arg * kTracksPerSet + i; if (loopTrack(t)) loopPickMask_ |= (uint16_t)(1u << t); } }   // a generate on a loop track picks a new loop (SPEC §9.19): the square blinks meanwhile
  else if (loopTrack(arg)) loopPickMask_ |= (uint16_t)(1u << arg);
  genWasBusy_ = true;
  genTotal_ = wholeSet ? kTracksPerSet : 1;                               // what the queued job will report once it starts
  genStaleDone_ = gen_->progress(); genStaleTotal_ = gen_->total();       // the previous job's numbers, shown as 0/n until then
  snprintf(progress_, sizeof progress_, "%s 0/%d", genLabel_, genTotal_); progressOn_ = true;
  statusUntil_ = 0;                                                       // a job that starts supersedes the previous job's result message
}

bool Ui::sdBusy() const { return (gen_ && gen_->busy()) || (sets_ && sets_->busy()); }

// Save / load run as the Sets state machine, stepped by the loop (SPEC §9.2); refused while the generator owns
// the card (a save would serialise sources the job is replacing, a load would race it on staging / slotAck).
void Ui::saveSet(int set, uint32_t now) {
  (void)now;
  if (!sets_) return;
  if (sets_->busy()) { showStatus("SETS BUSY"); return; }
  if (gen_ && gen_->busy()) { showStatus("GEN BUSY"); return; }
  if (!sets_->beginSave(set)) { showStatus(sets_->lastError()[0] ? sets_->lastError() : "SAVE FAILED"); return; }
  setsOp_ = 1; setsSet_ = (uint8_t)set;
  snprintf(progress_, sizeof progress_, "SAVE S%d 0/%d", set + 1, sets_->total()); progressOn_ = true; statusUntil_ = 0;
}

// The load Rng is stirred with the clock so two boots do not load the same sequence of sets — except while a session is
// recorded or rendered (SPEC §9.23): rng_ was seeded from the session's seed at that start (recorderToggle, tick), and
// the live clock (millis) and the replay clock (renderNowMs) differ, so mixing `now` in would make a random LOAD inside
// a recording pick another set on replay (test_rec a_random_load_inside_a_recording_picks_the_same_set_on_replay).
void Ui::loadSet(int set, uint32_t now) {
  if (!sets_) return;
  if (sets_->busy()) { showStatus("SETS BUSY"); return; }
  if (gen_ && gen_->busy()) { showStatus("GEN BUSY"); return; }
  rng_ = Rng(rng_.next() ^ (recording() || rendering() ? 0u : now));
  if (!sets_->beginLoad(set, rng_)) { showStatus(sets_->lastError()[0] ? sets_->lastError() : "LOAD FAILED"); return; }
  setsOp_ = 2; setsSet_ = (uint8_t)set;
  snprintf(progress_, sizeof progress_, "LOAD S%d 0/%d", set + 1, sets_->total()); progressOn_ = true; statusUntil_ = 0;
}

// ---------------------------------------------------------------------------------------------- gyro (SPEC §3, §9.11)

void Ui::setGyro(bool on, uint32_t now) {
  gyroOn_ = on; gyroFilter_ = gyroRev_ = gyroEcho_ = 0;
  if (on) {
    captureNeutral();                                                     // neutral captured at toggle-on (SPEC §3)
    push(CmdGyro, 0, 1, 0, 0.0f); gyroSentMs_ = now;
  } else push(CmdGyro, 0, 0, 0, 0.0f);
  recordGyro(0.0f, 0, 0);                                                 // SPEC §9.23: the neutral command (off = neutral values)
  showNote(on ? "GYRO ON" : "GYRO OFF", kNeonGreen, now);
}

// SPEC §3: the axis assignment and signs come from lp/config.txt (imu.pitch_axis = x | -x | y | -y ...) until the
// orientation is confirmed on the device; whoever parses the file calls this. The default is the probe's guess.
void Ui::setImuMapping(int pitchAxis, int pitchSign, int rollAxis, int rollSign) {
  if (pitchAxis < 0 || pitchAxis > 2 || rollAxis < 0 || rollAxis > 2 || pitchAxis == rollAxis) return;
  imuPitchAxis_ = (uint8_t)pitchAxis; imuRollAxis_ = (uint8_t)rollAxis;
  imuPitchSign_ = pitchSign < 0 ? -1 : 1; imuRollSign_ = rollSign < 0 ? -1 : 1;
  if (gyroOn_) captureNeutral();                                          // the old neutral angles mean nothing in the new frame
}

// One mapped axis against the remaining ("up") axis: pitch = atan2(±ax, az) with the default x / y mapping.
float Ui::imuAngle(const ImuReading& r, int axis, int sign) const {
  const float v[3] = {r.ax, r.ay, r.az};
  const int up = 3 - imuPitchAxis_ - imuRollAxis_;                        // the axes differ (setImuMapping), so this is the third one
  return atan2f((float)sign * v[axis], v[up]);
}

void Ui::captureNeutral() {
  const ImuReading n = lastImu_.valid ? lastImu_ : ImuReading();
  neutralPitch_ = imuAngle(n, imuPitchAxis_, imuPitchSign_); neutralRoll_ = imuAngle(n, imuRollAxis_, imuRollSign_);
}

// Forward tilt → HP (+filter), backward → LP (−), roll left → reverb, right → echo; ±10° dead band, full at 45°
// beyond it, one-pole τ 60 ms at 50 Hz, one CmdGyro per 20 ms at most. Axes and signs via setImuMapping.
void Ui::onImu(const ImuReading& r, uint32_t now) {
  lastNow_ = now; lastImu_ = r;
  if (!gyroOn_ || !r.valid) return;
  const float dp = wrapDeg((imuAngle(r, imuPitchAxis_, imuPitchSign_) - neutralPitch_) * kRadToDeg);
  const float dr = wrapDeg((imuAngle(r, imuRollAxis_, imuRollSign_) - neutralRoll_) * kRadToDeg);
  const float ft = mapTilt(dp), rt = mapTilt(dr);
  const float revT = rt < 0 ? -rt : 0.0f, echoT = rt > 0 ? rt : 0.0f;
  gyroFilter_ += (ft - gyroFilter_) * kGyroSmooth;
  gyroRev_ += (revT - gyroRev_) * kGyroSmooth;
  gyroEcho_ += (echoT - gyroEcho_) * kGyroSmooth;
  if (now - gyroSentMs_ < kGyroSendMs) return;
  gyroSentMs_ = now;
  const int rev = (int)(gyroRev_ * 255.0f + 0.5f), echo = (int)(gyroEcho_ * 255.0f + 0.5f);
  push(CmdGyro, 0, 1, (rev << 8) | echo, gyroFilter_);
  recordGyro(gyroFilter_, rev, echo);                                     // SPEC §9.23 type 2
}

// ---------------------------------------------------------------------------------------------- pages

void Ui::goMain(uint32_t now) { (void)now; page_ = PgMain; pageUntil_ = 0; fullRedraw_ = true; }

void Ui::openParamPage(PageParam p, uint32_t now) {
  if (modal()) return;                                                    // overlays never replace a modal page (SPEC §9.14)
  if (page_ != PgParam || paramPage_ != p) fullRedraw_ = true;
  page_ = PgParam; paramPage_ = p; pageUntil_ = now + kParamPageMs;
}

// 0.6 s note page ("S1 BEAT 3 +SNARE", SPEC §9.14); inside a modal page it becomes a status line instead.
void Ui::showNote(const char* text, Color c, uint32_t now) {
  if (modal()) { showStatus(text); return; }
  copyStr(note_, sizeof note_, text); noteColor_ = c;
  page_ = PgNote; pageUntil_ = now + kNotePageMs; fullRedraw_ = true;
}

// Every modal page freezes a hold-ramp still held on the main page first: the key's release would land on the new
// page, where it is dropped or means something else, and the ramp would run to the end (SPEC §9.17c).
void Ui::openEditor(uint32_t now) {
  (void)now;
  freezeHoldRamps();
  edTrack_ = selected_; edCursorX_ = edCursorY_ = 0; edPage_ = 0; edPlayCol_ = -1;
  page_ = PgEditor; pageUntil_ = 0; edDirty_ = true; fullRedraw_ = true;
}

// The perform page plays `track` (SPEC §9.17c): its own voices for a synth track, the kit slots for Beat /
// Percussion, nothing for a Vocal. The looper records into that track.
void Ui::openPerform(int track, uint32_t now) {
  (void)now;
  if (loopTrack(clampi(track, 0, kTracks - 1))) { showStatus("PERFORM: LOOP TRACK"); return; }   // a streamed loop has no voices to play (SPEC §9.19)
  freezeHoldRamps();
  perfTrack_ = (int8_t)clampi(track, 0, kTracks - 1);
  memset(perfHeld_, 0, sizeof perfHeld_); memset(perfHeldChord_, 0, sizeof perfHeldChord_); memset(perfHeldTick_, 0xFF, sizeof perfHeldTick_); memset(perfShadow_, 0xFF, sizeof perfShadow_);
  for (int i = 0; i < kPerfPendMax; i++) perfPend_[i].used = 0;
  perfBeatShown_ = -1; perfHeader_[0] = 0; delDown_ = false;
  if (gen_ && gen_->info(perfTrack_).userLoop) {                          // an existing loop: overdub / clear apply to it
    rec_ = eng_->sourcePattern(perfTrack_); loopTicks_ = rec_.lengthTicks ? rec_.lengthTicks : kTicksPerBar; recStart_ = 0; looper_ = LpLooping;
  } else { looper_ = LpIdle; loopTicks_ = 0; }
  page_ = PgPerform; pageUntil_ = 0; perfDirty_ = true; fullRedraw_ = true;
}

// SHIFT + pat1..4 of a row: the synth page of that set's Chord / Lead / Pad / Bass (SPEC §9.17c). A hold-ramp of
// the page we leave is frozen first (its release would otherwise land on a differently mapped key).
void Ui::openSynth(int track, uint32_t now) {
  (void)now;
  if (!isKnobTrack(trackCategory(track))) return;
  freezeHoldRamps();
  synthTrack_ = (int8_t)track;
  page_ = PgSynth; pageUntil_ = 0; synthDirty_ = true; fullRedraw_ = true;
}

// Leaving the perform page never loses what was played: an overdub is published, a take with notes closes at its
// bar line (tick() keeps dispatching bars outside the page), an empty take or a mere arm is dropped.
void Ui::closeModal(uint32_t now) {
  if (page_ == PgPerform) {
    perfReleaseAll(now);                                                  // records the note-offs of held keys first
    switch (looper_) {
      case LpArmed: looper_ = LpIdle; break;
      case LpRecording:
        if (rec_.count) { looperCloseRequest(now); if (looper_ == LpClosing) showStatus("LOOP CLOSING"); } else looper_ = LpIdle;
        break;
      case LpOverdub: looper_ = LpLooping; publishLoop(now); break;       // the merged loop becomes the track's source (SPEC §9.10)
      default: break;                                                     // LpClosing finishes at its bar line; LpLooping / LpIdle need nothing
    }
  }
  if (page_ == PgPresets) { psRename_ = 0; psDelArmUntil_ = 0; }          // a half-typed label is dropped with the page
  if (page_ == PgRecorder) rcDelArmUntil_ = 0;
  freezeHoldRamps();                                                      // a knob still held on the synth page: its ramp stops here
  goMain(now);
}

// ---------------------------------------------------------------------------------------------- synth page (SPEC §9.17c, §9.22)

// vol± → cutoff, rev± → resonance, echo± → env amount, dist± → decay, filt± → drive, tempo± → SAT, key± → ACC and the
// gyro / edit keys (8,2) / (8,3) → CHO (all with hold-ramps and SHIFT ramps), len± → wave (stepped). Exit: the same
// SHIFT + pat combo (another combo switches the page to that track) or `. Track keys are not live here; the edit key is
// CHO− since SPEC §9.22 (it used to exit).
void Ui::handleSynth(const KeyEvent& e, const KeyMapping& m, uint32_t now) {
  const bool press = e.kind == KeyEvent::Press;
  if (m.fn == KParamUp || m.fn == KParamDown) {
    const int dir = m.fn == KParamUp ? 1 : -1;
    const ParamId k = synthKnobParam((ParamId)m.a);
    if (k == ParamCount) return;
    if (k == PWave) { if (press) waveStep(synthTrack_, dir); return; }
    continuousKey(synthTrack_, k, dir, e.kind, now);
    return;
  }
  if (m.fn == KGyro || m.fn == KEdit) { continuousKey(synthTrack_, PChorus, m.fn == KGyro ? 1 : -1, e.kind, now); return; }   // CHO ± (SPEC §9.22)
  if (!press) return;
  if (e.x == 0 && e.y == 0) { closeModal(now); return; }
  if (m.fn == KVariant && shift_) {
    const int t = m.a * kTracksPerSet + m.b;
    if (t == synthTrack_) closeModal(now); else openSynth(t, now);
  }
}

// ---------------------------------------------------------------------------------------------- editor (SPEC §4, §9.14)

void Ui::handleEditor(const KeyEvent& e, const KeyMapping& m, uint32_t now) {
  if (e.kind == KeyEvent::Release) return;
  const bool press = e.kind == KeyEvent::Press;
  if (press && m.fn == KEdit && shift_) { loopToggle(now); return; }     // SHIFT + edit inside the editor: the same toggle, the page follows (SPEC §9.19)
  if (press && (m.fn == KEdit || (e.x == 0 && e.y == 0))) { closeModal(now); return; }
  if (loopTrack(edTrack_)) return;                                        // the loop view: no grid, no edits (SPEC §9.19)
  const Category c = trackCategory(edTrack_);
  const int rows = edRows();
  if (e.x == 11 && e.y == 2) { if (edCursorY_ > 0) edCursorY_--; edDirty_ = true; return; }            // ;  up
  if (e.x == 11 && e.y == 3) { if (edCursorY_ < rows - 1) edCursorY_++; edDirty_ = true; return; }     // .  down
  if (e.x == 10 && e.y == 3) { edCursorX_ = (edCursorX_ + 15) % 16; edDirty_ = true; return; }         // ,  left
  if (e.x == 12 && e.y == 3) { edCursorX_ = (edCursorX_ + 1) % 16; edDirty_ = true; return; }          // /  right
  if (e.x == 13 && e.y == 2) { if (press && rows > 0) edToggle(now); return; }                         // enter toggles
  if (m.fn != KParamUp && m.fn != KParamDown) return;
  const int dir = m.fn == KParamUp ? 1 : -1;
  switch (m.a) {
    case PVolume: if (rows > 0) edVelocity(dir, now); break;                                           // velocity ±10
    case PTempo: edPage_ = clampi(edPage_ + dir, 0, edBars() - 1); edDirty_ = true; break;             // pages bars
    case PLength:                                                                                      // octave (melodic) / clip length (vocal)
      if (c == Vocal && gen_) { lenFrom_ = gen_->setLengthStep(edTrack_, 0); lenTo_ = gen_->setLengthStep(edTrack_, dir); sessionDirty(); }
      else if (isSynthCategory(c)) edOctave_ = clampi(edOctave_ + dir, 1, 7);
      edDirty_ = true;
      break;
    case PDist: if (isSynthCategory(c)) { edGate_ = clampi(edGate_ + dir, 1, 4); edDirty_ = true; } break;   // gate 1..4 steps
    default: break;
  }
}

int Ui::edRows() const { const Category c = trackCategory(edTrack_); return isSampleCategory(c) ? 4 : (c == Vocal ? 0 : 7); }
int Ui::edBars() const {
  const int bars = (eng_->sourcePattern(edTrack_).lengthTicks + kTicksPerBar - 1) / kTicksPerBar;
  return bars < 1 ? 1 : bars;
}

// A Percussion pitch the stored fold map (SPEC §9.7) puts on `voice` (36..39): one already in the source first.
int Ui::percVoicePitch(int track, int voice) const {
  const uint8_t* map = gen_ ? gen_->foldMap(trackSet(track)) : nullptr;
  if (!map) return voice;
  const Pattern& src = eng_->sourcePattern(track);
  for (int i = 0; i < src.count; i++) if (map[src.notes[i].pitch()] == voice) return src.notes[i].pitch();
  for (int p = 36; p < 128; p++) if (map[p] == voice) return p;
  for (int p = 0; p < 36; p++) if (map[p] == voice) return p;
  return voice;
}

// Row → pitch. Beat rows are the §9.6 contract (36..39); Percussion rows are the four voices of the stored fold
// map (SPEC §9.7): the pitch written is one the map folds onto that voice; melodic rows are scale degrees of the
// track's key, bottom row = root (SPEC §4).
int Ui::edRowPitch(int row) const {
  const Category c = trackCategory(edTrack_);
  if (c == Beat) return 36 + row;
  if (c == Percussion) return percVoicePitch(edTrack_, 36 + row);
  ProjectKey k; k.root = gen_ ? gen_->info(edTrack_).keyRoot : 9;
  return k.degreePitch(6 - row, edOctave_);
}

// Pitch → row for display: Beat accents (40..58) sit on the OH row, Percussion through the fold map, melodic
// notes on the nearest scale row of the shown octave (within 2 semitones), else −1 = not on this page.
int Ui::edRowOf(int pitch) const {
  const Category c = trackCategory(edTrack_);
  if (c == Beat) return pitch >= 36 && pitch <= 39 ? pitch - 36 : (pitch >= 40 && pitch <= 58 ? 3 : -1);
  if (c == Percussion) {
    const uint8_t* map = gen_ ? gen_->foldMap(trackSet(edTrack_)) : nullptr;
    const int v = map ? map[pitch & 127] - 36 : (pitch & 3);
    return v >= 0 && v < 4 ? v : -1;
  }
  int best = -1, bestD = 1000;
  for (int r = 0; r < 7; r++) { int d = pitch - edRowPitch(r); if (d < 0) d = -d; if (d < bestD) { bestD = d; best = r; } }
  return bestD <= 2 ? best : -1;
}

bool Ui::edPitchExact(int pitch, int row) const {
  const Category c = trackCategory(edTrack_);
  if (c == Beat) return pitch <= 39;
  if (c == Percussion) return true;
  return edRowPitch(row) == pitch;
}

void Ui::edRowLabel(int row, char* out, int cap) const {
  const Category c = trackCategory(edTrack_);
  if (c == Beat) { static const char* n[4] = {"KICK", "SNR", "HH", "OH"}; copyStr(out, cap, n[row & 3]); return; }
  if (c == Percussion) { copyStr(out, cap, kPercSlotName[row & 3]); return; }
  copyStr(out, cap, noteName(edRowPitch(row) % 12));
}

int Ui::edNoteAt(uint16_t tick, int pitch) const {
  const Pattern& src = eng_->sourcePattern(edTrack_);
  for (int i = 0; i < src.count; i++) if (src.notes[i].start() == tick && src.notes[i].pitch() == pitch) return i;
  return -1;
}

// enter: add / remove the note at the cursor in the UI-owned source (SPEC §4 "edits go to the original").
void Ui::edToggle(uint32_t now) {
  Pattern& src = eng_->sourcePattern(edTrack_);
  const uint16_t tick = edTick(); const int pitch = edRowPitch(edCursorY_);
  const int idx = edNoteAt(tick, pitch);
  if (idx >= 0) {
    for (int i = idx + 1; i < src.count; i++) src.notes[i - 1] = src.notes[i];
    src.count--;
  } else {
    const uint16_t len = isSampleCategory(trackCategory(edTrack_)) ? (uint16_t)kNoteLenUnit : (uint16_t)(edGate_ * kSixteenth);
    if (tick >= src.lengthTicks) src.lengthTicks = (uint16_t)((edPage_ + 1) * kTicksPerBar);
    if (!src.add(Note::make(tick, len, (uint8_t)pitch, kVelDefault))) { showStatus("PATTERN FULL"); return; }
    src.sortByStart();
  }
  edCommit(now);
}

void Ui::edVelocity(int dir, uint32_t now) {
  Pattern& src = eng_->sourcePattern(edTrack_);
  const int idx = edNoteAt(edTick(), edRowPitch(edCursorY_));
  if (idx < 0) return;
  Note& n = src.notes[idx];
  n.setVel((uint8_t)clampi((int)n.vel() + dir * 10, 1, 127));
  edCommit(now);
}

// After every edit: flag the track, rebuild the current variant from the edited source (SPEC §4).
void Ui::edCommit(uint32_t now) {
  (void)now;
  edDirty_ = true;
  if (!gen_) return;
  TrackGenInfo& inf = gen_->info(edTrack_);
  inf.edited = 1;
  inf.sourceTicks = eng_->sourcePattern(edTrack_).lengthTicks;
  const int v = inf.loaded ? inf.variant : (isSampleCategory(trackCategory(edTrack_)) ? 3 : 0);
  gen_->rebuildVariant(edTrack_, v);
  sessionDirty();
}

// ---------------------------------------------------------------------------------------------- perform (SPEC §9.10, §9.17c)

// Raw matrix positions: ` exits; tab starts/stops the track; - / = octave (melodic); del = looper; [ cycles the
// quantise (OFF 1/16 1/8 1/4); \ toggles CHORD mode (Chord / Pad); grid keys play (synth tracks: the degree grid of
// SPEC §9.10; Beat / Percussion: row y0 keys x1..x8 = slots); a Vocal has no keys but the exits.
void Ui::handlePerform(const KeyEvent& e, const KeyMapping& m, uint32_t now) {
  (void)m;
  if (e.kind == KeyEvent::Repeat) return;
  const bool press = e.kind == KeyEvent::Press;
  const Category c = perfCategory();
  if (press && e.x == 0 && e.y == 0) { closeModal(now); return; }        // ` is the only exit (SPEC §9.10): (9,3) is grid degree 15
  if (e.x == 0 && e.y == 1) { if (press) toggleTrack(perfTrack_); return; }                            // tab: the track's start/stop
  if (c == Vocal) return;                                                                               // NO PERFORM FOR VOCAL
  if (e.y == 0 && (e.x == 11 || e.x == 12)) {                                                           // - / = : octave (melodic tracks)
    if (press && perfMelodic()) { perfOctave_ = clampi(perfOctave_ + (e.x == 11 ? -1 : 1), 1, 6); perfDirty_ = true; }
    return;
  }
  if (e.x == 13 && e.y == 0) {                                                                          // del: looper
    if (press) { delDown_ = true; delPressMs_ = now; }
    else if (delDown_) { delDown_ = false; if (e.heldMs >= kLooperHoldMs || now - delPressMs_ >= kLooperHoldMs) looperClear(now); else looperPress(now); }
    return;
  }
  if (e.x == 11 && e.y == 1) {                                                                          // [ : quantise OFF 1/16 1/8 1/4
    if (press) { perfQuant_ = (uint8_t)((perfQuant_ + 1) % kPerfQuantSteps); char t[16]; snprintf(t, sizeof t, "QUANT %s", kQuantName[perfQuant_]); showStatus(t); perfDirty_ = true; }
    return;
  }
  if (e.x == 13 && e.y == 1) {                                                                          // \ : CHORD mode (Chord / Pad)
    if (!press) return;
    if (c == Chord || c == Pad) { perfChord_ = !perfChord_; showStatus(perfChord_ ? "CHORD ON" : "CHORD OFF"); perfDirty_ = true; }
    else showStatus("CHORD: CHORD/PAD ONLY");
    return;
  }
  if (isSampleCategory(c)) { if (perfDrumSlot(e.x, e.y) < 0) return; }
  else if (perfDegree(e.x, e.y) < 0) return;
  if (press) perfNoteOn(e.x, e.y, now); else perfNoteOff(e.x, e.y, now);
}

// Beat: row y0 keys x1..x8 → slots 0..7 (pitches 36..39 kick snare hh oh, 40..43 accents, SPEC §9.6); Percussion: x1..x4 → voices A..D.
int Ui::perfDrumSlot(int x, int y) const {
  if (y != 0 || x < 1 || x > kPerfDrumSlots) return -1;
  const int slot = x - 1;
  if (perfCategory() == Percussion && slot >= 4) return -1;
  return slot;
}

int Ui::perfPitch(int degree) const {
  ProjectKey k; if (gen_) k = gen_->key();
  int p = k.degreePitch(degree % 7, perfOctave_ + degree / 7);
  if (shift_) p++;                                                        // momentary chromatic (SPEC §9.10)
  return clampi(p, 0, 127);
}

// What a key plays now: a drum slot, a scale degree, or in CHORD mode the diatonic triad degree / +2 / +4 (SPEC §9.17c).
int Ui::perfPitches(int x, int y, int out[3]) const {
  const Category c = perfCategory();
  if (isSampleCategory(c)) { const int s = perfDrumSlot(x, y); if (s < 0) return 0; out[0] = 36 + s; return 1; }
  if (!isSynthCategory(c)) return 0;
  const int d = perfDegree(x, y); if (d < 0) return 0;
  out[0] = perfPitch(d);
  if (perfChord_ && (c == Chord || c == Pad)) { out[1] = perfPitch(d + 2); out[2] = perfPitch(d + 4); return 3; }
  return 1;
}

// The pitches a held key sounds, from the root and the packed triad intervals stored at note-on (the octave, SHIFT
// or CHORD mode may have changed since: the note-off must match the note-on).
int Ui::perfHeldPitches(int x, int y, int out[3]) const {
  if (!perfHeld_[y][x]) return 0;
  const int root = perfHeld_[y][x] - 1; out[0] = root;
  const uint8_t ch = perfHeldChord_[y][x];
  if (!ch) return 1;
  out[1] = clampi(root + (ch >> 4), 0, 127); out[2] = clampi(root + (ch & 15), 0, 127);
  return 3;
}

// Key-down: with the quantise on and the transport running the note waits for the next grid point (tick()
// fires it); otherwise it sounds now (SPEC §9.17c).
void Ui::perfNoteOn(int x, int y, uint32_t now) {
  if (perfHeld_[y][x]) return;
  int p[3]; const int n = perfPitches(x, y, p);
  if (!n) return;
  perfHeld_[y][x] = (uint8_t)(p[0] + 1);
  perfHeldChord_[y][x] = n == 3 ? (uint8_t)(((p[1] - p[0]) << 4) | (p[2] - p[0])) : 0;
  const EngineStatus& st = eng_->status();
  const int q = quantTicks(perfQuant_);
  if (q && st.running) {
    const uint32_t fire = ((st.tick + (uint32_t)q - 1) / (uint32_t)q) * (uint32_t)q;
    if (fire > st.tick) {
      for (int i = 0; i < kPerfPendMax; i++) if (!perfPend_[i].used) { PerfPend& pd = perfPend_[i]; pd.used = 1; pd.x = (uint8_t)x; pd.y = (uint8_t)y; pd.released = 0; pd.tick = fire; return; }
    }
  }
  perfFire(x, y, st.tick, false, now);
}

// The note-on itself: one CmdPerformNote, or three CmdPerformChord in CHORD mode, on the perform track; the looper
// gets the (grid) tick. A key released before its grid point sounds a short note there.
void Ui::perfFire(int x, int y, uint32_t tick, bool releaseNow, uint32_t now) {
  int p[3]; const int n = perfHeldPitches(x, y, p);
  if (!n) return;
  const CmdType t = n == 3 ? CmdPerformChord : CmdPerformNote;
  for (int i = 0; i < n; i++) push(t, perfTrack_, 0, p[i], kPerfVelocity);
  looperRecordOn(x, y, tick);
  if (releaseNow) perfNoteOff(x, y, now);
}

void Ui::perfNoteOff(int x, int y, uint32_t now) {
  if (!perfHeld_[y][x]) return;
  for (int i = 0; i < kPerfPendMax; i++) if (perfPend_[i].used && perfPend_[i].x == x && perfPend_[i].y == y) { perfPend_[i].released = 1; return; }   // fires, then ends, at its grid point
  int p[3]; const int n = perfHeldPitches(x, y, p);
  perfHeld_[y][x] = 0; perfHeldChord_[y][x] = 0;
  const CmdType t = n == 3 ? CmdPerformChord : CmdPerformNote;
  for (int i = 0; i < n; i++) push(t, perfTrack_, 0, p[i], 0.0f);
  looperRecordOff(x, y, p, n, now);
}

// Every held key ends: pending (never sounded) keys are dropped, sounding ones get their note-off.
void Ui::perfReleaseAll(uint32_t now) {
  for (int i = 0; i < kPerfPendMax; i++) if (perfPend_[i].used) { PerfPend& pd = perfPend_[i]; pd.used = 0; perfHeld_[pd.y][pd.x] = 0; perfHeldChord_[pd.y][pd.x] = 0; perfHeldTick_[pd.y][pd.x] = 0xFFFF; }
  for (int y = 0; y < kKeyRows; y++) for (int x = 0; x < kKeyCols; x++) if (perfHeld_[y][x]) perfNoteOff(x, y, now);
}

// tick(): quantised note-ons whose grid point falls inside the next render block (at most kBlock samples ahead of
// the published tick, from the engine's samples per tick) sound now, so the command is queued before the engine
// crosses the point instead of a block (plus the loop period) after it; the recorded tick stays the grid point. A
// stopped transport fires them at once.
void Ui::perfPending(uint32_t now) {
  const EngineStatus& st = eng_->status();
  const uint64_t blockFp = (uint64_t)kBlock << 16;
  for (int i = 0; i < kPerfPendMax; i++) {
    PerfPend& pd = perfPend_[i];
    if (!pd.used) continue;
    if (st.running) {
      const int32_t ahead = (int32_t)(pd.tick - st.tick);
      if (ahead > 0 && (uint64_t)ahead * (uint64_t)eng_->samplesPerTickFp() > blockFp) continue;
    }
    pd.used = 0;
    perfFire(pd.x, pd.y, st.running ? pd.tick : st.tick, pd.released != 0, now);
  }
}

// del short press: arm → (count-in) record → close at the next bar line rounded up to 1/2/4 bars → loop,
// then overdub on/off.
void Ui::looperPress(uint32_t now) {
  switch (looper_) {
    case LpIdle: looper_ = LpArmed; break;
    case LpArmed: looper_ = LpIdle; break;
    case LpRecording: looperCloseRequest(now); break;
    case LpClosing: break;
    case LpLooping: looper_ = LpOverdub; break;
    case LpOverdub: looper_ = LpLooping; publishLoop(now); break;
  }
  perfDirty_ = true;
}

// Close at the next bar line rounded up to 1/2/4 bars (SPEC §9.10). tick() dispatches every bar line kSwapLeadTicks
// early, so a press inside that window (the last ≈ 40 ms of a bar at 120 BPM) targets a bar that has already been
// dispatched: close now, with that bar line as the end, instead of slipping a whole bar.
void Ui::looperCloseRequest(uint32_t now) {
  const int bars = (int)((eng_->status().tick - recStart_) / kTicksPerBar) + 1;
  closeTick_ = recStart_ + (uint32_t)roundUpBars(bars) * kTicksPerBar;
  looper_ = LpClosing;
  if ((int32_t)(closeTick_ - lastBarIndex_ * (uint32_t)kTicksPerBar) <= 0) looperClose(closeTick_, now);
}

// Hold 700 ms: the loop is dropped, the track plays silence until the next generate re-rolls it.
void Ui::looperClear(uint32_t now) {
  (void)now;
  looper_ = LpIdle; rec_.clear(); loopTicks_ = 0;
  memset(perfHeldTick_, 0xFF, sizeof perfHeldTick_);
  const int t = perfTrack_;
  eng_->sourcePattern(t).clear();
  if (gen_) { TrackGenInfo& inf = gen_->info(t); inf.userLoop = 0; inf.edited = 0; inf.file[0] = 0; gen_->rebuildVariant(t, isSampleCategory(perfCategory()) ? 3 : 0); }
  showStatus("LOOP CLEARED");
  perfDirty_ = true;
  sessionDirty();
}

// Called a few ticks ahead of every bar line (kSwapLeadTicks) so a publish lands on that bar (SPEC §9.5).
void Ui::looperBar(uint32_t barTick, uint32_t now) {
  switch (looper_) {
    case LpArmed: recStart_ = barTick; rec_.clear(kTicksPerBar); looper_ = LpRecording; perfDirty_ = true; break;
    case LpRecording: if (barTick - recStart_ >= (uint32_t)kLooperMaxBars * kTicksPerBar) looperClose(barTick, now); break;
    case LpClosing: if ((int32_t)(barTick - closeTick_) >= 0) looperClose(barTick, now); break;
    default: break;
  }
}

void Ui::addRec(uint16_t start, uint16_t len, int pitch) {
  if (len < kSixteenth) len = kSixteenth;
  if (len > 255 * kNoteLenUnit) len = 255 * kNoteLenUnit;
  rec_.add(Note::make(start, len, (uint8_t)pitch, (uint8_t)(kPerfVelocity * 127.0f + 0.5f)));
}

void Ui::looperClose(uint32_t barTick, uint32_t now) {
  uint32_t len = barTick - recStart_;
  if (len < (uint32_t)kTicksPerBar) len = kTicksPerBar;
  if (len > (uint32_t)kLooperMaxBars * kTicksPerBar) len = kLooperMaxBars * kTicksPerBar;
  loopTicks_ = (uint16_t)len;
  for (int y = 0; y < kKeyRows; y++) for (int x = 0; x < kKeyCols; x++) {   // notes still held end at the loop end
    if (!perfHeld_[y][x] || perfHeldTick_[y][x] == 0xFFFF) continue;
    const uint16_t on = perfHeldTick_[y][x];
    int p[3]; const int n = perfHeldPitches(x, y, p);
    if (on < loopTicks_) for (int i = 0; i < n; i++) addRec(on, (uint16_t)(loopTicks_ - on), recPitch(p[i]));
    perfHeldTick_[y][x] = 0xFFFF;
  }
  // The engine is phase-locked (position = globalTick mod len, SPEC §9.4), so stored starts must be global phases:
  // rotate the relative ticks by the record origin's phase inside the loop (a 2-bar take from an odd bar → +384).
  const uint16_t phase = (uint16_t)(recStart_ % loopTicks_);
  for (int i = 0; i < rec_.count; i++) rec_.notes[i].setStart((uint16_t)((rec_.notes[i].start() + phase) % loopTicks_));
  rec_.lengthTicks = loopTicks_;
  rec_.sortByStart();
  looper_ = LpLooping;
  publishLoop(now);
  const PlayState s = eng_->status().track[perfTrack_].state;
  if (s != PSPlaying && s != PSQueued) push(CmdTrackToggle, perfTrack_);   // the loop starts on the bar it closes on
  perfDirty_ = true;
}

// While recording, ticks are relative to the record origin (rotated to global phase at close); an overdub lands on
// a loop the engine already plays phase-locked, so it quantises the global phase (tick mod loop) directly. The
// timestamp snaps to the perform quantise grid (1/16 when OFF, SPEC §9.17c).
void Ui::looperRecordOn(int x, int y, uint32_t tick) {
  if (looper_ != LpRecording && looper_ != LpClosing && looper_ != LpOverdub) { perfHeldTick_[y][x] = 0xFFFF; return; }
  const uint16_t g = recGrid();
  uint16_t q;
  if (looper_ == LpOverdub && loopTicks_) { q = quantTo(tick % loopTicks_, g); if (q >= loopTicks_) q = 0; }
  else { int32_t rel = (int32_t)(tick - recStart_); if (rel < 0) rel = 0; q = quantTo((uint32_t)rel, g); }
  perfHeldTick_[y][x] = q;
}

void Ui::looperRecordOff(int x, int y, const int* pitches, int n, uint32_t now) {
  const uint16_t on = perfHeldTick_[y][x]; perfHeldTick_[y][x] = 0xFFFF;
  if (on == 0xFFFF) return;
  if (looper_ != LpRecording && looper_ != LpClosing && looper_ != LpOverdub) return;
  const uint32_t tick = eng_->status().tick;
  int len;
  if (looper_ == LpOverdub && loopTicks_) { const uint16_t off = (uint16_t)(quantize16(tick % loopTicks_) % loopTicks_); len = off >= on ? off - on : loopTicks_ - on + off; }
  else { int32_t rel = (int32_t)(tick - recStart_); if (rel < 0) rel = 0; len = (int)quantize16((uint32_t)rel) - on; }
  for (int i = 0; i < n; i++) addRec(on, (uint16_t)(len < kSixteenth ? kSixteenth : len), recPitch(pitches[i]));
  if (looper_ == LpRecording && rec_.count >= kMaxNotes) looperCloseRequest(now);   // 192 notes: close at the next rounded bar
}

// Percussion slots sound as pitch 36 + slot live (CmdPerformNote) but are stored as a pitch the set's stored fold map
// (SPEC §9.7) puts on that voice, like the editor writes them: the published loop is rebuilt through that map
// (Generator::rebuildVariant), which ranks the loaded file's pitches, so 36 + slot itself could land on another voice.
int Ui::recPitch(int pitch) const { return perfCategory() == Percussion ? percVoicePitch(perfTrack_, pitch) : pitch; }

// The loop becomes the perform track's source; userLoop makes GENERATE / gen pattern keep it (SPEC §9.10, §9.17c).
// Drum tracks publish as the Full layer / density so every recorded hit sounds.
void Ui::publishLoop(uint32_t now) {
  (void)now;
  const int t = perfTrack_;
  eng_->sourcePattern(t) = rec_;
  if (!gen_) return;
  TrackGenInfo& inf = gen_->info(t);
  const int v = isSampleCategory(perfCategory()) ? 3 : 0;
  inf.userLoop = 1; inf.edited = 1; inf.variant = (uint8_t)v; inf.keyRoot = gen_->key().root; inf.sourceTicks = rec_.lengthTicks; inf.loaded = 1;
  gen_->rebuildVariant(t, v);
  sessionDirty();
}

// ---------------------------------------------------------------------------------------------- tick

void Ui::tick(uint32_t now) {
  lastNow_ = now;
  if (!started_) { started_ = true; pageUntil_ = now + kSplashMs; }
  if (statusArmed_) { statusArmed_ = false; statusUntil_ = now + statusArmMs_; }
  if (!eng_) return;
  const EngineStatus& st = eng_->status();
  { Ev ev; int guard = 0; while (guard++ < 64 && eng_->events().pop(ev)) {} }   // drained; bars are derived from the clock below
  const uint32_t barIdx = (st.tick + kSwapLeadTicks) / kTicksPerBar;
  if (barIdx != lastBarIndex_) { lastBarIndex_ = barIdx; looperBar(barIdx * kTicksPerBar, now); }
  perfPending(now);
  if (page_ == PgSplash) { if ((int32_t)(now - pageUntil_) >= 0) goMain(now); }
  else if (page_ == PgParam) {
    const bool shiftRamp = shift_ && paramPage_ <= PPTempo && (rampsActive_ & paramBit(paramPage_));
    const bool holdRamp = paramPage_ < PPLength && (holdRamps_ & paramBit(paramPage_));
    if (shiftRamp || holdRamp) pageUntil_ = now + kParamPageMs;           // stays while its ramp runs
    if ((int32_t)(now - pageUntil_) >= 0) goMain(now);
  } else if (page_ == PgNote) { if ((int32_t)(now - pageUntil_) >= 0) goMain(now); }
  if (gyroKeyDown_ && !gyroToggled_ && now - gyroHeldSince_ >= (uint32_t)kHoldToggleMs) { gyroToggled_ = true; setGyro(!gyroOn_, now); }
  if (page_ == PgPerform && delDown_ && now - delPressMs_ >= kLooperHoldMs) { delDown_ = false; looperClear(now); }
  // SHIFT + gen pattern held (SPEC §9.22): the status first, the hook (a reboot into the drive firmware) kUsbDriveStatusMs
  // later so the text reaches the screen; without a hook the status says why nothing happens. Not during an offline render.
  // A replayed hold (the only way the key reaches usbDriveKey during a render: live keys stop at onKey) neither reboots nor
  // shows anything under the overlay — live it ended the recording with the reboot, so the replay just runs out.
  if (usbState_ == 1 && now - usbArmMs_ >= kUsbDriveHoldMs) { usbState_ = 2; usbArmMs_ = now; if (!rendering()) showStatus("USB DRIVE...", 5000); }
  else if (usbState_ == 2 && now - usbArmMs_ >= kUsbDriveStatusMs) {
    if (rendering()) {} else if (usbDriveHook_) usbDriveHook_(); else showStatus("NO DRIVE FIRMWARE");
    usbState_ = usbDown_ ? 3 : 0;
  }
  if (sets_ && page_ == PgPresets) {                                      // preset manager (SPEC §9.22): the list request / page / label write, stepped by the loop's Sets::step()
    // The re-list requests wait for a save / load started from a page (setsOp_) to be REPORTED below: Sets::beginPresetList
    // resets ok() / lastError() and makes Sets busy again, so a listing started first would turn a failed load (OPEN SET, a
    // missing asset, NO SET) into "LOADED <name>" with the listing's ok() — and AUTO would start a set that did not load.
    if (psOp_ == 1) { if (!setsOp_) presetRequestPage(); }               // Sets was busy when the page asked: try again
    else if (psOp_ == 2 && sets_->presetListReady()) {
      psOp_ = 0; psDirty_ = true;
      const int total = sets_->presetListTotal();
      if (psCursor_ >= total) psCursor_ = (int16_t)(total > 0 ? total - 1 : 0);
      if (psTop_ > psCursor_) psTop_ = psCursor_;
      if (!presetRowLoaded(psCursor_) && total > 0) presetRequestPage();   // the page that came back does not hold the cursor (a delete shrank the list)
    } else if (psOp_ == 3 && !sets_->busy()) {                            // the label is written (Sets updates its row in place; a lost page is re-listed)
      psOp_ = 0; showStatus(sets_->ok() ? "LABEL SET" : (sets_->lastError()[0] ? sets_->lastError() : "LABEL FAILED"));
      psDirty_ = true;
    } else if (psOp_ == 4 && !sets_->busy()) {                            // the delete finished (Sets dropped the row and the count itself)
      psOp_ = 0; showStatus(sets_->ok() ? "DELETED" : (sets_->lastError()[0] ? sets_->lastError() : "DELETE FAILED"));
      psDirty_ = true;
    }
    if (psOp_ == 0 && psFirst_ >= 0 && !sets_->presetListReady() && !sets_->busy() && !setsOp_) presetRequestPage();   // the rows were overwritten by a load / save: list again (once that load has reported, see above)
    if (psOp_ == 0 && psFirst_ >= 0 && sets_->presetListReady()) {       // a finished delete may have emptied the cursor's row
      const int total = sets_->presetListTotal();
      if (psCursor_ >= total && total >= 0) { psCursor_ = (int16_t)(total > 0 ? total - 1 : 0); if (psTop_ > psCursor_) psTop_ = psCursor_; psDirty_ = true; }
    }
    if (psDelArmUntil_ && (int32_t)(now - psDelArmUntil_) >= 0) psDelArmUntil_ = 0;
  }
  if (recorder_) {                                                        // session recorder (SPEC §9.23)
    if (page_ == PgRecorder) {
      if (rcDelArmUntil_ && (int32_t)(now - rcDelArmUntil_) >= 0) rcDelArmUntil_ = 0;
      const int rows = rcCount_ - rcTop_ < kRecRowsVisible ? rcCount_ - rcTop_ : kRecRowsVisible;
      if (rcInfoN_ < rows) {                                              // one header read per tick: the visible rows' bars and WAV marks
        const int i = rcTop_ + rcInfoN_; RecHeader h;
        rcBars_[rcInfoN_] = (int16_t)(recorder_->sessionInfo(i, h) ? (int)(h.lengthTicks / (uint32_t)kTicksPerBar) : -1);
        if (recorder_->hasWav(i)) rcWav_ |= (uint8_t)(1u << rcInfoN_); else rcWav_ &= (uint8_t)~(1u << rcInfoN_);
        rcInfoN_++; rcDirty_ = true;
      }
    }
    // The render ended (or was cancelled) since the last tick: DONE for 3 s, or the status, then the main page. Checked BEFORE
    // the cancel below so a cancel takes effect on the following tick — Recorder::renderStep delivers the step's replayed
    // events after ticking the Ui, and those must not land on the page the cleanup just left.
    if (recorder_->rendering()) {                                         // a render the sketch started (serial `rec render`): as recorderRender, before its first replayed event (renderStep ticks first)
      if (!rcWasRendering_) { rcWasRendering_ = true; rcLiveShift_ = shift_; rng_ = Rng(recorder_->sessionSeed()); if (modal()) closeModal(now); }
    }
    else if (rcWasRendering_) {
      rcWasRendering_ = rcOverlayDrawn_ = false; rcCancelArmed_ = false;
      // The replay's key state must not outlive it (a recording can end with SHIFT down, a gyro / drive / del key held, a knob
      // ramping): SHIFT back to the physical key, every held key let go, the ramps frozen, the page closed as ` would.
      if (shift_ != rcLiveShift_) { shift_ = rcLiveShift_; if (!shift_) onShiftRelease(now); }
      gyroKeyDown_ = false; usbDown_ = false; usbState_ = 0; delDown_ = false; loopRushKeys_ = 0;
      if (rcCancelled_) { rcCancelled_ = false; showStatus("RENDER CANCELLED"); } else rcDoneUntil_ = now + kRenderDoneMs;
      if (modal()) closeModal(now); else { freezeHoldRamps(); goMain(now); }
    }
    if (rcCancelArmed_ && recorder_->rendering()) {                       // ` held on the RENDERING overlay (a replayed tap releases before the hold)
      if (!rcCancelMs_) rcCancelMs_ = now;
      else if (now - rcCancelMs_ >= kRenderCancelHoldMs) { rcCancelArmed_ = false; rcCancelled_ = true; recorder_->cancelRender(); }
    }
    if (rcDoneUntil_ && (int32_t)(now - rcDoneUntil_) >= 0) { rcDoneUntil_ = 0; fullRedraw_ = true; }
  }
  for (int s = 0; s < kSets; s++) {
    if (queuedVariant_[s] && !st.generating[s]) { const int v = queuedVariant_[s] - 1; queuedVariant_[s] = 0; applyVariant(queuedVariantTrack_[s], v, now); }
    if (queuedLen_[s] && !st.generating[s] && gen_) {                     // len± queued during generation: applied after the swap (SPEC §9.5)
      const int n = queuedLen_[s], dir = n > 0 ? 1 : -1, track = queuedLenTrack_[s]; queuedLen_[s] = 0;
      lenFrom_ = gen_->setLengthStep(track, 0);
      for (int i = 0; i < (n > 0 ? n : -n); i++) lenTo_ = gen_->setLengthStep(track, dir);
      openParamPage(PPLength, now);
      sessionDirty();
    }
    if (now - kickCutMs_[s] > kKickCutSyncMs) kickCut_[s] = st.track[s * kTracksPerSet + Beat].kickCut;
  }
  if (gen_) {                                                             // loop mode (SPEC §9.19): the tempo-to-125 edge, the underrun baseline, the pick blink
    const bool any = gen_->anyLoop();
    anyLoopSeen_ = (any && !anyLoopSeen_) ? loopTempoRule(now) : any;   // the false → true edge runs the rule; a full command queue keeps the edge for the next tick
    if (!any) for (int k = 0; k < kLoopSlots; k++) loopUnderrunBase_[k] = eng_->streamUnderruns(kSets + k);
    if (!gen_->busy()) loopPickMask_ = 0;
  }
  // Job progress lives in progress_ and never overwrites an explicit showStatus() message (statusWant() prefers
  // the message while it lasts), so "PATTERN FULL", "SAVED …" or a note inside a modal page survive the job.
  progressOn_ = false;
  if (gen_) {                                                             // generation progress / result (SPEC §4 status overlay)
    if (gen_->busy()) {
      int done = gen_->progress(), total = gen_->total();
      if (genTotal_ && done == genStaleDone_ && total == genStaleTotal_) { done = 0; total = genTotal_; }   // not started yet
      else genStaleDone_ = genStaleTotal_ = -1;
      snprintf(progress_, sizeof progress_, "%s %d/%d", genLabel_[0] ? genLabel_ : "GEN", done, total);
      progressOn_ = true; genWasBusy_ = true;
    } else if (genWasBusy_) {
      genWasBusy_ = false; genLabel_[0] = 0; genTotal_ = 0;
      if (gen_->lastError()[0]) showStatus(gen_->lastError()); else if (gen_->lastNote()[0]) showStatus(gen_->lastNote()); else showStatus("GEN DONE");
    }
  }
  if (sets_ && setsOp_) {                                                 // resumable save / load, stepped by the loop (SPEC §9.2)
    if (sets_->busy()) { snprintf(progress_, sizeof progress_, "%s S%d %d/%d", setsOp_ == 1 ? "SAVE" : "LOAD", setsSet_ + 1, sets_->progress(), sets_->total()); progressOn_ = true; }
    else {
      char t[48];
      if (sets_->ok()) snprintf(t, sizeof t, "%s %.38s", setsOp_ == 1 ? "SAVED" : "LOADED", sets_->lastName());
      else copyStr(t, sizeof t, sets_->lastError()[0] ? sets_->lastError() : "FAILED");
      showStatus(t); setsOp_ = 0;
      if (psPlayAfter_) { psPlayAfter_ = 0; if (sets_->ok()) push(CmdPlaySet, 0, setsSet_); }   // preset manager AUTO (SPEC §9.22): the loaded set starts at the bar
    }
  }
  if (sets_ && page_ == PgPresets && psOp_ >= 2 && sets_->busy() && !setsOp_) {   // the preset manager's own card work (SPEC §9.22), as a progress text
    copyStr(progress_, sizeof progress_, psOp_ == 2 ? "LISTING..." : psOp_ == 3 ? "WRITING LABEL..." : "DELETING..."); progressOn_ = true;
  }
  if (sets_ && !sets_->sessionBusy()) {                                   // a session restore brought lp.volume: apply it once (SPEC §9.18) ...
    const int v = sets_->sessionVolume();
    if (v >= 0 && v != (int)sessionVolSeen_) { sessionVolSeen_ = (int16_t)v; if (!volExplicit_) applyRestoredVolume(v); }   // ... unless config / serial / a key set the level since boot
  }
  if (statusUntil_ && (int32_t)(now - statusUntil_) >= 0) statusUntil_ = 0;
  if (outVolFooterUntil_ && (int32_t)(now - outVolFooterUntil_) >= 0) outVolFooterUntil_ = 0;   // "OUT 78%" leaves the footer
}

// ---------------------------------------------------------------------------------------------- drawing

void Ui::textCentered(int y, const char* text, int size, Color c) {
  int x = (kScreenW - gfx_->textWidth(text, size)) / 2;
  if (x < 0) x = 0;
  gfx_->drawText(x, y, text, size, c);
}

// The RENDERING overlay covers every page while an offline render runs (the replay drives the pages underneath, SPEC
// §9.23), the DONE line follows it for kRenderDoneMs; otherwise the page draws.
void Ui::draw(uint32_t now) {
  if (!gfx_ || !eng_) return;
  if (everDrawn_ && now - lastDraw_ < kDrawIntervalMs) return;            // SPEC §7: at most every 40 ms
  everDrawn_ = true; lastDraw_ = now;
  const bool full = fullRedraw_; fullRedraw_ = false;
  gfx_->startWrite();
  if (rendering()) { drawRendering(full || !rcOverlayDrawn_); rcOverlayDrawn_ = true; }
  else if (rcDoneUntil_) { if (full) drawRenderDone(); }
  else {
    switch (page_) {
      case PgSplash: if (full) drawSplash(); break;
      case PgMain: case PgStatus: drawMain(now, full); break;
      case PgParam: drawParam(full); break;
      case PgNote: if (full) drawNote(); break;
      case PgEditor: drawEditor(full); break;
      case PgPerform: drawPerform(full); break;
      case PgSynth: drawSynth(full); break;
      case PgPresets: drawPresets(full); break;
      case PgRecorder: drawRecorder(full); break;
    }
  }
  gfx_->endWrite();
  gfx_->present();
}

void Ui::drawSplash() {
  gfx_->fillScreen(kBlack);
  textCentered(40, "meehaw", 4, kNeonGreen);
  textCentered(82, "live performer", 1, kNeonGreen);
}

// Main page: 2 × 8 squares diffed cell by cell against the shadow (SPEC §4, §9.14).
void Ui::drawMain(uint32_t now, bool full) {
  if (full) {
    gfx_->fillScreen(kBlack);
    for (int t = 0; t < kTracks; t++) {
      cell_[t] = CellLook();
      const Category c = trackCategory(t);
      gfx_->drawText(kCellX0 + (t % kTracksPerSet) * kCellPitch + 1, kRowY[t / kTracksPerSet] - kLabelDy, categoryShort(c), 1, scaleColor(categoryColor(c), 60));
    }
    footerShown_[0] = 0; batShown_[0] = 0; statusShown_[0] = 0; recMarkShown_ = 0;
  }
  const EngineStatus& st = eng_->status();
  for (int t = 0; t < kTracks; t++) {
    const TrackStatus& ts = st.track[t];
    CellLook look;
    look.state = ts.state;
    look.loop = loopTrack(t) ? 1 : 0;                                      // the 'L' mark (SPEC §9.19) ...
    look.pick = loopPickPending(t) ? 1 : 0;                                // ... and the blink while its loop pick is in flight
    look.blink = (ts.state == PSQueued || ts.state == PSStopQueued || look.pick) ? (uint8_t)((now / kBlinkHalfMs) & 1) : 0;
    look.sel = t == selected_;
    look.ticks = look.sel ? (uint8_t)((ts.variant & 3) + 1) : 0;
    look.cut = trackCategory(t) == Beat ? ts.kickCut : 0;
    look.acid = (gen_ && isKnobTrack(trackCategory(t)) && gen_->acidEnabled(t)) ? 1 : 0;   // the 'A' marker (SPEC §9.17c)
    if (memcmp(&look, &cell_[t], sizeof look) != 0) { drawCell(t, look); cell_[t] = look; }
  }
  drawFooter(false);
  drawRecMark(now, false);
  drawStatusLine(false);
}

void Ui::drawCell(int t, const CellLook& k) {
  const int x = kCellX0 + (t % kTracksPerSet) * kCellPitch, y = kRowY[t / kTracksPerSet];
  const Color col = categoryColor(trackCategory(t)), dimc = scaleColor(col, 30);
  const bool filled = k.state == PSPlaying;
  if (filled) {
    gfx_->fillRect(x, y, kCellSize, kCellSize, (k.pick && k.blink) ? scaleColor(col, 50) : col);   // a loop pick in flight blinks the square (SPEC §9.19)
    gfx_->fillRect(x, y + kCellSize, kCellSize, kCellBoxH - kCellSize, kBlack);
  } else {
    gfx_->fillRect(x, y, kCellSize, kCellBoxH, kBlack);
    Color oc = dimc;
    if (k.state == PSStopped) oc = col;
    else if (k.state == PSQueued || k.state == PSStopQueued) oc = k.blink ? col : dimc;
    if (k.pick) oc = k.blink ? col : dimc;
    gfx_->drawRect(x, y, kCellSize, kCellSize, oc);
  }
  if (k.sel) { const int s = kCellSize * 30 / 100, o = (kCellSize - s) / 2; gfx_->fillRect(x + o, y + o, s, s, filled ? kBlack : categoryColor(Beat)); }   // selection square: the Beat grey, not white (too glaring on a dark cell)
  if (k.cut) gfx_->drawLine(x, y, x + kCellSize - 1, y + kCellSize - 1, filled ? kBlack : col);
  if (k.acid) gfx_->drawText(x + kCellSize - 7, y + 1, "A", 1, filled ? kBlack : col);   // small acid marker, top right
  if (k.loop) gfx_->drawText(x + 2, y + 1, "L", 1, filled ? kBlack : col);               // loop mode marker, top left (SPEC §9.19)
  for (int i = 0; i < k.ticks && i < kVariants; i++) gfx_->fillRect(x + 3 + i * 6, y + kCellTickY, 4, 2, categoryColor(Beat));   // variant ticks in the same grey
}

// The left footer text, fitted to maxWidth px (the room left of the battery readout): key, BPM, genre and
// the selected clip's length, then the GYRO note and, briefly after a SHIFT + kick cut step, the output
// level. The notes are whole segments: a line too wide for the room first shrinks its double-space
// separators to single ones, then drops the OUT note, then GYRO, and only a base that still does not fit
// is cut character-wise (fitTextWidth) — a note is shown whole or not at all, never as a partial number.
// While a loop slot has underrun (SPEC §9.19) the line carries "LOOP UNDERRUNS n" as well: that diagnostic
// outranks the OUT and GYRO notes, the genre name and even the clip length (a dropout matters more than the
// pack the user chose or the LEN they can read on the param page), so with it up the drop order is OUT, GYRO,
// genre, LEN; only when the note still does not fit is it dropped and the line built as without it. The count
// is written in thousands from 10 000 (loopUnderrunText: the engine counts samples, 13 312 in half a second of
// starvation), so the widest line that keeps the note, "C# MIN 240 LOOP UNDERRUNS 131k" (30 characters), fits
// beside the widest readout, "CHG 100%" (240 − 1 − 48 − 4 − 2 = 185 px ≥ 30 × 6): the note is never dropped
// in practice.
void Ui::footerText(char* out, int cap, int maxWidth) const {
  const EngineStatus& st = eng_->status();
  char genre[16]; upperStr(genre, sizeof genre, gen_ ? gen_->genreName() : "");
  if (!genre[0]) copyStr(genre, sizeof genre, "NO PACK");
  char len[8]; formatBeats(len, sizeof len, st.track[selected_].lenTicks);
  char outv[12] = "";
  if (outVolFooterUntil_ && (int32_t)(lastNow_ - outVolFooterUntil_) < 0) snprintf(outv, sizeof outv, "OUT %d%%", volumePercent(outVol_));
  const char* gyro = gyroOn_ ? "GYRO" : "";
  char loop[24] = "";
  const uint32_t ur = loopUnderruns();
  if (ur) { char n[8]; loopUnderrunText(n, sizeof n, ur); snprintf(loop, sizeof loop, "LOOP UNDERRUNS %s", n); }
  for (int pass = loop[0] ? 0 : 1; pass < 2; pass++) {                    // pass 0: with the LOOP note (drop levels 3 / 4 = without the genre / LEN), pass 1: without it
    const char* n3 = pass == 0 ? loop : "";
    for (int drop = 0; drop < (pass == 0 ? 5 : 3); drop++) {              // 0: every note, 1: without OUT, 2: without GYRO either, 3: without the genre, 4: without LEN
      for (int narrow = 0; narrow < 2; narrow++) {                        // double-space separators, then single
        const char* sep = narrow ? " " : "  ";
        const char* g = drop < 3 ? genre : "";
        const char* n0 = drop < 4 ? len : ""; const char* n1 = drop < 2 ? gyro : ""; const char* n2 = drop < 1 ? outv : "";
        snprintf(out, (size_t)cap, "%s MIN %d%s%s%s%s%s%s%s%s%s%s%s", gen_ ? gen_->key().rootName() : "A", st.bpm, g[0] ? " " : "", g,
                 n0[0] ? sep : "", n0[0] ? "LEN " : "", n0, n1[0] ? sep : "", n1, n2[0] ? sep : "", n2, n3[0] ? sep : "", n3);
        if (!gfx_ || gfx_->textWidth(out, 1) <= maxWidth) return;
      }
    }
  }
  fitTextWidth(gfx_, out, maxWidth, 1);                                   // last resort: the bare base, cut
}

// The footer's loop-underrun count: the engine's sample count as it is below 10 000, in thousands from there ("13k";
// the two uint16 slot counters sum to at most 131 070 = "131k"), so the note is at most "LOOP UNDERRUNS 131k".
void Ui::loopUnderrunText(char* out, int cap, uint32_t n) {
  if (n < 10000) snprintf(out, (size_t)cap, "%u", (unsigned)n);
  else snprintf(out, (size_t)cap, "%uk", (unsigned)(n / 1000));
}

// Footer line (SPEC §4): the left text and the battery readout right-aligned at the right edge, each diffed
// against its own shadow and repainted only when its own text changed (the display is drawn straight, so a
// needless repaint blinks). A changed left text clears the line up to the gap before the readout, a changed
// readout alone clears the line right of the left text (the old readout sat there too), both clear it whole.
// The left text is fitted (footerText) so it never runs into the readout, kFooterGap px before it.
// While a session recording runs (SPEC §9.23) a blinking REC sits kFooterGap left of the readout (drawRecMark) and the
// left text is fitted to the room before it.
void Ui::drawFooter(bool force) {
  char bat[12]; Color batCol; batteryText(bat, sizeof bat, batCol);
  const int batX = kScreenW - kFooterBatMargin - gfx_->textWidth(bat, 1);
  const int recW = recording() ? gfx_->textWidth("REC", 1) + kFooterGap : 0;
  char buf[64]; footerText(buf, sizeof buf, batX - kFooterGap - recW - kFooterTextX);
  const bool leftChanged = force || strcmp(buf, footerShown_) != 0;
  const bool batChanged = force || strcmp(bat, batShown_) != 0;
  if (!leftChanged && !batChanged) return;
  copyStr(footerShown_, sizeof footerShown_, buf); copyStr(batShown_, sizeof batShown_, bat);
  const int x0 = leftChanged ? 0 : kFooterTextX + gfx_->textWidth(buf, 1) + kFooterGap;
  const int x1 = batChanged ? kScreenW : batX - kFooterGap - recW;
  gfx_->fillRect(x0, kFooterY, x1 - x0, kScreenH - kFooterY, kBlack);
  if (leftChanged) gfx_->drawText(kFooterTextX, kFooterY + 1, buf, 1, kDimGrey);
  if (batChanged) { gfx_->drawText(batX, kFooterY + 1, bat, 1, batCol); recMarkShown_ = 0; }   // the marker's spot was cleared with the readout
}

// Main footer REC marker (SPEC §9.23): "REC" in red, lit / dark every kBlinkHalfMs while recording, kFooterGap left of
// the battery readout; repainted only when its phase changes, cleared once when the recording stops.
void Ui::drawRecMark(uint32_t now, bool force) {
  const uint8_t want = recording() ? (uint8_t)(((now / kBlinkHalfMs) & 1) ? 2 : 1) : 0;
  if (!force && want == recMarkShown_) return;
  recMarkShown_ = want;
  const int w = gfx_->textWidth("REC", 1), x = kScreenW - kFooterBatMargin - gfx_->textWidth(batShown_, 1) - kFooterGap - w;
  gfx_->fillRect(x, kFooterY, w, kScreenH - kFooterY, kBlack);
  if (want == 1) gfx_->drawText(x, kFooterY + 1, "REC", 1, kRed);
}

void Ui::drawStatusLine(bool force) {
  const char* want = statusWant();
  if (!force && strcmp(want, statusShown_) == 0) return;
  copyStr(statusShown_, sizeof statusShown_, want);
  gfx_->fillRect(0, kStatusY, kScreenW, kStatusH, kBlack);
  if (want[0]) textCentered(kStatusY + 2, want, 1, kWhite);
}

void Ui::paramValueText(char* out, int cap, int& barPct) const {
  const EngineStatus& st = eng_->status();
  barPct = 0;
  switch (paramPage_) {
    case PPVolume: case PPReverb: case PPEcho: case PPDist: {
      const float v = paramValue(selected_, (ParamId)paramPage_); fmtFixed(out, cap, v, 2, false); barPct = (int)(v * 100.0f + 0.5f); break; }
    case PPFilter: { const float v = paramValue(selected_, PFilter); fmtFixed(out, cap, v, 1, true); barPct = (int)((v + 1.0f) * 50.0f + 0.5f); break; }
    case PPLength: {
      if (trackCategory(selected_) == Vocal) {                                         // SPEC §9.16: len± steps the pause
        if (gen_ && gen_->info(selected_).variant >= 2) { copyStr(out, cap, "NO PAUSE"); barPct = 0; break; }   // loop variants: nothing to step
        const int p = gen_ ? Generator::clampPause(gen_->info(selected_).pause) : kVocalPauseDefault;
        if (p == 0) copyStr(out, cap, "PAUSE 0"); else snprintf(out, (size_t)cap, "PAUSE %d BAR%s", p, p == 1 ? "" : "S");
        barPct = p == 0 ? 0 : p == 1 ? 33 : p == 2 ? 66 : 100; break;
      }
      const uint16_t len = lenTo_ ? lenTo_ : st.track[selected_].lenTicks;
      char b[8]; formatBeats(b, sizeof b, len); snprintf(out, (size_t)cap, "%s BEATS", b);
      int k = 0; for (uint32_t l = len; l > (uint32_t)(kTicksPerBeat / 2) && k < 5; l >>= 1) k++;
      barPct = k * 20; break; }
    case PPTempo: snprintf(out, (size_t)cap, "%d", st.bpm); barPct = ((int)st.bpm - kMinBpm) * 100 / (kMaxBpm - kMinBpm); break;
    case PPKey: { const int root = gen_ ? gen_->key().root : 9; snprintf(out, (size_t)cap, "%s MIN", noteName(root)); barPct = circleIndex(root) * 100 / 11; break; }
    case PPMaster: barPct = volumePercent(outVol_); snprintf(out, (size_t)cap, "%d%%", barPct); break;   // output volume as percent of 255
    case PPGenre: upperStr(out, cap, gen_ ? gen_->genreName() : ""); if (!out[0]) copyStr(out, cap, "NONE"); barPct = gen_ ? (gen_->genreIndex() % 8) * 100 / 7 : 0; break;
    case PPRampBars: snprintf(out, (size_t)cap, "%d BARS", rampBars_); barPct = rampIndex(rampBars_) * 25; break;
    default:                                                                            // a synth knob (no page of its own): its real-unit value
      if ((int)paramPage_ >= (int)PCutoff && (int)paramPage_ <= (int)PChorus) { const float v = paramValue(selected_, (ParamId)paramPage_); knobValueText((ParamId)paramPage_, v, out, cap); barPct = (int)paramPage_ == (int)PWave ? (int)v * 100 / (WaveCount - 1) : (int)(v * 100.0f + 0.5f); }
      else out[0] = 0;
      break;
  }
  barPct = clampi(barPct, 0, 100);
}

// Parameter page: name small, value huge, bar, one neon colour per parameter (SPEC §4); live value each draw.
void Ui::drawParam(bool full) {
  const Color col = pageParamColor(paramPage_);
  if (full) {
    gfx_->fillScreen(kBlack);
    textCentered(6, pageParamName(paramPage_), 2, col);
    gfx_->drawRect(20, 92, 200, 10, col);
    valueShown_[0] = 0; subShown_[0] = 0; barShown_ = -1;
  }
  char val[24]; int pct = 0; paramValueText(val, sizeof val, pct);
  if (strcmp(val, valueShown_) != 0) {
    copyStr(valueShown_, sizeof valueShown_, val);
    gfx_->fillRect(0, 40, kScreenW, 40, kBlack);
    const int size = (int)strlen(val) * 24 <= kScreenW ? 4 : 3;
    textCentered(size == 4 ? 44 : 48, val, size, col);
  }
  if (pct != barShown_) {
    barShown_ = pct;
    gfx_->fillRect(21, 93, 198, 8, kBlack);
    if (pct > 0) gfx_->fillRect(21, 93, 198 * pct / 100, 8, col);
  }
  char sub[40]; sub[0] = 0;
  const int set = trackSet(selected_) + 1; const char* cat = categoryShort(trackCategory(selected_));
  const EngineStatus& st = eng_->status();
  if (paramPage_ == PPLength && trackCategory(selected_) == Vocal) {                   // the cycle this pause makes (SPEC §9.16)
    const int variant = gen_ ? gen_->info(selected_).variant : 0;
    if (variant >= 2) snprintf(sub, sizeof sub, "S%d %.4s  LOOP %d BEAT%s", set, cat, variant == 2 ? 2 : 1, variant == 2 ? "S" : "");   // the clip is the loop, no pause
    else {
      const int bars = kVocalWindowBars + (gen_ ? Generator::clampPause(gen_->info(selected_).pause) : kVocalPauseDefault);
      snprintf(sub, sizeof sub, "S%d %.4s  CYCLE %d BARS", set, cat, bars);
    }
  }
  else if (paramPage_ == PPLength) { char a[8], b[8]; formatBeats(a, sizeof a, lenFrom_); formatBeats(b, sizeof b, lenTo_); snprintf(sub, sizeof sub, "S%d %.4s  %.5s -> %.5s", set, cat, a, b); }
  else if (paramPage_ < PPLength) snprintf(sub, sizeof sub, "S%d %s%s", set, cat, (rampsActive_ & paramBit(paramPage_)) ? "  RAMP" : (holdRamps_ & paramBit(paramPage_)) ? "  HOLD" : "");
  else if (paramPage_ == PPTempo && (rampsActive_ & paramBit(PTempo))) copyStr(sub, sizeof sub, "RAMP");
  else if (paramPage_ == PPTempo && driftUntilTick_ && (int32_t)(st.tick - driftUntilTick_) < 0) copyStr(sub, sizeof sub, "DRIFT");   // SHIFT + tempo (SPEC §9.17c)
  else if (paramPage_ == PPMaster) copyStr(sub, sizeof sub, "SHIFT+CUT  S1 UP  S2 DOWN");
  if (strcmp(sub, subShown_) != 0) {
    copyStr(subShown_, sizeof subShown_, sub);
    gfx_->fillRect(0, 110, kScreenW, 10, kBlack);
    if (sub[0]) textCentered(111, sub, 1, kDimGrey);
  }
}

void Ui::drawNote() {
  gfx_->fillScreen(kBlack);
  const int size = (int)strlen(note_) * 12 <= kScreenW ? 2 : 1;
  textCentered((kScreenH - 8 * size) / 2, note_, size, noteColor_);
}

// Editor: rows × 16 steps of the current bar, cursor, notes (dim when off-grid / out of scale), playhead (SPEC §4, §9.14).
void Ui::drawEditor(bool full) {
  const Category c = trackCategory(edTrack_); const Color col = categoryColor(c); const int rows = edRows();
  const Pattern& src = eng_->sourcePattern(edTrack_); const TrackStatus& ts = eng_->status().track[edTrack_];
  if (full || edDirty_) {
    edDirty_ = false; edPlayCol_ = -1; statusShown_[0] = 0;
    gfx_->fillScreen(kBlack);
    if (loopTrack(edTrack_)) {                                            // loop mode (SPEC §9.19): the loop's name, bars and variant; no grid, no edits
      char hdr[40]; snprintf(hdr, sizeof hdr, "EDIT S%d %s  LOOP MODE", trackSet(edTrack_) + 1, categoryShort(c));
      gfx_->drawText(2, 2, hdr, 1, col);
      textCentered(22, "LOOP", 3, col);
      char name[kMaxNameLen]; copyStr(name, sizeof name, gen_->loopFile(edTrack_));
      if (!name[0]) copyStr(name, sizeof name, "PICKING ...");
      else { const int n = (int)strlen(name); if (n > 4 && strcmp(name + n - 4, ".wav") == 0) name[n - 4] = 0; }
      fitTextWidth(gfx_, name, kScreenW - 4, 1);
      gfx_->drawText(2, 58, name, 1, kWhite);
      const int bars = gen_->loopBars(edTrack_), variant = gen_->info(edTrack_).variant & 3;
      char f[48];
      if (bars) snprintf(f, sizeof f, "%d BAR%s  VARIANT %d %s", bars, bars == 1 ? "" : "S", variant + 1, loopVariantName(variant));
      else snprintf(f, sizeof f, "VARIANT %d %s", variant + 1, loopVariantName(variant));
      gfx_->drawText(2, 72, f, 1, kDimGrey);
      gfx_->drawText(2, kFooterY + 1, "SHIFT+EDIT LOOP OFF  NO EDITS", 1, kDimGrey);
      copyStr(subShown_, sizeof subShown_, name);                         // the view is redrawn when the picked name arrives (below)
      drawStatusLine(true);
      return;
    }
    char hdr[40]; snprintf(hdr, sizeof hdr, "EDIT S%d %s  BAR %d/%d", trackSet(edTrack_) + 1, categoryShort(c), edPage_ + 1, edBars());
    gfx_->drawText(2, 2, hdr, 1, col);
    if (c == Vocal) {
      textCentered(56, "VOCAL: no pattern", 2, col);
      const int p = gen_ ? Generator::clampPause(gen_->info(edTrack_).pause) : kVocalPauseDefault;   // SPEC §9.16
      const int variant = gen_ ? gen_->info(edTrack_).variant : 0;
      char f[48]; char b[8]; formatBeats(b, sizeof b, ts.lenTicks);
      if (variant >= 2) snprintf(f, sizeof f, "LOOP %d BEAT%s  NO PAUSE", variant == 2 ? 2 : 1, variant == 2 ? "S" : "");   // the loop variants have no pause to step
      else snprintf(f, sizeof f, "PAUSE %d BAR%s  CYCLE %s  (len +/-)", p, p == 1 ? "" : "S", b);
      gfx_->drawText(2, kFooterY + 1, f, 1, kDimGrey);
      drawStatusLine(true);
      return;
    }
    for (int r = 0; r < rows; r++) { char lab[6]; edRowLabel(r, lab, sizeof lab); gfx_->drawText(2, kEdY0 + r * kEdCell + 2, lab, 1, r == edCursorY_ ? kWhite : kDimGrey); }
    const int gridH = rows * kEdCell;
    for (int s = 0; s <= 16; s++) gfx_->drawLine(kEdX0 + s * kEdCell, kEdY0, kEdX0 + s * kEdCell, kEdY0 + gridH, (s % 4) == 0 ? kDimGrey : scaleColor(kDimGrey, 50));
    for (int r = 0; r <= rows; r++) gfx_->drawLine(kEdX0, kEdY0 + r * kEdCell, kEdX0 + 16 * kEdCell, kEdY0 + r * kEdCell, scaleColor(kDimGrey, 50));
    const uint16_t pageStart = (uint16_t)(edPage_ * kTicksPerBar), pageEnd = (uint16_t)(pageStart + kTicksPerBar);
    for (int i = 0; i < src.count; i++) {
      const Note& n = src.notes[i];
      if (n.start() < pageStart || n.start() >= pageEnd) continue;
      const int row = edRowOf(n.pitch()); if (row < 0) continue;
      const int step = (n.start() - pageStart) / kSixteenth;
      const bool onGrid = (n.start() % kSixteenth) == 0 && edPitchExact(n.pitch(), row);
      int steps = isSynthCategory(c) ? (n.len() + kSixteenth - 1) / kSixteenth : 1;
      if (steps < 1) steps = 1;
      if (step + steps > 16) steps = 16 - step;
      gfx_->fillRect(kEdX0 + step * kEdCell + 2, kEdY0 + row * kEdCell + 2, steps * kEdCell - 3, kEdCell - 3, onGrid ? col : scaleColor(col, 35));
    }
    gfx_->drawRect(kEdX0 + edCursorX_ * kEdCell, kEdY0 + edCursorY_ * kEdCell, kEdCell, kEdCell, kWhite);
    char f[48]; const int idx = edNoteAt(edTick(), edRowPitch(edCursorY_)); const int vel = idx >= 0 ? src.notes[idx].vel() : 0;
    if (isSampleCategory(c)) snprintf(f, sizeof f, "STEP %d  VEL %d  %d NOTES", edCursorX_ + 1, vel, src.count);
    else snprintf(f, sizeof f, "OCT %d  GATE %d  VEL %d  %s%d", edOctave_, edGate_, vel, noteName(edRowPitch(edCursorY_) % 12), edRowPitch(edCursorY_) / 12 - 1);
    gfx_->fillRect(0, kFooterY, kScreenW, kScreenH - kFooterY, kBlack);
    gfx_->drawText(2, kFooterY + 1, f, 1, kDimGrey);
  }
  if (loopTrack(edTrack_)) {                                              // the pick landed (or the loop changed): show the new name next draw
    const char* f = gen_->loopFile(edTrack_);
    if (f[0] && strncmp(f, subShown_, strlen(subShown_)) != 0) edDirty_ = true;
    drawStatusLine(false); return;
  }
  if (c == Vocal) { drawStatusLine(false); return; }
  int playCol = -1;
  if (ts.state == PSPlaying) { const int s = ts.posTick / kSixteenth - edPage_ * 16; if (s >= 0 && s < 16) playCol = s; }
  if (playCol != edPlayCol_) {
    const int y = kEdY0 + rows * kEdCell + 2;
    if (edPlayCol_ >= 0) gfx_->fillRect(kEdX0 + edPlayCol_ * kEdCell, y, kEdCell, 2, kBlack);
    if (playCol >= 0) gfx_->fillRect(kEdX0 + playCol * kEdCell, y, kEdCell, 2, kWhite);
    edPlayCol_ = playCol;
  }
  drawStatusLine(false);
}

// Perform page (SPEC §9.10, §9.17c): header "S1 LEAD OCT3 Q1/16 CHD LOOP  PLAY", the degree grid (synth tracks) or
// the slot row (Beat / Percussion), beat indicator; a Vocal says NO PERFORM FOR VOCAL.
void Ui::drawPerform(bool full) {
  const Category c = perfCategory(); const Color col = categoryColor(c); const EngineStatus& st = eng_->status();
  const bool drums = isSampleCategory(c), chordable = c == Chord || c == Pad;
  if (full || perfDirty_) {
    perfDirty_ = false;
    gfx_->fillScreen(kBlack);
    memset(perfShadow_, 0xFF, sizeof perfShadow_); perfBeatShown_ = -1; perfHeader_[0] = 0; statusShown_[0] = 0;
    char f[48];
    if (c == Vocal) { copyStr(f, sizeof f, "TAB PLAY  ` EXIT"); textCentered(56, "NO PERFORM FOR VOCAL", 2, col); }
    else if (drums) copyStr(f, sizeof f, "TAB PLAY  DEL LOOP  [ QNT");
    else snprintf(f, sizeof f, "TAB PLAY  DEL LOOP  [ QNT%s  %s MIN", chordable ? "  \\ CHD" : "", gen_ ? gen_->key().rootName() : "A");
    gfx_->drawText(2, kFooterY + 1, f, 1, kDimGrey);
  }
  static const char* names[6] = {"", "ARM", "REC", "CLOSE", "LOOP", "OVR"};
  char hdr[40]; char cat[12]; catUpper(c, cat, sizeof cat);
  snprintf(hdr, sizeof hdr, "S%d %s", trackSet(perfTrack_) + 1, cat);
  if (perfMelodic()) { char o[8]; snprintf(o, sizeof o, " OCT%d", perfOctave_); appendStr(hdr, sizeof hdr, o); }
  if (c != Vocal && perfQuant_) { appendStr(hdr, sizeof hdr, " Q"); appendStr(hdr, sizeof hdr, kQuantName[perfQuant_]); }
  if (chordable && perfChord_) appendStr(hdr, sizeof hdr, " CHD");
  if (c != Vocal && looper_) { appendStr(hdr, sizeof hdr, " "); appendStr(hdr, sizeof hdr, names[looper_ < 6 ? looper_ : 0]); }
  if (st.track[perfTrack_].state == PSPlaying) appendStr(hdr, sizeof hdr, "  PLAY");
  if (strcmp(hdr, perfHeader_) != 0) { copyStr(perfHeader_, sizeof perfHeader_, hdr); gfx_->fillRect(0, 0, kScreenW, 12, kBlack); gfx_->drawText(2, 2, hdr, 1, col); }
  if (c == Vocal) { drawStatusLine(false); return; }
  if (drums) {                                                            // one row of slot cells: Beat 8, Percussion 4
    const int slots = c == Beat ? kPerfDrumSlots : 4;
    for (int s = 0; s < slots; s++) {
      const uint8_t held = perfHeld_[0][s + 1] ? 1 : 0;
      if (held == perfShadow_[0][s]) continue;
      perfShadow_[0][s] = held;
      const int px = kPerfX0 + s * kPerfCellW, py = kPerfY0;
      const char* nm = c == Beat ? kBeatSlotName[s] : kPercSlotName[s];
      if (held) { gfx_->fillRect(px, py, kPerfCellW - 1, kPerfCellH - 1, col); gfx_->drawText(px + 2, py + 6, nm, 1, kBlack); }
      else { gfx_->fillRect(px, py, kPerfCellW - 1, kPerfCellH - 1, kBlack); gfx_->drawRect(px, py, kPerfCellW - 1, kPerfCellH - 1, scaleColor(col, 40)); gfx_->drawText(px + 2, py + 6, nm, 1, col); }
    }
  } else {
    for (int r = 0; r < kPerfRows; r++) for (int ci = 0; ci < kPerfCols; ci++) {
      const uint8_t held = perfHeld_[r][ci + kPerfRowOffset[r]] ? 1 : 0;
      if (held == perfShadow_[r][ci]) continue;
      perfShadow_[r][ci] = held;
      const int px = kPerfX0 + ci * kPerfCellW, py = kPerfY0 + r * kPerfCellH, d = ci + 3 * r;
      char nm[6]; snprintf(nm, sizeof nm, "%d%s", d % 7 + 1, d / 7 == 0 ? "" : (d / 7 == 1 ? "'" : "\""));
      if (held) { gfx_->fillRect(px, py, kPerfCellW - 1, kPerfCellH - 1, col); gfx_->drawText(px + 4, py + 6, nm, 1, kBlack); }
      else { gfx_->fillRect(px, py, kPerfCellW - 1, kPerfCellH - 1, kBlack); gfx_->drawRect(px, py, kPerfCellW - 1, kPerfCellH - 1, scaleColor(col, 40)); gfx_->drawText(px + 4, py + 6, nm, 1, col); }
    }
  }
  const int beat = st.running ? st.beat : -1;
  if (beat != perfBeatShown_) {
    perfBeatShown_ = beat;
    for (int b = 0; b < kBeatsPerBar; b++) {
      const int bx = kPerfX0 + b * 16;
      if (b == beat) gfx_->fillRect(bx, 100, 12, 8, kWhite);
      else { gfx_->fillRect(bx, 100, 12, 8, kBlack); gfx_->drawRect(bx, 100, 12, 8, kDimGrey); }
    }
  }
  drawStatusLine(false);
}

// Synth page (SPEC §9.17c, §9.22): header "S1 LEAD  PluckWide  ACID", nine knobs CUT RES ENV DEC DRV / WAV SAT ACC CHO in
// two rows, drawn as 270° arcs with their real-unit values, each repainted only when its value changed. Knob i is the
// ParamId PCutoff + i (PCutoff … PWave, then PSat PAccent PChorus — contiguous in types.h).
void Ui::drawSynth(bool full) {
  const int t = synthTrack_; const Category c = trackCategory(t); const Color col = categoryColor(c);
  if (full || synthDirty_) {
    synthDirty_ = false;
    gfx_->fillScreen(kBlack);
    synthHeader_[0] = 0; memset(knobShown_, 0, sizeof knobShown_); memset(knobPctShown_, -1, sizeof knobPctShown_); statusShown_[0] = 0;
    gfx_->drawText(2, kFooterY + 1, "VOL-KEY GYR EDT = KNOBS  HOLD RAMPS", 1, kDimGrey);
  }
  char hdr[40]; char cat[12]; catUpper(c, cat, sizeof cat);
  const bool acid = gen_ && gen_->acidEnabled(t);
  snprintf(hdr, sizeof hdr, "S%d %s  %.16s%s", trackSet(t) + 1, cat, gen_ ? gen_->info(t).sound : "", acid ? "  ACID" : "");
  if (strcmp(hdr, synthHeader_) != 0) { copyStr(synthHeader_, sizeof synthHeader_, hdr); gfx_->fillRect(0, 0, kScreenW, 12, kBlack); gfx_->drawText(2, 2, hdr, 1, col); }
  static_assert(PCutoff + kSynthKnobs - 1 == PChorus, "the nine knobs are the contiguous ParamIds PCutoff … PChorus");
  for (int i = 0; i < kSynthKnobs; i++) {
    const ParamId p = (ParamId)(PCutoff + i);
    const float v = paramValue(t, p);
    char val[8]; knobValueText(p, v, val, sizeof val);
    const int pct = p == PWave ? clampi((int)(v + 0.5f) * 100 / (WaveCount - 1), 0, 100) : clampi((int)(v * 100.0f + 0.5f), 0, 100);
    if (pct == knobPctShown_[i] && strcmp(val, knobShown_[i]) == 0) continue;
    knobPctShown_[i] = (int8_t)pct; copyStr(knobShown_[i], sizeof knobShown_[i], val);
    drawKnob(i, pct, val, kPageParamColor[p]);
  }
  drawStatusLine(false);
}

// One knob: a 270° arc (−135° … +135° from the top, clockwise) in 18 segments, lit up to the value, a pointer, the
// label and the value centred below. Knobs 0..4 form the top row, 5..8 the (centred) bottom row.
void Ui::drawKnob(int i, int pct, const char* value, Color col) {
  const int row = i / kKnobCols, colIdx = i % kKnobCols;
  const int cx = (row == 0 ? kKnobX0 : kKnobX0Row2) + colIdx * kKnobPitch, cy = kKnobCy + row * kKnobRowPitch;
  gfx_->fillRect(cx - kKnobPitch / 2, cy + kKnobBoxDy, kKnobPitch, kKnobBoxH, kBlack);
  const float a0 = -135.0f, sweep = 270.0f, va = a0 + sweep * (float)pct / 100.0f;
  const int segs = 18; const float da = sweep / (float)segs;
  const Color dim = scaleColor(col, 30);
  for (int s = 0; s < segs; s++) {
    const float sa = a0 + da * (float)s, ea = sa + da, mid = sa + da * 0.5f;
    const int x0 = cx + (int)lroundf(kKnobR * sinf(sa * kDegToRad)), y0 = cy - (int)lroundf(kKnobR * cosf(sa * kDegToRad));
    const int x1 = cx + (int)lroundf(kKnobR * sinf(ea * kDegToRad)), y1 = cy - (int)lroundf(kKnobR * cosf(ea * kDegToRad));
    gfx_->drawLine(x0, y0, x1, y1, mid <= va ? col : dim);
  }
  const int px = cx + (int)lroundf((kKnobR - 3) * sinf(va * kDegToRad)), py = cy - (int)lroundf((kKnobR - 3) * cosf(va * kDegToRad));
  gfx_->drawLine(cx, cy, px, py, kWhite);
  const char* label = kKnobLabel[i];
  gfx_->drawText(cx - gfx_->textWidth(label, 1) / 2, cy + kKnobLabelDy, label, 1, col);
  gfx_->drawText(cx - gfx_->textWidth(value, 1) / 2, cy + kKnobValueDy, value, 1, kWhite);
}

// ---------------------------------------------------------------------------------------------- PLAY / STOP ALL, USB drive (SPEC §9.22)

// SHIFT + gyro key: anything playing or queued → CmdStopSet for both sets (stops land on the bar line, SPEC §9.20);
// nothing → CmdPlaySet for both (every loaded track starts at the bar). A track whose stop is still pending counts as
// stopped, so a second press before the bar line turns into PLAY ALL and un-queues those stops (CmdCancelPending).
void Ui::playStopAll(uint32_t now) {
  (void)now;
  const EngineStatus& st = eng_->status();
  bool any = false;
  for (int t = 0; t < kTracks; t++) { const PlayState s = st.track[t].state; if (s == PSPlaying || s == PSQueued) any = true; }
  if (any) { for (int s = 0; s < kSets; s++) push(CmdStopSet, 0, s); showStatus("STOP ALL"); return; }
  for (int t = 0; t < kTracks; t++) if (st.track[t].state == PSStopQueued) push(CmdCancelPending, t);
  for (int s = 0; s < kSets; s++) push(CmdPlaySet, 0, s);
  showStatus("PLAY ALL");
}

// SHIFT + gen pattern (SPEC §9.22): the press only arms; a release before kUsbDriveHoldMs is the plain gen pattern of
// the selected track, a hold shows "USB DRIVE..." and calls the drive hook from tick() (see there). Repeats are ignored.
void Ui::usbDriveKey(KeyEvent::Kind kind, uint32_t now) {
  if (kind == KeyEvent::Press) { usbDown_ = true; usbState_ = 1; usbArmMs_ = now; return; }
  if (kind != KeyEvent::Release) return;
  usbDown_ = false;
  if (usbState_ == 1) { usbState_ = 0; requestGen(1, selected_, now); }  // a short press
  else if (usbState_ == 3) usbState_ = 0;                                 // state 2 keeps waiting for the hook
}

// ---------------------------------------------------------------------------------------------- session recorder (SPEC §9.23)

bool Ui::recording() const { return recorder_ && recorder_->recording(); }
bool Ui::rendering() const { return recorder_ && recorder_->rendering(); }
bool Ui::replaying() const { return recorder_ && recorder_->replaying(); }

// Key presses / releases as received (type 0 / 1) at the engine's published tick; the Recorder buffers them.
void Ui::recordKey(const KeyEvent& e) {
  if (!recording() || !eng_) return;
  RecEvent ev; ev.tick = eng_->status().tick; ev.type = e.kind == KeyEvent::Press ? 0 : 1; ev.x = e.x; ev.y = e.y; ev.extra = 0;
  recorder_->onInput(ev);
}

// The gyro command as sent (type 2): x = filter −1..1 as a signed byte (×127), y = reverb 0..255, extra = echo 0..255.
void Ui::recordGyro(float filter, int rev, int echo) {
  if (!recording() || !eng_) return;
  RecEvent ev; ev.tick = eng_->status().tick; ev.type = 2;
  ev.x = (uint8_t)(int8_t)clampi((int)lroundf(clampf(filter, -1.0f, 1.0f) * 127.0f), -127, 127);
  ev.y = (uint8_t)clampi(rev, 0, 255); ev.extra = (uint8_t)clampi(echo, 0, 255);
  recorder_->onInput(ev);
}

// The master output level (type 3, extra = 0..255) whenever it is applied.
void Ui::recordVolume() {
  if (!recording() || !eng_) return;
  RecEvent ev; ev.tick = eng_->status().tick; ev.type = 3; ev.x = ev.y = 0; ev.extra = outVol_;
  recorder_->onInput(ev);
}

// SHIFT + save 1: the recorder page (modal). Without a recorder the key says so; during an offline render the page
// stays closed (the overlay owns the screen and the replay the pages). The same combo on the page closes it.
void Ui::openRecorder(uint32_t now) {
  if (page_ == PgRecorder) { closeModal(now); return; }
  if (!recorder_) { showStatus("NO RECORDER"); return; }
  if (rendering()) return;
  freezeHoldRamps();
  rcCursor_ = rcTop_ = 0; rcDelArmUntil_ = 0;
  recorderRefresh();
  page_ = PgRecorder; pageUntil_ = 0; rcDirty_ = true; fullRedraw_ = true;
}

// Re-lists the sessions and forgets the visible rows' header data (tick() reads it back one row per iteration).
void Ui::recorderRefresh() {
  rcCount_ = (int16_t)(recorder_ ? recorder_->sessionCount() : 0);
  if (rcCount_ < 0) rcCount_ = 0;
  if (rcCursor_ >= rcCount_) rcCursor_ = (int16_t)(rcCount_ > 0 ? rcCount_ - 1 : 0);
  if (rcTop_ > rcCursor_) rcTop_ = rcCursor_;
  if (rcCursor_ >= rcTop_ + kRecRowsVisible) rcTop_ = (int16_t)(rcCursor_ - kRecRowsVisible + 1);
  memset(rcBars_, -1, sizeof rcBars_); rcWav_ = 0; rcInfoN_ = 0; rcDirty_ = true;
}

void Ui::recorderMove(int dir) {
  if (rcCount_ <= 0) return;
  const int16_t top = rcTop_;
  rcCursor_ = (int16_t)clampi(rcCursor_ + dir, 0, rcCount_ - 1);
  if (rcCursor_ < rcTop_) rcTop_ = rcCursor_;
  if (rcCursor_ >= rcTop_ + kRecRowsVisible) rcTop_ = (int16_t)(rcCursor_ - kRecRowsVisible + 1);
  if (rcTop_ != top) { memset(rcBars_, -1, sizeof rcBars_); rcWav_ = 0; rcInfoN_ = 0; }   // the window moved: re-read its headers
  rcDirty_ = true;
}

// ent: start a recording (the page closes, the performance goes on) or, while one runs, stop it.
void Ui::recorderToggle(uint32_t now) {
  if (recorder_->recording()) { recorder_->stopRecording(); showStatus("REC STOPPED"); recorderRefresh(); return; }
  if (recorder_->rendering()) { showStatus("RENDERING"); return; }
  if (!recorder_->startRecording()) { showStatus("REC FAILED"); return; }
  rng_ = Rng(recorder_->sessionSeed());                                   // the load Rng from the session's seed (re-seeded the same way at the render): a random LOAD inside the recording picks the same preset on replay
  showStatus("REC ON", 2000);
  rcStartKey_ = true;                                                     // the ent's release lands on the main page: not an event of the session
  closeModal(now);
}

// m: render the highlighted session to /lp/wav (the sketch takes the engine offline; the overlay shows the progress). The
// page closes at once: the recording started on the main page (the ent that started it closed this page), so the replay
// must land there too — left open, the replayed keys would be the page's (a replayed del deleting a session).
void Ui::recorderRender(uint32_t now) {
  if (rcCount_ <= 0) { showStatus("NO SESSIONS"); return; }
  if (recorder_->recording()) { showStatus("STOP REC FIRST"); return; }
  copyStr(rcRenderName_, sizeof rcRenderName_, recorder_->sessionName(rcCursor_));
  if (!recorder_->startRender(rcCursor_)) { showStatus("RENDER FAILED"); return; }
  rcPctShown_ = -1; rcCancelled_ = false; rcCancelArmed_ = false; rcWasRendering_ = true; rcOverlayDrawn_ = false; rcLiveShift_ = shift_;
  rng_ = Rng(recorder_->sessionSeed());                                   // as at the recording's start (recorderToggle)
  if (modal()) closeModal(now);
}

// del: DELETE? PRESS AGAIN for kDeleteConfirmMs, the second press ON THE SAME ROW deletes the session (and its WAV); a del
// on another row inside the window is a fresh first press (the cursor moves with hold repeats, the question must follow it).
void Ui::recorderDelete(uint32_t now) {
  if (rcCount_ <= 0) return;
  const bool armed = rcDelArmUntil_ && (int32_t)(now - rcDelArmUntil_) < 0 && rcDelArmRow_ == rcCursor_;
  if (!armed) { rcDelArmUntil_ = now + kDeleteConfirmMs; rcDelArmRow_ = rcCursor_; showStatus("DELETE? PRESS AGAIN", kDeleteConfirmMs); return; }
  rcDelArmUntil_ = 0; rcDelArmRow_ = -1;
  if (recorder_->deleteSession(rcCursor_)) { showStatus("DELETED"); recorderRefresh(); } else showStatus("DELETE FAILED");
}

// Recorder page keys: vol± cursor (hold repeats), ent start / stop, m render, del delete (confirm), ` or SHIFT + save 1 exit.
void Ui::handleRecorder(const KeyEvent& e, const KeyMapping& m, uint32_t now) {
  if (e.kind == KeyEvent::Release) return;
  const bool press = e.kind == KeyEvent::Press;
  if ((m.fn == KParamUp || m.fn == KParamDown) && m.a == PVolume) { recorderMove(m.fn == KParamUp ? -1 : 1); return; }
  if (!press) return;
  if ((e.x == 0 && e.y == 0) || (m.fn == KSaveSet && m.a == 0 && shift_)) { closeModal(now); return; }
  if (e.x == 13 && e.y == 2) recorderToggle(now);                         // ent
  else if (e.x == 9 && e.y == 3) recorderRender(now);                     // m
  else if (e.x == 13 && e.y == 0) recorderDelete(now);                    // del
}

// ---------------------------------------------------------------------------------------------- preset manager (SPEC §9.22, SHIFT + save 2 since §9.24)

// SHIFT + save 2: the preset manager (modal); the same combo closes it. The TARGET SET (what ent loads into, "-> SET n" in
// the header) is toggled on the page by any key of track row y0 (set 1) / y1 (set 2) and, like the filter and the AUTO
// flag, kept across opens (Ui members, nothing persisted).
void Ui::openPresets(uint32_t now) {
  if (page_ == PgPresets) { closeModal(now); return; }
  if (!sets_) return;
  freezeHoldRamps();
  psCursor_ = psTop_ = 0; psFirst_ = -1; psOp_ = 0; psRename_ = 0; psDelArmUntil_ = 0; psPlayAfter_ = 0;
  page_ = PgPresets; pageUntil_ = 0; psDirty_ = true; fullRedraw_ = true;
  presetRequestPage();
}

// A track-row key on the page: the set the next ent loads into. The list, cursor and filter stay (the rows are the genre's,
// not a set's); the colour, the header and the cycle dots (SPEC §9.25: the mask is per set) follow the target.
void Ui::presetTarget(int set) {
  set &= 1;
  if ((int)psSet_ == set) return;
  psSet_ = (uint8_t)set; psDirty_ = true;
}

// SPEC §9.25: Sets::presetLoadedInCycle of the target set, when Sets offers it (see cycleLoaded above).
bool Ui::presetRowInCycle(const Sets::PresetRow& row) const { return sets_ && cycleLoaded(sets_, (int)psSet_, row.name, CycleRank3()) == 1; }
bool Ui::presetCycleMarks() { return cycleAvailable((const Sets*)nullptr, CycleRank3()); }

// Asks Sets for the kPresetPage rows from the kPresetScrollPage boundary below the first visible row (two visible windows
// per Sets page: scrolling inside them costs no card work). Sets refuses while busy (a save / load / listing runs):
// psOp_ 1 makes tick() ask again.
void Ui::presetRequestPage() {
  const int first = (psTop_ / kPresetScrollPage) * kPresetScrollPage;
  if (!sets_->beginPresetList(psFilter_, first)) { psOp_ = 1; return; }
  psFirst_ = (int16_t)first; psOp_ = 2; psDirty_ = true;
}

// Sets keeps the rows in its names_ page buffer: they are valid only while presetListReady() (another Sets machine — a
// load from this very page — overwrites them; tick() re-lists then).
bool Ui::presetRowLoaded(int i) const { return sets_ && psOp_ == 0 && psFirst_ >= 0 && sets_->presetListReady() && i >= psFirst_ && i < psFirst_ + sets_->presetRowCount(); }
const Sets::PresetRow* Ui::presetRowAt(int i) const { return presetRowLoaded(i) ? &sets_->presetRow(i - psFirst_) : nullptr; }

// "007 A  My label" / "012 -- Testpack_Amin_012": the file's NNN, its key root (-- unlabelled), lp.label or the file name.
void Ui::presetRowText(const Sets::PresetRow& row, char* out, int cap) const {
  const int n = Sets::parsePresetNumber(row.name);
  char num[8]; if (n >= 0) snprintf(num, sizeof num, "%03u", (unsigned)n % 1000u); else copyStr(num, sizeof num, "---");
  char name[kSetNameMax];
  if (row.label[0]) copyStr(name, sizeof name, row.label);
  else { copyStr(name, sizeof name, row.name); const int l = (int)strlen(name); if (l > 6 && hasExt(name, ".lpset")) name[l - 6] = 0; }
  snprintf(out, (size_t)cap, "%s %-2s %s", num, row.keyRoot >= 0 ? noteName(row.keyRoot) : "--", name);
}

// vol+ / vol−: the cursor through the filtered list; the window follows it and a new Sets page is requested when the
// cursor leaves the loaded one.
void Ui::presetMove(int dir) {
  if (!sets_ || psOp_ != 0 || psFirst_ < 0) return;                      // nothing listed yet (or a page in flight)
  const int total = sets_->presetListTotal();
  if (total <= 0) return;
  psCursor_ = (int16_t)clampi(psCursor_ + dir, 0, total - 1);
  if (psCursor_ < psTop_) psTop_ = psCursor_;
  if (psCursor_ >= psTop_ + kPresetRowsVisible) psTop_ = (int16_t)(psCursor_ - kPresetRowsVisible + 1);
  if (!presetRowLoaded(psCursor_)) presetRequestPage();
  psDirty_ = true;
}

// key±: ALL ↔ the current key (the generator's root at toggle time); the list restarts from the top.
void Ui::presetToggleFilter() {
  psFilter_ = (int8_t)(psFilter_ < 0 ? (gen_ ? gen_->key().root % 12 : 9) : -1);
  psCursor_ = psTop_ = 0; psFirst_ = -1;
  presetRequestPage();
}

// ent: Sets::beginLoadNamed of the highlighted preset into the page's set; with AUTO on, tick() starts the set at the
// bar (CmdPlaySet) once the load completed (psPlayAfter_ rides on the setsOp_ completion).
void Ui::presetLoad(uint32_t now) {
  const Sets::PresetRow* row = presetRowAt(psCursor_);
  if (!row) return;
  if (sets_->busy()) { showStatus("SETS BUSY"); return; }
  if (gen_ && gen_->busy()) { showStatus("GEN BUSY"); return; }
  rng_ = Rng(rng_.next() ^ (recording() || rendering() ? 0u : now));   // as loadSet: no clock inside a session (SPEC §9.23)
  if (!sets_->beginLoadNamed(psSet_, row->name, rng_)) { showStatus(sets_->lastError()[0] ? sets_->lastError() : "LOAD FAILED"); return; }
  setsOp_ = 2; setsSet_ = psSet_; psPlayAfter_ = psAuto_;
  snprintf(progress_, sizeof progress_, "LOAD S%d 0/%d", psSet_ + 1, sets_->total()); progressOn_ = true; statusUntil_ = 0;
}

// del: DELETE? PRESS AGAIN for kDeleteConfirmMs, the second press ON THE SAME ROW (of the same set and filter) unlinks the
// preset and re-lists; a del elsewhere inside the window is a fresh first press. Replayed (an offline render), the unlink
// is skipped: every render of the session would otherwise remove another preset from the card.
void Ui::presetDelete(uint32_t now) {
  const Sets::PresetRow* row = presetRowAt(psCursor_);
  if (!row) return;
  const bool armed = psDelArmUntil_ && (int32_t)(now - psDelArmUntil_) < 0 && psDelArmRow_ == psCursor_ && psDelArmSet_ == (int8_t)psSet_ && psDelArmFilter_ == psFilter_;
  if (!armed) {
    psDelArmUntil_ = now + kDeleteConfirmMs; psDelArmRow_ = psCursor_; psDelArmSet_ = (int8_t)psSet_; psDelArmFilter_ = psFilter_;
    showStatus("DELETE? PRESS AGAIN", kDeleteConfirmMs); return;
  }
  psDelArmUntil_ = 0; psDelArmRow_ = -1;
  if (replaying()) return;
  if (sets_->busy()) { showStatus("SETS BUSY"); return; }
  if (!sets_->deletePreset(row->name)) { showStatus(sets_->lastError()[0] ? sets_->lastError() : "DELETE FAILED"); return; }
  psOp_ = 4; statusUntil_ = 0;                                            // tick() shows DELETING... and reports DELETED once the machine is through (file, then the index line)
}

// n: RENAME — the label is edited in place (the current one to start from), see presetRenameKey.
void Ui::presetRenameStart() {
  const Sets::PresetRow* row = presetRowAt(psCursor_);
  if (!row) return;
  copyStr(psText_, sizeof psText_, row->label);
  psRename_ = 1; psDirty_ = true;
}

// Text entry: letters (SHIFT = upper case) and digits from the key legends, '-' (11,0), del = backspace, ent = confirm
// (Sets::beginSetLabel, psOp_ 3 until written), ` = cancel. Everything else is ignored while typing.
void Ui::presetRenameKey(const KeyEvent& e, uint32_t now) {
  (void)now;
  if (e.x == 0 && e.y == 0) { psRename_ = 0; psDirty_ = true; return; }
  const int n = (int)strlen(psText_);
  if (e.x == 13 && e.y == 0) { if (n > 0) psText_[n - 1] = 0; psDirty_ = true; return; }   // del = backspace
  if (e.x == 13 && e.y == 2) {                                            // ent = confirm
    const Sets::PresetRow* row = presetRowAt(psCursor_);
    psRename_ = 0; psDirty_ = true;
    if (!row || replaying()) return;                                      // replayed: the typing is followed (its keys swallow nothing else), the card is not rewritten
    if (sets_->busy()) { showStatus("SETS BUSY"); return; }
    if (!sets_->beginSetLabel(row->name, psText_)) { showStatus(sets_->lastError()[0] ? sets_->lastError() : "LABEL FAILED"); return; }
    psOp_ = 3; statusUntil_ = 0;                                          // tick() shows WRITING LABEL... meanwhile
    return;
  }
  const char c = legendChar(e.x, e.y, shift_);
  if (!c || n >= kPresetLabelMax) return;
  psText_[n] = c; psText_[n + 1] = 0; psDirty_ = true;
}

// Preset manager keys: vol± cursor (hold repeats), key± filter, ent load into the target set, any track-row key = target
// set 1 (row y0, x 1–7: (0,0) is the ` exit) / set 2 (row y1), m AUTO, del delete (confirm), n rename, ` or SHIFT + save 2
// exit. SHIFT + load 1 / 2 (the theory generate of the main page) is not live here: the page is modal.
void Ui::handlePresets(const KeyEvent& e, const KeyMapping& m, uint32_t now) {
  if (e.kind == KeyEvent::Release) return;
  const bool press = e.kind == KeyEvent::Press;
  if (psRename_) { if (press) presetRenameKey(e, now); return; }
  if ((m.fn == KParamUp || m.fn == KParamDown) && m.a == PVolume) { presetMove(m.fn == KParamUp ? -1 : 1); return; }
  if (!press) return;
  if (m.fn == KSaveSet && m.a == 1 && shift_) { closeModal(now); return; }   // the page's own combo
  if (e.x == 0 && e.y == 0) { closeModal(now); return; }                 // ` exits (it is track key (0,0) on the main page: checked before the target toggle)
  if (m.fn == KTrack) { presetTarget(trackSet(m.a)); return; }           // row y0 (1–7,0) → SET 1, row y1 (0–7,1) → SET 2 (SHIFT or not)
  if ((m.fn == KParamUp || m.fn == KParamDown) && m.a == PKey) { presetToggleFilter(); return; }
  if (e.x == 13 && e.y == 2) presetLoad(now);                             // ent
  else if (e.x == 9 && e.y == 3) { psAuto_ ^= 1; psDirty_ = true; }        // m
  else if (e.x == 13 && e.y == 0) presetDelete(now);                      // del
  else if (e.x == 8 && e.y == 3) presetRenameStart();                     // n
}

// Preset manager page: header "-> SET 1  ALL  23  AUTO" (the target set, the filter, the count once the listing finished —
// "..." before — and the AUTO flag), eight rows of "NNN K label" at kPresetTextX, a kPresetDotSize dot in the margin before a
// row loaded in the target set's current random-load cycle (SPEC §9.25), the cursor row inverted in the target set's colour,
// the row being renamed with a trailing '_', the help line in the footer, the status line (load progress, DELETE? …) at kStatusY.
void Ui::drawPresets(bool full) {
  const Color col = psSet_ == 0 ? kNeonGreen : rgb(0x00, 0xE5, 0xFF);
  if (full || psDirty_) {
    psDirty_ = false; statusShown_[0] = 0;
    gfx_->fillScreen(kBlack);
    const bool listed = psOp_ == 0 && psFirst_ >= 0;
    const int total = listed ? sets_->presetListTotal() : -1;
    char filt[8]; if (psFilter_ < 0) copyStr(filt, sizeof filt, "ALL"); else snprintf(filt, sizeof filt, "%s MIN", noteName(psFilter_));
    char hdr[40];
    if (total >= 0) snprintf(hdr, sizeof hdr, "%s SET %d  %s  %d%s", kPresetTargetPrefix, psSet_ + 1, filt, total, psAuto_ ? "  AUTO" : "");
    else snprintf(hdr, sizeof hdr, "%s SET %d  %s  ...%s", kPresetTargetPrefix, psSet_ + 1, filt, psAuto_ ? "  AUTO" : "");
    gfx_->drawText(2, 2, hdr, 1, col);
    if (total == 0) textCentered(50, "NO PRESETS", 2, col);
    for (int r = 0; r < kPresetRowsVisible; r++) {
      const int i = psTop_ + r, y = kPresetRowY0 + r * kPresetRowH;
      if (total >= 0 && i >= total) break;
      const Sets::PresetRow* row = presetRowAt(i);
      char line[kSetNameMax + 16];
      if (!row) copyStr(line, sizeof line, "...");
      else if (psRename_ && i == psCursor_) {
        const int n = Sets::parsePresetNumber(row->name);
        snprintf(line, sizeof line, "%03u %-2s %s_", n < 0 ? 0u : (unsigned)n % 1000u, row->keyRoot >= 0 ? noteName(row->keyRoot) : "--", psText_);
      } else presetRowText(*row, line, sizeof line);
      fitTextWidth(gfx_, line, kScreenW - kPresetTextX - 4, 1);
      const bool cur = i == psCursor_;
      if (cur) gfx_->fillRect(0, y - 1, kScreenW, kPresetRowH - 1, col);
      gfx_->drawText(kPresetTextX, y + 1, line, 1, cur ? kBlack : kWhite);
      if (row && presetRowInCycle(*row)) gfx_->fillRect(kPresetDotX, y + kPresetDotDy, kPresetDotSize, kPresetDotSize, cur ? kBlack : col);   // loaded in this cycle (SPEC §9.25)
    }
    gfx_->drawText(2, kFooterY + 1, psRename_ ? "TYPE LABEL  ENT OK  ` CANCEL" : "ENT LOAD  M AUTO  N NAME  DEL  KEY FILT", 1, kDimGrey);
  }
  drawStatusLine(false);
}

// Recorder page: header "RECORDER  3 SESSIONS" (+ "  REC" in red while recording), rows "rec_007  4 BARS  W", the
// cursor row inverted, help in the footer, the status line at kStatusY.
void Ui::drawRecorder(bool full) {
  const Color col = kRed;
  if (full || rcDirty_) {
    rcDirty_ = false; statusShown_[0] = 0;
    gfx_->fillScreen(kBlack);
    char hdr[40]; snprintf(hdr, sizeof hdr, "RECORDER  %d SESSION%s", (int)rcCount_, rcCount_ == 1 ? "" : "S");
    gfx_->drawText(2, 2, hdr, 1, kWhite);
    if (recording()) gfx_->drawText(kScreenW - 2 - gfx_->textWidth("REC", 1), 2, "REC", 1, col);
    if (rcCount_ == 0) textCentered(50, "NO SESSIONS", 2, kDimGrey);
    for (int r = 0; r < kRecRowsVisible; r++) {
      const int i = rcTop_ + r, y = kPresetRowY0 + r * kPresetRowH;
      if (i >= rcCount_) break;
      char bars[12];
      if (rcBars_[r] < 0) copyStr(bars, sizeof bars, "--");
      else snprintf(bars, sizeof bars, "%d BAR%s", (int)rcBars_[r], rcBars_[r] == 1 ? "" : "S");
      char line[40]; snprintf(line, sizeof line, "%-8.8s %8s  %s", recorder_->sessionName(i), bars, (rcWav_ >> r) & 1 ? "W" : "");
      if (i == rcCursor_) { gfx_->fillRect(0, y - 1, kScreenW, kPresetRowH - 1, col); gfx_->drawText(4, y + 1, line, 1, kBlack); }
      else gfx_->drawText(4, y + 1, line, 1, kWhite);
    }
    gfx_->drawText(2, kFooterY + 1, recording() ? "ENT STOP  M WAV  DEL  ` EXIT" : "ENT REC  M WAV  DEL  ` EXIT", 1, kDimGrey);
  }
  drawStatusLine(false);
}

// RENDERING overlay: the session name, the percentage huge (repainted only when it changes), the cancel hint.
void Ui::drawRendering(bool full) {
  if (full) {
    gfx_->fillScreen(kBlack);
    char hdr[32]; snprintf(hdr, sizeof hdr, "RENDERING %.7s", rcRenderName_);
    textCentered(8, hdr, 2, kRed);
    gfx_->drawText(2, kFooterY + 1, "HOLD ` TO CANCEL", 1, kDimGrey);
    rcPctShown_ = -1; statusShown_[0] = 0;
  }
  const int pct = clampi(recorder_->renderPercent(), 0, 100);
  if (pct != rcPctShown_) {
    rcPctShown_ = (int8_t)pct;
    char p[8]; snprintf(p, sizeof p, "%d%%", pct);
    gfx_->fillRect(0, 44, kScreenW, 36, kBlack);
    textCentered(46, p, 4, kWhite);
    gfx_->fillRect(21, 93, 198, 8, kBlack); gfx_->drawRect(20, 92, 200, 10, kRed);
    if (pct > 0) gfx_->fillRect(21, 93, 198 * pct / 100, 8, kRed);
  }
}

void Ui::drawRenderDone() {
  gfx_->fillScreen(kBlack);
  textCentered(30, "DONE", 3, kNeonGreen);
  char path[40]; snprintf(path, sizeof path, "%s/%.7s.wav", Recorder::kWavDir, rcRenderName_);
  textCentered(72, path, 1, kWhite);
}

} // namespace lp
