// meehaw live performer — SD library index and shuffle bags (SPEC §6, §9.2, §9.7, §9.9).
// Bag semantics are ported from shuffle_bag.lua; the keyed-kick rule from drum_kit.lua:56. No heap, every
// name goes through caller buffers, every listing is one walk (never a walk inside a walk callback,
// because SdFs::walk keeps its directory handle open while the callback runs).
//
// SD budget (SPEC §9.2): a content folder that carries `_index.txt` is counted by streaming that file in
// 4 KB reads (one per prepare*() call, all of them inside a pick*() that was not prepared) and an ordinal is
// resolved with one 4 KB read (two when the name straddles a chunk edge) through a per-chunk line table. A
// folder without an index is walked once per cache life (its files and shard subfolders in the same walk):
// bounded only by the folder's size, which is why tools/write_indexes.py indexes every media folder. Counts
// survive invalidate(): a key or genre change restarts bags and history without touching the card.
//
// Shards (SPEC §9.2 FAT folder sizes): a content folder holding more than 64 files is split into subfolders
// s00, s01, ... (each indexed) and holds no files of its own. One cache slot holds the whole TREE: the walk of
// the root lists its own files and its shards, each shard's index is then streamed once (one chunk per
// prepare step) for its count, and the tree's ordinals run over the root's files and then shard by shard. A
// draw resolves its ordinal through the chunk table of a leaf, or through the index of the shard it falls in
// (<= 64 names: one read, a second when the name straddles a chunk edge). Every pick — MIDI key folders,
// Beat / Percussion NA roots, drum role folders, vocal key folders — therefore costs one slot per root
// (plus the category folder for Beat / Percussion), and a set generate's working set stays in the cache.
#include "library.h"
#include <string.h>
#include <stdio.h>

namespace lp {
namespace {

constexpr int kPickAttempts = 4;              // draws before the per-track history accepts a recent file (SPEC §6)
constexpr int kResolveTries = 4;              // bag draws before a tree of unresolvable names is given up
constexpr uint8_t kUnbounded = 255;           // opsLeft_ in pick mode: finish the counting in this call
constexpr int kVocalTries = 2;                // draws per pickVocal: a "loop"-named draw is repeated once (SPEC §6)
constexpr int kIndexMaxChunks = 256;          // 1 MB: guards the count loop against a stream without EOF
const char kIndexFile[] = "_index.txt";

// Every slot one pick touches (its roots and, for Beat / Percussion, the category folder) must stay cached
// between the prepare steps of that pick: the LRU evicts the oldest slot, so a pick needing more slots than
// the cache holds would evict its own trees on every step and never warm up. A tree is one slot whatever its
// shard count, so the bound is the number of roots.
static_assert(1 + kRootsMax <= kFolderSlots, "one pick's slots (category folder + roots) must fit the cache");
static_assert(1 + kLoopRootsMax <= kFolderSlots, "a key-less loop pick's slots (category folder + NA + key folders) must fit the cache");
static_assert(kRootsMax <= 32 && kLoopRootsMax <= 32, "pickFromFolders tracks dead candidates in a 32-bit mask");
static_assert(kTreeMax <= 16, "FolderIndex::kidIndexed holds one bit per shard");
static_assert(kTreeNameLen >= 6, "a decoded subfolder name (\"s00\", \"A#min\", a named shard) and its terminator must fit");
static_assert(kKidNamed + kNamedShardCount < 255, "subfolder codes are one byte, 255 = none");
static_assert(kFolderPathLen <= kMaxPathLen, "a cached path is a card path");
static_assert(sizeof(FolderIndex) <= 272, "RAM diet 2026-10-16: 16 slots of <= 272 B keep Library near 10 KB");
static_assert(kGenreNameLen <= kMaxNameLen && kGenreNameLen >= 8, "a genre slot holds a pack folder name (never longer than a card name)");

inline char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(char c) { char l = lower(c); return l >= 'a' && l <= 'z'; }
inline bool isAlnum(char c) { return isDigit(c) || isAlpha(c); }

int ciCompare(const char* a, const char* b) {
  for (;; a++, b++) {
    int d = (int)(uint8_t)lower(*a) - (int)(uint8_t)lower(*b);
    if (d != 0 || *a == 0) return d;
  }
}

const char* baseName(const char* path) {
  const char* base = path;
  for (const char* p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  return base;
}

// The last two components of a folder path ("Beat/NA", "drum/kick"): what a status line can show.
const char* tailName(const char* path) {
  const char* tail = path; const char* prev = path;
  for (const char* p = path; *p; p++) if (*p == '/') { prev = tail; tail = p + 1; }
  return prev;
}

// MIDI folders hold .mid files, everything else under /lp/packs holds .wav one-shots and phrases (SPEC §6).
const char* folderExt(const char* path) { return strstr(path, "/midi/") ? ".mid" : ".wav"; }

// FNV-1a over the folder path and the entry ordinal: the per-track history (SPEC §6: last 8 picks per track
// kind) is checked before the name is read, so a rejected draw costs no SD work. An ordinal names the same
// file for the life of the tree's count.
uint32_t hashEntry(const char* path, int ordinal) {
  uint32_t h = 2166136261u;
  for (; *path; path++) { h ^= (uint8_t)*path; h *= 16777619u; }
  for (int i = 0; i < 4; i++) { h ^= (uint8_t)(ordinal >> (8 * i)); h *= 16777619u; }
  return h;
}

bool historyHas(const uint32_t* h, uint32_t v) {
  for (int i = 0; i < kHistory; i++) if (h[i] == v) return true;
  return false;
}

// a + "/" + b into out, refusing (empty out) when the joined path would not fit: a truncated path would
// name a file that does not exist.
bool joinChecked(char* out, int cap, const char* a, const char* b) {
  int need = (int)strlen(a) + 1 + (int)strlen(b) + 1;
  if (need > cap) { if (cap > 0) out[0] = 0; return false; }
  joinPath(out, cap, a, b);
  return true;
}

// Bag ordinals cover at most kBagMax entries. A larger tree (Beat/NA holds 748) is split into kBagMax
// strata of 11-12 files: the bag cycles over the strata and each stratum rotates through its files with the
// bag's cycle count, so every file comes up exactly once in ceil(count / kBagMax) cycles (a 66-file tree in
// two). Consecutive draws never repeat (different strata); files in smaller strata are favoured a little.
int entryOrdinal(int bagOrdinal, int bagCount, int count, int cycle) {
  if (count <= bagCount) return bagOrdinal;
  int lo = (bagOrdinal * count) / bagCount;                  // < 128 * 65535, fits an int
  int hi = ((bagOrdinal + 1) * count) / bagCount;            // exclusive
  return lo + (cycle + bagOrdinal) % (hi - lo);
}

// Reads a whole chunk unless the file ends first: a short read is only ever the tail of the file, so one
// read call per chunk reaches the card (SPEC §9.2 counts them).
size_t fillChunk(IFile& file, uint8_t* buf) {
  size_t n = 0;
  while (n < (size_t)kIndexChunk) {
    const size_t want = (size_t)kIndexChunk - n;
    const size_t got = file.read(buf + n, want);
    n += got;
    if (got < want) break;
  }
  return n;
}

// drum_kit.lua:56 kick_key_root: the Reaper label before "__" (key.lua) or the compact one-shot forms
// "A_Kick", "F#-Kick", "Bb Kick" (SPEC §9.7). Case-insensitive like the Lua, which lowercases first.
int kickKeyRoot(const char* name) {
  LibraryKey k = parseLibraryFilename(name);
  if (k.valid) return k.keyless ? -1 : k.root;
  const char* base = baseName(name);
  int root;
  switch (lower(base[0])) {                                  // drum_kit.lua:54 NATURAL_ROOT
    case 'c': root = 0; break; case 'd': root = 2; break; case 'e': root = 4; break; case 'f': root = 5; break;
    case 'g': root = 7; break; case 'a': root = 9; break; case 'b': root = 11; break;
    default: return -1;
  }
  const char* p = base + 1;
  if (*p == '#') { root += 1; p++; } else if (*p == 'b') { root -= 1; p++; }   // drum_kit.lua:62 "[#b]?"
  if (*p != '_' && *p != '-' && *p != ' ') return -1;                            // "[_%-%s]" must follow
  return ((root % 12) + 12) % 12;
}

// SPEC §6: a vocal name must have no "loop" token (alphanumeric words, case-insensitive).
bool hasLoopToken(const char* name) {
  const char* p = name;
  while (*p) {
    while (*p && !isAlnum(*p)) p++;
    const char* start = p;
    while (*p && isAlnum(*p)) p++;
    size_t len = (size_t)(p - start);
    if (len == 4 || len == 5) {
      char w[6] = {0};
      for (size_t i = 0; i < len; i++) w[i] = lower(start[i]);
      if (strcmp(w, "loop") == 0 || strcmp(w, "loops") == 0) return true;
    }
  }
  return false;
}

// ---- walk / entry callbacks ----------------------------------------------------------------------------

struct GenreList { char (*names)[kGenreNameLen]; int n; int cap; int tooLong; };   // tooLong: pack folders skipped for a name of >= kGenreNameLen chars

// Keeps the alphabetically (case-insensitive) first `cap` directory names, sorted, whatever the walk order. A name that
// does not fit a genre slot (kGenreNameLen, RAM diet 2026-10-16) is skipped and counted: such a pack is unreachable, as
// its loop folders would not fit a cached path either (kFolderPathLen).
bool genreFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  GenreList& g = *static_cast<GenreList*>(user);
  if (!isDir) return true;
  if ((int)strlen(name) >= kGenreNameLen) { g.tooLong++; return true; }
  int pos = 0;
  while (pos < g.n && ciCompare(g.names[pos], name) <= 0) pos++;
  if (pos >= g.cap) return true;                             // sorts after every kept name
  int last = g.n < g.cap ? g.n : g.cap - 1;                  // the last kept slot (dropped when full)
  for (int i = last; i > pos; i--) memmove(g.names[i], g.names[i - 1], kGenreNameLen);   // rows never overlap
  strcpy(g.names[pos], name);
  if (g.n < g.cap) g.n++;
  return true;
}

// One walk of a folder without an index: counts the files with the folder's extension and remembers its
// first kTreeMax subfolders that have a code (folderKidCode: shards sNN and the named shards of a content folder,
// the key folders of a listing — one byte each, library.h); every other subfolder is counted as dropped so the
// layout problem can be reported. Walking is the unindexed fallback and looks at every entry; a conforming card
// indexes every media folder.
struct WalkCount { const char* ext; int files; uint8_t* kids; int kidCount; int dropped; };

bool walkCountFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  WalkCount& w = *static_cast<WalkCount*>(user);
  if (isDir) {
    if (!w.kids) return true;                                // a shard: its own subfolders are not looked at
    const int code = folderKidCode(name);
    if (code >= 0 && w.kidCount < kTreeMax) w.kids[w.kidCount++] = (uint8_t)code;
    else w.dropped++;
  } else if (hasExt(name, w.ext)) {
    w.files++;
  }
  return true;
}

// The n-th file of a walked folder, telling a missing ordinal from a name that does not fit the buffer.
struct WalkEntry { const char* ext; int want; int seen; char* out; int cap; int result; };

bool walkEntryFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  WalkEntry& w = *static_cast<WalkEntry*>(user);
  if (isDir || !hasExt(name, w.ext)) return true;
  if (w.seen++ != w.want) return true;
  if ((int)strlen(name) < w.cap) { strcpy(w.out, name); w.result = 0; } else w.result = 1;   // FetchOk / FetchTooLong
  return false;
}

