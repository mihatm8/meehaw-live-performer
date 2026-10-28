// Pattern transforms ported from pattern_variants.lua (SPEC §5, §6).
//
// The Lua works on quarter-note floats; here notes are the packed engine Note at kPPQN ticks:
//   1 qn = 96 ticks, 0.125 qn (drum hit, mutate shift) = 12 ticks, 0.25 qn (fx grid) = 24 ticks,
//   MIN_NOTE_LENGTH (1/128 qn) → 1 tick, which Note then rounds up to one kNoteLenUnit (12 ticks).
// Every transform reads a note's fields into int locals, computes at full precision and packs once; only
// normalize() touches packed lengths again (clip to the loop, merge duplicates, sort). Runs on the UI
// task: no heap, one static scratch Pattern, stack frames well under 1 KB.
#include "variants.h"
#include "theory_tables.hpp"                 // SPEC §9.24: the theory generator's flash tables (generated, tools/make_theory_tables.py)

namespace lp {
using namespace theory;
namespace {

constexpr int kQn = kPPQN;                   // 96 ticks per quarter note
constexpr int kHit = kQn / 8;                // 12 ticks = 0.125 qn (drum_note length, mutate time delta)
constexpr int kFxGrid = kQn / 4;             // 24 ticks = 0.25 qn (generated_fx grid, :518)
constexpr uint32_t kSeedMix = 0x9E3779B9u;   // per-variant seed spread (the Lua shares one rng across the bank)

// murmur3 fmix32: xorshift32's first outputs for small raw seeds are poorly mixed (seeds 1..15 all give a
// first draw ≡ 0 mod 3), so the Rng state is derived from a hashed (seed, variant) pair.
uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16; return x; }

Pattern gScratch;                            // sorted source copy for transforms whose output can grow (lead split)

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
int absi(int v) { return v < 0 ? -v : v; }
int roundTicks(float t) { return (int)(t + 0.5f); }          // nearest tick, t >= 0
int scaleVel(int vel, float scale) { return clampi((int)((float)vel * scale + 0.5f), 1, 127); }   // filtered(), :302

// random_int / random_chance, :75-83. random_int draws nothing when maximum <= 1 (same as the Lua).
int randomInt(Rng& rng, int maximum) { return maximum <= 1 ? 1 : rng.range(1, maximum); }
bool randomChance(Rng& rng, float percent) { return (float)randomInt(rng, 1000) <= percent * 10.0f; }

Note mkNote(int start, int len, int pitch, int vel) {
  if (len < 1) len = 1;
  return Note::make((uint16_t)start, (uint16_t)len, (uint8_t)clampi(pitch, 0, 127), (uint8_t)clampi(vel, 1, 127));
}
// Appends [start, end) (dropped silently when the pattern is full).
void put(Pattern& p, int start, int end, int pitch, int vel) { p.add(mkNote(start, end - start, pitch, vel)); }
void copyPattern(const Pattern& src, Pattern& out) { if (&out != &src) out = src; }

// sort_notes order, :38-49: start, then pitch, then velocity descending.
bool before(const Note& a, const Note& b) {
  if (a.start() != b.start()) return a.start() < b.start();
  if (a.pitch() != b.pitch()) return a.pitch() < b.pitch();
  return a.vel() > b.vel();
}
void sortNotes(Pattern& p) {                                   // stable insertion sort, patterns are small
  for (int i = 1; i < p.count; i++) {
    Note k = p.notes[i]; int j = i - 1;
    while (j >= 0 && before(k, p.notes[j])) { p.notes[j + 1] = p.notes[j]; j--; }
    p.notes[j + 1] = k;
  }
}
// Index one past the onset group (equal start) that begins at i, group_onsets() :85-97 on a sorted pattern.
int groupEnd(const Pattern& p, int i) {
  const uint16_t s = p.notes[i].start(); int j = i + 1;
  while (j < p.count && p.notes[j].start() == s) j++;
  return j;
}

// Packed length for [start, end] inside a loop of `length` ticks. Note rounds lengths up to kNoteLenUnit;
// when that would push the note past the loop end it is rounded down instead, never below one unit.
uint16_t packedLen(int start, int end, int length) {
  int len = end - start; if (len < 1) len = 1;
  int units = (len + kNoteLenUnit - 1) / kNoteLenUnit;
  if (units > 1 && start + units * kNoteLenUnit > length) units--;
  return (uint16_t)(units * kNoteLenUnit);
}

// Merge duplicates by (start, pitch) keeping the longest end and loudest velocity, :61-69. Sorted input.
void mergeDuplicates(Pattern& p) {
  int w = 0;
  for (int i = 0; i < p.count; i++) {                          // duplicates are adjacent once sorted
    const Note n = p.notes[i];
    if (w > 0 && p.notes[w - 1].start() == n.start() && p.notes[w - 1].pitch() == n.pitch()) {
      Note& e = p.notes[w - 1];
      if (n.end() > e.end()) e.setLen((uint16_t)(n.end() - e.start()));
      if (n.vel() > e.vel()) e.setVel(n.vel());
    } else {
      p.notes[w++] = n;
    }
  }
  p.count = (uint16_t)w;
}

// normalize(), :51-73: drop notes starting at or after the loop end, clamp ends into (start, length],
// merge duplicates by (start, pitch) keeping the longest end and loudest velocity, sort.
void normalize(Pattern& p, int length) {
  if (length < 1) length = 1;
  p.lengthTicks = (uint16_t)length;
  int w = 0;
  for (int i = 0; i < p.count; i++) {
    const Note n = p.notes[i];
    const int start = n.start();
    if (start >= length) continue;                             // :58
    int end = n.end(); if (end > length) end = length;         // :59-60
    if (end < start + 1) end = start + 1;
    p.notes[w++] = Note::make((uint16_t)start, packedLen(start, end, length), n.pitch(), n.vel());
  }
  p.count = (uint16_t)w;
  sortNotes(p);
  mergeDuplicates(p);
}

// The "Original" slot (build_bank :548): the source as loaded or edited, normalized so the sequencer cursor
// (engine.cpp:194-196, which walks notes sorted by start and fires every entry) never sees an unsorted
// appended edit or an exact duplicate note twice. Lengths are unchanged.
void original(const Pattern& src, Pattern& out) {
  copyPattern(src, out);
  normalize(out, src.lengthTicks);
}

// Pitch i of chord candidate c (0 base, 1 up: lowest + 12, 2 down: highest − 12) for a sorted base, :176-191.
// The moved pitch is placed by counting the base pitches below it instead of materialising sorted copies.
int candidatePitch(const uint8_t* base, int n, int c, int i) {
  if (c == 0) return base[i];
  if (c == 1) {
    const int moved = base[0] + 12; int k = 0;
    while (k + 1 < n && base[k + 1] < moved) k++;
    return i < k ? base[i + 1] : (i == k ? moved : base[i]);
  }
  const int moved = base[n - 1] - 12; int k = 0;
  while (k < n - 1 && base[k] < moved) k++;
  return i < k ? base[i] : (i == k ? moved : base[i - 1]);
}

// drum_note(), :309-313
void putDrum(Pattern& p, int pitch, int start, int vel, int length) {
  int end = start + kHit; if (end > length) end = length;
  put(p, start, end, pitch, vel);
}
// Core voices take priority over auxiliary hits when the pattern is full (firmware cap, not in the Lua): the
// latest accent (pitch >= 40) is evicted to make room; with only core hits left the new hit is dropped.
void putCore(Pattern& p, int pitch, int start, int vel, int length) {
  if (p.count >= kMaxNotes) {
    int victim = -1;
    for (int i = p.count - 1; i >= 0; i--) if (p.notes[i].pitch() >= 40) { victim = i; break; }
    if (victim < 0) return;
    for (int i = victim; i + 1 < p.count; i++) p.notes[i] = p.notes[i + 1];
    p.count--;
  }
  putDrum(p, pitch, start, vel, length);
}

// ensure_beat_core(), :317-343: keep pitches 36..58, fill absent kick / hat / snare on a plain grid.
void ensureBeatCore(Pattern& p, int length) {
  normalize(p, length);
  bool present[128] = {};
  int w = 0;
  for (int i = 0; i < p.count; i++) {
    const int pitch = p.notes[i].pitch();
    if (pitch < 36 || pitch > 58) continue;                    // :320
    present[pitch] = true;
    p.notes[w++] = p.notes[i];
  }
  p.count = (uint16_t)w;
  if (!present[36]) {                                          // :325-329, one kick per beat
    for (int pos = 0; pos < length; pos += kQn) putCore(p, 36, pos, 94, length);
  }
  if (!present[38] && !present[39]) {                          // :330-335, closed hat on the off-beats
    const int first = length > kQn / 2 ? kQn / 2 : 0;
    for (int pos = first; pos < length; pos += kQn) putCore(p, 38, pos, 69, length);
  }
  if (!present[37]) {                                          // :336-341, snare on 2 and 4
    const int first = length > kQn ? kQn : 0;
    for (int pos = first; pos < length; pos += 2 * kQn) putCore(p, 37, pos, 86, length);
  }
  normalize(p, length);
}

} // namespace

