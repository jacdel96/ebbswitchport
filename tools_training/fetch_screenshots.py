#!/usr/bin/env python3
"""Download native-resolution SNES in-game screenshots from VGMuseum for the
SR training set.

Unlike the VGMaps map rips (fetch_vgmaps.py), these are real gameplay
captures — sprites, HUDs, text boxes, Mode 7 — i.e. exactly what the
network upscaler sees at runtime.  Every image is validated to be a native
SNES framebuffer size (256xH or 512xH hires) before it is kept; anything
scaled, cropped, or box-art is discarded.

Usage:  <python-with-pillow> tools_training/fetch_screenshots.py \
            [--max-per-game N] [--games key1,key2]

Output: training_data/screenshots/<game>/<name>.png  (RGB PNG, native res)
"""
import argparse
import html
import io
import pathlib
import re
import sys
import time
import urllib.parse
import urllib.request

from PIL import Image

BASE = "https://www.vgmuseum.com/images/"
REPO = pathlib.Path(__file__).resolve().parent.parent
OUT = REPO / "training_data" / "screenshots"

# game key -> list of VGMuseum screenshot pages (relative to BASE).
# Multiple pages = regional variants; 404s are skipped gracefully.
GAMES = {
    "super_mario_world": ["snes/01/smw.html"],
    "yoshis_island": ["snes/01/smw2.html"],
    "super_metroid": ["snes/01/metroid3.html"],
    "zelda_alttp": ["snes/01/zelda3.html"],
    "earthbound": ["snes/01/earthbound.html"],
    "final_fantasy_iv": ["snes/01/ff2.html"],       # US "Final Fantasy II"
    "final_fantasy_vi": ["snes/01/ff3.html"],       # US "Final Fantasy III"
    "dragon_quest_v": ["snes/01/dragonquest5.html"],
    "chrono_trigger": ["snes/01/chrono.html", "snes/02/chronoj.html"],
    "tmnt4_turtles_in_time": ["snes/01/tmnt4.html"],
    "dkc2": ["snes/01/dkc2.html"],
    "mega_man_x": ["snes/01/megamanx.html"],
    "super_mario_kart": ["snes/01/kart.html"],
    "street_fighter_2_turbo": ["snes/01/SF2TSNES.html"],
    "secret_of_mana": ["snes/01/mana.html"],
}

UA = {"User-Agent": "Mozilla/5.0 (personal SR training data fetch; low volume)"}
DELAY_S = 0.4          # politeness gap between requests
MAX_BYTES = 4 * 1024 * 1024

# Native SNES framebuffer widths/heights (incl. PAL overscan and hires mode).
VALID_W = {256: range(200, 241), 512: range(400, 481)}


def fetch(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read(MAX_BYTES)


def page_images(page_url, body):
    """img srcs + direct <a href> image links, resolved against the page."""
    refs = re.findall(r'(?:src|href)\s*=\s*["\']([^"\']+\.(?:png|gif))["\']',
                      html.unescape(body), re.IGNORECASE)
    seen, out = set(), []
    for r in refs:
        u = urllib.parse.urljoin(page_url, r)
        if u not in seen:
            seen.add(u)
            out.append(u)
    return out


def native_size(img):
    r = VALID_W.get(img.width)
    return r is not None and img.height in r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-per-game", type=int, default=80)
    ap.add_argument("--games", help="comma-separated subset of game keys")
    args = ap.parse_args()

    keys = args.games.split(",") if args.games else list(GAMES)
    unknown = [k for k in keys if k not in GAMES]
    if unknown:
        sys.exit(f"unknown game keys: {unknown}; known: {sorted(GAMES)}")

    grand = 0
    for key in keys:
        gdir = OUT / key
        gdir.mkdir(parents=True, exist_ok=True)
        kept = rejected = 0
        for page in GAMES[key]:
            page_url = urllib.parse.urljoin(BASE, page)
            try:
                body = fetch(page_url).decode("utf-8", "replace")
            except Exception as e:
                print(f"[{key}] page {page}: {e} — skipped")
                continue
            time.sleep(DELAY_S)
            for img_url in page_images(page_url, body):
                if kept >= args.max_per_game:
                    break
                name = pathlib.Path(urllib.parse.urlparse(img_url).path).stem + ".png"
                dst = gdir / name
                if dst.exists():
                    kept += 1
                    continue
                try:
                    raw = fetch(img_url)
                    img = Image.open(io.BytesIO(raw))
                    img.load()
                except Exception as e:
                    print(f"[{key}] {img_url}: {e}")
                    time.sleep(DELAY_S)
                    continue
                time.sleep(DELAY_S)
                if not native_size(img):
                    rejected += 1
                    continue
                img.convert("RGB").save(dst)
                kept += 1
        grand += kept
        print(f"[{key}] kept {kept}, rejected {rejected} (non-native size)")
    print(f"done: {grand} screenshots under {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
