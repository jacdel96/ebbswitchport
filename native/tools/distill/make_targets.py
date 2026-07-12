#!/usr/bin/env python3
"""Batch-convert native-resolution frame PNGs (LR) into teacher-upscaled
target PNGs (HR, exactly --scale times larger) for student distillation.

    python make_targets.py --frames DIR --out DIR --teacher xbrz

Teachers (see teachers.py): xbrz (primary), bicubic (baseline), scalefx
(manual/GLSL — stub), artcnn (x2 ONNX + resample, comparison only).

--manifest CSV ("filename,teacher" per line, no header) overrides the
teacher per image — this is the hook for the upcoming per-image teacher
review app: the app writes the manifest, this script executes it.

Inputs are RGB PNGs of any size (frame dumps, tiled map rips, synthetic
frames); teachers run on RGB.  Luma conversion happens later, in training.
"""

import argparse
import csv
import os
import sys
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from teachers import get_teacher, teacher_help, teacher_names  # noqa: E402


def iter_pngs(d):
    """Relative paths of all PNGs under d (recursive — supports both flat
    frame dirs and the per-game training_data/screenshots layout)."""
    out = []
    for root, _, files in os.walk(d):
        for f in files:
            if f.lower().endswith(".png"):
                out.append(os.path.relpath(os.path.join(root, f), d))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", required=True, help="dir of LR input PNGs")
    ap.add_argument("--out", required=True, help="dir for HR target PNGs (same filenames)")
    ap.add_argument("--teacher", default="xbrz", choices=teacher_names(),
                    help=teacher_help())
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--manifest", help="CSV filename,teacher — per-image teacher override")
    ap.add_argument("--artcnn-onnx", help="path to ArtCNN ONNX model (teacher=artcnn)")
    ap.add_argument("--overwrite", action="store_true",
                    help="regenerate targets that already exist")
    args = ap.parse_args()

    overrides = {}
    if args.manifest:
        with open(args.manifest, newline="") as f:
            for row in csv.reader(f):
                if len(row) >= 2 and row[0].strip():
                    key = row[0].strip().replace("\\", "/")
                    overrides[key] = row[1].strip()

    opts = {"artcnn_onnx": args.artcnn_onnx}
    teachers = {args.teacher: get_teacher(args.teacher, **opts)}

    os.makedirs(args.out, exist_ok=True)
    names = iter_pngs(args.frames)
    if not names:
        raise SystemExit(f"no .png files in {args.frames}")

    done = skipped = 0
    t0 = time.perf_counter()
    for name in names:
        dst = os.path.join(args.out, name)
        if not args.overwrite and os.path.exists(dst):
            skipped += 1
            continue
        rel = name.replace("\\", "/")
        # manifest matches on relative path first, bare filename second
        tname = overrides.get(rel, overrides.get(os.path.basename(rel), args.teacher))
        if tname == "exclude":   # review-app verdict: don't train on this image
            skipped += 1
            continue
        if tname not in teachers:
            teachers[tname] = get_teacher(tname, **opts)
        img = Image.open(os.path.join(args.frames, name)).convert("RGB")
        out = teachers[tname](img, args.scale)
        expect = (img.width * args.scale, img.height * args.scale)
        if out.size != expect:
            raise SystemExit(f"{name}: teacher {tname} produced {out.size}, expected {expect}")
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        out.save(dst)
        done += 1
        if done % 50 == 0:
            rate = done / (time.perf_counter() - t0)
            print(f"  {done}/{len(names) - skipped} ({rate:.1f} img/s)")
    dt = time.perf_counter() - t0
    print(f"[targets] {done} written, {skipped} skipped (existing) in {dt:.1f}s "
          f"-> {args.out} (teacher={args.teacher}"
          + (f", {len(overrides)} manifest overrides" if overrides else "") + ")")


if __name__ == "__main__":
    main()
