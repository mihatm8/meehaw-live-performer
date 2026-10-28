// meehaw live performer — placeholder Ui for the desktop build while src/ui/ui.cpp does not exist yet.
// The Makefile compiles this file only when the real one is absent (and defines LP_UI_STUB); it draws
// nothing and ignores keys, so the selftest's fallback driver in main.cpp performs the scripted actions.
#include "../liveplayer/src/ui/ui.h"

namespace lp {

void Ui::init(IGfx* gfx, Engine* eng, Generator* gen, Sets* sets) { gfx_ = gfx; eng_ = eng; gen_ = gen; sets_ = sets; page_ = PgMain; }
void Ui::onKey(const KeyEvent& e, uint32_t nowMs) {}
void Ui::onImu(const ImuReading& r, uint32_t nowMs) {}
void Ui::tick(uint32_t nowMs) {}
void Ui::draw(uint32_t nowMs) {}
void Ui::showStatus(const char* text, uint32_t ms) {}
void Ui::setOutputVolumeHook(void (*fn)(int vol255)) { outVolHook_ = fn; if (fn) fn(outVol_); }
void Ui::setOutputVolume(int vol255) { outVol_ = (uint8_t)(vol255 < 0 ? 0 : vol255 > 255 ? 255 : vol255); if (outVolHook_) outVolHook_(outVol_); }
void Ui::setBattery(int percent, int charging) { batPct_ = (int8_t)(percent < 0 ? -1 : percent > 100 ? 100 : percent); batChg_ = (int8_t)(charging < 0 ? -1 : charging ? 1 : 0); }

Color categoryColor(Category c) {                       // SPEC §4 palette, for anything that links against it
  static const Color k[CategoryCount] = {rgb(255, 140, 0), rgb(255, 215, 0), rgb(155, 89, 255), rgb(181, 101, 29),
                                          rgb(255, 79, 163), rgb(46, 204, 113), rgb(0, 229, 255), rgb(160, 160, 160)};
  return k[c < CategoryCount ? c : 0];
}
Color paramColor(ParamId p) { return rgb(255, 255, 255); }

} // namespace lp
