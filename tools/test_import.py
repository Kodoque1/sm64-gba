#!/usr/bin/env python3
"""Checks tools/import_rom.py's ROM reading against the decomp's own tools
(tools/sm64tools/mio0, n64graphics and tools/skyconv, built from the pinned
decomp's sources into build/tools):

  1. MIO0: data compressed by the decomp's mio0 decompresses to the original.
  2. Texture formats: random texels in every format the import supports,
     decoded by import_rom.py and exported by n64graphics, are identical.
  3. Byte orders: .v64 and .n64 orderings normalize to the .z64 bytes.
  4. With a ROM (--rom): every texture the import reads (Mario's and the four
     levels') and the skyboxes, decoded by import_rom.py and extracted the
     way the decomp's extract_assets.py does it, are identical.

Usage: test_import.py [--decomp sm64] [--rom ROM]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import import_rom as ir  # noqa: E402
import n64gfx  # noqa: E402

FORMATS = {"rgba16": 2, "rgba32": 4, "ia16": 2, "ia8": 1, "ia4": 0.5, "i8": 1, "i4": 0.5}


def build_tools(decomp, out):
    os.makedirs(out, exist_ok=True)
    t = os.path.join(decomp, "tools")
    st = os.path.join(t, "sm64tools")
    cmds = [
        ["gcc", "-O2", "-I", st, "-DMIO0_STANDALONE", os.path.join(st, "libmio0.c"), "-o", os.path.join(out, "mio0")],
        ["gcc", "-O2", "-I", st, "-DN64GRAPHICS_STANDALONE", os.path.join(st, "n64graphics.c"),
         os.path.join(st, "utils.c"), "-o", os.path.join(out, "n64graphics"), "-lm"],
        ["gcc", "-O2", "-I", t, "-I", st, os.path.join(t, "skyconv.c"), os.path.join(st, "n64graphics.c"),
         os.path.join(st, "utils.c"), "-o", os.path.join(out, "skyconv"), "-lm"],
    ]
    for c in cmds:
        subprocess.run(c, check=True, capture_output=True)


def png_pixels(path):
    b = Image.open(path).convert("RGBA").tobytes()
    return [tuple(b[i:i + 4]) for i in range(0, len(b), 4)]


def check(name, ok, detail=""):
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail else ""))
    return ok


def test_mio0(tools, tmp):
    rng = random.Random(1)
    ok = True
    for size in (1, 17, 4096, 70000):
        # compressible: runs, repeats and noise
        data = bytearray()
        while len(data) < size:
            k = rng.random()
            if k < 0.3:
                data += bytes([rng.randrange(256)]) * rng.randrange(1, 40)
            elif k < 0.6 and len(data) > 8:
                start = rng.randrange(len(data))
                data += data[start:start + rng.randrange(3, 30)]
            else:
                data += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 20)))
        data = bytes(data[:size])
        raw, comp = os.path.join(tmp, "raw.bin"), os.path.join(tmp, "comp.bin")
        open(raw, "wb").write(data)
        subprocess.run([os.path.join(tools, "mio0"), "-c", raw, comp], check=True, capture_output=True)
        blob = b"\x55" * 37 + open(comp, "rb").read()  # at an offset, as in a ROM
        ok &= check("mio0 %d bytes" % size, ir.mio0_decode(blob, 37) == data)
    return ok


def test_formats(tools, tmp):
    rng = random.Random(2)
    ok = True
    w, h = 32, 16
    for fmt, bpp in FORMATS.items():
        raw = bytes(rng.randrange(256) for _ in range(int(w * h * bpp)))
        rawf, png = os.path.join(tmp, "t.bin"), os.path.join(tmp, "t.png")
        open(rawf, "wb").write(raw)
        subprocess.run([os.path.join(tools, "n64graphics"), "-e", rawf, "-g", png, "-f", fmt,
                        "-w", str(w), "-h", str(h)], check=True, capture_output=True)
        ref = png_pixels(png)
        ours = [tuple(p) for p in ir.decode_texture(raw, fmt, w, h)]
        bad = sum(1 for a, b in zip(ours, ref) if a != b)
        ok &= check("texture format %s" % fmt, bad == 0 and len(ours) == len(ref),
                    "" if bad == 0 else "%d of %d texels differ" % (bad, len(ref)))
    return ok


def test_byte_orders():
    rng = random.Random(3)
    z64 = b"\x80\x37\x12\x40" + bytes(rng.randrange(256) for _ in range(4092))
    v64 = bytearray(len(z64))
    v64[0::2], v64[1::2] = z64[1::2], z64[0::2]
    n64 = bytearray(len(z64))
    for i in range(0, len(z64), 4):
        n64[i:i + 4] = z64[i:i + 4][::-1]
    ok = True
    for name, data in (("z64", z64), ("v64", bytes(v64)), ("n64", bytes(n64))):
        norm, kind = ir.normalize_rom(data)
        ok &= check("byte order %s" % name, norm == z64 and kind == name)
    return ok


def textures_used(decomp):
    paths = set()
    _, tris = n64gfx.mario_triangles(decomp)
    for lv in ir.LEVELS:
        tris = tris + n64gfx.level_triangles(decomp, lv)
    for t in tris:
        tex = t["state"]["texture"]
        if tex and not tex.startswith("?"):
            paths.add(tex)
    return sorted(paths)


def test_rom(tools, tmp, decomp, rom_path):
    rom, _ = ir.normalize_rom(open(rom_path, "rb").read())
    ver, _ = ir.identify(rom, decomp)
    assets = ir.Assets(rom, ver, decomp)
    romz = os.path.join(tmp, "rom.z64")
    open(romz, "wb").write(rom)
    amap = assets.map
    ok, n = True, 0
    for path in textures_used(decomp):
        meta = amap[path]
        size, pos = meta[-2], meta[-1][ver]
        if len(pos) == 2:
            block = subprocess.run([os.path.join(tools, "mio0"), "-d", "-o", str(pos[0]), romz, "-"],
                                   check=True, capture_output=True).stdout
            raw = block[pos[1]:pos[1] + size]
        else:
            raw = rom[pos[0]:pos[0] + size]
        rawf, png = os.path.join(tmp, "a.bin"), os.path.join(tmp, "a.png")
        open(rawf, "wb").write(raw)
        subprocess.run([os.path.join(tools, "n64graphics"), "-e", rawf, "-g", png, "-f", path.split(".")[-2],
                        "-w", str(meta[0]), "-h", str(meta[1])], check=True, capture_output=True)
        w, h, px = assets.texture(path)
        same = [tuple(p) for p in px] == png_pixels(png)
        if not same:
            check("texture %s" % path, False)
        ok &= same
        n += 1
    check("%d textures from the ROM identical to the decomp's extraction" % n, ok)
    sky_ok = True
    for name in ir.SKYBOXES:
        raw, _ = assets.raw("textures/skyboxes/%s.png" % name)
        rawf, png = os.path.join(tmp, "s.bin"), os.path.join(tmp, "s.png")
        open(rawf, "wb").write(raw)
        subprocess.run([os.path.join(tools, "skyconv"), "--type", "sky", "--combine", rawf, png],
                       check=True, capture_output=True)
        ours = [tuple(p) for row in assets.skybox(name) for p in row]
        sky_ok &= ours == png_pixels(png)
    ok &= check("%d skyboxes identical to skyconv's" % len(ir.SKYBOXES), sky_ok)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--decomp", default="sm64")
    ap.add_argument("--rom")
    ap.add_argument("--tools", default="build/tools")
    args = ap.parse_args()
    build_tools(args.decomp, args.tools)
    with tempfile.TemporaryDirectory() as tmp:
        ok = test_mio0(args.tools, tmp)
        ok &= test_formats(args.tools, tmp)
        ok &= test_byte_orders()
        if args.rom:
            ok &= test_rom(args.tools, tmp, args.decomp, args.rom)
        else:
            print("(no --rom: skipping the checks against a real ROM)")
    print("PASS: import checks" if ok else "FAIL: import checks")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
