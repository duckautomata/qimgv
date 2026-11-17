#!/usr/bin/env python3
"""Draw the icons Windows shows for files associated with qimgv.

Each kind of file gets the app tile with its own pair of letters, so a folder
of mixed media sorts itself out at a glance in Explorer:

    images   "qi" on grey   the app icon itself (res/icons/common/logo/app)
    video    "qv" on blue   res/icons/common/logo/filetype/video.ico
    audio    "qa" on red    res/icons/common/logo/filetype/audio.ico

The two .ico files are embedded in qimgv.exe by qimgv/qimgv.rc, and the
installer (scripts/qimgv.iss) points each file type's DefaultIcon at them.

The art follows the app logo: a full-bleed tile with light letters built only
from axis-aligned rectangles. The logo's "i" is a single bar, but "a" and "v"
are as wide as the "q", so the pair uses a slightly lighter stroke than the
logo to fit the tile. Every size is drawn on its own pixel grid, never scaled,
so each one is crisp. 64, 128 and 256 are the 32 px drawing at 2x, 4x and 8x,
the same way the app icon's large sizes are made.

    python scripts/make-filetype-icons.py                    # rewrite the .ico files
    python scripts/make-filetype-icons.py --preview out.png  # and a preview sheet

Standard library only. The output is deterministic: rerunning it on unchanged
code produces byte-identical files.
"""

import argparse
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "qimgv", "res", "icons", "common", "logo", "filetype")
APP_LOGO_DIR = os.path.join(ROOT, "qimgv", "res", "icons", "common", "logo", "app")

INK = "#e6e6e6"  # the app logo's letter colour
TILES = {
    # name: (tile colour, second letter). Muted, so they sit beside the
    # logo's #373737 grey; both keep the letters above 4.5:1 contrast.
    "video": ("#2a5ea8", "v"),
    "audio": ("#a8322b", "a"),
}

# Sizes written into each .ico. 16/20/24/32 are Explorer's small icons at
# 100-200% display scale, 32/40/48/64 its medium ones, 256 large and up.
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

# Per-size letter metrics, in pixels:
#   s   stroke width          xh  x-height (the "q" bowl, all of "a" and "v")
#   d   the "q" descender     wq  width of the "q"
#   g   gap between letters   w2  width of the second letter
GRIDS = {
    16: dict(s=2, xh=8, d=2, wq=6, g=1, w2=6),
    20: dict(s=2, xh=9, d=2, wq=7, g=1, w2=7),
    24: dict(s=3, xh=12, d=2, wq=9, g=2, w2=9),
    32: dict(s=3, xh=13, d=3, wq=11, g=2, w2=11),
    40: dict(s=4, xh=16, d=4, wq=13, g=3, w2=13),
    48: dict(s=5, xh=21, d=4, wq=17, g=3, w2=17),
}


def grid_for(size):
    """The metrics to draw `size` with, and the integer scale to draw them at."""
    if size in GRIDS:
        return GRIDS[size], 1
    if size % 32 == 0:
        return GRIDS[32], size // 32
    raise ValueError(f"no pixel grid for {size}px")


def letter_rects(size, letter):
    """The "q" plus `letter`, as (x, y, w, h) rectangles on the size's grid."""
    m, scale = grid_for(size)
    s, xh, d, wq, g, w2 = (m[k] for k in ("s", "xh", "d", "wq", "g", "w2"))
    base = size // scale
    # Centre the pair; any odd pixel goes to the right and bottom, as in the logo.
    x0 = (base - (wq + g + w2)) // 2
    y0 = (base - (xh + d)) // 2
    x1 = x0 + wq + g

    rects = [
        (x0, y0, s, xh),               # q: left
        (x0, y0, wq, s),               # q: top
        (x0, y0 + xh - s, wq, s),      # q: bottom
        (x0 + wq - s, y0, s, xh + d),  # q: right, down into the descender
    ]
    if letter == "a":
        # Two-storey square "a": top bar, right stem, middle bar and a closed
        # bowl below it.
        mid = s + (xh - 3 * s) // 2
        rects += [
            (x1, y0, w2, s),
            (x1 + w2 - s, y0, s, xh),
            (x1, y0 + mid, w2, s),
            (x1, y0 + mid, s, xh - mid),
            (x1, y0 + xh - s, w2, s),
        ]
    elif letter == "v":
        # Two strokes that run straight for the top third, then step in
        # evenly until they meet on the baseline.
        inset = (w2 - s) // 2
        straight = int(0.35 * xh + 0.5)
        for row in range(xh):
            step = 0 if row < straight else int((row - straight + 1) * inset / (xh - straight) + 0.5)
            rects += [(x1 + step, y0 + row, s, 1), (x1 + w2 - s - step, y0 + row, s, 1)]
    else:
        raise ValueError(letter)
    return [(x * scale, y * scale, w * scale, h * scale) for x, y, w, h in rects]


