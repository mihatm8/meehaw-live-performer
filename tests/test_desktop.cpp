// Host tests for firmware/desktop: the SDL IGfx (headless surface: rects, lines, text, colours, clipping, frame
// dumps, the present() / pump() flip rate limit), the 5x7 font table, the US-layout scancode → 4x14 matrix table
// (SPEC §3 legend, fed through the real KeyMatrix and key map), the arrow-key accelerometer model (and that a held
// arrow saturates the UI's gyro mapping) and the selftest helpers of selftest.h (the scripted checks, the verdict,
// SECONDS parsing, the frame path). The scripted end-to-end session is `liveplayer_desktop --selftest N`
// (tests/run_desktop.sh runs it after this binary, together with the argument guard).
#include "check.h"
#include <SDL.h>
#include <string.h>
#include <sys/stat.h>
#include "../firmware/desktop/sdl_gfx.h"
#include "../firmware/desktop/font5x7.h"
#include "../firmware/desktop/desktop_input.h"
#include "../firmware/desktop/selftest.h"
#include "io/keys.h"

using namespace lp;

namespace {

int glyphBits(const uint8_t* g) { int n = 0; for (int c = 0; c < kFontCols; c++) for (int r = 0; r < 8; r++) n += (g[c] >> r) & 1; return n; }

struct Frame {                                         // a fresh headless gfx per test
  SdlGfx g;
  Frame() { CHECK(g.begin(true, "test")); }
  ~Frame() { g.end(); }
};

// Fake SDL_GetKeyboardState array with the given scancodes down.
struct Keys {
  Uint8 ks[SDL_NUM_SCANCODES];
  Keys() { memset(ks, 0, sizeof ks); }
  Keys& down(SDL_Scancode sc) { ks[sc] = 1; return *this; }
};

} // namespace

// ---- font ---------------------------------------------------------------------------------------------

TEST(font_table_shape) {
  CHECK_EQ(kFontLast - kFontFirst + 1, 95);
  for (int i = 0; i < 95; i++) for (int c = 0; c < kFontCols; c++) CHECK((kFont5x7[i][c] & 0x80) == 0);   // 7 rows only
  CHECK_EQ(glyphBits(fontGlyph(' ')), 0);
  CHECK_EQ(glyphBits(fontGlyph('A')), 18);
  CHECK_EQ(glyphBits(fontGlyph('|')), 7);
  CHECK_EQ(glyphBits(fontGlyph('_')), 5);
  CHECK(fontGlyph((char)0x80) == fontGlyph('?'));      // outside the table draws '?'
  CHECK(fontGlyph('\n') == fontGlyph('?'));
  int empty = 0; for (int i = 1; i < 95; i++) if (glyphBits(kFont5x7[i]) == 0) empty++;
  CHECK_EQ(empty, 0);                                  // every printable glyph has pixels
}

// ---- gfx ----------------------------------------------------------------------------------------------

TEST(gfx_begin_and_clear) {
  Frame f;
  CHECK_EQ(f.g.width(), 240); CHECK_EQ(f.g.height(), 135);
  CHECK_EQ(f.g.litPixels(), 0u);
  f.g.fillScreen(kWhite);
  CHECK_EQ(f.g.litPixels(), 240u * 135u);
  f.g.fillScreen(0);
  CHECK_EQ(f.g.litPixels(), 0u);
  f.g.present();                                       // headless: only bookkeeping, must not crash
  f.g.pump();
}

