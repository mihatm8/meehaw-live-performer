// meehaw live performer — saved sets (.lpset) writer and loader (SPEC §6, §9.2, §9.5, §9.8, §9.14, §9.29).
// The file is Java-properties text (the reader keeps the line rules the format was born with: CRLF, backslash
// continuations, '#'/'!' comments, the first ':' or '=' splits, escapes). Since §9.29 a preset is written in the
// SESSION format of §9.18: lp.* keys only, every source as an absolute path in lp.src.i (library MIDI, vocal and loop
// files are never copied), edited / recorded / theory patterns as presets/<name>_t<i>.mid next to the preset. Files
// saved before that (clip=assets/<basename>, clipType, track.i.slot) still load through the same reader.
// Runs on the loop task only: resumable state machines, one bounded SD action per step(), no heap, every
// buffer lives inside the Sets object (≈ 10 KB) and stack frames stay well under 2 KB.
//
// Load = like generate (SPEC §9.5, §9.8): every track's variant is built into the engine's next buffer without
// a swap; the eight swaps, the Beat mask and CmdSwapSet go out in ONE burst through Generator::requestSetSwap
// (in the same breath as the vocal's switch commands when a vocal is streaming), so the whole set changes on
// the same bar line. A stopped transport ends the load right away: the swaps apply on resume, exactly like the
// generator's own set job. SPEC §9.17b: a load does NOT reset the tracks' params — the synth knobs (lp.track.N.cutoff
// .. .wave) are restored with CmdSetParam right after the track's CmdSetPatch, and the strip (.vol .dist .filter
// .echo .reverb) once the set has swapped (LdWaitSwap), so the old pattern never plays with the new mix (§9.8 "the
// effects change at the swap"); a file from before §9.17b gets CmdResetTrack per track at the swap instead.
//
// Session (SPEC §9.18): /lp/state/session_set<N>.lpset and session_set<N>_b.lpset — two slots per set, written
// alternately with lp.seq counting up and lp.end=1 as the very last line — hold what a preset holds plus lp.src.N
// (absolute source paths; "acid" for a generated line, a /lp/state/session_s<N>_t<M>.mid for an edited or recorded
// pattern, also when a library source is gone from the card), lp.kit.N, lp.volume, acid flags, params and knobs.
// markSessionDirty() (or a Generator::changeCount() bump seen by step()) arms a 5 s debounce on the engine's sample
// clock (and at least 15 s pass between two completed writes, SPEC §9.19 §3 c); the write then runs through this same
// state machine (SvTrack / SvResolve / SvKitPaths shared with the preset writer, one bounded SD action per step),
// yielding to any user save, load or generate (a file whose next chunk completes it is finished first). Its source
// paths come from what the generator remembered at pick time (Generator::sourcePath / kitPath / loopPath / vocalPath):
// nothing is opened or probed to verify them (SPEC §9.19 §3 b); only a track whose folder is unknown is probed as before.
// Every step() of a save, load, write or restore waits for Generator::streamsHealthy() (SPEC §9.19 §3 a): the card is
// touched only while every consumed stream ring holds enough lead. Loop mode (SPEC §9.19 §1.7): a track in loop mode is
// saved as its loop — lp.src.i = the loop's absolute path, lp.track.i.loopmode=1 — and a load / restore with loopmode=1
// restores loop mode with that exact file through Generator::loadLoop, or picks a loop of the category when the file is
// gone. restoreSession() queues the boot restore: per set the slot with the highest
// lp.seq among those ending in lp.end is parsed and reconstructed like a load (sounds, params, patterns from the
// absolute paths or the acid seed, kits, vocal), nothing starts playing, and a missing, malformed or truncated file
// (a power-off mid-write) is skipped silently. The writer's intent is what is written (the generator's variant and
// len = natural × 2^k, not the engine's active pattern, which only follows at a bar line).
#include "sets.h"
#include "smf.h"
#include "key.h"
#include "variants.h"
#include "../dsp/presets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

namespace lp {
namespace {

constexpr int kProbeNone = -1;
constexpr uint16_t kFlagGenerating = 1;                                 // CmdSetGenerating 1 has been sent
constexpr uint16_t kFlagLastPage = 2;                                   // the directory page just walked was the last one
constexpr uint16_t kFlagSlotSkip = 4;                                   // the current kit slot is skipped (no file): no ack to wait for
constexpr uint16_t kFlagRushDue = 8;                                    // the Beat's rush loop could not be rebuilt (rush held): retried
constexpr uint16_t kFlagPushed = 16;                                    // the swap burst is out (pushClock_ valid)
constexpr uint16_t kFlagIndex = 32;                                     // the listing streams _index.txt
constexpr uint16_t kFlagChunk = 64;                                     // chunk_ holds index chunk page_ (a probe clobbers it)
constexpr uint16_t kFlagPartial = 128;                                  // a source was not found: file written, ok() false
constexpr uint16_t kFlagEof = 256;                                      // the index chunk in hand is the last one
constexpr uint16_t kFlagWarm = 512;                                     // the library is warm for the kit slot's fallback pick (prepared in earlier steps)
constexpr uint16_t kFlagComplete = 1024;                                // session restore: the slot being probed ended with lp.end
constexpr uint16_t kFlagNewCycle = 2048;                                // loader: the second pass over the listing that starts a new load cycle (SPEC §9.25): every match's bit is cleared as it is offered
constexpr int kHandleWaitSteps = 1024;                                  // SPEC §9.19g: steps a load waits for a free card handle before its stream open (a pending switch frees one at the next bar line; ≈ 12 s is longer than any bar)
constexpr int kPublishRetries = 8;                                      // steps a refused next-buffer write is retried
constexpr int kRenameRetries = 3;                                       // steps a failed .tmp → original rename of the preset manager's rewrites is retried before the op fails
constexpr int kMaxLenStep = 5;                                          // |k| of len = natural × 2^k (Generator::setLengthStep)
// Preset bank layout (lp.banks). SPEC §9.22 (research §4) inserted 128 generated library rows between the hand library
// rows and the hand '303' rows of every bank (presets_gen.hpp: [hand lib][gen lib][hand 303][gen 303]), so an
// lp.track.N.preset index written by the firmware BEFORE that wave — no lp.banks line — that pointed into the hand '303'
// family (Chord 10..17, Lead / Pad / Bass 9..16) now names a generated library row. Such files are remapped on load onto
// the moved rows (the same sound: new index = libraryPresetCount + (old − old hand library count)); old library indices
// are unchanged and files written since (lp.banks=2) are taken verbatim. The old counts are FROZEN HISTORY: they
// describe files on the user's card and must never follow presets_gen.hpp.
constexpr int kBanksLayout = 2;
inline int oldHandLibrary(Category c) { return c == Chord ? 10 : (c == Lead || c == Pad || c == Bass || c == Fx) ? 9 : 0; }
inline int oldHandAcid(Category c) { return (c == Chord || c == Lead || c == Pad || c == Bass) ? 8 : 0; }
inline uint16_t presetForLayout(Category c, uint16_t p, int banks) {
  if (banks >= kBanksLayout) return p;
  const int lib = oldHandLibrary(c), acid = oldHandAcid(c);
  if (acid <= 0 || p < lib || p >= lib + acid) return p;
  return (uint16_t)(libraryPresetCount(c) + (p - lib));
}
constexpr uint32_t kSlotCapMs[kSampleSlots] = {kCapKickMs, kCapSnareMs, kCapHhMs, kCapOhMs, kCapPercMs, kCapPercMs, kCapPercMs, kCapPercMs};
const char* const kSlotRoles[kSampleSlots][3] = {                       // SPEC §6 kit roles, first = preferred folder
  {"kick", nullptr, nullptr}, {"snare", "clap", nullptr}, {"hh", nullptr, nullptr}, {"oh", "hh", nullptr},
  {"perc", "clap", "cymbal"}, {"perc", "clap", "cymbal"}, {"perc", "clap", "cymbal"}, {"perc", "clap", "cymbal"}};
const char kIndexName[] = "_index.txt";

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }   // Lua %s
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { char l = lower(c); return l >= 'a' && l <= 'z'; }
inline float clampf(float v, float lo, float hi) { return v != v ? lo : v < lo ? lo : v > hi ? hi : v; }   // NaN → lo

const char* baseName(const char* path) {
  const char* b = path;
  for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}
void copyStr(char* dst, int cap, const char* src) { if (cap <= 0) return; strncpy(dst, src ? src : "", (size_t)cap - 1); dst[cap - 1] = 0; }
uint32_t slotCapSamples(int slot) { return kSlotCapMs[slot & 7] * (uint32_t)kSampleRate / 1000u; }

// k of len = natural × 2^k clamped like Generator::setLengthStep (SPEC §9.4: 0.5 .. 16 beats, |k| ≤ 5; a streaming
// loop track 1 beat .. kMaxLoopTicks, SPEC §9.19 §1.4).
int clampLenStep(uint32_t natural, int k, bool loop = false) {
  if (!natural) natural = kTicksPerBar;
  const uint32_t lo = loop ? (uint32_t)kTicksPerBeat : (uint32_t)(kTicksPerBeat / 2), hi = loop ? (uint32_t)kMaxLoopTicks : (uint32_t)kMaxPatternTicks;
  if (k > kMaxLenStep) k = kMaxLenStep;
  if (k < -kMaxLenStep) k = -kMaxLenStep;
  while (k > 0 && (natural << k) > hi) k--;
  while (k < 0 && (natural >> (-k)) < lo) k++;
  return k;
}
// The clip length natural × 2^k in ticks (k already clamped), 0.5 .. 16 beats (a loop: 1 beat .. kMaxLoopBars bars).
uint16_t lenOfStep(uint32_t natural, int k, bool loop = false) {
  if (!natural) natural = kTicksPerBar;
  const uint32_t lo = loop ? (uint32_t)kTicksPerBeat : (uint32_t)(kTicksPerBeat / 2), hi = loop ? (uint32_t)kMaxLoopTicks : (uint32_t)kMaxPatternTicks;
  uint32_t len = k >= 0 ? natural << k : natural >> (-k);
  if (len < lo) len = lo;
  if (len > hi) len = hi;
  return (uint16_t)len;
}

// Category of the running set-file key "track.<i>.<rest>" / "lp.track.<i>.<rest>": returns i or -1 and points rest.
int trackKey(const char* key, const char* prefix, const char** rest) {
  size_t n = strlen(prefix);
  if (strncmp(key, prefix, n) != 0 || !isDigit(key[n])) return -1;
  int i = key[n] - '0';
  if (key[n + 1] != '.' || i >= kTracksPerSet) return -1;
  *rest = key + n + 2;
  return i;
}

// set_presets.lua:56-71 safe_asset: relative, no control characters, no drive letter, no ".." component;
// backslashes become '/'. Modifies the clip in place; false = the clip must be ignored.
bool sanitizeClip(char* clip) {
  if (!clip || !clip[0]) return false;
  for (char* p = clip; *p; p++) { if ((uint8_t)*p < 32 || *p == 127) return false; if (*p == '\\') *p = '/'; }
  if (clip[0] == '/' || (isAlpha(clip[0]) && clip[1] == ':')) return false;
  for (const char* p = clip; *p;) {
    const char* e = p; while (*e && *e != '/') e++;
    if (e - p == 2 && p[0] == '.' && p[1] == '.') return false;
    p = *e ? e + 1 : e;
  }
  return true;
}

// Trim per set_presets.lua:40 (^%s*(.-)%s*$), in place; returns the start.
char* trim(char* s) {
  while (isSpace(*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isSpace(s[n - 1])) s[--n] = 0;
  return s;
}

void formatBeats(char* out, int cap, float beats) {          // 4 → "4", 0.5 → "0.5" (len = natural × 2^k, SPEC §9.4)
  int whole = (int)beats; int cents = (int)((beats - (float)whole) * 100.0f + 0.5f);
  if (cents == 0) snprintf(out, (size_t)cap, "%d", whole);
  else if (cents == 50) snprintf(out, (size_t)cap, "%d.5", whole);
  else snprintf(out, (size_t)cap, "%d.%02d", whole, cents);
}

// Key folder candidates for a library file name (SPEC §6: /lp/midi/<Category>/<Key>/<Key>__...): the label
// before the first "__" parsed as a key ("Cmaj" and "C" both live in /C, "Amin" in /Amin, keyless in /NA)
// and, when it spells differently, the raw label. Returns the number of candidates (0..2).
int keyFolders(const char* file, char folder[2][16]) {
  const char* sep = strstr(file, "__");
  if (!sep || sep == file) return 0;
  int n = (int)(sep - file); if (n > 15) n = 15;
  memcpy(folder[1], file, (size_t)n); folder[1][n] = 0;
  LibraryKey k = parseLibraryLabel(folder[1]);
  if (!k.valid) { strcpy(folder[0], folder[1]); return 1; }
  if (k.keyless) strcpy(folder[0], "NA");
  else snprintf(folder[0], 16, "%s%s", noteName(k.root), k.minor ? "min" : "");
  return strcmp(folder[0], folder[1]) == 0 ? 1 : 2;
}

// A pattern source that belongs to one file and may go with it — a preset's <name>_t<i>.mid (deletePreset removes them)
// or a session SMF under /lp/state (rewritten by the next session write) — as opposed to the library (/lp/midi) and a
// legacy presets/assets/ copy, which the firmware never deletes (SPEC §9.29): the writers give such a pattern its own SMF.
bool ownedSource(const char* path) { return strncmp(path, "/lp/midi/", 9) != 0 && !strstr(path, "/presets/assets/"); }

struct PageScan { Sets* self; int skip; int seen; int taken; int stored; bool collect; };
struct KidScan { Sets* self; int seen; int n; };

} // namespace

// ---------------------------------------------------------------------------------------------- SetFile / parser

void SetFile::clear() {
  schemaVersion = 0; setIndex = -1; keyRoot = -1; bpm = 0; keyName[0] = 0; genre[0] = 0; label[0] = 0;
  for (int i = 0; i < kTracksPerSet; i++) { track[i] = SetFileTrack(); src[i][0] = 0; }
  for (int s = 0; s < kSampleSlots; s++) kit[s][0] = 0;
  volume = -1; seq = 0; complete = 0; banks = 0;
}

// set_presets.lua:9-18 unescape: "\t" "\r" "\n" decode, "\:" "\=" "\#" "\!" "\ " "\\" drop the backslash,
// any other pair stays as written (so a Windows path component like \takes is not turned into a tab twice).
void unescapeProperty(char* s) {
  char* w = s;
  for (const char* r = s; *r; r++) {
    if (*r != '\\' || !r[1]) { *w++ = *r; continue; }
    char c = *++r;
    if (c == 't') *w++ = '\t';
    else if (c == 'r') *w++ = '\r';
    else if (c == 'n') *w++ = '\n';
    else if (c == ':' || c == '=' || c == '#' || c == '!' || c == ' ' || c == '\\') *w++ = c;
    else { *w++ = '\\'; *w++ = c; }
  }
  *w = 0;
}

// set_presets.lua:40-47 on one assembled logical line: trim, skip blank and '#'/'!' lines, split at the
// first ':' or '=' with surrounding whitespace dropped (:42), else at the first whitespace run (:44).
bool parsePropertyLine(char* line, const char** key, const char** value) {
  char* s = trim(line);
  if (!*s || *s == '#' || *s == '!') return false;
  char* sep = s; while (*sep && *sep != ':' && *sep != '=') sep++;
  char* val;
  if (*sep) {
    val = sep + 1;
    *sep = 0;
    char* e = sep; while (e > s && isSpace(e[-1])) *--e = 0;       // key trailing whitespace (%s* before the separator)
  } else {
    sep = s; while (*sep && !isSpace(*sep)) sep++;
    if (!*sep) return false;                                        // no separator and no whitespace: not a property
    *sep = 0; val = sep + 1;
  }
  while (isSpace(*val)) val++;
  unescapeProperty(s); unescapeProperty(val);
  *key = s; *value = val;
  return true;
}

// Raw bytes → logical lines. set_presets.lua:27-39: one trailing "\r" stripped per raw line, an odd number
// of trailing backslashes joins the next raw line (the last backslash dropped). A line longer than
// kSetLineMax - 1 is dropped whole (the rest of it is still consumed).
void PropertiesReader::feed(const char* data, int n) {
  for (int i = 0; i < n; i++) {
    char c = data[i];
    if (c == '\n') { endRawLine(); continue; }
    if (len_ < kSetLineMax - 1) line_[len_++] = c; else overflow_ = true;
  }
}
void PropertiesReader::endRawLine() {
  if (len_ > 0 && line_[len_ - 1] == '\r') len_--;                 // :28
  int slashes = 0;
  for (int i = len_ - 1; i >= 0 && line_[i] == '\\'; i--) slashes++; // :30-35
  if (slashes & 1) { len_--; return; }                              // :36-37 pending continuation
  line_[len_] = 0;
  if (!overflow_) {
    const char* k; const char* v;
    if (parsePropertyLine(line_, &k, &v) && fn_) fn_(k, v, user_);
  }
  len_ = 0; overflow_ = false;
}
void PropertiesReader::finish() {
  if (len_ > 0) endRawLine();                                       // lines() yields a last line without "\n" too
  len_ = 0; overflow_ = false;
}

// Known keys (SPEC §9.8). Everything else, including track.i.category and track.i.instrument, is ignored.
void applySetProperty(SetFile& f, const char* key, const char* value) {
  const char* rest = nullptr;
  if (strcmp(key, "schemaVersion") == 0) f.schemaVersion = (int)strtol(value, nullptr, 10);
  else if (strcmp(key, "setIndex") == 0) f.setIndex = (int)strtol(value, nullptr, 10);
  else if (strcmp(key, "keyRoot") == 0) { int r = (int)strtol(value, nullptr, 10); f.keyRoot = ((r % 12) + 12) % 12; }   // :105 key_root % 12
  else if (strcmp(key, "keyName") == 0) copyStr(f.keyName, sizeof f.keyName, value);
  else if (strcmp(key, "genre") == 0) copyStr(f.genre, sizeof f.genre, value);
  else if (strcmp(key, "lp.bpm") == 0) f.bpm = (int)strtol(value, nullptr, 10);
  else if (strcmp(key, "lp.label") == 0) Sets::copyLabelText(f.label, value);                               // SPEC §9.22 preset manager tag
  else if (strcmp(key, "lp.banks") == 0) { long b = strtol(value, nullptr, 10); f.banks = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b); }   // preset bank layout of the indices (see presetForLayout)
  else if (strcmp(key, "lp.volume") == 0) { long v = strtol(value, nullptr, 10); f.volume = v < 0 ? 0 : v > 255 ? 255 : (int)v; }   // session (SPEC §9.18)
  else if (strcmp(key, "lp.seq") == 0) f.seq = (uint32_t)strtoul(value, nullptr, 10);                    // session: write counter
  else if (strcmp(key, "lp.end") == 0) f.complete = 1;                                                    // session: the last line written
  else if (strncmp(key, "lp.src.", 7) == 0 && isDigit(key[7]) && key[8] == 0) {                     // absolute source path or "acid" (SPEC §9.18, §9.29); wins over a legacy clip line
    char* dst = f.src[key[7] - '0'];
    if (key[7] - '0' < kTracksPerSet) { copyStr(dst, kMaxPathLen, value); for (char* q = dst; *q; q++) if (*q == '\\') *q = '/'; }
  }
  else if (strncmp(key, "lp.kit.", 7) == 0 && isDigit(key[7]) && key[8] == 0) {                     // session: one absolute slot path
    const int slot = key[7] - '0';
    if (slot < kSampleSlots) { copyStr(f.kit[slot], kMaxPathLen, value); for (char* q = f.kit[slot]; *q; q++) if (*q == '\\') *q = '/'; }
  }
  else {
    int i = trackKey(key, "track.", &rest);
    if (i >= 0) {                                                   // the keys of a preset saved before SPEC §9.29 (the REAPER export days); clipType / category are implied
      SetFileTrack& t = f.track[i];
      if (strcmp(rest, "clip") == 0) { if (!f.src[i][0] && value[0] != '/' && strlen(value) < kMaxPathLen) copyStr(f.src[i], kMaxPathLen, value); }   // relative to the preset folder (sourceAbs sanitises it); an lp.src.i line read before it stays
      else if (strcmp(rest, "slot") == 0) t.variant = (uint8_t)(strtol(value, nullptr, 10) & 3);
      else if (strcmp(rest, "beatLayer") == 0) { if (i == Beat) t.variant = (uint8_t)(strtol(value, nullptr, 10) & 3); }
      else if (strcmp(rest, "beatKickMuted") == 0) t.beatKickMuted = strcmp(value, "true") == 0;
      return;
    }
    i = trackKey(key, "lp.track.", &rest);
    if (i < 0) return;
    SetFileTrack& t = f.track[i];
    if (strcmp(rest, "variant") == 0) t.variant = (uint8_t)(strtol(value, nullptr, 10) & 3);          // SPEC §9.29 (was track.i.slot / beatLayer)
    else if (strcmp(rest, "kickcut") == 0) t.beatKickMuted = strtol(value, nullptr, 10) != 0;         // SPEC §9.29 (was track.7.beatKickMuted)
    else if (strcmp(rest, "preset") == 0) { long p = strtol(value, nullptr, 10); t.preset = (uint16_t)(p < 0 ? 0 : p > 65535 ? 65535 : p); t.hasPreset = 1; }
    else if (strcmp(rest, "seed") == 0) { t.seed = (uint32_t)strtoul(value, nullptr, 10); t.hasSeed = 1; }
    else if (strcmp(rest, "len") == 0) { t.lenBeats = strtof(value, nullptr); t.hasLen = t.lenBeats > 0; }
    else if (strcmp(rest, "key") == 0) { t.keyRoot = (int8_t)(((strtol(value, nullptr, 10) % 12) + 12) % 12); t.hasKey = 1; }
    else if (strcmp(rest, "lenk") == 0) t.lenk = (int8_t)strtol(value, nullptr, 10);
    else if (strcmp(rest, "pause") == 0) { t.pause = Generator::clampPause((int)strtol(value, nullptr, 10)); t.hasPause = 1; }   // SPEC §9.16
    // SPEC §9.17b strip params and synth knobs (0..1 floats, filter -1..1, wave an index, acid 0/1)
    else if (strcmp(rest, "vol") == 0) { t.vol = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasVol; }
    else if (strcmp(rest, "dist") == 0) { t.dist = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasDist; }
    else if (strcmp(rest, "filter") == 0) { t.filter = clampf(strtof(value, nullptr), -1, 1); t.has |= SetFileTrack::HasFilter; }
    else if (strcmp(rest, "echo") == 0) { t.echo = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasEcho; }
    else if (strcmp(rest, "reverb") == 0) { t.reverb = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasReverb; }
    else if (strcmp(rest, "cutoff") == 0) { t.cutoff = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasCutoff; }
    else if (strcmp(rest, "res") == 0) { t.res = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasRes; }
    else if (strcmp(rest, "env") == 0) { t.env = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasEnv; }
    else if (strcmp(rest, "decay") == 0) { t.decay = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasDecay; }
    else if (strcmp(rest, "drive") == 0) { t.drive = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasDrive; }
    else if (strcmp(rest, "wave") == 0) { long w = strtol(value, nullptr, 10); if (w >= 0 && w < WaveCount) { t.wave = (uint8_t)w; t.has |= SetFileTrack::HasWave; } }
    else if (strcmp(rest, "acid") == 0) { t.acid = strtol(value, nullptr, 10) != 0; t.has |= SetFileTrack::HasAcid; }
    else if (strcmp(rest, "sat") == 0) { t.sat = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasSat; }           // SPEC §9.22 303 character + chorus send
    else if (strcmp(rest, "accent") == 0) { t.accent = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasAccent; }
    else if (strcmp(rest, "chorus") == 0) { t.chorus = clampf(strtof(value, nullptr), 0, 1); t.has |= SetFileTrack::HasChorus; }
    else if (strcmp(rest, "edited") == 0) t.edited = strtol(value, nullptr, 10) != 0;   // session (SPEC §9.18)
    else if (strcmp(rest, "loop") == 0) t.loop = strtol(value, nullptr, 10) != 0;
    else if (strcmp(rest, "loopmode") == 0) t.loopMode = strtol(value, nullptr, 10) != 0;   // SPEC §9.19: the clip is a streamed loop
    else if (strcmp(rest, "kit") == 0) {                             // semicolon-separated absolute paths
      int slot = i == Beat ? 0 : i == Percussion ? 4 : -1;
      if (slot < 0) return;
      const char* p = value;
      for (int n = 0; n < 4 && *p; n++, slot++) {
        const char* e = p; while (*e && *e != ';') e++;
        int len = (int)(e - p); if (len > kMaxPathLen - 1) len = kMaxPathLen - 1;
        memcpy(f.kit[slot], p, (size_t)len); f.kit[slot][len] = 0;
        for (char* q = f.kit[slot]; *q; q++) if (*q == '\\') *q = '/';
        p = *e ? e + 1 : e;
      }
    }
  }
}

