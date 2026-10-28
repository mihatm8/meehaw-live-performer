// Host tests for src/gen/variants.cpp (pattern_variants.lua port, SPEC §5, §6, §9.6, §9.7).
#include "check.h"
#include "gen/variants.h"
#include <set>
#include <string>
#include <vector>
using namespace lp;

namespace {
const int Q = kPPQN;                                               // 96 ticks per beat
int qv(int v) { return ((v >> 2) & 31) * 4 + 3; }                  // velocity after Note packing
int sv(int v, float scale) { int r = (int)(v * scale + 0.5f); return qv(r < 1 ? 1 : r); }   // filtered() :302 then packed
void add(Pattern& p, int start, int len, int pitch, int vel) { CHECK(p.add(Note::make((uint16_t)start, (uint16_t)len, (uint8_t)pitch, (uint8_t)vel))); }
Pattern mk(int lengthTicks) { Pattern p; p.clear((uint16_t)lengthTicks); return p; }

bool same(const Pattern& a, const Pattern& b) {
  if (a.count != b.count || a.lengthTicks != b.lengthTicks) return false;
  for (int i = 0; i < a.count; i++) if (a.notes[i].packed != b.notes[i].packed) return false;
  return true;
}
std::string sig(const Pattern& p) {
  std::string s = std::to_string(p.lengthTicks) + "|";
  for (int i = 0; i < p.count; i++) s += std::to_string(p.notes[i].packed) + ",";
  return s;
}
// Sorted by (start, pitch), every start inside the loop, lengths in whole units, ends inside the loop
// except a single-unit note at the very end (the packed minimum), count within the buffer.
bool valid(const Pattern& p) {
  if (p.count > kMaxNotes) return false;
  for (int i = 0; i < p.count; i++) {
    const Note& n = p.notes[i];
    if (n.start() >= p.lengthTicks) return false;
    if (n.len() < kNoteLenUnit || n.len() % kNoteLenUnit) return false;
    if (n.end() > p.lengthTicks && n.len() != kNoteLenUnit) return false;
    if (i > 0) {
      const Note& m = p.notes[i - 1];
      if (m.start() > n.start()) return false;
      if (m.start() == n.start() && m.pitch() > n.pitch()) return false;
      if (m.start() == n.start() && m.pitch() == n.pitch()) return false;   // duplicates merged
    }
  }
  return true;
}
std::set<int> pitchesAt(const Pattern& p, int start) { std::set<int> s; for (int i = 0; i < p.count; i++) if (p.notes[i].start() == start) s.insert(p.notes[i].pitch()); return s; }
std::set<int> pitches(const Pattern& p) { std::set<int> s; for (int i = 0; i < p.count; i++) s.insert(p.notes[i].pitch()); return s; }
int countPitch(const Pattern& p, int pitch) { int c = 0; for (int i = 0; i < p.count; i++) if (p.notes[i].pitch() == pitch) c++; return c; }
const Note* find(const Pattern& p, int start, int pitch) { for (int i = 0; i < p.count; i++) if (p.notes[i].start() == start && p.notes[i].pitch() == pitch) return &p.notes[i]; return nullptr; }
bool inKey(const ProjectKey& k, const Pattern& p) { for (int i = 0; i < p.count; i++) if (!k.contains(p.notes[i].pitch())) return false; return true; }

// Fixtures (ticks): the chord progression, lead motif, breakbeat and percussion loop of the Lua tests.
Pattern chordsFixture() {                                          // Am (57 60 64) then F (53 57 60), 1.8 qn each
  Pattern p = mk(4 * Q);
  add(p, 0, 173, 57, 90); add(p, 0, 173, 60, 84); add(p, 0, 173, 64, 80);
  add(p, 2 * Q, 173, 53, 90); add(p, 2 * Q, 173, 57, 84); add(p, 2 * Q, 173, 60, 80);
  return p;
}
Pattern leadFixture() {                                            // 60 62 65 64 60
  Pattern p = mk(4 * Q);
  add(p, 0, 36, 60, 90); add(p, 48, 36, 62, 80); add(p, 96, 72, 65, 90); add(p, 192, 72, 64, 85); add(p, 288, 72, 60, 90);
  return p;
}
Pattern coreBeat() {                                               // 36@0,2 37@1,3 38@0.5 39@2.5
  Pattern p = mk(4 * Q);
  add(p, 0, 12, 36, 100); add(p, 192, 12, 36, 100); add(p, 96, 12, 37, 100); add(p, 288, 12, 37, 100);
  add(p, 48, 12, 38, 100); add(p, 240, 12, 39, 100);
  return p;
}
Pattern busyBeat() {                                               // core + every accent 40..58 at 3.5 with its own velocity
  Pattern p = coreBeat();
  for (int pitch = 40; pitch <= 58; pitch++) add(p, 336, 12, pitch, 60 + (pitch - 40) * 3);
  return p;
}
Pattern percEight() { Pattern p = mk(4 * Q); for (int i = 0; i < 8; i++) add(p, i * 48, 24, 50, 100); return p; }
Pattern percFive() {                                               // 50 ×8, 52 ×4, 58 ×4, 54 ×2, 56 ×1 (56 shares voice 36 with 50)
  Pattern p = mk(4 * Q);
  for (int i = 0; i < 8; i++) add(p, i * 48, 24, 50, 100);
  for (int i = 0; i < 4; i++) add(p, 24 + i * 96, 24, 52, 90);
  for (int i = 0; i < 4; i++) add(p, 72 + i * 96, 24, 58, 80);
  add(p, 36, 24, 54, 70); add(p, 228, 24, 54, 70);
  add(p, 12, 24, 56, 120);
  return p;
}
Pattern tonalChords() {                                            // C E G | D F A, one beat each
  Pattern p = mk(4 * Q);
  add(p, 0, 96, 60, 90); add(p, 0, 96, 64, 85); add(p, 0, 96, 67, 80);
  add(p, 96, 96, 62, 88); add(p, 96, 96, 65, 84); add(p, 96, 96, 69, 80);
  return p;
}
const ProjectKey kKey;                                             // A minor
uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16; return x; }
Rng R(uint32_t seed) { return Rng(mix32(seed)); }                  // hashed seed: raw small seeds give correlated first draws
} // namespace

// ---------------------------------------------------------------- rng / key stub sanity
TEST(rng_is_deterministic_and_bounded) {
  Rng a(7), b(7), z(0);
  CHECK(z.s != 0);
  for (int i = 0; i < 1000; i++) { int r = a.range(1, 3); CHECK(r >= 1 && r <= 3); CHECK_EQ(r, b.range(1, 3)); float u = z.unit(); CHECK(u >= 0.0f && u < 1.0f); }
}
TEST(key_natural_minor_contract) {
  CHECK(kKey.contains(69) && kKey.contains(60) && !kKey.contains(61) && !kKey.contains(66));
  CHECK_EQ(kKey.snap(61), 60);                                     // down first (key.lua:87)
  CHECK_EQ(kKey.snap(63), 62);
  CHECK_EQ(kKey.moveScaleSteps(60, 1), 62);
  CHECK_EQ(kKey.moveScaleSteps(60, -1), 59);
  CHECK_EQ(kKey.moveScaleSteps(69, 2), 72);
  CHECK_EQ(kKey.moveScaleSteps(69, -2), 65);
}

