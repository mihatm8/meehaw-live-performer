#!/usr/bin/env python3
"""Printable key stickers for the meehaw live performer on the M5Stack Cardputer ADV (SPEC §9.22 / §9.27, docs/KEYMAP.md).

Writes docs/stickers/keys-main.svg and keys-shift.svg (and a .pdf beside each): A4 sheets with the 4 x 14 key matrix
at 1:1, one sticker per key. keys-main shows each key's MAIN legend large (<= 5 characters) and its SHIFT function as a
small second line; keys-shift shows the SHIFT function large and the main legend small. Stickers are colour-coded: the
track keys in their category colour, the parameter keys in their parameter-page colour (both read from
firmware/liveplayer/src/ui/palette.h so the sheet follows the firmware), the function keys grey. Every sheet carries a
50 mm calibration bar (print at 100 %, never "fit to page"), cut marks on the cell grid and the geometry it was made with.

Geometry (the Cardputer ADV keyboard is 56 keys in 4 x 14 on a device 84.0 x 54.0 x 19.6 mm wide, per M5Stack's
Cardputer-Adv documentation; the key pitch itself is not published): the default pitch is 5.8 mm, which puts the 14
columns at 81.2 mm inside the 84 mm case, and the sticker ("cap") 5.0 mm, leaving 0.8 mm between stickers. Measure your
unit — the distance between the centres of ` and del divided by 13 is the pitch — and pass --pitch / --cap when it differs.

Usage: python3 -I tools/make_stickers.py [--pitch 5.8] [--cap 5.0] [--gap 0] [--copies 2] [--out docs/stickers] [--no-pdf]
  --pitch  centre-to-centre distance of the keys in mm (columns and rows)
  --cap    sticker size in mm (square); must be <= pitch
  --gap    extra space added between the four rows beyond the pitch, in mm (0 = the block is exactly 1:1; a few mm make
           the rows easier to cut as strips, each row still 1:1 horizontally)
PDF: cairosvg (tools/.venv) or rsvg-convert when either works; otherwise a built-in writer emits the same drawing with
the standard Helvetica fonts, so a PDF is always produced. Standard library only; run with -I.
"""
import argparse
import datetime
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PALETTE_H = os.path.join(ROOT, "firmware", "liveplayer", "src", "ui", "palette.h")
DEFAULT_OUT = os.path.join(ROOT, "docs", "stickers")

CATEGORIES = ["Chord", "Lead", "Pad", "Bass", "Fx", "Percussion", "Vocal", "Beat"]
# Fallbacks = palette.h as of SPEC §9.22 (used only when the header cannot be parsed).
FALLBACK_CATEGORY = [(0xFF, 0x8C, 0x00), (0xFF, 0xD7, 0x00), (0x9B, 0x59, 0xFF), (0xB5, 0x65, 0x1D),
                     (0xFF, 0x4F, 0xA3), (0x2E, 0xCC, 0x71), (0x00, 0xE5, 0xFF), (0xA0, 0xA0, 0xA0)]
PARAM_NAMES = ["vol", "reverb", "echo", "dist", "filter", "length", "tempo", "key", "output",
               "cutoff", "res", "env", "decay", "drive", "wave", "sat", "accent", "chorus", "genre", "ramp"]
FALLBACK_PARAM = [(0x39, 0xFF, 0x14), (0x00, 0xFF, 0xFF), (0xFF, 0x00, 0xFF), (0xFF, 0x7A, 0x00), (0xFF, 0xFF, 0x00),
                  (0xB0, 0x26, 0xFF), (0xFF, 0xFF, 0xFF), (0x29, 0x79, 0xFF), (0xFF, 0x3B, 0x3B),
                  (0xFF, 0xFF, 0x00), (0x00, 0xFF, 0xFF), (0xFF, 0x00, 0xFF), (0xB0, 0x26, 0xFF), (0xFF, 0x7A, 0x00), (0xFF, 0xFF, 0xFF),
                  (0xFF, 0x45, 0x00), (0xC6, 0xFF, 0x00), (0x4F, 0xC3, 0xFF), (0xFF, 0x4F, 0xA3), (0x1D, 0xE9, 0xB6)]
