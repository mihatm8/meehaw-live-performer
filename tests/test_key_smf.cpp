// Host tests for gen/key.cpp (port of key.lua) and gen/smf.cpp (SMF streaming parser / writer).
// Corpus checks run over every file under sdcard/lp/midi (override with LP_MIDI_DIR). The aggregate
// constants come from tests/refmodel_smf.py, a Python port of the same parsing rules: they are a regression
// snapshot of those rules for this card image, not an independent oracle. The properties derived from a raw
// event walk of each file (note-on counts, truncation) are the independent part.
#include "check.h"
#include "gen/key.h"
#include "gen/smf.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <map>
#include <string>
#include <vector>

using namespace lp;

// ---------------------------------------------------------------------------------------------------------
// Test-only IFile implementations
// ---------------------------------------------------------------------------------------------------------
class PosixFile : public IFile {                       // FILE*-based IFile, defined only in this test
 public:
  explicit PosixFile(const char* path, const char* mode = "rb") {
    f_ = std::fopen(path, mode);
    if (f_) { std::fseek(f_, 0, SEEK_END); size_ = (uint32_t)std::ftell(f_); std::fseek(f_, 0, SEEK_SET); }
  }
  ~PosixFile() override { close(); }
  bool valid() const override { return f_ != nullptr; }
  size_t read(void* dst, size_t n) override { return f_ ? std::fread(dst, 1, n, f_) : 0; }
  size_t write(const void* src, size_t n) override {
    if (!f_) return 0;
    size_t w = std::fwrite(src, 1, n, f_);
    uint32_t p = (uint32_t)std::ftell(f_);
    if (p > size_) size_ = p;
    return w;
  }
  bool seek(uint32_t pos) override { return f_ && std::fseek(f_, (long)pos, SEEK_SET) == 0; }
  uint32_t position() const override { return f_ ? (uint32_t)std::ftell(f_) : 0; }
  uint32_t size() const override { return size_; }
  void close() override { if (f_) { std::fclose(f_); f_ = nullptr; } }
 private:
  FILE* f_ = nullptr;
  uint32_t size_ = 0;
};

class VecFile : public IFile {                         // growable memory file; maxRead simulates short reads
 public:
  std::vector<uint8_t> data;
  size_t maxRead = (size_t)-1;
  uint32_t pos = 0;
  int reads = 0;                                       // read() calls, to bound the card work of a parse
  VecFile() {}
  explicit VecFile(std::vector<uint8_t> d, size_t mr = (size_t)-1) : data(std::move(d)), maxRead(mr) {}
  bool valid() const override { return true; }
  size_t read(void* dst, size_t n) override {
    reads++;
    if (pos >= data.size()) return 0;
    n = std::min(n, std::min(maxRead, data.size() - pos));
    memcpy(dst, data.data() + pos, n);
    pos += (uint32_t)n;
    return n;
  }
  size_t write(const void* src, size_t n) override {
    if (pos + n > data.size()) data.resize(pos + n);
    memcpy(data.data() + pos, src, n);
    pos += (uint32_t)n;
    return n;
  }
  bool seek(uint32_t p) override { if (p > data.size()) return false; pos = p; return true; }
  uint32_t position() const override { return pos; }
  uint32_t size() const override { return (uint32_t)data.size(); }
  void close() override {}
};

// ---------------------------------------------------------------------------------------------------------
// SMF builder helpers
// ---------------------------------------------------------------------------------------------------------
typedef std::vector<uint8_t> Bytes;
static void putVlq(Bytes& t, uint32_t v) {
  uint8_t tmp[5]; int n = 0;
  do { tmp[n++] = v & 0x7F; v >>= 7; } while (v);
  while (n--) t.push_back(tmp[n] | (n ? 0x80 : 0));
}
static void put32(Bytes& b, uint32_t v) { b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v); }
static void put16(Bytes& b, uint16_t v) { b.push_back(v >> 8); b.push_back(v); }
static void ev(Bytes& t, uint32_t delta, std::initializer_list<int> bytes) { putVlq(t, delta); for (int b : bytes) t.push_back((uint8_t)b); }
static void on(Bytes& t, uint32_t delta, int pitch, int vel, int ch = 0) { ev(t, delta, {0x90 | ch, pitch, vel}); }
static void off(Bytes& t, uint32_t delta, int pitch, int ch = 0) { ev(t, delta, {0x80 | ch, pitch, 0x40}); }
static void meta(Bytes& t, uint32_t delta, int type, const Bytes& data) { putVlq(t, delta); t.push_back(0xFF); t.push_back((uint8_t)type); putVlq(t, (uint32_t)data.size()); t.insert(t.end(), data.begin(), data.end()); }
static void eot(Bytes& t, uint32_t delta) { meta(t, delta, 0x2F, {}); }
static void tempo(Bytes& t, uint32_t delta, uint32_t mpq) { meta(t, delta, 0x51, {(uint8_t)(mpq >> 16), (uint8_t)(mpq >> 8), (uint8_t)mpq}); }
static Bytes smfFile(int format, int ppq, const std::vector<Bytes>& tracks, const char* trackId = "MTrk") {
  Bytes b = {'M', 'T', 'h', 'd'};
  put32(b, 6); put16(b, (uint16_t)format); put16(b, (uint16_t)tracks.size()); put16(b, (uint16_t)ppq);
  for (const Bytes& t : tracks) { b.insert(b.end(), trackId, trackId + 4); put32(b, (uint32_t)t.size()); b.insert(b.end(), t.begin(), t.end()); }
  return b;
}
static bool parseBytes(const Bytes& b, Pattern& p, SmfInfo* info = nullptr) { return smfParseBuffer(b.data(), (uint32_t)b.size(), p, info); }
static void expectNote(const Note& n, int start, int pitch, int lenUnits, int velQ) {
  CHECK_EQ(n.start(), start); CHECK_EQ(n.pitch(), pitch); CHECK_EQ(n.len() / kNoteLenUnit, lenUnits); CHECK_EQ(n.vel() >> 2, velQ);
}
static bool sameNotes(const Pattern& a, const Pattern& b) {
  if (a.count != b.count) return false;
  for (int i = 0; i < a.count; i++) if (a.notes[i].packed != b.notes[i].packed) return false;
  return true;
}
static bool samePattern(const Pattern& a, const Pattern& b) { return a.lengthTicks == b.lengthTicks && sameNotes(a, b); }
static int ceilBeats(int ticks) { return ((ticks + kTicksPerBeat - 1) / kTicksPerBeat) * kTicksPerBeat; }
// Independent raw walk of an SMF: counts note-on events (velocity > 0) over every MTrk chunk, with running
// status, meta/sysex skipping and 5-byte VLQs as the parser reads them. Returns -1 when the walk goes wrong.
static long rawNoteOns(const Bytes& b, int* ppqOut = nullptr) {
  if (b.size() < 14 || memcmp(b.data(), "MThd", 4) != 0) return -1;
  uint32_t hl = ((uint32_t)b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7];
  int ntracks = (b[10] << 8) | b[11]; if (ppqOut) *ppqOut = (b[12] << 8) | b[13];
  size_t i = 8 + hl; long ons = 0; int tracks = 0;
  auto vlq = [&](size_t& k, uint32_t& v) { v = 0; for (int n = 0; n < 8; n++) { if (k >= b.size()) return false; uint8_t c = b[k++]; v = (v << 7) | (c & 0x7F); if (!(c & 0x80)) return true; } return false; };
  while (i + 8 <= b.size() && tracks < ntracks) {
    uint32_t tl = ((uint32_t)b[i + 4] << 24) | (b[i + 5] << 16) | (b[i + 6] << 8) | b[i + 7];
    bool mtrk = memcmp(b.data() + i, "MTrk", 4) == 0; i += 8;
    size_t end = std::min(b.size(), (size_t)i + tl);
    if (!mtrk) { i = end; continue; }
    tracks++; uint8_t status = 0;
    while (i < end) {
      uint32_t d; if (!vlq(i, d) || i >= end) break;
      uint8_t s = b[i], d1, d2;
      if (s == 0xFF) { i++; if (i >= b.size()) break; uint8_t ty = b[i++]; uint32_t l; if (!vlq(i, l)) break; i += l; if (ty == 0x2F) break; continue; }
      if (s == 0xF0 || s == 0xF7) { i++; uint32_t l; if (!vlq(i, l)) break; i += l; continue; }
      if (s >= 0xF8) { i++; continue; }
      if (s >= 0xF0) { i += 1 + ((s == 0xF1 || s == 0xF3) ? 1 : (s == 0xF2 ? 2 : 0)); continue; }
      if (s < 0x80) { if (!status) break; s = status; } else { i++; status = s; }
      if (i >= b.size()) break; d1 = b[i++]; if (d1 & 0x80) break;
      uint8_t kind = s & 0xF0;
      if (kind == 0xC0 || kind == 0xD0) continue;
      if (i >= b.size()) break; d2 = b[i++]; if (d2 & 0x80) break;
      if (kind == 0x90 && d2 > 0) ons++;
    }
    i = end;
  }
  return tracks ? ons : -1;
}
static long to96(uint32_t srcTicks, int ppq) { return (long)(((uint64_t)srcTicks * kPPQN + ppq / 2) / ppq); }
static int maxPolyphony(const Pattern& p) {
  int best = 0;
  for (int i = 0; i < p.count; i++) {
    int s = p.notes[i].start(), c = 0;
    for (int j = 0; j < p.count; j++) if (p.notes[j].start() <= s && s < p.notes[j].end()) c++;
    best = std::max(best, c);
  }
  return best;
}

// ---------------------------------------------------------------------------------------------------------
// key.lua port
// ---------------------------------------------------------------------------------------------------------
static void expectKey(const LibraryKey& k, int root, bool minor) { CHECK(k.valid); CHECK(!k.keyless); CHECK_EQ(k.root, root); CHECK_EQ(k.minor, minor); }

