/*
 * Equivalence test: optimized collision (src/opt/surface_collision.c) against
 * the unmodified decomp (linked alongside with its symbols renamed ref_*).
 *
 * For every level, millions of queries are run through both and every output
 * is compared bit for bit:
 *   find_floor            height bits, surface pointer
 *   find_ceil             height bits, surface pointer
 *   find_wall_collisions  return value, x/y/z bits after the push, numWalls, walls[]
 *
 * Query positions are built to hit the places where rewrites go wrong:
 * points near random surfaces with fractional offsets, points exactly on
 * vertices and edges (and one ulp off them), heights at the +/-78 floor/ceiling
 * buffers and at wall lowerY/upperY, radii at the wall-distance boundary,
 * random points anywhere in the level, and far-out "parallel universe"
 * positions whose s16 cast wraps back into the level. Both values of
 * gCheckingSurfaceCollisionsForCamera are used.
 *
 * Build: make equiv (host) / make equiv-arm (ARM7 soft-float under qemu).
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "engine/surface_collision.h"
#include "engine/surface_load.h"
#include "game/object_list_processor.h"
#include "levels.h"

int printf(const char *fmt, ...);

#ifndef EQUIV_QUERIES_PER_LEVEL
#define EQUIV_QUERIES_PER_LEVEL 1000000
#endif

struct MarioState gMarioStates[1];

f32 ref_find_floor(f32 x, f32 y, f32 z, struct Surface **pfloor);
f32 ref_find_ceil(f32 x, f32 y, f32 z, struct Surface **pceil);
s32 ref_find_wall_collisions(struct WallCollisionData *data);

/* ---------- random numbers and float bit helpers ---------- */

static u32 sRng = 0x9E3779B9u;

