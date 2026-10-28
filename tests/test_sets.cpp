// Host tests for gen/sets.cpp: the properties parser, the device-native .lpset writer (SPEC §9.8, §9.29: the session
// format, lp.src.i absolute paths, <name>_t<i>.mid SMFs beside the preset, no asset copies), the legacy reader
// (clip=assets/<basename>), the _index.txt listing, and the resumable loader (SPEC §9.5, §9.14) against a real Engine +
// Library + Generator over the card image at sdcard/lp (override with LP_SDCARD). Every card access of a sets().step()
// goes through a counting file system so the SD budget of SPEC §9.2 is asserted per step. Files the tests
// create under /lp/packs/House/presets (and one shard folder under /lp/midi/Chord/Amin) are removed by the
// last test; the card's own House presets are only ever read.
#include "check.h"
#include "gen/sets.h"
#include "gen/smf.h"
#include "gen/key.h"
#include "dsp/presets.h"
#include "dsp/ulaw.h"
#include "../firmware/desktop/posix_fs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <chrono>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace lp;

// ---------------------------------------------------------------------------------------------- counting fs

// IFileSystem decorator: counts probes, opens, walked entries and bytes while `on` (sets().step() only).
struct Counters { int probes = 0, opens = 0, walks = 0, entries = 0, renames = 0; size_t bytesRead = 0, bytesWritten = 0; };
struct CountingFs;
struct CountingFile : IFile {
  IFile* in = nullptr; CountingFs* fs = nullptr;
  bool valid() const override { return in && in->valid(); }
  size_t read(void* dst, size_t n) override;
  size_t write(const void* src, size_t n) override;
  bool seek(uint32_t pos) override { return in->seek(pos); }
  uint32_t position() const override { return in->position(); }
  uint32_t size() const override { return in->size(); }
  void close() override { if (in) { in->close(); in = nullptr; } }
};
struct CountingFs : IFileSystem {
  PosixFs& base; CountingFile pool[16]; Counters cur; bool on = false;
  // Slow-card model (SPEC §9.19 §3 d): every open / write / exists stalls the loop task for slowOpenMs while the engine
  // keeps rendering, every read for slowReadMs; `stall` renders that much audio. 0 = a fast card.
  int slowOpenMs = 0, slowReadMs = 0; void (*stall)(int ms) = nullptr; int slowOps = 0; bool logOps = false;   // logOps: print every slow operation (LP_SLOW_DEBUG)
  void slowOp(int ms) { if (ms > 0 && stall) { stall(ms); slowOps++; } }
  explicit CountingFs(PosixFs& b) : base(b) {}
  bool exists(const char* p) override { if (on) cur.probes++; if (logOps && slowOpenMs) std::printf("    [op] exists %s\n", p); slowOp(slowOpenMs); return base.exists(p); }
  bool isDir(const char* p) override { return base.isDir(p); }
  IFile* open(const char* p, const char* mode = "r") override {
    if (logOps && slowOpenMs) std::printf("    [op] open %s %s\n", mode, p);
    slowOp(slowOpenMs);
    IFile* f = base.open(p, mode); if (!f) return nullptr;
    if (on) cur.opens++;
    for (auto& w : pool) if (!w.in) { w.in = f; w.fs = this; return &w; }
    f->close(); return nullptr;
  }
  bool mkdir(const char* p) override { return base.mkdir(p); }
  bool remove(const char* p) override { return base.remove(p); }
  // Rename failures to inject (SPEC §9.22 rewrites): the next failRenames calls answer false; with unlinkOnFailedRename they
  // first remove the destination — SdFs::rename's shape when FatFs' second rename fails after the unlink of `to`.
  int failRenames = 0; bool unlinkOnFailedRename = false;
  bool rename(const char* a, const char* b) override {
    if (on) cur.renames++; slowOp(slowOpenMs);
    if (failRenames > 0) { failRenames--; if (unlinkOnFailedRename) base.remove(b); return false; }
    return base.rename(a, b);                                      // one per step at most
  }
  int openCount() const override { return base.openCount(); }     // the generator's handle budget asks (SPEC §9.19g)
  struct Walk { DirEntryFn fn; void* user; CountingFs* fs; };
  static bool walkFn(const char* name, bool isDir, uint32_t size, void* user) { Walk& w = *static_cast<Walk*>(user); if (w.fs->on) w.fs->cur.entries++; return w.fn(name, isDir, size, w.user); }
  bool walk(const char* p, DirEntryFn fn, void* user) override { if (on) cur.walks++; Walk w = {fn, user, this}; return base.walk(p, walkFn, &w); }
};
size_t CountingFile::read(void* dst, size_t n) { fs->slowOp(fs->slowReadMs); size_t got = in->read(dst, n); if (fs->on) fs->cur.bytesRead += got; return got; }
size_t CountingFile::write(const void* src, size_t n) { if (fs->logOps && fs->slowOpenMs) std::printf("    [op] write %zu B\n", n); fs->slowOp(fs->slowOpenMs); size_t put = in->write(src, n); if (fs->on) fs->cur.bytesWritten += put; return put; }

// ---------------------------------------------------------------------------------------------- fixtures

static std::string sdRoot() {
  const char* env = getenv("LP_SDCARD");
  if (env && *env) return env;
  struct stat st;
  if (stat("sdcard/lp", &st) == 0) return "sdcard";
  return "/Users/miha/Documents/cardputer/liveplayer/sdcard";
}
static PosixFs& sd() { static PosixFs fs(sdRoot().c_str()); return fs; }
static CountingFs& cfs() { static CountingFs* c = nullptr; if (!c) c = new CountingFs(sd()); return *c; }
static int16_t g_loopMem[kLoopSlots][kLoopRingBytes / 2];    // the loop slots' ring memory (the sketch's bootBlock, SPEC §9.19)
static Engine& eng() {
  static Engine* e = nullptr;
  if (!e) { e = new Engine(); ulawInit(); e->init(); for (int k = 0; k < kLoopSlots; k++) e->setLoopRing(k, g_loopMem[k], kLoopRingBytes / 2); Cmd t; t.type = CmdSetTempo; t.i = 120; e->cmds().push(t); static int16_t tmp[kBlock]; e->render(tmp, kBlock); }   // the suite reasons at 120 BPM; the firmware default is kDefaultBpm (125)
  return *e;
}
static Library& lib() { static Library l; static bool init = false; if (!init) { l.init(&cfs()); l.scanGenres(); init = true; } return l; }
static Generator& gen() {
  static Generator* g = nullptr;
  if (!g) {
    g = new Generator(); g->init(&cfs(), &eng(), &lib());
    for (int i = 0; i < lib().genreCount(); i++) if (strcmp(lib().genreName(i), "House") == 0) g->setGenre(i);
  }
  return *g;
}
static Sets& sets() { static Sets s; static bool init = false; if (!init) { s.init(&cfs(), &eng(), &gen(), &lib()); init = true; } return s; }

static const std::string kPresets = "/lp/packs/House/presets";
static const std::string kIndex = kPresets + "/_index.txt";
static const std::string kShardDir = "/lp/midi/Chord/Amin/zz_sets_test_shard";
static std::vector<std::string> g_created;               // card paths created by the tests (removed at the end)
static bool g_presetsExisted = false, g_assetsExisted = false;
static double g_maxStepMs = 0;
static Counters g_max;                                   // per-step maxima over every sets().step() while strict
static bool g_strict = true;                             // false while the known O(n²) directory-walk fallback runs

static std::string full(const std::string& card) { return sdRoot() + card; }
static bool exists(const std::string& card) { struct stat st; return stat(full(card).c_str(), &st) == 0; }
static std::vector<uint8_t> readAll(const std::string& card) {
  std::vector<uint8_t> v; FILE* f = fopen(full(card).c_str(), "rb"); if (!f) return v;
  uint8_t buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) v.insert(v.end(), buf, buf + n); fclose(f); return v;
}
static std::string readText(const std::string& card) { auto v = readAll(card); return std::string(v.begin(), v.end()); }
static void writeText(const std::string& card, const std::string& text) { FILE* f = fopen(full(card).c_str(), "wb"); if (f) { fwrite(text.data(), 1, text.size(), f); fclose(f); } }
static std::vector<std::string> listDir(const std::string& card, const char* ext) {
  std::vector<std::string> v; DIR* d = opendir(full(card).c_str()); if (!d) return v;
  while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n[0] == '.') continue; if (ext && !hasExt(n.c_str(), ext)) continue; v.push_back(n); }
  closedir(d); return v;
}
static bool hasLine(const std::string& text, const std::string& line) { return text.find("\n" + line + "\n") != std::string::npos || text.compare(0, line.size() + 1, line + "\n") == 0; }
static std::string valueOf(const std::string& text, const std::string& key) {
  size_t p = text.find("\n" + key + "="); if (p == std::string::npos) { if (text.compare(0, key.size() + 1, key + "=") == 0) p = (size_t)-1; else return ""; }
  size_t s = p + key.size() + 2; size_t e = text.find('\n', s); return text.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
static std::string baseOf(const std::string& p) { return p.substr(p.rfind('/') + 1); }
static std::string labelOf(const std::string& name) {       // key folder of a library file: Cmaj → C, Amin → Amin, NA → NA
  size_t s = name.find("__"); if (s == std::string::npos) return "";
  LibraryKey k = parseLibraryLabel(name.substr(0, s).c_str());
  if (!k.valid) return name.substr(0, s);
  return k.keyless ? "NA" : std::string(noteName(k.root)) + (k.minor ? "min" : "");
}
static std::string locate(const std::string& folder, const std::string& base) {   // folder/base, else folder/<shard>/base (a content folder may be sharded, SPEC §9.2)
  if (exists(folder + "/" + base)) return folder + "/" + base;
  for (const std::string& d : listDir(folder, nullptr)) if (exists(folder + "/" + d + "/" + base)) return folder + "/" + d + "/" + base;
  return "";
}
static void note(const std::string& card) { g_created.push_back(card); }
static std::string stemOf(const std::string& name) { return hasExt(name.c_str(), ".lpset") ? name.substr(0, name.size() - 6) : name; }
static std::string smfOf(const std::string& dir, const std::string& name, int t) { return dir + "/" + stemOf(name) + "_t" + std::to_string(t) + ".mid"; }
static void notePreset(const std::string& name) { note(kPresets + "/" + name); for (int t = 0; t < kTracksPerSet; t++) note(smfOf(kPresets, name, t)); }   // the file and whatever _t<i>.mid it wrote
// The presets this run wrote into the House folder (noted for the cleanup) that are still there. The card's own House
// presets are legacy files (SPEC §9.29: clip=assets/<basename>) whose asset copies are not part of the card image, so a
// random load must only ever see the suite's files: the index is rewritten with these names whenever the suite changes
// the folder (the legacy reader is exercised by legacy_preset_with_asset_copies_still_loads with a complete fixture).
static std::vector<std::string> suitePresets() {
  std::set<std::string> seen; std::vector<std::string> v;
  for (const std::string& c : g_created) {
    if (c.compare(0, kPresets.size() + 1, kPresets + "/") != 0 || !hasExt(c.c_str(), ".lpset")) continue;
    const std::string n = c.substr(kPresets.size() + 1);
    if (n.find('/') == std::string::npos && exists(c) && seen.insert(n).second) v.push_back(n);
  }
  return v;
}
static void rewriteIndex() {                                       // after a test deleted files by hand: the index lists the suite's own presets that are there
  std::string idx; for (auto& f : suitePresets()) idx += f + "\n";
  FILE* w = fopen(full(kIndex).c_str(), "wb"); if (w) { fwrite(idx.data(), 1, idx.size(), w); fclose(w); }
}
static void removePreset(const std::string& name) {                // a preset and its own SMFs (what Sets::deletePreset removes)
  sd().remove((kPresets + "/" + name).c_str());
  for (int t = 0; t < kTracksPerSet; t++) { const std::string m = smfOf(kPresets, name, t); if (exists(m)) sd().remove(m.c_str()); }
  rewriteIndex();
}
static std::set<std::string> indexNames() { std::set<std::string> s; std::string t = readText(kIndex); size_t p = 0; while (p < t.size()) { size_t e = t.find('\n', p); if (e == std::string::npos) e = t.size(); if (e > p) s.insert(t.substr(p, e - p)); p = e + 1; } return s; }
static void push(CmdType t, int track = 0, int a = 0, int32_t i = 0, float f = 0) { Cmd c; c.type = t; c.track = (uint8_t)track; c.a = (uint8_t)a; c.i = i; c.f = f; eng().cmds().push(c); }

// One counted sets().step(): the SD budget of SPEC §9.2 per step (≤ 32 walked entries, one file open plus a
// few existence probes, one 4 KB chunk) is tracked as maxima and asserted by sd_budget_per_step.
static void stepSets() {
  CountingFs& fs = cfs(); fs.cur = Counters(); fs.on = true;
  auto t0 = std::chrono::steady_clock::now();
  sets().step();
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  fs.on = false;
  if (ms > g_maxStepMs) g_maxStepMs = ms;
  if (!g_strict) return;
  const Counters& c = fs.cur;
  if (c.probes > g_max.probes) g_max.probes = c.probes; if (c.opens > g_max.opens) g_max.opens = c.opens; if (c.walks > g_max.walks) g_max.walks = c.walks;
  if (c.entries > g_max.entries) g_max.entries = c.entries; if (c.bytesRead > g_max.bytesRead) g_max.bytesRead = c.bytesRead; if (c.bytesWritten > g_max.bytesWritten) g_max.bytesWritten = c.bytesWritten;
  if (c.renames > g_max.renames) g_max.renames = c.renames;
}
// One loop iteration of the firmware: generator, sets, render.
static void pump(int iterations, int blocksPerStep = 1) {
  static int16_t buf[kBlock];
  for (int i = 0; i < iterations; i++) {
    gen().step(); gen().pumpVocal();
    stepSets();
    for (int b = 0; b < blocksPerStep; b++) eng().render(buf, kBlock);
  }
}
static bool pendingSwaps(int set) { for (int i = 0; i < kTracksPerSet; i++) if (eng().status().track[set * kTracksPerSet + i].pendingSwap) return true; return false; }
static void settle() { for (int i = 0; i < 4000 && (gen().busy() || sets().busy() || pendingSwaps(0)); i++) pump(1); }
static bool saveSync(int set, char* name, int cap) {          // save() with counted steps (the budget covers the save paths too)
  Sets& s = sets();
  if (name && cap) name[0] = 0;
  if (!s.beginSave(set)) return false;
  for (int n = 0; s.busy() && n < 100000; n++) stepSets();
  if (name && cap) { strncpy(name, s.lastName(), (size_t)cap - 1); name[cap - 1] = 0; }
  return s.ok();
}
static int loadSync(int set, Rng& rng, char* name, int cap) {   // loadRandom with counted steps; returns steps until the choice (-1 = refused)
  Sets& s = sets();
  if (!s.beginLoad(set, rng)) { if (name && cap) name[0] = 0; return -1; }
  int n = 0;
  while (s.busy() && s.lastName()[0] == 0 && n < 100000) { stepSets(); n++; }   // lastName() is set once the file is parsed
  if (name && cap) { strncpy(name, s.busy() ? s.lastName() : "", (size_t)cap - 1); name[cap - 1] = 0; }
  return n;
}

struct Snapshot { std::string file; uint32_t seed; int variant, len, preset; int state; std::string kit[kSampleSlots]; };
static std::vector<Snapshot> snapshot() {
  std::vector<Snapshot> v;
  for (int t = 0; t < kTracksPerSet; t++) {
    Snapshot s; const TrackGenInfo& i = gen().info(t); const TrackStatus& st = eng().status().track[t];
    s.file = i.file; s.seed = i.seed; s.variant = st.variant; s.len = st.lenTicks; s.preset = i.presetIndex; s.state = st.state;
    for (int k = 0; k < kSampleSlots; k++) s.kit[k] = eng().kit(0).name[k];
    v.push_back(s);
  }
  return v;
}

// ---------------------------------------------------------------------------------------------- parser

TEST(parser_split_rules) {
  const char* k; const char* v; char line[128];
  strcpy(line, "key=value"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "key") == 0); CHECK(strcmp(v, "value") == 0);
  strcpy(line, "key: value"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "key") == 0); CHECK(strcmp(v, "value") == 0);
  strcpy(line, "   key   =   spaced value   "); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "key") == 0); CHECK(strcmp(v, "spaced value") == 0);
  strcpy(line, "a=b=c:d"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "a") == 0); CHECK(strcmp(v, "b=c:d") == 0);   // first separator only (:42)
  strcpy(line, "key  value with spaces"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "key") == 0); CHECK(strcmp(v, "value with spaces") == 0);   // :44
  strcpy(line, "# comment = x"); CHECK(!parsePropertyLine(line, &k, &v));
  strcpy(line, "  ! bang"); CHECK(!parsePropertyLine(line, &k, &v));
  strcpy(line, ""); CHECK(!parsePropertyLine(line, &k, &v));
  strcpy(line, "   \t  "); CHECK(!parsePropertyLine(line, &k, &v));
  strcpy(line, "lonely"); CHECK(!parsePropertyLine(line, &k, &v));
  strcpy(line, "empty="); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "empty") == 0); CHECK(strcmp(v, "") == 0);
  strcpy(line, "=novalue"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "") == 0); CHECK(strcmp(v, "novalue") == 0);
  strcpy(line, "key=value\r"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(v, "value") == 0);   // trim eats a stray CR too
}

TEST(parser_escapes) {
  char s[64];
  strcpy(s, "a\\tb"); unescapeProperty(s); CHECK(strcmp(s, "a\tb") == 0);
  strcpy(s, "a\\nb\\rc"); unescapeProperty(s); CHECK(strcmp(s, "a\nb\rc") == 0);
  strcpy(s, "x\\=y\\:z\\#w\\!v\\ u"); unescapeProperty(s); CHECK(strcmp(s, "x=y:z#w!v u") == 0);
  strcpy(s, "C:\\\\takes\\\\x"); unescapeProperty(s); CHECK(strcmp(s, "C:\\takes\\x") == 0);     // \\ then 't' is not a tab (:10-11)
  strcpy(s, "\\q\\z"); unescapeProperty(s); CHECK(strcmp(s, "\\q\\z") == 0);                       // unknown pairs stay
  strcpy(s, "end\\"); unescapeProperty(s); CHECK(strcmp(s, "end\\") == 0);                         // lone trailing backslash
  const char* k; const char* v; char line[64];
  strcpy(line, "k\\:ey=v"); CHECK(parsePropertyLine(line, &k, &v)); CHECK(strcmp(k, "k\\") == 0); CHECK(strcmp(v, "ey=v") == 0);   // split before unescape (:42 then :47)
}

struct Props { std::vector<std::pair<std::string, std::string>> kv; };
static void collect(const char* k, const char* v, void* u) { static_cast<Props*>(u)->kv.push_back({k, v}); }

TEST(reader_crlf_continuations_and_chunking) {
  const char* text = "schemaVersion=1\r\n# comment\r\n\r\nlong=part1\\\r\npart2\\\\\r\nnext: value\r\neven=two\\\\\\\\\r\nlast=no newline";
  for (int chunk = 1; chunk <= 7; chunk += 3) {                    // any chunking must give the same lines
    Props p; PropertiesReader r; r.begin(collect, &p);
    int n = (int)strlen(text);
    for (int i = 0; i < n; i += chunk) r.feed(text + i, i + chunk <= n ? chunk : n - i);
    r.finish();
    CHECK_EQ((int)p.kv.size(), 5);
    if (p.kv.size() == 5) {
      CHECK(p.kv[0].first == "schemaVersion" && p.kv[0].second == "1");
      CHECK(p.kv[1].first == "long" && p.kv[1].second == "part1part2\\");   // odd backslashes join (:36), the even tail unescapes to one
      CHECK(p.kv[2].first == "next" && p.kv[2].second == "value");
      CHECK(p.kv[3].first == "even" && p.kv[3].second == "two\\\\");
      CHECK(p.kv[4].first == "last" && p.kv[4].second == "no newline");
    }
  }
  // a continuation pending at EOF is dropped (lines() never yields it)
  Props p; PropertiesReader r; r.begin(collect, &p); r.feed("a=1\\", 4); r.finish(); CHECK_EQ((int)p.kv.size(), 0);
  // over-long lines are dropped whole, the following line survives
  std::string big = "big=" + std::string(kSetLineMax + 10, 'x') + "\nok=1\n";
  Props q; PropertiesReader r2; r2.begin(collect, &q); r2.feed(big.c_str(), (int)big.size()); r2.finish();
  CHECK_EQ((int)q.kv.size(), 1); if (!q.kv.empty()) CHECK(q.kv[0].first == "ok");
}

TEST(set_file_properties) {
  // a legacy preset (SPEC §9.29: the keys of the REAPER export days) and the current lp keys side by side
  const char* text =
    "schemaVersion=1\nsetIndex=1\nkeyRoot=21\nkeyName=A minor\ngenre=House\n"
    "track.0.category=CHORD\ntrack.0.clipType=MIDI\ntrack.0.clip=assets\\Amin__Chord__x.mid\ntrack.0.slot=2\n"
    "track.0.instrument=preset:Chord/PluckWide\n"
    "track.6.clipType=WAV\ntrack.6.clip=assets/NA__110bpm__v.wav\ntrack.6.slot=7\n"
    "track.7.clipType=NONE\ntrack.7.beatLayer=1\ntrack.7.beatKickMuted=true\ntrack.7.beatGroupStart=true\n"
    "track.2.clip=/abs/olute.mid\n"                                 // a legacy clip is relative: an absolute one is ignored
    "lp.src.3=/lp/midi/Bass/Amin/x.mid\ntrack.3.clip=assets/other.mid\n"   // lp.src.i wins over a clip line, whatever the order
    "track.4.clip=assets/first.mid\nlp.src.4=/lp/packs/House/presets/House_Amin_001_t4.mid\n"
    "lp.track.1.variant=3\nlp.track.1.kickcut=1\nlp.track.5.variant=6\n"
    "lp.bpm=124\nlp.track.0.preset=5\nlp.track.0.seed=4000000000\nlp.track.0.len=0.5\nlp.track.0.key=-3\nlp.track.0.lenk=-1\n"
    "lp.track.6.pause=2\nlp.track.1.pause=9\n"
    "lp.track.7.kit=/lp/a/k.wav;/lp/b/s.wav;/lp/c/h.wav;/lp/d/o.wav\nlp.track.5.kit=/p/1.wav;/p/2.wav\n"
    "unknown.key=ignored\ntrack.9.slot=3\ntrack.x.slot=3\n";
  SetFile f; parseSetFileText(text, (int)strlen(text), f);
  CHECK_EQ((int)f.track[6].pause, 2); CHECK_EQ((int)f.track[6].hasPause, 1);                      // SPEC §9.16
  CHECK_EQ((int)f.track[1].pause, 4); CHECK_EQ((int)f.track[1].hasPause, 1);                      // 9 is not a step: 4
  CHECK_EQ((int)f.track[0].pause, kVocalPauseDefault); CHECK_EQ((int)f.track[0].hasPause, 0);     // absent: default 1
  CHECK_EQ(f.schemaVersion, 1); CHECK_EQ(f.setIndex, 1); CHECK_EQ(f.keyRoot, 9); CHECK(strcmp(f.keyName, "A minor") == 0); CHECK(strcmp(f.genre, "House") == 0);
  CHECK(strcmp(f.src[0], "assets\\Amin__Chord__x.mid") == 0); CHECK_EQ((int)f.track[0].variant, 2);   // the legacy clip as written (sourceAbs sanitises it at the load)
  CHECK(strcmp(f.src[6], "assets/NA__110bpm__v.wav") == 0); CHECK_EQ((int)f.track[6].variant, 3);   // slot & 3
  CHECK(f.src[7][0] == 0); CHECK_EQ((int)f.track[7].variant, 1); CHECK_EQ((int)f.track[7].beatKickMuted, 1);
  CHECK(f.src[2][0] == 0);
  CHECK(strcmp(f.src[3], "/lp/midi/Bass/Amin/x.mid") == 0); CHECK(strcmp(f.src[4], "/lp/packs/House/presets/House_Amin_001_t4.mid") == 0);
  CHECK_EQ((int)f.track[1].variant, 3); CHECK_EQ((int)f.track[1].beatKickMuted, 1); CHECK_EQ((int)f.track[5].variant, 2);   // the current keys (variant & 3)
  CHECK_EQ(f.bpm, 124); CHECK_EQ((int)f.track[0].preset, 5); CHECK_EQ((int)f.track[0].hasPreset, 1);
  CHECK_EQ((long long)f.track[0].seed, 4000000000LL); CHECK_NEAR(f.track[0].lenBeats, 0.5, 1e-6); CHECK_EQ((int)f.track[0].keyRoot, 9); CHECK_EQ((int)f.track[0].lenk, -1);
  CHECK(strcmp(f.kit[0], "/lp/a/k.wav") == 0); CHECK(strcmp(f.kit[3], "/lp/d/o.wav") == 0);
  CHECK(strcmp(f.kit[4], "/p/1.wav") == 0); CHECK(strcmp(f.kit[5], "/p/2.wav") == 0); CHECK(f.kit[6][0] == 0);
  CHECK_EQ((int)f.track[1].hasPreset, 0); CHECK_EQ((int)f.track[1].hasLen, 0);
}