TEST(key_label_basic) {
  expectKey(parseLibraryLabel("Amin"), 9, true);
  expectKey(parseLibraryLabel("C"), 0, false);
  expectKey(parseLibraryLabel("F#min"), 6, true);
  expectKey(parseLibraryLabel("F#min7"), 6, true);
  expectKey(parseLibraryLabel("A#maj"), 10, false);
  expectKey(parseLibraryLabel("Cmaj7"), 0, false);
  expectKey(parseLibraryLabel("amin"), 9, true);       // key.lua:134 uppercases
  expectKey(parseLibraryLabel("G#MIN"), 8, true);
  LibraryKey na = parseLibraryLabel("NA"); CHECK(na.valid); CHECK(na.keyless);
  LibraryKey na2 = parseLibraryLabel("na"); CHECK(na2.valid); CHECK(na2.keyless);
}

TEST(key_label_accidentals) {
  expectKey(parseLibraryLabel("Bb"), 10, false);        // 'B' as second char = flat (key.lua:45)
  expectKey(parseLibraryLabel("Bbmin"), 10, true);
  expectKey(parseLibraryLabel("BMIN"), 11, true);       // 'B' then MIN: the B is the root, not a flat
  expectKey(parseLibraryLabel("Bmin"), 11, true);
  expectKey(parseLibraryLabel("B"), 11, false);
  expectKey(parseLibraryLabel("Eb7"), 3, false);
  expectKey(parseLibraryLabel("Cb"), 11, false);        // floor_mod(0 - 1, 12)
  expectKey(parseLibraryLabel("E#"), 5, false);
  CHECK(!parseLibraryLabel("Db min").valid);            // tail " MIN" fails the tail rule (key.lua:159)
}

TEST(key_label_tail_rules) {
  const char* ok[] = {"Cadd9", "Csus4", "Cdim", "Caug", "Cdom7", "C7", "Cmin9", "Cmaj7", "C#min11", "Aminadd9", "Cmajsus2"};
  for (const char* s : ok) { LibraryKey k = parseLibraryLabel(s); CHECK(k.valid); CHECK(!k.keyless); }
  const char* bad[] = {"Dm", "Cm7", "Cx", "Cmin/E", "Hmin", "", "#", "min", "C major", "Amin-x", "NAX", "N"};
  for (const char* s : bad) { LibraryKey k = parseLibraryLabel(s); CHECK(!k.valid); CHECK(!k.keyless); }
  CHECK(!parseLibraryLabel(nullptr).valid);
}

TEST(key_label_trim) {
  expectKey(parseLibraryLabel("  Amin "), 9, true);
  expectKey(parseLibraryLabel("\tC\n"), 0, false);
  CHECK(parseLibraryLabel(" na ").keyless);
  CHECK(!parseLibraryLabel("   ").valid);
}

TEST(key_filename) {
  expectKey(parseLibraryFilename("Amin__Chord__x.mid"), 9, true);
  expectKey(parseLibraryFilename("/lp/midi/Chord/Amin/Amin__Chord__DHS-20-UptempoPiano-Am__cb4c2447.mid"), 9, true);
  expectKey(parseLibraryFilename("C:\\Music\\Midi\\Emin__Pad__x.mid"), 4, true);
  expectKey(parseLibraryFilename("C__x"), 0, false);
  expectKey(parseLibraryFilename("A__x"), 9, false);
  expectKey(parseLibraryFilename("Cmin__x"), 0, true);
  expectKey(parseLibraryFilename("Bb__x"), 10, false);
  expectKey(parseLibraryFilename("F#min7__x"), 6, true);
  expectKey(parseLibraryFilename("A#min__Chord__BassDerived__Shift-Along-Bass-1-Bb-min__a119ad34.mid"), 10, true);
  LibraryKey na = parseLibraryFilename("NA__Beat__x.mid"); CHECK(na.valid); CHECK(na.keyless);
  CHECK(!parseLibraryFilename("Dm__x").valid);
  CHECK(!parseLibraryFilename("__x").valid);            // empty label (key.lua:130)
  CHECK(!parseLibraryFilename("/dir/__x").valid);
  CHECK(!parseLibraryFilename("noseparator.mid").valid);
  CHECK(!parseLibraryFilename("A_x").valid);
  CHECK(!parseLibraryFilename("").valid);
  CHECK(!parseLibraryFilename(nullptr).valid);
}

TEST(key_accepts_a_minor) {
  ProjectKey key; CHECK_EQ(key.root, 9);
  CHECK(key.accepts(parseLibraryFilename("Amin__x")));
  CHECK(key.accepts(parseLibraryFilename("C__x")));
  CHECK(key.accepts(parseLibraryFilename("Cmaj__x")));
  CHECK(key.accepts(parseLibraryFilename("NA__x")));
  CHECK(!key.accepts(parseLibraryFilename("A__x")));
  CHECK(!key.accepts(parseLibraryFilename("Cmin__x")));
  CHECK(!key.accepts(parseLibraryFilename("Dm__x")));   // unparseable never accepted
  CHECK(!key.accepts(parseLibraryFilename("Emin__x")));
  CHECK(!key.accepts(parseLibraryFilename("G__x")));
  CHECK(!key.accepts(LibraryKey()));
}

TEST(key_accepts_other_roots) {
  ProjectKey c; c.root = 0;                               // C minor <-> D# major
  CHECK(c.accepts(parseLibraryLabel("Cmin"))); CHECK(c.accepts(parseLibraryLabel("D#"))); CHECK(c.accepts(parseLibraryLabel("Eb")));
  CHECK(!c.accepts(parseLibraryLabel("C"))); CHECK(!c.accepts(parseLibraryLabel("D#min")));
  ProjectKey as; as.root = 10;                            // A# minor <-> C# major
  CHECK(as.accepts(parseLibraryLabel("Bbmin"))); CHECK(as.accepts(parseLibraryLabel("A#min"))); CHECK(as.accepts(parseLibraryLabel("C#"))); CHECK(as.accepts(parseLibraryLabel("Db")));
  CHECK(!as.accepts(parseLibraryLabel("C#min")));
  ProjectKey b; b.root = 11;                              // B minor <-> D major
  CHECK(b.accepts(parseLibraryLabel("D"))); CHECK(!b.accepts(parseLibraryLabel("B")));
}

TEST(key_contains) {
  ProjectKey key;                                          // A natural minor: A B C D E F G
  const int in[] = {57, 59, 60, 62, 64, 65, 67, 69, 45, 0, 2, 4, 5, 7, 9, 11, 127};
  const int out[] = {58, 61, 63, 66, 68, 1, 3, 6, 8, 10, 126};
  for (int p : in) CHECK(key.contains(p));
  for (int p : out) CHECK(!key.contains(p));
  CHECK(key.contains(-3));                                 // floor_mod: -3 -> 9 (A)
  CHECK(!key.contains(-2));
  ProjectKey c; c.root = 0;                                // C minor: C D D# F G G# A#
  CHECK(c.contains(60)); CHECK(c.contains(63)); CHECK(c.contains(68)); CHECK(!c.contains(64)); CHECK(!c.contains(69));
}

TEST(key_snap) {
  ProjectKey key;
  CHECK_EQ(key.snap(57), 57);                              // in scale: unchanged (key.lua:90)
  CHECK_EQ(key.snap(58), 57);                              // down first (key.lua:96)
  CHECK_EQ(key.snap(61), 60);
  CHECK_EQ(key.snap(63), 62);
  CHECK_EQ(key.snap(66), 65);
  CHECK_EQ(key.snap(68), 67);
  CHECK_EQ(key.snap(1), 0);                                // 0 = C is in A minor
  CHECK_EQ(key.snap(-5), 0);                               // clamped (key.lua:89)
  CHECK_EQ(key.snap(130), 127);                            // 127 = G
  ProjectKey fs; fs.root = 6;                              // F# minor: F# G# A B C# D E ; pitch 0 (C) -> no down -> up to 1 (C#)
  CHECK_EQ(fs.snap(0), 1);
  CHECK_EQ(fs.snap(127), 126);                             // 127 = G not in F# minor, 126 = F# is
}

TEST(key_move_scale_steps) {
  ProjectKey key;
  CHECK_EQ(key.moveScaleSteps(57, 1), 59);
  CHECK_EQ(key.moveScaleSteps(57, 2), 60);
  CHECK_EQ(key.moveScaleSteps(57, 7), 69);                 // one octave = 7 steps
  CHECK_EQ(key.moveScaleSteps(57, -1), 55);
  CHECK_EQ(key.moveScaleSteps(57, -7), 45);
  CHECK_EQ(key.moveScaleSteps(57, 0), 57);
  CHECK_EQ(key.moveScaleSteps(58, 1), 59);                 // snapped first (key.lua:108)
  CHECK_EQ(key.moveScaleSteps(58, 0), 57);
  CHECK_EQ(key.moveScaleSteps(127, 1), 127);               // top: candidate leaves range -> break (key.lua:115)
  CHECK_EQ(key.moveScaleSteps(0, -1), 0);
  CHECK_EQ(key.moveScaleSteps(126, 2), 127);               // 126 -> snap 125 (F) -> 127 (G) -> stop
}

TEST(key_degree_pitch) {
  ProjectKey key;                                          // A minor, octave 3 -> degree 0 in [48, 60)
  CHECK_EQ(key.degreePitch(0, 3), 57);
  CHECK_EQ(key.degreePitch(1, 3), 59);
  CHECK_EQ(key.degreePitch(2, 3), 60);
  CHECK_EQ(key.degreePitch(3, 3), 62);
  CHECK_EQ(key.degreePitch(6, 3), 67);
  CHECK_EQ(key.degreePitch(7, 3), 69);                     // degree 7 = degree 0 an octave up
  CHECK_EQ(key.degreePitch(9, 3), 72);
  CHECK_EQ(key.degreePitch(-1, 3), 55);                    // degree -1 = degree 6 an octave down
  CHECK_EQ(key.degreePitch(-7, 3), 45);
  CHECK_EQ(key.degreePitch(0, -1), 9);
  CHECK_EQ(key.degreePitch(0, 9), 127);                    // clamped
  for (int d = 0; d < 14; d++) CHECK(key.contains(key.degreePitch(d, 3)));
  ProjectKey c; c.root = 0;
  CHECK_EQ(c.degreePitch(0, 3), 48);
  CHECK_EQ(c.degreePitch(0, 4), 60);
  ProjectKey b; b.root = 11;
  CHECK_EQ(b.degreePitch(0, 3), 59);
}

