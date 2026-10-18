/*
 * Packed copy of the static collision lists, built once after each level
 * load (load_area_terrain is wrapped at link time: -Wl,--wrap=load_area_terrain).
 *
 * The decomp walks a linked list per grid cell and surface type; on the GBA
 * that is mostly EWRAM pointer chasing (node->surface, node->next, then the
 * surface's fields). The cache keeps every node of every static list, in the
 * same order, as one packed 32-bit word, plus a per-cell start/count:
 *
 *   floors, ceilings:  (subcellMask << 16) | surfaceIndex
 *       subcellMask has bit (sz * 4 + sx) set if the surface's x/z bounding
 *       box overlaps 256-unit sub-cell (sx, sz) of the 1024-unit cell. A
 *       floor or ceiling can only be returned for an integer point (x, z)
 *       inside its projected triangle (the three integer edge tests), and a
 *       floor/ceiling triangle is never degenerate in x/z (|normal.y| > 0.01
 *       requires a nonzero cross product), so the point is inside the
 *       bounding box: entries whose mask bit is clear can't match.
 *   walls:  (u16 upperY << 16) | (u16 lowerY), with the surface index in a
 *       parallel u16 array. Same height test as before, one load per wall.
 *       Per wall surface, the integer inputs of the plane-distance filter
 *       (Q14 normals, floor(originOffset)), computed by the same functions
 *       the query uses, so the filter decides exactly as before.
 *       (No sub-cell pruning for walls: float edge tests near the tip of a
 *       sliver triangle can accept points well outside its bounding box.)
 *
 * If a level has more static nodes than the cache holds, it isn't built and
 * the collision code walks the original lists.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "engine/surface_load.h"
#include "game/object_list_processor.h"
#include "collision_cache.h"

u8 gCollCacheValid;
u16 gCollCellStart[NUM_CELLS][NUM_CELLS][3];
u16 gCollCellCount[NUM_CELLS][NUM_CELLS][3];
u32 gCollEntries[COLL_CACHE_MAX_NODES];
u16 gCollWallSurf[COLL_CACHE_MAX_WALLS];
u16 gCollWallBase[NUM_CELLS][NUM_CELLS]; /* index into gCollWallSurf of the cell's first wall */
struct CollWallPlane gCollWallPlane[COLL_CACHE_MAX_SURFS];

static s32 min3(s32 a, s32 b, s32 c) {
    s32 m = a < b ? a : b;
    return m < c ? m : c;
}

static s32 max3(s32 a, s32 b, s32 c) {
    s32 m = a > b ? a : b;
    return m > c ? m : c;
}

/* Bits of the 4x4 sub-cells of cell (cx, cz) that [x0,x1] x [z0,z1] touches. */
static u16 subcell_mask(s32 cx, s32 cz, s32 x0, s32 x1, s32 z0, s32 z1) {
    s32 baseX = cx * CELL_SIZE - LEVEL_BOUNDARY_MAX;
    s32 baseZ = cz * CELL_SIZE - LEVEL_BOUNDARY_MAX;
    u16 mask = 0;
    s32 sx, sz;
    for (sz = 0; sz < 4; sz++) {
        s32 lo = baseZ + sz * COLL_SUBCELL, hi = lo + COLL_SUBCELL - 1;
        if (z1 < lo || z0 > hi) {
            continue;
        }
        for (sx = 0; sx < 4; sx++) {
            s32 lx = baseX + sx * COLL_SUBCELL, hx = lx + COLL_SUBCELL - 1;
            if (x1 < lx || x0 > hx) {
                continue;
            }
            mask |= 1 << (sz * 4 + sx);
        }
    }
    return mask;
}

void collision_cache_build(void) {
    s32 cx, cz, t;
    u32 n = 0, nw = 0;

    gCollCacheValid = FALSE;
    for (cz = 0; cz < NUM_CELLS; cz++) {
        for (cx = 0; cx < NUM_CELLS; cx++) {
            for (t = 0; t < 3; t++) {
                struct SurfaceNode *node = gStaticSurfacePartition[cz][cx][t].next;
                gCollCellStart[cz][cx][t] = (u16) n;
                if (t == SPATIAL_PARTITION_WALLS) {
                    gCollWallBase[cz][cx] = (u16) nw;
                }
                for (; node != NULL; node = node->next) {
                    struct Surface *s = node->surface;
                    u32 idx = (u32) (s - sSurfacePool);
                    if (n >= COLL_CACHE_MAX_NODES || idx >= COLL_CACHE_MAX_SURFS) {
                        return; /* too big: stay invalid, use the lists */
                    }
                    if (t == SPATIAL_PARTITION_WALLS) {
                        if (nw >= COLL_CACHE_MAX_WALLS) {
                            return;
                        }
                        gCollEntries[n] = ((u32) (u16) s->upperY << 16) | (u16) s->lowerY;
                        gCollWallSurf[nw++] = (u16) idx;
                        gCollWallPlane[idx].nq[0] = (s16) coll_unit_q14(s->normal.x);
                        gCollWallPlane[idx].nq[1] = (s16) coll_unit_q14(s->normal.y);
                        gCollWallPlane[idx].nq[2] = (s16) coll_unit_q14(s->normal.z);
                        gCollWallPlane[idx].ooFloor = coll_floor_s32(s->originOffset);
                    } else {
                        u16 mask = subcell_mask(
                            cx, cz, min3(s->vertex1[0], s->vertex2[0], s->vertex3[0]),
                            max3(s->vertex1[0], s->vertex2[0], s->vertex3[0]),
                            min3(s->vertex1[2], s->vertex2[2], s->vertex3[2]),
                            max3(s->vertex1[2], s->vertex2[2], s->vertex3[2]));
                        gCollEntries[n] = ((u32) mask << 16) | idx;
                    }
                    n++;
                }
                gCollCellCount[cz][cx][t] = (u16) (n - gCollCellStart[cz][cx][t]);
            }
        }
    }
    gCollCacheValid = TRUE;
}

/* Link-time hook: every static-surface load rebuilds the cache. */
void __real_load_area_terrain(s16 index, TerrainData *data, RoomData *surfaceRooms,
                              s16 *macroObjects);

void __wrap_load_area_terrain(s16 index, TerrainData *data, RoomData *surfaceRooms,
                              s16 *macroObjects) {
    gCollCacheValid = FALSE;
    __real_load_area_terrain(index, data, surfaceRooms, macroObjects);
    collision_cache_build();
}
