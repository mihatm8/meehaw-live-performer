// Musical key logic — port of Scripts/LivePlayer/lib/key.lua (SPEC §2). The project key is always a natural
// minor scale; library labels follow the MusicalKey semantics of key.lua with sharps-only note names.
// Lua line numbers are cited next to each ported rule.
#include "key.h"
#include <string.h>

namespace lp {
namespace {

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};  // key.lua:6
const uint8_t kMinorIntervals[7] = {0, 2, 3, 5, 7, 8, 10};                                          // key.lua:8
const uint8_t kMinorCircle[12] = {9, 4, 11, 6, 1, 8, 3, 10, 5, 0, 7, 2};                              // key.lua:9
constexpr int kLabelCap = 32;                       // longest label we inspect (rules only look at prefixes)

int floorMod(int value, int divisor) { return ((value % divisor) + divisor) % divisor; }              // key.lua:11

int naturalRoot(char letter) {                                                                        // key.lua:29
  switch (letter) {
    case 'C': return 0; case 'D': return 2; case 'E': return 4; case 'F': return 5;
    case 'G': return 7; case 'A': return 9; case 'B': return 11;
    default: return -1;
  }
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool startsWith(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

// Trims surrounding whitespace and uppercases (key.lua:134). Returns false for an empty label.
bool normaliseLabel(const char* label, char* out) {
  while (isSpace(*label)) label++;
  int n = 0;
  while (label[n] && n < kLabelCap - 1) { out[n] = upper(label[n]); n++; }
  while (n > 0 && isSpace(out[n - 1])) n--;
  out[n] = 0;
  return n > 0;
}

bool minorContains(int root, int pitch) {                                                             // key.lua:260
  int pc = floorMod(pitch - root, 12);
  for (uint8_t iv : kMinorIntervals) if (iv == pc) return true;                                       // key.lua:51
  return false;
}

int clampPitch(int p) { return p < 0 ? 0 : (p > 127 ? 127 : p); }

} // namespace

// key.lua:127-167 (Key.from_library_filename applied to the label part). "NA" is reported as keyless instead
// of nil so callers can tell "explicitly keyless" from "unparseable" (key.lua:309).
LibraryKey parseLibraryLabel(const char* label) {
  LibraryKey k;
  char buf[kLabelCap];
  if (!label || !normaliseLabel(label, buf)) return k;
  if (strcmp(buf, "NA") == 0) { k.valid = true; k.keyless = true; return k; }                        // key.lua:135
  int root = naturalRoot(buf[0]);                                                                     // key.lua:139
  if (root < 0) return k;
  const char* rest = buf + 1;                                                                         // key.lua:143
  char accidental = 0;
  if (rest[0] == '#' || rest[0] == 'B') { accidental = rest[0]; rest++; }                            // key.lua:145
  bool minor = false;
  if (startsWith(rest, "MIN")) { minor = true; rest += 3; }                                           // key.lua:152
  else if (startsWith(rest, "MAJ")) { rest += 3; }                                                    // key.lua:155
  if (rest[0] != 0 && !isDigit(rest[0]) && !startsWith(rest, "ADD") && !startsWith(rest, "SUS") &&
      !startsWith(rest, "DIM") && !startsWith(rest, "AUG") && !startsWith(rest, "DOM")) return k;     // key.lua:159
  // parse_note_token(letter .. accidental): the token has two characters exactly when an accidental was
  // captured, so '#' raises and 'B' lowers the natural root (key.lua:43-47).
  if (accidental == '#') root = floorMod(root + 1, 12);
  else if (accidental == 'B') root = floorMod(root - 1, 12);
  k.root = (uint8_t)root;
  k.minor = minor;
  k.valid = true;
  return k;
}

LibraryKey parseLibraryFilename(const char* name) {
  LibraryKey k;
  if (!name) return k;
  const char* base = name;                                                                            // key.lua:22
  for (const char* p = name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  const char* sep = strstr(base, "__");                                                               // key.lua:129
  if (!sep || sep == base) return k;                                                                  // key.lua:130
  int n = (int)(sep - base);
  if (n > kLabelCap - 1) n = kLabelCap - 1;
  char label[kLabelCap];
  memcpy(label, base, (size_t)n);
  label[n] = 0;
  return parseLibraryLabel(label);
}

bool ProjectKey::contains(int pitch) const { return minorContains(root, pitch); }

// key.lua:284-298. A natural minor scale always has a canonical major/minor equivalent (itself and its
// relative major), so the fallback-quality branch of the Lua can never be reached: acceptance is exactly
// "identical pitch-class set" — minor at the root or major a minor third up — plus explicitly keyless files.
bool ProjectKey::accepts(const LibraryKey& k) const {
  if (!k.valid) return false;
  if (k.keyless) return true;
  if (k.minor) return k.root == root;
  return k.root == floorMod(root + 3, 12);
}

int ProjectKey::snap(int pitch) const {                                                               // key.lua:87
  int clamped = clampPitch(pitch);
  if (contains(clamped)) return clamped;
  for (int distance = 1; distance <= 6; distance++) {
    int down = clamped - distance, up = clamped + distance;
    if (down >= 0 && contains(down)) return down;                                                     // key.lua:96
    if (up <= 127 && contains(up)) return up;                                                         // key.lua:99
  }
  return clamped;
}

int ProjectKey::moveScaleSteps(int pitch, int steps) const {                                          // key.lua:106
  int result = snap(pitch);
  int direction = steps < 0 ? -1 : (steps > 0 ? 1 : 0);
  int count = steps < 0 ? -steps : steps;
  for (int i = 0; i < count; i++) {
    int candidate = result + direction;
    while (candidate >= 0 && candidate <= 127 && !contains(candidate)) candidate += direction;        // key.lua:112
    if (candidate < 0 || candidate > 127) break;                                                      // key.lua:115
    result = candidate;
  }
  return result;
}

// MIDI octave convention: octave 3 starts at C3 = 48, so degree 0 of octave 3 lies in [48, 60) and the
// scale ascends from there. Degrees outside 0..6 carry into neighbouring octaves (perform page, SPEC §4).
int ProjectKey::degreePitch(int degree, int octave) const {
  int carry = degree >= 0 ? degree / 7 : -((-degree + 6) / 7);
  int d = floorMod(degree, 7);
  return clampPitch(12 * (octave + carry + 1) + root + kMinorIntervals[d]);
}

const char* ProjectKey::rootName() const { return noteName(root); }

void ProjectKey::labels(char minorLabel[8], char majorLabel[8]) const {
  strcpy(minorLabel, noteName(root));
  strcat(minorLabel, "min");                                                                          // "Amin"
  strcpy(majorLabel, noteName(root + 3));                                                             // relative major "C"
}

int cycleMinorRoot(int root, int direction) {                                                         // key.lua:323
  int normalized = floorMod(root, 12);
  int index = 0;
  for (int i = 0; i < 12; i++) if (kMinorCircle[i] == normalized) { index = i; break; }
  int delta = direction < 0 ? -1 : (direction > 0 ? 1 : 0);                                           // key.lua:333
  return kMinorCircle[floorMod(index + delta, 12)];
}

const char* noteName(int root) { return kNoteNames[floorMod(root, 12)]; }                             // key.lua:318

} // namespace lp
