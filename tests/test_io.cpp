// Host tests for module io: fs helpers, key matrix + map (SPEC §3), WAV loader/streamer (SPEC §5/§6), PosixFs.
#include "check.h"
#include "io/fs.h"
#include "io/keys.h"
#include "io/wav.h"
#include "dsp/ulaw.h"
#include "core/types.h"
#include "../firmware/desktop/posix_fs.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

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

static std::string scratchRoot() {
  static std::string dir;
  if (dir.empty()) {
    char tmpl[] = "/tmp/lp_test_io_XXXXXX";
    const char* d = mkdtemp(tmpl);
    dir = d ? d : "/tmp";
  }
  return dir;
}
static PosixFs& scratch() { static PosixFs fs(scratchRoot().c_str()); return fs; }

static std::vector<std::string> listDir(const std::string& dir, const char* ext) {
  std::vector<std::string> v;
  DIR* d = opendir(dir.c_str());
  if (!d) return v;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n[0] == '.') continue;
    if (ext) { if (n.size() < strlen(ext)) continue; std::string tail = n.substr(n.size() - strlen(ext)); for (auto& c : tail) c = (char)tolower(c); if (tail != ext) continue; }
    v.push_back(n);
  }
  closedir(d);
  return v;
}
static long fileSize(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }

// Reads int16 samples straight from a file (independent of the module under test).
static std::vector<int16_t> rawSamples(const std::string& p, uint32_t offset, uint32_t bytes) {
  std::vector<int16_t> v(bytes / 2);
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return {};
  fseek(f, offset, SEEK_SET);
  size_t got = fread(v.data(), 2, v.size(), f);
  fclose(f);
  v.resize(got);
  return v;
}

// Writes a WAV with the given format; `pre` chunks are inserted before 'fmt ' (as raw id+size+body) and
// `mid` chunks between 'fmt ' and 'data'.
struct Chunk { std::string id; std::vector<uint8_t> body; };
static void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(v & 255); b.push_back((v >> 8) & 255); }
static void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }
static void putChunk(std::vector<uint8_t>& b, const Chunk& c) {
  b.insert(b.end(), c.id.begin(), c.id.end()); put32(b, (uint32_t)c.body.size());
  b.insert(b.end(), c.body.begin(), c.body.end());
  if (c.body.size() & 1) b.push_back(0);             // RIFF word alignment
}
static void writeWav(const std::string& path, uint32_t rate, uint16_t ch, uint16_t bits, const std::vector<uint8_t>& data,
                     std::vector<Chunk> pre = {}, std::vector<Chunk> mid = {}, bool liesAboutDataSize = false) {
  Chunk fmt{"fmt ", {}};
  put16(fmt.body, 1); put16(fmt.body, ch); put32(fmt.body, rate); put32(fmt.body, rate * ch * bits / 8);
  put16(fmt.body, ch * bits / 8); put16(fmt.body, bits);
  std::vector<uint8_t> body;
  for (auto& c : pre) putChunk(body, c);
  putChunk(body, fmt);
  for (auto& c : mid) putChunk(body, c);
  body.insert(body.end(), {'d', 'a', 't', 'a'}); put32(body, liesAboutDataSize ? 0xFFFFFFFFu : (uint32_t)data.size());
  body.insert(body.end(), data.begin(), data.end());
  std::vector<uint8_t> file = {'R', 'I', 'F', 'F'}; put32(file, (uint32_t)(4 + body.size()));
  file.insert(file.end(), {'W', 'A', 'V', 'E'}); file.insert(file.end(), body.begin(), body.end());
  FILE* f = fopen(path.c_str(), "wb"); fwrite(file.data(), 1, file.size(), f); fclose(f);
}
static std::vector<uint8_t> pcm16(const std::vector<int16_t>& s) {
  std::vector<uint8_t> b; for (int16_t v : s) put16(b, (uint16_t)v); return b;
}
static double db(double a, double b) { return 20.0 * log10(a / b); }

// ------------------------------------------------------------------------------------- fs helpers

TEST(hasExt_cases) {
  CHECK(hasExt("a.mid", ".mid"));
  CHECK(hasExt("A.MID", ".mid"));
  CHECK(hasExt("a.mid", ".MID"));
  CHECK(!hasExt("a.midi", ".mid"));
  CHECK(!hasExt("mid", ".mid"));
  CHECK(!hasExt("", ".mid"));
  CHECK(hasExt(".mid", ".mid"));
  CHECK(hasExt("anything", nullptr));
  CHECK(hasExt("anything", ""));
  CHECK(!hasExt(nullptr, ".mid"));
}

TEST(joinPath_cases) {
  char b[32];
  joinPath(b, sizeof b, "/lp", "midi");  CHECK(strcmp(b, "/lp/midi") == 0);
  joinPath(b, sizeof b, "/lp/", "midi"); CHECK(strcmp(b, "/lp/midi") == 0);
  joinPath(b, sizeof b, "/lp", "/midi"); CHECK(strcmp(b, "/lp/midi") == 0);
  joinPath(b, sizeof b, "/lp/", "/midi"); CHECK(strcmp(b, "/lp/midi") == 0);
  joinPath(b, sizeof b, "", "x");        CHECK(strcmp(b, "x") == 0);
  joinPath(b, sizeof b, "/lp", "");      CHECK(strcmp(b, "/lp") == 0);
  joinPath(b, sizeof b, "/", "lp");      CHECK(strcmp(b, "/lp") == 0);
  joinPath(b, sizeof b, nullptr, "lp");  CHECK(strcmp(b, "lp") == 0);
  joinPath(b, sizeof b, "", "/lp/midi"); CHECK(strcmp(b, "/lp/midi") == 0);   // empty a: b verbatim, stays absolute
  joinPath(b, sizeof b, nullptr, "/lp"); CHECK(strcmp(b, "/lp") == 0);
  joinPath(b, sizeof b, "", "");         CHECK(b[0] == 0);
  joinPath(b, sizeof b, nullptr, nullptr); CHECK(b[0] == 0);
  joinPath(b, sizeof b, "/", "/lp");     CHECK(strcmp(b, "/lp") == 0);        // a lone "/" root still joins cleanly
  joinPath(b, sizeof b, "/lp", "//midi"); CHECK(strcmp(b, "/lp/midi") == 0);
  joinPath(b, 6, "/lp", "midi");         CHECK(strcmp(b, "/lp/m") == 0);      // truncates, NUL-terminated
  joinPath(b, 4, "/lp", "midi");         CHECK(strcmp(b, "/lp") == 0);        // no room for the separator
  strcpy(b, "/lp"); joinPath(b, sizeof b, b, "midi/Chord"); CHECK(strcmp(b, "/lp/midi/Chord") == 0);   // in place
  joinPath(b, 1, "/lp", "midi");         CHECK(b[0] == 0);
}

// A MIDI folder holding files directly: the A-minor chord folder, or its first shard when the library expansion sharded
// it (SPEC §9.2: a content folder is flat or holds only sNN subfolders).
static std::string flatMidiFolder() {
  const std::string key = "/lp/midi/Chord/Amin";
  if (!listDir(sdRoot() + key, ".mid").empty()) return key;
  std::vector<std::string> kids = listDir(sdRoot() + key, nullptr);
  std::sort(kids.begin(), kids.end());
  for (const std::string& k : kids) if (!listDir(sdRoot() + key + "/" + k, ".mid").empty()) return key + "/" + k;
  return key;
}

TEST(countEntries_matches_directory) {
  const std::string flat = flatMidiFolder();
  const std::string dir = sdRoot() + flat;
  auto mids = listDir(dir, ".mid");
  CHECK(mids.size() > 0);
  CHECK_EQ(countEntries(sd(), flat.c_str(), ".mid"), (int)mids.size());
  CHECK_EQ(countEntries(sd(), flat.c_str(), ".MID"), (int)mids.size());
  CHECK_EQ(countEntries(sd(), flat.c_str(), ".wav"), 0);
  CHECK_EQ(countEntries(sd(), flat.c_str(), nullptr), (int)listDir(dir, nullptr).size());
  CHECK_EQ(countEntries(sd(), (flat + "/").c_str(), ".mid"), (int)mids.size());   // trailing slash tolerated
  CHECK_EQ(countEntries(sd(), "/lp/midi/Chord/Nope", ".mid"), 0);
  CHECK_EQ(countEntries(sd(), nullptr, ".mid"), 0);
}

TEST(countEntries_without_ext_includes_directories) {
  const std::string dir = sdRoot() + "/lp/packs";
  auto all = listDir(dir, nullptr);
  CHECK(all.size() >= 1);
  CHECK_EQ(countEntries(sd(), "/lp/packs", nullptr), (int)all.size());
  CHECK_EQ(countEntries(sd(), "/lp/packs", ".wav"), 0);         // folders never match an extension
}

static bool collectNames(const char* name, bool isDir, uint32_t, void* u) {
  if (!isDir && hasExt(name, ".mid")) static_cast<std::vector<std::string>*>(u)->push_back(name);
  return true;
}

