// Host tests for gen/library.cpp: shuffle bags (shuffle_bag.lua semantics), the tree cache (one slot per
// content folder with its shards), weighted key-folder picks (SPEC §6), keyed kick selection (SPEC §9.7),
// vocal picks, genre scanning, _index.txt streaming, shard subfolders (a content folder of > 64 files is split
// into s00, s01, ... — SPEC §9.2 FAT folder sizes) and the SD operation budget of SPEC §9.2 (counted through a
// wrapping IFileSystem). Corpus checks run over sdcard/lp (override with LP_SDCARD) and count the files
// themselves; scratch checks build their own tiny card image.
#include "check.h"
#include "gen/library.h"
#include "gen/key.h"
#include "io/fs.h"
#include "../firmware/desktop/posix_fs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace lp;

// ---------------------------------------------------------------------------------------------- setup

static std::string sdRoot() {
  const char* env = getenv("LP_SDCARD");
  if (env && *env) return env;
  struct stat st;
  if (stat("sdcard/lp", &st) == 0) return "sdcard";
  return "/Users/miha/Documents/cardputer/liveplayer/sdcard";
}
static PosixFs& sd() { static PosixFs fs(sdRoot().c_str()); return fs; }

// IFileSystem wrapper that counts what the library asks of the card (SPEC §9.2 budget: opens, 4 KB reads,
// walked directory entries). Every open attempt counts, failed ones separately.
class CountingFs : public IFileSystem {
 public:
  explicit CountingFs(IFileSystem& inner) : inner_(inner) { for (auto& f : pool_) f.fs = this; }
  int opens = 0, failedOpens = 0, reads = 0, readBytes = 0, walks = 0, walked = 0;
  void reset() { opens = failedOpens = reads = readBytes = walks = walked = 0; }
  bool exists(const char* path) override { return inner_.exists(path); }
  bool isDir(const char* path) override { return inner_.isDir(path); }
  IFile* open(const char* path, const char* mode = "r") override {
    opens++;
    IFile* f = inner_.open(path, mode);
    if (!f) { failedOpens++; return nullptr; }
    for (auto& w : pool_) if (!w.f) { w.f = f; return &w; }
    f->close();
    return nullptr;
  }
  bool mkdir(const char* path) override { return inner_.mkdir(path); }
  bool remove(const char* path) override { return inner_.remove(path); }
  bool walk(const char* path, DirEntryFn fn, void* user) override {
    walks++;
    Hop h = {this, fn, user};
    return inner_.walk(path, hopFn, &h);
  }
  std::string summary() const {
    char b[96]; snprintf(b, sizeof b, "%d opens (%d failed), %d reads (%d B), %d walks / %d entries", opens, failedOpens, reads, readBytes, walks, walked);
    return b;
  }
 private:
  struct File : IFile {
    IFile* f = nullptr; CountingFs* fs = nullptr;
    bool valid() const override { return f && f->valid(); }
    size_t read(void* dst, size_t n) override { fs->reads++; size_t got = f->read(dst, n); fs->readBytes += (int)got; return got; }
    size_t write(const void* src, size_t n) override { return f->write(src, n); }
    bool seek(uint32_t pos) override { return f->seek(pos); }
    uint32_t position() const override { return f->position(); }
    uint32_t size() const override { return f->size(); }
    void close() override { if (f) f->close(); f = nullptr; }
  };
  struct Hop { CountingFs* fs; DirEntryFn fn; void* user; };
  static bool hopFn(const char* name, bool isDir, uint32_t size, void* user) {
    Hop& h = *static_cast<Hop*>(user);
    h.fs->walked++;
    return h.fn(name, isDir, size, h.user);
  }
  IFileSystem& inner_;
  File pool_[8];
};
static CountingFs& csd() { static CountingFs fs(sd()); return fs; }
static Library& clib() {                              // a Library over the counting card
  static Library l;
  static bool init = false;
  if (!init) { l.init(&csd()); init = true; }
  return l;
}

// Library holds 16 FolderIndex entries plus a 4 KB index chunk (≈ 12 KB): static, never on the stack.
static Library& lib() {
  static Library l;
  static bool init = false;
  if (!init) { l.init(&sd()); init = true; }
  return l;
}

static std::string scratchRoot() {
  static std::string dir;
  if (dir.empty()) {
    char tmpl[] = "/tmp/lp_test_library_XXXXXX";
    const char* d = mkdtemp(tmpl);
    dir = d ? d : "/tmp";
  }
  return dir;
}
static PosixFs& scratch() { static PosixFs fs(scratchRoot().c_str()); return fs; }
static Library& scratchLib() {
  static Library l;
  static bool init = false;
  if (!init) { l.init(&scratch()); init = true; }
  return l;
}
static CountingFs& cscratch() { static CountingFs fs(scratch()); return fs; }
static Library& cscratchLib() {
  static Library l;
  static bool init = false;
  if (!init) { l.init(&cscratch()); init = true; }
  return l;
}


static void mkdirs(const std::string& rel) {           // every prefix of rel beneath the scratch root
  std::string path = scratchRoot();
  size_t pos = 0;
  while (pos <= rel.size()) {
    size_t next = rel.find('/', pos);
    if (next == std::string::npos) next = rel.size();
    path += "/" + rel.substr(pos, next - pos);
    ::mkdir(path.c_str(), 0755);
    pos = next + 1;
  }
}
static void touch(const std::string& rel) {
  std::string full = scratchRoot() + "/" + rel;
  FILE* f = fopen(full.c_str(), "wb");
  if (f) fclose(f);
}
static void writeFile(const std::string& rel, const std::string& content) {
  std::string full = scratchRoot() + "/" + rel;
  FILE* f = fopen(full.c_str(), "wb");
  if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
}
static bool scratchExists(const std::string& cardPath) {
  struct stat st;
  return stat((scratchRoot() + cardPath).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static std::vector<std::string> readLines(const std::string& full) {   // non-empty lines, CR stripped
  std::vector<std::string> v;
  std::ifstream in(full);
  std::string line;
  while (std::getline(in, line)) { if (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty()) v.push_back(line); }
  return v;
}
static void findIndexes(const std::string& dir, std::vector<std::string>& out) {   // every _index.txt beneath dir
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n[0] == '.') continue;
    std::string full = dir + "/" + n;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) findIndexes(full, out);
    else if (n == "_index.txt") out.push_back(full);
  }
  closedir(d);
}

static bool fileExists(const std::string& rel) {
  struct stat st;
  return stat((sdRoot() + rel).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static std::string dirOf(const std::string& path) { return path.substr(0, path.rfind('/')); }
static std::string baseOf(const std::string& path) { return path.substr(path.rfind('/') + 1); }
static bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
static bool endsWithCi(const std::string& s, const std::string& e) {
  if (s.size() < e.size()) return false;
  std::string tail = s.substr(s.size() - e.size());
  for (auto& c : tail) c = (char)tolower(c);
  return tail == e;
}
static std::vector<std::string> listDir(const std::string& dir, const char* ext, bool dirsOnly = false) {
  std::vector<std::string> v;
  DIR* d = opendir(dir.c_str());
  if (!d) return v;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n[0] == '.') continue;
    struct stat st;
    if (stat((dir + "/" + n).c_str(), &st) != 0) continue;
    if (dirsOnly && !S_ISDIR(st.st_mode)) continue;
    if (ext && (S_ISDIR(st.st_mode) || !endsWithCi(n, ext))) continue;
    v.push_back(n);
  }
  closedir(d);
  return v;
}

// ---- the sharded card (SPEC §9.2 FAT folder sizes): a content folder is FLAT (media files, indexed) or SHARDED
// (only subfolders s00, s01, ... of <= 64 files, each with its own index), never both. The library caches a
// folder and its shards as one tree in one slot, so the corpus checks look at the whole tree: its folders, its
// files, the index chunks its counting streams and the prepare steps that costs.
static std::vector<std::string> shardsOf(const std::string& cardFolder) {       // shard subfolder names, sorted
  std::vector<std::string> v = listDir(sdRoot() + cardFolder, nullptr, true);
  std::sort(v.begin(), v.end());
  return v;
}
static std::vector<std::string> treeFolders(const std::string& cardFolder) {    // the folder itself, then its shards (card paths)
  std::vector<std::string> v = {cardFolder};
  for (const std::string& s : shardsOf(cardFolder)) v.push_back(cardFolder + "/" + s);
  return v;
}
static std::vector<std::string> treeFiles(const std::string& cardFolder, const char* ext) {   // media basenames of the whole tree
  std::vector<std::string> v;
  for (const std::string& f : treeFolders(cardFolder)) { std::vector<std::string> d = listDir(sdRoot() + f, ext); v.insert(v.end(), d.begin(), d.end()); }
  return v;
}
static bool inTree(const std::string& path, const std::string& cardFolder) {   // the file sits in the folder or in a shard right under it
  const std::string d = dirOf(path);
  return d == cardFolder || (startsWith(d, cardFolder + "/") && d.find('/', cardFolder.size() + 1) == std::string::npos);
}
static int indexChunks(const std::string& cardFolder) {   // 4 KB chunks of a folder's _index.txt (0 when absent)
  struct stat st;
  if (stat((sdRoot() + cardFolder + "/_index.txt").c_str(), &st) != 0) return 0;
  return (int)((st.st_size + kIndexChunk - 1) / kIndexChunk);
}
static int countSteps(const std::string& cardFolder) {    // prepare steps that count ONE folder: its index chunks, else its single walk
  const int c = indexChunks(cardFolder);
  return c > 0 ? c : 1;
}
static int treeSteps(const std::string& cardFolder) {     // ... that count a tree: the root (walked when sharded or absent) and every shard, one slot
  int n = 0;
  for (const std::string& f : treeFolders(cardFolder)) n += countSteps(f);
  return n;
}
static int treeChunks(const std::string& cardFolder) {    // index chunks over the tree: what one scan of every name streams
  int n = 0;
  for (const std::string& f : treeFolders(cardFolder)) n += indexChunks(f);
  return n;
}

// ---------------------------------------------------------------------------------------------- Bag

TEST(bag_empty_and_single) {
  Rng rng(1);
  Bag b;
  b.reset(0);
  CHECK_EQ(b.count(), 0);
  CHECK_EQ(b.next(rng), -1);
  CHECK_EQ(b.next(rng), -1);
  b.reset(1);                                        // one item: the only choice, repeated (shuffle_bag.lua:109)
  CHECK_EQ(b.count(), 1);
  for (int i = 0; i < 5; i++) CHECK_EQ(b.next(rng), 0);
  b.reset(-3);
  CHECK_EQ(b.count(), 0);
  CHECK_EQ(b.next(rng), -1);
}

TEST(bag_five_items_every_ordinal_once_per_cycle_no_immediate_repeat) {
  Rng rng(2024);
  Bag b;
  b.reset(5);
  std::vector<int> draws;
  for (int i = 0; i < 50; i++) draws.push_back(b.next(rng));
  for (int v : draws) CHECK(v >= 0 && v < 5);
  // first cycle: a permutation of 0..4
  std::set<int> first(draws.begin(), draws.begin() + 5);
  CHECK_EQ((int)first.size(), 5);
  // no immediate repeats anywhere
  for (size_t i = 1; i < draws.size(); i++) CHECK(draws[i] != draws[i - 1]);
  // later cycles: 4 distinct ordinals, none equal to the previous cycle's last draw (shuffle_bag.lua:107)
  size_t pos = 5;
  int cycles = 0;
  while (pos + 4 <= draws.size()) {
    std::set<int> cyc(draws.begin() + pos, draws.begin() + pos + 4);
    CHECK_EQ((int)cyc.size(), 4);
    CHECK(cyc.count(draws[pos - 1]) == 0);
    pos += 4; cycles++;
  }
  CHECK(cycles >= 10);
  // every ordinal keeps coming back (≈ 10 of 50 each)
  int hist[5] = {0, 0, 0, 0, 0};
  for (int v : draws) hist[v]++;
  for (int i = 0; i < 5; i++) CHECK(hist[i] >= 6 && hist[i] <= 14);
}

TEST(bag_two_items_alternate) {
  Rng rng(7);
  Bag b;
  b.reset(2);
  int prev = b.next(rng);
  for (int i = 0; i < 20; i++) { int v = b.next(rng); CHECK_EQ(v, 1 - prev); prev = v; }
}

TEST(bag_is_deterministic_per_seed_and_clamps_to_max) {
  Bag a, b;
  Rng ra(99), rb(99);
  a.reset(37); b.reset(37);
  for (int i = 0; i < 100; i++) CHECK_EQ(a.next(ra), b.next(rb));
  Bag big;
  big.reset(5000);
  CHECK_EQ(big.count(), kBagMax);
  Rng r(3);
  std::set<int> seen;
  for (int i = 0; i < kBagMax; i++) { int v = big.next(r); CHECK(v >= 0 && v < kBagMax); seen.insert(v); }
  CHECK_EQ((int)seen.size(), kBagMax);               // the first cycle offers every ordinal exactly once
}

TEST(bag_reset_restarts_cycle) {
  Rng rng(5);
  Bag b;
  b.reset(3);
  b.next(rng); b.next(rng);
  b.reset(3);                                        // a fresh bag: the next three draws are a full permutation
  std::set<int> s;
  for (int i = 0; i < 3; i++) s.insert(b.next(rng));
  CHECK_EQ((int)s.size(), 3);
}

// ---------------------------------------------------------------------------------------------- genres