// ---------------------------------------------------------------- normalize (through crop at the source length)
TEST(normalize_clips_merges_and_sorts) {
  Pattern p = mk(4 * Q);
  add(p, 380, 96, 62, 100);                                        // runs past the loop: one unit kept, overshoot allowed
  add(p, 370, 96, 64, 100);                                        // 14 ticks left: rounded down to one unit, end 382
  add(p, 0, 48, 60, 50);                                           // duplicate (0, 60): merged, max end + max velocity
  add(p, 0, 96, 60, 100);
  add(p, 200, 48, 61, 70);
  Pattern out; cropPattern(p, 4 * Q, out);
  CHECK(valid(out));
  CHECK_EQ(out.count, 4);
  CHECK_EQ(out.notes[0].start(), 0); CHECK_EQ(out.notes[0].pitch(), 60); CHECK_EQ(out.notes[0].len(), 96); CHECK_EQ(out.notes[0].vel(), qv(100));
  CHECK_EQ(out.notes[1].start(), 200);
  CHECK_EQ(out.notes[2].start(), 370); CHECK_EQ(out.notes[2].len(), 12);
  CHECK_EQ(out.notes[3].start(), 380); CHECK_EQ(out.notes[3].len(), 12);
  Pattern late = mk(4 * Q); add(late, 400, 12, 36, 100); add(late, 0, 12, 36, 100);
  cropPattern(late, 4 * Q, out);
  CHECK_EQ(out.count, 1);                                          // start >= length dropped
}

// ---------------------------------------------------------------- crop (:102-124)
TEST(crop_keeps_leading_fragment_and_clamps) {
  Pattern p = mk(4 * Q);
  add(p, 0, 96, 60, 100); add(p, 48, 96, 64, 100); add(p, 200, 48, 67, 100); add(p, 300, 48, 69, 100);
  Pattern out; cropPattern(p, 2 * Q, out);
  CHECK(valid(out)); CHECK_EQ(out.lengthTicks, 192); CHECK_EQ(out.count, 2);
  CHECK_EQ(out.notes[1].start(), 48); CHECK_EQ(out.notes[1].end(), 144);
  cropPattern(p, Q / 2, out);
  CHECK(valid(out)); CHECK_EQ(out.lengthTicks, 48); CHECK_EQ(out.count, 1);
  CHECK_EQ(out.notes[0].pitch(), 60); CHECK_EQ(out.notes[0].end(), 48);
  cropPattern(p, 0, out);                                          // degenerate length → 1 tick
  CHECK_EQ(out.lengthTicks, 1); CHECK(valid(out));
}
TEST(crop_moves_first_onset_group_when_nothing_kept) {
  Pattern p = mk(4 * Q);
  add(p, 288, 96, 60, 100); add(p, 288, 48, 64, 90); add(p, 300, 48, 67, 80);
  Pattern out; cropPattern(p, Q / 2, out);
  CHECK(valid(out)); CHECK_EQ(out.count, 2); CHECK_EQ(out.lengthTicks, 48);
  CHECK_EQ(out.notes[0].start(), 0); CHECK_EQ(out.notes[0].pitch(), 60); CHECK_EQ(out.notes[0].end(), 48);
  CHECK_EQ(out.notes[1].start(), 0); CHECK_EQ(out.notes[1].pitch(), 64); CHECK_EQ(out.notes[1].end(), 48);
  Pattern one = mk(4 * Q); add(one, 300, 12, 60, 100);
  cropPattern(one, Q / 2, out);
  CHECK_EQ(out.count, 1); CHECK_EQ(out.notes[0].start(), 0); CHECK_EQ(out.notes[0].len(), 12);   // duration kept
  Pattern empty = mk(4 * Q);
  cropPattern(empty, Q / 2, out);
  CHECK_EQ(out.count, 0); CHECK_EQ(out.lengthTicks, 48);
  cropPattern(one, 2 * Q, one);                                    // aliasing src == out
  CHECK_EQ(one.count, 1); CHECK_EQ(one.notes[0].start(), 0); CHECK_EQ(one.lengthTicks, 192);
}

// ---------------------------------------------------------------- chord_variation (:169-216)
TEST(chord_variation_keeps_harmony_and_count) {
  const Pattern src = chordsFixture();
  const std::set<int> c0 = {9, 0, 4}, c2 = {5, 9, 0};
  std::set<std::string> seen;
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Rng rng = R(seed); Pattern out; chordVariation(src, rng, kKey, out);
    CHECK(valid(out)); CHECK_EQ(out.count, 6); CHECK_EQ(out.lengthTicks, 384); CHECK(inKey(kKey, out));
    int len0 = out.notes[0].len();
    for (int i = 0; i < out.count; i++) {
      const Note& n = out.notes[i];
      const std::set<int>& classes = n.start() == 0 ? c0 : c2;
      CHECK(n.start() == 0 || n.start() == 192);
      CHECK(classes.count(n.pitch() % 12) == 1);
      CHECK_EQ(n.len(), len0);                                     // one articulation gate for the whole bank
      CHECK(n.len() == 168 || n.len() == 144 || n.len() == 180);   // 180 × 0.88 / 0.75 / 0.95 in 12-tick units
      CHECK(n.vel() <= qv(90));
    }
    // first chord: base always wins (anchor is base, up/down cost >= 1.15 > max tie-break 0.3)
    CHECK(pitchesAt(out, 0) == std::set<int>({57, 60, 64}));
    // second chord: inversion {57, 60, 65} is closest to {57, 60, 64} (score 2.8 vs 11 for base)
    CHECK(pitchesAt(out, 192) == std::set<int>({57, 60, 65}));
    CHECK_EQ(out.notes[0].vel(), qv(90));                          // voice 1 velocity untouched
    seen.insert(sig(out));
    Rng rng2 = R(seed); Pattern again; chordVariation(src, rng2, kKey, again);
    CHECK(same(out, again));                                       // deterministic
  }
  CHECK(seen.size() >= 2);                                         // articulation differs between seeds
  CHECK(same(src, chordsFixture()));                               // source untouched
}
TEST(chord_variation_scores_in_integers_first_candidate_wins_ties) {
  // {45, 69} then {57, 60}: for the second chord the up {60, 69} and down {48, 57} inversions tie exactly
  // (movement 15, deviation 12 each) and the base {57, 60} loses (movement 21). With integer scores the
  // winner depends only on the two tie-break draws: up unless the down draw is strictly smaller (:199).
  Pattern p = mk(4 * Q);
  add(p, 0, 96, 45, 100); add(p, 0, 96, 69, 100); add(p, 192, 96, 57, 100); add(p, 192, 96, 60, 100);
  int ups = 0, downs = 0;
  for (uint32_t seed = 1; seed <= 60; seed++) {
    Rng replay = R(seed);
    for (int i = 0; i < 5; i++) replay.range(1, 3);                // articulation, chord 1 ×3, chord 2 base
    const int tUp = replay.range(1, 3), tDown = replay.range(1, 3);
    Rng rng = R(seed); Pattern out; chordVariation(p, rng, kKey, out);
    CHECK(valid(out)); CHECK_EQ(out.count, 4);
    CHECK(pitchesAt(out, 0) == std::set<int>({45, 69}));           // first chord: the base always wins
    const std::set<int> expect = tUp <= tDown ? std::set<int>({60, 69}) : std::set<int>({48, 57});
    CHECK(pitchesAt(out, 192) == expect);
    if (tUp <= tDown) ups++; else downs++;
  }
  CHECK(ups > 0 && downs > 0);
}
TEST(chord_variation_snaps_and_handles_edges) {
  Pattern p = mk(2 * Q); add(p, 0, 96, 61, 100); add(p, 0, 96, 66, 100);   // out-of-key C# F#
  Rng rng = R(3); Pattern out; chordVariation(p, rng, kKey, out);
  CHECK(valid(out)); CHECK_EQ(out.count, 2); CHECK(inKey(kKey, out));
  Pattern empty = mk(Q); Pattern e; chordVariation(empty, rng, kKey, e);
  CHECK_EQ(e.count, 0); CHECK_EQ(e.lengthTicks, 96);
  Pattern single = mk(Q); add(single, 0, 96, 69, 100); Pattern s; chordVariation(single, rng, kKey, s);
  CHECK_EQ(s.count, 1); CHECK_EQ(s.notes[0].pitch(), 69);          // one voice: only the base candidate
}

