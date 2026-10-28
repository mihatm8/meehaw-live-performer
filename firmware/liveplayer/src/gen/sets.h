// Saved sets as device-native .lpset presets (SPEC §6, §9.8, §9.14, §9.29): the session format of §9.18.
//   /lp/packs/<Genre>/presets/<Genre>_<KeyLabel>_<NNN>.lpset   Java-properties text, ASCII, forward slashes, lp.* keys
//   /lp/packs/<Genre>/presets/<Genre>_<KeyLabel>_<NNN>_t<i>.mid  SMF of track i when its pattern is edited / recorded / theory
//   /lp/packs/<Genre>/presets/_index.txt                        one .lpset name per LF line, kept by the writer
// Library MIDI, vocal and loop files are referenced by their absolute path (lp.src.i), never copied (§9.29: no
// presets/assets/ copies any more; a file saved before that change, clip=assets/<basename>, still loads). A library
// pattern that moved to another sNN shard since the save (a re-imported card) is found again by its basename; one
// that is gone leaves its track as it was (the load still ends LOADED).
// Save and load are resumable state machines (SPEC §9.2): every step() does one bounded piece of SD work
// (one directory page of ≤ 32 entries, one 4 KB read or write, ≤ 3 exists() probes, one file open) so the
// loop task stays under 20 ms. The listing goes through _index.txt when it exists (one 4 KB read per ≈ 160
// names; the first save on a card without one builds it from the directory walk); files copied by hand
// become visible after deleting the index or re-running tools/write_indexes.py. The key filter of a load
// reads the key label from the file name (<Genre>_<Key>_<NNN>) and only opens files named otherwise.
// save() is a blocking wrapper; loadRandom() blocks only until a set is chosen and parsed (see below).
// The UI should use begin*() + step() and poll busy()/progress(). A save or load refuses to start while the
// generator is busy (both would share staging, slotAck and the set's next buffers, SPEC §9.1 / §9.5).
#pragma once
#include "../core/types.h"
#include "../io/fs.h"
#include "../io/wav.h"
#include "generator.h"
#include "library.h"
namespace lp {

constexpr int kSetChunk = 4096;             // one SD read or write per step (SPEC §9.2)
constexpr int kSetPage = 32;                // directory entries processed per step (SPEC §9.2)
constexpr int kSetLineMax = 672;            // longest .lpset line kept (a 4-path kit line is <= 4 × kMaxPathLen + 18 = 658 B, lp.loaded.N 268 B); longer lines are dropped (RAM diet 2026-10-16: 1024 → 768 → 672, the SPEC §9.27 reserve lever)
static_assert(kSetLineMax >= 4 * kMaxPathLen + 18 + 1, "a 4-path kit line (lp.track.7.kit=p;p;p;p) must fit a set line");
constexpr int kSetNameMax = 64;             // preset file names (and shard subfolder names) longer than this are skipped
constexpr int kSetNumberRetries = 64;       // NNN collision probes before a save gives up
constexpr int kSetLabelMax = 20;            // lp.label: up to 19 characters of [A-Za-z0-9 _-] plus the terminator (SPEC §9.22 preset manager)

// One parsed or to-be-written .lpset (SPEC §9.8, §9.29). Fixed size, no pointers. The track's source (a path, "acid" or
// a legacy clip) lives in SetFile::src[i].
struct SetFileTrack {
  uint8_t variant = 0;                      // lp.track.i.variant (legacy files: track.i.slot / track.7.beatLayer)
  uint8_t beatKickMuted = 0;                // lp.track.7.kickcut (legacy: track.7.beatKickMuted=true)
  uint8_t hasPreset = 0, hasSeed = 0, hasLen = 0, hasKey = 0, hasPause = 0;
  int8_t keyRoot = -1;                      // lp.track.i.key
  int8_t lenk = 0;                          // lp.track.i.lenk (×2^k exponent, SPEC §9.4; 0 on the Vocal)
  uint8_t pause = kVocalPauseDefault;       // lp.track.i.pause: the Vocal's pause bars 0 / 1 / 2 / 4 (SPEC §9.16), default 1 when absent
  uint32_t seed = 1;                        // lp.track.i.seed
  float lenBeats = 0;                       // lp.track.i.len (0.5 .. 16; Vocal: the phrase's natural power-of-two beats, SPEC §9.3)
  // SPEC §9.17b: the strip and the synth knobs (lp.track.i.vol .dist .filter .echo .reverb .cutoff .res .env .decay
  // .drive .wave .acid), each present when its bit is set in `has`; a file without them (before §9.17b) resets the track.
  // SPEC §9.22 adds the 303 character and the chorus send (lp.track.i.sat .accent .chorus) the same way.
  enum : uint16_t { HasVol = 1, HasDist = 2, HasFilter = 4, HasEcho = 8, HasReverb = 16, HasCutoff = 32, HasRes = 64, HasEnv = 128, HasDecay = 256, HasDrive = 512, HasWave = 1024, HasAcid = 2048,
                    HasSat = 4096, HasAccent = 8192, HasChorus = 16384 };
  uint16_t has = 0;
  uint16_t preset = 0;                      // lp.track.i.preset (absolute bank index; 16 bits for the generated banks of SPEC §9.22; placed here to pack without padding)
  float vol = 0.8f, dist = 0, filter = 0, echo = 0, reverb = 0;
  float cutoff = 0.5f, res = 0.2f, env = 0.25f, decay = 0.4f, drive = 0;
  float sat = 0, accent = 0.5f, chorus = 0;   // SPEC §9.22 (TrackParams defaults when absent)
  uint8_t wave = WSaw, acid = 0;
  uint8_t edited = 0, loop = 0;             // session files (SPEC §9.18): the source is a session SMF of an edited / recorded (Fx loop) pattern
  uint8_t loopMode = 0;                     // lp.track.i.loopmode=1 (SPEC §9.19): the source is a streamed loop (lp.src.i = its absolute path)
};
struct SetFile {
  int schemaVersion = 0;
  int setIndex = -1;
  int keyRoot = -1;
  int bpm = 0;                              // lp.bpm (0 = absent)
  char keyName[16] = "";
  char genre[kGenreNameLen] = "";           // a pack folder name (Library::genreName: <= 23 chars since the RAM diet 2026-10-16; was kMaxNameLen = 96, −72 B)
  char label[kSetLabelMax] = "";            // lp.label (SPEC §9.22 preset manager): a user tag of ≤ 19 chars of [A-Za-z0-9 _-], "" none; the writer emits it when set (a set carries the label of the preset it was loaded from into its later saves and its session file)
  SetFileTrack track[kTracksPerSet];
  char kit[kSampleSlots][kMaxPathLen];      // lp.track.7.kit → slots 0..3, lp.track.5.kit → slots 4..7 (absolute paths); session: lp.kit.<slot>
  char src[kTracksPerSet][kMaxPathLen];     // lp.src.i = absolute source path of the pattern / vocal / loop, "acid" for a generated line (SPEC §9.18, §9.29);
                                            // a legacy preset's track.i.clip ("assets/<basename>", relative to the preset folder) lands here when no lp.src.i came first
  int volume = -1;                          // session files: lp.volume (master output 0..255), -1 = absent
  uint32_t seq = 0;                         // session files: lp.seq, one up per file written (the restore takes the newest complete slot)
  uint8_t complete = 0;                     // session files: lp.end=1 was read — the last line written; absent = a truncated file (skipped)
  uint8_t banks = 0;                        // lp.banks: the preset bank layout the lp.track.N.preset indices refer to (kBanksLayout = 2 since the 2026-10-16 wave's generated banks; 0 = absent = the hand-only layout before it, whose '303' indices the loader remaps). Fits the padding after `complete`.
  void clear();
};

// Java-properties reader mirroring set_presets.lua:9-54: feed raw bytes in any chunking; every completed
// logical line (CRLF stripped, backslash continuations joined, trimmed, '#'/'!' comments skipped, split at
// the first ':' or '=' else at whitespace, both halves unescaped) is handed to the callback.
typedef void (*PropertyFn)(const char* key, const char* value, void* user);
class PropertiesReader {
 public:
  void begin(PropertyFn fn, void* user) { fn_ = fn; user_ = user; len_ = 0; overflow_ = false; }
  void feed(const char* data, int n);
  void finish();                            // flushes a last line without a trailing newline
 private:
  PropertyFn fn_ = nullptr; void* user_ = nullptr;
  char line_[kSetLineMax]; int len_ = 0; bool overflow_ = false;
  void endRawLine();
};
// In-place split + unescape of one assembled line (set_presets.lua:40-47). Returns false when the line yields no key.
bool parsePropertyLine(char* line, const char** key, const char** value);
void unescapeProperty(char* s);             // set_presets.lua:9-18, in place
// Applies one key/value to a SetFile (unknown keys ignored). Exposed for tests.
void applySetProperty(SetFile& f, const char* key, const char* value);
// Parses a whole .lpset held in memory (tests / small files); the loader itself streams in kSetChunk reads.
void parseSetFileText(const char* text, int n, SetFile& out);

class Sets {
 public:
  // lib is optional (kit fallback picks when a stored one-shot path is missing); the .ino call without it still compiles.
  void init(IFileSystem* fs, Engine* eng, Generator* gen, Library* lib = nullptr);