TEST(helpers_numbers_bpm_natural_length) {
  CHECK_EQ(Sets::parsePresetNumber("House_Amin_007.lpset"), 7);
  CHECK_EQ(Sets::parsePresetNumber("House_A#min_12.LPSET"), 12);
  CHECK_EQ(Sets::parsePresetNumber("Deep_House_Fmin_123456.lpset"), 123456);
  CHECK_EQ(Sets::parsePresetNumber("x.lpset"), -1);
  CHECK_EQ(Sets::parsePresetNumber("House_Amin_001.mid"), -1);
  CHECK_EQ(Sets::parsePresetNumber("001.lpset"), -1);
  { char m[kMaxPathLen]; Sets::presetSmfPath(m, sizeof m, "/lp/packs/House/presets", "House_Amin_007.lpset", 4); CHECK(std::string(m) == "/lp/packs/House/presets/House_Amin_007_t4.mid");
    Sets::presetSmfPath(m, sizeof m, "/x", "hand_made", 0); CHECK(std::string(m) == "/x/hand_made_t0.mid"); }
  // the key label in the name (SPEC §9.8 names) decides the loader's filter without opening the file
  CHECK_EQ(Sets::presetKeyRoot("House_Amin_007.lpset"), 9);
  CHECK_EQ(Sets::presetKeyRoot("Deep_House_F#min_123.lpset"), 6);
  CHECK_EQ(Sets::presetKeyRoot("House_C_003.lpset"), 9);              // C major ≡ A minor (SPEC §2)
  CHECK_EQ(Sets::presetKeyRoot("House_Emin_001.lpset"), 4);
  CHECK_EQ(Sets::presetKeyRoot("House_NA_001.lpset"), -1);            // keyless: probe
  CHECK_EQ(Sets::presetKeyRoot("custom_set.lpset"), -1);
  CHECK_EQ(Sets::presetKeyRoot("House_Amin.lpset"), -1);
  CHECK_EQ(Sets::presetKeyRoot("Amin_001.lpset"), -1);                // no genre part
  CHECK_NEAR(Sets::bpmToken("Bmin__100bpm__GHUAC__Vocal__x.wav"), 100, 0);
  CHECK_NEAR(Sets::bpmToken("/lp/packs/House/vocal/NA/NA__128BPM__y.wav"), 128, 0);
  CHECK_NEAR(Sets::bpmToken("phrase_bpm_124.wav"), 124, 0);
  CHECK_NEAR(Sets::bpmToken("phrase 90 - bpm.wav"), 90, 0);
  CHECK_NEAR(Sets::bpmToken("nobpm.wav"), 0, 0);
  CHECK_NEAR(Sets::bpmToken("fast_300bpm.wav"), 0, 0);
  CHECK_EQ((int)Sets::naturalVocalTicks(22050 * 2, 22050, 120), 4 * kTicksPerBeat);       // 2 s at 120 = 4 beats
  CHECK_EQ((int)Sets::naturalVocalTicks(22050 * 3 / 2, 22050, 120), 4 * kTicksPerBeat);   // 3 beats → 4 (nearest in log2)
  CHECK_EQ((int)Sets::naturalVocalTicks(44100 * 7, 44100, 120), 16 * kTicksPerBeat);      // 14 beats → 16
  CHECK_EQ((int)Sets::naturalVocalTicks(22050 * 20, 22050, 128), 16 * kTicksPerBeat);     // 42.7 beats clamps to 16
  CHECK_EQ((int)Sets::naturalVocalTicks(22050 / 4, 22050, 120), 1 * kTicksPerBeat);       // 0.5 beats → 1
  CHECK_EQ((int)Sets::naturalVocalTicks(0, 22050, 120), kTicksPerBar);
  CHECK_EQ((int)Sets::naturalVocalTicks(22050, 22050, 0), kTicksPerBar);
}

// ---------------------------------------------------------------------------------------------- round trip

TEST(generate_set0) {
  g_presetsExisted = exists(kPresets); g_assetsExisted = exists(kPresets + "/assets");   // (a legacy assets/ folder on the card is left exactly as found)
  CHECK(lib().genreCount() > 0);
  CHECK(strcmp(gen().genreName(), "House") == 0);
  CHECK(gen().generateSet(0));
  settle();
  CHECK(!gen().busy());
  int midiTracks = 0, kitSlots = 0;
  for (int t = 0; t < kTracksPerSet; t++) { if (gen().info(t).file[0]) midiTracks++; std::printf("  track %d %-10s %s\n", t, categoryName(trackCategory(t)), gen().info(t).file); }
  std::printf("  generator lastError='%s' lastNote='%s'\n", gen().lastError(), gen().lastNote());
  for (int k = 0; k < kSampleSlots; k++) if (eng().kit(0).name[k][0]) kitSlots++;
  CHECK(midiTracks >= 7);                                           // 7 MIDI categories + vocal (NA folder present)
  CHECK_EQ(kitSlots, kSampleSlots);
  CHECK_EQ((int)eng().status().track[7].variant, 0);                // a new Beat starts on Kick only (SPEC §9.17b)
  CHECK_EQ((int)eng().status().track[7].state, (int)PSStopped);     // stopped tracks adopt silently
  CHECK_EQ((int)eng().status().generating[0], 0);
}

static std::string g_name1, g_name2, g_name3;
static int g_saveStepsVocal = 0;                         // steps of the first save (a generated set with a vocal), reported at the end

TEST(save_writes_a_session_format_preset) {
  Sets& s = sets();
  uint16_t chordNatural = eng().status().track[Chord].lenTicks;
  CHECK_EQ((int)gen().setLengthStep(Chord, +1), 2 * chordNatural);   // len± before saving: lenk = 1 must round-trip (SPEC §9.4)
  pump(22050 * 4 / kBlock + 2);                                     // past the bar line where CmdSetLength applies
  CHECK_EQ((int)eng().status().track[Chord].lenTicks, 2 * chordNatural);
  CHECK(s.beginSave(0));
  CHECK(s.busy()); CHECK(s.saving());
  int saveSteps = 0; while (s.busy() && saveSteps < 100000) { pump(1); saveSteps++; }
  CHECK(!s.busy());
  CHECK(s.ok()); if (!s.ok()) std::printf("  error: %s\n", s.lastError());
  g_name1 = s.lastName();
  std::printf("  saved %s in %d steps (max step %.2f ms)\n", g_name1.c_str(), saveSteps, g_maxStepMs);
  g_saveStepsVocal = saveSteps;
  CHECK(g_name1 == "House_Amin_001.lpset" || Sets::parsePresetNumber(g_name1.c_str()) >= 1);
  std::string card = kPresets + "/" + g_name1; notePreset(g_name1); note(kIndex);
  CHECK(exists(card));
  CHECK(exists(kIndex)); CHECK(indexNames().count(g_name1) == 1);   // the writer keeps the listing index (SPEC §9.2)
  if (g_presetsExisted) { CHECK(indexNames().size() >= 1 + listDir(kPresets, ".lpset").size() - 1); rewriteIndex(); }   // built from the whole folder; from here on the random loads see the suite's files only (see suitePresets)
  CHECK(exists(kPresets + "/assets") == g_assetsExisted);           // SPEC §9.29: no assets/ folder is created, nothing is copied
  std::string text = readText(card);
  // header
  CHECK(hasLine(text, "schemaVersion=1")); CHECK(hasLine(text, "setIndex=0")); CHECK(hasLine(text, "keyRoot=9"));
  CHECK(hasLine(text, "keyName=A minor")); CHECK(hasLine(text, "genre=House")); CHECK(hasLine(text, "lp.bpm=120")); CHECK(hasLine(text, "lp.banks=2"));
  CHECK(text.find('\\') == std::string::npos);
  CHECK(text.find("instrument") == std::string::npos);
  for (char c : text) CHECK((unsigned char)c < 128);
  // SPEC §9.29: lp.* keys only — no REAPER keys (track.i.category / clipType / clip / slot / beatLayer / beatKickMuted)
  for (size_t p = 0; p < text.size();) {
    const size_t e = text.find('\n', p); const std::string line = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
    const bool known = line.compare(0, 3, "lp.") == 0 || line.compare(0, 14, "schemaVersion=") == 0 || line.compare(0, 9, "setIndex=") == 0 || line.compare(0, 8, "keyRoot=") == 0 || line.compare(0, 8, "keyName=") == 0 || line.compare(0, 6, "genre=") == 0;
    if (!known) std::printf("  unexpected line: %s\n", line.c_str());
    CHECK(known);
    p = e == std::string::npos ? text.size() : e + 1;
  }
  CHECK(text.find("clip") == std::string::npos); CHECK(text.find("track.0.slot") == std::string::npos); CHECK(text.find("beatLayer") == std::string::npos); CHECK(text.find("beatKickMuted") == std::string::npos);
  // per track: the source as an absolute path that exists on the card (library MIDI in its key folder or shard, the
  // vocal phrase under the pack), its basename the generator's file; variant and kick cut as lp keys
  for (int i = 0; i < kTracksPerSet; i++) {
    const std::string src = valueOf(text, "lp.src." + std::to_string(i));
    CHECK(!src.empty());
    if (!src.empty()) {
      CHECK(src[0] == '/'); CHECK(src.find("..") == std::string::npos); CHECK(exists(src));
      CHECK(src.find("/presets/") == std::string::npos);            // a library file, not a copy
      CHECK(src.compare(0, (i == 6 ? std::string("/lp/packs/House/vocal/") : "/lp/midi/" + std::string(categoryName((Category)i)) + "/").size(), i == 6 ? std::string("/lp/packs/House/vocal/") : "/lp/midi/" + std::string(categoryName((Category)i)) + "/") == 0);
      CHECK(baseOf(src) == gen().info(i).file);
      for (int t = 0; t < kTracksPerSet; t++) CHECK(!exists(smfOf(kPresets, g_name1, t)));   // nothing edited: no SMF beside the preset
    }
    CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".variant") == std::to_string(eng().status().track[i].variant));
    CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".kickcut") == (i == Beat ? "0" : ""));
    CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".seed") == std::to_string(gen().info(i).seed));
    CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".key") == "9");
    CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".lenk") == (i == Chord ? "1" : "0"));
    std::string len = valueOf(text, "lp.track." + std::to_string(i) + ".len");
    // SPEC §9.16: the Vocal's len stays Reaper's loop length (natural power-of-two beats), its cycle travels as pause
    if (i == Vocal) { CHECK(len == std::to_string(gen().info(i).naturalTicks / kTicksPerBeat)); CHECK_EQ((int)eng().status().track[i].lenTicks, 3 * kTicksPerBar); }
    else CHECK(len == std::to_string(eng().status().track[i].lenTicks / kTicksPerBeat));
    CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".pause") == (i == Vocal ? "1" : ""));
    if (isSynthCategory((Category)i)) CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".preset") == std::to_string(gen().info(i).presetIndex));
    else CHECK(valueOf(text, "lp.track." + std::to_string(i) + ".preset") == "");
  }
  CHECK(hasLine(text, "lp.track.7.variant=0")); CHECK(hasLine(text, "lp.track.7.kickcut=0"));
  // SPEC §9.17b: every track writes its strip, the synth tracks their knobs and acid flag (defaults here)
  for (int i = 0; i < kTracksPerSet; i++) {
    const std::string p = "lp.track." + std::to_string(i) + ".";
    CHECK(valueOf(text, p + "vol") == "0.800"); CHECK(valueOf(text, p + "dist") == "0.000"); CHECK(valueOf(text, p + "filter") == "0.000");
    CHECK(valueOf(text, p + "echo") == (i == Vocal ? "0.350" : "0.000")); CHECK(valueOf(text, p + "reverb") == "0.000");
    if (isSynthCategory((Category)i)) { CHECK(!valueOf(text, p + "cutoff").empty()); CHECK(!valueOf(text, p + "res").empty()); CHECK(valueOf(text, p + "acid") == "0"); CHECK(!valueOf(text, p + "wave").empty()); }
    else { CHECK(valueOf(text, p + "cutoff").empty()); CHECK(valueOf(text, p + "acid").empty()); }
  }
  CHECK(text.find("lp.kit.") == std::string::npos); CHECK(text.find("lp.volume") == std::string::npos); CHECK(text.find("lp.seq") == std::string::npos); CHECK(text.find("lp.end") == std::string::npos); CHECK(text.find("lp.loaded") == std::string::npos);   // session-only keys
  // kits: four absolute, existing paths per drum track, basenames equal the engine's kit names
  for (int tr : {7, 5}) {
    std::string kit = valueOf(text, "lp.track." + std::to_string(tr) + ".kit");
    int n = 0; size_t pos = 0;
    while (pos <= kit.size()) {
      size_t e = kit.find(';', pos); std::string path = kit.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
      CHECK(path.compare(0, 15, "/lp/packs/House") == 0); CHECK(exists(path));
      CHECK(baseOf(path) == eng().kit(0).name[(tr == 7 ? 0 : 4) + n]);
      n++; pos = e == std::string::npos ? kit.size() + 1 : e + 1;
    }
    CHECK_EQ(n, 4);
  }
  // what the writer serialised is what the parser reads back
  SetFile back; parseSetFileText(text.c_str(), (int)text.size(), back);
  const SetFile& written = s.file();
  CHECK_EQ(back.keyRoot, written.keyRoot); CHECK_EQ(back.bpm, written.bpm);
  for (int i = 0; i < kTracksPerSet; i++) {
    CHECK(strcmp(back.src[i], written.src[i]) == 0);
    CHECK_EQ((long long)back.track[i].seed, (long long)written.track[i].seed); CHECK_EQ((int)back.track[i].variant, (int)written.track[i].variant); CHECK_EQ((int)back.track[i].beatKickMuted, (int)written.track[i].beatKickMuted);
    CHECK_NEAR(back.track[i].lenBeats, written.track[i].lenBeats, 1e-6);
    CHECK_EQ((int)back.track[i].pause, (int)written.track[i].pause); CHECK_EQ((int)back.track[i].hasPause, i == Vocal ? 1 : 0);
  }
  for (int k = 0; k < kSampleSlots; k++) CHECK(strcmp(back.kit[k], written.kit[k]) == 0);
}

TEST(save_again_increments_number_and_copies_nothing) {
  Sets& s = sets();
  const int filesBefore = (int)listDir(kPresets, nullptr).size();
  char name[64] = "";
  CHECK(s.save(0, name, sizeof name));
  g_name2 = name; notePreset(g_name2);
  CHECK_EQ(Sets::parsePresetNumber(g_name2.c_str()), Sets::parsePresetNumber(g_name1.c_str()) + 1);
  CHECK(readText(kPresets + "/" + g_name2) == readText(kPresets + "/" + g_name1));   // same state → same text (the sources are the library's paths)
  CHECK_EQ((int)listDir(kPresets, nullptr).size(), filesBefore + 1);   // the one .lpset, no SMF, no copy
  CHECK(exists(kPresets + "/assets") == g_assetsExisted);
  CHECK(indexNames().count(g_name2) == 1);
  CHECK(!s.save(-1, name, sizeof name));                           // bad set index
}

TEST(edited_pattern_written_with_smf_writer) {
  const int fx = Fx;
  TrackGenInfo saved = gen().info(fx);
  Pattern& src = eng().sourcePattern(fx);
  Pattern original = src;
  const uint16_t fxLen = eng().status().track[fx].lenTicks;
  src.clear(2 * kTicksPerBar);
  src.add(Note::make(0, 48, 57, 100)); src.add(Note::make(96, 24, 60, 90)); src.add(Note::make(384, 96, 64, 80)); src.add(Note::make(720, 36, 69, 110));
  gen().info(fx).file[0] = 0;                                      // a recorded loop: notes without a library file (SPEC §9.10)
  gen().rebuildVariant(fx, 0); pump(22050 * 4 / kBlock + 2);        // ... published as the looper does (Ui::publishLoop → rebuildVariant): the clip is the loop's length
  CHECK_EQ((int)eng().status().track[fx].lenTicks, 2 * kTicksPerBar); CHECK_EQ((int)gen().info(fx).naturalTicks, 2 * kTicksPerBar);
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  g_name3 = name; notePreset(g_name3);
  std::string text = readText(kPresets + "/" + g_name3);
  const std::string smf = smfOf(kPresets, g_name3, Fx);              // SPEC §9.29: <name>_t4.mid next to the preset
  CHECK(valueOf(text, "lp.src.4") == smf); CHECK(valueOf(text, "lp.track.4.edited").empty());   // (edited / loop are session keys)
  CHECK(exists(smf));
  for (int t = 0; t < kTracksPerSet; t++) if (t != Fx) CHECK(!exists(smfOf(kPresets, g_name3, t)));
  CHECK(exists(kPresets + "/assets") == g_assetsExisted);
  IFile* f = sd().open(smf.c_str(), "r"); CHECK(f != nullptr);
  if (f) {
    Pattern back; SmfInfo info; CHECK(smfParse(*f, back, &info)); f->close();
    CHECK_EQ((int)back.count, (int)src.count); CHECK_EQ((int)back.lengthTicks, (int)src.lengthTicks);
    for (int i = 0; i < back.count && i < src.count; i++) { CHECK_EQ((int)back.notes[i].start(), (int)src.notes[i].start()); CHECK_EQ((int)back.notes[i].pitch(), (int)src.notes[i].pitch()); CHECK_EQ((int)back.notes[i].len(), (int)src.notes[i].len()); }
    CHECK_NEAR(info.bpm, 120, 0.5);
  }
  src = original; gen().info(fx) = saved;                          // restore the generated state
  gen().rebuildVariant(fx, saved.variant); pump(22050 * 4 / kBlock + 2);
  CHECK_EQ((int)eng().status().track[fx].lenTicks, (int)fxLen);
}

TEST(save_key_follows_the_tracks_not_the_global_key) {
  // The material was generated in A minor; a key change after that affects future generation only (SPEC §2,
  // §9.9), so the set is filed under A minor whatever the global key says now.
  Sets& s = sets();
  const uint8_t savedRoot = gen().key().root;
  gen().key().root = 4;                                            // E
  CHECK_EQ(s.keyRootOf(0), 9);
  TrackGenInfo fxSaved = gen().info(Fx); Pattern fxSrc = eng().sourcePattern(Fx);
  gen().info(Fx).edited = 1;                                       // an edit: the SMF asset carries the track's own label
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  std::string n = name; note(kPresets + "/" + n);
  CHECK(n.compare(0, 11, "House_Amin_") == 0);
  std::string text = readText(kPresets + "/" + n);
  CHECK(hasLine(text, "keyRoot=9")); CHECK(hasLine(text, "keyName=A minor"));
  const std::string src4 = valueOf(text, "lp.src.4");
  std::printf("  saved %s with Fx source %s (Fx keyRoot %d, source notes %d)\n", n.c_str(), src4.c_str(), gen().info(Fx).keyRoot, eng().sourcePattern(Fx).count);
  CHECK(src4 == smfOf(kPresets, n, Fx)); CHECK(exists(src4));
  gen().info(Fx) = fxSaved; eng().sourcePattern(Fx) = fxSrc;
  // one re-rolled track in another key: the majority decides
  const uint8_t leadRoot = gen().info(Lead).keyRoot;
  gen().info(Lead).keyRoot = 4;
  CHECK_EQ(s.keyRootOf(0), 9);
  gen().info(Lead).keyRoot = leadRoot;
  // an empty set reports the global key
  TrackGenInfo keep[kTracksPerSet]; for (int t = 0; t < kTracksPerSet; t++) { keep[t] = gen().info(t); gen().info(t) = TrackGenInfo(); }
  CHECK_EQ(s.keyRootOf(0), 4);
  for (int t = 0; t < kTracksPerSet; t++) gen().info(t) = keep[t];
  gen().key().root = savedRoot;
  removePreset(n);                                                 // keep the random loads below on complete sets (the SMF goes with it)
  CHECK(!exists(src4));
}

TEST(save_resolves_sharded_sources_and_reports_missing_ones) {
  // A pattern picked from a shard subfolder (/lp/midi/Chord/Amin/<shard>/) is found through a bounded walk.
  Sets& s = sets();
  TrackGenInfo chordSaved = gen().info(Chord); Pattern chordSrc = eng().sourcePattern(Chord);
  const std::string original = locate("/lp/midi/Chord/" + labelOf(chordSaved.file), chordSaved.file);   // its key folder, flat or sharded
  CHECK(exists(original));
  const std::string shardName = "Amin__Chord__SetsShardCopy__00000001.mid";
  CHECK(sd().mkdir(kShardDir.c_str()));
  writeText(kShardDir + "/" + shardName, readText(original));
  note(kShardDir); note(kShardDir + "/" + shardName);              // removed in reverse order: the file before its folder
  CHECK(gen().loadMidiToSource(Chord, (kShardDir + "/" + shardName).c_str()));
  CHECK(std::string(gen().info(Chord).file) == shardName);
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  std::string n = name; notePreset(n);
  std::string text = readText(kPresets + "/" + n);
  CHECK(valueOf(text, "lp.src.0") == kShardDir + "/" + shardName);   // the shard path itself (no copy)
  CHECK(!exists(smfOf(kPresets, n, Chord)));
  // a source that exists nowhere is not dropped quietly: the file is written without that track, ok() is false, lastError names it
  strcpy(gen().info(Chord).file, "Amin__Chord__Vanished__deadbeef.mid");
  std::vector<std::string> before = listDir(kPresets, ".lpset");
  CHECK(!saveSync(0, name, sizeof name));
  CHECK(std::string(s.lastError()) == "NO SRC CHD");
  std::string partial = name; CHECK(!partial.empty()); notePreset(partial);
  CHECK(exists(kPresets + "/" + partial));
  CHECK_EQ((int)listDir(kPresets, ".lpset").size(), (int)before.size() + 1);
  std::string pt = readText(kPresets + "/" + partial);
  CHECK(valueOf(pt, "lp.src.0").empty()); CHECK(!valueOf(pt, "lp.track.0.seed").empty());   // the track's params stay, its source line is absent
  CHECK(!valueOf(pt, "lp.src.1").empty());                        // the other tracks are complete
  removePreset(partial);                                           // keep the random loads below on complete sets
  gen().info(Chord) = chordSaved; eng().sourcePattern(Chord) = chordSrc;
}

// ---------------------------------------------------------------------------------------------- load

static void scramble() {                                           // make the current state differ from every saved file
  for (int t = 0; t < kTracksPerSet; t++) {
    TrackGenInfo& i = gen().info(t); strcpy(i.file, "scrambled"); i.seed = 0xDEADBEEF; i.variant = 9; i.presetIndex = 99; i.keyRoot = 3; i.lenStep = 2; i.userLoop = 1; i.edited = 1; i.pause = 4;
    Cmd c; c.type = CmdSetParam; c.track = (uint8_t)t; c.a = PVolume; c.f = 0.1f; eng().cmds().push(c);
  }
  for (int k = 0; k < kSampleSlots; k++) eng().setKitName(0, k, "gone.wav");
  Cmd m; m.type = CmdBeatMask; m.a = 0; m.i = 0x01; eng().cmds().push(m);   // Kick layer (variant 0) at the bar
  pump(1);
}

static void checkLoaded(const std::string& name) {
  std::string text = readText(kPresets + "/" + name);
  SetFile f; parseSetFileText(text.c_str(), (int)text.size(), f);
  for (int t = 0; t < kTracksPerSet; t++) {
    const SetFileTrack& ft = f.track[t]; const TrackGenInfo& i = gen().info(t); const TrackStatus& st = eng().status().track[t];
    const std::string src = f.src[t];                                // absolute (SPEC §9.29) or a legacy clip relative to the preset folder
    if (src.empty() || src == "acid") continue;
    CHECK(baseOf(src) == i.file);
    CHECK_EQ((long long)i.seed, (long long)ft.seed);
    CHECK_EQ((int)i.variant, (int)ft.variant);
    CHECK_EQ((int)st.variant, (int)ft.variant);
    if (t == Vocal) {                                                // SPEC §9.16: the clip is the window + pause cycle, the pause restored (default 1)
      CHECK_EQ((int)i.pause, (int)ft.pause); CHECK_EQ((int)st.lenTicks, (int)Generator::vocalClipTicks(ft.variant, ft.pause));
      CHECK_EQ((int)i.naturalTicks, (int)(ft.lenBeats * kTicksPerBeat + 0.5f));
    } else {
      if ((int)st.lenTicks != (int)(ft.lenBeats * kTicksPerBeat + 0.5f)) std::printf("  %s track %d: engine len %d variant %d; file len %.2f beats lenk %d slot %d; info natural %d lenStep %d variant %d\n", name.c_str(), t, (int)st.lenTicks, (int)st.variant, (double)ft.lenBeats, (int)ft.lenk, (int)ft.variant, (int)i.naturalTicks, (int)i.lenStep, (int)i.variant);
      CHECK_EQ((int)st.lenTicks, (int)(ft.lenBeats * kTicksPerBeat + 0.5f));
    }
    CHECK_EQ((int)i.keyRoot, (int)ft.keyRoot); CHECK_EQ((int)i.lenStep, (int)ft.lenk); CHECK_EQ((int)i.userLoop, 0); CHECK_EQ((int)i.loaded, 1);
    const bool ownSmf = src.compare(0, 9, "/lp/midi/") != 0 && src.find("/presets/assets/") == std::string::npos && t != Vocal;   // SPEC §9.29: the preset's own _t<i>.mid is this device's edit; the library and a legacy asset are referenced
    CHECK_EQ((int)i.edited, ownSmf ? 1 : 0);
    if (isSynthCategory((Category)t)) { CHECK_EQ((int)i.presetIndex, (int)ft.preset); CHECK(i.sound[0] != 0); }
    if (t != Vocal && t != Beat) CHECK(eng().activePattern(t).count > 0);
    CHECK((int)st.state != (int)PSEmpty);
    CHECK_EQ((int)st.pendingSwap, 0);
  }
  for (int k = 0; k < kSampleSlots; k++) { CHECK(f.kit[k][0] != 0); CHECK(baseOf(f.kit[k]) == eng().kit(0).name[k]); CHECK(eng().kit(0).slot[k].present == 1); }
  CHECK(std::string(gen().info(Beat).sound) == std::string(eng().kit(0).name[0]).substr(0, sizeof(TrackGenInfo::sound) - 1));
  CHECK_EQ((int)eng().status().track[Beat].kickCut, 0);
  CHECK_NEAR(eng().params(0).volume, 0.8f, 1e-4);                  // the file's value (SPEC §9.17b: restored, not reset; every saved set here keeps track 0 at 0.8)
  CHECK_EQ((int)eng().status().generating[0], 0);
}

