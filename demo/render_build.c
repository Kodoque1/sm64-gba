/*
 * Level mesh for the renderer, built once per level load from the static
 * collision surfaces the decomp just loaded (sSurfacePool): what you see is
 * exactly the geometry Mario collides with.
 *
 * Runs from ROM; nothing here is time critical.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "surface_terrains.h"
#include "engine/surface_load.h"
#include "game/object_list_processor.h"
#include "engine/surface_collision.h"
#include "render.h"

struct RMesh gRMesh;

/* Material slots (see RLevelStyle): */
enum { M_GROUND, M_SLOPE, M_WALL, M_SLICK, M_STONE, M_HOT, M_CEIL };

static s32 isqrt(u32 v) {
    u32 r = 0, b = 1u << 30;
    while (b > v) {
        b >>= 2;
    }
    while (b != 0) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (s32) r;
}

/* Surfaces that are invisible in the original game. */
static s32 surface_hidden(s16 type) {
    switch (type) {
        case SURFACE_DEATH_PLANE:
        case SURFACE_CAMERA_BOUNDARY:
        case SURFACE_INTANGIBLE:
        case SURFACE_VERTICAL_WIND:
            return TRUE;
    }
    return FALSE;
}

static s32 surface_material(const struct Surface *s, s32 terrainType) {
    f32 ny = s->normal.y;
    (void) terrainType;
    switch (s->type) {
        case SURFACE_SLIPPERY:
        case SURFACE_VERY_SLIPPERY:
        case SURFACE_NOISE_SLIPPERY:
        case SURFACE_NOISE_VERY_SLIPPERY:
        case SURFACE_ICE:
            return ny > 0.01f ? M_SLICK : M_WALL;
        case SURFACE_HARD:
        case SURFACE_HARD_NOT_SLIPPERY:
            return ny > 0.01f ? M_STONE : M_WALL;
        case SURFACE_BURNING:
        case SURFACE_SHALLOW_QUICKSAND:
        case SURFACE_INSTANT_QUICKSAND:
            return M_HOT;
    }
    if (ny < -0.01f) {
        return M_CEIL;
    }
    if (ny <= 0.01f) {
        return M_WALL;
    }
    if (ny >= 0.86f) {
        return M_GROUND;
    }
    return ny >= 0.5f ? M_SLOPE : M_WALL;
}

/* 0..7: ambient + diffuse against a fixed sun, high and to one side. */
static s32 surface_light(const struct Surface *s) {
    /* sun direction (0.40, 0.82, 0.41), normalized */
    f32 d = s->normal.x * 0.40f + s->normal.y * 0.82f + s->normal.z * 0.41f;
    s32 l;
    if (d < 0.0f) {
        d = 0.0f;
    }
    l = (s32) (3.0f + d * 4.9f); /* ambient keeps shaded walls readable (>= 54%) */
    return l > 7 ? 7 : l;
}

/* Vertex dedupe hash. */
#define HASH_SIZE 2048 /* u16 entries in gRScratch (4 KB of its 6.4 KB) */
#define sHash ((u16 *) gRScratch)

