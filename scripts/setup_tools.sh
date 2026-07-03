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

# Source is vendored in-repo (upstream The-4n/hacBrewPack no longer exists;
# this is the final v3.05, from the dragonflylee mirror fork).
SRC="$REPO/vendor/hacBrewPack"

[[ -f "$SRC/config.mk" ]] || cp "$SRC/config.mk.template" "$SRC/config.mk"
make -C "$SRC"
mkdir -p "$TOOLS/bin"
cp "$SRC/hacbrewpack" "$TOOLS/bin/hacbrewpack"
echo "Built tools/bin/hacbrewpack"
"$TOOLS/bin/hacbrewpack" --help 2>&1 | head -3 || true