  // ---- resumable API (SPEC §9.2) ----
  bool beginSave(int set);                  // false when a save/load or the generator is running, or the genre is unusable
  bool beginLoad(int set, Rng& rng);        // draws one value from rng to seed the loader's own Rng
  void step();                              // call every loop iteration; one bounded SD action per call
  // A user save / load or the boot restore (they own the engine's next buffers, staging and slotAck, SPEC §9.5). A
  // session WRITE is not "busy": it yields to the user (beginSave / beginLoad / the generator abort it and it is
  // re-armed), so a GENERATE pressed inside the write window is never refused; sessionBusy() reports it.
  bool busy() const { return state_ != Idle && session_ != SsWriting; }
  int progress() const { return done_; }    // tracks finished (0..8)
  int total() const { return kTracksPerSet; }
  bool ok() const { return ok_; }           // result of the last finished operation
  const char* lastError() const { return err_; }
  // Session persistence (SPEC §9.18): /lp/state/session_set<N>.lpset and session_set<N>_b.lpset (two slots per set,
  // written alternately, lp.seq counts up, lp.end=1 is the last line) hold both sets with params/knobs/acid/pause/
  // variants and absolute source paths (no asset copies; edited / recorded patterns go to
  // /lp/state/session_s<set>_t<track>.mid, an acid line is regenerated from its seed); written debounced 5 s after
  // the last change (markSessionDirty, or any Generator::changeCount() bump), at least 15 s after the previous write,
  // only while Generator::streamsHealthy() and never while a loop slot is being consumed (Generator::loopSlotStreaming:
  // a write due then waits, one in flight pauses, until the slot tracks or the transport stop; SPEC §9.19 §3 — a 139 ms
  // slot ring cannot absorb a slow card's 150 ms open), through the same chunked state machine
  // as save(); restored at boot without starting playback from the newest slot that carries lp.end. A missing,
  // malformed or truncated file (a power-off mid-write) is ignored silently; the other slot still restores.
  void markSessionDirty();                             // also arms session persistence (like restoreSession): hosts that never call either write nothing
  void restoreSession();                               // queues the restore of both sets (no-op when the files are absent) and arms the writer
  bool sessionBusy() const { return session_ != SsNone; }   // a session write or the boot restore is running
  void setConfigBpm(int b) { configBpm_ = b; }              // config.txt bpm= (SPEC §9.19f, user decision 2026-10-16): a restored session's lp.bpm yields to it; presets still bring their tempo
  void setSessionVolume(int v) { sessionVolume_ = v; }      // the UI's master output level (0..255): written as lp.volume when ≥ 0
  int sessionVolume() const { return restoredVolume_; }     // lp.volume read by the last restore, -1 when none (the UI applies it)
  static void sessionFilePath(char* out, int cap, int set, int slot = 0);   // /lp/state/session_set<N>.lpset (slot 0) / session_set<N>_b.lpset (slot 1)
  uint32_t sessionSeq() const { return sessionSeq_; }       // lp.seq of the last file written / the newest restored (tests)
  static constexpr const char* kSessionDir = "/lp/state";
  static constexpr int kSessionDebounceSamples = 5 * kSampleRate;   // 5 s on the engine's sample clock after the last change (SPEC §9.19 §3 c)
  static constexpr int kSessionSpacingSamples = 15 * kSampleRate;   // at least 15 s between two completed session writes (SPEC §9.19 §3 c)
  const char* lastName() const { return name_; }          // file name of the last saved / loaded preset
  bool saving() const { return session_ == SsNone && state_ >= SvMkdir && state_ <= SvIndexAppend; }
  bool loading() const { return session_ == SsNone && state_ >= LdOpen && state_ < PlOpen; }

