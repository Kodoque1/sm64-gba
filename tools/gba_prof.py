#!/usr/bin/env python3
"""Where do Mario's GBA cycles go? Runs the profiling ROMs (build/gbaprof)
and splits each frame's cost into wall / floor / ceiling / water queries
and everything else. Also lists the most expensive frames."""
import argparse
import concurrent.futures as cf
import os
import subprocess

KINDS = ["wall", "floor", "ceil", "water"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--roms", required=True)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    def job(path):
        name = os.path.splitext(os.path.basename(path))[0]
        r = subprocess.run([args.runner, os.path.join(args.roms, name + ".gba")],
                           capture_output=True, timeout=1800)
        frames = []
        for l in r.stderr.decode().splitlines():
            if l.startswith("c "):
                v = [int(x) for x in l[2:].split()]
                frames.append((name, v[0], v[1:5], v[5:9]))
        return frames

    allf = []
    with cf.ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for frames in ex.map(job, sorted(args.inputs)):
            allf += frames

    total = sum(f[1] for f in allf)
    parts = [sum(f[2][i] for f in allf) for i in range(4)]
    calls = [sum(f[3][i] for f in allf) for i in range(4)]
    print(f"{len(allf)} frames, {total / len(allf):.0f} cycles/frame on average")
    for k, c, n in zip(KINDS, parts, calls):
        print(f"  {k:6s} {100 * c / total:5.1f}%   {n / len(allf):5.1f} calls/frame   "
              f"{c / max(n, 1):7.0f} cycles/call")
    print(f"  other  {100 * (total - sum(parts)) / total:5.1f}%")

    print(f"\nmost expensive frames:")
    for name, cyc, p, n in sorted(allf, key=lambda f: -f[1])[: args.top]:
        share = ", ".join(f"{k} {100 * x / cyc:.0f}%" for k, x in zip(KINDS, p) if x)
        print(f"  {name:22s} {cyc:7d} cycles  ({share}; wall calls {n[0]})")


if __name__ == "__main__":
    main()
