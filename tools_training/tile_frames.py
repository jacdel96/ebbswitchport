#!/usr/bin/env python3
"""Tile downloaded map rips into native 256x224 pseudo-frames.

Crops are filtered so we keep visually busy, representative tiles
(no empty void / flat letterboxing), and sampled evenly across each
game's maps up to a per-game cap.

Usage:  .venv/bin/python tools_training/tile_frames.py [--max-per-game N]
"""
import argparse
import pathlib
import random
import sys

import numpy as np
from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
RAW = REPO / "training_data" / "raw"
FRAMES = REPO / "training_data" / "frames"

W, H = 256, 224
MIN_UNIQUE_COLORS = 12   # reject flat/void tiles
MIN_LUMA_STD = 10.0      # reject near-uniform tiles
Image.MAX_IMAGE_PIXELS = None  # map rips are huge but trusted


def crop_candidates(img: Image.Image):
    """Yield (x, y, score) for grid-aligned crops that look like real frames."""
    arr = np.asarray(img, dtype=np.uint8)
    h, w = arr.shape[:2]
    for y in range(0, h - H + 1, H):
        for x in range(0, w - W + 1, W):
            tile = arr[y : y + H, x : x + W]
            luma = tile @ np.array([0.299, 0.587, 0.114])
            if luma.std() < MIN_LUMA_STD:
                continue
            colors = len(np.unique(tile.reshape(-1, 3), axis=0))
            if colors < MIN_UNIQUE_COLORS:
                continue
            yield x, y, colors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-per-game", type=int, default=30)
    ap.add_argument("--seed", type=int, default=1234)
    args = ap.parse_args()
    rng = random.Random(args.seed)

    total = 0
    for gamedir in sorted(p for p in RAW.iterdir() if p.is_dir()):
        game = gamedir.name
        outdir = FRAMES / game
        outdir.mkdir(parents=True, exist_ok=True)
        per_map: dict[str, list] = {}
        for png in sorted(gamedir.glob("*.png")):
            try:
                img = Image.open(png).convert("RGB")
            except Exception as e:  # noqa: BLE001
                print(f"[{game}] unreadable {png.name}: {e}")
                continue
            cands = list(crop_candidates(img))
            if not cands:
                continue
            rng.shuffle(cands)
            # prefer busier tiles but keep randomness
            cands.sort(key=lambda c: -c[2])
            per_map[png.stem] = [(png, x, y) for x, y, _ in cands[:20]]

        # round-robin across maps so one big map doesn't dominate
        picked = []
        while len(picked) < args.max_per_game and any(per_map.values()):
            for stem in list(per_map):
                if per_map[stem]:
                    picked.append(per_map[stem].pop(0))
                    if len(picked) >= args.max_per_game:
                        break

        cache: dict[pathlib.Path, Image.Image] = {}
        for png, x, y in picked:
            if png not in cache:
                cache[png] = Image.open(png).convert("RGB")
            tile = cache[png].crop((x, y, x + W, y + H))
            name = f"{png.stem}__{x}_{y}.png".replace(" ", "_")
            tile.save(outdir / name, optimize=True)
        total += len(picked)
        print(f"[{game}] {len(picked)} frames")
    print(f"total frames: {total}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
