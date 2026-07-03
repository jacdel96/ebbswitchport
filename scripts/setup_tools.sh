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
    git clone --depth 1 https://github.com/The-4n/hacBrewPack "$TOOLS/hacBrewPack"
fi

make -C "$TOOLS/hacBrewPack"
mkdir -p "$TOOLS/bin"
cp "$TOOLS/hacBrewPack/hacbrewpack" "$TOOLS/bin/hacbrewpack"
echo "Built tools/bin/hacbrewpack"
"$TOOLS/bin/hacbrewpack" --help 2>&1 | head -3 || true
