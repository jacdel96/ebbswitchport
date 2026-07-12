#!/usr/bin/env python3
"""Generate pixel-art-like synthetic SNES frames (256x224 RGB PNGs) so the
distillation pipeline can be smoke-tested before real frame dumps / ripped
game art exist.

    python make_synthetic.py --out DIR [--count 50] [--seed 0]

Frame styles (each drawn from a limited palette of <=64 colors):
  tiles      repeating 8x8/16x16 tile patterns with per-tile variation
  dither     checkerboard/Bayer-style dithering between palette pairs
  sprites    blobby outlined sprite-like shapes over a flat/tiled backdrop
  textbox    dialog rectangles with 8x8 glyph-like bit patterns inside
"""

import argparse
import os

import numpy as np
from PIL import Image

W, H = 256, 224


def make_palette(rng, n):
    """<=64 saturated-ish colors + a dark outline color + a light color."""
    base = rng.integers(0, 8, size=(n, 3)) * 36  # coarse steps, SNES-ish
    base[0] = (16, 16, 24)     # outline/dark
    base[1] = (240, 240, 224)  # light
    return base.astype(np.uint8)


def tile_frame(rng, pal):
    ts = int(rng.choice([8, 16]))
    tile = rng.integers(2, len(pal), size=(ts, ts))
    # give the tile some structure: mirror half of it
    tile[:, ts // 2:] = tile[:, : ts // 2][:, ::-1]
    idx = np.tile(tile, (H // ts + 1, W // ts + 1))[:H, :W].copy()
    # sprinkle variant tiles
    for _ in range(rng.integers(4, 12)):
        ty, tx = rng.integers(0, H // ts), rng.integers(0, W // ts)
        idx[ty * ts:(ty + 1) * ts, tx * ts:(tx + 1) * ts] = rng.integers(2, len(pal))
    return idx


def dither_frame(rng, pal):
    idx = np.zeros((H, W), dtype=np.int64)
    n_bands = rng.integers(3, 7)
    edges = np.sort(rng.integers(0, H, size=n_bands - 1))
    yy, xx = np.mgrid[0:H, 0:W]
    checker = (yy + xx) % 2
    bayer = ((yy % 2) * 2 + (xx % 2))  # 0..3 mini-Bayer
    y0 = 0
    for e in list(edges) + [H]:
        a, b = rng.integers(2, len(pal), size=2)
        style = rng.integers(0, 3)
        band = slice(y0, e)
        if style == 0:      # hard fill
            idx[band] = a
        elif style == 1:    # 50% checker dither
            idx[band] = np.where(checker[band] == 0, a, b)
        else:               # 25/75 Bayer dither
            idx[band] = np.where(bayer[band] == 0, a, b)
        y0 = e
    return idx


def sprite_frame(rng, pal):
    idx = tile_frame(rng, pal) if rng.random() < 0.5 else \
        np.full((H, W), int(rng.integers(2, len(pal))), dtype=np.int64)
    yy, xx = np.mgrid[0:H, 0:W]
    for _ in range(rng.integers(5, 14)):
        cy, cx = rng.integers(24, H - 24), rng.integers(24, W - 24)
        r = rng.integers(6, 20)
        # blobby: sum of two offset ellipses
        d1 = ((yy - cy) / r) ** 2 + ((xx - cx) / (r * rng.uniform(0.6, 1.4))) ** 2
        d2 = ((yy - cy - r // 2) / (r * 0.7)) ** 2 + ((xx - cx + r // 3) / (r * 0.7)) ** 2
        body = (d1 < 1) | (d2 < 1)
        # 1px-ish outline via dilation difference
        pad = np.pad(body, 1)
        dil = pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:] | body
        color = rng.integers(2, len(pal))
        idx[body] = color
        idx[dil & ~body] = 0  # dark outline
        # a couple of highlight pixels
        idx[cy - r // 3:cy - r // 3 + 2, cx - r // 3:cx - r // 3 + 2] = 1
    return idx


def glyph(rng):
    """8x8 glyph-like bit pattern: random strokes, vertically symmetric-ish."""
    g = np.zeros((8, 8), dtype=bool)
    for _ in range(rng.integers(2, 5)):
        if rng.random() < 0.5:
            r = rng.integers(1, 7)
            g[r, rng.integers(0, 3):rng.integers(5, 8)] = True
        else:
            c = rng.integers(1, 7)
            g[rng.integers(0, 3):rng.integers(5, 8), c] = True
    return g


def textbox_frame(rng, pal):
    idx = sprite_frame(rng, pal)
    # dialog box: dark fill, light 2px border
    x0, y0 = rng.integers(4, 24), rng.integers(120, 150)
    x1, y1 = W - x0, y0 + rng.integers(56, 72)
    idx[y0:y1, x0:x1] = 0
    idx[y0:y0 + 2, x0:x1] = 1
    idx[y1 - 2:y1, x0:x1] = 1
    idx[y0:y1, x0:x0 + 2] = 1
    idx[y0:y1, x1 - 2:x1] = 1
    # rows of glyphs
    gy = y0 + 8
    while gy + 8 < y1 - 8:
        gx = x0 + 10
        while gx + 8 < x1 - 10:
            if rng.random() < 0.85:
                idx[gy:gy + 8, gx:gx + 8][glyph(rng)] = 1
            gx += 9
        gy += 12
    return idx


STYLES = [tile_frame, dither_frame, sprite_frame, textbox_frame]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    for i in range(args.count):
        pal = make_palette(rng, int(rng.integers(12, 65)))
        style = STYLES[i % len(STYLES)]
        idx = style(rng, pal)
        rgb = pal[idx]
        name = f"syn_{i:03d}_{style.__name__.replace('_frame', '')}.png"
        Image.fromarray(rgb, "RGB").save(os.path.join(args.out, name))
    print(f"[synthetic] wrote {args.count} frames ({W}x{H}) to {args.out}")


if __name__ == "__main__":
    main()