// Keyed kick scan over a walked folder (unindexed fallback): tree ordinals (from `ordinal` on) whose kick key
// root matches.
struct KeyedWalk { const char* ext; int root; int ordinal; uint8_t* n; uint16_t* match; };

bool keyedWalkFn(const char* name, bool isDir, uint32_t size, void* user) {
  (void)size;
  KeyedWalk& k = *static_cast<KeyedWalk*>(user);
  if (isDir || !hasExt(name, k.ext)) return true;
  const int o = k.ordinal++;
  if (kickKeyRoot(name) == k.root && *k.n < kKeyedMax && (int)strlen(name) < kMaxNameLen) k.match[(*k.n)++] = (uint16_t)o;
  return true;
}

bool sameBase(const char* path, const char* avoid) {
  return avoid && avoid[0] && strcmp(baseName(path), baseName(avoid)) == 0;
}

// Index lines (SPEC §9.2): an entry is a line holding at least one character other than space, tab or CR;
// its name is the line trimmed of leading and trailing blanks (interior blanks stay: "Bb Kick.wav").
inline bool isBlank(uint8_t c) { return c == ' ' || c == '\t' || c == '\r'; }

inline uint16_t clamp16(int v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

} // namespace

// ---- Subfolder codes (FolderIndex::kids, library.h) ---------------------------------------------------------

// "sNN" → NN; "NA" → kKidNA; a minor label "Amin" → kKidKey + root; a major label "C" → kKidKey + 12 + root; a named
// shard → kKidNamed + i; anything else -1 (the walk drops it). The labels are those of ProjectKey::labels (key.cpp).
int folderKidCode(const char* name) {
  if (!name || !name[0]) return -1;
  if (name[0] == 's' && isDigit(name[1]) && isDigit(name[2]) && name[3] == 0) return (name[1] - '0') * 10 + (name[2] - '0');
  if (strcmp(name, "NA") == 0) return kKidNA;
  for (int r = 0; r < 12; r++) {
    const char* note = noteName(r);
    const size_t nl = strlen(note);
    if (strncmp(name, note, nl) != 0) continue;              // "C" also prefixes "C#": the loop goes on to the sharp
    if (name[nl] == 0) return kKidKey + 12 + r;              // the major label
    if (strcmp(name + nl, "min") == 0) return kKidKey + r;   // the minor label
  }
  for (int i = 0; i < kNamedShardCount; i++) if (strcmp(name, kNamedShards[i]) == 0) return kKidNamed + i;
  return -1;
}

