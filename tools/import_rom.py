#!/usr/bin/env python3
"""Import tool, in the spirit of libsm64: reads YOUR copy of Super Mario 64
and converts what the demo needs into build/import/import_data.c.

    tools/import_rom.py ROM [--decomp sm64] [--out build/import]

- The ROM can be .z64, .v64 or .n64 (byte order is detected) and must be one
  the decomp knows (US, JP, EU, Shindou, iQue): its SHA-1 is checked against
  the decomp's sm64.<version>.sha1.
- Geometry (Mario's low-poly model, the level meshes) comes from the decomp's
  sources; the ROM provides the textures, located with the decomp's
  assets.json (MIO0 decompression and N64 texture decoding are done here, so
  no decomp tools need to be built).
- Nothing from the ROM is copied as is: textures are reduced to one average
  color per triangle, skyboxes to a 16-step gradient.

The output is derived from your ROM: keep it, and the demo ROM built from it,
to yourself.
"""
import argparse
import hashlib
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import n64gfx  # noqa: E402

LEVELS = ["bob", "wf", "ccm", "bitdw"]  # the demo's level order
SKYBOXES = ["water", "bitfs", "wdw", "cloud_floor", "ccm", "ssl", "bbh", "bidw", "clouds", "bits"]
BACKGROUNDS = ["BACKGROUND_OCEAN_SKY", "BACKGROUND_FLAMING_SKY", "BACKGROUND_UNDERWATER_CITY",
               "BACKGROUND_BELOW_CLOUDS", "BACKGROUND_SNOW_MOUNTAINS", "BACKGROUND_DESERT",
               "BACKGROUND_HAUNTED", "BACKGROUND_GREEN_SKY", "BACKGROUND_ABOVE_CLOUDS",
               "BACKGROUND_PURPLE_SKY"]

LEVEL_COLORS = 48     # palette: 48 colors x 4 fog levels = indices 0..191
MARIO_MATERIALS = 8   # palette: 8 materials x 4 light levels = 192..223
SUN = (0.40, 0.82, 0.41)  # the demo's sun (render_build.c)


# --- ROM --------------------------------------------------------------------

def normalize_rom(data):
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data, "z64"
    if magic == b"\x37\x80\x40\x12":  # v64: 16-bit byte swapped
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b), "v64"
    if magic == b"\x40\x12\x37\x80":  # n64: 32-bit little endian
        b = bytearray(len(data))
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b), "n64"
    raise SystemExit("not an N64 ROM (unknown header %s)" % magic.hex())


def identify(rom, decomp):
    sha = hashlib.sha1(rom).hexdigest()
    for ver in ("us", "jp", "eu", "sh", "cn"):
        path = os.path.join(decomp, "sm64.%s.sha1" % ver)
        if os.path.exists(path) and open(path).read().split()[0] == sha:
            return ver, sha
    raise SystemExit("ROM not recognized (SHA-1 %s): the decomp supports the US, JP, EU, "
                     "Shindou and iQue releases, unmodified" % sha)


