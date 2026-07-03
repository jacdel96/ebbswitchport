#!/bin/bash
# Copy everything the Switch needs onto the microSD card.
#
# Usage: scripts/deploy_sd.sh /Volumes/<SD-CARD-NAME>
#
# Copies:
#   sd-stage/retroarch/   -> SD /retroarch/        (RetroArch config, assets, cores)
#   sd-stage/switch/      -> SD /switch/           (RetroArch app, on-Switch forwarder generator)
#   roms/*.sfc            -> SD /roms/snes/        (base + patched ROMs)
#   assets/icons/*.jpg    -> SD /switch/icons/     (for the on-Switch generator UI)
#   out/*.nsp             -> SD /nsp/              (install these with DBI/Goldleaf)
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

echo "==> RetroArch + cores"
rsync -rt --progress "$REPO/sd-stage/retroarch/" "$SD/retroarch/"
rsync -rt "$REPO/sd-stage/switch/" "$SD/switch/"
[[ -f "$REPO/sd-stage/retroarch.jpg" ]] && cp "$REPO/sd-stage/retroarch.jpg" "$SD/retroarch.jpg"

echo "==> RetroArch seed config (only if none exists yet — won't clobber your settings)"
if [[ ! -f "$SD/retroarch/retroarch.cfg" ]]; then
    mkdir -p "$SD/retroarch"
    cp "$REPO/config/retroarch.cfg" "$SD/retroarch/retroarch.cfg"
    echo "  seeded retroarch.cfg (SRAM autosave, save/state paths)"
fi

echo "==> ROMs"
mkdir -p "$SD/roms/snes"
found_rom=0
for rom in "$REPO/roms/"*.sfc; do
    [[ -e "$rom" ]] || continue
    found_rom=1
    rsync -t "$rom" "$SD/roms/snes/"
done
[[ "$found_rom" == 1 ]] || echo "  (no ROMs in roms/ yet — run scripts/apply_patches.py first)"

echo "==> Icons (for the on-Switch forwarder generator)"
mkdir -p "$SD/switch/icons"
rsync -t "$REPO/assets/icons/"*.jpg "$SD/switch/icons/"

echo "==> NSP forwarders"
mkdir -p "$SD/nsp"
found_nsp=0
for nsp in "$REPO/out/"*.nsp; do
    [[ -e "$nsp" ]] || continue
    found_nsp=1
    rsync -t "$nsp" "$SD/nsp/"
done
[[ "$found_nsp" == 1 ]] || echo "  (no NSPs in out/ yet — run scripts/build_forwarders.py first)"

echo
echo "Done. Eject the card cleanly:  diskutil eject '$SD'"
echo "Then on the Switch: install /nsp/*.nsp with DBI or Goldleaf."
