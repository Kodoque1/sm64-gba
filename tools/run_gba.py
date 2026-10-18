#!/usr/bin/env python3
"""Run each script's GBA ROM in mGBA, diff against the host reference, and
report the per-frame CPU cost of Mario's update measured with GBA timers.

Budget: SM64 runs its game logic at 30 Hz, i.e. once every two GBA frames
(2 x 280,896 = 561,792 cycles at 16.78 MHz). The whole game (objects,
camera, rendering) has to fit in that, so the movement core should use a
small slice of it.
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys

GBA_FRAME = 280_896
BUDGET_30HZ = 2 * GBA_FRAME


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * (len(xs) - 1) + 0.5))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--runner", required=True)
    ap.add_argument("--roms", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    def job(path):
        name = os.path.splitext(os.path.basename(path))[0]
        with open(path, "rb") as f:
            ref = subprocess.run([args.host], stdin=f, capture_output=True).stdout.decode()
        rom = os.path.join(args.roms, name + ".gba")
        r = subprocess.run([args.runner, rom], capture_output=True, timeout=1800)
        cycles = [int(l[2:]) for l in r.stderr.decode().splitlines() if l.startswith("c ")]
        return name, r.returncode, ref, r.stdout.decode(), cycles, r.stderr.decode()

    failures = 0
    all_cycles = []
    print(f"{'script':26s} {'parity':8s} {'frames':>6s} {'mean':>8s} {'p95':>8s} {'max':>8s}  max/30Hz")
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for name, rc, ref, out, cycles, err in ex.map(job, sorted(args.inputs)):
            ok = rc == 0 and ref == out
            failures += not ok
            if not cycles:
                print(f"{name:26s} {'FAIL':8s} no timing data (rc={rc})\n{err[-500:]}")
                continue
            all_cycles += cycles
            mean = sum(cycles) / len(cycles)
            print(f"{name:26s} {'PASS' if ok else 'FAIL':8s} {len(cycles):6d} {mean:8.0f} "
                  f"{pct(cycles, 0.95):8d} {max(cycles):8d}  {100 * max(cycles) / BUDGET_30HZ:5.1f}%")

    if all_cycles:
        mean = sum(all_cycles) / len(all_cycles)
        print()
        print(f"{len(args.inputs) - failures}/{len(args.inputs)} ROMs bit-exact vs reference")
        print(f"Mario update, all frames: mean {mean:.0f} cycles, p95 {pct(all_cycles, 0.95)}, "
              f"p99 {pct(all_cycles, 0.99)}, max {max(all_cycles)}")
        print(f"  = {100 * mean / BUDGET_30HZ:.1f}% (mean) / {100 * max(all_cycles) / BUDGET_30HZ:.1f}% (worst) "
              f"of a 30 Hz game frame ({BUDGET_30HZ} cycles)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