FN_RGB = (0x60, 0x60, 0x60)           # function keys (gyro, genre, save, load, gen, edit, perform, shift)

PAGE_W, PAGE_H = 210.0, 297.0         # A4 portrait, mm
MARGIN = 12.0
ROWS, COLS = 4, 14

# ----------------------------------------------------------------------------------------------------------- legends
# Per key: (main legend <= 5 chars, SHIFT legend <= 5 chars, SHIFT function spelled out, colour key).
# Colour key: ("cat", i) a category, ("par", name) a parameter page, ("fn",) a function key. docs/KEYMAP.md is the
# authority; these are its one-word forms.
def _tracks(row):
    out = []
    for i, cat in enumerate(CATEGORIES):
        short = ["CHD", "LED", "PAD", "BAS", "FX", "PRC", "VOC", "BEAT"][i]
        out.append((short, "SEL", "select only (no start / stop)", ("cat", i)))
    layers = [("KICK", "SYNTH", "SYNTH page: Chord"), ("+HATS", "SYNTH", "SYNTH page: Lead"),
              ("+SNR", "SYNTH", "SYNTH page: Pad"), ("FULL", "SYNTH", "SYNTH page: Bass")]
    for main, big, small in layers:
        out.append((main, big, small, ("cat", 7)))
    out.append(("RUSH", "RUSH", "snare rush (as plain)", ("cat", 7)))
    out.append(("CUT", "OUT+" if row == 0 else "OUT-", "output volume up" if row == 0 else "output volume down", ("par", "output")))
    return out

LEGENDS = [
    _tracks(0),
    _tracks(1),
    [("VOL+", "RAMP+", "ramp to max, snap back", ("par", "vol")),
     ("REV+", "RAMP+", "ramp to max, snap back", ("par", "reverb")),
     ("ECHO+", "RAMP+", "ramp to max, snap back", ("par", "echo")),
     ("DIST+", "RAMP+", "ramp to max, snap back", ("par", "dist")),
     ("FILT+", "RAMP+", "ramp to HP, snap back", ("par", "filter")),
     ("LEN+", "BARS+", "ramp bars 1 2 4 8 16", ("par", "length")),
     ("TMP+", "DRIFT", "tempo drift +10 BPM", ("par", "tempo")),
     ("KEY+", "OCT+", "pattern an octave up", ("par", "key")),
     ("GYRO", "ALL", "PLAY ALL / STOP ALL", ("fn",)),
     ("GENRE", "ACID", "acid mode on / off", ("par", "genre")),
     ("SAVE1", "REC", "RECORDER page", ("fn",)),
     ("LOAD1", "THRY1", "THEORY set 1 (one chord progression; SPEC §9.25)", ("fn",)),
     ("GEN P", "USB", "hold 1 s: USB drive", ("fn",)),
     ("GEN 1", "GEN 2", "GENERATE set 2", ("fn",))],
    [("VOL-", "RAMP-", "ramp to min, snap back", ("par", "vol")),
     ("REV-", "RAMP-", "ramp to min, snap back", ("par", "reverb")),
     ("ECHO-", "RAMP-", "ramp to min, snap back", ("par", "echo")),
     ("DIST-", "RAMP-", "ramp to min, snap back", ("par", "dist")),
     ("FILT-", "RAMP-", "ramp to LP, snap back", ("par", "filter")),
     ("LEN-", "BARS-", "ramp bars 16 8 4 2 1", ("par", "length")),
     ("TMP-", "DRIFT", "tempo drift -10 BPM", ("par", "tempo")),
     ("KEY-", "OCT-", "pattern an octave down", ("par", "key")),
     ("EDIT", "LOOP", "loop mode on / off", ("fn",)),
     ("PERF", "PRF 2", "perform: other set", ("fn",)),
     ("SAVE2", "PRSET", "PRESET MANAGER (target set chosen on the page; SPEC §9.27)", ("fn",)),
     ("LOAD2", "THRY2", "THEORY set 2 (one chord progression; SPEC §9.25)", ("fn",)),
     ("GEN S", "GEN S", "gen sound (as plain)", ("fn",)),
     ("SHIFT", "SHIFT", "hold = SHIFT layer", ("fn",))],
]
ROW_NOTES = ["y0  set 1", "y1  set 2", "y2", "y3"]