TEST(entryAt_returns_existing_names_in_walk_order) {
  const std::string flat = flatMidiFolder(); const char* path = flat.c_str();
  int n = countEntries(sd(), path, ".mid");
  std::vector<std::string> order;
  CHECK(sd().walk(path, collectNames, &order));
  CHECK_EQ((int)order.size(), n);
  CHECK(n > 0); if (n == 0) return;                              // nothing to index here: the checks below need a name
  std::set<std::string> seen;
  for (int i = 0; i < n; i++) {
    char name[kMaxNameLen];
    CHECK(entryAt(sd(), path, ".mid", i, name, sizeof name));
    CHECK(name[0] != 0);
    CHECK(hasExt(name, ".mid"));
    CHECK(fileSize(sdRoot() + path + "/" + name) > 0);
    CHECK(order[i] == name);
    seen.insert(name);
  }
  CHECK_EQ((int)seen.size(), n);                                 // all distinct
  char name[kMaxNameLen] = "x";
  CHECK(!entryAt(sd(), path, ".mid", n, name, sizeof name));     // out of range
  CHECK(name[0] == 0);                                           // cleared on failure
  CHECK(!entryAt(sd(), path, ".mid", -1, name, sizeof name));
  CHECK(!entryAt(sd(), "/lp/midi/Chord/Nope", ".mid", 0, name, sizeof name));
  CHECK(!entryAt(sd(), path, ".wav", 0, name, sizeof name));
  // a buffer too small for the name: false and "" rather than a silently truncated name (which would
  // join into a path that does not exist)
  char tiny[5] = "xyz";
  CHECK(!entryAt(sd(), path, ".mid", 0, tiny, sizeof tiny));
  CHECK(tiny[0] == 0);
  std::vector<char> exact(order[0].size() + 1, 'x');
  CHECK(entryAt(sd(), path, ".mid", 0, exact.data(), (int)exact.size()));     // exactly strlen + 1 fits
  CHECK(order[0] == exact.data());
  std::vector<char> oneShort(order[0].size(), 'x');
  CHECK(!entryAt(sd(), path, ".mid", 0, oneShort.data(), (int)oneShort.size()));   // strlen alone does not
  CHECK(oneShort[0] == 0);
  CHECK_EQ(countEntries(sd(), path, ".mid"), n);                 // counting is unaffected by buffer sizes
}

// ------------------------------------------------------------------------------------------ PosixFs

TEST(posixfs_exists_isdir_open) {
  CHECK(sd().exists("/lp/midi"));
  CHECK(sd().isDir("/lp/midi"));
  CHECK(sd().exists("/lp/midi/Chord/Amin"));
  CHECK(!sd().exists("/lp/nothing/here"));
  CHECK(!sd().isDir("/lp/nothing/here"));
  CHECK(sd().open("/lp/midi") == nullptr);                       // directories are not files
  CHECK(sd().open("/lp/nothing.bin") == nullptr);
  char name[kMaxNameLen];
  const std::string flat = flatMidiFolder();
  CHECK(entryAt(sd(), flat.c_str(), ".mid", 0, name, sizeof name));
  char path[kMaxPathLen];
  joinPath(path, sizeof path, flat.c_str(), name);
  CHECK(sd().exists(path));
  CHECK(!sd().isDir(path));
  IFile* f = sd().open(path);
  CHECK(f != nullptr);
  if (!f) return;
  CHECK(f->valid());
  CHECK_EQ((long)f->size(), fileSize(sdRoot() + path));
  uint8_t hdr[4] = {0};
  CHECK_EQ((int)f->read(hdr, 4), 4);
  CHECK(memcmp(hdr, "MThd", 4) == 0);
  CHECK_EQ((int)f->position(), 4);
  CHECK(f->seek(0));
  CHECK_EQ((int)f->position(), 0);
  CHECK_EQ((int)f->write(hdr, 4), 0);                            // read-only handle
  f->close();
  CHECK(!f->valid());
  CHECK_EQ(sd().openCount(), 0);
}

TEST(posixfs_pool_of_eight) {
  char name[kMaxNameLen], path[kMaxPathLen];
  const std::string flat = flatMidiFolder();
  CHECK(entryAt(sd(), flat.c_str(), ".mid", 0, name, sizeof name));
  joinPath(path, sizeof path, flat.c_str(), name);
  IFile* h[PosixFs::kMaxOpen];
  for (int i = 0; i < PosixFs::kMaxOpen; i++) { h[i] = sd().open(path); CHECK(h[i] != nullptr); }
  CHECK_EQ(sd().openCount(), PosixFs::kMaxOpen);
  CHECK(sd().open(path) == nullptr);                             // ninth is refused
  h[3]->close();
  IFile* again = sd().open(path);
  CHECK(again == h[3]);                                          // slot reused
  for (int i = 0; i < PosixFs::kMaxOpen; i++) if (h[i]) h[i]->close();
  CHECK_EQ(sd().openCount(), 0);
}

static bool countWalk(const char* name, bool isDir, uint32_t size, void* u) {
  auto* v = static_cast<std::vector<std::string>*>(u);
  v->push_back(std::string(name) + (isDir ? "/" : "") + ":" + std::to_string(size));
  return true;
}
static bool stopAfterOne(const char*, bool, uint32_t, void* u) { (*static_cast<int*>(u))++; return false; }

TEST(posixfs_mkdir_write_read_remove_walk) {
  PosixFs& fs = scratch();
  CHECK(fs.mkdir("/d"));
  CHECK(fs.mkdir("/d"));                                         // already a directory: still true
  CHECK(fs.isDir("/d"));
  CHECK(fs.mkdir("/d/sub"));
  CHECK(!fs.mkdir("/missing/parent"));
  IFile* w = fs.open("/d/a.txt", "w");
  CHECK(w != nullptr);
  if (!w) return;
  CHECK_EQ((int)w->write("hello", 5), 5);
  CHECK_EQ((int)w->size(), 5);
  CHECK_EQ((int)w->position(), 5);
  w->close();
  IFile* a = fs.open("/d/a.txt", "a");
  CHECK(a != nullptr);
  if (a) { CHECK_EQ((int)a->write("!!", 2), 2); a->close(); }
  IFile* r = fs.open("/d/a.txt");
  CHECK(r != nullptr);
  if (r) {
    char buf[16] = {0};
    CHECK_EQ((int)r->size(), 7);
    CHECK_EQ((int)r->read(buf, sizeof buf), 7);
    CHECK(strcmp(buf, "hello!!") == 0);
    CHECK_EQ((int)r->read(buf, sizeof buf), 0);                  // at EOF
    CHECK(r->seek(5));
    CHECK_EQ((int)r->read(buf, 2), 2);
    CHECK(strncmp(buf, "!!", 2) == 0);
    r->close();
  }
  // dotfile created behind the module's back must be skipped by walk
  FILE* hidden = fopen((scratchRoot() + "/d/.hidden").c_str(), "wb"); fputs("x", hidden); fclose(hidden);
  std::vector<std::string> seen;
  CHECK(fs.walk("/d", countWalk, &seen));
  CHECK_EQ((int)seen.size(), 2);
  bool sawFile = false, sawDir = false;
  for (auto& s : seen) { if (s == "a.txt:7") sawFile = true; if (s == "sub/:0") sawDir = true; }
  CHECK(sawFile); CHECK(sawDir);
  int stops = 0;
  CHECK(fs.walk("/d", stopAfterOne, &stops));
  CHECK_EQ(stops, 1);                                            // early stop honoured
  CHECK(!fs.walk("/nope", countWalk, &seen));                    // missing dir: false
  CHECK(!fs.walk("/d/a.txt", countWalk, &seen));                 // a file is not walkable
  CHECK(fs.remove("/d/.hidden"));
  CHECK(fs.remove("/d/a.txt"));
  CHECK(!fs.exists("/d/a.txt"));
  CHECK(!fs.remove("/d/a.txt"));
  CHECK(fs.remove("/d/sub"));
  CHECK(fs.remove("/d"));
  CHECK(!fs.exists("/d"));
}

// IFile::sync (SPEC §9.23: the recorder's write-behind flushes): after it a second handle sees the bytes and the size while
// the writer stays open; a read-only handle and the interface default answer false.
TEST(posixfile_sync_makes_the_size_visible_to_a_second_handle) {
  PosixFs& fs = scratch();
  CHECK(fs.mkdir("/s"));
  IFile* w = fs.open("/s/log.bin", "w");
  CHECK(w != nullptr); if (!w) return;
  uint8_t b[64]; for (int i = 0; i < 64; i++) b[i] = (uint8_t)i;
  CHECK_EQ((int)w->write(b, 64), 64);
  CHECK(w->sync());
  IFile* r = fs.open("/s/log.bin");
  CHECK(r != nullptr);
  if (r) {
    CHECK_EQ((int)r->size(), 64);
    uint8_t got[64] = {0}; CHECK_EQ((int)r->read(got, 64), 64); CHECK(memcmp(got, b, 64) == 0);
    CHECK(!r->sync());                                           // read-only: nothing to push
    r->close();
  }
  CHECK_EQ((int)w->write(b, 8), 8); CHECK(w->sync());
  r = fs.open("/s/log.bin"); CHECK(r != nullptr); if (r) { CHECK_EQ((int)r->size(), 72); r->close(); }
  w->close();
  CHECK(!w->sync());                                             // closed: false
  struct Plain : IFile {
    bool valid() const override { return true; } size_t read(void*, size_t) override { return 0; } size_t write(const void*, size_t n) override { return n; }
    bool seek(uint32_t) override { return true; } uint32_t position() const override { return 0; } uint32_t size() const override { return 0; } void close() override {}
  } plain;
  CHECK(!plain.sync());
  CHECK(fs.remove("/s/log.bin")); CHECK(fs.remove("/s"));
  CHECK_EQ(fs.openCount(), 0);
}