// crop(), :102-124: keep the notes starting inside the first L ticks. If that fragment would be silent,
// move the source's first onset group to 0 so the short variant is still useful.
void cropPattern(const Pattern& src, uint16_t lengthTicks, Pattern& out) {
  const int length = lengthTicks < 1 ? 1 : lengthTicks;
  copyPattern(src, out);
  const int n = out.count;
  int w = 0;
  for (int i = 0; i < n; i++) if (out.notes[i].start() < length) out.notes[w++] = out.notes[i];   // :105-109
  if (w == 0 && n > 0) {                                       // :110-122 (nothing written yet, notes intact)
    int minStart = 0x7FFF;
    for (int i = 0; i < n; i++) if (out.notes[i].start() < minStart) minStart = out.notes[i].start();
    for (int i = 0; i < n; i++) {
      const Note m = out.notes[i];
      if (m.start() != minStart) continue;
      int duration = m.end() - m.start(); if (duration < 1) duration = 1;   // :116
      int end = duration; if (end > length) end = length;                   // :118
      out.notes[w++] = mkNote(0, end, m.pitch(), m.vel());
    }
  }
  out.count = (uint16_t)w;
  normalize(out, length);                                      // :123
}

// mutate(), :128-162 (+ the normalize build_bank applies at :564): one forced onset group plus a random
// share of the others move by scale steps, velocity and sometimes ±1/8 note.
void mutatePattern(const Pattern& src, float strength, Rng& rng, const ProjectKey& key, Pattern& out) {
  const int length = src.lengthTicks;
  copyPattern(src, out);
  sortNotes(out);
  if (out.count == 0) { normalize(out, length); return; }      // :132
  int groups = 0;
  for (int g = 0; g < out.count; g = groupEnd(out, g)) groups++;
  const int forced = randomInt(rng, groups);                   // :134
  const bool strong = strength >= 0.4f;
  int index = 0;
  for (int g = 0; g < out.count;) {
    const int gEnd = groupEnd(out, g);
    index++;
    if (index == forced || randomChance(rng, strength * 100.0f)) {       // :136 (no draw for the forced group)
      int steps = randomInt(rng, strong ? 4 : 2);                        // :137
      if (randomInt(rng, 2) == 1) steps = -steps;                        // :138
      const int velDelta = randomInt(rng, strong ? 31 : 15) - (strong ? 16 : 8);   // :139-140
      int timeDelta = 0;
      if (randomChance(rng, strong ? 45.0f : 20.0f)) {                   // :142-145
        timeDelta = randomInt(rng, 2) == 1 ? -kHit : kHit;
        const int gStart = out.notes[g].start();
        if (timeDelta < -gStart) timeDelta = -gStart;
      }
      for (int i = g; i < gEnd; i++) {                                   // :146-152
        const Note n = out.notes[i];
        int duration = n.end() - n.start(); if (duration < 1) duration = 1;
        int start = n.start() + timeDelta; if (start < 0) start = 0;
        out.notes[i] = mkNote(start, duration, key.moveScaleSteps(n.pitch(), steps), n.vel() + velDelta);
      }
    }
    g = gEnd;
  }
  normalize(out, length);                                      // :155 sort, :564 normalize
}

// chord_variation(), :169-216: keep each chord's pitch classes, pick the inversion with the least voice
// movement from the previous chord (small random tie-break), apply one articulation gate to the bank.
void chordVariation(const Pattern& src, Rng& rng, const ProjectKey& key, Pattern& out) {
  const int length = src.lengthTicks;
  copyPattern(src, out);
  sortNotes(out);
  uint8_t prev[kMaxNotes]; int prevCount = 0;                  // `previous`, :170
  uint8_t base[kMaxNotes];
  const int articulation = randomInt(rng, 3);                  // :171
  for (int g = 0; g < out.count;) {
    const int gEnd = groupEnd(out, g);
    const int n = gEnd - g;
    for (int i = 0; i < n; i++) base[i] = (uint8_t)key.snap(out.notes[g + i].pitch());   // :173-175
    for (int i = 1; i < n; i++) { uint8_t k = base[i]; int j = i - 1; while (j >= 0 && base[j] > k) { base[j + 1] = base[j]; j--; } base[j + 1] = k; }
    // Scores are the Lua's (:194-197) scaled by 20 and kept in integers: tie-break 0.1·t → 2·t, movement
    // 1·|d| → 20·|d|, deviation 0.15·|e| → 3·|e|. Exact ties between candidates are common (brute force
    // over two-note chords finds thousands) and must fall to the first candidate by the strict '<' on every
    // compiler and FPU, not to float rounding or evaluation order (Xtensa GCC contracts differently).
    int best = 0; int bestScore = 0x7FFFFFFF;
    for (int c = 0; c < 3; c++) {                              // :192-200
      if (c == 1 && (n < 2 || base[0] + 12 > 127)) continue;   // :177-184
      if (c == 2 && (n < 2 || base[n - 1] - 12 < 0)) continue; // :185-190
      int score = randomInt(rng, 3) * 2;                       // :194
      for (int i = 0; i < n; i++) {
        const int pitch = candidatePitch(base, n, c, i);
        const int anchor = prevCount > 0 ? prev[i < prevCount ? i : prevCount - 1] : base[i];   // :196
        score += 20 * absi(pitch - anchor) + 3 * absi(pitch - base[i]);                          // :197
      }
      if (score < bestScore) { bestScore = score; best = c; }
    }
    for (int i = 0; i < n; i++) prev[i] = (uint8_t)candidatePitch(base, n, best, i);          // :201
    prevCount = n;
    const int nextStart = gEnd < out.count ? out.notes[gEnd].start() : length;                // :202
    const float gate = articulation == 1 ? 0.88f : (articulation == 2 ? 0.75f : 0.95f);        // :208
    for (int v = 0; v < n; v++) {                              // :203-213
      const Note note = out.notes[g + v];
      const int start = note.start();
      int end = note.end(); if (end > nextStart) end = nextStart;
      const int len = roundTicks((float)(end - start) * gate);
      out.notes[g + v] = mkNote(start, len, prev[v], note.vel() - (v == 0 ? 0 : 3));
    }
    g = gEnd;
  }
  normalize(out, length);                                      // :215
}