TEST(load_restores_the_saved_state) {
  Sets& s = sets();
  scramble();
  Rng rng(77);
  CHECK(s.beginLoad(0, rng));
  CHECK(s.loading());
  settle();
  CHECK(!s.busy()); CHECK(s.ok()); if (!s.ok()) std::printf("  error: %s\n", s.lastError());
  std::string name = s.lastName();
  std::printf("  loaded %s (max step %.2f ms)\n", name.c_str(), g_maxStepMs);
  if (!g_presetsExisted) CHECK(name.compare(0, 11, "House_Amin_") == 0);   // only our files when the folder is ours
  checkLoaded(name);
  // a loaded set can be saved again: the assets are the sources, nothing is copied twice, no edit asset is
  // written although every track was flagged edited before the load
  for (int t = 0; t < kTracksPerSet; t++) std::printf("  after load: track %d file=%s edited=%d loaded=%d lenStep=%d sound=%s\n", t, gen().info(t).file, gen().info(t).edited, gen().info(t).loaded, gen().info(t).lenStep, gen().info(t).sound);
  std::string t1 = readText(kPresets + "/" + name);
  SetFile f1; parseSetFileText(t1.c_str(), (int)t1.size(), f1);
  int none = 0;
  for (int t = 0; t < kTracksPerSet; t++) if (!f1.src[t][0]) { none++; gen().info(t) = TrackGenInfo(); eng().sourcePattern(t).clear(); }   // untouched by the load: make them empty
  CHECK_EQ(none, 0);                                                // every track of a freshly generated set has a source
  const int filesBefore = (int)listDir(kPresets, nullptr).size();
  char again[64] = ""; CHECK(saveSync(0, again, sizeof again)); notePreset(again);
  std::string t2 = readText(kPresets + "/" + again);
  int ownSmfs = 0;
  for (int t = 0; t < kTracksPerSet; t++) {
    // SPEC §9.29: a library file (or a legacy assets/ copy) is referenced again by its path; a pattern that came from
    // another preset's _t<i>.mid gets this preset's own SMF (a delete of that preset must not break this one)
    const std::string s1 = f1.src[t][0] == '/' ? std::string(f1.src[t]) : kPresets + "/" + f1.src[t], s2 = valueOf(t2, "lp.src." + std::to_string(t));
    CHECK(!s2.empty()); CHECK(exists(s2));
    const bool shared = s1.compare(0, 9, "/lp/midi/") == 0 || s1.find("/presets/assets/") != std::string::npos || t == Vocal;
    if (shared) { if (s1 != s2) std::printf("  source differs on track %d: %s vs %s\n", t, s1.c_str(), s2.c_str()); CHECK(s1 == s2); }
    else { CHECK(s2 == smfOf(kPresets, again, t)); ownSmfs++; }
  }
  CHECK_EQ((int)listDir(kPresets, nullptr).size(), filesBefore + 1 + ownSmfs);
  CHECK(exists(kPresets + "/assets") == g_assetsExisted);
  removePreset(again);
}

// SPEC §9.17b: strip params, synth knobs and the acid flag round-trip through a preset; a load restores them (no
// reset) with CmdSetParam after the sound. Track 0 keeps its default volume: checkLoaded() asserts it for every load.
static bool engineAppliesKnobs() {
  static int known = -1;
  if (known < 0) {
    const float before = eng().params(kTracksPerSet + Pad).cutoff;
    push(CmdSetParam, kTracksPerSet + Pad, PCutoff, 0, 0.3125f); pump(2);
    known = std::fabs(eng().params(kTracksPerSet + Pad).cutoff - 0.3125f) < 1e-5f ? 1 : 0;
    push(CmdSetParam, kTracksPerSet + Pad, PCutoff, 0, before); pump(2);
  }
  return known == 1;
}
static std::set<std::string> presetNames() { std::set<std::string> v; for (auto& f : listDir(kPresets, ".lpset")) v.insert(f); return v; }

TEST(params_knobs_and_acid_round_trip_through_a_preset) {
  Sets& s = sets();
  const std::set<std::string> listing = presetNames();
  push(CmdSetParam, Pad, PVolume, 0, 0.55f); push(CmdSetParam, Pad, PEcho, 0, 0.25f); push(CmdSetParam, Pad, PFilter, 0, -0.4f);
  push(CmdSetParam, Lead, PCutoff, 0, 0.33f); push(CmdSetParam, Lead, PResonance, 0, 0.77f); push(CmdSetParam, Lead, PWave, WSquare, (float)WSquare);
  gen().info(Lead).acid = 1;                                        // the flag alone (no re-roll): what the file carries
  pump(2);
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  const std::string mine = name; notePreset(mine);
  std::string text = readText(kPresets + "/" + mine);
  CHECK(valueOf(text, "lp.track.2.vol") == "0.550"); CHECK(valueOf(text, "lp.track.2.echo") == "0.250"); CHECK(valueOf(text, "lp.track.2.filter") == "-0.400");
  CHECK(valueOf(text, "lp.track.1.acid") == "1"); CHECK(valueOf(text, "lp.track.0.acid") == "0");
  const bool knobs = engineAppliesKnobs();
  if (knobs) { CHECK(valueOf(text, "lp.track.1.cutoff") == "0.330"); CHECK(valueOf(text, "lp.track.1.res") == "0.770"); CHECK(valueOf(text, "lp.track.1.wave") == std::to_string((int)WSquare)); }
  else std::printf("  (engine does not apply the knob ParamIds yet: knobs written from its defaults)\n");
  // SPEC §9.22: the 303 character and the chorus send travel like the other knobs (written from the engine's targets)
  CHECK(!valueOf(text, "lp.track.1.sat").empty()); CHECK(!valueOf(text, "lp.track.1.accent").empty()); CHECK(!valueOf(text, "lp.track.1.chorus").empty());
  CHECK(valueOf(text, "lp.track.7.sat").empty());                  // no knobs on the drums
  SetFile back; parseSetFileText(text.c_str(), (int)text.size(), back);
  CHECK((back.track[1].has & SetFileTrack::HasSat) != 0); CHECK((back.track[1].has & SetFileTrack::HasAccent) != 0); CHECK((back.track[1].has & SetFileTrack::HasChorus) != 0);
  CHECK_NEAR(back.track[2].vol, 0.55f, 1e-3); CHECK_NEAR(back.track[2].echo, 0.25f, 1e-3); CHECK_NEAR(back.track[2].filter, -0.4f, 1e-3);
  CHECK_EQ((int)back.track[1].acid, 1); CHECK((back.track[1].has & SetFileTrack::HasAcid) != 0); CHECK((back.track[2].has & SetFileTrack::HasVol) != 0);
  CHECK_EQ((int)(back.track[7].has & SetFileTrack::HasCutoff), 0);   // no knobs on the drums
  if (knobs) { CHECK_NEAR(back.track[1].cutoff, 0.33f, 1e-3); CHECK_NEAR(back.track[1].res, 0.77f, 1e-3); CHECK_EQ((int)back.track[1].wave, (int)WSquare); }
  // load this very file (random among the key's sets: retry until it is the one), scrambled first
  bool loadedMine = false;
  for (int attempt = 0; attempt < 12 && !loadedMine; attempt++) {
    scramble(); gen().info(Lead).acid = 0;
    push(CmdSetParam, Pad, PVolume, 0, 0.1f); push(CmdSetParam, Lead, PCutoff, 0, 0.9f); pump(2);
    Rng rng(500 + attempt); char got[64] = "";
    CHECK(s.loadRandom(0, rng, got, sizeof got)); settle();
    CHECK(s.ok());
    loadedMine = mine == got;
  }
  CHECK(loadedMine);
  if (loadedMine) {
    CHECK_NEAR(eng().params(Pad).volume, 0.55f, 2e-3); CHECK_NEAR(eng().params(Pad).echo, 0.25f, 2e-3); CHECK_NEAR(eng().params(Pad).filter, -0.4f, 2e-3);
    CHECK_NEAR(eng().params(Chord).volume, 0.8f, 1e-4);              // the file's value, not a reset
    CHECK_EQ((int)gen().info(Lead).acid, 1); CHECK_EQ((int)gen().info(Chord).acid, 0);
    if (knobs) { CHECK_NEAR(eng().params(Lead).cutoff, 0.33f, 2e-3); CHECK_NEAR(eng().params(Lead).resonance, 0.77f, 2e-3); CHECK_EQ((int)eng().params(Lead).wave, (int)WSquare); }
    checkLoaded(mine);
  }
  // a file without the §9.17b keys resets the track instead (the pre-§9.17b rule): strip the keys from a copy
  std::string old;                                                  // every lp.track.2.<param> line dropped
  for (size_t p = 0; p < text.size();) {
    size_t e = text.find('\n', p); if (e == std::string::npos) e = text.size();
    const std::string line = text.substr(p, e - p);
    static const char* const kParamKeys[] = {".vol=", ".dist=", ".filter=", ".echo=", ".reverb=", ".cutoff=", ".res=", ".env=", ".decay=", ".drive=", ".wave=", ".acid=", ".sat=", ".accent=", ".chorus="};
    bool drop = line.compare(0, 11, "lp.track.2.") == 0;
    if (drop) { drop = false; for (const char* k : kParamKeys) if (line.find(k) != std::string::npos) drop = true; }
    if (!drop) old += line + "\n";
    p = e + 1;
  }
  SetFile stripped; parseSetFileText(old.c_str(), (int)old.size(), stripped);
  CHECK_EQ((int)stripped.track[2].has, 0); CHECK((stripped.track[1].has & SetFileTrack::HasVol) != 0);
  // the listing only grew by this file: nothing was deleted by the save or the loads
  std::set<std::string> after = presetNames(); after.erase(mine);
  CHECK(after == listing);
  push(CmdSetParam, Pad, PVolume, 0, 0.8f); push(CmdSetParam, Pad, PEcho, 0, 0.0f); push(CmdSetParam, Pad, PFilter, 0, 0.0f);
  push(CmdSetParam, Lead, PCutoff, 0, 0.5f); push(CmdSetParam, Lead, PResonance, 0, 0.2f); push(CmdSetParam, Lead, PWave, WSaw, (float)WSaw); pump(2);
  gen().info(Lead).acid = 0;
  removePreset(mine);                                               // not a candidate for the later random loads
}

// SPEC §9.22b index compatibility: before the generated bank the '303' family sat right after the hand library rows
// (Chord [10, 18), Lead / Pad / Bass [9, 17)); a file without the lp.banks line (written since the move, =2) predates
// it, so the loader (sets.cpp presetForLayout) maps such an index onto the same '303' row at [libraryPresetCount, +8).
// Library indices never moved, Fx has no '303' family, and a file carrying lp.banks=2 passes through untouched.
TEST(load_remaps_the_303_rows_of_a_file_saved_before_the_generated_bank) {
  Sets& s = sets();
  settle();
  const std::string head = "schemaVersion=1\nsetIndex=0\nkeyRoot=21\nkeyName=A minor\ngenre=House\n";
  const std::string body = "lp.track.0.preset=10\nlp.track.0.seed=11\nlp.track.1.preset=9\nlp.track.1.seed=12\nlp.track.1.acid=1\n"
                           "lp.track.2.preset=16\nlp.track.2.seed=13\nlp.track.3.preset=3\nlp.track.3.seed=14\nlp.track.4.preset=9\nlp.track.4.seed=15\n";
  const std::string legacy = "zz_legacy_303.lpset", current = "zz_current_303.lpset";
  writeText(kPresets + "/" + legacy, head + body); note(kPresets + "/" + legacy);
  writeText(kPresets + "/" + current, head + "lp.banks=2\n" + body); note(kPresets + "/" + current);
  auto loadNamed = [&](const std::string& name) {
    for (int t = 0; t < Fx + 1; t++) { TrackGenInfo& i = gen().info(t); i.presetIndex = 99; i.sound[0] = 0; i.acid = 0; }   // the synth tracks differ from every file; the drums and the vocal keep their sources (a save needs the vocal's)
    Rng rng(9); CHECK(s.beginLoadNamed(0, name.c_str(), rng)); settle();
    CHECK(s.ok()); if (!s.ok()) std::printf("  %s: %s\n", name.c_str(), s.lastError());
    CHECK(std::string(s.lastName()) == name);
  };
  const int lc = libraryPresetCount(Chord), ll = libraryPresetCount(Lead), lpd = libraryPresetCount(Pad);
  loadNamed(legacy);
  CHECK_EQ((int)gen().info(Chord).presetIndex, lc); CHECK(isAcidPreset(Chord, gen().info(Chord).presetIndex)); CHECK(std::string(gen().info(Chord).sound) == "303 Stab");
  CHECK_EQ((int)gen().info(Lead).presetIndex, ll); CHECK(isAcidPreset(Lead, gen().info(Lead).presetIndex)); CHECK(std::string(gen().info(Lead).sound) == "303 Saw");
  CHECK_EQ((int)gen().info(Lead).acid, 1);                                                             // the §9.17b flag travels as before
  CHECK(std::string(eng().nextPatch(Lead).name) == "303 Saw");                                         // the patch handed to the engine is that row (+ the seed spread)
  CHECK_EQ((int)gen().info(Pad).presetIndex, lpd + 7); CHECK(std::string(gen().info(Pad).sound) == "303 Dirty");   // the last old row is the last hand '303' row
  CHECK_EQ((int)gen().info(Bass).presetIndex, 3); CHECK(!isAcidPreset(Bass, 3)); CHECK(std::string(gen().info(Bass).sound) == preset(Bass, 3).name);   // a library index never moved
  CHECK_EQ((int)gen().info(Fx).presetIndex, 9); CHECK(std::string(gen().info(Fx).sound) == preset(Fx, 9).name);   // Fx: no '303' family, nothing to remap
  std::printf("  legacy Chord 10 -> %d '%s', Lead 9 -> %d '%s', Pad 16 -> %d '%s', Bass 3 '%s'\n", lc, gen().info(Chord).sound, ll, gen().info(Lead).sound, lpd + 7, gen().info(Pad).sound, gen().info(Bass).sound);
  // a save after the load writes the new index under lp.banks=2: the file is migrated
  char name[64] = ""; CHECK(saveSync(0, name, sizeof name)); if (!s.ok()) std::printf("  save after the legacy load: %s\n", s.lastError()); notePreset(name);
  const std::string text = readText(kPresets + "/" + name);
  CHECK(valueOf(text, "lp.banks") == "2");
  CHECK(valueOf(text, "lp.track.0.preset") == std::to_string(lc)); CHECK(valueOf(text, "lp.track.1.preset") == std::to_string(ll));
  CHECK(valueOf(text, "lp.track.2.preset") == std::to_string(lpd + 7)); CHECK(valueOf(text, "lp.track.3.preset") == "3");
  CHECK(!valueOf(text, "lp.track.1.sat").empty()); CHECK(valueOf(text, "lp.track.1.acid") == "1");
  removePreset(name);
  // the same indices in a file written since the move (lp.banks=2) are taken as they are
  loadNamed(current);
  CHECK_EQ((int)gen().info(Chord).presetIndex, 10); CHECK(!isAcidPreset(Chord, 10)); CHECK(std::string(gen().info(Chord).sound) == preset(Chord, 10).name);
  CHECK_EQ((int)gen().info(Lead).presetIndex, 9); CHECK(!isAcidPreset(Lead, 9)); CHECK(std::string(gen().info(Lead).sound) == preset(Lead, 9).name);
  CHECK_EQ((int)gen().info(Lead).acid, 1);                                                             // the flag only: the sound is the file's preset (SPEC §9.17b)
  CHECK_EQ((int)gen().info(Pad).presetIndex, 16); CHECK(!isAcidPreset(Pad, 16));
  CHECK_EQ((int)gen().info(Bass).presetIndex, 3); CHECK_EQ((int)gen().info(Fx).presetIndex, 9);
  gen().info(Lead).acid = 0;
  removePreset(legacy); removePreset(current);
}

TEST(load_is_one_set_wide_swap) {
  // 240 BPM with two engine blocks per loop step: the load spans bar lines (the review's reproduction). Every
  // track must adopt its pattern in the same block, on a bar line (SPEC §9.5, §9.8 "one set-wide swap").
  Sets& s = sets(); static int16_t buf[kBlock];
  push(CmdSetTempo, 0, 0, 240); pump(2);
  CHECK_EQ((int)eng().status().bpm, 240);
  while ((int32_t)(eng().status().nextBarSample - eng().status().sampleClock) > 6 * kBlock) pump(1, 1);   // a few blocks before a bar line
  scramble();
  Rng rng(21); char name[64] = "";
  CHECK(s.loadRandom(0, rng, name, sizeof name)); if (!s.busy()) std::printf("  error: %s\n", s.lastError());
  int adopt[kTracksPerSet]; bool seen[kTracksPerSet]; for (int t = 0; t < kTracksPerSet; t++) { adopt[t] = -1; seen[t] = false; }
  int block = 0; uint32_t adoptTick = 0;
  while ((s.busy() || pendingSwaps(0)) && block < 6000) {
    gen().step(); gen().pumpVocal(); stepSets();
    for (int r = 0; r < 2; r++) {
      eng().render(buf, kBlock); block++;
      for (int t = 0; t < kTracksPerSet; t++) {
        if (eng().status().track[t].pendingSwap) seen[t] = true;
        else if (seen[t] && adopt[t] < 0) { adopt[t] = block; adoptTick = eng().status().tick; }
      }
    }
  }
  CHECK(!s.busy()); CHECK(s.ok());
  std::printf("  %s: adoption blocks", name);
  for (int t = 0; t < kTracksPerSet; t++) std::printf(" %d", adopt[t]);
  std::printf(" (tick %u, bar %u)\n", (unsigned)adoptTick, (unsigned)(adoptTick / kTicksPerBar));
  for (int t = 0; t < kTracksPerSet; t++) { CHECK(adopt[t] > 0); CHECK_EQ(adopt[t], adopt[0]); }
  CHECK((adoptTick % kTicksPerBar) < 8);                            // the block that crossed the bar line (≈ 4.5 ticks per block at 240)
  checkLoaded(name);
  push(CmdSetTempo, 0, 0, 120); pump(2);
}

TEST(load_completes_with_transport_stopped) {
  Sets& s = sets();
  push(CmdMasterPlay, 0, 0); pump(2);
  CHECK_EQ((int)eng().status().running, 0);
  scramble();
  Rng rng(33); char name[64] = "";
  CHECK(s.loadRandom(0, rng, name, sizeof name));
  int steps = 0; while (s.busy() && steps < 3000) { pump(1); steps++; }
  std::printf("  stopped transport: %s done after %d steps\n", name, steps);
  CHECK(!s.busy()); CHECK(s.ok());                                  // no bar line while stopped: the loader must not wait for one
  CHECK_EQ((int)eng().status().generating[0], 0);
  int pending = 0; for (int t = 0; t < kTracksPerSet; t++) if (eng().status().track[t].pendingSwap) pending++;
  CHECK(pending >= 7);                                              // the swaps wait for the resume
  CHECK(!s.busy()); CHECK(s.beginSave(0)); settle(); CHECK(s.ok()); if (!s.ok()) std::printf("  save error: %s\n", s.lastError());
  removePreset(s.lastName());                                      // not stuck: a later save runs
  push(CmdMasterPlay, 0, 1); pump(2);
  CHECK_EQ((int)eng().status().running, 1);
  settle();
  CHECK(!pendingSwaps(0));
  checkLoaded(name);
}

// A set file carries the generator's INTENT: a variant or len± pressed while the transport is stopped (or inside the
// ≤ 1-bar wait for the swap) is written as pressed, not as the engine's still-active pattern (what a restore rebuilds).
TEST(save_writes_the_generators_intent_not_the_active_pattern) {
  settle();
  push(CmdMasterPlay, 0, 0); pump(2);
  CHECK_EQ((int)eng().status().running, 0);
  const int chordWas = eng().status().track[Chord].variant, chordNow = (chordWas + 2) & 3;
  const uint16_t leadLen = eng().status().track[Lead].lenTicks;
  gen().rebuildVariant(Chord, chordNow);                           // swap queued, no bar line while stopped
  const uint16_t leadWant = gen().setLengthStep(Lead, +1);         // CmdSetLength queued likewise
  pump(2);
  CHECK_EQ((int)eng().status().track[Chord].variant, chordWas); CHECK_EQ((int)eng().status().track[Lead].lenTicks, (int)leadLen);   // the engine still plays the old state
  CHECK_EQ((int)gen().info(Chord).variant, chordNow); CHECK_EQ((int)gen().info(Lead).lenStep, 1);
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name)); const std::string n = name; notePreset(n);
  const std::string text = readText(kPresets + "/" + n);
  CHECK(valueOf(text, "lp.track.0.variant") == std::to_string(chordNow));
  CHECK(valueOf(text, "lp.track.1.lenk") == "1");
  CHECK(valueOf(text, "lp.track.1.len") == std::to_string(leadWant / kTicksPerBeat));
  CHECK(valueOf(text, "lp.track.0.lenk") == "0");
  push(CmdMasterPlay, 0, 1); pump(2); settle();
  CHECK_EQ((int)eng().status().track[Chord].variant, chordNow); CHECK_EQ((int)eng().status().track[Lead].lenTicks, (int)leadWant);
  gen().rebuildVariant(Chord, chordWas); gen().setLengthStep(Lead, -1); pump(2); settle();   // (pump first: settle() looks at the engine's pendingSwap, set once the command is drained)
  removePreset(n);
}

// SPEC §9.8 / §9.17b: a load restores the knobs with the sound (right after CmdSetPatch) but the strip only once the set
// has swapped, so for the bar the old patterns still play the mix stays theirs. A file from before §9.17b (no strip /
// knob keys) resets the track at the swap, as before.
TEST(load_applies_the_strip_at_the_swap) {
  Sets& s = sets();
  pump(2); settle();
  // a copy of a saved set without track 2's §9.17b keys, listed alone
  std::vector<std::string> files = suitePresets(); CHECK(!files.empty());
  std::string base; for (auto& f : files) if (f.compare(0, 11, "House_Amin_") == 0 && base.empty()) base = f;
  CHECK(!base.empty());
  const std::string src = readText(kPresets + "/" + base);
  std::string old;
  for (size_t p = 0; p < src.size();) {
    size_t e = src.find('\n', p); if (e == std::string::npos) e = src.size();
    const std::string line = src.substr(p, e - p);
    static const char* const kParamKeys[] = {".vol=", ".dist=", ".filter=", ".echo=", ".reverb=", ".cutoff=", ".res=", ".env=", ".decay=", ".drive=", ".wave=", ".acid=", ".sat=", ".accent=", ".chorus="};
    bool drop = line.compare(0, 11, "lp.track.2.") == 0;
    if (drop) { drop = false; for (const char* k : kParamKeys) if (line.find(k) != std::string::npos) drop = true; }
    if (!drop) old += line + "\n";
    p = e + 1;
  }
  const std::string stripped = "House_Amin_980.lpset";
  writeText(kPresets + "/" + stripped, old); note(kPresets + "/" + stripped);
  sd().remove(kIndex.c_str()); writeText(kIndex, stripped + "\n");
  CHECK_EQ((int)eng().status().running, 1);
  const std::string fileCut = valueOf(src, "lp.track.1.cutoff");     // the Lead's saved knob; the probe value sits far from it
  const float cutFile = fileCut.empty() ? 0.5f : strtof(fileCut.c_str(), nullptr), probe = cutFile > 0.5f ? 0.1f : 0.9f;
  scramble();                                                        // volume 0.1 on every track
  push(CmdSetParam, Lead, PCutoff, 0, probe); pump(2);
  CHECK_NEAR(eng().params(Chord).volume, 0.1f, 2e-3); CHECK_NEAR(eng().params(Pad).volume, 0.1f, 2e-3); CHECK_NEAR(eng().params(Lead).cutoff, probe, 2e-3);
  Rng rng(61); char name[64] = "";
  CHECK(!pendingSwaps(0));
  CHECK(s.loadRandom(0, rng, name, sizeof name)); CHECK(std::string(name) == stripped);
  int guard = 0; while (s.busy() && !pendingSwaps(0) && guard++ < 4000) pump(1);
  std::printf("  swaps pending after %d steps, %d tracks loaded; pending:", guard, s.progress());
  for (int t = 0; t < kTracksPerSet; t++) std::printf(" %d", (int)eng().status().track[t].pendingSwap);
  std::printf("\n");
  CHECK(s.busy()); CHECK(pendingSwaps(0));                           // the swaps wait for the bar line ...
  CHECK_NEAR(eng().params(Chord).volume, 0.1f, 2e-3); CHECK_NEAR(eng().params(Pad).volume, 0.1f, 2e-3);   // ... and the strip with them
  std::printf("  %s: Lead cutoff %s in the file, %.3f while the swap is pending\n", stripped.c_str(), fileCut.empty() ? "absent" : fileCut.c_str(), (double)eng().params(Lead).cutoff);
  if (!fileCut.empty()) CHECK_NEAR(eng().params(Lead).cutoff, cutFile, 2e-3);   // the knobs went out with the sound, a bar ahead of the strip
  settle(); CHECK(s.ok());
  CHECK_NEAR(eng().params(Chord).volume, 0.8f, 2e-3);               // the file's strip, applied at the swap
  CHECK_NEAR(eng().params(Pad).volume, 0.8f, 2e-3);                 // no keys for the Pad: CmdResetTrack at the swap (the old rule)
  rewriteIndex(); removePreset(stripped);
}

TEST(load_restores_a_vocal_saved_on_variant_1) {
  // Vocal variant 2 (slot 1) is "the alternate file": a set carries one clip, so after a load both file slots
  // must be that clip, whatever another generate left in the alternate slot (SPEC §9.3, §9.8).
  Sets& s = sets();
  CHECK(gen().info(Vocal).file[0] != 0);
  gen().rebuildVariant(Vocal, 1);
  for (int i = 0; i < 2000 && (gen().vocalSwitchPending(0) || eng().status().track[Vocal].variant != 1 || pendingSwaps(0)); i++) pump(1);
  CHECK_EQ((int)eng().status().track[Vocal].variant, 1);
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  std::string n = name; notePreset(n);
  std::string text = readText(kPresets + "/" + n);
  CHECK(hasLine(text, "lp.track.6.variant=1"));
  const std::string phrase = valueOf(text, "lp.src.6"); CHECK(phrase == gen().vocalPath(0, 1)); CHECK(exists(phrase));   // the alternate slot's file, by its path
  const std::string saved = baseOf(phrase);
  // re-roll the vocal sound: the generator's two file slots now hold other phrases
  CHECK(gen().generateSound(Vocal)); settle();
  CHECK(std::string(gen().info(Vocal).file) != saved || std::string(gen().vocalPath(0, 1)) != phrase);
  // load that very file
  sd().remove(kIndex.c_str()); writeText(kIndex, n + "\n");          // only this set is listed
  scramble();
  Rng rng(8); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == n);
  CHECK(std::string(gen().info(Vocal).file) == saved);
  CHECK_EQ((int)eng().status().track[Vocal].variant, 1);
  CHECK_EQ((int)gen().info(Vocal).variant, 1);
  CHECK(std::string(gen().vocalPath(0, 0)) == phrase);
  CHECK(std::string(gen().vocalPath(0, 1)) == phrase);
  checkLoaded(n);
  rewriteIndex();                                                  // restore the full listing
}

