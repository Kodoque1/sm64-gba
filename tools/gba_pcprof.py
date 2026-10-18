#!/usr/bin/env python3
"""Symbolize mgba_runner's PC/LR samples (MGBA_PROFILE) into a profile.

Three views:
  * by source object (which .o the PC is in, from the linker map)
  * flat by function (self time)
  * by caller, with soft-float charged to the caller: a sample inside a libgcc
    float routine is attributed to the function its LR points into, so you
    see e.g. "mario_step.c: perform_ground_quarter_step 9% (of which 6% float)"

Usage: gba_pcprof.py run1.elf:run1.prof [run2.elf:run2.prof ...] [--top N]
"""
import argparse
import bisect
import collections
import re
import subprocess

FLOAT_RE = re.compile(
    r"^(__aeabi_[fd]|__aeabi_[iu]2[fd]|__aeabi_[fd]2|__(add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|"
    r"unord|fix|fixuns|float|floatun|extend|trunc)[sd]f|sqrtf$)")
VENEER_RE = re.compile(r"^(__(.+)_from_thumb|__(.+)_veneer|_+(.+)_from_arm)$")


def load_symbols(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-n", "--defined-only", elf],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3 or parts[1].lower() not in "tw":
            continue
        addr, name = int(parts[0], 16), parts[2]
        if name.startswith("$") or name.startswith(".L"):
            continue
        syms.append((addr & ~1, name))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def load_objects(mapfile):
    """(start, end, object basename) for every .text input section in the map."""
    ranges = []
    pending = None
    for line in open(mapfile):
        m = re.match(r"^ (\.text|\.iwram)\S*\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)", line)
        if not m:
            m2 = re.match(r"^ (\.text|\.iwram)\S*\s*$", line)
            pending = bool(m2)
            if pending:
                continue
            m3 = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.(o|a)\S*)", line) if pending is not None else None
            if m3 and pending:
                start, size, obj = int(m3.group(1), 16), int(m3.group(2), 16), m3.group(3)
                if size:
                    ranges.append((start, start + size, short_obj(obj)))
            pending = None
            continue
        start, size, obj = int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        if size:
            ranges.append((start, start + size, short_obj(obj)))
    ranges.sort()
    return ranges


def short_obj(obj):
    m = re.search(r"\(([^)]+)\)$", obj)          # libgcc.a(_arm_addsubsf3.o)
    if m:
        return "libgcc:" + m.group(1)
    return obj.rsplit("/", 1)[-1]


def lookup(addrs, names, pc):
    i = bisect.bisect_right(addrs, pc & ~1) - 1
    return names[i] if i >= 0 else "?"


def lookup_obj(ranges, starts, pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and ranges[i][0] <= pc < ranges[i][1]:
        return ranges[i][2]
    return "?"


def canon(name):
    m = VENEER_RE.match(name)
    if m:
        return next(g for g in m.groups()[1:] if g)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+", help="elf:profile pairs")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    self_t = collections.Counter()
    by_obj = collections.Counter()
    caller_t = collections.Counter()
    caller_float = collections.Counter()
    total = 0
    period = None

    for run in args.runs:
        elf, prof = run.split(":")
        addrs, names = load_symbols(elf)
        ranges = load_objects(elf + ".map")
        starts = [r[0] for r in ranges]
        for line in open(prof):
            if line.startswith("#"):
                period = int(line.split()[2])
                continue
            pc_s, lr_s, n_s = line.split()
            pc, lr, n = int(pc_s, 16), int(lr_s, 16), int(n_s)
            total += n
            fn = canon(lookup(addrs, names, pc))
            self_t[fn] += n
            obj = lookup_obj(ranges, starts, pc)
            is_float = bool(FLOAT_RE.match(fn)) or obj.startswith("libgcc:_arm_")
            by_obj["soft-float (libgcc)" if is_float else obj] += n
            if is_float:
                caller = canon(lookup(addrs, names, lr))
                if FLOAT_RE.match(caller):
                    caller = "(float routine, nested)"
                caller_t[caller] += n
                caller_float[caller] += n
            else:
                caller_t[fn] += n

    pct = lambda n: 100.0 * n / total
    print(f"{total} samples, period {period} cycles\n")
    print("by object (soft-float pooled):")
    for name, n in by_obj.most_common(12):
        print(f"  {pct(n):5.1f}%  {name}")
    print("\nflat, self time:")
    for name, n in self_t.most_common(args.top):
        print(f"  {pct(n):5.1f}%  {name}")
    print("\nby function, soft-float charged to the caller:")
    for name, n in caller_t.most_common(args.top):
        f = caller_float[name]
        print(f"  {pct(n):5.1f}%  {name:40s} (float {pct(f):4.1f}%)")


if __name__ == "__main__":
    main()