TEST(scanGenres_lists_packs_sorted) {
  Library& l = lib();
  int n = l.scanGenres();
  std::vector<std::string> expect = listDir(sdRoot() + "/lp/packs", nullptr, true);
  std::sort(expect.begin(), expect.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  if (expect.size() > 8) expect.resize(8);
  CHECK_EQ(n, (int)expect.size());
  CHECK_EQ(l.genreCount(), n);
  bool house = false, test = false;
  for (int i = 0; i < n; i++) {
    CHECK(std::string(l.genreName(i)) == expect[i]);
    if (strcmp(l.genreName(i), "House") == 0) house = true;
    if (strcmp(l.genreName(i), "Testpack") == 0) test = true;
  }
  CHECK(house);
  CHECK(test);
  CHECK_EQ(l.scanGenres(), n);                       // rescanning is idempotent
  // SPEC §9.19: the scan notes which packs carry a loops/ folder (one isDir per pack, at boot), so Generator::setLoopMode
  // refuses a pack without loop content at once and without card work; the answer itself costs nothing afterwards
  Library& cl = clib(); CHECK_EQ(cl.scanGenres(), n);
  csd().reset();
  for (int i = 0; i < n; i++) {
    const bool has = cl.genreHasLoops(i);
    if (strcmp(cl.genreName(i), "House") == 0) CHECK(has);
    if (strcmp(cl.genreName(i), "Testpack") == 0) CHECK(!has);
    struct stat st; const std::string loops = sdRoot() + "/lp/packs/" + cl.genreName(i) + "/loops";
    CHECK_EQ((int)has, (int)(stat(loops.c_str(), &st) == 0 && S_ISDIR(st.st_mode)));
  }
  CHECK_EQ(csd().opens + csd().walks, 0);
  CHECK(!cl.genreHasLoops(-1)); CHECK(!cl.genreHasLoops(n));
}

TEST(scanGenres_keeps_eight_alphabetically_first_case_insensitive) {
  const char* names[] = {"zulu", "Alpha", "mike", "Bravo", "yankee", "charlie", "Delta", "echo", "Foxtrot", "golf", "hotel"};
  for (const char* n : names) mkdirs(std::string("lp/packs/") + n);
  touch("lp/packs/notadir.txt");                     // files are not genres
  Library& l = scratchLib();
  CHECK_EQ(l.scanGenres(), 8);
  const char* expect[] = {"Alpha", "Bravo", "charlie", "Delta", "echo", "Foxtrot", "golf", "hotel"};
  for (int i = 0; i < 8; i++) CHECK(std::string(l.genreName(i)) == expect[i]);
}

TEST(scanGenres_skips_a_pack_name_that_does_not_fit_a_genre_slot) {
  // RAM diet 2026-10-16: a genre slot is kGenreNameLen = 24 B (was kMaxNameLen = 96). A pack folder of 23 chars is listed
  // (it sorts first here and pushes "hotel" out); one of 24 chars or more is not — its loop folders would not fit a cached
  // path (kFolderPathLen) either — and lastError reports the skipped names, like an unreachable shard; the card is
  // otherwise unaffected. The folders are removed again so the listing of the test above stays what later tests expect.
  const std::string fits(kGenreNameLen - 1, 'A'), over(kGenreNameLen, 'A'), far = "zz_" + std::string(40, 'q');
  mkdirs("lp/packs/" + fits); mkdirs("lp/packs/" + over); mkdirs("lp/packs/" + far);
  Library& l = scratchLib();
  l.dropCache();
  CHECK_EQ((int)strlen(l.lastError()), 0);
  CHECK_EQ(l.scanGenres(), 8);
  const char* expect[] = {"", "Alpha", "Bravo", "charlie", "Delta", "echo", "Foxtrot", "golf"};
  for (int i = 0; i < 8; i++) CHECK(std::string(l.genreName(i)) == (i == 0 ? fits : std::string(expect[i])));
  CHECK_EQ((int)strlen(l.genreName(0)), kGenreNameLen - 1);
  CHECK(std::string(l.lastError()) == "packs: 2 names over 23 chars");
  for (const std::string& d : {fits, over, far}) ::rmdir((scratchRoot() + "/lp/packs/" + d).c_str());
  l.dropCache();
  CHECK_EQ((int)strlen(l.lastError()), 0);
  CHECK_EQ(l.scanGenres(), 8);
  CHECK(std::string(l.genreName(7)) == "hotel");
  CHECK_EQ((int)strlen(l.lastError()), 0);               // nothing to report on a card of short names
}

TEST(scanGenres_without_fs_or_folder_is_zero) {
  static Library none;
  CHECK_EQ(none.scanGenres(), 0);
  char out[kMaxPathLen];
  Rng rng(1);
  ProjectKey key;
  CHECK(!none.pickMidi(Chord, key, 0, rng, out, sizeof out));
  CHECK(!none.pickSample("House", "kick", 9, rng, out, sizeof out));
  CHECK(!none.pickVocal("House", key, 0, rng, out, sizeof out));
}

// ---------------------------------------------------------------------------------------------- pickMidi

TEST(pickMidi_chord_a_minor_uses_key_folders_and_existing_files) {
  Library& l = lib();
  Rng rng(11);
  ProjectKey key; key.root = 9;                      // A minor: Amin, C, NA
  char out[kMaxPathLen];
  int fromAmin = 0, fromC = 0;
  std::vector<std::string> aminOrder;
  for (int i = 0; i < 50; i++) {
    CHECK(l.pickMidi(Chord, key, 0, rng, out, sizeof out));
    std::string p = out;
    bool ok = inTree(p, "/lp/midi/Chord/Amin") || inTree(p, "/lp/midi/Chord/C") || inTree(p, "/lp/midi/Chord/NA");
    CHECK(ok);
    if (!ok) printf("    got %s\n", out);
    CHECK(endsWithCi(p, ".mid"));
    CHECK(fileExists(p));
    if (inTree(p, "/lp/midi/Chord/Amin")) { fromAmin++; aminOrder.push_back(baseOf(p)); }
    if (inTree(p, "/lp/midi/Chord/C")) fromC++;
  }
  CHECK_EQ(fromAmin + fromC, 50);
  const size_t aminFiles = treeFiles("/lp/midi/Chord/Amin", ".mid").size(), cFiles = treeFiles("/lp/midi/Chord/C", ".mid").size();
  printf("    Chord/Amin %zu files, Chord/C %zu files: %d / %d picks\n", aminFiles, cFiles, fromAmin, fromC);
  CHECK(aminFiles > cFiles);
  CHECK(fromAmin > fromC);                           // weighted by count (33 against 4 on the import)
  // bag inside Amin: the first 20 Amin picks are all different files
  std::set<std::string> first(aminOrder.begin(), aminOrder.begin() + std::min<size_t>(20, aminOrder.size()));
  CHECK_EQ((int)first.size(), (int)std::min<size_t>(20, aminOrder.size()));
}

TEST(pickMidi_weights_folders_by_count) {
  Library& l = lib();
  l.invalidate();
  Rng rng(21);
  ProjectKey key; key.root = 9;                      // Bass: Amin (249 files over 4 shards), C (109 over 2)
  char out[kMaxPathLen];
  const size_t aminFiles = treeFiles("/lp/midi/Bass/Amin", ".mid").size(), cFiles = treeFiles("/lp/midi/Bass/C", ".mid").size();
  CHECK(shardsOf("/lp/midi/Bass/Amin").size() >= 2);   // the weighting spans every shard of a sharded key folder
  int amin = 0, c = 0;
  std::set<std::string> aminDirs;
  for (int i = 0; i < 400; i++) {
    CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
    if (inTree(out, "/lp/midi/Bass/Amin")) { amin++; aminDirs.insert(dirOf(out)); } else if (inTree(out, "/lp/midi/Bass/C")) c++;
  }
  CHECK_EQ(amin + c, 400);
  double share = (double)c / 400.0, expected = (double)cFiles / (double)(aminFiles + cFiles);   // 109 / 358 ≈ 0.30
  printf("    Bass A minor: C share %.2f (expected %.2f), Amin picks spread over %zu of %zu shards\n", share, expected, aminDirs.size(), shardsOf("/lp/midi/Bass/Amin").size());
  CHECK(share > expected - 0.10 && share < expected + 0.12);
  CHECK_EQ(aminDirs.size(), shardsOf("/lp/midi/Bass/Amin").size());
}

// The reference card has every minor and relative-major folder of every tonal category since the 2026-10-16 content
// wave (Fx keeps its 3-file NA besides them), so the "key folders missing" cases live on the scratch card, under a
// Lead folder no other scratch test builds and removed again afterwards (a leftover NA folder would be weighed by a
// later scratch pick): a category holding only NA, and a category without the key's minor, major and NA folders.
static void removeScratchTree(const std::string& rel) {   // files and subfolders beneath the scratch root, then the folder
  const std::string dir = scratchRoot() + "/" + rel;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name; if (n == "." || n == "..") continue;
      struct stat st; if (stat((dir + "/" + n).c_str(), &st) != 0) continue;
      if (S_ISDIR(st.st_mode)) removeScratchTree(rel + "/" + n); else ::remove((dir + "/" + n).c_str());
    }
    closedir(d);
  }
  ::rmdir(dir.c_str());
}

TEST(pickMidi_only_NA_when_key_folders_are_missing) {
  mkdirs("lp/midi/Lead/NA");
  std::string idx;
  for (int i = 0; i < 3; i++) { std::string n = "NA__Lead__only_" + std::to_string(i) + ".mid"; touch("lp/midi/Lead/NA/" + n); idx += n + "\n"; }
  writeFile("lp/midi/Lead/NA/_index.txt", idx);
  Library& l = scratchLib();
  l.dropCache();
  Rng rng(3);
  ProjectKey key; key.root = 10;                     // A# minor: no A#min, no C# folder, only NA (3 files)
  char out[kMaxPathLen];
  std::set<std::string> names;
  for (int i = 0; i < 12; i++) {
    CHECK(l.pickMidi(Lead, key, 1, rng, out, sizeof out));
    CHECK(inTree(out, "/lp/midi/Lead/NA"));
    CHECK(scratchExists(out));
    names.insert(baseOf(out));
  }
  CHECK_EQ((int)names.size(), 3);                    // every file of a 3-file folder shows up
  removeScratchTree("lp/midi/Lead"); l.dropCache();
}

TEST(pickMidi_returns_false_without_candidates) {
  mkdirs("lp/midi/Lead/Amin");
  touch("lp/midi/Lead/Amin/Amin__Lead__only.mid");   // the category exists, but not for B minor
  Library& l = scratchLib();
  l.dropCache();
  Rng rng(3);
  ProjectKey key; key.root = 11;                     // B minor: no Bmin, no D, no NA under Lead
  char out[kMaxPathLen] = "stale";
  CHECK(!l.pickMidi(Lead, key, 1, rng, out, sizeof out));
  CHECK_EQ((int)strlen(out), 0);
  ProjectKey amin; amin.root = 9;                    // the folder that is there still picks
  CHECK(l.pickMidi(Lead, amin, 1, rng, out, sizeof out)); CHECK(inTree(out, "/lp/midi/Lead/Amin"));
  removeScratchTree("lp/midi/Lead"); l.dropCache();
}

TEST(pickMidi_beat_and_percussion_ignore_the_key) {
  Library& l = lib();
  Rng rng(8);
  char out[kMaxPathLen];
  for (int root = 0; root < 12; root++) {
    ProjectKey key; key.root = (uint8_t)root;
    CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
    CHECK(inTree(out, "/lp/midi/Beat/NA"));
    CHECK(fileExists(out));
    CHECK(l.pickMidi(Percussion, key, 5, rng, out, sizeof out));
    CHECK(inTree(out, "/lp/midi/Percussion/NA"));
    CHECK(fileExists(out));
  }
}

