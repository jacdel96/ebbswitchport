#!/bin/bash
# Copy the built native NROs onto the Switch microSD card.
#
# Usage: scripts/deploy_sd.sh /Volumes/<SD-CARD-NAME>
#
# Each NRO is fully self-contained (frontend + snes9x core + embedded ROM),
# so this just drops native/build-out/*.nro into the SD's /switch/ folder —
# launch them from the Homebrew Menu. Saves are written on-console to
# sdmc:/switch/ebbswitchport/.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:-}"

if [[ -z "$SD" || ! -d "$SD" ]]; then
    echo "Usage: $0 /Volumes/<SD-CARD-NAME>" >&2
    echo "Mounted volumes:" >&2
    ls /Volumes >&2
    exit 1
fi
if [[ ! -d "$SD/Nintendo" && ! -d "$SD/atmosphere" ]]; then
    echo "WARNING: $SD does not look like a Switch SD card (no Nintendo/ or atmosphere/ folder)." >&2
    read -r -p "Continue anyway? [y/N] " ans
    [[ "$ans" == "y" || "$ans" == "Y" ]] || exit 1
fi

echo "==> Native NROs -> $SD/switch/"
mkdir -p "$SD/switch"
found=0
for nro in "$REPO/native/build-out/"*.nro; do
    [[ -e "$nro" ]] || continue
    found=1
    rsync -t --progress "$nro" "$SD/switch/"
done
if [[ "$found" == 0 ]]; then
    echo "  (no NROs in native/build-out/ — run scripts/build_native.sh first)" >&2
    exit 1
fi

echo
echo "Done. Eject the card cleanly:  diskutil eject '$SD'"
echo "Then on the Switch: Homebrew Menu -> launch each game from /switch/."
