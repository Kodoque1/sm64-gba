#!/usr/bin/env python3
"""Reads the decomp's model sources (Vtx arrays, Lights1, display lists, geo
layouts) and runs display lists through a small F3D state machine, producing
triangles with everything needed to color them: positions, per-vertex normals
or colors, texture coordinates, the texture bound at the time, the lights and
the combine mode.

No ROM data is involved here: the decomp ships geometry as source. Textures
come from the user's ROM (tools/import_rom.py) and are only referenced by
name (symbol -> asset path, e.g. "actors/mario/mario_mustache.rgba16.png").
"""
import os
import re

NUM = r"-?(?:0x[0-9a-fA-F]+|\d+)"


def num(s):
    s = s.strip()
    return int(s, 0) if not s.startswith("-") else -int(s[1:], 0)


class Symbols:
    """Index of Vtx, Lights1, Gfx and Texture definitions in a set of files."""

    def __init__(self, root):
        self.root = root
        self.vtx, self.lights, self.gfx, self.tex = {}, {}, {}, {}
        self.files = set()

    def load(self, rel):
        if rel in self.files:
            return
        self.files.add(rel)
        src = open(os.path.join(self.root, rel)).read()
        for m in re.finditer(r"Vtx\s+(\w+)\[\]\s*=\s*\{(.*?)\n\};", src, re.S):
            verts = []
            for v in re.finditer(r"\{\{\{\s*(%s),\s*(%s),\s*(%s)\},\s*(%s),\s*\{\s*(%s),\s*(%s)\},\s*\{\s*(%s),\s*(%s),\s*(%s),\s*(%s)\}\}\}"
                                 % ((NUM,) * 10), m.group(2)):
                g = [num(x) for x in v.groups()]
                verts.append({"pos": g[0:3], "uv": g[4:6], "c": [x & 0xFF for x in g[6:10]]})
            self.vtx[m.group(1)] = verts
        for m in re.finditer(r"Lights1\s+(\w+)\s*=\s*gdSPDefLights1\(([^)]*)\)", src):
            a = [num(x) for x in m.group(2).split(",")]
            self.lights[m.group(1)] = {"amb": a[0:3], "col": a[3:6], "dir": [x - 256 if x > 127 else x for x in a[6:9]]}
        for m in re.finditer(r"Gfx\s+(\w+)\[\]\s*=\s*\{(.*?)\n\};", src, re.S):
            cmds = []
            for c in re.finditer(r"(gs\w+)\((.*?)\),?\s*$", m.group(2), re.M):
                cmds.append((c.group(1), c.group(2)))
            self.gfx[m.group(1)] = cmds
        for m in re.finditer(r"Texture\s+(\w+)\[\]\s*=\s*\{\s*#include\s+\"([^\"]+)\.inc\.c\"", src):
            self.tex[m.group(1)] = m.group(2) + ".png"

    def load_tree(self, rel_dir, pattern=r".*\.c$"):
        for dp, _, fs in os.walk(os.path.join(self.root, rel_dir)):
            for f in sorted(fs):
                if re.match(pattern, f):
                    self.load(os.path.relpath(os.path.join(dp, f), self.root))


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class State:
    def __init__(self):
        self.lighting = True
        self.light = None          # {"amb", "col", "dir"}
        self.texture = None        # asset path
        self.tex_on = False
        self.tex_scale = (1.0, 1.0)
        self.combine = "G_CC_SHADE"
        self.env_alpha = 255
        self.texgen = False
        self.cull = True
        self.clamp = (False, False)

    def snapshot(self):
        return {"lighting": self.lighting, "light": self.light,
                "texture": self.texture if self.tex_on else None,
                "tex_scale": self.tex_scale, "combine": self.combine,
                "env_alpha": self.env_alpha, "texgen": self.texgen,
                "cull": self.cull, "clamp_s": self.clamp[0], "clamp_t": self.clamp[1]}


