// meehaw live performer — IGfx over SDL2 (SPEC §7). See sdl_gfx.h.
#include "sdl_gfx.h"
#include "font5x7.h"
#include <SDL.h>
#include <string.h>

namespace lp {

namespace {
// RGB565 -> 8-bit channels (full range: 0x1F -> 255, 0x3F -> 255).
inline uint8_t r8(Color c) { return (uint8_t)((((c >> 11) & 0x1F) * 255 + 15) / 31); }
inline uint8_t g8(Color c) { return (uint8_t)((((c >> 5) & 0x3F) * 255 + 31) / 63); }
inline uint8_t b8(Color c) { return (uint8_t)(((c & 0x1F) * 255 + 15) / 31); }
} // namespace

bool SdlGfx::begin(bool headless, const char* title) {
  end();
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");          // nearest-neighbour: crisp x3 pixels
  if (headless) {
    surf_ = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf_) return false;
    ren_ = SDL_CreateSoftwareRenderer(surf_);
  } else {
    win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kW * kScale, kH * kScale, SDL_WINDOW_SHOWN);
    if (!win_) return false;
    ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE | SDL_RENDERER_TARGETTEXTURE);
    if (!ren_) return false;
    tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, kW, kH);
    if (!tex_ || SDL_SetRenderTarget(ren_, tex_) != 0) { end(); return false; }
  }
  if (!ren_) { end(); return false; }
  SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
  fillScreen(0);
  return true;
}

void SdlGfx::end() {
  if (tex_) SDL_DestroyTexture(tex_);
  if (ren_) SDL_DestroyRenderer(ren_);
  if (win_) SDL_DestroyWindow(win_);
  if (surf_) SDL_FreeSurface(surf_);
  tex_ = nullptr; ren_ = nullptr; win_ = nullptr; surf_ = nullptr; pending_ = false; flipped_ = false;
}

void SdlGfx::color(Color c) { SDL_SetRenderDrawColor(ren_, r8(c), g8(c), b8(c), 255); }

void SdlGfx::fillScreen(Color c) {
  if (!ren_) return;
  calls_++; color(c); SDL_RenderClear(ren_);
}

void SdlGfx::fillRect(int x, int y, int w, int h, Color c) {
  if (!ren_ || w <= 0 || h <= 0) return;
  calls_++; color(c);
  SDL_Rect r = {x, y, w, h}; SDL_RenderFillRect(ren_, &r);
}

void SdlGfx::drawRect(int x, int y, int w, int h, Color c) {
  if (!ren_ || w <= 0 || h <= 0) return;
  calls_++; color(c);
  SDL_Rect r = {x, y, w, h}; SDL_RenderDrawRect(ren_, &r);
}

void SdlGfx::drawLine(int x0, int y0, int x1, int y1, Color c) {
  if (!ren_) return;
  calls_++; color(c); SDL_RenderDrawLine(ren_, x0, y0, x1, y1);
}

// Each set font pixel becomes a size x size square; the cell advances kFontCellW x size (6 px at size 1).
void SdlGfx::drawText(int x, int y, const char* text, int size, Color c) {
  if (!ren_ || !text) return;
  if (size < 1) size = 1;
  calls_++; color(c);
  SDL_Rect px[kFontCols * kFontRows];
  for (const char* p = text; *p; p++, x += kFontCellW * size) {
    if (x >= kW || y >= kH) break;
    const uint8_t* g = fontGlyph(*p); int n = 0;
    for (int col = 0; col < kFontCols; col++)
      for (int row = 0; row < kFontRows; row++)
        if ((g[col] >> row) & 1) px[n++] = SDL_Rect{x + col * size, y + row * size, size, size};
    if (n) SDL_RenderFillRects(ren_, px, n);
  }
}

int SdlGfx::textWidth(const char* text, int size) {
  if (!text) return 0;
  if (size < 1) size = 1;
  return (int)strlen(text) * kFontCellW * size;
}

// Hands the frame to the window (headless: the surface already holds it, nothing to copy) and restarts the
// kMinFlipMs interval on SDL's clock.
void SdlGfx::flip() {
  if (win_ && ren_ && tex_) {
    SDL_SetRenderTarget(ren_, nullptr);
    SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
    SDL_RenderPresent(ren_);
    SDL_SetRenderTarget(ren_, tex_);
  }
  lastFlip_ = SDL_GetTicks(); flipped_ = true; pending_ = false; flips_++;
}

void SdlGfx::present() {
  if (!ren_) return;
  if (!flipped_ || SDL_GetTicks() - lastFlip_ >= kMinFlipMs) flip(); else pending_ = true;
}

// Same clock as present(): a deferred frame flips once kMinFlipMs have passed since the last flip. (Comparing a
// caller-supplied time against lastFlip_ would wrap whenever the caller's epoch differs from SDL_GetTicks().)
void SdlGfx::pump() {
  if (pending_ && SDL_GetTicks() - lastFlip_ >= kMinFlipMs) flip();
}

SDL_Surface* SdlGfx::readFrame() {
  if (!ren_) return nullptr;
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, kW, kH, 32, SDL_PIXELFORMAT_ARGB8888);
  if (!s) return nullptr;
  if (surf_) { SDL_BlitSurface(surf_, nullptr, s, nullptr); return s; }
  if (SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) != 0) { SDL_FreeSurface(s); return nullptr; }
  return s;
}

bool SdlGfx::saveBmp(const char* path) {
  SDL_Surface* s = readFrame();
  if (!s) return false;
  SDL_Surface* bgr = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_BGR24, 0);   // plain 24-bit BMP: every viewer opens it
  bool ok = bgr && SDL_SaveBMP(bgr, path) == 0;
  if (bgr) SDL_FreeSurface(bgr);
  SDL_FreeSurface(s);
  return ok;
}

uint32_t SdlGfx::pixel(int x, int y) {
  if (x < 0 || y < 0 || x >= kW || y >= kH) return 0;
  SDL_Surface* s = readFrame();
  if (!s) return 0;
  uint32_t v = ((const uint32_t*)((const uint8_t*)s->pixels + y * s->pitch))[x] & 0x00FFFFFF;
  SDL_FreeSurface(s);
  return v;
}

uint32_t SdlGfx::litPixels() {
  SDL_Surface* s = readFrame();
  if (!s) return 0;
  uint32_t lit = 0;
  for (int y = 0; y < kH; y++) {
    const uint32_t* row = (const uint32_t*)((const uint8_t*)s->pixels + y * s->pitch);
    for (int x = 0; x < kW; x++) if (row[x] & 0x00FFFFFF) lit++;
  }
  SDL_FreeSurface(s);
  return lit;
}

} // namespace lp