TEST(pickMidi_beat_shards_are_all_reached_and_weighted) {
  // Beat/NA (748 files) is sharded into s00.. of <= 64 files (SPEC §9.2): the pick weighs every shard by its
  // count and draws inside that shard's bag, so every line of every shard's _index.txt is reachable, two
  // consecutive picks never repeat, and each shard's share follows its size. (The > kBagMax strata mapping of
  // one large flat folder is covered by index_entries_resolve_across_chunk_edges_synthetic.)
  Library& l = lib();
  l.invalidate();
  const std::string root = "/lp/midi/Beat/NA";
  const std::vector<std::string> shards = shardsOf(root);
  CHECK(shards.size() >= 2);
  CHECK_EQ(l.folderCount(root.c_str()), 0);          // a sharded root holds no files of its own
  std::map<std::string, std::string> shardOf;        // index line -> shard
  std::map<std::string, int> size;
  int total = 0;
  for (const std::string& s : shards) {
    std::vector<std::string> lines = readLines(sdRoot() + root + "/" + s + "/_index.txt");
    CHECK(!lines.empty() && (int)lines.size() <= kBagMax);
    CHECK_EQ((int)lines.size(), countEntries(sd(), (root + "/" + s).c_str(), ".mid"));
    for (const std::string& n : lines) shardOf[n] = s;
    size[s] = (int)lines.size(); total += (int)lines.size();
  }
  CHECK_EQ(l.treeCount(root.c_str()), total);
  CHECK(total > kBagMax);
  Rng rng(17);
  char out[kMaxPathLen];
  std::set<std::string> seen;
  std::map<std::string, int> hits;
  int unknown = 0, misplaced = 0;
  auto t0 = std::chrono::steady_clock::now();
  std::string prev;
  for (int i = 0; i < 1100; i++) {
    CHECK(l.pickMidi(Beat, ProjectKey(), 7, rng, out, sizeof out));
    std::string b = baseOf(out);
    CHECK(b != prev);                                // never the same file twice in a row
    prev = b;
    seen.insert(b);
    if (!shardOf.count(b)) { unknown++; continue; }
    if (dirOf(out) != root + "/" + shardOf[b]) misplaced++;   // the path names the shard the line came from
    hits[shardOf[b]]++;
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  double worst = 1.0;                                // hits / expected share, over the shards
  for (const std::string& s : shards) { double r = hits[s] / (1100.0 * size[s] / total); if (r < worst) worst = r; }
  printf("    1100 Beat picks over %zu shards (%d index lines): %zu distinct, worst shard at %.2f of its expected share, %.0f ms\n",
         shards.size(), total, seen.size(), worst, ms);
  CHECK_EQ(unknown, 0);                              // every pick is an index line
  CHECK_EQ(misplaced, 0);
  CHECK((int)seen.size() > 540);
  CHECK(worst > 0.6);                                // every shard is drawn in proportion to its count
}

TEST(pickMidi_small_key_folders_every_key_works_and_cache_evicts) {
  // 12 roots × Chord/Lead/Bass touches up to 108 folders (plus shards) through a 16-entry cache: LRU eviction must be silent.
  Library& l = lib();
  l.invalidate();
  Rng rng(31);
  char out[kMaxPathLen];
  for (int root = 0; root < 12; root++) {
    ProjectKey key; key.root = (uint8_t)root;
    char minorLabel[8], majorLabel[8];
    key.labels(minorLabel, majorLabel);
    const Category cats[3] = {Chord, Lead, Bass};
    for (Category c : cats) {
      for (int k = 0; k < 3; k++) {
        CHECK(l.pickMidi(c, key, (int)c, rng, out, sizeof out));
        std::string base = std::string("/lp/midi/") + categoryName(c) + "/";
        CHECK(inTree(out, base + minorLabel) || inTree(out, base + majorLabel) || inTree(out, base + "NA"));
        CHECK(fileExists(out));
      }
    }
  }
}

TEST(pickMidi_rejects_small_buffer) {
  Library& l = lib();
  Rng rng(4);
  ProjectKey key; key.root = 9;
  char tiny[24] = "x";
  CHECK(!l.pickMidi(Chord, key, 0, rng, tiny, sizeof tiny));
  CHECK_EQ((int)strlen(tiny), 0);
  CHECK(!l.pickMidi(Chord, key, 0, rng, nullptr, 0));
}

TEST(pickMidi_is_deterministic_per_seed) {
  Library& l = lib();
  ProjectKey key; key.root = 2;                      // D minor
  std::vector<std::string> a, b;
  for (int pass = 0; pass < 2; pass++) {
    l.invalidate();
    Rng rng(777);
    char out[kMaxPathLen];
    for (int i = 0; i < 20; i++) { CHECK(l.pickMidi(Lead, key, 1, rng, out, sizeof out)); (pass ? b : a).push_back(out); }
  }
  CHECK(a == b);
}

// ---------------------------------------------------------------------------------------------- pickSample

TEST(pickSample_kick_prefers_key_root_files) {
  Library& l = lib();
  Rng rng(42);
  char out[kMaxPathLen];
  int keyed = 0;
  std::set<std::string> names, dirs;
  size_t asharp = 0;                                 // A#__ kicks across the shards of House/drum/kick
  for (const std::string& n : treeFiles("/lp/packs/House/drum/kick", ".wav")) if (startsWith(n, "A#__")) asharp++;
  for (int i = 0; i < 50; i++) {
    CHECK(l.pickSample("House", "kick", 10, rng, out, sizeof out));        // A#
    CHECK(inTree(out, "/lp/packs/House/drum/kick"));
    CHECK(fileExists(out));
    if (startsWith(baseOf(out), "A#__")) keyed++;
    names.insert(baseOf(out)); dirs.insert(dirOf(out));
  }
  printf("    A# kicks: %zu on the card, %d of 50 picks keyed, %zu distinct from %zu shard folder(s)\n", asharp, keyed, names.size(), dirs.size());
  CHECK(asharp >= 10);
  CHECK(keyed >= 45);                                // >= 90 %; with 15 A#__ files it is in fact 100 %
  CHECK((int)names.size() >= 10);                    // spread over the keyed pool, not one file
  for (int root = 0; root < 12; root++) {            // every root has keyed kicks in House
    CHECK(l.pickSample("House", "kick", root, rng, out, sizeof out));
    LibraryKey k = parseLibraryFilename(baseOf(out).c_str());
    CHECK(k.valid && !k.keyless && k.root == root);
    dirs.insert(dirOf(out));
  }
  CHECK(dirs.size() >= 2);                           // the keyed scan reaches beyond one shard (the sorted names of a root mostly share one)
}

TEST(pickSample_kick_falls_back_when_no_keyed_file) {
  Library& l = lib();
  Rng rng(43);
  char out[kMaxPathLen];
  std::vector<std::string> all = treeFiles("/lp/packs/Testpack/drum/kick", ".wav");
  CHECK(all.size() <= (size_t)kBagMax);              // a flat folder: one bag cycle offers every file
  std::set<std::string> seen;
  for (int i = 0; i < (int)all.size(); i++) {        // Testpack kicks carry no key: the whole bag cycles once
    CHECK(l.pickSample("Testpack", "kick", 9, rng, out, sizeof out));
    CHECK(inTree(out, "/lp/packs/Testpack/drum/kick"));
    CHECK(fileExists(out));
    seen.insert(baseOf(out));
  }
  CHECK_EQ((int)seen.size(), (int)all.size());
}

TEST(pickSample_other_roles_and_avoid) {
  Library& l = lib();
  Rng rng(44);
  char out[kMaxPathLen];
  const char* roles[] = {"snare", "hh", "oh", "perc", "clap", "cymbal"};
  for (const char* role : roles) {
    CHECK(l.pickSample("House", role, 9, rng, out, sizeof out));
    CHECK(inTree(out, std::string("/lp/packs/House/drum/") + role));
    CHECK(fileExists(out));
  }
  // avoid: Testpack/oh has 8 files; the avoided name is never returned
  std::string prev;
  for (int i = 0; i < 30; i++) {
    CHECK(l.pickSample("Testpack", "oh", -1, rng, out, sizeof out, prev.empty() ? nullptr : prev.c_str()));
    CHECK(baseOf(out) != prev);
    prev = out;                                      // a full path works as `avoid` too
  }
  // avoid for the keyed kick path
  std::string prevKick;
  for (int i = 0; i < 30; i++) {
    CHECK(l.pickSample("House", "kick", 1, rng, out, sizeof out, prevKick.empty() ? nullptr : baseOf(prevKick).c_str()));   // C#: 16 files
    CHECK(startsWith(baseOf(out), "C#__"));
    CHECK(baseOf(out) != baseOf(prevKick));
    prevKick = out;
  }
}

TEST(pickSample_missing_role_or_genre) {
  Library& l = lib();
  Rng rng(45);
  char out[kMaxPathLen] = "stale";
  CHECK(!l.pickSample("House", "nope", 9, rng, out, sizeof out));
  CHECK_EQ((int)strlen(out), 0);
  CHECK(!l.pickSample("Nopack", "kick", 9, rng, out, sizeof out));
  CHECK(!l.pickSample("Testpack", "clap", -1, rng, out, sizeof out));   // Testpack has no clap folder
  CHECK(!l.pickSample(nullptr, "kick", 9, rng, out, sizeof out));
  CHECK(!l.pickSample("House", nullptr, 9, rng, out, sizeof out));
}

TEST(pickSample_compact_kick_names_scratch) {
  // SPEC §9.7: A_Kick, F#-Kick and Bb Kick forms count as keyed; others fall back to the whole folder.
  mkdirs("lp/packs/Scratch/drum/kick");
  const char* files[] = {"A_Kick.wav", "F#-Kick.wav", "Bb Kick.wav", "bd_808.wav", "NA__x__Kick.wav", "G#min__Kick__1.wav", "cb_rytm.wav", "ab-two.WAV"};
  for (const char* f : files) touch(std::string("lp/packs/Scratch/drum/kick/") + f);
  Library& l = scratchLib();
  l.dropCache();
  Rng rng(46);
  char out[kMaxPathLen];
  for (int i = 0; i < 10; i++) { CHECK(l.pickSample("Scratch", "kick", 9, rng, out, sizeof out)); CHECK(baseOf(out) == "A_Kick.wav"); }
  for (int i = 0; i < 10; i++) { CHECK(l.pickSample("Scratch", "kick", 6, rng, out, sizeof out)); CHECK(baseOf(out) == "F#-Kick.wav"); }
  for (int i = 0; i < 10; i++) { CHECK(l.pickSample("Scratch", "kick", 10, rng, out, sizeof out)); CHECK(baseOf(out) == "Bb Kick.wav"); }
  // "cb_rytm" is C-flat = B (drum_kit.lua:62, the port keeps that reading); "ab-two" is A-flat = G# like G#min__
  std::set<std::string> b11;
  for (int i = 0; i < 10; i++) { CHECK(l.pickSample("Scratch", "kick", 11, rng, out, sizeof out)); b11.insert(baseOf(out)); }
  CHECK_EQ((int)b11.size(), 1);
  CHECK(b11.count("cb_rytm.wav") == 1);
  std::set<std::string> g8;
  for (int i = 0; i < 20; i++) { CHECK(l.pickSample("Scratch", "kick", 8, rng, out, sizeof out)); g8.insert(baseOf(out)); }
  CHECK_EQ((int)g8.size(), 2);                       // G#min__ and ab-two
  // root 0 (C): no keyed file -> any of the eight, all of them over one bag cycle
  std::set<std::string> any;
  for (int i = 0; i < 8; i++) { CHECK(l.pickSample("Scratch", "kick", 0, rng, out, sizeof out)); any.insert(baseOf(out)); }
  CHECK_EQ((int)any.size(), 8);
  // keyRoot < 0: no key preference at all (fresh bags after invalidate: one cycle offers every file)
  l.invalidate();
  std::set<std::string> none;
  for (int i = 0; i < 8; i++) { CHECK(l.pickSample("Scratch", "kick", -1, rng, out, sizeof out)); none.insert(baseOf(out)); }
  CHECK_EQ((int)none.size(), 8);
  // a non-kick role never uses the key
  mkdirs("lp/packs/Scratch/drum/snare");
  touch("lp/packs/Scratch/drum/snare/A_Snare.wav");
  touch("lp/packs/Scratch/drum/snare/B_Snare.wav");
  std::set<std::string> sn;
  for (int i = 0; i < 4; i++) { CHECK(l.pickSample("Scratch", "snare", 9, rng, out, sizeof out)); sn.insert(baseOf(out)); }
  CHECK_EQ((int)sn.size(), 2);
}

// ---------------------------------------------------------------------------------------------- pickVocal

TEST(pickVocal_house_c_minor_from_key_folders) {
  Library& l = lib();
  l.invalidate();
  Rng rng(51);
  ProjectKey key; key.root = 0;                      // C minor: Cmin (541, sharded), D# (8), NA (174, sharded) on the 2026-10-16 card
  const std::string cminDir = "/lp/packs/House/vocal/Cmin", dsDir = "/lp/packs/House/vocal/D#", naDir = "/lp/packs/House/vocal/NA";
  const size_t cminFiles = treeFiles(cminDir, ".wav").size(), otherFiles = treeFiles(dsDir, ".wav").size() + treeFiles(naDir, ".wav").size();
  CHECK(cminFiles > otherFiles);
  char out[kMaxPathLen];
  int cmin = 0, other = 0, bpm = 0;
  for (int i = 0; i < 40; i++) {
    CHECK(l.pickVocal("House", key, 0, rng, out, sizeof out));
    CHECK(inTree(out, cminDir) || inTree(out, dsDir) || inTree(out, naDir));
    CHECK(endsWithCi(out, ".wav"));
    CHECK(fileExists(out));
    if (inTree(out, cminDir)) cmin++; else other++;
    if (strstr(out, "bpm")) bpm++;
  }
  CHECK_EQ(cmin + other, 40);
  CHECK(cmin > other);                               // weighted by count
  CHECK_EQ(bpm, 40);                                 // every House phrase carries a BPM token
}

TEST(pickVocal_sharded_key_folder_weighs_its_shards) {
  // E minor: House/vocal/Emin is sharded (438 phrases over s00 … s06 and rap on the 2026-10-16 card), G holds one
  // phrase, NA is sharded too (174 over three shards). The pick weighs the three content trees by count, reaches
  // every folder of every tree and every phrase of the two sharded trees comes out through the bags (the trees hold
  // more than kBagMax entries, so their bags are stratified: a full pass over a tree takes a few cycles).
  Library& l = lib();
  l.invalidate();
  const std::string emin = "/lp/packs/House/vocal/Emin", g = "/lp/packs/House/vocal/G", na = "/lp/packs/House/vocal/NA";
  CHECK(shardsOf(emin).size() >= 2);
  CHECK_EQ(l.folderCount(emin.c_str()), 0);
  const size_t eminFiles = treeFiles(emin, ".wav").size(), gFiles = treeFiles(g, ".wav").size(), naFiles = treeFiles(na, ".wav").size();
  CHECK_EQ(l.treeCount(emin.c_str()), (int)eminFiles);
  CHECK_EQ(l.treeCount(na.c_str()), (int)naFiles);
  auto foldersOf = [](const std::string& root) { std::vector<std::string> v = treeFolders(root); if (v.size() > 1) v.erase(v.begin()); return v; };   // a sharded root holds no files of its own
  std::set<std::string> expectDirs;
  for (const std::string& r : {emin, g, na}) if (treeFiles(r, ".wav").size()) for (const std::string& d : foldersOf(r)) expectDirs.insert(d);
  const int picks = 2500;                            // ≈ 4 × the 612 phrases: every stratum of both sharded trees cycles
  Rng rng(54);
  ProjectKey key; key.root = 4;
  char out[kMaxPathLen];
  std::map<std::string, int> perDir;
  std::set<std::string> names, eminNames, naNames;
  int fromEmin = 0;
  for (int i = 0; i < picks; i++) {
    CHECK(l.pickVocal("House", key, 1, rng, out, sizeof out));
    CHECK(inTree(out, emin) || inTree(out, g) || inTree(out, na));
    CHECK(fileExists(out));
    CHECK(strstr(out, "bpm") != nullptr);
    perDir[dirOf(out)]++;
    names.insert(baseOf(out));
    if (inTree(out, emin)) { fromEmin++; eminNames.insert(baseOf(out)); } else if (inTree(out, na)) naNames.insert(baseOf(out));
  }
  const double share = fromEmin / (double)picks, expected = (double)eminFiles / (double)(eminFiles + gFiles + naFiles);
  printf("    E minor vocal: %zu + %zu + %zu phrases, Emin share %.2f (expected %.2f), %zu distinct over %zu folders\n", eminFiles, gFiles, naFiles, share, expected, names.size(), perDir.size());
  for (const std::string& d : expectDirs) CHECK(perDir.count(d) == 1);   // every shard of Emin and NA, and G
  CHECK_EQ(perDir.size(), expectDirs.size());
  CHECK(share > expected - 0.10 && share < expected + 0.10);
  // A tree beyond kBagMax entries is stratified (entryOrdinal: a bag ordinal names a stratum, the cycle rotates through
  // its files), and a stratum skipped by a cycle (the no-immediate-repeat rule, a history rejection) misses the file its
  // rotation pointed at until the rotation comes round, so a bounded run cannot promise every last phrase; it must come
  // close (437 / 438 and 174 / 174 measured with this seed), which no unreachable shard or stratum would allow.
  printf("    reached %zu / %zu Emin and %zu / %zu NA phrases\n", eminNames.size(), eminFiles, naNames.size(), naFiles);
  CHECK(eminNames.size() * 100 >= eminFiles * 97);
  CHECK(naNames.size() * 100 >= naFiles * 97);
}

TEST(pickVocal_falls_back_to_NA_and_untagged_packs) {
  Library& l = lib();
  Rng rng(52);
  ProjectKey key; key.root = 9;                      // A minor: House has an Amin vocal folder (the RAP bank) but no relative-major C one
  char out[kMaxPathLen];
  int fromNA = 0;
  for (int i = 0; i < 10; i++) {                     // the picks span Amin and NA (weighted by count): NA is reached without a C folder
    CHECK(l.pickVocal("House", key, 1, rng, out, sizeof out));
    CHECK(inTree(out, "/lp/packs/House/vocal/Amin") || inTree(out, "/lp/packs/House/vocal/NA"));
    CHECK(fileExists(out));
    if (inTree(out, "/lp/packs/House/vocal/NA")) fromNA++;
  }
  CHECK(fromNA > 0);
  std::set<std::string> names;
  for (int i = 0; i < 20; i++) {                     // Testpack names have no BPM token: still returned
    CHECK(l.pickVocal("Testpack", key, 0, rng, out, sizeof out));
    CHECK(inTree(out, "/lp/packs/Testpack/vocal/NA"));
    CHECK(fileExists(out));
    names.insert(baseOf(out));
  }
  CHECK((int)names.size() >= 10);
  CHECK(!l.pickVocal("Nopack", key, 0, rng, out, sizeof out));
  CHECK(!l.pickVocal(nullptr, key, 0, rng, out, sizeof out));
}

TEST(pickVocal_redraws_loop_token_names_once_within_two_draws) {
  // SPEC §6: a vocal name must carry no "loop" token. A loop-named draw is repeated once (two of six names
  // here, so a loop name comes back only when both draws hit one); untagged names are returned as they are
  // (the generator re-picks them one per step, SPEC §9.3), which bounds the call at two opens + four reads.
  mkdirs("lp/packs/Scratch/vocal/NA");
  const char* files[] = {"NA__124bpm__Phrase_A.wav", "NA__bpm_128__Phrase_B.wav", "NA__Loop__124bpm__x.wav", "NA__untagged.wav", "NA__loops_124bpm.wav", "NA__300bpm__no.wav"};
  std::string index;
  for (const char* f : files) { touch(std::string("lp/packs/Scratch/vocal/NA/") + f); index += std::string(f) + "\n"; }
  writeFile("lp/packs/Scratch/vocal/NA/_index.txt", index);
  Library& l = cscratchLib();
  CountingFs& c = cscratch();
  l.dropCache();
  Rng rng(53);
  ProjectKey key; key.root = 9;
  char out[kMaxPathLen];
  CHECK(l.pickVocal("Scratch", key, 0, rng, out, sizeof out));   // counts the folders
  int loops = 0, good = 0, worstOpens = 0, worstReads = 0;
  for (int i = 0; i < 60; i++) {
    c.reset();
    CHECK(l.pickVocal("Scratch", key, 0, rng, out, sizeof out));
    std::string b = baseOf(out);
    if (b == "NA__Loop__124bpm__x.wav" || b == "NA__loops_124bpm.wav") loops++;
    if (b == "NA__124bpm__Phrase_A.wav" || b == "NA__bpm_128__Phrase_B.wav") good++;
    worstOpens = std::max(worstOpens, c.opens); worstReads = std::max(worstReads, c.reads);
    CHECK(c.opens <= 2); CHECK(c.reads <= 4); CHECK_EQ(c.walked, 0);
  }
  printf("    loop-named draws returned: %d of 60 (20 without the redraw), usable %d; worst %d opens / %d reads\n", loops, good, worstOpens, worstReads);
  CHECK(loops <= 8);
  CHECK(good >= 12);
}

// ---------------------------------------------------------------------------------------------- invalidate / history

TEST(invalidate_then_pick_still_works) {
  Library& l = lib();
  Rng rng(61);
  ProjectKey key; key.root = 9;
  char out[kMaxPathLen];
  CHECK(l.pickMidi(Chord, key, 0, rng, out, sizeof out));
  l.invalidate();
  CHECK(l.pickMidi(Chord, key, 0, rng, out, sizeof out));
  CHECK(fileExists(out));
  CHECK(l.pickSample("House", "snare", -1, rng, out, sizeof out));
  CHECK(fileExists(out));
  l.invalidate();
  l.invalidate();                                    // idempotent
  CHECK(l.pickVocal("House", key, 0, rng, out, sizeof out));
  CHECK(fileExists(out));
  CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
  CHECK(fileExists(out));
}

TEST(invalidate_resets_bags_to_a_fresh_cycle) {
  // After invalidate the 3-file Fx/NA folder is recounted and its bag restarts: three draws = all three files.
  Library& l = lib();
  Rng rng(62);
  ProjectKey key; key.root = 10;
  char out[kMaxPathLen];
  for (int i = 0; i < 2; i++) CHECK(l.pickMidi(Fx, key, 4, rng, out, sizeof out));
  l.invalidate();
  std::set<std::string> s;
  for (int i = 0; i < 3; i++) { CHECK(l.pickMidi(Fx, key, 4, rng, out, sizeof out)); s.insert(baseOf(out)); }
  CHECK_EQ((int)s.size(), 3);
}

TEST(history_avoids_recent_repeats_per_track) {
  // Chord/Amin (33) + C (4): the C folder cycles every 4 of its draws; the per-track history keeps a
  // file from returning within 8 picks of the same track as long as alternatives exist.
  Library& l = lib();
  l.invalidate();
  Rng rng(63);
  ProjectKey key; key.root = 9;
  char out[kMaxPathLen];
  std::vector<std::string> picks;
  for (int i = 0; i < 120; i++) { CHECK(l.pickMidi(Chord, key, 0, rng, out, sizeof out)); picks.push_back(out); }
  int repeats = 0;
  for (size_t i = 0; i < picks.size(); i++)
    for (size_t j = i + 1; j < picks.size() && j < i + kHistory; j++) if (picks[i] == picks[j]) repeats++;
  CHECK_EQ(repeats, 0);
}

TEST(history_is_skipped_for_tiny_candidate_sets) {
  // Fx in A# minor has 3 candidates in total: picks must keep flowing instead of being rejected.
  Library& l = lib();
  l.invalidate();
  Rng rng(64);
  ProjectKey key; key.root = 10;
  char out[kMaxPathLen];
  for (int i = 0; i < 30; i++) { CHECK(l.pickMidi(Fx, key, 4, rng, out, sizeof out)); CHECK(fileExists(out)); }
}

// ---------------------------------------------------------------------------------------------- size

TEST(sizeof_library_is_bounded) {
  printf("    sizeof(Library) = %zu B, sizeof(FolderIndex) = %zu B, sizeof(Bag) = %zu B\n", sizeof(Library), sizeof(FolderIndex), sizeof(Bag));
  // RAM diet 2026-10-16 (the device booted with 8.8 KB free): 16 trees × 268 B (a 56 B folder path, 12 one-byte
  // subfolder codes, 12 shard counts, a 138 B bag) + the 4 KB index chunk + 8 genre slots of 24 B (kGenreNameLen; were
  // 96 B: −576 B, the review's extra lever) + histories and the keyed kick cache: 9 488 B, 2 752 B under the 12 240 B of
  // the 96 B paths / 12 × 8 B shard names (404 B slots) / 96 B genres. Was 14.3 KB with 160 B paths and 16 B shard names,
  // 59 KB with kBagMax 512 and 48 folders.
  CHECK(sizeof(Library) <= 9 * 1024 + 512);
  CHECK(sizeof(FolderIndex) <= 272);
  CHECK_EQ(kGenreNameLen, 24);
  CHECK(sizeof(Bag) <= 140);
  CHECK_EQ(kBagMax, 64);
  CHECK_EQ(kFolderSlots, 16);
  CHECK_EQ(kTreeMax, 12);
  CHECK_EQ(kTreeNameLen, 8);                         // a decoded subfolder name ("A#min" the longest) and a terminator
  CHECK_EQ(kFolderPathLen, 56);                      // "/lp/packs/<genre>/loops/percussion/A#min" with a genre name of up to 22 chars
  CHECK(1 + kRootsMax <= kFolderSlots);              // one pick's slots (category folder + roots) always fit
}

TEST(subfolder_codes_round_trip_shards_key_folders_and_named_shards) {
  // One byte per subfolder (FolderIndex::kids): s00..s99, the 24 key labels and NA of a listing folder, the named
  // shards (rap/ of docs/RAP_BANK.md); any other name has no code and is dropped by the walk.
  char name[kTreeNameLen];
  for (int n = 0; n < kKidShards; n++) {
    char s[8]; snprintf(s, sizeof s, "s%02d", n);
    CHECK_EQ(folderKidCode(s), n);
    folderKidName((uint8_t)n, name); CHECK(std::string(name) == s);
  }
  for (int r = 0; r < 12; r++) {
    ProjectKey k; k.root = (uint8_t)r;
    char minorLabel[8], majorLabel[8]; k.labels(minorLabel, majorLabel);
    CHECK_EQ(folderKidCode(minorLabel), kKidKey + r);
    folderKidName((uint8_t)(kKidKey + r), name); CHECK(std::string(name) == minorLabel);
    const int majorRoot = (r + 3) % 12;                 // the relative major's label is noteName(root + 3)
    CHECK_EQ(folderKidCode(majorLabel), kKidKey + 12 + majorRoot);
    folderKidName((uint8_t)(kKidKey + 12 + majorRoot), name); CHECK(std::string(name) == majorLabel);
  }
  CHECK_EQ(folderKidCode("NA"), kKidNA); folderKidName((uint8_t)kKidNA, name); CHECK(std::string(name) == "NA");
  CHECK_EQ(folderKidCode("rap"), kKidNamed); folderKidName((uint8_t)kKidNamed, name); CHECK(std::string(name) == "rap");
  CHECK_EQ(kNamedShardCount, 1);
  for (const char* bad : {"", "s0", "s100", "sh01ab", "shard_01", "a", "b", "t00", "Cmaj", "H", "Amin ", "na", "Rap", "inst"}) CHECK_EQ(folderKidCode(bad), -1);
  CHECK_EQ(folderKidCode(nullptr), -1);
  folderKidName(kKidNone, name); CHECK_EQ((int)strlen(name), 0);
  folderKidName((uint8_t)(kKidNamed + kNamedShardCount), name); CHECK_EQ((int)strlen(name), 0);
  // every code decodes to a name that encodes back to it, and no two codes share a name
  std::set<std::string> names;
  for (int c = 0; c < kKidNamed + kNamedShardCount; c++) { folderKidName((uint8_t)c, name); CHECK_EQ(folderKidCode(name), c); names.insert(name); }
  CHECK_EQ((int)names.size(), kKidNamed + kNamedShardCount);
}

// ---------------------------------------------------------------------------------------------- _index.txt

TEST(index_file_drives_count_and_entries_scratch) {
  // Five files on disk, an index naming three of them: the index is the folder (SPEC §9.2 index files).
  mkdirs("lp/midi/Lead/Gmin");
  const char* files[] = {"Gmin__Lead__a.mid", "Gmin__Lead__b.mid", "Gmin__Lead__c.mid", "Gmin__Lead__d.mid", "Gmin__Lead__e.mid"};
  for (const char* f : files) touch(std::string("lp/midi/Lead/Gmin/") + f);
  writeFile("lp/midi/Lead/Gmin/_index.txt", "Gmin__Lead__b.mid\nGmin__Lead__d.mid\nGmin__Lead__e.mid\n");
  Library& l = scratchLib();
  l.dropCache();
  CHECK(l.folderUsesIndex("/lp/midi/Lead/Gmin"));
  CHECK_EQ(l.folderCount("/lp/midi/Lead/Gmin"), 3);
  char out[kMaxNameLen];
  CHECK(l.folderEntry("/lp/midi/Lead/Gmin", 0, out, sizeof out)); CHECK(std::string(out) == "Gmin__Lead__b.mid");
  CHECK(l.folderEntry("/lp/midi/Lead/Gmin", 1, out, sizeof out)); CHECK(std::string(out) == "Gmin__Lead__d.mid");
  CHECK(l.folderEntry("/lp/midi/Lead/Gmin", 2, out, sizeof out)); CHECK(std::string(out) == "Gmin__Lead__e.mid");
  CHECK(!l.folderEntry("/lp/midi/Lead/Gmin", 3, out, sizeof out)); CHECK_EQ((int)strlen(out), 0);
  CHECK(!l.folderEntry("/lp/midi/Lead/Gmin", -1, out, sizeof out));
  CHECK(!l.folderEntry("/lp/midi/Lead/Gmin", 0, nullptr, 0));
  char tiny[8];
  CHECK(!l.folderEntry("/lp/midi/Lead/Gmin", 0, tiny, sizeof tiny));   // does not fit: "" and false
  CHECK_EQ((int)strlen(tiny), 0);
  // picks only ever return indexed names, every one of them over a bag cycle
  Rng rng(71);
  ProjectKey key; key.root = 7;                      // G minor: Gmin, A#, NA (only Gmin exists here)
  char path[kMaxPathLen];
  std::set<std::string> seen;
  for (int i = 0; i < 30; i++) {
    CHECK(l.pickMidi(Lead, key, 1, rng, path, sizeof path));
    std::string b = baseOf(path);
    CHECK(b == "Gmin__Lead__b.mid" || b == "Gmin__Lead__d.mid" || b == "Gmin__Lead__e.mid");
    CHECK(scratchExists(path));
    seen.insert(b);
  }
  CHECK_EQ((int)seen.size(), 3);
}

TEST(index_accepts_crlf_blank_lines_and_missing_final_newline) {
  // Empty and whitespace-only lines are not entries (a pick of "  " would name a file that cannot exist);
  // names are trimmed of leading and trailing blanks, interior blanks stay ("Bb Kick.wav").
  mkdirs("lp/midi/Lead/A#");
  touch("lp/midi/Lead/A#/x.mid"); touch("lp/midi/Lead/A#/y.mid"); touch("lp/midi/Lead/A#/z z.mid");
  writeFile("lp/midi/Lead/A#/_index.txt", "x.mid\r\n\r\n\n  \r\n\t\r\n  y.mid \t\r\n\tz z.mid");
  Library& l = scratchLib();
  l.dropCache();
  CHECK(l.folderUsesIndex("/lp/midi/Lead/A#"));
  CHECK_EQ(l.folderCount("/lp/midi/Lead/A#"), 3);
  char out[kMaxNameLen];
  CHECK(l.folderEntry("/lp/midi/Lead/A#", 0, out, sizeof out)); CHECK(std::string(out) == "x.mid");
  CHECK(l.folderEntry("/lp/midi/Lead/A#", 1, out, sizeof out)); CHECK(std::string(out) == "y.mid");
  CHECK(l.folderEntry("/lp/midi/Lead/A#", 2, out, sizeof out)); CHECK(std::string(out) == "z z.mid");
  CHECK(!l.folderEntry("/lp/midi/Lead/A#", 3, out, sizeof out));
  Rng rng(78);
  ProjectKey key; key.root = 7;                      // G minor -> Gmin (3 indexed names), A# (these 3), NA (absent)
  char path[kMaxPathLen];
  for (int i = 0; i < 24; i++) {
    CHECK(l.pickMidi(Lead, key, 1, rng, path, sizeof path));
    CHECK(scratchExists(path));                      // never "/lp/midi/Lead/A#/  "
  }
}

TEST(folder_without_index_walks_and_an_added_index_takes_over_after_dropCache) {
  mkdirs("lp/midi/Lead/NA");
  const char* files[] = {"NA__Lead__1.mid", "NA__Lead__2.mid", "NA__Lead__3.mid", "NA__Lead__4.mid", "notes.txt"};
  for (const char* f : files) touch(std::string("lp/midi/Lead/NA/") + f);
  Library& l = scratchLib();
  l.dropCache();
  CHECK(!l.folderUsesIndex("/lp/midi/Lead/NA"));
  CHECK_EQ(l.folderCount("/lp/midi/Lead/NA"), 4);   // walk: .mid files only
  Rng rng(72);
  ProjectKey key; key.root = 11;                     // B minor: Bmin, D, NA -> only NA exists
  char path[kMaxPathLen];
  std::set<std::string> seen;
  for (int i = 0; i < 4; i++) { CHECK(l.pickMidi(Lead, key, 1, rng, path, sizeof path)); CHECK(scratchExists(path)); seen.insert(baseOf(path)); }
  CHECK_EQ((int)seen.size(), 4);
  writeFile("lp/midi/Lead/NA/_index.txt", "NA__Lead__2.mid\nNA__Lead__3.mid\n");
  CHECK_EQ(l.folderCount("/lp/midi/Lead/NA"), 4);   // cached (SPEC §6); invalidate() keeps counts, only bags restart
  l.invalidate();
  CHECK_EQ(l.folderCount("/lp/midi/Lead/NA"), 4);
  CHECK(!l.folderUsesIndex("/lp/midi/Lead/NA"));
  l.dropCache();                                     // the card changed: forget the counts
  CHECK(l.folderUsesIndex("/lp/midi/Lead/NA"));
  CHECK_EQ(l.folderCount("/lp/midi/Lead/NA"), 2);
  for (int i = 0; i < 6; i++) {
    CHECK(l.pickMidi(Lead, key, 1, rng, path, sizeof path));
    CHECK(baseOf(path) == "NA__Lead__2.mid" || baseOf(path) == "NA__Lead__3.mid");
  }
}

TEST(empty_index_falls_back_to_the_walk) {
  mkdirs("lp/midi/Chord/NA");
  touch("lp/midi/Chord/NA/NA__Chord__p.mid"); touch("lp/midi/Chord/NA/NA__Chord__q.mid"); touch("lp/midi/Chord/NA/NA__Chord__r.mid");
  writeFile("lp/midi/Chord/NA/_index.txt", "");
  Library& l = scratchLib();
  l.dropCache();
  CHECK(!l.folderUsesIndex("/lp/midi/Chord/NA"));
  CHECK_EQ(l.folderCount("/lp/midi/Chord/NA"), 3);
  char out[kMaxNameLen];
  CHECK(l.folderEntry("/lp/midi/Chord/NA", 2, out, sizeof out));
  CHECK(endsWithCi(out, ".mid"));
}

TEST(index_overlong_name_is_refused_but_keeps_its_ordinal) {
  mkdirs("lp/midi/Chord/B");
  std::string longName(kMaxNameLen + 20, 'z'); longName += ".mid";
  writeFile("lp/midi/Chord/B/_index.txt", "first.mid\n" + longName + "\nthird.mid\n");
  Library& l = scratchLib();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/midi/Chord/B"), 3);
  char out[kMaxNameLen];
  CHECK(l.folderEntry("/lp/midi/Chord/B", 0, out, sizeof out)); CHECK(std::string(out) == "first.mid");
  CHECK(!l.folderEntry("/lp/midi/Chord/B", 1, out, sizeof out)); CHECK_EQ((int)strlen(out), 0);
  CHECK(l.folderEntry("/lp/midi/Chord/B", 2, out, sizeof out)); CHECK(std::string(out) == "third.mid");
}

TEST(index_entries_resolve_across_chunk_edges_synthetic) {
  // An 80 KB index (beyond the 16-chunk table) whose lines are placed so that: a line straddles 4096, a
  // line ends exactly at 8191 (the next starts at 8192), a line's LF sits exactly at 12288 (chunk 3 opens
  // with "\n"), a CR/LF pair is split at 16384, and the last line has no newline. Every ordinal must come
  // back as its own line, read through the table (chunks 0..15) or streamed beyond it.
  mkdirs("lp/packs/Scratch/drum/perc");
  std::vector<std::string> lines;
  std::string content;
  int i = 0;
  while (content.size() < 80 * 1024) {
    char num[16]; snprintf(num, sizeof num, "%05d", i);
    std::string line = std::string("perc_") + num + "_" + std::string(8 + (i * 7) % 29, 'a' + i % 26) + ".wav";
    size_t off = content.size();
    std::string term = "\n";
    auto fit = [&](size_t endExclusive) {              // reshape the line to occupy exactly [off, endExclusive)
      line = line.substr(0, 11) + std::string(endExclusive - off - 15, 'x') + ".wav";   // keeps "perc_NNNNN_"
    };
    if (off < 8191 - 16 && off + line.size() + 1 > 8191 - 16) fit(8191);                        // LF at 8191
    else if (off < 12288 - 16 && off + line.size() + 1 > 12288 - 16) fit(12288);                // LF at 12288
    else if (off < 16383 - 16 && off + line.size() + 2 > 16383 - 16) { fit(16383); term = "\r\n"; }   // CR 16383, LF 16384
    lines.push_back(line);
    content += line + term;
    i++;
  }
  content.erase(content.size() - 1);                   // no final newline
  CHECK(content[8191] == '\n'); CHECK(content[8190] != '\n');
  CHECK(content[12288] == '\n'); CHECK(content[12287] != '\n');
  CHECK(content[16383] == '\r'); CHECK(content[16384] == '\n');
  writeFile("lp/packs/Scratch/drum/perc/_index.txt", content);
  Library& l = scratchLib();
  l.dropCache();
  CHECK(l.folderUsesIndex("/lp/packs/Scratch/drum/perc"));
  CHECK_EQ(l.folderCount("/lp/packs/Scratch/drum/perc"), (int)lines.size());
  CHECK((int)lines.size() > kBagMax);
  char out[kMaxNameLen];
  int bad = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (size_t n = 0; n < lines.size(); n++) {
    bool ok = l.folderEntry("/lp/packs/Scratch/drum/perc", (int)n, out, sizeof out);
    if (!ok || out != lines[n]) { if (bad++ < 5) printf("    entry %zu: ok=%d got '%s' want '%s'\n", n, (int)ok, out, lines[n].c_str()); }
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("    %zu lines, %zu bytes, %d mismatches, %.1f us per entry\n", lines.size(), content.size(), bad, ms * 1000.0 / lines.size());
  CHECK_EQ(bad, 0);
  CHECK(!l.folderEntry("/lp/packs/Scratch/drum/perc", (int)lines.size(), out, sizeof out));
  // the bag + strata reach every line: picks are index names and never repeat back to back
  Rng rng(73);
  std::string prev;
  std::set<std::string> seen;
  for (int k = 0; k < 400; k++) {
    char path[kMaxPathLen];
    CHECK(l.pickSample("Scratch", "perc", -1, rng, path, sizeof path));
    std::string b = baseOf(path);
    CHECK(b != prev); prev = b;
    CHECK(std::binary_search(lines.begin(), lines.end(), b));
    seen.insert(b);
  }
  CHECK((int)seen.size() > 300);
}

static bool coveredByWriter(const std::string& dir) {   // the folders tools/write_indexes.py indexes (SPEC §6 media)
  return dir.find("/lp/midi/") != std::string::npos || dir.find("/drum/") != std::string::npos || dir.find("/vocal/") != std::string::npos ||
         dir.find("/fx") != std::string::npos || dir.find("/inst/") != std::string::npos;
}

TEST(index_entries_match_lines_and_files_corpus) {
  // Every _index.txt tools/write_indexes.py wrote on the card (other modules keep their own, e.g. the sets
  // listing under presets/): the library's count is the line count and the media file count, entry n is
  // line n, and the writer sorted the names case-insensitively.
  std::vector<std::string> all, indexes;
  findIndexes(sdRoot() + "/lp", all);
  for (const std::string& idx : all) if (coveredByWriter(dirOf(idx))) indexes.push_back(idx);
  printf("    %zu index files (%zu on the card)\n", indexes.size(), all.size());
  CHECK(indexes.size() >= 100);
  Library& l = lib();
  l.invalidate();
  char out[kMaxNameLen];
  int folders = 0, entries = 0, mismatches = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (const std::string& idx : indexes) {
    std::string dir = dirOf(idx);
    std::string card = dir.substr(sdRoot().size());
    std::vector<std::string> lines = readLines(idx);
    const char* ext = card.find("/midi/") != std::string::npos ? ".mid" : ".wav";
    std::vector<std::string> files = listDir(dir, ext);
    CHECK(l.folderUsesIndex(card.c_str()));
    CHECK_EQ(l.folderCount(card.c_str()), (int)lines.size());
    CHECK_EQ((int)files.size(), (int)lines.size());
    std::sort(files.begin(), files.end());
    std::vector<std::string> sortedLines = lines;
    std::sort(sortedLines.begin(), sortedLines.end());
    CHECK(files == sortedLines);                     // the index names exactly the media files
    for (size_t n = 1; n < lines.size(); n++) CHECK(strcasecmp(lines[n - 1].c_str(), lines[n].c_str()) <= 0);
    for (size_t n = 0; n < lines.size(); n++) {
      bool ok = l.folderEntry(card.c_str(), (int)n, out, sizeof out);
      if (!ok || out != lines[n]) { if (mismatches++ < 5) printf("    %s entry %zu: got '%s' want '%s'\n", card.c_str(), n, out, lines[n].c_str()); }
      entries++;
    }
    folders++;
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("    %d folders, %d entries verified in %.0f ms, %d mismatches\n", folders, entries, ms, mismatches);
  CHECK_EQ(mismatches, 0);
  CHECK(entries > 6000);
}

TEST(beat_pick_completes_under_5ms_with_index) {
  // Wall-clock smoke test only (the host page cache hides SD costs; the SD budget is asserted in the
  // sd_budget_* tests through the counting filesystem). Beat/NA: 748 files over 12 shards, each with its own
  // index; the cold pick walks the category folder and the NA root and streams every shard's index.
  const std::string root = "/lp/midi/Beat/NA";
  CHECK(shardsOf(root).size() >= 2);
  for (const std::string& s : shardsOf(root)) CHECK(fileExists(root + "/" + s + "/_index.txt"));
  Library& l = lib();
  l.dropCache();
  Rng rng(74);
  char out[kMaxPathLen];
  auto msSince = [](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); };
  auto t0 = std::chrono::steady_clock::now();
  CHECK(l.pickMidi(Beat, ProjectKey(), 7, rng, out, sizeof out));
  double cold = msSince(t0);
  CHECK(!l.folderUsesIndex(root.c_str()));          // the root is walked for its shards ...
  for (const std::string& s : shardsOf(root)) CHECK(l.folderUsesIndex((root + "/" + s).c_str()));   // ... each of which is indexed
  CHECK_EQ(l.treeCount(root.c_str()), (int)treeFiles(root, ".mid").size());
  double maxMs = 0, sum = 0;
  const int n = 300;
  for (int i = 0; i < n; i++) {
    auto a = std::chrono::steady_clock::now();
    CHECK(l.pickMidi(Beat, ProjectKey(), 7, rng, out, sizeof out));
    double ms = msSince(a);
    CHECK(fileExists(out));
    CHECK(inTree(out, root));
    if (ms > maxMs) maxMs = ms;
    sum += ms;
  }
  printf("    Beat/NA pick: cold %.2f ms, warm avg %.3f ms, max %.3f ms over %d picks\n", cold, sum / n, maxMs, n);
  CHECK(maxMs < 5.0);
  CHECK(cold < 50.0);
  // the keyed kick (SPEC §9.7): House/drum/kick is sharded too; the cold pick counts every shard and scans
  // each index for the key root, a warm pick is one entry read
  const std::string kick = "/lp/packs/House/drum/kick";
  CHECK(shardsOf(kick).size() >= 2);
  l.dropCache();
  t0 = std::chrono::steady_clock::now();
  CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
  const double kickCold = msSince(t0);
  CHECK(inTree(out, kick));
  maxMs = 0; sum = 0;
  for (int i = 0; i < n; i++) {
    auto a = std::chrono::steady_clock::now();
    CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
    double ms = msSince(a);
    CHECK(fileExists(out));
    CHECK(startsWith(baseOf(out), "A__"));
    if (ms > maxMs) maxMs = ms;
    sum += ms;
  }
  printf("    keyed kick pick: cold %.2f ms, warm avg %.3f ms, max %.3f ms over %d picks\n", kickCold, sum / n, maxMs, n);
  CHECK(maxMs < 5.0);
  CHECK(kickCold < 50.0);
  // Percussion/NA (5 shards) and a tonal pick (Bass A minor: Amin 4 shards + C 2 shards) stay under the bound
  // once warm, and alternating them stays warm too: a tree is one cache slot whatever its shard count (5 slots
  // for both), so neither pick evicts the other's trees.
  ProjectKey key; key.root = 9;
  maxMs = 0;
  for (int i = 0; i < 100; i++) {
    auto a = std::chrono::steady_clock::now();
    CHECK(l.pickMidi(Percussion, ProjectKey(), 5, rng, out, sizeof out));
    if (i > 0 && msSince(a) > maxMs) maxMs = msSince(a);
    CHECK(inTree(out, "/lp/midi/Percussion/NA"));
  }
  printf("    Percussion pick: warm max %.3f ms\n", maxMs);
  CHECK(maxMs < 5.0);
  maxMs = 0;
  for (int i = 0; i < 100; i++) {
    auto a = std::chrono::steady_clock::now();
    CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
    if (i > 0 && msSince(a) > maxMs) maxMs = msSince(a);
  }
  printf("    Bass A minor pick: warm max %.3f ms\n", maxMs);
  CHECK(maxMs < 5.0);
  maxMs = 0;
  for (int i = 0; i < 20; i++) {
    auto a = std::chrono::steady_clock::now();
    CHECK(l.pickMidi(Percussion, ProjectKey(), 5, rng, out, sizeof out));
    CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
    if (msSince(a) > maxMs) maxMs = msSince(a);
  }
  printf("    Percussion + Bass pick pair: max %.3f ms\n", maxMs);
  CHECK(maxMs < 5.0);
}

// ---------------------------------------------------------------------------------------------- shards

TEST(tonal_key_folder_shards_are_weighted_scratch) {
  // D minor Pad: Dmin is a shard root without an index (one walk finds its loose file and the shards s00
  // (6 files, indexed) and s01 (2 files, walked)); F holds 3 files. All 12 are candidates, weighted by their
  // folder's count (SPEC §6, key folder shards).
  mkdirs("lp/midi/Pad/Dmin/s00"); mkdirs("lp/midi/Pad/Dmin/s01"); mkdirs("lp/midi/Pad/F");
  touch("lp/midi/Pad/Dmin/Dmin__Pad__root.mid");
  std::string s0;
  for (int i = 0; i < 6; i++) { std::string n = "Dmin__Pad__s0_" + std::to_string(i) + ".mid"; touch("lp/midi/Pad/Dmin/s00/" + n); s0 += n + "\n"; }
  writeFile("lp/midi/Pad/Dmin/s00/_index.txt", s0);
  touch("lp/midi/Pad/Dmin/s01/Dmin__Pad__s1_a.mid"); touch("lp/midi/Pad/Dmin/s01/Dmin__Pad__s1_b.mid");
  for (int i = 0; i < 3; i++) touch("lp/midi/Pad/F/Fmaj__Pad__" + std::to_string(i) + ".mid");
  Library& l = scratchLib();
  l.dropCache();
  Rng rng(75);
  ProjectKey key; key.root = 2;
  char out[kMaxPathLen];
  std::map<std::string, int> perDir;
  std::set<std::string> names;
  for (int i = 0; i < 600; i++) {
    CHECK(l.pickMidi(Pad, key, 2, rng, out, sizeof out));
    CHECK(scratchExists(out));
    perDir[dirOf(out)]++;
    names.insert(baseOf(out));
  }
  CHECK_EQ((int)names.size(), 12);
  CHECK_EQ((int)perDir.size(), 4);
  double s0Share = perDir["/lp/midi/Pad/Dmin/s00"] / 600.0, rootShare = perDir["/lp/midi/Pad/Dmin"] / 600.0;
  double s1Share = perDir["/lp/midi/Pad/Dmin/s01"] / 600.0, fShare = perDir["/lp/midi/Pad/F"] / 600.0;
  printf("    shares: root %.2f s0 %.2f s1 %.2f F %.2f (expected 0.08 0.50 0.17 0.25)\n", rootShare, s0Share, s1Share, fShare);
  CHECK(s0Share > 0.40 && s0Share < 0.60);
  CHECK(rootShare > 0.03 && rootShare < 0.15);
  CHECK(fShare > 0.17 && fShare < 0.33);
  // An indexed folder is a leaf: once Dmin carries an index naming its loose file, its shards are no longer
  // looked for (tools/write_indexes.py warns about a folder that mixes media and subfolders).
  writeFile("lp/midi/Pad/Dmin/_index.txt", "Dmin__Pad__root.mid\n");
  l.dropCache();
  perDir.clear();
  for (int i = 0; i < 60; i++) { CHECK(l.pickMidi(Pad, key, 2, rng, out, sizeof out)); perDir[dirOf(out)]++; }
  CHECK_EQ((int)perDir.size(), 2);
  CHECK(perDir.count("/lp/midi/Pad/Dmin") == 1 && perDir.count("/lp/midi/Pad/F") == 1);
  ::remove((scratchRoot() + "/lp/midi/Pad/Dmin/_index.txt").c_str());
  l.dropCache();
}

TEST(beat_shards_under_NA_scratch) {
  // Beat ignores the key (SPEC §9.9): /lp/midi/Beat/NA holds only shards s00 (indexed) and s01 (walked).
  mkdirs("lp/midi/Beat/NA/s00"); mkdirs("lp/midi/Beat/NA/s01");
  std::string a;
  for (int i = 0; i < 4; i++) { std::string n = "NA__Beat__a" + std::to_string(i) + ".mid"; touch("lp/midi/Beat/NA/s00/" + n); a += n + "\n"; }
  writeFile("lp/midi/Beat/NA/s00/_index.txt", a);
  touch("lp/midi/Beat/NA/s01/NA__Beat__b0.mid"); touch("lp/midi/Beat/NA/s01/NA__Beat__b1.mid");
  Library& l = scratchLib();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/midi/Beat/NA"), 0);   // nothing directly inside
  Rng rng(76);
  char out[kMaxPathLen];
  std::set<std::string> names;
  for (int root = 0; root < 12; root++) {
    ProjectKey key; key.root = (uint8_t)root;
    for (int i = 0; i < 3; i++) {
      CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
      CHECK(scratchExists(out));
      CHECK(dirOf(out) == "/lp/midi/Beat/NA/s00" || dirOf(out) == "/lp/midi/Beat/NA/s01");
      names.insert(baseOf(out));
    }
  }
  CHECK_EQ((int)names.size(), 6);
}

TEST(keyed_kick_scan_goes_through_the_index_scratch) {
  // The Scratch kick folder (8 files on disk) gets an index naming three: the keyed scan and the bag both
  // see only those three. (A content folder becomes a leaf the moment it carries an index.)
  writeFile("lp/packs/Scratch/drum/kick/_index.txt", "A_Kick.wav\nbd_808.wav\ncb_rytm.wav\n");
  Library& l = scratchLib();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/packs/Scratch/drum/kick"), 3);
  Rng rng(77);
  char out[kMaxPathLen];
  for (int i = 0; i < 6; i++) { CHECK(l.pickSample("Scratch", "kick", 9, rng, out, sizeof out)); CHECK(baseOf(out) == "A_Kick.wav"); }
  for (int i = 0; i < 6; i++) { CHECK(l.pickSample("Scratch", "kick", 11, rng, out, sizeof out)); CHECK(baseOf(out) == "cb_rytm.wav"); }
  std::set<std::string> fsharp;                      // F#-Kick.wav exists on disk but is not indexed: plain bag
  for (int i = 0; i < 9; i++) { CHECK(l.pickSample("Scratch", "kick", 6, rng, out, sizeof out)); fsharp.insert(baseOf(out)); }
  CHECK_EQ((int)fsharp.size(), 3);
  CHECK(fsharp.count("F#-Kick.wav") == 0);
  // a lone keyed file is returned even when avoided (no alternative); the plain bag honours avoid
  CHECK(l.pickSample("Scratch", "kick", 9, rng, out, sizeof out, "A_Kick.wav")); CHECK(baseOf(out) == "A_Kick.wav");
  for (int i = 0; i < 6; i++) { CHECK(l.pickSample("Scratch", "kick", -1, rng, out, sizeof out, "bd_808.wav")); CHECK(baseOf(out) != "bd_808.wav"); }
}

TEST(keyed_kick_scan_covers_every_shard_scratch) {
  // A sharded kick folder (SPEC §9.2): the role folder holds only the shards s00 (indexed), s01 (walked) and
  // s02 (indexed), each with one A kick among others. The keyed scan (SPEC §9.7) streams every shard's index
  // (or walks it) into tree ordinals, the keyed draw is uniform across the shards, `avoid` moves to another
  // shard's A kick, and without a keyed match the tree's bag covers all shards. Through the counting card:
  // every prepare step is one bounded operation and a warm pick one open (or the walk of s01) + <= 2 reads.
  mkdirs("lp/packs/Sharded/drum/kick/s00"); mkdirs("lp/packs/Sharded/drum/kick/s01"); mkdirs("lp/packs/Sharded/drum/kick/s02");
  touch("lp/packs/Sharded/drum/kick/s00/A_Kick.wav"); touch("lp/packs/Sharded/drum/kick/s00/C_Kick.wav");
  writeFile("lp/packs/Sharded/drum/kick/s00/_index.txt", "A_Kick.wav\nC_Kick.wav\n");
  touch("lp/packs/Sharded/drum/kick/s01/A__Kick__two.wav"); touch("lp/packs/Sharded/drum/kick/s01/D_Kick.wav"); touch("lp/packs/Sharded/drum/kick/s01/E_Kick.wav");
  touch("lp/packs/Sharded/drum/kick/s02/Amin__Kick__three.wav"); touch("lp/packs/Sharded/drum/kick/s02/F_Kick.wav");
  writeFile("lp/packs/Sharded/drum/kick/s02/_index.txt", "Amin__Kick__three.wav\nF_Kick.wav\n");
  const std::set<std::string> aKicks = {"A_Kick.wav", "A__Kick__two.wav", "Amin__Kick__three.wav"};
  Library& l = cscratchLib();
  CountingFs& c = cscratch();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/packs/Sharded/drum/kick"), 0);
  CHECK_EQ(l.treeCount("/lp/packs/Sharded/drum/kick"), 7);
  // prepare: the root walk, s00 (one chunk), s01 (one walk), s02 (one chunk), then the keyed scan of s00, s01, s02
  // (the root holds no files of its own: nothing to walk for the scan)
  l.dropCache();
  int steps = 0; bool ready = false;
  while (!ready && steps < 32) {
    c.reset(); ready = l.prepareSample("Sharded", "kick", 9); steps++;
    CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walks <= 1); CHECK(c.walked <= 32);
  }
  CHECK(ready);
  CHECK_EQ(steps, 7);
  c.reset();
  CHECK(l.prepareSample("Sharded", "kick", 9));      // warm: nothing to do
  CHECK_EQ(c.opens + c.reads + c.walks, 0);
  Rng rng(79);
  char out[kMaxPathLen];
  std::map<std::string, int> hits;
  for (int i = 0; i < 60; i++) {
    c.reset();
    CHECK(l.pickSample("Sharded", "kick", 9, rng, out, sizeof out));
    if (c.opens + c.walks > 1 || c.reads > 2) printf("    pick %d: %s -> %s\n", i, c.summary().c_str(), out);
    CHECK(c.opens + c.walks <= 1); CHECK(c.reads <= 2); CHECK(c.walked <= 3);
    CHECK(scratchExists(out));
    hits[baseOf(out)]++;
  }
  CHECK_EQ((int)hits.size(), 3);
  for (const std::string& a : aKicks) CHECK(hits[a] >= 8);           // uniform over the three shards' A kicks (20 each expected)
  std::string prev;                                  // avoid: never the avoided A kick, always another one
  for (int i = 0; i < 20; i++) {
    CHECK(l.pickSample("Sharded", "kick", 9, rng, out, sizeof out, prev.empty() ? nullptr : prev.c_str()));
    CHECK(aKicks.count(baseOf(out)) == 1);
    CHECK(baseOf(out) != baseOf(prev));
    prev = out;
  }
  l.invalidate();                                    // the keyed list survives invalidate() (counts and epochs stay) ...
  c.reset();
  CHECK(l.pickSample("Sharded", "kick", 9, rng, out, sizeof out));
  CHECK(aKicks.count(baseOf(out)) == 1);
  CHECK(c.opens + c.walks <= 1); CHECK(c.reads <= 2);
  std::set<std::string> all;                         // root 7 (G): no keyed file, every shard's bag is weighed (the keyed cache serves one root at a time)
  for (int i = 0; i < 40; i++) { CHECK(l.pickSample("Sharded", "kick", 7, rng, out, sizeof out)); CHECK(scratchExists(out)); all.insert(baseOf(out)); }
  CHECK_EQ((int)all.size(), 7);
  l.dropCache();                                     // ... and is rebuilt after dropCache: counts, scan and the draw in one call
  c.reset();
  CHECK(l.pickSample("Sharded", "kick", 9, rng, out, sizeof out));
  CHECK(aKicks.count(baseOf(out)) == 1);
  CHECK(c.reads >= 4); CHECK(c.walks >= 3);
  char rel[kEntryLen];                               // tree ordinals: the root's own files first (none), then shard by shard, relative names
  CHECK_EQ(l.folderCount("/lp/packs/Sharded/drum/kick"), 0);
  int found = 0;
  for (int n = 0; n < 7; n++) { CHECK(l.folderEntry("/lp/packs/Sharded/drum/kick", n, rel, sizeof rel)); if (strchr(rel, '/')) found++; CHECK(scratchExists(std::string("/lp/packs/Sharded/drum/kick/") + rel)); }
  CHECK_EQ(found, 7);
  CHECK(!l.folderEntry("/lp/packs/Sharded/drum/kick", 7, rel, sizeof rel));
}

// ---------------------------------------------------------------------------------------------- SD budget (SPEC §9.2)

TEST(sd_budget_warm_picks_cost_one_open_and_at_most_two_reads) {
  // Every warm pick (folders counted, keyed scan cached) is one open of the index plus one 4 KB read, two
  // when the name straddles a chunk edge; no directory entry is walked. Picks are made through the counting
  // filesystem over the real card.
  Library& l = clib();
  CountingFs& c = csd();
  l.dropCache();
  Rng rng(81);
  char out[kMaxPathLen];
  ProjectKey key; key.root = 9;                      // A minor
  const Category cats[] = {Chord, Lead, Pad, Bass, Fx, Percussion, Beat};
  for (Category cat : cats) {
    CHECK(l.pickMidi(cat, key, (int)cat, rng, out, sizeof out));          // cold: counts the folders
    int maxOpens = 0, maxReads = 0, walked = 0;
    for (int i = 0; i < 40; i++) {
      c.reset();
      CHECK(l.pickMidi(cat, key, (int)cat, rng, out, sizeof out));
      CHECK(fileExists(out));
      maxOpens = std::max(maxOpens, c.opens); maxReads = std::max(maxReads, c.reads); walked += c.walked;
    }
    printf("    %-10s warm pick: <= %d opens, <= %d reads, %d walked entries\n", categoryName(cat), maxOpens, maxReads, walked);
    CHECK_EQ(maxOpens, 1);
    CHECK(maxReads <= 2);
    CHECK_EQ(walked, 0);
  }
  // keyed kick (SPEC §9.7): the first keyed pick scans the index once, every later one draws from the cache
  CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
  int maxOpens = 0, maxReads = 0, walked = 0;
  for (int i = 0; i < 40; i++) {
    c.reset();
    CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
    CHECK(startsWith(baseOf(out), "A__"));
    maxOpens = std::max(maxOpens, c.opens); maxReads = std::max(maxReads, c.reads); walked += c.walked;
  }
  printf("    keyed kick warm pick: <= %d opens, <= %d reads, %d walked entries\n", maxOpens, maxReads, walked);
  CHECK_EQ(maxOpens, 1);
  CHECK(maxReads <= 2);
  CHECK_EQ(walked, 0);
  // other roles and the vocal of a pack whose names all carry a BPM token (House): one open
  const char* roles[] = {"snare", "hh", "oh", "perc"};
  for (const char* role : roles) {
    CHECK(l.pickSample("House", role, -1, rng, out, sizeof out));
    c.reset();
    CHECK(l.pickSample("House", role, -1, rng, out, sizeof out));
    CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  }
  ProjectKey cmin; cmin.root = 0;
  CHECK(l.pickVocal("House", cmin, 0, rng, out, sizeof out));
  maxOpens = 0; maxReads = 0; walked = 0;
  for (int i = 0; i < 40; i++) {
    c.reset();
    CHECK(l.pickVocal("House", cmin, 0, rng, out, sizeof out));
    maxOpens = std::max(maxOpens, c.opens); maxReads = std::max(maxReads, c.reads); walked += c.walked;
  }
  printf("    vocal warm pick: <= %d opens, <= %d reads, %d walked entries\n", maxOpens, maxReads, walked);
  CHECK_EQ(maxOpens, 1);
  CHECK(maxReads <= 2);
  CHECK_EQ(walked, 0);
  // a pack without BPM tokens (Testpack): untagged names are returned as they are, one draw (SPEC §9.3)
  CHECK(l.pickVocal("Testpack", key, 1, rng, out, sizeof out));
  for (int i = 0; i < 20; i++) {
    c.reset();
    CHECK(l.pickVocal("Testpack", key, 1, rng, out, sizeof out));
    CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  }
}

TEST(sd_budget_invalidate_keeps_counts_so_a_key_change_costs_no_sd_work) {
  // SPEC §9.9: a key change restarts the bags; the folder counts and index tables stay, so the first pick
  // after invalidate() is as cheap as a warm one (a genre or key change is a live action). Bass A minor (3
  // trees), Beat (2) and the keyed kick (1) share the 16-slot cache without evicting each other.
  Library& l = clib();
  CountingFs& c = csd();
  Rng rng(82);
  char out[kMaxPathLen];
  ProjectKey key; key.root = 9;
  CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
  l.invalidate();
  c.reset();
  CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walks, 0);
  CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
  l.invalidate();
  c.reset();
  CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walks, 0);
  CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
  l.invalidate();
  c.reset();
  CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));       // the keyed list survives as well
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walks, 0);
  CHECK(startsWith(baseOf(out), "A__"));
  // the bags did restart: the 3-file Fx/NA folder offers all three again
  ProjectKey asharp; asharp.root = 10;
  for (int i = 0; i < 2; i++) CHECK(l.pickMidi(Fx, asharp, 4, rng, out, sizeof out));
  l.invalidate();
  std::set<std::string> s;
  for (int i = 0; i < 3; i++) { CHECK(l.pickMidi(Fx, asharp, 4, rng, out, sizeof out)); s.insert(baseOf(out)); }
  CHECK_EQ((int)s.size(), 3);
}

