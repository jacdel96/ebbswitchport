#!/bin/bash
# Build hacbrewpack (NSP packer) from source into tools/bin/.
# Run once; scripts/build_forwarders.py uses the resulting binary.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$REPO/tools"
mkdir -p "$TOOLS"

if [[ -x "$TOOLS/bin/hacbrewpack" ]]; then
    echo "hacbrewpack already built at tools/bin/hacbrewpack"
    exit 0
fi

if [[ ! -d "$TOOLS/hacBrewPack" ]]; then
    # The-4n's original repo is gone; this fork is a pristine mirror at final v3.05.
    git clone --depth 1 --branch v3.05 https://github.com/dragonflylee/hacBrewPack "$TOOLS/hacBrewPack"
fi

[[ -f "$TOOLS/hacBrewPack/config.mk" ]] || cp "$TOOLS/hacBrewPack/config.mk.template" "$TOOLS/hacBrewPack/config.mk"
make -C "$TOOLS/hacBrewPack"
mkdir -p "$TOOLS/bin"
cp "$TOOLS/hacBrewPack/hacbrewpack" "$TOOLS/bin/hacbrewpack"
echo "Built tools/bin/hacbrewpack"
"$TOOLS/bin/hacbrewpack" --help 2>&1 | head -3 || true