void folderKidName(uint8_t code, char out[kTreeNameLen]) {
  out[0] = 0;
  if (code < kKidShards) { out[0] = 's'; out[1] = (char)('0' + code / 10); out[2] = (char)('0' + code % 10); out[3] = 0; return; }
  if (code == kKidNA) { strcpy(out, "NA"); return; }
  if (code < kKidKey + 12) { strcpy(out, noteName(code - kKidKey)); strcat(out, "min"); return; }   // <= 2 + 3 chars
  if (code < kKidKey + 24) { strcpy(out, noteName(code - kKidKey - 12)); return; }
  if (code >= kKidNamed && code < kKidNamed + kNamedShardCount) {
    const char* s = kNamedShards[code - kKidNamed];
    int i = 0;
    for (; i < kTreeNameLen - 1 && s[i]; i++) out[i] = s[i];
    out[i] = 0;
  }
}

// ---- Bag ----------------------------------------------------------------------------------------------

void Bag::reset(int count) {
  count_ = (int16_t)(count < 0 ? 0 : (count > kBagMax ? kBagMax : count));
  pos_ = 0; cycleLen_ = 0; last_ = -1; cycle_ = 0;           // empty cycle: the first next() refills
}

// shuffle_bag.lua:107 _refill: every ordinal except the last returned one (when alternatives exist),
// then a Fisher-Yates shuffle (shuffle_bag.lua:99).
void Bag::refill(Rng& rng) {
  int n = count_;
  for (int i = 0; i < n; i++) order_[i] = (uint16_t)i;
  if (n > 1 && last_ >= 0 && last_ < n) { order_[last_] = order_[n - 1]; n--; }
  for (int i = n - 1; i > 0; i--) {
    int j = rng.range(0, i);
    uint16_t t = order_[i]; order_[i] = order_[j]; order_[j] = t;
  }
  cycleLen_ = (int16_t)n;
  pos_ = 0;
  cycle_++;
}

int Bag::next(Rng& rng) {
  if (count_ <= 0) return -1;
  if (pos_ >= cycleLen_) refill(rng);                        // shuffle_bag.lua:124
  last_ = (int16_t)order_[pos_++];
  return last_;
}


// ---- Library: tree cache --------------------------------------------------------------------------------

// One SD operation of the call budget: pick*() run unbounded, prepare*() allow a single one (SPEC §9.2).
bool Library::spend() {
  if (opsLeft_ == kUnbounded) return true;
  if (opsLeft_ == 0) return false;
  opsLeft_--;
  return true;
}

// SD steps it takes to count a slot again: one per index chunk of a leaf, the walk plus one per shard of a
// tree (an absent folder: its one failed walk).
int rebuildCost(const FolderIndex& f) { return f.source == 1 ? (f.chunks > 0 ? f.chunks : 1) : 1 + f.kidCount; }

// Cached tree (count, shards, bag) per folder path. The count is taken on first use (SPEC §6 "cached on first
// use") and survives invalidate(); while the budget of a prepare call is spent the entry comes back with
// count < 0 ("cold") and no SD work is done. A path of kFolderPathLen or more characters is refused (nullptr):
// the slot cannot hold it.
// Replacement: the slot that is CHEAPEST to rebuild (rebuildCost: an absent folder or a flat leaf of one index
// chunk = 1 step, a listing folder = its walk + 1, a sharded tree = its walk + one step per shard, Beat/NA 13),
// the least recently used among equals. The trees one set generate touches (29 on the reference card, SPEC §9.5
// track order; 18 of them sharded since the 2026-10-16 content wave) do not fit the cache, so plain LRU would
// stream every sharded tree again on every generate (a sequential scan over more slots than the cache holds
// evicts each tree right before it is needed again); cheapest-first keeps the expensive trees resident, lets the
// flat folders cycle through the one or two slots the cheapest trees leave, and re-streams only the few cheapest
// sharded trees that do not fit beside them (test_library sd_budget_sharded_trees_survive_a_set_generate counts
// the steps). The former policy — LRU among the cheap slots while at least 1 + kRootsMax of them exist, else
// plain LRU — fell back to the plain LRU exactly when the sharded trees had filled the cache. A slot the CURRENT
// operation already touched (stamped after opStart_) is never its own victim: a key-less loop pick weighs up to
// kLoopRootsMax + 1 trees (SPEC §9.19) and would otherwise evict its first root while adding its last one, leaving
// a candidate pointer at a reused slot; with every slot pinned (impossible under the static_asserts) nullptr is
// returned.
FolderIndex* Library::folder(const char* path, bool content) {
  if (!fs_ || !path || (int)strlen(path) >= kFolderPathLen) return nullptr;
  int slot = -1;
  for (int i = 0; i < folderCount_; i++) if (strcmp(folders_[i].path, path) == 0) { slot = i; break; }
  if (slot < 0) {
    if (folderCount_ < kFolderSlots) {
      slot = folderCount_++;
    } else {
      int best = 0;
      for (int i = 0; i < kFolderSlots; i++) {
        if (folderUse_[i] > opStart_) continue;                   // pinned: touched by the operation in progress
        const int cost = rebuildCost(folders_[i]);
        if (slot < 0 || cost < best || (cost == best && folderUse_[i] < folderUse_[slot])) { slot = i; best = cost; }
      }
      if (slot < 0) return nullptr;
    }
    FolderIndex& f = folders_[slot];
    strcpy(f.path, path);
    f.count = -1; f.source = 0; f.chunks = 0; f.chunkMid = 0; f.chunkNext = 0; f.partial = 0; f.midLine = 0;
    f.files = 0; f.kidCount = 0; f.kidsDropped = 0; f.kidCur = 0; f.kidIndexed = 0; f.listing = content ? 0 : 1;
    f.epoch++;
  }
  FolderIndex& f = folders_[slot];
  if (f.count < 0) countStep(f);
  folderUse_[slot] = ++useClock_;
  return &f;
}

// Steps towards the tree's count: one bounded SD operation per step in prepare mode (the next chunk of an
// index, or one directory walk), all of them in pick mode. A content folder tries its own index first (a
// leaf); without a usable one it is walked for its files and shards, and every shard is then counted in turn
// (its index chunk by chunk, or its walk). A listing folder (a category, never indexed) is only walked.
// Returns true once the count is known.
bool Library::countStep(FolderIndex& f) {
  for (;;) {
    if (f.count >= 0) return true;
    if (f.source == 4 && f.kidCur >= f.kidCount) {           // every shard counted: the tree total
      int total = f.files;
      for (int k = 0; k < f.kidCount; k++) total += f.kidFiles[k];
      finishCount(f, total, 2);
      return true;
    }
    if (!spend()) return false;
    if (f.source == 4) { countShardStep(f); continue; }
    if (!f.listing) {                                        // source 0 or 3: the folder's own index
      const int r = streamCount(f.path, f, true);
      if (r > 0) { finishCount(f, f.partial, 1); return true; }
      if (r == 0) { f.source = 3; continue; }                // more chunks next step
    }
    walkFolder(f);                                           // no index, an empty one, or the file vanished
    if (f.listing || f.kidCount == 0) { finishCount(f, f.files, 2); return true; }
    f.source = 4; f.kidCur = 0;                              // the shards follow, one step each
  }
}