// ---------------------------------------------------------------- lead_variation (:222-260)
TEST(lead_variation_stays_in_key_and_anchored) {
  const Pattern src = leadFixture();
  std::set<std::string> seen; int maxCount = 0;
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Rng rng = R(seed); Pattern out; leadVariation(src, rng, kKey, out);
    CHECK(valid(out)); CHECK(inKey(kKey, out)); CHECK_EQ(out.lengthTicks, 384);
    CHECK(out.count >= 5 && out.count <= 7);
    CHECK_EQ(out.notes[0].pitch(), 60); CHECK_EQ(out.notes[out.count - 1].pitch(), 60);   // phrase boundaries anchored
    for (int i = 0; i < out.count; i++) CHECK(out.notes[i].start() % 12 == 0);
    maxCount = out.count > maxCount ? out.count : maxCount;
    seen.insert(sig(out));
    Rng rng2 = R(seed); Pattern again; leadVariation(src, rng2, kKey, again); CHECK(same(out, again));
  }
  CHECK(seen.size() >= 3);
  CHECK_EQ(maxCount, 7);                                           // style 3 splits the two notes >= 1/2 beat (not the last)
}
TEST(lead_variation_passing_tone_only_on_weak_beats) {
  Pattern weak = mk(2 * Q); add(weak, 0, 36, 60, 90); add(weak, 48, 36, 62, 80); add(weak, 96, 72, 65, 90);
  Pattern strong = mk(3 * Q); add(strong, 0, 36, 60, 90); add(strong, 96, 36, 62, 80); add(strong, 192, 72, 65, 90);
  bool sawPassing = false, sawPlain = false;
  for (uint32_t seed = 1; seed <= 30; seed++) {
    Rng rng = R(seed); Pattern out; leadVariation(weak, rng, kKey, out);
    const Note* n = find(out, 48, 64); if (n) sawPassing = true;
    if (find(out, 48, 62)) sawPlain = true;                        // style 3 leaves the pitch alone
    Rng rng2 = R(seed); Pattern s; leadVariation(strong, rng2, kKey, s);
    CHECK(find(s, 96, 62) != nullptr);                             // on the beat: never a passing tone
  }
  CHECK(sawPassing); CHECK(sawPlain);
}
TEST(lead_variation_split_and_gate_lengths) {
  const Pattern src = leadFixture();
  for (uint32_t seed = 1; seed <= 30; seed++) {
    Rng rng = R(seed); Pattern out; leadVariation(src, rng, kKey, out);
    if (out.count == 7) {                                          // style 3: 65@96 (72 ticks) → 96..132 and 132..168, repeat -9
      const Note* a = find(out, 96, 65); const Note* b = find(out, 132, 65);
      CHECK(a && b);
      if (a && b) { CHECK_EQ(a->len(), 36); CHECK_EQ(b->end(), 168); CHECK_EQ(b->vel(), qv(qv(90) - 9)); }
      CHECK(find(out, 288, 60) && find(out, 288, 60)->len() == 72);   // last note never split
    } else {
      const Note* a = find(out, 96, 65); CHECK(a != nullptr);
      if (a) CHECK(a->len() == 60 || a->len() == 72);              // 72 × 0.80 = 57.6 → 60, 72 × 0.94 = 67.7 → 72
    }
  }
}
TEST(lead_variation_split_keeps_later_onsets_at_capacity) {
  // short notes 6 ticks apart (duration clamps to 6: never split) then nine notes of 48 ticks from 1100
  // (eight of them splittable); the last long onset at 1484 must survive every style.
  auto fixture = [](int shortCount) {
    Pattern p = mk(16 * Q);
    for (int i = 0; i < shortCount; i++) add(p, i * 6, 12, 60, 100);
    for (int i = 0; i < 9; i++) add(p, 1100 + i * 48, 48, 60, 100);
    return p;
  };
  const Pattern full = fixture(183);                               // 192 notes: no room for any split
  const Pattern room = fixture(180);                               // 189 notes: room for three splits
  int seedsWithSplits = 0;
  for (uint32_t seed = 1; seed <= 24; seed++) {
    Pattern a; buildVariant(Lead, full, 1, seed, kKey, a);
    CHECK(valid(a)); CHECK_EQ(a.count, kMaxNotes);
    for (int i = 0; i < 9; i++) CHECK(find(a, 1100 + i * 48, 60) != nullptr);   // every long onset kept, unsplit
    for (int i = 0; i < 8; i++) CHECK(find(a, 1124 + i * 48, 60) == nullptr);
    Pattern b; buildVariant(Lead, room, 1, seed, kKey, b);
    CHECK(valid(b)); CHECK(b.count <= kMaxNotes);
    for (int i = 0; i < 9; i++) CHECK(find(b, 1100 + i * 48, 60) != nullptr);
    int splits = 0; for (int i = 0; i < 8; i++) if (find(b, 1124 + i * 48, 60)) splits++;   // repeats start at the half
    CHECK(splits == 0 || splits == 3);                             // style 3 splits until the later onsets need the slots
    CHECK_EQ(b.count, 189 + splits);
    if (splits) {
      seedsWithSplits++;
      CHECK(find(b, 1124, 60) && find(b, 1172, 60) && find(b, 1220, 60) && !find(b, 1268, 60));   // the first three
      CHECK_EQ(find(b, 1100, 60)->len(), 24); CHECK_EQ(find(b, 1124, 60)->vel(), qv(qv(100) - 9));
      CHECK_EQ(find(b, 1244, 60)->len(), 48);                      // unsplit long note: gated 0.94 → 45 → 48 packed
    }
  }
  CHECK(seedsWithSplits >= 1);
}
TEST(lead_variation_polyphonic_falls_back_to_chords) {
  const Pattern src = chordsFixture();
  for (uint32_t seed = 1; seed <= 10; seed++) {
    Rng a = R(seed), b = R(seed); Pattern lead, chord;
    leadVariation(src, a, kKey, lead); chordVariation(src, b, kKey, chord);
    CHECK(same(lead, chord));
  }
}

