#!/usr/bin/env python3
"""Turn the bench's key script into C (build/demo/bench_keys.c, in ROM).

Script lines: "first last KEYS", game frames, inclusive, KEYS from
A B L R S(start) E(select) < > ^ v as in mgba_runner's --keys; and
"end N", the bench's length in game frames.
"""
import sys

BITS = {"A": 0x001, "B": 0x002, "E": 0x004, "S": 0x008, ">": 0x010, "<": 0x020,
        "^": 0x040, "v": 0x080, "R": 0x100, "L": 0x200}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    ranges, end = [], None
    for line in open(src):
        f = line.split("#")[0].split()
        if not f:
            continue
        if f[0] == "end":
            end = int(f[1])
            continue
        keys = 0
        for c in f[2] if len(f) > 2 else "":
            keys |= BITS[c]
        ranges.append((int(f[0]), int(f[1]), keys))
    if end is None:
        sys.exit(f"{src}: no 'end N' line")
    with open(dst, "w") as out:
        out.write(f'/* Generated from {src} */\n#include "bench.h"\n\n')
        out.write("const unsigned short gBenchKeys[][3] = {\n")
        for a, b, k in ranges:
            out.write(f"    {{ {a}, {b}, 0x{k:03x} }},\n")
        out.write("};\n")
        out.write(f"const unsigned int gNumBenchKeys = {len(ranges)};\n")
        out.write(f"const unsigned int gBenchFrames = {end};\n")
        out.write("_Static_assert(" + str(end) + " <= BENCH_MAX_FRAMES, \"the record must fit in save RAM\");\n")


if __name__ == "__main__":
    main()