  // ---- synchronous wrappers ----
  // save blocks until the file and its SMFs are written (no engine involved); false with lastError when a
  // source could not be found (the file is still written without that track, lastName() names it). loadRandom
  // blocks only until a matching set has been chosen and parsed (false + lastError "NO SET IN <Key>" when none);
  // the tracks are then reconstructed by step() (kit slots need the engine's acks, the swap lands at the bar
  // line) and busy() stays true until the engine has adopted every pattern (or the transport is stopped).
  // ---- preset manager (SPEC §9.22): SHIFT + load 1 / load 2 lists this genre's presets ----
  // Rows are the names of the genre's presets/ folder ordered newest (highest NNN) first, ties by name, then the names
  // without an NNN (hand-made files, keyRoot -1; a key filter leaves them out — the loader's keyRoot probe is not made here).
  // The listing streams _index.txt (or walks the folder in pages of kSetPage) once per aligned page of 16 below a running
  // threshold — page p costs p + 1 passes from scratch, ONE pass when it follows the page listed before (the usual paging) —
  // then reads lp.label from each row's file, one open or one 4 KB read per step. The rows live in the names_ page buffer:
  // they stay valid (presetListReady) until a save, load, session write or restore begins; deletePreset / beginSetLabel keep
  // them current. presetListTotal() is -1 until the listing finished.
  struct PresetRow { char name[kSetNameMax]; char label[20]; int8_t keyRoot; };   // file name, lp.label ("" none), minor root of <Key> (-1 unlabelled)
  static constexpr int kPresetPage = 16;                      // rows held at once
  bool beginPresetList(int filterKeyRoot, int firstIndex);    // rows [firstIndex, +kPresetPage) of the genre's presets, newest (highest NNN) first, filtered to a key root (-1 = all); one card operation per step(); false while busy
  bool presetListReady() const;                               // the requested page is complete (labels read)
  int presetListTotal() const;                                // presets matching the filter (known once the listing finished)
  int presetRowCount() const; const PresetRow& presetRow(int i) const;
  bool beginLoadNamed(int set, const char* name, Rng& rng);  // a chosen preset into `set` (the file's setIndex is ignored); otherwise as beginLoad
  bool deletePreset(const char* name);                       // unlinks presets/<name> and its <stem>_t<i>.mid SMFs (one op per step; legacy assets/ copies stay, SPEC §9.29); false while busy or when the name is not a preset of this genre
  bool beginSetLabel(const char* name, const char* label);   // rewrites presets/<name> with lp.label=<label> (<= 19 chars of [A-Za-z0-9 _-]); one card operation per step()
  bool presetBusy() const { return state_ >= PlOpen; }       // a listing, delete or label rewrite is running (busy() covers it; saving() / loading() do not)
  static bool validLabel(const char* label);                 // ≤ 19 chars, each of [A-Za-z0-9 _-] ("" = no label)
  static void copyLabelText(char* dst, const char* src);     // into a char[kSetLabelMax]: the allowed characters only, cut at 19 (what the parser keeps of lp.label)
  bool save(int set, char* outName, int cap);
  bool loadRandom(int set, Rng& rng, char* outName, int cap);