// Streams dir/_index.txt from f.chunkNext in kIndexChunk reads (one per call in prepare mode, all of them in
// pick mode), counting entries into f.partial with f.midLine carrying a line split at the chunk edge. With
// `table` (a leaf's own index) every chunk's first-entry ordinal and mid-line flag are recorded, which is what
// indexEntry needs to seek straight to the chunk holding line n. -1 = no usable index (absent, unreadable or
// without a single entry): the caller walks instead; 0 = more chunks follow next step; 1 = counted.
int Library::streamCount(const char* dir, FolderIndex& f, bool table) {
  char p[kMaxPathLen];
  if (!joinChecked(p, sizeof p, dir, kIndexFile)) return -1;
  IFile* file = fs_->open(p, "r");
  if (!file) return -1;
  if (!file->seek((uint32_t)f.chunkNext * kIndexChunk)) { file->close(); return -1; }
  const int maxChunks = opsLeft_ == kUnbounded ? kIndexMaxChunks : 1;
  int entries = f.partial;
  bool inEntry = f.midLine != 0, eof = false;
  for (int k = 0; k < maxChunks && !eof && f.chunkNext < kIndexMaxChunks; k++) {
    const size_t got = fillChunk(*file, buf_);
    if (got == 0) { eof = true; break; }
    if (table && f.chunkNext < kIndexChunks) {
      f.chunkLine[f.chunkNext] = clamp16(entries);
      if (inEntry) f.chunkMid |= (uint16_t)(1u << f.chunkNext); else f.chunkMid &= (uint16_t)~(1u << f.chunkNext);
      f.chunks = (uint8_t)(f.chunkNext + 1);
    }
    for (size_t i = 0; i < got; i++) {
      const uint8_t ch = buf_[i];
      if (ch == '\n') { if (inEntry) entries++; inEntry = false; }
      else if (!isBlank(ch)) inEntry = true;
    }
    f.chunkNext++;
    eof = got < (size_t)kIndexChunk;                         // a short read is only ever the tail of the file
  }
  file->close();
  if (!eof && f.chunkNext < kIndexMaxChunks) {               // more to stream next step
    f.partial = clamp16(entries); f.midLine = inEntry ? 1 : 0;
    return 0;
  }
  if (inEntry) entries++;                                    // last line without a newline
  f.chunkNext = 0; f.midLine = 0;
  if (entries <= 0) { f.partial = 0; if (table) { f.chunks = 0; f.chunkMid = 0; } return -1; }
  f.partial = clamp16(entries);
  return 1;
}

// One step of counting shard kidCur: the next chunk of its index, or (without a usable index) its single
// walk; the shard's count and resolution mode are recorded and the next shard follows.
void Library::countShardStep(FolderIndex& f) {
  char dir[kMaxPathLen];
  kidPath(dir, sizeof dir, f, f.kidCur);
  const uint16_t bit = (uint16_t)(1u << f.kidCur);
  const int r = streamCount(dir, f, false);
  if (r == 0) return;                                        // more chunks next step
  int n;
  if (r > 0) { n = f.partial; f.kidIndexed |= bit; }
  else { n = walkFiles(dir, folderExt(f.path)); f.kidIndexed &= (uint16_t)~bit; }
  f.kidFiles[f.kidCur++] = clamp16(n);
  f.chunkNext = 0; f.partial = 0; f.midLine = 0;
}

void Library::finishCount(FolderIndex& f, int count, uint8_t source) {
  f.count = count; f.source = source; f.epoch++;
  f.chunkNext = 0; f.partial = 0; f.midLine = 0;
  if (source == 1) { f.kidCount = 0; f.kidIndexed = 0; f.files = 0; }
  f.bag.reset(f.count);
}

// The directory walk (SPEC §6): files with the folder's extension are the folder's own entries, in walk
// order; the subfolders seen are its shards. A missing folder counts 0.
void Library::walkFolder(FolderIndex& f) {
  WalkCount w = {folderExt(f.path), 0, f.kids, 0, 0};
  if (!fs_->walk(f.path, walkCountFn, &w)) { w.files = 0; w.kidCount = 0; w.dropped = 0; }
  f.files = clamp16(w.files); f.kidCount = (uint8_t)w.kidCount; f.kidsDropped = (uint8_t)(w.dropped > 255 ? 255 : w.dropped);
  f.kidIndexed = 0; f.kidCur = 0; f.chunks = 0; f.chunkMid = 0; f.chunkNext = 0; f.partial = 0; f.midLine = 0;
  if (f.kidsDropped && !f.listing) noteDropped(f);
}

int Library::walkFiles(const char* dir, const char* ext) {
  WalkCount w = {ext, 0, nullptr, 0, 0};
  if (!fs_->walk(dir, walkCountFn, &w)) return 0;
  return w.files;
}

// f.path/<name of shard or key folder k> (the code decoded into a kTreeNameLen buffer).
void Library::kidPath(char* out, int cap, const FolderIndex& f, int k) {
  char kid[kTreeNameLen];
  folderKidName(f.kids[k], kid);
  joinPath(out, cap, f.path, kid);
}

// A content folder with subfolders beyond kTreeMax, or named without a code (not sNN, not a named shard): their
// files are unreachable (tools/write_indexes.py warns). Kept for the status line / serial `stat`.
void Library::noteDropped(const FolderIndex& f) {
  snprintf(warn_, sizeof warn_, "%.23s: %d shards unreachable", tailName(f.path), (int)f.kidsDropped);   // 23 + 24 = 47 chars: fits warn_ (GCC -Wformat-truncation clean)
}

// The tree changed on the card (a line vanished): count again in this call, whatever the mode.
void Library::recount(FolderIndex& f) {
  const uint8_t saved = opsLeft_;
  opsLeft_ = kUnbounded;
  f.count = -1; f.source = 0; f.chunks = 0; f.chunkMid = 0; f.chunkNext = 0; f.partial = 0; f.midLine = 0;
  f.files = 0; f.kidCount = 0; f.kidIndexed = 0; f.kidCur = 0;
  countStep(f);
  opsLeft_ = saved;
}