namespace { void textFn(const char* k, const char* v, void* u) { applySetProperty(*static_cast<SetFile*>(u), k, v); } }
void parseSetFileText(const char* text, int n, SetFile& out) {
  static PropertiesReader reader;                                   // 1 KB line buffer: static, not on the stack
  out.clear();
  reader.begin(textFn, &out);
  reader.feed(text, n);
  reader.finish();
}

// ---------------------------------------------------------------------------------------------- static helpers

// "<Genre>_<Key>_<NNN>.lpset" → NNN (digits after the last '_' before the extension), -1 otherwise.
int Sets::parsePresetNumber(const char* name) {
  if (!name || !hasExt(name, ".lpset")) return -1;
  int end = (int)strlen(name) - 6, start = end;
  while (start > 0 && isDigit(name[start - 1])) start--;
  if (start == end || start == 0 || name[start - 1] != '_' || end - start > 6) return -1;
  return (int)strtol(name + start, nullptr, 10);
}

// The key label between the last two underscores of "<Genre>_<Key>_<NNN>.lpset" as a minor root (SPEC §9.8
// names, SPEC §2 relative-major equivalence: "House_C_001" is A minor material). -1 when the name carries no
// label the writer would produce; the loader then opens the file to read keyRoot.
int Sets::presetKeyRoot(const char* name) {
  if (parsePresetNumber(name) < 0) return -1;
  int end = (int)strlen(name) - 6;
  while (end > 0 && isDigit(name[end - 1])) end--;
  end--;                                                            // the '_' before NNN
  int start = end;
  while (start > 0 && name[start - 1] != '_') start--;
  if (start == 0 || end - start <= 0 || end - start > 8) return -1;
  char label[9]; memcpy(label, name + start, (size_t)(end - start)); label[end - start] = 0;
  LibraryKey k = parseLibraryLabel(label);
  if (!k.valid || k.keyless) return -1;
  return k.minor ? k.root : (k.root + 9) % 12;
}

// "128bpm", "128_bpm", "bpm_128", "bpm128" (library.lua:297 contains_bpm + SPEC §9.3), value 60..249.
float Sets::bpmToken(const char* name) {
  if (!name) return 0;
  const char* b = baseName(name);
  size_t n = strlen(b);
  for (size_t i = 0; i + 3 <= n; i++) {
    if (lower(b[i]) != 'b' || lower(b[i + 1]) != 'p' || lower(b[i + 2]) != 'm') continue;
    size_t j = i; while (j > 0 && (b[j - 1] == '_' || b[j - 1] == '-' || b[j - 1] == ' ')) j--;
    size_t end = j; while (j > 0 && isDigit(b[j - 1])) j--;
    if (end > j && end - j <= 3 && !(i + 3 < n && isAlpha(b[i + 3]))) { int v = (int)strtol(b + j, nullptr, 10); if (v >= 60 && v <= 249) return (float)v; }
    size_t a = i + 3; while (a < n && (b[a] == '_' || b[a] == '-' || b[a] == ' ')) a++;
    size_t e = a; while (e < n && isDigit(b[e])) e++;
    if (e > a && e - a <= 3 && !(e < n && isDigit(b[e]))) { int v = (int)strtol(b + a, nullptr, 10); if (v >= 60 && v <= 249) return (float)v; }
  }
  return 0;
}

// <dir>/<name without .lpset>_t<track>.mid: the SMF a preset keeps for an edited / recorded / theory pattern (SPEC §9.29);
// a name of < kSetNameMax chars under a presets folder (≤ 41 chars) fits kMaxPathLen with room to spare.
void Sets::presetSmfPath(char* out, int cap, const char* dir, const char* name, int track) {
  int n = (int)strlen(name);
  if (hasExt(name, ".lpset")) n -= 6;
  snprintf(out, (size_t)cap, "%s/%.*s_t%d.mid", dir, n, name, track);
}

// SPEC §9.3: duration × fileBpm / 60 rounded to the nearest power-of-two beats (1..16), in ticks.
uint16_t Sets::naturalVocalTicks(uint32_t frames, uint32_t rate, float fileBpm) {
  if (!frames || !rate || !(fileBpm > 0)) return (uint16_t)kTicksPerBar;
  float beats = (float)frames / (float)rate * fileBpm / 60.0f;
  int best = 1; float bestD = -1;
  for (int p = 1; p <= kMaxPatternBeats; p *= 2) {
    float d = beats > (float)p ? beats / (float)p : (float)p / beats;   // ratio distance = nearest in the log domain
    if (bestD < 0 || d < bestD) { bestD = d; best = p; }
  }
  return (uint16_t)(best * kTicksPerBeat);
}

// The key of a set as its material was generated (SPEC §9.9, §2 "key change affects future generation only"):
// a majority vote of the tonal tracks' stored keys (Chord, Lead, Pad, Bass, Fx, Vocal with content); ties go to
// the global key when it is among the leaders; an empty set reports the global key.
int Sets::keyRootOf(int set) const {
  if (!gen_ || set < 0 || set >= kSets) return 9;
  const int global = gen_->key().root % 12;
  int votes[12] = {0}; int n = 0;
  for (int i = 0; i < kTracksPerSet; i++) {
    if (isSampleCategory((Category)i)) continue;
    const TrackGenInfo& info = gen_->info(set * kTracksPerSet + i);
    if (!info.file[0] && !info.loaded) continue;
    votes[info.keyRoot % 12]++; n++;
  }
  if (!n) return global;
  int best = global, bestN = votes[global];
  for (int r = 0; r < 12; r++) if (votes[r] > bestN) { best = r; bestN = votes[r]; }
  return best;
}

// ---------------------------------------------------------------------------------------------- shared

void Sets::fail(const char* msg) {
  copyStr(err_, sizeof err_, msg);
  closeFiles();
  if (flags_ & kFlagGenerating) {                                   // never leave a set half-generating (SPEC §9.5)
    if (gen_->setSwapWaiting(set_)) gen_->requestSetSwap(set_, 0);  // the burst the generator holds for us goes out now
    else { Cmd c; c.type = CmdSwapSet; c.a = (uint8_t)set_; eng_->cmds().push(c); }
    Cmd c; c.type = CmdSetGenerating; c.a = (uint8_t)set_; c.i = 0; eng_->cmds().push(c);
    flags_ &= (uint16_t)~kFlagGenerating;
  }
  mask_ = 0; ok_ = false; state_ = Idle;
  if (session_ == SsRestoring) nextRestoreSet();                   // this set's file was unusable: the other one still counts (SPEC §9.18)
  else if (session_ == SsWriting) session_ = SsNone;               // the write gave up (card error); the next change re-arms it
}

void Sets::closeFiles() {
  if (src_) { src_->close(); src_ = nullptr; }
  if (dst_) { dst_->close(); dst_ = nullptr; }
}

void Sets::indexPath(char* out, int cap) const { joinPath(out, cap, dir_, kIndexName); }

// One directory page (SPEC §9.2: ≤ kSetPage entries handled per step). Entries before `skip` are passed over.
bool Sets::pageFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  PageScan& ps = *static_cast<PageScan*>(user);
  if (ps.seen++ < ps.skip) return true;
  if (!isDir && hasExt(name, ".lpset")) {
    if (ps.collect && strlen(name) < kSetNameMax && ps.stored < kSetPage) strcpy(ps.self->names_[ps.stored++], name);
    int n = parsePresetNumber(name); if (n > ps.self->number_) ps.self->number_ = n;
  }
  return ++ps.taken < kSetPage;
}

// Shard subfolders of a key folder among its first kSetPage entries (Library::scanKids rule: a sharded folder
// holds only directories, so they are always seen).
bool Sets::kidFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  KidScan& k = *static_cast<KidScan*>(user);
  k.seen++;
  if (isDir && strlen(name) < kSetNameMax && k.n < kSetPage) strcpy(k.self->names_[k.n++], name);
  return k.seen < kSetPage;
}

// Chunk page_ of _index.txt into chunk_ (one open + seek + read, SPEC §9.2); false at the end of the file.
bool Sets::readIndexChunk() {
  char p[kMaxPathLen]; indexPath(p, sizeof p);
  IFile* f = fs_->open(p, "r");
  if (!f) return false;
  size_t n = 0;
  if (f->seek((uint32_t)page_ * kSetChunk)) n = f->read(chunk_, kSetChunk);
  f->close();
  count_ = (int)n;
  flags_ |= kFlagChunk;
  if (n < (size_t)kSetChunk) flags_ |= kFlagEof;
  return n > 0;
}

// Names from chunk_[item_ .. count_): one basename per LF line (CR ignored, over-long lines dropped), each
// handed to considerName (load) or the NNN maximum (save). True when a probe interrupted the scan.
void Sets::scanIndex() {
  while (item_ < count_) {
    const char c = (char)chunk_[item_++];
    if (c == '\n') {
      if (idxLen_ > 0) {
        idxLine_[idxLen_] = 0; idxLen_ = 0;
        if (state_ == SvIndexRead) { int n = parsePresetNumber(idxLine_); if (n > number_) number_ = n; }
        else if (state_ == PlIndexRead) considerRow(idxLine_);     // SPEC §9.22 listing: no probes, the rows come from the names
        else if (considerName(idxLine_)) { state_ = LdProbe; return; }
      }
      idxLen_ = 0;
      continue;
    }
    if (c == '\r' || idxLen_ < 0) continue;
    if (idxLen_ < kSetNameMax - 1) idxLine_[idxLen_++] = c; else idxLen_ = -1;   // too long: dropped whole
  }
}

// The key filter from the name alone (SPEC §9.8 "<Genre>_<Key>_<NNN>"): a match is offered, an unlabelled
// name needs a probe (its keyRoot line is read), everything else is skipped without SD work.
bool Sets::considerName(const char* name) {
  if (!hasExt(name, ".lpset") || strlen(name) >= kSetNameMax) return false;
  const int root = presetKeyRoot(name);
  if (root < 0) return true;
  if (root == wantRoot_) offer(name);
  return false;
}

// Reservoir over the matches: uniform over the candidates — the matches not yet loaded in this cycle (SPEC §9.25) — and
// the last loaded name only as the fallback (SPEC §9.8). A match whose bit is set is counted (skipped_) and passed over;
// in the new-cycle pass (kFlagNewCycle, chooseFile) every match's bit is cleared first, so all of them are candidates again.
void Sets::offer(const char* name) {
  const int bit = presetBit(name);
  if (bit >= 0 && (flags_ & kFlagNewCycle)) setLoadedBit(set_, bit, false);
  if (strcmp(name, lastName_[set_]) == 0) { copyStr(fallback_, sizeof fallback_, name); return; }
  if (bit >= 0 && loadedBit(set_, bit)) { skipped_++; return; }
  matches_++;
  if (rng_.range(0, matches_ - 1) == 0) copyStr(chosen_, sizeof chosen_, name);
}