def mio0_decode(buf, off):
    if buf[off:off + 4] != b"MIO0":
        raise ValueError("no MIO0 block at 0x%x" % off)
    size = int.from_bytes(buf[off + 4:off + 8], "big")
    comp = off + int.from_bytes(buf[off + 8:off + 12], "big")
    raw = off + int.from_bytes(buf[off + 12:off + 16], "big")
    out = bytearray()
    bit = 0
    while len(out) < size:
        if buf[off + 16 + bit // 8] & (0x80 >> (bit % 8)):
            out.append(buf[raw])
            raw += 1
        else:
            v = (buf[comp] << 8) | buf[comp + 1]
            comp += 2
            length, back = (v >> 12) + 3, (v & 0xFFF) + 1
            for _ in range(length):
                out.append(out[-back])
        bit += 1
    return bytes(out)


def s5(v):
    return v * 255 // 31


def decode_texture(raw, fmt, w, h):
    """N64 texture -> list of (r, g, b, a), 0..255 (as the decomp's n64graphics)."""
    px = []
    n = w * h
    if fmt == "rgba16":
        for i in range(n):
            v = (raw[2 * i] << 8) | raw[2 * i + 1]
            px.append((s5(v >> 11), s5((v >> 6) & 31), s5((v >> 1) & 31), 255 if v & 1 else 0))
    elif fmt == "rgba32":
        for i in range(n):
            px.append(tuple(raw[4 * i:4 * i + 4]))
    elif fmt == "ia16":
        for i in range(n):
            px.append((raw[2 * i],) * 3 + (raw[2 * i + 1],))
    elif fmt == "ia8":
        for i in range(n):
            px.append(((raw[i] >> 4) * 0x11,) * 3 + ((raw[i] & 15) * 0x11,))
    elif fmt == "ia4":
        for i in range(n):
            b = raw[i // 2] >> 4 if i % 2 == 0 else raw[i // 2] & 15
            px.append((((b >> 1) & 7) * 0x24,) * 3 + (255 if b & 1 else 0,))
    elif fmt == "i8":
        for i in range(n):
            px.append((raw[i],) * 3 + (255,))
    elif fmt == "i4":
        for i in range(n):
            b = raw[i // 2] >> 4 if i % 2 == 0 else raw[i // 2] & 15
            px.append((b * 0x11,) * 3 + (b * 0x11,))
    else:
        raise ValueError("texture format %s not supported" % fmt)
    return px


class Assets:
    def __init__(self, rom, ver, decomp):
        self.rom, self.ver = rom, ver
        self.map = json.load(open(os.path.join(decomp, "assets.json")))
        self.blocks, self.cache = {}, {}

    def raw(self, path):
        meta = self.map[path]
        size, pos = meta[-2], meta[-1][self.ver]
        if len(pos) == 2:
            if pos[0] not in self.blocks:
                self.blocks[pos[0]] = mio0_decode(self.rom, pos[0])
            return self.blocks[pos[0]][pos[1]:pos[1] + size], meta
        return self.rom[pos[0]:pos[0] + size], meta

    def texture(self, path):
        """(w, h, pixels) for an asset path like 'actors/mario/mario_logo.rgba16.png'."""
        if path not in self.cache:
            data, meta = self.raw(path)
            w, h = meta[0], meta[1]
            self.cache[path] = (w, h, decode_texture(data, path.split(".")[-2], w, h))
        return self.cache[path]

    def skybox(self, name):
        """The skybox as 8 rows x 8 columns of 31x31 tiles (skyconv's layout)."""
        data, _ = self.raw("textures/skyboxes/%s.png" % name)
        table_at = len(data) - 80 * 4
        table = [int.from_bytes(data[table_at + 4 * i:table_at + 4 * i + 4], "big") for i in range(80)]
        base = table[0]
        img = [[None] * 248 for _ in range(248)]
        for row in range(8):
            for col in range(8):
                t = (table[row * 10 + col] - base) // 0x800
                tile = decode_texture(data[t * 2048:(t + 1) * 2048], "rgba16", 32, 32)
                for y in range(31):
                    for x in range(31):
                        img[row * 31 + y][col * 31 + x] = tile[y * 32 + x]
        return img


# --- colors -----------------------------------------------------------------

def sample_points(n):
    """Barycentric sample points spread over a triangle."""
    pts = []
    for i in range(n):
        for j in range(n - i):
            a, b = (i + 1 / 3) / n, (j + 1 / 3) / n
            pts.append((1 - a - b, a, b))
    return pts


SAMPLES = sample_points(8)


def texel(tex, s, t, clamp_s, clamp_t):
    w, h, px = tex
    s, t = int(math.floor(s)), int(math.floor(t))
    s = min(max(s, 0), w - 1) if clamp_s else s % w
    t = min(max(t, 0), h - 1) if clamp_t else t % h
    return px[t * w + s]


def light_dir(light):
    d = light["dir"]
    l = math.sqrt(sum(x * x for x in d)) or 1
    return [x / l for x in d]


def shade_of(tri, k, bake):
    """(ambient, diffuse) per vertex k, each RGB 0..1. Unlit: the vertex
    color. Lit: the Lights1 colors; with bake, the vertex is lit as the RSP
    does (ambient + color * max(0, n.l), clamped) against the light's own
    direction; without, the diffuse part is kept for per-frame lighting."""
    st, v = tri["state"], tri["v"][k]
    if st["lighting"] and st["light"] is not None:
        amb = [c / 255 for c in st["light"]["amb"]]
        col = [c / 255 for c in st["light"]["col"]]
        if not bake:
            return amb, col
        d = max(0.0, sum(a * b for a, b in zip(vertex_normal(v), light_dir(st["light"]))))
        return [min(1.0, amb[c] + col[c] * d) for c in range(3)], [0.0, 0.0, 0.0]
    return [c / 255 for c in v["c"][:3]], [0.0, 0.0, 0.0]


def vertex_normal(v):
    n = [x - 256 if x > 127 else x for x in v["c"][:3]]
    l = math.sqrt(sum(x * x for x in n)) or 1
    return [x / l for x in n]


def tri_color(tri, assets, bake=False):
    """Per-triangle color as (ambient RGB, diffuse RGB, coverage): the average
    over sample points of what the combiner outputs, with the light's diffuse
    term kept separate (Mario is lit per frame; levels are baked by the caller)."""
    st = tri["state"]
    shades = [shade_of(tri, k, bake) for k in range(3)]
    tex = None
    if st["texture"] and not st["texture"].startswith("?") and not st["texgen"]:
        tex = assets.texture(st["texture"])
    comb = st["combine"]
    amb, dif, cov = [0.0] * 3, [0.0] * 3, 0.0
    for b in SAMPLES:
        sa = [sum(b[k] * shades[k][0][c] for k in range(3)) for c in range(3)]
        sd = [sum(b[k] * shades[k][1][c] for k in range(3)) for c in range(3)]
        if tex is not None:
            s = sum(b[k] * tri["v"][k]["uv"][0] for k in range(3)) * st["tex_scale"][0] / 32
            t = sum(b[k] * tri["v"][k]["uv"][1] for k in range(3)) * st["tex_scale"][1] / 32
            r, g, bb, a = texel(tex, s, t, st.get("clamp_s"), st.get("clamp_t"))
            tc, ta = [r / 255, g / 255, bb / 255], a / 255
        else:
            tc, ta = [1.0, 1.0, 1.0], 1.0
        if tex is None or "SHADE" in comb and "MODULATE" not in comb and "BLEND" not in comb:
            ca, cd, alpha = sa, sd, 1.0
        elif "DECAL" in comb:
            ca, cd, alpha = tc, [0.0] * 3, ta
        elif "BLEND" in comb:
            ca = [sa[c] + (tc[c] - sa[c]) * ta for c in range(3)]
            cd = [sd[c] * (1 - ta) for c in range(3)]
            alpha = 1.0
        else:  # MODULATE*
            ca = [sa[c] * tc[c] for c in range(3)]
            cd = [sd[c] * tc[c] for c in range(3)]
            alpha = ta if "RGBA" in comb or "IA" in comb or tri["layer"] == "LAYER_ALPHA" else 1.0
        for c in range(3):
            amb[c] += ca[c] * alpha
            dif[c] += cd[c] * alpha
        cov += alpha
    if not st["lighting"] and "TRANSPARENT" in (tri["layer"] or ""):
        cov *= sum(v["c"][3] for v in tri["v"]) / (3 * 255.0)  # vertex alpha
    if cov <= 0:
        return [0.0] * 3, [0.0] * 3, 0.0
    return [x / cov for x in amb], [x / cov for x in dif], cov / len(SAMPLES)


def face_normal(p):
    a = [p[1][i] - p[0][i] for i in range(3)]
    b = [p[2][i] - p[0][i] for i in range(3)]
    n = [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
    l = math.sqrt(sum(x * x for x in n))
    return [x / l for x in n] if l > 0 else None


def kmeans(points, weights, k, iters=40):
    """Weighted k-means in RGB (deterministic seeding: farthest point)."""
    if len(points) <= k:
        return [list(p) for p in points], list(range(len(points)))
    order = sorted(range(len(points)), key=lambda i: -weights[i])
    centers = [list(points[order[0]])]
    while len(centers) < k:
        best, bd = None, -1
        for i in order:
            d = min(sum((points[i][c] - ce[c]) ** 2 for c in range(len(ce))) for ce in centers) * (weights[i] ** 0.5)
            if d > bd:
                best, bd = i, d
        centers.append(list(points[best]))
    assign = [0] * len(points)
    for _ in range(iters):
        for i, p in enumerate(points):
            assign[i] = min(range(k), key=lambda j: sum((p[c] - centers[j][c]) ** 2 for c in range(len(p))))
        for j in range(k):
            ws = sum(weights[i] for i in range(len(points)) if assign[i] == j)
            if ws > 0:
                centers[j] = [sum(points[i][c] * weights[i] for i in range(len(points)) if assign[i] == j) / ws
                              for c in range(len(points[0]))]
    return centers, assign


def rgb15(c):
    r, g, b = [min(31, max(0, int(round(x * 31)))) for x in c]
    return r | (g << 5) | (b << 10)


# --- Mario ------------------------------------------------------------------

def convert_mario(decomp, assets):
    parts, tris = n64gfx.mario_triangles(decomp)
    verts, vindex, out = [], {}, []
    feats, weights = [], []
    for t in tris:
        p = [v["pos"] for v in t["v"]]
        n = face_normal(p)
        if n is None:
            continue
        amb, dif, cov = tri_color(t, assets)
        if cov < 0.3:
            continue
        ids = []
        for v in p:
            key = (t["part"], tuple(v))
            if key not in vindex:
                vindex[key] = len(verts)
                verts.append(key)
            ids.append(vindex[key])
        area = 0.5 * math.sqrt(sum(x * x for x in [
            (p[1][1] - p[0][1]) * (p[2][2] - p[0][2]) - (p[1][2] - p[0][2]) * (p[2][1] - p[0][1]),
            (p[1][2] - p[0][2]) * (p[2][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[2][2] - p[0][2]),
            (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0])]))
        out.append({"v": ids, "n": n, "part": t["part"]})
        feats.append(amb + dif)
        weights.append(area + 1)
    centers, assign = kmeans(feats, weights, MARIO_MATERIALS)
    pal = []
    for j in range(MARIO_MATERIALS):
        c = centers[j] if j < len(centers) else [0.0] * 6
        for lvl in range(4):
            pal.append(rgb15([min(1.0, c[i] + c[3 + i] * lvl / 3) for i in range(3)]))
    for t, a in zip(out, assign):
        t["mat"] = a
    return verts, out, pal


# --- levels -----------------------------------------------------------------

def level_background(decomp, level):
    src = open(os.path.join(decomp, "levels/%s/areas/1/geo.inc.c" % level)).read()
    for i, name in enumerate(BACKGROUNDS):
        if "GEO_BACKGROUND(%s," % name in src:
            return SKYBOXES[i]
    return None


def sky_gradient(assets, name):
    """16 colors from 43.6 degrees above the horizon (entry 0) down to the
    horizon (entry 15), averaged around the full 360 degrees. Skybox rows: the
    horizon is at tile row 4, one tile row per 30 degrees (skybox.c)."""
    img = assets.skybox(name)
    out = []
    for i in range(16):
        elev = 43.6 * (15 - i) / 15
        row = 4 - elev / 30
        y0, y1 = int(row * 31) - 4, int(row * 31) + 4
        acc, n = [0, 0, 0], 0
        for y in range(max(0, y0), min(248, y1)):
            for p in img[y]:
                for c in range(3):
                    acc[c] += p[c]
                n += 1
        out.append([x / n / 255 for x in acc])
    return out


def convert_level(decomp, assets, level):
    tris = n64gfx.level_triangles(decomp, level)
    verts, vindex, out = [], {}, []
    colors, weights = [], []
    for t in tris:
        p = [[int(round(x)) for x in v["pos"]] for v in t["v"]]
        n = face_normal(p)
        if n is None:
            continue
        col, _, cov = tri_color(t, assets, bake=True)
        if t["layer"] == "LAYER_ALPHA" and cov < 0.5:
            continue  # mostly transparent cutouts (grass tufts, fence gaps)
        if "TRANSPARENT" in t["layer"] and cov < 0.6:
            continue  # translucent decals (soft shadows, dirt): no blending here
        st = t["state"]
        ids = []
        for v in p:
            key = tuple(v)
            if key not in vindex:
                vindex[key] = len(verts)
                verts.append(key)
            ids.append(vindex[key])
        flags = 0
        if "DECAL" in t["layer"]:
            flags |= 1
        if not st.get("cull", True):
            flags |= 2
        area = 0.5 * math.sqrt(sum((x * 1.0) ** 2 for x in [
            (p[1][1] - p[0][1]) * (p[2][2] - p[0][2]) - (p[1][2] - p[0][2]) * (p[2][1] - p[0][1]),
            (p[1][2] - p[0][2]) * (p[2][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[2][2] - p[0][2]),
            (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0])]))
        out.append({"v": ids, "flags": flags, "n": n, "area": area})
        colors.append(col)
        weights.append(math.sqrt(area) + 1)
    centers, assign = kmeans(colors, weights, LEVEL_COLORS)
    for t, a in zip(out, assign):
        t["color"] = a
    sky = sky_gradient(assets, level_background(decomp, level))
    horizon = sky[15]
    pal = []
    for j in range(LEVEL_COLORS):
        c = centers[j] if j < len(centers) else [0.0] * 3
        for f in range(4):
            pal.append(rgb15([c[i] + (horizon[i] - c[i]) * f * 5 / 16 for i in range(3)]))
    # ground color: the most common (by area) color of upward-facing triangles
    ground = {}
    for t in out:
        if t["n"][1] > 0.7:
            ground[t["color"]] = ground.get(t["color"], 0) + t["area"]
    groundColor = max(ground, key=ground.get) if ground else 0
    return {"verts": verts, "tris": out, "pal": pal, "sky": [rgb15(c) for c in sky],
            "ground": groundColor * 4, "groundIndex": groundColor}


# --- output -----------------------------------------------------------------

def write_c(path, mario, levels, info):
    mverts, mtris, mpal = mario
    with open(path, "w") as f:
        f.write("/* Generated by tools/import_rom.py from %s. Derived from your ROM:\n"
                " * do not distribute. */\n#include \"import.h\"\n\n" % info)
        f.write("const u16 gImpMarioPal[32] = {%s};\n" % ", ".join("0x%04x" % c for c in mpal))
        f.write("const struct ImpMVert gImpMarioVerts[%d] = {\n" % len(mverts))
        for part, p in mverts:
            f.write("    {%d, %d, %d, %d},\n" % (p[0], p[1], p[2], part))
        f.write("};\nconst struct ImpMTri gImpMarioTris[%d] = {\n" % len(mtris))
        for t in mtris:
            n = [int(round(x * 16384)) for x in t["n"]]
            n = [max(-16384, min(16384, x)) for x in n]
            f.write("    {{%d, %d, %d}, {%d, %d, %d}, %d, %d},\n" % (tuple(t["v"]) + tuple(n) + (t["mat"], t["part"])))
        f.write("};\nconst u16 gImpMarioNumVerts = %d, gImpMarioNumTris = %d;\n\n" % (len(mverts), len(mtris)))
        for lv, d in zip(LEVELS, levels):
            f.write("static const s16 sVerts_%s[%d][3] = {\n" % (lv, len(d["verts"])))
            for v in d["verts"]:
                f.write("    {%d, %d, %d},\n" % v)
            f.write("};\nstatic const struct ImpTri sTris_%s[%d] = {\n" % (lv, len(d["tris"])))
            for t in d["tris"]:
                f.write("    {{%d, %d, %d}, %d, %d},\n" % (tuple(t["v"]) + (t["color"] * 4, t["flags"])))
            f.write("};\n")
        f.write("\nconst struct ImpLevel gImpLevels[%d] = {\n" % len(LEVELS))
        for lv, d in zip(LEVELS, levels):
            f.write("    { sVerts_%s, sTris_%s, %d, %d, %d,\n      {%s},\n      {%s} },\n" % (
                lv, lv, len(d["verts"]), len(d["tris"]), d["ground"],
                ", ".join("0x%04x" % c for c in d["pal"]),
                ", ".join("0x%04x" % c for c in d["sky"])))
        f.write("};\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--decomp", default="sm64")
    ap.add_argument("--out", default="build/import")
    args = ap.parse_args()

    rom, order = normalize_rom(open(args.rom, "rb").read())
    ver, sha = identify(rom, args.decomp)
    print("ROM: %s byte order, version %s, SHA-1 %s" % (order, ver, sha))
    assets = Assets(rom, ver, args.decomp)

    mario = convert_mario(args.decomp, assets)
    print("Mario: %d vertices, %d triangles, %d materials" % (len(mario[0]), len(mario[1]), MARIO_MATERIALS))
    levels = []
    for lv in LEVELS:
        d = convert_level(args.decomp, assets, lv)
        print("%-6s %4d vertices, %4d triangles, %d colors" % (lv, len(d["verts"]), len(d["tris"]), LEVEL_COLORS))
        levels.append(d)
    os.makedirs(args.out, exist_ok=True)
    write_c(os.path.join(args.out, "import_data.c"), mario, levels, "SM64 %s (%s)" % (ver.upper(), sha[:12]))
    print("wrote %s" % os.path.join(args.out, "import_data.c"))


if __name__ == "__main__":
    main()
