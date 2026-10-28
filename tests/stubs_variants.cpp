// Minimal ProjectKey for the variants host test (natural minor only), used until src/gen/key.cpp exists.
// Mirrors key.lua:81-118 (contains / snap / move_scale_steps). run_variants.sh picks key.cpp when present.
#include "gen/key.h"
namespace lp {
namespace {
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
int floorMod(int v, int m) { return ((v % m) + m) % m; }
}
bool ProjectKey::contains(int pitch) const {
  const int iv = floorMod(pitch - root, 12);
  for (int i = 0; i < 7; i++) if (kMinor[i] == iv) return true;
  return false;
}
int ProjectKey::snap(int pitch) const {                       // key.lua:87-103, nearest in-scale pitch, down first
  int clamped = pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch);
  if (contains(clamped)) return clamped;
  for (int d = 1; d <= 6; d++) {
    if (clamped - d >= 0 && contains(clamped - d)) return clamped - d;
    if (clamped + d <= 127 && contains(clamped + d)) return clamped + d;
  }
  return clamped;
}
int ProjectKey::moveScaleSteps(int pitch, int steps) const { // key.lua:106-118
  int result = snap(pitch);
  const int dir = steps < 0 ? -1 : (steps > 0 ? 1 : 0);
  for (int n = 0; n < (steps < 0 ? -steps : steps); n++) {
    int c = result + dir;
    while (c >= 0 && c <= 127 && !contains(c)) c += dir;
    if (c < 0 || c > 127) break;
    result = c;
  }
  return result;
}
int ProjectKey::degreePitch(int degree, int octave) const {
  const int d = floorMod(degree, 7);
  return (octave + 1) * 12 + root + kMinor[d];
}
}