// ---- the random-load cycle (SPEC §9.25): per set, one bit per preset number of the current genre ----
int Sets::presetBit(const char* name) { const int n = parsePresetNumber(name); return n < 0 ? -1 : n % kLoadedPresets; }
bool Sets::loadedBit(int set, int bit) const { return ((loaded_[set][bit >> 3] >> (bit & 7)) & 1) != 0; }
void Sets::setLoadedBit(int set, int bit, bool on) {
  uint8_t& b = loaded_[set][bit >> 3];
  const uint8_t m = (uint8_t)(1u << (bit & 7)), was = b;
  b = on ? (uint8_t)(b | m) : (uint8_t)(b & (uint8_t)~m);
  if (b != was && sessionOn_ && session_ != SsRestoring) markSessionDirty();   // the cycle survives power-off: the next session write carries the masks
}
// The masks describe one genre's presets: a load, save or preset op in another genre starts fresh cycles there.
void Sets::syncLoadedGenre() {
  const uint8_t g = (uint8_t)gen_->genreIndex();
  if (g == loadedGenre_) return;
  loadedGenre_ = g;
  memset(loaded_, 0, sizeof loaded_);
}
bool Sets::presetLoadedInCycle(int set, int nnn) const {
  if (set < 0 || set >= kSets || nnn < 0 || !gen_ || loadedGenre_ != (uint8_t)gen_->genreIndex()) return false;
  return loadedBit(set, nnn % kLoadedPresets);
}
// The recorder's hook (sets.h): masks, genre tag and the no-repeat names back to the state a fresh Sets has, so a recording
// and its render start their random loads from the same known state (review 2026-10-16: with the live pick kept as
// lastName_ and its cycle bit set, the render's reservoir ran over a different candidate set and loaded another preset).
void Sets::resetLoadCycle() {
  bool changed = false;
  for (int s = 0; s < kSets; s++) {
    if (lastName_[s][0]) { lastName_[s][0] = 0; changed = true; }
    for (int i = 0; i < kLoadedPresets / 8; i++) if (loaded_[s][i]) { changed = true; break; }
  }
  memset(loaded_, 0, sizeof loaded_); loadedGenre_ = 255;
  if (changed && sessionOn_ && session_ != SsRestoring) markSessionDirty();   // the persisted cycle follows the RAM one (setLoadedBit's rule)
}

void Sets::init(IFileSystem* fs, Engine* eng, Generator* gen, Library* lib) {
  fs_ = fs; eng_ = eng; gen_ = gen; lib_ = lib; state_ = Idle;
  session_ = SsNone; sessionSet_ = 0; sessionDirty_ = 0; restorePending_ = 0; sessionOn_ = 0; dirtyClock_ = 0;
  genChanges_ = gen ? gen->changeCount() : 0; sessionVolume_ = -1; restoredVolume_ = -1;
  sessionSeq_ = 0; sessionSlot_ = 0; probeSeq_[0] = probeSeq_[1] = 0; probeOk_ = 0;
  listValid_ = 0; listDone_ = 0; listTotal_ = -1; passTotal_ = 0; filterRoot_ = -1; listGenre_ = 0; listFirst_ = 0;   // SPEC §9.22
  rowCount_ = 0; rowBase_ = 0; passIdx_ = 0; passTarget_ = 0; listRem_ = 0; labelRow_ = 0; thrSet_ = 0;
  lineMode_ = 0; lineInsert_ = 0; lineKey_ = 0; label_[0] = 0; name_[0] = 0;
  for (int s = 0; s < kSets; s++) { loadedLabel_[s][0] = 0; lastName_[s][0] = 0; }   // lastName_: a Sets that is not value-initialised (a test's member, a reused block) would otherwise start with a stale "last loaded" name and skip it (SPEC §9.8)
  resetLoadCycle(); skipped_ = 0;                                     // SPEC §9.25: the masks start empty (the restore brings the persisted ones back)
}

void Sets::step() {
  pollSession();                                                    // SPEC §9.18: changes → dirty, the debounce, the queued restore
  if (state_ == Idle) return;
  if (session_ == SsWriting && gen_->busy()) { abortSession(); return; }   // the generator took the card: re-armed, written later
  if (session_ == SsWriting && gen_->loopSlotStreaming()) return;   // SPEC §9.19 §3: a slot ring (139 ms) cannot absorb a card open; the write pauses where it is until the slot tracks stop
  if (!gen_->streamsHealthy()) return;                              // SPEC §9.19 §3 a: card work only while every consumed stream ring holds its lead (the pump refills first)
  if (state_ >= PlOpen) stepPreset(); else if (state_ >= LdOpen) stepLoad(); else stepSave();
}

bool Sets::save(int set, char* outName, int cap) {
  if (!beginSave(set)) return false;
  for (uint32_t guard = 0; guard < 4000000u && busy(); guard++) step();
  if (busy()) fail("TIMEOUT");
  if (outName && cap > 0) copyStr(outName, cap, name_);
  return ok_;
}

// Synchronous up to the choice: listing, probing and parsing need no engine; the reconstruction (slot acks,
// bar-line swap) continues in step() and needs the render task, so it is never waited for here.
bool Sets::loadRandom(int set, Rng& rng, char* outName, int cap) {
  if (outName && cap > 0) outName[0] = 0;
  if (!beginLoad(set, rng)) return false;
  for (uint32_t guard = 0; guard < 4000000u && busy() && state_ < LdTrack; guard++) step();
  if (busy() && state_ < LdTrack) fail("TIMEOUT");
  bool chosen = busy() || ok_;
  if (outName && cap > 0) copyStr(outName, cap, chosen ? name_ : "");
  return chosen;
}

// ---------------------------------------------------------------------------------------------- save

bool Sets::beginSave(int set) {
  if (session_ == SsWriting) abortSession();                        // the user's save wins; the session is rewritten afterwards
  if (busy()) { copyStr(err_, sizeof err_, "BUSY"); return false; }
  if (!fs_ || !eng_ || !gen_ || set < 0 || set >= kSets) { copyStr(err_, sizeof err_, "NO FS"); return false; }
  if (gen_->busy()) { copyStr(err_, sizeof err_, "GEN BUSY"); return false; }   // one card state machine at a time (SPEC §9.1, §9.5)
  const char* genre = gen_->genreName();
  if (!genre || !genre[0]) { copyStr(err_, sizeof err_, "NO GENRE"); return false; }
  syncLoadedGenre();                                                // SPEC §9.25: the new preset's bit is cleared in this genre's masks when the save completes
  set_ = set; track_ = 0; done_ = 0; page_ = 0; item_ = 0; count_ = 0; number_ = 0; slot_ = 0; flags_ = 0; phase_ = 0; attempts_ = 0; idxLen_ = 0; mask_ = 0; wait_ = 0;
  ok_ = false; err_[0] = 0; name_[0] = 0;
  listValid_ = 0; listDone_ = 0;                                    // SPEC §9.22: names_ is the writer's again
  file_.clear();
  joinPath(dir_, sizeof dir_, "/lp/packs", genre);
  joinPath(dir_, sizeof dir_, dir_, "presets");
  file_.schemaVersion = 1; file_.setIndex = set; file_.keyRoot = keyRootOf(set); file_.bpm = eng_->status().bpm;   // the tracks' key (SPEC §9.9)
  snprintf(file_.keyName, sizeof file_.keyName, "%s minor", noteName(file_.keyRoot));
  copyStr(file_.genre, sizeof file_.genre, genre);
  copyStr(file_.label, sizeof file_.label, loadedLabel_[set]);      // SPEC §9.22: the label of the preset this set came from travels into the new file
  state_ = SvMkdir;
  return true;
}

void Sets::stepSave() {
  switch (state_) {
    case SvMkdir:                                                   // presets/ (SPEC §9.8; no assets/ folder since §9.29): one op
      if (!fs_->mkdir(dir_)) { fail("MKDIR"); return; }
      state_ = SvIndexProbe;
      break;
    case SvIndexProbe:                                              // index present? (one probe, its own step: one card stall under one ring lead, SPEC §9.19 §3)
      indexPath(path_, sizeof path_);
      page_ = 0; item_ = 0; count_ = 0; idxLen_ = 0; number_ = 0;
      if (fs_->exists(path_)) { flags_ |= kFlagIndex; state_ = SvIndexRead; } else state_ = SvScan;
      break;
    case SvIndexRead:                                               // NNN = max over the index, one 4 KB chunk per step
      if (!(flags_ & kFlagChunk)) {
        if (((flags_ & kFlagEof) && item_ >= count_) || !readIndexChunk()) { indexDone(); break; }
      }
      scanIndex();
      if (flags_ & kFlagEof) { indexDone(); break; }
      page_++; item_ = 0; flags_ &= (uint16_t)~kFlagChunk;
      break;
    case SvScan: {                                                  // no index yet: one directory page per step, the index is built from it
      PageScan ps = {this, page_ * kSetPage, 0, 0, 0, true};
      fs_->walk(dir_, pageFn, &ps);
      count_ = ps.stored;
      flags_ = ps.taken < kSetPage ? (uint16_t)(flags_ | kFlagLastPage) : (uint16_t)(flags_ & ~kFlagLastPage);
      state_ = SvIndexPage;
      break; }
    case SvIndexPage: {                                             // the page's names → _index.txt (one open, then one write per step)
      int n = 0;
      for (int i = 0; i < count_ && n + kSetNameMax + 1 < kSetChunk; i++) n += snprintf(reinterpret_cast<char*>(chunk_) + n, (size_t)(kSetChunk - n), "%s\n", names_[i]);
      const int r = indexStep(reinterpret_cast<const char*>(chunk_), n, page_ == 0);
      if (r < 0) { fail("INDEX"); return; }
      if (r == 0) break;
      if (flags_ & kFlagLastPage) indexDone(); else { page_++; state_ = SvScan; }
      break; }
    case SvNumber: {                                                // a hand-copied file may hold the number already: one probe per step
      char minorLabel[8], majorLabel[8]; ProjectKey k; k.root = (uint8_t)file_.keyRoot; k.labels(minorLabel, majorLabel);
      snprintf(name_, sizeof name_, "%.40s_%s_%03d.lpset", file_.genre, minorLabel, number_);   // ≤ 58 chars
      joinPath(path_, sizeof path_, dir_, name_);
      if (fs_->exists(path_)) { number_++; if (++attempts_ > kSetNumberRetries) { fail("NAME"); return; } break; }
      attempts_ = 0; track_ = 0; state_ = SvTrack;
      break; }
    case SvTrack:
      if (track_ >= kTracksPerSet) { state_ = SvKitPaths; slot_ = 0; phase_ = 0; attempts_ = 0; item_ = -1; break; }
      if (!trackBegin()) { track_++; done_++; }                     // the track's source is known (or there is none): no SD work
      break;
    case SvResolve: resolveStep(); break;
    case SvEdit: writeEdit(); break;                                // the pattern as an SMF next to the preset / in /lp/state (one open + write)
    case SvKitPaths: resolveKitPaths(); break;                      // ≤ 3 exists() probes or one bounded walk per step
    case SvWrite: {                                                 // ≤ 4 KB per step, the file stays open in dst_ (SPEC §9.2)
      if (!dst_) joinPath(path_, sizeof path_, dir_, name_);
      const int r = stepWriteFile(path_);
      if (r < 0) { fail("WRITE SET"); return; }
      if (r > 0) state_ = SvIndexAppend;
      break; }
    case SvIndexAppend: {                                           // the new name joins the index (one open, then one write)
      char line[kSetNameMax + 2]; int n = snprintf(line, sizeof line, "%s\n", name_);
      const int r = indexStep(line, n, false);
      if (r < 0) { fail("INDEX"); return; }
      if (r == 0) break;
      ok_ = !(flags_ & kFlagPartial); state_ = Idle;                // partial: written, but err_ names the missing source
      { const int bit = presetBit(name_); if (bit >= 0) for (int s = 0; s < kSets; s++) setLoadedBit(s, bit, false); }   // SPEC §9.25: a new preset comes up in the current cycle (its number may have been a deleted preset's)
      break; }
    // ---- session write (SPEC §9.18): SsMkdir → SvTrack (SvResolve / SvEdit per track) → SvKitPaths → SsWrite, per set
    case SsMkdir:
      if (!fs_->mkdir(kSessionDir)) { session_ = SsNone; state_ = Idle; return; }   // no card: give up until the next change
      track_ = 0; state_ = SvTrack;
      break;
    case SsWrite: {
      if (!dst_) sessionFilePath(path_, sizeof path_, set_, (sessionSlot_ >> set_) & 1);
      const int r = stepWriteFile(path_);
      if (r == 0) break;
      if (r > 0) sessionSlot_ ^= (uint8_t)(1u << set_);           // this slot holds the newest complete file: the next write takes the other
      if (r < 0 || ++sessionSet_ >= kSets) { session_ = SsNone; state_ = Idle; lastWriteClock_ = eng_->status().sampleClock; wrote_ = 1; }   // the 15 s spacing counts from here
      else startSessionSet();
      break; }
    default: state_ = Idle; break;
  }
}

// End of the index stream (save: NNN = max + 1; load: choose). A last line without LF is flushed first.
void Sets::indexDone() {
  if (idxLen_ > 0) {
    idxLine_[idxLen_] = 0; idxLen_ = 0;
    if (state_ == SvIndexRead) { int n = parsePresetNumber(idxLine_); if (n > number_) number_ = n; }
    else if (state_ == PlIndexRead) considerRow(idxLine_);
    else if (considerName(idxLine_)) { state_ = LdProbe; return; }
  }
  idxLen_ = 0;
  if (state_ == SvIndexRead || state_ == SvIndexPage) { number_++; attempts_ = 0; state_ = SvNumber; }
  else if (state_ == PlIndexRead) listPassDone();
  else state_ = LdChoose;
}

// _index.txt in two steps (SPEC §9.19 §3: an open and a write are two card stalls, one per step): the first call opens
// the index into dst_ (truncated or appended) and returns 0; the next writes `text` and closes it (1, or -1 on a short write).
int Sets::indexStep(const char* text, int n, bool truncate) {
  if (!dst_) {
    char p[kMaxPathLen]; indexPath(p, sizeof p);
    dst_ = fs_->open(p, truncate ? "w" : "a");
    return dst_ ? 0 : -1;
  }
  const bool ok = n <= 0 || dst_->write(text, (size_t)n) == (size_t)n;
  closeFiles();
  return ok ? 1 : -1;
}

// The fields both writers share (trackBegin): variant, kick cut, seed, preset, key, len / lenk, pause (SPEC §9.8, §9.16) and the
// strip params + synth knobs + acid flag of SPEC §9.17b, read from the engine's targets (what a load restores).
// Variant and length are the generator's INTENT (info.variant, natural × 2^lenStep): the engine's active pattern only
// follows at a bar line (never while the transport is stopped), and a save or session write inside that wait must
// not bring the previous variant / Beat layer / clip length back. The kick cut applies at once, so it is the engine's.
void Sets::fillTrackFields(int t, SetFileTrack& ft) {
  const Category cat = trackCategory(t);
  const TrackGenInfo& info = gen_->info(t);
  const TrackStatus& st = eng_->status().track[t];
  ft.variant = (uint8_t)(info.variant & 3); ft.beatKickMuted = st.kickCut;
  ft.seed = info.seed; ft.hasSeed = 1;
  ft.preset = info.presetIndex; ft.hasPreset = isSynthCategory(cat);
  ft.keyRoot = (int8_t)(info.keyRoot % 12); ft.hasKey = 1;        // the key the pattern was generated with (SPEC §9.9)
  // SPEC §9.16: the Vocal's clip is its window + pause cycle; lp.track.6.len stays the natural power-of-two beats of the
  // phrase (SPEC §9.3) and the pause travels as lp.track.6.pause. Other tracks write natural × 2^k (SPEC §9.4).
  const bool loop = gen_->loopEnabled(t);                           // SPEC §9.19: the clip is the streamed loop: natural = its crop, len = natural × 2^k up to kMaxLoopBars bars
  const uint32_t natural = info.naturalTicks ? info.naturalTicks : (uint32_t)kTicksPerBar;
  const int k = (cat == Vocal && !loop) ? 0 : clampLenStep(natural, info.lenStep, loop);
  const uint16_t len = (cat == Vocal && !loop) ? (uint16_t)natural : lenOfStep(natural, k, loop);
  ft.lenBeats = (float)len / (float)kTicksPerBeat; ft.hasLen = 1;
  ft.lenk = (int8_t)k;                                              // len = natural × 2^k (SPEC §9.4)
  ft.loopMode = loop ? 1 : 0;
  ft.pause = Generator::clampPause(info.pause); ft.hasPause = cat == Vocal;
  const TrackParams& p = eng_->params(t);
  ft.vol = p.volume; ft.dist = p.dist; ft.filter = p.filter; ft.echo = p.echo; ft.reverb = p.reverb;
  ft.has = SetFileTrack::HasVol | SetFileTrack::HasDist | SetFileTrack::HasFilter | SetFileTrack::HasEcho | SetFileTrack::HasReverb;
  if (isSynthCategory(cat)) {
    ft.cutoff = p.cutoff; ft.res = p.resonance; ft.env = p.envAmt; ft.decay = p.decay; ft.drive = p.drive; ft.wave = p.wave; ft.acid = info.acid ? 1 : 0;
    ft.sat = p.sat; ft.accent = p.accent; ft.chorus = p.chorus;                                   // SPEC §9.22
    ft.has |= SetFileTrack::HasCutoff | SetFileTrack::HasRes | SetFileTrack::HasEnv | SetFileTrack::HasDecay | SetFileTrack::HasDrive | SetFileTrack::HasWave | SetFileTrack::HasAcid
            | SetFileTrack::HasSat | SetFileTrack::HasAccent | SetFileTrack::HasChorus;
  }
}

