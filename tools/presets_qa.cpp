// Preset loudness QA harness (host). Renders every preset of every synth category on the fixed phrases of
// tools/preset_loudness.hpp and prints one machine-readable line per preset plus a per-family summary.
// Driven by tools/gen_presets.py --qa (which compiles it with clang++ against the real dsp sources, reads the
// ROW lines, computes the level trims and bakes them into dsp/presets_gen.hpp), and handy on its own:
//   clang++ -std=c++17 -O2 -Ifirmware/liveplayer/src tools/presets_qa.cpp firmware/liveplayer/src/dsp/{synth_voice,presets,wavetables,svf,env}.cpp -o /tmp/lp_presets_qa && /tmp/lp_presets_qa
// Output:
//   ROW <cat> <index> <family> <name> <level> <loudDb> <rmsDb> <peak> <peakC4> <peakExtreme> [<w100> <w200> <w400>]
//   TARGET <cat> <family> <medianDb> <handMin> <handMax>        (family: lib | acid; hand rows only)
//   OUTLIER ...                                                   (generated rows beyond +-1.5 dB of their target)
//   SUMMARY ...
// Exit code 0 always (gen_presets.py judges the numbers); 'quiet' as the first argument drops the extra windows.
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "preset_loudness.hpp"

using namespace lp;

int main(int argc, char** argv) {
  const bool extra = !(argc > 1 && std::strcmp(argv[1], "quiet") == 0);
  const Category cats[] = { Chord, Lead, Pad, Bass, Fx };
  int outliers = 0, total = 0, clampedPeaks = 0;
  for (Category c : cats) {
    const int lib = libraryPresetCount(c), all = presetCount(c);
    const int hl = lp_qa::handLibrary(c), ha = lp_qa::handAcid(c);
    std::vector<lp_qa::Measure> ms((size_t)all);
    std::vector<float> handLib, handAcid;
    for (int i = 0; i < all; i++) {
      const SynthPatch& p = preset(c, i);
      ms[(size_t)i] = lp_qa::measure(c, p);
      const lp_qa::Measure& m = ms[(size_t)i];
      const bool acid = i >= lib;
      const bool hand = acid ? (i - lib) < ha : i < hl;
      if (hand) (acid ? handAcid : handLib).push_back(m.loudDb);
      std::printf("ROW %s %d %s %s %.4f %.2f %.2f %.3f %.3f %.3f", categoryName(c), i, acid ? "acid" : "lib", p.name, (double)p.level,
                  (double)m.loudDb, (double)m.rmsDb, (double)m.peak, (double)m.peakC4, (double)m.peakExtreme);
      if (extra) {
        std::vector<float> x = lp_qa::renderPhrase(c, p);
        std::printf(" %.2f %.2f %.2f", (double)lp_qa::maxWindowRmsDb(x, 0.100f, 0.025f), (double)lp_qa::maxWindowRmsDb(x, 0.200f, 0.050f),
                    (double)lp_qa::maxWindowRmsDb(x, 0.400f, 0.100f));
      }
      std::printf("%s\n", m.finite ? "" : " NONFINITE");
      total++;
    }
    auto spread = [](const std::vector<float>& v, float& lo, float& hi) { lo = 1e9f; hi = -1e9f; for (float x : v) { if (x < lo) lo = x; if (x > hi) hi = x; } if (v.empty()) lo = hi = 0; };
    float lo, hi;
    const float tLib = lp_qa::medianOf(handLib); spread(handLib, lo, hi);
    std::printf("TARGET %s lib %.2f %.2f %.2f\n", categoryName(c), (double)tLib, (double)lo, (double)hi);
    float tAcid = tLib;
    if (ha > 0) { tAcid = lp_qa::medianOf(handAcid); spread(handAcid, lo, hi); std::printf("TARGET %s acid %.2f %.2f %.2f\n", categoryName(c), (double)tAcid, (double)lo, (double)hi); }
    for (int i = 0; i < all; i++) {
      const bool acid = i >= lib;
      const bool hand = acid ? (i - lib) < ha : i < hl;
      if (hand) continue;
      const float d = ms[(size_t)i].loudDb - (acid ? tAcid : tLib);
      if (std::fabs(d) > 1.5f) { outliers++; std::printf("OUTLIER %s %d %s %+.2f dB (level %.3f)\n", categoryName(c), i, preset(c, i).name, (double)d, (double)preset(c, i).level); }
      if (ms[(size_t)i].peakC4 >= 1.5f || ms[(size_t)i].peakExtreme >= 1.5f) { clampedPeaks++; std::printf("PEAK %s %d %s c4 %.3f extreme %.3f\n", categoryName(c), i, preset(c, i).name, (double)ms[(size_t)i].peakC4, (double)ms[(size_t)i].peakExtreme); }
    }
  }
  std::printf("SUMMARY presets %d outliers %d peaks_over_1.5 %d\n", total, outliers, clampedPeaks);
  return 0;
}