// present() flips at most every kMinFlipMs and leaves a frame pending for pump(); both read SDL_GetTicks(), so a
// pump() a moment later (whatever clock the loop keeps) does not flip early, and one after the interval does.
TEST(gfx_present_rate_limit) {
  Frame f;
  CHECK_EQ(f.g.flips(), 0u);
  f.g.present(); CHECK_EQ(f.g.flips(), 1u);            // the first frame always flips
  f.g.present(); CHECK_EQ(f.g.flips(), 1u);            // a second within kMinFlipMs is deferred ...
  f.g.pump();    CHECK_EQ(f.g.flips(), 1u);            // ... and pump() does not flip it early
  SDL_Delay(SdlGfx::kMinFlipMs + 2);
  f.g.pump();    CHECK_EQ(f.g.flips(), 2u);            // after the interval pump() flips the deferred frame
  f.g.pump();    CHECK_EQ(f.g.flips(), 2u);            // nothing pending any more
  SDL_Delay(SdlGfx::kMinFlipMs + 2);
  f.g.present(); CHECK_EQ(f.g.flips(), 3u);            // present() after the interval flips directly
  f.g.present(); f.g.present(); f.g.present();
  CHECK_EQ(f.g.flips(), 3u);                           // a burst of presents coalesces into one pending frame
  SDL_Delay(SdlGfx::kMinFlipMs + 2);
  f.g.pump();    CHECK_EQ(f.g.flips(), 4u);
}

TEST(gfx_colors_rgb565) {
  Frame f;
  f.g.fillRect(0, 0, 1, 1, rgb(255, 255, 255)); CHECK_EQ(f.g.pixel(0, 0), 0xFFFFFFu);
  f.g.fillRect(1, 0, 1, 1, rgb(255, 0, 0));     CHECK_EQ(f.g.pixel(1, 0), 0xFF0000u);
  f.g.fillRect(2, 0, 1, 1, rgb(0, 255, 0));     CHECK_EQ(f.g.pixel(2, 0), 0x00FF00u);
  f.g.fillRect(3, 0, 1, 1, rgb(0, 0, 255));     CHECK_EQ(f.g.pixel(3, 0), 0x0000FFu);
  f.g.fillRect(4, 0, 1, 1, rgb(0x39, 0xFF, 0x14));   // neon green: 565 quantisation keeps it within one step
  uint32_t p = f.g.pixel(4, 0);
  CHECK((int)((p >> 16) & 0xFF) >= 0x30 && (int)((p >> 16) & 0xFF) <= 0x3A);
  CHECK_EQ((int)((p >> 8) & 0xFF), 0xFF);
  CHECK((int)(p & 0xFF) >= 0x10 && (int)(p & 0xFF) <= 0x18);
  CHECK_EQ(f.g.pixel(-1, 0), 0u); CHECK_EQ(f.g.pixel(240, 0), 0u); CHECK_EQ(f.g.pixel(0, 135), 0u);
}

TEST(gfx_rects_and_lines) {
  Frame f;
  f.g.fillRect(10, 10, 5, 5, kWhite);
  CHECK_EQ(f.g.litPixels(), 25u);
  CHECK_EQ(f.g.pixel(10, 10), 0xFFFFFFu); CHECK_EQ(f.g.pixel(14, 14), 0xFFFFFFu); CHECK_EQ(f.g.pixel(15, 15), 0u);
  f.g.fillScreen(0);
  f.g.drawRect(0, 0, 10, 10, kWhite);                  // outline only, inclusive w x h like M5GFX
  CHECK_EQ(f.g.litPixels(), 36u);
  CHECK_EQ(f.g.pixel(9, 9), 0xFFFFFFu); CHECK_EQ(f.g.pixel(5, 5), 0u); CHECK_EQ(f.g.pixel(10, 10), 0u);
  f.g.fillScreen(0);
  f.g.drawLine(0, 0, 239, 0, kWhite);
  CHECK_EQ(f.g.litPixels(), 240u);
  f.g.fillScreen(0);
  f.g.drawLine(0, 0, 134, 134, kWhite);               // diagonal: one pixel per row
  CHECK_EQ(f.g.litPixels(), 135u);
  f.g.fillScreen(0);
  f.g.drawRect(5, 5, 0, 3, kWhite); f.g.fillRect(5, 5, 3, 0, kWhite); f.g.fillRect(5, 5, -2, 4, kWhite);
  CHECK_EQ(f.g.litPixels(), 0u);                       // degenerate sizes draw nothing
}