// Fills file_.track[track_] and decides where the track's source line comes from — the preset and the session writer
// alike (SPEC §9.29): the loop's absolute path (loop mode), the vocal's own stream path, "acid" for a generated line (its
// seed and key are in the file), an SMF written now (SvEdit) for an edited / recorded / theory pattern — and for a pattern
// whose source is not a library file (a preset's own SMF, a session SMF: files a delete or the next session write may
// replace, so every preset owns its copy) — or the library file behind info.file (SvResolve probes). True when SD work
// follows; false = the track is done (no source: nothing is written for it).
bool Sets::trackBegin() {
  const bool session = session_ == SsWriting;
  const int t = set_ * kTracksPerSet + track_;
  const Category cat = trackCategory(t);
  const TrackGenInfo& info = gen_->info(t);
  SetFileTrack& ft = file_.track[track_];
  fillTrackFields(t, ft);
  ft.edited = 0; ft.loop = 0;
  char* src = file_.src[track_]; src[0] = 0;
  if (ft.loopMode) { const char* lp = gen_->loopPath(t); if (lp[0]) copyStr(src, kMaxPathLen, lp); return false; }   // SPEC §9.19 §1.7: the loop's absolute path as known (no probe; a pick still in flight leaves the line out)
  if (cat == Vocal) {                                               // the phrase behind the variant: the stream the generator opened (no probe), else the probes of resolveStep
    if (info.loaded && info.file[0]) {
      const char* vp = gen_->vocalPath(set_, info.variant == 1 ? 1 : 0);
      for (int s = 0; s < 2; s++) { const char* p = gen_->vocalPath(set_, s); if (p[0] && strcmp(baseName(p), info.file) == 0) { vp = p; break; } }   // the slot that holds the named file first
      if (vp[0]) { copyStr(src, kMaxPathLen, vp); return false; }
    }
    if (session || !info.file[0]) return false;
    phase_ = 0; item_ = -1; count_ = 0; state_ = SvResolve;
    return true;
  }
  const Pattern& pat = eng_->sourcePattern(t);
  if (pat.count == 0) return false;
  if (info.acidPat && !info.edited && !info.userLoop) { copyStr(src, kMaxPathLen, "acid"); return false; }
  bool own = info.edited || info.userLoop || !info.file[0];        // SPEC §9.8: edited / recorded / theory patterns are written with the SMF writer
  if (!own && gen_->sourcePath(t, path_, sizeof path_) && ownedSource(path_)) own = true;   // a preset's _t<i>.mid or a session SMF: this file gets its own copy (SPEC §9.29)
  if (own) { ft.edited = (info.edited || !info.file[0]) ? 1 : 0; ft.loop = info.userLoop ? 1 : 0; state_ = SvEdit; return true; }
  phase_ = 0; item_ = -1; count_ = 0; state_ = SvResolve;
  return true;
}

bool Sets::resolveProbe(const char* base, const char* folder, const char* kid, const char* file) {
  joinPath(path_, sizeof path_, base, folder);
  if (kid) joinPath(path_, sizeof path_, path_, kid);
  joinPath(path_, sizeof path_, path_, file);
  return fs_->exists(path_);
}

// The library file behind info.file, one SD action per step (SPEC §9.2): phase 0 takes the path the generator
// remembered (one probe for a preset — nothing is referenced that is not there; none for a session file, SPEC §9.19
// §3 b), then the vocal stream's own path and the ≤ 2 key folders (/lp/midi/<Category>/<KeyFolder>/ or
// <genre>/vocal/<KeyFolder>/); phases 1.. list each key folder's shard subfolders (one bounded walk) and probe them one
// per step; the last phase probes this genre's legacy presets/assets/ (a set loaded from a pre-§9.29 file). Found →
// lp.src.i = that absolute path. Not found → a preset leaves the track out, lastError names it ("NO SRC CHD") and ok()
// ends false (nothing is dropped quietly); a session file keeps the notes as a session SMF instead.
void Sets::resolveStep() {
  const int t = set_ * kTracksPerSet + track_;
  const Category cat = trackCategory(t);
  const char* file = gen_->info(t).file;
  const bool vocal = cat == Vocal;
  SetFileTrack& ft = file_.track[track_];
  char base[kMaxPathLen];
  if (vocal) { joinPath(base, sizeof base, "/lp/packs", file_.genre); joinPath(base, sizeof base, base, "vocal"); }
  else joinPath(base, sizeof base, "/lp/midi", categoryName(cat));
  char folder[2][16]; const int n = keyFolders(file, folder);
  bool found = false, done = false;
  for (;;) {
    if (phase_ == 0) {                                              // the generator's remembered folder first (SPEC §9.19 §3 b), then direct probes (≤ 3 exists)
      if (gen_->sourcePath(t, path_, sizeof path_)) {
        if (session_ == SsWriting) { found = true; done = true; break; }   // the session file takes the path as known: no verification open
        found = fs_->exists(path_);                                 // a preset references the file: one probe keeps the NO SRC rule for a gone file
        if (found) { done = true; break; }
      }
      if (file[0] == '/') { copyStr(path_, sizeof path_, file); found = fs_->exists(path_); done = true; break; }
      for (int s = 0; s < 2 && !found && vocal; s++) {
        const char* vp = gen_->vocalPath(set_, s);
        if (vp[0] && strcmp(baseName(vp), file) == 0) { copyStr(path_, sizeof path_, vp); found = fs_->exists(path_); }
      }
      for (int i = 0; i < n && !found; i++) found = resolveProbe(base, folder[i], nullptr, file);
      phase_ = 1; item_ = -1; count_ = 0;
      break;
    }
    if (phase_ <= n) {                                              // shard subfolders of key folder phase_-1
      const char* kf = folder[phase_ - 1];
      if (item_ < 0) {
        char dirp[kMaxPathLen]; joinPath(dirp, sizeof dirp, base, kf);
        KidScan ks = {this, 0, 0};
        fs_->walk(dirp, kidFn, &ks);
        count_ = ks.n; item_ = 0;
        break;                                                      // one walk this step
      }
      if (item_ < count_) { found = resolveProbe(base, kf, names_[item_], file); item_++; break; }
      phase_++; item_ = -1; count_ = 0;
      continue;                                                     // pure transition: next folder in the same step
    }
    joinPath(path_, sizeof path_, dir_, "assets"); joinPath(path_, sizeof path_, path_, file);   // a legacy presets/assets/ copy (SPEC §9.29)
    found = fs_->exists(path_); done = true;
    break;
  }
  if (found) {                                                      // lp.src.i = the absolute path itself (SPEC §9.18, §9.29)
    copyStr(file_.src[track_], kMaxPathLen, path_);
    track_++; done_++; state_ = SvTrack;
    return;
  }
  if (!done) return;                                                // more candidates next step
  if (session_ == SsWriting) {                                      // gone from the card: keep the notes as a session SMF instead
    if (eng_->sourcePattern(t).count > 0) { ft.edited = 1; state_ = SvEdit; }   // edited: a later preset save writes its own SMF (the library name is dead)
    else { track_++; done_++; state_ = SvTrack; }
    return;
  }
  file_.src[track_][0] = 0;
  snprintf(err_, sizeof err_, "NO SRC %s", categoryShort(cat));
  flags_ |= kFlagPartial;
  track_++; done_++; state_ = SvTrack;
}

// SvEdit: the engine's source pattern as an SMF (SPEC §9.8, §9.29) — presets/<name>_t<i>.mid next to the preset, or
// /lp/state/session_s<set>_t<i>.mid for a session file — one open + write; lp.src.i then points at it. A preset whose
// SMF could not be written is still written, without that track, and ok() ends false ("WRITE SMF").
void Sets::writeEdit() {
  const int t = set_ * kTracksPerSet + track_;
  if (session_ == SsWriting) snprintf(path_, sizeof path_, "%s/session_s%d_t%d.mid", kSessionDir, set_, track_);
  else presetSmfPath(path_, sizeof path_, dir_, name_, track_);
  IFile* f = fs_->open(path_, "w");
  bool written = f != nullptr;
  if (f) { written = smfWrite(*f, eng_->sourcePattern(t), (float)eng_->status().bpm); f->close(); }
  if (written) copyStr(file_.src[track_], kMaxPathLen, path_);
  else { file_.src[track_][0] = 0; if (session_ != SsWriting) { copyStr(err_, sizeof err_, "WRITE SMF"); flags_ |= kFlagPartial; } }
  track_++; done_++; state_ = SvTrack;
}

// Absolute path of the kit one-shot in slot_: the engine only keeps basenames (setKitName), so the file is
// looked up under the drum role folders (SPEC §6 roles) of this genre (phase 0) and then of every other genre.
// A role folder is either flat or sharded into subfolders (Library, SPEC §9.2 FAT folder sizes), so a genre
// costs one step of ≤ 3 probes (the role folders themselves) and then, per role folder (attempts_ - 1), one
// bounded walk listing its shard subfolders (kidFn, ≤ kSetPage entries) and ≤ 3 shard probes per step.
// Unresolved names are written bare; the loader then falls back to a random pick of the same role.
void Sets::resolveKitPaths() {
  const char* name = eng_->kit(set_).name[slot_];
  char* out = file_.kit[slot_];
  const int genres = lib_ ? lib_->genreCount() : 0;
  auto nextSlot = [&]() { phase_ = 0; attempts_ = 0; item_ = -1; if (++slot_ >= kSampleSlots) state_ = session_ == SsWriting ? SsWrite : SvWrite; };
  if (!name || !name[0]) { out[0] = 0; nextSlot(); return; }
  if (phase_ == 0 && attempts_ == 0 && gen_->kitPath(set_, slot_, out, kMaxPathLen)) { nextSlot(); return; }   // remembered at pick / load time: no probe (SPEC §9.19 §3 b)
  const char* genre = nullptr;
  for (;;) {                                                        // phase_: this genre, then every other one
    if (phase_ == 0) { genre = file_.genre; break; }
    if (phase_ - 1 >= genres) break;
    genre = lib_->genreName(phase_ - 1);
    if (strcmp(genre, file_.genre) != 0) break;
    phase_++;
  }
  if (!genre) { copyStr(out, kMaxPathLen, name); nextSlot(); return; }   // found nowhere: the bare name
  auto rolePath = [&](char* dst, int r) {
    joinPath(dst, kMaxPathLen, "/lp/packs", genre);
    joinPath(dst, kMaxPathLen, dst, "drum");
    joinPath(dst, kMaxPathLen, dst, kSlotRoles[slot_][r]);
  };
  if (attempts_ == 0) {                                             // the role folders themselves: ≤ 3 probes
    for (int r = 0; r < 3 && kSlotRoles[slot_][r]; r++) {
      rolePath(out, r); joinPath(out, kMaxPathLen, out, name);
      if (fs_->exists(out)) { nextSlot(); return; }
    }
    attempts_ = 1; item_ = -1;
    return;
  }
  const int r = attempts_ - 1;
  if (r >= 3 || !kSlotRoles[slot_][r]) { phase_++; attempts_ = 0; item_ = -1; return; }   // next genre next step
  if (item_ < 0) {                                                  // the shard subfolders of role folder r: one walk
    rolePath(path_, r);
    KidScan ks = {this, 0, 0};
    fs_->walk(path_, kidFn, &ks);
    count_ = ks.n; item_ = 0;
    if (count_ == 0) { attempts_++; item_ = -1; }                   // flat (or absent): next role folder next step
    return;
  }
  for (int p = 0; p < 3 && item_ < count_; p++, item_++) {          // ≤ 3 shard probes per step
    rolePath(out, r); joinPath(out, kMaxPathLen, out, names_[item_]); joinPath(out, kMaxPathLen, out, name);
    if (fs_->exists(out)) { nextSlot(); return; }
  }
  if (item_ >= count_) { attempts_++; item_ = -1; }
}

// Java properties, ASCII, forward slashes (the reader splits at the first ':' or '='; values may hold either). The
// header (schemaVersion, setIndex, keyRoot, keyName as a human hint, genre, lp.label, lp.banks), lp.bpm, then per track
// the lp.track.N.* keys — variant, kickcut (Beat), preset, seed, len, key, lenk, pause (Vocal), kit (Beat / Percussion:
// four absolute paths), the SPEC §9.17b strip {vol,dist,filter,echo,reverb} and, on the synth tracks, the knobs
// {cutoff,res,env,decay,drive,wave,acid,sat,accent,chorus}, loopmode — and lp.src.N, the track's source (SPEC §9.29).
// A session file (SPEC §9.18) adds lp.seq, lp.loaded.<set>, lp.volume, edited / loop, lp.kit.N and the lp.end
// terminator. The text is produced in kSetParts parts (header, lp.bpm, each track's lines, the session kit lines, the
// terminator), each well under 2 KB; stepWriteFile packs whole parts into chunk_ and writes at most one 4 KB chunk per
// step with the file kept open in dst_ (SPEC §9.2).
struct Sets::Sink {                                                 // appends into a buffer; `over` once a part did not fit
  char* buf; int cap; int n; bool over;
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (over) return;
    va_list ap; va_start(ap, fmt);
    const int w = vsnprintf(buf + n, (size_t)(cap - n), fmt, ap);
    va_end(ap);
    if (w < 0 || w >= cap - n) over = true; else n += w;
  }
};
namespace { constexpr int kSetParts = kTracksPerSet + 4; }          // header, lp.bpm, 8 track parts, the session kit lines, the session terminator

void Sets::writeSetPart(int part, Sink& out) {
  const bool session = session_ == SsWriting;
  if (part == 0) {
    out.add("schemaVersion=1\nsetIndex=%d\nkeyRoot=%d\nkeyName=%s\ngenre=%s\n", file_.setIndex, file_.keyRoot, file_.keyName, file_.genre);
    if (file_.label[0]) out.add("lp.label=%s\n", file_.label);     // SPEC §9.22: the preset's tag, right after genre= (where beginSetLabel puts it too); a session file keeps it for the restore
    out.add("lp.banks=%d\n", kBanksLayout);                          // the bank layout the lp.track.N.preset indices below refer to (the loader remaps files without it, see presetForLayout)
    if (session) out.add("lp.seq=%u\n", (unsigned)file_.seq);        // SPEC §9.18: the restore takes the newest complete slot
    if (session && loadedGenre_ == (uint8_t)gen_->genreIndex()) {     // SPEC §9.25: this set's load-cycle mask of the file's genre (byte i = presets 8i..8i+7, bit n & 7 = preset n); masks of another genre are not this file's
      out.add("lp.loaded.%d=", set_);
      for (int i = 0; i < kLoadedPresets / 8; i++) out.add("%02x", (unsigned)loaded_[set_][i]);
      out.add("\n");
    }
    return;
  }
  if (part == 1) {
    out.add("lp.bpm=%d\n", file_.bpm);
    if (session && file_.volume >= 0) out.add("lp.volume=%d\n", file_.volume);
    return;
  }
  if (part <= kTracksPerSet + 1) {
    const int i = part - 2; const SetFileTrack& t = file_.track[i];
    out.add("lp.track.%d.variant=%d\n", i, t.variant);              // SPEC §9.29: the variant (the Beat's layer) and the kick cut as lp keys
    if (i == Beat) out.add("lp.track.%d.kickcut=%d\n", i, t.beatKickMuted ? 1 : 0);
    if (t.hasPreset) out.add("lp.track.%d.preset=%d\n", i, t.preset);
    char beats[16]; formatBeats(beats, sizeof beats, t.lenBeats);
    out.add("lp.track.%d.seed=%u\nlp.track.%d.len=%s\nlp.track.%d.key=%d\nlp.track.%d.lenk=%d\n", i, (unsigned)t.seed, i, beats, i, t.keyRoot, i, t.lenk);
    if (i == Vocal) out.add("lp.track.%d.pause=%d\n", i, t.pause);   // SPEC §9.16
    if (i == Beat || i == Percussion) {
      const int first = i == Beat ? 0 : 4;
      out.add("lp.track.%d.kit=", i);
      for (int s = first; s < first + 4; s++) out.add("%s%s", s > first ? ";" : "", file_.kit[s]);
      out.add("\n");
    }
    if (t.has & SetFileTrack::HasVol) out.add("lp.track.%d.vol=%.3f\n", i, (double)t.vol);        // SPEC §9.17b strip + knobs
    if (t.has & SetFileTrack::HasDist) out.add("lp.track.%d.dist=%.3f\n", i, (double)t.dist);
    if (t.has & SetFileTrack::HasFilter) out.add("lp.track.%d.filter=%.3f\n", i, (double)t.filter);
    if (t.has & SetFileTrack::HasEcho) out.add("lp.track.%d.echo=%.3f\n", i, (double)t.echo);
    if (t.has & SetFileTrack::HasReverb) out.add("lp.track.%d.reverb=%.3f\n", i, (double)t.reverb);
    if (t.has & SetFileTrack::HasCutoff) out.add("lp.track.%d.cutoff=%.3f\n", i, (double)t.cutoff);
    if (t.has & SetFileTrack::HasRes) out.add("lp.track.%d.res=%.3f\n", i, (double)t.res);
    if (t.has & SetFileTrack::HasEnv) out.add("lp.track.%d.env=%.3f\n", i, (double)t.env);
    if (t.has & SetFileTrack::HasDecay) out.add("lp.track.%d.decay=%.3f\n", i, (double)t.decay);
    if (t.has & SetFileTrack::HasDrive) out.add("lp.track.%d.drive=%.3f\n", i, (double)t.drive);
    if (t.has & SetFileTrack::HasWave) out.add("lp.track.%d.wave=%d\n", i, (int)t.wave);
    if (t.has & SetFileTrack::HasAcid) out.add("lp.track.%d.acid=%d\n", i, t.acid ? 1 : 0);
    if (t.has & SetFileTrack::HasSat) out.add("lp.track.%d.sat=%.3f\n", i, (double)t.sat);            // SPEC §9.22
    if (t.has & SetFileTrack::HasAccent) out.add("lp.track.%d.accent=%.3f\n", i, (double)t.accent);
    if (t.has & SetFileTrack::HasChorus) out.add("lp.track.%d.chorus=%.3f\n", i, (double)t.chorus);
    if (t.loopMode) out.add("lp.track.%d.loopmode=1\n", i);         // SPEC §9.19: the source is a streamed loop (lp.src.i its absolute path)
    if (file_.src[i][0]) out.add("lp.src.%d=%s\n", i, file_.src[i]);   // SPEC §9.18 / §9.29: where the pattern / vocal / loop comes from (absolute), "acid", or the SMF written beside this file
    if (session) {                                                  // SPEC §9.18: a session SMF stays an edit / the kept Fx loop after the restore
      if (t.edited) out.add("lp.track.%d.edited=1\n", i);
      if (t.loop) out.add("lp.track.%d.loop=1\n", i);
    }
    return;
  }
  if (part == kTracksPerSet + 2) { if (session) for (int k = 0; k < kSampleSlots; k++) if (file_.kit[k][0]) out.add("lp.kit.%d=%s\n", k, file_.kit[k]); return; }
  if (session) out.add("lp.end=1\n");                               // the terminator: a file without it was cut short (power-off, abort) and is skipped by the restore
}

