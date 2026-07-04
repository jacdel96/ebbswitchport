#!/bin/bash
# Build the snes9x libretro core as a static lib for Switch (libnx),
# then drop it into native/lib/ for the native NRO Makefile to link.
#
# Prereq: devkitPro + switch-dev installed, DEVKITPRO set.
# Run once (and after updating the core). ~2-3 min.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
: "${DEVKITPRO:?Set DEVKITPRO (e.g. export DEVKITPRO=/opt/devkitpro). Run scripts/setup_devkitpro note in README.}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"

SRC="$REPO/native/vendor/snes9x"
LIBOUT="$REPO/native/lib"
mkdir -p "$LIBOUT"

if [[ ! -d "$SRC/.git" ]]; then
    echo "==> cloning snes9x"
    git clone --depth 1 https://github.com/snes9xgit/snes9x "$SRC"
fi

echo "==> building snes9x_libretro_libnx.a"
make -C "$SRC/libretro" platform=libnx -j"$(sysctl -n hw.ncpu)"

# The libretro Makefile emits snes9x_libretro_libnx.a in libretro/.
cp "$SRC/libretro/snes9x_libretro_libnx.a" "$LIBOUT/libsnes9x_libretro_libnx.a"
echo "==> native/lib/libsnes9x_libretro_libnx.a ready ($(du -h "$LIBOUT/libsnes9x_libretro_libnx.a" | cut -f1))"
