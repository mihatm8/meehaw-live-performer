// meehaw live performer — neon palette (SPEC §4, §9.14, §9.17c): Reaper category colours, one neon colour per
// parameter page / synth knob, and the few greys the screens share. Everything is constexpr RGB565 so it lives in flash.
#pragma once
#include "gfx.h"
#include "../core/types.h"

namespace lp {

constexpr Color kBlack = rgb(0, 0, 0);
constexpr Color kWhite = rgb(255, 255, 255);
constexpr Color kDimGrey = rgb(96, 96, 96);           // footer text, grid lines
constexpr Color kNeonGreen = rgb(0x39, 0xFF, 0x14);  // splash
constexpr Color kRed = rgb(255, 40, 40);             // errors
constexpr Color kBatteryLow = rgb(0xFF, 0x3B, 0x3B); // footer battery readout below 15 % and not charging (SPEC §4)

// Scales a packed RGB565 colour to pct percent (dim outlines of empty tracks = 30 %, SPEC §4).
constexpr Color scaleColor(Color c, int pct) {
  return (Color)((((((c >> 11) & 0x1F) * pct) / 100) << 11) | (((((c >> 5) & 0x3F) * pct) / 100) << 5) | (((c & 0x1F) * pct) / 100));
}

// Category colours from Reaper (SPEC §4): Chord orange, Lead yellow, Pad purple, Bass brown, Fx pink,
// Percussion green, Vocal cyan, Beat grey.
constexpr Color kCategoryColor[CategoryCount] = {
  rgb(0xFF, 0x8C, 0x00), rgb(0xFF, 0xD7, 0x00), rgb(0x9B, 0x59, 0xFF), rgb(0xB5, 0x65, 0x1D),
  rgb(0xFF, 0x4F, 0xA3), rgb(0x2E, 0xCC, 0x71), rgb(0x00, 0xE5, 0xFF), rgb(0xA0, 0xA0, 0xA0)};

// Parameter page colours (SPEC §4): vol green, reverb cyan, echo magenta, dist orange, filter yellow,
// length violet, tempo white, key blue, output (master volume) red; the synth knobs (SPEC §9.17c) cutoff
// yellow, resonance cyan, env amount magenta, decay violet, drive orange, wave white, and (SPEC §9.22) the 303
// character knobs SAT orange-red, ACC lime and the chorus send CHO sky blue; genre pink, ramp bars teal.
// Index = PageParam (ui.h); the first ParamCount entries are indexed by ParamId as well.
constexpr Color kSatColor = rgb(0xFF, 0x45, 0x00), kAccentColor = rgb(0xC6, 0xFF, 0x00), kChorusColor = rgb(0x4F, 0xC3, 0xFF);
constexpr Color kPageParamColor[ParamCount + 2] = {
  rgb(0x39, 0xFF, 0x14), rgb(0x00, 0xFF, 0xFF), rgb(0xFF, 0x00, 0xFF), rgb(0xFF, 0x7A, 0x00), rgb(0xFF, 0xFF, 0x00),
  rgb(0xB0, 0x26, 0xFF), rgb(0xFF, 0xFF, 0xFF), rgb(0x29, 0x79, 0xFF), rgb(0xFF, 0x3B, 0x3B),
  rgb(0xFF, 0xFF, 0x00), rgb(0x00, 0xFF, 0xFF), rgb(0xFF, 0x00, 0xFF), rgb(0xB0, 0x26, 0xFF), rgb(0xFF, 0x7A, 0x00), rgb(0xFF, 0xFF, 0xFF),
  kSatColor, kAccentColor, kChorusColor,
  rgb(0xFF, 0x4F, 0xA3), rgb(0x1D, 0xE9, 0xB6)};
static_assert(PChorus + 1 == ParamCount, "kPageParamColor lists the 18 ParamIds (…, PSat, PAccent, PChorus), then genre and ramp bars");

} // namespace lp