// SPEC §9.16: the vocal's pause (len±) is written as lp.track.6.pause and restored by a load; lp.track.6.len keeps
// Reaper's loop length and lenk stays 0, so a set saved with a 2-bar pause plays a 4-bar cycle again after the load.
TEST(vocal_pause_round_trips_through_a_set_file) {
  Sets& s = sets();
  CHECK(gen().info(Vocal).file[0] != 0);
  CHECK_EQ((int)gen().info(Vocal).pause, 1);
  CHECK_EQ((int)gen().setLengthStep(Vocal, +1), 4 * kTicksPerBar);        // pause 2: a 4-bar cycle
  for (int i = 0; i < 2000 && (gen().vocalSwitchPending(0) || eng().status().track[Vocal].lenTicks != 4 * kTicksPerBar || pendingSwaps(0)); i++) pump(1);
  CHECK_EQ((int)eng().status().track[Vocal].lenTicks, 4 * kTicksPerBar);
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  std::string n = name; notePreset(n);
  std::string text = readText(kPresets + "/" + n);
  CHECK(hasLine(text, "lp.track.6.pause=2")); CHECK(hasLine(text, "lp.track.6.lenk=0"));
  CHECK(valueOf(text, "lp.track.6.len") == std::to_string(gen().info(Vocal).naturalTicks / kTicksPerBeat));
  CHECK(valueOf(text, "lp.track.0.pause").empty());
  CHECK(exists(valueOf(text, "lp.src.6")));
  CHECK_EQ((int)s.file().track[Vocal].pause, 2);
  // a different pause before the load (its switch applied at a bar line first: a switch still pending would be replaced
  // by the load's own, like any len± press during a load), then only this set is listed
  CHECK_EQ((int)gen().setLengthStep(Vocal, -1), 3 * kTicksPerBar);
  for (int i = 0; i < 2000 && (gen().vocalSwitchPending(0) || eng().status().track[Vocal].lenTicks != 3 * kTicksPerBar || pendingSwaps(0)); i++) pump(1);
  CHECK_EQ((int)eng().status().track[Vocal].lenTicks, 3 * kTicksPerBar);
  sd().remove(kIndex.c_str()); writeText(kIndex, n + "\n");
  scramble();                                                        // pause 4 on every track
  Rng rng(9); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == n);
  CHECK_EQ((int)s.file().track[Vocal].pause, 2); CHECK_EQ((int)s.file().track[Vocal].hasPause, 1);
  CHECK_EQ((int)gen().info(Vocal).pause, 2); CHECK_EQ((int)gen().info(Vocal).lenStep, 0);
  CHECK_EQ((int)eng().status().track[Vocal].lenTicks, 4 * kTicksPerBar);
  CHECK_EQ((int)gen().vocalLoopTicks(0), 4 * kTicksPerBar);
  checkLoaded(n);
  CHECK_EQ((int)gen().setLengthStep(Vocal, -1), 3 * kTicksPerBar);        // back to the default for the tests that follow
  for (int i = 0; i < 2000 && (gen().vocalSwitchPending(0) || eng().status().track[Vocal].lenTicks != 3 * kTicksPerBar || pendingSwaps(0)); i++) pump(1);
  CHECK_EQ((int)gen().info(Vocal).pause, 1);
  rewriteIndex();                                                    // restore the full listing
}

TEST(load_and_save_refuse_while_the_generator_is_busy) {
  Sets& s = sets();
  CHECK(gen().generatePattern(Lead));                              // queued: busy() includes the queue
  CHECK(gen().busy());
  Rng rng(4); char name[64] = "x";
  CHECK(!s.beginLoad(0, rng)); CHECK(std::string(s.lastError()) == "GEN BUSY"); CHECK(!s.busy());
  CHECK(!s.beginSave(0)); CHECK(std::string(s.lastError()) == "GEN BUSY");
  CHECK(!s.loadRandom(0, rng, name, sizeof name)); CHECK(name[0] == 0);
  settle();
  CHECK(!gen().busy());
  CHECK(s.beginSave(0));                                           // (the UI refuses GENERATE while sets are busy)
  settle(); CHECK(s.ok()); if (!s.ok()) std::printf("  save error: %s\n", s.lastError());
  removePreset(s.lastName());
}

TEST(load_never_repeats_the_last_name) {
  Sets& s = sets();
  Rng rng(5);
  std::string last = s.lastName(); int loads = 0;
  for (int n = 0; n < 6; n++) {
    char name[64] = "";
    CHECK(s.loadRandom(0, rng, name, sizeof name));                // returns once a set is chosen; step() applies it
    CHECK(s.busy());
    CHECK(std::string(name) == s.lastName());
    settle();
    CHECK(s.ok());
    CHECK(std::string(name) != last);
    checkLoaded(name);
    last = name; loads++;
  }
  CHECK_EQ(loads, 6);
}

TEST(load_bpm_only_when_nothing_plays) {
  Sets& s = sets(); Rng rng(9);
  Cmd c; c.type = CmdSetTempo; c.i = 100; eng().cmds().push(c); pump(2);
  CHECK_EQ((int)eng().status().bpm, 100);
  CHECK(s.loadRandom(0, rng, nullptr, 0)); settle();
  CHECK_EQ((int)eng().status().bpm, 120);                          // the files carry lp.bpm=120, nothing plays
  Cmd st; st.type = CmdTrackStartNow; st.track = Beat; eng().cmds().push(st);
  c.i = 100; eng().cmds().push(c); pump(2);
  CHECK_EQ((int)eng().status().track[Beat].state, (int)PSPlaying);
  CHECK(s.loadRandom(0, rng, nullptr, 0)); settle();
  CHECK_EQ((int)eng().status().bpm, 100);                          // a track plays: tempo untouched
  CHECK_EQ((int)eng().status().track[Beat].state, (int)PSPlaying);   // play state unchanged by the load (SPEC §9.8)
  Cmd stop; stop.type = CmdTrackStopNow; stop.track = Beat; eng().cmds().push(stop);
  c.i = 120; eng().cmds().push(c); pump(2);
}

TEST(load_falls_back_to_a_pick_when_a_kit_file_is_gone) {
  // a stored one-shot path that no longer exists: a random file of the same role replaces it, one card action per step
  Sets& s = sets();
  std::string name = s.lastName(); std::string text = readText(kPresets + "/" + name);
  std::string kit = valueOf(text, "lp.track.7.kit");
  size_t semi = kit.find(';'); std::string kick = kit.substr(0, semi);
  std::string patched = text; patched.replace(patched.find(kick), kick.size(), "/lp/packs/House/drum/kick/gone__Kick.wav");
  const std::string tmp = "House_Amin_900.lpset"; writeText(kPresets + "/" + tmp, patched); note(kPresets + "/" + tmp);
  sd().remove(kIndex.c_str()); writeText(kIndex, tmp + "\n");
  scramble();
  Rng rng(12); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == tmp);
  CHECK(eng().kit(0).slot[0].present == 1);
  CHECK(std::string(eng().kit(0).name[0]) != "gone.wav" && std::string(eng().kit(0).name[0]) != "gone__Kick.wav");
  CHECK(!locate("/lp/packs/House/drum/kick", eng().kit(0).name[0]).empty());   // a kick of the role folder or one of its shards
  std::printf("  kick fell back to %s\n", eng().kit(0).name[0]);
  removePreset(tmp);
}

TEST(load_finds_a_kit_file_whose_shard_changed_since_the_save) {
  // a stored absolute kit path whose sNN went stale (the card was re-sharded after the save, SPEC §9.2): the file
  // still exists in another shard of the same role folder, so the loader probes the role folder's shards (one
  // kidFn walk, then <= 3 probes per step — what the save's resolveKitPaths does too) and restores that very
  // file instead of replacing it with a random same-role pick
  Sets& s = sets();
  std::string name, text, kick;
  for (const std::string& f : suitePresets()) {                         // a saved set of this run whose kick sits in a shard
    std::string t = readText(kPresets + "/" + f); std::string kit = valueOf(t, "lp.track.7.kit");
    std::string k = kit.substr(0, kit.find(';'));
    if (k.find("/drum/kick/s") != std::string::npos && exists(k)) { name = f; text = t; kick = k; break; }
  }
  CHECK(!name.empty());
  if (name.empty()) return;
  const std::string folder = kick.substr(0, kick.rfind('/')), base = baseOf(kick), shard = baseOf(folder);
  const std::string other = shard == "s00" ? "s01" : "s00";
  const std::string stale = "/lp/packs/House/drum/kick/" + other + "/" + base;
  CHECK(!exists(stale));
  std::string patched = text; patched.replace(patched.find(kick), kick.size(), stale);
  const std::string tmp = "House_Amin_901.lpset"; writeText(kPresets + "/" + tmp, patched); note(kPresets + "/" + tmp);
  sd().remove(kIndex.c_str()); writeText(kIndex, tmp + "\n");
  scramble();
  Rng rng(13); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == tmp);
  CHECK(eng().kit(0).slot[0].present == 1);
  CHECK(std::string(eng().kit(0).name[0]) == base);
  std::printf("  kick %s stored under %s, found again under %s\n", base.c_str(), other.c_str(), shard.c_str());
  removePreset(tmp);
}

TEST(load_finds_a_pattern_whose_shard_changed_since_the_save) {
  // SPEC §9.29: lp.src.N names the library file by its absolute path, so a card re-imported and re-sharded after the
  // save (tools/shard_folders.py moves files between sNN folders) holds the file under another shard. The loader then
  // looks for the basename under its key folder and that folder's shards (one kidFn walk, then <= 3 probes per step —
  // what the save's resolveStep does) and restores that very file instead of leaving the track as it was.
  Sets& s = sets();
  std::string name, text, src; int tr = -1;
  for (const std::string& f : suitePresets()) {                         // a saved set of this run with a library pattern in a shard
    std::string t = readText(kPresets + "/" + f);
    for (int i = 0; i < kTracksPerSet && tr < 0; i++) {
      if (i == Vocal) continue;
      std::string p = valueOf(t, "lp.src." + std::to_string(i));
      if (p.compare(0, 9, "/lp/midi/") != 0 || !exists(p)) continue;
      const std::string shard = baseOf(p.substr(0, p.rfind('/')));
      if (shard.size() == 3 && shard[0] == 's' && shard[1] >= '0' && shard[1] <= '9' && shard[2] >= '0' && shard[2] <= '9') { name = f; text = t; src = p; tr = i; }
    }
    if (tr >= 0) break;
  }
  CHECK(tr >= 0);
  if (tr < 0) return;
  const std::string folder = src.substr(0, src.rfind('/')), base = baseOf(src), shard = baseOf(folder), keyFolder = folder.substr(0, folder.rfind('/'));
  const std::string other = shard == "s00" ? "s01" : "s00";
  const std::string stale = keyFolder + "/" + other + "/" + base;
  CHECK(!exists(stale));
  const std::string line = "lp.src." + std::to_string(tr) + "=";
  std::string patched = text; patched.replace(patched.find(line + src), (line + src).size(), line + stale);
  const std::string tmp = "House_Amin_902.lpset"; writeText(kPresets + "/" + tmp, patched); note(kPresets + "/" + tmp);
  sd().remove(kIndex.c_str()); writeText(kIndex, tmp + "\n");
  Pattern notes; { IFile* f = sd().open(src.c_str(), "r"); CHECK(f != nullptr); if (f) { CHECK(smfParse(*f, notes)); f->close(); } }
  scramble();
  Rng rng(14); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == tmp);
  CHECK(std::string(gen().info(tr).file) == base);                       // that very file, not a leftover of the scramble
  char remembered[kMaxPathLen] = ""; CHECK(gen().sourcePath(tr, remembered, sizeof remembered)); CHECK(std::string(remembered) == src);   // the generator remembers where it was found: the next save / session write names the real path
  { const Pattern& now = eng().sourcePattern(tr); bool same = now.count == notes.count; for (int i = 0; same && i < now.count; i++) same = now.notes[i].packed == notes.notes[i].packed; CHECK(same); }
  checkLoaded(tmp);
  std::printf("  %s %s stored under %s, found again under %s\n", categoryName(trackCategory(tr)), base.c_str(), other.c_str(), shard.c_str());
  removePreset(tmp);
}

TEST(load_keeps_a_track_whose_pattern_is_gone_from_the_library) {
  // SPEC §9.29: a library pattern that is nowhere under its key folder (the card holds another library now): the search
  // ends after the flat key folder and its shards, the track keeps the pattern it had (the engine's source, the
  // generator's file and seed), and the load still ends LOADED with every other track restored from the file.
  Sets& s = sets();
  std::string name, text, src;
  for (const std::string& f : suitePresets()) {                         // a saved set of this run whose Chord came from the library
    std::string t = readText(kPresets + "/" + f); std::string p = valueOf(t, "lp.src.0");
    if (p.compare(0, 15, "/lp/midi/Chord/") == 0 && exists(p)) { name = f; text = t; src = p; break; }
  }
  CHECK(!name.empty());
  if (name.empty()) return;
  const size_t keyEnd = src.find('/', 15); CHECK(keyEnd != std::string::npos);
  if (keyEnd == std::string::npos) return;
  const std::string keyFolder = src.substr(0, keyEnd), keyLabel = src.substr(15, keyEnd - 15);
  const std::string gone = keyFolder + "/s00/" + keyLabel + "__Chord__gone_from_the_card__00000000.mid";
  CHECK(!exists(gone));
  std::string patched = text; patched.replace(patched.find("lp.src.0=" + src), ("lp.src.0=" + src).size(), "lp.src.0=" + gone);
  const std::string tmp = "House_Amin_903.lpset"; writeText(kPresets + "/" + tmp, patched); note(kPresets + "/" + tmp);
  sd().remove(kIndex.c_str()); writeText(kIndex, tmp + "\n");
  scramble();
  std::vector<uint32_t> before; { const Pattern& p = eng().sourcePattern(Chord); for (int i = 0; i < p.count; i++) before.push_back((uint32_t)p.notes[i].packed); }
  Rng rng(15); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == tmp);                      // LOADED: nothing failed
  CHECK(std::string(gen().info(Chord).file) == "scrambled"); CHECK_EQ((long long)gen().info(Chord).seed, 0xDEADBEEFLL);   // the Chord track as it was
  { const Pattern& now = eng().sourcePattern(Chord); bool same = now.count == (int)before.size(); for (int i = 0; same && i < now.count; i++) same = (uint32_t)now.notes[i].packed == before[(size_t)i]; CHECK(same); }
  for (int i = 1; i < kTracksPerSet; i++) {                              // every other track came from the file
    const std::string p = valueOf(patched, "lp.src." + std::to_string(i));
    if (p.empty() || p == "acid") continue;
    CHECK(baseOf(p) == gen().info(i).file);
  }
  std::printf("  Chord kept %s with %s gone; %d other tracks restored\n", gen().info(Chord).file, baseOf(gone).c_str(), kTracksPerSet - 1);
  removePreset(tmp);
  CHECK(s.beginLoadNamed(0, name.c_str(), rng)); settle(); CHECK(s.ok());   // back to a plain saved state: the scrambled Chord is not one a session round trip can reproduce
}

TEST(load_no_match_changes_nothing) {
  Sets& s = sets();
  int root = -1;                                                    // a key with no preset in the folder
  std::vector<std::string> files = listDir(kPresets, ".lpset");
  for (int r = 0; r < 12 && root < 0; r++) {
    bool used = false;
    for (auto& n : files) { std::string t = readText(kPresets + "/" + n); SetFile f; parseSetFileText(t.c_str(), (int)t.size(), f); if (f.keyRoot == r) used = true; }
    if (!used) root = r;
  }
  CHECK(root >= 0);
  uint8_t saved = gen().key().root; gen().key().root = (uint8_t)root;
  std::vector<Snapshot> before = snapshot();
  Rng rng(3); char name[64] = "x";
  CHECK(!s.loadRandom(0, rng, name, sizeof name));
  CHECK(!s.busy()); CHECK(!s.ok());
  char minorLabel[8], majorLabel[8]; gen().key().labels(minorLabel, majorLabel);
  CHECK(std::string(s.lastError()) == std::string("NO SET IN ") + minorLabel);
  CHECK(name[0] == 0);
  pump(400);
  std::vector<Snapshot> after = snapshot();
  for (int t = 0; t < kTracksPerSet; t++) {
    CHECK(before[t].file == after[t].file); CHECK_EQ((long long)before[t].seed, (long long)after[t].seed);
    CHECK_EQ(before[t].variant, after[t].variant); CHECK_EQ(before[t].len, after[t].len); CHECK_EQ(before[t].state, after[t].state);
    for (int k = 0; k < kSampleSlots; k++) CHECK(before[t].kit[k] == after[t].kit[k]);
  }
  CHECK_EQ((int)eng().status().generating[0], 0);
  gen().key().root = saved;
  // a missing genre folder is a no-match as well
  int g = gen().genreIndex(); gen().setGenre(g + 1);                 // another genre (Testpack has no presets folder)
  CHECK(!s.loadRandom(0, rng, name, sizeof name)); CHECK(std::string(s.lastError()).compare(0, 9, "NO SET IN") == 0);
  gen().setGenre(g);
}

// ---------------------------------------------------------------------------------------------- listing index

// ---------------------------------------------------------------------------------------------- session (SPEC §9.18)

static const std::string kState = "/lp/state";
static bool g_stateExisted = false;
static std::set<std::string> g_stateBefore;                      // files already in /lp/state before the session tests (never removed)
static bool isSessionFile(const std::string& n) { return n.compare(0, 11, "session_set") == 0 || (n.compare(0, 9, "session_s") == 0 && hasExt(n.c_str(), ".mid")); }
static void pumpSession(int maxIterations) { for (int i = 0; i < maxIterations && (sets().sessionBusy() || sets().busy() || gen().busy()); i++) pump(1); }
struct TrackSnap { std::string file, sound; uint32_t seed; int variant, preset, keyRoot, lenStep, pause, acid, acidPat, edited, loop; Pattern src; };
static TrackSnap snapTrack(int t) {
  TrackSnap s; const TrackGenInfo& i = gen().info(t);
  s.file = i.file; s.sound = i.sound; s.seed = i.seed; s.variant = i.variant; s.preset = i.presetIndex; s.keyRoot = i.keyRoot; s.lenStep = i.lenStep; s.pause = i.pause;
  s.acid = i.acid; s.acidPat = i.acidPat; s.edited = i.edited; s.loop = i.userLoop; s.src = eng().sourcePattern(t);
  return s;
}

TEST(session_write_is_debounced_and_yields_to_the_user) {
  Sets& s = sets();
  g_stateExisted = exists(kState);
  for (auto& f : listDir(kState, nullptr)) { if (isSessionFile(f)) sd().remove((kState + "/" + f).c_str()); else g_stateBefore.insert(f); }   // a stale run's files would fake the timing below
  const std::set<std::string> listing = presetNames();
  CHECK(!s.sessionBusy());
  // shape the state: a changed strip, an acid lead with a generated line, a recorded Fx loop, a pause and a variant
  push(CmdSetParam, Pad, PVolume, 0, 0.55f); push(CmdSetParam, Pad, PReverb, 0, 0.3f); pump(2);
  gen().setAcid(Lead, true); pump(2);
  CHECK(gen().generatePattern(Lead)); settle();
  CHECK_EQ((int)gen().info(Lead).acidPat, 1);
  Pattern& fx = eng().sourcePattern(Fx); fx.clear(kTicksPerBar);
  fx.add(Note::make(0, 48, 57, 100)); fx.add(Note::make(96, 24, 60, 90)); fx.add(Note::make(192, 96, 64, 80)); fx.add(Note::make(336, 36, 69, 110));
  TrackGenInfo& fxi = gen().info(Fx); fxi.file[0] = 0; fxi.userLoop = 1; fxi.edited = 1; fxi.variant = 0; fxi.keyRoot = gen().key().root; fxi.sourceTicks = kTicksPerBar; fxi.loaded = 1;
  gen().rebuildVariant(Fx, 0);
  gen().setLengthStep(Vocal, +1);                                   // pause 2
  gen().rebuildVariant(Chord, 1);
  s.setSessionVolume(144);
  settle();
  // the debounce: nothing for 2 s of the engine's clock after the last change, then the write (busy() stays false)
  s.markSessionDirty();
  CHECK(!s.sessionBusy()); CHECK(!s.busy());
  pump(Sets::kSessionDebounceSamples / kBlock - 8);
  CHECK(!s.sessionBusy()); CHECK(!exists(kState + "/session_set0.lpset"));
  s.markSessionDirty();                                             // a change inside the window restarts it
  pump(Sets::kSessionDebounceSamples / kBlock - 8);
  CHECK(!s.sessionBusy());
  for (int i = 0; i < 40 && !s.sessionBusy(); i++) pump(1);
  CHECK(s.sessionBusy()); CHECK(!s.busy());                        // writing, and not "busy" for the UI
  // a user save aborts the write (the user's card work wins) and re-arms it
  CHECK(s.beginSave(0)); CHECK(!s.sessionBusy()); CHECK(s.busy());
  for (int n = 0; s.busy() && n < 100000; n++) stepSets();
  CHECK(s.ok()); const std::string saved = s.lastName(); notePreset(saved);
  pumpSession(2000);
  pump(Sets::kSessionDebounceSamples / kBlock + 40);
  pumpSession(2000);
  CHECK(!s.sessionBusy());
  for (const char* f : {"/session_set0.lpset", "/session_set1.lpset", "/session_s0_t4.mid"}) { note(kState + f); CHECK(exists(kState + f)); }
  const std::string t0 = readText(kState + "/session_set0.lpset");
  CHECK(hasLine(t0, "schemaVersion=1")); CHECK(hasLine(t0, "setIndex=0")); CHECK(hasLine(t0, "keyRoot=9")); CHECK(hasLine(t0, "genre=House")); CHECK(hasLine(t0, "lp.volume=144"));
  CHECK(valueOf(t0, "lp.bpm") == std::to_string(eng().status().bpm));
  CHECK(t0.find("clipType") == std::string::npos); CHECK(t0.find("track.0.clip=") == std::string::npos);   // no asset copies
  CHECK(valueOf(t0, "lp.src.1") == "acid"); CHECK(valueOf(t0, "lp.track.1.acid") == "1");
  CHECK(valueOf(t0, "lp.src.4") == kState + "/session_s0_t4.mid"); CHECK(valueOf(t0, "lp.track.4.edited") == "1"); CHECK(valueOf(t0, "lp.track.4.loop") == "1");
  for (int i : {0, 2, 3, 5, 7}) {                                   // the path the pattern was loaded from (SPEC §9.19 §3 b: the generator remembers it): the library or a legacy preset asset; a pattern loaded from a preset's own _t<i>.mid gets a session SMF (SPEC §9.29: the preset may be deleted)
    std::string src = valueOf(t0, "lp.src." + std::to_string(i));
    const bool sessionSmf = src == kState + "/session_s0_t" + std::to_string(i) + ".mid";
    CHECK(src.compare(0, 9, "/lp/midi/") == 0 || src.find("/presets/assets/") != std::string::npos || sessionSmf); CHECK(exists(src));
    if (!sessionSmf) CHECK(baseOf(src) == gen().info(i).file);
  }
  { std::string v = valueOf(t0, "lp.src.6"); CHECK(v == gen().vocalPath(0, 0)); CHECK(exists(v)); }
  for (int k = 0; k < kSampleSlots; k++) { std::string p = valueOf(t0, "lp.kit." + std::to_string(k)); CHECK(exists(p)); CHECK(baseOf(p) == eng().kit(0).name[k]); }
  CHECK(valueOf(t0, "lp.track.2.vol") == "0.550"); CHECK(valueOf(t0, "lp.track.2.reverb") == "0.300");
  CHECK(valueOf(t0, "lp.track.6.pause") == "2"); CHECK(valueOf(t0, "lp.track.0.variant") == "1"); CHECK(t0.find("track.0.slot") == std::string::npos);
  CHECK(valueOf(t0, "lp.track.1.seed") == std::to_string(gen().info(Lead).seed));
  const std::string t1 = readText(kState + "/session_set1.lpset");
  CHECK(hasLine(t1, "setIndex=1")); CHECK(t1.find("lp.src.") == std::string::npos);   // set 2 holds nothing on this run
  // lp.end=1 is the last line of both files (a file cut short carries none); lp.seq counts up across the files written
  CHECK(t0.size() > 9 && t0.compare(t0.size() - 9, 9, "lp.end=1\n") == 0); CHECK(t1.size() > 9 && t1.compare(t1.size() - 9, 9, "lp.end=1\n") == 0);
  const unsigned long seq0 = strtoul(valueOf(t0, "lp.seq").c_str(), nullptr, 10), seq1 = strtoul(valueOf(t1, "lp.seq").c_str(), nullptr, 10);
  CHECK(seq0 >= 1); CHECK(seq1 > seq0); CHECK_EQ((long long)s.sessionSeq(), (long long)seq1);
  { SetFile f; parseSetFileText(t0.c_str(), (int)t0.size(), f); CHECK_EQ((int)f.complete, 1); CHECK_EQ((long long)f.seq, (long long)seq0); }
  // the session SMF is the recorded loop
  IFile* f = sd().open((kState + "/session_s0_t4.mid").c_str(), "r"); CHECK(f != nullptr);
  if (f) { Pattern back; CHECK(smfParse(*f, back)); f->close(); CHECK_EQ((int)back.count, (int)fx.count); CHECK_EQ((int)back.lengthTicks, (int)fx.lengthTicks); }
  // nothing in the presets folder was touched by the session (the user's save added its one file)
  std::set<std::string> after = presetNames(); after.erase(saved);
  CHECK(after == listing);
  removePreset(saved);
}