// ---------------------------------------------------------------- mutate (:128-162)
TEST(mutate_changes_at_least_one_onset_deterministically) {
  const Pattern src = leadFixture();
  std::set<std::string> seen;
  for (uint32_t seed = 1; seed <= 30; seed++) {
    Rng rng = R(seed); Pattern out; mutatePattern(src, 0.55f, rng, kKey, out);
    CHECK(valid(out)); CHECK(inKey(kKey, out)); CHECK_EQ(out.lengthTicks, 384);
    CHECK(out.count <= src.count && out.count >= src.count - 1);  // a +1/8 shift can push the last note out
    CHECK(!same(out, src));                                        // the forced group always moves
    for (int i = 0; i < out.count; i++) CHECK(out.notes[i].start() % 12 == 0);   // shifts are ±12 ticks
    seen.insert(sig(out));
    Rng rng2 = R(seed); Pattern again; mutatePattern(src, 0.55f, rng2, kKey, again); CHECK(same(out, again));
  }
  CHECK(seen.size() >= 5);
}
TEST(mutate_weak_strength_and_chords) {
  const Pattern src = chordsFixture();
  for (uint32_t seed = 1; seed <= 20; seed++) {
    Rng rng = R(seed); Pattern out; mutatePattern(src, 0.18f, rng, kKey, out);
    CHECK(valid(out)); CHECK(inKey(kKey, out)); CHECK_EQ(out.count, 6);
    // a group moves as a whole: both onsets keep three notes each
    std::set<int> starts; for (int i = 0; i < out.count; i++) starts.insert(out.notes[i].start());
    CHECK_EQ((int)starts.size(), 2);
    for (int s : starts) CHECK_EQ((int)pitchesAt(out, s).size(), 3);
  }
  Pattern empty = mk(Q); Rng r = R(1); Pattern e; mutatePattern(empty, 0.55f, r, kKey, e);
  CHECK_EQ(e.count, 0); CHECK_EQ(e.lengthTicks, 96);
}

// ---------------------------------------------------------------- limit_polyphony (:265-295)
TEST(limit_polyphony_outer_voices_and_highest) {
  const Pattern src = tonalChords();
  Pattern two; limitPolyphony(src, 2, two);
  CHECK(valid(two)); CHECK_EQ(two.count, 4);
  CHECK(pitchesAt(two, 0) == std::set<int>({60, 67})); CHECK(pitchesAt(two, 96) == std::set<int>({62, 69}));
  Pattern one; limitPolyphony(src, 1, one);
  CHECK(valid(one)); CHECK_EQ(one.count, 2);
  CHECK(pitchesAt(one, 0) == std::set<int>({67})); CHECK(pitchesAt(one, 96) == std::set<int>({69}));
  Pattern held = mk(4 * Q);                                        // sustained chord cut at the next onset
  add(held, 0, 384, 60, 90); add(held, 0, 384, 64, 85); add(held, 0, 384, 67, 80); add(held, 192, 192, 62, 88); add(held, 192, 96, 65, 84);
  limitPolyphony(held, 2, two);
  CHECK_EQ(two.count, 4);
  CHECK_EQ(find(two, 0, 60)->end(), 192); CHECK_EQ(find(two, 0, 67)->end(), 192);
  CHECK_EQ(find(two, 192, 62)->end(), 384); CHECK_EQ(find(two, 192, 65)->end(), 288);
  limitPolyphony(held, 1, one);
  CHECK_EQ(one.count, 2); CHECK(find(one, 0, 67) && find(one, 192, 65));
  Pattern mono = mk(Q); add(mono, 0, 48, 69, 100);                 // a single note survives "two voices"
  limitPolyphony(mono, 2, two); CHECK_EQ(two.count, 1);
}

// ---------------------------------------------------------------- generated_fx (:513-536)
TEST(generated_fx_grid_pool_and_ranges) {
  const Pattern src = leadFixture();
  for (uint32_t seed = 1; seed <= 30; seed++) {
    Rng rng = R(seed); Pattern out; generatedFx(src, 2 * Q, rng, kKey, out);
    CHECK(valid(out)); CHECK_EQ(out.lengthTicks, 192); CHECK(inKey(kKey, out));
    CHECK(out.count >= 1 && out.count <= 8);
    CHECK_EQ(out.notes[0].start(), 0);                             // step 0 always sounds
    for (int i = 0; i < out.count; i++) {
      const Note& n = out.notes[i];
      CHECK(n.start() % 24 == 0);
      CHECK(n.len() == 12 || n.len() == 24);
      CHECK(n.vel() >= qv(56) && n.vel() <= qv(115));
      CHECK(n.pitch() >= 55 && n.pitch() <= 70);                   // pool 60..65 moved by at most two scale steps
    }
    Rng rng2 = R(seed); Pattern again; generatedFx(src, 2 * Q, rng2, kKey, again); CHECK(same(out, again));
    Pattern half; Rng r3 = R(seed); generatedFx(src, Q / 2, r3, kKey, half);
    CHECK(valid(half)); CHECK_EQ(half.lengthTicks, 48); CHECK(half.count >= 1 && half.count <= 2);
    for (int i = 0; i < half.count; i++) CHECK(half.notes[i].end() <= 48);
  }
  Pattern empty = mk(4 * Q);
  std::set<int> seenPitch;
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Rng rng = R(seed); Pattern out; generatedFx(empty, 2 * Q, rng, kKey, out);
    CHECK(out.count >= 1);
    for (int i = 0; i < out.count; i++) { CHECK(out.notes[i].pitch() >= 65 && out.notes[i].pitch() <= 72); seenPitch.insert(out.notes[i].pitch()); }   // 60 + root(9) = 69 ± 2 steps
  }
  CHECK(seenPitch.size() >= 3);
}