TEST(sd_budget_prepare_steps_are_bounded_and_make_the_pick_warm) {
  // prepare*() spend at most one open + one 4 KB read, or one walk of <= 32 entries, per call (SPEC §9.2);
  // once they return true the pick itself is warm (one open, <= 2 reads, nothing walked).
  Library& l = clib();
  CountingFs& c = csd();
  Rng rng(83);
  char out[kMaxPathLen];
  ProjectKey key; key.root = 9;
  struct Case { Category cat; int steps; } cases[] = {{Beat, 0}, {Percussion, 0}, {Bass, 0}, {Chord, 0}, {Lead, 0}, {Pad, 0}, {Fx, 0}};
  for (Case& cs : cases) {
    l.dropCache();
    int steps = 0, worstOpens = 0, worstReads = 0, worstWalked = 0;
    bool ready = false;
    while (!ready && steps < 64) {
      c.reset();
      ready = l.prepareMidi(cs.cat, key);
      steps++;
      worstOpens = std::max(worstOpens, c.opens); worstReads = std::max(worstReads, c.reads); worstWalked = std::max(worstWalked, c.walked);
      CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walked <= 32); CHECK(c.walks <= 1);
    }
    CHECK(ready);
    cs.steps = steps;
    c.reset();
    CHECK(l.prepareMidi(cs.cat, key));                 // warm: no SD work at all
    CHECK_EQ(c.opens + c.reads + c.walks, 0);
    c.reset();
    CHECK(l.pickMidi(cs.cat, key, (int)cs.cat, rng, out, sizeof out));
    CHECK(fileExists(out));
    printf("    %-10s prepared in %2d steps (worst step %d opens %d reads %d walked); pick: %s\n", categoryName(cs.cat), steps, worstOpens, worstReads, worstWalked, c.summary().c_str());
    CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  }
  // Beat: one walk of /lp/midi/Beat (its NA root), the walk of NA for its shards, then every shard's index chunks
  CHECK_EQ(cases[0].steps, 1 + treeSteps("/lp/midi/Beat/NA"));
  CHECK_EQ(cases[1].steps, 1 + treeSteps("/lp/midi/Percussion/NA"));
  // Bass A minor: the Amin and C trees (root walk + shard chunks each), plus one step for the absent NA folder
  CHECK_EQ(cases[2].steps, treeSteps("/lp/midi/Bass/Amin") + treeSteps("/lp/midi/Bass/C") + 1);
  // keyed kick: the tree's count steps, then the keyed scan of every shard's index, one chunk per step
  l.dropCache();
  int steps = 0; bool ready = false;
  while (!ready && steps < 32) { c.reset(); ready = l.prepareSample("House", "kick", 9); steps++; CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walks <= 1); CHECK(c.walked <= 32); }
  CHECK(ready);
  CHECK_EQ(steps, treeSteps("/lp/packs/House/drum/kick") + treeChunks("/lp/packs/House/drum/kick"));
  c.reset();
  CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
  CHECK(startsWith(baseOf(out), "A__"));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  steps = 0; ready = false;                               // a plain role: the count steps only
  while (!ready && steps < 32) { c.reset(); ready = l.prepareSample("House", "snare", -1); steps++; CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walks <= 1); CHECK(c.walked <= 32); }
  CHECK_EQ(steps, treeSteps("/lp/packs/House/drum/snare"));
  c.reset();
  CHECK(l.pickSample("House", "snare", -1, rng, out, sizeof out));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  // vocal C minor: the Cmin and NA trees, one step for the absent D# folder
  ProjectKey cmin; cmin.root = 0;
  steps = 0; ready = false;
  while (!ready && steps < 32) { c.reset(); ready = l.prepareVocal("House", cmin); steps++; CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walked <= 32); }
  CHECK(ready);
  CHECK_EQ(steps, treeSteps("/lp/packs/House/vocal/Cmin") + treeSteps("/lp/packs/House/vocal/NA") + 1);
  // vocal E minor: the sharded Emin tree (its root walk and every shard's chunks) and the absent G folder; NA
  // is still cached from the C minor warm-up
  ProjectKey emin; emin.root = 4;
  steps = 0; ready = false;
  while (!ready && steps < 32) { c.reset(); ready = l.prepareVocal("House", emin); steps++; CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walks <= 1); CHECK(c.walked <= 32); }
  CHECK(ready);
  CHECK_EQ(steps, treeSteps("/lp/packs/House/vocal/Emin") + 1);
  c.reset();
  CHECK(l.pickVocal("House", emin, 0, rng, out, sizeof out));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  c.reset();
  CHECK(l.pickVocal("House", cmin, 0, rng, out, sizeof out));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
  // nothing to warm once the missing folders are known (one failed open + walk each): a missing pack or
  // role, or an absent file system
  c.reset();
  CHECK(l.prepareSample("Nopack", "kick", 9));
  CHECK_EQ(c.opens, 1); CHECK_EQ(c.failedOpens, 1); CHECK_EQ(c.reads, 0);
  CHECK(l.prepareSample("House", "nope", -1));
  steps = 0; ready = false;
  while (!ready && steps < 8) { c.reset(); ready = l.prepareVocal("Nopack", cmin); steps++; CHECK(c.opens <= 1); CHECK_EQ(c.reads, 0); CHECK_EQ(c.walked, 0); }
  CHECK_EQ(steps, 3);                                  // Cmin, D#, NA of the missing pack
  c.reset();
  CHECK(l.prepareVocal("Nopack", cmin));
  CHECK_EQ(c.opens + c.walks, 0);
  static Library none;
  CHECK(none.prepareMidi(Chord, key));
  CHECK(none.prepareSample("House", "kick", 9));
  CHECK(none.prepareVocal("House", key));
}