def rgba(hex_colour):
    h = hex_colour.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16), 255)


def draw(size, tile, letter):
    """The icon as rows of (r, g, b, a) pixels."""
    pixels = [[rgba(tile)] * size for _ in range(size)]
    ink = rgba(INK)
    for x, y, w, h in letter_rects(size, letter):
        for row in range(y, y + h):
            pixels[row][x:x + w] = [ink] * w
    return pixels


# --- File formats -------------------------------------------------------------

def png_bytes(pixels):
    height, width = len(pixels), len(pixels[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def read_png(path):
    """Decode an 8-bit RGBA, non-interlaced PNG (all the app logo PNGs are)."""
    data = open(path, "rb").read()
    pos, idat = 8, b""
    while pos < len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if (depth, colour) != (8, 6):
                raise ValueError(f"{path}: not 8-bit RGBA")
        elif tag == b"IDAT":
            idat += body
    raw, stride = zlib.decompress(idat), width * 4
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x:x + 4]) for x in range(0, stride, 4)])
        prev = line
    return rows


def dib_bytes(pixels):
    """An icon directory entry's 32-bit BMP: BGRA bottom-up, then an empty AND mask."""
    size = len(pixels)
    colour = b"".join(bytes((b, g, r, a)) for row in reversed(pixels) for r, g, b, a in row)
    mask = b"\x00" * (((size + 31) // 32) * 4 * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         len(colour) + len(mask), 0, 0, 0, 0)
    return header + colour + mask


def ico_bytes(images):
    """images: [(size, pixels)]. 128 and 256 px are stored as PNG, as in the
    app icon; smaller sizes as BMP, which every icon reader understands."""
    blobs = [(size, png_bytes(px) if size >= 128 else dib_bytes(px)) for size, px in images]
    out = struct.pack("<HHH", 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    for size, blob in blobs:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    return out + b"".join(blob for _, blob in blobs)


# --- Preview ------------------------------------------------------------------

def preview(path, zoom):
    """Every type at every small size, on a light and a dark background, with
    the app icon (which image files use) above for comparison."""
    sizes = [n for n in ICO_SIZES if n <= 64]
    pad = 8
    rows = []
    logo = []
    for n in sizes:
        p = os.path.join(APP_LOGO_DIR, f"{n}.png")
        logo.append(read_png(p) if os.path.exists(p) else None)  # no 20/40 px logo
    rows.append(logo)
    for tile, letter in TILES.values():
        rows.append([draw(n, tile, letter) for n in sizes])

    col_w = [n * zoom for n in sizes]
    row_h = max(sizes) * zoom
    half_w = sum(col_w) + pad * (len(sizes) + 1)
    width, height = half_w * 2, row_h * len(rows) + pad * (len(rows) + 1)
    sheet = [[rgba("#ffffff")] * half_w + [rgba("#202020")] * half_w for _ in range(height)]
    for half in (0, 1):
        for r, icons in enumerate(rows):
            x = half * half_w + pad
            y = pad + r * (row_h + pad)
            for n, px in zip(sizes, icons):
                if px is not None:
                    for yy in range(n * zoom):
                        src = px[yy // zoom]
                        sheet[y + yy][x:x + n * zoom] = [src[xx // zoom] for xx in range(n * zoom)]
                x += n * zoom + pad
    with open(path, "wb") as f:
        f.write(png_bytes(sheet))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--preview", metavar="PNG", help="also write a preview sheet here")
    parser.add_argument("--zoom", type=int, default=3, help="preview magnification (default 3)")
    args = parser.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    for name, (tile, letter) in TILES.items():
        path = os.path.join(OUT_DIR, f"{name}.ico")
        with open(path, "wb") as f:
            f.write(ico_bytes([(n, draw(n, tile, letter)) for n in ICO_SIZES]))
        print(f"wrote {os.path.relpath(path, ROOT)}")
    if args.preview:
        preview(args.preview, args.zoom)
        print(f"wrote {args.preview}")


if __name__ == "__main__":
    main()
