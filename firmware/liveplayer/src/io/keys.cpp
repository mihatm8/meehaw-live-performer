// meehaw live performer — key matrix diffing and the logical key map (SPEC §3).
#include "keys.h"
#include "../core/types.h"

namespace lp {

namespace {

inline bool emit(KeyEvent* out, int cap, int& n, int x, int y, KeyEvent::Kind kind, uint32_t ms, uint32_t held) {
  if (n >= cap) return false;
  KeyEvent& e = out[n++];
  e.x = (uint8_t)x; e.y = (uint8_t)y; e.kind = kind; e.ms = ms; e.heldMs = held;
  return true;
}

} // namespace

// Diffs scan_ (this scan) against down_ (previous state). A key whose event does not fit in out keeps
// its previous state so the event is produced by the next scan instead of being lost.
//
// Event order within one scan is fixed so the UI's modifier state (derived from KShift events) is
// always right for the other events of the scan: the physical SHIFT (kKeyCols-1, kKeyRows-1) is
// pressed before everything else and released after everything else, and releases come before
// presses (note-off before note-on, as the sequencer does per tick, SPEC §9.12). A SHIFT + track
// chord that lands in a single 10-40 ms scan therefore selects the track (SPEC §3) instead of
// toggling it, and an ambiguous same-scan SHIFT release + key press resolves to the shifted,
// non-launching action.
int KeyMatrix::endScan(uint32_t nowMs, KeyEvent* out, int cap) {
  int n = 0;
  // One key: emits its press (or repeat) when wantPress, else its release; commits state only on emit.
  auto key = [&](int x, int y, bool wantPress) {
    const uint16_t bit = (uint16_t)(1u << x);
    const bool now = (scan_[y] & bit) != 0, was = (down_[y] & bit) != 0;
    if (now && !was) {                                           // new press
      if (!wantPress || !emit(out, cap, n, x, y, KeyEvent::Press, nowMs, 0)) return;
      downAt_[y][x] = nowMs;
      nextRepeat_[y][x] = nowMs + kKeyRepeatDelayMs;            // SPEC §3: hold repeats after 350 ms
      down_[y] |= bit;
    } else if (!now && was) {                                    // release
      if (wantPress || !emit(out, cap, n, x, y, KeyEvent::Release, nowMs, nowMs - downAt_[y][x])) return;
      down_[y] &= (uint16_t)~bit;
    } else if (now && was && wantPress && (int32_t)(nowMs - nextRepeat_[y][x]) >= 0) {
      if (!emit(out, cap, n, x, y, KeyEvent::Repeat, nowMs, nowMs - downAt_[y][x])) return;
      nextRepeat_[y][x] += kKeyRepeatIntervalMs;                 // ... then every 80 ms, on schedule
      if ((int32_t)(nextRepeat_[y][x] - nowMs) <= 0) nextRepeat_[y][x] = nowMs + kKeyRepeatIntervalMs;  // stalled scans: one repeat, no burst
    }
  };
  const int sx = kKeyCols - 1, sy = kKeyRows - 1;               // physical SHIFT = space (13,3)
  const uint16_t sbit = (uint16_t)(1u << sx);
  if ((scan_[sy] & sbit) && !(down_[sy] & sbit)) key(sx, sy, true);   // 1. SHIFT press (its repeats stay in pass 3)
  for (int pass = 0; pass < 2; pass++) {                         // 2. releases, 3. presses and repeats
    const bool presses = pass == 1;
    for (int y = 0; y < kKeyRows; y++) {
      if (!(scan_[y] ^ down_[y]) && !(presses && (scan_[y] & down_[y]))) continue;   // nothing to do in this row
      for (int x = 0; x < kKeyCols; x++) {
        if (x == sx && y == sy && !presses) continue;            // SHIFT releases last; its repeats are ordinary
        key(x, y, presses);
      }
    }
  }
  key(sx, sy, false);                                            // 4. SHIFT release
  return n;
}

int KeyMatrix::downCount() const {
  int c = 0;
  for (int y = 0; y < kKeyRows; y++) for (uint16_t b = down_[y]; b; b &= (uint16_t)(b - 1)) c++;
  return c;
}

namespace {

// SPEC §3 keyboard map, rows y0..y3, columns x0..x13. Lives in flash.
constexpr KeyMapping M(KeyFn f, uint8_t a = 0, uint8_t b = 0) { return KeyMapping{f, a, b}; }
const KeyMapping kKeyMap[kKeyRows][kKeyCols] = {
  // y0: set 1 tracks CHD LED PAD BAS FX PRC VOC BEAT | pat1..pat4 | snare rush | kick cut
  { M(KTrack, 0), M(KTrack, 1), M(KTrack, 2), M(KTrack, 3), M(KTrack, 4), M(KTrack, 5), M(KTrack, 6), M(KTrack, 7),
    M(KVariant, 0, 0), M(KVariant, 0, 1), M(KVariant, 0, 2), M(KVariant, 0, 3), M(KSnareRush, 0), M(KKickCut, 0) },
  // y1: set 2, same layout, tracks 8..15
  { M(KTrack, 8), M(KTrack, 9), M(KTrack, 10), M(KTrack, 11), M(KTrack, 12), M(KTrack, 13), M(KTrack, 14), M(KTrack, 15),
    M(KVariant, 1, 0), M(KVariant, 1, 1), M(KVariant, 1, 2), M(KVariant, 1, 3), M(KSnareRush, 1), M(KKickCut, 1) },
  // y2: vol+ rev+ echo+ dist+ filt+ len+ tempo+ key+ | gyro fx | genre | save S1 | load S1 | gen pattern | GENERATE S1
  { M(KParamUp, PVolume), M(KParamUp, PReverb), M(KParamUp, PEcho), M(KParamUp, PDist),
    M(KParamUp, PFilter), M(KParamUp, PLength), M(KParamUp, PTempo), M(KParamUp, PKey),
    M(KGyro), M(KGenre), M(KSaveSet, 0), M(KLoadSet, 0), M(KGenPattern), M(KGenSetAll) },
  // y3: vol- rev- echo- dist- filt- len- tempo- key- | edit | perform | save S2 | load S2 | gen sound | SHIFT (space)
  { M(KParamDown, PVolume), M(KParamDown, PReverb), M(KParamDown, PEcho), M(KParamDown, PDist),
    M(KParamDown, PFilter), M(KParamDown, PLength), M(KParamDown, PTempo), M(KParamDown, PKey),
    M(KEdit), M(KPerform), M(KSaveSet, 1), M(KLoadSet, 1), M(KGenSound), M(KShift) },
};

} // namespace

KeyMapping mapKey(int x, int y) {
  if (x < 0 || x >= kKeyCols || y < 0 || y >= kKeyRows) return M(KNone);
  return kKeyMap[y][x];
}

} // namespace lp
