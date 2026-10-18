/*
 * Optimized copy of the decomp's src/engine/surface_collision.c.
 *
 * Every change here must be bit-exact with the original: same branches
 * taken, same float operations in the same order where a float result is
 * kept. Changes (search for "OPT:"):
 *
 *  1. Wall height pre-check done in integers. lowerY/upperY are s16, and for
 *     any real y and integer L:  y < L <=> floor(y) < L,  y > U <=> ceil(y) > U.
 *     floor/ceil come straight from y's bit pattern, once per query, instead
 *     of two int->float conversions and two float compares per wall.
 *  2. Comparisons against 0 and against +/-radius read the IEEE bit pattern
 *     with integer instructions instead of calling __aeabi_fcmp*. Identical
 *     for all non-NaN values (NaN never reaches these: inputs are finite).
 *  3. Conservative integer plane-distance filter for walls. Before the float
 *     offset = n.p + originOffset, estimate it in integers (normals in Q14,
 *     position floored). The estimate is within 11 units of the exact value
 *     and the float result within 0.5 of it, so a wall whose estimate is more
 *     than ceil(radius) + 12 away is a wall the float test rejects too: we
 *     skip it without computing. Anything closer takes the original path.
 *  4. Integer pre-test for the wall edge checks (wall_edge_sign, bound
 *     derived next to it). Decides the sign of each edge function in
 *     integers when it is certain; undecided edges run the original float
 *     expression.
 *  6. Per-query overhead: the float -> s16 position casts use an inline copy
 *     of libgcc's __aeabi_f2iz (same truncation, saturation and NaN result),
 *     empty object (dynamic) surface lists are skipped (the list functions
 *     return without side effects when given NULL), and the radius clamp and
 *     dynamic-vs-static height comparisons use the bit-pattern compare
 *     (operands are never NaN: radii are game constants, heights are finite).
 *  7. Static surfaces are walked from a packed cache (collision_cache.c)
 *     instead of the linked lists: same surfaces, same order, plus sub-cell
 *     masks for floors and ceilings. The per-surface tests are unchanged
 *     (floor_accepts / ceil_accepts hold the original loop bodies).
 *  5. Fast paths 3 and 4 are only used when |x|, |z| < 2^15. The level-bounds
 *     check in find_wall_collisions() casts to s16, so far-out ("parallel
 *     universe") positions wrap back into range; their error bounds don't
 *     hold there, so such queries take the original float code throughout.
 *
 * Built as ARM code into IWRAM on the GBA (see Makefile, gba/gba_opt.ld).
 */
#include <PR/ultratypes.h>

#include "sm64.h"
#include "game/debug.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/object_list_processor.h"
#include "surface_collision.h"
#include "surface_load.h"
#include "collision_cache.h"


/* ---------- OPT: exact float helpers on IEEE bit patterns ---------- */

static inline u32 f32_bits(f32 f) {
    union { f32 f; u32 u; } x;
    x.f = f;
    return x.u;
}

/* Signed-magnitude -> two's complement ordering key; -0 and +0 both map to 0.
 * a < b (IEEE, non-NaN) <=> f32_key(a) < f32_key(b). */
static inline s32 f32_key(f32 f) {
    u32 b = f32_bits(f);
    s32 mag = (s32) (b & 0x7FFFFFFF);
    return (b & 0x80000000) ? -mag : mag;
}

/* f > 0.0f and f < 0.0f for non-NaN f. */
#define F32_GT0(f) ((s32) f32_bits(f) > 0)
#define F32_LT0(f) (f32_bits(f) > 0x80000000u)

/* floor() of a float as an integer, from its bits. Magnitudes beyond 2^30
 * clamp to +/-2^30, which is still outside every s16 bound we compare with. */
static inline s32 f32_floor_s32(f32 f) {
    u32 b = f32_bits(f);
    s32 neg = (b & 0x80000000) != 0;
    s32 e = (s32) ((b >> 23) & 0xFF) - 127;
    u32 mant, ip, fracMask;

    if ((b & 0x7FFFFFFF) == 0) {
        return 0;
    }
    if (e < 0) {
        return neg ? -1 : 0; /* 0 < |f| < 1 */
    }
    if (e >= 30) {
        return neg ? -(1 << 30) : (1 << 30);
    }
    mant = (b & 0x7FFFFF) | 0x800000;
    if (e >= 23) {
        ip = mant << (e - 23);
        return neg ? -(s32) ip : (s32) ip;
    }
    ip = mant >> (23 - e);
    fracMask = (1u << (23 - e)) - 1;
    if (neg) {
        return -(s32) ip - ((mant & fracMask) != 0);
    }
    return (s32) ip;
}

/* A unit-normal component as Q14, truncated toward zero (error < 2^-14). */
static inline s32 f32_unit_q14(f32 f) {
    u32 b = f32_bits(f);
    s32 e = (s32) ((b >> 23) & 0xFF) - 127; /* <= 0 for |f| <= 1 */
    s32 shift = 9 - e;
    s32 q;
    if (shift >= 24) {
        return 0;
    }
    q = (s32) (((b & 0x7FFFFF) | 0x800000) >> shift);
    return (b & 0x80000000) ? -q : q;
}

static inline s32 f32_ceil_s32(f32 f) {
    union { f32 f; u32 u; } x;
    x.f = f;
    x.u ^= 0x80000000;
    return -f32_floor_s32(x.f);
}

/* OPT 6. Exactly libgcc's __aeabi_f2iz (ieee754-sf.S, fixsfsi): truncate
 * toward zero; |f| >= 2^31 saturates to INT_MIN/INT_MAX by sign; NaN -> 0.
 * The C cast (TerrainData) f compiles to __aeabi_f2iz then a 16-bit
 * truncation on ARM, so (TerrainData) f32_to_s32_libgcc(f) matches it. */