// lead_variation(), :222-260: monophonic ornaments (passing tones on weak beats, gate or repeated-note
// split by style); any polyphonic onset falls back to chord_variation. Exact duplicate notes (same start and
// pitch, present in some library files and in edited sources) are merged first so they do not count as
// polyphony: the Lua would take the chord fallback for them and silently lose the lead treatment.
void leadVariation(const Pattern& src, Rng& rng, const ProjectKey& key, Pattern& out) {
  const int length = src.lengthTicks;
  Pattern& s = gScratch;
  copyPattern(src, s);
  sortNotes(s);
  mergeDuplicates(s);
  for (int i = 1; i < s.count; i++) {
    if (s.notes[i].start() == s.notes[i - 1].start()) { chordVariation(s, rng, key, out); return; }   // :224-226
  }
  const int style = randomInt(rng, 3);                         // :227
  out.clear((uint16_t)(length < 1 ? 1 : length));
  const int groups = s.count;
  for (int i = 0; i < groups; i++) {
    const Note n = s.notes[i];
    const int start = n.start();
    int pitch = key.snap(n.pitch());                           // :230
    const bool following = i + 1 < groups;
    const int nextStart = following ? s.notes[i + 1].start() : length;   // :232
    int end = n.end(); if (end > nextStart) end = nextStart;             // :233
    const bool weak = (start % kQn) != 0;                                // :234
    if (i > 0 && following && weak && style != 3) {                      // :235-244
      const int target = key.snap(s.notes[i + 1].pitch());
      const int direction = target > pitch ? 1 : (target < pitch ? -1 : 0);
      const int passing = key.moveScaleSteps(pitch, direction);
      if (direction != 0 && absi(target - pitch) <= 7 && absi(target - passing) < absi(target - pitch)) pitch = passing;
    }
    int duration = end - start; if (duration < 1) duration = 1;          // :245
    // Firmware cap: a split takes two slots, so it is only made while both halves and one slot for every
    // later onset still fit; otherwise the note is gated like style 1 so the rest of the phrase survives.
    const bool room = (int)out.count + 2 + (groups - 1 - i) <= kMaxNotes;
    if (style == 3 && duration >= kQn / 2 && i < groups - 1 && room) {   // :246-253
      const float split = (float)start + (float)duration * 0.5f;
      float gap = (float)duration * 0.05f; if (gap > 3.0f) gap = 3.0f;   // 0.03125 qn = 3 ticks
      put(out, start, roundTicks(split - gap), pitch, n.vel());
      put(out, roundTicks(split), end, pitch, n.vel() - 9);
    } else {
      put(out, start, roundTicks((float)start + (float)duration * (style == 2 ? 0.80f : 0.94f)), pitch, n.vel());   // :255
    }
  }
  normalize(out, length);                                      // :259
}

// limit_polyphony(), :265-295: outer two voices or the highest voice per onset, ended at the next onset.
void limitPolyphony(const Pattern& src, int voices, Pattern& out) {
  const int length = src.lengthTicks;
  copyPattern(src, out);
  sortNotes(out);                                              // :270-273 (pitch asc, velocity desc)
  int w = 0;
  for (int g = 0; g < out.count;) {
    const int gEnd = groupEnd(out, g);
    const int nextOnset = gEnd < out.count ? out.notes[gEnd].start() : length;   // :283-284
    int pick[2]; int picks = 0;
    if (voices <= 1) pick[picks++] = gEnd - 1;                 // :275-276 highest
    else if (gEnd - g == 1) pick[picks++] = g;                 // :277-278
    else { pick[picks++] = g; pick[picks++] = gEnd - 1; }      // :279-281 lowest + highest
    for (int k = 0; k < picks; k++) {                          // reads precede the write at w <= g
      const Note n = out.notes[pick[k]];
      const int start = n.start();
      int end = n.end(); if (end > nextOnset) end = nextOnset;
      if (end <= start) end = start + 1;                       // :288-290
      out.notes[w++] = mkNote(start, end - start, n.pitch(), n.vel());
    }
    g = gEnd;
  }
  out.count = (uint16_t)w;
  normalize(out, length);                                      // :294
}

// generated_fx(), :513-536: a new phrase on a 1/16 grid from the source's pitch pool.
void generatedFx(const Pattern& src, uint16_t lengthTicks, Rng& rng, const ProjectKey& key, Pattern& out) {
  uint8_t pool[kMaxNotes]; int poolCount = 0;
  for (int i = 0; i < src.count && poolCount < kMaxNotes; i++) pool[poolCount++] = src.notes[i].pitch();   // :514-515
  if (poolCount == 0) pool[poolCount++] = (uint8_t)(60 + key.root);                                   // :516
  const int length = lengthTicks < 1 ? 1 : lengthTicks;
  out.clear((uint16_t)length);
  const int steps = length / kFxGrid < 1 ? 1 : length / kFxGrid;   // :519
  for (int step = 0; step < steps; step++) {
    if (step != 0 && !randomChance(rng, 42.0f)) continue;      // :521
    int pitch = pool[randomInt(rng, poolCount) - 1];           // :522
    pitch = key.moveScaleSteps(pitch, randomInt(rng, 5) - 3);  // :523
    const int start = step * kFxGrid;                          // :524
    int end = start + (randomInt(rng, 2) == 1 ? kHit : kFxGrid); if (end > length) end = length;   // :528
    const int vel = 55 + randomInt(rng, 60);                   // :531
    put(out, start, end, pitch, vel);
  }
  normalize(out, length);                                      // :535
}

// beat_layers(), :345-380: cumulative Kick / Kick+Hats / +Snare / Full layers over the completed core.
// Full keeps ONE accent pitch from 40..58 present in the source (or a pitch-40 pickup); the pitch stays as
// it is in the pattern, the engine plays 40..58 on the open-hat slot at ×0.8 (SPEC §9.6).
void beatLayer(const Pattern& src, int layer, Rng& rng, Pattern& out) {
  const int length = src.lengthTicks < 1 ? 1 : src.lengthTicks;   // :346
  layer = clampi(layer, 0, kVariants - 1);
  copyPattern(src, out);
  ensureBeatCore(out, length);                                 // :347
  bool present[128] = {};                                      // :348-355 scanned from the completed core, since
  for (int i = 0; i < out.count; i++) present[out.notes[i].pitch()] = true;   // putCore may have evicted accents
  uint8_t aux[19]; int auxCount = 0;                           // ascending like the Lua sort
  for (int pitch = 40; pitch <= 58; pitch++) if (present[pitch]) aux[auxCount++] = (uint8_t)pitch;
  const int accent = auxCount > 0 ? aux[randomInt(rng, auxCount) - 1] : 40;   // :356
  if (auxCount == 0) {                                         // :357-360 pickup so Full differs from +Snare
    float back = (float)length * 0.25f; if (back > (float)(kQn / 2)) back = (float)(kQn / 2);
    int pickup = (int)((float)length - back); if (pickup < 0) pickup = 0;
    putCore(out, accent, pickup, 62, length);                  // kMaxNotes core hits leave no room: Full == +Snare then
  }
  int w = 0;
  for (int i = 0; i < out.count; i++) {                        // :361-368 + filtered() :297-307
    const Note n = out.notes[i];
    const int pitch = n.pitch();
    bool keep;
    if (layer == 0) keep = pitch == 36;
    else if (layer == 1) keep = pitch == 36 || pitch == 38 || pitch == 39;
    else if (layer == 2) keep = pitch >= 36 && pitch <= 39;
    else keep = (pitch >= 36 && pitch <= 39) || pitch == accent;
    if (!keep) continue;
    out.notes[w++] = n;
  }
  out.count = (uint16_t)w;
  normalize(out, length);
}

// percussion_layers(), :483-511: every pitch keeps its hits by 1-based ordinal in time order and the
// layer's velocity scale (Sparse (n−1)%4==0 ×0.55, Light n odd ×0.70, Driving n%4≠0 ×0.85, Full ×1).
void percussionLayer(const Pattern& src, int layer, Pattern& out) {
  const int length = src.lengthTicks;
  layer = clampi(layer, 0, kVariants - 1);
  static const float kScale[kVariants] = {0.55f, 0.70f, 0.85f, 1.0f};   // :493
  copyPattern(src, out);
  sortNotes(out);                                              // :484
  uint8_t ordinalByVoice[128] = {};                            // :486-491 (channel is always 0 here)
  int w = 0;
  for (int i = 0; i < out.count; i++) {
    Note n = out.notes[i];
    const int ordinal = ++ordinalByVoice[n.pitch()];
    bool keep;
    if (layer == 0) keep = (ordinal - 1) % 4 == 0;             // :495
    else if (layer == 1) keep = ordinal % 2 == 1;              // :496
    else if (layer == 2) keep = ordinal % 4 != 0;              // :497
    else keep = true;                                          // :498
    if (!keep) continue;
    n.setVel((uint8_t)scaleVel(n.vel(), kScale[layer]));       // :302
    out.notes[w++] = n;
  }
  out.count = (uint16_t)w;
  normalize(out, length);                                      // :306
}