// IFileSystem::rename (SPEC §9.22: Sets' .tmp rewrites): POSIX semantics on the host, an existing destination is replaced
// atomically; a missing source, a missing destination folder or a null path answers false and changes nothing. The
// interface default (a file system without rename) answers false.
TEST(posixfs_rename_replaces_the_destination) {
  PosixFs& fs = scratch();
  CHECK(fs.mkdir("/r"));
  auto put = [&](const char* p, const char* text) { IFile* w = fs.open(p, "w"); CHECK(w != nullptr); if (w) { CHECK_EQ((int)w->write(text, strlen(text)), (int)strlen(text)); w->close(); } };
  auto get = [&](const char* p) { IFile* r = fs.open(p); if (!r) return std::string("<missing>"); char buf[64]; const size_t n = r->read(buf, sizeof buf); r->close(); return std::string(buf, n); };
  put("/r/a.txt", "new"); put("/r/b.txt", "old");
  CHECK(fs.rename("/r/a.txt", "/r/b.txt"));
  CHECK(!fs.exists("/r/a.txt")); CHECK(get("/r/b.txt") == "new");
  CHECK(fs.rename("/r/b.txt", "/r/c.txt"));
  CHECK(!fs.exists("/r/b.txt")); CHECK(get("/r/c.txt") == "new");
  CHECK(!fs.rename("/r/missing.txt", "/r/d.txt")); CHECK(!fs.exists("/r/d.txt"));
  CHECK(!fs.rename("/r/c.txt", "/nope/x.txt")); CHECK(get("/r/c.txt") == "new");
  CHECK(!fs.rename(nullptr, "/r/d.txt")); CHECK(!fs.rename("/r/c.txt", nullptr)); CHECK(get("/r/c.txt") == "new");
  struct Plain : IFileSystem {
    bool exists(const char*) override { return false; } bool isDir(const char*) override { return false; }
    IFile* open(const char*, const char*) override { return nullptr; } bool mkdir(const char*) override { return false; }
    bool remove(const char*) override { return false; } bool walk(const char*, DirEntryFn, void*) override { return false; }
  } plain;
  CHECK(!plain.rename("/a", "/b"));
  CHECK(fs.remove("/r/c.txt")); CHECK(fs.remove("/r"));
  CHECK_EQ(fs.openCount(), 0);
}

TEST(posixfs_root_mapping) {
  PosixFs trailing((sdRoot() + "/").c_str());
  CHECK(trailing.exists("/lp/midi"));
  CHECK(strcmp(trailing.root(), sdRoot().c_str()) == 0);         // trailing slash trimmed
  CHECK(trailing.exists("lp/midi"));                             // leading slash optional
}

static bool noEntry(const char*, bool, uint32_t, void*) { return true; }

TEST(posixfs_empty_root_is_host_root_and_null_paths_refused) {
  // an empty or null root must not turn absolute card paths into CWD-relative ones
  PosixFs empty("");
  PosixFs nul(nullptr);
  CHECK(strcmp(empty.root(), "/") == 0);
  CHECK(strcmp(nul.root(), "/") == 0);
  CHECK(empty.exists("/"));
  CHECK(empty.isDir("/"));
  CHECK(nul.exists("/"));
  CHECK(empty.isDir(scratchRoot().c_str()));                     // "/tmp/lp_test_io_XXXXXX" resolves as itself
  char abs[PATH_MAX];
  if (realpath((sdRoot() + "/lp/midi").c_str(), abs)) CHECK(empty.isDir(abs)); else CHECK(false);
  CHECK(!empty.exists("/lp_test_io_this_does_not_exist"));
  // a null path is never the root
  for (PosixFs* fs : {&empty, &nul, &sd()}) {
    CHECK(!fs->exists(nullptr));
    CHECK(!fs->isDir(nullptr));
    CHECK(fs->open(nullptr) == nullptr);
    CHECK(fs->open(nullptr, "w") == nullptr);
    CHECK(!fs->mkdir(nullptr));
    CHECK(!fs->remove(nullptr));
    CHECK(!fs->walk(nullptr, noEntry, nullptr));
  }
  CHECK(sd().exists("/"));                                       // ... while the real root is
  CHECK(!sd().walk("/", nullptr, nullptr));                      // and a null callback is refused
  CHECK_EQ(sd().openCount(), 0);
}

// ---------------------------------------------------------------------------------------- key matrix

static int scan(KeyMatrix& k, uint32_t ms, std::vector<std::pair<int, int>> keys, KeyEvent* out, int cap = 16) {
  k.beginScan();
  for (auto& p : keys) k.markPressed(p.first, p.second);
  return k.endScan(ms, out, cap);
}

TEST(keys_press_then_release_with_heldMs) {
  KeyMatrix k; KeyEvent ev[16];
  CHECK_EQ(scan(k, 0, {}, ev), 0);
  CHECK_EQ(k.downCount(), 0);
  int n = scan(k, 10, {{3, 1}}, ev);
  CHECK_EQ(n, 1);
  CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK_EQ(ev[0].x, 3); CHECK_EQ(ev[0].y, 1);
  CHECK_EQ(ev[0].ms, 10u); CHECK_EQ(ev[0].heldMs, 0u);
  CHECK(k.isDown(3, 1)); CHECK(!k.isDown(3, 0)); CHECK_EQ(k.downCount(), 1);
  CHECK_EQ(scan(k, 30, {{3, 1}}, ev), 0);                        // still held, nothing new
  CHECK_EQ(k.heldMs(3, 1, 30), 20u);
  CHECK_EQ(k.heldMs(0, 0, 30), 0u);
  n = scan(k, 110, {}, ev);
  CHECK_EQ(n, 1);
  CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK_EQ(ev[0].x, 3); CHECK_EQ(ev[0].y, 1);
  CHECK_EQ(ev[0].heldMs, 100u); CHECK_EQ(ev[0].ms, 110u);
  CHECK(!k.isDown(3, 1)); CHECK_EQ(k.downCount(), 0);
  CHECK_EQ(scan(k, 120, {}, ev), 0);
}

TEST(keys_repeat_timing) {
  KeyMatrix k; KeyEvent ev[16];
  CHECK_EQ(scan(k, 0, {{6, 2}}, ev), 1);
  std::vector<uint32_t> repeatsAt;
  for (uint32_t t = 10; t <= 600; t += 10) {
    int n = scan(k, t, {{6, 2}}, ev);
    for (int i = 0; i < n; i++) { CHECK_EQ(ev[i].kind, KeyEvent::Repeat); CHECK_EQ(ev[i].heldMs, t); CHECK_EQ(ev[i].ms, t); repeatsAt.push_back(t); }
  }
  // SPEC §3: first repeat after 350 ms, then every 80 ms
  CHECK_EQ((int)repeatsAt.size(), 4);
  if (repeatsAt.size() == 4) { CHECK_EQ(repeatsAt[0], 350u); CHECK_EQ(repeatsAt[1], 430u); CHECK_EQ(repeatsAt[2], 510u); CHECK_EQ(repeatsAt[3], 590u); }
  int n = scan(k, 610, {}, ev);
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK_EQ(ev[0].heldMs, 610u);
  // after a release, the next press starts the delay afresh
  CHECK_EQ(scan(k, 700, {{6, 2}}, ev), 1);
  CHECK_EQ(scan(k, 1040, {{6, 2}}, ev), 0);
  n = scan(k, 1050, {{6, 2}}, ev);
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Repeat); CHECK_EQ(ev[0].heldMs, 350u);
}

TEST(keys_repeat_schedule_is_stable_with_scan_jitter) {
  KeyMatrix k; KeyEvent ev[16];
  scan(k, 0, {{0, 0}}, ev);
  // irregular scans: repeats stay on the 350 + 80 n grid, one per scan, never a burst
  // slots: 350 430 510 590 ... ; 355 fires 350, 431 fires 430, 600 fires 510 (590 already due -> rescheduled to 680), 700 fires 680
  uint32_t times[] = {100, 355, 360, 420, 431, 500, 600, 601, 700};
  std::vector<uint32_t> at;
  for (uint32_t t : times) { int n = scan(k, t, {{0, 0}}, ev); CHECK(n <= 1); if (n) at.push_back(t); }
  CHECK_EQ((int)at.size(), 4);
  if (at.size() == 4) { CHECK_EQ(at[0], 355u); CHECK_EQ(at[1], 431u); CHECK_EQ(at[2], 600u); CHECK_EQ(at[3], 700u); }
}

TEST(keys_stalled_scans_catch_up_without_burst) {
  KeyMatrix k; KeyEvent ev[16];
  scan(k, 0, {{2, 3}}, ev);
  int n = scan(k, 1000, {{2, 3}}, ev);
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Repeat); CHECK_EQ(ev[0].heldMs, 1000u);
  CHECK_EQ(scan(k, 1050, {{2, 3}}, ev), 0);
  n = scan(k, 1080, {{2, 3}}, ev);
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].heldMs, 1080u);
}

