# ebbswitchport

Home-screen apps for a Nintendo Switch running Atmosphere that boot straight into:

1. **EarthBound** (SNES)
2. **EarthBound Beginnings Remake** — the [2026 fan remake of MOTHER 1](https://ebbr.neocities.org/) as a BPS patch over EarthBound
3. **EarthBound: Giygas Strikes Back** — the companion QoL patch (run button, batch buying, bug fixes)

Architecture: **RetroArch (snes9x core)** as the emulator, plus one installable **NSP forwarder per game** — a tiny title with custom icon that launches the core with the right ROM, full-screen, no menus. RetroArch provides SRAM saves (auto-flushed every 60s), save states, and rewind.

> **Never commit ROMs or console keys.** `roms/`, `keys/`, `out/`, `sd-stage/` and all `.sfc/.smc/.nsp/.nro` files are gitignored. This repo holds only scripts, BPS patches, icons, and docs. Keep the repo private regardless.

## What you supply (not in the repo)

| File | Where it goes | How to get it |
|---|---|---|
| `EarthBound (USA).sfc` | `roms/` | Your legally owned cartridge dump. Headerless CRC32 must be `DC9BB451` — the base both BPS patches require (a 512-byte copier header is stripped automatically). |
| `prod.keys` | `keys/` | Dump once from *your* console with Lockpick_RCM. Only needed for the on-Mac NSP build (Path A). |

## One-time setup (Mac)

```bash
python3 -m venv .venv && .venv/bin/pip install pillow
./scripts/setup_tools.sh          # builds hacbrewpack from source into tools/bin/
```

## Build everything

```bash
.venv/bin/python scripts/apply_patches.py     # base ROM -> 2 patched ROMs (CRC-verified)
.venv/bin/python scripts/make_icons.py        # assets/art-src -> 256x256 icons
.venv/bin/python scripts/build_forwarders.py  # -> out/*.nsp (needs keys/prod.keys)
```

Title IDs are pinned in `build_forwarders.py` — do not change them once installed, or the console treats a rebuild as a different app.

## Getting it onto the Switch

### Transfer method 1 — mount the SD card (fastest)

1. Power the Switch **fully off** (hold power → Power Options → Turn Off), remove the microSD.
2. Insert it into the Mac (reader/adapter). It mounts under `/Volumes/<NAME>` (FAT32/exFAT both mount natively).
3. ```bash
   scripts/deploy_sd.sh /Volumes/<NAME>
   ```
4. **Eject cleanly** — `diskutil eject /Volumes/<NAME>` or Finder eject. Yanking it corrupts FAT filesystems.
5. Reinsert the card, boot back into Atmosphere.

### Transfer method 2 — wireless FTP (no SD removal)

1. On the Switch, launch **ftpd** from the Homebrew Menu (install it via the Homebrew App Store if missing). It shows an IP and port.
2. From the Mac, connect with Cyberduck/ForkLift (or `ftp`), and upload to the same paths `deploy_sd.sh` uses: `/retroarch/`, `/switch/`, `/roms/snes/`, `/switch/icons/`, `/nsp/`.
3. Slower than a direct mount (Wi-Fi), but fine for updating a ROM or a single NSP.

### Path A — install the prebuilt NSPs

Prereq: **signature patches (sigpatches)**, since forwarder NSPs are unsigned homebrew. Without them, launching a forwarder throws `fsOpenFileSystemWithId()`. This repo stages [**sys-patch**](https://github.com/impeeza/sys-patch) into `sd-stage/atmosphere/` — a sysmodule that applies the patches at runtime and auto-adapts across firmware updates (no version-matched zip to chase). `deploy_sd.sh` copies it. After deploying, reboot the console once so the sysmodule loads (check the `sys-patch` overlay via Tesla, or just confirm forwarders now launch). Requires booting Atmosphere via hekate or an IPS-capable fusee. If you'd rather use static patches, the GBAtemp "Sigpatches for Atmosphere" thread publishes per-version bundles that extract over the SD root instead.

1. On the Switch, open **DBI** (or Goldleaf) from the Homebrew Menu.
2. *Browse SD* → `/nsp/` → install each of the three NSPs to the SD card.
3. The three icons appear on the home screen.

### Path B — generate forwarders on the Switch itself (no keys, no Mac build)

Use this if you don't want `prod.keys` on the Mac, or after a firmware update broke the installed forwarders and you just want to regenerate quickly.

1. Launch **NSP Forwarder** (`nsp-forwarder.nro`, already deployed to `/switch/`) from the Homebrew Menu.
2. Pick the NRO: `/retroarch/cores/snes9x_libretro_libnx.nro`.
3. Set the ROM argument to the game's path, e.g. `/roms/snes/EarthBound Beginnings Remake.sfc`.
4. Pick the icon from `/switch/icons/`, set the title name, generate, and install when prompted.
5. Repeat per game.

## First boot checklist

- Launch each icon: it should go straight into the game, full screen. (First RetroArch launch may briefly build its config.)
- **In-game saves**: save at any save point (phone). `retroarch.cfg` flushes SRAM every 60 s and on clean exit; saves land in `/retroarch/saves/`. To be safe, quit via RetroArch's menu (press both sticks → Close Content) rather than just sleeping the console mid-save.
- **Save states**: press both analog sticks to open the Quick Menu → Save/Load State. States live in `/retroarch/states/`, auto-indexed per game.
- Back up `/retroarch/saves/` occasionally — it's just files on the SD card.

## After a firmware or Atmosphere update

Installed forwarders sometimes stop launching after major updates (sigpatches go stale, or the hbl the stub relies on changes). Fix:

1. Update Atmosphere + matching sigpatches.
2. If a forwarder still fails: reinstall the same NSP (Path A) — title IDs are pinned, so it's an in-place update — or regenerate on-console (Path B).
3. RetroArch itself may also want updating: re-run `deploy_sd.sh` after refreshing `sd-stage/` with a newer release.

## Rebuilding from scratch

```bash
git clone <this repo> && cd ebbswitchport
python3 -m venv .venv && .venv/bin/pip install pillow
./scripts/setup_tools.sh
# drop EarthBound ROM into roms/, prod.keys into keys/
# re-download RetroArch into sd-stage/ (see scripts/README note below)
.venv/bin/python scripts/apply_patches.py
.venv/bin/python scripts/make_icons.py
.venv/bin/python scripts/build_forwarders.py
scripts/deploy_sd.sh /Volumes/<SD>
```

`sd-stage/` is not committed (it's ~1 GB of RetroArch binaries). To recreate it:

```bash
mkdir -p sd-stage && cd sd-stage
curl -fLO https://buildbot.libretro.com/stable/1.22.2/nintendo/switch/libnx/RetroArch.7z
tar -xf RetroArch.7z && rm RetroArch.7z
curl -fsL -o switch/nsp-forwarder.nro https://github.com/TooTallNate/switch-nsp-forwarder/releases/download/0.0.8/nsp-forwarder.nro
```

The RetroArch seed config lives in the repo at `config/retroarch.cfg`; `deploy_sd.sh` copies it to the SD card only when no config exists there yet (so it never clobbers settings you've changed on-console).

## Credits

- [EarthBound Beginnings Remake](https://ebbr.neocities.org/) by Gabbls & team (lineage back to Tomato's 2007 project)
- [RetroArch](https://www.retroarch.com/) / snes9x (libretro)
- Forwarder stub + recipe from [nton](https://github.com/rlaphoenix/nton) by rlaphoenix (see `assets/forwarder-stub/hacbrewpack.license`)
- [switch-nsp-forwarder](https://github.com/TooTallNate/switch-nsp-forwarder) by TooTallNate
- [hacBrewPack](https://github.com/The-4n/hacBrewPack) by The-4n