// Line n of dir/_index.txt, scanning forward from chunk `chunk`, before which `entries` lines ended (and whose
// first line is a continuation when `skipping`). One 4 KB read, a second only when the name straddles the
// chunk edge. Trimmed of blanks; a name that does not fit `cap` is FetchTooLong (a permanent property of that
// line, no recount), a line that is not there FetchMissing.
Library::Fetch Library::indexLine(const char* dir, int chunk, int entries, bool skipping, int n, char* out, int cap) {
  char p[kMaxPathLen];
  if (!joinChecked(p, sizeof p, dir, kIndexFile)) return FetchMissing;
  IFile* file = fs_->open(p, "r");
  if (!file) return FetchMissing;
  if (!file->seek((uint32_t)chunk * kIndexChunk)) { file->close(); return FetchMissing; }
  int raw = 0, len = 0;                                      // chars since the first non-blank / trimmed length
  bool found = false;
  while (!found) {
    const size_t got = fillChunk(*file, buf_);
    if (got == 0) { found = !skipping && len > 0 && entries == n; break; }   // last line without LF
    for (size_t i = 0; i < got && !found; i++) {
      const uint8_t ch = buf_[i];
      if (ch == '\n') {
        if (skipping || len > 0) { if (entries == n) { found = true; break; } entries++; }
        skipping = false; raw = 0; len = 0;
      } else if (skipping || (isBlank(ch) && raw == 0)) {
        continue;                                            // not ours, or a leading blank
      } else {
        if (entries == n && raw < cap - 1) out[raw] = (char)ch;
        raw++;
        if (!isBlank(ch)) len = raw;
      }
    }
  }
  file->close();
  if (!found) { out[0] = 0; return FetchMissing; }
  if (len > cap - 1) { out[0] = 0; return FetchTooLong; }
  out[len] = 0;
  return FetchOk;
}

// Line n of a leaf's index: seek to the last tabled chunk whose first complete entry is at or before n (a
// chunk that opens inside entry n's own line is passed over for the previous one, so the name is read from its
// first character), then scan forward; a line beyond the table streams on from its last chunk.
Library::Fetch Library::indexEntry(FolderIndex& f, int n, char* out, int cap) {
  if (n < 0 || n >= f.count) return FetchMissing;
  int c = 0;
  for (int i = f.chunks - 1; i > 0; i--) {
    const bool mid = (f.chunkMid >> i) & 1;
    if (f.chunkLine[i] < n || (f.chunkLine[i] == n && !mid)) { c = i; break; }
  }
  const int entries = c < f.chunks ? f.chunkLine[c] : 0;
  return indexLine(f.path, c, entries, ((f.chunkMid >> c) & 1) != 0, n, out, cap);
}

Library::Fetch Library::walkEntry(const char* dir, const char* ext, int n, char* out, int cap) {
  WalkEntry w = {ext, n, 0, out, cap, 2};
  fs_->walk(dir, walkEntryFn, &w);
  if (w.result != 0) out[0] = 0;
  return w.result == 0 ? FetchOk : w.result == 1 ? FetchTooLong : FetchMissing;
}

// Tree ordinal n of a folder, relative to it: a leaf's line n (or the n-th matching file of its walk), the
// folder's own n-th file, or "sNN/<name>" from the shard the ordinal falls in (its index streamed from the
// start — at most two reads for a shard of <= 64 names — or its walk).
Library::Fetch Library::entry(FolderIndex& f, int n, char* out, int cap) {
  if (!out || cap <= 0) return FetchMissing;
  out[0] = 0;
  if (n < 0 || n >= f.count) return FetchMissing;
  if (f.source == 1) return indexEntry(f, n, out, cap);
  const char* ext = folderExt(f.path);
  if (n < f.files) return walkEntry(f.path, ext, n, out, cap);
  n -= f.files;
  for (int k = 0; k < f.kidCount; k++) {
    if (n >= f.kidFiles[k]) { n -= f.kidFiles[k]; continue; }
    char kid[kTreeNameLen];
    folderKidName(f.kids[k], kid);
    const int pre = (int)strlen(kid) + 1;                    // "sNN/"
    if (pre + 1 >= cap) return FetchTooLong;
    char dir[kMaxPathLen];
    joinPath(dir, sizeof dir, f.path, kid);
    memcpy(out, kid, (size_t)pre - 1); out[pre - 1] = '/'; out[pre] = 0;
    const Fetch r = ((f.kidIndexed >> k) & 1) ? indexLine(dir, 0, 0, false, n, out + pre, cap - pre) : walkEntry(dir, ext, n, out + pre, cap - pre);
    if (r != FetchOk) out[0] = 0;
    return r;
  }
  return FetchMissing;
}

// Tree ordinal where a part ends: part 0 is the folder's own files, part 1 + k is shard k.
int Library::partEnd(const FolderIndex& f, int part) const {
  int end = f.source == 1 ? f.count : f.files;
  for (int k = 0; k < part && k < f.kidCount; k++) end += f.kidFiles[k];
  return end;
}

// ---- Library: keyed kick scan (SPEC §9.7) -------------------------------------------------------------

// Tree ordinals of the kick tree whose key root matches, kept in keyed_ for as long as the tree's count stands
// (the root only changes through the key, which also restarts the bags). The parts are visited in order —
// the folder's own entries (its index, or the walk of its files), then every shard (index or walk) — one
// bounded step per call in prepare mode (one index chunk, or one walk); returns true once the list is
// complete. A part whose index cannot be read any more contributes nothing (its draws fall back to the bag,
// where a missing line recounts the tree).
bool Library::keyedStep(FolderIndex& f, int root) {
  KeyedCache& k = keyed_;
  const uint32_t treeHash = hashEntry(f.path, -1);
  if (k.treeHash != treeHash || k.epoch != f.epoch || k.root != root) {
    k.treeHash = treeHash; k.epoch = f.epoch; k.root = (int8_t)root;
    k.done = 0; k.n = 0; k.cur = 0; k.chunk = 0; k.ordinal = 0; k.raw = 0; k.len = 0;
  }
  const int parts = f.source == 1 ? 1 : 1 + f.kidCount;
  while (!k.done) {
    if (k.cur >= parts) { k.done = 1; break; }
    if (k.cur == 0 && f.source != 1 && f.files == 0) { k.cur++; continue; }   // a sharded root without files of its own: nothing to walk
    if (!spend()) return false;
    char dir[kMaxPathLen];
    bool indexed;
    if (k.cur == 0) { strcpy(dir, f.path); indexed = f.source == 1; }
    else { kidPath(dir, sizeof dir, f, k.cur - 1); indexed = ((f.kidIndexed >> (k.cur - 1)) & 1) != 0; }
    bool finished = true;
    if (indexed) finished = keyedIndexStep(dir) != 0;        // -1: index gone, its names are given up; 1: streamed to the end
    else keyedWalk(dir, folderExt(f.path));
    if (finished) { k.ordinal = clamp16(partEnd(f, k.cur)); k.cur++; k.chunk = 0; k.raw = 0; k.len = 0; }
  }
  return true;
}

