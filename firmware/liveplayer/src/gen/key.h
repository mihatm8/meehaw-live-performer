// Musical key logic ported from key.lua (SPEC §2): natural-minor project keys, library label parsing,
// acceptance with relative-major equivalence, circle-of-fifths cycling, scale snapping.
#pragma once
#include <stdint.h>
namespace lp {
struct LibraryKey { uint8_t root = 0; bool minor = false; bool valid = false; bool keyless = false; };
LibraryKey parseLibraryLabel(const char* label);        // "Amin", "C", "F#min", "A#maj", "NA"
LibraryKey parseLibraryFilename(const char* name);      // text before the first "__"
struct ProjectKey {
  uint8_t root = 9;                                     // natural minor root (A)
  bool contains(int pitch) const;
  bool accepts(const LibraryKey& k) const;              // same pitch classes (minor root or relative major) or keyless
  int snap(int pitch) const;                            // nearest in-scale pitch (down first), key.lua:87
  int moveScaleSteps(int pitch, int steps) const;       // key.lua:106
  int degreePitch(int degree, int octave) const;        // degree 0..6 of the minor scale in MIDI octave
  const char* rootName() const;                         // "A", "A#", ...
  void labels(char minorLabel[8], char majorLabel[8]) const;   // "Amin", "C"
};
int cycleMinorRoot(int root, int direction);            // circle of fifths order A E B F# C# G# D# A# F C G D
const char* noteName(int root);
}
