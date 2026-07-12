#!/usr/bin/env python3
"""
Pack a LEDTREES firmware bundle (.ltfw) — see features/ota.md §4.

Layout:
  LtFwHeader (little-endian, 160 bytes):
    magic           4s   b"LTFW"
    formatVersion   u32  1
    version         32s  bundle version, NUL-padded utf-8
    clrOffset       u32  \
    clrSize         u32   } nanoCLR.bin (IDF app image)
    clrSha256       32s  /
    mngdOffset      u32  \
    mngdSize        u32   } managed deployment image (deploy partition content)
    mngdSha256      32s  /
    webOffset       u32  \
    webSize         u32   } web section (may be empty: size 0)
    webSha256       32s  /
  sections follow in that order.

Web section format (matches the existing app-bundle file records):
    count u32, then per file: nameLen u32, name utf-8, dataLen u32, data

Usage:
  pack-ltfw.py --version 2.4.0 --clr build/nanoCLR.bin --managed deploy.bin \
               [--wwwroot path/to/wwwroot] --out firmware-2.4.0.ltfw \
               [--manifest-fragment out.json]
"""

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"LTFW"
FORMAT_VERSION = 1
HEADER_FMT = "<4sI32s" + "II32s" * 3
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 160, HEADER_SIZE


def build_web_section(wwwroot: Path) -> bytes:
    files = sorted(p for p in wwwroot.rglob("*") if p.is_file())
    out = bytearray(struct.pack("<I", len(files)))
    for f in files:
        name = f.relative_to(wwwroot).as_posix().encode("utf-8")
        data = f.read_bytes()
        out += struct.pack("<I", len(name)) + name
        out += struct.pack("<I", len(data)) + data
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", required=True, help="bundle version, e.g. 2.4.0")
    ap.add_argument("--clr", required=True, type=Path, help="nanoCLR.bin (IDF app image)")
    ap.add_argument("--managed", required=True, type=Path, help="managed deployment image")
    ap.add_argument("--wwwroot", type=Path, help="wwwroot directory (optional)")
    ap.add_argument("--out", required=True, type=Path, help="output .ltfw path")
    ap.add_argument("--manifest-fragment", type=Path, help="write a JSON fragment for the manifest")
    args = ap.parse_args()

    version = args.version.encode("utf-8")
    if len(version) > 31:
        ap.error("version longer than 31 bytes")

    clr = args.clr.read_bytes()
    managed = args.managed.read_bytes()
    web = build_web_section(args.wwwroot) if args.wwwroot else b""

    # cheap sanity checks: catch swapped --clr/--managed arguments and broken
    # managed images here instead of on the device
    if not clr or clr[0] != 0xE9:
        ap.error(f"{args.clr}: not an ESP-IDF app image (first byte 0x{clr[0]:02X} != 0xE9)"
                 if clr else f"{args.clr}: empty file")
    if not managed:
        ap.error(f"{args.managed}: empty file")
    if len(managed) % 4 != 0:
        # the CLR walks the deploy region as 4-byte-aligned .pe records
        ap.error(f"{args.managed}: size {len(managed)} is not 4-byte aligned")
    if managed[0] == 0xE9:
        ap.error(f"{args.managed}: looks like an ESP-IDF app image - swapped --clr/--managed?")

    sections = []
    offset = HEADER_SIZE
    for data in (clr, managed, web):
        sections.append((offset, len(data), hashlib.sha256(data).digest()))
        offset += len(data)

    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        FORMAT_VERSION,
        version.ljust(32, b"\0"),
        *sections[0],
        *sections[1],
        *sections[2],
    )

    bundle = header + clr + managed + web
    args.out.write_bytes(bundle)

    info = {
        "version": args.version,
        "size": len(bundle),
        "sha256": hashlib.sha256(bundle).hexdigest(),
        "clrSha256": sections[0][2].hex(),
        "mngdSha256": sections[1][2].hex(),
        "webSha256": sections[2][2].hex(),
        # CRC32 (zlib) of the managed image: what the device passes to Ota.StageCommit
        "mngdCrc32": zlib.crc32(managed) & 0xFFFFFFFF,
    }

    if args.manifest_fragment:
        args.manifest_fragment.write_text(json.dumps(info, indent=2))

    print(f"{args.out}: {len(bundle)} bytes")
    print(f"  clr:     {len(clr):>9} bytes  sha256 {info['clrSha256'][:16]}…")
    print(f"  managed: {len(managed):>9} bytes  sha256 {info['mngdSha256'][:16]}…  crc32 0x{info['mngdCrc32']:08X}")
    print(f"  web:     {len(web):>9} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
