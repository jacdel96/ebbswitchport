#!/bin/bash
# Build one self-contained .nro per game: the libnx frontend + snes9x core
# with that game's ROM embedded in romfs. Output: native/build-out/<name>.nro
# (gitignored — embeds the ROM).
#
# Prereq: scripts/build_core.sh has produced native/lib/libsnes9x_libretro_libnx.a
# and roms/*.sfc exist (scripts/apply_patches.py). DEVKITPRO set.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
: "${DEVKITPRO:?Set DEVKITPRO (e.g. export DEVKITPRO=/opt/devkitpro).}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"

NATIVE="$REPO/native"
OUT="$NATIVE/build-out"
mkdir -p "$OUT"

if [[ ! -f "$NATIVE/lib/libsnes9x_libretro_libnx.a" ]]; then
    echo "Missing native/lib/libsnes9x_libretro_libnx.a — run scripts/build_core.sh first." >&2
    exit 1
fi

# name | rom+icon basename (in roms/ and assets/icons/) | game_id (save basename) | app title
GAMES=(
    "EarthBound|earthbound|EarthBound"
    "EarthBound Beginnings Remake|eb_beginnings|EarthBound Beginnings Remake"
    "EarthBound Giygas Strikes Back|eb_giygas|EarthBound: Giygas Strikes Back"
)

for entry in "${GAMES[@]}"; do
    IFS='|' read -r name game_id title <<< "$entry"
    rompath="$REPO/roms/$name.sfc"
    iconpath="$REPO/assets/icons/$name.jpg"
    if [[ ! -f "$rompath" ]]; then
        echo "  skip '$name' — missing roms/$name.sfc (run apply_patches.py / drop base ROM)" >&2
        continue
    fi
    echo "==> building '$name'"
    # Swap the embedded ROM: frontend always loads romfs:/game.sfc.
    cp "$rompath" "$NATIVE/romfs/game.sfc"
    # Stage the icon to a space-free path (elf2nro's --icon arg is unquoted).
    cp "$iconpath" "$NATIVE/.icon.jpg"
    make -C "$NATIVE" clean >/dev/null
    make -C "$NATIVE" \
        GAME_ID="$game_id" \
        APP_TITLE="$title" \
        APP_AUTHOR="ebbswitchport" \
        APP_ICON="$NATIVE/.icon.jpg" \
        -j"$(sysctl -n hw.ncpu)"
    cp "$NATIVE/native.nro" "$OUT/$name.nro"
    echo "   -> native/build-out/$name.nro"
done

rm -f "$NATIVE/romfs/game.sfc" "$NATIVE/.icon.jpg"
echo "Done. NROs in native/build-out/ (gitignored)."
