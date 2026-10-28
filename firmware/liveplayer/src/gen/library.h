// SD library index and shuffle bags (SPEC §6, §9.2). Folders: /lp/midi/<Category>/<Key>/ and /lp/packs/<Genre>/...
// A content folder may carry an `_index.txt` (one basename per LF line, written by tools/write_indexes.py):
// counts and ordinal lookups then stream that file in 4 KB reads instead of walking the directory. A content
// folder (MIDI key folder, drum role folder, vocal key folder) is either FLAT (files, indexed: a leaf) or
// SHARDED (only subfolders s00, s01, ... of <= 64 files, each with its own index; FAT directory operations are
// linear in the folder size), never both. One cache slot describes one TREE: the folder's own files, the entry
// count of every shard and one bag over the whole tree, so a pick weighs a root by its tree total and touches
// one slot per root (two to four slots per pick, SPEC §9.2 working set). Counting is resumable: prepare*()
// spend at most one bounded SD operation per call (one index chunk or one directory walk) so a cold tree is
// warmed over several loop steps; pick*() finish the work themselves when the caller did not prepare.
#pragma once
#include "../core/types.h"
#include "../io/fs.h"
#include "key.h"
#include "variants.h"                         // Rng
namespace lp {
constexpr int kBagMax = 64;                      // ordinals tracked per bag (larger trees are stratified)
constexpr int kHistory = 8;
constexpr int kFolderSlots = 16;                 // cached trees (replacement: cheapest to rebuild first, LRU among equals — Library::folder)
constexpr int kIndexChunk = 4096;                // bytes per _index.txt read (SPEC §9.2)
constexpr int kIndexChunks = 8;                  // chunk table: the first 32 KB of a leaf's index (≈ 750 names)
constexpr int kTreeMax = 12;                     // shard subfolders per content folder; more are reported (Library::lastError)
constexpr int kTreeNameLen = 8;                  // a decoded subfolder name ("s00".."s99", "A#min", "NA", a named shard) and its terminator: folderKidName
constexpr int kFolderPathLen = 56;               // cached folder paths (RAM diet 2026-10-16, was 96): "/lp/packs/<genre>/loops/percussion/A#min" leaves 22 chars for a genre name (vocal / drum / midi roots 32); a longer path is not cached and never picked from
constexpr int kKeyedMax = 64;                    // kick ordinals of one key root cached per scan (SPEC §9.7)
constexpr int kRootsMax = 3;                     // trees weighed by one pick: minor label, relative major label, NA (or a category's NA roots)
constexpr int kLoopRootsMax = kTreeMax + 1;      // a loop pick that ignores the key (SPEC §9.19 Beat / Percussion / Fx): NA plus every key folder the category folder lists
constexpr int kEntryLen = kTreeNameLen + kMaxNameLen;   // an entry relative to its tree root: "sNN/<name>"
constexpr int kGenreNameLen = 24;                // a pack folder name and its terminator (RAM diet 2026-10-16, was kMaxNameLen = 96): a genre of >= 24 chars is not listed by scanGenres (reported through lastError) — its loop paths would not fit kFolderPathLen anyway

// Subfolder codes (RAM diet 2026-10-16): a walked folder remembers each subfolder it keeps as ONE byte, FolderIndex::kids,
// instead of its name. A sharded content folder holds shards s00..s99 (code 0..99, tools/shard_folders.py) and the named
// shards of kNamedShards (kKidNamed + i: tools/make_rap.py's rap/ beside the sNN of a sharded vocal key folder, docs/RAP_BANK.md);
// a listing folder (a category under /lp/midi, a loops/<folder>) holds key folders (kKidKey + k: the 12 minor labels "Amin",
// the 12 major labels "C", then "NA"). Every other subfolder is dropped — unreachable, counted in FolderIndex::kidsDropped and
// reported for a content folder (Library::lastError) — where before any name shorter than kTreeNameLen was kept.
constexpr int kKidShards = 100;                  // codes 0..99: "s00".."s99"
constexpr int kKidKey = kKidShards;              // codes 100..124: key folder k (0..11 minor labels, 12..23 major labels, 24 "NA")
constexpr int kKidKeys = 25;
constexpr int kKidNA = kKidKey + 24;
constexpr int kKidNamed = kKidKey + kKidKeys;    // codes 125..: kNamedShards[i]
constexpr const char* const kNamedShards[] = {"rap"};
constexpr int kNamedShardCount = (int)(sizeof(kNamedShards) / sizeof(kNamedShards[0]));
constexpr uint8_t kKidNone = 255;
int folderKidCode(const char* name);             // the code of a subfolder name, -1 when it is none of the above (dropped by the walk)
void folderKidName(uint8_t code, char out[kTreeNameLen]);   // the name of a code ("" for an unknown one)

// A shuffle bag over ordinals 0..count-1 of one tree: offers each once per cycle, no immediate repeat.
class Bag {
 public:
  void reset(int count);
  int next(Rng& rng);                            // -1 if count == 0
  int count() const { return count_; }
  int cycle() const { return cycle_; }           // refills so far (1 after the first draw): strata rotate through their files with it (wraps at 65535: a stratum's rotation jumps once, still valid)
 private:
  int16_t count_ = 0; uint16_t order_[kBagMax]; int16_t pos_ = 0; int16_t last_ = -1;   // 16-bit (RAM diet 2026-10-16): count_ <= kBagMax, pos_ / last_ / cycleLen_ < count_
  int16_t cycleLen_ = 0;                         // entries in the current cycle (count_ - 1 once a last_ is excluded)
  uint16_t cycle_ = 0;
  void refill(Rng& rng);
};

// One cached tree: the folder's own entries, its shards with their counts, one bag over the tree and how entries
// are resolved. Tree ordinals run over the folder's own files first, then shard by shard in kids order. While an
// index is being counted (source 3 for the leaf's own, 4 for shard kidCur's) chunkNext / partial / midLine carry
// the stream state between steps. 268 B (RAM diet 2026-10-16; was 404 with a 96 B path and 12 × 8 B shard names).
struct FolderIndex {
  char path[kFolderPathLen];
  int count = -1;                                // entries of the tree; -1 = not counted yet (or still counting)
  uint8_t source = 0;                            // 0 unknown, 1 _index.txt (leaf), 2 walked (files + shards), 3 leaf index streaming, 4 shards counting
  uint8_t chunks = 0;                            // valid entries of chunkLine (leaf)
  uint8_t epoch = 0;                             // bumped by every (re)count: a keyed scan of an older epoch is stale
  uint8_t midLine = 0;                           // counting: the line in progress at chunkNext is an entry
  uint16_t chunkNext = 0;                        // counting: next index chunk to stream
  uint16_t partial = 0;                          // counting: entries completed so far
  uint16_t chunkLine[kIndexChunks];              // leaf: entries completed before index chunk c starts
  uint16_t chunkMid = 0;                         // leaf: bit c: chunk c starts inside an entry's line
  uint16_t files = 0;                            // files directly in a walked folder (a leaf's files are `count`)
  uint16_t kidFiles[kTreeMax];                   // entries of each shard
  uint16_t kidIndexed = 0;                       // bit k: shard k is resolved through its _index.txt (else walked)
  uint8_t kidCount = 0;                          // shards remembered (subfolders of a walked content folder)
  uint8_t kidsDropped = 0;                       // subfolders the walk could not keep (beyond kTreeMax, or a name without a code): unreachable
  uint8_t kidCur = 0;                            // counting: the shard in progress
  uint8_t listing = 0;                           // 1: a category folder listed for its key folders (its subfolders are not shards)
  uint8_t kids[kTreeMax];                        // subfolder codes (folderKidCode), in walk order: shards of a content folder, key folders of a listing
  Bag bag;
};

// Loop folder of a category under /lp/packs/<genre>/loops/ (SPEC §9.19, docs/LOOPS.md): Beat → drum, Percussion →
// percussion, Bass → bass, Chord → chord, Lead → lead, Pad → pad, Fx → fx, Vocal → vocal (Generator::loopFolder returns this).
inline const char* loopFolderName(Category c) {
  static const char* n[] = {"chord", "lead", "pad", "bass", "fx", "percussion", "vocal", "drum"};
  return n[c < CategoryCount ? c : 0];
}
// Beat, Percussion and Fx loops ignore the key (drum loops carry song keys the firmware ignores, SPEC §9.19).
inline bool loopIgnoresKey(Category c) { return c == Beat || c == Percussion || c == Fx; }

// Entry enumeration callback (index line or walked file): return false to stop.
typedef bool (*EntryFn)(const char* name, int ordinal, void* user);

class Library {
 public:
  void init(IFileSystem* fs) { fs_ = fs; }
  // Genres
  int scanGenres();                              // lists /lp/packs/* into genres_ (and notes which packs carry a loops/ folder); returns count; a name of >= kGenreNameLen chars is skipped and reported (lastError)
  int genreCount() const { return genreCount_; }
  const char* genreName(int i) const { return genres_[i]; }
  // The pack has a loops/ folder (SPEC §9.19): one isDir per pack at scanGenres (boot), so Generator::setLoopMode refuses a
  // pack without loop content at once and without card work; a pack with the folder is still counted lazily (loopTotal).
  bool genreHasLoops(int i) const { return i >= 0 && i < genreCount_ && ((genreLoops_ >> i) & 1) != 0; }
  // MIDI: picks a file for category in key (minor label, relative major label, NA). Writes full path. Returns false if none.
  bool pickMidi(Category c, const ProjectKey& key, int track, Rng& rng, char* outPath, int cap);
  // Samples: picks from /lp/packs/<genre>/drum/<role>/ (the folder and its shards as one tree); keyRoot >= 0
  // prefers kick files whose key root matches, scanned across the folder's own index and every shard's.
  bool pickSample(const char* genre, const char* role, int keyRoot, Rng& rng, char* outPath, int cap, const char* avoid = nullptr);
  // Vocal: /lp/packs/<genre>/vocal/<Key>/ (minor, major, NA, each a tree); a "loop"-named draw is repeated
  // once (SPEC §6), the BPM token is the caller's check (SPEC §9.3: re-pick untagged names one per step)
  bool pickVocal(const char* genre, const ProjectKey& key, int set, Rng& rng, char* outPath, int cap);
  // Resumable warm-up (SPEC §9.2): each call performs at most ONE bounded SD operation (one index chunk read,
  // or one directory walk of a folder without an index) towards caching every tree the matching pick
  // touches, and returns true once that pick is warm (it then costs one open + at most two 4 KB reads).
  // Call again next loop step while false. A pick made without preparing is still correct; it just finishes
  // the counting itself (over the step budget on a large cold tree).
  bool prepareMidi(Category c, const ProjectKey& key);
  bool prepareSample(const char* genre, const char* role, int keyRoot);
  bool prepareVocal(const char* genre, const ProjectKey& key);
  // Loop mode (SPEC §9.19): /lp/packs/<genre>/loops/<Generator::loopFolder(c)>/<root>[/sNN]. Roots: tonal categories (Bass Chord
  // Lead Pad Vocal) = the key's minor label, its relative major label, NA (as pickVocal); Beat / Percussion / Fx = NA plus every
  // key folder present (drum loops carry song keys the firmware ignores). Shuffle bags and shards as every other picker.
  bool prepareLoop(const char* genre, Category c, const ProjectKey& key);
  bool pickLoop(const char* genre, Category c, const ProjectKey& key, int track, Rng& rng, char* outPath, int cap);
  // Files a pickLoop would weigh, without any card work: -1 while a root is still uncounted (cold), else the tree total
  // (0 = nothing to pick: Generator::setLoopMode refuses LoopNoContent at once when the library already knows).
  int loopTotal(const char* genre, Category c, const ProjectKey& key);
  void invalidate();                             // genre or key change (SPEC §6, §9.9): bags and history restart; counts stay
  void dropCache();                              // forgets every cached count and index table (the card changed)
  // Index-aware folder access (also for tests and set files): cached counts, n-th entry, resolution mode.
  // folderCount is the folder's OWN files (a sharded root holds none: 0), treeCount the files of the whole
  // tree (what a pick weighs); folderEntry returns entry n of the tree relative to the folder ("name", or
  // "sNN/name" for a shard's file).
  int folderCount(const char* path);             // 0 when missing
  bool folderEntry(const char* path, int n, char* out, int cap);
  bool folderUsesIndex(const char* path);        // true when _index.txt drives the folder (a leaf)
  int treeCount(const char* path);
  // The last layout problem seen on the card ("" when none; cleared by dropCache): a content folder whose
  // subfolders exceed kTreeMax or carry names without a code (not sNN, not a named shard), i.e. files no pick can reach.
  const char* lastError() const { return warn_; }
 private:
  enum Fetch : uint8_t { FetchOk = 0, FetchTooLong, FetchMissing };   // entry lookup outcome (SPEC §9.2 index lines)
  enum Draw : uint8_t { DrawOk = 0, DrawRecent, DrawFailed };          // bag draw outcome (SPEC §6 history)
  // Tree ordinals of the kick tree whose key root matches, scanned once per tree count (SPEC §9.7,
  // drum_kit.lua:56): the folder's own entries, then every shard. Valid while the tree keeps the epoch it was
  // scanned under (a recount or a reused cache slot bumps it); a mismatch restarts the scan.
  struct KeyedCache {
    uint32_t treeHash = 0; uint8_t epoch = 0; int8_t root = -1; uint8_t done = 0;
    uint8_t cur = 0;                             // part being scanned: 0 the folder's own entries, 1 + k shard k
    uint16_t chunk = 0, ordinal = 0;             // scan progress: next index chunk of the part, tree ordinal of the next entry
    uint8_t raw = 0, len = 0; char name[kMaxNameLen];   // the line in progress (chars since the first non-blank / trimmed length)
    uint8_t n = 0; uint16_t match[kKeyedMax];    // tree ordinals of every match
  };
  IFileSystem* fs_ = nullptr;
  char genres_[8][kGenreNameLen]; int genreCount_ = 0; uint8_t genreLoops_ = 0;   // bit i: genres_[i] has a loops/ folder (scanGenres); 8 × 24 B (RAM diet 2026-10-16: was 8 × 96 B, −576 B)
  FolderIndex folders_[kFolderSlots]; int folderCount_ = 0;
  uint32_t folderUse_[kFolderSlots] = {}; uint32_t useClock_ = 0;   // recency stamps for folders_ (the tie-break of the replacement)
  uint32_t opStart_ = 0;                         // useClock_ when the current pick / prepare began: its slots (stamped later) are never its own victims
  uint32_t history_[kTracks][kHistory] = {}; uint8_t historyPos_[kTracks] = {};   // last picks per track (SPEC §6)
  KeyedCache keyed_;
  uint8_t opsLeft_ = 255;                        // SD operations this call may still spend (255 = unbounded, pick mode)
  char warn_[48] = "";                           // lastError
  uint8_t buf_[kIndexChunk];                     // one index chunk (never on the stack: frames stay < 2 KB)
  FolderIndex* folder(const char* path, bool content = true);   // cached tree per path (content: index or files + shards; else a listing); count < 0 while cold
  bool countStep(FolderIndex& f);                // bounded steps of counting; true when the count is known
  int streamCount(const char* dir, FolderIndex& f, bool table);   // next chunk(s) of dir/_index.txt: -1 no usable index, 0 more chunks, 1 counted (f.partial)
  void countShardStep(FolderIndex& f);           // one step of counting shard kidCur
  void finishCount(FolderIndex& f, int count, uint8_t source);
  void walkFolder(FolderIndex& f);               // files with the folder's extension + coded subfolders (shards / key folders), one walk
  static void kidPath(char* out, int cap, const FolderIndex& f, int k);   // f.path/<name of kids[k]>
  int walkFiles(const char* dir, const char* ext);   // files of a shard, one walk
  void recount(FolderIndex& f);                  // the tree changed on the card: count again (unbounded)
  void noteDropped(const FolderIndex& f);        // lastError: subfolders of a content folder no pick can reach
  Fetch indexLine(const char* dir, int chunk, int entries, bool skipping, int n, char* out, int cap);   // line n of dir/_index.txt from a known chunk
  Fetch indexEntry(FolderIndex& f, int n, char* out, int cap);   // line n of a leaf through its chunk table
  Fetch walkEntry(const char* dir, const char* ext, int n, char* out, int cap);   // the n-th matching file of a walk
  Fetch entry(FolderIndex& f, int n, char* out, int cap);        // tree ordinal n -> "name" or "sNN/name"
  int partEnd(const FolderIndex& f, int part) const;             // tree ordinal where part (0 own files, 1 + k shard k) ends
  bool keyedStep(FolderIndex& f, int root);      // one bounded step of the keyed kick scan over the tree; true when complete
  int keyedIndexStep(const char* dir);           // -1 index unreadable, 0 more chunks to stream, 1 folder finished
  void keyedWalk(const char* dir, const char* ext);
  void rolePath(char* out, int cap, const char* genre, const char* role);        // /lp/packs/<genre>/drum/<role>
  void keyFolders(const char* base, const ProjectKey& key, FolderIndex** cand, int& n, bool& cold);
  void naFolders(const char* base, FolderIndex** cand, int& n, bool& cold);     // Beat / Percussion roots (SPEC §9.9)
  void allKeyFolders(const char* base, FolderIndex** cand, int& n, bool& cold); // loop roots of a key-less category: NA + every key folder listed (SPEC §9.19)
  void loopRoots(const char* genre, Category c, const ProjectKey& key, FolderIndex** cand, int& n, bool& cold);   // the roots of a loop pick (cand holds kLoopRootsMax)
  static void loopBase(char* out, int cap, const char* genre, Category c);       // /lp/packs/<genre>/loops/<loopFolderName(c)>
  bool pickFromFolders(FolderIndex* const* cand, int n, int track, Rng& rng, char* outPath, int cap);
  bool pickKeyFolders(const char* base, const ProjectKey& key, int track, Rng& rng, char* outPath, int cap);
  Draw resolve(FolderIndex& f, Rng& rng, const uint32_t* recent, char* outPath, int cap, uint32_t& hash);   // bag draw -> entry -> full path
  bool spend();                                  // consumes one SD operation of the call budget; false when exhausted
};
}