static inline s32 f32_to_s32_libgcc(f32 f) {
    u32 b = f32_bits(f);
    u32 e = (b >> 23) & 0xFF;
    u32 mag;
    if (e < 127) {
        return 0;
    }
    if (e >= 127 + 31) {
        if (e == 0xFF && (b & 0x7FFFFF) != 0) {
            return 0;
        }
        return (b & 0x80000000) ? (s32) 0x80000000 : 0x7FFFFFFF;
    }
    mag = ((b << 8) | 0x80000000) >> (127 + 31 - e);
    return (b & 0x80000000) ? -(s32) mag : (s32) mag;
}

/* OPT 5. |f| < 2^15, from the exponent alone. */
static inline s32 f32_abs_lt_2p15(f32 f) {
    return ((f32_bits(f) >> 23) & 0xFF) < 127 + 15;
}

static inline s64 s64_abs(s64 v) {
    return v < 0 ? -v : v;
}

/* OPT 4. Sign of one wall edge function, decided in integers when certain.
 *
 * The original evaluates, in single precision,
 *     R = (yi - y) * (wj - wi) - (wi - q) * (yj - yi)
 * where yi, wi, yj, wj are vertex coordinates (integers), y is Mario's
 * height and q his horizontal coordinate in the wall's projection (x, or -z).
 * Write A = yi - y, C = wi - q, D1 = wj - wi, D2 = yj - yi (D1, D2 exact).
 *
 * Float error: with u = 2^-24, fl(A) = A(1+e1) and fl(fl(A)*D1) =
 * A*D1*(1+e1)(1+e2), likewise for C*D2, and the final subtraction keeps the
 * sign (and zero-ness) of its exact result. So the float result has the sign
 * of R + err, |err| <= (|A*D1| + |C*D2|)(2u + u^2) < (|A*D1| + |C*D2|) 2^-22.
 * (Underflow adds at most ~2^-149 absolute per operation, absorbed by +1.)
 *
 * Integer estimate: yF = floor(y), qF = floor(q), A' = yi - yF, C' = wi - qF,
 * so |A - A'| < 1 and |C - C'| < 1, and E = A'*D1 - C'*D2 satisfies
 * |E - R| < |D1| + |D2|. With T = (|A'|+1)|D1| + (|C'|+1)|D2| >= |A D1| + |C D2|:
 *     |E| > |D1| + |D2| + (T >> 22) + 1
 * implies |R| > T 2^-22 > |err|, so the float result is nonzero with E's sign.
 *
 * Magnitudes (|y| <= 32768 after the height check, |q| < 2^15 from OPT 5,
 * vertices s16): A', C' < 2^17, D1, D2 < 2^17, products < 2^34: s64.
 * Returns +1 / -1 when certain, 0 when the float expression must decide. */
static inline s32 wall_edge_sign(s32 yi, s32 wi, s32 yj, s32 wj, s32 yF, s32 qF) {
    s32 a = yi - yF;
    s32 c = wi - qF;
    s32 d1 = wj - wi;
    s32 d2 = yj - yi;
    s64 e = (s64) a * d1 - (s64) c * d2;
    s32 ad1 = d1 < 0 ? -d1 : d1;
    s32 ad2 = d2 < 0 ? -d2 : d2;
    s64 t = (s64) ((a < 0 ? -a : a) + 1) * ad1 + (s64) ((c < 0 ? -c : c) + 1) * ad2;
    s64 thr = (s64) ad1 + ad2 + (t >> 22) + 1;
    if (e > thr) {
        return 1;
    }
    if (e < -thr) {
        return -1;
    }
    return 0;
}

/* Reject test for one edge: the original "if (EXPR > 0.0f) continue;"
 * (dir > 0) or "if (EXPR < 0.0f) continue;" (dir < 0), with the integer
 * pre-test first. floatExpr is only evaluated when the pre-test can't decide
 * (or the fast paths are off). Uses locals intPaths, edgeSign, yFloor, qF. */
#define WALL_FLOAT_REJECTS(dir, floatExpr) ((dir) > 0 ? F32_GT0(floatExpr) : F32_LT0(floatExpr))
#define WALL_EDGE_REJECTS(dir, yi, wi, yj, wj, floatExpr)                                   \
    ((intPaths && (edgeSign = wall_edge_sign(yi, wi, yj, wj, yFloor, qF)) != 0)            \
         ? edgeSign == (dir)                                                                \
         : WALL_FLOAT_REJECTS(dir, floatExpr))

/* Exported for collision_cache.c, which precomputes the plane filter inputs. */
s32 coll_unit_q14(f32 f) {
    return f32_unit_q14(f);
}

s32 coll_floor_s32(f32 f) {
    return f32_floor_s32(f);
}

/* OPT 3. Threshold for the plane filter, from the (clamped) wall radius. */
static inline s32 wall_plane_reject_threshold(f32 radius) {
    return f32_ceil_s32(radius) + 12;
}

/* OPT 3. TRUE only if the float test |offset| <= radius is certain to fail.
 * Error budget, with |x|,|z| < 8193 and |y| <= 32768:
 *   per term n*p: position floor error < |n| <= 1, Q14 normal error
 *   < 32768 * 2^-14 = 2  ->  < 3 per term, < 9 for three; the >> 14 and the
 *   originOffset floor add < 1 each: |est - exact| < 11. The float sum of
 *   four terms of magnitude < 2^17 is within 0.5 of exact. So |est| >
 *   ceil(radius) + 12 implies |float offset| > radius. Checked by
 *   `make props` on every wall of every level. */
static inline s32 wall_plane_surely_beyond(struct Surface *surf, s32 xFloor, s32 yFloor,
                                           s32 zFloor, s32 planeReject) {
    s32 est = (f32_unit_q14(surf->normal.x) * xFloor
               + f32_unit_q14(surf->normal.y) * yFloor
               + f32_unit_q14(surf->normal.z) * zFloor) >> 14;
    est += f32_floor_s32(surf->originOffset);
    return est > planeReject || est < -planeReject;
}

/**************************************************
 *                      WALLS                     *
 **************************************************/

/**
 * Iterate through the list of walls until all walls are checked and
 * have given their wall push.
 */