// SPEC §6 / §9.7: the four most frequent pitches become voices 36..39 (count desc, pitch asc), every other
// pitch goes to 36 + (pitch & 3). The map is a pure function of the source's pitch counts; the generator
// computes it once at load and stores it, and buildVariant(..., foldMap) applies the stored map so later
// edits (which change the counts) never permute the voices of a loaded pattern.
void percussionFoldMap(const Pattern& src, uint8_t map[128]) {
  uint8_t count[128] = {};
  for (int i = 0; i < src.count; i++) {
    const int pitch = src.notes[i].pitch();
    if (count[pitch] < 255) count[pitch]++;
  }
  for (int pitch = 0; pitch < 128; pitch++) map[pitch] = (uint8_t)(36 + (pitch & 3));
  for (int rank = 0; rank < 4; rank++) {
    int best = -1;
    for (int pitch = 0; pitch < 128; pitch++) if (count[pitch] > 0 && (best < 0 || count[pitch] > count[best])) best = pitch;
    if (best < 0) break;
    map[best] = (uint8_t)(36 + rank);
    count[best] = 0;
  }
}
void applyPercussionFold(Pattern& p, const uint8_t map[128]) {
  for (int i = 0; i < p.count; i++) p.notes[i].setPitch(map[p.notes[i].pitch()]);
  normalize(p, p.lengthTicks);                                 // merged hits keep the loudest velocity
}
void foldPercussionVoices(Pattern& p) {
  uint8_t map[128];
  percussionFoldMap(p, map);
  applyPercussionFold(p, map);
}

// SPEC §9.6: snare rush = crop(Full, 1 beat) without the kick; when no snare is left, snares on every 1/16.
void snareRush(const Pattern& fullLayer, Pattern& out) {
  cropPattern(fullLayer, (uint16_t)kTicksPerBeat, out);
  int w = 0; bool snare = false;
  for (int i = 0; i < out.count; i++) {
    const Note n = out.notes[i];
    if (n.pitch() == 36) continue;
    if (n.pitch() == 37) snare = true;
    out.notes[w++] = n;
  }
  out.count = (uint16_t)w;
  if (!snare) {
    static const uint8_t kVel[4] = {100, 80, 90, 80};
    for (int i = 0; i < 4; i++) putDrum(out, 37, i * (kTicksPerBeat / 4), kVel[i], kTicksPerBeat);
  }
  normalize(out, kTicksPerBeat);
}

// ---------------------------------------------------------------------------------------------- acid (SPEC §9.17b)

namespace {
constexpr int kSixteenth = kQn / 4;                            // 24 ticks: the acid step
constexpr uint32_t kAcidLineSalt = 0xAC1D11EEu, kAcidStabSalt = 0x57AB5EEDu;   // the line never correlates with variant 1 of the same seed
}

void acidLine(uint32_t seed, const ProjectKey& key, Pattern& out, int baseOctave) {
  Rng rng(mix32(seed ^ kAcidLineSalt));
  const int steps = rng.unit() < 0.6f ? 16 : 32;
  const int length = steps * kSixteenth;
  out.clear((uint16_t)length);
  const float restP = 0.10f + rng.unit() * 0.15f;             // 10..25 % rests, decided per line
  bool on[32]; int pitch[32];
  for (int s = 0; s < steps; s++) on[s] = s == 0 || rng.unit() >= restP;   // the downbeat always sounds
  static const int kDegrees[8] = {0, 0, 0, 0, 4, 6, 2, 3};     // root-heavy: root, 5th, b7, b3, 4th
  for (int s = 0; s < steps; s++) {
    if (!on[s]) { pitch[s] = 0; continue; }
    int oct = baseOctave;
    const float u = rng.unit();
    if (u < 0.22f) oct += 1; else if (u < 0.28f) oct -= 1;     // octave jumps
    int p = clampi(key.degreePitch(kDegrees[rng.range(0, 7)], oct), 12, 115);
    if (!key.contains(p)) p = key.snap(p);
    pitch[s] = clampi(p, 12, 115);
  }
  for (int s = 0; s < steps; s++) {
    if (!on[s]) continue;
    const int start = s * kSixteenth;
    const bool slide = s + 1 < steps && on[s + 1] && rng.unit() < 0.36f;   // ≈ 30 % of the notes slide into the next step
    const bool accent = rng.unit() < 0.20f;
    const int vel = accent ? 100 + rng.range(0, 27) : 64 + rng.range(0, 31);
    int end = slide ? (s + 1) * kSixteenth + kHit                           // next onset + 12 ticks: the note-off follows the next note-on
                    : start + (rng.unit() < 0.7f ? kHit : kSixteenth);      // staccato 1/32 or the full step
    if (end > length) end = length;
    put(out, start, end, pitch[s], vel);
  }
  normalize(out, length);
}

void acidStabs(uint32_t seed, const ProjectKey& key, Pattern& out, int baseOctave) {
  Rng rng(mix32(seed ^ kAcidStabSalt));
  const int bars = rng.unit() < 0.6f ? 1 : 2;
  const int length = bars * kTicksPerBar;
  out.clear((uint16_t)length);
  static const int kVoicing[4][3] = {{0, 2, 4}, {0, 2, 6}, {0, 4, 6}, {0, 3, 4}};   // triad, m7 (no 5th), 7 (no 3rd), sus4
  static const int kRoots[6] = {0, 0, 0, 3, 5, 6};             // tonic mostly, iv, bVI, bVII
  const int voicing = rng.range(0, 3);
  const int voices = rng.unit() < 0.35f ? 2 : 3;
  for (int b = 0; b < bars; b++) {
    const int chordRoot = kRoots[rng.range(0, 5)];
    const bool invert = rng.unit() < 0.30f;                    // lowest voice up an octave
    for (int pos = kSixteenth; pos < kTicksPerBar; pos += kSixteenth) {
      if (pos % kQn == 0) continue;                            // never on a quarter: offbeats only
      const bool eighth = pos % (2 * kSixteenth) == 0;         // the "and" of each beat
      const bool forced = pos == 2 * kSixteenth;               // at least one stab per bar
      if (!forced && rng.unit() >= (eighth ? 0.72f : 0.15f)) continue;
      const int start = b * kTicksPerBar + pos;
      const int len = rng.unit() < 0.5f ? kSixteenth : kSixteenth + kHit;   // 24 or 36 ticks
      const bool accent = rng.unit() < 0.20f;
      const int vel = accent ? 100 + rng.range(0, 20) : 76 + rng.range(0, 20);
      for (int v = 0; v < voices; v++) {
        const int deg = kVoicing[voicing][voices == 2 ? (v == 0 ? 0 : 2) : v];   // two voices: the outer ones
        int p = key.degreePitch(chordRoot + deg, baseOctave);
        if (v == 0 && invert) p += 12;
        p = clampi(p, 12, 115);
        if (!key.contains(p)) p = key.snap(p);
        int end = start + len; if (end > length) end = length;
        put(out, start, end, clampi(p, 12, 115), v == 0 ? vel : vel - 3);
      }
    }
  }
  normalize(out, length);
}

// ---------------------------------------------------------------------------------------------- theory set generator (SPEC §9.24)