TEST(session_restore_rebuilds_both_sets_without_playing) {
  Sets& s = sets();
  const std::set<std::string> listing = presetNames();
  CHECK(exists(kState + "/session_set0.lpset"));
  // what the files should bring back
  TrackSnap want[kTracksPerSet]; for (int t = 0; t < kTracksPerSet; t++) want[t] = snapTrack(t);
  std::string kit[kSampleSlots]; for (int k = 0; k < kSampleSlots; k++) kit[k] = eng().kit(0).name[k];
  const int vocalLen = eng().status().track[Vocal].lenTicks, chordVariant = eng().status().track[Chord].variant;
  // scramble everything the restore must rebuild
  scramble();
  for (int t = 0; t < kTracksPerSet; t++) { eng().sourcePattern(t).clear(); gen().info(t).acid = 0; gen().info(t).acidPat = 0; }
  gen().info(Vocal).pause = 0;
  push(CmdSetParam, Pad, PVolume, 0, 0.1f); push(CmdSetParam, Pad, PReverb, 0, 0.0f); push(CmdSetTempo, 0, 0, 97);
  gen().setKeyRoot(4);                                              // E: the restore brings A back
  pump(2);
  writeText(kState + "/session_set1.lpset", "schemaVersion=7\nsetIndex=1\n=\n#truncated\x01\x02 garbage ::: \n\\");   // malformed set 2 (unknown schema, junk): ignored
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  pump(2);
  const std::string fileBpm = valueOf(readText(kState + "/session_set0.lpset"), "lp.bpm");
  s.restoreSession();
  CHECK(!s.busy());                                                 // queued: starts on the next step
  pump(1);
  CHECK(s.sessionBusy()); CHECK(s.busy());                          // restoring owns the card like a load
  CHECK(!s.beginSave(0)); CHECK(strcmp(s.lastError(), "BUSY") == 0);
  pumpSession(4000);
  CHECK(!s.sessionBusy()); CHECK(!s.busy());
  pump(kSampleRate * 3 / kBlock);                                   // the swaps land on the bar line
  CHECK_EQ((int)gen().key().root, 9);
  CHECK_EQ(s.sessionVolume(), 144);
  CHECK(std::to_string(eng().status().bpm) == fileBpm);           // lp.bpm applied (nothing plays)
  for (int t = 0; t < kTracksPerSet; t++) {
    const TrackSnap now = snapTrack(t); const TrackSnap& w = want[t];
    if (t != Fx) CHECK(now.file == w.file);
    CHECK_EQ((long long)now.seed, (long long)w.seed); CHECK_EQ(now.variant, w.variant); CHECK_EQ(now.preset, w.preset);
    CHECK_EQ(now.keyRoot, w.keyRoot); CHECK_EQ(now.lenStep, w.lenStep); CHECK_EQ(now.acid, w.acid); CHECK_EQ(now.acidPat, w.acidPat);
    if (t == Fx) { CHECK_EQ(now.edited, 1); CHECK_EQ(now.loop, 1); CHECK(now.file == "session_s0_t4.mid"); }
    if (t != Vocal) { bool same = now.src.count == w.src.count && now.src.lengthTicks == w.src.lengthTicks; for (int i = 0; same && i < now.src.count; i++) same = now.src.notes[i].packed == w.src.notes[i].packed; CHECK(same); if (!same) std::printf("  track %d source differs (%d vs %d notes)\n", t, now.src.count, w.src.count); }
    if (isSynthCategory((Category)t)) CHECK(now.sound == w.sound);
    CHECK(eng().status().track[t].state != PSPlaying);             // nothing starts playing
    CHECK_EQ((int)eng().status().track[t].pendingSwap, 0);
  }
  CHECK_EQ((int)gen().info(Vocal).pause, 2); CHECK_EQ((int)eng().status().track[Vocal].lenTicks, vocalLen);
  CHECK_EQ((int)eng().status().track[Chord].variant, chordVariant);
  for (int k = 0; k < kSampleSlots; k++) { CHECK(kit[k] == eng().kit(0).name[k]); CHECK(eng().kit(0).slot[k].present == 1); }
  CHECK_NEAR(eng().params(Pad).volume, 0.55f, 2e-3); CHECK_NEAR(eng().params(Pad).reverb, 0.3f, 2e-3); CHECK_NEAR(eng().params(Chord).volume, 0.8f, 1e-4);
  CHECK_EQ((int)gen().info(Lead).acid, 1); CHECK_EQ((int)gen().info(Lead).acidPat, 1);
  CHECK(strncmp(gen().info(Lead).sound, "303", 3) == 0);
  for (int t = kTracksPerSet; t < kTracks; t++) CHECK_EQ((int)eng().status().track[t].state, (int)PSEmpty);   // the malformed set-2 file changed nothing
  CHECK_EQ((int)eng().status().generating[0], 0); CHECK_EQ((int)eng().status().generating[1], 0);
  // a missing file is ignored silently too
  sd().remove((kState + "/session_set1.lpset").c_str());
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate * 2 / kBlock);
  CHECK(!s.sessionBusy()); CHECK_EQ((int)gen().key().root, 9);
  CHECK(presetNames() == listing);                                  // the session never touches the presets folder
  // back to a plain state for the later tests
  gen().setAcid(Lead, false); pump(2);
  gen().info(Fx) = TrackGenInfo(); eng().sourcePattern(Fx).clear();
  gen().setLengthStep(Vocal, -1);
  push(CmdSetParam, Pad, PVolume, 0, 0.8f); push(CmdSetParam, Pad, PReverb, 0, 0.0f); pump(2);
  pumpSession(2000);
}

// The slot holding the newest complete session file of a set (-1 = none), as the restore picks it, and its text.
static std::string sessionPath(int set, int slot) { char p[kMaxPathLen]; Sets::sessionFilePath(p, sizeof p, set, slot); return p; }
static int newestSessionSlot(int set) {
  int best = -1; unsigned long bestSeq = 0;
  for (int slot = 0; slot < 2; slot++) {
    const std::string p = sessionPath(set, slot);
    if (!exists(p)) continue;
    const std::string t = readText(p);
    SetFile f; parseSetFileText(t.c_str(), (int)t.size(), f);
    if (f.complete && (best < 0 || f.seq > bestSeq)) { best = slot; bestSeq = f.seq; }
  }
  return best;
}
static std::string newestSessionText(int set) { const int slot = newestSessionSlot(set); return slot < 0 ? std::string() : readText(sessionPath(set, slot)); }
static void removeSessionFiles(int set) { for (int slot = 0; slot < 2; slot++) { const std::string p = sessionPath(set, slot); if (exists(p)) sd().remove(p.c_str()); } }
// Pumps until the armed session write begins (the 5 s debounce and the 15 s spacing of SPEC §9.19 §3 c both pass) and
// runs it to the end; false when no write began within maxBlocks.
static bool awaitSessionWrite(int maxBlocks = (Sets::kSessionDebounceSamples + Sets::kSessionSpacingSamples) / kBlock + 400) {
  int i = 0;
  for (; i < maxBlocks && !sets().sessionBusy(); i++) pump(1);
  const bool began = sets().sessionBusy();
  pumpSession(6000);
  return began;
}
// Lets a pending (dirty) session write run to the end so the files hold the current state and nothing is dirty.
static void flushSession() { sets().markSessionDirty(); CHECK(awaitSessionWrite()); CHECK(!sets().sessionBusy()); }

// SPEC §9.18: two slots per set. A file cut short (a power-off or an abort mid-write leaves no lp.end) is skipped and
// the other, complete slot is restored; the next write takes the slot that is not the newest; both slots cut short
// restore nothing for that set.
// SPEC §9.19f (user decision 2026-10-16): config.txt bpm= wins over a restored session's lp.bpm; without a config tempo the
// session's tempo comes back (the previous test). A preset load is unaffected (it is not a restore).
TEST(session_restore_keeps_the_config_tempo) {
  Sets& s = sets();
  CHECK(exists(kState + "/session_set0.lpset"));
  const std::string fileBpm = valueOf(readText(kState + "/session_set0.lpset"), "lp.bpm");
  const int cfg = fileBpm == "125" ? 110 : 125;                     // a config tempo that differs from the file's
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  push(CmdSetTempo, 0, 0, cfg); pump(2);                            // what loadConfig() does at boot, then it tells Sets
  s.setConfigBpm(cfg);
  s.restoreSession(); pump(1); pumpSession(4000);
  CHECK(!s.sessionBusy()); CHECK(!s.busy());
  pump(kSampleRate * 3 / kBlock);
  CHECK_EQ((int)eng().status().bpm, cfg);                           // the session's lp.bpm did not override the config tempo
  s.setConfigBpm(0);
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate * 3 / kBlock);
  CHECK(std::to_string(eng().status().bpm) == fileBpm);           // no config tempo: the session's comes back
}

TEST(session_restore_skips_a_truncated_slot_and_takes_the_newest_complete_one) {
  Sets& s = sets();
  const std::set<std::string> listing = presetNames();
  CHECK(sessionPath(0, 1) == kState + "/session_set0_b.lpset"); note(sessionPath(0, 1)); note(sessionPath(1, 1));
  settle(); flushSession();                                            // the files hold the current state, nothing is dirty
  const int newest = newestSessionSlot(0); CHECK(newest >= 0);
  const std::string a = sessionPath(0, newest < 0 ? 0 : newest), b = sessionPath(0, newest < 0 ? 1 : newest ^ 1);
  removeSessionFiles(1);                                               // set 2 contributes nothing (its file carries the global key too)
  std::string ta = readText(a);
  CHECK(ta.size() > 9 && ta.compare(ta.size() - 9, 9, "lp.end=1\n") == 0);
  const unsigned long seqA = strtoul(valueOf(ta, "lp.seq").c_str(), nullptr, 10); CHECK(seqA >= 1);
  CHECK_EQ((long long)s.sessionSeq(), (long long)seqA + 1);            // set 2's file was the last one written
  // the other slot: a newer complete copy with another Pad volume; the newest slot: cut short in the middle of the lp lines
  std::string tb = ta;
  size_t p = tb.find("lp.seq=" + std::to_string(seqA) + "\n"); CHECK(p != std::string::npos);
  if (p != std::string::npos) tb.replace(p, ("lp.seq=" + std::to_string(seqA) + "\n").size(), "lp.seq=" + std::to_string(seqA + 2) + "\n");
  p = tb.find("lp.track.2.vol="); CHECK(p != std::string::npos);
  if (p != std::string::npos) { const size_t e = tb.find('\n', p); tb.replace(p, e - p, "lp.track.2.vol=0.333"); }
  writeText(b, tb);
  writeText(a, ta.substr(0, ta.size() / 2));
  { SetFile f; parseSetFileText(ta.c_str(), (int)(ta.size() / 2), f); CHECK_EQ((int)f.schemaVersion, 1); CHECK_EQ((int)f.complete, 0); }   // header intact, no terminator
  push(CmdSetParam, Pad, PVolume, 0, 0.1f); gen().setKeyRoot(4); pump(2);
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  pump(2);
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate / kBlock);   // (under the 2 s debounce: the key change above must not write yet)
  CHECK(!s.sessionBusy());
  CHECK_EQ((int)gen().key().root, 9);                                  // restored from the complete slot
  CHECK_NEAR(eng().params(Pad).volume, 0.333f, 2e-3);
  CHECK_EQ((long long)s.sessionSeq(), (long long)(seqA + 2));
  for (int t = 0; t < kTracksPerSet; t++) CHECK(eng().status().track[t].state != PSPlaying);
  // the next write goes to the other slot (the truncated one), complete, with a higher seq; the restored slot stays as it was
  flushSession();
  ta = readText(a);
  CHECK(ta.size() > 9 && ta.compare(ta.size() - 9, 9, "lp.end=1\n") == 0);
  CHECK(strtoul(valueOf(ta, "lp.seq").c_str(), nullptr, 10) > seqA + 2);
  CHECK(valueOf(ta, "lp.track.2.vol") == "0.333");
  CHECK(readText(b) == tb);
  CHECK(newestSessionText(0) == ta);
  // both slots cut short: the set is skipped silently (the key stays where it was), nothing else happens
  const std::string full = ta;
  writeText(a, ta.substr(0, ta.size() - 9)); writeText(b, tb.substr(0, tb.size() - 9));   // everything but the terminator
  removeSessionFiles(1);
  gen().setKeyRoot(4); pump(2);
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate / kBlock);
  CHECK(!s.sessionBusy()); CHECK_EQ((int)gen().key().root, 4);
  CHECK(presetNames() == listing);
  writeText(a, full); gen().setKeyRoot(9);
  push(CmdSetParam, Pad, PVolume, 0, 0.8f); pump(2);
  flushSession();
}

// SPEC §9.18: a library pattern whose file is no longer on the card is kept as a session SMF and flagged edited, so a
// restore followed by a preset save writes the SMF asset instead of losing the track as NONE.
TEST(gone_library_source_is_written_as_an_edited_session_smf) {
  Sets& s = sets();
  settle();
  TrackGenInfo& inf = gen().info(Chord); const TrackGenInfo saved = inf; const Pattern src0 = eng().sourcePattern(Chord);
  CHECK(src0.count > 0);
  strcpy(inf.file, "Amin__Chord__gone_from_the_card__00000000.mid"); inf.edited = 0; inf.acidPat = 0; inf.userLoop = 0;
  s.markSessionDirty();
  CHECK(awaitSessionWrite());                                        // (the renamed file no longer matches the remembered pick: the writer probes, finds nothing and keeps the notes)
  CHECK(!s.sessionBusy());
  const std::string t0 = newestSessionText(0);
  CHECK(!t0.empty());
  CHECK(valueOf(t0, "lp.src.0") == kState + "/session_s0_t0.mid"); CHECK(valueOf(t0, "lp.track.0.edited") == "1");
  note(kState + "/session_s0_t0.mid"); CHECK(exists(kState + "/session_s0_t0.mid"));
  // restore: the track is an edit now (its source is the session SMF)
  scramble();
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  pump(2);
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate * 3 / kBlock);
  CHECK(!s.sessionBusy());
  CHECK_EQ((int)inf.edited, 1); CHECK(std::string(inf.file) == "session_s0_t0.mid");
  { const Pattern& now = eng().sourcePattern(Chord); bool same = now.count == src0.count; for (int i = 0; same && i < now.count; i++) same = now.notes[i].packed == src0.notes[i].packed; CHECK(same); }
  for (int t = 0; t < kTracksPerSet; t++) if (valueOf(t0, "lp.src." + std::to_string(t)).empty()) { gen().info(t) = TrackGenInfo(); eng().sourcePattern(t).clear(); }   // tracks the session did not carry are empty on a real boot (the scramble above is not)
  // ... so a preset save writes it as an SMF asset, not NONE
  char name[64] = "";
  const bool savedOk = saveSync(0, name, sizeof name); CHECK(savedOk); if (!savedOk) std::printf("  save error: %s (%s)\n", s.lastError(), name);
  const std::string n = name; notePreset(n);
  const std::string text = readText(kPresets + "/" + n);
  CHECK(valueOf(text, "lp.src.0") == smfOf(kPresets, n, Chord)); CHECK(exists(smfOf(kPresets, n, Chord)));   // its own SMF (the session SMF is the next write's)
  removePreset(n);
  inf = saved; eng().sourcePattern(Chord) = src0; gen().rebuildVariant(Chord, saved.variant); settle();
}

// ---------------------------------------------------------------------------------------------- loop mode (SPEC §9.19)

static bool startsWith(const std::string& v, const std::string& prefix) { return v.compare(0, prefix.size(), prefix) == 0; }
static void stopAll() { for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t); pump(2); }
static bool startTrack(int t) { push(CmdTrackToggle, t); for (int i = 0; i < 400 && eng().status().track[t].state != PSPlaying; i++) pump(1); return eng().status().track[t].state == PSPlaying; }
static void loopOff(int t) { gen().setLoopMode(t, false); pump(kSampleRate * 2 / kBlock); settle(); }
static std::string loopDir(Category c) { return std::string("/lp/packs/House/loops/") + Generator::loopFolder(c) + "/"; }

// SPEC §9.19 §1.7 / §9.29: a track in loop mode is saved as its loop — lp.src.i = the loop's absolute path (no copy),
// lp.track.i.loopmode=1 with the loop's variant and len — and a load restores loop mode with that exact file.
TEST(loop_mode_round_trips_through_a_preset) {
  Sets& s = sets();
  settle(); stopAll();
  const std::set<std::string> listing = presetNames();
  CHECK(gen().setLoopMode(Beat, true)); settle(); CHECK(gen().loopEnabled(Beat)); CHECK(gen().loopFile(Beat)[0] != 0);
  gen().rebuildVariant(Beat, 2); gen().setLengthStep(Beat, +1);     // the first 2 beats, k 1 → a 1-bar clip
  settle();
  const std::string loop = gen().loopFile(Beat), loopPath = gen().loopPath(Beat);
  CHECK(startsWith(loopPath, loopDir(Beat)));
  char name[64] = "";
  CHECK(saveSync(0, name, sizeof name));
  const std::string n = name; notePreset(n);
  const std::string text = readText(kPresets + "/" + n);
  CHECK(valueOf(text, "lp.src.7") == loopPath);
  CHECK(hasLine(text, "lp.track.7.loopmode=1")); CHECK(valueOf(text, "lp.track.7.variant") == "2");
  CHECK(valueOf(text, "lp.track.7.len") == "4"); CHECK(valueOf(text, "lp.track.7.lenk") == "1");   // 2 beats × 2^1
  CHECK(text.find("lp.track.0.loopmode") == std::string::npos);
  CHECK(exists(kPresets + "/assets") == g_assetsExisted); CHECK(!exists(smfOf(kPresets, n, Beat)));   // nothing copied, no SMF for a loop
  SetFile back; parseSetFileText(text.c_str(), (int)text.size(), back);
  CHECK_EQ((int)back.track[7].loopMode, 1); CHECK(std::string(back.src[7]) == loopPath); CHECK_EQ((int)back.track[0].loopMode, 0);
  // off, scrambled, then load this very file: loop mode comes back with the asset copy, the variant and the length step
  loopOff(Beat); CHECK(!gen().loopEnabled(Beat));
  sd().remove(kIndex.c_str()); writeText(kIndex, n + "\n");          // only this set is listed
  scramble(); gen().setLoopMode(Chord, true); settle();            // a loop on another track: the file says plain → it ends with the load
  CHECK(gen().loopEnabled(Chord));
  Rng rng(900); char loaded[64] = "";
  CHECK(s.loadRandom(0, rng, loaded, sizeof loaded)); settle();
  CHECK(s.ok()); CHECK(std::string(loaded) == n);
  CHECK(gen().loopEnabled(Beat)); CHECK(!gen().loopEnabled(Chord));
  CHECK(std::string(gen().loopFile(Beat)) == loop); CHECK(std::string(gen().loopPath(Beat)) == loopPath);
  CHECK_EQ((int)gen().info(Beat).variant, 2); CHECK_EQ((int)gen().info(Beat).lenStep, 1); CHECK_EQ((int)gen().setLengthStep(Beat, 0), kTicksPerBar);
  pump(kSampleRate * 3 / kBlock);
  CHECK(eng().trackStreams(Beat)); CHECK_EQ((int)eng().status().track[Beat].lenTicks, kTicksPerBar); CHECK_EQ((int)eng().status().track[Beat].variant, 2);
  CHECK(!eng().trackStreams(Chord));
  // re-saving the loaded set references the same loop file; tracks the file did not carry are empty on a real boot
  // (the scramble above is not)
  for (int t = 0; t < kTracksPerSet; t++) if (std::string(gen().info(t).file) == "scrambled" && !gen().loopEnabled(t)) { gen().info(t) = TrackGenInfo(); eng().sourcePattern(t).clear(); }
  char again[64] = ""; const bool againOk = saveSync(0, again, sizeof again); CHECK(againOk); if (!againOk) std::printf("  re-save error: %s (%s)\n", s.lastError(), again); notePreset(again);
  CHECK(valueOf(readText(kPresets + "/" + again), "lp.src.7") == loopPath);
  removePreset(again);
  loopOff(Beat);
  rewriteIndex();
  std::set<std::string> after = presetNames(); after.erase(n); CHECK(after == listing);
  removePreset(n);
}

// SPEC §9.19 §1.7: a session file carries lp.track.i.loopmode=1 and the loop's absolute path in lp.src.i (no asset
// copy); the restore brings loop mode back with that exact file, a missing file → a pick of the category.
TEST(loop_mode_in_session_files_restores_and_falls_back_to_a_pick) {
  Sets& s = sets();
  settle(); stopAll();
  CHECK(gen().setLoopMode(Percussion, true)); settle(); CHECK(gen().loopEnabled(Percussion));
  CHECK(gen().setLoopMode(Vocal, true)); settle(); CHECK(gen().loopEnabled(Vocal));     // the Vocal track's loop through its own stream
  const std::string perc = gen().loopPath(Percussion), voc = gen().loopPath(Vocal);
  CHECK(startsWith(perc, loopDir(Percussion))); CHECK(startsWith(voc, loopDir(Vocal)));
  flushSession();
  const std::string t0 = newestSessionText(0);
  CHECK(valueOf(t0, "lp.src.5") == perc); CHECK(hasLine(t0, "lp.track.5.loopmode=1"));
  CHECK(valueOf(t0, "lp.src.6") == voc); CHECK(hasLine(t0, "lp.track.6.loopmode=1"));
  CHECK(t0.find("lp.track.7.loopmode") == std::string::npos);
  CHECK(t0.find("clipType") == std::string::npos);
  // restore: both loops come back with their files, nothing plays
  loopOff(Percussion); loopOff(Vocal); scramble();
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  pump(2);
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate * 3 / kBlock);
  CHECK(!s.sessionBusy());
  CHECK(gen().loopEnabled(Percussion)); CHECK(std::string(gen().loopPath(Percussion)) == perc);
  CHECK(gen().loopEnabled(Vocal)); CHECK(std::string(gen().loopPath(Vocal)) == voc); CHECK_EQ(gen().loopSlot(Vocal), -1);
  CHECK(eng().trackStreams(Percussion)); CHECK(eng().trackStreams(Vocal));
  for (int t = 0; t < kTracksPerSet; t++) CHECK(eng().status().track[t].state != PSPlaying);
  // the file gone: the restore picks another loop of the category instead
  int slot = newestSessionSlot(0); CHECK(slot >= 0);
  std::string edited = readText(sessionPath(0, slot));
  size_t p = edited.find("lp.src.5=" + perc); CHECK(p != std::string::npos);
  if (p != std::string::npos) edited.replace(p, ("lp.src.5=" + perc).size(), "lp.src.5=" + loopDir(Percussion) + "NA/gone__125bpm__LOOP__2bars__nothing.wav");
  writeText(sessionPath(0, slot), edited);
  loopOff(Percussion); loopOff(Vocal);
  s.restoreSession(); pump(1); pumpSession(4000); settle(); pump(kSampleRate * 3 / kBlock);
  CHECK(!s.sessionBusy());
  CHECK(gen().loopEnabled(Percussion)); CHECK(gen().loopFile(Percussion)[0] != 0);
  CHECK(startsWith(gen().loopPath(Percussion), loopDir(Percussion))); CHECK(exists(gen().loopPath(Percussion)));
  CHECK(std::string(gen().loopPath(Vocal)) == voc);
  loopOff(Percussion); loopOff(Vocal);
  CHECK(!gen().anyLoop());
  flushSession();                                                   // the files hold the plain state again
}

// SPEC §9.19 §3 b: the session writer composes every path from what the generator remembered at pick / load time —
// nothing is opened or probed to verify a source; the write's card work is the two session files and the SMF edits.
TEST(session_write_opens_nothing_to_verify_sources) {
  Sets& s = sets();
  settle(); stopAll();
  for (int t = 0; t < kTracks; t++) { gen().info(t).userLoop = 0; gen().info(t).edited = 0; }   // (an earlier scramble() left recorded-loop flags, which GENERATE keeps on every track now)
  CHECK(gen().generateSet(0)); settle();                            // fresh library picks on every track, a fresh kit
  CHECK(gen().generateSet(1)); settle();
  s.markSessionDirty();
  for (int i = 0; i < (Sets::kSessionDebounceSamples + Sets::kSessionSpacingSamples) / kBlock + 400 && !s.sessionBusy(); i++) pump(1);
  CHECK(s.sessionBusy());
  Counters total; int steps = 0;
  while (s.sessionBusy() && steps < 4000) {                         // every card call of the write, summed over its steps
    gen().step(); gen().pumpVocal();
    cfs().cur = Counters(); cfs().on = true; s.step(); cfs().on = false;
    total.probes += cfs().cur.probes; total.opens += cfs().cur.opens; total.walks += cfs().cur.walks; total.entries += cfs().cur.entries; total.bytesRead += cfs().cur.bytesRead; total.bytesWritten += cfs().cur.bytesWritten;
    static int16_t buf[kBlock]; eng().render(buf, kBlock); steps++;
  }
  CHECK(!s.sessionBusy());
  std::printf("  session write: %d steps, %d probes, %d opens, %d walks, %zu B read, %zu B written\n", steps, total.probes, total.opens, total.walks, total.bytesRead, total.bytesWritten);
  CHECK_EQ(total.probes, 0); CHECK_EQ(total.walks, 0); CHECK_EQ(total.bytesRead, 0u);
  CHECK_EQ(total.opens, 2);                                         // the two session files, nothing else
  for (int set = 0; set < kSets; set++) {
    const std::string t = newestSessionText(set); CHECK(!t.empty());
    for (int i = 0; i < kTracksPerSet; i++) {
      const std::string src = valueOf(t, "lp.src." + std::to_string(i));
      if (src.empty() || src == "acid") continue;
      CHECK(exists(src)); CHECK(baseOf(src) == gen().info(set * kTracksPerSet + i).file);
    }
    for (int k = 0; k < kSampleSlots; k++) { const std::string kp = valueOf(t, "lp.kit." + std::to_string(k)); CHECK(exists(kp)); CHECK(baseOf(kp) == eng().kit(set).name[k]); }
  }
}

