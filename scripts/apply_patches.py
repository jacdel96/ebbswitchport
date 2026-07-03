#!/usr/bin/env python3
"""Apply every BPS patch in patches/ to the base EarthBound ROM in roms/.

Usage: python3 scripts/apply_patches.py [path/to/base-rom.sfc]

If no ROM path is given, picks the first .sfc/.smc in roms/ whose checksum
matches the expected EarthBound (USA) base. A 512-byte copier header is
stripped automatically. Every patch's own embedded CRCs (source and target)
are enforced, so a wrong base ROM or corrupt output fails loudly.
"""

import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PATCHES = REPO / "patches"
ROMS = REPO / "roms"

# EarthBound (USA), headerless
EXPECTED_BASE_CRC = 0x31C99B0A

SOURCE_READ, TARGET_READ, SOURCE_COPY, TARGET_COPY = range(4)


def read_varint(buf: bytes, pos: int):
    result, shift = 0, 1
    while True:
        b = buf[pos]
        pos += 1
        result += (b & 0x7F) * shift
        if b & 0x80:
            return result, pos
        shift <<= 7
        result += shift


def read_signed_varint(buf: bytes, pos: int):
    value, pos = read_varint(buf, pos)
    magnitude = value >> 1
    return (-magnitude if value & 1 else magnitude), pos


def apply_bps(patch: bytes, source: bytes) -> bytes:
    if patch[:4] != b"BPS1":
        raise ValueError("not a BPS patch (bad magic)")
    patch_crc = int.from_bytes(patch[-4:], "little")
    if zlib.crc32(patch[:-4]) != patch_crc:
        raise ValueError("patch file is corrupt (patch CRC mismatch)")
    source_crc = int.from_bytes(patch[-12:-8], "little")
    target_crc = int.from_bytes(patch[-8:-4], "little")
    if zlib.crc32(source) != source_crc:
        raise ValueError(
            f"base ROM CRC {zlib.crc32(source):08X} does not match the CRC "
            f"{source_crc:08X} this patch expects"
        )

    pos = 4
    source_size, pos = read_varint(patch, pos)
    target_size, pos = read_varint(patch, pos)
    metadata_size, pos = read_varint(patch, pos)
    pos += metadata_size
    if source_size != len(source):
        raise ValueError("base ROM size does not match patch expectation")

    target = bytearray(target_size)
    out = src_rel = tgt_rel = 0
    end = len(patch) - 12
    while pos < end:
        data, pos = read_varint(patch, pos)
        command, length = data & 3, (data >> 2) + 1
        if command == SOURCE_READ:
            target[out:out + length] = source[out:out + length]
        elif command == TARGET_READ:
            target[out:out + length] = patch[pos:pos + length]
            pos += length
        elif command == SOURCE_COPY:
            offset, pos = read_signed_varint(patch, pos)
            src_rel += offset
            target[out:out + length] = source[src_rel:src_rel + length]
            src_rel += length
        else:  # TARGET_COPY: may overlap itself, must copy byte-by-byte
            offset, pos = read_signed_varint(patch, pos)
            tgt_rel += offset
            for i in range(length):
                target[out + i] = target[tgt_rel + i]
            tgt_rel += length
        out += length

    if zlib.crc32(bytes(target)) != target_crc:
        raise ValueError("patched output failed its CRC check")
    return bytes(target)


def load_base_rom(explicit: Path | None) -> bytes:
    candidates = [explicit] if explicit else sorted(
        p for p in ROMS.glob("*.s[fm]c") if p.is_file()
    )
    if not candidates:
        sys.exit(f"No base ROM found. Drop your EarthBound (USA) .sfc into {ROMS}/")
    for rom_path in candidates:
        data = rom_path.read_bytes()
        if len(data) % 1024 == 512:
            print(f"  stripping 512-byte copier header from {rom_path.name}")
            data = data[512:]
        crc = zlib.crc32(data)
        if crc == EXPECTED_BASE_CRC:
            print(f"Base ROM: {rom_path.name} (CRC {crc:08X} — verified)")
            return data
        if explicit:
            print(f"WARNING: {rom_path.name} CRC {crc:08X} != expected "
                  f"{EXPECTED_BASE_CRC:08X}; trying anyway (patch CRC will decide)")
            return data
    sys.exit(
        f"No ROM in {ROMS}/ matches EarthBound (USA) CRC {EXPECTED_BASE_CRC:08X}. "
        "Pass the ROM path explicitly to override."
    )


def main():
    explicit = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    source = load_base_rom(explicit)
    patches = sorted(PATCHES.glob("*.bps"))
    if not patches:
        sys.exit(f"No .bps patches found in {PATCHES}/")
    for patch_path in patches:
        out_path = ROMS / (patch_path.stem + ".sfc")
        print(f"Applying {patch_path.name} ...")
        target = apply_bps(patch_path.read_bytes(), source)
        out_path.write_bytes(target)
        print(f"  -> {out_path.relative_to(REPO)} "
              f"({len(target)} bytes, CRC {zlib.crc32(target):08X} — verified)")
    print("Done.")


if __name__ == "__main__":
    main()