TEST(key_names_and_labels) {
  CHECK_EQ(strcmp(noteName(0), "C"), 0); CHECK_EQ(strcmp(noteName(10), "A#"), 0); CHECK_EQ(strcmp(noteName(11), "B"), 0);
  CHECK_EQ(strcmp(noteName(12), "C"), 0); CHECK_EQ(strcmp(noteName(-1), "B"), 0);
  ProjectKey key; char mn[8], mj[8];
  CHECK_EQ(strcmp(key.rootName(), "A"), 0);
  key.labels(mn, mj); CHECK_EQ(strcmp(mn, "Amin"), 0); CHECK_EQ(strcmp(mj, "C"), 0);
  key.root = 10; key.labels(mn, mj); CHECK_EQ(strcmp(mn, "A#min"), 0); CHECK_EQ(strcmp(mj, "C#"), 0);   // sharps only
  key.root = 0; key.labels(mn, mj); CHECK_EQ(strcmp(mn, "Cmin"), 0); CHECK_EQ(strcmp(mj, "D#"), 0);
  key.root = 11; key.labels(mn, mj); CHECK_EQ(strcmp(mn, "Bmin"), 0); CHECK_EQ(strcmp(mj, "D"), 0);
  // The labels are the library folder names and must parse back to keys the project accepts.
  for (int r = 0; r < 12; r++) { key.root = (uint8_t)r; key.labels(mn, mj); CHECK(key.accepts(parseLibraryLabel(mn))); CHECK(key.accepts(parseLibraryLabel(mj))); }
}

TEST(key_cycle_minor_root) {
  const int order[] = {9, 4, 11, 6, 1, 8, 3, 10, 5, 0, 7, 2};   // A E B F# C# G# D# A# F C G D
  int r = 9;
  for (int i = 1; i <= 12; i++) { r = cycleMinorRoot(r, 1); CHECK_EQ(r, order[i % 12]); }
  CHECK_EQ(r, 9);                                          // full circle
  for (int i = 11; i >= 0; i--) { r = cycleMinorRoot(r, -1); CHECK_EQ(r, order[i]); }
  CHECK_EQ(cycleMinorRoot(9, 0), 9);
  CHECK_EQ(cycleMinorRoot(9, 5), 4);                       // any positive = one step
  CHECK_EQ(cycleMinorRoot(9, -3), 2);
  CHECK_EQ(cycleMinorRoot(21, 1), 4);                      // floor_mod normalisation
  CHECK_EQ(cycleMinorRoot(-3, 1), 4);
}

// ---------------------------------------------------------------------------------------------------------
// SMF: synthetic files
// ---------------------------------------------------------------------------------------------------------
TEST(smf_basic_format0) {
  Bytes t; on(t, 0, 60, 100); off(t, 48, 60); on(t, 48, 64, 64); off(t, 96, 64); eot(t, 0);
  Pattern p; SmfInfo info;
  CHECK(parseBytes(smfFile(0, 96, {t}), p, &info));
  CHECK_EQ(p.count, 2); CHECK_EQ(p.lengthTicks, 192);
  expectNote(p.notes[0], 0, 60, 4, 25); expectNote(p.notes[1], 96, 64, 8, 16);
  CHECK_EQ(info.ppq, 96); CHECK_NEAR(info.bpm, 0.0f, 1e-6); CHECK_EQ(info.notes, 2); CHECK(!info.truncated); CHECK_EQ(info.lengthTicksSrc, 192u);
}

TEST(smf_running_status_and_vel0_off) {
  Bytes t; ev(t, 0, {0x90, 60, 100}); ev(t, 48, {60, 0}); ev(t, 0, {62, 90}); ev(t, 48, {62, 0});  // running status 0x90
  meta(t, 0, 0x01, {'t', 'x', 't'}); ev(t, 0, {64, 80}); ev(t, 24, {64, 0});                     // running status survives meta
  eot(t, 0);
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.count, 3);
  expectNote(p.notes[0], 0, 60, 4, 25); expectNote(p.notes[1], 48, 62, 4, 22); expectNote(p.notes[2], 96, 64, 2, 20);
  CHECK_EQ(p.lengthTicks, 192);
}

TEST(smf_ppq_conversion_rounding) {
  Bytes t; on(t, 479, 60, 100); off(t, 1, 60); eot(t, 0);         // 479/480*96 = 95.8 -> 96; 480 -> 96 (+1 min)
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(0, 480, {t}), p, &info));
  CHECK_EQ(p.count, 1); CHECK_EQ(p.notes[0].start(), 96); CHECK_EQ(p.notes[0].len(), 12); CHECK_EQ(info.ppq, 480);
  Bytes u; on(u, 2, 60, 100); off(u, 243, 60); eot(u, 0);          // 2 -> 0.4 -> 0 ; 245 -> 49 -> len 49 -> 5 units
  CHECK(parseBytes(smfFile(0, 480, {u}), p));
  CHECK_EQ(p.notes[0].start(), 0); CHECK_EQ(p.notes[0].len(), 60);
  Bytes v; on(v, 1030, 60, 100); off(v, 480, 60); eot(v, 0);       // 960 PPQ: 1030 -> 103, 1510 -> 151
  CHECK(parseBytes(smfFile(0, 960, {v}), p));
  CHECK_EQ(p.notes[0].start(), 103); CHECK_EQ(p.notes[0].len(), 48); CHECK_EQ(p.lengthTicks, 192);
  Bytes w; on(w, 5, 60, 100); off(w, 5, 60); eot(w, 0);            // 5/960 and 10/960 both round to 1 -> min 1 tick
  CHECK(parseBytes(smfFile(0, 960, {w}), p));
  CHECK_EQ(p.notes[0].start(), 1); CHECK_EQ(p.notes[0].len(), 12);
}

TEST(smf_tempo_first_wins) {
  Bytes t; tempo(t, 0, 500000); tempo(t, 0, 428571); on(t, 0, 60, 100); off(t, 96, 60); eot(t, 0);
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(0, 96, {t}), p, &info));
  CHECK_NEAR(info.bpm, 120.0f, 1e-3);
  Bytes a; on(a, 0, 60, 100); off(a, 96, 60); eot(a, 0);
  Bytes b; tempo(b, 0, 461538); eot(b, 0);                           // 130 BPM in the second track
  CHECK(parseBytes(smfFile(1, 96, {a, b}), p, &info));
  CHECK_NEAR(info.bpm, 130.0f, 1e-2);
  Bytes c; tempo(c, 0, 476190); eot(c, 0);                            // 126 BPM first, 130 later: first wins
  CHECK(parseBytes(smfFile(1, 96, {c, b}), p, &info));
  CHECK_NEAR(info.bpm, 126.0f, 1e-2);
  Bytes d; meta(d, 0, 0x51, {0, 0, 0}); on(d, 0, 60, 100); off(d, 96, 60); eot(d, 0);   // zero tempo ignored
  CHECK(parseBytes(smfFile(0, 96, {d}), p, &info)); CHECK_NEAR(info.bpm, 0.0f, 1e-6);
}

TEST(smf_format1_merge_sorted) {
  Bytes a; meta(a, 0, 0x03, {'n', 'a', 'm', 'e'}); on(a, 96, 64, 100); off(a, 96, 64); eot(a, 192);   // EOT at 384
  Bytes b; on(b, 0, 60, 90); off(b, 48, 60); on(b, 240, 67, 80); off(b, 48, 67); eot(b, 0);           // 60@0, 67@288
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(1, 96, {a, b}), p, &info));
  CHECK_EQ(p.count, 3);
  expectNote(p.notes[0], 0, 60, 4, 22); expectNote(p.notes[1], 96, 64, 8, 25); expectNote(p.notes[2], 288, 67, 4, 20);
  CHECK_EQ(p.lengthTicks, 384);                            // max EOT 384, within 2 beats of the last end 336
  CHECK_EQ(info.lengthTicksSrc, 384u);
}

TEST(smf_skips_other_events) {
  Bytes t;
  ev(t, 0, {0xF0, 3, 0x7E, 0x09, 0xF7});                   // sysex with length 3
  ev(t, 0, {0xB0, 7, 100});                                // CC
  ev(t, 0, {0xC0, 5});                                     // program change (1 data byte)
  ev(t, 0, {0xD0, 64});                                    // channel pressure
  ev(t, 0, {0xE0, 0, 64});                                 // pitch bend
  on(t, 0, 60, 100, 9);                                    // channel 10 (drum map) is treated like any other
  ev(t, 10, {0xA0, 60, 50});                               // poly pressure
  meta(t, 0, 0x58, {4, 2, 24, 8});                         // time signature
  meta(t, 0, 0x59, {0xFF, 1});                             // key signature
  ev(t, 0, {0xF7, 2, 1, 2});                               // escape sysex
  ev(t, 0, {0xF8});                                        // real-time (no data)
  ev(t, 0, {0xF2, 0, 0});                                  // song position (2 data bytes)
  off(t, 38, 60, 9);
  meta(t, 0, 0x7F, {1, 2, 3, 4, 5});                       // sequencer specific
  eot(t, 0);
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.count, 1); expectNote(p.notes[0], 0, 60, 4, 25); CHECK_EQ(p.lengthTicks, 96);
}

TEST(smf_trailing_silence_rule) {
  Pattern p;
  auto len = [&](uint32_t noteEnd, uint32_t eotAt) {
    Bytes t; on(t, 0, 60, 100); off(t, noteEnd, 60); eot(t, eotAt - noteEnd); CHECK(parseBytes(smfFile(0, 96, {t}), p)); return (int)p.lengthTicks;
  };
  CHECK_EQ(len(96, 96), 96);
  CHECK_EQ(len(96, 288), 288);                             // exactly two beats of silence: kept
  CHECK_EQ(len(96, 289), 96);                              // more than two beats: ignored
  CHECK_EQ(len(96, 200), 288);                             // EOT rounds up to 3 beats, within the limit
  CHECK_EQ(len(100, 300), 192);                            // raw ticks: 200 of silence > 2 beats -> ignored
  CHECK_EQ(len(100, 292), 384);                            // exactly 192 raw ticks of silence -> kept, rounded up
  CHECK_EQ(len(100, 385), 192);
  CHECK_EQ(len(50, 60), 96);                               // EOT before the beat boundary: no effect
  CHECK_EQ(len(1500, 1536), 1536);
}

