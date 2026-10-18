#!/usr/bin/env python3
"""Report on a bench run from the save RAM it left (a .sav; demo/bench.h).

  bench_report.py run.sav [--rom sm64bench.gba] [--ref other.sav] [--harness H]

--rom      check the ROM checksum the bench recorded against this file
--ref      another run of the same ROM (mGBA's): Mario's inputs and position
           must match on every frame; the frame timings are compared on the
           frames both rendered at the same quality step
--harness  replay the first level through the host reference harness:
           Mario's position must match bit for bit on every frame

Exits 1 if the run is incomplete or Mario doesn't match.
"""
import argparse
import collections
import struct
import subprocess
import sys

HEADER, MAGIC = 32, b"SM64BNCH"
DONE = 0xD0
LEVELS = ["bob", "wf", "ccm", "bitdw"]


def load(path):
    data = open(path, "rb").read()
    if data[:8] != MAGIC:
        sys.exit(f"{path}: no bench record (did the bench ROM run, with SRAM saves?)")
    version, size, count, state, flags = struct.unpack_from("<BBHBB", data, 8)
    if version != 1:
        sys.exit(f"{path}: record version {version}, this tool reads version 1")
    run = {
        "path": path, "done": state == DONE, "debug_regs": bool(flags & 1),
        "checksum": struct.unpack_from("<I", data, 16)[0],
        "loads": struct.unpack_from("<4H", data, 20), "frames": [],
    }
    for i in range(count):
        at = HEADER + i * size
        vb, ql, x, y, buttons, yaw = struct.unpack_from("<BBbbBxh", data, at)
        phys = int.from_bytes(data[at + 8:at + 11], "little")
        render = int.from_bytes(data[at + 11:at + 14], "little")
        pos = struct.unpack_from("<I", data, at + 16)[0]
        run["frames"].append({"v": vb, "q": ql & 15, "l": ql >> 4, "input": (x, y, buttons << 8, yaw),
                              "pos": pos, "phys": phys, "render": render})
    return run


def rom_checksum(path):
    rom = open(path, "rb").read()
    rom += bytes(-len(rom) % 4)
    return sum(struct.unpack(f"<{len(rom) // 4}I", rom)) & 0xFFFFFFFF


def fnv1a(words):
    h = 2166136261
    for w in words:
        for b in w.to_bytes(4, "little"):
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def pct(a, b):
    return 100 * a / b if b else 0.0


def summary(run):
    f = run["frames"]
    kind = "an emulator (mGBA's debug registers answered)" if run["debug_regs"] else \
        "a GBA, or an emulator without mGBA's debug registers"
    state = "complete" if run["done"] else "INCOMPLETE (stopped early)"
    print(f"{run['path']}: {len(f)} frames, {state}")
    print(f"  ran on {kind}")
    loads = ", ".join(f"{LEVELS[i]} {v / 59.7275:.2f} s" for i, v in enumerate(run["loads"]) if v)
    print(f"  level loads: {loads}")
    if not f:
        return
    vb = collections.Counter(x["v"] for x in f)
    print(f"  {pct(vb.get(2, 0), len(f)):.1f}% of frames at 30 fps (VBlanks per frame: "
          + ", ".join(f"{k}: {v}" for k, v in sorted(vb.items())) + ")")
    for lv in sorted(set(x["l"] for x in f)):
        sub = [x for x in f if x["l"] == lv]
        ok = sum(1 for x in sub if x["v"] == 2)
        print(f"    {LEVELS[lv]:6s} {len(sub):4d} frames, {pct(ok, len(sub)):5.1f}% at 30 fps, "
              f"quality steps used: {sorted(set(x['q'] for x in sub))}")
    for key, name in (("phys", "Mario update"), ("render", "render")):
        v = [x[key] for x in f]
        print(f"  {name}: mean {sum(v) // len(v)} max {max(v)} cycles")


def compare(run, ref):
    a, b = run["frames"], ref["frames"]
    for i, (x, y) in enumerate(zip(a, b)):
        if (x["l"], x["input"], x["pos"]) != (y["l"], y["input"], y["pos"]):
            print(f"FAIL: frame {i}: Mario differs from {ref['path']} "
                  f"(input {x['input']} vs {y['input']}, position hash {x['pos']:08x} vs {y['pos']:08x})")
            return False
    print(f"  vs {ref['path']}: Mario's inputs and position identical on all {min(len(a), len(b))} frames")
    same = [(x, y) for x, y in zip(a, b) if x["q"] == y["q"]]
    if not same:
        return True
    for key, name in (("phys", "Mario update"), ("render", "render")):
        ratios = sorted(x[key] / y[key] for x, y in same if y[key])
        total = pct(sum(x[key] for x, _ in same), sum(y[key] for _, y in same)) - 100
        print(f"    {name} cycles: {total:+.2f}% in total over the {len(same)} frames at the same quality "
              f"step (per frame {100 * ratios[0] - 100:+.1f}% to {100 * ratios[-1] - 100:+.1f}%)")
    at30 = [pct(sum(1 for x in r["frames"] if x["v"] == 2), len(r["frames"])) for r in (run, ref)]
    print(f"    at 30 fps: {at30[0]:.1f}% here, {at30[1]:.1f}% there")
    return True


def replay(run, harness):
    first = []
    for f in run["frames"]:
        if f["l"] != 0:
            break
        first.append(f)
    lines = [f"level {LEVELS[0]}"] + [f"{x} {y} {b:04x} {yaw}" for x, y, b, yaw in (f["input"] for f in first)]
    out = subprocess.run([harness], input="\n".join(lines) + "\n", capture_output=True, text=True).stdout
    ref = [l.split()[4:7] for l in out.splitlines() if l and l[0].isdigit()][1:]
    for i, (f, r) in enumerate(zip(first, ref)):
        if f["pos"] != fnv1a(int(v, 16) for v in r):
            print(f"FAIL: frame {i}: Mario's position differs from the host reference harness")
            return False
    if len(ref) < len(first):
        print("FAIL: the harness stopped early")
        return False
    print(f"  {LEVELS[0]}, {len(first)} frames replayed through the host reference: "
          "Mario's position bit-identical on every frame")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sav")
    ap.add_argument("--rom")
    ap.add_argument("--ref")
    ap.add_argument("--harness")
    args = ap.parse_args()

    run = load(args.sav)
    summary(run)
    ok = run["done"]
    if args.rom:
        same = rom_checksum(args.rom) == run["checksum"]
        print(f"  ROM checksum {run['checksum']:08x}: "
              + ("matches " + args.rom if same else
                 f"differs from {args.rom} (another build, or a flash cart that patched the ROM)"))
    if args.ref:
        ok = compare(run, load(args.ref)) and ok
    if args.harness:
        ok = replay(run, args.harness) and ok
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