TEST(keys_two_at_once) {
  KeyMatrix k; KeyEvent ev[16];
  int n = scan(k, 5, {{2, 1}, {7, 0}}, ev);
  CHECK_EQ(n, 2);
  CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK_EQ(ev[1].kind, KeyEvent::Press);
  CHECK(ev[0].x == 7 && ev[0].y == 0);                           // ordinary keys: row-major
  CHECK(ev[1].x == 2 && ev[1].y == 1);
  CHECK_EQ(ev[0].ms, 5u); CHECK_EQ(ev[1].ms, 5u);
  CHECK_EQ(k.downCount(), 2);
  n = scan(k, 65, {}, ev);
  CHECK_EQ(n, 2);
  CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK_EQ(ev[0].heldMs, 60u);
  CHECK_EQ(ev[1].kind, KeyEvent::Release); CHECK_EQ(ev[1].heldMs, 60u);
  CHECK_EQ(k.downCount(), 0);
}

// The UI derives its modifier from events (Ui::shift_), so within one scan SHIFT (13,3) must be pressed
// before everything else and released after everything else (SPEC §3: SHIFT + track = select, not toggle).
TEST(keys_shift_brackets_the_scan) {
  KeyMatrix k; KeyEvent ev[16];
  int n = scan(k, 5, {{0, 0}, {13, 3}}, ev);                    // SHIFT + track in the same scan
  CHECK_EQ(n, 2);
  CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK(ev[0].x == 13 && ev[0].y == 3);   // SHIFT first
  CHECK_EQ(ev[1].kind, KeyEvent::Press); CHECK(ev[1].x == 0 && ev[1].y == 0);
  CHECK_EQ(k.downCount(), 2);
  n = scan(k, 65, {}, ev);                                       // both released in the same scan
  CHECK_EQ(n, 2);
  CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK(ev[0].x == 0 && ev[0].y == 0); CHECK_EQ(ev[0].heldMs, 60u);
  CHECK_EQ(ev[1].kind, KeyEvent::Release); CHECK(ev[1].x == 13 && ev[1].y == 3); CHECK_EQ(ev[1].heldMs, 60u);   // SHIFT last
  CHECK_EQ(k.downCount(), 0);
  // SHIFT pressed together with keys on every row, including row 3 right of it: SHIFT still comes first
  n = scan(k, 100, {{12, 3}, {5, 2}, {13, 3}, {1, 1}, {0, 0}}, ev);
  CHECK_EQ(n, 5);
  CHECK(ev[0].x == 13 && ev[0].y == 3);
  CHECK(ev[1].x == 0 && ev[1].y == 0); CHECK(ev[2].x == 1 && ev[2].y == 1); CHECK(ev[3].x == 5 && ev[3].y == 2); CHECK(ev[4].x == 12 && ev[4].y == 3);
  // same-scan SHIFT release + new key press: the press is delivered before the SHIFT release, so the
  // ambiguous gesture resolves to the shifted (select / ramp) action rather than an audible launch
  n = scan(k, 200, {{12, 3}, {5, 2}, {1, 1}, {0, 0}, {3, 0}}, ev);
  CHECK_EQ(n, 2);
  CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK(ev[0].x == 3 && ev[0].y == 0);
  CHECK_EQ(ev[1].kind, KeyEvent::Release); CHECK(ev[1].x == 13 && ev[1].y == 3); CHECK_EQ(ev[1].heldMs, 100u);
  CHECK(!k.isDown(13, 3)); CHECK_EQ(k.downCount(), 5);
  // releases before presses for ordinary keys (note-off before note-on on the perform page)
  n = scan(k, 300, {{5, 2}, {1, 1}, {0, 0}, {3, 0}, {9, 1}}, ev);   // (12,3) up, (9,1) down
  CHECK_EQ(n, 2);
  CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK(ev[0].x == 12 && ev[0].y == 3);
  CHECK_EQ(ev[1].kind, KeyEvent::Press); CHECK(ev[1].x == 9 && ev[1].y == 1);
  // SHIFT re-pressed while a key is released and another pressed: SHIFT press, release, press
  n = scan(k, 400, {{5, 2}, {1, 1}, {0, 0}, {3, 0}, {13, 3}, {4, 0}}, ev);   // (9,1) up, (4,0) + SHIFT down
  CHECK_EQ(n, 3);
  CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK(ev[0].x == 13 && ev[0].y == 3);
  CHECK_EQ(ev[1].kind, KeyEvent::Release); CHECK(ev[1].x == 9 && ev[1].y == 1);
  CHECK_EQ(ev[2].kind, KeyEvent::Press); CHECK(ev[2].x == 4 && ev[2].y == 0);
  // SHIFT repeats are ordinary (not hoisted) and its held time is tracked like any key
  n = scan(k, 750, {{5, 2}, {1, 1}, {0, 0}, {3, 0}, {13, 3}, {4, 0}}, ev);
  CHECK_EQ(n, 6);
  for (int i = 0; i < n; i++) CHECK_EQ(ev[i].kind, KeyEvent::Repeat);
  CHECK(ev[5].x == 13 && ev[5].y == 3); CHECK_EQ(ev[5].heldMs, 350u);
  CHECK_EQ(ev[0].heldMs, 650u);                                  // (0,0) re-pressed at t = 100
}

TEST(keys_shift_order_survives_a_full_buffer) {
  KeyMatrix k; KeyEvent ev[16];
  int n = scan(k, 0, {{0, 0}, {13, 3}}, ev, 1);                  // room for one event: SHIFT wins
  CHECK_EQ(n, 1); CHECK(ev[0].x == 13 && ev[0].y == 3); CHECK_EQ(ev[0].kind, KeyEvent::Press);
  CHECK(k.isDown(13, 3)); CHECK(!k.isDown(0, 0));
  n = scan(k, 10, {{0, 0}, {13, 3}}, ev, 4);                     // the deferred track press follows, still after SHIFT
  CHECK_EQ(n, 1); CHECK(ev[0].x == 0 && ev[0].y == 0); CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK_EQ(ev[0].ms, 10u);
  n = scan(k, 20, {}, ev, 1);                                    // room for one release: the track's, SHIFT stays down
  CHECK_EQ(n, 1); CHECK(ev[0].x == 0 && ev[0].y == 0); CHECK_EQ(ev[0].kind, KeyEvent::Release);
  CHECK(k.isDown(13, 3)); CHECK_EQ(k.downCount(), 1);
  n = scan(k, 30, {}, ev, 4);
  CHECK_EQ(n, 1); CHECK(ev[0].x == 13 && ev[0].y == 3); CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK_EQ(ev[0].heldMs, 30u);
  CHECK_EQ(k.downCount(), 0);
}

TEST(keys_chord_release_one) {
  KeyMatrix k; KeyEvent ev[16];
  CHECK_EQ(scan(k, 0, {{13, 3}}, ev), 1);                        // SHIFT down
  int n = scan(k, 50, {{13, 3}, {5, 0}}, ev);                    // + track key
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK(ev[0].x == 5 && ev[0].y == 0);
  CHECK_EQ(k.downCount(), 2);
  n = scan(k, 200, {{13, 3}}, ev);                               // track key released, SHIFT held
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK(ev[0].x == 5 && ev[0].y == 0); CHECK_EQ(ev[0].heldMs, 150u);
  CHECK(k.isDown(13, 3)); CHECK(!k.isDown(5, 0)); CHECK_EQ(k.downCount(), 1);
  n = scan(k, 350, {{13, 3}}, ev);                               // SHIFT reaches its repeat delay
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Repeat); CHECK(ev[0].x == 13 && ev[0].y == 3); CHECK_EQ(ev[0].heldMs, 350u);
  n = scan(k, 400, {}, ev);
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Release); CHECK_EQ(ev[0].heldMs, 400u);
}

TEST(keys_cap_defers_events) {
  KeyMatrix k; KeyEvent ev[16];
  int n = scan(k, 0, {{1, 1}, {2, 2}}, ev, 1);
  CHECK_EQ(n, 1); CHECK(ev[0].x == 1 && ev[0].y == 1);
  CHECK_EQ(k.downCount(), 1);                                    // the unreported press is not yet state
  n = scan(k, 10, {{1, 1}, {2, 2}}, ev, 4);
  CHECK_EQ(n, 1); CHECK_EQ(ev[0].kind, KeyEvent::Press); CHECK(ev[0].x == 2 && ev[0].y == 2); CHECK_EQ(ev[0].ms, 10u);
  CHECK_EQ(k.downCount(), 2);
  n = scan(k, 20, {}, ev, 0);                                    // no room at all: nothing lost
  CHECK_EQ(n, 0); CHECK_EQ(k.downCount(), 2);
  n = scan(k, 30, {}, ev, 4);
  CHECK_EQ(n, 2); CHECK_EQ(ev[0].heldMs, 30u); CHECK_EQ(ev[1].heldMs, 20u);
}

TEST(keys_markPressed_ignores_out_of_range) {
  KeyMatrix k; KeyEvent ev[16];
  k.beginScan(); k.markPressed(14, 0); k.markPressed(0, 4); k.markPressed(-1, 0); k.markPressed(0, -1);
  CHECK_EQ(k.endScan(0, ev, 16), 0);
  CHECK_EQ(k.downCount(), 0);
}

