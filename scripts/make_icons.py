#!/usr/bin/env python3
"""Build the three 256x256 JPEG home-screen icons from assets/art-src/.

Usage: .venv/bin/python scripts/make_icons.py

Inputs (assets/art-src/):
  earthbound_box.jpg        -> EarthBound.jpg (center square crop)
                            -> EarthBound Giygas Strikes Back.jpg (red-shifted variant)
  eb_beginnings_cover.png   -> EarthBound Beginnings Remake.jpg (center square crop)

Switch icons must be exactly 256x256 baseline JPEG, no EXIF.
"""

from pathlib import Path

from PIL import Image, ImageEnhance

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "assets" / "art-src"
OUT = REPO / "assets" / "icons"

ICON_SIZE = (256, 256)


def square_crop(img: Image.Image) -> Image.Image:
    w, h = img.size
    side = min(w, h)
    left, top = (w - side) // 2, (h - side) // 2
    return img.crop((left, top, left + side, top + side))


def save_icon(img: Image.Image, name: str) -> None:
    icon = square_crop(img.convert("RGB")).resize(ICON_SIZE, Image.LANCZOS)
    path = OUT / f"{name}.jpg"
    icon.save(path, "JPEG", quality=92, exif=b"")
    print(f"wrote {path.relative_to(REPO)}")


def giygas_variant(img: Image.Image) -> Image.Image:
    """Dark red-shifted take on the box art so the QoL hack is distinguishable."""
    rgb = img.convert("RGB")
    r, g, b = rgb.split()
    shifted = Image.merge("RGB", (r, g.point(lambda v: v * 0.35), b.point(lambda v: v * 0.35)))
    return ImageEnhance.Contrast(shifted).enhance(1.15)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    eb_box = Image.open(SRC / "earthbound_box.jpg")
    save_icon(eb_box, "EarthBound")
    save_icon(giygas_variant(eb_box), "EarthBound Giygas Strikes Back")
    save_icon(Image.open(SRC / "eb_beginnings_cover.png"), "EarthBound Beginnings Remake")


if __name__ == "__main__":
    main()