// One step of the set-file write: the first call only opens the file (item_ = next part; one card operation per step,
// SPEC §9.2 / §9.19 §3 — an open and a write in one step would be two stalls under one ring lead), each later call
// packs whole parts into chunk_ and writes ≤ kSetChunk bytes. 1 = file complete and closed, 0 = more steps, -1 = error
// (files closed).
int Sets::stepWriteFile(const char* path) {
  if (!dst_) {
    dst_ = fs_->open(path, "w");
    if (!dst_) return -1;
    item_ = 0;
    return 0;
  }
  Sink out = {reinterpret_cast<char*>(chunk_), kSetChunk, 0, false};
  while (item_ < kSetParts) {
    const int mark = out.n;
    writeSetPart(item_, out);
    if (out.over) { out.n = mark; out.over = false; if (mark == 0) { closeFiles(); return -1; } break; }   // next chunk (a part never exceeds 4 KB)
    item_++;
  }
  if (out.n > 0 && dst_->write(chunk_, (size_t)out.n) != (size_t)out.n) { closeFiles(); return -1; }
  if (item_ < kSetParts) return 0;
  closeFiles();
  return 1;
}

// ---------------------------------------------------------------------------------------------- session (SPEC §9.18)

void Sets::sessionFilePath(char* out, int cap, int set, int slot) { snprintf(out, (size_t)cap, "%s/session_set%d%s.lpset", kSessionDir, set & 1, (slot & 1) ? "_b" : ""); }

void Sets::markSessionDirty() {
  sessionOn_ = 1; sessionDirty_ = 1;
  dirtyClock_ = eng_ ? eng_->status().sampleClock : 0;
}

void Sets::restoreSession() { sessionOn_ = 1; restorePending_ = 1; }

// Generator changes arm the debounce too (its jobs, variants, len / pause, key, genre, acid, knobs, transposes) once
// persistence is on; the restore's own changes are skipped. A write or the restore starts only while nothing else
// owns the card, and a write not while a loop slot is being consumed (SPEC §9.19 §3: its 139 ms ring cannot hide a
// 150 ms card open, so the write waits — debounced and spaced as usual — until the slot tracks or the transport stop;
// the Vocal track's loop rides the 186 ms vocal ring, which the gate protects, and does not hold the write back).
void Sets::pollSession() {
  if (!gen_ || !eng_ || !fs_) return;
  const uint32_t gc = gen_->changeCount();
  if (gc != genChanges_) { genChanges_ = gc; if (sessionOn_ && session_ != SsRestoring) markSessionDirty(); }
  if (state_ != Idle || gen_->busy()) return;
  if (restorePending_) { restorePending_ = 0; session_ = SsRestoring; sessionSet_ = 0; item_ = 0; listValid_ = 0; listDone_ = 0; state_ = RsProbe; return; }
  const uint32_t now = eng_->status().sampleClock;
  if (sessionDirty_ && (int32_t)(now - dirtyClock_) >= kSessionDebounceSamples && (!wrote_ || (int32_t)(now - lastWriteClock_) >= kSessionSpacingSamples)) {   // SPEC §9.19 §3 c
    if (gen_->loopSlotStreaming()) return;                          // deferred while a loop slot streams
    beginSessionWrite();
  }
}

void Sets::beginSessionWrite() {
  session_ = SsWriting; sessionDirty_ = 0; sessionSet_ = 0;
  startSessionSet();
}

// One set's session file: the header from the live state, then the tracks (SvTrack), the kit paths and the write.
void Sets::startSessionSet() {
  set_ = sessionSet_; track_ = 0; done_ = 0; slot_ = 0; phase_ = 0; attempts_ = 0; item_ = -1; count_ = 0; flags_ = 0; mask_ = 0; number_ = 0;
  listValid_ = 0; listDone_ = 0;                                    // SPEC §9.22: the writer's shard walks use names_
  file_.clear();
  const char* genre = gen_->genreName();
  file_.schemaVersion = 1; file_.setIndex = set_; file_.keyRoot = gen_->key().root % 12; file_.bpm = eng_->status().bpm; file_.volume = sessionVolume_;
  file_.seq = ++sessionSeq_;                                        // one up per file (a restarted write counts again: still monotonic)
  snprintf(file_.keyName, sizeof file_.keyName, "%s minor", noteName(file_.keyRoot));
  copyStr(file_.genre, sizeof file_.genre, genre ? genre : "");
  copyStr(file_.label, sizeof file_.label, loadedLabel_[set_]);     // SPEC §9.22: the restore brings the loaded preset's label back
  joinPath(dir_, sizeof dir_, "/lp/packs", file_.genre);            // resolveStep's last phase probes this genre's legacy presets/assets
  joinPath(dir_, sizeof dir_, dir_, "presets");
  state_ = SsMkdir;
}

// The user's card work wins, but a file already being written gets one more chunk (one bounded SD action, SPEC §9.2):
// when that chunk completes it the slot is whole and the previous slot may be overwritten next; otherwise the half
// file carries no lp.end and the restore keeps the other slot. Either way the write is re-armed.
void Sets::abortSession() {
  if (state_ == SsWrite && dst_) {
    if (stepWriteFile(path_) > 0) sessionSlot_ ^= (uint8_t)(1u << set_);
  }
  closeFiles();
  state_ = Idle; session_ = SsNone;
  sessionDirty_ = 1; dirtyClock_ = eng_ ? eng_->status().sampleClock : 0;   // re-armed: written once the card is free again
}

// The absolute source of a parsed track (SPEC §9.29): lp.src.i as written, or a legacy preset's clip (relative to the
// preset folder, forward slashes, no control characters, no drive letter, no ".." — the rule the format always had)
// joined onto dir_. False for a track without a source, an acid line, an unsafe clip, or a relative path in a session file.
bool Sets::sourceAbs(int track, char* out, int cap) {
  const char* src = file_.src[track];
  if (!src[0] || strcmp(src, "acid") == 0) return false;
  if (src[0] == '/') { copyStr(out, cap, src); return true; }
  if (session_ == SsRestoring || !dir_[0]) return false;
  char clip[kMaxPathLen]; copyStr(clip, sizeof clip, src);
  if (!sanitizeClip(clip)) return false;
  joinPath(out, cap, dir_, clip);
  return true;
}

// RsProbe: slot item_ of the set is opened for a light parse (lp.seq, lp.end only; one open, then 4 KB chunks in
// RsProbeRead); an absent slot counts as nothing. After both slots restoreChoose picks the file to rebuild from.
void Sets::restoreProbe() {
  if (item_ == 0) { set_ = sessionSet_; probeOk_ = 0; probeSeq_[0] = probeSeq_[1] = 0; }
  sessionFilePath(path_, sizeof path_, set_, item_);
  src_ = fs_->open(path_, "r");
  if (!src_) { item_++; restoreChoose(); return; }
  flags_ &= (uint16_t)~kFlagComplete;
  reader_.begin(sessionProbeFn, this);
  state_ = RsProbeRead;
}

// The newest complete slot (highest lp.seq among those ending in lp.end) is the one restored; the next write of this
// set goes to the other slot. No complete slot: nothing saved (or every copy cut short) — the set is skipped silently.
void Sets::restoreChoose() {
  if (item_ < 2) { state_ = RsProbe; return; }
  int pick = -1;
  for (int k = 0; k < 2; k++) if (((probeOk_ >> k) & 1) && (pick < 0 || (int32_t)(probeSeq_[k] - probeSeq_[pick]) > 0)) pick = k;
  if (pick < 0) { nextRestoreSet(); return; }
  if ((int32_t)(probeSeq_[pick] - sessionSeq_) > 0) sessionSeq_ = probeSeq_[pick];
  sessionSlot_ = (uint8_t)((sessionSlot_ & ~(1u << set_)) | ((uint32_t)(pick ^ 1) << set_));
  phase_ = (uint8_t)pick;
  state_ = RsOpen;
}

void Sets::sessionProbeFn(const char* key, const char* value, void* user) {
  Sets& s = *static_cast<Sets*>(user);
  if (strcmp(key, "lp.seq") == 0) s.probeSeq_[s.item_ & 1] = (uint32_t)strtoul(value, nullptr, 10);
  else if (strcmp(key, "lp.end") == 0) s.flags_ |= kFlagComplete;
}

// RsOpen: the chosen slot's file (one open). Parsing continues in LdParse.
void Sets::restoreOpen() {
  const int slot = phase_ & 1;
  set_ = sessionSet_; track_ = 0; done_ = 0; slot_ = 0; flags_ = 0; srcLoaded_ = 0; mask_ = 0; attempts_ = 0; idxLen_ = 0; item_ = 0; loopFallback_ = 0; wait_ = 0;
  ok_ = false; err_[0] = 0; dir_[0] = 0;
  rng_ = Rng(eng_->status().sampleClock ^ 0x5E55u);
  file_.clear();
  sessionFilePath(path_, sizeof path_, set_, slot);
  src_ = fs_->open(path_, "r");
  if (!src_) { nextRestoreSet(); return; }                          // vanished between the probe and now: ignored silently
  reader_.begin(parseFn, this);
  state_ = LdParse;
}

void Sets::nextRestoreSet() {
  closeFiles();
  if (++sessionSet_ < kSets) { item_ = 0; state_ = RsProbe; return; }
  session_ = SsNone; state_ = Idle;
  genChanges_ = gen_ ? gen_->changeCount() : 0;                     // the restore's own changes are not a reason to write
}

// Global state of a session file: the key (future generation), the genre pack, the master volume for the UI; the
// load-cycle masks the parse stored (parseFn) belong to the file's genre, which is the current one from here on.
void Sets::applySessionGlobals() {
  if (file_.keyRoot >= 0) gen_->setKeyRoot(file_.keyRoot);
  if (file_.genre[0]) gen_->setGenreByName(file_.genre);
  if (file_.volume >= 0) restoredVolume_ = file_.volume;
  loadedGenre_ = (uint8_t)gen_->genreIndex();                        // SPEC §9.25
}

// SPEC §9.17b: a load restores the knobs from the file right after the track's sound (so the patch's knob init is
// overridden) and the strip once the set has swapped (SPEC §9.8: the mix changes with the pattern, never a bar ahead
// of it); a file from before the knobs resets the track to its category defaults at the swap instead (the old rule).
namespace { void pushParam(Engine& e, int track, ParamId id, float v, int32_t i) { Cmd c; c.type = CmdSetParam; c.track = (uint8_t)track; c.a = (uint8_t)id; c.f = v; c.i = i; e.cmds().push(c); } }

void Sets::pushKnobParams(int track, const SetFileTrack& ft) {
  auto param = [&](uint16_t bit, ParamId id, float v, int32_t i) { if (ft.has & bit) pushParam(*eng_, track, id, v, i); };
  param(SetFileTrack::HasCutoff, PCutoff, ft.cutoff, 0); param(SetFileTrack::HasRes, PResonance, ft.res, 0); param(SetFileTrack::HasEnv, PEnvAmt, ft.env, 0);
  param(SetFileTrack::HasDecay, PDecay, ft.decay, 0); param(SetFileTrack::HasDrive, PDrive, ft.drive, 0); param(SetFileTrack::HasWave, PWave, (float)ft.wave, ft.wave);
  param(SetFileTrack::HasSat, PSat, ft.sat, 0); param(SetFileTrack::HasAccent, PAccent, ft.accent, 0); param(SetFileTrack::HasChorus, PChorus, ft.chorus, 0);   // SPEC §9.22
}

void Sets::pushStripParams(int track, const SetFileTrack& ft) {
  if (!ft.has) { Cmd c; c.type = CmdResetTrack; c.track = (uint8_t)track; eng_->cmds().push(c); return; }
  auto param = [&](uint16_t bit, ParamId id, float v) { if (ft.has & bit) pushParam(*eng_, track, id, v, 0); };
  param(SetFileTrack::HasVol, PVolume, ft.vol); param(SetFileTrack::HasDist, PDist, ft.dist); param(SetFileTrack::HasFilter, PFilter, ft.filter);
  param(SetFileTrack::HasEcho, PEcho, ft.echo); param(SetFileTrack::HasReverb, PReverb, ft.reverb);
}

// The eight strips (≤ 40 commands) in one go, only when the queue holds them all plus the CmdSetGenerating that follows.
bool Sets::pushAllStrips() {
  if (eng_->cmds().size() + 5 * kTracksPerSet + 1 > kCmdQueueSize - 1) return false;
  for (int i = 0; i < kTracksPerSet; i++) pushStripParams(set_ * kTracksPerSet + i, file_.track[i]);
  return true;
}

// SPEC §9.19 §1.7: a loop track whose saved file is gone picks a loop of its category once the set has swapped. The
// library is warmed here one bounded op per step (Library::prepareLoop) so the generator's synchronous check costs
// the key handler's budget nothing, then setLoopMode(on) queues the pick (the generator is idle: a load / restore
// refused to start while it was busy, and nothing of ours runs any more).
void Sets::loopFallbackStep() {
  for (int i = 0; i < kTracksPerSet; i++) {
    if (!((loopFallback_ >> i) & 1)) continue;
    const int t = set_ * kTracksPerSet + i;
    if (lib_ && !lib_->prepareLoop(gen_->genreName(), trackCategory(t), gen_->key())) return;   // one op this step
    loopFallback_ &= (uint8_t)~(1u << i);
    gen_->setLoopMode(t, true);
    return;
  }
  if (session_ == SsRestoring) nextRestoreSet(); else state_ = Idle;
}

void Sets::finishLoadDone() {
  if (loopFallback_) { state_ = LdLoopFallback; return; }
  if (session_ == SsRestoring) nextRestoreSet(); else state_ = Idle;
}

// ---------------------------------------------------------------------------------------------- load

bool Sets::beginLoad(int set, Rng& rng) {
  if (session_ == SsWriting) abortSession();
  if (busy()) { copyStr(err_, sizeof err_, "BUSY"); return false; }
  if (!fs_ || !eng_ || !gen_ || set < 0 || set >= kSets) { copyStr(err_, sizeof err_, "NO FS"); return false; }
  if (gen_->busy()) { copyStr(err_, sizeof err_, "GEN BUSY"); return false; }   // queued requests included (SPEC §9.1, §9.5)
  const char* genre = gen_->genreName();
  if (!genre || !genre[0]) { copyStr(err_, sizeof err_, "NO GENRE"); return false; }
  rng_ = Rng(rng.next());
  syncLoadedGenre();                                                // SPEC §9.25: the cycle masks of this genre
  set_ = set; track_ = 0; done_ = 0; page_ = 0; item_ = 0; count_ = 0; matches_ = 0; skipped_ = 0; slot_ = 0; flags_ = 0; srcLoaded_ = 0; idxLen_ = 0; mask_ = 0; attempts_ = 0; loopFallback_ = 0; wait_ = 0;
  listValid_ = 0; listDone_ = 0;                                    // SPEC §9.22: the loader's pages and shard walks use names_
  wantRoot_ = gen_->key().root % 12;
  ok_ = false; err_[0] = 0; name_[0] = 0; chosen_[0] = 0; fallback_[0] = 0;
  file_.clear();
  joinPath(dir_, sizeof dir_, "/lp/packs", genre);
  joinPath(dir_, sizeof dir_, dir_, "presets");
  state_ = LdOpen;
  return true;
}