// ---------------------------------------------------------------- beat_layers (:317-380)
TEST(beat_layers_complete_degenerate_sources) {
  std::vector<Pattern> inputs;
  inputs.push_back(mk(4 * Q));                                                          // empty
  { Pattern p = mk(4 * Q); add(p, 192, 12, 51, 100); inputs.push_back(p); }             // aux only
  { Pattern p = mk(4 * Q); add(p, 96, 12, 37, 100); inputs.push_back(p); }              // snare only
  { Pattern p = mk(4 * Q); add(p, 960, 12, 36, 100); inputs.push_back(p); }             // out of bounds
  const int lengths[] = {12, 48, 192, 384};
  for (size_t k = 0; k < inputs.size(); k++) {
    for (int L : lengths) {
      Pattern src = inputs[k]; src.lengthTicks = (uint16_t)L;
      Pattern layer[4];
      for (int v = 0; v < 4; v++) { Rng rng = R(10); beatLayer(src, v, rng, layer[v]); CHECK(valid(layer[v])); CHECK_EQ(layer[v].lengthTicks, L); CHECK(layer[v].count > 0); }
      std::set<int> p0 = pitches(layer[0]), p1 = pitches(layer[1]), p2 = pitches(layer[2]), p3 = pitches(layer[3]);
      CHECK(p0 == std::set<int>({36}));
      CHECK(p1.count(36) && (p1.count(38) || p1.count(39)) && !p1.count(37) && p1.size() <= 3);
      CHECK(p2.count(36) && p2.count(37) && (p2.count(38) || p2.count(39)) && p2.size() <= 4);
      int aux = 0; for (int p : p3) { CHECK(p >= 36 && p <= 58); if (p >= 40) aux++; }
      CHECK(aux == 1);                                             // exactly one accent (or the pitch-40 pickup)
      CHECK(layer[0].count <= layer[1].count && layer[1].count <= layer[2].count && layer[2].count < layer[3].count);
    }
  }
  // grid of the filled core: kicks every beat, hats on the off-beats from 0.5, snares on 2 and 4
  Pattern src = mk(4 * Q); Pattern full; Rng rng = R(1); beatLayer(src, 3, rng, full);
  CHECK_EQ(countPitch(full, 36), 4); CHECK_EQ(countPitch(full, 38), 4); CHECK_EQ(countPitch(full, 37), 2);
  CHECK(find(full, 0, 36) && find(full, 288, 36) && find(full, 48, 38) && find(full, 336, 38) && find(full, 96, 37) && find(full, 288, 37));
  CHECK_EQ(find(full, 0, 36)->vel(), qv(94)); CHECK_EQ(find(full, 48, 38)->vel(), qv(69)); CHECK_EQ(find(full, 96, 37)->vel(), qv(86));
  CHECK_EQ(find(full, 0, 36)->len(), 12);
  CHECK(find(full, 336, 40) != nullptr);                           // pickup: length − min(0.5 qn, length/4)
  CHECK_EQ(find(full, 336, 40)->vel(), qv(62));
}
TEST(beat_layers_preserve_breakbeat_and_pick_one_accent) {
  const Pattern src = busyBeat();
  std::set<int> accents;
  for (uint32_t seed = 1; seed <= 40; seed++) {
    Pattern layer[4];
    for (int v = 0; v < 4; v++) { Rng rng = R(seed); beatLayer(src, v, rng, layer[v]); CHECK(valid(layer[v])); }
    CHECK_EQ(layer[0].count, 2); CHECK_EQ(layer[1].count, 4); CHECK_EQ(layer[2].count, 6); CHECK_EQ(layer[3].count, 7);
    CHECK(pitchesAt(layer[2], 0) == std::set<int>({36}) && pitchesAt(layer[2], 48) == std::set<int>({38}));
    int accent = -1;
    for (int i = 0; i < layer[3].count; i++) if (layer[3].notes[i].pitch() >= 40) { accent = layer[3].notes[i].pitch(); CHECK_EQ(layer[3].notes[i].start(), 336); CHECK_EQ(layer[3].notes[i].vel(), qv(60 + (accent - 40) * 3)); }
    CHECK(accent >= 40 && accent <= 58);
    accents.insert(accent);
    for (int i = 0; i < layer[2].count; i++) CHECK(find(layer[3], layer[2].notes[i].start(), layer[2].notes[i].pitch()) != nullptr);   // Full ⊇ +Snare
    Rng again = R(seed); Pattern full2; beatLayer(src, 3, again, full2); CHECK(same(layer[3], full2));
  }
  CHECK(accents.size() >= 3);                                      // the seed chooses the accent
  Pattern core = coreBeat(); Rng rng = R(5); Pattern full; beatLayer(core, 3, rng, full);
  CHECK_EQ(full.count, 7); CHECK(find(full, 336, 40) && find(full, 336, 40)->vel() == qv(62));   // no aux: pickup
  Pattern wide = mk(4 * Q); add(wide, 0, 12, 35, 100); add(wide, 0, 12, 59, 100); add(wide, 0, 12, 36, 100);
  Rng r2 = R(5); beatLayer(wide, 3, r2, full);
  for (int i = 0; i < full.count; i++) CHECK(full.notes[i].pitch() >= 36 && full.notes[i].pitch() <= 58);   // outside 36..58 dropped
}
TEST(beat_core_wins_when_the_pattern_is_full) {
  Pattern src = mk(16 * Q);
  for (int i = 0; i < kMaxNotes; i++) add(src, i * 8, 12, 45, 100);
  Rng rng = R(1); Pattern kick; beatLayer(src, 0, rng, kick);
  CHECK(valid(kick)); CHECK_EQ(kick.count, 16);                     // 16 kicks fitted by evicting accents
  Pattern full; Rng r2 = R(1); beatLayer(src, 3, r2, full);
  CHECK(valid(full)); CHECK(full.count <= kMaxNotes);
  CHECK_EQ(countPitch(full, 36), 16); CHECK_EQ(countPitch(full, 38), 16); CHECK_EQ(countPitch(full, 37), 8);
}

TEST(beat_full_layer_after_accent_evictions) {
  Pattern src = mk(4 * Q);                                         // 180 open hats + six 45s + six 50s = 192 (full)
  for (int i = 0; i < 180; i++) add(src, i * 2, 12, 39, 100);
  for (int i = 0; i < 6; i++) add(src, 360 + i * 2, 12, 45, 100);
  for (int i = 0; i < 6; i++) add(src, 372 + i * 2, 12, 50, 100);
  for (uint32_t seed = 1; seed <= 20; seed++) {
    Rng a = R(seed), b = R(seed); Pattern full, snare; beatLayer(src, 3, a, full); beatLayer(src, 2, b, snare);
    CHECK(valid(full)); CHECK(valid(snare));
    CHECK_EQ(countPitch(full, 36), 4); CHECK_EQ(countPitch(full, 37), 2);   // 4 kicks + 2 snares evict the six latest accents
    CHECK_EQ(countPitch(full, 50), 0); CHECK_EQ(countPitch(full, 45), 6);   // the accent is chosen among the survivors only
    CHECK_EQ(full.count, kMaxNotes); CHECK_EQ(snare.count, kMaxNotes - 6);
    CHECK(!same(full, snare));
  }
  // every accent evicted and the core at capacity: nothing can be added, Full equals +Snare (documented limit)
  Pattern probe = mk(16 * Q);
  for (int i = 0; i < 190; i++) add(probe, i * 8, 12, 39, 100);
  add(probe, 1520, 12, 45, 100); add(probe, 1528, 12, 50, 100);
  Rng a = R(1), b = R(1); Pattern full, snare; beatLayer(probe, 3, a, full); beatLayer(probe, 2, b, snare);
  CHECK(valid(full)); CHECK_EQ(full.count, kMaxNotes); CHECK(same(full, snare));
  CHECK_EQ(countPitch(full, 45) + countPitch(full, 50), 0); CHECK_EQ(countPitch(full, 36), 2);
}

// ---------------------------------------------------------------- snare rush (SPEC §9.6)
TEST(snare_rush_drops_kick_and_fills_snares) {
  Rng rng = R(3); Pattern full; beatLayer(coreBeat(), 3, rng, full);    // 36@0 38@48 before beat 1, snare at 96
  Pattern rush; snareRush(full, rush);
  CHECK(valid(rush)); CHECK_EQ(rush.lengthTicks, 96);
  CHECK_EQ(countPitch(rush, 36), 0);
  CHECK(find(rush, 48, 38) != nullptr);
  CHECK_EQ(countPitch(rush, 37), 4);
  CHECK_EQ(find(rush, 0, 37)->vel(), qv(100)); CHECK_EQ(find(rush, 24, 37)->vel(), qv(80));
  CHECK_EQ(find(rush, 48, 37)->vel(), qv(90)); CHECK_EQ(find(rush, 72, 37)->vel(), qv(80));
  CHECK_EQ(rush.count, 5);
  Pattern withSnare = full; add(withSnare, 24, 12, 37, 77);         // a snare inside the beat: no fill
  snareRush(withSnare, rush);
  CHECK_EQ(countPitch(rush, 37), 1); CHECK_EQ(find(rush, 24, 37)->vel(), qv(77)); CHECK_EQ(countPitch(rush, 36), 0);
  Pattern late = mk(4 * Q); add(late, 192, 12, 36, 100); add(late, 192, 12, 39, 100);   // nothing in beat 1: first onset moves to 0
  snareRush(late, rush);
  CHECK(find(rush, 0, 39) != nullptr); CHECK_EQ(countPitch(rush, 36), 0); CHECK_EQ(countPitch(rush, 37), 4);
}

