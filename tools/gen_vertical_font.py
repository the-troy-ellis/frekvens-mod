"""Regenerates esp32/src/font5x7_vertical.h from esp32/src/font5x7.h.

Rotates every glyph 90 degrees clockwise so vertical-orientation text mode
(each character stacked along Y instead of X) reads correctly. Run this any
time the base font5x7.h table changes; the vertical table is not meant to be
hand-edited.

Usage: python3 tools/gen_vertical_font.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "esp32" / "src" / "font5x7.h"
DST = ROOT / "esp32" / "src" / "font5x7_vertical.h"


def rotate_cw(glyph):
    # Original: 5 columns (x=0..4) x 7 rows (y=0..6), bit 0 = top row.
    # True 90 deg CLOCKWISE: a pixel at (x, y) maps to (x' = 6 - y, y' = x),
    # giving a 7-column x 5-row glyph. CW (not CCW) is what makes vertical text
    # read correctly when stacked first-char-at-top (book-spine convention):
    # rotating the display to upright then yields the letters in reading order.
    rotated = [0] * 7  # indexed by x' (column); bits are y' (row)
    for col in range(5):       # col = x
        for row in range(7):   # row = y
            if glyph[col] & (1 << row):
                xp = 6 - row
                yp = col
                rotated[xp] |= (1 << yp)
    return rotated


def main():
    text = SRC.read_text()
    entries = re.findall(
        r"\{(0x[0-9A-Fa-f]{2}(?:,0x[0-9A-Fa-f]{2}){4})\},\s*//\s*(\d+)\s*(.*)", text
    )
    assert len(entries) == 96, f"expected 96 glyphs, found {len(entries)}"

    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "",
        "// Vertical-orientation companion to font5x7.h — each glyph rotated 90 deg",
        "// clockwise, generated from font5x7[] by tools/gen_vertical_font.py.",
        "// Do not hand-edit: regenerate from font5x7.h if the base font ever changes.",
        "// Each char: 7 bytes (columns), 5 bits used per byte (bit 0 = top row).",
        "static const uint8_t font5x7_vertical[96][7] = {",
    ]
    for bytes_str, code, name in entries:
        glyph = [int(b, 16) for b in bytes_str.split(",")]
        rot = rotate_cw(glyph)
        hex_str = ",".join(f"0x{b:02X}" for b in rot)
        lines.append(f"    {{{hex_str}}}, // {code} {name}")
    lines.append("};")
    lines.append("")

    DST.write_text("\n".join(lines))
    print(f"wrote {DST} ({len(entries)} glyphs)")


if __name__ == "__main__":
    main()