// -------------------------------------------------------------------------------------------- mapKey

struct Expect { int x, y; KeyFn fn; int a, b; };
static const Expect kExpected[] = {
  // y0 (SPEC §3): set 1 tracks, variants, snare rush, kick cut
  {0,0,KTrack,0,0},{1,0,KTrack,1,0},{2,0,KTrack,2,0},{3,0,KTrack,3,0},{4,0,KTrack,4,0},{5,0,KTrack,5,0},{6,0,KTrack,6,0},{7,0,KTrack,7,0},
  {8,0,KVariant,0,0},{9,0,KVariant,0,1},{10,0,KVariant,0,2},{11,0,KVariant,0,3},{12,0,KSnareRush,0,0},{13,0,KKickCut,0,0},
  // y1: set 2
  {0,1,KTrack,8,0},{1,1,KTrack,9,0},{2,1,KTrack,10,0},{3,1,KTrack,11,0},{4,1,KTrack,12,0},{5,1,KTrack,13,0},{6,1,KTrack,14,0},{7,1,KTrack,15,0},
  {8,1,KVariant,1,0},{9,1,KVariant,1,1},{10,1,KVariant,1,2},{11,1,KVariant,1,3},{12,1,KSnareRush,1,0},{13,1,KKickCut,1,0},
  // y2: param up, gyro, genre, save/load S1, gen pattern, generate S1
  {0,2,KParamUp,PVolume,0},{1,2,KParamUp,PReverb,0},{2,2,KParamUp,PEcho,0},{3,2,KParamUp,PDist,0},
  {4,2,KParamUp,PFilter,0},{5,2,KParamUp,PLength,0},{6,2,KParamUp,PTempo,0},{7,2,KParamUp,PKey,0},
  {8,2,KGyro,0,0},{9,2,KGenre,0,0},{10,2,KSaveSet,0,0},{11,2,KLoadSet,0,0},{12,2,KGenPattern,0,0},{13,2,KGenSetAll,0,0},
  // y3: param down, edit, perform, save/load S2, gen sound, SHIFT (space)
  {0,3,KParamDown,PVolume,0},{1,3,KParamDown,PReverb,0},{2,3,KParamDown,PEcho,0},{3,3,KParamDown,PDist,0},
  {4,3,KParamDown,PFilter,0},{5,3,KParamDown,PLength,0},{6,3,KParamDown,PTempo,0},{7,3,KParamDown,PKey,0},
  {8,3,KEdit,0,0},{9,3,KPerform,0,0},{10,3,KSaveSet,1,0},{11,3,KLoadSet,1,0},{12,3,KGenSound,0,0},{13,3,KShift,0,0},
};

TEST(mapKey_all_56_positions) {
  CHECK_EQ((int)(sizeof kExpected / sizeof kExpected[0]), kKeyCols * kKeyRows);
  bool covered[kKeyRows][kKeyCols] = {{false}};
  for (const Expect& e : kExpected) {
    KeyMapping m = mapKey(e.x, e.y);
    if (m.fn != e.fn || m.a != e.a || m.b != e.b) std::printf("  mapKey(%d,%d) = {%d,%d,%d} expected {%d,%d,%d}\n", e.x, e.y, m.fn, m.a, m.b, e.fn, e.a, e.b);
    CHECK(m.fn == e.fn && m.a == e.a && m.b == e.b);
    covered[e.y][e.x] = true;
  }
  for (int y = 0; y < kKeyRows; y++) for (int x = 0; x < kKeyCols; x++) CHECK(covered[y][x]);
  CHECK_EQ(mapKey(13, 3).fn, KShift);                            // physical SHIFT = space
  CHECK_EQ(mapKey(14, 0).fn, KNone);
  CHECK_EQ(mapKey(0, 4).fn, KNone);
  CHECK_EQ(mapKey(-1, 0).fn, KNone);
  CHECK_EQ(mapKey(0, -1).fn, KNone);
  // param rows line up with ParamId order
  for (int x = 0; x < 8; x++) { CHECK_EQ(mapKey(x, 2).a, x); CHECK_EQ(mapKey(x, 3).a, x); }
}

// ----------------------------------------------------------------------------------------------- WAV

static const char* kKickDir = "/lp/packs/House/drum/kick/s00";   // sharded folder (SPEC §10.1)
static const uint32_t kKickCap = (uint32_t)kCapKickMs * kSampleRate / 1000;   // 8820 samples (400 ms, SPEC §9.1)

// First kick on the card in name order. Fails loudly (empty name + CHECK) rather than falling back.
static std::string pickKick() {
  auto names = listDir(sdRoot() + kKickDir, ".wav");
  std::sort(names.begin(), names.end());
  if (names.empty()) { std::printf("  no .wav under %s%s\n", sdRoot().c_str(), kKickDir); CHECK(!names.empty()); return ""; }
  return names[0];
}

// First vocal key folder under /lp/packs/House/vocal that holds a .wav (the key folders on the card change
// with the import, so nothing is hardcoded), and the first .wav in it. Both empty when there is none.
static bool pickVocal(std::string& dirOut, std::string& nameOut) {
  const char* base = "/lp/packs/House/vocal";
  int folders = countEntries(sd(), base, nullptr);
  for (int i = 0; i < folders; i++) {
    char key[kMaxNameLen];
    if (!entryAt(sd(), base, nullptr, i, key, sizeof key)) continue;
    char dir[kMaxPathLen];
    joinPath(dir, sizeof dir, base, key);
    if (!sd().isDir(dir) || countEntries(sd(), dir, ".wav") == 0) continue;
    char name[kMaxNameLen];
    if (!entryAt(sd(), dir, ".wav", 0, name, sizeof name)) continue;
    dirOut = dir; nameOut = name;
    return true;
  }
  std::printf("  no vocal .wav under %s%s/*\n", sdRoot().c_str(), base);
  return false;
}

// A deterministic over-cap test signal: a decaying 55 Hz sine with a 2 kHz overtone, peak just under
// full scale at the start, still clearly audible (> 3000) across the whole cap window.
static std::vector<int16_t> kickLike(uint32_t n) {
  std::vector<int16_t> s(n);
  for (uint32_t i = 0; i < n; i++) {
    double t = (double)i / kSampleRate;
    double env = 0.3 + 0.7 * exp(-t * 2.0);
    double v = 30000.0 * env * (0.8 * sin(2 * M_PI * 55 * t) + 0.2 * sin(2 * M_PI * 2000 * t));
    s[i] = (int16_t)lrint(v);
  }
  return s;
}

static double peakOf(const std::vector<int16_t>& s, uint32_t n) { double p = 0; for (uint32_t i = 0; i < n && i < s.size(); i++) p = std::max(p, (double)std::abs(s[i])); return p; }
static double peakDecoded(const uint8_t* d, uint32_t n) { double p = 0; for (uint32_t i = 0; i < n; i++) p = std::max(p, (double)std::abs(ulawDecode(d[i]))); return p; }
// µ-law keeps ~ 4 % relative accuracy (16 steps per octave), so compare with a relative + absolute slack.
static bool nearUlaw(double got, double want) { return std::fabs(got - want) <= 0.05 * std::fabs(want) + 40; }
// Expected loader gain at output i for a load of nOut samples with a fade of fadeN at the end: 1 -> exactly 0.
static double fadeGain(uint32_t i, uint32_t nOut, uint32_t fadeN) {
  if (!fadeN || i + fadeN < nOut) return 1.0;
  return fadeN > 1 ? (double)(nOut - 1 - i) / (double)(fadeN - 1) : 0.0;
}

TEST(wav_header_of_real_kick) {
  std::string name = pickKick();
  if (name.empty()) return;
  std::string path = std::string(kKickDir) + "/" + name;
  IFile* f = sd().open(path.c_str());
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.sampleRate, 22050u);
  CHECK_EQ(info.channels, 1);
  CHECK_EQ(info.bits, 16);
  CHECK_EQ(info.dataOffset, 44u);                                // canonical 44-byte header (prepare_content.py)
  CHECK_EQ((long)info.dataBytes, fileSize(sdRoot() + path) - 44);
  CHECK_EQ(info.dataBytes % 2, 0u);
  CHECK(info.dataBytes / 2 <= kKickCap);                         // the import script already trims kicks to the cap
  CHECK_EQ(f->position(), info.dataOffset);                      // positioned at the first sample
  f->close();
}

