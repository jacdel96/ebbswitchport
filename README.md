# ebbswitchport

Self-contained Nintendo Switch homebrew apps that boot straight into SNES
EarthBound games. Each is a single `.nro`: a small **libnx** C frontend that
statically links the **snes9x libretro core** and embeds the ROM in RomFS — no
RetroArch, no NSP forwarder, no sigpatches. Launch from the Homebrew Menu.

Three titles:

1. **EarthBound** (SNES)
2. **EarthBound Beginnings Remake** — the [2026 fan remake of MOTHER 1](https://ebbr.neocities.org/), a BPS patch over EarthBound
3. **EarthBound: Giygas Strikes Back** — companion QoL patch (run button, batch buying, bug fixes)

> **Never commit ROMs.** `roms/`, all `.sfc/.smc`, and the built `.nro` (which
> embeds the ROM) are gitignored. This repo holds only C source, scripts, BPS
> patches, icons, and docs. Keep it private regardless. The built NRO contains
> copyrighted content — personal use only, never distribute.

## Layout

```
native/
├── Makefile           devkitPro switch-app build; links the snes9x core + romfs
├── source/
│   ├── main.c         libretro frontend: video, input, saves, main loop
│   ├── audio.c/.h     resamples core audio (~32 kHz) -> 48 kHz -> audout
│   └── libretro.h     libretro API (from snes9x, permissively licensed)
├── lib/               snes9x_libretro_libnx.a lands here (gitignored)
├── romfs/             game.sfc is swapped in per build (gitignored)
└── build-out/         finished .nro per game (gitignored — embeds ROM)
scripts/
├── apply_patches.py   base EarthBound ROM -> patched ROMs (CRC-verified)
├── make_icons.py      assets/art-src -> 256x256 icons
├── build_core.sh      clone + build the snes9x static lib
├── build_native.sh    build one .nro per game (ROM + icon + title baked in)
└── deploy_sd.sh       copy native/build-out/*.nro to the SD's /switch/
patches/               BPS patches (diffs only — no Nintendo code)
assets/                art-src/ (box art) + icons/ (built 256x256 JPEGs)
roms/                  (gitignored) base + patched ROMs
```

## What you supply

Drop your legally-owned **`EarthBound (USA)` ROM** into `roms/`. Headerless CRC32
must be `DC9BB451` (a 512-byte copier header is stripped automatically). That's the
only non-repo input — no console keys needed for the native track.

## Prerequisites (one time, Mac)

**devkitPro** (the Switch toolchain):

```bash
curl -L -o /tmp/dkp.pkg https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman-installer.pkg
sudo installer -pkg /tmp/dkp.pkg -target /
sudo /opt/devkitpro/pacman/bin/pacman -Sy switch-dev
```

If pacman errors with `GPGME error: Invalid crypto engine`: `brew install gnupg`,
or set `SigLevel = Never` under `[options]` in
`/opt/devkitpro/pacman/etc/pacman.conf` (installs are over HTTPS from devkitPro).

**Python + Pillow** (for the ROM patcher and icon maker):

```bash
python3 -m venv .venv && .venv/bin/pip install pillow
```

## Build

```bash
export DEVKITPRO=/opt/devkitpro
./scripts/build_core.sh                       # once: snes9x_libretro_libnx.a -> native/lib/
.venv/bin/python scripts/apply_patches.py     # base ROM -> 2 patched ROMs (CRC-verified)
.venv/bin/python scripts/make_icons.py        # -> assets/icons/
./scripts/build_native.sh                     # -> native/build-out/*.nro (one per game)
```

## Deploy to the Switch

1. Power the Switch **fully off**, remove the microSD, insert it in the Mac.
2. ```bash
   scripts/deploy_sd.sh "/Volumes/<SD-NAME>"
   ```
   Copies the three `.nro` files into `/switch/`.
3. **Eject cleanly** (`diskutil eject "/Volumes/<SD-NAME>"`), reinsert, boot Atmosphere.
4. Open the **Homebrew Menu** and launch **EarthBound** (etc.).

(Alternatively, wireless: run `ftpd` on the Switch and upload the NROs to `/switch/`.)

## Controls

| Switch | SNES |
|---|---|
| A / B / X / Y | A / B / X / Y |
| L / R | L / R |
| D-pad / left stick | D-pad |
| + | Start |
| − | Select |
| **ZR** | Open the in-game **menu** (does not affect SNES input) |

The menu (ZR) is the only way to save, load, quit, and resume:

```
MENU
> Resume        close the menu, keep playing
  Save          pick one of 10 manual slots to write the current state to
  Load          restore any state: the 10 manual slots + the 2 auto slots
  Exit          quit to the Homebrew Menu
```

Navigate with Up/Down, **A** = select, **B** = back, **ZR** = close. The game
pauses while the menu is open.

## Saves

There are **12 state slots** per game, all in `sdmc:/switch/ebbswitchport/`:

- **10 manual slots** (`<game>.slot0` … `<game>.slot9`) — written from the menu's
  **Save**, restored from **Load**.
- **2 automatic slots**, updated in the background while you play:
  - `<game>.auto1` — refreshed **every ~1 minute** and on exit. This is your
    crash/battery safety net **and** the auto-resume point.
  - `<game>.auto10` — refreshed **every ~10 minutes** (a deeper fallback).
- **Auto-resume:** on launch the game silently continues from `auto1`, so it
  picks up exactly where you left off.
- **Battery (SRAM):** in-game saves (at a phone) write to `<game>.srm`,
  auto-flushed ~every 10 s and on exit.

Back up `sdmc:/switch/ebbswitchport/` occasionally — it's just files on the SD.

Native NROs launched from the Homebrew Menu don't need sigpatches, so a firmware
or Atmosphere update won't break them the way installed forwarders did. After a
major update just re-launch from hbmenu.

## Rebuilding from scratch

```bash
git clone <this repo> && cd ebbswitchport
python3 -m venv .venv && .venv/bin/pip install pillow
export DEVKITPRO=/opt/devkitpro
# drop EarthBound (USA).sfc into roms/
./scripts/build_core.sh
.venv/bin/python scripts/apply_patches.py
.venv/bin/python scripts/make_icons.py
./scripts/build_native.sh
scripts/deploy_sd.sh "/Volumes/<SD>"
```

## Credits

- [EarthBound Beginnings Remake](https://ebbr.neocities.org/) by Gabbls & team
- [snes9x](https://github.com/snes9xgit/snes9x) / the libretro core
- [devkitPro / libnx](https://devkitpro.org/) — the Switch homebrew toolchain
- libretro API (`libretro.h`)
