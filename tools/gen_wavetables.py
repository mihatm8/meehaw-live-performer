#!/usr/bin/env python3
"""Generates firmware/liveplayer/src/dsp/wavetables_data.hpp (SPEC §9.15: pitch-ranged wavetable sets).

The data file is a .hpp (included once, by wavetables.cpp) because arduino-cli copies only recognised source and
header extensions into its build tree: a .inc file is invisible to the target build.

Single-cycle int16 tables built additively so they are band-limited. A table whose highest partial H sits
above Nyquist at the played pitch folds that partial back into the band, so the tables come in SETS with
decreasing harmonic counts and the synth picks the set per oscillator from its current pitch, once per
modulation block (dsp/wavetables.h waveSetForPitch, dsp/synth_voice.cpp selectTables). Every set is
alias-free over the whole range it serves (H x f(top pitch + 0.49) < 11025 Hz, the half-step margin
because the voice rounds a fractional glide pitch to the nearest integer):
  set 0  harmonics <= 40  MIDI <= 60   2048 samples   (40 x 261.6 Hz = 10.47 kHz; 61 would fold: 11.09 kHz)
  set 1  harmonics <= 16  MIDI 61..76   512 samples   (16 x 659.3 Hz = 10.55 kHz; 77 would fold: 11.18 kHz)
  set 2  harmonics <= 6   MIDI 77..93   256 samples   ( 6 x 1760 Hz = 10.56 kHz; 94 would fold: 11.19 kHz)
  set 3  harmonics <= 3   MIDI >= 94    128 samples   ( 3 x 3520 Hz = 10.56 kHz at MIDI 105; 106 folds)
Table lengths: set 0 keeps 2048 samples; a set with fewer partials needs fewer samples (>= 32 samples per
period of its top partial keeps the linear-interpolation images below -74 dB re the fundamental, see
tests/test_dsp_core.cpp wave_sample_interpolation), so the sets together take 29 KB of flash instead of
68 KB, and the whole working set stays resident in the ESP32-S3's 32 KB data cache while 32 voices
stream through their tables (SPEC §9.15). Every table carries ONE guard sample (a copy of sample 0)
after its last sample so the oscillator reads t[i + 1] without a wrap mask (wavetables.h waveSampleN).
Series per wave (k = harmonic number, H = the set's harmonic cap):
  WSine    sin(theta)                                              (one partial in every set)
  WTri     sum over odd k <= H of (-1)^((k-1)/2) sin(k theta) / k^2
  WSaw     sum over k <= H of sin(k theta) / k                    (falling ramp, jump at theta = 0)
  WSquare  sum over odd k <= H of sin(k theta) / k
  WFat     saw + 0.4 * square + 0.25 * saw2, where saw2 is the saw one octave up (its k <= H/2 so the
           highest partial is still H). The three natural series are mixed directly (no per-part
           normalisation), so in units of the saw's fundamental harmonic j of fat is 1.4 / j for odd j
           and 1.5 / j for even j (h2 / h1 = 0.536, h3 / h1 = 0.333), for every set.
All series use positive coefficients so their partials add constructively. Every table is normalised
once, after mixing, to a peak of 29000 (headroom for interpolation and for summing two oscillators).
Set 0 is bit-identical to the former single table set (same series, same rounding) plus its guard sample.

Output: 20 tables (4 sets x 5 waves): 5 x 2049 + 5 x 513 + 5 x 257 + 5 x 129 int16 = 29 480 B of const
data (flash), plus the pointer matrix kWaveSetTables[set][Wave] and the per-set kWaveSetHarmonicsData /
kWaveSetShiftData / kWaveSetMinPitchData that wavetables.cpp static_asserts against wavetables.h.

Standard library only. Usage: python3 tools/gen_wavetables.py [output path]
"""
import math
import os
import sys

PEAK = 29000
VALUES_PER_LINE = 16
# (set index, harmonic cap, first MIDI pitch served, table length). Keep in sync with dsp/wavetables.h
# kWaveSetHarmonics / kWaveSetMinPitch / kWaveSetShift (the C++ side static_asserts them against this file).
SETS = [(0, 40, 0, 2048), (1, 16, 61, 512), (2, 6, 77, 256), (3, 3, 94, 128)]
WAVE_NAMES = ["Sine", "Tri", "Saw", "Square", "Fat"]   # order of enum Wave in core/types.h
NYQUIST = 11025.0


def midi_to_hz(m):
  return 440.0 * 2.0 ** ((m - 69.0) / 12.0)


def check_sets():
  """Every set must be alias-free up to half a semitone above the last integer pitch it serves."""
  for i, (index, h, lo, length) in enumerate(SETS):
    top = SETS[i + 1][2] - 1 if i + 1 < len(SETS) else 105
    top_hz = h * midi_to_hz(top + 0.49)
    assert top_hz < NYQUIST, "set %d: %d partials fold at MIDI %d (%.0f Hz)" % (index, h, top, top_hz)
    assert length >= 32 * h and length & (length - 1) == 0, "set %d: length %d" % (index, length)
    if i + 1 < len(SETS):   # the boundary is tight: one pitch higher the set would fold
      assert h * midi_to_hz(top + 1) > NYQUIST, "set %d could serve MIDI %d" % (index, top + 1)