  // ---- the random-load cycle (SPEC §9.25, user request 2026-10-16) ----
  // A random load never offers a preset again until every other preset of the genre that matches the filter (the key
  // rules of the loader) has been loaded into that set: per set a LOADED bitmask over the preset numbers NNN of the
  // current genre (bit NNN % kLoadedPresets; a name without an NNN has no bit — always a candidate, never marked). The
  // candidates of a load are the matches whose bit is clear (the last loaded name stays the fallback, SPEC §9.8); when
  // every other match is marked, a second pass over the listing clears the bits of that candidate set (a new cycle) and
  // the draw is made again. A finished load — random, or named from the preset manager (beginLoadNamed) — sets the bit;
  // a saved preset's bit is cleared (it comes up in the current cycle, also when its number belonged to a deleted one);
  // a deleted preset is never listed, so it never blocks the cycle. The masks belong to ONE genre (Generator::genreIndex):
  // a load / save / preset op in another genre clears both. Persisted in the session files as lp.loaded.<set>=<hex> (256
  // hex chars, byte i = presets 8i..8i+7, bit n & 7 = preset n) and restored with them; a change marks the session dirty.
  static constexpr int kLoadedPresets = 1024;                // preset numbers tracked per set (128 B per set)
  bool presetLoadedInCycle(int set, int nnn) const;          // the preset manager's dot: preset nnn of this genre was loaded into `set` in the current cycle (false for nnn < 0 or another genre's masks)
  // Both masks AND the per-set "last loaded" names (the SPEC §9.8 no-immediate-repeat fallback) cleared, so the next random
  // load of each set draws over every match: the Recorder calls it at a recording start and at a render start (beside
  // Generator::seed, which restarts the preset bags the same way), so a random LOAD inside a recording runs its reservoir over
  // the same candidates on replay and picks the same preset (SPEC §9.23 / §9.26). A change marks the session dirty while
  // persistence is armed (the file then carries the empty masks, as any other mask change).
  void resetLoadCycle();