TEST(smf_min_length_and_unterminated) {
  Bytes t; on(t, 0, 60, 100); off(t, 0, 60); on(t, 0, 62, 100); eot(t, 300);   // zero-length + unterminated
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.count, 2);
  expectNote(p.notes[0], 0, 60, 1, 25);                    // 1 tick -> one length unit
  expectNote(p.notes[1], 0, 62, 25, 25);                   // closed at end of track (300 -> 25 units)
  CHECK_EQ(p.lengthTicks, 384);
  Bytes u; on(u, 0, 60, 100); on(u, 48, 62, 100); off(u, 48, 62);            // no EOT at all: closed at last event
  CHECK(parseBytes(smfFile(0, 96, {u}), p));
  CHECK_EQ(p.count, 2); expectNote(p.notes[0], 0, 60, 8, 25); expectNote(p.notes[1], 48, 62, 4, 25);
}

TEST(smf_same_pitch_fifo) {
  // A note-on for a pitch that is still sounding, followed by the note-off meant for the earlier note:
  // the off closes the oldest note (FIFO), so both notes keep their real lengths.
  Bytes t; on(t, 0, 42, 100); on(t, 24, 42, 90); off(t, 0, 42); off(t, 24, 42); eot(t, 0);
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.count, 2); expectNote(p.notes[0], 0, 42, 2, 25); expectNote(p.notes[1], 24, 42, 2, 22);
  Bytes u; off(u, 0, 60); on(u, 0, 60, 100); off(u, 48, 60); off(u, 0, 60); eot(u, 0);   // stray offs ignored
  CHECK(parseBytes(smfFile(0, 96, {u}), p));
  CHECK_EQ(p.count, 1); expectNote(p.notes[0], 0, 60, 4, 25);
}

TEST(smf_active_overflow) {
  Bytes t; for (int i = 0; i < 70; i++) on(t, 0, 30 + i, 100);                 // 70 simultaneous notes: 6 past the table
  for (int i = 0; i < 70; i++) off(t, i == 0 ? 96 : 0, 30 + i);
  eot(t, 0);
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(0, 96, {t}), p, &info));
  CHECK_EQ(p.count, 70);                                   // the 6 oldest were closed early but not lost
  int longOnes = 0; for (int i = 0; i < p.count; i++) if (p.notes[i].len() == 96) longOnes++;
  CHECK_EQ(longOnes, 64);
  CHECK(info.truncated);                                   // the caller is told the notes were mangled
  Bytes u; for (int i = 0; i < 64; i++) on(u, 0, 30 + i, 100);                 // exactly the table size: untouched
  for (int i = 0; i < 64; i++) off(u, i == 0 ? 96 : 0, 30 + i);
  eot(u, 0);
  CHECK(parseBytes(smfFile(0, 96, {u}), p, &info));
  CHECK_EQ(p.count, 64); CHECK(!info.truncated);
  for (int i = 0; i < p.count; i++) CHECK_EQ(p.notes[i].len(), 96);
}

TEST(smf_empty_track) {
  Bytes t; eot(t, 0);
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(0, 96, {t}), p, &info));
  CHECK_EQ(p.count, 0); CHECK_EQ(p.lengthTicks, 96); CHECK_EQ(info.notes, 0); CHECK(!info.truncated);
  Bytes u; meta(u, 0, 0x03, {'x'}); eot(u, 192);          // two beats of nothing
  CHECK(parseBytes(smfFile(0, 96, {u}), p)); CHECK_EQ(p.lengthTicks, 192);
  Bytes v; eot(v, 193); CHECK(parseBytes(smfFile(0, 96, {v}), p)); CHECK_EQ(p.lengthTicks, 96);
}

TEST(smf_cap_16_beats) {
  Bytes t; for (int i = 0; i < 20; i++) { on(t, i ? 48 : 0, 60, 100); off(t, 48, 60); } eot(t, 0);   // 20 beats
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(0, 96, {t}), p, &info));
  CHECK_EQ(p.count, 16); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
  CHECK_EQ(info.lengthTicksSrc, 1584u);                    // reading stopped at the first event beyond the window (a lower bound)
  for (int i = 0; i < p.count; i++) { CHECK_EQ(p.notes[i].start(), i * 96); CHECK(p.notes[i].end() <= 1536); }
  Bytes u; on(u, 1500, 60, 100); off(u, 96, 60); on(u, 0, 62, 100); off(u, 100, 62); eot(u, 0);     // crosses the cut
  CHECK(parseBytes(smfFile(0, 96, {u}), p, &info));
  CHECK_EQ(p.count, 1); expectNote(p.notes[0], 1500, 60, 3, 25); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
  Bytes v; on(v, 1530, 60, 100); off(v, 96, 60); eot(v, 0);                                         // starts in the last unit: kept as a tail
  CHECK(parseBytes(smfFile(0, 96, {v}), p, &info)); CHECK_EQ(p.count, 1); expectNote(p.notes[0], 1530, 60, 1, 25); CHECK(info.truncated);
  Bytes v2; on(v2, 1536, 60, 100); off(v2, 96, 60); eot(v2, 0);                                       // starts at the cut: dropped
  CHECK(parseBytes(smfFile(0, 96, {v2}), p, &info)); CHECK_EQ(p.count, 0); CHECK(info.truncated);
  Bytes w; on(w, 0, 60, 100); off(w, 96, 60); on(w, 3000, 62, 100); off(w, 96, 62); eot(w, 0);     // far note dropped: cut file
  CHECK(parseBytes(smfFile(0, 96, {w}), p, &info)); CHECK_EQ(p.count, 1); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
}

TEST(smf_cut_file_length_rule) {
  // A file that goes on beyond 16 beats was a longer loop: it is cut to exactly 16 beats whether the notes
  // beyond were dropped or the rest is silence, so bars stay aligned; the trailing-silence rule is for uncut files.
  Pattern p; SmfInfo info;
  Bytes a; on(a, 0, 60, 100); off(a, 8 * 96, 60); eot(a, 24 * 96);                     // 32-beat file, notes in the first 8 beats
  CHECK(parseBytes(smfFile(0, 96, {a}), p, &info)); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated); CHECK_EQ(p.count, 1);
  Bytes b; on(b, 0, 60, 100); off(b, 15 * 96, 60); eot(b, 16 * 96);                    // 31-beat file, notes end at beat 15
  CHECK(parseBytes(smfFile(0, 96, {b}), p, &info)); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
  Bytes c; on(c, 0, 60, 100); off(c, 8 * 96, 60); for (int i = 0; i < 8; i++) { on(c, i ? 96 : 16 * 96, 62, 100); off(c, 48, 62); } eot(c, 0);
  CHECK(parseBytes(smfFile(0, 96, {c}), p, &info));                                     // notes beyond are dropped: 16, not 8
  CHECK_EQ(p.lengthTicks, 1536); CHECK_EQ(p.count, 1); CHECK(info.truncated);
  Bytes d; on(d, 0, 60, 100); off(d, 15 * 96, 60); eot(d, 96);                         // uncut: 15 beats of notes, EOT at 16
  CHECK(parseBytes(smfFile(0, 96, {d}), p, &info)); CHECK_EQ(p.lengthTicks, 1536); CHECK(!info.truncated);
  Bytes e; on(e, 0, 60, 100); off(e, 13 * 96, 60); eot(e, 3 * 96);                     // uncut: 3 beats of silence -> 13
  CHECK(parseBytes(smfFile(0, 96, {e}), p, &info)); CHECK_EQ(p.lengthTicks, 13 * 96); CHECK(!info.truncated);
  Bytes f; eot(f, 20 * 96);                                                             // empty but 20 beats long: cut, 16 beats
  CHECK(parseBytes(smfFile(0, 96, {f}), p, &info)); CHECK_EQ(p.count, 0); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
  Bytes g; eot(g, 16 * 96);                                                             // exactly 16 beats: not cut, but 16 beats of silence > 2
  CHECK(parseBytes(smfFile(0, 96, {g}), p, &info)); CHECK_EQ(p.lengthTicks, 96); CHECK(!info.truncated);
  // Same rules at 480 PPQ: 7682 rounds to 1536 (inside), 7683 to 1537 (beyond).
  Bytes h; on(h, 0, 60, 100); off(h, 480, 60); eot(h, 7682 - 480);
  CHECK(parseBytes(smfFile(0, 480, {h}), p, &info)); CHECK_EQ(p.lengthTicks, 96); CHECK(!info.truncated);
  Bytes k; on(k, 0, 60, 100); off(k, 480, 60); eot(k, 7683 - 480);
  CHECK(parseBytes(smfFile(0, 480, {k}), p, &info)); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
  // Evicted notes still count toward the musical length (max note end over every note, not only kept ones).
  Bytes m; for (int i = 0; i < 200; i++) { on(m, i ? 3 : 0, 60, 100); off(m, 3, 60); } eot(m, 0);   // 200 notes, period 6, last ends at 1197
  CHECK(parseBytes(smfFile(0, 96, {m}), p, &info)); CHECK_EQ(p.count, kMaxNotes); CHECK_EQ(p.lengthTicks, 13 * 96); CHECK(info.truncated);
}