// The real kick through the loader: full length (never above the cap on the card), decoded peak within
// 1 dB of the WAV peak, and a smaller cap cuts with a fade that ends exactly at zero.
TEST(wav_load_ulaw_real_kick) {
  ulawInit();
  std::string name = pickKick();
  if (name.empty()) return;
  std::string path = std::string(kKickDir) + "/" + name;
  IFile* f = sd().open(path.c_str());
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  const uint32_t total = info.dataBytes / 2;
  static uint8_t dst[kKickCap + 16];
  memset(dst, 0xAA, sizeof dst);
  uint32_t n = wavLoadUlaw(*f, info, dst, kKickCap, 0.0f);
  CHECK_EQ(n, std::min(total, kKickCap));
  CHECK_EQ(dst[n], 0xAA);                                        // no overrun
  auto ref = rawSamples(sdRoot() + path, info.dataOffset, info.dataBytes);
  double wavPeak = peakOf(ref, n), decPeak = peakDecoded(dst, n);
  CHECK(wavPeak > 1000);
  CHECK_NEAR(db(decPeak, wavPeak), 0.0, 1.0);                    // decoded peak within 1 dB of the WAV peak
  std::printf("  kick %s: %u samples, wav peak %.0f, decoded peak %.0f (%.2f dB)\n", name.c_str(), n, wavPeak, decPeak, db(decPeak, wavPeak));
  int bad = 0;
  for (uint32_t i = 0; i < n; i++) if (!nearUlaw(ulawDecode(dst[i]), ref[i] * fadeGain(i, n, total > kKickCap ? kCapFadeSamples : 0))) bad++;
  CHECK_EQ(bad, 0);
  if (total <= kKickCap) CHECK(nearUlaw(ulawDecode(dst[n - 1]), ref[n - 1]));   // nothing faded below the cap
  // a smaller cap cuts and fades to exactly zero
  const uint32_t cap = total / 2;
  CHECK_EQ(wavLoadUlaw(*f, info, dst, cap, 0.0f), cap);
  CHECK_EQ(ulawDecode(dst[cap - 1]), 0);
  bad = 0;
  for (uint32_t i = 0; i < cap; i++) if (!nearUlaw(ulawDecode(dst[i]), ref[i] * fadeGain(i, cap, kCapFadeSamples))) bad++;
  CHECK_EQ(bad, 0);
  CHECK_EQ(wavLoadUlaw(*f, info, dst, 0, 0.0f), 0u);
  f->close();
}

// Writes the synthetic 2 x cap kick into the scratch root once; returns its card path.
static const char* overCapKick(std::vector<int16_t>& signal) {
  static std::vector<int16_t> s;
  static bool written = false;
  if (!written) { s = kickLike(2 * kKickCap); writeWav(scratchRoot() + "/overcap.wav", 22050, 1, 16, pcm16(s)); written = true; }
  signal = s;
  return "/overcap.wav";
}

TEST(wav_load_ulaw_with_cap_and_fade) {
  ulawInit();
  std::vector<int16_t> ref;
  const char* path = overCapKick(ref);
  IFile* f = scratch().open(path);
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.dataBytes / 2, 2 * kKickCap);                    // this file is longer than the cap
  static uint8_t dst[kKickCap + 16];
  memset(dst, 0xAA, sizeof dst);
  uint32_t n = wavLoadUlaw(*f, info, dst, kKickCap, 0.0f);
  CHECK_EQ(n, kKickCap);
  CHECK_EQ(dst[kKickCap], 0xAA);                                 // no overrun
  double wavPeak = peakOf(ref, kKickCap), decPeak = peakDecoded(dst, kKickCap);
  CHECK(wavPeak > 20000);
  CHECK_NEAR(db(decPeak, wavPeak), 0.0, 1.0);                    // decoded peak within 1 dB of the WAV peak
  // SPEC §9.1: a 10 ms (220-sample) linear fade at the cap, from the untouched sample to exactly 0
  CHECK_EQ(kCapFadeMs, 10);
  CHECK_EQ(kCapFadeSamples, 220u);
  const uint32_t fade = kCapFadeSamples;
  int bad = 0;
  for (uint32_t i = 0; i < kKickCap; i++) if (!nearUlaw(ulawDecode(dst[i]), ref[i] * fadeGain(i, kKickCap, fade))) bad++;
  CHECK_EQ(bad, 0);
  CHECK_EQ(ulawDecode(dst[kKickCap - 1]), 0);                    // the ramp ends at silence: no step at the one-shot end
  CHECK(nearUlaw(ulawDecode(dst[kKickCap - fade]), ref[kKickCap - fade]));   // first faded sample at full level
  CHECK(nearUlaw(ulawDecode(dst[kKickCap - fade - 1]), ref[kKickCap - fade - 1]));   // ... and the one before is untouched
  // midway through the fade the level is half
  const uint32_t mid = kKickCap - 1 - (fade - 1) / 2;
  CHECK(nearUlaw(ulawDecode(dst[mid]), ref[mid] * fadeGain(mid, kKickCap, fade)));
  CHECK_NEAR(fadeGain(mid, kKickCap, fade), 0.5, 0.01);
  // the fade actually attenuates: the last 32 samples are much quieter than the same WAV samples
  double refTail = 0, gotTail = 0;
  for (uint32_t i = kKickCap - 32; i < kKickCap; i++) { refTail += std::abs(ref[i]); gotTail += std::abs(ulawDecode(dst[i])); }
  CHECK(refTail > 32 * 1000);
  CHECK(gotTail < refTail * 0.1);
  // a tiny cap: the whole output is the fade (fadeN = cap), still ending at 0 and starting at full level
  CHECK_EQ(wavLoadUlaw(*f, info, dst, 100, 0.0f), 100u);
  CHECK_EQ(ulawDecode(dst[99]), 0);
  CHECK(nearUlaw(ulawDecode(dst[0]), ref[0]));
  CHECK_EQ(wavLoadUlaw(*f, info, dst, 1, 0.0f), 1u);
  CHECK_EQ(ulawDecode(dst[0]), 0);                               // cap 1: the single sample is the fade end
  f->close();
}

TEST(wav_load_ulaw_gain) {
  ulawInit();
  std::vector<int16_t> ref;
  const char* path = overCapKick(ref);
  IFile* f = scratch().open(path);
  if (!f) { CHECK(false); return; }
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  double wavPeak = peakOf(ref, kKickCap);
  static uint8_t dst[kKickCap], unity[kKickCap];
  CHECK_EQ(wavLoadUlaw(*f, info, unity, kKickCap, 0.0f), kKickCap);
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, -6.0f), kKickCap);
  CHECK_NEAR(db(peakDecoded(dst, kKickCap), wavPeak), -6.0, 1.0);
  CHECK_EQ(ulawDecode(dst[kKickCap - 1]), 0);                    // fade still ends at zero with gain
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, -20.0f), kKickCap);
  CHECK_NEAR(db(peakDecoded(dst, kKickCap), wavPeak), -20.0, 1.0);
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, 12.0f), kKickCap);   // hot: clamps instead of wrapping
  double p12 = peakDecoded(dst, kKickCap);
  CHECK(p12 >= 30000 && p12 <= 32767);
  // a NaN gain (bad lp/config.txt value) means 0 dB, never undefined int conversion
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, nanf("")), kKickCap);
  CHECK_EQ(memcmp(dst, unity, kKickCap), 0);
  // out-of-range gains clamp to [-40, +24] dB
  CHECK_EQ(kMinGainDb, -40.0f); CHECK_EQ(kMaxGainDb, 24.0f);
  static uint8_t lo[kKickCap], hi[kKickCap];
  CHECK_EQ(wavLoadUlaw(*f, info, lo, kKickCap, -40.0f), kKickCap);
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, -1000.0f), kKickCap);
  CHECK_EQ(memcmp(dst, lo, kKickCap), 0);
  CHECK_NEAR(db(peakDecoded(lo, kKickCap), wavPeak), -40.0, 1.5);   // µ-law is coarser down here
  CHECK_EQ(wavLoadUlaw(*f, info, hi, kKickCap, 24.0f), kKickCap);
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, INFINITY), kKickCap);
  CHECK_EQ(memcmp(dst, hi, kKickCap), 0);
  CHECK_EQ(wavLoadUlaw(*f, info, dst, kKickCap, -INFINITY), kKickCap);
  CHECK_EQ(memcmp(dst, lo, kKickCap), 0);
  f->close();
}

// Counts the reads an IFile receives, so the loader's SD bursts can be measured (SPEC §9.2).
struct CountingFile : IFile {
  IFile* in; size_t bytes = 0, reads = 0, maxRead = 0, burst = 0;   // burst: bytes since the last mark()
  explicit CountingFile(IFile* f) : in(f) {}
  void mark() { burst = 0; }
  bool valid() const override { return in && in->valid(); }
  size_t read(void* dst, size_t n) override { size_t g = in->read(dst, n); bytes += g; burst += g; reads++; maxRead = std::max(maxRead, g); return g; }
  size_t write(const void* src, size_t n) override { return in->write(src, n); }
  bool seek(uint32_t pos) override { return in->seek(pos); }
  uint32_t position() const override { return in->position(); }
  uint32_t size() const override { return in->size(); }
  void close() override { in->close(); }
};