// SPEC §9.19 §3 c: a change arms a 5 s debounce; a completed write keeps the next one at least 15 s away.
TEST(session_debounce_is_5s_and_writes_are_15s_apart) {
  Sets& s = sets();
  CHECK_EQ(Sets::kSessionDebounceSamples, 5 * kSampleRate); CHECK_EQ(Sets::kSessionSpacingSamples, 15 * kSampleRate);
  settle(); flushSession();                                          // a write just completed
  const uint32_t t0 = eng().status().sampleClock;
  s.markSessionDirty();
  pump(Sets::kSessionDebounceSamples / kBlock + 40);
  CHECK(!s.sessionBusy());                                          // debounced, but within 15 s of the last write
  while ((int32_t)(eng().status().sampleClock - t0) < Sets::kSessionSpacingSamples - 8 * kBlock) pump(1);
  CHECK(!s.sessionBusy());
  int waited = 0; for (; waited < 400 && !s.sessionBusy(); waited++) pump(1);
  CHECK(s.sessionBusy());
  std::printf("  second write began %.2f s after the first completed\n", (eng().status().sampleClock - t0) / (double)kSampleRate);
  CHECK((int32_t)(eng().status().sampleClock - t0) >= Sets::kSessionSpacingSamples);
  pumpSession(4000);
  // a change made while the write runs is written by the next one, 15 s later
  s.markSessionDirty();
  pump(Sets::kSessionDebounceSamples / kBlock + 400);
  CHECK(!s.sessionBusy());
  CHECK(awaitSessionWrite());
}

// SPEC §9.19 §3 a: Sets::step() does no card work while a consumed stream ring is below its lead; the pump tops it up.
TEST(sets_step_waits_for_the_stream_gate) {
  Sets& s = sets();
  settle(); stopAll();
  CHECK(gen().info(Vocal).file[0] != 0);
  CHECK(startTrack(Vocal)); pump(kSampleRate / kBlock);
  s.markSessionDirty();
  CHECK(awaitSessionWrite(((Sets::kSessionDebounceSamples + Sets::kSessionSpacingSamples) / kBlock + 400)) || true);   // let the spacing pass
  s.markSessionDirty();
  for (int i = 0; i < (Sets::kSessionDebounceSamples + Sets::kSessionSpacingSamples) / kBlock + 400 && !s.sessionBusy(); i++) pump(1);
  CHECK(s.sessionBusy());
  VocalRing& r = eng().vocalRing(0);
  r.r.store(r.w.load() - 300);                                      // the ring lost its lead (a long stall): the gate closes
  CHECK(!gen().streamsHealthy());
  cfs().cur = Counters(); cfs().on = true; s.step(); cfs().on = false;
  CHECK_EQ(cfs().cur.opens, 0); CHECK_EQ(cfs().cur.probes, 0); CHECK_EQ(cfs().cur.walks, 0); CHECK_EQ(cfs().cur.bytesRead + cfs().cur.bytesWritten, 0u);
  CHECK(s.sessionBusy());
  pump(1);                                                          // the refused query made the pump top the ring up
  CHECK(gen().streamsHealthy()); CHECK(r.available() >= Generator::kGateLeadSamples);
  pumpSession(4000);
  CHECK(!s.sessionBusy());
  stopAll();
}

// SPEC §9.19 §3 d: the slow-card model — 150 ms per open / write / exists and 10 ms per read, with the engine
// rendering meanwhile — through a session write with both vocal streams active: the vocal streams never starve (the
// gate admits an op only with 160 ms of lead, the write probes nothing, the top-up after a stall refills the rings a
// chunk at a time). The arithmetic bounds the model: a 186 ms vocal ring survives a 150 ms op only when the refill
// reads that follow drain less than 36 ms — two other rings' reads at 10 ms each; the contract's 20 ms per read would
// need a 190 + ms ring (recorded as a deviation, SPEC §9.19c). The two loop rings (139 ms) cannot hold a 150 ms op at
// all — measured at the review: 10 570 / 10 790 lost samples (≈ 0.48 s) per slot over one write — so the writer keeps
// away from them (Generator::loopSlotStreaming): with a slot streaming a due write waits and one in flight pauses
// (zero card work), it runs once the slot tracks stop, and the loop underruns are asserted zero throughout.
static int g_stallBlocks = 0;
static void stallRender(int ms) {                                    // exactly ms of audio, in blocks of ≤ kBlock (the engine renders what it is asked)
  static int16_t buf[kBlock];
  int left = ms * kSampleRate / 1000;
  while (left > 0) { const int n = left > kBlock ? kBlock : left; eng().render(buf, n); left -= n; g_stallBlocks++; }
}
TEST(slow_card_session_write_starves_no_vocal_stream) {
  Sets& s = sets();
  settle(); stopAll();
  CHECK(gen().info(Vocal).file[0] != 0); CHECK(gen().info(kTracksPerSet + Vocal).file[0] != 0);
  CHECK(startTrack(Vocal)); CHECK(startTrack(kTracksPerSet + Vocal));
  pump(kSampleRate * 2 / kBlock);
  push(CmdSetParam, Pad, PVolume, 0, 0.6f); pump(2);
  CountingFs& fs = cfs();
  const int waitMax = (Sets::kSessionDebounceSamples + Sets::kSessionSpacingSamples) / kBlock + 400;
  // The write (already begun) to its end under the slow card: zero vocal AND zero loop underruns.
  auto runWriteSlow = [&](const char* label) {
    CHECK(s.sessionBusy());
    const uint16_t v0 = eng().status().vocalUnderruns; uint16_t l0[kLoopSlots]; for (int k = 0; k < kLoopSlots; k++) l0[k] = eng().streamUnderruns(kSets + k);
    fs.slowOpenMs = 150; fs.slowReadMs = 10; fs.stall = stallRender; fs.slowOps = 0; g_stallBlocks = 0;
    int steps = 0; const bool dbg = getenv("LP_SLOW_DEBUG") != nullptr; fs.logOps = dbg;
    while (s.sessionBusy() && steps < 4000) {                        // the device pumps first thing after a step (the loop runs several times per block)
      static int16_t buf[kBlock];
      const uint16_t u0 = eng().status().vocalUnderruns; const int ops0 = fs.slowOps;
      gen().step(); gen().pumpVocal();
      const int a0 = eng().vocalRing(0).available(), a1 = eng().vocalRing(1).available(); const bool healthy = gen().streamsHealthy();
      stepSets(); gen().pumpVocal();
      eng().render(buf, kBlock); steps++;
      if (dbg && (eng().status().vocalUnderruns != u0 || fs.slowOps != ops0)) std::printf("    step %d: leads %d %d healthy %d, slow ops +%d, underruns +%u, lead after %d %d\n", steps, a0, a1, (int)healthy, fs.slowOps - ops0, (unsigned)(eng().status().vocalUnderruns - u0), eng().vocalRing(0).available(), eng().vocalRing(1).available());
    }
    fs.slowOpenMs = 0; fs.slowReadMs = 0; fs.stall = nullptr; fs.logOps = false;
    CHECK(!s.sessionBusy());
    std::printf("  %s: %d steps, %d slow card calls (%d render calls inside them), vocal underruns %u, loop underruns %u %u\n", label, steps, fs.slowOps, g_stallBlocks,
                (unsigned)(eng().status().vocalUnderruns - v0), (unsigned)(eng().streamUnderruns(kSets) - l0[0]), (unsigned)(eng().streamUnderruns(kSets + 1) - l0[1]));
    CHECK_EQ((int)(eng().status().vocalUnderruns - v0), 0);
    for (int k = 0; k < kLoopSlots; k++) CHECK_EQ((int)(eng().streamUnderruns(kSets + k) - l0[k]), 0);
    CHECK(fs.slowOps >= 2);
  };
  // 1. both vocal streams active: the write starts after the debounce / spacing and starves neither
  s.markSessionDirty();
  for (int i = 0; i < waitMax && !s.sessionBusy(); i++) pump(1);
  runWriteSlow("both vocals");
  // 2. one vocal and two loop slots streaming: no write starts while a slot ring is consumed (it could not survive a
  //    150 ms open), the loops play on without a lost sample ...
  push(CmdTrackStopNow, kTracksPerSet + Vocal); pump(2);
  CHECK(gen().setLoopMode(Beat, true)); settle(); CHECK(gen().setLoopMode(kTracksPerSet + Percussion, true)); settle();
  CHECK(gen().loopEnabled(Beat) && gen().loopEnabled(kTracksPerSet + Percussion));
  CHECK(startTrack(Beat)); CHECK(startTrack(kTracksPerSet + Percussion));
  pump(kSampleRate * 2 / kBlock);
  for (int k = 0; k < kLoopSlots; k++) CHECK(eng().loopTrack(k) >= 0);
  CHECK(gen().loopSlotStreaming());
  push(CmdSetParam, Pad, PVolume, 0, 0.7f); pump(2);
  s.markSessionDirty();
  uint16_t l0[kLoopSlots]; for (int k = 0; k < kLoopSlots; k++) l0[k] = eng().streamUnderruns(kSets + k);
  const uint16_t v0 = eng().status().vocalUnderruns;
  pump(waitMax);
  CHECK(!s.sessionBusy());                                           // deferred: debounce and spacing long passed, a slot streams
  for (int k = 0; k < kLoopSlots; k++) CHECK_EQ((int)(eng().streamUnderruns(kSets + k) - l0[k]), 0);
  CHECK_EQ((int)(eng().status().vocalUnderruns - v0), 0);
  // ... the slot tracks stop: the write begins within a few blocks; a slot track starting again pauses it where it is
  // (no card work at all while it streams) and it resumes once that track stops ...
  push(CmdTrackStopNow, Beat); push(CmdTrackStopNow, kTracksPerSet + Percussion); pump(2);
  CHECK(!gen().loopSlotStreaming());
  int began = 0; for (; began < 400 && !s.sessionBusy(); began++) pump(1);
  std::printf("  the write began %d blocks after the slot tracks stopped\n", began);
  CHECK(s.sessionBusy());
  CHECK(startTrack(Beat)); CHECK(gen().loopSlotStreaming()); CHECK(s.sessionBusy());
  Counters paused; int pausedSteps = 0;
  for (int n = 0; n < 60; n++) {
    static int16_t buf[kBlock];
    gen().step(); gen().pumpVocal();
    cfs().cur = Counters(); cfs().on = true; s.step(); cfs().on = false;
    paused.probes += cfs().cur.probes; paused.opens += cfs().cur.opens; paused.walks += cfs().cur.walks; paused.bytesRead += cfs().cur.bytesRead; paused.bytesWritten += cfs().cur.bytesWritten;
    eng().render(buf, kBlock); pausedSteps++;
  }
  std::printf("  paused for %d steps while the Beat loop streamed: %d opens, %d probes, %d walks, %zu B read, %zu B written\n", pausedSteps, paused.opens, paused.probes, paused.walks, paused.bytesRead, paused.bytesWritten);
  CHECK_EQ(paused.opens, 0); CHECK_EQ(paused.probes, 0); CHECK_EQ(paused.walks, 0); CHECK_EQ(paused.bytesRead + paused.bytesWritten, 0u);
  CHECK(s.sessionBusy());
  for (int k = 0; k < kLoopSlots; k++) CHECK_EQ((int)(eng().streamUnderruns(kSets + k) - l0[k]), 0);
  push(CmdTrackStopNow, Beat); pump(2);
  CHECK(!gen().loopSlotStreaming());
  // ... and finishes under the slow card with the vocal streaming and both loop files open but idle: no underruns anywhere
  runWriteSlow("one vocal + two idle loop slots (resumed)");
  for (int k = 0; k < kLoopSlots; k++) CHECK_EQ((int)(eng().streamUnderruns(kSets + k) - l0[k]), 0);
  loopOff(Beat); loopOff(kTracksPerSet + Percussion);
  push(CmdSetParam, Pad, PVolume, 0, 0.8f); pump(2);
  stopAll();
  flushSession();
}

// SPEC §9.29: a save streams no copies any more — every step is one probe, one open or one write (the index chunk read
// adds its 10 ms read to the open) — so under the slow-card model of SPEC §9.19 §3 d a save with BOTH vocal streams
// active starves neither: the vocal starvation a save used to cause while its WAV copies streamed (SPEC §9.18) is gone.
TEST(slow_card_save_starves_no_vocal_stream) {
  Sets& s = sets();
  settle(); stopAll();
  CHECK(gen().info(Vocal).file[0] != 0); CHECK(gen().info(kTracksPerSet + Vocal).file[0] != 0);
  CHECK(startTrack(Vocal)); CHECK(startTrack(kTracksPerSet + Vocal));
  pump(kSampleRate * 2 / kBlock);
  CountingFs& fs = cfs();
  const uint16_t v0 = eng().status().vocalUnderruns;
  CHECK(s.beginSave(0)); CHECK(s.saving());
  fs.slowOpenMs = 150; fs.slowReadMs = 10; fs.stall = stallRender; fs.slowOps = 0; g_stallBlocks = 0;
  const bool dbg = getenv("LP_SLOW_DEBUG") != nullptr; fs.logOps = dbg;
  int steps = 0, maxOps = 0, opSteps = 0;
  while (s.busy() && steps < 4000) {                                 // the device pumps first thing after a step
    static int16_t buf[kBlock];
    const uint16_t u0 = eng().status().vocalUnderruns;
    gen().step(); gen().pumpVocal();
    const int a0 = eng().vocalRing(0).available(), a1 = eng().vocalRing(1).available(); const int ops0 = fs.slowOps;
    stepSets();
    const int ops = fs.slowOps - ops0;                               // the step's own card calls (the pump's refill reads are not the save's)
    gen().pumpVocal();
    if (ops > maxOps) maxOps = ops;
    if (ops) opSteps++;
    eng().render(buf, kBlock); steps++;
    if (dbg && (eng().status().vocalUnderruns != u0 || ops)) std::printf("    step %d: leads %d %d, slow ops +%d, underruns +%u\n", steps, a0, a1, ops, (unsigned)(eng().status().vocalUnderruns - u0));
  }
  fs.slowOpenMs = 0; fs.slowReadMs = 0; fs.stall = nullptr; fs.logOps = false;
  CHECK(!s.busy()); CHECK(s.ok()); if (!s.ok()) std::printf("  save error: %s\n", s.lastError());
  const std::string n = s.lastName(); notePreset(n);
  std::printf("  save under the slow card: %d steps, %d slow card calls in %d steps (at most %d in one step, %d render calls inside them), vocal underruns %u\n",
              steps, fs.slowOps, opSteps, maxOps, g_stallBlocks, (unsigned)(eng().status().vocalUnderruns - v0));
  CHECK_EQ((int)(eng().status().vocalUnderruns - v0), 0);
  CHECK(fs.slowOps >= 2); CHECK(maxOps <= 2);                        // the index chunk: one open + its read; everything else one op per step
  CHECK(exists(kPresets + "/" + n)); CHECK(!valueOf(readText(kPresets + "/" + n), "lp.src.6").empty());
  removePreset(n);
  stopAll();
}

// ---------------------------------------------------------------------------------------------- preset manager (SPEC §9.22)
// The fixture lives under a Testpack presets folder these tests create and remove; the House presets are never touched here.

static const std::string kTpPresets = "/lp/packs/Testpack/presets";
static const std::string kTpIndex = kTpPresets + "/_index.txt";
static bool g_tpExisted = false;
static int g_houseGenre = 0;
struct TpPreset { std::string name; int num; int root; std::string label; };   // the fixture as the host knows it
static std::vector<TpPreset> g_tp;

static std::string tpText(const std::string& label, int keyRoot, const std::string& extra = "") {
  std::string t = "schemaVersion=1\nsetIndex=0\nkeyRoot=" + std::to_string(keyRoot) + "\nkeyName=" + std::string(noteName(keyRoot)) + " minor\ngenre=Testpack\n";
  if (!label.empty()) t += "lp.label=" + label + "\n";
  return t + "lp.bpm=120\n" + extra;
}
// The rows the manager must produce for a filter and first index: newest (highest NNN) first, ties by name, the names without a number last.
static std::vector<TpPreset> tpExpected(int filterRoot, int first, int count = Sets::kPresetPage) {
  std::vector<TpPreset> v;
  for (const TpPreset& p : g_tp) if (filterRoot < 0 || p.root == filterRoot) v.push_back(p);
  std::sort(v.begin(), v.end(), [](const TpPreset& a, const TpPreset& b) { return a.num != b.num ? a.num > b.num : a.name > b.name; });
  std::vector<TpPreset> page;
  for (int i = first; i < (int)v.size() && i < first + count; i++) page.push_back(v[i]);
  return page;
}
static int tpMatches(int filterRoot) { int n = 0; for (const TpPreset& p : g_tp) if (filterRoot < 0 || p.root == filterRoot) n++; return n; }
static int listSync(int filterRoot, int first) {                   // beginPresetList + counted steps until done; -1 refused, else the steps taken
  Sets& s = sets();
  if (!s.beginPresetList(filterRoot, first)) return -1;
  CHECK(!s.presetListReady()); CHECK_EQ(s.presetListTotal(), -1); CHECK_EQ(s.presetRowCount(), 0);
  int n = 0; while (s.busy() && n < 100000) { stepSets(); n++; }
  return n;
}
static void checkPage(int filterRoot, int first, const char* what) {
  Sets& s = sets();
  const std::vector<TpPreset> want = tpExpected(filterRoot, first);
  CHECK(s.presetListReady()); CHECK(s.ok());
  CHECK_EQ(s.presetListTotal(), tpMatches(filterRoot));
  CHECK_EQ(s.presetRowCount(), (int)want.size());
  for (int i = 0; i < s.presetRowCount() && i < (int)want.size(); i++) {
    const Sets::PresetRow& r = s.presetRow(i);
    const bool same = std::string(r.name) == want[i].name && std::string(r.label) == want[i].label && r.keyRoot == want[i].root;
    if (!same) std::printf("  %s row %d: got %s '%s' %d, want %s '%s' %d\n", what, i, r.name, r.label, r.keyRoot, want[i].name.c_str(), want[i].label.c_str(), want[i].root);
    CHECK(same);
  }
}
static int rowOf(const std::string& name) { for (int i = 0; i < sets().presetRowCount(); i++) if (name == sets().presetRow(i).name) return i; return -1; }
static std::set<std::string> tpIndexNames() { std::set<std::string> s; std::string t = readText(kTpIndex); size_t p = 0; while (p < t.size()) { size_t e = t.find('\n', p); if (e == std::string::npos) e = t.size(); if (e > p) s.insert(t.substr(p, e - p)); p = e + 1; } return s; }
static int runOp(const char* what) {                                // counted steps of a delete / label rewrite until idle
  Counters before = g_max; g_max = Counters();
  int n = 0; while (sets().busy() && n < 1000) { stepSets(); n++; }
  std::printf("  %s: %d steps; per step max opens %d read %zu B written %zu B renames %d\n", what, n, g_max.opens, g_max.bytesRead, g_max.bytesWritten, g_max.renames);
  CHECK(g_max.opens <= 1); CHECK(g_max.renames <= 1); CHECK(g_max.bytesRead <= (size_t)kSetChunk); CHECK(g_max.bytesWritten <= (size_t)kSetChunk + 128);
  if (g_max.probes < before.probes) g_max.probes = before.probes; if (g_max.opens < before.opens) g_max.opens = before.opens;
  if (g_max.entries < before.entries) g_max.entries = before.entries; if (g_max.walks < before.walks) g_max.walks = before.walks;
  if (g_max.bytesRead < before.bytesRead) g_max.bytesRead = before.bytesRead; if (g_max.bytesWritten < before.bytesWritten) g_max.bytesWritten = before.bytesWritten;
  if (g_max.renames < before.renames) g_max.renames = before.renames;
  return n;
}

TEST(preset_manager_lists_newest_first_with_labels_and_pages) {
  Sets& s = sets();
  flushSession();                                                   // nothing dirty: no session write starts under these tests (the 15 s spacing)
  g_houseGenre = gen().genreIndex();
  int tp = -1; for (int i = 0; i < lib().genreCount(); i++) if (strcmp(lib().genreName(i), "Testpack") == 0) tp = i;
  CHECK(tp >= 0); if (tp < 0) return;
  gen().setGenre(tp); CHECK(strcmp(gen().genreName(), "Testpack") == 0);
  g_tpExisted = exists(kTpPresets);
  CHECK(!g_tpExisted);                                              // the card image has no Testpack presets (load_no_match relies on that too)
  // a genre without presets lists nothing
  CHECK(listSync(-1, 0) >= 0); CHECK(s.presetListReady()); CHECK_EQ(s.presetRowCount(), 0); CHECK_EQ(s.presetListTotal(), 0);
  // the fixture: 36 numbered presets (every third one E minor, the rest A minor, some labelled) and one hand-made file without a number
  CHECK(sd().mkdir(kTpPresets.c_str()));
  g_tp.clear(); std::string idx;
  for (int i = 1; i <= 36; i++) {
    const bool emin = i % 3 == 0; const int root = emin ? 4 : 9;
    char n[64]; snprintf(n, sizeof n, "Testpack_%s_%03d.lpset", emin ? "Emin" : "Amin", i);
    const std::string label = i % 5 == 0 ? "Tag " + std::to_string(i) : (i == 7 ? "Drop_7" : "");
    writeText(kTpPresets + "/" + n, tpText(label, root));
    g_tp.push_back({n, i, root, label}); idx = std::string(n) + "\n" + idx;   // the index in reverse order: the row order never comes from it
  }
  writeText(kTpPresets + "/hand_made.lpset", tpText("", 9)); g_tp.push_back({"hand_made.lpset", -1, -1, ""}); idx += "hand_made.lpset\n";
  writeText(kTpIndex, idx);
  // page 0 of everything, then page 1 as a continuation (one pass) and page 2 from scratch (three passes)
  Counters before = g_max; g_max = Counters();
  const int steps0 = listSync(-1, 0); CHECK(steps0 > 0); checkPage(-1, 0, "page 0");
  std::printf("  page 0: %d steps; per step max opens %d walks %d read %zu B\n", steps0, g_max.opens, g_max.walks, g_max.bytesRead);
  CHECK_EQ(g_max.walks, 0); CHECK(g_max.opens <= 1); CHECK(g_max.bytesRead <= (size_t)kSetChunk);
  const int steps1 = listSync(-1, 16); CHECK(steps1 > 0); checkPage(-1, 16, "page 1 (follow)");
  const int steps2 = listSync(-1, 32); CHECK(steps2 > 0); checkPage(-1, 32, "page 2");   // 5 rows: 4 numbered + the hand-made file last
  CHECK_EQ(s.presetRowCount(), 5); CHECK(std::string(s.presetRow(4).name) == "hand_made.lpset"); CHECK_EQ((int)s.presetRow(4).keyRoot, -1);
  const int steps1b = listSync(-1, 16); CHECK(steps1b > 0); checkPage(-1, 16, "page 1 (scratch)");   // not the page after page 2: two passes
  CHECK(steps1 < steps1b);
  std::printf("  page 1: %d steps following page 0, %d steps from scratch; page 2: %d steps\n", steps1, steps1b, steps2);
  // an unaligned first index, and one past the end
  CHECK(listSync(-1, 5) > 0); checkPage(-1, 5, "first 5");
  CHECK(listSync(-1, 30) > 0); checkPage(-1, 30, "first 30");
  CHECK(listSync(-1, 60) > 0); CHECK(s.presetListReady()); CHECK_EQ(s.presetRowCount(), 0); CHECK_EQ(s.presetListTotal(), 37);
  // the key filter: A minor (24), E minor (12), C# minor (none); the hand-made file carries no key label and never matches a key
  CHECK(listSync(9, 0) > 0); checkPage(9, 0, "Amin page 0"); CHECK(listSync(9, 16) > 0); checkPage(9, 16, "Amin page 1");
  CHECK(listSync(4, 0) > 0); checkPage(4, 0, "Emin");
  CHECK(listSync(1, 0) > 0); CHECK_EQ(s.presetRowCount(), 0); CHECK_EQ(s.presetListTotal(), 0);
  // without an index the folder is walked in pages (the fallback's known re-walk cost is not held to the strict budget)
  sd().remove(kTpIndex.c_str());
  g_strict = false;
  CHECK(listSync(-1, 0) > 0); checkPage(-1, 0, "page 0 (walk)");
  CHECK(listSync(-1, 16) > 0); checkPage(-1, 16, "page 1 (walk)");
  CHECK(listSync(9, 5) > 0); checkPage(9, 5, "Amin first 5 (walk)");
  g_strict = true;
  writeText(kTpIndex, idx);
  // refused while a listing is in flight; the rewrites too
  CHECK(s.beginPresetList(-1, 0)); CHECK(s.busy()); CHECK(s.presetBusy()); CHECK(!s.loading()); CHECK(!s.saving());
  CHECK(!s.beginPresetList(-1, 16)); CHECK(std::string(s.lastError()) == "BUSY");
  CHECK(!s.deletePreset("Testpack_Amin_001.lpset")); CHECK(!s.beginSetLabel("Testpack_Amin_001.lpset", "x"));
  while (s.busy()) stepSets();
  checkPage(-1, 0, "page 0 again");
  if (g_max.probes < before.probes) g_max.probes = before.probes; if (g_max.opens < before.opens) g_max.opens = before.opens;
  if (g_max.entries < before.entries) g_max.entries = before.entries; if (g_max.walks < before.walks) g_max.walks = before.walks;
  if (g_max.bytesRead < before.bytesRead) g_max.bytesRead = before.bytesRead; if (g_max.bytesWritten < before.bytesWritten) g_max.bytesWritten = before.bytesWritten;
  CHECK(sd().openCount() <= 2);
}

