#!/usr/bin/env python3
"""Turn an .inp script into a C file defining gInputScript (stored in ROM)."""
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src).read()
    with open(dst, "w") as f:
        f.write(f"/* Generated from {src} */\n")
        f.write("const char gInputScript[] =\n")
        for line in text.splitlines():
            esc = line.replace("\\", "\\\\").replace('"', '\\"')
            f.write(f'    "{esc}\\n"\n')
        f.write("    ;\n")


if __name__ == "__main__":
    main()