TEST(smf_corrupt_time_stays_inside_loop) {
  // Saturated source times (PPQ 1, or a delta chain reaching the clamp) must never produce a note outside the
  // loop: toTicks caps just past the window, so the start/length arithmetic cannot wrap.
  auto inside = [](const Pattern& p) { for (int i = 0; i < p.count; i++) if (p.notes[i].start() >= p.lengthTicks || p.notes[i].end() > p.lengthTicks + kNoteLenUnit - 1) return false; return true; };
  Pattern p; SmfInfo info;
  Bytes a; on(a, 0, 60, 100); on(a, 15, 62, 100); a.insert(a.end(), {0x8F, 0xFF, 0xFF, 0x7F}); a.push_back(0x80); a.push_back(60); a.push_back(0x40); eot(a, 0);   // PPQ 1: 1 tick = 1 beat
  CHECK(parseBytes(smfFile(0, 1, {a}), p, &info));
  CHECK_EQ(p.count, 2); CHECK(inside(p)); CHECK(info.truncated); CHECK_EQ(p.lengthTicks, 1536);
  expectNote(p.notes[0], 0, 60, 128, 25); expectNote(p.notes[1], 1440, 62, 8, 25);   // both clipped at the cut
  Bytes b; on(b, 0, 60, 100); on(b, 24 * 15, 62, 100);                                // PPQ 24: four 4-byte deltas reach kTimeCap
  for (int i = 0; i < 4; i++) { b.insert(b.end(), {0xFF, 0xFF, 0xFF, 0x7F}); b.push_back(0xB0); b.push_back(1); b.push_back(1); }
  off(b, 0, 60); off(b, 0, 62); eot(b, 0);
  CHECK(parseBytes(smfFile(0, 24, {b}), p, &info));
  CHECK_EQ(p.count, 2); CHECK(inside(p)); CHECK(info.truncated); CHECK_EQ(p.lengthTicks, 1536);
  expectNote(p.notes[0], 0, 60, 128, 25); expectNote(p.notes[1], 1440, 62, 8, 25);
  Bytes c; on(c, 0, 60, 100); c.insert(c.end(), {0xFF, 0xFF, 0xFF, 0x7F}); c.push_back(0x80); c.push_back(60); c.push_back(0x40);   // no EOT, huge off
  CHECK(parseBytes(smfFile(0, 96, {c}), p, &info)); CHECK_EQ(p.count, 1); CHECK(inside(p)); CHECK(info.truncated);
  Bytes d; on(d, 1530, 60, 100); d.insert(d.end(), {0xFF, 0xFF, 0xFF, 0x7F}); d.push_back(0x80); d.push_back(60); d.push_back(0x40); eot(d, 0);   // start in the last unit
  CHECK(parseBytes(smfFile(0, 96, {d}), p, &info)); CHECK_EQ(p.count, 1); CHECK(inside(p)); expectNote(p.notes[0], 1530, 60, 1, 25); CHECK(info.truncated); CHECK_EQ(p.lengthTicks, 1536);
}

TEST(smf_early_exit_bounds_reading) {
  // Nothing after the first event beyond the window can change the pattern, so the reader stops there: a 70 KB
  // file whose notes sit in the first 16 beats costs one or two card reads, not 140.
  Bytes t; on(t, 0, 60, 100); off(t, 96, 60); on(t, 1304, 62, 100); off(t, 200, 62);   // second note 1400..1600 crosses the cut
  for (int i = 0; i < 20000; i++) ev(t, 10, {0xB0, 1, i & 0x7F});                     // 80 KB of CCs beyond the window
  on(t, 0, 64, 100); off(t, 96, 64); eot(t, 0);
  Bytes file = smfFile(0, 96, {t});
  CHECK(file.size() > 70000);
  VecFile v(file); Pattern p; SmfInfo info; CHECK(smfParse(v, p, &info));
  CHECK(v.reads <= 2);
  CHECK_EQ(p.count, 2); expectNote(p.notes[0], 0, 60, 8, 25); expectNote(p.notes[1], 1400, 62, 11, 25);   // 1400..1600 clipped at 1536
  CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated); CHECK_EQ(info.lengthTicksSrc, 1600u);          // exit time (the off at 1600), not the EOT
  Bytes a; on(a, 0, 60, 100); off(a, 96, 60); eot(a, 20 * 96);                                        // format 1: the big track is skipped
  Bytes b; on(b, 48, 62, 100); off(b, 48, 62); eot(b, 0);
  VecFile w(smfFile(1, 96, {a, t, b})); CHECK(smfParse(w, p, &info));
  CHECK(w.reads <= 4);
  CHECK_EQ(p.count, 4); CHECK_EQ(p.lengthTicks, 1536); CHECK(info.truncated);
  expectNote(p.notes[2], 48, 62, 4, 25);                                                                // the track after the big one is read
}

TEST(smf_channel_matching) {
  // The same pitch sounding on two channels: each note-off closes the note of its own channel.
  Bytes t; on(t, 0, 60, 100, 0); on(t, 0, 60, 90, 1); off(t, 48, 60, 1); off(t, 48, 60, 0); eot(t, 0);
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.count, 2); expectNote(p.notes[0], 0, 60, 4, 22); expectNote(p.notes[1], 0, 60, 8, 25);   // ch2 48 ticks (vel 90), ch1 96 (vel 100)
  Bytes u; on(u, 0, 60, 100, 0); on(u, 0, 60, 90, 1); off(u, 48, 60, 0); off(u, 48, 60, 1); eot(u, 0);   // the other order
  CHECK(parseBytes(smfFile(0, 96, {u}), p));
  CHECK_EQ(p.count, 2); expectNote(p.notes[0], 0, 60, 4, 25); expectNote(p.notes[1], 0, 60, 8, 22);
  Bytes v; on(v, 0, 60, 100, 0); off(v, 48, 60, 9); eot(v, 0);                                           // no match on the channel: pitch-only fallback
  CHECK(parseBytes(smfFile(0, 96, {v}), p)); CHECK_EQ(p.count, 1); expectNote(p.notes[0], 0, 60, 4, 25);
  Bytes w; on(w, 0, 42, 100, 9); on(w, 24, 42, 90, 9); off(w, 0, 42, 9); off(w, 24, 42, 9); eot(w, 0);  // same channel: FIFO as before
  CHECK(parseBytes(smfFile(0, 96, {w}), p)); CHECK_EQ(p.count, 2); expectNote(p.notes[0], 0, 42, 2, 25); expectNote(p.notes[1], 24, 42, 2, 22);
  Bytes x; on(x, 0, 60, 100, 0); on(x, 0, 60, 90, 1); on(x, 0, 60, 80, 0); off(x, 24, 60, 1); off(x, 24, 60, 0); off(x, 48, 60, 0); eot(x, 0);
  CHECK(parseBytes(smfFile(0, 96, {x}), p)); CHECK_EQ(p.count, 3);                                      // channel first, then FIFO within it
  expectNote(p.notes[0], 0, 60, 2, 22); expectNote(p.notes[1], 0, 60, 4, 25); expectNote(p.notes[2], 0, 60, 8, 20);
}

TEST(smf_cap_192_notes_earliest_first) {
  Bytes a; for (int i = 0; i < 200; i++) { on(a, i ? 3 : 100, 60, 100); off(a, 3, 60); } eot(a, 0);   // 200 notes, period 6 from tick 100
  Bytes b; for (int i = 0; i < 10; i++) { on(b, i ? 3 : 0, 40, 100); off(b, 3, 40); } eot(b, 0);     // 10 earlier notes
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(1, 96, {a, b}), p, &info));
  CHECK_EQ(p.count, kMaxNotes); CHECK(info.truncated); CHECK_EQ(info.notes, kMaxNotes);
  int low = 0; for (int i = 0; i < p.count; i++) if (p.notes[i].pitch() == 40) low++;
  CHECK_EQ(low, 10);                                       // all ten early notes survived the cap
  CHECK_EQ(p.notes[0].start(), 0); CHECK_EQ(p.notes[p.count - 1].start(), 100 + 6 * 181);
  for (int i = 1; i < p.count; i++) CHECK(p.notes[i - 1].start() <= p.notes[i].start());
  Bytes c; for (int i = 0; i < 195; i++) { on(c, i ? 3 : 0, 60, 100); off(c, 3, 60); } eot(c, 0);    // single track overflow
  CHECK(parseBytes(smfFile(0, 96, {c}), p, &info)); CHECK_EQ(p.count, kMaxNotes); CHECK(info.truncated); CHECK_EQ(p.notes[191].start(), 191 * 6);
}

TEST(smf_fit_notes_inside_length) {
  Bytes t; on(t, 1, 60, 100); off(t, 95, 60); eot(t, 0);              // late start, ends on the beat
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.lengthTicks, 96); expectNote(p.notes[0], 1, 60, 7, 25); CHECK(p.notes[0].end() <= 96);
  Bytes u; on(u, 90, 60, 100); off(u, 6, 60); eot(u, 0);              // blip shorter than a unit at the very end
  CHECK(parseBytes(smfFile(0, 96, {u}), p));
  CHECK_EQ(p.lengthTicks, 96); expectNote(p.notes[0], 90, 60, 1, 25);  // keeps its start; end 102 overruns by < one unit
  CHECK(p.notes[0].end() < 96 + kNoteLenUnit);
  Bytes u2; on(u2, 767, 41, 89); off(u2, 1, 41); eot(u2, 0);          // the GROOVE-E pre-beat accent, clipped by REAPER at 768
  CHECK(parseBytes(smfFile(0, 96, {u2}), p)); CHECK_EQ(p.lengthTicks, 768); expectNote(p.notes[0], 767, 41, 1, 22);
  VecFile f; CHECK(smfWrite(f, p, 120.0f)); Pattern q; SmfInfo qi; CHECK(parseBytes(f.data, q, &qi));   // the writer cuts it at the loop end
  CHECK(samePattern(p, q)); CHECK_EQ(qi.lengthTicksSrc, 768u); CHECK(!qi.truncated);
  Bytes u3; on(u3, 1530, 60, 100); off(u3, 6, 60); eot(u3, 0);        // the same at the 16-beat cap: inside the loop, start < length
  CHECK(parseBytes(smfFile(0, 96, {u3}), p)); CHECK_EQ(p.lengthTicks, 1536); expectNote(p.notes[0], 1530, 60, 1, 25);
  Bytes v; on(v, 0, 60, 100); off(v, 97, 60); eot(v, 0);              // genuinely past the beat: two beats
  CHECK(parseBytes(smfFile(0, 96, {v}), p)); CHECK_EQ(p.lengthTicks, 192); CHECK_EQ(p.notes[0].len(), 108);
}

TEST(smf_velocity_quantised) {
  const int vels[] = {1, 4, 64, 100, 127};
  const int expect[] = {3, 7, 67, 103, 127};
  for (int i = 0; i < 5; i++) {
    Bytes t; on(t, 0, 60, vels[i]); off(t, 48, 60); eot(t, 0);
    Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p)); CHECK_EQ(p.notes[0].vel(), expect[i]);
  }
}