void Sets::stepLoad() {
  switch (state_) {
    case LdOpen:                                                    // index present? (one probe)
      indexPath(path_, sizeof path_);
      page_ = 0; item_ = 0; count_ = 0; idxLen_ = 0;
      if (fs_->exists(path_)) { flags_ |= kFlagIndex; state_ = LdIndexRead; } else state_ = LdScan;
      break;
    case LdIndexRead:                                               // one 4 KB chunk of names per step, filtered by name (SPEC §9.14)
      if (!(flags_ & kFlagChunk)) {
        if (((flags_ & kFlagEof) && item_ >= count_) || !readIndexChunk()) { indexDone(); break; }
      }
      scanIndex();
      if (state_ != LdIndexRead) break;                             // a probe took over
      if (flags_ & kFlagEof) { indexDone(); break; }
      page_++; item_ = 0; flags_ &= (uint16_t)~kFlagChunk;
      break;
    case LdScan: {                                                  // no index: one directory page per step
      PageScan ps = {this, page_ * kSetPage, 0, 0, 0, true};
      fs_->walk(dir_, pageFn, &ps);
      count_ = ps.stored; item_ = 0;
      flags_ = ps.taken < kSetPage ? (uint16_t)(flags_ | kFlagLastPage) : (uint16_t)(flags_ & ~kFlagLastPage);
      state_ = LdPage;
      break; }
    case LdPage:                                                    // the page's names, pure until one needs a probe
      while (item_ < count_) {
        if (considerName(names_[item_])) { copyStr(idxLine_, sizeof idxLine_, names_[item_]); state_ = LdProbe; break; }
        item_++;
      }
      if (state_ != LdPage) break;
      if (flags_ & kFlagLastPage) state_ = LdChoose; else { page_++; state_ = LdScan; }
      break;
    case LdProbe:                                                   // one file open
      probeBegin(idxLine_);
      break;
    case LdProbeRead: {                                             // 4 KB per step until keyRoot is known
      size_t n = src_->read(chunk_, kSetChunk);
      flags_ &= (uint16_t)~kFlagChunk;                              // chunk_ no longer holds the index chunk
      reader_.feed(reinterpret_cast<const char*>(chunk_), (int)n);
      if (number_ != kProbeNone || n < (size_t)kSetChunk) {
        reader_.finish(); closeFiles();
        if (number_ == wantRoot_) offer(idxLine_);                  // keyRoot only; setIndex is informational (SPEC §9.8)
        resumeListing();
      }
      break; }
    case LdChoose: chooseFile(); break;
    case LdParse: {
      size_t n = src_->read(chunk_, kSetChunk);
      reader_.feed(reinterpret_cast<const char*>(chunk_), (int)n);
      if (n < (size_t)kSetChunk) {
        reader_.finish(); closeFiles();
        if (file_.schemaVersion != 1 || (session_ == SsRestoring && !file_.complete)) {   // set_presets.lua:78; a session file must end in lp.end
          if (session_ == SsRestoring) { nextRestoreSet(); return; }   // a malformed or truncated session file is skipped silently (SPEC §9.18)
          fail("BAD SET"); return;
        }
        Cmd c; c.type = CmdSetGenerating; c.a = (uint8_t)set_; c.i = 1; eng_->cmds().push(c);
        flags_ |= kFlagGenerating;
        if (session_ == SsRestoring) applySessionGlobals(); else copyStr(name_, sizeof name_, chosen_);
        copyStr(loadedLabel_[set_], sizeof loadedLabel_[set_], file_.label);   // SPEC §9.22: the set carries this preset's label into its later saves
        track_ = 0; done_ = 0; srcLoaded_ = 0; mask_ = 0; state_ = LdTrack;
      }
      break; }
    case LdTrack:
      if (track_ >= kTracksPerSet) { state_ = LdVocal; break; }
      if (track_ == Vocal) { track_++; break; }                     // streamed last, with the swap burst (see loadVocal)
      loadTrack();
      break;
    case LdSrcFind: srcFind(); break;
    case LdSlotFind: slotFind(); break;
    case LdSlotOpen: slotOpen(); break;
    case LdSlotLoad: {                                              // ≤ 4 KB of WAV per step into staging (SPEC §9.1/§9.2)
      if (loader_.step(kSetChunk)) break;
      closeFiles();
      uint32_t len = loader_.written();
      ackWait_ = eng_->status().slotAck;
      if (len == 0) { state_ = LdSlotAck; flags_ |= kFlagSlotSkip; break; }   // nothing decoded: skip
      Cmd c; c.type = CmdSlotSwap; c.a = (uint8_t)set_; c.i = (int32_t)(((uint32_t)slot_) | (len << 4)); c.f = 0; eng_->cmds().push(c);
      state_ = LdSlotAck;
      break; }
    case LdSlotAck: {
      const bool skipped = (flags_ & kFlagSlotSkip) != 0;
      if (!skipped && eng_->status().slotAck == ackWait_) break;    // the engine copies staging at its next block
      flags_ &= (uint16_t)~kFlagSlotSkip;
      const int t = set_ * kTracksPerSet + track_;
      if (!skipped) {
        eng_->setKitName(set_, slot_, baseName(path_));
        gen_->noteKitPath(set_, slot_, path_);                      // the session writer composes the kit paths from this (SPEC §9.19 §3 b)
        if (slot_ == 0 || slot_ == 4) copyStr(gen_->info(t).sound, sizeof gen_->info(t).sound, baseName(path_));   // display (SPEC §4)
      }
      slot_++; attempts_ = 0;
      if (slot_ >= (trackCategory(t) == Beat ? 4 : kSampleSlots)) { track_++; done_++; state_ = LdTrack; }
      else state_ = LdSlotFind;
      break; }
    case LdVocal: loadVocal(); break;
    case LdWaitSwap: {                                              // one burst, then the bar line (SPEC §9.5); stopped transport: done now
      const EngineStatus& st = eng_->status();
      const int beat = set_ * kTracksPerSet + Beat;
      if ((flags_ & kFlagRushDue) && !st.track[beat].rush) { snareRush(eng_->nextPattern(beat), eng_->rushPattern(set_)); flags_ &= (uint16_t)~kFlagRushDue; }
      if (!(flags_ & kFlagPushed)) {
        if (gen_->setSwapPending(set_)) {
          if (gen_->setSwapWaiting(set_) && !st.running) gen_->requestSetSwap(set_, 0);   // transport stopped: the pump sleeps, push now
          if (gen_->setSwapPending(set_)) return;                   // the pump pushes the burst with the vocal's commands (or once the queue has room)
        }
        flags_ |= kFlagPushed; pushClock_ = st.sampleClock;
        return;
      }
      if (session_ == SsRestoring) {                                // boot: nothing plays, the swaps apply on the next bar line unheard
        if (!pushAllStrips()) return;                               // the strips now (nothing is heard), with the queue room for them
        Cmd c; c.type = CmdSetGenerating; c.a = (uint8_t)set_; c.i = 0; eng_->cmds().push(c);
        flags_ &= (uint16_t)~kFlagGenerating;
        ok_ = true; finishLoadDone();
        break;
      }
      if ((int32_t)(st.sampleClock - pushClock_) < 2 * kBlock) return;   // commands not yet seen by the engine
      if (st.running) for (int i = 0; i < kTracksPerSet; i++) if (st.track[set_ * kTracksPerSet + i].pendingSwap) return;
      // SPEC §9.8 / §9.17b: the strip of every track follows the swap (the knobs went out with CmdSetPatch); a file
      // without the §9.17b keys gets CmdResetTrack here, as the effects reset of old. Stopped transport: nothing plays.
      if (!pushAllStrips()) return;                                 // queue full: next step
      Cmd c; c.type = CmdSetGenerating; c.a = (uint8_t)set_; c.i = 0; eng_->cmds().push(c);
      flags_ &= (uint16_t)~kFlagGenerating;
      ok_ = true; finishLoadDone();
      break; }
    case LdLoopFallback: loopFallbackStep(); break;
    case RsOpen: restoreOpen(); break;
    case RsProbe: restoreProbe(); break;
    case RsProbeRead: {                                             // 4 KB per step: lp.seq and lp.end of the slot
      size_t n = src_->read(chunk_, kSetChunk);
      reader_.feed(reinterpret_cast<const char*>(chunk_), (int)n);
      if (n < (size_t)kSetChunk) {
        reader_.finish(); closeFiles();
        if (flags_ & kFlagComplete) probeOk_ |= (uint8_t)(1u << (item_ & 1));
        item_++;
        restoreChoose();
      }
      break; }
    default: state_ = Idle; break;
  }
}

// Back to the listing after a probe: the index chunk is re-read (the probe used chunk_), a page continues.
void Sets::resumeListing() {
  if (flags_ & kFlagIndex) state_ = LdIndexRead;
  else { item_++; state_ = LdPage; }
}

void Sets::probeBegin(const char* name) {
  joinPath(path_, sizeof path_, dir_, name);
  src_ = fs_->open(path_, "r");
  if (!src_) { resumeListing(); return; }
  number_ = kProbeNone;
  reader_.begin(probeFn, this);
  state_ = LdProbeRead;
}

void Sets::probeFn(const char* key, const char* value, void* user) {
  Sets& s = *static_cast<Sets*>(user);
  if (strcmp(key, "keyRoot") == 0) { int r = (int)strtol(value, nullptr, 10); s.number_ = ((r % 12) + 12) % 12; }
}

namespace {
// lp.loaded.<set>=<hex> (SPEC §9.25): 256 hex chars, byte i = presets 8i..8i+7 (bit n & 7 = preset n). A shorter value
// leaves the rest clear; the value ends at the first character that is not a hex digit.
int hexDigit(char c) { return isDigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }
void loadedFromHex(const char* hex, uint8_t* mask, int bytes) {
  memset(mask, 0, (size_t)bytes);
  for (int i = 0; i < bytes; i++) {
    const int hi = hexDigit(hex[2 * i]); if (hi < 0) return;
    const int lo = hexDigit(hex[2 * i + 1]); if (lo < 0) return;
    mask[i] = (uint8_t)((hi << 4) | lo);
  }
}
}

// Every key of the file goes to the SetFile (applySetProperty); lp.loaded.<set> (SPEC §9.25) goes straight into the set's
// load-cycle mask instead — only during a restore (a preset file never carries it; a stray line is ignored), and the
// genre these bits belong to follows in applySessionGlobals.
void Sets::parseFn(const char* key, const char* value, void* user) {
  Sets& s = *static_cast<Sets*>(user);
  if (strncmp(key, "lp.loaded.", 10) == 0) {
    if (s.session_ == SsRestoring && isDigit(key[10]) && key[11] == 0 && key[10] - '0' < kSets) loadedFromHex(value, s.loaded_[key[10] - '0'], kLoadedPresets / 8);
    return;
  }
  applySetProperty(s.file_, key, value);
}

// Uniform pick among the candidates, never the last loaded name unless it is the only match (SPEC §9.8); when every
// other match was loaded in this cycle (skipped_, nothing chosen), the listing runs once more with kFlagNewCycle — every
// match's bit is cleared as it is offered (the new cycle of SPEC §9.25) and the draw is made among them; no match →
// "NO SET IN <Key>" and nothing has been touched.
void Sets::chooseFile() {
  if (!chosen_[0] && skipped_ > 0 && !(flags_ & kFlagNewCycle)) {
    flags_ = kFlagNewCycle; skipped_ = 0; matches_ = 0; fallback_[0] = 0; page_ = 0; item_ = 0; count_ = 0; idxLen_ = 0;
    state_ = LdOpen;                                                // the second pass: index chunks or directory pages as before (one bounded operation per step)
    return;
  }
  if (!chosen_[0]) {
    if (!fallback_[0]) {
      char minorLabel[8], majorLabel[8]; gen_->key().labels(minorLabel, majorLabel);
      char msg[32]; snprintf(msg, sizeof msg, "NO SET IN %s", minorLabel);
      fail(msg); return;
    }
    copyStr(chosen_, sizeof chosen_, fallback_);
  }
  joinPath(path_, sizeof path_, dir_, chosen_);
  src_ = fs_->open(path_, "r");
  if (!src_) {
    if (healTmp(path_)) return;                                     // only its <name>.tmp was left (a rename cut short): renamed back, opened next step
    if (flags_ & kFlagIndex) { indexPath(path_, sizeof path_); fs_->remove(path_); }   // deleted by hand but still indexed: the index is stale — the next load walks, the next save rebuilds it
    fail("OPEN SET"); return;
  }
  file_.clear();
  reader_.begin(parseFn, this);
  state_ = LdParse;
}

// source + stored seed/key → next (SPEC §9.5) exactly like Generator::buildNext, but the swap is NOT pushed:
// the whole set goes out in one burst from finishLoad. Beat builds Full and refreshes the rush loop (SPEC
// §9.6; the mask travels with the burst); Percussion applies the fold map loadMidiToSource stored (SPEC §9.7).
bool Sets::buildNext(int track, int variant) {
  const Category cat = trackCategory(track); const int set = trackSet(track); TrackGenInfo& info = gen_->info(track);
  variant &= 3;
  if (!eng_->beginNextWrite(track)) return false;
  Pattern& out = eng_->nextPattern(track);
  ProjectKey k; k.root = (uint8_t)(info.keyRoot % 12);
  buildVariant(cat, eng_->sourcePattern(track), cat == Beat ? 3 : variant, info.seed, k, out, cat == Percussion ? gen_->foldMap(set) : nullptr);
  eng_->endNextWrite(track);
  info.naturalTicks = out.lengthTicks; info.variant = (uint8_t)variant; info.lenStep = 0; info.loaded = 1;
  if (cat == Beat) {
    if (eng_->status().track[track].rush) flags_ |= kFlagRushDue;   // rush held: the loop is refreshed once released
    else snareRush(out, eng_->rushPattern(set));
  }
  mask_ |= (uint8_t)(1u << (track % kTracksPerSet));
  return true;
}

// One MIDI track per step (SPEC §9.14): sound, clip → source (Generator::loadMidiToSource), then the variant
// from the restored seed, key, variant and length step into the next buffer (no swap yet, SPEC §9.5) so a
// loaded track behaves exactly like a generated one. Beat and Percussion then continue with their kit slots.
// A library pattern (/lp/midi/…) whose path fails to open is looked for again by its basename under its key folder
// and that folder's shards (LdSrcFind / srcFind, one bounded card op per step: the card was re-imported and re-sharded
// since the save, SPEC §9.29) and opened on a later step (srcLoaded_ 4); a source still missing, or unsafe, leaves
// the track as it is (srcLoaded_ 2) and the load goes on. A refused next-buffer write (lock held by the engine
// mid-copy) is retried on later steps.
void Sets::loadTrack() {
  const int t = set_ * kTracksPerSet + track_;
  const Category cat = trackCategory(t);
  SetFileTrack& ft = file_.track[track_];
  TrackGenInfo& info = gen_->info(t);
  const int v = ft.variant & 3;
  if (!srcLoaded_) {
    attempts_ = 0;
    if (ft.hasPreset) ft.preset = presetForLayout(cat, ft.preset, file_.banks);   // a pre-wave file's '303' index onto the moved row (once per track: srcLoaded_ guards the re-entry)
    if (!ft.loopMode && gen_->loopEnabled(t)) gen_->setLoopMode(t, false);   // the file describes the track: a plain clip ends loop mode (CmdLoopMode off, no card work)
    if (ft.loopMode) {                                              // SPEC §9.19 §1.7: loop mode with that exact file; a missing one → a pick of the category after the swap
      if (!gen_->streamOpenAllowed() && wait_ < kHandleWaitSteps) { wait_++; return; }   // SPEC §9.19g: the loop's open needs a handle; the same track next step, nothing pushed yet
      wait_ = 0;
      if (isSynthCategory(cat) && ft.hasPreset) {                    // the parked sound still travels
        SynthPatch& p = eng_->nextPatch(t);
        makePatch(cat, ft.preset, ft.seed, p);
        Cmd c; c.type = CmdSetPatch; c.track = (uint8_t)t; eng_->cmds().push(c);
        info.presetIndex = ft.preset; copyStr(info.sound, sizeof info.sound, p.name);
      }
      if (isSynthCategory(cat) && (ft.has & SetFileTrack::HasAcid)) info.acid = ft.acid;
      if (isSynthCategory(cat)) pushKnobParams(t, ft);
      const bool ok = sourceAbs(track_, path_, sizeof path_) && gen_->loadLoop(t, path_, v, ft.lenk);
      if (!ok) loopFallback_ |= (uint8_t)(1u << track_);
      info.keyRoot = (uint8_t)(ft.hasKey ? ft.keyRoot : (file_.keyRoot >= 0 ? file_.keyRoot : gen_->key().root));
      srcLoaded_ = 2;                                               // the loop's carrier swap goes with its stream switch (the burst waits for it): nothing to publish here
    }
  }
  if (!srcLoaded_) {
    if (isSynthCategory(cat) && ft.hasPreset) {                     // sound: preset + seed spread (SPEC §5)
      SynthPatch& p = eng_->nextPatch(t);
      makePatch(cat, ft.preset, ft.seed, p);
      Cmd c; c.type = CmdSetPatch; c.track = (uint8_t)t; eng_->cmds().push(c);
      info.presetIndex = ft.preset; copyStr(info.sound, sizeof info.sound, p.name);
    }
    if (isSynthCategory(cat) && (ft.has & SetFileTrack::HasAcid)) info.acid = ft.acid;   // SPEC §9.17b: the flag only, the sound is the file's preset
    if (isSynthCategory(cat)) pushKnobParams(t, ft);                // after the sound: the knobs from the file override the patch's init (SPEC §9.17b); the strip waits for the swap
    const uint8_t keyRoot = (uint8_t)(ft.hasKey ? ft.keyRoot : (file_.keyRoot >= 0 ? file_.keyRoot : gen_->key().root));
    bool usable = false;                                            // SPEC §9.18 / §9.29: the acid line from its seed, or one file from its absolute path (a legacy clip joined onto the preset folder)
    if (strcmp(file_.src[track_], "acid") == 0 && Generator::acidCategory(cat)) { gen_->buildAcidSource(t, ft.hasSeed ? ft.seed : rng_.next(), keyRoot); usable = true; }
    else if (sourceAbs(track_, path_, sizeof path_)) {
      usable = gen_->loadMidiToSource(t, path_);                     // one file
      if (!usable && strncmp(path_, "/lp/midi/", 9) == 0) {          // SPEC §9.29: the library file is not where the preset says (a re-imported, re-sharded card): its basename is looked for under its key folder and shards, one op per step
        srcLoaded_ = 3; attempts_ = 0; item_ = -1; state_ = LdSrcFind;
        return;
      }
    }
    if (usable) adoptSource(t, ft, v);
    srcLoaded_ = usable ? 1 : 2;                                    // 2 = nothing to publish
  } else if (srcLoaded_ == 4) {                                     // srcFind found the pattern under another shard: one open (the sound and knobs went out before the search)
    const bool usable = gen_->loadMidiToSource(t, path_);
    if (usable) adoptSource(t, ft, v);
    srcLoaded_ = usable ? 1 : 2;
  }
  if (srcLoaded_ == 1) {
    if (!buildNext(t, v)) { if (++attempts_ < kPublishRetries) return; }   // lock held: retry next step
    else info.lenStep = (int8_t)clampLenStep(info.naturalTicks, ft.lenk);   // len = natural × 2^k travels with the swap (SPEC §9.4)
  }
  srcLoaded_ = 0; attempts_ = 0;
  if (cat == Beat) { slot_ = 0; state_ = LdSlotFind; }
  else if (cat == Percussion) { slot_ = 4; state_ = LdSlotFind; }
  else { track_++; done_++; }
}

