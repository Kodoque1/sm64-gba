#!/usr/bin/env python3
"""Fill in the GBA cartridge header fields that emulators check.

Sets the 12-byte title, the fixed byte 0x96 at 0xB2 and the complement
checksum at 0xBD. The Nintendo logo area is left zeroed: it isn't in this
repository. A real GBA's BIOS refuses to boot without it, so for hardware
either run devkitPro's gbafix on the ROM, or pass --logo-from with a GBA ROM
you have, to copy the logo from its header.
"""
import sys

LOGO = slice(0x04, 0xA0)


def complement(rom):
    return (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF


def read_logo(path):
    src = open(path, "rb").read(0xC0)
    if len(src) < 0xC0 or src[0xB2] != 0x96 or src[0xBD] != complement(src):
        sys.exit(f"{path}: not a GBA ROM with a valid header")
    if not any(src[LOGO]):
        sys.exit(f"{path}: its logo area is empty")
    return src[LOGO]


def main():
    args = sys.argv[1:]
    logo = None
    if "--logo-from" in args:
        i = args.index("--logo-from")
        logo = read_logo(args[i + 1])
        del args[i:i + 2]
    path, title = args[0], args[1] if len(args) > 1 else "SM64PARITY"
    rom = bytearray(open(path, "rb").read())
    if logo is not None:
        rom[LOGO] = logo
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