TEST(sd_budget_cold_pick_without_prepare_streams_the_index_in_one_call) {
  // A caller that does not prepare still gets a correct pick in one call; its cost is the whole tree of each
  // cold root: one index open per folder (the open of a sharded root's absent index fails and its walk lists
  // the shards), every shard's chunks, then the entry read — for Beat/NA 13 opens and 13 reads, i.e. above
  // the per-step budget on the device (SPEC §9.2); this test documents the bound. Only the category folder
  // and the sharded roots are walked (<= kTreeMax + a few entries each); the entry read of a shard's line
  // streams that shard's index from its start (<= 64 names: one chunk).
  Library& l = clib();
  CountingFs& c = csd();
  Rng rng(84);
  char out[kMaxPathLen];
  ProjectKey key; key.root = 9;
  auto folders = [](std::initializer_list<const char*> roots) { int n = 0; for (const char* r : roots) n += (int)treeFolders(r).size(); return n; };
  auto chunks = [](std::initializer_list<const char*> roots) { int n = 0; for (const char* r : roots) n += treeChunks(r); return n; };
  l.dropCache(); c.reset();
  CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
  printf("    cold Beat pick: %s\n", c.summary().c_str());
  CHECK(c.opens <= folders({"/lp/midi/Beat/NA"}) + 1); CHECK(c.reads <= chunks({"/lp/midi/Beat/NA"}) + 2);
  CHECK_EQ(c.walks, 2); CHECK(c.walked <= 2 * (kTreeMax + 4));     // the category folder and the NA root
  l.dropCache(); c.reset();
  CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
  printf("    cold Bass Amin pick: %s\n", c.summary().c_str());
  CHECK(c.opens <= folders({"/lp/midi/Bass/Amin", "/lp/midi/Bass/C"}) + 2); CHECK(c.reads <= chunks({"/lp/midi/Bass/Amin", "/lp/midi/Bass/C"}) + 2);
  CHECK_EQ(c.walks, 3); CHECK(c.walked <= 2 * (kTreeMax + 4));     // the Amin and C roots, the absent NA folder
  l.dropCache(); c.reset();
  CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
  printf("    cold keyed kick pick: %s\n", c.summary().c_str());
  CHECK(c.opens <= 2 * folders({"/lp/packs/House/drum/kick"}) + 1); CHECK(c.reads <= 2 * chunks({"/lp/packs/House/drum/kick"}) + 2);
  CHECK_EQ(c.walks, 1); CHECK(c.walked <= kTreeMax + 4);           // the role folder
  ProjectKey cmin; cmin.root = 0;
  l.dropCache(); c.reset();
  CHECK(l.pickVocal("House", cmin, 0, rng, out, sizeof out));
  printf("    cold House vocal Cmin pick: %s\n", c.summary().c_str());
  CHECK(c.opens <= folders({"/lp/packs/House/vocal/Cmin", "/lp/packs/House/vocal/NA"}) + 2);
  CHECK(c.reads <= chunks({"/lp/packs/House/vocal/Cmin", "/lp/packs/House/vocal/NA"}) + 2);
  CHECK(c.walked <= kTreeMax + 4);                                 // the sharded Cmin root lists its shards (its index open fails)
}