namespace {
constexpr int kStep = kQn / 4;                                 // 24 ticks: the 1/16 step of every template
constexpr uint32_t kTheoryChordSalt = 0x7E0C40DDu, kTheoryPadSalt = 0x7E0BADD0u, kTheoryBassSalt = 0x7E0BA550u,
                   kTheoryLeadSalt = 0x7E01EADDu, kTheoryFxSalt = 0x7E0F0F0Fu;
constexpr int kLeadMaxNotes = 20;                              // motif notes per bar (16 onsets + pickups)

int pickW(Rng& rng, const uint8_t* w, int n) {                 // weighted draw over w[0..n)
  int total = 0;
  for (int i = 0; i < n; i++) total += w[i];
  if (total <= 0) return 0;
  int r = rng.range(0, total - 1);
  for (int i = 0; i < n; i++) { r -= w[i]; if (r < 0) return i; }
  return n - 1;
}
int stepCode(uint32_t rhythm, int k) { return (int)((rhythm >> (2 * (k & 15))) & 3u); }
int bassCode(uint64_t tpl, int k) { return (int)((tpl >> (4 * (k & 15))) & 15u); }
int foldInto(int p, int lo, int hi) {                          // by octaves into [lo, hi]; the band is at least an octave wide
  while (p < lo) p += 12;
  while (p > hi) p -= 12;
  return clampi(p, lo, hi);
}
int floorMod12(int v) { return ((v % 12) + 12) % 12; }
const Prog& progOf(const TheoryPlan& plan) { return kProgs[plan.prog < kProgCount ? plan.prog : 0]; }
int voiceCount(const TheoryPlan& plan, int c) { int n = 0; while (n < 4 && plan.voicing[c][n]) n++; return n; }
// Steps the hold codes extend an onset at step k by (the template repeats every 16 steps inside the pattern).
int holdSteps(uint32_t rhythm, int k, int steps) { int h = 0; while (k + 1 + h < steps && stepCode(rhythm, k + 1 + h) == StepHold) h++; return h; }

// Candidate voicing of `pcs` (n ≤ 4, root first) with pcs[r] as the bass note, shifted s octaves, inside [lo, hi]: the
// close voicing (every other class in the octave above the bass, so the span is under an octave) or its drop-2 (the
// second-highest voice an octave down). False when it does not fit the band.
bool voiceCandidate(const uint8_t* pcs, int n, int r, int s, bool open, int lo, int hi, uint8_t* v) {
  int base = lo; while (floorMod12(base) != pcs[r % n]) base++;
  base += 12 * s;
  int p[4]; p[0] = base;
  for (int i = 1; i < n; i++) p[i] = base + floorMod12(pcs[(r + i) % n] - base);
  for (int i = 1; i < n; i++) { int x = p[i], j = i - 1; while (j >= 0 && p[j] > x) { p[j + 1] = p[j]; j--; } p[j + 1] = x; }   // sort
  if (open && n >= 3) { p[n - 2] -= 12; for (int i = 1; i < n; i++) { int x = p[i], j = i - 1; while (j >= 0 && p[j] > x) { p[j + 1] = p[j]; j--; } p[j + 1] = x; } }
  for (int i = 0; i < n; i++) if (p[i] < lo || p[i] > hi) return false;
  for (int i = 1; i < n; i++) if (p[i] == p[i - 1]) return false;   // a drop-2 that landed on another voice
  for (int i = 0; i < n; i++) v[i] = (uint8_t)p[i];
  return true;
}

// The chord's voicing with the least total voice movement from `prev` (voices matched by rank; first chord: nearest the
// band's centre). The open shape is tried first when the template asks for it, then the close one (a close voicing of
// 2–4 classes always has an inversion inside an 18-semitone band); the last resort drops the top class and tries again.
void voiceChord(const uint8_t* pcs, int n, bool open, const uint8_t* prev, int prevN, Rng& rng, uint8_t* out) {
  for (int voices = n; voices >= 2; voices--) {
    for (int pass = open && voices >= 3 ? 0 : 1; pass < 2; pass++) {
      int bestCost = 1 << 30; bool found = false; uint8_t best[4] = {0, 0, 0, 0};
      for (int r = 0; r < voices; r++) for (int s = -1; s <= 1; s++) {
        uint8_t v[4];
        if (!voiceCandidate(pcs, voices, r, s, pass == 0, kTheoryPadLo, kTheoryPadHi, v)) continue;
        int sum = 0; for (int i = 0; i < voices; i++) sum += v[i];
        const int centre = (kTheoryPadLo + kTheoryPadHi) / 2;
        int cost = absi(sum * 2 - centre * 2 * voices) / voices;   // 2 × |mean − centre|
        if (prevN > 0) { int m = 0; const int k = voices < prevN ? voices : prevN; for (int i = 0; i < k; i++) m += absi((int)v[i] - (int)prev[i]); cost = 4 * m + cost / 2; }
        cost += rng.range(0, 2);                               // deterministic tie-break
        if (cost < bestCost) { bestCost = cost; found = true; for (int i = 0; i < 4; i++) best[i] = i < voices ? v[i] : 0; }
      }
      if (found) { for (int i = 0; i < 4; i++) out[i] = best[i]; return; }
    }
  }
  for (int i = 0; i < 4; i++) out[i] = i < n ? (uint8_t)foldInto(60 + pcs[i], kTheoryPadLo, kTheoryPadHi) : 0;   // never reached: two classes always fit
}

int rootPc(const TheoryPlan& plan, int c) { uint8_t pc; const uint8_t st = 0; theoryChordPcs(plan.root, theoryChordCode(plan, c), &st, 1, &pc); return pc; }

void buildPad(const TheoryPlan& plan, Rng& rng, Pattern& out) {
  const int span = plan.chordSteps * kStep, length = plan.nChords * span;
  out.clear((uint16_t)length);
  const bool legato = (rng.next() & 3) == 0;                   // 25 %: the chords overlap by a 1/32 instead of breathing
  const int vel = 86 + rng.range(0, 3) * 4;
  for (int c = 0; c < plan.nChords; c++) {
    const int n = voiceCount(plan, c), start = c * span;
    const bool last = c + 1 == plan.nChords;
    const int endGap = last ? 0 : (legato ? -kHit : kHit);     // the last chord ends on the loop (normalize clamps)
    for (int i = 0; i < n; i++) {
      const int p = plan.voicing[c][i], v = vel - 2 * i;
      if (plan.padStyle == 1) {                                // swell: two halves, the second softer
        const int half = span / 2;
        put(out, start, start + half - kHit, p, v);
        put(out, start + half, start + span - endGap, p, v - 8);
      } else if (plan.padStyle == 2) {                         // pulse: quarter notes at 3/4 gate
        for (int t = 0; t < span; t += kQn) put(out, start + t, start + t + kQn - kStep, p, v - ((t / kQn) & 1 ? 6 : 0));
      } else put(out, start, start + span - endGap, p, v);     // sustain
    }
  }
  normalize(out, length);
}

void buildChord(const TheoryPlan& plan, Rng& rng, Pattern& out) {
  const int steps = plan.nChords * plan.chordSteps, length = steps * kStep;
  out.clear((uint16_t)length);
  const uint32_t rhythm = kChordRhythms[plan.chordRhythm < kChordRhythmCount ? plan.chordRhythm : 0];
  int onsets = 0;
  for (int k = 0; k < steps; k++) if (stepCode(rhythm, k) == StepOn || stepCode(rhythm, k) == StepAccent) onsets++;
  int voicesMax = 4;
  while (voicesMax > 2 && onsets * voicesMax > kMaxNotes) voicesMax--;   // dense templates on 4 bars: thinner voicings, never a dropped onset
  int low = 0;
  if (plan.chordLow) { int lo = 127; for (int c = 0; c < plan.nChords; c++) if (plan.voicing[c][0] && plan.voicing[c][0] < lo) lo = plan.voicing[c][0]; if (lo - 12 >= kTheoryChordLo) low = -12; }
  const int stabLen = plan.chordStyle == 2 ? (rng.unit() < 0.5f ? kHit : kStep) : kStep;
  int ordinal = 0;
  for (int k = 0; k < steps; k++) {
    const int code = stepCode(rhythm, k);
    if (code != StepOn && code != StepAccent) continue;
    const int c = k / plan.chordSteps, n = voiceCount(plan, c);
    if (n == 0) continue;
    const int h = holdSteps(rhythm, k, steps);
    const int start = k * kStep, end = h ? (k + 1 + h) * kStep : start + stabLen;
    const int vel = code == StepAccent ? 104 + rng.range(0, 11) : 80 + rng.range(0, 15);
    int skipLow = n > voicesMax ? n - voicesMax : 0;           // drop inner voices first (keep the root and the top)
    int vi = 0;
    for (int i = 0; i < n; i++) {
      if (skipLow && i > 0 && i < n - 1) { skipLow--; continue; }
      bool play = true;
      if (plan.chordStyle == 1) play = (ordinal & 1) == 0 || i >= n / 2;             // broken: full chord, then the upper voices
      else if (plan.chordStyle == 2) play = i == (ordinal % n) || (code == StepAccent && i == n - 1);   // plucks: one voice per onset (+ the top on accents)
      if (!play) continue;
      put(out, start, end, plan.voicing[c][i] + low, vel - 3 * vi);
      vi++;
    }
    if (vi == 0) put(out, start, end, plan.voicing[c][0] + low, vel);   // a style never leaves an onset silent
    ordinal++;
  }
  normalize(out, length);
}

void buildBass(const TheoryPlan& plan, Rng& rng, Pattern& out) {
  const int steps = plan.nChords * plan.chordSteps, length = steps * kStep;
  out.clear((uint16_t)length);
  const uint64_t tpl = kBassTpls[plan.bassTpl < kBassTplCount ? plan.bassTpl : 0];
  ProjectKey key; key.root = plan.root;
  int roots[kMaxChords];
  for (int c = 0; c < plan.nChords; c++) roots[c] = foldInto(36 + rootPc(plan, c), kTheoryBassLo, kTheoryBassLo + 11);   // 38..49: the octave stays <= 61 → 59 by the fold below
  int lastNote = -1;                                           // index of the note a hold extends
  for (int k = 0; k < steps; k++) {
    const int code = bassCode(tpl, k);
    if (code == BassRest) { lastNote = -1; continue; }
    if (code == BassHold) {                                    // extend the previous note through this step
      if (lastNote >= 0 && lastNote < out.count) { Note& nt = out.notes[lastNote]; nt.setLen((uint16_t)((k + 1) * kStep - nt.start())); }
      continue;
    }
    const int c = k / plan.chordSteps, root = roots[c];
    int p = root;
    switch (code) {
      case BassOct: case BassOctAccent: p = root + 12 <= kTheoryBassHi ? root + 12 : root; break;
      case BassFifth: case BassFifthBelow: {                   // the chord's own fifth (a diminished chord's is flat): above the root, or the octave below it
        uint8_t pcs[2]; const uint8_t st[2] = {0, 4}; const int n = theoryChordPcs(plan.root, theoryChordCode(plan, c), st, 2, pcs);
        const int above = n > 1 ? root + floorMod12(pcs[1] - root) : root + 7;
        if (code == BassFifthBelow) p = above - 12 >= kTheoryBassLo ? above - 12 : above;
        else p = above <= kTheoryBassHi ? above : above - 12;
        break;
      }
      case BassThird: { uint8_t pcs[2]; const uint8_t st[2] = {0, 2}; const int n = theoryChordPcs(plan.root, theoryChordCode(plan, c), st, 2, pcs); p = n > 1 ? root + floorMod12(pcs[1] - root) : root; break; }
      case BassApproach: {                                     // a scale step into the next chord's root (above or below), inside the band
        const int nr = roots[(c + 1) % plan.nChords];
        p = key.moveScaleSteps(nr, (rng.next() & 1) ? 1 : -1);
        break;
      }
      default: break;                                          // root
    }
    p = foldInto(p, kTheoryBassLo, kTheoryBassHi);
    const bool accent = code == BassRootAccent || code == BassOctAccent;
    const int vel = accent ? 100 + rng.range(0, 15) : 72 + rng.range(0, 19);
    const int h = [&] { int n = 0; while (k + 1 + n < steps && bassCode(tpl, k + 1 + n) == BassHold) n++; return n; }();
    int next = k + 1 + h; while (next < steps && bassCode(tpl, next) == BassRest) next++;   // the next onset step (or the end)
    int end;
    if (h) end = (k + 1 + h) * kStep;
    else if (plan.bassGate == 0) end = k * kStep + kHit;
    else if (plan.bassGate == 1) end = (k + 1) * kStep;
    else end = next * kStep - (next < steps ? kHit : 0);
    if (next < steps && next - (k + 1 + h) <= 3 && rng.unit() < 0.20f) end = next * kStep + kHit;   // ≈ 20 % slides into a note within the beat: the note-off follows the next note-on (legato glide, SPEC §9.12)
    if (end > length) end = length;
    lastNote = out.count;
    put(out, k * kStep, end, p, vel);
  }
  normalize(out, length);
}

struct MotifNote { uint8_t step, len, accent; int8_t deg; };   // step 0..15 of its bar, held steps, accent, absolute scale degree (0 = the root, 7 = an octave up)
struct Motif { MotifNote n[kLeadMaxNotes]; uint8_t count = 0; };

// Nearest absolute degree with the pitch class of scale degree d (0..6) to `from`; ties go up when `up`.
int nearestDegree(int from, int d, bool up) {
  const int base = ((from - d) % 7 + 7) % 7;                   // steps down to a degree ≡ d
  const int lo = from - base, hi = lo + 7;
  if (from - lo < hi - from) return lo;
  if (hi - from < from - lo) return hi;
  return up ? hi : lo;
}

void walkMotif(uint32_t rhythm, int startDeg, Rng& rng, Motif& m) {
  m.count = 0;
  int deg = startDeg;
  for (int k = 0; k < 16 && m.count < kLeadMaxNotes; k++) {
    const int code = stepCode(rhythm, k);
    if (code != StepOn && code != StepAccent) continue;
    if (m.count > 0) {
      const int d = pickW(rng, kLeadMarkov[((deg % 7) + 7) % 7], 7);
      deg = nearestDegree(deg, d, (rng.next() & 1) != 0);
      if (rng.unit() < 0.10f) deg += (rng.next() & 1) ? 7 : -7;   // an octave displacement
      deg = clampi(deg, -7, 13);
    }
    MotifNote& n = m.n[m.count++];
    n.step = (uint8_t)k; n.deg = (int8_t)deg; n.accent = (uint8_t)(code == StepAccent || rng.unit() < 0.08f ? 1 : 0);   // the template's accents + a few random ones ≈ 20 % of the notes
    n.len = (uint8_t)(1 + holdSteps(rhythm, k, 16));
  }
}

void varyMotif(const Motif& a, int op, Rng& rng, Motif& out) {
  out = a;
  if (out.count == 0) return;
  switch (op) {
    case VarSequence: { const int d = (rng.next() & 1) ? 1 : -1; for (int i = 0; i < out.count; i++) out.n[i].deg = (int8_t)clampi(out.n[i].deg + d, -7, 13); break; }
    case VarEnding: {                                          // the last one or two notes resolve to the root / fifth, the last one rings to the bar end
      const int first = out.count >= 2 ? out.count - 2 : out.count - 1;
      for (int i = first; i < out.count; i++) out.n[i].deg = (int8_t)nearestDegree(out.n[i].deg, i == out.count - 1 ? 0 : 4, false);
      MotifNote& l = out.n[out.count - 1]; l.len = (uint8_t)(16 - l.step);
      break;
    }
    case VarThin: {
      if (out.count > 2) {
        int weak = -1, seen = 0;
        for (int i = 1; i < out.count; i++) if (out.n[i].step % 4 != 0) { seen++; if (rng.range(1, seen) == 1) weak = i; }   // reservoir: one weak-step note
        if (weak >= 0) { for (int i = weak + 1; i < out.count; i++) out.n[i - 1] = out.n[i]; out.count--; }
      }
      break;
    }
    case VarOctave: { const int i = rng.range(0, out.count - 1); out.n[i].deg = (int8_t)clampi(out.n[i].deg + ((rng.next() & 1) ? 7 : -7), -7, 13); break; }
    case VarPickup: {                                          // a 1/16 before a beat that has a note and a free step before it
      for (int i = 0; i < out.count && out.count < kLeadMaxNotes; i++) {
        const int s = out.n[i].step;
        if (s == 0 || s % 4 != 0) continue;
        bool busy = false; for (int j = 0; j < out.count; j++) if (out.n[j].step == s - 1 || (out.n[j].step < s - 1 && out.n[j].step + out.n[j].len > s - 1)) busy = true;
        if (busy) continue;
        MotifNote p; p.step = (uint8_t)(s - 1); p.len = 1; p.accent = 0; p.deg = (int8_t)clampi(out.n[i].deg - ((rng.next() & 1) ? 1 : 2), -7, 13);
        for (int j = out.count; j > i; j--) out.n[j] = out.n[j - 1];
        out.n[i] = p; out.count++;
        break;
      }
      break;
    }
    case VarInvert: { const int base = out.n[0].deg; for (int i = 1; i < out.count; i++) out.n[i].deg = (int8_t)clampi(2 * base - out.n[i].deg, -7, 13); break; }
    default: break;
  }
}

// The nearest pitch to p whose pitch class is in the chord (bit mask), kept inside the lead band.
int snapToChord(int p, uint16_t chordMask) {
  if ((chordMask >> floorMod12(p)) & 1) return p;
  for (int d = 1; d <= 6; d++) {
    if ((chordMask >> floorMod12(p - d)) & 1) return foldInto(p - d, kTheoryLeadLo, kTheoryLeadHi);
    if ((chordMask >> floorMod12(p + d)) & 1) return foldInto(p + d, kTheoryLeadLo, kTheoryLeadHi);
  }
  return p;
}

void buildLead(const TheoryPlan& plan, Rng& rng, Pattern& out) {
  const int steps = plan.nChords * plan.chordSteps, length = steps * kStep, bars = steps / 16;
  out.clear((uint16_t)length);
  ProjectKey key; key.root = plan.root;
  const uint32_t rhythmA = kLeadRhythms[plan.leadRhythmA < kLeadRhythmCount ? plan.leadRhythmA : 0];
  const uint32_t rhythmB = kLeadRhythms[plan.leadRhythmB < kLeadRhythmCount ? plan.leadRhythmB : 0];
  Motif a, a2, b;
  walkMotif(rhythmA, pickW(rng, kLeadStart, 7), rng, a);
  varyMotif(a, plan.leadVar < kLeadVarCount ? plan.leadVar : 0, rng, a2);
  walkMotif(rhythmB, (rng.next() & 1) ? 4 : 2, rng, b);       // B starts on the fifth or the third: a contrasting phrase
  int prevPitch = 0;
  for (int bar = 0; bar < bars; bar++) {
    const Motif& m = bar == 0 ? a : (bars == 4 && bar == 2) ? b : a2;   // A A' B A' (A A' on two bars)
    for (int i = 0; i < m.count; i++) {
      const MotifNote& n = m.n[i];
      const int step = bar * 16 + n.step, start = step * kStep;
      if (start >= length) break;
      const int c = step / plan.chordSteps;
      int p = foldInto(key.degreePitch(n.deg, 4), kTheoryLeadLo, kTheoryLeadHi);   // degree 0 of octave 4 = the root near the band's centre
      if (prevPitch && absi(p - prevPitch) > 7) {              // keep the line connected: the octave nearer the previous note
        const int alt = p > prevPitch ? p - 12 : p + 12;
        if (alt >= kTheoryLeadLo && alt <= kTheoryLeadHi && absi(alt - prevPitch) < absi(p - prevPitch)) p = alt;
      }
      if (n.step % 4 == 0) {                                   // strong beats on chord tones (the chord's stack with its alteration)
        uint8_t pcs[4]; const uint8_t st[4] = {0, 2, 4, 6};
        const int np = theoryChordPcs(plan.root, theoryChordCode(plan, c), st, 4, pcs);
        uint16_t mask = 0; for (int j = 0; j < np; j++) mask = (uint16_t)(mask | (1u << pcs[j]));
        p = snapToChord(p, mask);
      } else if (!key.contains(p)) p = foldInto(key.snap(p), kTheoryLeadLo, kTheoryLeadHi);   // passing tones stay diatonic
      prevPitch = p;
      const int nextStart = i + 1 < m.count ? (bar * 16 + m.n[i + 1].step) * kStep : (bar + 1) * 16 * kStep;
      int end;
      if (n.len > 1) end = start + n.len * kStep;
      else if (plan.leadGate == 0) end = start + kStep;
      else if (plan.leadGate == 1) end = start + kHit;
      else end = start + ((nextStart - start) * 3 / 4 / kNoteLenUnit) * kNoteLenUnit;
      if (end <= start) end = start + kHit;
      if (end > nextStart && n.len <= 1) end = nextStart;      // monophonic: never past the next onset
      if (end > length) end = length;
      const int vel = n.accent ? 104 + rng.range(0, 15) : 76 + rng.range(0, 19);
      put(out, start, end, p, vel);
    }
  }
  normalize(out, length);
}

void buildFx(const TheoryPlan& plan, Rng& rng, Pattern& out) {
  const int span = plan.chordSteps * kStep, length = plan.nChords * span;
  out.clear((uint16_t)length);
  for (int c = 0; c < plan.nChords; c++) {
    const int start = c * span, p = foldInto(72 + rootPc(plan, c), 72, 83);
    const int vel = 96 + rng.range(0, 3) * 4;
    if (plan.fxStyle == 2) { if (c == 0 || start == length / 2) put(out, start, start + 2 * kStep, p, vel); continue; }
    put(out, start, start + (rng.next() & 1 ? kStep : 2 * kStep), p, vel);
    if (plan.fxStyle == 1 && c + 1 == plan.nChords) put(out, start, length, foldInto(p - 12, kTheoryFxLo, kTheoryFxHi), 80);   // the riser under the last chord
  }
  normalize(out, length);
}
} // namespace