TEST(preset_manager_named_load_delete_and_label) {
  Sets& s = sets();
  CHECK(strcmp(gen().genreName(), "Testpack") == 0); if (strcmp(gen().genreName(), "Testpack") != 0) return;
  // a named load brings this very file, whatever a random pick would say; its label is parsed; the rows are gone (a load owns names_)
  Rng rng(77);
  CHECK(s.beginLoadNamed(0, "Testpack_Emin_015.lpset", rng));
  for (int n = 0; s.busy() && s.lastName()[0] == 0 && n < 100000; n++) stepSets();
  CHECK(std::string(s.lastName()) == "Testpack_Emin_015.lpset");
  settle(); CHECK(s.ok()); CHECK(std::string(s.file().label) == "Tag 15"); CHECK_EQ(s.file().keyRoot, 4);
  CHECK(!s.presetListReady()); CHECK_EQ(s.presetRowCount(), 0); CHECK_EQ(s.presetListTotal(), -1);
  CHECK(!s.beginLoadNamed(0, "../House_Amin_001.lpset", rng)); CHECK(std::string(s.lastError()) == "BAD NAME");
  CHECK(s.beginLoadNamed(0, "Testpack_Amin_999.lpset", rng));      // a name that is not there fails as a stale index entry does
  while (s.busy()) stepSets();
  CHECK(!s.ok()); CHECK(std::string(s.lastError()) == "OPEN SET"); CHECK(exists(kTpIndex));   // the index was not consulted, so it stays
  // delete: the file and its index line go, the rows and the total follow, nothing else moves
  CHECK(listSync(-1, 0) > 0); checkPage(-1, 0, "before delete");
  const int filesBefore = (int)listDir(kTpPresets, ".lpset").size();
  CHECK(!s.deletePreset("House_Amin_001.lpset")); CHECK(std::string(s.lastError()) == "OTHER GENRE");
  CHECK(!s.deletePreset("Testpack_Amin_001.txt")); CHECK(std::string(s.lastError()) == "BAD NAME");
  CHECK(!s.deletePreset("sub/Testpack_Amin_001.lpset")); CHECK(std::string(s.lastError()) == "BAD NAME");
  CHECK(!s.deletePreset(nullptr)); CHECK(!s.deletePreset(""));
  writeText(kTpPresets + "/Testpack_Amin_034_t2.mid", "MThd"); writeText(kTpPresets + "/Testpack_Amin_034_t7.mid", "MThd");   // the preset's own SMFs (SPEC §9.29) ...
  writeText(kTpPresets + "/Testpack_Amin_033_t2.mid", "MThd");      // ... and another preset's, which stays
  CHECK(s.deletePreset("Testpack_Amin_034.lpset"));                 // row 2 of the page
  CHECK(s.busy()); CHECK(s.presetListReady());                      // the rows stay valid through the rewrite
  runOp("delete");
  CHECK(s.ok()); CHECK(!exists(kTpPresets + "/Testpack_Amin_034.lpset")); CHECK(!exists(kTpPresets + "/_index.tmp"));
  CHECK(!exists(kTpPresets + "/Testpack_Amin_034_t2.mid")); CHECK(!exists(kTpPresets + "/Testpack_Amin_034_t7.mid")); CHECK(exists(kTpPresets + "/Testpack_Amin_033_t2.mid"));
  sd().remove((kTpPresets + "/Testpack_Amin_033_t2.mid").c_str());
  CHECK_EQ((int)listDir(kTpPresets, ".lpset").size(), filesBefore - 1);
  { std::set<std::string> onDisk; for (auto& f : listDir(kTpPresets, ".lpset")) onDisk.insert(f); CHECK(tpIndexNames() == onDisk); CHECK_EQ((int)onDisk.size(), 36); }
  g_tp.erase(std::remove_if(g_tp.begin(), g_tp.end(), [](const TpPreset& p) { return p.name == "Testpack_Amin_034.lpset"; }), g_tp.end());
  CHECK(s.presetListReady()); CHECK_EQ(s.presetRowCount(), 15); CHECK_EQ(s.presetListTotal(), 36);   // the row left the page (no re-listing: the page is one short)
  { const std::vector<TpPreset> want = tpExpected(-1, 0); for (int i = 0; i < 15 && i < (int)want.size(); i++) CHECK(std::string(s.presetRow(i).name) == want[i].name); }
  CHECK(s.deletePreset("Testpack_Amin_034.lpset")); while (s.busy()) stepSets();   // gone already: fails, the index is left alone
  CHECK(!s.ok()); CHECK(std::string(s.lastError()) == "NO PRESET"); CHECK_EQ((int)tpIndexNames().size(), 36);
  // label: replaced, removed and inserted right after genre=, everything else byte for byte; the row follows
  const std::string f1 = "Testpack_Amin_035.lpset"; const std::string orig1 = readText(kTpPresets + "/" + f1);
  CHECK(orig1.find("lp.label=Tag 35\n") != std::string::npos); CHECK(rowOf(f1) >= 0);
  CHECK(!s.beginSetLabel(f1.c_str(), "too long label here!")); CHECK(std::string(s.lastError()) == "BAD LABEL");
  CHECK(!s.beginSetLabel(f1.c_str(), "bad=key")); CHECK(!s.beginSetLabel(f1.c_str(), nullptr)); CHECK(!s.beginSetLabel("House_Amin_001.lpset", "x"));
  CHECK(!Sets::validLabel("twenty characters!!!")); CHECK(Sets::validLabel("Nineteen chars ok 1")); CHECK(Sets::validLabel("")); CHECK(!Sets::validLabel("a:b")); CHECK(!Sets::validLabel("tab\there"));
  { char lb[kSetLabelMax]; Sets::copyLabelText(lb, "My Label! (v2) and more text"); CHECK(std::string(lb) == "My Label v2 and mor"); }
  auto relabel = [&](const std::string& file, const char* label, const std::string& want, const char* what) {
    CHECK(s.beginSetLabel(file.c_str(), label)); runOp(what);
    CHECK(s.ok()); CHECK(!exists(kTpPresets + "/" + file + ".tmp"));
    const std::string got = readText(kTpPresets + "/" + file);
    if (got != want) std::printf("  %s: got\n%s\n  want\n%s\n", what, got.c_str(), want.c_str());
    CHECK(got == want);
  };
  std::string noLabel = orig1; noLabel.erase(noLabel.find("lp.label=Tag 35\n"), strlen("lp.label=Tag 35\n"));
  auto withLabel = [&](const std::string& base, const std::string& label) { std::string t = base; const size_t g = t.find("genre=Testpack\n"); CHECK(g != std::string::npos); t.insert(g + strlen("genre=Testpack\n"), "lp.label=" + label + "\n"); return t; };
  relabel(f1, "Intro A", withLabel(noLabel, "Intro A"), "label replace");
  CHECK(std::string(s.presetRow(rowOf(f1)).label) == "Intro A"); CHECK(s.presetListReady());
  relabel(f1, "", noLabel, "label remove");
  CHECK(std::string(s.presetRow(rowOf(f1)).label) == "");
  relabel(f1, "Intro A", withLabel(noLabel, "Intro A"), "label insert");
  CHECK(std::string(s.presetRow(rowOf(f1)).label) == "Intro A");
  for (TpPreset& p : g_tp) if (p.name == f1) p.label = "Intro A";
  // a file longer than one chunk whose old label line straddles the 4 KB boundary, then lines after it
  const std::string f2 = "Testpack_Amin_032.lpset";
  std::string big = "schemaVersion=1\nsetIndex=0\nkeyRoot=9\nkeyName=A minor\ngenre=Testpack\n";
  for (int k = 0; big.size() + 48 < 4089; k++) big += "# filler line " + std::to_string(k) + " ..........................\n";
  while (big.size() < 4089) big += "#";
  big += "\n";
  CHECK_EQ((int)big.size(), kSetChunk - 6);
  big += "lp.label=Old\n";                                          // bytes 4090..4102: split by the first read
  for (int k = 0; k < 160; k++) big += "# more filler " + std::to_string(k) + " ...................\n";
  big += "lp.bpm=120\nlp.track.1.sat=0.700\nlp.track.1.accent=0.600\nlp.track.1.chorus=0.250\n";
  CHECK(big.size() > 2 * (size_t)kSetChunk);
  writeText(kTpPresets + "/" + f2, big);
  std::string bigNoLabel = big; bigNoLabel.erase(bigNoLabel.find("lp.label=Old\n"), strlen("lp.label=Old\n"));
  relabel(f2, "New", withLabel(bigNoLabel, "New"), "label big");
  for (TpPreset& p : g_tp) if (p.name == f2) p.label = "New";
  { SetFile f; const std::string t = readText(kTpPresets + "/" + f2); parseSetFileText(t.c_str(), (int)t.size(), f);
    CHECK(std::string(f.label) == "New"); CHECK_NEAR(f.track[1].sat, 0.7f, 1e-4); CHECK_NEAR(f.track[1].accent, 0.6f, 1e-4); CHECK_NEAR(f.track[1].chorus, 0.25f, 1e-4);
    CHECK((f.track[1].has & SetFileTrack::HasSat) && (f.track[1].has & SetFileTrack::HasAccent) && (f.track[1].has & SetFileTrack::HasChorus)); }
  // no genre line and no trailing newline: the label is appended on a line of its own; an old label as the last, unterminated line is dropped
  const std::string f3 = "Testpack_Amin_002.lpset";
  writeText(kTpPresets + "/" + f3, "schemaVersion=1\nkeyRoot=9\nlp.bpm=120");
  relabel(f3, "End", "schemaVersion=1\nkeyRoot=9\nlp.bpm=120\nlp.label=End\n", "label append");
  writeText(kTpPresets + "/" + f3, "schemaVersion=1\ngenre=Testpack\nlp.bpm=120\nlp.label=Old");
  relabel(f3, "Fresh", "schemaVersion=1\ngenre=Testpack\nlp.label=Fresh\nlp.bpm=120\n", "label last line");
  writeText(kTpPresets + "/" + f3, tpText("", 9));
  // a missing file fails without leaving a copy; the parser keeps only the allowed characters of a label
  CHECK(s.beginSetLabel("Testpack_Amin_999.lpset", "x")); while (s.busy()) stepSets(); CHECK(!s.ok()); CHECK(std::string(s.lastError()) == "NO PRESET");
  CHECK(!exists(kTpPresets + "/Testpack_Amin_999.lpset.tmp"));
  { SetFile f; const std::string t = "schemaVersion=1\nlp.label = Hello, World (2)!\n"; parseSetFileText(t.c_str(), (int)t.size(), f); CHECK(std::string(f.label) == "Hello World 2"); }
  // the page lists the new labels
  CHECK(listSync(-1, 0) > 0); checkPage(-1, 0, "after labels");
  // the writer emits the label on later saves: set 0 was loaded from Testpack_Emin_015 ("Tag 15"), so its session file (and a
  // save of it) carries lp.label; relabelling that very preset moves the set's label along
  auto newestSessionText = [&](int set) {
    std::string best; uint32_t bestSeq = 0;
    for (int slot = 0; slot < 2; slot++) { const std::string t = readText(sessionPath(set, slot)); const uint32_t seq = (uint32_t)strtoul(valueOf(t, "lp.seq").c_str(), nullptr, 10); if (!t.empty() && (best.empty() || seq > bestSeq)) { best = t; bestSeq = seq; } }
    return best;
  };
  flushSession();
  CHECK(valueOf(newestSessionText(0), "lp.label") == "Tag 15");
  CHECK(s.beginSetLabel("Testpack_Emin_015.lpset", "Loaded X")); runOp("label the loaded preset");
  CHECK(s.ok()); CHECK(hasLine(readText(kTpPresets + "/Testpack_Emin_015.lpset"), "lp.label=Loaded X"));
  for (TpPreset& p : g_tp) if (p.name == "Testpack_Emin_015.lpset") p.label = "Loaded X";
  flushSession();
  CHECK(valueOf(newestSessionText(0), "lp.label") == "Loaded X");
  { SetFile f; const std::string t = newestSessionText(0); parseSetFileText(t.c_str(), (int)t.size(), f); CHECK(std::string(f.label) == "Loaded X"); }
  CHECK(sd().openCount() <= 2);
}

// The label rewrite's last step, <name>.tmp renamed over the original, never leaves the card without a copy (SPEC §9.22):
// a rename that fails with the original still there is retried kRenameRetries steps and then drops the .tmp (the label
// change is lost, the preset is not); SdFs's shape — the destination unlinked, then the rename failing — leaves the .tmp
// as the ONLY copy, which stays and is renamed back (healTmp) by the next listing or load that finds the original missing.
TEST(label_rewrite_survives_a_failed_rename_and_a_stray_tmp_is_healed) {
  Sets& s = sets(); CountingFs& fs = cfs();
  CHECK(strcmp(gen().genreName(), "Testpack") == 0); if (strcmp(gen().genreName(), "Testpack") != 0) return;
  const std::string f = "Testpack_Amin_031.lpset"; const std::string path = kTpPresets + "/" + f;   // an A-minor row of page 0 no other test rewrites (036 is E minor)
  const std::string before = readText(path); CHECK(!before.empty());
  // 1. a plain refusal (the original stays): three tries, then the .tmp goes and the original is byte for byte what it was
  fs.failRenames = 3; fs.unlinkOnFailedRename = false;
  CHECK(s.beginSetLabel(f.c_str(), "Lost")); const int n1 = runOp("label, rename refused");
  CHECK(!s.ok()); CHECK(std::string(s.lastError()) == "RENAME"); CHECK_EQ(fs.failRenames, 0);
  CHECK(readText(path) == before); CHECK(!exists(path + ".tmp"));
  CHECK(n1 >= 6);                                                   // open src, open dst, the copy, three rename steps
  // 2. the destination unlinked and the rename failing (the card between SdFs::rename's two steps): the .tmp is the preset now
  fs.failRenames = 3; fs.unlinkOnFailedRename = true;
  CHECK(s.beginSetLabel(f.c_str(), "Kept")); runOp("label, destination unlinked");
  CHECK(!s.ok()); CHECK(std::string(s.lastError()) == "RENAME"); CHECK_EQ(fs.failRenames, 0);
  fs.unlinkOnFailedRename = false;
  CHECK(!exists(path)); CHECK(exists(path + ".tmp"));
  { SetFile p; const std::string t = readText(path + ".tmp"); parseSetFileText(t.c_str(), (int)t.size(), p); CHECK(std::string(p.label) == "Kept"); }
  // 3. the listing heals it: the row's open fails, the .tmp is renamed back (one probe + one rename in that step, the strict
  //    per-step budget holds) and the row is re-read with the label the rewrite gave it
  for (TpPreset& p : g_tp) if (p.name == f) p.label = "Kept";
  CHECK(listSync(-1, 0) > 0);
  CHECK(exists(path)); CHECK(!exists(path + ".tmp"));
  CHECK(hasLine(readText(path), "lp.label=Kept"));
  checkPage(-1, 0, "healed page");
  // 4. the loader heals it too (a stray .tmp made by hand): the named load opens the healed original next step
  CHECK(::rename(full(path).c_str(), full(path + ".tmp").c_str()) == 0);
  CHECK(!exists(path)); CHECK(exists(path + ".tmp"));
  Rng rng(5);
  CHECK(s.beginLoadNamed(0, f.c_str(), rng)); settle();              // settle pumps the engine too: the load's swap wait needs its blocks
  CHECK(s.ok()); CHECK(std::string(s.lastName()) == f); CHECK(std::string(s.file().label) == "Kept");
  CHECK(exists(path)); CHECK(!exists(path + ".tmp"));
  // 5. a rename that fails once and then works: the retry on the next step succeeds, nothing is lost
  fs.failRenames = 1;
  CHECK(s.beginSetLabel(f.c_str(), "Third")); runOp("label, one failed rename");
  CHECK(s.ok()); CHECK_EQ(fs.failRenames, 0); CHECK(hasLine(readText(path), "lp.label=Third")); CHECK(!exists(path + ".tmp"));
  for (TpPreset& p : g_tp) if (p.name == f) p.label = "Third";
  CHECK(sd().openCount() <= 2);
}

// Preset bank layout (research §4, review 2026-10-16): files written before the generated banks carry no lp.banks line
// and their '303' indices (Chord 10..17, Lead / Pad / Bass 9..16) are remapped on load onto the rows that moved behind
// the 128 generated library rows — the same sound; library indices stay; a file with lp.banks=2 is taken verbatim; the
// writer puts lp.banks=2 into every file it writes now (the session file here).
TEST(old_layout_acid_indices_remap_on_load_and_new_files_carry_lp_banks) {
  Sets& s = sets();
  CHECK(strcmp(gen().genreName(), "Testpack") == 0); if (strcmp(gen().genreName(), "Testpack") != 0) return;
  const int libLead = libraryPresetCount(Lead), libBass = libraryPresetCount(Bass), libChord = libraryPresetCount(Chord);
  CHECK(libLead > 16 && libBass > 16 && libChord > 17);             // the generated banks are in: the old '303' range is library now
  CHECK(isAcidPreset(Lead, libLead + 3) && !isAcidPreset(Lead, 12));
  const std::string oldName = "Testpack_Amin_041.lpset", newName = "Testpack_Amin_042.lpset";
  const std::string tracks = "lp.track.1.preset=12\nlp.track.1.acid=1\nlp.track.1.seed=5\nlp.track.0.preset=7\nlp.track.0.seed=5\nlp.track.3.preset=9\nlp.track.3.seed=5\nlp.track.2.preset=17\nlp.track.2.seed=5\n";
  writeText(kTpPresets + "/" + oldName, tpText("", 9, tracks));                 // pre-wave: Lead '303' row 3 (old 12), Chord library 7, Bass '303' row 0 (old 9), Pad 17 (beyond the old bank: untouched)
  writeText(kTpPresets + "/" + newName, tpText("", 9, "lp.banks=2\n" + tracks));   // written since: verbatim
  g_tp.push_back({oldName, 41, 9, ""}); g_tp.push_back({newName, 42, 9, ""});
  { std::string idx = readText(kTpIndex); idx += oldName + "\n" + newName + "\n"; writeText(kTpIndex, idx); }
  Rng rng(3);
  CHECK(s.beginLoadNamed(0, oldName.c_str(), rng)); settle(); CHECK(s.ok());
  CHECK_EQ((int)s.file().banks, 0);
  CHECK_EQ((int)s.file().track[Lead].preset, libLead + 3); CHECK_EQ((int)gen().info(Lead).presetIndex, libLead + 3); CHECK(isAcidPreset(Lead, gen().info(Lead).presetIndex));
  CHECK_EQ((int)gen().info(Chord).presetIndex, 7);
  CHECK_EQ((int)gen().info(Bass).presetIndex, libBass); CHECK(isAcidPreset(Bass, gen().info(Bass).presetIndex));
  CHECK_EQ((int)gen().info(Pad).presetIndex, 17);
  CHECK(s.beginLoadNamed(0, newName.c_str(), rng)); settle(); CHECK(s.ok());
  CHECK_EQ((int)s.file().banks, 2);
  CHECK_EQ((int)gen().info(Lead).presetIndex, 12); CHECK_EQ((int)gen().info(Bass).presetIndex, 9); CHECK_EQ((int)gen().info(Chord).presetIndex, 7);
  { SetFile f; const std::string t = "schemaVersion=1\nlp.banks=2\n"; parseSetFileText(t.c_str(), (int)t.size(), f); CHECK_EQ((int)f.banks, 2); }
  { SetFile f; const std::string t = "schemaVersion=1\nlp.bpm=120\n"; parseSetFileText(t.c_str(), (int)t.size(), f); CHECK_EQ((int)f.banks, 0); }
  flushSession();
  CHECK(valueOf(newestSessionText(0), "lp.banks") == "2");
  CHECK(sd().openCount() <= 2);
}

// SPEC §9.29: a preset saved before the REAPER export was removed — clipType, clip=assets/<basename> relative to the
// preset folder (the copies in presets/assets/), track.i.slot, track.7.beatLayer / beatKickMuted — still loads through
// the same reader: the clips resolve against presets/assets/, the variant and the kick cut come from the legacy keys, an
// unsafe clip (".." or absolute) leaves its track alone. A save after that load references the assets by their path (a
// legacy copy is as permanent as the library: no SMF of its own) and writes no clip keys.
TEST(legacy_preset_with_asset_copies_still_loads) {
  Sets& s = sets();
  CHECK(strcmp(gen().genreName(), "Testpack") == 0); if (strcmp(gen().genreName(), "Testpack") != 0) return;
  settle(); stopAll();
  auto firstFile = [](const std::string& folder, const char* ext) {     // the first file of a folder, flat or sharded
    std::string r;
    for (auto& f : listDir(folder, ext)) { r = folder + "/" + f; break; }
    if (r.empty()) for (auto& d : listDir(folder, nullptr)) { for (auto& f : listDir(folder + "/" + d, ext)) { r = folder + "/" + d + "/" + f; break; } if (!r.empty()) break; }
    return r;
  };
  const std::string chordSrc = firstFile("/lp/midi/Chord/Amin", ".mid"), beatSrc = firstFile("/lp/midi/Beat/NA", ".mid");
  std::string vocalSrc; for (auto& d : listDir("/lp/packs/House/vocal", nullptr)) { vocalSrc = firstFile("/lp/packs/House/vocal/" + d, ".wav"); if (!vocalSrc.empty() && Sets::bpmToken(vocalSrc.c_str()) > 0) break; vocalSrc.clear(); }
  CHECK(!chordSrc.empty()); CHECK(!beatSrc.empty()); CHECK(!vocalSrc.empty());
  if (chordSrc.empty() || beatSrc.empty() || vocalSrc.empty()) return;
  const std::string assets = kTpPresets + "/assets";
  CHECK(sd().mkdir(assets.c_str()));
  const std::string chordBase = baseOf(chordSrc), beatBase = baseOf(beatSrc), vocalBase = baseOf(vocalSrc);
  writeText(assets + "/" + chordBase, readText(chordSrc)); writeText(assets + "/" + beatBase, readText(beatSrc));
  { std::vector<uint8_t> v = readAll(vocalSrc); FILE* f = fopen(full(assets + "/" + vocalBase).c_str(), "wb"); if (f) { fwrite(v.data(), 1, v.size(), f); fclose(f); } }
  const std::string kitLine = valueOf(readText(kPresets + "/House_Amin_001.lpset"), "lp.track.7.kit");   // the card's own House preset: absolute one-shot paths (read only)
  const std::string legacy = "Testpack_Amin_060.lpset";
  const std::string text =
    "schemaVersion=1\nsetIndex=0\nkeyRoot=9\nkeyName=A minor\ngenre=Testpack\nlp.banks=2\n"
    "track.0.category=CHORD\ntrack.0.clipType=MIDI\ntrack.0.clip=assets/" + chordBase + "\ntrack.0.slot=2\n"
    "track.1.category=LEAD\ntrack.1.clipType=MIDI\ntrack.1.clip=../../House/presets/assets/" + chordBase + "\ntrack.1.slot=1\n"   // unsafe: ignored
    "track.2.category=PAD\ntrack.2.clipType=MIDI\ntrack.2.clip=" + chordSrc + "\ntrack.2.slot=1\n"                                 // absolute: ignored
    "track.6.category=VOCAL\ntrack.6.clipType=WAV\ntrack.6.clip=assets/" + vocalBase + "\ntrack.6.slot=0\n"
    "track.7.category=BEAT\ntrack.7.clipType=MIDI\ntrack.7.clip=assets\\" + beatBase + "\ntrack.7.slot=3\ntrack.7.beatLayer=3\ntrack.7.beatKickMuted=true\n"
    "lp.bpm=120\nlp.track.0.seed=4242\nlp.track.0.len=4\nlp.track.0.key=9\nlp.track.0.lenk=0\nlp.track.0.preset=3\n"
    "lp.track.6.seed=7\nlp.track.6.len=4\nlp.track.6.key=9\nlp.track.6.lenk=0\nlp.track.6.pause=1\n"
    "lp.track.7.seed=9\nlp.track.7.len=4\nlp.track.7.key=9\nlp.track.7.lenk=0\nlp.track.7.kit=" + kitLine + "\n";
  writeText(kTpPresets + "/" + legacy, text);
  { std::string idx = readText(kTpIndex); idx += legacy + "\n"; writeText(kTpIndex, idx); }
  Pattern chordNotes; { IFile* f = sd().open(chordSrc.c_str(), "r"); CHECK(f != nullptr); if (f) { CHECK(smfParse(*f, chordNotes)); f->close(); } }
  scramble();
  Rng rng(606);
  CHECK(s.beginLoadNamed(0, legacy.c_str(), rng)); settle();
  CHECK(s.ok()); if (!s.ok()) std::printf("  legacy load: %s\n", s.lastError());
  CHECK(std::string(s.file().src[Chord]) == "assets/" + chordBase); CHECK(std::string(s.file().src[Beat]) == "assets\\" + beatBase);   // as written; resolved at the load
  CHECK(std::string(gen().info(Chord).file) == chordBase); CHECK_EQ((long long)gen().info(Chord).seed, 4242LL); CHECK_EQ((int)gen().info(Chord).variant, 2);
  CHECK_EQ((int)eng().status().track[Chord].variant, 2);
  { const Pattern& now = eng().sourcePattern(Chord); bool same = now.count == chordNotes.count; for (int i = 0; same && i < now.count; i++) same = now.notes[i].packed == chordNotes.notes[i].packed; CHECK(same); }
  CHECK(std::string(gen().info(Lead).file) == "scrambled"); CHECK(std::string(gen().info(Pad).file) == "scrambled");   // the unsafe clips left their tracks alone
  CHECK(std::string(gen().info(Vocal).file) == vocalBase); CHECK(std::string(gen().vocalPath(0, 0)) == assets + "/" + vocalBase);
  CHECK(std::string(gen().info(Beat).file) == beatBase); CHECK_EQ((int)gen().info(Beat).variant, 3); CHECK_EQ((int)eng().status().track[Beat].variant, 3);
  CHECK_EQ((int)eng().status().track[Beat].kickCut, 1);
  CHECK(eng().kit(0).slot[0].present == 1);
  for (int t : {Lead, Pad, Bass, Fx, Percussion}) { gen().info(t) = TrackGenInfo(); eng().sourcePattern(t).clear(); }   // tracks the file did not carry are empty on a real boot (the scramble above is not)
  // a save of the loaded set: the assets referenced by their paths, no clip keys, no SMF of its own
  char name[64] = ""; const bool savedOk = saveSync(0, name, sizeof name); CHECK(savedOk); if (!savedOk) std::printf("  save after the legacy load: %s\n", s.lastError());
  const std::string n = name, saved = readText(kTpPresets + "/" + n);
  CHECK(valueOf(saved, "lp.src.0") == assets + "/" + chordBase); CHECK(valueOf(saved, "lp.src.6") == assets + "/" + vocalBase); CHECK(valueOf(saved, "lp.src.7") == assets + "/" + beatBase);
  CHECK(valueOf(saved, "lp.src.1").empty()); CHECK(valueOf(saved, "lp.src.2").empty());   // (scrambled: no notes)
  CHECK(valueOf(saved, "lp.track.0.variant") == "2"); CHECK(valueOf(saved, "lp.track.7.variant") == "3"); CHECK(valueOf(saved, "lp.track.7.kickcut") == "1");
  CHECK(saved.find("clip") == std::string::npos); CHECK(saved.find("track.0.slot") == std::string::npos);
  for (int t = 0; t < kTracksPerSet; t++) CHECK(!exists(smfOf(kTpPresets, n, t)));
  // back to a plain state: set 0 from the suite's own House preset (library paths), then the fixture goes
  push(CmdKickCut, 0, 0, 0); pump(2);
  gen().setGenre(g_houseGenre); CHECK(strcmp(gen().genreName(), "House") == 0);
  CHECK(s.beginLoadNamed(0, g_name1.c_str(), rng)); settle(); CHECK(s.ok());
  int tp = -1; for (int i = 0; i < lib().genreCount(); i++) if (strcmp(lib().genreName(i), "Testpack") == 0) tp = i;
  gen().setGenre(tp); CHECK(strcmp(gen().genreName(), "Testpack") == 0);
  for (auto& f : listDir(assets, nullptr)) sd().remove((assets + "/" + f).c_str());
  CHECK(sd().remove(assets.c_str())); CHECK(!exists(assets));
  sd().remove((kTpPresets + "/" + legacy).c_str()); sd().remove((kTpPresets + "/" + n).c_str());
  for (int t = 0; t < kTracksPerSet; t++) { const std::string m = smfOf(kTpPresets, n, t); if (exists(m)) sd().remove(m.c_str()); }
  { std::string idx; for (auto& f : listDir(kTpPresets, ".lpset")) idx += f + "\n"; writeText(kTpIndex, idx); }
  CHECK(sd().openCount() <= 2);
}