TEST(sd_budget_sharded_trees_survive_a_set_generate) {
  // The trees one set generate touches in track order (SPEC §9.5: Chord, Lead, Pad, Bass, Fx, Percussion with
  // its kit roles, Vocal, Beat with its kit roles) are 29 slots on this card, more than the 16-slot cache: since
  // the 2026-10-16 content wave 18 of them are sharded trees (one step per shard to rebuild, 3 … 13 steps) and 2 are
  // the Beat / Percussion listing folders, so not even the expensive ones fit. Library::folder evicts the cheapest
  // slot first (LRU among equals): the flat and absent folders (one step each) cycle through the slots the cheapest
  // trees leave, the expensive trees stay resident, and a second generate re-streams only the few cheapest sharded
  // trees that do not fit beside them — measured 19 steps / 33 KB against 112 steps / 238 KB for the first pass
  // (plain LRU over this working set re-streamed all 112, the cache being smaller than the sequential scan).
  Library& l = clib();
  CountingFs& c = csd();
  l.dropCache();
  ProjectKey key; key.root = 9;                      // A minor
  auto pass = [&](int& steps, int& bytes) {
    steps = 0; c.reset();
    auto warm = [&](std::function<bool()> prep) { int n = 0; while (n < 100 && !prep()) n++; CHECK(n < 100); steps += n; };
    for (Category cat : {Chord, Lead, Pad, Bass, Fx}) warm([&] { return l.prepareMidi(cat, key); });
    warm([&] { return l.prepareMidi(Percussion, key); });
    for (const char* role : {"perc", "clap", "cymbal"}) warm([&] { return l.prepareSample("House", role, -1); });
    warm([&] { return l.prepareVocal("House", key); });
    warm([&] { return l.prepareMidi(Beat, key); });
    warm([&] { return l.prepareSample("House", "kick", 9); });
    for (const char* role : {"snare", "hh", "oh"}) warm([&] { return l.prepareSample("House", role, -1); });
    bytes = c.readBytes;
  };
  int s1 = 0, b1 = 0, s2 = 0, b2 = 0;
  pass(s1, b1);
  pass(s2, b2);
  printf("    set generate working set: first pass %d prepare steps / %d B read, second pass %d steps / %d B\n", s1, b1, s2, b2);
  CHECK(s1 >= 60);
  CHECK(s2 <= 24);                                   // the 11 flat or absent folders and the 2 listing walks, one step each, plus the two or three cheapest sharded trees (19 measured)
  CHECK(b2 * 4 <= b1);
  c.reset();                                         // the drum trees (one slot each, no listing folder) are still warm
  CHECK(l.prepareSample("House", "kick", 9));
  for (const char* role : {"snare", "hh", "oh", "perc", "clap", "cymbal"}) CHECK(l.prepareSample("House", role, -1));
  CHECK_EQ(c.opens + c.reads + c.walks, 0);
  // Beat and Percussion: their NA trees (13 and 6 steps) are warm; only a category listing folder (one walk, the
  // cheapest resident slot once the flats are gone) may have to be walked again, never a shard index.
  c.reset(); CHECK(l.prepareMidi(Beat, key)); CHECK(c.walks <= 1); CHECK_EQ(c.reads, 0);
  c.reset(); CHECK(l.prepareMidi(Percussion, key)); CHECK(c.walks <= 1); CHECK_EQ(c.reads, 0);
  char out[kMaxPathLen]; Rng rng(91);
  c.reset();
  CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out)); CHECK(fileExists(out));
  CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK(c.walked <= kTreeMax + 4);   // the entry read, and at most the category folder's own listing
}