/* Per-query state of a wall search. x, y, z are the query position at entry
 * (the original never updates its locals when a wall pushes data->x/z:
 * "Wall Overlaps"); the integer fields after radiusKey are filled lazily the
 * first time a wall passes the height test. */
struct WallQuery {
    struct WallCollisionData *data;
    f32 x, y, z, radius;
    s32 yFloor, yCeil, radiusKey, intPaths;
    s32 planeReject, xFloor, zFloor, qFloorX, qFloorZ;
};

static inline void wall_query_init(struct WallQuery *q, struct WallCollisionData *data) {
    q->data = data;
    q->radius = data->radius;
    q->x = data->x;
    q->y = data->y + data->offsetY;
    q->z = data->z;

    // Max collision radius = 200
    if (f32_key(q->radius) > 0x43480000) { // OPT 6: radius > 200.0f
        q->radius = 200.0f;
    }

    // OPT: integer forms of y and radius, computed once per query.
    q->yFloor = f32_floor_s32(q->y);
    q->yCeil = f32_ceil_s32(q->y);
    q->radiusKey = f32_key(q->radius);
    // OPT 5: integer fast paths only where their bounds hold. The rest of the
    // integer setup is done lazily, once a wall passes the height check:
    // most queries never get there.
    q->intPaths = f32_abs_lt_2p15(q->x) && f32_abs_lt_2p15(q->z);
    q->planeReject = -1; // not computed yet
    q->xFloor = q->zFloor = q->qFloorX = q->qFloorZ = 0;
}

/* OPT 7: everything the original loop does for a wall that passed the height
 * test, unchanged except "continue" -> "return 0" and the final
 * "numCols++" -> "return 1". Kept out of line: the hot loops below only do
 * the height test, and most walls stop there. */
static __attribute__((noinline)) s32 wall_check(struct Surface *surf, struct WallQuery *q) {
    struct WallCollisionData *data = q->data;
    register f32 offset;
    register f32 radius = q->radius;
    register f32 x = q->x;
    register f32 y = q->y;
    register f32 z = q->z;
    register f32 px, pz;
    register f32 w1, w2, w3;
    register f32 y1, y2, y3;
    s32 yFloor = q->yFloor, radiusKey = q->radiusKey, intPaths = q->intPaths;
    s32 qF, edgeSign;

    // OPT 3: conservative integer estimate of the plane distance (see header).
    // Reached only when lowerY <= y <= upperY, so |y| <= 32768, and
    // |x|, |z| < 2^15 when intPaths is set (OPT 5): no s32 overflow.
    if (intPaths) {
        if (q->planeReject < 0) {
            q->xFloor = f32_floor_s32(x);
            q->zFloor = f32_floor_s32(z);
            q->qFloorZ = q->xFloor;           // q = px for Z-projected walls
            q->qFloorX = -f32_ceil_s32(z); // q = -pz for X-projected walls: floor(-z) = -ceil(z)
            q->planeReject = wall_plane_reject_threshold(radius);
        }
        if (wall_plane_surely_beyond(surf, q->xFloor, yFloor, q->zFloor, q->planeReject)) {
            return 0;
        }
    }

    offset = surf->normal.x * x + surf->normal.y * y + surf->normal.z * z + surf->originOffset;

    // OPT: was (offset < -radius || offset > radius). -radius only flips
    // the sign bit, so its key is -radiusKey.
    {
        s32 offsetKey = f32_key(offset);
        if (offsetKey < -radiusKey || offsetKey > radiusKey) {
            return 0;
        }
    }

    px = x;
    pz = z;

    //! (Quantum Tunneling) Due to issues with the vertices walls choose and
    //  the fact they are floating point, certain floating point positions
    //  along the seam of two walls may collide with neither wall or both walls.
    if (surf->flags & SURFACE_FLAG_X_PROJECTION) {
        w1 = -surf->vertex1[2];            w2 = -surf->vertex2[2];            w3 = -surf->vertex3[2];
        y1 = surf->vertex1[1];            y2 = surf->vertex2[1];            y3 = surf->vertex3[1];

        // OPT 4: integer pre-test per edge, w = -vertex z, y = vertex y.
        qF = q->qFloorX;
        {
            s32 dir = F32_GT0(surf->normal.x) ? 1 : -1;
            s32 iw1 = -surf->vertex1[2], iw2 = -surf->vertex2[2], iw3 = -surf->vertex3[2];
            s32 iy1 = surf->vertex1[1], iy2 = surf->vertex2[1], iy3 = surf->vertex3[1];
            if (WALL_EDGE_REJECTS(dir, iy1, iw1, iy2, iw2, (y1 - y) * (w2 - w1) - (w1 - -pz) * (y2 - y1))) {
                return 0;
            }
            if (WALL_EDGE_REJECTS(dir, iy2, iw2, iy3, iw3, (y2 - y) * (w3 - w2) - (w2 - -pz) * (y3 - y2))) {
                return 0;
            }
            if (WALL_EDGE_REJECTS(dir, iy3, iw3, iy1, iw1, (y3 - y) * (w1 - w3) - (w3 - -pz) * (y1 - y3))) {
                return 0;
            }
        }
    } else {
        w1 = surf->vertex1[0];            w2 = surf->vertex2[0];            w3 = surf->vertex3[0];
        y1 = surf->vertex1[1];            y2 = surf->vertex2[1];            y3 = surf->vertex3[1];

        // OPT 4: integer pre-test per edge, w = vertex x, y = vertex y.
        qF = q->qFloorZ;
        {
            s32 dir = F32_GT0(surf->normal.z) ? 1 : -1;
            s32 iw1 = surf->vertex1[0], iw2 = surf->vertex2[0], iw3 = surf->vertex3[0];
            s32 iy1 = surf->vertex1[1], iy2 = surf->vertex2[1], iy3 = surf->vertex3[1];
            if (WALL_EDGE_REJECTS(dir, iy1, iw1, iy2, iw2, (y1 - y) * (w2 - w1) - (w1 - px) * (y2 - y1))) {
                return 0;
            }
            if (WALL_EDGE_REJECTS(dir, iy2, iw2, iy3, iw3, (y2 - y) * (w3 - w2) - (w2 - px) * (y3 - y2))) {
                return 0;
            }
            if (WALL_EDGE_REJECTS(dir, iy3, iw3, iy1, iw1, (y3 - y) * (w1 - w3) - (w3 - px) * (y1 - y3))) {
                return 0;
            }
        }
    }

    // Determine if checking for the camera or not.
    if (gCheckingSurfaceCollisionsForCamera) {
        if (surf->flags & SURFACE_FLAG_NO_CAM_COLLISION) {
            return 0;
        }
    } else {
        // Ignore camera only surfaces.
        if (surf->type == SURFACE_CAMERA_BOUNDARY) {
            return 0;
        }

        // If an object can pass through a vanish cap wall, pass through.
        if (surf->type == SURFACE_VANISH_CAP_WALLS) {
            // If an object can pass through a vanish cap wall, pass through.
            if (gCurrentObject != NULL
                && (gCurrentObject->activeFlags & ACTIVE_FLAG_MOVE_THROUGH_GRATE)) {
                return 0;
            }

            // If Mario has a vanish cap, pass through the vanish cap wall.
            if (gCurrentObject != NULL && gCurrentObject == gMarioObject
                && (gMarioState->flags & MARIO_VANISH_CAP)) {
                return 0;
            }
        }
    }

    //! (Wall Overlaps) Because this doesn't update the x and z local variables,
    //  multiple walls can push mario more than is required.
    data->x += surf->normal.x * (radius - offset);
    data->z += surf->normal.z * (radius - offset);

    //! (Unreferenced Walls) Since this only returns the first four walls,
    //  this can lead to wall interaction being missed. Typically unreferenced walls
    //  come from only using one wall, however.
    if (data->numWalls < 4) {
        data->walls[data->numWalls++] = surf;
    }

    return 1;
}