// Streams dir/_index.txt from keyed_.chunk (one chunk per call in prepare mode), continuing the line in
// progress across chunk edges so every name is examined once. Names of kMaxNameLen or more are skipped
// (entry() cannot return them either). -1 when the index cannot be opened, 0 while more chunks follow, 1 at
// the end of the file.
int Library::keyedIndexStep(const char* dir) {
  char p[kMaxPathLen];
  if (!joinChecked(p, sizeof p, dir, kIndexFile)) return -1;
  IFile* file = fs_->open(p, "r");
  if (!file) return -1;
  if (!file->seek((uint32_t)keyed_.chunk * kIndexChunk)) { file->close(); return -1; }
  const int maxChunks = opsLeft_ == kUnbounded ? kIndexMaxChunks : 1;
  KeyedCache& k = keyed_;
  bool eof = false;
  auto endLine = [&]() {
    if (k.len > 0) {
      if (k.len < kMaxNameLen) {
        k.name[k.len] = 0;
        if (kickKeyRoot(k.name) == k.root && k.n < kKeyedMax) k.match[k.n++] = k.ordinal;
      }
      if (k.ordinal < 65535) k.ordinal++;
    }
    k.raw = 0; k.len = 0;
  };
  for (int c = 0; c < maxChunks && !eof && k.chunk < kIndexMaxChunks; c++) {
    const size_t got = fillChunk(*file, buf_);
    if (got == 0) { eof = true; break; }
    for (size_t i = 0; i < got; i++) {
      const uint8_t ch = buf_[i];
      if (ch == '\n') { endLine(); continue; }
      if (isBlank(ch) && k.raw == 0) continue;               // leading blank
      if (k.raw < kMaxNameLen - 1) k.name[k.raw] = (char)ch;
      if (k.raw < 255) k.raw++;
      if (!isBlank(ch)) k.len = k.raw;
    }
    k.chunk++;
    eof = got < (size_t)kIndexChunk;
  }
  file->close();
  if (!eof && k.chunk < kIndexMaxChunks) return 0;           // more next step
  endLine();                                                 // last line without a newline
  return 1;
}

void Library::keyedWalk(const char* dir, const char* ext) {
  KeyedWalk k = {ext, keyed_.root, keyed_.ordinal, &keyed_.n, keyed_.match};
  fs_->walk(dir, keyedWalkFn, &k);
}

// ---- Library: candidate trees ---------------------------------------------------------------------------

// Tonal trees under base: the key's minor label, its relative major label and NA (SPEC §2, §6), each one
// slot. A tree whose count is still unknown (prepare budget spent) sets `cold`.
void Library::keyFolders(const char* base, const ProjectKey& key, FolderIndex** cand, int& n, bool& cold) {
  char minorLabel[8], majorLabel[8];
  key.labels(minorLabel, majorLabel);
  const char* labels[kRootsMax] = {minorLabel, majorLabel, "NA"};
  char path[kMaxPathLen];
  for (int i = 0; i < kRootsMax; i++) {
    joinPath(path, sizeof path, base, labels[i]);
    FolderIndex* f = folder(path);
    if (!f) continue;
    if (f->count < 0) cold = true;
    else if (f->count > 0) cand[n++] = f;
  }
}

void Library::rolePath(char* out, int cap, const char* genre, const char* role) {
  joinPath(out, cap, "/lp/packs", genre);
  joinPath(out, cap, out, "drum");
  joinPath(out, cap, out, role);
}

// Beat and Percussion ignore the key (SPEC §9.9): the category's subfolders (NA on a conforming card, at most
// kRootsMax of them, found by one walk of the category folder) are the roots, each one tree.
void Library::naFolders(const char* base, FolderIndex** cand, int& n, bool& cold) {
  FolderIndex* category = folder(base, false);
  if (!category) return;
  if (category->count < 0) { cold = true; return; }
  uint8_t kids[kTreeMax];
  memcpy(kids, category->kids, sizeof kids);                 // a private copy: folder() below may reuse slots
  const int kc = category->kidCount < kRootsMax ? category->kidCount : kRootsMax;
  char path[kMaxPathLen], kid[kTreeNameLen];
  for (int i = 0; i < kc; i++) {
    folderKidName(kids[i], kid);
    joinPath(path, sizeof path, base, kid);
    FolderIndex* f = folder(path);
    if (!f) continue;
    if (f->count < 0) cold = true;
    else if (f->count > 0) cand[n++] = f;
  }
}

// Loop roots (SPEC §9.19): a key-less category (Beat / Percussion / Fx) weighs NA plus every key folder the category
// folder lists. NA is addressed by name, so it is reached even when the listing dropped it (kTreeMax kids); the
// others come from the one walk of the category folder (a listing slot, as naFolders). A folder at kTreeMax + 1 key
// folders besides NA would lose one of them: tools/write_indexes.py warns at the limit.
void Library::allKeyFolders(const char* base, FolderIndex** cand, int& n, bool& cold) {
  char path[kMaxPathLen];
  joinPath(path, sizeof path, base, "NA");
  if (FolderIndex* f = folder(path)) { if (f->count < 0) cold = true; else if (f->count > 0) cand[n++] = f; }
  FolderIndex* category = folder(base, false);
  if (!category) return;
  if (category->count < 0) { cold = true; return; }
  uint8_t kids[kTreeMax];
  memcpy(kids, category->kids, sizeof kids);                 // a private copy: folder() below may reuse slots
  const int kc = category->kidCount;
  char kid[kTreeNameLen];
  for (int i = 0; i < kc && n < kLoopRootsMax; i++) {
    if (kids[i] == kKidNA) continue;
    folderKidName(kids[i], kid);
    joinPath(path, sizeof path, base, kid);
    FolderIndex* f = folder(path);
    if (!f) continue;
    if (f->count < 0) cold = true;
    else if (f->count > 0) cand[n++] = f;
  }
}

void Library::loopBase(char* out, int cap, const char* genre, Category c) {
  joinPath(out, cap, "/lp/packs", genre);
  joinPath(out, cap, out, "loops");
  joinPath(out, cap, out, loopFolderName(c));
}

// The roots one loop pick weighs: tonal categories (Bass Chord Lead Pad Vocal) the key's minor label, its relative
// major label and NA (as pickVocal); Beat / Percussion / Fx NA plus every key folder present.
void Library::loopRoots(const char* genre, Category c, const ProjectKey& key, FolderIndex** cand, int& n, bool& cold) {
  char base[kMaxPathLen];
  loopBase(base, sizeof base, genre, c);
  if (loopIgnoresKey(c)) allKeyFolders(base, cand, n, cold);
  else keyFolders(base, key, cand, n, cold);
}

// ---- Library: picks ----------------------------------------------------------------------------------