// ---------------------------------------------------------------------------------------------- unresolvable lines

TEST(overlong_index_names_are_redrawn_without_recount_and_other_folders_still_pick) {
  // Chord in B minor: Bmin's index (3 chunks) names only files longer than kMaxNameLen, B holds two good
  // names around one overlong line, NA is a walked folder of three files. A draw that lands on an overlong
  // line is redrawn (no re-read of the whole index, the count stands); when a whole folder is unresolvable
  // the pick moves on to another candidate instead of failing.
  mkdirs("lp/midi/Chord/Bmin");
  std::string bminIndex;
  for (int i = 0; i < 110; i++) bminIndex += "Bmin__Chord__" + std::string(kMaxNameLen, 'a' + i % 26) + ".mid\n";   // 110 × ~114 B
  writeFile("lp/midi/Chord/Bmin/_index.txt", bminIndex);
  CHECK(bminIndex.size() > 2 * (size_t)kIndexChunk);
  Library& l = cscratchLib();
  CountingFs& c = cscratch();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/midi/Chord/Bmin"), 110);
  CHECK_EQ(l.folderCount("/lp/midi/Chord/B"), 3);
  CHECK_EQ(l.folderCount("/lp/midi/Chord/NA"), 3);
  CHECK_EQ(l.folderCount("/lp/midi/Chord/D"), 0);     // the relative major folder is absent (counted once here)
  Rng rng(85);
  ProjectKey key; key.root = 11;                     // B minor: Bmin, D (absent), NA
  char out[kMaxPathLen];
  int fromNa = 0, worstOpens = 0, worstReads = 0;
  for (int i = 0; i < 60; i++) {
    c.reset();
    CHECK(l.pickMidi(Chord, key, 0, rng, out, sizeof out));
    CHECK(dirOf(out) == "/lp/midi/Chord/NA");
    CHECK(scratchExists(out));
    fromNa++;
    worstOpens = std::max(worstOpens, c.opens); worstReads = std::max(worstReads, c.reads);
    CHECK(c.opens <= 4);                             // at most kResolveTries draws in Bmin, each one open
    CHECK(c.reads <= 8);                             // ... and <= 2 reads: never the 3-chunk recount
  }
  printf("    60 picks with an unresolvable Bmin: worst %d opens / %d reads per pick\n", worstOpens, worstReads);
  CHECK_EQ(fromNa, 60);
  CHECK_EQ(l.folderCount("/lp/midi/Chord/Bmin"), 110);   // never recounted
  // B major (root 11 major label "B"): the good names of B are returned, the overlong one never
  for (int i = 0; i < 6; i++) {
    char p[kMaxNameLen];
    CHECK(l.folderEntry("/lp/midi/Chord/B", i % 3, p, sizeof p) == (i % 3 != 1));
  }
  ::remove((scratchRoot() + "/lp/midi/Chord/Bmin/_index.txt").c_str());
  ::rmdir((scratchRoot() + "/lp/midi/Chord/Bmin").c_str());
  l.dropCache();
}

TEST(missing_index_line_recounts_the_folder_once) {
  // The index shrinks on the card after it was counted (6 -> 2 lines): a draw beyond the new end is reported
  // missing, the folder is recounted in the same pick and the draw repeated.
  mkdirs("lp/midi/Fx/Emin");
  std::string six;
  for (int i = 0; i < 6; i++) { std::string n = "Emin__Fx__" + std::to_string(i) + ".mid"; touch("lp/midi/Fx/Emin/" + n); six += n + "\n"; }
  writeFile("lp/midi/Fx/Emin/_index.txt", six);
  Library& l = cscratchLib();
  CountingFs& c = cscratch();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/midi/Fx/Emin"), 6);
  writeFile("lp/midi/Fx/Emin/_index.txt", "Emin__Fx__0.mid\nEmin__Fx__1.mid\n");
  CHECK_EQ(l.folderCount("/lp/midi/Fx/Emin"), 6);     // cached
  Rng rng(86);
  ProjectKey key; key.root = 4;                      // E minor: Emin, G (absent), NA (absent)
  char out[kMaxPathLen];
  for (int i = 0; i < 12; i++) {
    c.reset();
    CHECK(l.pickMidi(Fx, key, 4, rng, out, sizeof out));
    CHECK(baseOf(out) == "Emin__Fx__0.mid" || baseOf(out) == "Emin__Fx__1.mid");
    CHECK(c.reads <= 4);                             // draw + recount + draw at most once
  }
  CHECK_EQ(l.folderCount("/lp/midi/Fx/Emin"), 2);
}

TEST(named_shard_rap_is_reachable_and_uncoded_subfolders_are_reported) {
  // One-byte subfolder codes (RAM diet 2026-10-16): the named shard "rap" (tools/make_rap.py beside the sNN of a
  // sharded key folder, docs/RAP_BANK.md) takes part in Beat picks like a sNN shard; "sh01ab" (6 chars, kept by the old
  // name-length rule) and "shard_01" have no code, are ignored by the picks and reported through lastError (tools/
  // write_indexes.py warns about such names too).
  mkdirs("lp/midi/Beat/NA/rap"); mkdirs("lp/midi/Beat/NA/sh01ab"); mkdirs("lp/midi/Beat/NA/shard_01");
  touch("lp/midi/Beat/NA/rap/NA__Beat__s12_0.mid"); touch("lp/midi/Beat/NA/rap/NA__Beat__s12_1.mid");
  writeFile("lp/midi/Beat/NA/rap/_index.txt", "NA__Beat__s12_0.mid\nNA__Beat__s12_1.mid\n");
  touch("lp/midi/Beat/NA/sh01ab/NA__Beat__s15.mid");
  touch("lp/midi/Beat/NA/shard_01/NA__Beat__s16.mid");
  Library& l = scratchLib();
  l.dropCache();
  CHECK_EQ((int)strlen(l.lastError()), 0);
  Rng rng(87);
  char out[kMaxPathLen];
  std::set<std::string> dirs, names;
  for (int i = 0; i < 80; i++) {
    CHECK(l.pickMidi(Beat, ProjectKey(), 7, rng, out, sizeof out));
    CHECK(scratchExists(out));
    dirs.insert(dirOf(out)); names.insert(baseOf(out));
  }
  CHECK(dirs.count("/lp/midi/Beat/NA/rap") == 1);
  CHECK(dirs.count("/lp/midi/Beat/NA/sh01ab") == 0);
  CHECK(dirs.count("/lp/midi/Beat/NA/shard_01") == 0);
  CHECK_EQ((int)names.size(), 8);                    // s00 (4) + s01 (2) + rap (2)
  CHECK(std::string(l.lastError()) == "Beat/NA: 2 shards unreachable");
  char rel[kEntryLen];                               // the rap shard's entries are "rap/<name>" like a sNN shard's
  int rapEntries = 0;
  for (int n = 0; n < 8; n++) { CHECK(l.folderEntry("/lp/midi/Beat/NA", n, rel, sizeof rel)); if (strncmp(rel, "rap/", 4) == 0) rapEntries++; }
  CHECK_EQ(rapEntries, 2);
  l.dropCache();                                     // the card changed: the report is cleared
  CHECK_EQ((int)strlen(l.lastError()), 0);
}

TEST(more_than_kTreeMax_shards_are_reported_and_the_first_kTreeMax_are_used) {
  // A content folder of 14 shards (one indexed file each): the walk keeps kTreeMax of them, the tree counts
  // those 12 files, every pick stays inside them and lastError names the folder and the 2 shards no pick can
  // reach (tools/write_indexes.py refuses such a card; tools/shard_folders.py never makes one).
  for (int i = 0; i < kTreeMax + 2; i++) {
    char sub[8]; snprintf(sub, sizeof sub, "s%02d", i);
    std::string dir = std::string("lp/packs/Many/drum/perc/") + sub;
    mkdirs(dir);
    std::string name = std::string("perc_") + sub + ".wav";
    touch(dir + "/" + name); writeFile(dir + "/_index.txt", name + "\n");
  }
  Library& l = scratchLib();
  l.dropCache();
  CHECK_EQ(l.folderCount("/lp/packs/Many/drum/perc"), 0);
  CHECK_EQ(l.treeCount("/lp/packs/Many/drum/perc"), kTreeMax);
  CHECK(std::string(l.lastError()) == "drum/perc: 2 shards unreachable");
  Rng rng(89);
  char out[kMaxPathLen];
  std::set<std::string> names;
  for (int i = 0; i < 60; i++) { CHECK(l.pickSample("Many", "perc", -1, rng, out, sizeof out)); CHECK(scratchExists(out)); names.insert(baseOf(out)); }
  CHECK_EQ((int)names.size(), kTreeMax);             // the bag cycles over exactly the reachable files
  l.dropCache();
}

TEST(prepare_converges_when_three_sharded_roots_hold_more_shards_than_cache_slots) {
  // F minor Bass on the scratch card: Fmin 7 shards, G# 7 shards, NA 3 shards — 17 shards under 3 roots, more
  // than the 16 cache slots. A tree is one slot whatever its shard count, so the warm-up holds 3 slots,
  // converges in exactly one step per root walk and per shard index (20), and the warm pick (one open, <= 2
  // reads) reaches every shard and every file. (With one slot per shard the 20 folders evicted each other on
  // every step and prepare never returned true.)
  const char* roots[] = {"Fmin", "G#", "NA"}; const int shards[] = {7, 7, 3};
  int files = 0;
  for (int r = 0; r < 3; r++) {
    for (int s = 0; s < shards[r]; s++) {
      char sub[8]; snprintf(sub, sizeof sub, "s%02d", s);
      std::string dir = std::string("lp/midi/Bass/") + roots[r] + "/" + sub;
      mkdirs(dir);
      std::string idx;
      for (int i = 0; i < 2; i++) { std::string n = std::string(roots[r]) + "__Bass__" + sub + "_" + std::to_string(i) + ".mid"; touch(dir + "/" + n); idx += n + "\n"; files++; }
      writeFile(dir + "/_index.txt", idx);
    }
  }
  CHECK_EQ(files, 34);
  Library& l = cscratchLib();
  CountingFs& c = cscratch();
  l.dropCache();
  ProjectKey key; key.root = 5;                      // F minor: Fmin, G#, NA
  int steps = 0; bool ready = false;
  while (!ready && steps < 200) {
    c.reset(); ready = l.prepareMidi(Bass, key); steps++;
    CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.walks <= 1); CHECK(c.walked <= 32);
  }
  CHECK(ready);
  CHECK_EQ(steps, 3 + 7 + 7 + 3);
  c.reset();
  CHECK(l.prepareMidi(Bass, key));                   // warm: nothing to do
  CHECK_EQ(c.opens + c.reads + c.walks, 0);
  CHECK_EQ(l.treeCount("/lp/midi/Bass/Fmin"), 14); CHECK_EQ(l.treeCount("/lp/midi/Bass/G#"), 14); CHECK_EQ(l.treeCount("/lp/midi/Bass/NA"), 6);
  CHECK_EQ(l.folderCount("/lp/midi/Bass/Fmin"), 0);
  Rng rng(90);
  char out[kMaxPathLen];
  std::set<std::string> dirs, names;
  for (int i = 0; i < 300; i++) {
    c.reset();
    CHECK(l.pickMidi(Bass, key, 3, rng, out, sizeof out));
    CHECK(scratchExists(out));
    CHECK(c.opens <= 1); CHECK(c.reads <= 2); CHECK_EQ(c.walked, 0);
    dirs.insert(dirOf(out)); names.insert(baseOf(out));
  }
  CHECK_EQ((int)dirs.size(), 17);
  CHECK_EQ((int)names.size(), 34);
  c.reset();
  CHECK(l.prepareMidi(Bass, key));                   // the picks evicted nothing
  CHECK_EQ(c.opens + c.reads + c.walks, 0);
}

// ---------------------------------------------------------------------------------------------- file handles

// ---------------------------------------------------------------------------------------------- loop picks (SPEC §9.19)

static std::string keyFolderOf(const std::string& path, const std::string& base) {   // the key folder right under a loops/<category> base
  std::string rel = path.substr(base.size() + 1);
  return rel.substr(0, rel.find('/'));
}