uint16_t theoryLengthTicks(const TheoryPlan& plan) { return (uint16_t)(plan.nChords * plan.chordSteps * kStep); }
int theoryChordAt(const TheoryPlan& plan, int tick) {
  const int span = plan.chordSteps * kStep;
  if (span <= 0 || plan.nChords == 0) return 0;
  int c = (tick < 0 ? 0 : tick) / span;
  return c % plan.nChords;
}
uint8_t theoryChordCode(const TheoryPlan& plan, int chord) { const Prog& pr = progOf(plan); return pr.chords[chord < 0 ? 0 : (chord < kMaxChords ? chord : kMaxChords - 1)]; }

// The chord's pitch classes: the diatonic stack above its degree, the third replaced for a sus chord, raised for the
// borrowed major (V / IV), the root raised for vii°. Stack offsets are taken modulo the scale (7 = the root an octave up,
// 8 = the ninth), so an octave offset carries the root's alteration too (vii° voiced {0,2,4,7} is G# B D G#, not G# B D G)
// and then merges into the root as a duplicate class.
int theoryChordPcs(int root, uint8_t code, const uint8_t* stack, int n, uint8_t* pcs) {
  static const uint8_t kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
  const int deg = code & 7, alt = (code >> 3) & 7;
  int count = 0;
  for (int i = 0; i < n; i++) {
    const int o = stack[i] % 7;
    int off = o;
    if (alt == AltSus4 && o == 2) off = 3;
    else if (alt == AltSus2 && o == 2) off = 1;
    int pc = floorMod12(root + kMinor[(deg + off) % 7]);
    if (alt == AltMajor && o == 2) pc = floorMod12(pc + 1);
    else if (alt == AltRaisedRoot && o == 0) pc = floorMod12(pc + 1);
    bool dup = false; for (int j = 0; j < count; j++) if (pcs[j] == pc) dup = true;
    if (!dup) pcs[count++] = (uint8_t)pc;
  }
  return count;
}