// One pick from a tree: bag ordinal -> tree ordinal -> (history) -> entry -> full path. A draw the track made
// recently is reported as DrawRecent before anything is read (the caller decides). A name that cannot be
// returned (too long for the buffers) is a permanent property of that line: the draw is simply repeated.
// A line that is not there means the tree changed on the card: it is recounted once and the draw repeated.
Library::Draw Library::resolve(FolderIndex& f, Rng& rng, const uint32_t* recent, char* outPath, int cap, uint32_t& hash) {
  char name[kEntryLen];
  bool recounted = false;
  for (int tries = 0; tries < kResolveTries; tries++) {
    const int o = f.bag.next(rng);
    if (o < 0) return DrawFailed;
    const int e = entryOrdinal(o, f.bag.count(), f.count, f.bag.cycle());
    hash = hashEntry(f.path, e);
    if (recent && historyHas(recent, hash)) return DrawRecent;
    const Fetch r = entry(f, e, name, sizeof name);
    if (r == FetchOk && joinChecked(outPath, cap, f.path, name)) return DrawOk;
    if (r == FetchMissing) {
      if (recounted) return DrawFailed;
      recount(f);
      recounted = true;
    }
  }
  return DrawFailed;
}

// Weighted choice of a tree by its entry count, then a bag draw inside it (SPEC §6). A tree whose draws
// cannot be resolved is dropped for this pick and another one is weighed. The per-track history rejects a
// file picked within the last kHistory draws of that track while enough alternatives exist (kPickAttempts
// draws, the last one accepted regardless); a rejected draw is consumed from its bag and costs no SD work.
bool Library::pickFromFolders(FolderIndex* const* cand, int n, int track, Rng& rng, char* outPath, int cap) {
  if (!fs_ || !outPath || cap <= 0) return false;
  outPath[0] = 0;
  int total = 0;
  for (int i = 0; i < n; i++) if (cand[i]->count > 0) total += cand[i]->count;
  const bool useHistory = track >= 0 && track < kTracks && total > kHistory;
  uint32_t dead = 0, hash = 0;
  for (int attempt = 0; attempt < kPickAttempts;) {
    total = 0;
    for (int i = 0; i < n; i++) if (!((dead >> i) & 1) && cand[i]->count > 0) total += cand[i]->count;
    if (total <= 0) { outPath[0] = 0; return false; }
    int r = rng.range(0, total - 1), idx = -1;
    for (int i = 0; i < n && idx < 0; i++) {
      if (((dead >> i) & 1) || cand[i]->count <= 0) continue;
      if (r < cand[i]->count) idx = i; else r -= cand[i]->count;
    }
    if (idx < 0) return false;
    const uint32_t* recent = (useHistory && attempt < kPickAttempts - 1) ? history_[track] : nullptr;
    const Draw d = resolve(*cand[idx], rng, recent, outPath, cap, hash);
    if (d == DrawFailed) { dead |= 1u << idx; continue; }
    if (d == DrawRecent) { attempt++; continue; }
    break;
  }
  if (useHistory) {
    history_[track][historyPos_[track]] = hash;
    historyPos_[track] = (uint8_t)((historyPos_[track] + 1) % kHistory);
  }
  return true;
}

bool Library::pickKeyFolders(const char* base, const ProjectKey& key, int track, Rng& rng, char* outPath, int cap) {
  FolderIndex* cand[kRootsMax];
  int n = 0;
  bool cold = false;
  keyFolders(base, key, cand, n, cold);
  return pickFromFolders(cand, n, track, rng, outPath, cap);
}

// ---- Library: public API ------------------------------------------------------------------------------

// /lp/packs/* directories, at most 8, sorted case-insensitively (SPEC §3 genre key rotates through them). A pack whose
// folder name is kGenreNameLen chars or longer is not listed (genreFn) and reported through lastError, like an
// unreachable shard: tools/write_indexes.py warns by name length too.
int Library::scanGenres() {
  genreCount_ = 0;
  if (!fs_) return 0;
  GenreList g = {genres_, 0, 8, 0};
  fs_->walk("/lp/packs", genreFn, &g);
  genreCount_ = g.n;
  if (g.tooLong) snprintf(warn_, sizeof warn_, "packs: %d name%s over %d chars", g.tooLong, g.tooLong == 1 ? "" : "s", kGenreNameLen - 1);
  genreLoops_ = 0;                                           // SPEC §9.19: one isDir per pack, at boot, so a pack without loops is refused with no card work later
  for (int i = 0; i < genreCount_; i++) {
    char p[kMaxPathLen];
    joinPath(p, sizeof p, "/lp/packs", genres_[i]);
    joinPath(p, sizeof p, p, "loops");
    if (fs_->isDir(p)) genreLoops_ |= (uint8_t)(1u << i);
  }
  return genreCount_;
}

// Tonal categories go through the key folders; Beat and Percussion through the category's NA roots (SPEC §9.9).
bool Library::pickMidi(Category c, const ProjectKey& key, int track, Rng& rng, char* outPath, int cap) {
  if (!fs_ || !outPath || cap <= 0) return false;
  opStart_ = useClock_;
  outPath[0] = 0;
  char base[kMaxPathLen];
  joinPath(base, sizeof base, "/lp/midi", categoryName(c));
  if (!isSampleCategory(c)) return pickKeyFolders(base, key, track, rng, outPath, cap);
  FolderIndex* cand[kRootsMax];
  int n = 0;
  bool cold = false;
  naFolders(base, cand, n, cold);
  return pickFromFolders(cand, n, track, rng, outPath, cap);
}

// /lp/packs/<genre>/drum/<role> as one tree (the folder's own files and its shards). For the kick with a key
// root (SPEC §6, §9.7) the tree's entries are scanned once per cache life for names whose root matches and
// one of them is drawn uniformly; otherwise the tree's bag decides (no per-track history: kit slots are not
// tracks). A name equal to `avoid` (a basename or full path) is skipped once so two kit slots do not share
// a file.
bool Library::pickSample(const char* genre, const char* role, int keyRoot, Rng& rng, char* outPath, int cap, const char* avoid) {
  if (!fs_ || !outPath || cap <= 0 || !genre || !role) return false;
  opStart_ = useClock_;
  outPath[0] = 0;
  char path[kMaxPathLen];
  rolePath(path, sizeof path, genre, role);
  FolderIndex* f = folder(path);
  if (!f || f->count <= 0) return false;
  if (keyRoot >= 0 && strcmp(role, "kick") == 0 && keyedStep(*f, keyRoot % 12) && keyed_.n > 0) {
    char name[kEntryLen];
    const int k = keyed_.n;
    int i = rng.range(0, k - 1);
    for (int tries = 0; tries < 2; tries++) {
      const bool ok = entry(*f, keyed_.match[i], name, sizeof name) == FetchOk;
      if (ok && !(sameBase(name, avoid) && k > 1)) return joinChecked(outPath, cap, f->path, name);
      if (k < 2) break;
      i = (i + 1 + rng.range(0, k - 2)) % k;                 // another keyed name
    }
  }
  FolderIndex* cand[1] = {f};
  if (!pickFromFolders(cand, 1, -1, rng, outPath, cap)) return false;
  if (sameBase(outPath, avoid) && f->count > 1 && !pickFromFolders(cand, 1, -1, rng, outPath, cap)) return false;
  return true;
}