TEST(pickLoop_tonal_categories_weigh_minor_major_and_NA_roots) {
  Library& l = lib();
  l.invalidate();
  Rng rng(61);
  char out[kMaxPathLen];
  // D minor chord loops: Dmin and the relative major F exist, NA does not: both reached, weighted by count, nothing else
  ProjectKey key; key.root = 2;
  const std::string base = "/lp/packs/House/loops/chord";
  const size_t dmin = treeFiles(base + "/Dmin", ".wav").size(), f = treeFiles(base + "/F", ".wav").size();
  CHECK(dmin > 0 && f > 0); CHECK(listDir(sdRoot() + base + "/NA", nullptr).empty());
  std::map<std::string, int> perDir; std::set<std::string> names;
  for (int i = 0; i < 200; i++) {
    CHECK(l.pickLoop("House", Chord, key, 0, rng, out, sizeof out));
    CHECK(inTree(out, base + "/Dmin") || inTree(out, base + "/F")); CHECK(fileExists(out)); CHECK(endsWithCi(out, ".wav"));
    CHECK(std::string(out).find("__LOOP__") != std::string::npos);
    perDir[dirOf(out)]++; names.insert(baseOf(out));
  }
  CHECK_EQ(perDir.size(), 2u); CHECK_EQ(names.size(), dmin + f);
  const double share = perDir[base + "/Dmin"] / 200.0, expected = (double)dmin / (double)(dmin + f);
  printf("    D minor chord loops: Dmin %zu + F %zu files, Dmin share %.2f (expected %.2f)\n", dmin, f, share, expected);
  CHECK(share > expected - 0.12 && share < expected + 0.12);
  CHECK_EQ(l.loopTotal("House", Chord, key), (int)(dmin + f));
  // A minor bass loops: Amin exists, C and NA do not: Amin only, every file reached
  key.root = 9;
  std::set<std::string> amin;
  for (int i = 0; i < 40; i++) { CHECK(l.pickLoop("House", Bass, key, 3, rng, out, sizeof out)); CHECK(inTree(out, "/lp/packs/House/loops/bass/Amin")); CHECK(fileExists(out)); amin.insert(baseOf(out)); }
  CHECK_EQ(amin.size(), treeFiles("/lp/packs/House/loops/bass/Amin", ".wav").size());
  // the Vocal track's loops come from loops/vocal with the same roots, never from the phrase folders
  for (int i = 0; i < 20; i++) { CHECK(l.pickLoop("House", Vocal, key, 6, rng, out, sizeof out)); CHECK(inTree(out, "/lp/packs/House/loops/vocal/Amin")); CHECK(fileExists(out)); }
  // pad loops in G minor: no Gmin, no A#: NA alone
  key.root = 7;
  for (int i = 0; i < 5; i++) { CHECK(l.pickLoop("House", Pad, key, 2, rng, out, sizeof out)); CHECK(inTree(out, "/lp/packs/House/loops/pad/NA")); }
  // lead loops in C# minor: C#min, E and NA all absent: nothing to pick, and the library says so without card work once it knows
  key.root = 1;
  CHECK(!l.pickLoop("House", Lead, key, 1, rng, out, sizeof out)); CHECK(out[0] == 0);
  CHECK_EQ(l.loopTotal("House", Lead, key), 0);
  CHECK(!l.pickLoop("Testpack", Chord, key, 0, rng, out, sizeof out));   // no loops folder at all
  CHECK(!l.pickLoop("Nopack", Chord, key, 0, rng, out, sizeof out));
  CHECK(!l.pickLoop(nullptr, Chord, key, 0, rng, out, sizeof out));
  CHECK(!l.pickLoop("House", Chord, key, 0, rng, out, 8));            // a buffer too small for any path
  CHECK_EQ(l.loopTotal(nullptr, Chord, key), 0);
}

TEST(pickLoop_drum_percussion_fx_use_NA_and_every_key_folder) {
  Library& l = lib();
  l.invalidate();
  Rng rng(62);
  char out[kMaxPathLen];
  struct Case { Category c; const char* folder; } cases[] = {{Beat, "drum"}, {Percussion, "percussion"}, {Fx, "fx"}};
  for (const Case& cs : cases) {
    const std::string base = std::string("/lp/packs/House/loops/") + cs.folder;
    std::map<std::string, size_t> count; size_t total = 0, nonEmpty = 0;
    for (const std::string& k : listDir(sdRoot() + base, nullptr, true)) { count[k] = treeFiles(base + "/" + k, ".wav").size(); total += count[k]; if (count[k]) nonEmpty++; }
    CHECK(total > 0); CHECK(count.count("NA") == 1 && count["NA"] > 0);
    std::map<std::string, int> perKey; std::set<std::string> names;
    const int n = 600;
    for (int i = 0; i < n; i++) {
      ProjectKey key; key.root = (uint8_t)(i % 12);                    // whatever the key: the roots are NA + every key folder present
      CHECK(l.pickLoop("House", cs.c, key, 7, rng, out, sizeof out));
      CHECK(fileExists(out)); CHECK(startsWith(out, base + "/"));
      perKey[keyFolderOf(out, base)]++; names.insert(baseOf(out));
    }
    const double naShare = perKey["NA"] / (double)n, expected = (double)count["NA"] / (double)total;
    printf("    %-10s %zu key folders (%zu files, NA %zu): %zu folders reached, %zu distinct files, NA share %.2f (expected %.2f)\n", cs.folder, count.size(), total, count["NA"], perKey.size(), names.size(), naShare, expected);
    CHECK_EQ(perKey.size(), nonEmpty);                                   // NA and every key folder with files
    CHECK_EQ(names.size(), total);                                       // every loop of the category reachable
    CHECK(naShare > expected - 0.10 && naShare < expected + 0.10);       // weighted by count
    ProjectKey key; key.root = 9;
    CHECK_EQ(l.loopTotal("House", cs.c, key), (int)total);
  }
}

TEST(prepareLoop_is_resumable_and_a_warm_pick_costs_one_open) {
  Library& l = clib(); CountingFs& fs = csd();
  l.dropCache(); l.invalidate();
  ProjectKey key; key.root = 9; Rng rng(63); char out[kMaxPathLen];
  // Beat (drum): the category listing, NA and the key folders are each one bounded step (an index chunk or one walk)
  fs.reset();
  CHECK_EQ(l.loopTotal("House", Beat, key), -1); CHECK_EQ(fs.opens + fs.walks, 0);   // cold: unknown, and the question costs no card work
  int steps = 0, worstOpens = 0, worstReads = 0, worstWalked = 0; bool done = false;
  while (!done && steps < 64) {
    fs.reset(); done = l.prepareLoop("House", Beat, key); steps++;
    if (fs.opens > worstOpens) worstOpens = fs.opens; if (fs.reads > worstReads) worstReads = fs.reads; if (fs.walked > worstWalked) worstWalked = fs.walked;
  }
  const size_t keyFolders = listDir(sdRoot() + "/lp/packs/House/loops/drum", nullptr, true).size();
  printf("    drum loops prepared in %d steps (%zu folders; worst step %d opens %d reads %d walked)\n", steps, keyFolders, worstOpens, worstReads, worstWalked);
  CHECK(done); CHECK(steps <= (int)keyFolders + 2); CHECK(steps >= (int)keyFolders);
  CHECK(worstOpens <= 1); CHECK(worstReads <= 1); CHECK(worstWalked <= 32);
  CHECK(l.loopTotal("House", Beat, key) > 0);
  fs.reset();
  CHECK(l.pickLoop("House", Beat, key, 7, rng, out, sizeof out));
  printf("    warm drum pick: %s\n", fs.summary().c_str());
  CHECK_EQ(fs.opens, 1); CHECK(fs.reads <= 2); CHECK_EQ(fs.walks, 0);
  CHECK(l.prepareLoop("House", Beat, key));                            // warm: true at once, no card work
  // a tonal pick warms its ≤ 3 roots
  l.dropCache(); key.root = 2; steps = 0; done = false;
  while (!done && steps < 64) { done = l.prepareLoop("House", Chord, key); steps++; }
  CHECK(done); CHECK(steps <= kRootsMax);
  fs.reset(); CHECK(l.pickLoop("House", Chord, key, 0, rng, out, sizeof out)); CHECK_EQ(fs.opens, 1); CHECK(fs.reads <= 2); CHECK_EQ(fs.walks, 0);
  CHECK(l.prepareLoop(nullptr, Chord, key)); CHECK(l.prepareLoop("", Chord, key));
  CHECK_EQ(fs.failedOpens, 0);
}

// SPEC §9.19g: the loop pick's warm-up on a FULL cache (the device's after a set generate: kFolderSlots trees of MIDI, kit and
// vocal roots) stays ONE card operation per step — the category listing (one walk), NA and every key folder (one index chunk
// or one walk each), slot by slot, never evicting its own roots while it runs — and the pick afterwards is one open and at
// most two reads. The per-step accounting behind SPEC §9.19f's 152 ms step: no step of the library lists more than one folder.
TEST(prepareLoop_on_a_full_cache_is_one_operation_per_step_for_every_root) {
  Library& l = clib(); CountingFs& c = csd();
  ProjectKey key; key.root = 9; Rng rng(77); char out[kMaxPathLen];
  l.dropCache(); l.invalidate();
  for (Category cat : {Chord, Lead, Pad, Bass, Fx, Percussion, Beat}) for (int i = 0; i < 128 && !l.prepareMidi(cat, key); i++) {}
  for (const char* role : {"kick", "snare", "hh", "oh", "perc"}) for (int i = 0; i < 128 && !l.prepareSample("House", role, role[0] == 'k' ? 9 : -1); i++) {}
  for (int i = 0; i < 128 && !l.prepareVocal("House", key); i++) {}
  struct Case { Category cat; const char* folder; } cases[] = {{Beat, "drum"}, {Percussion, "percussion"}, {Fx, "fx"}, {Bass, "bass"}, {Chord, "chord"}};
  for (const Case& cs : cases) {
    const std::string base = std::string("/lp/packs/House/loops/") + cs.folder;
    CHECK_EQ(l.loopTotal("House", cs.cat, key), -1);          // cold, and the question costs nothing
    int expect = 0;
    if (loopIgnoresKey(cs.cat)) {                              // the listing, NA (by name) and every key folder listed
      expect = 1 + treeSteps(base + "/NA");
      for (const std::string& k : listDir(sdRoot() + base, nullptr, true)) if (k != "NA") expect += treeSteps(base + "/" + k);
    } else {                                                   // minor, relative major, NA (an absent one is one failed open + walk)
      char minorLabel[8], majorLabel[8]; key.labels(minorLabel, majorLabel);
      expect = treeSteps(base + "/" + minorLabel) + treeSteps(base + "/" + majorLabel) + treeSteps(base + "/NA");
    }
    int steps = 0, worstOpens = 0, worstReads = 0, worstBytes = 0, worstWalks = 0, worstWalked = 0; bool done = false;
    while (!done && steps < 64) {
      c.reset(); done = l.prepareLoop("House", cs.cat, key); steps++;
      worstOpens = std::max(worstOpens, c.opens); worstReads = std::max(worstReads, c.reads); worstBytes = std::max(worstBytes, c.readBytes); worstWalks = std::max(worstWalks, c.walks); worstWalked = std::max(worstWalked, c.walked);
      CHECK(c.opens <= 1); CHECK(c.reads <= 1); CHECK(c.readBytes <= kIndexChunk); CHECK(c.walks <= 1); CHECK(c.walked <= 32);   // an absent or unindexed root: its one failed index open and its one walk in the same step (the existing accounting)
    }
    CHECK(done);
    c.reset();
    const bool picked = l.pickLoop("House", cs.cat, key, (int)cs.cat, rng, out, sizeof out);
    printf("    %-10s loops on a full cache: prepared in %2d steps (worst step %d opens %d reads %d B %d walks %d walked); pick: %s\n", cs.folder, steps, worstOpens, worstReads, worstBytes, worstWalks, worstWalked, c.summary().c_str());
    CHECK_EQ(steps, expect);                                   // one step per root (and one for the listing): a folder per step, never two
    CHECK(picked); CHECK(fileExists(out)); CHECK(inTree(out, dirOf(out)));
    CHECK_EQ(c.opens, 1); CHECK(c.reads <= 2); CHECK_EQ(c.walks, 0);
    c.reset();
    CHECK(l.prepareLoop("House", cs.cat, key)); CHECK_EQ(c.opens + c.reads + c.walks, 0);   // warm: nothing evicted by its own run
    CHECK(l.loopTotal("House", cs.cat, key) > 0);
  }
}

TEST(pickLoop_sharded_roots_scratch) {
  // a loop root split into shards (SPEC §9.2 FAT folder sizes): loops/drum/NA/s00 (40) + s01 (30), each indexed, a key
  // folder A (2) and a tonal root loops/bass/Amin/s00 (3) + s01 (3); every file reachable, the trees weighed by count
  const std::string na = "lp/packs/Shardy/loops/drum/NA", a = "lp/packs/Shardy/loops/drum/A", amin = "lp/packs/Shardy/loops/bass/Amin";
  auto fill = [&](const std::string& dir, const char* label, int from, int n) {
    mkdirs(dir); std::string idx;
    for (int i = 0; i < n; i++) { char name[96]; snprintf(name, sizeof name, "%s__125bpm__LOOP__2bars__shard_%03d.wav", label, from + i); touch(dir + "/" + name); idx += std::string(name) + "\n"; }
    writeFile(dir + "/_index.txt", idx);
  };
  fill(na + "/s00", "NA", 0, 40); fill(na + "/s01", "NA", 40, 30); fill(a, "A", 0, 2); fill(amin + "/s00", "Amin", 0, 3); fill(amin + "/s01", "Amin", 3, 3);
  Library& l = scratchLib();
  l.dropCache(); l.invalidate();
  Rng rng(64); char out[kMaxPathLen];
  ProjectKey key; key.root = 9;
  std::set<std::string> names; std::map<std::string, int> perDir;
  for (int i = 0; i < 500; i++) {
    CHECK(l.pickLoop("Shardy", Beat, key, 7, rng, out, sizeof out));
    CHECK(scratchExists(out)); names.insert(baseOf(out)); perDir[dirOf(out)]++;
  }
  printf("    sharded drum loops: %zu distinct over %zu folders (NA share %.2f)\n", names.size(), perDir.size(), (perDir["/" + na + "/s00"] + perDir["/" + na + "/s01"]) / 500.0);
  CHECK_EQ(names.size(), 72u); CHECK_EQ(perDir.size(), 3u);
  CHECK((perDir["/" + na + "/s00"] + perDir["/" + na + "/s01"]) / 500.0 > 70.0 / 72.0 - 0.08);
  CHECK_EQ(l.loopTotal("Shardy", Beat, key), 72);
  names.clear(); perDir.clear();
  for (int i = 0; i < 120; i++) { CHECK(l.pickLoop("Shardy", Bass, key, 3, rng, out, sizeof out)); CHECK(scratchExists(out)); names.insert(baseOf(out)); perDir[dirOf(out)]++; }
  CHECK_EQ(names.size(), 6u); CHECK_EQ(perDir.size(), 2u);
  CHECK_EQ(l.loopTotal("Shardy", Bass, key), 6);
  key.root = 0;                                                        // C minor: Cmin, D#, NA all absent for the bass
  CHECK(!l.pickLoop("Shardy", Bass, key, 3, rng, out, sizeof out)); CHECK_EQ(l.loopTotal("Shardy", Bass, key), 0);
}

TEST(no_file_handle_is_left_open) {
  // Every index open is closed in the same call, whatever the outcome (the SD pool holds 8 files and the
  // vocal streams keep up to four of them).
  CHECK_EQ(sd().openCount(), 0);
  CHECK_EQ(scratch().openCount(), 0);
  Library& l = clib();
  Rng rng(88);
  char out[kMaxPathLen];
  ProjectKey key; key.root = 9;
  l.dropCache();
  for (int i = 0; i < 20; i++) {
    CHECK(l.pickMidi(Beat, key, 7, rng, out, sizeof out));
    CHECK(l.pickMidi(Chord, key, 0, rng, out, sizeof out));
    CHECK(l.pickSample("House", "kick", 9, rng, out, sizeof out));
    CHECK(l.pickVocal("House", key, 0, rng, out, sizeof out));
    l.prepareMidi(Bass, key); l.prepareSample("House", "kick", 3); l.prepareVocal("Testpack", key);
    CHECK(!l.pickMidi(Pad, ProjectKey{}, 2, rng, out, 8));   // a buffer too small for any path
  }
  CHECK_EQ(sd().openCount(), 0);
}

int main() { return lp_test::run(); }