/**
 * Iterate through the list of walls until all walls are checked and
 * have given their wall push.
 */
static s32 find_wall_collisions_from_list(struct SurfaceNode *surfaceNode,
                                          struct WallCollisionData *data) {
    register struct Surface *surf;
    struct WallQuery q;
    s32 numCols = 0;

    wall_query_init(&q, data);

    // Stay in this loop until out of walls.
    while (surfaceNode != NULL) {
        surf = surfaceNode->surface;
        surfaceNode = surfaceNode->next;

        // Exclude a large number of walls immediately to optimize.
        // OPT: was (y < surf->lowerY || y > surf->upperY), see header.
        if (q.yFloor < surf->lowerY || q.yCeil > surf->upperY) {
            continue;
        }
        numCols += wall_check(surf, &q);
    }

    return numCols;
}

/* OPT 7: same walk over a cell's packed cache entries: the height test reads
 * the packed (upperY << 16 | lowerY) copy, so walls that fail it never touch
 * their surface. */
static s32 find_wall_collisions_from_cache(const u32 *cacheY, const u16 *cacheSurf, s32 count,
                                           struct WallCollisionData *data) {
    struct WallQuery q;
    s32 numCols = 0;
    s32 yFloor, yCeil, i;

    wall_query_init(&q, data);
    yFloor = q.yFloor;
    yCeil = q.yCeil;

    if (!q.intPaths) {
        for (i = 0; i < count; i++) {
            u32 w = cacheY[i];
            if (yFloor < (s16) w || yCeil > (s16) (w >> 16)) {
                continue;
            }
            numCols += wall_check(&sSurfacePool[cacheSurf[i]], &q);
        }
        return numCols;
    }

    // OPT 7: with the fast paths on, run the plane filter here on the
    // precomputed integers (same values wall_plane_surely_beyond computes from
    // the surface), so the ~90% of walls it rejects never reach wall_check.
    q.xFloor = f32_floor_s32(q.x);
    q.zFloor = f32_floor_s32(q.z);
    q.qFloorZ = q.xFloor;
    q.qFloorX = -f32_ceil_s32(q.z);
    q.planeReject = wall_plane_reject_threshold(q.radius);
    {
        s32 xF = q.xFloor, zF = q.zFloor, thr = q.planeReject;
        for (i = 0; i < count; i++) {
            u32 w = cacheY[i];
            const struct CollWallPlane *pl;
            s32 est;
            if (yFloor < (s16) w || yCeil > (s16) (w >> 16)) {
                continue;
            }
            pl = &gCollWallPlane[cacheSurf[i]];
            est = ((pl->nq[0] * xF + pl->nq[1] * yFloor + pl->nq[2] * zF) >> 14) + pl->ooFloor;
            if (est > thr || est < -thr) {
                continue;
            }
            numCols += wall_check(&sSurfacePool[cacheSurf[i]], &q);
        }
    }

    return numCols;
}

/**
 * Formats the position and wall search for find_wall_collisions.
 */
s32 f32_find_wall_collision(f32 *xPtr, f32 *yPtr, f32 *zPtr, f32 offsetY, f32 radius) {
    struct WallCollisionData collision;
    s32 numCollisions = 0;

    collision.offsetY = offsetY;
    collision.radius = radius;

    collision.x = *xPtr;
    collision.y = *yPtr;
    collision.z = *zPtr;

    collision.numWalls = 0;

    numCollisions = find_wall_collisions(&collision);

    *xPtr = collision.x;
    *yPtr = collision.y;
    *zPtr = collision.z;

    return numCollisions;
}

/**
 * Find wall collisions and receive their push.
 */
