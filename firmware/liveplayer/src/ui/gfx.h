// meehaw live performer — tiny immediate-mode graphics interface (M5GFX on device, SDL on desktop).
#pragma once
#include <stdint.h>

namespace lp {

typedef uint16_t Color;                       // RGB565
constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) { return (Color)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

class IGfx {
 public:
  virtual ~IGfx() {}
  virtual int width() const = 0;              // 240
  virtual int height() const = 0;             // 135
  virtual void startWrite() {}
  virtual void endWrite() {}
  virtual void fillScreen(Color c) = 0;
  virtual void fillRect(int x, int y, int w, int h, Color c) = 0;
  virtual void drawRect(int x, int y, int w, int h, Color c) = 0;
  virtual void drawLine(int x0, int y0, int x1, int y1, Color c) = 0;
  // Built-in font: size 1 = 6x8 px cells, size 2 = 12x16, size 3 = 18x24, size 4 = 24x32.
  virtual void drawText(int x, int y, const char* text, int size, Color c) = 0;
  virtual int textWidth(const char* text, int size) = 0;
  virtual void present() {}                   // no-op on device (direct draw), flips on desktop
};

} // namespace lp