def additive(amplitude_for_k, max_k, length):
  """Returns one cycle (float list) of sum_k amplitude(k) * sin(k theta); zero amplitudes skipped."""
  out = [0.0] * length
  for k in range(1, max_k + 1):
    a = amplitude_for_k(k)
    if a == 0.0:
      continue
    for n in range(length):
      out[n] += a * math.sin(2.0 * math.pi * k * n / length)
  return out


def normalise(table, peak=1.0):
  m = max(abs(v) for v in table)
  return [v * (peak / m) for v in table]


def to_int16(table):
  out = []
  for v in table:
    r = int(math.copysign(math.floor(abs(v) + 0.5), v))   # round half away from zero
    out.append(max(-32768, min(32767, r)))
  return out


def build_set(max_harmonic, length):
  """The five waves of one set (sine, tri, saw, square, fat), float, unnormalised, in enum Wave order."""
  h = max_harmonic
  sine = additive(lambda k: 1.0 if k == 1 else 0.0, 1, length)
  tri = additive(lambda k: ((-1.0) ** ((k - 1) // 2)) / (k * k) if k % 2 == 1 else 0.0, h, length)
  saw = additive(lambda k: 1.0 / k, h, length)
  square = additive(lambda k: 1.0 / k if k % 2 == 1 else 0.0, h, length)
  saw2 = additive(lambda k: 1.0 / (k // 2) if k % 2 == 0 else 0.0, h, length)   # saw(2 theta), partials <= h
  # Raw series mixed as specified (saw + 0.4 square + 0.25 saw2); emit() applies the single normalisation.
  fat = [saw[i] + 0.4 * square[i] + 0.25 * saw2[i] for i in range(length)]
  return [("Sine", sine), ("Tri", tri), ("Saw", saw), ("Square", square), ("Fat", fat)]


def build_tables():
  """Returns [(c name, float table)] in emission order: set 0 .. set 3, each sine, tri, saw, square, fat."""
  tables = []
  for index, max_harmonic, _, length in SETS:
    for wave, table in build_set(max_harmonic, length):
      tables.append(("kWave%s%d" % (wave, index), table))
  return tables


def table_name(wave, index):
  return "kWave%s%d" % (wave, index)


def emit(tables, path):
  total = sum(2 * (len(t) + 1) for _, t in tables)
  lines = [
    "// Generated by tools/gen_wavetables.py - do not edit. Included by src/dsp/wavetables.cpp (SPEC §9.15).",
    "// Peak %d. Four band-limited sets picked per oscillator by pitch (dsp/wavetables.h), each table" % PEAK,
    "// followed by one guard sample (= sample 0) so the oscillator reads t[i + 1] without a wrap mask:",
  ]
  for index, max_harmonic, min_pitch, length in SETS:
    lines.append("//   set %d: harmonics <= %-2d  MIDI >= %-3d %4d + 1 samples" % (index, max_harmonic, min_pitch, length))
  lines += [
    "// %d tables, %d B of const data (flash)." % (len(tables), total),
    "// Regenerate: python3 tools/gen_wavetables.py",
    "",
  ]
  for name, table in tables:
    ints = to_int16(normalise(table, PEAK))
    ints.append(ints[0])                                   # guard sample (SPEC §9.15)
    lines.append("static const int16_t %s[%d] = {" % (name, len(ints)))
    for i in range(0, len(ints), VALUES_PER_LINE):
      lines.append("  " + ", ".join("%6d" % v for v in ints[i:i + VALUES_PER_LINE]) + ",")
    lines.append("};")
    lines.append("")
  lines.append("// kWaveSetTables[set][Wave] (enum Wave order: %s)." % ", ".join(WAVE_NAMES))
  lines.append("static const int16_t* const kWaveSetTables[%d][%d] = {" % (len(SETS), len(WAVE_NAMES)))
  for index, _, _, _ in SETS:
    lines.append("  {" + ", ".join(table_name(w, index) for w in WAVE_NAMES) + "},")
  lines.append("};")
  lines.append("// Per set: highest partial, log2(table length), first MIDI pitch served (wavetables.cpp static_asserts")
  lines.append("// these against wavetables.h).")
  lines.append("constexpr int kWaveSetHarmonicsData[%d] = {%s};" % (len(SETS), ", ".join(str(h) for _, h, _, _ in SETS)))
  lines.append("constexpr int kWaveSetShiftData[%d] = {%s};" % (
    len(SETS), ", ".join(str(int(math.log2(n))) for _, _, _, n in SETS)))
  lines.append("constexpr int kWaveSetMinPitchData[%d] = {%s};" % (len(SETS), ", ".join(str(p) for _, _, p, _ in SETS)))
  lines.append("")
  with open(path, "w", newline="\n") as f:
    f.write("\n".join(lines))
  return len(tables), total


def main():
  check_sets()
  here = os.path.dirname(os.path.abspath(__file__))
  default = os.path.join(here, "..", "firmware", "liveplayer", "src", "dsp", "wavetables_data.hpp")
  path = os.path.normpath(sys.argv[1] if len(sys.argv) > 1 else default)
  count, total = emit(build_tables(), path)
  print("wrote %d tables (%d B of const data) to %s" % (count, total, path))


if __name__ == "__main__":
  main()