def run_dl(syms, name, state, emit, layer=None):
    """Interpret display list `name`, calling emit(tri) for each triangle."""
    if name not in syms.gfx:
        raise KeyError("display list %s not found" % name)
    vbuf = [None] * 32
    for cmd, argstr in syms.gfx[name]:
        args = split_args(argstr)
        if cmd == "gsSPVertex":
            m = re.match(r"(\w+)(?:\s*\+\s*(\d+))?", args[0])
            arr, off = m.group(1), int(m.group(2) or 0)
            n, v0 = num(args[1]), num(args[2])
            vbuf[v0:v0 + n] = syms.vtx[arr][off:off + n]
        elif cmd in ("gsSP1Triangle", "gsSP2Triangles"):
            idx = [num(a) for a in args]
            tris = [idx[0:3]] + ([idx[4:7]] if cmd == "gsSP2Triangles" else [])
            for t in tris:
                emit({"v": [vbuf[i] for i in t], "state": state.snapshot(), "layer": layer})
        elif cmd == "gsSPDisplayList":
            run_dl(syms, args[0], state, emit, layer)
        elif cmd == "gsSPBranchList":
            run_dl(syms, args[0], state, emit, layer)
            return
        elif cmd == "gsSPEndDisplayList":
            return
        elif cmd == "gsSPLight":
            m = re.match(r"&(\w+)\.(l|a)", args[0])
            state.light = syms.lights[m.group(1)]
        elif cmd == "gsSPSetGeometryMode":
            if "G_LIGHTING" in args[0]:
                state.lighting = True
            if "G_TEXTURE_GEN" in args[0]:
                state.texgen = True
            if "G_CULL_BACK" in args[0]:
                state.cull = True
        elif cmd == "gsSPClearGeometryMode":
            if "G_LIGHTING" in args[0]:
                state.lighting = False
            if "G_TEXTURE_GEN" in args[0]:
                state.texgen = False
            if "G_CULL_BACK" in args[0]:
                state.cull = False
        elif cmd == "gsSPTexture":
            state.tex_scale = (num(args[0]) / 65536.0, num(args[1]) / 65536.0)
            state.tex_on = args[4].strip() == "G_ON"
        elif cmd == "gsDPSetTextureImage":
            state.texture = syms.tex.get(args[3], "?" + args[3])
        elif cmd == "gsDPSetTile":
            if args[4].strip() == "G_TX_RENDERTILE":
                # cmt, maskt, shiftt, cms, masks, shifts
                state.clamp = ("G_TX_CLAMP" in args[9], "G_TX_CLAMP" in args[6])
        elif cmd == "gsDPLoadTextureBlock":
            state.texture = syms.tex.get(args[0], "?" + args[0])
            state.clamp = ("G_TX_CLAMP" in args[6], "G_TX_CLAMP" in args[7])
        elif cmd == "gsDPSetCombineMode":
            state.combine = args[0]
        elif cmd == "gsDPSetEnvColor":
            state.env_alpha = num(args[3])
        # pipe/tile/load syncs, tile setup, render modes, fog: no effect on color


# --- geo layouts -----------------------------------------------------------

def parse_geo(src):
    layouts = {}
    for m in re.finditer(r"const GeoLayout (\w+)\[\] = \{(.*?)\n\};", src, re.S):
        cmds = re.findall(r"(GEO_\w+)\(([^)]*(?:\([^)]*\)[^)]*)*)\)", m.group(2))
        layouts[m.group(1)] = [(c, split_args(a)) for c, a in cmds]
    return layouts


def geo_tree(layouts, name):
    cmds = layouts[name]
    pos = 0

    def level():
        nonlocal pos
        nodes = []
        while pos < len(cmds):
            cmd, args = cmds[pos]
            pos += 1
            if cmd == "GEO_CLOSE_NODE":
                return nodes
            if cmd in ("GEO_RETURN", "GEO_END"):
                continue
            if cmd == "GEO_OPEN_NODE":
                kids = level()
                if nodes:
                    nodes[-1][2].extend(kids)
                continue
            if cmd == "GEO_BRANCH":
                nodes.extend(geo_tree(layouts, args[1]))
            else:
                nodes.append((cmd, args, []))
        return nodes

    return level()


def walk_parts(nodes, parent, parts, dls, part=None):
    """Animated parts in channel order (switches: case 0), and the display
    lists attached to each part: (part index, dl name, layer)."""
    for cmd, args, kids in nodes:
        if cmd == "GEO_ANIMATED_PART":
            idx = len(parts)
            parts.append((parent, num(args[1]), num(args[2]), num(args[3])))
            if args[4] != "NULL":
                dls.append((idx, args[4], args[0]))
            walk_parts(kids, idx, parts, dls, idx)
        elif cmd == "GEO_SWITCH_CASE":
            if kids:
                walk_parts([kids[0]], parent, parts, dls, part)
        elif cmd == "GEO_DISPLAY_LIST":
            dls.append((part, args[1], args[0]))
            walk_parts(kids, parent, parts, dls, part)
        elif cmd in ("GEO_TRANSLATE_ROTATE", "GEO_TRANSLATE_ROTATE_WITH_DL"):
            continue  # wing cap wings (not shown with the normal cap)
        else:
            walk_parts(kids, parent, parts, dls, part)


def mario_triangles(root, layout="mario_geo_low_poly_body"):
    syms = Symbols(root)
    syms.load("actors/mario/model.inc.c")
    syms.load_tree("actors/mario", r".*\.c$")
    layouts = parse_geo(open(os.path.join(root, "actors/mario/geo.inc.c")).read())
    parts, dls = [], []
    walk_parts(geo_tree(layouts, layout), -1, parts, dls)
    state, tris = State(), []
    for part, dl, layer in dls:
        def emit(t, part=part):
            t["part"] = part
            tris.append(t)
        run_dl(syms, dl, state, emit, layer)
    return parts, tris


def _rot_y(p, yaw):
    """SM64's yaw rotation (mtxf_rotate_zxy with only the Y angle), row vectors."""
    import math
    a = (yaw & 0xFFFF) * math.pi * 2 / 65536.0
    c, s = math.cos(a), math.sin(a)
    return [p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c]