TEST(gfx_clipping) {
  Frame f;
  f.g.fillRect(-5, -5, 10, 10, kWhite);                // 5x5 visible corner
  CHECK_EQ(f.g.litPixels(), 25u);
  f.g.fillScreen(0);
  f.g.fillRect(235, 130, 50, 50, kWhite);              // 5x5 at the far corner
  CHECK_EQ(f.g.litPixels(), 25u);
  f.g.fillScreen(0);
  f.g.drawLine(-100, 50, 400, 50, kWhite);
  CHECK_EQ(f.g.litPixels(), 240u);
  f.g.fillScreen(0);
  f.g.drawText(236, 130, "AB", 1, kWhite);             // partly outside: no crash, only the inside pixels
  CHECK(f.g.litPixels() > 0 && f.g.litPixels() < 36u);
  f.g.drawText(300, 0, "zzz", 1, kWhite);
  f.g.drawText(0, 0, nullptr, 1, kWhite);
  f.g.drawText(0, 0, "", 4, kWhite);
}

TEST(gfx_text_metrics_and_pixels) {
  Frame f;
  CHECK_EQ(f.g.textWidth("meehaw", 1), 36);            // 6 px cells at size 1 (ui/gfx.h)
  CHECK_EQ(f.g.textWidth("meehaw", 2), 72);
  CHECK_EQ(f.g.textWidth("meehaw", 3), 108);
  CHECK_EQ(f.g.textWidth("12", 4), 48);
  CHECK_EQ(f.g.textWidth("", 1), 0);
  CHECK_EQ(f.g.textWidth(nullptr, 1), 0);
  CHECK_EQ(f.g.textWidth("x", 0), 6);                  // size < 1 behaves as 1
  f.g.drawText(0, 0, "A", 1, kWhite);
  CHECK_EQ(f.g.litPixels(), 18u);
  CHECK_EQ(f.g.pixel(0, 1), 0xFFFFFFu);                // 'A' column 0 = 0x7E: rows 1..6
  CHECK_EQ(f.g.pixel(0, 0), 0u);
  CHECK_EQ(f.g.pixel(5, 3), 0u);                       // the spacing column stays clear
  f.g.fillScreen(0);
  f.g.drawText(0, 0, "A", 2, kWhite);
  CHECK_EQ(f.g.litPixels(), 72u);                      // every font pixel becomes 2x2
  CHECK_EQ(f.g.pixel(1, 3), 0xFFFFFFu);
  f.g.fillScreen(0);
  f.g.drawText(0, 0, "AA", 1, kWhite);
  CHECK_EQ(f.g.litPixels(), 36u);
  CHECK_EQ(f.g.pixel(6, 1), 0xFFFFFFu);                // second glyph starts at x = 6
  f.g.fillScreen(0);
  f.g.drawText(10, 20, "S1 BEAT 3 +SNARE", 2, rgb(255, 215, 0));
  CHECK(f.g.litPixels() > 300u);
  CHECK_EQ(f.g.pixel(10 + 0, 20 + 0), 0u);             // 'S' = 0x46: row 0 of column 0 is clear
  CHECK_EQ(f.g.pixel(10 + 0, 20 + 2), 0xFFD700u);      // ... rows 1..2 set (size 2: y 22..25)
}

TEST(gfx_frame_dump) {
  Frame f;
  f.g.fillScreen(0);
  for (int t = 0; t < 8; t++) f.g.fillRect(2 + t * 30, 24, 26, 26, rgb(255, 140, 0));
  f.g.drawText(4, 4, "meehaw live performer", 1, rgb(0x39, 0xFF, 0x14));
  f.g.drawText(4, 60, "GEN S1 3/8", 2, kWhite);
  f.g.drawText(4, 90, "124", 4, rgb(0, 255, 255));
  const char* path = "/tmp/lp_test_desktop_frame.bmp";
  CHECK(f.g.saveBmp(path));
  struct stat st; CHECK(stat(path, &st) == 0 && st.st_size > 240 * 135 * 3);
  CHECK(f.g.drawCalls() >= 11u);
}

