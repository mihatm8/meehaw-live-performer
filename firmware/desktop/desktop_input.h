// meehaw live performer — desktop input models (SPEC §3, §7): the US-layout scancode → 4x14 matrix table that
// mirrors the Cardputer legend, and the accelerometer simulated from the arrow keys. This header pulls in no
// SDL header (tests/run_link_all.sh compiles every header standalone); the scancode table lives in
// desktop_input.cpp and is reached through plain ints / the SDL_GetKeyboardState byte array.
#pragma once
#include <stdint.h>
#include <math.h>
#include "../liveplayer/src/core/config.h"
#include "../liveplayer/src/io/keys.h"
#include "../liveplayer/src/ui/ui.h"

namespace lp {

// Row y, column x of the physical matrix (SPEC §3):
//   y0: `  1 2 3 4 5 6 7 8 9 0 - = del      y1: tab q w e r t y u i o p [ ] \
//   y2: fn sft a s d f g h j k l ; ' ent     y3: ctl opt alt z x c v b n m , . / spc
// fn = F1, sft = left shift, ctl = left ctrl, opt = left alt, alt = right alt, del = backspace, spc = space.
int matrixScancode(int x, int y);                          // SDL_Scancode as int; SDL_SCANCODE_UNKNOWN (0) outside the matrix
int btnAScancode();                                        // side button BtnA = F12 (master play/stop, SPEC §3)
// Marks every matrix key whose scancode is down in ks (SDL_GetKeyboardState) on the KeyMatrix scan; an ISO
// keyboard's key left of 1 counts as `.
void scanSdlKeyboard(const uint8_t* ks, KeyMatrix& keys);
// Arrow-key state from the same array: bit 0 up, 1 down, 2 left, 3 right.
int arrowBits(const uint8_t* ks);

// Accelerometer from the arrow keys: up/down = pitch ±kTilt, left/right = roll ±kTilt, released = neutral.
// kTilt lies beyond the UI's dead band plus range (ui.h kGyroDeadDeg 10° + kGyroRangeDeg 45°: full effect at
// 55°), so a held arrow reaches the maximum of the SPEC §3 gyro effects (high-pass / low-pass, reverb and echo
// sends). The angles slew at kRateDegPerS (a hand tilting, not a step) and the gyro reports that rate. Gravity
// in g: flat = (0, 0, 1); pitch moves it onto x and roll onto y, which is how ui.cpp reads it back
// (pitch = atan2f(ax, az), roll = atan2f(ay, az), relative to the neutral captured when gyro fx turns on).
struct ImuSim {
  static constexpr float kTilt = 60.0f, kRateDegPerS = 240.0f;
  float pitch = 0, roll = 0;                                 // degrees
  static float slew(float cur, float target, float step, float dt, float& rate) {
    float d = target - cur;
    if (fabsf(d) <= step) { rate = dt > 0 ? d / dt : 0; return target; }
    rate = d > 0 ? kRateDegPerS : -kRateDegPerS;
    return cur + (d > 0 ? step : -step);
  }
  ImuReading sample(bool up, bool down, bool left, bool right, float dtSec) {
    float tp = up ? kTilt : down ? -kTilt : 0.0f, tr = right ? kTilt : left ? -kTilt : 0.0f;
    float step = kRateDegPerS * dtSec, gp = 0, gr = 0;
    pitch = slew(pitch, tp, step, dtSec, gp); roll = slew(roll, tr, step, dtSec, gr);
    const float d = 3.14159265f / 180.0f;
    ImuReading r;
    r.ax = sinf(pitch * d); r.ay = sinf(roll * d) * cosf(pitch * d); r.az = cosf(pitch * d) * cosf(roll * d);
    r.gx = gr; r.gy = gp; r.gz = 0; r.valid = true;
    return r;
  }
  ImuReading sample(const uint8_t* ks, float dtSec) {       // ks = SDL_GetKeyboardState
    int a = arrowBits(ks);
    return sample((a & 1) != 0, (a & 2) != 0, (a & 4) != 0, (a & 8) != 0, dtSec);
  }
};

} // namespace lp