// ---------------------------------------------------------------- percussion_layers (:483-511)
TEST(percussion_layers_by_ordinal_with_velocity_scale) {
  const Pattern src = percEight();
  Pattern layer[4];
  for (int v = 0; v < 4; v++) { percussionLayer(src, v, layer[v]); CHECK(valid(layer[v])); CHECK_EQ(layer[v].lengthTicks, 384); }
  CHECK_EQ(layer[0].count, 2); CHECK_EQ(layer[1].count, 4); CHECK_EQ(layer[2].count, 6); CHECK_EQ(layer[3].count, 8);
  CHECK(find(layer[0], 0, 50) && find(layer[0], 192, 50));
  CHECK(find(layer[1], 0, 50) && find(layer[1], 96, 50) && find(layer[1], 192, 50) && find(layer[1], 288, 50));
  CHECK(!find(layer[2], 144, 50) && !find(layer[2], 336, 50) && find(layer[2], 48, 50));   // ordinals 4 and 8 rest
  CHECK_EQ(layer[0].notes[0].vel(), sv(qv(100), 0.55f));
  CHECK_EQ(layer[1].notes[0].vel(), sv(qv(100), 0.70f));
  CHECK_EQ(layer[2].notes[0].vel(), sv(qv(100), 0.85f));
  CHECK_EQ(layer[3].notes[0].vel(), qv(100));
  CHECK(layer[0].notes[0].vel() < layer[1].notes[0].vel() && layer[1].notes[0].vel() < layer[3].notes[0].vel());
  Pattern two = mk(2 * Q);                                         // ordinals are counted per pitch
  for (int i = 0; i < 4; i++) { add(two, i * 48, 12, 50, 100); add(two, 24 + i * 48, 12, 52, 100); }
  Pattern sparse; percussionLayer(two, 0, sparse);
  CHECK_EQ(sparse.count, 2); CHECK(find(sparse, 0, 50) && find(sparse, 24, 52));
  Pattern light; percussionLayer(two, 1, light);
  CHECK_EQ(light.count, 4); CHECK(find(light, 96, 50) && find(light, 120, 52));
}

// ---------------------------------------------------------------- percussion fold (SPEC §6, §9.7)
TEST(fold_map_ranks_by_frequency_then_pitch) {
  uint8_t map[128]; percussionFoldMap(percFive(), map);
  CHECK_EQ(map[50], 36); CHECK_EQ(map[52], 37); CHECK_EQ(map[58], 38); CHECK_EQ(map[54], 39);   // 52 before 58 on the tie
  CHECK_EQ(map[56], 36); CHECK_EQ(map[60], 36); CHECK_EQ(map[47], 39); CHECK_EQ(map[35], 39); CHECK_EQ(map[0], 36);
  Pattern p = percFive(); foldPercussionVoices(p);
  CHECK(valid(p)); CHECK_EQ(p.count, 19);
  CHECK_EQ(countPitch(p, 36), 9); CHECK_EQ(countPitch(p, 37), 4); CHECK_EQ(countPitch(p, 38), 4); CHECK_EQ(countPitch(p, 39), 2);
  CHECK(find(p, 12, 36) && find(p, 12, 36)->vel() == qv(120));
  Pattern again = p; foldPercussionVoices(again);
  CHECK(same(p, again));                                           // folded counts 9/4/4/2 are already in rank order
  Pattern dup = percFive(); dup.notes[dup.count - 1].setStart(0);  // 56 (voice 36) now coincides with 50@0
  foldPercussionVoices(dup);
  CHECK_EQ(dup.count, 18); CHECK_EQ(find(dup, 0, 36)->vel(), qv(120));                                  // merged, loudest wins
  Pattern folded = mk(Q); add(folded, 0, 12, 37, 100); add(folded, 24, 12, 37, 100); add(folded, 48, 12, 36, 100);
  percussionFoldMap(folded, map);
  CHECK_EQ(map[37], 36); CHECK_EQ(map[36], 37);                    // SPEC §9.7 ranks by count even inside 36..39:
  CHECK_EQ(map[38], 38); CHECK_EQ(map[39], 39);                    // voice stability comes from the stored map, not from
}                                                                  // an identity shortcut (next test)
TEST(build_percussion_stored_fold_map_keeps_voices_after_edits) {
  Pattern src = mk(4 * Q);                                         // 50 ×8, 52 ×4 as loaded
  for (int i = 0; i < 8; i++) add(src, i * 48, 24, 50, 100);
  for (int i = 0; i < 4; i++) add(src, 24 + i * 96, 24, 52, 90);
  uint8_t map[128]; percussionFoldMap(src, map);                   // computed at load and stored (SPEC §9.7)
  CHECK_EQ(map[50], 36); CHECK_EQ(map[52], 37); CHECK_EQ(map[37], 37);
  Pattern edited = src;                                            // the editor adds nine row-B hits as pitch 37 (SPEC §9.14)
  for (int i = 0; i < 9; i++) add(edited, 6 + i * 36, 12, 37, 80);
  uint8_t fresh[128]; percussionFoldMap(edited, fresh);
  CHECK_EQ(fresh[37], 36); CHECK_EQ(fresh[50], 37); CHECK_EQ(fresh[52], 38);   // a re-rank would permute every voice
  Pattern full; CHECK(std::string(buildVariant(Percussion, edited, 3, 9, kKey, full, map)) == "Full");
  CHECK(valid(full)); CHECK_EQ(full.count, 21);
  CHECK_EQ(countPitch(full, 36), 8); CHECK_EQ(countPitch(full, 37), 13); CHECK_EQ(countPitch(full, 38), 0);
  for (int i = 0; i < 8; i++) CHECK(find(full, i * 48, 36) != nullptr);          // the 50s stay on voice A
  for (int i = 0; i < 9; i++) CHECK(find(full, 6 + i * 36, 37) != nullptr);      // the edited hits sit on row B
  Pattern sparse; buildVariant(Percussion, edited, 0, 9, kKey, sparse, map);     // ordinals per original pitch
  CHECK(valid(sparse)); CHECK_EQ(sparse.count, 6);
  CHECK(find(sparse, 0, 36) && find(sparse, 192, 36));                           // 50: hits 1 and 5
  CHECK(find(sparse, 24, 37) != nullptr);                                        // 52: hit 1
  CHECK(find(sparse, 6, 37) && find(sparse, 150, 37) && find(sparse, 294, 37));  // edited 37: hits 1, 5 and 9
  Pattern computed; buildVariant(Percussion, edited, 3, 9, kKey, computed);      // no stored map: re-ranked from the edit
  CHECK_EQ(countPitch(computed, 36), 9); CHECK_EQ(countPitch(computed, 37), 8); CHECK_EQ(countPitch(computed, 38), 4);
  Pattern a, b; buildVariant(Percussion, src, 1, 9, kKey, a); buildVariant(Percussion, src, 1, 9, kKey, b, nullptr);
  CHECK(same(a, b));                                               // nullptr == the map of the source
  Pattern alias = edited; buildVariant(Percussion, alias, 3, 9, kKey, alias, map);
  CHECK(same(alias, full));                                        // src and out may alias with a stored map too
}
TEST(build_percussion_counts_ordinals_per_original_pitch) {
  const Pattern src = percFive();
  Pattern sparse; CHECK(std::string(buildVariant(Percussion, src, 0, 9, kKey, sparse)) == "Sparse");
  CHECK(valid(sparse)); CHECK_EQ(sparse.count, 6);
  CHECK(find(sparse, 0, 36) && find(sparse, 192, 36));             // 50: hits 1 and 5
  CHECK(find(sparse, 12, 36) != nullptr);                          // 56: its own first hit, even though it shares voice 36
  CHECK(find(sparse, 24, 37) && find(sparse, 72, 38) && find(sparse, 36, 39));
  CHECK_EQ(find(sparse, 0, 36)->vel(), sv(qv(100), 0.55f));
  Pattern full; buildVariant(Percussion, src, 3, 9, kKey, full);
  CHECK(valid(full)); CHECK_EQ(full.count, 19); CHECK_EQ(countPitch(full, 36), 9);
  for (int i = 0; i < full.count; i++) CHECK(full.notes[i].pitch() >= 36 && full.notes[i].pitch() <= 39);
}