// ---- keyboard -----------------------------------------------------------------------------------------

TEST(keyboard_table_is_a_permutation_of_56_keys) {
  int seen[SDL_NUM_SCANCODES] = {0};
  for (int y = 0; y < kKeyRows; y++) for (int x = 0; x < kKeyCols; x++) { int sc = matrixScancode(x, y); CHECK(sc > SDL_SCANCODE_UNKNOWN && sc < SDL_NUM_SCANCODES); seen[sc]++; }
  CHECK_EQ(matrixScancode(14, 0), (int)SDL_SCANCODE_UNKNOWN); CHECK_EQ(matrixScancode(0, 4), (int)SDL_SCANCODE_UNKNOWN); CHECK_EQ(matrixScancode(-1, 0), (int)SDL_SCANCODE_UNKNOWN);
  int dup = 0, total = 0;
  for (int i = 0; i < SDL_NUM_SCANCODES; i++) { if (seen[i] > 1) dup++; total += seen[i]; }
  CHECK_EQ(dup, 0); CHECK_EQ(total, 56);
}

TEST(keyboard_legend_positions) {
  CHECK(matrixScancode(0, 0) == (int)SDL_SCANCODE_GRAVE); CHECK(matrixScancode(1, 0) == (int)SDL_SCANCODE_1); CHECK(matrixScancode(10, 0) == (int)SDL_SCANCODE_0);
  CHECK(matrixScancode(11, 0) == (int)SDL_SCANCODE_MINUS); CHECK(matrixScancode(12, 0) == (int)SDL_SCANCODE_EQUALS); CHECK(matrixScancode(13, 0) == (int)SDL_SCANCODE_BACKSPACE);
  CHECK(matrixScancode(0, 1) == (int)SDL_SCANCODE_TAB); CHECK(matrixScancode(1, 1) == (int)SDL_SCANCODE_Q); CHECK(matrixScancode(10, 1) == (int)SDL_SCANCODE_P);
  CHECK(matrixScancode(11, 1) == (int)SDL_SCANCODE_LEFTBRACKET); CHECK(matrixScancode(12, 1) == (int)SDL_SCANCODE_RIGHTBRACKET); CHECK(matrixScancode(13, 1) == (int)SDL_SCANCODE_BACKSLASH);
  CHECK(matrixScancode(0, 2) == (int)SDL_SCANCODE_F1); CHECK(matrixScancode(1, 2) == (int)SDL_SCANCODE_LSHIFT); CHECK(matrixScancode(2, 2) == (int)SDL_SCANCODE_A);
  CHECK(matrixScancode(10, 2) == (int)SDL_SCANCODE_L); CHECK(matrixScancode(11, 2) == (int)SDL_SCANCODE_SEMICOLON); CHECK(matrixScancode(12, 2) == (int)SDL_SCANCODE_APOSTROPHE); CHECK(matrixScancode(13, 2) == (int)SDL_SCANCODE_RETURN);
  CHECK(matrixScancode(0, 3) == (int)SDL_SCANCODE_LCTRL); CHECK(matrixScancode(1, 3) == (int)SDL_SCANCODE_LALT); CHECK(matrixScancode(2, 3) == (int)SDL_SCANCODE_RALT); CHECK(matrixScancode(3, 3) == (int)SDL_SCANCODE_Z);
  CHECK(matrixScancode(9, 3) == (int)SDL_SCANCODE_M); CHECK(matrixScancode(10, 3) == (int)SDL_SCANCODE_COMMA); CHECK(matrixScancode(11, 3) == (int)SDL_SCANCODE_PERIOD);
  CHECK(matrixScancode(12, 3) == (int)SDL_SCANCODE_SLASH); CHECK(matrixScancode(13, 3) == (int)SDL_SCANCODE_SPACE);
  CHECK(btnAScancode() == (int)SDL_SCANCODE_F12);
  // The legend meets the firmware key map (SPEC §3): space = SHIFT, return = GENERATE S1, F1 = vol+, ` = Chord of set 1.
  CHECK_EQ((int)mapKey(13, 3).fn, (int)KShift);
  CHECK_EQ((int)mapKey(13, 2).fn, (int)KGenSetAll);
  CHECK_EQ((int)mapKey(0, 2).fn, (int)KParamUp); CHECK_EQ((int)mapKey(0, 2).a, (int)PVolume);
  CHECK_EQ((int)mapKey(0, 0).fn, (int)KTrack); CHECK_EQ((int)mapKey(0, 0).a, 0);
  CHECK_EQ((int)mapKey(1, 1).fn, (int)KTrack); CHECK_EQ((int)mapKey(1, 1).a, 9);
}