// /lp/packs/<genre>/vocal/{minor, major, NA} as trees, weighted like pickMidi. A name with a "loop" token
// (SPEC §6) is drawn again once; the second draw is returned regardless so a pack of loop-named files still
// yields a file. Names without a BPM token are returned as they are: the generator re-picks those one per
// step (SPEC §9.3), which keeps this call at one open + two reads on a pack without loop names (SPEC §9.2).
bool Library::pickVocal(const char* genre, const ProjectKey& key, int set, Rng& rng, char* outPath, int cap) {
  if (!fs_ || !outPath || cap <= 0 || !genre) return false;
  opStart_ = useClock_;
  outPath[0] = 0;
  char base[kMaxPathLen];
  joinPath(base, sizeof base, "/lp/packs", genre);
  joinPath(base, sizeof base, base, "vocal");
  const int track = (set >= 0 && set < kSets) ? set * kTracksPerSet + Vocal : -1;
  for (int attempt = 0; attempt < kVocalTries; attempt++) {
    if (!pickKeyFolders(base, key, track, rng, outPath, cap)) return false;
    if (!hasLoopToken(baseName(outPath))) return true;
  }
  return true;
}

// /lp/packs/<genre>/loops/<folder>/<root>[/sNN] (SPEC §9.19): the roots of loopRoots as trees, weighted by count,
// with the per-track history and the bags of every other picker. A pick made without preparing counts cold trees itself.
bool Library::pickLoop(const char* genre, Category c, const ProjectKey& key, int track, Rng& rng, char* outPath, int cap) {
  if (!fs_ || !outPath || cap <= 0 || !genre || !genre[0]) return false;
  opStart_ = useClock_;
  outPath[0] = 0;
  FolderIndex* cand[kLoopRootsMax];
  int n = 0;
  bool cold = false;
  loopRoots(genre, c, key, cand, n, cold);
  return pickFromFolders(cand, n, track, rng, outPath, cap);
}

// No card work at all (opsLeft_ 0): the sum of the counted roots, -1 while any root is still cold.
int Library::loopTotal(const char* genre, Category c, const ProjectKey& key) {
  if (!fs_ || !genre || !genre[0]) return 0;
  opStart_ = useClock_;
  FolderIndex* cand[kLoopRootsMax];
  int n = 0;
  bool cold = false;
  opsLeft_ = 0;
  loopRoots(genre, c, key, cand, n, cold);
  opsLeft_ = kUnbounded;
  if (cold) return -1;
  int total = 0;
  for (int i = 0; i < n; i++) total += cand[i]->count;
  return total;
}

// ---- Library: resumable warm-up (SPEC §9.2) -----------------------------------------------------------

bool Library::prepareMidi(Category c, const ProjectKey& key) {
  if (!fs_) return true;
  opStart_ = useClock_;
  char base[kMaxPathLen];
  joinPath(base, sizeof base, "/lp/midi", categoryName(c));
  FolderIndex* cand[kRootsMax];
  int n = 0;
  bool cold = false;
  opsLeft_ = 1;
  if (isSampleCategory(c)) naFolders(base, cand, n, cold); else keyFolders(base, key, cand, n, cold);
  opsLeft_ = kUnbounded;
  return !cold;
}

// The role tree is counted first (one chunk or walk per call), then the keyed kick scan streams its parts,
// one chunk or walk per call.
bool Library::prepareSample(const char* genre, const char* role, int keyRoot) {
  if (!fs_ || !genre || !role) return true;
  opStart_ = useClock_;
  char path[kMaxPathLen];
  rolePath(path, sizeof path, genre, role);
  opsLeft_ = 1;
  FolderIndex* f = folder(path);
  bool ready = !f || f->count >= 0;
  if (ready && f && f->count > 0 && keyRoot >= 0 && strcmp(role, "kick") == 0) ready = keyedStep(*f, keyRoot % 12);
  opsLeft_ = kUnbounded;
  return ready;
}

bool Library::prepareVocal(const char* genre, const ProjectKey& key) {
  if (!fs_ || !genre) return true;
  opStart_ = useClock_;
  char base[kMaxPathLen];
  joinPath(base, sizeof base, "/lp/packs", genre);
  joinPath(base, sizeof base, base, "vocal");
  FolderIndex* cand[kRootsMax];
  int n = 0;
  bool cold = false;
  opsLeft_ = 1;
  keyFolders(base, key, cand, n, cold);
  opsLeft_ = kUnbounded;
  return !cold;
}

// One bounded SD operation per call towards every root of the loop pick (the category listing, NA, the key
// folders); true once all of them are counted.
bool Library::prepareLoop(const char* genre, Category c, const ProjectKey& key) {
  if (!fs_ || !genre || !genre[0]) return true;
  opStart_ = useClock_;
  FolderIndex* cand[kLoopRootsMax];
  int n = 0;
  bool cold = false;
  opsLeft_ = 1;
  loopRoots(genre, c, key, cand, n, cold);
  opsLeft_ = kUnbounded;
  return !cold;
}

// ---- Library: cache control ---------------------------------------------------------------------------

// Genre or key change (SPEC §6, §9.9): every bag restarts and the pick history is forgotten, so the same seed
// reproduces the same picks afterwards. Counts, index tables and shard lists are kept: they describe the card,
// not the key, and re-reading them is exactly the SD burst SPEC §9.2 forbids during a performance.
void Library::invalidate() {
  for (int i = 0; i < folderCount_; i++) folders_[i].bag.reset(folders_[i].count);
  memset(history_, 0, sizeof history_);
  memset(historyPos_, 0, sizeof historyPos_);
}

// Forgets everything cached (the card changed): trees are counted again on first use.
void Library::dropCache() {
  folderCount_ = 0;
  useClock_ = 0;
  for (int i = 0; i < kFolderSlots; i++) folderUse_[i] = 0;
  keyed_.treeHash = 0; keyed_.epoch = 0; keyed_.root = -1; keyed_.done = 0; keyed_.n = 0; keyed_.cur = 0;
  warn_[0] = 0;
  memset(history_, 0, sizeof history_);
  memset(historyPos_, 0, sizeof historyPos_);
}

int Library::folderCount(const char* path) {
  if (!fs_) return 0;
  opStart_ = useClock_;
  FolderIndex* f = folder(path);
  if (!f || f->count < 0) return 0;
  return f->source == 1 ? f->count : f->files;
}

bool Library::folderEntry(const char* path, int n, char* out, int cap) {
  if (out && cap > 0) out[0] = 0;
  if (!fs_ || !out || cap <= 0) return false;
  opStart_ = useClock_;
  FolderIndex* f = folder(path);
  return f && entry(*f, n, out, cap) == FetchOk;
}

bool Library::folderUsesIndex(const char* path) {
  if (!fs_) return false;
  opStart_ = useClock_;
  FolderIndex* f = folder(path);
  return f && f->source == 1;
}

int Library::treeCount(const char* path) {
  if (!fs_) return 0;
  opStart_ = useClock_;
  FolderIndex* f = folder(path);
  return (f && f->count > 0) ? f->count : 0;
}

} // namespace lp
