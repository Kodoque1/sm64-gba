/*
 * Data produced by tools/import_rom.py from the user's own ROM
 * (build/import/import_data.c). Only compiled in with DEMO_IMPORTED.
 */
#ifndef DEMO_IMPORT_H
#define DEMO_IMPORT_H

#include "gba.h"

/* Mario's low-poly model: vertices in their animated part's frame (model
 * units), triangles with a model-space normal (Q14) and a material
 * (palette 192 + mat * 4 + light level 0..3). */
struct ImpMVert {
    s16 x, y, z;
    u8 part;
};

struct ImpMTri {
    u16 v[3];
    s16 n[3];
    u8 mat, part;
};

/* Level triangles: world-space vertices, color = palette base (fog 0..3
 * added per frame), flags: 1 = decal, 2 = double sided. */
struct ImpTri {
    u16 v[3];
    u8 color, flags;
};

struct ImpLevel {
    const s16 (*verts)[3];
    const struct ImpTri *tris;
    u16 numVerts, numTris;
    u8 groundColor;
    u16 pal[192];
    u16 sky[16];
};

#define IMP_FLAG_DECAL 1
#define IMP_FLAG_DOUBLE 2
#define IMP_MARIO_PAL 192

extern const u16 gImpMarioPal[32];
extern const struct ImpMVert gImpMarioVerts[];
extern const struct ImpMTri gImpMarioTris[];
extern const u16 gImpMarioNumVerts, gImpMarioNumTris;
extern const struct ImpLevel gImpLevels[4];

#endif