TEST(wav_loader_resumable_steps_bound_the_sd_burst) {
  ulawInit();
  std::vector<int16_t> ref;
  const char* path = overCapKick(ref);
  IFile* raw = scratch().open(path);
  if (!raw) { CHECK(false); return; }
  CountingFile f(raw);
  WavInfo info;
  CHECK(wavReadHeader(f, info));
  static uint8_t oneShot[kKickCap], stepped[kKickCap + 16];
  // the one-shot wrapper: 9 reads (8 x 2 KB + the rest) for a 400 ms 22050 mono kick = exactly the
  // 17 640 B the cap needs, in one burst; the second half of the file is never read
  f.bytes = f.reads = f.maxRead = 0;
  CHECK_EQ(wavLoadUlaw(f, info, oneShot, kKickCap, -3.0f), kKickCap);
  CHECK_EQ((unsigned)f.bytes, 2 * kKickCap);
  CHECK_EQ((unsigned)f.reads, (2 * kKickCap + kLoaderChunkBytes - 1) / kLoaderChunkBytes);   // 9
  CHECK_EQ((unsigned)f.maxRead, kLoaderChunkBytes);
  std::printf("  one-shot: %zu reads, %zu B\n", f.reads, f.bytes);
  // the resumable loader: every step reads at most 4 KB (two chunks), the result is identical
  UlawLoader ld;
  CHECK(ld.done());
  CHECK_EQ(ld.written(), 0u);
  CHECK(ld.begin(f, info, stepped, kKickCap, -3.0f));
  CHECK(!ld.done());
  memset(stepped, 0xAA, sizeof stepped);
  f.bytes = f.reads = f.maxRead = 0;
  int steps = 0; size_t maxBurst = 0; uint32_t lastWritten = 0;
  while (true) {
    f.mark();
    bool more = ld.step(4096);
    maxBurst = std::max(maxBurst, f.burst);
    steps++;
    CHECK(ld.written() >= lastWritten);
    lastWritten = ld.written();
    if (!more) break;
    CHECK(!ld.done());
    CHECK(f.burst > 0);                                          // every step that continues did some SD work
  }
  CHECK(ld.done());
  CHECK_EQ(ld.written(), kKickCap);
  CHECK_EQ((unsigned)maxBurst, 4096u);                           // SPEC §9.2: 4 KB per step, never more
  CHECK_EQ(steps, 5);                                            // 17 640 B / 4096 -> 5 steps
  CHECK_EQ((unsigned)f.bytes, 2 * kKickCap);
  CHECK_EQ(stepped[kKickCap], 0xAA);
  CHECK_EQ(memcmp(stepped, oneShot, kKickCap), 0);
  CHECK(!ld.step(4096));                                         // stepping a finished loader is a no-op
  CHECK_EQ(ld.written(), kKickCap);
  std::printf("  stepped: %d steps, max burst %zu B, %zu reads\n", steps, maxBurst, f.reads);
  // step(0) still makes progress (one chunk), step(huge) does everything at once
  CHECK(ld.begin(f, info, stepped, kKickCap, -3.0f));
  f.mark();
  CHECK(ld.step(0));
  CHECK_EQ((unsigned)f.burst, kLoaderChunkBytes);
  CHECK(!ld.step(0xFFFFFFFFu));
  CHECK_EQ(memcmp(stepped, oneShot, kKickCap), 0);
  // a cap shorter than the file stops reading once the cap is reached
  f.bytes = 0;
  CHECK(ld.begin(f, info, stepped, 1000, 0.0f));
  while (ld.step(4096)) {}
  CHECK_EQ(ld.written(), 1000u);
  CHECK_EQ((unsigned)f.bytes, 2000u);                            // exactly the 1000 frames the cap needs
  CHECK_EQ(ulawDecode(stepped[999]), 0);
  // 44.1 kHz stereo of the same cap reads 4x the bytes (nearest-frame stepping needs every other frame's chunk)
  WavInfo st44 = info; st44.sampleRate = 44100; st44.channels = 2; st44.dataBytes = 2 * kKickCap * 4;   // reinterpret
  f.bytes = 0;
  CHECK(ld.begin(f, st44, stepped, kKickCap / 4, 0.0f));
  while (ld.step(4096)) {}
  CHECK_EQ(ld.written(), kKickCap / 4);
  CHECK_EQ((unsigned)f.bytes, (unsigned)((2 * (kKickCap / 4 - 1) + 1) * 4));   // frames round((n-1)*2)+1, 4 B each
  // begin() on an unusable file reports failure and done()
  WavInfo bad = info; bad.bits = 24;
  CHECK(!ld.begin(f, bad, stepped, kKickCap, 0.0f));
  CHECK(ld.done()); CHECK_EQ(ld.written(), 0u);
  CHECK(!ld.begin(f, info, nullptr, kKickCap, 0.0f));
  CHECK(!ld.begin(f, info, stepped, 0, 0.0f));
  f.close();
}

// The vocal refill may run between loader steps: both share the 2 KB chunk buffer, so a stereo stream read
// in between must not corrupt the loader's output.
TEST(wav_loader_steps_interleave_with_stream_reads) {
  ulawInit();
  std::vector<int16_t> ref;
  const char* path = overCapKick(ref);
  std::vector<int16_t> st;
  for (int i = 0; i < 3000; i++) { st.push_back((int16_t)(i * 7)); st.push_back((int16_t)(i * 7 + 2)); }
  writeWav(scratchRoot() + "/interleave_st.wav", 22050, 2, 16, pcm16(st));
  IFile* kick = scratch().open(path);
  IFile* voc = scratch().open("/interleave_st.wav");
  CHECK(kick && voc);
  if (!kick || !voc) return;
  WavInfo ki, vi;
  CHECK(wavReadHeader(*kick, ki)); CHECK(wavReadHeader(*voc, vi));
  static uint8_t oneShot[kKickCap], stepped[kKickCap];
  CHECK_EQ(wavLoadUlaw(*kick, ki, oneShot, kKickCap, 0.0f), kKickCap);
  UlawLoader ld;
  CHECK(ld.begin(*kick, ki, stepped, kKickCap, 0.0f));
  static int16_t ring[700];
  uint32_t streamed = 0;
  int steps = 0;
  while (ld.step(2048)) {
    uint32_t got = wavReadMono(*voc, vi, ring, 700);           // stereo path goes through the shared chunk buffer
    for (uint32_t i = 0; i < got; i++) CHECK_EQ(ring[i], (int16_t)((streamed + i) * 7 + 1));
    streamed += got;
    steps++;
  }
  CHECK(steps >= 8);
  CHECK(streamed > 0);
  CHECK_EQ(memcmp(stepped, oneShot, kKickCap), 0);
  kick->close(); voc->close();
  scratch().remove("/interleave_st.wav");
}

// Data claimed beyond EOF (a hand-made WavInfo; wavReadHeader itself clamps): the loader returns what the
// file really holds and never pads with a held frame.
TEST(wav_loader_short_file_truncates) {
  ulawInit();
  std::vector<int16_t> s(1500);
  for (int i = 0; i < 1500; i++) s[i] = (int16_t)(1000 + i * 10);
  writeWav(scratchRoot() + "/short_data.wav", 22050, 1, 16, pcm16(s));
  IFile* f = scratch().open("/short_data.wav");
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.dataBytes, 3000u);
  WavInfo lying = info; lying.dataBytes = 3000 * 4;              // claims 6000 frames
  static uint8_t dst[8192];
  memset(dst, 0xAA, sizeof dst);
  CHECK_EQ(wavLoadUlaw(*f, lying, dst, 8000, 0.0f), 1500u);
  CHECK_EQ(dst[1500], 0xAA);
  int bad = 0;
  for (int i = 0; i < 1500; i++) if (!nearUlaw(ulawDecode(dst[i]), s[i])) bad++;
  CHECK_EQ(bad, 0);                                              // the 1500 real samples, unfaded (cap not reached)
  // upsampled, the rounding at the very end holds the last real frame (not EOF padding): 11025 -> 22050
  WavInfo up = info; up.sampleRate = 11025;
  CHECK_EQ(wavLoadUlaw(*f, up, dst, 8000, 0.0f), 3000u);
  CHECK(nearUlaw(ulawDecode(dst[2999]), s[1499]));
  f->close();
  scratch().remove("/short_data.wav");
}