// The file just loaded is the track's source now (SPEC §9.8): what the variant build depends on comes from the file's
// lines — seed, key, variant, len step 0 — and the edited / loop flags: a pattern from the preset's own _t<i>.mid is this
// device's edit (the next save writes its own copy; the library and a legacy asset are referenced, SPEC §9.29), a
// session SMF stays an edit / the kept Fx loop.
void Sets::adoptSource(int t, const SetFileTrack& ft, int v) {
  TrackGenInfo& info = gen_->info(t);
  info.seed = ft.hasSeed ? ft.seed : rng_.next();
  info.keyRoot = (uint8_t)(ft.hasKey ? ft.keyRoot : (file_.keyRoot >= 0 ? file_.keyRoot : gen_->key().root));
  info.variant = (uint8_t)v; info.userLoop = 0; info.edited = 0; info.lenStep = 0;
  if (!info.acidPat && ownedSource(path_)) info.edited = 1;
  if (session_ == SsRestoring && !info.acidPat) { info.edited = ft.edited; info.userLoop = ft.loop; }
  info.sourceTicks = eng_->sourcePattern(t).lengthTicks;
}

// LdSrcFind (SPEC §9.29): a library pattern the file names by an absolute path that failed to open — the card was
// re-imported or re-sharded since the save (tools/shard_folders.py moves files between sNN folders) — is looked for
// by its basename the way the writer's resolveStep does: the ≤ 2 key folders of /lp/midi/<Category>/ flat (one step,
// ≤ 2 probes), then each key folder's shard subfolders (one kidFn walk into names_, then ≤ 3 probes per step).
// attempts_ walks the stages (0: the flat folders, 1..n: the shards of key folder attempts_ - 1). Found → path_ = the
// file, which loadTrack opens next step (srcLoaded_ 4); exhausted → the track keeps what it had (srcLoaded_ 2) and
// the load goes on. No buffer of its own: path_, names_ and the loader's counters (the kit slot search uses the same).
void Sets::srcFind() {
  const int t = set_ * kTracksPerSet + track_;
  const char* file = baseName(file_.src[track_]);
  char base[kMaxPathLen]; joinPath(base, sizeof base, "/lp/midi", categoryName(trackCategory(t)));
  char folder[2][16]; const int n = keyFolders(file, folder);
  bool found = false;
  for (;;) {
    const int a = attempts_;
    if (a == 0) {                                                   // the key folders themselves: ≤ 2 probes
      attempts_++; item_ = -1;
      for (int i = 0; i < n && !found; i++) found = resolveProbe(base, folder[i], nullptr, file);
      break;
    }
    if (a <= n) {                                                   // the shards of key folder a - 1: one walk, then ≤ 3 probes per step
      const char* kf = folder[a - 1];
      if (item_ < 0) {
        char dirp[kMaxPathLen]; joinPath(dirp, sizeof dirp, base, kf);
        KidScan ks = {this, 0, 0};
        fs_->walk(dirp, kidFn, &ks);
        count_ = ks.n; item_ = 0;
        if (count_ == 0) { attempts_++; item_ = -1; }               // flat (or absent): the next key folder next step
        break;
      }
      for (int p = 0; p < 3 && item_ < count_ && !found; p++, item_++) found = resolveProbe(base, kf, names_[item_], file);
      if (!found && item_ >= count_) { attempts_++; item_ = -1; }
      break;
    }
    attempts_ = 0; break;                                           // exhausted
  }
  if (found) { srcLoaded_ = 4; attempts_ = 0; item_ = 0; state_ = LdTrack; return; }   // path_ = the file: opened next step
  if (attempts_) return;                                            // more candidates next step
  srcLoaded_ = 2; state_ = LdTrack;                                 // still gone: the track keeps its pattern
}

// Kit slot: one SD action per step (SPEC §9.2) over the candidates — the stored absolute path, the bare name
// under this genre's role folders, the bare name under each role folder's shards (one kidFn walk of the role
// folder into names_, then <= 3 exists() probes per step: a card re-sharded since the save moved the file to
// another sNN, which is what resolveKitPaths probes too), then a random pick of the same role when the file is
// gone (SPEC §9.14). attempts_ walks the stages; a slot without a stored path keeps its current one-shot. The
// fallback pick is warmed first (Library::prepareSample, one index chunk or folder walk per step): a cold role
// folder is a tree of shards whose indexes would otherwise all be streamed in one step.
void Sets::slotFind() {
  const char* stored = file_.kit[slot_];
  bool found = false;
  if (stored[0]) {
    const char* base = baseName(stored);
    auto rolePath = [&](int r) {
      joinPath(path_, sizeof path_, "/lp/packs", gen_->genreName());
      joinPath(path_, sizeof path_, path_, "drum");
      joinPath(path_, sizeof path_, path_, kSlotRoles[slot_][r]);
    };
    for (;;) {
      const int a = attempts_;
      if (a == 0) { attempts_++; item_ = -1; if (stored[0] != '/') continue; copyStr(path_, sizeof path_, stored); found = fs_->exists(path_); break; }
      if (a <= 3) {                                                 // the role folders themselves: one probe per step
        attempts_++; item_ = -1;
        if (!kSlotRoles[slot_][a - 1]) continue;
        rolePath(a - 1); joinPath(path_, sizeof path_, path_, base);
        found = fs_->exists(path_); break;
      }
      if (a <= 6) {                                                 // the shards of role folder a - 4: one walk, then <= 3 probes per step
        const int r = a - 4;
        if (!kSlotRoles[slot_][r]) { attempts_++; item_ = -1; continue; }
        if (item_ < 0) {
          rolePath(r);
          KidScan ks = {this, 0, 0};
          fs_->walk(path_, kidFn, &ks);
          count_ = ks.n; item_ = 0;
          if (count_ == 0) { attempts_++; item_ = -1; }               // flat (or absent): next stage next step
          break;
        }
        for (int p = 0; p < 3 && item_ < count_ && !found; p++, item_++) {
          rolePath(r); joinPath(path_, sizeof path_, path_, names_[item_]); joinPath(path_, sizeof path_, path_, base);
          found = fs_->exists(path_);
        }
        if (!found && item_ >= count_) { attempts_++; item_ = -1; }
        break;
      }
      if (a <= 9) {                                                 // a random file of the same role (prepared, then picked)
        const char* role = kSlotRoles[slot_][a - 7]; if (!role || !lib_) { attempts_++; continue; }
        const int keyRoot = slot_ == 0 ? (int)gen_->key().root : -1;
        if (!(flags_ & kFlagWarm)) {                                // warm-up steps first; the pick follows in a later step
          if (lib_->prepareSample(gen_->genreName(), role, keyRoot)) flags_ |= kFlagWarm;
          return;
        }
        flags_ &= (uint16_t)~kFlagWarm;
        attempts_++;
        found = lib_->pickSample(gen_->genreName(), role, keyRoot, rng_, path_, sizeof path_);
        break;
      }
      attempts_ = 0; break;                                         // exhausted: skip the slot
    }
    if (!found && attempts_) return;                                // more candidates next step
  }
  if (found) { attempts_ = 0; item_ = 0; state_ = LdSlotOpen; return; }
  flags_ |= kFlagSlotSkip; ackWait_ = eng_->status().slotAck; state_ = LdSlotAck;   // slot skipped: keep the current one-shot
}

// One file open + the WAV header per step; the µ-law decode follows in 4 KB steps (SPEC §9.1).
void Sets::slotOpen() {
  src_ = fs_->open(path_, "r");
  if (src_ && wavReadHeader(*src_, wav_) && loader_.begin(*src_, wav_, eng_->staging(), slotCapSamples(slot_), 0.0f)) { state_ = LdSlotLoad; return; }
  closeFiles();
  flags_ |= kFlagSlotSkip; ackWait_ = eng_->status().slotAck; state_ = LdSlotAck;
}

// The vocal comes last so its switch and the seven other swaps leave in one burst (SPEC §9.5, §9.3): the
// saved phrase is opened in the slot its variant uses (1 = alternate file), the other slot points at the same
// file, the pause (SPEC §9.16, default 1 when the file has none) and the variant are restored, then finishLoad
// hands the burst to the generator in the same step (before any pump could push the vocal's commands alone).
// One file open.
void Sets::loadVocal() {
  const int t = set_ * kTracksPerSet + Vocal;
  SetFileTrack& ft = file_.track[Vocal];
  TrackGenInfo& info = gen_->info(t);
  const int v = ft.variant & 3;
  const int sel = v == 1 ? 1 : 0;                                   // the Vocal's strip (its echo send) follows the swap like every track's (LdWaitSwap)
  bool usable;
  if (!gen_->streamOpenAllowed() && wait_ < kHandleWaitSteps) { wait_++; return; }   // SPEC §9.19g: the phrase / loop open needs a handle; again next step
  wait_ = 0;
  if (ft.loopMode) {                                                // SPEC §9.19: the Vocal track's loop through its own stream; a missing file → a pick after the swap
    const bool ok = sourceAbs(Vocal, path_, sizeof path_) && gen_->loadLoop(t, path_, v, ft.lenk);
    if (!ok) loopFallback_ |= (uint8_t)(1u << Vocal);
    info.keyRoot = (uint8_t)(ft.hasKey ? ft.keyRoot : (file_.keyRoot >= 0 ? file_.keyRoot : gen_->key().root));
    done_++;
    finishLoad();
    return;
  }
  usable = sourceAbs(Vocal, path_, sizeof path_) && gen_->loadVocal(set_, path_, sel);   // SPEC §9.18 / §9.29: the phrase's absolute path (a legacy clip joined onto the preset folder)
  if (usable) {
    gen_->setVocalPath(set_, sel ^ 1, path_);                        // variants 1 and 2 both play the saved phrase (no alternate in a set)
    info.seed = ft.hasSeed ? ft.seed : rng_.next();
    info.keyRoot = (uint8_t)(ft.hasKey ? ft.keyRoot : (file_.keyRoot >= 0 ? file_.keyRoot : gen_->key().root));
    info.userLoop = 0; info.edited = 0; info.lenStep = 0;
    info.pause = Generator::fitPause(ft.hasPause ? ft.pause : kVocalPauseDefault, info.windowBars);   // within the engine's cap under this file's window
    gen_->rebuildVariant(t, v);                                     // variant + cycle on the opened stream (pure, switches with the burst)
  }
  done_++;
  finishLoad();
}

// Everything is in the next buffers: kick cut, the one set-wide burst (SPEC §9.5; deferred by the generator to
// the vocal's switch bar while it streams), tempo only when nothing of either set plays (SPEC §9.8); the
// effects reset and "generating" clear follow in LdWaitSwap once the bar has passed.
void Sets::finishLoad() {
  if (mask_ & (1u << Beat)) { Cmd k; k.type = CmdKickCut; k.a = (uint8_t)set_; k.i = file_.track[Beat].beatKickMuted ? 1 : 0; eng_->cmds().push(k); }
  if (file_.bpm >= kMinBpm && file_.bpm <= kMaxBpm && !(session_ == SsRestoring && configBpm_ > 0)) {   // SPEC §9.19f: config.txt bpm= wins over a session's tempo
    bool playing = false;
    for (int t = 0; t < kTracks; t++) { PlayState s = eng_->status().track[t].state; if (s == PSPlaying || s == PSQueued || s == PSStopQueued) playing = true; }
    if (!playing) { Cmd b; b.type = CmdSetTempo; b.i = file_.bpm; eng_->cmds().push(b); }
  }
  if (session_ == SsNone) {
    copyStr(lastName_[set_], sizeof lastName_[set_], name_);
    const int bit = presetBit(name_); if (bit >= 0) setLoadedBit(set_, bit, true);   // SPEC §9.25: loaded in this cycle (a random or a named load; a restore keeps the file's masks)
  }
  if (gen_->requestSetSwap(set_, mask_)) { flags_ |= kFlagPushed; pushClock_ = eng_->status().sampleClock; }
  mask_ = 0;
  state_ = LdWaitSwap;
}

// ---------------------------------------------------------------------------------------------- preset manager (SPEC §9.22)
//
// Listing. The rows are the k = 16 names of rank [first, first + 16) in the order "highest NNN first, ties by name, the
// names without an NNN last" among the genre's presets that pass the key filter (the key read from the name as the loader
// does; a name without a key label is row keyRoot -1 and matches only the "all" filter — no file is opened for it). One
// PASS streams every name once (the index in 4 KB chunks, or directory pages of kSetPage without an index) and keeps the
// best rows_[rowBase_ .. 16) that lie strictly below a THRESHOLD row in that order (none on the first pass); an aligned
// page p = first / 16 therefore takes p + 1 passes, each one's last row the next threshold, and an unaligned first keeps
// rows first % 16 .. of its page and fills the rest with one pass more. Paging on from the page listed before is one
// pass (the threshold is its last row). Then PlLabel opens each row's file for lp.label, one open or one 4 KB read per
// step. Memory: the rows live in the names_ union (16 × 85 B of its 2 KB); the walk fallback feeds considerRow straight
// from the callback, so names_ is never written while rows_ is live.
//
// Rewrites. deletePreset unlinks the .lpset (one op), then its <stem>_t<i>.mid SMFs (one unlink per step, SPEC §9.29; a
// legacy assets/ copy is shared and stays), and then copies _index.txt to _index.tmp without that line and
// renames it over the index (so a later random load never meets the stale entry); beginSetLabel copies <name> to
// <name>.tmp through the same line filter with the old lp.label line dropped and the new one written right after the
// genre= line (or appended), then renames the copy over the original. Both are 4 KB of reads per step; the copy exists
// whole before the rename, so a power loss leaves either the old file or the new one, never none (SPEC §9.22).

static_assert(sizeof(Sets::PresetRow) * Sets::kPresetPage <= (size_t)kSetPage * kSetNameMax, "the preset rows live inside the names_ page buffer (SPEC §9.22: Sets grows by its bookkeeping only)");

namespace {
inline bool labelChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-'; }
// Row order: higher NNN first, ties by name (descending strcmp), the number-less names (-1) last.
inline bool rowBetter(int numA, const char* nameA, int numB, const char* nameB) { return numA != numB ? numA > numB : strcmp(nameA, nameB) > 0; }
// The held line (n bytes, no newline) equals `s` once a trailing CR is ignored.
inline bool heldIs(const char* held, int n, const char* s) {
  if (n > 0 && held[n - 1] == '\r') n--;
  return (int)strlen(s) == n && strncmp(held, s, (size_t)n) == 0;
}
// The held bytes trimmed (set_presets.lua:40-42: blanks around the key) equal key `k`.
inline bool heldKeyIs(const char* held, int n, const char* k) {
  int a = 0; while (a < n && isSpace(held[a])) a++;
  while (n > a && isSpace(held[n - 1])) n--;
  return (int)strlen(k) == n - a && strncmp(held + a, k, (size_t)(n - a)) == 0;
}
}

bool Sets::validLabel(const char* label) {
  if (!label) return false;
  int n = 0;
  for (; label[n]; n++) if (!labelChar(label[n]) || n >= kSetLabelMax - 1) return false;
  return true;
}

void Sets::copyLabelText(char* dst, const char* src) {
  int n = 0;
  if (src) for (const char* p = src; *p && n < kSetLabelMax - 1; p++) if (labelChar(*p)) dst[n++] = *p;
  dst[n] = 0;
}

bool Sets::presetDir() {
  const char* genre = gen_ ? gen_->genreName() : nullptr;
  if (!genre || !genre[0]) return false;
  joinPath(dir_, sizeof dir_, "/lp/packs", genre);
  joinPath(dir_, sizeof dir_, dir_, "presets");
  return true;
}

bool Sets::presetListReady() const { return listValid_ && listDone_; }
int Sets::presetListTotal() const { return presetListReady() ? listTotal_ : -1; }
int Sets::presetRowCount() const { return presetListReady() ? rowCount_ : 0; }
const Sets::PresetRow& Sets::presetRow(int i) const { return rows_[i < 0 ? 0 : i >= kPresetPage ? kPresetPage - 1 : i]; }

void Sets::setThreshold(const PresetRow& r) { copyStr(fallback_, sizeof fallback_, r.name); thrSet_ = 1; }

int Sets::rowIndexOf(const char* name) const {
  if (!presetListReady()) return -1;
  for (int i = 0; i < rowCount_; i++) if (strcmp(rows_[i].name, name) == 0) return i;
  return -1;
}

void Sets::dropRow(int i) {
  if (i < 0 || i >= rowCount_) return;
  for (int k = i; k + 1 < rowCount_; k++) rows_[k] = rows_[k + 1];
  rowCount_--;
}

bool Sets::beginPresetList(int filterKeyRoot, int firstIndex) {
  if (session_ == SsWriting) abortSession();                        // the user's listing wins; the session is rewritten afterwards
  if (busy()) { copyStr(err_, sizeof err_, "BUSY"); return false; }
  if (!fs_ || !gen_) { copyStr(err_, sizeof err_, "NO FS"); return false; }
  if (gen_->busy()) { copyStr(err_, sizeof err_, "GEN BUSY"); return false; }   // one card state machine at a time (SPEC §9.1)
  if (firstIndex < 0) firstIndex = 0;
  if (firstIndex > 255 * kPresetPage) firstIndex = 255 * kPresetPage;
  const int8_t filter = filterKeyRoot < 0 ? -1 : (int8_t)(filterKeyRoot % 12);
  const uint8_t genre = (uint8_t)gen_->genreIndex();
  // the page after the one listed before: one pass below its last row
  const bool follow = presetListReady() && filter == filterRoot_ && genre == listGenre_ && rowCount_ == kPresetPage && firstIndex == listFirst_ + kPresetPage;
  if (follow) setThreshold(rows_[kPresetPage - 1]); else thrSet_ = 0;
  if (!presetDir()) { copyStr(err_, sizeof err_, "NO GENRE"); return false; }
  listValid_ = 0; listDone_ = 0; listTotal_ = -1; passTotal_ = 0;
  filterRoot_ = filter; listGenre_ = genre; listFirst_ = (int16_t)firstIndex;
  rowCount_ = 0; rowBase_ = 0; labelRow_ = 0;
  if (follow) { passIdx_ = 0; passTarget_ = 0; listRem_ = 0; }
  else { passTarget_ = (uint8_t)(firstIndex / kPresetPage); passIdx_ = 0; listRem_ = (uint8_t)(firstIndex % kPresetPage); }
  ok_ = false; err_[0] = 0; flags_ = 0; mask_ = 0; idxLen_ = 0;
  state_ = PlOpen;
  return true;
}

