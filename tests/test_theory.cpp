// Host tests for the THEORY SET GENERATOR of gen/variants.cpp (SPEC §9.24): the flash tables of gen/theory_tables.hpp
// (generated from tools/theory_data.py — the clip generator's own tables), the harmony helpers and the five melodic
// builders (Pad voicings, Chord stabs, Bass, Lead A A' B A', Fx) over 1000 seeds in every key: in key (the natural
// minor plus the borrowed pitch classes a chord code allows, only inside that chord's span), the register bands, the
// note cap, an onset at tick 0, non-empty variants through buildVariant, determinism per seed, the duplicate rate and
// the progression coverage. Pure: no card, no engine. The generator's JobTheory wiring is tested in test_generator.cpp.
#include "check.h"
#include "gen/variants.h"
#include "gen/theory_tables.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace lp;
using namespace lp::theory;

namespace {
constexpr int kSeeds = 1000;
constexpr Category kMelodic[5] = {Chord, Lead, Pad, Bass, Fx};

// Sorted by (start, pitch), starts inside the loop, lengths in whole units, no exact duplicates, count within the buffer.
bool valid(const Pattern& p) {
  if (p.count > kMaxNotes || p.count == 0) return false;
  if (p.lengthTicks % kTicksPerBar || p.lengthTicks < kTicksPerBar || p.lengthTicks > kMaxPatternTicks) return false;
  for (int i = 0; i < p.count; i++) {
    const Note& n = p.notes[i];
    if (n.start() >= p.lengthTicks) return false;
    if (n.len() < kNoteLenUnit || n.len() % kNoteLenUnit) return false;
    if (n.end() > p.lengthTicks && n.len() != kNoteLenUnit) return false;
    if (i > 0) {
      const Note& m = p.notes[i - 1];
      if (m.start() > n.start()) return false;
      if (m.start() == n.start() && m.pitch() >= n.pitch()) return false;
    }
  }
  return true;
}
std::string sig(const Pattern& p) {
  std::string s = std::to_string(p.lengthTicks) + "|";
  for (int i = 0; i < p.count; i++) s += std::to_string(p.notes[i].packed) + ",";
  return s;
}
// Every note's pitch class is allowed by the chord sounding at its start (the key, plus that chord's borrowed classes).
int outOfKey(const TheoryPlan& plan, const Pattern& p) {
  int bad = 0;
  for (int i = 0; i < p.count; i++) {
    const uint16_t allowed = theoryAllowedPcs(plan.root, theoryChordCode(plan, theoryChordAt(plan, p.notes[i].start())));
    if (!((allowed >> (p.notes[i].pitch() % 12)) & 1)) bad++;
  }
  return bad;
}
int outOfBand(const Pattern& p, int lo, int hi) { int bad = 0; for (int i = 0; i < p.count; i++) if (p.notes[i].pitch() < lo || p.notes[i].pitch() > hi) bad++; return bad; }
bool onsetAtZero(const Pattern& p) { for (int i = 0; i < p.count; i++) if (p.notes[i].start() == 0) return true; return false; }
void band(Category c, int& lo, int& hi) {
  switch (c) {
    case Pad: lo = kTheoryPadLo; hi = kTheoryPadHi; break;
    case Bass: lo = kTheoryBassLo; hi = kTheoryBassHi; break;
    case Lead: lo = kTheoryLeadLo; hi = kTheoryLeadHi; break;
    case Fx: lo = kTheoryFxLo; hi = kTheoryFxHi; break;
    default: lo = kTheoryChordLo; hi = kTheoryChordHi; break;
  }
}
int progIndex(const char* name) {                               // the table index of a progression by its composed name
  for (int i = 0; i < kProgCount; i++) { TheoryPlan p; p.prog = (uint8_t)i; char buf[64]; theoryProgName(p, buf, sizeof buf); if (strcmp(buf, name) == 0) return i; }
  return -1;
}
std::set<int> pcsOf(const uint8_t* pcs, int n) { std::set<int> s; for (int i = 0; i < n; i++) s.insert(pcs[i]); return s; }
} // namespace

// ---------------------------------------------------------------------------------------------- tables

