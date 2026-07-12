#!/usr/bin/env python3
"""Download native-resolution SNES map rips from VGMaps for the training set.

Maps are full-screen-accurate pixel art at native resolution, ideal for
tiling into 256x224 pseudo-frames. Downloads are rate-limited and capped
per game to be polite to vgmaps.com.

Usage:  .venv/bin/python tools_training/fetch_vgmaps.py [--max-per-game N]
"""
import argparse
import html
import pathlib
import re
import sys
import time
import urllib.request

ATLAS = "https://www.vgmaps.com/Atlas/SuperNES/"
REPO = pathlib.Path(__file__).resolve().parent.parent
RAW = REPO / "training_data" / "raw"

# game key -> list of filename-prefix regexes to match against PNG hrefs
GAMES = {
    "earthbound": [r"EarthBound-"],
    "final_fantasy_vi": [r"FinalFantasyVI-", r"FinalFantasyIII-"],
    "chrono_trigger": [r"ChronoTrigger-"],
    "zelda_alttp": [r"LegendOfZelda[^\"]*LinkToThePast", r"ALinkToThePast"],
    "super_metroid": [r"SuperMetroid-"],
    "super_mario_world": [r"SuperMarioWorld-"],
    "dkc2": [r"DonkeyKongCountry2"],
    "kirby_super_star": [r"Kirby-SuperStar"],
    "mario_and_wario": [r"Mario(&|And|%26)Wario"],
    "dragon_quest_v": [r"DragonQuestV\("],
    "shin_megami_tensei": [r"ShinMegamiTensei"],
    "mega_man_x": [r"MegaManX-"],
    "super_mario_kart": [r"^SuperMarioKart-"],
    "tmnt4_turtles_in_time": [r"TeenageMutantNinjaTurtles(IV|.*TurtlesInTime)"],
    "street_fighter_2_turbo": [r"StreetFighterII"],
    "super_mario_allstars": [r"SuperMarioAll-?Stars"],
    "super_mario_rpg": [r"SuperMarioRPG"],
    "secret_of_mana": [r"SecretOfMana"],
    "yoshis_island": [r"SuperMarioWorld2"],
}

MAX_BYTES = 30 * 1024 * 1024  # skip pathological map files
UA = {"User-Agent": "Mozilla/5.0 (personal SR training data fetch; low volume)"}


def fetch(url: str) -> bytes:
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-per-game", type=int, default=12)
    args = ap.parse_args()

    print("fetching atlas index ...")
    index = fetch(ATLAS + "index.htm").decode("utf-8", "replace")
    hrefs = sorted(set(re.findall(r'href="([^"]+\.(?:png|PNG))"', index)))
    print(f"  {len(hrefs)} map links on index")

    manifest = []
    for game, patterns in GAMES.items():
        matches = [h for h in hrefs if any(re.search(p, h) for p in patterns)]
        outdir = RAW / game
        outdir.mkdir(parents=True, exist_ok=True)
        if not matches:
            print(f"[{game}] NO MATCHES on vgmaps index")
            continue
        # spread picks across the game's maps instead of taking the first N
        # (first N tends to be one town/area only)
        step = max(1, len(matches) // args.max_per_game)
        picks = matches[::step][: args.max_per_game]
        print(f"[{game}] {len(matches)} maps, downloading {len(picks)}")
        for h in picks:
            name = pathlib.Path(html.unescape(h)).name
            dest = outdir / name
            if dest.exists():
                print(f"    skip (have) {name}")
                continue
            url = ATLAS + urllib.request.quote(html.unescape(h))
            try:
                data = fetch(url)
            except Exception as e:  # noqa: BLE001
                print(f"    FAIL {name}: {e}")
                continue
            if len(data) > MAX_BYTES:
                print(f"    skip (too big, {len(data)//2**20}MB) {name}")
                continue
            dest.write_bytes(data)
            manifest.append(f"{game}\t{name}\t{url}")
            print(f"    got {name} ({len(data)//1024}KB)")
            time.sleep(1.0)  # be polite

    (RAW / "MANIFEST.tsv").open("a").write("\n".join(manifest) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