# ----------------------------------------------------------------------------------------------------------- palette
_RGB = re.compile(r"rgb\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)")


def _int(s):
    return int(s, 16) if s.lower().startswith("0x") else int(s)


def parse_palette(path):
    """kCategoryColor[8] and kPageParamColor[20] from palette.h; the fallbacks when the file is missing or changed shape."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return FALLBACK_CATEGORY, FALLBACK_PARAM, False
    named = {}
    for m in re.finditer(r"(k\w+Color)\s*=\s*" + _RGB.pattern, text):
        named[m.group(1)] = (_int(m.group(2)), _int(m.group(3)), _int(m.group(4)))

    def array(name, n):
        m = re.search(name + r"\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
        if not m:
            return None
        out = []
        for tok in re.finditer(_RGB.pattern + r"|(k\w+Color)", m.group(1)):
            if tok.group(4):
                if tok.group(4) not in named:
                    return None
                out.append(named[tok.group(4)])
            else:
                out.append((_int(tok.group(1)), _int(tok.group(2)), _int(tok.group(3))))
        return out if len(out) == n else None

    cats = array("kCategoryColor", len(CATEGORIES))
    pars = array("kPageParamColor", len(PARAM_NAMES))
    if cats is None or pars is None:
        return FALLBACK_CATEGORY, FALLBACK_PARAM, False
    return cats, pars, True


def mix(rgb, white_part):
    return tuple(int(round(c + (255 - c) * white_part)) for c in rgb)


def hexcol(rgb):
    return "#%02X%02X%02X" % rgb


# ----------------------------------------------------------------------------------------------------------- metrics
# Helvetica / Helvetica-Bold advance widths (AFM, per 1000 em) for ASCII 32..126: text fitting and PDF centring.
_W_REG = [278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
          556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
          667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556,
          556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584]
_W_BOLD = [278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556, 556, 556,
           556, 556, 333, 333, 584, 584, 584, 611, 975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
           667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556, 333, 556, 611, 556, 611, 556, 333, 611,
           611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584]
assert len(_W_REG) == 95 and len(_W_BOLD) == 95


def text_width(s, size, bold):
    table = _W_BOLD if bold else _W_REG
    return sum(table[ord(c) - 32] if 32 <= ord(c) < 127 else 600 for c in s) * size / 1000.0


def fit_size(s, size, max_w, bold, min_size):
    w = text_width(s, size, bold)
    if w <= max_w:
        return size
    return max(min_size, size * max_w / w)


# ----------------------------------------------------------------------------------------------------------- scene
class Scene:
    """Drawing primitives in mm, emitted as SVG or PDF. Text y is the baseline; anchor start | middle | end."""

    def __init__(self, title):
        self.title = title
        self.items = []

    def rect(self, x, y, w, h, fill=None, stroke=None, sw=0.2, r=0.0):
        self.items.append(("rect", x, y, w, h, fill, stroke, sw, r))

    def line(self, x1, y1, x2, y2, rgb=(0, 0, 0), sw=0.2, dash=None):
        self.items.append(("line", x1, y1, x2, y2, rgb, sw, dash))

    def text(self, x, y, s, size, rgb=(0, 0, 0), bold=False, anchor="start"):
        self.items.append(("text", x, y, s, size, rgb, bold, anchor))

    # ---- SVG ----
    def svg(self):
        out = ['<?xml version="1.0" encoding="UTF-8"?>',
               '<svg xmlns="http://www.w3.org/2000/svg" width="%gmm" height="%gmm" viewBox="0 0 %g %g">' % (PAGE_W, PAGE_H, PAGE_W, PAGE_H),
               '<title>%s</title>' % _xml(self.title),
               '<rect x="0" y="0" width="%g" height="%g" fill="#FFFFFF"/>' % (PAGE_W, PAGE_H)]
        for it in self.items:
            if it[0] == "rect":
                _, x, y, w, h, fill, stroke, sw, r = it
                out.append('<rect x="%.3f" y="%.3f" width="%.3f" height="%.3f" rx="%.2f" fill="%s" stroke="%s" stroke-width="%.2f"/>'
                           % (x, y, w, h, r, hexcol(fill) if fill else "none", hexcol(stroke) if stroke else "none", sw if stroke else 0))
            elif it[0] == "line":
                _, x1, y1, x2, y2, rgb, sw, dash = it
                out.append('<line x1="%.3f" y1="%.3f" x2="%.3f" y2="%.3f" stroke="%s" stroke-width="%.2f"%s/>'
                           % (x1, y1, x2, y2, hexcol(rgb), sw, ' stroke-dasharray="%s"' % dash if dash else ""))
            else:
                _, x, y, s, size, rgb, bold, anchor = it
                out.append('<text x="%.3f" y="%.3f" font-family="Helvetica, Arial, sans-serif" font-size="%.3f" font-weight="%s" fill="%s" text-anchor="%s">%s</text>'
                           % (x, y, size, "bold" if bold else "normal", hexcol(rgb), anchor, _xml(s)))
        out.append("</svg>")
        return "\n".join(out) + "\n"

    # ---- PDF (built-in fallback: standard Helvetica fonts, no embedding) ----
    def pdf(self):
        k = 72.0 / 25.4
        ops = []

        def col(rgb, op):
            ops.append("%.3f %.3f %.3f %s" % (rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, op))

        for it in self.items:
            if it[0] == "rect":
                _, x, y, w, h, fill, stroke, sw, r = it
                if fill:
                    col(fill, "rg")
                if stroke:
                    col(stroke, "RG")
                    ops.append("%.3f w" % (sw * k))
                ops.append("%.3f %.3f %.3f %.3f re %s" % (x * k, (PAGE_H - y - h) * k, w * k, h * k, "B" if fill and stroke else "f" if fill else "S" if stroke else "n"))
            elif it[0] == "line":
                _, x1, y1, x2, y2, rgb, sw, dash = it
                col(rgb, "RG")
                ops.append("%.3f w" % (sw * k))
                ops.append("[%s] 0 d" % (" ".join("%.2f" % (float(v) * k) for v in dash.split(",")) if dash else ""))
                ops.append("%.3f %.3f m %.3f %.3f l S" % (x1 * k, (PAGE_H - y1) * k, x2 * k, (PAGE_H - y2) * k))
            else:
                _, x, y, s, size, rgb, bold, anchor = it
                w = text_width(s, size, bold)
                x0 = x - (w / 2 if anchor == "middle" else w if anchor == "end" else 0)
                col(rgb, "rg")
                ops.append("BT /%s %.3f Tf %.3f %.3f Td (%s) Tj ET" % ("F1" if bold else "F2", size * k, x0 * k, (PAGE_H - y) * k, _pdf_str(s)))
        stream = "\n".join(ops).encode("latin-1", "replace")
        objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
                b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] /Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>" % (PAGE_W * k, PAGE_H * k)).encode(),
                b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream",
                b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>",
                b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
                ("<< /Title (%s) /Producer (tools/make_stickers.py) >>" % _pdf_str(self.title)).encode("latin-1", "replace")]
        out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
        offsets = []
        for i, body in enumerate(objs, 1):
            offsets.append(len(out))
            out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
        xref = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
        for o in offsets:
            out += b"%010d 00000 n \n" % o
        out += b"trailer\n<< /Size %d /Root 1 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, len(objs), xref)
        return bytes(out)


def _xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def _pdf_str(s):
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


# ----------------------------------------------------------------------------------------------------------- sheet
def key_colour(ck, cats, pars):
    if ck[0] == "cat":
        return cats[ck[1]]
    if ck[0] == "par":
        return pars[PARAM_NAMES.index(ck[1])]
    return FN_RGB


def draw_block(sc, x0, y0, pitch, cap, gap, big_is_shift, cats, pars):
    """One 4 x 14 block at 1:1 with cut marks; returns its height."""
    row_pitch = pitch + gap
    inset = (pitch - cap) / 2.0
    main_size = 0.36 * cap
    small_size = 0.21 * cap
    for r in range(ROWS):
        for c in range(COLS):
            main, big, small_shift, ck = LEGENDS[r][c]
            rgb = key_colour(ck, cats, pars)
            border = rgb if rgb != (255, 255, 255) else (0x90, 0x90, 0x90)
            x = x0 + c * pitch + inset
            y = y0 + r * row_pitch + inset
            sc.rect(x, y, cap, cap, fill=mix(rgb, 0.72), stroke=border, sw=0.25, r=0.45)
            if big_is_shift:
                large, line2 = big, main
            else:
                large, line2 = main, big
            fs = fit_size(large, main_size, cap - 0.5, True, 0.22 * cap)
            sc.text(x + cap / 2, y + cap * 0.50, large, fs, (0, 0, 0), True, "middle")
            if line2 and line2 != large:
                fs2 = fit_size(line2, small_size, cap - 0.5, False, 0.15 * cap)
                sc.text(x + cap / 2, y + cap * 0.84, line2, fs2, (0x30, 0x30, 0x30), False, "middle")
    width = COLS * pitch
    height = ROWS * row_pitch
    tick, grey = 1.6, (0x80, 0x80, 0x80)
    for c in range(COLS + 1):                       # cut marks on the cell grid: columns (top and bottom) ...
        x = x0 + c * pitch
        sc.line(x, y0 - 0.4, x, y0 - 0.4 - tick, grey, 0.15)
        sc.line(x, y0 + height + 0.4, x, y0 + height + 0.4 + tick, grey, 0.15)
    for r in range(ROWS + 1):                       # ... and rows (left and right)
        y = y0 + r * row_pitch - (gap / 2.0 if 0 < r < ROWS else 0)
        sc.line(x0 - 0.4, y, x0 - 0.4 - tick, y, grey, 0.15)
        sc.line(x0 + width + 0.4, y, x0 + width + 0.4 + tick, y, grey, 0.15)
    for r in range(ROWS):                           # row notes left of the block
        sc.text(x0 - 3.0, y0 + r * row_pitch + pitch * 0.62, ROW_NOTES[r], 2.0, (0x60, 0x60, 0x60), False, "end")
    if gap > 0:                                     # dashed cut lines between the rows
        for r in range(1, ROWS):
            y = y0 + r * row_pitch - gap / 2.0
            sc.line(x0, y, x0 + width, y, (0xB0, 0xB0, 0xB0), 0.12, "1,1")
    return height


def build_sheet(which, pitch, cap, gap, copies, cats, pars, palette_ok):
    big_is_shift = which == "shift"
    title = "meehaw live performer - key stickers (%s layer large)" % ("SHIFT" if big_is_shift else "MAIN")
    sc = Scene(title)
    y = MARGIN + 4
    sc.text(MARGIN, y, title, 4.2, (0, 0, 0), True)
    y += 6
    sub = ("Print at 100 %% (actual size, no 'fit to page'). The bar must measure 50 mm. Pitch %.2f mm, sticker %.2f mm, row gap %.1f mm. "
           % (pitch, cap, gap))
    sc.text(MARGIN, y, sub, 2.4, (0x30, 0x30, 0x30))
    y += 4
    sc.text(MARGIN, y, ("Large: %s legend. Small: %s. Colours: track keys by category, parameter keys by their page colour (ui/palette.h%s), function keys grey."
                        % ("SHIFT" if big_is_shift else "main", "the main legend" if big_is_shift else "the SHIFT function", "" if palette_ok else " fallback")), 2.4, (0x30, 0x30, 0x30))
    y += 7
    # calibration bar: 50 mm with 10 mm ticks
    bx, bh = MARGIN, 3.0
    sc.rect(bx, y, 50.0, bh, fill=None, stroke=(0, 0, 0), sw=0.25)
    for i in range(6):
        sc.line(bx + i * 10.0, y, bx + i * 10.0, y + bh, (0, 0, 0), 0.25)
    sc.text(bx + 53.0, y + bh - 0.6, "50 mm", 2.6, (0, 0, 0), True)
    sc.text(bx + 68.0, y + bh - 0.6, "0   10   20   30   40   50", 2.0, (0x60, 0x60, 0x60))
    y += bh + 10
    width = COLS * pitch
    x0 = MARGIN + 14.0                              # room for the row notes
    if x0 + width > PAGE_W - MARGIN:
        x0 = PAGE_W - MARGIN - width
    for n in range(copies):
        sc.text(x0, y - 2.0, "copy %d of %d  -  1:1" % (n + 1, copies), 2.2, (0x60, 0x60, 0x60))
        h = draw_block(sc, x0, y, pitch, cap, gap, big_is_shift, cats, pars)
        y += h + 14
    # colour legend
    sc.text(MARGIN, y, "Colours", 3.0, (0, 0, 0), True)
    y += 5
    entries = [(CATEGORIES[i], cats[i]) for i in range(len(CATEGORIES))]
    entries += [("vol", pars[0]), ("reverb", pars[1]), ("echo", pars[2]), ("dist", pars[3]), ("filter", pars[4]), ("length", pars[5]),
                ("tempo", pars[6]), ("key", pars[7]), ("output", pars[8]), ("genre", pars[18]), ("function", FN_RGB)]
    col_w, row_h, per_row = 36.0, 5.0, 5
    for i, (name, rgb) in enumerate(entries):
        cx = MARGIN + (i % per_row) * col_w
        cy = y + (i // per_row) * row_h
        border = rgb if rgb != (255, 255, 255) else (0x90, 0x90, 0x90)
        sc.rect(cx, cy - 3.2, 4.0, 4.0, fill=mix(rgb, 0.72), stroke=border, sw=0.25, r=0.4)
        sc.text(cx + 5.5, cy, name, 2.6, (0x20, 0x20, 0x20))
    y += ((len(entries) + per_row - 1) // per_row) * row_h + 6
    notes = ["Rows: y0 = set 1 tracks / Beat layers / rush / kick cut, y1 = set 2 (same), y2 = parameter + keys and functions, y3 = parameter - keys and functions; SHIFT = the space key (13,3).",
             "Cut on the grey marks (one sticker per cell, %.1f mm of white around each). The full map with every page is docs/KEYMAP.md; the manual is docs/MANUAL.md." % ((pitch - cap) / 2.0),
             "Generated %s by tools/make_stickers.py --pitch %g --cap %g --gap %g (device: M5Stack Cardputer ADV, 56 keys 4 x 14, case 84.0 x 54.0 mm per M5Stack docs; the pitch is not published - measure yours)."
             % (datetime.date.today().isoformat(), pitch, cap, gap)]
    for n in notes:
        for line in _wrap(n, 2.2, PAGE_W - 2 * MARGIN):
            sc.text(MARGIN, y, line, 2.2, (0x40, 0x40, 0x40))
            y += 3.2
        y += 1.0
    if y > PAGE_H - MARGIN:
        print("warning: the sheet overflows the page by %.1f mm (fewer --copies or a smaller --gap)" % (y - (PAGE_H - MARGIN)), file=sys.stderr)
    return sc


def _wrap(text, size, max_w):
    words, lines, cur = text.split(), [], ""
    for w in words:
        cand = (cur + " " + w).strip()
        if text_width(cand, size, False) > max_w and cur:
            lines.append(cur)
            cur = w
        else:
            cur = cand
    if cur:
        lines.append(cur)
    return lines


# ----------------------------------------------------------------------------------------------------------- PDF export
def write_pdf(svg_path, pdf_path, scene):
    """cairosvg (tools/.venv or the running Python), then rsvg-convert, then the built-in writer. Returns the method used."""
    try:
        import cairosvg  # type: ignore
        cairosvg.svg2pdf(url=svg_path, write_to=pdf_path)
        return "cairosvg"
    except Exception:
        pass
    venv_py = os.path.join(ROOT, "tools", ".venv", "bin", "python")
    if os.path.exists(venv_py) and os.path.realpath(venv_py) != os.path.realpath(sys.executable):
        try:
            subprocess.run([venv_py, "-I", "-c", "import cairosvg,sys; cairosvg.svg2pdf(url=sys.argv[1], write_to=sys.argv[2])", svg_path, pdf_path],
                           check=True, capture_output=True, timeout=60)
            return "cairosvg (tools/.venv)"
        except Exception:
            pass
    rsvg = shutil.which("rsvg-convert")
    if rsvg:
        try:
            subprocess.run([rsvg, "-f", "pdf", "-o", pdf_path, svg_path], check=True, capture_output=True, timeout=60)
            return "rsvg-convert"
        except Exception:
            pass
    with open(pdf_path, "wb") as f:
        f.write(scene.pdf())
    return "built-in writer (Helvetica)"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Printable 1:1 key stickers for the Cardputer ADV (docs/stickers).")
    ap.add_argument("--pitch", type=float, default=5.8, help="key pitch in mm, centre to centre (default 5.8)")
    ap.add_argument("--cap", type=float, default=5.0, help="sticker size in mm (default 5.0)")
    ap.add_argument("--gap", type=float, default=0.0, help="extra space between the rows in mm (default 0 = exact 1:1 block)")
    ap.add_argument("--copies", type=int, default=2, help="copies of the block per sheet (default 2)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output folder (default docs/stickers)")
    ap.add_argument("--palette", default=PALETTE_H, help="palette.h to read the colours from")
    ap.add_argument("--no-pdf", action="store_true", help="write the SVGs only")
    args = ap.parse_args(argv)
    if args.cap > args.pitch or args.cap <= 0 or args.pitch <= 0:
        ap.error("--cap must be positive and at most --pitch")
    if args.copies < 1 or args.copies > 4:
        ap.error("--copies 1..4")
    for r in range(ROWS):
        for c in range(COLS):
            main, big = LEGENDS[r][c][0], LEGENDS[r][c][1]
            assert len(main) <= 5 and len(big) <= 5, (r, c, main, big)
    cats, pars, ok = parse_palette(args.palette)
    if not ok:
        print("warning: could not parse %s, using the built-in colours" % args.palette, file=sys.stderr)
    os.makedirs(args.out, exist_ok=True)
    for which in ("main", "shift"):
        sc = build_sheet(which, args.pitch, args.cap, args.gap, args.copies, cats, pars, ok)
        svg_path = os.path.join(args.out, "keys-%s.svg" % which)
        with open(svg_path, "w", encoding="utf-8") as f:
            f.write(sc.svg())
        print("wrote %s" % os.path.relpath(svg_path, ROOT))
        if not args.no_pdf:
            pdf_path = os.path.join(args.out, "keys-%s.pdf" % which)
            how = write_pdf(svg_path, pdf_path, sc)
            print("wrote %s (%s)" % (os.path.relpath(pdf_path, ROOT), how))
    return 0


if __name__ == "__main__":
    sys.exit(main())
