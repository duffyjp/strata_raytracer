#!/usr/bin/env python3
"""make_favicon.py - generate browser/favicon.ico from scratch.

The page had no icon, so every browser run asked the server for `/favicon.ico`, got a 404, and the 404
landed in the run log next to the real measurements. The fix is an icon served from browser/ and
referenced from index.html. It is generated here rather than downloaded, in 118 lines and with no
dependency, because a third-party binary blob in the repo is the wrong trade for a 1 KB file: this script
*is* the provenance of the asset, and running it twice produces the same bytes.

The image is the page's own subject in 16x16: a dark frame, one diagonal ray from the lower left to a
hit point, and a lit pixel where it lands. 32-bit BGRA, no palette, one image.

    python browser/tools/make_favicon.py [out.png.ico]
"""
import struct
import sys

W = H = 16
BG = (0x14, 0x12, 0x18, 0xFF)          # near-black, matches index.html's background
RAY = (0xE8, 0xE4, 0xDC, 0xFF)         # the ray, warm white
HIT = (0x4C, 0xC8, 0xFF, 0xFF)         # BGRA: the pixel it lands on, cyan
FLOOR = (0x2A, 0x26, 0x30, 0xFF)       # the ground line the ray comes off


def build():
    px = [[BG for _ in range(W)] for _ in range(H)]
    for i in range(H):                   # rows top -> bottom; x decreases as i grows, so the ray
        x = 13 - i                       # runs from lower-left to upper-right, diagonally connected
        if 0 <= x < W:
            px[i][x] = RAY
    for x in range(1, W - 1):
        px[H - 3][x] = FLOOR
    for dy in range(2):                  # 2x2 hit point at the top of the ray
        for dx in range(2):
            px[dy][13 + dx] = HIT

    xor = bytearray()
    for row in range(H - 1, -1, -1):      # DIB rows are bottom-up
        for b, g, r, a in (px[row][x] for x in range(W)):
            xor += bytes((b, g, r, a))
    # AND mask: 1 bit per pixel, rows padded to 4 bytes; all zero = use the XOR alpha.
    and_row = b"\x00" * ((W + 31) // 32 * 4)
    and_mask = and_row * H

    bmi = struct.pack("<IiiHHIIiiII", 40, W, H * 2, 1, 32, 0, len(xor), 0, 0, 0, 0)
    img = bmi + bytes(xor) + and_mask
    header = struct.pack("<HHH", 0, 1, 1)                     # reserved, type=icon, count
    entry = struct.pack("<BBBBHHII", W % 256, H % 256, 0, 0, 1, 32, len(img), 6 + 16)
    return header + entry + img


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "browser/favicon.ico"
    data = build()
    with open(out, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %dx%d 32-bit" % (out, len(data), W, H))
