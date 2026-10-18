/*
 * Software renderer for the demo: the level's collision mesh, flat shaded,
 * drawn into a mode 4 page and scaled to full screen by BG2's affine unit.
 */
#ifndef DEMO_RENDER_H
#define DEMO_RENDER_H

#include "gba.h"

#define R_MAX_VERTS 960
#define R_MAX_TRIS 1280
#define R_MAX_CHUNKS 256    /* 16x16 chunks of 1024 units, grouped 2x2 under 64 parents */
#define R_MAX_SUPERS 64
#define R_MAX_POLYS 1200    /* polygons queued per frame */
#define R_MAX_CLIPVERTS 1600
#define R_NUM_BUCKETS 256
#define R_MAX_PITS 8
#define R_MAX_W 160
#define R_MAX_H 108

/* Palette layout (mode 4, 256 colors):
 *   0..223   7 materials x 8 light levels x 4 fog levels
 *   224..239 sky gradient (top to bottom)
 *   240..255 player and shadow */
#define PAL_MATERIALS 7
#define PAL_SKY 224
#define PAL_PLAYER 240
#define PAL_SHADOW 255

struct RVert {
    s16 x, y, z;
};

struct RTri {
    u16 v[3];
    u8 color; /* palette base: material * 32 + light * 4 (fog added per frame) */
    u8 flags; /* R_TRI_DECAL, R_TRI_DOUBLE (imported meshes) */
    s16 n[3]; /* plane normal, Q14 */
    s16 sc[3], sr; /* bounding sphere: tested before any vertex is transformed */
    s32 d;    /* plane offset: n . p + d = 0, in world units * 2^14 */
};

#define R_TRI_DECAL 1   /* drawn as if nearer: decals lie on other triangles */
#define R_TRI_DOUBLE 2  /* no backface culling */
#define R_TRI_PIT_SHIFT 4 /* flags bits 4-7: index + 1 of the pit the triangle is in */

struct RChunk {
    s16 cx, cy, cz; /* bounding sphere; radius < 0: empty */
    s16 radius;
    u16 first, count;
};

/* A pit (render_build.c): a floor closed in by walls that rise from its
 * edges to a rim of floor. Its opening is a convex polygon at the rim. */
struct RPit {
    s16 x[4], z[4]; /* opening corners, in order around it */
    s16 rimY;
    s16 cx, cy, cz, r; /* bounding sphere */
    u8 n;           /* 3 or 4 corners */
};

struct RMesh {
    struct RVert verts[R_MAX_VERTS];
    struct RTri tris[R_MAX_TRIS];
    struct RChunk chunks[R_MAX_CHUNKS];  /* index = parent * 4 + child */
    struct RChunk supers[R_MAX_SUPERS];  /* parents: spheres around 4 chunks */
    struct RPit pits[R_MAX_PITS];
    u16 numVerts, numTris, numChunks, numPits;
};

/* A level's look: material colors and sky. */
struct RLevelStyle {
    u8 mat[PAL_MATERIALS][3];
    u8 skyTop[3], skyHorizon[3];
};

/* Per-frame camera, in world units and binary angles (0x10000 = 360 deg). */
struct RCamera {
    s32 x, y, z;
    u16 yaw, pitch;
};

/* A posed skeleton joint: rotation rows Q14 (row vectors), origin in world
 * units * 16 (demo/mannequin.c). */
struct RFrame {
    s32 r[3][3];
    s32 t[3];
};

/* Player stand-in: position, facing, and body shape for this frame. */
struct RPlayer {
    s32 x, y, z;
    u16 yaw;
    s32 height;   /* body height in world units */
    s32 lean;     /* forward lean, world units at the top */
    s32 shadowY;  /* floor height under the player */
    u8 visible;
    u8 drawBody;  /* 0: shadow only (the body is drawn by the mannequin sprites) */
    const struct RFrame *frames; /* imported model: Mario's posed parts, or NULL */
};

/* Quality settings chosen by the governor. */
struct RQuality {
    s32 width, height; /* render resolution, scaled to 240x160 by BG2 */
    s32 farDist;       /* draw distance (fog reaches full at farDist) */
    u32 groundColor;   /* palette index for the background below the horizon */
};

struct RStats {
    u32 chunksDrawn, trisConsidered, trisDrawn, clipped;
    u32 cyclesClear, cyclesSetup, cyclesDraw;
    u32 vertsTransformed;
    u32 marioTris, occluders, cyclesMario;
};

extern struct RMesh gRMesh;
/* Shared scratch: the per-frame clip buffer, reused as the vertex hash while
 * a level loads (never needed at the same time). */
extern s16 gRScratch[R_MAX_CLIPVERTS * 2];
extern s32 gRWallsTrimmed, gRWallsDropped;
extern struct RStats gRStats;

void render_build_level(const struct RLevelStyle *style, s32 terrainType);
void render_set_palette(const struct RLevelStyle *style);
#ifdef DEMO_IMPORTED
struct ImpLevel;
void render_build_imported(const struct ImpLevel *lv);
void render_set_imported_palette(const struct ImpLevel *lv);
#endif
void render_frame(u16 *page, const struct RCamera *cam, const struct RPlayer *player,
                  const struct RQuality *q);

#endif
