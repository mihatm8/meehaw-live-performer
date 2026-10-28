// meehaw live performer — IGfx over SDL2 for the desktop build (SPEC §7, §4: 240x135 UI shown at x3).
// The UI draws into a 240x135 render target (a texture behind a 720x405 window, or an offscreen surface when
// headless); present() copies it to the window with nearest-neighbour scaling, at most every kMinFlipMs
// (no vsync wait on the loop task; pump() flips a frame that present() deferred). Both measure the interval
// on SDL_GetTicks(), never on the loop's own clock. Headless, present() and pump() only keep the flip
// bookkeeping (flips()), which is how the rate limit is tested. Text comes from the built-in 5x7 font
// (font5x7.h) so SDL_ttf is not needed. Includes no SDL header: tests/run_link_all.sh compiles
// every header standalone without sdl2-config flags.
#pragma once
#include <stdint.h>
#include "../liveplayer/src/ui/gfx.h"

struct SDL_Window; struct SDL_Renderer; struct SDL_Texture; struct SDL_Surface;   // SDL.h only in sdl_gfx.cpp (header compiles standalone)

namespace lp {

class SdlGfx : public IGfx {
 public:
  static constexpr int kW = 240, kH = 135, kScale = 3;
  static constexpr uint32_t kMinFlipMs = 15;                // ≈ 60 fps cap for the window

  bool begin(bool headless, const char* title);             // window + accelerated renderer, or a software surface
  void end();
  bool ok() const { return ren_ != nullptr; }

  int width() const override { return kW; }
  int height() const override { return kH; }
  void fillScreen(Color c) override;
  void fillRect(int x, int y, int w, int h, Color c) override;
  void drawRect(int x, int y, int w, int h, Color c) override;
  void drawLine(int x0, int y0, int x1, int y1, Color c) override;
  void drawText(int x, int y, const char* text, int size, Color c) override;
  int textWidth(const char* text, int size) override;
  void present() override;                                  // flips now, or defers to pump() when flipping too fast
  void pump();                                              // call every loop iteration: flips a deferred frame

  bool saveBmp(const char* path);                           // the 240x135 frame as drawn so far
  uint32_t litPixels();                                     // non-black pixels in the frame (selftest)
  uint32_t pixel(int x, int y);                             // 0x00RRGGBB of one frame pixel (tests), 0 outside
  uint32_t drawCalls() const { return calls_; }
  uint32_t flips() const { return flips_; }                 // frames handed to the window (headless: counted only)

 private:
  SDL_Window* win_ = nullptr; SDL_Renderer* ren_ = nullptr; SDL_Texture* tex_ = nullptr; SDL_Surface* surf_ = nullptr;
  uint32_t lastFlip_ = 0; bool flipped_ = false; bool pending_ = false; uint32_t calls_ = 0, flips_ = 0;
  void color(Color c);
  void flip();
  SDL_Surface* readFrame();                                 // caller frees (headless: a copy of surf_)
};

} // namespace lp