  // ---- helpers exposed for tests ----
  static int parsePresetNumber(const char* name);       // "<Genre>_<Key>_<NNN>.lpset" → NNN, -1 otherwise
  static int presetKeyRoot(const char* name);           // "<Genre>_<Key>_<NNN>.lpset" → minor root 0..11 of <Key>, -1 when unlabelled
  static float bpmToken(const char* name);              // "128bpm" / "bpm_128" → 128, 0 when absent (SPEC §9.3)
  static void presetSmfPath(char* out, int cap, const char* dir, const char* name, int track);   // <dir>/<name without .lpset>_t<track>.mid (SPEC §9.29)
  static uint16_t naturalVocalTicks(uint32_t frames, uint32_t rate, float fileBpm);   // SPEC §9.3 power-of-two beats
  const SetFile& file() const { return file_; }         // what was last written / parsed
  int keyRootOf(int set) const;                         // the key a set's tonal tracks were generated with (SPEC §9.9), majority vote

 private:
  enum State : uint8_t {
    Idle = 0,
    SvMkdir, SvIndexProbe, SvIndexRead, SvScan, SvIndexPage, SvNumber, SvTrack, SvResolve, SvEdit, SvKitPaths, SvWrite, SvIndexAppend,
    SsMkdir, SsWrite,                                    // session write (SPEC §9.18): shares SvTrack / SvResolve / SvEdit / SvKitPaths
    LdOpen, LdIndexRead, LdScan, LdPage, LdProbe, LdProbeRead, LdChoose, LdParse,
    LdTrack, LdSrcFind, LdSlotFind, LdSlotOpen, LdSlotLoad, LdSlotAck, LdVocal, LdWaitSwap,   // LdSrcFind: a library pattern found again by its basename under its key folder's shards (SPEC §9.29)
    RsOpen,                                              // session restore: opens the chosen slot of the set, then LdParse .. LdWaitSwap
    RsProbe, RsProbeRead,                                // session restore: lp.seq / lp.end of each slot (light parse) to choose the newest complete one
    LdLoopFallback,                                      // after the swap: the loop tracks whose file was gone pick a loop of their category (one library op per step, SPEC §9.19 §1.7)
    // preset manager (SPEC §9.22); every state >= PlOpen belongs to it (stepPreset)
    PlOpen, PlIndexRead, PlScan, PlLabel, PlLabelRead,   // listing: index present? → index chunks / directory pages → one lp.label read per row
    DlUnlink, DlSmf, DlIndexOpen, DlTmpOpen, DlCopy, DlRename,   // delete: the .lpset, its _t<i>.mid files (one per step), then _index.txt rewritten without it through _index.tmp + rename
    SlOpenSrc, SlOpenDst, SlCopy, SlRename               // label: <name>.tmp = the file with lp.label inserted / replaced / removed, then renamed over it
  };
  enum Session : uint8_t { SsNone = 0, SsWriting, SsRestoring };
  IFileSystem* fs_ = nullptr; Engine* eng_ = nullptr; Generator* gen_ = nullptr; Library* lib_ = nullptr;
  State state_ = Idle; bool ok_ = false; char err_[32] = "";
  // session (SPEC §9.18)
  Session session_ = SsNone; uint8_t sessionSet_ = 0;    // which set's file is being written / restored
  uint8_t sessionDirty_ = 0, restorePending_ = 0, sessionOn_ = 0;   // sessionOn_: persistence armed by restoreSession / markSessionDirty
  uint32_t dirtyClock_ = 0;                              // engine sampleClock of the last markSessionDirty
  uint32_t genChanges_ = 0;                              // Generator::changeCount() last seen
  int sessionVolume_ = -1, restoredVolume_ = -1; int configBpm_ = 0;
  uint32_t sessionSeq_ = 0;                              // lp.seq of the last session file written (restored from the files at boot)
  uint8_t sessionSlot_ = 0;                              // bit s: the slot set s's next write goes to (the other one holds the newest complete file)
  uint32_t probeSeq_[2] = {0, 0}; uint8_t probeOk_ = 0;   // restore: lp.seq per slot, bit k = slot k ended with lp.end
  uint32_t lastWriteClock_ = 0; uint8_t wrote_ = 0;      // the sample clock a session write last completed at (the 15 s spacing, SPEC §9.19 §3 c)
  uint8_t loopFallback_ = 0;                             // load / restore: bit i = track i's loop file was missing, a loop of its category is picked once the set has swapped (SPEC §9.19 §1.7)
  int set_ = 0; int track_ = 0; int done_ = 0; int slot_ = 0; int page_ = 0; int item_ = 0; int count_ = 0;
  int number_ = 0;                                       // NNN of the file being written / max seen / probe result
  int wantRoot_ = 0;                                     // loader: keyRoot the sets must match
  uint32_t pushClock_ = 0; uint16_t ackWait_ = 0; uint16_t flags_ = 0;
  uint8_t srcLoaded_ = 0; uint8_t attempts_ = 0; uint8_t phase_ = 0; uint8_t mask_ = 0;   // srcLoaded_: loadTrack's phase for the track — 0 to do, 1 source loaded (publish), 2 nothing to publish, 3 LdSrcFind running, 4 found (opened next step)
  uint16_t wait_ = 0;                                    // load / restore: steps waited for a free card handle before a stream open (SPEC §9.19g)
  int idxLen_ = 0;                                       // partial index line carried between chunks
  Rng rng_{1};
  IFile* src_ = nullptr; IFile* dst_ = nullptr;
  char dir_[kMaxPathLen];                                // /lp/packs/<Genre>/presets
  char path_[kMaxPathLen];                               // scratch: current source / candidate path
  char name_[kSetNameMax];                               // result name
  char lastName_[kSets][kSetNameMax];                    // last loaded per set (no immediate repeat, SPEC §9.8)
  char chosen_[kSetNameMax]; char fallback_[kSetNameMax];   // loader reservoir: best non-repeat / the repeat
  char idxLine_[kSetNameMax];                            // index line being assembled
  int matches_ = 0;
  union {
    char names_[kSetPage][kSetNameMax];                  // one directory page of .lpset names / shard kids (save, load, session, restore)
    PresetRow rows_[kPresetPage];                        // the preset manager's page (SPEC §9.22), valid while listValid_ (no machine above wrote names_ since)
  };
  // preset manager (SPEC §9.22)
  uint8_t listValid_ = 0, listDone_ = 0;                 // rows_ hold a listing; that listing is complete (labels read)
  int8_t filterRoot_ = -1; uint8_t listGenre_ = 0;      // the listing's key filter and genre index (a continuation needs the same)
  int16_t listFirst_ = 0, listTotal_ = -1, passTotal_ = 0;   // first rank listed, matches of the filter (-1 unknown), matches counted by the running pass
  uint8_t rowCount_ = 0, rowBase_ = 0;                   // rows filled; rows [0, rowBase_) are final, the pass fills [rowBase_, kPresetPage)
  uint8_t passIdx_ = 0, passTarget_ = 0, listRem_ = 0, labelRow_ = 0;   // pass number, aligned page wanted, unaligned tail (first % 16), row whose label is read
  uint8_t thrSet_ = 0;                                   // threshold set: only names strictly below the row named by fallback_ (its NNN re-read from the name) in the row order are taken
  uint8_t lineMode_ = 0, lineInsert_ = 0, lineKey_ = 0;   // the line filter of the rewrites: hold / keep / drop, the label line is due after this line / was written, the output ends in a newline
  char label_[kSetLabelMax];                             // beginSetLabel's label
  char loadedLabel_[kSets][kSetLabelMax];                // lp.label of the preset each set was last loaded from (or its session file carried): written by the set's saves and session files
  // the random-load cycle (SPEC §9.25)
  uint8_t loaded_[kSets][kLoadedPresets / 8];            // bit NNN % kLoadedPresets: preset NNN of loadedGenre_ was loaded into the set in the current cycle
  uint8_t loadedGenre_ = 255;                            // Generator::genreIndex() the masks belong to (another genre clears them, syncLoadedGenre)
  uint16_t skipped_ = 0;                                 // loader: matches passed over because their bit was set (chooseFile starts a new cycle when nothing else was offered)
  static int presetBit(const char* name);                // NNN % kLoadedPresets of a numbered name, -1 for an unnumbered one
  bool loadedBit(int set, int bit) const;
  void setLoadedBit(int set, int bit, bool on);          // a change marks the session dirty (the cycle is persisted) while persistence is armed
  void syncLoadedGenre();                                // the masks follow the current genre: a change clears both
  uint8_t chunk_[kSetChunk];                             // copy / parse buffer
  SetFile file_;
  PropertiesReader reader_;
  UlawLoader loader_;
  WavInfo wav_;
  // save
  void stepSave();
  bool trackBegin();                                     // fills file_.track[track_] and file_.src[track_] (preset and session writer alike); true when SD work follows
  void fillTrackFields(int track, SetFileTrack& ft);     // the fields both writers share
  void resolveStep();                                    // one SD action towards the source path of track_
  bool resolveProbe(const char* base, const char* folder, const char* kid, const char* file);
  void writeEdit();                                      // SvEdit: smfWrite of the engine source as presets/<name>_t<i>.mid (or /lp/state/session_s<set>_t<i>.mid), one open + write
  void resolveKitPaths();
  struct Sink;
  void writeSetPart(int part, Sink& out);                // one part of the file text (header / lp.bpm / a track's lp lines / kit lines); session_ selects the keys
  int stepWriteFile(const char* path);                   // ≤ 4 KB of parts per step into dst_: 1 done, 0 more, -1 error
  int indexStep(const char* text, int n, bool truncate); // _index.txt: the first call opens it into dst_ (0 = more), the next writes text and closes (1 done, -1 error)
  bool sourceAbs(int track, char* out, int cap);         // the absolute source of file_.src[track]: as written, or a legacy clip joined onto dir_ (sanitised); false for "", "acid" or an unsafe clip
  // session (SPEC §9.18)
  void pollSession();                                    // generator changes → dirty; the 2 s debounce; the queued restore
  void beginSessionWrite(); void startSessionSet(); void abortSession();
  void restoreProbe();                                   // RsProbe: open slot item_ of the set for the light parse (absent → next slot / choose)
  void restoreChoose();                                  // after both slots: the newest complete one → RsOpen, none → next set
  void restoreOpen();                                    // RsOpen: open the chosen slot's file; parsing continues in LdParse
  void nextRestoreSet();
  void applySessionGlobals();                            // key, genre, volume from the parsed file
  void pushKnobParams(int track, const SetFileTrack& ft);    // the synth knobs of the file, right after the track's CmdSetPatch (SPEC §9.17b)
  void pushStripParams(int track, const SetFileTrack& ft);   // the strip (vol, dist, filter, echo, reverb) at the swap, or CmdResetTrack for a file without them
  bool pushAllStrips();                                  // every track's strip in one burst once the set has swapped; false = no queue room yet
  void loopFallbackStep();                               // LdLoopFallback: warms the library for the next missing loop track (one op), then Generator::setLoopMode(on) picks (SPEC §9.19 §1.7)
  void finishLoadDone();                                 // the load / restore is over: Idle, or the next set of the restore
  // load
  void stepLoad();
  bool readIndexChunk();                                 // chunk page_ of _index.txt into chunk_ (one open + read)
  void scanIndex();                                      // consumes chunk_ from item_: names → considerName / NNN max
  void indexDone();                                      // end of the index stream (save: NNN = max + 1; load: choose)
  void resumeListing();                                  // after a probe: back to the index chunk or the page
  bool considerName(const char* name);                   // key from the name: offer / skip; true = needs a probe
  void offer(const char* name);                          // reservoir (uniform, no immediate repeat)
  void probeBegin(const char* name);
  void chooseFile();
  void loadTrack();
  void adoptSource(int t, const SetFileTrack& ft, int v);   // the file just loaded is the track's source: seed, key, variant and the edited / loop flags from the file's lines
  void srcFind();                                        // LdSrcFind: a /lp/midi pattern whose path failed to open, looked for by its basename under its key folders and their shards (≤ 2 probes, one walk or ≤ 3 probes per step, SPEC §9.29); found → loadTrack opens it next step, gone → the track keeps its pattern
  bool buildNext(int track, int variant);                // source + seed/key → next buffer, no swap pushed (SPEC §9.5)
  void slotFind();
  void slotOpen();
  void loadVocal();
  void finishLoad();
  // preset manager (SPEC §9.22)
  void stepPreset();
  bool beginPresetOp(const char* name, const char* err);   // the shared refusals (busy, generator, genre) and the name check of delete / label / named load; sets dir_ and chosen_
  bool presetDir();                                      // dir_ = this genre's presets folder; false without a genre
  void considerRow(const char* name);                    // listing: the key filter, the total, the ordered insertion below the threshold
  void listPassDone();                                   // end of one pass over the names: another pass, the unaligned tail, or the labels
  void listFinish();                                     // the rows are final: read their labels (PlLabel)
  void setThreshold(const PresetRow& r);
  int rowIndexOf(const char* name) const;                // row holding this name, -1
  void dropRow(int i);
  int filterStep(bool labelMode);                        // one 4 KB read of src_ → dst_ through the line filter: 1 EOF reached, 0 more, -1 write error
  bool filterOut(const void* p, size_t n);               // dst_->write; remembers whether the output ends in a newline
  bool insertLabel();                                    // the lp.label line of label_ into dst_ (nothing for an empty label); marks it written
  static bool listFn(const char* name, bool isDir, uint32_t size, void* user);
  static void labelFn(const char* key, const char* value, void* user);
  bool healTmp(const char* path);                        // a missing preset whose <path>.tmp exists (a rewrite's rename cut short): renamed back (one probe + one rename), true = retry the open next step
  // shared
  void fail(const char* msg);
  void closeFiles();
  void indexPath(char* out, int cap) const;
  static bool pageFn(const char* name, bool isDir, uint32_t size, void* user);
  static bool kidFn(const char* name, bool isDir, uint32_t size, void* user);
  static void probeFn(const char* key, const char* value, void* user);
  static void parseFn(const char* key, const char* value, void* user);
  static void sessionProbeFn(const char* key, const char* value, void* user);
};
}