uint16_t theoryAllowedPcs(int root, uint8_t code) {
  static const uint8_t kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
  uint16_t m = 0;
  for (int i = 0; i < 7; i++) m = (uint16_t)(m | (1u << floorMod12(root + kMinor[i])));
  const int deg = code & 7, alt = (code >> 3) & 7;
  if (alt == AltMajor) m = (uint16_t)(m | (1u << floorMod12(root + kMinor[(deg + 2) % 7] + 1)));
  if (alt == AltRaisedRoot) m = (uint16_t)(m | (1u << floorMod12(root + kMinor[deg] + 1)));
  return m;
}

int theoryProgName(const TheoryPlan& plan, char* out, int cap) {
  static const char* const kRoman[7] = {"i", "ii", "III", "iv", "v", "VI", "VII"};
  if (!out || cap <= 0) return 0;
  const Prog& pr = progOf(plan);
  int n = 0; out[0] = 0;
  for (int c = 0; c < pr.n && c < kMaxChords; c++) {
    const int deg = pr.chords[c] & 7, alt = (pr.chords[c] >> 3) & 7;
    char tok[12]; int t = 0;
    for (const char* s = kRoman[deg]; *s && t < 8; s++) {
      char ch = *s;
      if (alt == AltMajor) ch = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;              // V, IV
      else if (alt == AltRaisedRoot) ch = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;    // vii
      tok[t++] = ch;
    }
    tok[t] = 0;
    const char* suffix = deg == 1 || alt == AltRaisedRoot ? "dim" : alt == AltSus4 ? "sus4" : alt == AltSus2 ? "sus2" : "";
    for (const char* s = suffix; *s && t < 11; s++) tok[t++] = *s;
    tok[t] = 0;
    if (c > 0 && n < cap - 1) out[n++] = '-';
    for (int i = 0; tok[i] && n < cap - 1; i++) out[n++] = tok[i];
    out[n] = 0;
  }
  return n;
}