def level_static_objects(root, level, area=1):
    """Static level-geometry objects (bhvStaticObject): from the level script
    (OBJECT with act 1) and from the collision's special objects
    (special_level_geo_XX). Moving platforms and other objects are not
    simulated by the demo (no collision either), so they are left out."""
    script = open(os.path.join(root, "levels/%s/script.c" % level)).read()
    models = dict(re.findall(r"LOAD_MODEL_FROM_GEO\((\w+),\s*(\w+)\)", script))
    out = []
    for m in re.finditer(r"OBJECT(?:_WITH_ACTS)?\(([^;]*?)\),?\s*$", script, re.M):
        a = [re.sub(r"/\*.*?\*/", "", x).strip() for x in split_args(m.group(1))]
        if len(a) < 9 or a[8] != "bhvStaticObject":
            continue
        if len(a) > 9 and "ACT_1" not in a[9] and "ALL_ACTS" not in a[9]:
            continue
        yaw = int(round(num(a[5]) * 65536 / 360.0)) & 0xFFFF
        out.append((models[a[0]], [num(a[1]), num(a[2]), num(a[3])], yaw))
    coll = open(os.path.join(root, "levels/%s/areas/%d/collision.inc.c" % (level, area))).read()
    for m in re.finditer(r"SPECIAL_OBJECT_WITH_YAW\(([^)]*)\)", coll):
        a = [re.sub(r"/\*.*?\*/", "", x).strip() for x in split_args(m.group(1))]
        g = re.match(r"special_level_geo_([0-9A-F]{2})$", a[0])
        if not g:
            continue
        model = "MODEL_LEVEL_GEOMETRY_" + g.group(1)
        yaw = ((num(a[4]) & 0xFF) << 8) & 0xFFFF
        yaw = {0x3F00: 0x4000, 0x7F00: 0x8000, 0xBF00: 0xC000, 0xFF00: 0x0000}.get(yaw, yaw)
        out.append((models[model], [num(a[1]), num(a[2]), num(a[3])], yaw))
    return out


def static_dls(nodes, lod, out=None):
    """Display lists of a static model; GEO_RENDER_RANGE picks one LOD
    (lod='near': the range starting closest, 'far': the one starting farthest)."""
    out = [] if out is None else out
    ranges = [n for n in nodes if n[0] == "GEO_RENDER_RANGE"]
    if ranges:
        key = lambda n: num(n[1][0])
        pick = min(ranges, key=key) if lod == "near" else max(ranges, key=key)
    for node in nodes:
        cmd, args, kids = node
        if cmd == "GEO_RENDER_RANGE" and node is not pick:
            continue
        if cmd == "GEO_DISPLAY_LIST":
            out.append((args[0], args[1]))
        elif cmd not in ("GEO_CULLING_RADIUS", "GEO_RENDER_RANGE"):
            raise ValueError("unsupported node " + cmd)
        static_dls(kids, lod, out)
    return out


def level_triangles(root, level, area=1, lod="near"):
    syms = Symbols(root)
    syms.load_tree("levels/%s" % level, r".*\.c$")
    syms.load_tree("bin", r".*\.c$")
    geo_src = ""
    for dp, _, fs in os.walk(os.path.join(root, "levels/%s" % level)):
        for f in fs:
            if f == "geo.inc.c":
                geo_src += open(os.path.join(dp, f)).read() + "\n"
    layouts = parse_geo(geo_src)
    area_geo = open(os.path.join(root, "levels/%s/areas/%d/geo.inc.c" % (level, area))).read()
    tris = []
    for layer, dl in re.findall(r"GEO_DISPLAY_LIST\((\w+),\s*(\w+)\)", area_geo):
        run_dl(syms, dl, State(), tris.append, layer)
    for geo, pos, yaw in level_static_objects(root, level, area):
        def emit(t, pos=pos, yaw=yaw):
            t["v"] = [dict(v, pos=[a + b for a, b in zip(_rot_y(v["pos"], yaw), pos)]) for v in t["v"]]
            tris.append(t)
        for layer, dl in static_dls(geo_tree(layouts, geo), lod):
            run_dl(syms, dl, State(), emit, layer)
    return tris


if __name__ == "__main__":
    import sys
    root = sys.argv[1] if len(sys.argv) > 1 else "sm64"
    parts, tris = mario_triangles(root)
    print("mario low poly: %d parts, %d triangles" % (len(parts), len(tris)))
    for lv in ("bob", "wf", "ccm", "bitdw"):
      for lod in ("near", "far"):
        t = level_triangles(root, lv, lod=lod)
        verts = {tuple(v["pos"]) for tri in t for v in tri["v"]}
        layers = {}
        for x in t:
            layers[x["layer"]] = layers.get(x["layer"], 0) + 1
        print(lod, "%-6s %5d triangles, %5d distinct positions, layers %s" % (lv, len(t), len(verts), layers))