static u16 add_vertex(s16 x, s16 y, s16 z) {
    u32 h = ((u32) (x * 73856093) ^ (u32) (y * 19349663) ^ (u32) (z * 83492791)) & (HASH_SIZE - 1);
    for (;;) {
        u16 i = sHash[h];
        if (i == 0xFFFF) {
            break;
        }
        if (gRMesh.verts[i].x == x && gRMesh.verts[i].y == y && gRMesh.verts[i].z == z) {
            return i;
        }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    if (gRMesh.numVerts >= R_MAX_VERTS) {
        return 0xFFFF;
    }
    gRMesh.verts[gRMesh.numVerts].x = x;
    gRMesh.verts[gRMesh.numVerts].y = y;
    gRMesh.verts[gRMesh.numVerts].z = z;
    sHash[h] = gRMesh.numVerts;
    return gRMesh.numVerts++;
}

/* Chunks: 16 x 16 cells of 1024 units over [-8192, 8192), stored parent-major
 * (parent = 2x2 block of cells) so each parent's children are contiguous. */
static s32 chunk_of(s32 x, s32 z) {
    s32 cx = (x + 8192) >> 10, cz = (z + 8192) >> 10;
    cx = cx < 0 ? 0 : cx > 15 ? 15 : cx;
    cz = cz < 0 ? 0 : cz > 15 ? 15 : cz;
    return ((cz >> 1) * 8 + (cx >> 1)) * 4 + (cz & 1) * 2 + (cx & 1);
}

static void sphere_from_box(struct RChunk *c, const s32 *mn, const s32 *mx) {
    u32 dx = (u32) (mx[0] - mn[0]) / 2 + 1, dy = (u32) (mx[1] - mn[1]) / 2 + 1, dz = (u32) (mx[2] - mn[2]) / 2 + 1;
    s32 r = isqrt(dx * dx + dy * dy + dz * dz) + 1;
    c->cx = (s16) ((mn[0] + mx[0]) / 2);
    c->cy = (s16) ((mn[1] + mx[1]) / 2);
    c->cz = (s16) ((mn[2] + mx[2]) / 2);
    c->radius = (s16) (r > 32767 ? 32767 : r);
}

/* Collision walls usually continue below the ground (invisible in the game),
 * and with painter's sorting a buried part can be drawn over the floor. Trim
 * it: for each vertex, the ground height just in front of the wall comes from
 * the game's own find_floor; the triangle is cut where it goes more than
 * TRIM_KEEP units below that ground. Rendering only; physics is untouched.
 * Returns the trimmed polygon's vertex count: 0 (fully buried), 3 or 4. */
#define TRIM_KEEP 16
static s32 sWallsTrimmed, sWallsDropped;
s32 gRWallsTrimmed, gRWallsDropped;

static s32 surface_polygon(const struct Surface *s, s32 out[4][3]) {
    const s16 *v[3] = { s->vertex1, s->vertex2, s->vertex3 };
    s32 d[3], k, n = 0, maxY, buried = 0;

    for (k = 0; k < 3; k++) {
        out[k][0] = v[k][0];
        out[k][1] = v[k][1];
        out[k][2] = v[k][2];
    }
    if (s->normal.y > 0.01f || s->normal.y < -0.01f) {
        return 3; /* floors and ceilings as they are */
    }
    maxY = v[0][1] > v[1][1] ? v[0][1] : v[1][1];
    maxY = maxY > v[2][1] ? maxY : v[2][1];
    for (k = 0; k < 3; k++) {
        /* probe a quarter of the way toward the centroid (not on the
         * vertex: shafts and steps share edge lines with the ground around
         * them) and 16 units out in front of the wall */
        struct Surface *floor;
        s32 cx = (v[0][0] + v[1][0] + v[2][0]) / 3, cz = (v[0][2] + v[1][2] + v[2][2]) / 3;
        f32 fx = v[k][0] + (cx - v[k][0]) / 4 + s->normal.x * 16.0f;
        f32 fz = v[k][2] + (cz - v[k][2]) / 4 + s->normal.z * 16.0f;
        f32 fy = find_floor(fx, (f32) (maxY + 50), fz, &floor);
        d[k] = floor != NULL ? v[k][1] - (s32) fy + TRIM_KEEP : 0x7FFF;
        if (d[k] < 0) buried++;
    }
    if (buried == 0) {
        return 3;
    }
    if (buried == 3) {
        sWallsDropped++;
        return 0;
    }
    /* keep the part with d >= 0 (Sutherland-Hodgman on one plane) */
    sWallsTrimmed++;
    for (k = 0; k < 3; k++) {
        s32 j = k == 2 ? 0 : k + 1, a;
        if (d[k] >= 0) {
            out[n][0] = v[k][0]; out[n][1] = v[k][1]; out[n][2] = v[k][2];
            n++;
        }
        if ((d[k] >= 0) != (d[j] >= 0)) {
            for (a = 0; a < 3; a++) {
                out[n][a] = v[k][a] + (v[j][a] - v[k][a]) * d[k] / (d[k] - d[j]);
            }
            n++;
        }
    }
    return n;
}

static s32 surface_chunk(const struct Surface *s) {
    return chunk_of((s->vertex1[0] + s->vertex2[0] + s->vertex3[0]) / 3,
                    (s->vertex1[2] + s->vertex2[2] + s->vertex3[2]) / 3);
}

static void finish_chunks(void) {
    s32 i, k;
    /* Bounding spheres: each chunk's box, and each parent's box around its
     * four children. */
    gRMesh.numChunks = 0;
    for (i = 0; i < R_MAX_SUPERS; i++) {
        s32 pmn[3] = { 32767, 32767, 32767 }, pmx[3] = { -32768, -32768, -32768 };
        s32 child, any = 0, a;
        for (child = 0; child < 4; child++) {
            struct RChunk *c = &gRMesh.chunks[i * 4 + child];
            s32 mn[3] = { 32767, 32767, 32767 }, mx[3] = { -32768, -32768, -32768 };
            s32 j;
            if (c->count == 0) {
                c->radius = -1;
                continue;
            }
            for (j = c->first; j < c->first + c->count; j++) {
                for (k = 0; k < 3; k++) {
                    const struct RVert *v = &gRMesh.verts[gRMesh.tris[j].v[k]];
                    s32 p[3] = { v->x, v->y, v->z };
                    for (a = 0; a < 3; a++) {
                        if (p[a] < mn[a]) mn[a] = p[a];
                        if (p[a] > mx[a]) mx[a] = p[a];
                    }
                }
            }
            sphere_from_box(c, mn, mx);
            for (a = 0; a < 3; a++) {
                if (mn[a] < pmn[a]) pmn[a] = mn[a];
                if (mx[a] > pmx[a]) pmx[a] = mx[a];
            }
            any = 1;
            gRMesh.numChunks++;
        }
        if (any) {
            sphere_from_box(&gRMesh.supers[i], pmn, pmx);
        } else {
            gRMesh.supers[i].radius = -1;
        }
    }
}

/* Pits: a floor closed in by walls that rise from all its edges to a rim of
 * floor behind them, like BoB's cannon shaft (a lid object covers it in the
 * game; the demo has no objects). With painter's sorting, a pit's far walls
 * get drawn over the ground in front of its opening. Seen from above its
 * rim, a pit is only visible through its opening, so render_iw.c clips its
 * triangles to the projected opening and sorts them at the opening's depth.
 *
 * Found in the collision surfaces by exact vertex matches (surfaces share
 * the level's vertices). Working memory is gRScratch, free once the mesh is
 * built. */
#define PIT_HASH 2048        /* bottom edges of rising walls: u16 surface index */
#define PIT_MAX_FLOORS 128   /* floors with an edge on a rising wall */
#define sPitHash ((u16 *) gRScratch)
#define sPitFloor ((u16 *) gRScratch + PIT_HASH)
#define sPitLabel ((u8 *) (sPitFloor + PIT_MAX_FLOORS)) /* pit label, 0xFF: none */

static u32 point_hash(const s16 *p) {
    return (u32) (p[0] * 73856093) ^ (u32) (p[1] * 19349663) ^ (u32) (p[2] * 83492791);
}

static s32 same_point(const s16 *a, const s16 *b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/* Is (a, b) an edge of wall w with w's third vertex above it? */
static s32 rises_from(const struct Surface *w, const s16 *a, const s16 *b) {
    const s16 *v[3] = { w->vertex1, w->vertex2, w->vertex3 };
    s32 k, hits = 0;
    const s16 *other = NULL;
    for (k = 0; k < 3; k++) {
        if (same_point(v[k], a) || same_point(v[k], b)) {
            hits++;
        } else {
            other = v[k];
        }
    }
    return hits == 2 && other != NULL && other[1] > a[1] && other[1] > b[1];
}

/* The wall rising from edge (a, b), or NULL. */
static const struct Surface *rising_wall(const s16 *a, const s16 *b) {
    u32 h = (point_hash(a) + point_hash(b)) & (PIT_HASH - 1);
    u16 i;
    while ((i = sPitHash[h]) != 0xFFFF) {
        if (rises_from(&sSurfacePool[i], a, b)) {
            return &sSurfacePool[i];
        }
        h = (h + 1) & (PIT_HASH - 1);
    }
    return NULL;
}

/* Another labeled candidate floor with edge (a, b), or -1. */
static s32 floor_sharing(s32 self, s32 n, const s16 *a, const s16 *b) {
    s32 j;
    for (j = 0; j < n; j++) {
        const struct Surface *g = &sSurfacePool[sPitFloor[j]];
        const s16 *v[3] = { g->vertex1, g->vertex2, g->vertex3 };
        s32 k, hits = 0;
        if (j == self || sPitLabel[j] == 0xFF) continue;
        for (k = 0; k < 3; k++) {
            hits += same_point(v[k], a) || same_point(v[k], b);
        }
        if (hits == 2) return j;
    }
    return -1;
}

/* Does corner a come before corner b going around the centroid? */
static s32 corner_before(s32 ax, s32 az, s32 bx, s32 bz) {
    s32 ha = az < 0 || (az == 0 && ax < 0), hb = bz < 0 || (bz == 0 && bx < 0);
    if (ha != hb) return ha < hb;
    return (s64) ax * bz - (s64) az * bx > 0;
}

/* One pit from the floors labeled `label`: checks its walls reach a rim,
 * orders its opening, and tags the render triangles inside it. With
 * `shade`, they're also darkened: lit by the sun like the ground outside,
 * an open shaft seen from above reads as a bump (BoB's cannon hole looked
 * like a pyramid), and a hole is in shadow anyway. */
static void add_pit(s32 label, s32 n, s32 shade) {
    struct RPit *pit = &gRMesh.pits[gRMesh.numPits];
    s32 i, k, nc = 0, floorY = 32767, rimY = -32768, cx = 0, cz = 0;
    s32 minX, maxX, minZ, maxZ;
    s32 x[4], z[4];

    for (i = 0; i < n; i++) {
        const struct Surface *s = &sSurfacePool[sPitFloor[i]];
        const s16 *v[3] = { s->vertex1, s->vertex2, s->vertex3 };
        if (sPitLabel[i] != label) continue;
        for (k = 0; k < 3; k++) {
            const struct Surface *w = rising_wall(v[k], v[k == 2 ? 0 : k + 1]);
            s32 c;
            for (c = 0; c < nc; c++) {
                if (x[c] == v[k][0] && z[c] == v[k][2]) break;
            }
            if (c == nc) {
                if (nc == 4) return; /* only 3- or 4-cornered openings */
                x[nc] = v[k][0];
                z[nc] = v[k][2];
                nc++;
            }
            if (v[k][1] < floorY) floorY = v[k][1];
            if (w != NULL) {
                /* the wall's top must meet a floor behind it: the rim */
                struct Surface *rim;
                s32 top = w->vertex1[1];
                f32 wx = (w->vertex1[0] + w->vertex2[0] + w->vertex3[0]) / 3.0f;
                f32 wz = (w->vertex1[2] + w->vertex2[2] + w->vertex3[2]) / 3.0f;
                f32 h;
                if (w->vertex2[1] > top) top = w->vertex2[1];
                if (w->vertex3[1] > top) top = w->vertex3[1];
                h = find_floor(wx - w->normal.x * 16.0f, (f32) (top + 50), wz - w->normal.z * 16.0f, &rim);
                if (rim == NULL || h < (f32) (top - 8) || h > (f32) (top + 8)) return;
                if (top > rimY) rimY = top;
            }
        }
    }
    if (nc < 3 || rimY <= floorY) return;

    /* corners in order around the centroid; the opening must be convex */
    for (i = 0; i < nc; i++) {
        cx += x[i];
        cz += z[i];
    }
    cx /= nc;
    cz /= nc;
    for (i = 1; i < nc; i++) {
        s32 tx = x[i], tz = z[i];
        for (k = i; k > 0 && corner_before(tx - cx, tz - cz, x[k - 1] - cx, z[k - 1] - cz); k--) {
            x[k] = x[k - 1];
            z[k] = z[k - 1];
        }
        x[k] = tx;
        z[k] = tz;
    }
    for (i = 0; i < nc; i++) {
        s32 j = i + 1 == nc ? 0 : i + 1, l = j + 1 == nc ? 0 : j + 1;
        if ((s64) (x[j] - x[i]) * (z[l] - z[j]) - (s64) (z[j] - z[i]) * (x[l] - x[j]) <= 0) return;
    }

    minX = maxX = x[0];
    minZ = maxZ = z[0];
    for (i = 0; i < nc; i++) {
        pit->x[i] = (s16) x[i];
        pit->z[i] = (s16) z[i];
        if (x[i] < minX) minX = x[i];
        if (x[i] > maxX) maxX = x[i];
        if (z[i] < minZ) minZ = z[i];
        if (z[i] > maxZ) maxZ = z[i];
    }
    pit->n = (u8) nc;
    pit->rimY = (s16) rimY;
    {
        s32 mn[3] = { minX, floorY, minZ }, mx[3] = { maxX, rimY, maxZ };
        struct RChunk tmp;
        sphere_from_box(&tmp, mn, mx);
        pit->cx = tmp.cx;
        pit->cy = tmp.cy;
        pit->cz = tmp.cz;
        pit->r = tmp.radius;
    }
    gRMesh.numPits++;
    /* the render triangles inside the pit's box: its walls and floor */
    for (i = 0; i < gRMesh.numTris; i++) {
        struct RTri *t = &gRMesh.tris[i];
        for (k = 0; k < 3; k++) {
            const struct RVert *rv = &gRMesh.verts[t->v[k]];
            if (rv->x < minX - 1 || rv->x > maxX + 1 || rv->z < minZ - 1 || rv->z > maxZ + 1
                || rv->y < floorY - 1 || rv->y > rimY + 1) {
                break;
            }
        }
        if (k == 3) {
            t->flags |= (u8) (gRMesh.numPits << R_TRI_PIT_SHIFT);
            if (shade) {
                /* color = material * 32 + light * 4: floor black, walls 0..2 */
                s32 light = (t->color >> 2) & 7;
                light = t->n[1] > 164 ? 0 : light * 2 / 7;
                t->color = (u8) ((t->color & 0xE0) | (light << 2));
            }
        }
    }
}

static void find_pits(s32 shade) {
    s32 i, k, n = 0, used = 0, changed;

    gRMesh.numPits = 0;
    for (i = 0; i < PIT_HASH; i++) {
        sPitHash[i] = 0xFFFF;
    }
    /* walls by the edge they rise from */
    for (i = 0; i < gSurfacesAllocated; i++) {
        const struct Surface *s = &sSurfacePool[i];
        const s16 *v[3] = { s->vertex1, s->vertex2, s->vertex3 };
        if (surface_hidden(s->type) || s->normal.y > 0.01f || s->normal.y < -0.01f) continue;
        for (k = 0; k < 3; k++) {
            const s16 *a = v[k], *b = v[k == 2 ? 0 : k + 1], *c = v[k == 0 ? 2 : k - 1];
            if (c[1] > a[1] && c[1] > b[1]) {
                u32 h = (point_hash(a) + point_hash(b)) & (PIT_HASH - 1);
                if (++used > PIT_HASH / 2) return; /* table too full: no pits */
                while (sPitHash[h] != 0xFFFF) {
                    h = (h + 1) & (PIT_HASH - 1);
                }
                sPitHash[h] = (u16) i;
            }
        }
    }
    /* candidate floors: an edge on a rising wall */
    for (i = 0; i < gSurfacesAllocated && n < PIT_MAX_FLOORS; i++) {
        const struct Surface *s = &sSurfacePool[i];
        const s16 *v[3] = { s->vertex1, s->vertex2, s->vertex3 };
        if (surface_hidden(s->type) || s->normal.y <= 0.01f) continue;
        for (k = 0; k < 3; k++) {
            if (rising_wall(v[k], v[k == 2 ? 0 : k + 1]) != NULL) {
                sPitFloor[n] = (u16) i;
                sPitLabel[n] = (u8) n;
                n++;
                break;
            }
        }
    }
    /* Keep the floors whose every edge is on a rising wall or shared with
     * another kept floor; floors sharing edges end up with the same label. */
    do {
        changed = 0;
        for (i = 0; i < n; i++) {
            const struct Surface *s = &sSurfacePool[sPitFloor[i]];
            const s16 *v[3] = { s->vertex1, s->vertex2, s->vertex3 };
            if (sPitLabel[i] == 0xFF) continue;
            for (k = 0; k < 3; k++) {
                const s16 *a = v[k], *b = v[k == 2 ? 0 : k + 1];
                s32 j;
                if (rising_wall(a, b) != NULL) continue;
                j = floor_sharing(i, n, a, b);
                if (j < 0) {
                    sPitLabel[i] = 0xFF;
                    changed = 1;
                    break;
                }
                if (sPitLabel[j] != sPitLabel[i]) {
                    u8 lo = sPitLabel[j] < sPitLabel[i] ? sPitLabel[j] : sPitLabel[i];
                    sPitLabel[i] = sPitLabel[j] = lo;
                    changed = 1;
                }
            }
        }
    } while (changed);

    for (i = 0; i < n && gRMesh.numPits < R_MAX_PITS; i++) {
        for (k = 0; k < n; k++) {
            if (sPitLabel[k] == i) break;
        }
        if (k < n) {
            add_pit(i, n, shade);
        }
    }
}

void render_build_level(const struct RLevelStyle *style, s32 terrainType) {
    s32 i, k, n = 0;
    u16 count[R_MAX_CHUNKS];
    (void) style;

    gRMesh.numVerts = 0;
    for (i = 0; i < HASH_SIZE; i++) {
        sHash[i] = 0xFFFF;
    }

    /* Pass 1: triangles per chunk. */
    for (i = 0; i < R_MAX_CHUNKS; i++) {
        count[i] = 0;
    }
    sWallsTrimmed = sWallsDropped = 0;
    for (i = 0; i < gSurfacesAllocated; i++) {
        const struct Surface *s = &sSurfacePool[i];
        s32 poly[4][3], pieces;
        if (surface_hidden(s->type)) continue;
        pieces = surface_polygon(s, poly) - 2; /* triangles: 0, 1 or 2 */
        if (pieces <= 0 || n + pieces > R_MAX_TRIS) continue;
        count[surface_chunk(s)] += (u16) pieces;
        n += pieces;
    }
    k = 0;
    for (i = 0; i < R_MAX_CHUNKS; i++) {
        gRMesh.chunks[i].first = (u16) k;
        gRMesh.chunks[i].count = 0;
        k += count[i];
    }

    /* Pass 2: build each triangle straight into its chunk's slot. */
    n = 0;
    for (i = 0; i < gSurfacesAllocated; i++) {
        const struct Surface *s = &sSurfacePool[i];
        struct RChunk *c;
        s32 poly[4][3], np, piece, nx, ny, nz, color;
        if (surface_hidden(s->type)) continue;
        np = surface_polygon(s, poly);
        if (np < 3 || n + (np - 2) > R_MAX_TRIS) continue;
        c = &gRMesh.chunks[surface_chunk(s)];
        nx = (s32) (s->normal.x * 16384.0f);
        ny = (s32) (s->normal.y * 16384.0f);
        nz = (s32) (s->normal.z * 16384.0f);
        color = surface_material(s, terrainType) * 32 + surface_light(s) * 4;
        for (piece = 0; piece < np - 2; piece++) {
            struct RTri *t = &gRMesh.tris[c->first + c->count];
            /* fan: (0, 1, 2) and (0, 2, 3) */
            t->v[0] = add_vertex((s16) poly[0][0], (s16) poly[0][1], (s16) poly[0][2]);
            t->v[1] = add_vertex((s16) poly[piece + 1][0], (s16) poly[piece + 1][1], (s16) poly[piece + 1][2]);
            t->v[2] = add_vertex((s16) poly[piece + 2][0], (s16) poly[piece + 2][1], (s16) poly[piece + 2][2]);
            if (t->v[0] == 0xFFFF || t->v[1] == 0xFFFF || t->v[2] == 0xFFFF) {
                /* vertex budget exhausted: leave a degenerate triangle */
                t->v[0] = t->v[1] = t->v[2] = 0;
            }
            t->n[0] = (s16) nx;
            t->n[1] = (s16) ny;
            t->n[2] = (s16) nz;
            t->d = -(nx * s->vertex1[0] + ny * s->vertex1[1] + nz * s->vertex1[2]);
            t->color = (u8) color;
            t->flags = 0;
            {
                /* sphere around this piece's box */
                s32 mn[3], mx[3], a, k2;
                for (a = 0; a < 3; a++) {
                    mn[a] = mx[a] = poly[0][a];
                    for (k2 = 1; k2 < 3; k2++) {
                        s32 val = poly[k2 == 1 ? piece + 1 : piece + 2][a];
                        if (val < mn[a]) mn[a] = val;
                        if (val > mx[a]) mx[a] = val;
                    }
                }
                {
                    struct RChunk tmp;
                    sphere_from_box(&tmp, mn, mx);
                    t->sc[0] = tmp.cx;
                    t->sc[1] = tmp.cy;
                    t->sc[2] = tmp.cz;
                    t->sr = tmp.radius;
                }
            }
            c->count++;
            n++;
        }
    }
    gRMesh.numTris = (u16) n;
    gRWallsTrimmed = sWallsTrimmed;
    gRWallsDropped = sWallsDropped;

    finish_chunks();
    find_pits(1);
}

/* Palette: materials x light x fog, sky gradient, player colors. */
void render_set_palette(const struct RLevelStyle *style) {
    s32 m, l, f, i;
    for (m = 0; m < PAL_MATERIALS; m++) {
        for (l = 0; l < 8; l++) {
            for (f = 0; f < 4; f++) {
                s32 c[3];
                for (i = 0; i < 3; i++) {
                    s32 lit = style->mat[m][i] * (5 + l * 3) / 26;           /* 19%..100% */
                    s32 fog = style->skyHorizon[i];
                    c[i] = lit + (fog - lit) * f * 5 / 16;                   /* 0..94% fog */
                    c[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : c[i];
                }
                PAL_BG[m * 32 + l * 4 + f] = RGB15(c[0], c[1], c[2]);
            }
        }
    }
    for (i = 0; i < 16; i++) {
        s32 c[3], j;
        for (j = 0; j < 3; j++) {
            c[j] = style->skyTop[j] + (style->skyHorizon[j] - style->skyTop[j]) * i / 15;
        }
        PAL_BG[PAL_SKY + i] = RGB15(c[0], c[1], c[2]);
    }
    /* Player stand-in: body (4 light levels), front marker, cap, shadow. */
    for (l = 0; l < 4; l++) {
        s32 k2 = 120 + l * 45;
        PAL_BG[PAL_PLAYER + l] = RGB15(k2, k2, k2 * 9 / 10);
        PAL_BG[PAL_PLAYER + 4 + l] = RGB15(k2 * 255 / 255, k2 * 150 / 255, 40);
        PAL_BG[PAL_PLAYER + 8 + l] = RGB15(40, k2 * 180 / 255, k2 * 200 / 255);
    }
    PAL_BG[PAL_SHADOW] = RGB15(24, 24, 32);
}

#ifdef DEMO_IMPORTED
#include "import.h"

/* Level mesh from the import (the level's visual geometry, colored from the
 * user's ROM by tools/import_rom.py): same chunk and sphere structure as the
 * collision mesh, planes computed here from the integer vertices. */
void render_build_imported(const struct ImpLevel *lv) {
    s32 i, k, n = 0;
    u16 count[R_MAX_CHUNKS];
    s32 nv = lv->numVerts < R_MAX_VERTS ? lv->numVerts : R_MAX_VERTS;
    s32 nt = lv->numTris < R_MAX_TRIS ? lv->numTris : R_MAX_TRIS;

    for (i = 0; i < nv; i++) {
        gRMesh.verts[i].x = lv->verts[i][0];
        gRMesh.verts[i].y = lv->verts[i][1];
        gRMesh.verts[i].z = lv->verts[i][2];
    }
    gRMesh.numVerts = (u16) nv;
    gRWallsTrimmed = gRWallsDropped = 0;

#define TRI_CHUNK(t) chunk_of((lv->verts[(t)->v[0]][0] + lv->verts[(t)->v[1]][0] + lv->verts[(t)->v[2]][0]) / 3, \
                              (lv->verts[(t)->v[0]][2] + lv->verts[(t)->v[1]][2] + lv->verts[(t)->v[2]][2]) / 3)
    for (i = 0; i < R_MAX_CHUNKS; i++) count[i] = 0;
    for (i = 0; i < nt; i++) count[TRI_CHUNK(&lv->tris[i])]++;
    k = 0;
    for (i = 0; i < R_MAX_CHUNKS; i++) {
        gRMesh.chunks[i].first = (u16) k;
        gRMesh.chunks[i].count = 0;
        k += count[i];
    }
    for (i = 0; i < nt; i++) {
        const struct ImpTri *it = &lv->tris[i];
        struct RChunk *c = &gRMesh.chunks[TRI_CHUNK(it)];
        struct RTri *t = &gRMesh.tris[c->first + c->count];
        const s16 *a = lv->verts[it->v[0]], *b = lv->verts[it->v[1]], *d = lv->verts[it->v[2]];
        s32 e1[3], e2[3], nn[3], len, mn[3], mx[3], j;
        struct RChunk tmp;
        if (it->v[0] >= nv || it->v[1] >= nv || it->v[2] >= nv) continue;
        for (j = 0; j < 3; j++) {
            e1[j] = b[j] - a[j];
            e2[j] = d[j] - a[j];
        }
        /* counter-clockwise front faces (F3D): n = e1 x e2 */
        {
            s64 cx = (s64) e1[1] * e2[2] - (s64) e1[2] * e2[1];
            s64 cy = (s64) e1[2] * e2[0] - (s64) e1[0] * e2[2];
            s64 cz = (s64) e1[0] * e2[1] - (s64) e1[1] * e2[0];
            /* scale down to keep the square in 32 bits, then normalize to Q14 */
            while (cx > 30000 || cx < -30000 || cy > 30000 || cy < -30000 || cz > 30000 || cz < -30000) {
                cx /= 2;
                cy /= 2;
                cz /= 2;
            }
            len = isqrt((u32) (cx * cx + cy * cy + cz * cz));
            if (len == 0) continue;
            nn[0] = (s32) (cx * 16384 / len);
            nn[1] = (s32) (cy * 16384 / len);
            nn[2] = (s32) (cz * 16384 / len);
        }
        t->v[0] = it->v[0];
        t->v[1] = it->v[1];
        t->v[2] = it->v[2];
        t->n[0] = (s16) nn[0];
        t->n[1] = (s16) nn[1];
        t->n[2] = (s16) nn[2];
        t->d = -(nn[0] * a[0] + nn[1] * a[1] + nn[2] * a[2]);
        t->color = it->color;
        t->flags = it->flags;
        for (j = 0; j < 3; j++) {
            mn[j] = mx[j] = a[j];
            if (b[j] < mn[j]) mn[j] = b[j];
            if (b[j] > mx[j]) mx[j] = b[j];
            if (d[j] < mn[j]) mn[j] = d[j];
            if (d[j] > mx[j]) mx[j] = d[j];
        }
        sphere_from_box(&tmp, mn, mx);
        t->sc[0] = tmp.cx;
        t->sc[1] = tmp.cy;
        t->sc[2] = tmp.cz;
        t->sr = tmp.radius;
        c->count++;
        n++;
    }
    gRMesh.numTris = (u16) n;
    finish_chunks();
    find_pits(0); /* the import's colors come from the ROM */
}

void render_set_imported_palette(const struct ImpLevel *lv) {
    s32 i;
    for (i = 0; i < 192; i++) PAL_BG[i] = lv->pal[i];
    for (i = 0; i < 32; i++) PAL_BG[IMP_MARIO_PAL + i] = gImpMarioPal[i];
    for (i = 0; i < 16; i++) PAL_BG[PAL_SKY + i] = lv->sky[i];
    PAL_BG[PAL_SHADOW] = RGB15(24, 24, 32);
}
#endif
