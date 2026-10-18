#!/usr/bin/env python3
"""Fill in the GBA cartridge header fields that emulators check.

Sets the 12-byte title, the fixed byte 0x96 at 0xB2 and the complement
checksum at 0xBD. The Nintendo logo area is left zeroed: it isn't in this
repository. A real GBA's BIOS refuses to boot without it; for the demo ROMs
devkitPro's gbafix writes it (see the Makefile).

  fix_gba_header.py rom.gba TITLE
  fix_gba_header.py rom.gba --check   the header a GBA's BIOS checks: logo,
                                      fixed byte and complement checksum
"""
import hashlib
import sys

LOGO = slice(0x04, 0xA0)
# SHA-256 of the logo bytes as gbafix writes them (the logo itself stays out
# of this repository)
LOGO_SHA256 = "08a0153cfd6b0ea54b938f7d209933fa849da0d56f5a34c481060c9ff2fad818"


def complement(rom):
    return (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF


def check(path):
    rom = open(path, "rb").read(0xC0)
    errors = []
    if hashlib.sha256(rom[LOGO]).hexdigest() != LOGO_SHA256:
        errors.append("the Nintendo logo is missing or wrong")
    if rom[0xB2] != 0x96:
        errors.append("byte 0xB2 isn't 0x96")
    if rom[0xBD] != complement(rom):
        errors.append("the complement checksum is wrong")
    if errors:
        sys.exit(f"{path}: a GBA won't boot it: " + "; ".join(errors))
    print(f"  {path}: header OK for a GBA (logo, fixed byte, checksum)")


def main():
    path = sys.argv[1]
    if sys.argv[2:] == ["--check"]:
        check(path)
        return
    title = sys.argv[2] if len(sys.argv) > 2 else "SM64PARITY"
    rom = bytearray(open(path, "rb").read())
    rom[0xA0:0xAC] = title.upper().encode("ascii")[:12].ljust(12, b"\0")
    rom[0xAC:0xB0] = b"SMPE"
    rom[0xB0:0xB2] = b"01"
    rom[0xB2] = 0x96
    rom[0xBD] = complement(rom)
    # Pad to a multiple of 4 bytes; some loaders care.
    while len(rom) % 4:
        rom.append(0)
    open(path, "wb").write(rom)


if __name__ == "__main__":
    main()
