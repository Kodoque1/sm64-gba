#!/usr/bin/env python3
"""Generate the input corpus for the parity harness.

Two kinds of scripts:
  * scripted: specific movement tech (jumps, long jumps, dives, slides...)
    so every major action group is exercised on purpose;
  * fuzz: long seeded random runs with human-like stick motion and button
    holds, which find the edge cases nobody thinks to script.

Usage: gen_inputs.py <out_dir> [--fuzz-seeds N] [--fuzz-frames N]
"""
import argparse
import math
import os
import random

A, B, Z = 0x8000, 0x4000, 0x2000
LEVELS = ["bob", "wf", "ccm", "bitdw"]


class Script:
    def __init__(self, level, title):
        self.lines = [f"# {title}", f"level {level}"]
        self.cam = 0

    def frames(self, n, x=0, y=0, buttons=0):
        for _ in range(n):
            self.lines.append(f"{x} {y} {buttons:04x} {self.cam}")
        return self

    def stick(self, angle_deg, mag=80):
        rad = math.radians(angle_deg)
        return round(math.sin(rad) * mag), round(math.cos(rad) * mag)

    def text(self):
        return "\n".join(self.lines) + "\n"


def scripted():
    out = {}

    s = Script("bob", "walk, run, turn in place, circles under a rotating camera")
    s.frames(10)
    s.frames(30, 0, 30)          # walk
    s.frames(60, 0, 80)          # run
    s.frames(20, 0, -80)         # turnaround / skid
    for i in range(240):
        s.cam = (i * 137) & 0xFFFF
        x, y = s.stick(i * 3)
        s.frames(1, x, y)
    s.frames(30)
    out["bob_run_turn"] = s

    s = Script("bob", "single, double, triple jump chains")
    s.frames(10)
    for _ in range(3):
        s.frames(40, 0, 80)
        s.frames(6, 0, 80, A)
        s.frames(18, 0, 80)
        s.frames(3, 0, 80)
        s.frames(4, 0, 80, A)    # double, pressed right on landing
        s.frames(25, 0, 80)
        s.frames(4, 0, 80, A)    # triple
        s.frames(40, 0, 80)
        s.frames(30)
    out["bob_jump_chain"] = s

    s = Script("bob", "long jumps, backflips, side flips")
    s.frames(10)
    for _ in range(3):
        s.frames(45, 0, 80)
        s.frames(2, 0, 80, Z)
        s.frames(5, 0, 80, Z | A)   # long jump
        s.frames(35, 0, 80)
        s.frames(20)
        s.frames(8, 0, 0, Z)        # crouch
        s.frames(5, 0, 0, Z | A)    # backflip
        s.frames(35)
        s.frames(20, 0, 80)
        s.frames(4, 0, -80)         # turnaround
        s.frames(5, 0, -80, A)      # side flip
        s.frames(35, 0, -80)
        s.frames(20)
    out["bob_flips_longjump"] = s

    s = Script("bob", "dives, belly slides, slide kicks, ground pounds")
    s.frames(10)
    for _ in range(3):
        s.frames(40, 0, 80)
        s.frames(3, 0, 80, B)       # dive
        s.frames(30, 0, 80)
        s.frames(5, 0, 80, A)       # rollout
        s.frames(30, 0, 80)
        s.frames(5, 0, 80, Z)
        s.frames(3, 0, 80, Z | B)   # slide kick
        s.frames(40, 0, 80)
        s.frames(5, 0, 0, A)        # jump
        s.frames(10)
        s.frames(5, 0, 0, Z)        # ground pound
        s.frames(40)
    out["bob_dive_slidekick_gp"] = s

    s = Script("ccm", "run down the mountain: slopes, slides, steep ground")
    s.frames(10)
    for i in range(900):
        s.cam = 0x4000
        x, y = s.stick(90 + 30 * math.sin(i / 40.0))
        b = A if (i % 90) < 4 else (Z if 40 <= (i % 120) < 44 else 0)
        s.frames(1, x, y, b)
    out["ccm_slopes"] = s

    s = Script("wf", "wall contact: run at walls from several angles and jump")
    s.frames(10)
    for k in range(8):
        ang = k * 45
        x, y = s.stick(ang)
        s.frames(60, x, y)
        s.frames(4, x, y, A)
        s.frames(10, x, y)
        s.frames(4, -x, -y, A)      # wall kick attempt
        s.frames(30, -x, -y)
        s.frames(15)
    out["wf_walls"] = s

    s = Script("bitdw", "edges and long falls, including off the course")
    s.frames(10)
    for k in range(6):
        x, y = s.stick(k * 60)
        s.frames(90, x, y)
        s.frames(5, x, y, A)
        s.frames(60, x, y)
    out["bitdw_edges"] = s

    return out


def fuzz(level, seed, frames):
    """Smooth random stick, human-ish button holds, drifting camera."""
    rng = random.Random(seed)
    s = Script(level, f"fuzz seed={seed} frames={frames}")
    angle = rng.uniform(0, 360)
    mag = 0.0
    target_mag = 80.0
    held = {A: 0, B: 0, Z: 0}
    cam = rng.randrange(0x10000)
    for _ in range(frames):
        angle += rng.gauss(0, 8)
        if rng.random() < 0.02:
            target_mag = rng.choice([0, 0, 20, 50, 80, 80, 80])
        if rng.random() < 0.005:
            angle += 180                       # sharp turnarounds
        mag += (target_mag - mag) * 0.3
        x, y = s.stick(angle, mag)
        x = max(-128, min(127, x))
        y = max(-128, min(127, y))
        buttons = 0
        for btn, p in ((A, 0.05), (B, 0.015), (Z, 0.02)):
            if held[btn] > 0:
                held[btn] -= 1
            elif rng.random() < p:
                held[btn] = rng.choice([1, 2, 3, 5, 8, 20])
            if held[btn] > 0:
                buttons |= btn
        cam = (cam + int(rng.gauss(0, 60))) & 0xFFFF
        if cam >= 0x8000:
            s.cam = cam - 0x10000
        else:
            s.cam = cam
        s.frames(1, x, y, buttons)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir")
    ap.add_argument("--fuzz-seeds", type=int, default=4)
    ap.add_argument("--fuzz-frames", type=int, default=3000)
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)

    scripts = scripted()
    for level in LEVELS:
        for seed in range(args.fuzz_seeds):
            rng_seed = seed * 7919 + LEVELS.index(level)
            scripts[f"fuzz_{level}_{seed:02d}"] = fuzz(level, rng_seed, args.fuzz_frames)

    for name, s in scripts.items():
        with open(os.path.join(args.out_dir, name + ".inp"), "w") as f:
            f.write(s.text())
    print(f"wrote {len(scripts)} scripts to {args.out_dir}")


if __name__ == "__main__":
    main()