TEST(smf_sorted_output_canonical) {
  Bytes t; on(t, 0, 67, 100); on(t, 0, 60, 100); on(t, 0, 64, 100); off(t, 96, 67); off(t, 0, 64); off(t, 0, 60);
  on(t, 0, 48, 50); on(t, 0, 48, 90); off(t, 48, 48); off(t, 48, 48); eot(t, 0);
  Pattern p; CHECK(parseBytes(smfFile(0, 96, {t}), p));
  CHECK_EQ(p.count, 5);
  expectNote(p.notes[0], 0, 60, 8, 25); expectNote(p.notes[1], 0, 64, 8, 25); expectNote(p.notes[2], 0, 67, 8, 25);
  expectNote(p.notes[3], 96, 48, 4, 12); expectNote(p.notes[4], 96, 48, 8, 22);   // same start & pitch: by length
}

TEST(smf_malformed) {
  Pattern p; SmfInfo info;
  Bytes t; on(t, 0, 60, 100); off(t, 48, 60); eot(t, 0);
  Bytes good = smfFile(0, 96, {t});
  Bytes bad = good; bad[3] = 'x'; CHECK(!parseBytes(bad, p));                      // not MThd
  Bytes shortHdr(good.begin(), good.begin() + 10); CHECK(!parseBytes(shortHdr, p)); // truncated header
  CHECK(!parseBytes(Bytes(), p));
  Bytes smpte = good; smpte[12] = 0xE7; smpte[13] = 0x28; CHECK(!parseBytes(smpte, p));   // SMPTE division
  Bytes zeroDiv = good; zeroDiv[12] = 0; zeroDiv[13] = 0; CHECK(!parseBytes(zeroDiv, p));
  Bytes fmt2 = good; fmt2[9] = 2; CHECK(!parseBytes(fmt2, p));                      // format 2 unsupported
  CHECK(!parseBytes(smfFile(0, 96, {}), p));                                        // no tracks
  CHECK(!parseBytes(smfFile(0, 96, {t}, "MXyz"), p));                               // no MTrk chunk
  Bytes big = good; big[18] = 0xFF; big[19] = 0xFF; big[20] = 0xFF; big[21] = 0xFF;  // bogus track length -> read to EOF
  CHECK(parseBytes(big, p, &info)); CHECK_EQ(p.count, 1);
  Bytes cut(good.begin(), good.end() - 5);                                          // file ends mid-track
  CHECK(parseBytes(cut, p, &info)); CHECK_EQ(p.count, 1); CHECK_EQ(p.notes[0].len(), 48);
  Bytes u; on(u, 0, 60, 100); off(u, 48, 60); ev(u, 0, {0x90, 0x85, 100}); on(u, 0, 62, 100); off(u, 48, 62); eot(u, 0);
  CHECK(parseBytes(smfFile(0, 96, {u}), p)); CHECK_EQ(p.count, 1);                 // data byte >= 0x80 ends the track
  Bytes v; ev(v, 0, {60, 100}); eot(v, 0);                                          // running status before any status
  CHECK(parseBytes(smfFile(0, 96, {v}), p)); CHECK_EQ(p.count, 0);
  Bytes longHdr = {'M', 'T', 'h', 'd'}; put32(longHdr, 8); put16(longHdr, 0); put16(longHdr, 1); put16(longHdr, 96); put16(longHdr, 0xABCD);
  longHdr.insert(longHdr.end(), good.begin() + 14, good.end());                     // header longer than 6: extra skipped
  CHECK(parseBytes(longHdr, p)); CHECK_EQ(p.count, 1);
  Bytes twoChunks = good; Bytes extra = {'X', 'T', 'r', 'k'}; put32(extra, 3); extra.insert(extra.end(), {1, 2, 3});
  twoChunks.insert(twoChunks.begin() + 14, extra.begin(), extra.end()); twoChunks[11] = 2;   // alien chunk first, ntracks 2
  CHECK(parseBytes(twoChunks, p)); CHECK_EQ(p.count, 1);
}

TEST(smf_reaper_negative_delta) {
  // REAPER writes a pickup note (starting before the item) with a 5-byte VLQ holding -128 as two's complement.
  Bytes t; t.insert(t.end(), {0x8F, 0xFF, 0xFF, 0xFF, 0x00, 0x90, 59, 54}); off(t, 272, 59); on(t, 0, 62, 100); off(t, 96, 62); eot(t, 0);
  Pattern p; SmfInfo info; CHECK(parseBytes(smfFile(0, 96, {t}), p, &info));
  CHECK_EQ(p.count, 2);
  expectNote(p.notes[0], 0, 59, 12, 13);                   // -128..144 -> clamped to 0..144
  expectNote(p.notes[1], 144, 62, 8, 25);                  // later events keep their true positions
  CHECK_EQ(p.lengthTicks, 288); CHECK_EQ(info.lengthTicksSrc, 240u);
}

TEST(smf_chunk_boundaries_and_short_reads) {
  // > 1 KB file whose events straddle the 512-byte chunk boundaries; identical results for whole-buffer parsing,
  // 512-byte chunking (MemFile) and 7-byte short reads through the IFile interface.
  Bytes t; meta(t, 0, 0x01, Bytes(497, 'x'));              // 0x14 header+chunk = 22 bytes, meta ends at byte 525
  for (int i = 0; i < 120; i++) { on(t, i ? 9 : 0, 40 + i % 40, 60 + i % 60); off(t, 3, 40 + i % 40); }   // period 12
  eot(t, 0);
  Bytes file = smfFile(0, 96, {t});
  CHECK(file.size() > 1400);
  Pattern a, b, c; SmfInfo ia, ib, ic;
  CHECK(parseBytes(file, a, &ia));
  VecFile v7(file, 7); CHECK(smfParse(v7, b, &ib));
  VecFile v1(file, 1); CHECK(smfParse(v1, c, &ic));
  CHECK_EQ(a.count, 120); CHECK(samePattern(a, b)); CHECK(samePattern(a, c));
  CHECK_EQ(ib.lengthTicksSrc, ia.lengthTicksSrc); CHECK_EQ(ic.notes, 120); CHECK_EQ(a.lengthTicks, 1440);
  for (int i = 0; i < a.count; i++) { CHECK_EQ(a.notes[i].start(), i * 12); CHECK_EQ(a.notes[i].len(), 12); }
  VecFile v(file); v.seek(100);                            // reader must not assume the file is at position 0
  CHECK(smfParse(v, b, &ib)); CHECK(samePattern(a, b));
}

static Pattern makeTestPattern() {
  Pattern p; p.clear(384);
  p.add(Note::make(0, 48, 60, 100)); p.add(Note::make(0, 48, 64, 90)); p.add(Note::make(0, 48, 67, 80));   // chord
  p.add(Note::make(48, 24, 60, 100)); p.add(Note::make(72, 24, 60, 70));                                   // retriggered pitch
  p.add(Note::make(96, 12, 36, 127)); p.add(Note::make(96, 12, 42, 1));
  p.add(Note::make(192, 96, 48, 64)); p.add(Note::make(240, 96, 48, 64));                                  // staggered same pitch, ends at 336
  p.add(Note::make(372, 12, 50, 40));                                                                      // ends exactly at 384
  return p;
}

TEST(smf_write_roundtrip_synthetic) {
  Pattern p = makeTestPattern();
  VecFile f; CHECK(smfWrite(f, p, 124.0f));
  CHECK(f.data.size() > 40);
  CHECK_EQ(memcmp(f.data.data(), "MThd", 4), 0); CHECK_EQ(f.data[9], 0); CHECK_EQ(f.data[11], 1); CHECK_EQ(f.data[12], 0); CHECK_EQ(f.data[13], 96);
  CHECK_EQ(memcmp(f.data.data() + 14, "MTrk", 4), 0);
  uint32_t declared = ((uint32_t)f.data[18] << 24) | (f.data[19] << 16) | (f.data[20] << 8) | f.data[21];
  CHECK_EQ(declared, f.data.size() - 22);                  // declared track length matches the bytes written
  CHECK_EQ(f.data.back(), 0); CHECK_EQ(f.data[f.data.size() - 2], 0x2F);
  Pattern q; SmfInfo info; CHECK(parseBytes(f.data, q, &info));
  CHECK(samePattern(p, q)); CHECK_NEAR(info.bpm, 124.0f, 0.01); CHECK_EQ(info.ppq, 96); CHECK(!info.truncated);
  VecFile short7(f.data, 7); Pattern r; CHECK(smfParse(short7, r)); CHECK(samePattern(p, r));
  // Through a real file as well (seek/read/write on FILE*).
  const char* path = "/tmp/lp_test_key_smf_roundtrip.mid";
  { PosixFile w(path, "wb"); CHECK(w.valid()); CHECK(smfWrite(w, p, 90.0f)); }
  { PosixFile rd(path); CHECK(rd.valid()); CHECK_EQ(rd.size(), f.data.size()); Pattern s; SmfInfo si; CHECK(smfParse(rd, s, &si)); CHECK(samePattern(p, s)); CHECK_NEAR(si.bpm, 90.0f, 0.01); }
  std::remove(path);
}

TEST(smf_write_nested_same_pitch) {
  // MIDI cannot express a note nested inside a longer note of the same pitch; FIFO matching re-reads the
  // pair as two staggered notes covering the same span (the sequencer steals the voice anyway, SPEC §9.12).
  Pattern p; p.clear(96); p.add(Note::make(0, 96, 60, 100)); p.add(Note::make(24, 24, 60, 100));
  VecFile f; CHECK(smfWrite(f, p, 120.0f));
  Pattern q; CHECK(parseBytes(f.data, q)); CHECK_EQ(q.count, 2);
  expectNote(q.notes[0], 0, 60, 4, 25); expectNote(q.notes[1], 24, 60, 6, 25);
}