TEST(keyboard_scan_feeds_key_matrix) {
  KeyMatrix km; KeyEvent ev[16];
  Keys k; k.down(SDL_SCANCODE_RETURN).down(SDL_SCANCODE_Q).down(SDL_SCANCODE_SPACE);
  km.beginScan(); scanSdlKeyboard(k.ks, km);
  int n = km.endScan(100, ev, 16);
  CHECK_EQ(n, 3);
  CHECK_EQ((int)ev[0].x, 13); CHECK_EQ((int)ev[0].y, 3);             // SHIFT press first (keys.cpp ordering)
  bool sawRet = false, sawQ = false;
  for (int i = 1; i < n; i++) { if (ev[i].x == 13 && ev[i].y == 2) sawRet = true; if (ev[i].x == 1 && ev[i].y == 1) sawQ = true; CHECK(ev[i].kind == KeyEvent::Press); }
  CHECK(sawRet && sawQ);
  Keys none; km.beginScan(); scanSdlKeyboard(none.ks, km);
  n = km.endScan(150, ev, 16);
  CHECK_EQ(n, 3);
  CHECK_EQ((int)ev[n - 1].x, 13); CHECK_EQ((int)ev[n - 1].y, 3); CHECK(ev[n - 1].kind == KeyEvent::Release);   // SHIFT release last
  Keys iso; iso.down(SDL_SCANCODE_NONUSBACKSLASH);                   // ISO key left of 1 = `
  km.beginScan(); scanSdlKeyboard(iso.ks, km);
  n = km.endScan(200, ev, 16);
  CHECK_EQ(n, 1); CHECK_EQ((int)ev[0].x, 0); CHECK_EQ((int)ev[0].y, 0);
  CHECK_EQ(km.downCount(), 1);
}

// ---- IMU model ----------------------------------------------------------------------------------------

TEST(imu_neutral) {
  ImuSim s;
  ImuReading r = s.sample(false, false, false, false, 0.02f);
  CHECK(r.valid);
  CHECK_NEAR(r.ax, 0.0, 1e-6); CHECK_NEAR(r.ay, 0.0, 1e-6); CHECK_NEAR(r.az, 1.0, 1e-6);
  CHECK_NEAR(r.gx, 0.0, 1e-6); CHECK_NEAR(r.gy, 0.0, 1e-6);
}

TEST(imu_pitch_slews_to_full_tilt) {
  CHECK_NEAR(ImuSim::kTilt, 60.0, 1e-6);
  ImuSim s; ImuReading r;
  r = s.sample(true, false, false, false, 0.02f);                    // first 20 ms: 4.8° of 60°, rate 240°/s
  CHECK_NEAR(s.pitch, 4.8, 1e-4); CHECK_NEAR(r.gy, 240.0, 1e-3); CHECK(r.ax > 0.08f && r.ax < 0.09f);
  for (int i = 0; i < 11; i++) r = s.sample(true, false, false, false, 0.02f);   // 240 ms: 57.6°, still moving
  CHECK_NEAR(s.pitch, 57.6, 1e-3); CHECK_NEAR(r.gy, 240.0, 1e-3);
  r = s.sample(true, false, false, false, 0.02f);                    // 260 ms: the last 2.4° at 120°/s
  CHECK_NEAR(s.pitch, 60.0, 1e-3); CHECK_NEAR(r.gy, 120.0, 1e-2);
  r = s.sample(true, false, false, false, 0.02f);                    // settled
  CHECK_NEAR(r.ax, 0.8660254, 1e-4); CHECK_NEAR(r.ay, 0.0, 1e-6); CHECK_NEAR(r.az, 0.5, 1e-4);
  CHECK_NEAR(r.gy, 0.0, 1e-3);                                        // at rest once settled
  r = s.sample(false, true, false, false, 1.0f);                     // one long step straight to −60°
  CHECK_NEAR(s.pitch, -60.0, 1e-4); CHECK_NEAR(r.ax, -0.8660254, 1e-4);
  r = s.sample(false, false, false, false, 1.0f);                    // released: neutral
  CHECK_NEAR(r.ax, 0.0, 1e-6); CHECK_NEAR(r.az, 1.0, 1e-6);
}