TEST(preset_manager_cleanup) {
  // everything the two tests above created goes; Testpack is left as found (no presets folder) and House is the genre again
  for (auto& f : listDir(kTpPresets, nullptr)) sd().remove((kTpPresets + "/" + f).c_str());
  if (!g_tpExisted) sd().remove(kTpPresets.c_str());
  CHECK(exists(kTpPresets) == g_tpExisted);
  gen().setGenre(g_houseGenre); CHECK(strcmp(gen().genreName(), "House") == 0);
  CHECK(!sets().presetListReady() || sets().presetRowCount() >= 0);   // a stale listing of another genre is not an error; the UI re-lists per genre
  g_tp.clear();
}

// ---------------------------------------------------------------------------------------------- random-load cycle (SPEC §9.25)

static const int kCycleBase = 950;                                   // House_Amin_950 .. 954: numbers no earlier test loaded
static std::vector<std::string> g_cycle;                             // the five fixture presets (removed by the cleanup)
static bool cycleLoad(Rng& rng, std::string& got) {                  // one random load into set 1, settled; false (with the error printed) when it failed
  Sets& s = sets(); char name[64] = "";
  const bool began = s.loadRandom(0, rng, name, sizeof name);
  if (!began) { std::printf("  load refused: %s\n", s.lastError()); return false; }
  settle();
  if (!s.ok()) std::printf("  load of %s failed: %s\n", name, s.lastError());
  got = name;
  return s.ok();
}

// User request 2026-10-16 (SPEC §9.25): a randomly loaded preset does not come up again until every other preset of the
// filter has been loaded into that set. Five fresh A-minor presets (copies of a set the suite saved) are listed alone: five
// random loads hit each once in any order, the sixth starts a new cycle (never the fifth: no immediate repeat, SPEC §9.8),
// presetLoadedInCycle follows every step (the preset manager's dot), a named load marks its preset too, a deleted preset
// never blocks the cycle and a save leaves the new preset's bit clear.
TEST(random_loads_cycle_through_every_preset_before_repeating) {
  Sets& s = sets();
  settle();
  CHECK(strcmp(gen().genreName(), "House") == 0);
  std::string source = exists(kPresets + "/" + g_name1) ? g_name1 : "";
  if (source.empty()) for (auto& f : listDir(kPresets, ".lpset")) if (f.compare(0, 11, "House_Amin_") == 0 && Sets::parsePresetNumber(f.c_str()) < 900) { source = f; break; }
  CHECK(!source.empty()); if (source.empty()) return;
  const std::string text = readText(kPresets + "/" + source);
  CHECK(valueOf(text, "keyRoot") == "9");
  for (int i = 0; i < 5; i++) { char n[64]; snprintf(n, sizeof n, "House_Amin_%03d.lpset", kCycleBase + i); writeText(kPresets + "/" + n, text); note(kPresets + "/" + n); g_cycle.push_back(n); }
  std::string idx; for (auto& n : g_cycle) idx += n + "\n";
  sd().remove(kIndex.c_str()); writeText(kIndex, idx);                // only the five are listed
  auto isFixture = [&](const std::string& n) { return std::find(g_cycle.begin(), g_cycle.end(), n) != g_cycle.end(); };
  auto num = [](const std::string& n) { return Sets::parsePresetNumber(n.c_str()); };
  for (int i = 0; i < 5; i++) CHECK(!s.presetLoadedInCycle(0, kCycleBase + i));   // fresh numbers (the Testpack tests before this one left masks of another genre)
  CHECK(!s.presetLoadedInCycle(0, -1)); CHECK(!s.presetLoadedInCycle(-1, kCycleBase)); CHECK(!s.presetLoadedInCycle(kSets, kCycleBase));
  Rng rng(2025);
  std::set<std::string> seen; std::string last;
  for (int i = 0; i < 5; i++) {
    std::string got; CHECK(cycleLoad(rng, got));
    CHECK(isFixture(got));
    CHECK(seen.insert(got).second);                                   // each preset once before any repeat
    CHECK(s.presetLoadedInCycle(0, num(got)));
    CHECK_EQ((int)seen.size(), i + 1);
    for (auto& n : g_cycle) CHECK_EQ((int)s.presetLoadedInCycle(0, num(n)), (int)(seen.count(n) == 1));
    last = got;
  }
  CHECK_EQ((int)seen.size(), 5);
  CHECK(!s.presetLoadedInCycle(1, kCycleBase));                       // set 2 keeps its own cycle
  // the sixth load starts a new cycle: not the fifth (no immediate repeat), and every bit but its own is clear again
  std::string sixth; CHECK(cycleLoad(rng, sixth));
  CHECK(isFixture(sixth)); CHECK(sixth != last);
  for (auto& n : g_cycle) CHECK_EQ((int)s.presetLoadedInCycle(0, num(n)), (int)(n == sixth));
  std::printf("  five loads: %zu distinct; sixth (new cycle) %s after %s\n", seen.size(), sixth.c_str(), last.c_str());
  // a named load (the preset manager's ent) marks its preset as loaded in this cycle too
  std::string named; for (auto& n : g_cycle) if (n != sixth) { named = n; break; }
  CHECK(s.beginLoadNamed(0, named.c_str(), rng)); settle(); CHECK(s.ok());
  CHECK(std::string(s.lastName()) == named);
  CHECK(s.presetLoadedInCycle(0, num(named))); CHECK(s.presetLoadedInCycle(0, num(sixth)));
  // a deleted preset never blocks the cycle: one of the three unloaded ones goes through Sets::deletePreset (its index line
  // with it); the two unloaded ones left come up next, and the load after them starts a new cycle over the four
  std::string gone; for (auto& n : g_cycle) if (n != sixth && n != named) { gone = n; break; }
  CHECK(s.deletePreset(gone.c_str())); while (s.busy()) stepSets(); CHECK(s.ok());
  CHECK(!exists(kPresets + "/" + gone)); CHECK(indexNames().count(gone) == 0); CHECK_EQ((int)indexNames().size(), 4);
  std::set<std::string> rest; for (auto& n : g_cycle) if (n != sixth && n != named && n != gone) rest.insert(n);
  CHECK_EQ((int)rest.size(), 2);
  for (int i = 0; i < 2; i++) { std::string got; CHECK(cycleLoad(rng, got)); CHECK_EQ((int)rest.erase(got), 1); }
  CHECK(rest.empty());
  std::string again; CHECK(cycleLoad(rng, again));                    // every remaining preset was loaded: a new cycle, the deleted one never waited for
  CHECK(isFixture(again)); CHECK(again != gone);
  int marked = 0; for (auto& n : g_cycle) if (n != gone && s.presetLoadedInCycle(0, num(n))) marked++;
  CHECK_EQ(marked, 1); CHECK(s.presetLoadedInCycle(0, num(again)));
  // deleting a loaded preset clears its bit; a save writes the next number with its bit clear (a candidate at once)
  CHECK(s.deletePreset(again.c_str())); while (s.busy()) stepSets(); CHECK(s.ok());
  CHECK(!s.presetLoadedInCycle(0, num(again)));
  g_cycle.erase(std::remove(g_cycle.begin(), g_cycle.end(), gone), g_cycle.end());
  g_cycle.erase(std::remove(g_cycle.begin(), g_cycle.end(), again), g_cycle.end());
  char savedName[64] = ""; CHECK(saveSync(0, savedName, sizeof savedName)); notePreset(savedName);
  CHECK_EQ(Sets::parsePresetNumber(savedName), kCycleBase + 5);       // max of the index (954) + 1
  CHECK(!s.presetLoadedInCycle(0, kCycleBase + 5));
  g_cycle.push_back(savedName);
  // another genre: the masks are that genre's, so the dots vanish and come back with the genre (no load in between)
  const int g = gen().genreIndex(); gen().setGenre(g + 1);
  CHECK(strcmp(gen().genreName(), "House") != 0);
  for (auto& n : g_cycle) CHECK(!s.presetLoadedInCycle(0, num(n)));
  gen().setGenre(g); CHECK(strcmp(gen().genreName(), "House") == 0);
  int back = 0; for (auto& n : g_cycle) if (s.presetLoadedInCycle(0, num(n))) back++;
  CHECK_EQ(back, 0);                                                  // the cycle restarted with the deletes above: nothing marked right now
  std::string first; CHECK(cycleLoad(rng, first)); CHECK(s.presetLoadedInCycle(0, num(first)));
  rewriteIndex();                                                     // the full listing again (the fixture stays until the cleanup)
}

// SPEC §9.25: the masks are persisted in the session files as lp.loaded.<set>=<hex> (256 hex chars, byte i = presets 8i..8i+7,
// bit n & 7 = preset n) and the restore brings the FILE's masks back; a short or malformed value fills what it can.
TEST(load_cycle_masks_round_trip_through_a_session_write_and_restore) {
  Sets& s = sets();
  settle();
  CHECK(strcmp(gen().genreName(), "House") == 0);
  auto hexBit = [](const std::string& h, int n) { if ((int)h.size() < 2 * (n / 8) + 2) return false; const int byte = (int)strtol(h.substr((size_t)(2 * (n / 8)), 2).c_str(), nullptr, 16); return ((byte >> (n & 7)) & 1) != 0; };
  flushSession();                                                     // the masks of the test above reach the card
  const std::string t0 = newestSessionText(0), t1 = newestSessionText(1);
  const std::string hex0 = valueOf(t0, "lp.loaded.0"), hex1 = valueOf(t1, "lp.loaded.1");
  CHECK_EQ((int)hex0.size(), 2 * Sets::kLoadedPresets / 8); CHECK_EQ((int)hex1.size(), 2 * Sets::kLoadedPresets / 8);
  CHECK(hex0.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(valueOf(t0, "lp.loaded.1").empty());                           // each file carries its own set's mask
  int setBits = 0;
  for (int n = 0; n < Sets::kLoadedPresets; n++) { if (hexBit(hex0, n)) setBits++; CHECK_EQ((int)hexBit(hex0, n), (int)s.presetLoadedInCycle(0, n)); }
  CHECK(setBits >= 1);                                                // the last load of the test above
  CHECK(hex1 == std::string(hex1.size(), '0'));                        // set 2 never loaded anything
  CHECK(!g_cycle.empty());                                            // the fixture of the test above is still listed
  // the RAM masks move on (a load marks another preset); the restore replaces them with the file's
  Rng rng(31); std::string got; CHECK(cycleLoad(rng, got));
  const int gotNum = Sets::parsePresetNumber(got.c_str());
  CHECK(s.presetLoadedInCycle(0, gotNum));
  int differ = 0; for (int n = 0; n < Sets::kLoadedPresets; n++) if (hexBit(hex0, n) != s.presetLoadedInCycle(0, n)) differ++;
  CHECK(differ >= 1);
  for (int t = 0; t < kTracks; t++) push(CmdTrackStopNow, t);
  pump(2);
  // the session writer must not overwrite the file first: the restore is queued right away (the debounce is 5 s)
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate * 2 / kBlock);
  CHECK(!s.sessionBusy()); CHECK(!s.busy());
  for (int n = 0; n < Sets::kLoadedPresets; n++) CHECK_EQ((int)hexBit(hex0, n), (int)s.presetLoadedInCycle(0, n));
  CHECK_EQ((int)s.presetLoadedInCycle(0, gotNum), (int)hexBit(hex0, gotNum));
  std::printf("  lp.loaded.0: %d bits set, restored from the file after a load had set 1 more\n", setBits);
  // a hand-edited newer slot with a short value: presets 0..7 and 8..15 from "ff0f", then "zz" ends it
  const int newest = newestSessionSlot(0); CHECK(newest >= 0); if (newest < 0) return;
  std::string edited = t0;
  const size_t p = edited.find("lp.loaded.0="); CHECK(p != std::string::npos); if (p == std::string::npos) return;
  const size_t e = edited.find('\n', p);
  edited.replace(p, e - p, "lp.loaded.0=ff0fzz00");
  const size_t q = edited.find("lp.seq="); const size_t qe = edited.find('\n', q);
  edited.replace(q, qe - q, "lp.seq=" + std::to_string(s.sessionSeq() + 7));
  writeText(sessionPath(0, newest ^ 1), edited); note(sessionPath(0, newest ^ 1));
  s.restoreSession(); pump(1); pumpSession(4000); pump(kSampleRate * 2 / kBlock);
  CHECK(!s.sessionBusy());
  for (int n = 0; n < 8; n++) CHECK(s.presetLoadedInCycle(0, n));
  for (int n = 8; n < 12; n++) CHECK(s.presetLoadedInCycle(0, n));
  for (int n = 12; n < 16; n++) CHECK(!s.presetLoadedInCycle(0, n));
  for (int n = 16; n < Sets::kLoadedPresets; n++) CHECK(!s.presetLoadedInCycle(0, n));
  // a preset file never carries the key; a load leaves the masks alone apart from its own bit
  std::string one; CHECK(cycleLoad(rng, one));
  CHECK(s.presetLoadedInCycle(0, Sets::parsePresetNumber(one.c_str())));
  for (int n = 0; n < 12; n++) CHECK(s.presetLoadedInCycle(0, n));
  pumpSession(2000);
}

// The recorder's hook (SPEC §9.23 / §9.26, review 2026-10-16): Sets::resetLoadCycle clears the masks AND the per-set "last
// loaded" names (the §9.8 no-immediate-repeat fallback), so two random loads from the same Rng seed, each after a reset,
// pick the SAME preset — a render that starts from the state its recording left behind (the live pick as set 1's last name,
// its bit set) draws over the recording's candidates again. Without the reset the same seed cannot repeat the pick. The
// reset marks the session dirty like any mask change: the next session write carries no masks of this genre (the restore
// then starts from empty ones).
TEST(reset_load_cycle_makes_a_seeded_random_load_repeatable) {
  Sets& s = sets();
  settle();
  CHECK(strcmp(gen().genreName(), "House") == 0);
  int aminor = 0; for (auto& f : listDir(kPresets, ".lpset")) if (Sets::presetKeyRoot(f.c_str()) == 9) aminor++;
  CHECK(aminor >= 2); if (aminor < 2) return;                          // the fixture of the cycle test is still listed
  auto num = [](const std::string& n) { return Sets::parsePresetNumber(n.c_str()); };
  flushSession();                                                     // nothing pending: the write below is the reset's own
  s.resetLoadCycle();
  Rng r1(60616); std::string first; CHECK(cycleLoad(r1, first));
  CHECK(num(first) >= 0); CHECK(s.presetLoadedInCycle(0, num(first)));
  Rng r2(60616); std::string second; CHECK(cycleLoad(r2, second));     // the same seed, no reset: `first` is the fallback now
  CHECK(second != first); CHECK(s.presetLoadedInCycle(0, num(first))); CHECK(s.presetLoadedInCycle(0, num(second)));
  s.resetLoadCycle();                                                 // as the recorder does at a recording / render start
  for (int n = 0; n < Sets::kLoadedPresets; n++) CHECK(!s.presetLoadedInCycle(0, n));
  CHECK(awaitSessionWrite());                                         // the reset alone made the session dirty (no markSessionDirty here)
  { const std::string hex = valueOf(newestSessionText(0), "lp.loaded.0");   // masks of no genre: the file carries none (or all clear), the restore starts empty
    CHECK(hex.empty() || hex == std::string(2 * Sets::kLoadedPresets / 8, '0')); }
  Rng r3(60616); std::string third; CHECK(cycleLoad(r3, third));
  CHECK(third == first);                                              // the same candidates, the same draws
  CHECK(s.presetLoadedInCycle(0, num(third)));
  std::printf("  seed 60616: %s, then %s without a reset, %s again after one\n", first.c_str(), second.c_str(), third.c_str());
  pumpSession(2000);
}

TEST(index_listing_is_bounded_and_filters_by_name) {
  Sets& s = sets();
  // 70 E-minor sets and one unlabelled A-minor set join the folder and the index, as the writer would list them
  std::string idx = readText(kIndex); CHECK(!idx.empty());
  const std::string dummy = "schemaVersion=1\nsetIndex=0\nkeyRoot=4\nkeyName=E minor\ngenre=House\nlp.bpm=120\n";
  for (int i = 0; i < 70; i++) { char n[64]; snprintf(n, sizeof n, "House_Emin_%03d.lpset", 100 + i); writeText(kPresets + "/" + n, dummy); note(kPresets + "/" + n); idx += std::string(n) + "\n"; }
  const std::string custom = "custom_set.lpset";
  writeText(kPresets + "/" + custom, "# hand-made\nschemaVersion=1\nsetIndex=0\nkeyRoot=9\nkeyName=A minor\ngenre=House\n"); note(kPresets + "/" + custom); idx += custom + "\n";
  writeText(kIndex, idx);
  const int total = (int)listDir(kPresets, ".lpset").size();
  CHECK(total >= 74);
  // A minor: the index is streamed in 4 KB chunks, no directory walk, only the unlabelled name is opened
  Counters saveMax = g_max; g_max = Counters();
  Rng rng(44); char name[64] = "";
  int steps = loadSync(0, rng, name, sizeof name);
  std::printf("  %d names: chosen %s after %d steps; per step max walks %d entries %d opens %d probes %d read %zu B\n", total, name, steps, g_max.walks, g_max.entries, g_max.opens, g_max.probes, g_max.bytesRead);
  CHECK(steps > 0 && steps < 40);
  CHECK_EQ(g_max.walks, 0); CHECK_EQ(g_max.entries, 0);
  CHECK(g_max.opens <= 1); CHECK(g_max.bytesRead <= (size_t)kSetChunk);
  CHECK(std::string(name).compare(0, 11, "House_Amin_") == 0 || std::string(name) == custom);
  settle(); CHECK(s.ok());
  // the unlabelled file is reachable (its keyRoot line was read): a listing with only it chooses it
  sd().remove(kIndex.c_str()); writeText(kIndex, custom + "\n");
  CHECK(s.loadRandom(0, rng, name, sizeof name)); CHECK(std::string(name) == custom); settle(); CHECK(s.ok());
  writeText(kIndex, idx);
  // E minor picks among the 70 labelled files without opening any of them
  const uint8_t savedRoot = gen().key().root; gen().key().root = 4;
  g_max = Counters();
  CHECK(loadSync(0, rng, name, sizeof name) > 0);
  CHECK(std::string(name).compare(0, 11, "House_Emin_") == 0);
  CHECK(g_max.opens <= 1);                                          // chosen from names alone; only the unlabelled file is ever opened
  settle(); CHECK(s.ok());                                          // an all-NONE set: nothing is touched
  gen().key().root = savedRoot;
  if (g_max.probes < saveMax.probes) g_max.probes = saveMax.probes; if (g_max.opens < saveMax.opens) g_max.opens = saveMax.opens;
  if (g_max.entries < saveMax.entries) g_max.entries = saveMax.entries; if (g_max.walks < saveMax.walks) g_max.walks = saveMax.walks;
  if (g_max.bytesRead < saveMax.bytesRead) g_max.bytesRead = saveMax.bytesRead; if (g_max.bytesWritten < saveMax.bytesWritten) g_max.bytesWritten = saveMax.bytesWritten;
  // NNN comes from the index (max 169 here), and a hand-copied file holding that number is never overwritten
  int maxN = 0; for (auto& n : indexNames()) { int v = Sets::parsePresetNumber(n.c_str()); if (v > maxN) maxN = v; }
  char hand[64]; snprintf(hand, sizeof hand, "House_Amin_%03d.lpset", maxN + 1);
  writeText(kPresets + "/" + hand, "# hand copied, not indexed\n"); note(kPresets + "/" + hand);
  { const bool ok = saveSync(0, name, sizeof name); CHECK(ok); if (!ok) std::printf("  save error: %s (%s)\n", s.lastError(), name); }
  CHECK_EQ(Sets::parsePresetNumber(name), maxN + 2);
  notePreset(name);
  CHECK(readText(kPresets + "/" + hand) == "# hand copied, not indexed\n");
  CHECK(indexNames().count(name) == 1);
  // a stale index entry (a file deleted by hand) fails that one load and drops the index: self-healing
  writeText(kIndex, "House_Amin_777.lpset\n");
  CHECK(!s.loadRandom(0, rng, name, sizeof name)); CHECK(std::string(s.lastError()) == "OPEN SET"); CHECK(!s.busy());
  CHECK(!exists(kIndex));
  // without an index the directory walk lists pages of 32 (page p re-walks p×32 entries: the known fallback
  // cost), still finds a match, and the next save rebuilds the index from the real folder contents
  g_strict = false;
  CHECK(s.loadRandom(0, rng, name, sizeof name)); CHECK(std::string(name).compare(0, 11, "House_Amin_") == 0 || std::string(name) == custom); settle(); CHECK(s.ok());
  CHECK(!exists(kIndex));
  CHECK(saveSync(0, name, sizeof name)); notePreset(name);
  g_strict = true;
  CHECK(exists(kIndex));
  std::set<std::string> onDisk; for (auto& f : listDir(kPresets, ".lpset")) onDisk.insert(f);
  CHECK(indexNames() == onDisk);
}

TEST(sd_budget_per_step) {
  // SPEC §9.2 for every sets().step() of this run: ≤ 32 walked entries, one file open (+ ≤ 3 probes), one 4 KB chunk.
  std::printf("  per-step maxima: walks %d, entries %d, opens %d, probes %d, renames %d, read %zu B, written %zu B, %.2f ms\n", g_max.walks, g_max.entries, g_max.opens, g_max.probes, g_max.renames, g_max.bytesRead, g_max.bytesWritten, g_maxStepMs);
  CHECK(g_max.walks <= 1); CHECK(g_max.entries <= kSetPage);
  CHECK(g_max.opens <= 1);                                          // one file open per step (SPEC §9.2)
  CHECK(g_max.probes <= 3); CHECK(g_max.renames <= 1);
  CHECK(g_max.bytesRead <= 2 * (size_t)kSetChunk);                  // a MIDI file is parsed in one go (≤ 2.2 KB on this card)
  CHECK(g_max.bytesWritten <= (size_t)kSetChunk + 128);             // a label rewrite step writes its 4 KB chunk plus a held line split by the read and the label line (SPEC §9.22)
}

TEST(sizes_and_cleanup) {
  std::printf("  sizeof(Sets) = %zu, sizeof(SetFile) = %zu, sizeof(PropertiesReader) = %zu, sizeof(PresetRow) = %zu, max step %.2f ms; first save (vocal streaming) %d steps\n", sizeof(Sets), sizeof(SetFile), sizeof(PropertiesReader), sizeof(Sets::PresetRow), g_maxStepMs, g_saveStepsVocal);
  CHECK(sizeof(Sets) <= 11500);                                     // SPEC §9.29: the REAPER clip fields left SetFileTrack (−776 B of the 12 264 B host / 12 220 B device of §9.28): host 11 488 B
  CHECK_EQ(kSetLineMax, 672); CHECK(sizeof(((SetFile*)nullptr)->genre) == (size_t)kGenreNameLen);
  CHECK(sizeof(Sets::PresetRow) * Sets::kPresetPage <= (size_t)kSetPage * kSetNameMax);
  CHECK(sd().openCount() <= 2);                                     // the generator may keep its vocal streams open; sets closed everything
  if (getenv("LP_SETS_KEEP")) { std::printf("  LP_SETS_KEEP set: leaving %s\n", kPresets.c_str()); return; }
  for (auto it = g_created.rbegin(); it != g_created.rend(); ++it) if (exists(*it)) sd().remove(it->c_str());   // only what we created
  CHECK(exists(kPresets + "/assets") == g_assetsExisted);           // SPEC §9.29: the firmware neither creates nor removes an assets/ folder
  if (!g_presetsExisted) { for (auto& f : listDir(kPresets, nullptr)) sd().remove((kPresets + "/" + f).c_str()); sd().remove(kPresets.c_str()); }
  CHECK(exists(kPresets) == g_presetsExisted);
  CHECK(!exists(kShardDir));
  for (auto& f : listDir(kState, nullptr)) if (isSessionFile(f) && !g_stateBefore.count(f)) sd().remove((kState + "/" + f).c_str());   // the session tests' files (later pumps may have rewritten them)
  if (!g_stateExisted) { sd().remove(kState.c_str()); CHECK(!exists(kState)); }
  lib().invalidate();
}

int main() { return lp_test::run(); }