TEST(smf_write_edge_cases) {
  Pattern e; e.clear(384);                                  // empty: only tempo, time signature and EOT
  VecFile f; CHECK(smfWrite(f, e, 120.0f));
  Pattern q; SmfInfo info; CHECK(parseBytes(f.data, q, &info)); CHECK_EQ(q.count, 0); CHECK_NEAR(info.bpm, 120.0f, 0.01);
  CHECK_EQ(q.lengthTicks, 96);                              // four silent beats collapse to the minimum (trailing-silence rule)
  Pattern full; full.clear(kMaxPatternTicks);               // 16 beats, 192 notes, last one ending exactly at the cap
  for (int i = 0; i < kMaxNotes; i++) full.add(Note::make((uint16_t)(i * 8), 12, (uint8_t)(36 + i % 48), (uint8_t)(1 + i % 127)));
  full.notes[kMaxNotes - 1] = Note::make(1524, 12, 100, 127);
  VecFile g; CHECK(smfWrite(g, full, 174.0f));
  CHECK(parseBytes(g.data, q, &info)); CHECK(samePattern(full, q)); CHECK_EQ(q.lengthTicks, kMaxPatternTicks); CHECK(!info.truncated);
  VecFile h; CHECK(smfWrite(h, e, 0.0f));                   // non-positive tempo falls back to the default
  CHECK(parseBytes(h.data, q, &info)); CHECK_NEAR(info.bpm, (float)kDefaultBpm, 0.01);
  Pattern tail; tail.clear(768); tail.add(Note::make(0, 48, 60, 100)); tail.add(Note::make(760, 24, 62, 100));   // end 784 > loop: cut at 768
  VecFile m; CHECK(smfWrite(m, tail, 120.0f)); CHECK(parseBytes(m.data, q, &info));
  CHECK_EQ(q.count, 2); CHECK_EQ(q.lengthTicks, 768); expectNote(q.notes[1], 760, 62, 1, 25); CHECK_EQ(info.lengthTicksSrc, 768u);
  Pattern sp; sp.clear(1536); sp.add(Note::make(0, 96, 60, 100)); sp.add(Note::make(1344, 96, 62, 100));   // silence inside the loop
  VecFile k; CHECK(smfWrite(k, sp, 120.0f)); CHECK(parseBytes(k.data, q, &info)); CHECK(samePattern(sp, q));
}

TEST(smf_write_tempo_range) {
  // Tempos the 24-bit set_tempo cannot hold, NaN and infinities are written as the default; the float -> uint32_t
  // cast never sees an out-of-range value.
  Pattern e; e.clear(96);
  const float bad[] = {0.001f, 3.0f, 3.9f, -5.0f, 1.0f / 0.0f, -1.0f / 0.0f, 0.0f / 0.0f, 1001.0f, 1e9f, 1e-30f};
  for (float bpm : bad) { VecFile f; CHECK(smfWrite(f, e, bpm)); Pattern q; SmfInfo info; CHECK(parseBytes(f.data, q, &info)); CHECK_NEAR(info.bpm, (float)kDefaultBpm, 0.01); }
  const float good[] = {4.0f, 40.0f, 123.456f, 240.0f, 1000.0f};
  for (float bpm : good) { VecFile f; CHECK(smfWrite(f, e, bpm)); Pattern q; SmfInfo info; CHECK(parseBytes(f.data, q, &info)); CHECK_NEAR(info.bpm, bpm, bpm * 1e-5 + 1e-3); }
  VecFile f; CHECK(smfWrite(f, e, 4.0f));                   // 15 000 000 us per quarter fits the 24-bit field
  CHECK_EQ(f.data[22 + 4], 15000000 >> 16); CHECK_EQ(f.data[22 + 5], (15000000 >> 8) & 0xFF); CHECK_EQ(f.data[22 + 6], 15000000 & 0xFF);
}

// ---------------------------------------------------------------------------------------------------------
// SMF: the real library on the card
// ---------------------------------------------------------------------------------------------------------
static bool endsWithMid(const std::string& n) { return n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".mid") == 0; }
static std::string midiRoot() { const char* e = std::getenv("LP_MIDI_DIR"); return e ? e : "sdcard/lp/midi"; }
static void listMidi(const std::string& dir, std::vector<std::string>& out) {
  DIR* d = opendir(dir.c_str()); if (!d) return;
  std::vector<std::string> names; while (dirent* e = readdir(d)) { if (e->d_name[0] != '.') names.push_back(e->d_name); } closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    std::string p = dir + "/" + n;
    DIR* sub = opendir(p.c_str());
    if (sub) { closedir(sub); listMidi(p, out); } else if (endsWithMid(n)) out.push_back(p);
  }
}
// A key folder may be sharded into subfolders s00, s01, ... (SPEC §9.2 FAT folder sizes): <Category>/<Key>/<file>
// is looked up in the folder itself, then in each of its subfolders.
static std::string locateMidi(const std::string& rel) {
  const std::string flat = midiRoot() + "/" + rel;
  if (FILE* f = std::fopen(flat.c_str(), "rb")) { std::fclose(f); return flat; }
  const size_t slash = rel.rfind('/');
  if (slash == std::string::npos) return flat;
  const std::string dir = midiRoot() + "/" + rel.substr(0, slash), base = rel.substr(slash + 1);
  DIR* d = opendir(dir.c_str()); if (!d) return flat;
  std::string found;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string p = dir + "/" + e->d_name + "/" + base;
    if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); found = p; break; }
  }
  closedir(d);
  return found.empty() ? flat : found;
}
static std::string categoryOf(const std::string& path) {
  size_t k = path.find("/midi/"); if (k == std::string::npos) return "?";
  size_t a = k + 6, b = path.find('/', a); return path.substr(a, b - a);
}
struct Agg { long files = 0, notes = 0, trunc = 0, beats = 0, tempo = 0; };

TEST(corpus_parse_all_files) {
  std::vector<std::string> files; listMidi(midiRoot(), files);
  CHECK(files.size() >= 300);
  std::map<std::string, Agg> agg; long chordPoly3 = 0, chordFiles = 0, maxNotes = 0, tails = 0, cutFiles = 0; double usec = 0;
  int failures = 0;
  for (const std::string& f : files) {
    PosixFile in(f.c_str()); CHECK(in.valid());
    Pattern p; SmfInfo info;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = smfParse(in, p, &info);
    usec += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) { failures++; std::printf("  parse failed: %s\n", f.c_str()); continue; }
    std::string cat = categoryOf(f); Agg& a = agg[cat];
    a.files++; a.notes += p.count; a.trunc += info.truncated; a.beats += p.lengthTicks / kTicksPerBeat; a.tempo += info.bpm > 0;
    maxNotes = std::max<long>(maxNotes, p.count);
    // Invariants: whole beats in 1..16, every note starts inside the loop and ends at or before it (a sub-unit
    // tail may overrun by < one unit), sorted, sane pitches/velocities.
    bool okLen = p.lengthTicks % kTicksPerBeat == 0 && p.lengthTicks >= kTicksPerBeat && p.lengthTicks <= kMaxPatternTicks;
    bool okNotes = p.count <= kMaxNotes && info.notes == p.count;
    for (int i = 0; i < p.count; i++) {
      const Note& n = p.notes[i];
      if (n.start() >= p.lengthTicks || n.end() >= p.lengthTicks + kNoteLenUnit || n.len() < kNoteLenUnit || n.pitch() > 127 || n.vel() < 3) okNotes = false;
      if (n.end() > p.lengthTicks) { tails++; if (n.len() != kNoteLenUnit) okNotes = false; }
      if (i && p.notes[i - 1].start() > n.start()) okNotes = false;
      if (cat == "Beat" && (n.pitch() < 36 || n.pitch() > 58)) okNotes = false;
    }
    if (!okLen || !okNotes) { std::printf("  invariant violated: %s (len %d, notes %d)\n", f.c_str(), p.lengthTicks, p.count); failures++; }
    // Properties against an independent raw walk of the file: every note-on becomes a note unless the file was
    // cut, and truncation is reported exactly when notes were lost or the source reaches beyond the window.
    in.seek(0); Bytes raw(in.size()); CHECK_EQ(in.read(raw.data(), raw.size()), raw.size());
    int ppq = 0; long ons = rawNoteOns(raw, &ppq);
    bool beyond = to96(info.lengthTicksSrc, ppq) > kMaxPatternTicks;
    bool okProps = ons >= 0 && ppq == info.ppq && p.count <= ons && (info.truncated || p.count == ons) && info.truncated == (ons > p.count || beyond);
    if (!okProps) { std::printf("  raw-walk property violated: %s (ons %ld, notes %d, truncated %d, srcLen %u)\n", f.c_str(), ons, p.count, info.truncated, info.lengthTicksSrc); failures++; }
    if (beyond) { cutFiles++; if (p.lengthTicks != kMaxPatternTicks) { std::printf("  cut file not 16 beats: %s\n", f.c_str()); failures++; } }
    if (cat == "Chord") { chordFiles++; if (maxPolyphony(p) >= 3) chordPoly3++; }
    if (info.bpm > 0) { if (info.bpm < 60.0f || info.bpm > 200.0f) { std::printf("  odd tempo %g in %s\n", info.bpm, f.c_str()); failures++; } }
  }
  CHECK_EQ(failures, 0);
  CHECK_EQ(maxNotes, kMaxNotes);
  CHECK(chordPoly3 * 100 >= chordFiles * 85);               // 255 of 293 Chord files are chordal; the rest are named melodies
  std::printf("  %zu files, %.1f ms total, %.1f us/file, chord files with >= 3 voices: %ld/%ld, loop-end tails %ld, cut files %ld\n",
              files.size(), usec / 1000.0, usec / files.size(), chordPoly3, chordFiles, tails, cutFiles);
  // Regression snapshot of the rules for this card image (tests/refmodel_smf.py reproduces these numbers).
  struct Ref { const char* cat; long files, notes, trunc, beats, tempo; } ref[] = {
      {"Bass", 2136, 79744, 50, 21866, 75}, {"Beat", 748, 48798, 20, 8325, 296}, {"Chord", 293, 16026, 26, 3811, 0},
      {"Fx", 76, 992, 2, 656, 0}, {"Lead", 161, 3227, 19, 2087, 26}, {"Pad", 94, 1618, 19, 1303, 0}, {"Percussion", 307, 12849, 2, 3979, 183}};
  long total = 0; bool sameImage = true;
  for (const Ref& r : ref) {
    const Agg& a = agg[r.cat]; total += a.files;
    if (a.files != r.files) { sameImage = false; std::printf("  %s: %ld files (card image differs from the snapshot: %ld) — skipping exact totals\n", r.cat, a.files, r.files); continue; }
    CHECK_EQ(a.notes, r.notes); CHECK_EQ(a.trunc, r.trunc); CHECK_EQ(a.beats, r.beats); CHECK_EQ(a.tempo, r.tempo);
  }
  if (sameImage) { CHECK_EQ(tails, 77); CHECK_EQ(cutFiles, 135); }   // 41 of the tails are pre-beat hits in the last unit of 16-beat files
  CHECK_EQ(total, (long)files.size());
}