TEST(imu_roll_and_combined_stay_unit_length) {
  ImuSim s; ImuReading r;
  for (int i = 0; i < 13; i++) r = s.sample(false, false, false, true, 0.02f);    // 60° in 13 steps (12 × 4.8 + 2.4)
  CHECK_NEAR(s.roll, 60.0, 1e-3); CHECK_NEAR(r.ay, 0.8660254, 1e-4); CHECK_NEAR(r.ax, 0.0, 1e-6);
  for (int i = 0; i < 10; i++) r = s.sample(false, false, true, false, 0.02f);    // +60 → +12 after 200 ms (240°/s)
  CHECK_NEAR(s.roll, 12.0, 1e-3); CHECK_NEAR(r.ay, 0.2079117, 1e-4);
  for (int i = 0; i < 15; i++) r = s.sample(false, false, true, false, 0.02f);    // ... −60 after 25 steps in all
  CHECK_NEAR(s.roll, -60.0, 1e-3); CHECK_NEAR(r.ay, -0.8660254, 1e-4);
  for (int i = 0; i < 30; i++) r = s.sample(true, false, false, true, 0.02f);     // pitch and roll both at +60°
  CHECK_NEAR(r.ax, 0.8660254, 1e-4); CHECK_NEAR(r.ay, 0.4330127, 1e-4); CHECK_NEAR(r.az, 0.25, 1e-4);
  CHECK_NEAR(r.ax * r.ax + r.ay * r.ay + r.az * r.az, 1.0, 1e-5);
  Keys k; k.down(SDL_SCANCODE_DOWN); ImuSim t;                       // the scancode overload
  for (int i = 0; i < 15; i++) r = t.sample(k.ks, 0.02f);
  CHECK_NEAR(r.ax, -0.8660254, 1e-4);
}

// The UI maps tilt beyond a 10° dead band over 45° (ui.h kGyroDeadDeg / kGyroRangeDeg; ui.cpp mapTilt with
// pitch = atan2f(ax, az), roll = atan2f(ay, az)): a held arrow must reach full scale (SPEC §3 "high-pass up to
// max", "reverb send 0→max"), and the slew passes through partial values on the way.
TEST(imu_full_tilt_saturates_ui_mapping) {
  CHECK(ImuSim::kTilt >= kGyroDeadDeg + kGyroRangeDeg);
  const float kRadToDeg = 180.0f / 3.14159265f;
  ImuSim s; ImuReading r;
  for (int i = 0; i < 6; i++) r = s.sample(true, false, false, false, 0.02f);     // 120 ms: 28.8°, partial
  float pitch = atan2f(r.ax, r.az) * kRadToDeg;
  CHECK_NEAR(pitch, 28.8, 1e-2);
  CHECK_NEAR((pitch - kGyroDeadDeg) / kGyroRangeDeg, 0.4178, 1e-3);
  for (int i = 0; i < 10; i++) r = s.sample(true, false, false, false, 0.02f);    // settled at 60°: beyond full scale
  pitch = atan2f(r.ax, r.az) * kRadToDeg;
  CHECK_NEAR(pitch, 60.0, 1e-2);
  CHECK((pitch - kGyroDeadDeg) / kGyroRangeDeg >= 1.0f);
  ImuSim t;
  for (int i = 0; i < 16; i++) r = t.sample(false, false, true, false, 0.02f);    // roll left: the reverb send to max
  float roll = atan2f(r.ay, r.az) * kRadToDeg;
  CHECK_NEAR(roll, -60.0, 1e-2);
  CHECK((-roll - kGyroDeadDeg) / kGyroRangeDeg >= 1.0f);
}

