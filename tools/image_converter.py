#!/usr/bin/env python3
"""
image_converter.py — Convert images/GIFs to the Frekvens 8-bit formats.

Output formats (must match esp32/src/gif_decoder.h + the firmware playback):
  .raw   — single frame: 8-bit grayscale, 1 byte/pixel, row-major, 16×(N×16) px
  .anim  — multi-frame, with header:
             [4] "ANIM"  [1] w  [1] h  [2] num_frames (LE)
             [num_frames × w*h]  pixel data (8-bit, 1 byte/pixel)
             [num_frames × 2]    per-frame delays in ms (LE)

Usage:
  python image_converter.py input.png  output.raw  --panels 2
  python image_converter.py input.gif  output.anim --panels 2
  python image_converter.py input.png  --preview          # show preview, no output
"""

import argparse
import sys
import struct
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Requires Pillow: pip install Pillow")
    sys.exit(1)

PANEL_W = 16
PANEL_H = 16


def to_8bit_grayscale(img: Image.Image, width: int, height: int) -> bytes:
    """Resize image to width×height and convert to 8-bit grayscale, 1 byte per pixel."""
    img = img.convert("L")
    img = img.resize((width, height), Image.LANCZOS)
    return bytes(img.getdata())  # already 0-255, 1 byte per pixel


def convert_image(src: Path, dst: Path, num_panels: int):
    img = Image.open(src)
    h = num_panels * PANEL_H
    raw = to_8bit_grayscale(img, PANEL_W, h)
    dst.write_bytes(raw)
    print(f"Wrote {len(raw)} bytes → {dst}  ({PANEL_W}×{h} px, 8-bit, {num_panels} panel(s))")


def convert_gif(src: Path, dst: Path, num_panels: int):
    img = Image.open(src)
    h = num_panels * PANEL_H
    frames, delays = [], []
    try:
        while True:
            frames.append(to_8bit_grayscale(img.copy(), PANEL_W, h))
            # Per-frame duration in ms (default 100 if the GIF omits it)
            delays.append(min(max(int(img.info.get("duration", 100)), 0), 65535))
            img.seek(img.tell() + 1)
    except EOFError:
        pass

    num = len(frames)
    if num == 0:
        print("No frames decoded")
        sys.exit(1)
    frame_bytes = len(frames[0])

    # ANIM format: header, then all pixel frames, then all delays (matches firmware)
    with open(dst, "wb") as f:
        f.write(struct.pack("<4sBBH", b"ANIM", PANEL_W, h, num))
        for fr in frames:
            f.write(fr)
        for d in delays:
            f.write(struct.pack("<H", d))

    total = 8 + num * frame_bytes + num * 2
    print(f"Wrote {num} frames ({PANEL_W}×{h}, 8-bit) = {total} bytes → {dst}")


def show_preview(src: Path, num_panels: int):
    """ASCII preview of the converted image in terminal."""
    img = Image.open(src)
    h = num_panels * PANEL_H
    img = img.convert("L").resize((PANEL_W, h), Image.LANCZOS)
    RAMP = " .:-=+*#%@"
    print(f"┌{'─' * PANEL_W * 2}┐")
    for y in range(h):
        row = ""
        for x in range(PANEL_W):
            v = img.getpixel((x, y))
            row += RAMP[round(v / 255 * (len(RAMP) - 1))] * 2
        print(f"│{row}│")
    print(f"└{'─' * PANEL_W * 2}┘")


def main():
    parser = argparse.ArgumentParser(description="Frekvens image/animation converter")
    parser.add_argument("input",                         help="Input image or GIF")
    parser.add_argument("output",   nargs="?",           help="Output .raw or .anim file")
    parser.add_argument("--panels", type=int, default=1, help="Number of panels (default 1)")
    parser.add_argument("--preview", action="store_true", help="Show ASCII preview")
    args = parser.parse_args()

    src = Path(args.input)
    if not src.exists():
        print(f"File not found: {src}")
        sys.exit(1)

    if args.preview:
        show_preview(src, args.panels)
        if not args.output:
            return

    if not args.output:
        print("Specify output file or use --preview")
        sys.exit(1)

    dst = Path(args.output)
    if src.suffix.lower() == ".gif":
        convert_gif(src, dst, args.panels)
    else:
        convert_image(src, dst, args.panels)


if __name__ == "__main__":
    main()
