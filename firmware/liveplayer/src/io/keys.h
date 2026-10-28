// meehaw live performer — key matrix event model (SPEC §3). The platform feeds the set of pressed
// (x, y) coordinates every scan; KeyMatrix diffs them into press/release/repeat events.
#pragma once
#include <stdint.h>
#include "../core/config.h"

namespace lp {

struct KeyEvent {
  uint8_t x = 0, y = 0;
  enum Kind : uint8_t { Press = 0, Release, Repeat } kind = Press;
  uint32_t ms = 0;             // time of the event
  uint32_t heldMs = 0;         // for Release/Repeat: how long the key has been down
};

class KeyMatrix {
 public:
  void beginScan() { for (int i = 0; i < kKeyRows; i++) scan_[i] = 0; }
  void markPressed(int x, int y) { if (x >= 0 && x < kKeyCols && y >= 0 && y < kKeyRows) scan_[y] |= (1u << x); }
  // Produces events into out (cap). Returns the number written. Repeats fire after kKeyRepeatDelayMs then every kKeyRepeatIntervalMs.
  // Order within a scan: SHIFT (13,3) press, releases, presses/repeats, SHIFT release — so the modifier
  // always brackets the other events of the same scan (a one-scan SHIFT+track chord selects, never toggles).
  int endScan(uint32_t nowMs, KeyEvent* out, int cap);
  bool isDown(int x, int y) const { return (down_[y] >> x) & 1u; }
  uint32_t heldMs(int x, int y, uint32_t nowMs) const { return isDown(x, y) ? nowMs - downAt_[y][x] : 0; }
  int downCount() const;
 private:
  uint16_t scan_[kKeyRows] = {0};
  uint16_t down_[kKeyRows] = {0};
  uint32_t downAt_[kKeyRows][kKeyCols] = {{0}};
  uint32_t nextRepeat_[kKeyRows][kKeyCols] = {{0}};
};

// Logical key map (SPEC §3). Physical SHIFT is (13,3) = space.
enum KeyFn : uint8_t {
  KTrack = 0,        // a = track index 0..15
  KVariant,          // a = set, b = variant 0..3
  KSnareRush,        // a = set
  KKickCut,          // a = set
  KParamUp, KParamDown,   // a = ParamId (vol rev echo dist filter length tempo key)
  KGyro, KGenre, KSaveSet, KLoadSet, KGenPattern, KGenSetAll,
  KEdit, KPerform, KGenSound, KShift,
  KNone,
};
struct KeyMapping { KeyFn fn; uint8_t a; uint8_t b; };
KeyMapping mapKey(int x, int y);

} // namespace lp
