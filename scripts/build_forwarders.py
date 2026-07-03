#!/usr/bin/env python3
"""Build the three NSP forwarders (macOS-native reimplementation of nton's recipe).

Usage: .venv/bin/python scripts/build_forwarders.py

Needs:
  keys/prod.keys            dumped from your console with Lockpick_RCM
  tools/bin/hacbrewpack     built once via scripts/setup_tools.sh
  assets/forwarder-stub/    forwarder exefs + logo (vendored from nton)
  assets/icons/<name>.jpg   built via scripts/make_icons.py
  sd-stage/retroarch/cores/snes9x_libretro_libnx.nro   (for its embedded NACP)

Each forwarder is a tiny installed title whose romfs tells the stub which NRO
to launch (the snes9x core) and with what argument (the ROM path on SD).
Title IDs are pinned so rebuilt/reinstalled forwarders keep their identity.
"""

import shutil
import struct
import subprocess
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
STUB = REPO / "assets" / "forwarder-stub"
ICONS = REPO / "assets" / "icons"
KEYS = REPO / "keys" / "prod.keys"
HACBREWPACK = REPO / "tools" / "bin" / "hacbrewpack"
CORE_NRO_LOCAL = REPO / "sd-stage" / "retroarch" / "cores" / "snes9x_libretro_libnx.nro"
CORE_NRO_SDMC = "sdmc:/retroarch/cores/snes9x_libretro_libnx.nro"
OUT = REPO / "out"
BUILD = REPO / "out" / ".build"

PUBLISHER = "ebbswitchport"
VERSION = "1.0.0"

# name, pinned title ID, ROM path on the SD card
# IDs are spaced 0x2000 apart so each title's AddOnContentBaseId (+0x1000)
# can never collide with a neighbour.
GAMES = [
    ("EarthBound", "01ebb000eb000000", "/roms/snes/EarthBound.sfc"),
    ("EarthBound Beginnings Remake", "01ebb000eb002000", "/roms/snes/EarthBound Beginnings Remake.sfc"),
    ("EarthBound Giygas Strikes Back", "01ebb000eb004000", "/roms/snes/EarthBound Giygas Strikes Back.sfc"),
]


def extract_nacp_from_nro(nro_path: Path) -> bytes:
    """Pull the 0x4000-byte NACP out of an NRO's trailing ASET section."""
    data = nro_path.read_bytes()
    if data[0x10:0x14] != b"NRO0":
        raise ValueError(f"{nro_path.name}: not an NRO (bad magic)")
    nro_size = struct.unpack_from("<I", data, 0x18)[0]
    asset = data[nro_size:]
    if asset[:4] != b"ASET":
        raise ValueError(f"{nro_path.name}: no ASET section (no embedded NACP)")
    nacp_off, nacp_size = struct.unpack_from("<QQ", asset, 0x18)
    if nacp_size != 0x4000:
        raise ValueError(f"{nro_path.name}: unexpected NACP size {nacp_size:#x}")
    return asset[nacp_off:nacp_off + nacp_size]


def make_nacp(base: bytes, name: str, title_id: str) -> bytes:
    """Apply nton's NACP mutations for a forwarder title."""
    nacp = bytearray(base)
    tid = int(title_id, 16)

    # AmericanEnglish name/publisher only; all other language slots zeroed.
    nacp[0x0:0x3000] = b"\x00" * 0x3000
    nacp[0x0:0x200] = name.encode("utf8").ljust(0x200, b"\x00")
    nacp[0x200:0x300] = PUBLISHER.encode("utf8").ljust(0x100, b"\x00")

    nacp[0x3060:0x3070] = VERSION.encode("utf8").ljust(0x10, b"\x00")

    struct.pack_into("<Q", nacp, 0x3038, tid)             # PresenceGroupId
    struct.pack_into("<Q", nacp, 0x3070, tid + 0x1000)    # AddOnContentBaseId
    struct.pack_into("<Q", nacp, 0x3078, tid)             # SaveDataOwnerId
    for i in range(8):                                    # LocalCommunicationId
        struct.pack_into("<Q", nacp, 0x30B0 + i * 8, tid)

    nacp[0x3025] = 0x00  # StartupUserAccount: no profile picker
    nacp[0x3034] = 0x00  # Screenshot: enabled
    nacp[0x3035] = 0x02  # VideoCapture: enabled

    # No console-side save data — RetroArch saves straight to the SD card.
    for off in (0x3080, 0x3088, 0x3090, 0x3098, 0x30A0,
                0x3148, 0x3150, 0x3158, 0x3160, 0x3168, 0x3170, 0x3178, 0x3180):
        struct.pack_into("<Q", nacp, off, 0)
    nacp[0x3188:0x318A] = b"\x00\x00"  # CacheStorageIndexMax

    return bytes(nacp)


def clean_icon(src: Path, dst: Path) -> None:
    """256x256 RGB JPEG with zero metadata, or the home screen shows a '?'."""
    im = Image.open(src)
    if im.size != (256, 256):
        im = im.resize((256, 256))
    im = im.convert("RGB")
    clean = Image.new("RGB", im.size)
    clean.putdata(list(im.getdata()))
    clean.save(dst, format="JPEG")


def build_one(name: str, title_id: str, rom_sd_path: str, base_nacp: bytes) -> Path:
    build_dir = BUILD / title_id
    if build_dir.exists():
        shutil.rmtree(build_dir)
    (build_dir / "control").mkdir(parents=True)
    (build_dir / "romfs").mkdir()
    shutil.copytree(STUB / "exefs", build_dir / "exefs")
    shutil.copytree(STUB / "logo", build_dir / "logo")

    (build_dir / "control" / "control.nacp").write_bytes(make_nacp(base_nacp, name, title_id))
    clean_icon(ICONS / f"{name}.jpg", build_dir / "control" / "icon_AmericanEnglish.dat")

    (build_dir / "romfs" / "nextNroPath").write_text(CORE_NRO_SDMC)
    (build_dir / "romfs" / "nextArgv").write_text(f'{CORE_NRO_SDMC} "sdmc:{rom_sd_path}"')

    subprocess.run(
        [str(HACBREWPACK), "--titleid", title_id,
         "--nspdir", str(OUT), "-k", str(KEYS)],
        cwd=build_dir, check=True, capture_output=True, text=True,
    )
    built = OUT / f"{title_id}.nsp"
    final = OUT / f"{name} [{title_id}].nsp"
    shutil.move(built, final)
    return final


def main():
    missing = [str(p.relative_to(REPO)) for p in (KEYS, HACBREWPACK, CORE_NRO_LOCAL) if not p.exists()]
    if missing:
        sys.exit("Missing prerequisites: " + ", ".join(missing))

    base_nacp = extract_nacp_from_nro(CORE_NRO_LOCAL)
    OUT.mkdir(exist_ok=True)
    for name, title_id, rom in GAMES:
        try:
            final = build_one(name, title_id, rom, base_nacp)
        except subprocess.CalledProcessError as e:
            sys.exit(f"hacbrewpack failed for {name}:\n{e.stdout}\n{e.stderr}")
        print(f"built {final.relative_to(REPO)}")
    shutil.rmtree(BUILD, ignore_errors=True)
    print("Done. Install the NSPs from out/ with DBI or Goldleaf.")


if __name__ == "__main__":
    main()
