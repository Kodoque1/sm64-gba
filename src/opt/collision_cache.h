#ifndef COLLISION_CACHE_H
#define COLLISION_CACHE_H

#include <PR/ultratypes.h>
#include "engine/surface_load.h"

/* See collision_cache.c. Sized for the four harness levels (max 5,422 static
 * nodes, 1,867 walls); bigger levels fall back to the linked lists. */
#define COLL_CACHE_MAX_NODES 5632
#define COLL_CACHE_MAX_WALLS 2048
#define COLL_SUBCELL 256
#define COLL_CACHE_MAX_SURFS 1088

/* Inputs of the wall plane filter (wall_plane_surely_beyond), precomputed
 * per surface with the same functions the query uses. */
struct CollWallPlane {
    s16 nq[3]; /* normal components, Q14, truncated toward zero */
    s16 pad;
    s32 ooFloor; /* floor(originOffset) */
};

extern u8 gCollCacheValid;
extern u16 gCollCellStart[NUM_CELLS][NUM_CELLS][3];
extern u16 gCollCellCount[NUM_CELLS][NUM_CELLS][3];
extern u32 gCollEntries[COLL_CACHE_MAX_NODES];
extern u16 gCollWallSurf[COLL_CACHE_MAX_WALLS];
extern u16 gCollWallBase[NUM_CELLS][NUM_CELLS];
extern struct CollWallPlane gCollWallPlane[COLL_CACHE_MAX_SURFS];

/* Set by the collision code: f32 -> Q14 and floor, exactly as used at query time. */
s32 coll_unit_q14(f32 f);
s32 coll_floor_s32(f32 f);

/* Sub-cell bit for an integer position inside its cell. */
#define COLL_SUBCELL_BIT(x, z)                                                       \
    (1u << (((((z) + LEVEL_BOUNDARY_MAX) & (CELL_SIZE - 1)) / COLL_SUBCELL) * 4      \
            + ((((x) + LEVEL_BOUNDARY_MAX) & (CELL_SIZE - 1)) / COLL_SUBCELL)))

void collision_cache_build(void);

#endif
