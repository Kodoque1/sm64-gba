#!/usr/bin/env python3
"""Fill in the GBA cartridge header fields that emulators check.

Sets the 12-byte title, the fixed byte 0x96 at 0xB2 and the complement
checksum at 0xBD. The Nintendo logo area is left zeroed on purpose.
"""
import sys


def main():
    path, title = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "SM64PARITY"
    rom = bytearray(open(path, "rb").read())
    rom[0xA0:0xAC] = title.upper().encode("ascii")[:12].ljust(12, b"\0")
    rom[0xAC:0xB0] = b"SMPE"
    rom[0xB0:0xB2] = b"01"
    rom[0xB2] = 0x96
    rom[0xBD] = (-(sum(rom[0xA0:0xBD]) + 0x19)) & 0xFF
    # Pad to a multiple of 4 bytes; some loaders care.
    while len(rom) % 4:
        rom.append(0)
    open(path, "wb").write(rom)


if __name__ == "__main__":
    main()