// The refusals every rewrite and the named load share, and the name check: a .lpset basename of this genre (a name
// carrying an NNN must start with "<Genre>_"; a hand-made name without one passes).
bool Sets::beginPresetOp(const char* name, const char* op) {
  if (session_ == SsWriting) abortSession();
  if (busy()) { copyStr(err_, sizeof err_, "BUSY"); return false; }
  if (!fs_ || !gen_) { copyStr(err_, sizeof err_, "NO FS"); return false; }
  if (gen_->busy()) { copyStr(err_, sizeof err_, "GEN BUSY"); return false; }
  if (!presetDir()) { copyStr(err_, sizeof err_, "NO GENRE"); return false; }
  syncLoadedGenre();                                                // SPEC §9.25: a delete clears the preset's bit in this genre's masks
  if (!name || !name[0] || name[0] == '.' || strlen(name) >= kSetNameMax || strchr(name, '/') || strchr(name, '\\') || !hasExt(name, ".lpset")) { copyStr(err_, sizeof err_, "BAD NAME"); return false; }
  if (parsePresetNumber(name) >= 0) {
    const char* g = gen_->genreName(); const size_t gl = strlen(g);
    if (strncmp(name, g, gl) != 0 || name[gl] != '_') { copyStr(err_, sizeof err_, "OTHER GENRE"); return false; }
  }
  copyStr(chosen_, sizeof chosen_, name);
  ok_ = false; err_[0] = 0; flags_ = 0; mask_ = 0; idxLen_ = 0; lineMode_ = 0; lineInsert_ = 0; lineKey_ = 1; page_ = 0; attempts_ = 0;
  (void)op;
  return true;
}

// A preset whose original is missing but whose <name>.tmp is there: the rewrite that wrote the .tmp whole was cut short
// between unlinking the original and renaming (SdFs::rename's two steps, or a power loss there), so the .tmp IS the
// preset (complete, with the label it was given). One probe + one rename; true when the original exists again, and the
// caller retries its open on the next step. Called wherever a preset's open fails: the listing's label read, the loader's
// chooseFile and the label rewrite's own open.
bool Sets::healTmp(const char* path) {
  char tmp[kMaxPathLen]; const size_t n = strlen(path);
  if (n + 5 > sizeof tmp) return false;
  memcpy(tmp, path, n); memcpy(tmp + n, ".tmp", 5);
  if (!fs_->exists(tmp)) return false;
  return fs_->rename(tmp, path);
}

bool Sets::beginLoadNamed(int set, const char* name, Rng& rng) {
  if (!name || !name[0] || strlen(name) >= kSetNameMax || strchr(name, '/') || strchr(name, '\\') || !hasExt(name, ".lpset")) { copyStr(err_, sizeof err_, "BAD NAME"); return false; }
  if (!beginLoad(set, rng)) return false;                           // the same refusals and setup as a random load
  copyStr(chosen_, sizeof chosen_, name);
  state_ = LdChoose;                                                // no listing: chooseFile opens and parses it; a missing file fails "OPEN SET"
  return true;
}

bool Sets::deletePreset(const char* name) {
  if (!beginPresetOp(name, "DELETE")) return false;
  state_ = DlUnlink;
  return true;
}

bool Sets::beginSetLabel(const char* name, const char* label) {
  if (!validLabel(label)) { copyStr(err_, sizeof err_, "BAD LABEL"); return false; }
  if (!beginPresetOp(name, "LABEL")) return false;
  copyStr(label_, sizeof label_, label);
  state_ = SlOpenSrc;
  return true;
}

// One entry of a directory page of the listing (the walk fallback): every .lpset name goes straight to considerRow, no
// storage (names_ holds the rows). Entries before `skip` are passed over, ≤ kSetPage are taken per step (SPEC §9.2).
bool Sets::listFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  PageScan& ps = *static_cast<PageScan*>(user);
  if (ps.seen++ < ps.skip) return true;
  if (!isDir) ps.self->considerRow(name);
  return ++ps.taken < kSetPage;
}

// A name of the pass: the key filter, the total, then the ordered insertion into rows_[rowBase_ .. kPresetPage) when it
// lies strictly below the threshold (the rows already final above rowBase_ are never touched).
void Sets::considerRow(const char* name) {
  if (!hasExt(name, ".lpset") || strlen(name) >= kSetNameMax) return;
  const int num = parsePresetNumber(name);
  const int root = presetKeyRoot(name);
  if (filterRoot_ >= 0 && root != filterRoot_) return;
  passTotal_++;                                                     // every pass sees every match: the total of the filter
  if (thrSet_ && !rowBetter(parsePresetNumber(fallback_), fallback_, num, name)) return;   // at or above the threshold: an earlier page's row
  int pos = rowCount_;
  while (pos > rowBase_ && rowBetter(num, name, parsePresetNumber(rows_[pos - 1].name), rows_[pos - 1].name)) pos--;
  if (pos > rowBase_ && strcmp(rows_[pos - 1].name, name) == 0) { passTotal_--; return; }   // a name listed twice (an index with a duplicate line)
  if (pos >= kPresetPage) return;                                   // below every row of a full page
  const int last = rowCount_ < kPresetPage ? rowCount_ : kPresetPage - 1;
  for (int i = last; i > pos; i--) rows_[i] = rows_[i - 1];
  copyStr(rows_[pos].name, kSetNameMax, name); rows_[pos].label[0] = 0; rows_[pos].keyRoot = (int8_t)root;
  if (rowCount_ < kPresetPage) rowCount_++;
}

// End of a pass: the aligned pages before the one wanted set the next threshold (a pass that found fewer rows than its
// capacity means the names ran out: the wanted page is empty), an unaligned first keeps rows first % 16 .. and fills the
// rest with one more pass, then the labels.
void Sets::listPassDone() {
  const int filled = rowCount_ - rowBase_, cap = kPresetPage - rowBase_;
  listTotal_ = passTotal_;
  if (passIdx_ < passTarget_) {
    if (filled < cap) { rowCount_ = 0; rowBase_ = 0; listFinish(); return; }
    setThreshold(rows_[kPresetPage - 1]); passIdx_++; rowCount_ = 0; state_ = PlOpen;
    return;
  }
  if (listRem_) {
    const int r = listRem_; listRem_ = 0;
    if (filled <= r) { rowCount_ = 0; listFinish(); return; }
    const bool more = filled == cap;
    if (more) setThreshold(rows_[kPresetPage - 1]);
    for (int i = r; i < filled; i++) rows_[i - r] = rows_[i];
    rowCount_ = (uint8_t)(filled - r);
    if (!more) { listFinish(); return; }
    rowBase_ = rowCount_; passIdx_++; state_ = PlOpen;
    return;
  }
  listFinish();
}

void Sets::listFinish() { labelRow_ = 0; rowBase_ = 0; state_ = PlLabel; }

void Sets::labelFn(const char* key, const char* value, void* user) {
  Sets& s = *static_cast<Sets*>(user);
  if (strcmp(key, "lp.label") != 0 || s.labelRow_ >= kPresetPage) return;
  copyLabelText(s.rows_[s.labelRow_].label, value);
  s.number_ = 1;                                                    // found: the rest of the file is not read
}

bool Sets::filterOut(const void* p, size_t n) {
  if (!n) return true;
  if (dst_->write(p, n) != n) return false;
  lineKey_ = static_cast<const char*>(p)[n - 1] == '\n' ? 1 : 0;
  return true;
}

bool Sets::insertLabel() {
  lineInsert_ = 2;
  if (!label_[0]) return true;                                      // an empty label removes the line
  char line[kSetLabelMax + 12]; const int m = snprintf(line, sizeof line, "lp.label=%s\n", label_);
  return filterOut(line, (size_t)m);
}

// One 4 KB read of src_ through the line filter into dst_. Lines are held (idxLine_, ≤ 63 bytes) until they can be
// judged — the index copy at the newline (the deleted name is dropped), the label copy at the key's separator (an
// lp.label line is dropped, the genre line is kept and the new label line follows it) — then passed through or skipped
// to their end; a held line longer than the buffer is kept as it is. The hold carries over chunk boundaries, so a line
// split by a read is judged whole. 1 = EOF reached (files still open), 0 = more, -1 = a write failed.
int Sets::filterStep(bool labelMode) {
  const size_t n = src_->read(chunk_, kSetChunk);
  const char* c = reinterpret_cast<const char*>(chunk_);
  size_t i = 0;
  while (i < n) {
    if (lineMode_ != 0) {                                           // 1 keep: pass through, 2 drop: skip — up to the end of the line
      size_t j = i; while (j < n && c[j] != '\n') j++;
      const bool eol = j < n; if (eol) j++;
      if (lineMode_ == 1 && !filterOut(c + i, j - i)) return -1;
      const bool kept = lineMode_ == 1;
      i = j;
      if (eol) { lineMode_ = 0; idxLen_ = 0; if (kept && lineInsert_ == 1 && !insertLabel()) return -1; }
      continue;
    }
    const char ch = c[i++];
    int decide = 0; bool eol = false;                               // 0 undecided, 1 keep, 2 drop
    if (ch == '\n') { eol = true; decide = (!labelMode && heldIs(idxLine_, idxLen_, chosen_)) ? 2 : 1; }
    else if (labelMode && (ch == '=' || ch == ':')) {
      decide = heldKeyIs(idxLine_, idxLen_, "lp.label") ? 2 : 1;
      if (decide == 1 && lineInsert_ == 0 && heldKeyIs(idxLine_, idxLen_, "genre")) lineInsert_ = 1;
    }
    else if (idxLen_ >= kSetNameMax - 1) decide = 1;                 // over-long: kept as it is
    if (!decide) { idxLine_[idxLen_++] = ch; continue; }
    if (decide == 1) { idxLine_[idxLen_] = ch; if (!filterOut(idxLine_, (size_t)idxLen_ + 1)) return -1; }
    idxLen_ = 0;
    if (eol) { lineMode_ = 0; if (decide == 1 && lineInsert_ == 1 && !insertLabel()) return -1; }
    else lineMode_ = (uint8_t)decide;
  }
  if (n < (size_t)kSetChunk) {                                      // EOF
    if (idxLen_ > 0) {                                              // a last line without a newline, still held
      const bool drop = !labelMode && heldIs(idxLine_, idxLen_, chosen_);
      idxLine_[idxLen_] = '\n';
      if (!drop && !filterOut(idxLine_, (size_t)idxLen_ + 1)) return -1;
      idxLen_ = 0;
    }
    if (labelMode && lineInsert_ != 2 && label_[0]) {              // no genre line (or it was the last, unterminated): the label at the end
      if (!lineKey_ && !filterOut("\n", 1)) return -1;
      if (!insertLabel()) return -1;
    }
    return 1;
  }
  return 0;
}

void Sets::stepPreset() {
  switch (state_) {
    // ---- listing
    case PlOpen:                                                    // index present? (one probe); the pass starts
      indexPath(path_, sizeof path_);
      page_ = 0; item_ = 0; count_ = 0; idxLen_ = 0; passTotal_ = 0; rowCount_ = rowBase_;
      flags_ &= (uint16_t)~(kFlagIndex | kFlagChunk | kFlagEof);
      if (fs_->exists(path_)) { flags_ |= kFlagIndex; state_ = PlIndexRead; } else state_ = PlScan;
      break;
    case PlIndexRead:                                               // one 4 KB chunk of names per step
      if (!(flags_ & kFlagChunk)) {
        if (((flags_ & kFlagEof) && item_ >= count_) || !readIndexChunk()) { indexDone(); break; }
      }
      scanIndex();
      if (flags_ & kFlagEof) { indexDone(); break; }
      page_++; item_ = 0; flags_ &= (uint16_t)~kFlagChunk;
      break;
    case PlScan: {                                                  // no index: one directory page per step, straight into the rows
      PageScan ps = {this, page_ * kSetPage, 0, 0, 0, false};
      fs_->walk(dir_, listFn, &ps);                                 // a missing folder is an empty listing
      if (ps.taken < kSetPage) listPassDone(); else page_++;
      break; }
    case PlLabel:                                                   // the next row's file (one open); a row whose file is gone leaves the page
      if (labelRow_ >= rowCount_) { listValid_ = 1; listDone_ = 1; ok_ = true; state_ = Idle; break; }
      joinPath(path_, sizeof path_, dir_, rows_[labelRow_].name);
      src_ = fs_->open(path_, "r");
      if (!src_) { if (healTmp(path_)) break; dropRow(labelRow_); if (listTotal_ > 0) listTotal_--; break; }   // a stray <name>.tmp is renamed back and the row re-read next step
      number_ = 0;
      reader_.begin(labelFn, this);
      state_ = PlLabelRead;
      break;
    case PlLabelRead: {                                             // 4 KB per step until lp.label or the end
      const size_t n = src_->read(chunk_, kSetChunk);
      reader_.feed(reinterpret_cast<const char*>(chunk_), (int)n);
      if (number_ || n < (size_t)kSetChunk) { reader_.finish(); closeFiles(); labelRow_++; state_ = PlLabel; }
      break; }
    // ---- delete: the file, then the index without its line
    case DlUnlink: {
      joinPath(path_, sizeof path_, dir_, chosen_);
      if (!fs_->remove(path_)) { fail("NO PRESET"); return; }
      const int r = rowIndexOf(chosen_); if (r >= 0) { dropRow(r); if (listTotal_ > 0) listTotal_--; }
      for (int s = 0; s < kSets; s++) if (strcmp(lastName_[s], chosen_) == 0) lastName_[s][0] = 0;
      { const int bit = presetBit(chosen_); if (bit >= 0) for (int s = 0; s < kSets; s++) setLoadedBit(s, bit, false); }   // SPEC §9.25: its bit is free for the number's next owner
      item_ = 0; state_ = DlSmf;
      break; }
    case DlSmf:                                                     // the preset's own SMFs, one unlink per step (most tracks have none: the unlink of a missing file is the probe)
      if (item_ >= kTracksPerSet) { state_ = DlIndexOpen; break; }
      presetSmfPath(path_, sizeof path_, dir_, chosen_, item_);
      fs_->remove(path_);
      item_++;
      break;
    case DlIndexOpen:
      indexPath(path_, sizeof path_);
      src_ = fs_->open(path_, "r");
      if (!src_) { ok_ = true; state_ = Idle; break; }              // no index: nothing to keep in step
      state_ = DlTmpOpen;
      break;
    case DlTmpOpen:
      joinPath(path_, sizeof path_, dir_, "_index.tmp");
      dst_ = fs_->open(path_, "w");
      if (!dst_) { fail("INDEX"); return; }                         // the preset is gone; the stale index entry heals on the next load as before
      idxLen_ = 0; lineMode_ = 0; lineInsert_ = 0; lineKey_ = 1; state_ = DlCopy;
      break;
    case DlCopy: {
      const int r = filterStep(false);
      if (r < 0) { closeFiles(); fs_->remove(path_); fail("INDEX"); return; }
      if (r > 0) { closeFiles(); state_ = DlRename; }
      break; }
    case DlRename: {
      char idx[kMaxPathLen]; indexPath(idx, sizeof idx);
      if (!fs_->rename(path_, idx)) {                               // retried a few steps; then _index.tmp goes only while the index is still there (an index is derived: a missing one is rebuilt by the next save)
        if (++attempts_ < kRenameRetries) break;
        if (fs_->exists(idx)) fs_->remove(path_);
        fail("INDEX"); return;
      }
      ok_ = true; state_ = Idle;
      break; }
    // ---- label: <name>.tmp with the label line, renamed over the original
    case SlOpenSrc:
      joinPath(path_, sizeof path_, dir_, chosen_);
      src_ = fs_->open(path_, "r");
      if (!src_) { if (healTmp(path_)) break; fail("NO PRESET"); return; }   // only its .tmp left: renamed back, opened next step
      state_ = SlOpenDst;
      break;
    case SlOpenDst: {
      joinPath(path_, sizeof path_, dir_, chosen_);                 // <presets>/<name>.tmp (dir_ ≤ 40 + a name < 64: fits kMaxPathLen)
      const size_t n = strlen(path_);
      if (n + 5 > sizeof path_) { fail("OPEN TMP"); return; }
      memcpy(path_ + n, ".tmp", 5);
      dst_ = fs_->open(path_, "w");
      if (!dst_) { fail("OPEN TMP"); return; }
      idxLen_ = 0; lineMode_ = 0; lineInsert_ = 0; lineKey_ = 1; state_ = SlCopy;
      break; }
    case SlCopy: {
      const int r = filterStep(true);
      if (r < 0) { closeFiles(); fs_->remove(path_); fail("WRITE"); return; }
      if (r > 0) { closeFiles(); state_ = SlRename; }
      break; }
    case SlRename: {
      char dst[kMaxPathLen]; joinPath(dst, sizeof dst, dir_, chosen_);
      if (!fs_->rename(path_, dst)) {
        // The .tmp is complete. SdFs::rename unlinks the original before its second rename, so after a failure the .tmp may be
        // the ONLY copy: retried kRenameRetries steps, then the .tmp goes only while the original is still there (the label
        // change is lost, nothing else); otherwise it stays as the preset — healTmp renames it back at the next listing / open.
        if (++attempts_ < kRenameRetries) break;
        if (fs_->exists(dst)) fs_->remove(path_);
        fail("RENAME"); return;
      }
      const int r = rowIndexOf(chosen_); if (r >= 0) copyLabelText(rows_[r].label, label_);
      for (int s = 0; s < kSets; s++) if (strcmp(lastName_[s], chosen_) == 0) copyLabelText(loadedLabel_[s], label_);   // the set loaded from it follows
      ok_ = true; state_ = Idle;
      break; }
    default: state_ = Idle; break;
  }
}

} // namespace lp