s32 find_wall_collisions(struct WallCollisionData *colData) {
    struct SurfaceNode *node;
    s16 cellX, cellZ;
    s32 numCollisions = 0;
    TerrainData x = (TerrainData) f32_to_s32_libgcc(colData->x); // OPT 6
    TerrainData z = (TerrainData) f32_to_s32_libgcc(colData->z);

    colData->numWalls = 0;

    if (x <= -LEVEL_BOUNDARY_MAX || x >= LEVEL_BOUNDARY_MAX) {
        return numCollisions;
    }
    if (z <= -LEVEL_BOUNDARY_MAX || z >= LEVEL_BOUNDARY_MAX) {
        return numCollisions;
    }

    // World (level) consists of a 16x16 grid. Find where the collision is on
    // the grid (round toward -inf)
    cellX = ((x + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;
    cellZ = ((z + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;

    // Check for surfaces belonging to objects. (OPT 6: skip empty lists.)
    node = gDynamicSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_WALLS].next;
    if (node != NULL) {
        numCollisions += find_wall_collisions_from_list(node, colData);
    }

    // Check for surfaces that are a part of level geometry.
    // OPT 7: packed cache when built, the original list otherwise.
    if (gCollCacheValid) {
        s32 count = gCollCellCount[cellZ][cellX][SPATIAL_PARTITION_WALLS];
        if (count > 0) {
            numCollisions += find_wall_collisions_from_cache(
                &gCollEntries[gCollCellStart[cellZ][cellX][SPATIAL_PARTITION_WALLS]],
                &gCollWallSurf[gCollWallBase[cellZ][cellX]], count, colData);
        }
    } else {
        node = gStaticSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_WALLS].next;
        if (node != NULL) {
            numCollisions += find_wall_collisions_from_list(node, colData);
        }
    }

    // Increment the debug tracker.
    gNumCalls.wall++;

    return numCollisions;
}

/**************************************************
 *                     CEILINGS                   *
 **************************************************/

/**
 * Iterate through the list of ceilings and find the first ceiling over a given point.
 */
/* OPT 7: the per-ceiling test of the original loop (see floor_accepts). */
static inline s32 ceil_accepts(struct Surface *surf, s32 x, s32 y, s32 z, f32 *pheight) {
    register s32 x1, z1, x2, z2, x3, z3;


    x1 = surf->vertex1[0];
    z1 = surf->vertex1[2];
    z2 = surf->vertex2[2];
    x2 = surf->vertex2[0];

    // Checking if point is in bounds of the triangle laterally.
    if ((z1 - z) * (x2 - x1) - (x1 - x) * (z2 - z1) > 0) {
        return FALSE;
    }

    // Slight optimization by checking these later.
    x3 = surf->vertex3[0];
    z3 = surf->vertex3[2];
    if ((z2 - z) * (x3 - x2) - (x2 - x) * (z3 - z2) > 0) {
        return FALSE;
    }
    if ((z3 - z) * (x1 - x3) - (x3 - x) * (z1 - z3) > 0) {
        return FALSE;
    }

    // Determine if checking for the camera or not.
    if (gCheckingSurfaceCollisionsForCamera != 0) {
        if (surf->flags & SURFACE_FLAG_NO_CAM_COLLISION) {
            return FALSE;
        }
    }
    // Ignore camera only surfaces.
    else if (surf->type == SURFACE_CAMERA_BOUNDARY) {
        return FALSE;
    }

    {
        f32 nx = surf->normal.x;
        f32 ny = surf->normal.y;
        f32 nz = surf->normal.z;
        f32 oo = surf->originOffset;
        f32 height;

        // If a wall, ignore it. Likely a remnant, should never occur.
        if (ny == 0.0f) {
            return FALSE;
        }

        // Find the ceil height at the specific point.
        height = -(x * nx + nz * z + oo) / ny;

        // Checks for ceiling interaction with a 78 unit buffer.
        //! (Exposed Ceilings) Because any point above a ceiling counts
        //  as interacting with a ceiling, ceilings far below can cause
        // "invisible walls" that are really just exposed ceilings.
        if (F32_GT0(y - (height - -78.0f))) { // OPT
            return FALSE;
        }

        *pheight = height;
        return TRUE;
    }
}

/**
 * Iterate through the list of ceilings and find the first ceiling over a given point.
 */
static struct Surface *find_ceil_from_list(struct SurfaceNode *surfaceNode, s32 x, s32 y, s32 z, f32 *pheight) {
    struct Surface *surf;

    // Stay in this loop until out of ceilings.
    while (surfaceNode != NULL) {
        surf = surfaceNode->surface;
        surfaceNode = surfaceNode->next;
        if (ceil_accepts(surf, x, y, z, pheight)) {
            return surf;
        }
    }

    //! (Surface Cucking) Since only the first ceil is returned and not the lowest,
    //  lower ceilings can be "cucked" by higher ceilings.
    return NULL;
}

/* OPT 7: cache walk for ceilings (see find_floor_from_cache). */
static struct Surface *find_ceil_from_cache(const u32 *entry, s32 count, u32 bit, s32 x, s32 y, s32 z,
                                            f32 *pheight) {
    for (; count > 0; count--, entry++) {
        u32 w = *entry;
        struct Surface *surf;
        if (!(w & bit)) {
            continue;
        }
        surf = &sSurfacePool[w & 0xFFFF];
        if (ceil_accepts(surf, x, y, z, pheight)) {
            return surf;
        }
    }
    return NULL;
}

/**
 * Find the lowest ceiling above a given position and return the height.
 */
f32 find_ceil(f32 posX, f32 posY, f32 posZ, struct Surface **pceil) {
    s16 cellZ, cellX;

    struct Surface *ceil, *dynamicCeil;
    struct SurfaceNode *surfaceList;

    f32 height = CELL_HEIGHT_LIMIT;
    f32 dynamicHeight = CELL_HEIGHT_LIMIT;

    //! (Parallel Universes) Because position is casted to an s16, reaching higher
    //  float locations can return ceilings despite them not existing there.
    //  (Dynamic ceilings will unload due to the range.)
    TerrainData x = (TerrainData) f32_to_s32_libgcc(posX); // OPT 6
    TerrainData y = (TerrainData) f32_to_s32_libgcc(posY);
    TerrainData z = (TerrainData) f32_to_s32_libgcc(posZ);

    *pceil = NULL;

    if (x <= -LEVEL_BOUNDARY_MAX || x >= LEVEL_BOUNDARY_MAX) {
        return height;
    }
    if (z <= -LEVEL_BOUNDARY_MAX || z >= LEVEL_BOUNDARY_MAX) {
        return height;
    }

    // Each level is split into cells to limit load, find the appropriate cell.
    cellX = ((x + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;
    cellZ = ((z + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;

    // Check for surfaces belonging to objects.
    surfaceList = gDynamicSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_CEILS].next;
    dynamicCeil = surfaceList != NULL ? find_ceil_from_list(surfaceList, x, y, z, &dynamicHeight) : NULL; // OPT 6

    // Check for surfaces that are a part of level geometry.
    // OPT 7: packed cache when built, the original list otherwise.
    if (gCollCacheValid) {
        ceil = find_ceil_from_cache(&gCollEntries[gCollCellStart[cellZ][cellX][SPATIAL_PARTITION_CEILS]],
                                    gCollCellCount[cellZ][cellX][SPATIAL_PARTITION_CEILS],
                                    COLL_SUBCELL_BIT(x, z) << 16, x, y, z, &height);
    } else {
        surfaceList = gStaticSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_CEILS].next;
        ceil = find_ceil_from_list(surfaceList, x, y, z, &height);
    }

    if (f32_key(dynamicHeight) < f32_key(height)) { // OPT 6
        ceil = dynamicCeil;
        height = dynamicHeight;
    }

    *pceil = ceil;

    // Increment the debug tracker.
    gNumCalls.ceil++;

    return height;
}

/**************************************************
 *                     FLOORS                     *
 **************************************************/

/**
 * Find the height of the highest floor below an object.
 */
f32 unused_obj_find_floor_height(struct Object *obj) {
    struct Surface *floor;
    f32 floorHeight = find_floor(obj->oPosX, obj->oPosY, obj->oPosZ, &floor);
    return floorHeight;
}

/**
 * Basically a local variable that passes through floor geo info.
 */
struct FloorGeometry sFloorGeo;

UNUSED static u8 unused8038BE50[0x40];

/**
 * Return the floor height underneath (xPos, yPos, zPos) and populate `floorGeo`
 * with data about the floor's normal vector and origin offset. Also update
 * sFloorGeo.
 */
f32 find_floor_height_and_data(f32 xPos, f32 yPos, f32 zPos, struct FloorGeometry **floorGeo) {
    struct Surface *floor;
    f32 floorHeight = find_floor(xPos, yPos, zPos, &floor);

    *floorGeo = NULL;

    if (floor != NULL) {
        sFloorGeo.normalX = floor->normal.x;
        sFloorGeo.normalY = floor->normal.y;
        sFloorGeo.normalZ = floor->normal.z;
        sFloorGeo.originOffset = floor->originOffset;

        *floorGeo = &sFloorGeo;
    }
    return floorHeight;
}

/**
 * Iterate through the list of floors and find the first floor under a given point.
 */
/* OPT 7: the per-floor test of the original loop, unchanged except that
 * "continue" is "return FALSE" and the accepting "break" is "return TRUE".
 * Shared by the list walk and the cache walk below. */
static inline s32 floor_accepts(struct Surface *surf, s32 x, s32 y, s32 z, f32 *pheight) {
    register s32 x1, z1, x2, z2, x3, z3;
    f32 nx, ny, nz;
    f32 oo;
    f32 height;


    x1 = surf->vertex1[0];
    z1 = surf->vertex1[2];
    x2 = surf->vertex2[0];
    z2 = surf->vertex2[2];

    // Check that the point is within the triangle bounds.
    if ((z1 - z) * (x2 - x1) - (x1 - x) * (z2 - z1) < 0) {
        return FALSE;
    }

    // To slightly save on computation time, set this later.
    x3 = surf->vertex3[0];
    z3 = surf->vertex3[2];

    if ((z2 - z) * (x3 - x2) - (x2 - x) * (z3 - z2) < 0) {
        return FALSE;
    }
    if ((z3 - z) * (x1 - x3) - (x3 - x) * (z1 - z3) < 0) {
        return FALSE;
    }

    // Determine if we are checking for the camera or not.
    if (gCheckingSurfaceCollisionsForCamera != 0) {
        if (surf->flags & SURFACE_FLAG_NO_CAM_COLLISION) {
            return FALSE;
        }
    }
    // If we are not checking for the camera, ignore camera only floors.
    else if (surf->type == SURFACE_CAMERA_BOUNDARY) {
        return FALSE;
    }

    nx = surf->normal.x;
    ny = surf->normal.y;
    nz = surf->normal.z;
    oo = surf->originOffset;

    // If a wall, ignore it. Likely a remnant, should never occur.
    if (ny == 0.0f) {
        return FALSE;
    }

    // Find the height of the floor at a given location.
    height = -(x * nx + nz * z + oo) / ny;
    // Checks for floor interaction with a 78 unit buffer.
    if (F32_LT0(y - (height + -78.0f))) { // OPT
        return FALSE;
    }

    *pheight = height;
    return TRUE;
}

/**
 * Iterate through the list of floors and find the first floor under a given point.
 */
static struct Surface *find_floor_from_list(struct SurfaceNode *surfaceNode, s32 x, s32 y, s32 z, f32 *pheight) {
    struct Surface *surf;

    // Iterate through the list of floors until there are no more floors.
    while (surfaceNode != NULL) {
        surf = surfaceNode->surface;
        surfaceNode = surfaceNode->next;
        if (floor_accepts(surf, x, y, z, pheight)) {
            return surf;
        }
    }

    //! (Surface Cucking) Since only the first floor is returned and not the highest,
    //  higher floors can be "cucked" by lower floors.
    return NULL;
}

/* OPT 7: same walk over the cell's packed cache entries, skipping entries
 * whose sub-cell mask excludes (x, z): see collision_cache.c. */
static struct Surface *find_floor_from_cache(const u32 *entry, s32 count, u32 bit, s32 x, s32 y, s32 z,
                                             f32 *pheight) {
    for (; count > 0; count--, entry++) {
        u32 w = *entry;
        struct Surface *surf;
        if (!(w & bit)) {
            continue;
        }
        surf = &sSurfacePool[w & 0xFFFF];
        if (floor_accepts(surf, x, y, z, pheight)) {
            return surf;
        }
    }
    return NULL;
}

/**
 * Find the height of the highest floor below a point.
 */
f32 find_floor_height(f32 x, f32 y, f32 z) {
    struct Surface *floor;

    f32 floorHeight = find_floor(x, y, z, &floor);

    return floorHeight;
}

/**
 * Find the highest dynamic floor under a given position. Perhaps originally static
 * and dynamic floors were checked separately.
 */
f32 unused_find_dynamic_floor(f32 xPos, f32 yPos, f32 zPos, struct Surface **pfloor) {
    struct SurfaceNode *surfaceList;
    struct Surface *floor;
    f32 floorHeight = FLOOR_LOWER_LIMIT;

    // Would normally cause PUs, but dynamic floors unload at that range.
    TerrainData x = (TerrainData) xPos;
    TerrainData y = (TerrainData) yPos;
    TerrainData z = (TerrainData) zPos;

    // Each level is split into cells to limit load, find the appropriate cell.
    s16 cellX = ((x + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;
    s16 cellZ = ((z + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;

    surfaceList = gDynamicSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_FLOORS].next;
    floor = find_floor_from_list(surfaceList, x, y, z, &floorHeight);

    *pfloor = floor;

    return floorHeight;
}

/**
 * Find the highest floor under a given position and return the height.
 */
f32 find_floor(f32 xPos, f32 yPos, f32 zPos, struct Surface **pfloor) {
    s16 cellZ, cellX;

    struct Surface *floor, *dynamicFloor;
    struct SurfaceNode *surfaceList;

    f32 height = FLOOR_LOWER_LIMIT;
    f32 dynamicHeight = FLOOR_LOWER_LIMIT;

    //! (Parallel Universes) Because position is casted to an s16, reaching higher
    //  float locations can return floors despite them not existing there.
    //  (Dynamic floors will unload due to the range.)
    TerrainData x = (TerrainData) f32_to_s32_libgcc(xPos); // OPT 6
    TerrainData y = (TerrainData) f32_to_s32_libgcc(yPos);
    TerrainData z = (TerrainData) f32_to_s32_libgcc(zPos);

    *pfloor = NULL;

    if (x <= -LEVEL_BOUNDARY_MAX || x >= LEVEL_BOUNDARY_MAX) {
        return height;
    }
    if (z <= -LEVEL_BOUNDARY_MAX || z >= LEVEL_BOUNDARY_MAX) {
        return height;
    }

    // Each level is split into cells to limit load, find the appropriate cell.
    cellX = ((x + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;
    cellZ = ((z + LEVEL_BOUNDARY_MAX) / CELL_SIZE) & NUM_CELLS_INDEX;

    // Check for surfaces belonging to objects.
    surfaceList = gDynamicSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_FLOORS].next;
    dynamicFloor = surfaceList != NULL ? find_floor_from_list(surfaceList, x, y, z, &dynamicHeight) : NULL; // OPT 6

    // Check for surfaces that are a part of level geometry.
    // OPT 7: packed cache when built, the original list otherwise.
    surfaceList = gStaticSurfacePartition[cellZ][cellX][SPATIAL_PARTITION_FLOORS].next;
    if (gCollCacheValid) {
        floor = find_floor_from_cache(&gCollEntries[gCollCellStart[cellZ][cellX][SPATIAL_PARTITION_FLOORS]],
                                      gCollCellCount[cellZ][cellX][SPATIAL_PARTITION_FLOORS],
                                      COLL_SUBCELL_BIT(x, z) << 16, x, y, z, &height);
    } else {
        floor = find_floor_from_list(surfaceList, x, y, z, &height);
    }

    // To prevent the Merry-Go-Round room from loading when Mario passes above the hole that leads
    // there, SURFACE_INTANGIBLE is used. This prevent the wrong room from loading, but can also allow
    // Mario to pass through.
    if (!gFindFloorIncludeSurfaceIntangible) {
        //! (BBH Crash) Most NULL checking is done by checking the height of the floor returned
        //  instead of checking directly for a NULL floor. If this check returns a NULL floor
        //  (happens when there is no floor under the SURFACE_INTANGIBLE floor) but returns the height
        //  of the SURFACE_INTANGIBLE floor instead of the typical -11000 returned for a NULL floor.
        if (floor != NULL && floor->type == SURFACE_INTANGIBLE) {
            if (gCollCacheValid) { // OPT 7
                floor = find_floor_from_cache(
                    &gCollEntries[gCollCellStart[cellZ][cellX][SPATIAL_PARTITION_FLOORS]],
                    gCollCellCount[cellZ][cellX][SPATIAL_PARTITION_FLOORS], COLL_SUBCELL_BIT(x, z) << 16,
                    x, (s32)(height - 200.0f), z, &height);
            } else {
                floor = find_floor_from_list(surfaceList, x, (s32)(height - 200.0f), z, &height);
            }
        }
    } else {
        // To prevent accidentally leaving the floor tangible, stop checking for it.
        gFindFloorIncludeSurfaceIntangible = FALSE;
    }

    // If a floor was missed, increment the debug counter.
    if (floor == NULL) {
        gNumFindFloorMisses++;
    }

    if (f32_key(dynamicHeight) > f32_key(height)) { // OPT 6
        floor = dynamicFloor;
        height = dynamicHeight;
    }

    *pfloor = floor;

    // Increment the debug tracker.
    gNumCalls.floor++;

    return height;
}

/**************************************************
 *               ENVIRONMENTAL BOXES              *
 **************************************************/

/**
 * Finds the height of water at a given location.
 */
f32 find_water_level(f32 x, f32 z) {
    s32 i;
    s32 numRegions;
    TerrainData val;
    f32 loX, hiX, loZ, hiZ;
    f32 waterLevel = FLOOR_LOWER_LIMIT;
    TerrainData *p = gEnvironmentRegions;

    if (p != NULL) {
        numRegions = *p++;

        for (i = 0; i < numRegions; i++) {
            val = *p++;
            loX = *p++;
            loZ = *p++;
            hiX = *p++;
            hiZ = *p++;

            // If the location is within a water box and it is a water box.
            // Water is less than 50 val only, while above is gas and such.
            if (loX < x && x < hiX && loZ < z && z < hiZ && val < 50) {
                // Set the water height. Since this breaks, only return the first height.
                waterLevel = *p;
                break;
            }
            p++;
        }
    }

    return waterLevel;
}

/**
 * Finds the height of the poison gas (used only in HMC) at a given location.
 */
f32 find_poison_gas_level(f32 x, f32 z) {
    s32 i;
    s32 numRegions;
    UNUSED u8 filler[4];
    TerrainData val;
    f32 loX, hiX, loZ, hiZ;
    f32 gasLevel = FLOOR_LOWER_LIMIT;
    TerrainData *p = gEnvironmentRegions;

    if (p != NULL) {
        numRegions = *p++;

        for (i = 0; i < numRegions; i++) {
            val = *p;

            if (val >= 50) {
                loX = p[1];
                loZ = p[2];
                hiX = p[3];
                hiZ = p[4];

                // If the location is within a gas's box and it is a gas box.
                // Gas has a value of 50, 60, etc.
                if (loX < x && x < hiX && loZ < z && z < hiZ && val % 10 == 0) {
                    // Set the gas height. Since this breaks, only return the first height.
                    gasLevel = p[5];
                    break;
                }
            }

            p += 6;
        }
    }

    return gasLevel;
}

/**************************************************
 *                      DEBUG                     *
 **************************************************/

/**
 * Finds the length of a surface list for debug purposes.
 */
static s32 surface_list_length(struct SurfaceNode *list) {
    s32 count = 0;

    while (list != NULL) {
        list = list->next;
        count++;
    }

    return count;
}

/**
 * Print the area,number of walls, how many times they were called,
 * and some allocation information.
 */
void debug_surface_list_info(f32 xPos, f32 zPos) {
    struct SurfaceNode *list;
    s32 numFloors = 0;
    s32 numWalls = 0;
    s32 numCeils = 0;

    s32 cellX = (xPos + LEVEL_BOUNDARY_MAX) / CELL_SIZE;
    s32 cellZ = (zPos + LEVEL_BOUNDARY_MAX) / CELL_SIZE;

    list = gStaticSurfacePartition[cellZ & NUM_CELLS_INDEX][cellX & NUM_CELLS_INDEX][SPATIAL_PARTITION_FLOORS].next;
    numFloors += surface_list_length(list);

    list = gDynamicSurfacePartition[cellZ & NUM_CELLS_INDEX][cellX & NUM_CELLS_INDEX][SPATIAL_PARTITION_FLOORS].next;
    numFloors += surface_list_length(list);

    list = gStaticSurfacePartition[cellZ & NUM_CELLS_INDEX][cellX & NUM_CELLS_INDEX][SPATIAL_PARTITION_WALLS].next;
    numWalls += surface_list_length(list);

    list = gDynamicSurfacePartition[cellZ & NUM_CELLS_INDEX][cellX & NUM_CELLS_INDEX][SPATIAL_PARTITION_WALLS].next;
    numWalls += surface_list_length(list);

    list = gStaticSurfacePartition[cellZ & NUM_CELLS_INDEX][cellX & NUM_CELLS_INDEX][SPATIAL_PARTITION_CEILS].next;
    numCeils += surface_list_length(list);

    list = gDynamicSurfacePartition[cellZ & NUM_CELLS_INDEX][cellX & NUM_CELLS_INDEX][SPATIAL_PARTITION_CEILS].next;
    numCeils += surface_list_length(list);

    print_debug_top_down_mapinfo("area   %x", cellZ * NUM_CELLS + cellX);

    // Names represent ground, walls, and roofs as found in SMS.
    print_debug_top_down_mapinfo("dg %d", numFloors);
    print_debug_top_down_mapinfo("dw %d", numWalls);
    print_debug_top_down_mapinfo("dr %d", numCeils);

    set_text_array_x_y(80, -3);

    print_debug_top_down_mapinfo("%d", gNumCalls.floor);
    print_debug_top_down_mapinfo("%d", gNumCalls.wall);
    print_debug_top_down_mapinfo("%d", gNumCalls.ceil);

    set_text_array_x_y(-80, 0);

    // listal- List Allocated?, statbg- Static Background?, movebg- Moving Background?
    print_debug_top_down_mapinfo("listal %d", gSurfaceNodesAllocated);
    print_debug_top_down_mapinfo("statbg %d", gNumStaticSurfaces);
    print_debug_top_down_mapinfo("movebg %d", gSurfacesAllocated - gNumStaticSurfaces);

    gNumCalls.floor = 0;
    gNumCalls.ceil = 0;
    gNumCalls.wall = 0;
}

/**
 * An unused function that finds and interacts with any type of surface.
 * Perhaps an original implementation of surfaces before they were more specialized.
 */
s32 unused_resolve_floor_or_ceil_collisions(s32 checkCeil, f32 *px, f32 *py, f32 *pz, f32 radius,
                                            struct Surface **psurface, f32 *surfaceHeight) {
    f32 nx, ny, nz, oo;
    f32 x = *px;
    f32 y = *py;
    f32 z = *pz;
    f32 offset, distance;

    *psurface = NULL;

    if (checkCeil) {
        *surfaceHeight = find_ceil(x, y, z, psurface);
    } else {
        *surfaceHeight = find_floor(x, y, z, psurface);
    }

    if (*psurface == NULL) {
        return -1;
    }

    nx = (*psurface)->normal.x;
    ny = (*psurface)->normal.y;
    nz = (*psurface)->normal.z;
    oo = (*psurface)->originOffset;

    offset = nx * x + ny * y + nz * z + oo;
    distance = offset >= 0 ? offset : -offset;

    // Interesting surface interaction that should be surf type independent.
    if (distance < radius) {
        *px += nx * (radius - offset);
        *py += ny * (radius - offset);
        *pz += nz * (radius - offset);

        return 1;
    }

    return 0;
}