// ---- selftest helpers (selftest.h) ---------------------------------------------------------------------

// The scripted checks read exactly what the engine publishes (status snapshot + target params) and compare
// with the documented tolerances: exact for states / variants / flags, > for the running ramp, |x| <= eps for
// the snap-back, and a fifth of a step for the volume hold (one repeat more or less must fail).
TEST(selftest_checks_read_published_state) {
  EngineStatus st; TrackParams p;
  st.track[7].state = PSPlaying; st.track[7].variant = 2; st.track[7].rush = 1; st.track[7].kickCut = 1;
  p.filter = 0.1875f; p.volume = 0.5f;
  CHECK_NEAR(checkValue(st, p, CkPlaying, 7), 1.0, 0.0); CHECK_NEAR(checkValue(st, p, CkPlaying, 5), 0.0, 0.0);
  CHECK_NEAR(checkValue(st, p, CkVariant, 7), 2.0, 0.0); CHECK_NEAR(checkValue(st, p, CkVariant, 0), 0.0, 0.0);
  CHECK_NEAR(checkValue(st, p, CkRush, 7), 1.0, 0.0);
  CHECK_NEAR(checkValue(st, p, CkKickCut, 7), 1.0, 0.0);
  CHECK_NEAR(checkValue(st, p, CkFilterAbove, 7), 0.1875, 1e-6); CHECK_NEAR(checkValue(st, p, CkFilterZero, 7), 0.1875, 1e-6);
  CHECK_NEAR(checkValue(st, p, CkVolume, 7), 0.5, 1e-6);
  CHECK_NEAR(checkValue(st, p, CkVariant, 99), 0.0, 0.0);                       // out-of-range track clamps, no crash
  CHECK(checkHolds(CkPlaying, 1, 1)); CHECK(!checkHolds(CkPlaying, 0, 1));
  CHECK(checkHolds(CkVariant, 2, 2)); CHECK(!checkHolds(CkVariant, 1, 2));
  CHECK(checkHolds(CkRush, 0, 0)); CHECK(!checkHolds(CkRush, 1, 0));
  CHECK(checkHolds(CkKickCut, 1, 1)); CHECK(!checkHolds(CkKickCut, 0, 1));
  CHECK(checkHolds(CkFilterAbove, 0.1875f, 0.05f)); CHECK(!checkHolds(CkFilterAbove, 0.0f, 0.05f)); CHECK(!checkHolds(CkFilterAbove, 0.05f, 0.05f));
  CHECK(checkHolds(CkFilterZero, 0.0f, 0.001f)); CHECK(checkHolds(CkFilterZero, -0.0005f, 0.001f)); CHECK(!checkHolds(CkFilterZero, 0.19f, 0.001f));
  CHECK(checkHolds(CkVolume, 0.5f, 0.5f)); CHECK(checkHolds(CkVolume, 0.5000001f, 0.5f));
  CHECK(!checkHolds(CkVolume, 0.55f, 0.5f)); CHECK(!checkHolds(CkVolume, 0.45f, 0.5f));
  CHECK(!strcmp(checkName(CkVariant), "variant")); CHECK(!strcmp(checkName(CkFilterZero), "filter==0")); CHECK(!strcmp(checkName(42), "?"));
}

