#!/usr/bin/env python3
"""Automatic per-image teacher selection for SR distillation targets.

Decision procedure (calibrated 2026-07 by visually comparing all candidate
teachers on zoomed crops for every game's art style, few-shot guided by the
reviewer's hand labels in teacher_manifest.csv, and by their stated criteria):

  1. NEVER pick "original" — a transformation must be chosen.
  2. Don't destroy the pixel look: oversmoothing that melts legible texture
     (xBRZ on dithered/gritty art) is disqualifying.
  3. Highest score to what captures the spirit of the original art style.
  4. No artificial brightness/darkness changes (all three candidates are
     mean-preserving in practice; ArtCNN was spot-checked).

The strongest predictor of the right teacher turned out to be the GAME's
art style, not per-image statistics (numeric features like flatness/dither
do not separate hand-drawn-smooth from gritty-textured styles reliably):

  * xBRZ where the art is flat-color / hand-drawn / pre-rendered-smooth:
    clean outlines and text round beautifully, nothing textured to melt
    (SMW, Yoshi's Island, DKC2, Mega Man X, ALttP, EarthBound, Secret of
    Mana, TMNT4, Mario Kart).
  * ArtCNN where the art is dense/textured/dither-shaded and xBRZ visibly
    melts it into blobs (Chrono Trigger — matches the reviewer's own
    labels, FF4/FF6, Super Metroid, SF2 Turbo, DQ5).
  * Per-image guard: inside xBRZ games, frames with heavy 2x2 dithering
    flip to ArtCNN (criterion 2).
  * bicubic is never auto-picked (the reviewer chose it once out of 26 —
    a translucency-heavy map — not enough signal to automate; the audit
    pass can correct individual frames).

Existing manifest rows (the human's) are never overwritten.  Prints a
tally at the end.

Usage:
    auto_label.py [--frames DIR] [--manifest CSV] [--dry-run]
"""

import argparse
import csv
import pathlib
import sys

import numpy as np
from PIL import Image

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "native" / "tools" / "distill"))

# game dir -> default teacher (see module docstring for the reasoning)
GAME_POLICY = {
    "super_mario_world": "xbrz",
    "yoshis_island": "xbrz",
    "dkc2": "xbrz",
    "mega_man_x": "xbrz",
    # zelda_alttp started as xbrz; the human audit overruled BOTH sampled
    # frames to artcnn (speckled cliff texture + HUD numerals melt) — flipped.
    "zelda_alttp": "artcnn",
    "earthbound": "xbrz",
    "secret_of_mana": "xbrz",
    "tmnt4_turtles_in_time": "xbrz",
    "super_mario_kart": "xbrz",
    "chrono_trigger": "artcnn",
    "final_fantasy_iv": "artcnn",
    "final_fantasy_vi": "artcnn",
    "super_metroid": "artcnn",
    "street_fighter_2_turbo": "artcnn",
    "dragon_quest_v": "artcnn",
}
DEFAULT = "artcnn"          # unknown games: the reviewer's dominant pick
DITHER_FLIP = 0.030         # 2x2-checker fraction above which xbrz would melt


def dither_score(img):
    p = np.asarray(img.convert("RGB"), dtype=np.int32)
    d = ((p[0:-1:2, 0:-1:2] == p[1::2, 1::2]).all(-1)
         & (p[0:-1:2, 1::2] == p[1::2, 0:-1:2]).all(-1)
         & (~(p[0:-1:2, 0:-1:2] == p[0:-1:2, 1::2]).all(-1)))
    return float(d.mean())


def is_line_art(img):
    """Thin-line-art detector (audit finding: the DKC2 Rareware wireframe
    logo must not go to xBRZ, which vectorizes 1px lines into smooth
    curves).  Line-art frames = sparse foreground that is almost entirely
    edge pixels.  Thick-stroke text over scenes (EarthBound dialogue —
    audit-approved as xbrz) has a much larger fill fraction and passes."""
    a = np.asarray(img.convert("RGB"), dtype=np.int32)
    lum = 0.299 * a[..., 0] + 0.587 * a[..., 1] + 0.114 * a[..., 2]
    vals, counts = np.unique(a.reshape(-1, 3), axis=0, return_counts=True)
    fg = 1.0 - counts.max() / counts.sum()
    if fg > 0.35 or fg < 0.01:
        return False
    gy, gx = np.gradient(lum)
    edge = (np.abs(gy) + np.abs(gx)) > 24
    return float(edge.mean()) / fg > 0.8


def choose(rel, img):
    game = rel.split("/", 1)[0] if "/" in rel else ""
    pick = GAME_POLICY.get(game, DEFAULT)
    if pick == "xbrz":
        if dither_score(img) > DITHER_FLIP:
            return "artcnn", "dither-guard"
        if is_line_art(img):
            return "artcnn", "line-art-guard"
    return pick, "game-style"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--frames", default=str(REPO / "training_data" / "screenshots"))
    ap.add_argument("--manifest", default=str(REPO / "training_data" / "teacher_manifest.csv"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    root = pathlib.Path(args.frames)
    images = sorted(str(p.relative_to(root)) for p in root.rglob("*.png"))

    manifest = pathlib.Path(args.manifest)
    existing = {}
    if manifest.exists():
        with open(manifest, newline="") as f:
            for row in csv.reader(f):
                if len(row) >= 2:
                    existing[row[0]] = row[1]

    todo = [r for r in images if r not in existing]
    print(f"[auto] {len(images)} images, {len(existing)} human rows kept, "
          f"{len(todo)} to label")

    new, reasons = {}, {"dither-guard": 0, "line-art-guard": 0}
    for rel in todo:
        img = Image.open(root / rel)
        pick, why = choose(rel, img)
        new[rel] = pick
        if why in reasons:
            reasons[why] += 1

    tally = {}
    for v in list(existing.values()) + list(new.values()):
        tally[v] = tally.get(v, 0) + 1
    print("[auto] tally (incl. human rows):",
          ", ".join(f"{k}={v}" for k, v in sorted(tally.items(), key=lambda x: -x[1])))
    print(f"[auto] guard flips to artcnn inside xbrz games: "
          f"dither={reasons['dither-guard']}, line-art={reasons['line-art-guard']}")
    per_game = {}
    for rel, v in {**existing, **new}.items():
        g = rel.split("/", 1)[0]
        per_game.setdefault(g, {}).setdefault(v, 0)
        per_game[g][v] += 1
    for g in sorted(per_game):
        print(f"    {g:24s} " + ", ".join(f"{k}={v}" for k, v in sorted(per_game[g].items())))

    if args.dry_run:
        return
    merged = dict(existing)
    merged.update(new)
    with open(manifest, "w", newline="") as f:
        wr = csv.writer(f)
        for k in sorted(merged):
            wr.writerow([k, merged[k]])
    # provenance sidecar: which rows were auto-labeled (vs human) — the
    # audit app samples only from these
    side = manifest.with_name("auto_labeled.txt")
    prior_auto = set()
    if side.exists():
        prior_auto = {l.strip() for l in side.read_text().splitlines() if l.strip()}
    prior_auto |= set(new)
    side.write_text("\n".join(sorted(r for r in prior_auto if r in merged)) + "\n")
    print(f"[auto] wrote {len(merged)} rows -> {manifest} "
          f"({len(prior_auto)} marked auto in {side.name})")


if __name__ == "__main__":
    main()
