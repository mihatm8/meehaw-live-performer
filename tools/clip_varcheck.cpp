// Host harness for generated / imported melodic clips (docs/CLIP_GENERATOR.md): parses every .mid beneath argv[1]
// with the firmware's smfParseBuffer and builds the four variants with the firmware's buildVariant (variants.cpp),
// then reports parse failures, truncations, empty variants, unchanged "Variation" slots (v1 == v0), wrong crop
// lengths and clips with a pitch outside the project key. argv[2] is the key root (0 = C ... 9 = A, default 9);
// the category is taken from the path (/Chord/, /Lead/, /Pad/, else Bass), so point it at one key's folders:
//
//   clang++ -std=c++17 -O2 -Ifirmware/liveplayer/src tools/clip_varcheck.cpp firmware/liveplayer/src/gen/key.cpp \
//       firmware/liveplayer/src/gen/smf.cpp firmware/liveplayer/src/gen/variants.cpp -o /tmp/lp_clip_varcheck
//   /tmp/lp_clip_varcheck <dir with Chord/ Lead/ Pad/ subfolders of ONE key> <root> [-v]
//
// -v also prints one line per file: "NOOP <path>" when the Variation equals the original, "OUTKEY <path>" for an
// out-of-key pitch, "EMPTY <path>" / "FAIL <path>" for the hard failures.
//
// Reads the firmware sources, never changes them. Adapted from the research prototype (scratchpad procgen/varcheck.cpp).
#include "core/types.h"
#include "gen/key.h"
#include "gen/smf.h"
#include "gen/variants.h"
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using namespace lp;

static void listMidi(const std::string& d, std::vector<std::string>& out) {
  DIR* dir = opendir(d.c_str());
  if (!dir) return;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] == '.') continue;
    std::string p = d + "/" + e->d_name;
    if (e->d_type == DT_DIR) listMidi(p, out);
    else if (p.size() > 4 && strcasecmp(p.c_str() + p.size() - 4, ".mid") == 0) out.push_back(p);
  }
  closedir(dir);
}

static Category catOf(const std::string& p) {
  if (p.find("/Chord/") != std::string::npos) return Chord;
  if (p.find("/Lead/") != std::string::npos) return Lead;
  if (p.find("/Pad/") != std::string::npos) return Pad;
  return Bass;
}

static bool same(const Pattern& a, const Pattern& b) {
  if (a.count != b.count || a.lengthTicks != b.lengthTicks) return false;
  for (int i = 0; i < a.count; i++) if (a.notes[i].packed != b.notes[i].packed) return false;
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <dir> [root 0..11]\n", argv[0]); return 2; }
  std::vector<std::string> files;
  listMidi(argv[1], files);
  int root = argc > 2 ? atoi(argv[2]) : 9;
  bool verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
  ProjectKey key;
  key.root = (uint8_t)root;
  struct Agg { long files = 0, parseFail = 0, trunc = 0, empty[4] = {0, 0, 0, 0}, unchanged1 = 0, badLen[4] = {0, 0, 0, 0}, outOfKey = 0, notes = 0; };
  std::map<Category, Agg> agg;
  for (const std::string& f : files) {
    FILE* fp = fopen(f.c_str(), "rb");
    if (!fp) continue;
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(fp);
    Category c = catOf(f);
    Agg& a = agg[c];
    a.files++;
    Pattern src;
    SmfInfo info;
    if (!smfParseBuffer(data.data(), (uint32_t)data.size(), src, &info)) {
      a.parseFail++;
      if (verbose) printf("FAIL %s\n", f.c_str());
      continue;
    }
    a.trunc += info.truncated;
    a.notes += src.count;
    for (int i = 0; i < src.count; i++) {
      if (!key.contains(src.notes[i].pitch())) { a.outOfKey++; if (verbose) printf("OUTKEY %s\n", f.c_str()); break; }
    }
    Pattern v[4];
    for (int k = 0; k < 4; k++) {
      buildVariant(c, src, k, 0xC0FFEEu + (uint32_t)a.files, key, v[k]);
      if (v[k].count == 0) { a.empty[k]++; if (verbose) printf("EMPTY v%d %s\n", k, f.c_str()); }
      uint16_t want = src.lengthTicks;
      if (c != Pad && k == 2) want = 2 * kPPQN;
      if (c != Pad && k == 3) want = kPPQN / 2;
      if (v[k].lengthTicks != want) a.badLen[k]++;
    }
    if (same(v[0], v[1])) { a.unchanged1++; if (verbose) printf("NOOP %s\n", f.c_str()); }
  }
  for (auto& kv : agg) {
    const Agg& a = kv.second;
    const char* name = kv.first == Chord ? "Chord" : kv.first == Lead ? "Lead" : kv.first == Pad ? "Pad" : "Other";
    printf("%-6s files %ld parseFail %ld truncated %ld notes/clip %.1f | empty variants v0 %ld v1 %ld v2 %ld v3 %ld | v1==v0 %ld | bad lengths v2 %ld v3 %ld | clips with out-of-key pitch %ld\n",
           name, a.files, a.parseFail, a.trunc, a.files ? (double)a.notes / a.files : 0.0, a.empty[0], a.empty[1], a.empty[2], a.empty[3],
           a.unchanged1, a.badLen[2], a.badLen[3], a.outOfKey);
  }
  return 0;
}