TEST(tables_meet_the_spec_counts_and_encodings) {
  CHECK(kProgCount >= 100);
  CHECK(kChordRhythmCount + kBassTplCount + kLeadRhythmCount >= 100);
  CHECK_EQ(sizeof(TheoryPlan), (size_t)52);                    // the generator's whole theory RAM (SPEC §9.24: <= 200 B)
  std::set<std::string> seen;
  int totalW = 0;
  for (int i = 0; i < kProgCount; i++) {
    const Prog& pr = kProgs[i];
    CHECK(pr.n == 2 || pr.n == 4 || pr.n == 8);
    CHECK(pr.beatsPerChord == 2 || pr.beatsPerChord == 4 || pr.beatsPerChord == 8);
    CHECK(pr.n * pr.beatsPerChord <= kMaxPatternBeats);          // a pattern is at most 16 beats
    CHECK(pr.n * pr.beatsPerChord * kTicksPerBeat >= kTicksPerBar);
    CHECK(kProgW[i] >= 1); totalW += kProgW[i];
    for (int c = 0; c < kMaxChords; c++) {
      if (c >= pr.n) { CHECK_EQ((int)pr.chords[c], 0); continue; }
      CHECK((pr.chords[c] & 7) <= 6); CHECK(((pr.chords[c] >> 3) & 7) <= AltRaisedRoot);
    }
    TheoryPlan p; p.prog = (uint8_t)i; char buf[64]; const int n = theoryProgName(p, buf, sizeof buf);
    CHECK(n > 0 && n < 48); CHECK_EQ((int)strlen(buf), n);
    const std::string key = std::string(buf) + "/" + std::to_string(pr.beatsPerChord);
    CHECK(seen.insert(key).second);                             // no duplicate (progression, harmonic rhythm)
  }
  std::printf("  %d progressions (total weight %d), %d voicings, %d chord + %d bass + %d lead rhythm templates\n", kProgCount, totalW, kVoicingCount, kChordRhythmCount, kBassTplCount, kLeadRhythmCount);
  for (int i = 0; i < kVoicingCount; i++) { CHECK(kVoicings[i].n >= 2 && kVoicings[i].n <= 4); CHECK(kVoicingW[i] >= 1); for (int j = 0; j < kVoicings[i].n; j++) CHECK(kVoicings[i].stack[j] <= 8); }
  for (int i = 0; i < kChordRhythmCount; i++) { const int c0 = kChordRhythms[i] & 3; CHECK(c0 == StepOn || c0 == StepAccent); CHECK(kChordRhythmW[i] >= 1); }   // an onset at step 0: the crop variants are never empty
  for (int i = 0; i < kLeadRhythmCount; i++) { const int c0 = kLeadRhythms[i] & 3; CHECK(c0 == StepOn || c0 == StepAccent); CHECK(kLeadRhythmW[i] >= 1); }
  for (int i = 0; i < kBassTplCount; i++) { const int c0 = (int)(kBassTpls[i] & 15); CHECK(c0 != BassRest && c0 != BassHold && c0 != BassApproach); CHECK(kBassTplW[i] >= 1); }
  for (int r = 0; r < 7; r++) { int s = 0; for (int c = 0; c < 7; c++) s += kLeadMarkov[r][c]; CHECK(s > 0); }
  // the named staples are in the table, as the clip generator's PROGRESSIONS have them
  CHECK(progIndex("i-VI-III-VII") >= 0); CHECK(progIndex("i-VII-VI-V") >= 0); CHECK(progIndex("i-iv-v-i") >= 0);
  CHECK(progIndex("i-iidim-V-i") >= 0); CHECK(progIndex("i-IV-i-IV") >= 0); CHECK(progIndex("i-VI-viidim-i") >= 0); CHECK(progIndex("i-isus4-i-VII") >= 0);
}

// ---------------------------------------------------------------------------------------------- harmony helpers