// ---------------------------------------------------------------- buildVariant dispatch
TEST(build_variant_names_lengths_and_determinism) {
  struct Case { Category c; Pattern src; const char* names[4]; int lengths[4]; };
  const Case cases[] = {
    {Chord, chordsFixture(), {"Original", "Variation", "2 beats", "Half beat"}, {384, 384, 192, 48}},
    {Lead, leadFixture(), {"Original", "Variation", "2 beats", "Half beat"}, {384, 384, 192, 48}},
    {Bass, leadFixture(), {"Original", "Variation", "2 beats", "Half beat"}, {384, 384, 192, 48}},
    {Pad, tonalChords(), {"Original", "Variation", "Two voices", "Mono"}, {384, 384, 384, 384}},
    {Fx, leadFixture(), {"Original", "Variation", "2 beats", "Half beat"}, {384, 384, 192, 48}},
    {Percussion, percFive(), {"Sparse", "Light", "Driving", "Full"}, {384, 384, 384, 384}},
    {Vocal, leadFixture(), {"Original", "Alt file", "2 beats", "1 beat"}, {384, 384, 192, 96}},   // SPEC §9.4 natural lengths
    {Beat, busyBeat(), {"Kick", "Kick+Hats", "+Snare", "Full"}, {384, 384, 384, 384}},
  };
  for (const Case& k : cases) {
    Pattern normalized; cropPattern(k.src, k.src.lengthTicks, normalized);   // crop at the source length == normalize
    for (int v = 0; v < 4; v++) {
      Pattern out, again;
      const char* name = buildVariant(k.c, k.src, v, 77, kKey, out);
      CHECK(std::string(name) == k.names[v]);
      CHECK(valid(out)); CHECK_EQ(out.lengthTicks, k.lengths[v]); CHECK(out.count <= kMaxNotes);
      buildVariant(k.c, k.src, v, 77, kKey, again);
      CHECK(same(out, again));
      if (v == 0 && k.c != Beat && k.c != Percussion) CHECK(same(out, normalized));   // Original is the normalized source
      if (k.c == Vocal) { if (v < 2) CHECK(same(out, normalized)); else CHECK_EQ(out.count, 0); }   // 2 / 1 beats, no notes
      if (k.c == Vocal || isSynthCategory(k.c)) CHECK(inKey(kKey, out) || v == 0);
    }
  }
  Pattern out;
  CHECK(std::string(buildVariant(Chord, chordsFixture(), 9, 1, kKey, out)) == "Half beat");   // index clamped
  CHECK(std::string(buildVariant(Chord, chordsFixture(), -2, 1, kKey, out)) == "Original");
  CHECK(std::string(buildVariant(CategoryCount, leadFixture(), 1, 1, kKey, out)) == "Variation");   // unknown: lead rules
  CHECK(valid(out));
}
TEST(build_variant_original_is_normalized) {
  Pattern src = mk(4 * Q);                                         // appended unsorted with an exact duplicate (an edited source)
  add(src, 192, 48, 64, 80); add(src, 0, 96, 60, 100); add(src, 0, 48, 60, 50); add(src, 96, 480, 62, 90);
  Pattern expect; cropPattern(src, 4 * Q, expect);                 // crop at the source length == normalize
  for (Category c : {Chord, Lead, Bass, Pad, Fx}) {
    Pattern out; CHECK(std::string(buildVariant(c, src, 0, 1, kKey, out)) == "Original");
    CHECK(valid(out)); CHECK_EQ(out.count, 3); CHECK_EQ(out.lengthTicks, 384);
    CHECK_EQ(out.notes[0].start(), 0); CHECK_EQ(out.notes[0].len(), 96); CHECK_EQ(out.notes[0].vel(), qv(100));   // merged
    CHECK_EQ(out.notes[1].start(), 96); CHECK_EQ(out.notes[1].end(), 384);                                       // clipped
    CHECK_EQ(out.notes[2].start(), 192);
    CHECK(same(out, expect));
    Pattern alias = src; buildVariant(c, alias, 0, 1, kKey, alias); CHECK(same(alias, expect));
  }
  Pattern vocal; buildVariant(Vocal, src, 0, 1, kKey, vocal); CHECK(same(vocal, expect));
  buildVariant(Vocal, src, 1, 1, kKey, vocal); CHECK(same(vocal, expect));
  buildVariant(Vocal, src, 2, 1, kKey, vocal); CHECK_EQ(vocal.count, 0); CHECK_EQ(vocal.lengthTicks, 192);
  buildVariant(Vocal, src, 3, 1, kKey, vocal); CHECK_EQ(vocal.count, 0); CHECK_EQ(vocal.lengthTicks, 96);
  Pattern alias = src; buildVariant(Vocal, alias, 3, 1, kKey, alias); CHECK_EQ(alias.count, 0); CHECK_EQ(alias.lengthTicks, 96);
  // a monophonic lead with one exact duplicate note gets the lead treatment, not the chord fallback (:224-226)
  const Pattern lead = leadFixture(); Pattern dup = lead; add(dup, 96, 72, 65, 60);
  for (uint32_t seed = 1; seed <= 20; seed++) {
    Rng a = R(seed), b = R(seed); Pattern x, y; leadVariation(dup, a, kKey, x); leadVariation(lead, b, kKey, y);
    CHECK(same(x, y));
  }
}
TEST(build_variant_depends_on_seed_and_matches_transforms) {
  struct Probe { Category c; Pattern src; int v; };
  const Probe probes[] = {{Chord, chordsFixture(), 1}, {Lead, leadFixture(), 1}, {Pad, tonalChords(), 1}, {Fx, leadFixture(), 1}, {Fx, leadFixture(), 2}, {Fx, leadFixture(), 3}, {Beat, busyBeat(), 3}};
  for (const Probe& p : probes) {
    std::set<std::string> seen;
    for (uint32_t seed = 1; seed <= 16; seed++) { Pattern out; buildVariant(p.c, p.src, p.v, seed, kKey, out); seen.insert(sig(out)); }
    CHECK(seen.size() >= 2);
  }
  Pattern a, b;                                                    // crops and polyphony limits are seed-independent
  buildVariant(Chord, chordsFixture(), 2, 1, kKey, a); buildVariant(Chord, chordsFixture(), 2, 999, kKey, b); CHECK(same(a, b));
  Pattern crop; cropPattern(chordsFixture(), 192, crop); CHECK(same(a, crop));
  buildVariant(Pad, tonalChords(), 3, 1, kKey, a); limitPolyphony(tonalChords(), 1, b); CHECK(same(a, b));
  buildVariant(Bass, leadFixture(), 3, 1, kKey, a); cropPattern(leadFixture(), 48, b); CHECK(same(a, b));
  buildVariant(Chord, chordsFixture(), 1, 5, kKey, a);              // Chord uses chord_variation ...
  for (int i = 0; i < a.count; i++) CHECK(a.notes[i].len() == a.notes[0].len());
  buildVariant(Lead, chordsFixture(), 1, 5, kKey, b);               // ... and Lead on polyphony falls back to it
  CHECK_EQ(b.count, 6);
  // the Beat accent of the Full slot is reproducible from seed alone
  buildVariant(Beat, busyBeat(), 3, 42, kKey, a); buildVariant(Beat, busyBeat(), 3, 42, kKey, b); CHECK(same(a, b));
  Pattern rush; snareRush(a, rush); CHECK(valid(rush)); CHECK_EQ(rush.lengthTicks, 96); CHECK_EQ(countPitch(rush, 36), 0);
}
// ---------------------------------------------------------------------------------------------- acid (SPEC §9.17b)