static u32 rnd(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static u32 fbits(f32 f) {
    union { f32 f; u32 u; } x;
    x.f = f;
    return x.u;
}

static f32 bitsf(u32 u) {
    union { f32 f; u32 u; } x;
    x.u = u;
    return x.f;
}

static f32 rnd_unit(void) { /* [0, 1) */
    return (f32) (rnd() >> 8) * (1.0f / 16777216.0f);
}

static f32 rnd_signed(f32 mag) {
    return (rnd_unit() * 2.0f - 1.0f) * mag;
}

/* Nudge a float by -2..+2 ulps, or not at all. */
static f32 jiggle(f32 f) {
    s32 d = (s32) (rnd() % 5) - 2;
    if (f == 0.0f || (rnd() & 3) != 0) {
        return f;
    }
    return bitsf(fbits(f) + d);
}

/* ---------- surfaces of the loaded level ---------- */

#define MAX_SURFS 2300
static struct Surface *sSurfs[MAX_SURFS];
static s32 sNumSurfs;

static void collect_surfaces(void) {
    s32 cx, cz, t, i;
    sNumSurfs = 0;
    for (cz = 0; cz < NUM_CELLS; cz++) {
        for (cx = 0; cx < NUM_CELLS; cx++) {
            for (t = 0; t < 3; t++) {
                struct SurfaceNode *n = gStaticSurfacePartition[cz][cx][t].next;
                for (; n != NULL; n = n->next) {
                    for (i = 0; i < sNumSurfs; i++) {
                        if (sSurfs[i] == n->surface) {
                            break;
                        }
                    }
                    if (i == sNumSurfs && sNumSurfs < MAX_SURFS) {
                        sSurfs[sNumSurfs++] = n->surface;
                    }
                }
            }
        }
    }
}

/* ---------- query generation ---------- */

struct Query {
    f32 x, y, z;
};

static void gen_query(struct Query *q) {
    u32 kind = rnd() % 16;
    struct Surface *s = sSurfs[rnd() % sNumSurfs];
    f32 a = rnd_unit(), b = rnd_unit();

    if (a + b > 1.0f) {
        a = 1.0f - a;
        b = 1.0f - b;
    }

    if (kind < 6) {
        /* inside or near a random triangle */
        f32 spread = (kind < 3) ? 4.0f : 200.0f;
        q->x = s->vertex1[0] + a * (s->vertex2[0] - s->vertex1[0]) + b * (s->vertex3[0] - s->vertex1[0]);
        q->y = s->vertex1[1] + a * (s->vertex2[1] - s->vertex1[1]) + b * (s->vertex3[1] - s->vertex1[1]);
        q->z = s->vertex1[2] + a * (s->vertex2[2] - s->vertex1[2]) + b * (s->vertex3[2] - s->vertex1[2]);
        q->x += rnd_signed(spread);
        q->z += rnd_signed(spread);
        q->y += rnd_signed((rnd() & 1) ? 100.0f : 400.0f);
    } else if (kind < 9) {
        /* exactly on a vertex or on an edge, maybe one ulp off */
        const TerrainData *v = (rnd() & 1) ? s->vertex1 : s->vertex2;
        if (kind == 6) {
            q->x = v[0];
            q->z = v[2];
        } else {
            q->x = s->vertex1[0] + a * (s->vertex2[0] - s->vertex1[0]);
            q->z = s->vertex1[2] + a * (s->vertex2[2] - s->vertex1[2]);
        }
        q->y = v[1] + rnd_signed(160.0f);
        q->x = jiggle(q->x);
        q->z = jiggle(q->z);
    } else if (kind < 11) {
        /* heights on the boundaries: the +/-78 buffers and wall Y bounds */
        static const f32 dy[] = { 78.0f, -78.0f, 0.0f, 5.0f, -5.0f, 77.99999f, -78.00001f };
        q->x = s->vertex1[0] + a * (s->vertex2[0] - s->vertex1[0]) + b * (s->vertex3[0] - s->vertex1[0]);
        q->z = s->vertex1[2] + a * (s->vertex2[2] - s->vertex1[2]) + b * (s->vertex3[2] - s->vertex1[2]);
        q->y = (kind == 9 ? s->lowerY : s->upperY) + dy[rnd() % 7];
        q->y = jiggle(q->y);
    } else if (kind < 14) {
        /* anywhere in the level */
        q->x = rnd_signed(8300.0f);
        q->y = rnd_signed(9000.0f);
        q->z = rnd_signed(8300.0f);
    } else if (kind == 14) {
        /* parallel universes: s16 casts wrap into the level */
        q->x = (f32) (s32) ((rnd() % 40) - 20) * 65536.0f + rnd_signed(8000.0f);
        q->z = (f32) (s32) ((rnd() % 40) - 20) * 65536.0f + rnd_signed(8000.0f);
        q->y = rnd_signed(9000.0f);
    } else {
        /* around the 2^15 fast-path guard and the level edge */
        static const f32 edges[] = { 32767.0f, 32768.0f, -32768.0f, 8191.0f, -8191.0f, 8192.0f };
        q->x = jiggle(edges[rnd() % 6]) + ((rnd() & 1) ? 0.0f : rnd_signed(2.0f));
        q->z = rnd_signed(8000.0f);
        if (rnd() & 1) {
            f32 t = q->x;
            q->x = q->z;
            q->z = t;
        }
        q->y = rnd_signed(4000.0f);
    }
}

/* ---------- comparison ---------- */

static u32 sFailures, sQueries, sFloorHits, sCeilHits, sWallHits;

static void report(const char *what, const char *level, const struct Query *q, u32 a, u32 b) {
    if (sFailures++ < 12) {
        printf("  FAIL %s on %s at (%08x %08x %08x): ref %08x opt %08x\n", what, level,
               fbits(q->x), fbits(q->y), fbits(q->z), a, b);
    }
}

static void check_floor_ceil(const char *level, const struct Query *q) {
    struct Surface *s1, *s2;
    f32 h1 = ref_find_floor(q->x, q->y, q->z, &s1);
    f32 h2 = find_floor(q->x, q->y, q->z, &s2);
    if (fbits(h1) != fbits(h2) || s1 != s2) {
        report("find_floor", level, q, fbits(h1), fbits(h2));
    }
    sFloorHits += s1 != NULL;

    h1 = ref_find_ceil(q->x, q->y, q->z, &s1);
    h2 = find_ceil(q->x, q->y, q->z, &s2);
    if (fbits(h1) != fbits(h2) || s1 != s2) {
        report("find_ceil", level, q, fbits(h1), fbits(h2));
    }
    sCeilHits += s1 != NULL;
}

static void check_walls(const char *level, const struct Query *q) {
    static const f32 radii[] = { 24.0f, 50.0f, 60.0f, 100.0f, 110.0f, 200.0f, 250.0f, 5.0f };
    static const f32 offsets[] = { 0.0f, 10.0f, 30.0f, 60.0f, 150.0f, -30.0f };
    struct WallCollisionData d1, d2;
    s32 r1, r2, i;

    d1.x = q->x;
    d1.y = q->y;
    d1.z = q->z;
    d1.radius = (rnd() & 3) ? radii[rnd() % 8] : rnd_unit() * 260.0f;
    d1.offsetY = (rnd() & 3) ? offsets[rnd() % 6] : rnd_signed(200.0f);
    d1.radius = jiggle(d1.radius);
    d1.numWalls = 0;
    d2 = d1;

    r1 = ref_find_wall_collisions(&d1);
    r2 = find_wall_collisions(&d2);
    if (r1 != r2) {
        report("find_wall_collisions (count)", level, q, (u32) r1, (u32) r2);
    }
    if (fbits(d1.x) != fbits(d2.x) || fbits(d1.y) != fbits(d2.y) || fbits(d1.z) != fbits(d2.z)) {
        report("find_wall_collisions (push)", level, q, fbits(d1.x), fbits(d2.x));
    }
    if (d1.numWalls != d2.numWalls) {
        report("find_wall_collisions (numWalls)", level, q, (u32) d1.numWalls, (u32) d2.numWalls);
    }
    for (i = 0; i < d1.numWalls && i < 4; i++) {
        if (d1.walls[i] != d2.walls[i]) {
            report("find_wall_collisions (walls[])", level, q, (u32) i, 0);
        }
    }
    sWallHits += r1 > 0;
}

/* A query that skims a wall: start on the wall's plane, step out to just
 * around the radius. These are the cases the distance and edge pre-tests
 * have to get right, and random points rarely produce them. */
static void gen_wall_skim(struct Query *q, f32 *radiusOut) {
    struct Surface *s;
    f32 a = rnd_unit(), b = rnd_unit(), off, target, radius;
    s32 tries = 0;
    do { /* prefer walls (|normal.y| < 0.01, as surface_load.c classifies them) */
        s = sSurfs[rnd() % sNumSurfs];
    } while ((s->normal.y > 0.01f || s->normal.y < -0.01f) && ++tries < 50);
    if (a + b > 1.0f) {
        a = 1.0f - a;
        b = 1.0f - b;
    }
    /* points slightly outside the triangle too, to exercise the edges */
    a = a * 1.1f - 0.05f;
    b = b * 1.1f - 0.05f;
    q->x = s->vertex1[0] + a * (s->vertex2[0] - s->vertex1[0]) + b * (s->vertex3[0] - s->vertex1[0]);
    q->y = s->vertex1[1] + a * (s->vertex2[1] - s->vertex1[1]) + b * (s->vertex3[1] - s->vertex1[1]);
    q->z = s->vertex1[2] + a * (s->vertex2[2] - s->vertex1[2]) + b * (s->vertex3[2] - s->vertex1[2]);
    radius = (rnd() & 1) ? 50.0f : (f32) (rnd() % 200 + 1);
    off = s->normal.x * q->x + s->normal.y * q->y + s->normal.z * q->z + s->originOffset;
    target = ((rnd() & 1) ? 1.0f : -1.0f) * (radius + rnd_signed(20.0f));
    q->x += s->normal.x * (target - off);
    q->z += s->normal.z * (target - off);
    q->x = jiggle(q->x);
    q->z = jiggle(q->z);
    *radiusOut = radius;
}

static void check_wall_skim(const char *level) {
    struct Query q;
    struct WallCollisionData d1, d2;
    f32 radius;
    s32 r1, r2, i;

    gen_wall_skim(&q, &radius);
    d1.x = q.x;
    d1.y = q.y;
    d1.z = q.z;
    d1.radius = radius;
    d1.offsetY = 0.0f;
    d1.numWalls = 0;
    d2 = d1;
    r1 = ref_find_wall_collisions(&d1);
    r2 = find_wall_collisions(&d2);
    if (r1 != r2 || fbits(d1.x) != fbits(d2.x) || fbits(d1.z) != fbits(d2.z)
        || d1.numWalls != d2.numWalls) {
        report("find_wall_collisions (skim)", level, &q, (u32) r1, (u32) r2);
    }
    for (i = 0; i < d1.numWalls && i < 4; i++) {
        if (d1.walls[i] != d2.walls[i]) {
            report("find_wall_collisions (skim walls[])", level, &q, (u32) i, 0);
        }
    }
    sWallHits += r1 > 0;
}

int main(void) {
    s32 l;
    u32 i;

    alloc_surface_pools();
    gCurrentObject = NULL;

    for (l = 0; l < gNumHarnessLevels; l++) {
        const char *name = gHarnessLevels[l].name;
        u32 f0 = sFailures, q0 = sQueries, fh = sFloorHits, ch = sCeilHits, wh = sWallHits;

        load_area_terrain(0, (TerrainData *) gHarnessLevels[l].collision, NULL, NULL);
        collect_surfaces();

        for (i = 0; i < EQUIV_QUERIES_PER_LEVEL; i++) {
            struct Query q;
            gCheckingSurfaceCollisionsForCamera = (rnd() & 7) == 0;
            gen_query(&q);
            check_floor_ceil(name, &q);
            check_walls(name, &q);
            check_wall_skim(name);
            sQueries += 3;
        }
        gCheckingSurfaceCollisionsForCamera = 0;
        printf("%-6s %8u queries, %u mismatches  (hits: floor %u, ceil %u, wall %u)\n", name,
               sQueries - q0, sFailures - f0, sFloorHits - fh, sCeilHits - ch, sWallHits - wh);
    }
    printf("%s: %u queries, %u mismatches\n", sFailures ? "FAIL" : "PASS", sQueries, sFailures);
    return sFailures != 0;
}