TEST(corpus_exact_files) {
  struct Want { const char* rel; int L, n, ppq; float bpm; bool trunc; uint32_t srcLen; int first[6][4]; int last[3][4]; } want[] = {
    {"Beat/NA/NA__Beat__DPH-K4-GROOVE-C-3__e57c6fb1.mid", 768, 44, 96, 0, false, 768,
      {{0, 36, 8, 25}, {0, 38, 4, 14}, {48, 38, 4, 24}, {72, 37, 2, 15}, {96, 36, 8, 25}, {96, 38, 4, 14}}, {{720, 38, 4, 24}, {720, 43, 2, 13}, {744, 37, 2, 18}}},
    {"Chord/Amin/Amin__Chord__DHS-20-UptempoPiano-Am__cb4c2447.mid", 1536, 74, 96, 0, false, 1535,
      {{0, 48, 3, 23}, {0, 59, 4, 23}, {0, 62, 4, 23}, {0, 64, 4, 24}, {0, 67, 4, 24}, {48, 48, 3, 23}}, {{1392, 65, 4, 23}, {1440, 41, 4, 23}, {1488, 43, 4, 23}}},
    {"Bass/Dmin/Dmin__Chord__BassDerived__Bass-DM-Analog-Dm-5PN__7873ea13.mid", 1536, 36, 960, 0, false, 15110,   // format 1, 960 PPQ
      {{0, 50, 4, 24}, {0, 53, 4, 22}, {0, 57, 4, 21}, {144, 50, 4, 24}, {144, 53, 4, 22}, {144, 57, 4, 21}}, {{1488, 50, 2, 23}, {1488, 53, 2, 22}, {1488, 57, 2, 21}}},
    {"Bass/A/Amaj__Bass__1-2-3-4-Bass-A__052c0ae1.mid", 1536, 20, 480, 126.0001f, false, 7438,                      // tempo meta, 480 PPQ
      {{0, 45, 4, 25}, {72, 45, 4, 25}, {168, 45, 2, 25}, {216, 45, 4, 16}, {288, 45, 4, 25}, {384, 45, 4, 25}}, {{1320, 45, 2, 25}, {1368, 45, 4, 16}, {1440, 45, 4, 25}}},
    {"Bass/A#min/A#min__Bass__Shift-Along-Bass-1-Bb-min__a119ad34.mid", 1536, 20, 480, 126.0001f, true, 8160,     // 64-beat file, cut; srcLen = exit time
      {{0, 34, 4, 30}, {72, 34, 2, 28}, {147, 36, 6, 26}, {216, 36, 4, 21}, {290, 36, 2, 27}, {408, 34, 4, 30}}, {{1371, 36, 6, 26}, {1440, 36, 4, 21}, {1514, 36, 1, 27}}},
    {"Bass/G#min/G#min__Bass__DBS-Ascending1-G#m__822cf421.mid", 768, 7, 96, 0, false, 728,                        // EOT extends the loop
      {{0, 20, 4, 20}, {48, 20, 4, 20}, {96, 25, 3, 17}, {175, 28, 2, 20}, {432, 20, 3, 20}, {480, 25, 3, 20}}, {{432, 20, 3, 20}, {480, 25, 3, 20}, {559, 28, 2, 17}}},
    {"Pad/Emin/Emin__Pad__Pad-DM-SineSpace-1-Em-5PN__ac753c56.mid", 1440, 31, 96, 0, false, 1408,                  // REAPER negative delta
      {{0, 59, 14, 13}, {0, 64, 14, 13}, {0, 67, 14, 13}, {16, 71, 12, 9}, {160, 59, 8, 14}, {160, 64, 8, 14}}, {{1312, 59, 8, 10}, {1312, 66, 8, 14}, {1312, 71, 8, 14}}},
    {"Percussion/NA/NA__Percussion__MT-HH-LP13__c5b2539f.mid", 1536, 32, 480, 126.0001f, false, 7555,              // unterminated note
      {{26, 39, 2, 21}, {50, 38, 2, 23}, {122, 39, 2, 22}, {146, 38, 2, 26}, {218, 39, 2, 21}, {242, 38, 2, 25}}, {{1415, 38, 2, 23}, {1487, 39, 2, 21}, {1511, 38, 1, 24}}},
    {"Beat/NA/NA__Beat__DHB-KI-06-GROOVE-E__a18a445a.mid", 768, 46, 96, 0, false, 768,                                 // pre-beat accents at 95, 191, ... 767: the last keeps its start
      {{0, 36, 4, 23}, {0, 42, 2, 10}, {48, 38, 2, 22}, {48, 40, 8, 5}, {95, 41, 2, 22}, {96, 36, 4, 23}}, {{720, 38, 2, 22}, {720, 40, 4, 5}, {767, 41, 1, 22}}},
    {"Bass/Dmin/Dmin__Bass__Bass-DM-Foundation-Dm-5PN__2c115ed6.mid", 1536, 12, 96, 0, true, 1608,                    // 31-beat file, notes end at beat 15: cut to 16
      {{72, 50, 2, 25}, {168, 50, 2, 25}, {264, 50, 2, 24}, {456, 50, 2, 25}, {552, 50, 2, 25}, {648, 50, 2, 24}}, {{1224, 45, 2, 25}, {1320, 45, 2, 25}, {1416, 45, 2, 24}}},
    {"Bass/Fmin/Fmin__Bass__Bookit-Bass-F-min__9a4f9a6c.mid", 1536, 10, 480, 126.0001f, true, 8023,                    // 32-beat file, first 16 beats hold 8 beats of notes: 16, not 8
      {{0, 48, 6, 23}, {71, 48, 6, 27}, {143, 48, 6, 25}, {216, 48, 6, 19}, {287, 48, 7, 25}, {384, 41, 6, 23}}, {{527, 41, 6, 25}, {600, 41, 6, 19}, {671, 41, 6, 25}}},
    {"Bass/A#min/A#min__Bass__DHB-123BPM-PART-09-BASS-01-A#__b5cfb398.mid", 1536, 14, 96, 0, true, 1560,              // EOT just past 16 beats: cut
      {{0, 46, 2, 24}, {72, 46, 2, 24}, {144, 46, 2, 24}, {336, 42, 2, 24}, {384, 46, 2, 24}, {456, 46, 2, 24}}, {{1152, 44, 2, 24}, {1224, 44, 2, 24}, {1296, 44, 2, 24}}},
  };
  for (const Want& w : want) {
    std::string path = locateMidi(w.rel);
    PosixFile in(path.c_str());
    if (!in.valid()) { std::printf("  missing %s\n", path.c_str()); CHECK(in.valid()); continue; }
    Pattern p; SmfInfo info; CHECK(smfParse(in, p, &info));
    CHECK_EQ(p.lengthTicks, w.L); CHECK_EQ(p.count, w.n); CHECK_EQ(info.ppq, w.ppq); CHECK_NEAR(info.bpm, w.bpm, 0.01);
    CHECK_EQ(info.truncated, w.trunc); CHECK_EQ(info.lengthTicksSrc, w.srcLen);
    if (p.count < 6) continue;
    for (int i = 0; i < 6; i++) expectNote(p.notes[i], w.first[i][0], w.first[i][1], w.first[i][2], w.first[i][3]);
    for (int i = 0; i < 3; i++) expectNote(p.notes[p.count - 3 + i], w.last[i][0], w.last[i][1], w.last[i][2], w.last[i][3]);
  }
}

TEST(corpus_roundtrip_and_buffer_equivalence) {
  std::vector<std::string> files; listMidi(midiRoot(), files);
  CHECK(files.size() >= 300);
  long roundTrips = 0, mismatches = 0, bufferMismatches = 0, shorter = 0;
  for (size_t k = 0; k < files.size(); k++) {
    PosixFile in(files[k].c_str());
    Pattern p; SmfInfo info; if (!smfParse(in, p, &info)) { mismatches++; continue; }
    VecFile out; if (!smfWrite(out, p, info.bpm > 0 ? info.bpm : 120.0f)) { mismatches++; continue; }
    Pattern q; SmfInfo qi;
    bool ok = parseBytes(out.data, q, &qi) && !qi.truncated && sameNotes(p, q);
    if (ok && q.lengthTicks != p.lengthTicks) {
      // A cut file is forced to 16 beats; when its notes end more than two beats before the cut the export
      // cannot say it was cut, so it re-parses to the note-based length (set files restore the loop length
      // themselves, SPEC §9.8 lp.track.N.len). Every other file keeps its length.
      int maxEnd = 0; for (int i = 0; i < p.count; i++) maxEnd = std::max<int>(maxEnd, std::min<int>(p.notes[i].end(), p.lengthTicks));
      ok = info.truncated && p.lengthTicks == kMaxPatternTicks && kMaxPatternTicks - maxEnd > 2 * kTicksPerBeat && q.lengthTicks == ceilBeats(maxEnd);
      if (ok) shorter++;
    }
    if (!ok) { mismatches++; std::printf("  round trip differs: %s\n", files[k].c_str()); continue; }
    roundTrips++;
    if (k % 7 == 0) {                                      // whole-buffer parse must match the streamed parse
      in.seek(0); Bytes raw(in.size()); CHECK_EQ(in.read(raw.data(), raw.size()), raw.size());
      Pattern r; SmfInfo ri; VecFile v(raw, 11);
      if (!parseBytes(raw, r, &ri) || !samePattern(p, r) || ri.lengthTicksSrc != info.lengthTicksSrc) bufferMismatches++;
      if (!smfParse(v, r, &ri) || !samePattern(p, r)) bufferMismatches++;
    }
  }
  CHECK_EQ(mismatches, 0); CHECK_EQ(bufferMismatches, 0);
  CHECK_EQ(roundTrips, (long)files.size());
  if (files.size() == 3815) CHECK_EQ(shorter, 7);
  std::printf("  %ld files round-tripped write -> parse (%ld cut files re-parse to their note-based length)\n", roundTrips, shorter);
}

int main() { return lp_test::run(); }