TEST(chord_pitch_classes_and_borrowed_notes_in_a_minor) {
  const int A = 9; uint8_t pcs[4]; const uint8_t triad[3] = {0, 2, 4}, seventh[4] = {0, 2, 4, 6};
  // i = A C E; the natural minor allows nothing else
  CHECK_EQ(theoryChordPcs(A, 0 | (AltNone << 3), triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({9, 0, 4}));
  CHECK_EQ((int)theoryAllowedPcs(A, 0), (1 << 9) | (1 << 11) | (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7));
  // V = E G# B (the leading tone is the one borrowed class); V7 adds D
  CHECK_EQ(theoryChordPcs(A, 4 | (AltMajor << 3), triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({4, 8, 11}));
  CHECK_EQ(theoryChordPcs(A, 4 | (AltMajor << 3), seventh, 4, pcs), 4); CHECK(pcsOf(pcs, 4) == std::set<int>({4, 8, 11, 2}));
  CHECK_EQ((int)theoryAllowedPcs(A, 4 | (AltMajor << 3)), (int)theoryAllowedPcs(A, 0) | (1 << 8));
  // IV (dorian) = D F# A: the raised sixth
  CHECK_EQ(theoryChordPcs(A, 3 | (AltMajor << 3), triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({2, 6, 9}));
  CHECK_EQ((int)theoryAllowedPcs(A, 3 | (AltMajor << 3)), (int)theoryAllowedPcs(A, 0) | (1 << 6));
  // vii° = G# B D (the root is raised), vii°7 adds F
  CHECK_EQ(theoryChordPcs(A, 6 | (AltRaisedRoot << 3), seventh, 4, pcs), 4); CHECK(pcsOf(pcs, 4) == std::set<int>({8, 11, 2, 5}));
  CHECK_EQ((int)theoryAllowedPcs(A, 6 | (AltRaisedRoot << 3)), (int)theoryAllowedPcs(A, 0) | (1 << 8));
  // sus chords stay diatonic: isus4 = A D E, isus2 = A B E, ivsus4 = D G A
  CHECK_EQ(theoryChordPcs(A, 0 | (AltSus4 << 3), triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({9, 2, 4}));
  CHECK_EQ(theoryChordPcs(A, 0 | (AltSus2 << 3), triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({9, 11, 4}));
  CHECK_EQ(theoryChordPcs(A, 3 | (AltSus4 << 3), triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({2, 7, 9}));
  CHECK_EQ((int)theoryAllowedPcs(A, 0 | (AltSus4 << 3)), (int)theoryAllowedPcs(A, 0));
  // an octave offset (7) in a stack is dropped as a duplicate class; ii° = B D F
  const uint8_t octTriad[4] = {0, 2, 4, 7};
  CHECK_EQ(theoryChordPcs(A, 0, octTriad, 4, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({9, 0, 4}));
  CHECK_EQ(theoryChordPcs(A, 1, triad, 3, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({11, 2, 5}));
  // the octave offset carries the root's alteration: vii° over the oct-triad / fifth-oct / 7th-no5 templates is G# B D (G#),
  // never G# B D G (the unraised natural 7th would be a major seventh over the raised root — a wrong chord, out of the allowed set)
  CHECK_EQ(theoryChordPcs(A, 6 | (AltRaisedRoot << 3), octTriad, 4, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({8, 11, 2}));
  const uint8_t fifthOct[3] = {0, 4, 7}, seventhNo5[4] = {0, 2, 6, 7};
  CHECK_EQ(theoryChordPcs(A, 6 | (AltRaisedRoot << 3), fifthOct, 3, pcs), 2); CHECK(pcsOf(pcs, 2) == std::set<int>({8, 2}));
  CHECK_EQ(theoryChordPcs(A, 6 | (AltRaisedRoot << 3), seventhNo5, 4, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({8, 11, 5}));
  // ... and the borrowed major's third (V over the octave template keeps G#, the 9th-no1 template's 8 is the ninth, not a third)
  CHECK_EQ(theoryChordPcs(A, 4 | (AltMajor << 3), octTriad, 4, pcs), 3); CHECK(pcsOf(pcs, 3) == std::set<int>({4, 8, 11}));
  const uint8_t ninthNo1[4] = {2, 4, 6, 8};
  CHECK_EQ(theoryChordPcs(A, 4 | (AltMajor << 3), ninthNo1, 4, pcs), 4); CHECK(pcsOf(pcs, 4) == std::set<int>({8, 11, 2, 5}));   // G# B D F: the ninth stays the natural-minor F
  // every voicing template of every chord code stays inside the chord's allowed classes (the in-key invariant the builders rely on)
  for (int code = 0; code < 64; code++) {
    const int deg = code & 7, alt = (code >> 3) & 7;
    if (deg > 6 || alt > AltRaisedRoot) continue;
    for (int v = 0; v < kVoicingCount; v++) {
      const int n = theoryChordPcs(A, (uint8_t)code, kVoicings[v].stack, kVoicings[v].n, pcs);
      for (int j = 0; j < n; j++) CHECK((theoryAllowedPcs(A, (uint8_t)code) >> pcs[j]) & 1);
    }
  }
  // the same in every key: the allowed set is the minor scale rotated to the root
  for (int root = 0; root < 12; root++) { int n = 0; for (int pc = 0; pc < 12; pc++) if ((theoryAllowedPcs(root, 0) >> pc) & 1) n++; CHECK_EQ(n, 7); CHECK((theoryAllowedPcs(root, 0) >> root) & 1); }
  // chordAt follows the harmonic rhythm
  TheoryPlan p; p.nChords = 4; p.chordSteps = 16;
  CHECK_EQ(theoryChordAt(p, 0), 0); CHECK_EQ(theoryChordAt(p, 383), 0); CHECK_EQ(theoryChordAt(p, 384), 1); CHECK_EQ(theoryChordAt(p, 1535), 3); CHECK_EQ((int)theoryLengthTicks(p), 1536);
  p.nChords = 8; p.chordSteps = 8; CHECK_EQ(theoryChordAt(p, 192), 1); CHECK_EQ(theoryChordAt(p, 1535), 7);
  p.nChords = 2; p.chordSteps = 32; CHECK_EQ(theoryChordAt(p, 767), 0); CHECK_EQ(theoryChordAt(p, 768), 1);
}

TEST(progression_names_use_ascii_roman_numerals) {
  const int i1 = progIndex("i-VI-III-VII"); CHECK(i1 >= 0);
  const int i2 = progIndex("i-VII-VI-V"); CHECK(i2 >= 0);
  const int i3 = progIndex("i-VI-viidim-i"); CHECK(i3 >= 0);
  const int i4 = progIndex("i-iidim-V-i"); CHECK(i4 >= 0);
  const int i5 = progIndex("i-isus2-VI-VII"); CHECK(i5 >= 0);
  const int i6 = progIndex("i-VII-ivsus4-iv"); CHECK(i6 >= 0);
  const int i7 = progIndex("i-iv-VII-III-VI-iidim-V-i"); CHECK(i7 >= 0);
  if (i2 >= 0) { CHECK_EQ((int)kProgs[i2].chords[3], 4 | (AltMajor << 3)); }
  if (i3 >= 0) { CHECK_EQ((int)kProgs[i3].chords[2], 6 | (AltRaisedRoot << 3)); }
  if (i7 >= 0) { CHECK_EQ((int)kProgs[i7].n, 8); CHECK_EQ((int)kProgs[i7].beatsPerChord, 2); }
  TheoryPlan p; p.prog = (uint8_t)(i7 < 0 ? 0 : i7); char small[8];
  CHECK(theoryProgName(p, small, sizeof small) == 7); CHECK_EQ((int)strlen(small), 7);   // a short buffer is cut, never overrun
  CHECK_EQ(theoryProgName(p, nullptr, 0), 0);
}

// ---------------------------------------------------------------------------------------------- the builders over 1000 seeds

TEST(thousand_seeds_in_key_in_band_capped_with_variants) {
  int outKey = 0, outBand = 0, overCap = 0, invalid = 0, noZero = 0, emptyVariants = 0, badLength = 0, polyBass = 0, polyLead = 0, chordOffVoicing = 0;
  int maxNotes[CategoryCount] = {0}; long notesSum[CategoryCount] = {0}; int patterns = 0;
  int leadAccents = 0, leadNotes = 0, bassSlides = 0, bassPairs = 0, padVoices = 0, padChords = 0;
  long voiceMoveSum = 0; int voiceMoves = 0, voiceMoveMax = 0;
  std::set<std::string> chordSigs, setSigs; std::map<int, int> progHits; std::set<int> voicingUsed, chordRhythmUsed, bassUsed, leadUsed;
  Pattern src, var;
  for (int seed = 1; seed <= kSeeds; seed++) {
    Rng rng((uint32_t)seed * 2654435761u);
    TheoryPlan plan; theoryPlan(rng, seed % 12, plan);
    progHits[plan.prog]++; voicingUsed.insert(plan.voicingTpl); chordRhythmUsed.insert(plan.chordRhythm); bassUsed.insert(plan.bassTpl); leadUsed.insert(plan.leadRhythmA); leadUsed.insert(plan.leadRhythmB);
    CHECK_EQ((int)plan.root, seed % 12);
    CHECK(plan.prog < kProgCount); CHECK_EQ((int)plan.nChords, (int)kProgs[plan.prog].n); CHECK_EQ((int)plan.chordSteps, (int)kProgs[plan.prog].beatsPerChord * 4);
    const int length = theoryLengthTicks(plan);
    CHECK(length == 2 * kTicksPerBar || length == 4 * kTicksPerBar);
    // pad voicings: 2..4 voices in the band, every voice a chord tone, smooth voice leading chord to chord
    for (int c = 0; c < plan.nChords; c++) {
      int n = 0; while (n < 4 && plan.voicing[c][n]) n++;
      const Voicing& vt = kVoicings[plan.voicingTpl];
      uint8_t pcs[4]; const int np = theoryChordPcs(plan.root, theoryChordCode(plan, c), vt.stack, vt.n, pcs);
      if (n != np) { outBand++; }
      for (int i = 0; i < n; i++) {
        const int p = plan.voicing[c][i];
        if (p < kTheoryPadLo || p > kTheoryPadHi) outBand++;
        if (i > 0 && p <= plan.voicing[c][i - 1]) invalid++;
        bool tone = false; for (int j = 0; j < np; j++) if (pcs[j] == p % 12) tone = true;
        if (!tone) outKey++;
      }
      padVoices += n; padChords++;
      if (c > 0) {
        int m = 0, mx = 0; int pn = 0; while (pn < 4 && plan.voicing[c - 1][pn]) pn++;
        const int k = n < pn ? n : pn;
        for (int i = 0; i < k; i++) { const int d = plan.voicing[c][i] > plan.voicing[c - 1][i] ? plan.voicing[c][i] - plan.voicing[c - 1][i] : plan.voicing[c - 1][i] - plan.voicing[c][i]; m += d; if (d > mx) mx = d; }
        voiceMoveSum += m; voiceMoves += k; if (mx > voiceMoveMax) voiceMoveMax = mx;
      }
    }
    std::string setSig;
    for (Category c : kMelodic) {
      theoryBuild(plan, c, src);
      patterns++;
      if (!valid(src)) invalid++;
      if ((int)src.lengthTicks != length) badLength++;
      if (src.count > kMaxNotes) overCap++;
      if (!onsetAtZero(src)) noZero++;
      outKey += outOfKey(plan, src);
      int lo, hi; band(c, lo, hi); outBand += outOfBand(src, lo, hi);
      if (src.count > maxNotes[c]) maxNotes[c] = src.count;
      notesSum[c] += src.count;
      ProjectKey key; key.root = plan.root;
      for (int v = 0; v < kVariants; v++) { buildVariant(c, src, v, (uint32_t)seed * 77u + (uint32_t)v, key, var); if (var.count == 0) emptyVariants++; if (var.count > kMaxNotes) overCap++; }
      if (c == Chord) {
        chordSigs.insert(sig(src));
        for (int i = 0; i < src.count; i++) {                   // every stab is a pad voicing tone of its chord (or that an octave down)
          const int ch = theoryChordAt(plan, src.notes[i].start()); bool ok = false;
          for (int j = 0; j < 4; j++) if (plan.voicing[ch][j] && plan.voicing[ch][j] % 12 == src.notes[i].pitch() % 12) ok = true;
          if (!ok) chordOffVoicing++;
        }
      }
      if (c == Bass || c == Lead) {                             // one voice: no two notes start together; the bass overlaps only by a slide (12 ticks past the next onset)
        for (int i = 1; i < src.count; i++) {
          if (src.notes[i].start() == src.notes[i - 1].start()) { if (c == Bass) polyBass++; else polyLead++; }
          const int over = (int)src.notes[i - 1].end() - (int)src.notes[i].start();
          if (c == Bass) { bassPairs++; if (over > 0) { bassSlides++; if (over > kNoteLenUnit) polyBass++; } }
          else if (over > 0) polyLead++;
        }
      }
      if (c == Lead) for (int i = 0; i < src.count; i++) { leadNotes++; if (src.notes[i].vel() >= 100) leadAccents++; }
      setSig += sig(src) + "#";
    }
    setSigs.insert(setSig);
  }
  CHECK_EQ(outKey, 0); CHECK_EQ(outBand, 0); CHECK_EQ(overCap, 0); CHECK_EQ(invalid, 0); CHECK_EQ(noZero, 0); CHECK_EQ(emptyVariants, 0); CHECK_EQ(badLength, 0);
  CHECK_EQ(polyBass, 0); CHECK_EQ(polyLead, 0); CHECK_EQ(chordOffVoicing, 0);
  CHECK_EQ(patterns, kSeeds * 5);
  const double dupRate = 100.0 * (double)(kSeeds - (int)chordSigs.size()) / kSeeds;
  std::printf("  %d seeds x 5 categories: notes max Chord %d Lead %d Pad %d Bass %d Fx %d (mean %.1f / %.1f / %.1f / %.1f / %.1f)\n", kSeeds, maxNotes[Chord], maxNotes[Lead], maxNotes[Pad], maxNotes[Bass], maxNotes[Fx],
              (double)notesSum[Chord] / kSeeds, (double)notesSum[Lead] / kSeeds, (double)notesSum[Pad] / kSeeds, (double)notesSum[Bass] / kSeeds, (double)notesSum[Fx] / kSeeds);
  std::printf("  duplicate Chord patterns %.1f %%, distinct sets %d; progressions used %zu of %d; voicings %zu / %d, chord rhythms %zu / %d, bass %zu / %d, lead %zu / %d\n",
              dupRate, (int)setSigs.size(), progHits.size(), kProgCount, voicingUsed.size(), kVoicingCount, chordRhythmUsed.size(), kChordRhythmCount, bassUsed.size(), kBassTplCount, leadUsed.size(), kLeadRhythmCount);
  std::printf("  pad voices %.2f per chord, voice leading: mean move %.2f semitones per voice, max %d; lead accents %.1f %%; bass slides %.1f %% of note pairs\n",
              (double)padVoices / padChords, (double)voiceMoveSum / (voiceMoves ? voiceMoves : 1), voiceMoveMax, 100.0 * leadAccents / (leadNotes ? leadNotes : 1), 100.0 * bassSlides / (bassPairs ? bassPairs : 1));
  CHECK(dupRate < 2.0);                                         // SPEC §9.24: 100+ progressions x templates x voicings
  CHECK((int)setSigs.size() >= kSeeds - 5);
  CHECK((int)progHits.size() >= 100);                           // most of the table comes up in 1000 draws
  CHECK((int)voicingUsed.size() == kVoicingCount); CHECK((int)chordRhythmUsed.size() >= kChordRhythmCount - 2); CHECK((int)bassUsed.size() >= kBassTplCount - 2); CHECK((int)leadUsed.size() >= kLeadRhythmCount - 2);
  CHECK((double)voiceMoveSum / voiceMoves < 3.5); CHECK(voiceMoveMax <= 12);
  CHECK((double)padVoices / padChords >= 2.9);
  CHECK(leadAccents * 100 >= leadNotes * 10 && leadAccents * 100 <= leadNotes * 40);
  CHECK(bassSlides * 100 >= bassPairs * 5 && bassSlides * 100 <= bassPairs * 30);
}

TEST(plan_and_patterns_are_deterministic_per_seed) {
  Pattern a, b;
  for (int seed = 1; seed <= 50; seed++) {
    Rng r1((uint32_t)seed), r2((uint32_t)seed);
    TheoryPlan p1, p2; theoryPlan(r1, 2, p1); theoryPlan(r2, 2, p2);
    CHECK(memcmp(&p1, &p2, sizeof p1) == 0);
    CHECK_EQ(r1.s, r2.s);                                       // the same number of draws
    for (Category c : kMelodic) { theoryBuild(p1, c, a); theoryBuild(p2, c, b); CHECK(sig(a) == sig(b)); }
  }
  // the key changes the pitches, not the choices
  Rng r1(99u), r2(99u); TheoryPlan pa, pe; theoryPlan(r1, 9, pa); theoryPlan(r2, 4, pe);
  CHECK_EQ((int)pa.prog, (int)pe.prog); CHECK_EQ((int)pa.chordRhythm, (int)pe.chordRhythm); CHECK_EQ((int)pa.bassTpl, (int)pe.bassTpl); CHECK_EQ(pa.seed, pe.seed);
  theoryBuild(pa, Bass, a); theoryBuild(pe, Bass, b);
  CHECK_EQ((int)a.count, (int)b.count);
  // a non-melodic category clears to the progression's length
  theoryBuild(pa, Beat, a); CHECK_EQ((int)a.count, 0); CHECK_EQ((int)a.lengthTicks, (int)theoryLengthTicks(pa));
}

TEST(lead_form_is_a_a_prime_b_a_prime) {
  int fourBar = 0, twoBar = 0, mismatch = 0, bDiffers = 0;
  Pattern src;
  for (int seed = 1; seed <= 300; seed++) {
    Rng rng((uint32_t)seed * 7919u);
    TheoryPlan plan; theoryPlan(rng, seed % 12, plan);
    theoryBuild(plan, Lead, src);
    const int bars = src.lengthTicks / kTicksPerBar;
    std::vector<std::set<int>> steps(bars);
    for (int i = 0; i < src.count; i++) steps[src.notes[i].start() / kTicksPerBar].insert((src.notes[i].start() % kTicksPerBar) / 24);
    if (bars == 4) {
      fourBar++;
      if (steps[1] != steps[3]) mismatch++;                     // bars 2 and 4 are both A': the same onsets
      if (steps[2] != steps[1] || steps[2] != steps[0]) bDiffers++;   // B is a different phrase (its own template) almost always
    } else twoBar++;
    CHECK(steps[0].count(0) == 1);                              // the downbeat of A always sounds
  }
  std::printf("  %d four-bar leads (A A' B A'), %d two-bar (A A'); B differs from A / A' in %d of them\n", fourBar, twoBar, bDiffers);
  CHECK_EQ(mismatch, 0); CHECK(fourBar > 0 && twoBar > 0); CHECK(bDiffers * 10 >= fourBar * 9);
}

TEST(pad_holds_each_chord_and_fx_hits_the_changes) {
  Pattern pad, fx, bass;
  int sustained = 0, chords = 0, fxOnChanges = 0, fxNotes = 0, bassOnRoot = 0;
  for (int seed = 1; seed <= 200; seed++) {
    Rng rng((uint32_t)seed * 104729u);
    TheoryPlan plan; theoryPlan(rng, seed % 12, plan);
    theoryBuild(plan, Pad, pad); theoryBuild(plan, Fx, fx); theoryBuild(plan, Bass, bass);
    const int span = plan.chordSteps * 24;
    for (int c = 0; c < plan.nChords; c++) {
      chords++;
      int voices = 0; for (int i = 0; i < pad.count; i++) if (pad.notes[i].start() == c * span) voices++;
      CHECK(voices >= 2);                                       // every chord enters with its voicing
      if (plan.padStyle == 0) { bool held = false; for (int i = 0; i < pad.count; i++) if (pad.notes[i].start() == c * span && (int)pad.notes[i].end() >= (c + 1) * span - 24) held = true; if (held) sustained++; }
      uint8_t pc; const uint8_t st = 0; theoryChordPcs(plan.root, theoryChordCode(plan, c), &st, 1, &pc);
      for (int i = 0; i < bass.count; i++) if (bass.notes[i].start() == c * span && bass.notes[i].pitch() % 12 == pc) { bassOnRoot++; break; }
    }
    for (int i = 0; i < fx.count; i++) { fxNotes++; if (fx.notes[i].start() % span == 0 || fx.notes[i].start() == fx.lengthTicks / 2) fxOnChanges++; }
  }
  std::printf("  %d chords: sustained pads held to the change in every sustain-style chord (%d), bass on the root at %d chord starts, %d of %d fx notes on chord changes\n", chords, sustained, bassOnRoot, fxOnChanges, fxNotes);
  CHECK_EQ(fxOnChanges, fxNotes);
  CHECK(bassOnRoot * 100 >= chords * 60);                       // most templates start a chord on its root (fifth / octave / third openers are the rest)
}

int main() { return lp_test::run(); }
