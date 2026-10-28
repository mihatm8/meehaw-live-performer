// meehaw live performer — the scancode → matrix table of the desktop build (SPEC §3 legend). See desktop_input.h.
#include "desktop_input.h"
#include <SDL.h>

namespace lp {

namespace {
const SDL_Scancode kMatrixScan[kKeyRows][kKeyCols] = {
  { SDL_SCANCODE_GRAVE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE },
  { SDL_SCANCODE_TAB, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U,
    SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_BACKSLASH },
  { SDL_SCANCODE_F1, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H,
    SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_RETURN },
  { SDL_SCANCODE_LCTRL, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B,
    SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_SPACE },
};
} // namespace

int matrixScancode(int x, int y) {
  if (x < 0 || x >= kKeyCols || y < 0 || y >= kKeyRows) return SDL_SCANCODE_UNKNOWN;
  return (int)kMatrixScan[y][x];
}

int btnAScancode() { return SDL_SCANCODE_F12; }

void scanSdlKeyboard(const uint8_t* ks, KeyMatrix& keys) {
  if (!ks) return;
  for (int y = 0; y < kKeyRows; y++)
    for (int x = 0; x < kKeyCols; x++)
      if (ks[kMatrixScan[y][x]]) keys.markPressed(x, y);
  if (ks[SDL_SCANCODE_NONUSBACKSLASH]) keys.markPressed(0, 0);
}

int arrowBits(const uint8_t* ks) {
  if (!ks) return 0;
  return (ks[SDL_SCANCODE_UP] ? 1 : 0) | (ks[SDL_SCANCODE_DOWN] ? 2 : 0) | (ks[SDL_SCANCODE_LEFT] ? 4 : 0) | (ks[SDL_SCANCODE_RIGHT] ? 8 : 0);
}

} // namespace lp
