#!/usr/bin/env python3
"""Run every input script on the reference and target builds and compare.

For each script: identical logs -> PASS. Otherwise report the first divergent
frame, which fields differ, and both values (float bits decoded), plus the
frame before it for context.

Also prints action coverage across the corpus, using action names parsed
from the decomp's include/sm64.h, so you can see what the inputs exercise.
"""
import argparse
import concurrent.futures as cf
import os
import re
import shlex
import struct
import subprocess
import sys

FIELDS = ["frame", "action", "actionState", "actionTimer",
          "pos.x", "pos.y", "pos.z", "vel.x", "vel.y", "vel.z", "forwardVel",
          "faceAngle.pitch", "faceAngle.yaw", "faceAngle.roll",
          "floorHeight", "animID", "animFrame"]
FLOAT_FIELDS = {"pos.x", "pos.y", "pos.z", "vel.x", "vel.y", "vel.z",
                "forwardVel", "floorHeight"}


def f32(hexbits):
    return struct.unpack(">f", bytes.fromhex(hexbits))[0]


def fmt(field, value, actions):
    if field in FLOAT_FIELDS:
        return f"{value} ({f32(value)!r})"
    if field == "action":
        return f"{value} ({actions.get(int(value, 16), '?')})"
    return value


def load_action_names(sm64_dir):
    names = {}
    path = os.path.join(sm64_dir, "include", "sm64.h")
    if not os.path.exists(path):
        return names
    for m in re.finditer(r"#define\s+(ACT_\w+)\s+0x([0-9A-Fa-f]+)", open(path).read()):
        name, val = m.group(1), int(m.group(2), 16)
        if name.startswith(("ACT_FLAG_", "ACT_GROUP_", "ACT_ID_")):
            continue
        names.setdefault(val, name)
    return names


def run(cmd, inp):
    with open(inp, "rb") as f:
        r = subprocess.run(cmd, stdin=f, capture_output=True, timeout=1800)
    return r.returncode, r.stdout.decode(), r.stderr.decode()


def compare(name, ref, tgt, actions):
    ref_lines, tgt_lines = ref.splitlines(), tgt.splitlines()
    for i, (a, b) in enumerate(zip(ref_lines, tgt_lines)):
        if a == b:
            continue
        fa, fb = a.split(), b.split()
        report = [f"FAIL {name}: first divergence at log line {i} (frame {fa[0]})"]
        if i > 0:
            report.append(f"  previous frame (identical): {ref_lines[i - 1]}")
        for field, va, vb in zip(FIELDS, fa, fb):
            if va != vb:
                report.append(f"  {field:16s} ref {fmt(field, va, actions)}")
                report.append(f"  {'':16s} tgt {fmt(field, vb, actions)}")
        return "\n".join(report)
    if len(ref_lines) != len(tgt_lines):
        return f"FAIL {name}: logs differ in length ({len(ref_lines)} vs {len(tgt_lines)})"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--arm", required=True)
    ap.add_argument("--qemu", default="qemu-arm -cpu ti925t")
    ap.add_argument("--sm64", default="sm64")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    actions = load_action_names(args.sm64)
    ref_cmd = [args.host]
    tgt_cmd = shlex.split(args.qemu) + [args.arm]

    def job(path):
        rc1, out1, err1 = run(ref_cmd, path)
        rc2, out2, err2 = run(tgt_cmd, path)
        return path, rc1, out1, err1, rc2, out2, err2

    failures = 0
    total_frames = 0
    seen = {}
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for path, rc1, out1, err1, rc2, out2, err2 in ex.map(job, sorted(args.inputs)):
            name = os.path.basename(path)
            if rc1 != 0 or rc2 != 0:
                failures += 1
                print(f"FAIL {name}: exit codes ref={rc1} tgt={rc2}\n{err1}{err2}")
                continue
            frames = [l for l in out1.splitlines() if l and l[0].isdigit()]
            total_frames += len(frames) - 1
            for l in frames:
                act = int(l.split()[1], 16)
                seen[act] = seen.get(act, 0) + 1
            problem = compare(name, out1, out2, actions)
            digest = out1.strip().splitlines()[-1]
            if problem:
                failures += 1
                print(problem)
            else:
                print(f"PASS {name:28s} {len(frames) - 1:6d} frames  {digest}")

    print()
    print(f"{len(args.inputs) - failures}/{len(args.inputs)} scripts bit-exact, "
          f"{total_frames} frames total")
    print(f"action coverage: {len(seen)} distinct actions reached")
    top = sorted(seen.items(), key=lambda kv: -kv[1])
    print("  " + ", ".join(f"{actions.get(a, hex(a))}" for a, _ in top))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