// Slides end past the next onset (note-off after the next note-on); accents are velocity >= 100; rests are the empty
// 1/16 steps. Aggregated over many seeds so the ≈ 30 % / 20 % / 10-25 % targets can be checked as ranges.
TEST(acid_line_is_in_key_seeded_and_shaped_like_a_303) {
  ProjectKey key; key.root = 9;
  int bars1 = 0, bars2 = 0, notes = 0, slides = 0, accents = 0, steps = 0, rests = 0, octaveJumps = 0, roots = 0;
  for (uint32_t seed = 1; seed <= 64; seed++) {
    Pattern a, b; acidLine(seed, key, a, 1); acidLine(seed, key, b, 1);
    CHECK(same(a, b));                                             // deterministic
    CHECK(valid(a)); CHECK(inKey(key, a));
    CHECK(a.lengthTicks == 16 * 24 || a.lengthTicks == 32 * 24);
    if (a.lengthTicks == 16 * 24) bars1++; else bars2++;
    CHECK(a.count >= 6 && a.count <= a.lengthTicks / 24);
    for (int i = 0; i < a.count; i++) {
      const Note& n = a.notes[i];
      CHECK(n.start() % 24 == 0);                                  // on the 1/16 grid
      CHECK(n.pitch() >= 12 && n.pitch() <= 115);
      if (n.vel() >= 100) accents++;
      if (i + 1 < a.count && n.end() > a.notes[i + 1].start()) { slides++; CHECK_EQ((int)n.end(), (int)a.notes[i + 1].start() + 12); }
      else CHECK(n.len() == 12 || n.len() == 24 || n.end() == a.lengthTicks);
      if ((n.pitch() - key.root) % 12 == 0) roots++;
      if (n.pitch() >= key.degreePitch(0, 2)) octaveJumps++;
    }
    notes += a.count; steps += a.lengthTicks / 24; rests += a.lengthTicks / 24 - a.count;
    CHECK_EQ((int)a.notes[0].start(), 0);                          // the downbeat always sounds
    Pattern lead; acidLine(seed, key, lead, 2);
    CHECK(inKey(key, lead)); CHECK(lead.notes[0].pitch() >= 33);   // Lead an octave above Bass
    if (seed > 1) { Pattern prev; acidLine(seed - 1, key, prev, 1); CHECK(!same(a, prev)); }   // the seed matters
  }
  const double slideShare = (double)slides / notes, accentShare = (double)accents / notes, restShare = (double)rests / steps;
  std::printf("  64 lines: %d one-bar, %d two-bar; slides %.0f %%, accents %.0f %%, rests %.0f %%, roots %.0f %%\n", bars1, bars2, slideShare * 100, accentShare * 100, restShare * 100, 100.0 * roots / notes);
  CHECK(bars1 > 10 && bars2 > 10);
  CHECK(slideShare > 0.18 && slideShare < 0.42);
  CHECK(accentShare > 0.12 && accentShare < 0.30);
  CHECK(restShare > 0.08 && restShare < 0.28);
  CHECK((double)roots / notes > 0.40);                             // root-heavy
  CHECK(octaveJumps > 0);
  ProjectKey other; other.root = 2;                                // another key: still in that key
  Pattern d; acidLine(7, other, d, 1); CHECK(inKey(other, d)); CHECK((d.notes[0].pitch() - other.root) % 12 == 0);   // the downbeat is the new root
}

TEST(acid_stabs_are_diatonic_offbeat_chords) {
  ProjectKey key; key.root = 9;
  int two = 0, three = 0, len24 = 0, len36 = 0, bars1 = 0, bars2 = 0, accents = 0, onsets = 0;
  for (uint32_t seed = 1; seed <= 48; seed++) {
    Pattern a, b; acidStabs(seed, key, a, 3); acidStabs(seed, key, b, 3);
    CHECK(same(a, b)); CHECK(valid(a)); CHECK(inKey(key, a));
    CHECK(a.lengthTicks == kTicksPerBar || a.lengthTicks == 2 * kTicksPerBar);
    if (a.lengthTicks == kTicksPerBar) bars1++; else bars2++;
    CHECK(a.count >= 2);
    for (int i = 0; i < a.count;) {
      const int start = a.notes[i].start(); int j = i;
      while (j < a.count && a.notes[j].start() == start) j++;
      const int voices = j - i;
      CHECK(voices == 2 || voices == 3); if (voices == 2) two++; else three++;
      CHECK(start % kTicksPerBeat != 0); CHECK(start % 24 == 0);  // offbeat 8ths / 16ths only
      for (int k = i; k < j; k++) { CHECK(a.notes[k].len() == 24 || a.notes[k].len() == 36); if (a.notes[k].len() == 24) len24++; else len36++; CHECK(a.notes[k].pitch() >= 12 && a.notes[k].pitch() <= 115); }
      if (a.notes[i].vel() >= 100) accents++;
      onsets++;
      i = j;
    }
    if (seed > 1) { Pattern prev; acidStabs(seed - 1, key, prev, 3); CHECK(!same(a, prev)); }
  }
  std::printf("  48 stab patterns: %d one-bar, %d two-bar; %d two-voice, %d three-voice onsets; lengths 24: %d, 36: %d; accents %.0f %%\n", bars1, bars2, two, three, len24, len36, 100.0 * accents / onsets);
  CHECK(two > 0 && three > 0); CHECK(len24 > 0 && len36 > 0); CHECK(bars1 > 5 && bars2 > 5);
  CHECK(accents > onsets / 10 && accents < onsets / 3);
}

TEST(build_variant_capacity_and_aliasing) {
  Pattern big = mk(16 * Q);
  for (int i = 0; i < kMaxNotes; i++) add(big, i * 7, 48, 60 + (i % 7), 100);   // 192 single notes 7 ticks apart (never split)
  for (Category c : {Chord, Lead, Pad, Bass, Fx, Percussion, Vocal, Beat}) {
    for (int v = 0; v < 4; v++) {
      Pattern out; buildVariant(c, big, v, 3, kKey, out); CHECK(valid(out));
      Pattern alias = big; buildVariant(c, alias, v, 3, kKey, alias);   // src and out may be the same object
      CHECK(same(out, alias));
    }
  }
  Pattern gen; Rng rng = R(2); generatedFx(big, 16 * Q, rng, kKey, gen);
  CHECK(valid(gen)); CHECK(gen.count <= 64);
}

int main() { return lp_test::run(); }