// Every choice from the generator's Rng in a fixed order (SPEC §9.23: the recorder re-seeds, the plan replays), then the
// pad voicings chord by chord with the least voice movement.
void theoryPlan(Rng& rng, int keyRoot, TheoryPlan& plan) {
  plan.root = (uint8_t)floorMod12(keyRoot);
  plan.prog = (uint8_t)pickW(rng, kProgW, kProgCount);
  const Prog& pr = kProgs[plan.prog];
  plan.nChords = pr.n; plan.chordSteps = (uint8_t)(pr.beatsPerChord * 4);
  plan.voicingTpl = (uint8_t)pickW(rng, kVoicingW, kVoicingCount);
  plan.chordRhythm = (uint8_t)pickW(rng, kChordRhythmW, kChordRhythmCount);
  static const uint8_t kStyleW[3] = {50, 25, 25}; plan.chordStyle = (uint8_t)pickW(rng, kStyleW, 3);
  plan.chordLow = (uint8_t)(rng.unit() < 0.30f ? 1 : 0);
  plan.bassTpl = (uint8_t)pickW(rng, kBassTplW, kBassTplCount);
  static const uint8_t kGateW[3] = {35, 40, 25}; plan.bassGate = (uint8_t)pickW(rng, kGateW, 3);
  plan.leadRhythmA = (uint8_t)pickW(rng, kLeadRhythmW, kLeadRhythmCount);
  plan.leadRhythmB = (uint8_t)pickW(rng, kLeadRhythmW, kLeadRhythmCount);
  if (plan.leadRhythmB == plan.leadRhythmA) plan.leadRhythmB = (uint8_t)((plan.leadRhythmA + 1 + rng.range(0, kLeadRhythmCount - 2)) % kLeadRhythmCount);
  plan.leadVar = (uint8_t)pickW(rng, kLeadVarW, kLeadVarCount);
  static const uint8_t kLeadGateW[3] = {45, 25, 30}; plan.leadGate = (uint8_t)pickW(rng, kLeadGateW, 3);
  static const uint8_t kPadW[3] = {60, 25, 15}; plan.padStyle = (uint8_t)pickW(rng, kPadW, 3);
  plan.fxStyle = (uint8_t)rng.range(0, 2);
  plan.seed = rng.next();
  const Voicing& vt = kVoicings[plan.voicingTpl];
  uint8_t prev[4] = {0, 0, 0, 0}; int prevN = 0;
  for (int c = 0; c < kMaxChords; c++) {
    if (c >= plan.nChords) { for (int i = 0; i < 4; i++) plan.voicing[c][i] = 0; continue; }
    uint8_t pcs[4]; const int n = theoryChordPcs(plan.root, pr.chords[c], vt.stack, vt.n, pcs);
    voiceChord(pcs, n, vt.open != 0, prev, prevN, rng, plan.voicing[c]);
    for (int i = 0; i < 4; i++) prev[i] = plan.voicing[c][i];
    prevN = voiceCount(plan, c);
  }
}

void theoryBuild(const TheoryPlan& plan, Category c, Pattern& out) {
  switch (c) {
    case Chord: { Rng rng(mix32(plan.seed ^ kTheoryChordSalt)); buildChord(plan, rng, out); break; }
    case Pad: { Rng rng(mix32(plan.seed ^ kTheoryPadSalt)); buildPad(plan, rng, out); break; }
    case Bass: { Rng rng(mix32(plan.seed ^ kTheoryBassSalt)); buildBass(plan, rng, out); break; }
    case Lead: { Rng rng(mix32(plan.seed ^ kTheoryLeadSalt)); buildLead(plan, rng, out); break; }
    case Fx: { Rng rng(mix32(plan.seed ^ kTheoryFxSalt)); buildFx(plan, rng, out); break; }
    default: out.clear(theoryLengthTicks(plan)); break;
  }
}

// build_bank(), :539-585, one slot at a time (SPEC §5 rebuilds variants from source + seed on demand).
const char* buildVariant(Category c, const Pattern& source, int variant, uint32_t seed, const ProjectKey& key, Pattern& out) {
  return buildVariant(c, source, variant, seed, key, out, nullptr);
}
const char* buildVariant(Category c, const Pattern& source, int variant, uint32_t seed, const ProjectKey& key, Pattern& out,
                         const uint8_t foldMap[128]) {
  variant = clampi(variant, 0, kVariants - 1);
  Rng rng(mix32(seed ^ (kSeedMix * (uint32_t)(variant + 1))));
  static const char* const kTonal[kVariants] = {"Original", "Variation", "2 beats", "Half beat"};
  static const char* const kPad[kVariants] = {"Original", "Variation", "Two voices", "Mono"};
  static const char* const kBeat[kVariants] = {"Kick", "Kick+Hats", "+Snare", "Full"};
  static const char* const kPerc[kVariants] = {"Sparse", "Light", "Driving", "Full"};
  static const char* const kVocal[kVariants] = {"Original", "Alt file", "2 beats", "1 beat"};
  switch (c) {
    case Beat:                                                 // :542-543
      beatLayer(source, variant, rng, out);
      return kBeat[variant];
    case Percussion: {                                         // :544-545, ordinals per original pitch, then the
      uint8_t computed[128];                                   // SPEC §9.7 fold map stored at load (or of the source)
      if (!foldMap) { percussionFoldMap(source, computed); foldMap = computed; }
      percussionLayer(source, variant, out);
      applyPercussionFold(out, foldMap);
      return kPerc[variant];
    }
    case Vocal:                                                // SPEC §9.3/§9.4: the engine streams the file and derives
      if (variant >= 2) out.clear((uint16_t)(variant == 2 ? 2 * kQn : kQn));   // the loop from lengthTicks: 2 / 1 beats
      else original(source, out);                              // full phrase / alternate file keep the source length
      return kVocal[variant];
    case Pad:                                                  // :549-559
      if (variant == 0) original(source, out);
      else if (variant == 1) chordVariation(source, rng, key, out);
      else limitPolyphony(source, variant == 2 ? 2 : 1, out);
      return kPad[variant];
    case Fx:                                                   // :560-570
      if (variant == 0) original(source, out);
      else if (variant == 1) mutatePattern(source, 0.55f, rng, key, out);
      else generatedFx(source, (uint16_t)(variant == 2 ? 2 * kQn : kQn / 2), rng, key, out);
      return kTonal[variant];
    default:                                                   // Chord, Lead, Bass and unknown, :576-584
      if (variant == 0) original(source, out);
      else if (variant == 1) { if (c == Chord) chordVariation(source, rng, key, out); else leadVariation(source, rng, key, out); }
      else cropPattern(source, (uint16_t)(variant == 2 ? 2 * kQn : kQn / 2), out);
      return kTonal[variant];
  }
}

} // namespace lp