// The verdict fails on every counter the run collects, in a fixed order, and tolerates only kLateBudget late
// pacer turns (host scheduling noise); render underruns, vocal underruns, a failed check and a blank UI are fatal.
TEST(selftest_verdict) {
  SelftestTally t; t.blocks = 1700; t.soundBlocks = 1200; t.peak = 9000; t.litPixels = 5000; t.scriptFinished = true;
  CHECK(selftestVerdict(t) == nullptr);
  t.late = kLateBudget; CHECK(selftestVerdict(t) == nullptr);
  t.late = kLateBudget + 1; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "late")); t.late = 0;
  t.vocalUnderruns = 1; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "vocal")); t.vocalUnderruns = 0;
  t.litPixels = 0; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "UI drew nothing")); t.litPixels = 5000;
  t.checkFails = 1; snprintf(t.firstCheckFail, sizeof t.firstCheckFail, "check failed: variant[7] expected 1 got 0");
  CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "variant[7]"));
  t.firstCheckFail[0] = 0; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "check")); t.checkFails = 0;
  t.underruns = 1; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "underrun")); t.underruns = 0;
  t.peak = (uint32_t)(kSoundPeak * 32767) - 1; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "no sound")); t.peak = 9000;
  t.soundBlocks = kMinSoundBlocks - 1; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "0.5 s")); t.soundBlocks = 1200;
  t.scriptFinished = false; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "script")); t.scriptFinished = true;
  t.genTimeout = true; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "generation")); t.genTimeout = false;
  t.playTimeout = true; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "start")); t.playTimeout = false;
  t.blocks = 0; CHECK(selftestVerdict(t) && strstr(selftestVerdict(t), "blocks")); t.blocks = 1700;
  CHECK(selftestVerdict(t) == nullptr);
  SelftestTally u = t; u.underruns = 1; u.checkFails = 1; u.litPixels = 0;        // precedence: underruns first ...
  CHECK(strstr(selftestVerdict(u), "underrun"));
  u.underruns = 0; CHECK(strstr(selftestVerdict(u), "check"));                     // ... then the failed check, then the UI
  u.checkFails = 0; CHECK(strstr(selftestVerdict(u), "UI"));
}

// --selftest N / --seconds N accept a whole number >= 1 only, so a headless run always ends.
TEST(selftest_seconds_argument) {
  uint32_t s = 1;
  CHECK(parseSeconds("25", s)); CHECK_EQ(s, 25u);
  CHECK(parseSeconds("1", s)); CHECK_EQ(s, 1u);
  CHECK(parseSeconds("86400", s)); CHECK_EQ(s, 86400u);
  s = 7;
  CHECK(!parseSeconds("0", s)); CHECK(!parseSeconds("-3", s)); CHECK(!parseSeconds("abc", s)); CHECK(!parseSeconds("12x", s));
  CHECK(!parseSeconds("", s)); CHECK(!parseSeconds(nullptr, s)); CHECK(!parseSeconds("86401", s)); CHECK(!parseSeconds("4294967297", s));
  CHECK(!parseSeconds(" ", s)); CHECK(!parseSeconds("1.5", s));
  CHECK_EQ(s, 7u);                                        // a rejected value leaves the output alone
}

// The frame goes next to the binary (SDL_GetBasePath, trailing separator) unless --frame names a path.
TEST(selftest_frame_path) {
  char p[256];
  framePath("/opt/lp/build/", nullptr, p, sizeof p); CHECK(!strcmp(p, "/opt/lp/build/selftest_frame.bmp"));
  framePath("/opt/lp/build/", "/tmp/x.bmp", p, sizeof p); CHECK(!strcmp(p, "/tmp/x.bmp"));
  framePath("/opt/lp/build/", "", p, sizeof p); CHECK(!strcmp(p, "/opt/lp/build/selftest_frame.bmp"));
  framePath(nullptr, nullptr, p, sizeof p); CHECK(!strcmp(p, "selftest_frame.bmp"));
  char tiny[8]; framePath("/opt/lp/build/", nullptr, tiny, sizeof tiny); CHECK_EQ((int)strlen(tiny), 7);   // always terminated
}

int main() {
  SDL_Init(0);
  int rc = lp_test::run();
  SDL_Quit();
  return rc;
}
