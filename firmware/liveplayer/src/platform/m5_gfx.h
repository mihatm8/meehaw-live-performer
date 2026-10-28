// IGfx over M5GFX (M5Cardputer.Display), direct drawing, landscape 240x135. Device only.
#pragma once
#ifdef ARDUINO
#include <M5Cardputer.h>
#include "../ui/gfx.h"
namespace lp {
class M5Gfx : public IGfx {
 public:
  void begin() { auto& d = M5Cardputer.Display; d.setRotation(1); d.setTextWrap(false); d.setTextDatum(top_left); d.fillScreen(TFT_BLACK); }
  int width() const override { return 240; }
  int height() const override { return 135; }
  void startWrite() override { M5Cardputer.Display.startWrite(); }
  void endWrite() override { M5Cardputer.Display.endWrite(); }
  void fillScreen(Color c) override { M5Cardputer.Display.fillScreen(c); }
  void fillRect(int x, int y, int w, int h, Color c) override { M5Cardputer.Display.fillRect(x, y, w, h, c); }
  void drawRect(int x, int y, int w, int h, Color c) override { M5Cardputer.Display.drawRect(x, y, w, h, c); }
  void drawLine(int x0, int y0, int x1, int y1, Color c) override { M5Cardputer.Display.drawLine(x0, y0, x1, y1, c); }
  void drawText(int x, int y, const char* text, int size, Color c) override {
    auto& d = M5Cardputer.Display; d.setTextSize(size); d.setTextColor(c, TFT_BLACK); d.drawString(text, x, y);
  }
  int textWidth(const char* text, int size) override { auto& d = M5Cardputer.Display; d.setTextSize(size); return d.textWidth(text); }
};
}
#endif