TEST(wav_header_rejects_8_and_24_bit_and_malformed) {
  std::vector<int16_t> s(100, 1000);
  std::string p8 = scratchRoot() + "/eight.wav", p24 = scratchRoot() + "/twentyfour.wav", pnd = scratchRoot() + "/nodata.wav";
  writeWav(p8, 22050, 1, 8, std::vector<uint8_t>(100, 128));
  writeWav(p24, 22050, 1, 24, std::vector<uint8_t>(300, 0));
  PosixFs& fs = scratch();
  WavInfo info;
  IFile* f = fs.open("/eight.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  f = fs.open("/twentyfour.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  // no data chunk at all
  { std::vector<uint8_t> b = {'R','I','F','F'}; put32(b, 4 + 24); b.insert(b.end(), {'W','A','V','E','f','m','t',' '}); put32(b, 16);
    put16(b, 1); put16(b, 1); put32(b, 22050); put32(b, 44100); put16(b, 2); put16(b, 16);
    FILE* o = fopen(pnd.c_str(), "wb"); fwrite(b.data(), 1, b.size(), o); fclose(o); }
  f = fs.open("/nodata.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  // not a RIFF at all
  { FILE* o = fopen((scratchRoot() + "/junk.wav").c_str(), "wb"); fputs("MThd this is not a wave file at all........", o); fclose(o); }
  f = fs.open("/junk.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  // too short
  { FILE* o = fopen((scratchRoot() + "/short.wav").c_str(), "wb"); fputs("RIFF", o); fclose(o); }
  f = fs.open("/short.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  // 3 channels
  writeWav(scratchRoot() + "/three.wav", 22050, 3, 16, std::vector<uint8_t>(600, 0));
  f = fs.open("/three.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  // compressed format tag
  { std::vector<uint8_t> d = pcm16(s); writeWav(scratchRoot() + "/ima.wav", 22050, 1, 16, d);
    FILE* o = fopen((scratchRoot() + "/ima.wav").c_str(), "r+b"); fseek(o, 20, SEEK_SET); uint8_t tag[2] = {0x11, 0}; fwrite(tag, 1, 2, o); fclose(o); }
  f = fs.open("/ima.wav"); CHECK(f && !wavReadHeader(*f, info)); if (f) f->close();
  for (const char* n : {"/eight.wav", "/twentyfour.wav", "/nodata.wav", "/junk.wav", "/short.wav", "/three.wav", "/ima.wav"}) fs.remove(n);
}

TEST(wav_header_walks_extra_chunks_and_odd_padding) {
  std::vector<int16_t> s; for (int i = 0; i < 50; i++) s.push_back((int16_t)(i * 100));
  Chunk list{"LIST", std::vector<uint8_t>(13, 'L')};            // odd size: needs a pad byte
  Chunk fact{"fact", std::vector<uint8_t>(4, 0)};
  writeWav(scratchRoot() + "/chunky.wav", 22050, 1, 16, pcm16(s), {list}, {fact});
  PosixFs& fs = scratch();
  IFile* f = fs.open("/chunky.wav");
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.sampleRate, 22050u); CHECK_EQ(info.channels, 1); CHECK_EQ(info.bits, 16);
  CHECK_EQ(info.dataOffset, 12u + (8 + 14) + (8 + 16) + (8 + 4) + 8);
  CHECK_EQ(info.dataBytes, 100u);
  int16_t buf[64];
  CHECK_EQ(wavReadMono(*f, info, buf, 64), 50u);
  for (int i = 0; i < 50; i++) CHECK_EQ(buf[i], i * 100);
  f->close();
  fs.remove("/chunky.wav");
}

TEST(wav_header_clamps_lying_data_size) {
  std::vector<int16_t> s(1000, 1234);
  writeWav(scratchRoot() + "/lies.wav", 22050, 1, 16, pcm16(s), {}, {}, true);   // data size 0xFFFFFFFF
  PosixFs& fs = scratch();
  IFile* f = fs.open("/lies.wav");
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.dataBytes, 2000u);
  static uint8_t dst[4096];
  CHECK_EQ(wavLoadUlaw(*f, info, dst, 4096, 0.0f), 1000u);
  int16_t buf[1200];
  CHECK(f->seek(info.dataOffset));
  CHECK_EQ(wavReadMono(*f, info, buf, 1200), 1000u);
  CHECK_EQ(wavReadMono(*f, info, buf, 1200), 0u);
  f->close();
  fs.remove("/lies.wav");
}

TEST(wav_load_stereo_averaged_and_44100_halved) {
  ulawInit();
  // 44100 stereo: frame i has L = 4i, R = 4i + 2 -> mono 4i + 1; at 22050 output j takes frame 2j -> 8j + 1
  std::vector<int16_t> s;
  for (int i = 0; i < 2000; i++) { s.push_back((int16_t)(4 * i)); s.push_back((int16_t)(4 * i + 2)); }
  writeWav(scratchRoot() + "/st44.wav", 44100, 2, 16, pcm16(s));
  PosixFs& fs = scratch();
  IFile* f = fs.open("/st44.wav");
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.channels, 2); CHECK_EQ(info.sampleRate, 44100u); CHECK_EQ(info.dataBytes, 8000u);
  static uint8_t dst[4096];
  uint32_t n = wavLoadUlaw(*f, info, dst, 4096, 0.0f);
  CHECK_EQ(n, 1000u);                                            // 2000 frames at 44100 -> 1000 at 22050
  int bad = 0;
  for (uint32_t j = 0; j < n; j++) { double want = 8.0 * j + 1, got = ulawDecode(dst[j]); if (std::fabs(got - want) > 0.05 * want + 8) bad++; }
  CHECK_EQ(bad, 0);
  // streaming path averages stereo exactly (no resampling)
  CHECK(f->seek(info.dataOffset));
  static int16_t mono[2048];
  CHECK_EQ(wavReadMono(*f, info, mono, 1500), 1500u);
  for (int i = 0; i < 1500; i++) CHECK_EQ(mono[i], 4 * i + 1);
  CHECK_EQ(wavReadMono(*f, info, mono, 1500), 500u);             // stops at the data end
  for (int i = 0; i < 500; i++) CHECK_EQ(mono[i], 4 * (1500 + i) + 1);
  CHECK_EQ(wavReadMono(*f, info, mono, 1500), 0u);
  f->close();
  fs.remove("/st44.wav");
}

TEST(wav_load_nearest_sample_stepping_48000) {
  ulawInit();
  // step from -8000 to +8000 at source frame 1006. Output j reads frame round(j * 48000 / 22050):
  // j = 461 -> 1003.5 -> 1004 (low), j = 462 -> 1005.7 -> 1006 (high). Floor stepping would give 1005 (low) at 462.
  std::vector<int16_t> s(3000, -8000);
  for (int i = 1006; i < 3000; i++) s[i] = 8000;
  writeWav(scratchRoot() + "/m48.wav", 48000, 1, 16, pcm16(s));
  PosixFs& fs = scratch();
  IFile* f = fs.open("/m48.wav");
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  static uint8_t dst[4096];
  uint32_t n = wavLoadUlaw(*f, info, dst, 4096, 0.0f);
  CHECK_EQ(n, (uint32_t)(3000ull * 22050 / 48000));            // 1378
  CHECK(ulawDecode(dst[461]) < 0);
  CHECK(ulawDecode(dst[462]) > 0);
  CHECK(ulawDecode(dst[0]) < 0);
  CHECK(ulawDecode(dst[n - 1]) > 0);
  // up-sampling: 11025 -> 22050 duplicates every frame
  std::vector<int16_t> u; for (int i = 0; i < 300; i++) u.push_back((int16_t)(i * 50));
  writeWav(scratchRoot() + "/m11.wav", 11025, 1, 16, pcm16(u));
  IFile* g = fs.open("/m11.wav");
  CHECK(g != nullptr);
  if (g) {
    WavInfo i2; CHECK(wavReadHeader(*g, i2));
    uint32_t m = wavLoadUlaw(*g, i2, dst, 4096, 0.0f);
    CHECK_EQ(m, 600u);
    int bad = 0;
    for (uint32_t j = 0; j < m; j++) { double want = 50.0 * ((j + 1) / 2 > 299 ? 299 : (j + 1) / 2); double got = ulawDecode(dst[j]); if (std::fabs(got - want) > 0.05 * want + 8) bad++; }
    CHECK_EQ(bad, 0);
    g->close();
  }
  f->close();
  fs.remove("/m48.wav"); fs.remove("/m11.wav");
}

TEST(wav_stream_vocal_2048) {
  std::string dirS, nameS;
  CHECK(pickVocal(dirS, nameS));
  if (dirS.empty()) return;
  const char* dir = dirS.c_str();
  const char* name = nameS.c_str();
  char path[kMaxPathLen];
  joinPath(path, sizeof path, dir, name);
  IFile* f = sd().open(path);
  CHECK(f != nullptr);
  if (!f) return;
  WavInfo info;
  CHECK(wavReadHeader(*f, info));
  CHECK_EQ(info.sampleRate, 22050u); CHECK_EQ(info.channels, 1); CHECK_EQ(info.bits, 16);
  CHECK(info.dataBytes / 2 > 2048 * 2);
  static int16_t ring[2048];
  uint32_t n = wavReadMono(*f, info, ring, 2048);
  CHECK_EQ(n, 2048u);
  CHECK_EQ(f->position(), info.dataOffset + 4096);
  auto ref = rawSamples(sdRoot() + path, info.dataOffset, info.dataBytes);
  int bad = 0;
  for (int i = 0; i < 2048; i++) if (ring[i] != ref[i]) bad++;
  CHECK_EQ(bad, 0);
  // second block continues where the first stopped
  n = wavReadMono(*f, info, ring, 2048);
  CHECK_EQ(n, 2048u);
  bad = 0;
  for (int i = 0; i < 2048; i++) if (ring[i] != ref[2048 + i]) bad++;
  CHECK_EQ(bad, 0);
  // drain to the end: total equals the data length, then 0
  uint64_t total = 4096;
  while ((n = wavReadMono(*f, info, ring, 2048)) > 0) total += n;
  CHECK_EQ(total, (uint64_t)info.dataBytes / 2);
  CHECK_EQ(wavReadMono(*f, info, ring, 2048), 0u);
  // re-seek (variant loops re-seek to the phrase start, SPEC §5) and read again
  CHECK(f->seek(info.dataOffset + 2 * 100));
  CHECK_EQ(wavReadMono(*f, info, ring, 10), 10u);
  for (int i = 0; i < 10; i++) CHECK_EQ(ring[i], ref[100 + i]);
  std::printf("  vocal %s/%s: %u samples (%.2f s)\n", dir, name, (unsigned)(info.dataBytes / 2), info.dataBytes / 2.0 / kSampleRate);
  f->close();
}

int main() {
  int rc = lp_test::run();
  scratch().remove("/overcap.wav");
  rmdir(scratchRoot().c_str());
  return rc;
}
