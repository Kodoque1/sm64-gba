/*
 * Per-frame renderer, compiled as ARM code into IWRAM (the *_iw.o rule).
 *
 *   1. chunk culling: bounding sphere against the view frustum and draw distance
 *   2. backface culling on the precomputed integer plane (before any transform)
 *   3. vertex transform with a per-frame cache, outcodes, projection
 *   4. clipping: always against the near plane; against the sides only when a
 *      vertex is outside a guard band 10x the screen (the rasterizer clamps
 *      spans to the screen, so anything inside the band can be drawn as is)
 *   5. bucket sort on depth (painter's algorithm, far to near)
 *   6. convex polygon fill straight into the mode 4 back page
 *
 * Fixed point throughout: world units, Q14 trig, 1/16-pixel screen coords,
 * 16.16 edge stepping, reciprocal tables in ROM instead of division.
 */
#include "render.h"

extern const s16 gSinQ14[4096];
extern const u32 gRecipZ[16384];
extern const u32 gRecipDy[2048];
extern const u32 gRecipClip[32768]; /* 2^30 / d */

#define NEAR_Z 32
#define OC_NEAR 0x01
#define OC_LEFT 0x02
#define OC_RIGHT 0x04
#define OC_TOP 0x08
#define OC_BOTTOM 0x10
#define OC_FAR 0x20
#define OC_GUARD 0x40 /* outside the guard band: needs side clipping */
#define OC_CLIP (OC_NEAR | OC_LEFT | OC_RIGHT | OC_TOP | OC_BOTTOM)

#define IWRAM_DATA __attribute__((section(".iwram")))

struct RStats gRStats IWRAM_DATA;

/* Per-frame vertex cache (EWRAM). */
/* Camera-space coordinates fit in s16: the camera stays within ~2000 units
 * of the level's [-8192, 8192] box, so rotated offsets stay under ~26000. */
static s16 sVX[R_MAX_VERTS], sVY[R_MAX_VERTS], sVZ[R_MAX_VERTS];
static s16 sSX[R_MAX_VERTS], sSY[R_MAX_VERTS];
static u8 sOC[R_MAX_VERTS];
static u16 sStamp[R_MAX_VERTS];
static u16 sFrame IWRAM_DATA;

/* Polygon queue. n == 0: a whole triangle, ref = triangle index (its
 * vertices are in the cache); n >= 3: a clipped polygon, ref = offset in sClip. */
struct RPoly {
    u16 next;
    u8 color;
    u8 n;
    u16 ref;
};
static struct RPoly sPolys[R_MAX_POLYS];
s16 gRScratch[R_MAX_CLIPVERTS * 2];
#define sClip gRScratch
static u16 sBucket[R_NUM_BUCKETS];
static s32 sNumPolys IWRAM_DATA, sClipUsed IWRAM_DATA;

/* Frame constants, in IWRAM: read for every vertex and triangle, and the
 * compiler has to reload them after each call. */
static s32 sCamX IWRAM_DATA, sCamY IWRAM_DATA, sCamZ IWRAM_DATA;
static s32 sSinY IWRAM_DATA, sCosY IWRAM_DATA, sSinP IWRAM_DATA, sCosP IWRAM_DATA;
static s32 sW IWRAM_DATA, sH IWRAM_DATA, sFs IWRAM_DATA, sFar IWRAM_DATA;
static s32 sBucketScale IWRAM_DATA, sFogScale IWRAM_DATA;
static u8 *sPage IWRAM_DATA;


/* GBA timers 2+3, cascaded, run by the demo as a cycle counter. */
static inline u32 cycles(void) {
    u32 hi, lo, hi2;
    do {
        hi = *(volatile u16 *) 0x0400010C;
        lo = *(volatile u16 *) 0x04000108;
        hi2 = *(volatile u16 *) 0x0400010C;
    } while (hi != hi2);
    return (hi << 16) | lo;
}

static inline s32 isin(u16 a) {
    return gSinQ14[a >> 4];
}

static inline s32 icos(u16 a) {
    return gSinQ14[(u16) (a + 0x4000) >> 4];
}

/* World -> camera space: x right, y up, z forward. */
static inline void to_camera(s32 x, s32 y, s32 z, s32 *o) {
    s32 dx = x - sCamX, dy = y - sCamY, dz = z - sCamZ;
    s32 zf = -((dx * sSinY + dz * sCosY) >> 14);
    o[0] = (dx * sCosY - dz * sSinY) >> 14;
    o[2] = (zf * sCosP - dy * sSinP) >> 14;
    o[1] = (zf * sSinP + dy * sCosP) >> 14;
}

/* Frustum: |x| <= 0.6 z, |y| <= 0.4 z (3:2 screen, ~62 x 44 degree fov). */
static inline u32 outcode(s32 x, s32 y, s32 z) {
    u32 oc = 0;
    if (z < NEAR_Z) oc |= OC_NEAR;
    if (5 * x + 3 * z < 0) oc |= OC_LEFT;
    if (3 * z - 5 * x < 0) oc |= OC_RIGHT;
    if (2 * z - 5 * y < 0) oc |= OC_TOP;
    if (2 * z + 5 * y < 0) oc |= OC_BOTTOM;
    if (z > sFar) oc |= OC_FAR;
    if (x > 6 * z || -x > 6 * z || y > 4 * z || -y > 4 * z) oc |= OC_GUARD;
    return oc;
}

/* Which planes a polygon with these combined outcodes must be clipped
 * against: near always, sides only when a vertex left the guard band. */
static inline u32 clip_planes(u32 ocOr) {
    if (!(ocOr & (OC_NEAR | OC_GUARD))) return 0;
    if (ocOr & OC_GUARD) return ocOr & OC_CLIP;
    return OC_NEAR;
}

/* Camera space -> 1/16-pixel screen coordinates. */
static inline void project(s32 x, s32 y, s32 z, s16 *sx, s16 *sy) {
    s32 r, tx, ty;
    if (z < 1) z = 1;
    if (z > 16383) z = 16383;
    r = (s32) gRecipZ[z];
    tx = (x * r) >> 8; /* x / z * 2^16 */
    ty = (y * r) >> 8;
    *sx = (s16) (sW * 8 + (s32) (((s64) tx * sFs) >> 16));
    *sy = (s16) (sH * 8 - (s32) (((s64) ty * sFs) >> 16));
}

static void vertex(u32 i) {
    const struct RVert *v = &gRMesh.verts[i];
    s32 c[3];
    u32 oc;
    to_camera(v->x, v->y, v->z, c);
    sVX[i] = (s16) c[0];
    sVY[i] = (s16) c[1];
    sVZ[i] = (s16) c[2];
    oc = outcode(c[0], c[1], c[2]);
    sOC[i] = (u8) oc;
    if (!(oc & (OC_NEAR | OC_GUARD))) {
        project(c[0], c[1], c[2], &sSX[i], &sSY[i]);
    }
    sStamp[i] = sFrame;
    gRStats.vertsTransformed++;
}

/* ---------- clipping (camera space, Sutherland-Hodgman) ---------- */

static inline s32 plane_dist(u32 plane, const s32 *p) {
    switch (plane) {
        case OC_NEAR: return p[2] - NEAR_Z;
        case OC_LEFT: return 5 * p[0] + 3 * p[2];
        case OC_RIGHT: return 3 * p[2] - 5 * p[0];
        case OC_TOP: return 2 * p[2] - 5 * p[1];
        default: return 2 * p[2] + 5 * p[1];
    }
}

#define CLIP_MAX 12

static s32 clip_polygon(s32 (*buf)[3], s32 n, u32 planes, s32 (*tmp)[3]) {
    u32 plane;
    for (plane = OC_NEAR; plane <= OC_BOTTOM; plane <<= 1) {
        s32 (*in)[3] = buf, (*out)[3] = tmp, m = 0, i;
        if (!(planes & plane)) continue;
        for (i = 0; i < n; i++) {
            const s32 *a = in[i], *b = in[i + 1 == n ? 0 : i + 1];
            s32 da = plane_dist(plane, a), db = plane_dist(plane, b);
            if (da >= 0 && m < CLIP_MAX) {
                out[m][0] = a[0]; out[m][1] = a[1]; out[m][2] = a[2]; m++;
            }
            if ((da >= 0) != (db >= 0) && m < CLIP_MAX) {
                /* Interpolate from the inside endpoint, so the two triangles
                 * sharing this edge compute the same point (no cracks).
                 * t = d_in / (d_in - d_out) in Q15. */
                const s32 *p0 = da >= 0 ? a : b, *p1 = da >= 0 ? b : a;
                s32 num = da >= 0 ? da : db, den = (da >= 0 ? da - db : db - da), t, k;
                while (den >= 32768 || den <= -32768) {
                    num >>= 1;
                    den >>= 1;
                }
                if (den < 0) {
                    num = -num;
                    den = -den;
                }
                t = den != 0 ? (s32) (((s64) num * (s32) gRecipClip[den]) >> 15) : 0;
                for (k = 0; k < 3; k++) {
                    out[m][k] = p0[k] + (s32) (((s64) (p1[k] - p0[k]) * t) >> 15);
                }
                m++;
            }
        }
        n = m;
        for (i = 0; i < n; i++) {
            buf[i][0] = out[i][0]; buf[i][1] = out[i][1]; buf[i][2] = out[i][2];
        }
        if (n < 3) return 0;
    }
    return n;
}

/* ---------- rasterizer ---------- */

void fill_words(u32 *dst, u32 value, s32 count); /* irq.s */

static void fill_span(u8 *row, s32 x0, s32 x1, u32 c) {
    u32 cc;
    if (x0 >= x1) return;
    if (x0 & 1) {
        u16 *p = (u16 *) (row + x0 - 1);
        *p = (u16) ((*p & 0x00FF) | (c << 8));
        x0++;
    }
    if (x1 & 1) {
        u16 *p = (u16 *) (row + x1 - 1);
        *p = (u16) ((*p & 0xFF00) | c);
        x1--;
    }
    if (x0 >= x1) return;
    cc = c | (c << 8);
    if (x0 & 2) {
        *(u16 *) (row + x0) = (u16) cc;
        x0 += 2;
    }
    cc |= cc << 16;
    {
        u32 *w = (u32 *) (row + x0);
        s32 nw = (x1 - x0) >> 2;
        if (nw >= 8) {
            fill_words(w, cc, nw);
            w += nw;
        } else {
            while (nw-- > 0) *w++ = cc;
        }
        x0 = (u8 *) w - row;
    }
    if (x1 - x0 >= 2) {
        *(u16 *) (row + x0) = (u16) cc;
    }
}

/* Edge position at the center of row r (1/16-pixel input, 16.16 output). */
static inline s32 edge_slope(s32 xa, s32 ya, s32 xb, s32 yb) {
    return (s32) (((s64) (xb - xa) * (s32) gRecipClip[yb - ya]) >> 14);
}

static inline s32 edge_x_at(s32 xa, s32 ya, s32 slope, s32 r) {
    return (xa << 12) + (s32) (((s64) slope * (r * 16 + 8 - ya)) >> 4);
}

/* Fill rows [r0, r1) between two edges given as 16.16 x at row r0 plus slopes.
 * Pixel i is filled when its center i + 0.5 is in [left, right). */
static inline void fill_rows(s32 r0, s32 r1, s32 xl, s32 sl, s32 xr, s32 sr, u32 color) {
    u8 *row = sPage + r0 * 240;
    const s32 wmax = sW;
#ifdef DEMO_IMPORTED
    /* The visual meshes have T-junctions (a vertex on another triangle's
     * edge), which leave pixel cracks; widening spans by half a pixel on
     * the right closes them (painter's order makes the overlap harmless). */
    xr += 0x8000;
#endif
    for (; r0 < r1; r0++, row += 240, xl += sl, xr += sr) {
        s32 x0 = (xl + 0x7FFF) >> 16, x1 = (xr + 0x7FFF) >> 16;
        if (x0 < 0) x0 = 0;
        if (x1 > wmax) x1 = wmax;
        fill_span(row, x0, x1, color);
    }
}

/* Flat triangle in 1/16-pixel coordinates: classic scan conversion, the long
 * edge (top to bottom vertex) on one side, the two short edges on the other. */
static void draw_tri(s32 xa, s32 ya, s32 xb, s32 yb, s32 xc, s32 yc, u32 color) {
    s32 t, rTop, rMid, rBot, sLong, xLong, cross;
    if (ya > yb) { t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; }
    if (yb > yc) { t = xb; xb = xc; xc = t; t = yb; yb = yc; yc = t; }
    if (ya > yb) { t = xa; xa = xb; xb = t; t = ya; ya = yb; yb = t; }
    if (ya == yc) return;

    rTop = (ya + 7) >> 4;
    rMid = (yb + 7) >> 4;
    rBot = (yc + 7) >> 4;
    if (rTop < 0) rTop = 0;
    if (rBot > sH) rBot = sH;
    if (rMid < rTop) rMid = rTop;
    if (rMid > rBot) rMid = rBot;
    if (rTop >= rBot) return;

    sLong = edge_slope(xa, ya, xc, yc);
    xLong = edge_x_at(xa, ya, sLong, rTop);
    /* which side is the middle vertex on? */
    cross = (s32) ((s64) (xb - xa) * (yc - ya) - (s64) (xc - xa) * (yb - ya));

    if (rTop < rMid) {
        s32 sShort = edge_slope(xa, ya, xb, yb);
        s32 xShort = edge_x_at(xa, ya, sShort, rTop);
        if (cross > 0) fill_rows(rTop, rMid, xLong, sLong, xShort, sShort, color);
        else fill_rows(rTop, rMid, xShort, sShort, xLong, sLong, color);
        xLong += sLong * (rMid - rTop);
    }
    if (rMid < rBot && yb != yc) {
        s32 sShort = edge_slope(xb, yb, xc, yc);
        s32 xShort = edge_x_at(xb, yb, sShort, rMid);
        if (cross > 0) fill_rows(rMid, rBot, xLong, sLong, xShort, sShort, color);
        else fill_rows(rMid, rBot, xShort, sShort, xLong, sLong, color);
    }
}

/* Convex polygon as a triangle fan (shared edges are computed identically,
 * so the fill rule leaves no cracks or double-filled pixels). */
static void draw_poly(const s16 *xy, s32 n, u32 color) {
    s32 i;
    for (i = 1; i + 1 < n; i++) {
        draw_tri(xy[0], xy[1], xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], color);
    }
}

/* ---------- queueing ---------- */

#define POLY_OCCLUDER 0x80 /* in RPoly.n: Mario is behind this polygon's plane */

static inline void queue(u32 color, u32 n, u32 ref, s32 depthSum) {
    struct RPoly *p;
    s32 b;
    if (sNumPolys >= R_MAX_POLYS) return;
    b = (depthSum * sBucketScale) >> 16;
    if (b < 0) b = 0;
    if (b >= R_NUM_BUCKETS) b = R_NUM_BUCKETS - 1;
    p = &sPolys[sNumPolys];
    p->color = (u8) color;
    p->n = (u8) n;
    p->ref = (u16) ref;
    p->next = sBucket[b];
    sBucket[b] = (u16) sNumPolys++;
}

static inline u32 fog_level(s32 depthSum) {
    s32 f = ((depthSum * sFogScale) >> 16) - 2;
    return f < 0 ? 0 : f > 3 ? 3 : (u32) f;
}

/* Clip a camera-space polygon, project it into sClip, return its offset or -1. */
static s32 clip_and_project(s32 (*buf)[3], s32 n, u32 planes, s32 *outN) {
    s32 tmp[CLIP_MAX][3];
    s32 i, base;
    n = clip_polygon(buf, n, planes, tmp);
    if (n < 3 || sClipUsed + n > R_MAX_CLIPVERTS) return -1;
    base = sClipUsed;
    for (i = 0; i < n; i++) {
        project(buf[i][0], buf[i][1], buf[i][2], &sClip[2 * (base + i)], &sClip[2 * (base + i) + 1]);
    }
    sClipUsed += n;
    *outN = n;
    return base;
}

/* Mario's position for the occluder test (world units), and whether the
 * imported model is drawn this frame. */
static s32 sMX IWRAM_DATA, sMY IWRAM_DATA, sMZ IWRAM_DATA, sMarioOn IWRAM_DATA, sNumOcc IWRAM_DATA;

/* ---------- pits ----------
 * Seen from above its rim, a pit (render_build.c) is only visible through
 * its opening: its triangles are clipped to the projected opening and sorted
 * at the opening's depth, so the ground around it and whatever is behind it
 * keep their order. Rare work, so it runs from ROM as Thumb code
 * (.text_rom, gba/gba_opt.ld). */
#ifdef __arm__
#define ROM_CODE __attribute__((section(".text_rom"), long_call, noinline, target("thumb")))
#else
#define ROM_CODE /* host build (src/test/render_check.c) */
#endif
enum { PIT_NORMAL, PIT_PORTAL };
static u8 sPitMode[R_MAX_PITS];
static s16 sPitXY[R_MAX_PITS][8];
static s32 sPitDepth[R_MAX_PITS];

static inline s32 sphere_visible(s32 x, s32 y, s32 z, s32 r);

ROM_CODE static void pits_frame(void) {
    u32 p, k;
    for (p = 0; p < gRMesh.numPits; p++) {
        const struct RPit *pit = &gRMesh.pits[p];
        s32 depth = 0;
        u32 oc = 0;
        sPitMode[p] = PIT_NORMAL;
        /* off screen: its triangles are culled anyway; below the rim: the
         * pit may stand above the ground around it, sort it normally */
        if (sCamY <= pit->rimY || !sphere_visible(pit->cx, pit->cy, pit->cz, pit->r)) continue;
        for (k = 0; k < pit->n; k++) {
            s32 c[3];
            to_camera(pit->x[k], pit->rimY, pit->z[k], c);
            oc |= outcode(c[0], c[1], c[2]);
            depth += c[2];
            if (!(oc & (OC_NEAR | OC_GUARD))) {
                project(c[0], c[1], c[2], &sPitXY[p][2 * k], &sPitXY[p][2 * k + 1]);
            }
        }
        if (!(oc & (OC_NEAR | OC_GUARD))) { /* otherwise: plain sorting */
            sPitMode[p] = PIT_PORTAL;
            sPitDepth[p] = depth * 3 / (s32) pit->n;
        }
    }
}

/* Clip a screen polygon (1/16 pixels) to a convex one; returns the count.
 * Everything here is inside the guard band (|coordinate| < 15000), so the
 * edge functions fit in 32 bits. */
ROM_CODE static s32 clip_to_opening(const s16 *in, s32 n, const s16 *q, s32 nq, s16 *out) {
    static s16 buf[2][2 * (CLIP_MAX + 4)]; /* EWRAM, not the IWRAM stack */
    const s16 *src = in;
    s32 e, i, area = 0;
    for (i = 0; i < nq; i++) {
        s32 j = i + 1 == nq ? 0 : i + 1;
        area += q[2 * i] * q[2 * j + 1] - q[2 * j] * q[2 * i + 1];
    }
    for (e = 0; e < nq; e++) {
        s32 j = e + 1 == nq ? 0 : e + 1, m = 0;
        s32 ex = q[2 * j] - q[2 * e], ey = q[2 * j + 1] - q[2 * e + 1];
        s16 *dst = e == nq - 1 ? out : buf[e & 1];
        if (area < 0) {
            ex = -ex;
            ey = -ey;
        }
        for (i = 0; i < n; i++) {
            const s16 *p0 = &src[2 * i], *p1 = &src[2 * (i + 1 == n ? 0 : i + 1)];
            s32 d0 = ex * (p0[1] - q[2 * e + 1]) - ey * (p0[0] - q[2 * e]);
            s32 d1 = ex * (p1[1] - q[2 * e + 1]) - ey * (p1[0] - q[2 * e]);
            if (d0 >= 0) {
                dst[2 * m] = p0[0];
                dst[2 * m + 1] = p0[1];
                m++;
            }
            if ((d0 >= 0) != (d1 >= 0)) {
                /* t = d0 / (d0 - d1) in Q15, from the reciprocal table
                 * (num <= den, so num * 2^30 / den fits) */
                u32 num = d0 >= 0 ? (u32) d0 : (u32) -d0;
                u32 den = num + (d1 >= 0 ? (u32) d1 : (u32) -d1);
                s32 t;
                while (den >= 32768) {
                    num >>= 1;
                    den >>= 1;
                }
                t = den != 0 ? (s32) ((num * gRecipClip[den]) >> 15) : 0;
                dst[2 * m] = (s16) (p0[0] + (((p1[0] - p0[0]) * t) >> 15));
                dst[2 * m + 1] = (s16) (p0[1] + (((p1[1] - p0[1]) * t) >> 15));
                m++;
            }
        }
        if (m < 3) return 0;
        n = m;
        src = dst;
    }
    return n;
}

/* A pit triangle seen through the pit's opening. */
ROM_CODE static void pit_triangle(const struct RTri *t, u32 p, u32 occ) {
    u32 a = t->v[0], b = t->v[1], c = t->v[2], ocOr;
    s16 xy[6];
    const s16 *src = xy;
    s32 n = 3, m;

    if (sStamp[a] != sFrame) vertex(a);
    if (sStamp[b] != sFrame) vertex(b);
    if (sStamp[c] != sFrame) vertex(c);
    if (sOC[a] & sOC[b] & sOC[c]) return;
    gRStats.trisDrawn++;
    ocOr = clip_planes(sOC[a] | sOC[b] | sOC[c]);
    if (!ocOr) {
        xy[0] = sSX[a]; xy[1] = sSY[a];
        xy[2] = sSX[b]; xy[3] = sSY[b];
        xy[4] = sSX[c]; xy[5] = sSY[c];
    } else {
        static s32 buf[CLIP_MAX][3]; /* EWRAM, not the IWRAM stack */
        s32 base;
        buf[0][0] = sVX[a]; buf[0][1] = sVY[a]; buf[0][2] = sVZ[a];
        buf[1][0] = sVX[b]; buf[1][1] = sVY[b]; buf[1][2] = sVZ[b];
        buf[2][0] = sVX[c]; buf[2][1] = sVY[c]; buf[2][2] = sVZ[c];
        base = clip_and_project(buf, 3, ocOr, &n);
        if (base < 0) return;
        src = &sClip[2 * base];
    }
    if (sClipUsed + CLIP_MAX + 4 > R_MAX_CLIPVERTS) return;
    m = clip_to_opening(src, n, sPitXY[p], gRMesh.pits[p].n, &sClip[2 * sClipUsed]);
    if (m < 3) return;
#ifdef DEMO_IMPORTED
    if (occ) sNumOcc++;
#endif
    queue(t->color + fog_level(sVZ[a] + sVZ[b] + sVZ[c]), m | occ, sClipUsed, sPitDepth[p]);
    sClipUsed += m;
}

static void level_triangle(u32 ti, u32 occ) {
    const struct RTri *t = &gRMesh.tris[ti];
    u32 a = t->v[0], b = t->v[1], c = t->v[2], ocAnd, ocOr, pit = t->flags >> R_TRI_PIT_SHIFT;
    s32 depth;

    if (pit && sPitMode[pit - 1] == PIT_PORTAL) {
        pit_triangle(t, pit - 1, occ);
        return;
    }
    if (sStamp[a] != sFrame) vertex(a);
    if (sStamp[b] != sFrame) vertex(b);
    if (sStamp[c] != sFrame) vertex(c);
    ocAnd = sOC[a] & sOC[b] & sOC[c];
    if (ocAnd) return;
    gRStats.trisDrawn++;
    depth = sVZ[a] + sVZ[b] + sVZ[c];
    ocOr = clip_planes(sOC[a] | sOC[b] | sOC[c]);
    {
        u32 color = t->color + fog_level(depth);
#ifdef DEMO_IMPORTED
        if (t->flags & R_TRI_DECAL) depth -= 3 * 192; /* on top of the surface below */
        if (occ) sNumOcc++;
#endif
        if (!ocOr) {
            queue(color, occ, ti, depth);
            return;
        }
    }
    {
        s32 buf[CLIP_MAX][3], n, base;
        buf[0][0] = sVX[a]; buf[0][1] = sVY[a]; buf[0][2] = sVZ[a];
        buf[1][0] = sVX[b]; buf[1][1] = sVY[b]; buf[1][2] = sVZ[b];
        buf[2][0] = sVX[c]; buf[2][1] = sVY[c]; buf[2][2] = sVZ[c];
        base = clip_and_project(buf, 3, ocOr, &n);
        if (base >= 0) {
            gRStats.clipped++;
            queue(t->color + fog_level(sVZ[a] + sVZ[b] + sVZ[c]), n | occ, base, depth);
        }
    }
}

/* Bounding sphere against frustum and draw distance. Plane normals for the
 * side planes are (5,0,3)/5.83 and (0,5,2)/5.39, so compare scaled. */
static inline s32 sphere_visible(s32 x, s32 y, s32 z, s32 r) {
    s32 c[3];
    to_camera(x, y, z, c);
    if (c[2] + r < NEAR_Z) return 0;
    if (c[2] - r > sFar) return 0;
    if (5 * c[0] + 3 * c[2] < -r * 6) return 0;
    if (3 * c[2] - 5 * c[0] < -r * 6) return 0;
    if (2 * c[2] - 5 * c[1] < -r * 6) return 0;
    if (2 * c[2] + 5 * c[1] < -r * 6) return 0;
    return 1;
}

static s32 chunk_visible(const struct RChunk *ch) {
    s32 c[3], r = ch->radius;
    to_camera(ch->cx, ch->cy, ch->cz, c);
    if (c[2] + r < NEAR_Z) return 0;
    if (c[2] - r > sFar) return 0;
    if (5 * c[0] + 3 * c[2] < -r * 6) return 0;
    if (3 * c[2] - 5 * c[0] < -r * 6) return 0;
    if (2 * c[2] - 5 * c[1] < -r * 6) return 0;
    if (2 * c[2] + 5 * c[1] < -r * 6) return 0;
    return 1;
}

/* ---------- immediate polygons (player, shadow), drawn after the level ---------- */

static void draw_world_poly(s32 (*w)[3], s32 n, u32 color) {
    s32 buf[CLIP_MAX][3], i;
    u32 ocAnd = 0xFF, ocOr = 0;
    s16 xy[2 * CLIP_MAX];
    for (i = 0; i < n; i++) {
        u32 oc;
        to_camera(w[i][0], w[i][1], w[i][2], buf[i]);
        oc = outcode(buf[i][0], buf[i][1], buf[i][2]);
        ocAnd &= oc;
        ocOr |= oc;
    }
    if (ocAnd) return;
    ocOr = clip_planes(ocOr);
    if (ocOr) {
        s32 tmp[CLIP_MAX][3];
        n = clip_polygon(buf, n, ocOr, tmp);
        if (n < 3) return;
    }
    for (i = 0; i < n; i++) {
        project(buf[i][0], buf[i][1], buf[i][2], &xy[2 * i], &xy[2 * i + 1]);
    }
    draw_poly(xy, n, color);
}

/* The player's blob shadow (the body is the mannequin's sprites, or the
 * imported model drawn by draw_mario). */
static void draw_player(const struct RPlayer *pl) {
    s32 k;

    /* Blob shadow: an octagon on the floor, shrinking with height. */
    if (pl->y - pl->shadowY < 1500) {
        s32 oct[8][3], rad = 52 - (pl->y - pl->shadowY) / 40;
        if (rad > 12) {
            for (k = 0; k < 8; k++) {
                u16 a = (u16) (k * 0x2000);
                oct[k][0] = pl->x + ((rad * isin(a)) >> 14);
                oct[k][1] = pl->shadowY + 3;
                oct[k][2] = pl->z + ((rad * icos(a)) >> 14);
            }
            draw_world_poly(oct, 8, PAL_SHADOW);
        }
    }

}

/* ---------- the imported Mario model ---------- */
#ifdef DEMO_IMPORTED
#include "import.h"

#define MARIO_MAX_VERTS 160
#define MARIO_MAX_TRIS 240 /* < 256: the sort packs the index in 8 bits */

static s16 sMCam[MARIO_MAX_VERTS][3], sMScr[MARIO_MAX_VERTS][2];
static u8 sMOC[MARIO_MAX_VERTS];
struct MPart {
    s32 o[3];
    s16 q[3][3], ls[3], eye[3]; /* eye: camera in the part's frame, model units / 4 */
};
static struct MPart sMPart[20];
struct MPoly {
    u8 n, color;
    u16 ref;
};
static struct MPoly sMPolys[MARIO_MAX_TRIS];
#define MARIO_BUCKETS 64
static u8 sMNext[MARIO_MAX_TRIS], sMHead[MARIO_BUCKETS]; /* depth buckets, 0xFF = end */

/* sun (0.40, 0.82, 0.41) in Q14, as the level's baked lighting */
#define SUN_X 6554
#define SUN_Y 13435
#define SUN_Z 6717

/* Mario's triangles: each vertex placed by its part's posed frame (the
 * game's animation, see mannequin.c), lit per triangle from its rotated
 * normal, clipped like level triangles and sorted among themselves. Drawn
 * after the level; then the level polygons that are in front of him
 * (POLY_OCCLUDER, and overlapping his screen box) are drawn again. */
/* front-facing triangle list: shares sMNext (pass 3 writes sMNext[n] only
 * after reading sMFront[i], and n <= i) */
#define sMFront sMNext

static void draw_mario(const struct RFrame *fr) {
    s32 i, n = 0, nf = 0, bx0 = 32767, by0 = 32767, bx1 = -32768, by1 = -32768;
    s32 zMin = 32767, zMax = -32768, zMin3, zScale;
    s32 nv = gImpMarioNumVerts < MARIO_MAX_VERTS ? gImpMarioNumVerts : MARIO_MAX_VERTS;
    s32 nt = gImpMarioNumTris < MARIO_MAX_TRIS ? gImpMarioNumTris : MARIO_MAX_TRIS;

    /* Per part: its frame rotated into camera space (rows q, origin o in
     * camera units * 16), the sun in the part's frame (lighting), and the
     * camera position in the part's frame, model units (backface culling
     * before any vertex is transformed). */
    for (i = 0; i < 20; i++) {
        const struct RFrame *f = &fr[i];
        struct MPart *mp = &sMPart[i];
        s32 k, d[3];
        for (k = 0; k < 3; k++) {
            s32 rx = f->r[k][0], ry = f->r[k][1], rz = f->r[k][2];
            s32 zf = -((rx * sSinY + rz * sCosY) >> 14);
            mp->q[k][0] = (s16) ((rx * sCosY - rz * sSinY) >> 14);
            mp->q[k][2] = (s16) ((zf * sCosP - ry * sSinP) >> 14);
            mp->q[k][1] = (s16) ((zf * sSinP + ry * sCosP) >> 14);
            mp->ls[k] = (s16) ((rx * SUN_X + ry * SUN_Y + rz * SUN_Z) >> 14);
        }
        /* camera - origin, world units */
        d[0] = (sCamX * 16 - f->t[0]) >> 4;
        d[1] = (sCamY * 16 - f->t[1]) >> 4;
        d[2] = (sCamZ * 16 - f->t[2]) >> 4;
        for (k = 0; k < 3; k++) {
            /* world units along the part's axis k, * 4 = model units */
            s32 e = (d[0] * f->r[k][0] + d[1] * f->r[k][1] + d[2] * f->r[k][2]) >> 14;
            mp->eye[k] = (s16) (e > 30000 ? 30000 : e < -30000 ? -30000 : e);
        }
        {
            /* world * 16 offsets reach ~2^17: rotate world units, keep 4 bits */
            s32 dx = -d[0], dy = -d[1], dz = -d[2];
            s32 zf = -((dx * sSinY + dz * sCosY) >> 10);
            mp->o[0] = (dx * sCosY - dz * sSinY) >> 10;
            mp->o[2] = ((zf >> 4) * sCosP - dy * sSinP) >> 10;
            mp->o[1] = ((zf >> 4) * sSinP + dy * sCosP) >> 10;
        }
    }

    /* Pass 1, model space: which triangles face the camera, and which
     * vertices they use. */
    for (i = 0; i < nv; i++) sMOC[i] = 0; /* 0: not needed this frame */
    for (i = 0; i < nt; i++) {
        const struct ImpMTri *t = &gImpMarioTris[i];
        const struct ImpMVert *v = &gImpMarioVerts[t->v[0]];
        const struct MPart *mp = &sMPart[t->part];
        s32 dx = mp->eye[0] - (v->x >> 2), dy = mp->eye[1] - (v->y >> 2), dz = mp->eye[2] - (v->z >> 2);
        if (t->n[0] * dx + t->n[1] * dy + t->n[2] * dz <= 0) continue;
        sMOC[t->v[0]] = sMOC[t->v[1]] = sMOC[t->v[2]] = 0x80; /* needed */
        sMFront[nf++] = (u8) i;
    }

    /* Pass 2: transform the vertices front faces use. */
    {
        s32 part = -1, q00 = 0, q01 = 0, q02 = 0, q10 = 0, q11 = 0, q12 = 0, q20 = 0, q21 = 0, q22 = 0;
        s32 o0 = 0, o1 = 0, o2 = 0;
        for (i = 0; i < nv; i++) {
            const struct ImpMVert *v;
            s32 c[3], x, y, z;
            u32 oc;
            if (!sMOC[i]) continue;
            v = &gImpMarioVerts[i];
            x = v->x;
            y = v->y;
            z = v->z;
            if (v->part != part) {
                const struct MPart *mp = &sMPart[v->part];
                part = v->part;
                q00 = mp->q[0][0]; q01 = mp->q[0][1]; q02 = mp->q[0][2];
                q10 = mp->q[1][0]; q11 = mp->q[1][1]; q12 = mp->q[1][2];
                q20 = mp->q[2][0]; q21 = mp->q[2][1]; q22 = mp->q[2][2];
                o0 = mp->o[0]; o1 = mp->o[1]; o2 = mp->o[2];
            }
            /* model units * 0.25 (GEO_SCALE) * 16 = * 4 = >> 12 from Q14 */
            c[0] = (o0 + ((x * q00 + y * q10 + z * q20) >> 12) + 8) >> 4;
            c[1] = (o1 + ((x * q01 + y * q11 + z * q21) >> 12) + 8) >> 4;
            c[2] = (o2 + ((x * q02 + y * q12 + z * q22) >> 12) + 8) >> 4;
            sMCam[i][0] = (s16) c[0];
            sMCam[i][1] = (s16) c[1];
            sMCam[i][2] = (s16) c[2];
            if (c[2] < zMin) zMin = c[2];
            if (c[2] > zMax) zMax = c[2];
            oc = outcode(c[0], c[1], c[2]);
            sMOC[i] = (u8) oc;
            if (!(oc & (OC_NEAR | OC_GUARD))) {
                s16 sx, sy;
                project(c[0], c[1], c[2], &sx, &sy);
                sMScr[i][0] = sx;
                sMScr[i][1] = sy;
                if (sx < bx0) bx0 = sx;
                if (sx > bx1) bx1 = sx;
                if (sy < by0) by0 = sy;
                if (sy > by1) by1 = sy;
            } else {
                bx0 = by0 = -32768; /* partly clipped: redraw occluders anywhere */
                bx1 = by1 = 32767;
            }
        }
    }

    /* Pass 3: front faces: clip, light, sort. */
    zMin3 = zMin * 3;
    zScale = (MARIO_BUCKETS << 16) / ((zMax - zMin) * 3 + 1);
    for (i = 0; i < MARIO_BUCKETS; i++) sMHead[i] = 0xFF;
    for (i = 0; i < nf; i++) {
        const struct ImpMTri *t = &gImpMarioTris[sMFront[i]];
        u32 a = t->v[0], b = t->v[1], c = t->v[2], ocOr, color;
        s32 depth, lit, k;
        if (sMOC[a] & sMOC[b] & sMOC[c]) continue;
        ocOr = clip_planes(sMOC[a] | sMOC[b] | sMOC[c]);
        if (n >= MARIO_MAX_TRIS) break;
        /* (n R) . sun = n . (R sun): the sun in the part's frame, 4 levels */
        {
            const struct MPart *mp = &sMPart[t->part];
            lit = (t->n[0] * mp->ls[0] + t->n[1] * mp->ls[1] + t->n[2] * mp->ls[2]) >> 14; /* Q14 */
        }
        lit = lit <= 0 ? 0 : (lit * 3 + 8192) >> 14;
        color = IMP_MARIO_PAL + t->mat * 4 + (lit > 3 ? 3 : lit);
        depth = sMCam[a][2] + sMCam[b][2] + sMCam[c][2];
        if (!ocOr) {
            s32 base = sClipUsed;
            if (base + 3 > R_MAX_CLIPVERTS) break;
            sClip[2 * base + 0] = sMScr[a][0]; sClip[2 * base + 1] = sMScr[a][1];
            sClip[2 * base + 2] = sMScr[b][0]; sClip[2 * base + 3] = sMScr[b][1];
            sClip[2 * base + 4] = sMScr[c][0]; sClip[2 * base + 5] = sMScr[c][1];
            sClipUsed += 3;
            sMPolys[n].n = 3;
            sMPolys[n].ref = (u16) base;
        } else {
            s32 buf[CLIP_MAX][3], m, base;
            for (k = 0; k < 3; k++) {
                buf[0][k] = sMCam[a][k];
                buf[1][k] = sMCam[b][k];
                buf[2][k] = sMCam[c][k];
            }
            base = clip_and_project(buf, 3, ocOr, &m);
            if (base < 0) continue;
            sMPolys[n].n = (u8) m;
            sMPolys[n].ref = (u16) base;
        }
        /* bucket sort over Mario's own depth range */
        {
            s32 bk = ((depth - zMin3) * zScale) >> 16;
            bk = bk < 0 ? 0 : bk >= MARIO_BUCKETS ? MARIO_BUCKETS - 1 : bk;
            sMNext[n] = sMHead[bk];
            sMHead[bk] = (u8) n;
        }
        sMPolys[n].color = (u8) color;
        n++;
    }

    for (i = MARIO_BUCKETS - 1; i >= 0; i--) {
        u32 p = sMHead[i];
        while (p != 0xFF) {
            const struct MPoly *mp = &sMPolys[p];
            draw_poly(&sClip[2 * mp->ref], mp->n, mp->color);
            p = sMNext[p];
        }
    }
    gRStats.marioTris = n;
    if (n == 0 || sNumOcc == 0) return;

    /* occluders: level polygons in front of Mario, redrawn far to near where
     * their screen box meets his */
    for (i = R_NUM_BUCKETS - 1; i >= 0; i--) {
        u32 p = sBucket[i];
        while (p != 0xFFFF) {
            const struct RPoly *poly = &sPolys[p];
            if (poly->n & POLY_OCCLUDER) {
                s16 xy[2 * CLIP_MAX];
                const s16 *pts;
                s32 m = poly->n & 0x7F, k, x0 = 32767, x1 = -32768, y0 = 32767, y1 = -32768;
                if (m == 0) {
                    const struct RTri *t = &gRMesh.tris[poly->ref];
                    for (k = 0; k < 3; k++) {
                        xy[2 * k] = sSX[t->v[k]];
                        xy[2 * k + 1] = sSY[t->v[k]];
                    }
                    pts = xy;
                    m = 3;
                } else {
                    pts = &sClip[2 * poly->ref];
                }
                for (k = 0; k < m; k++) {
                    if (pts[2 * k] < x0) x0 = pts[2 * k];
                    if (pts[2 * k] > x1) x1 = pts[2 * k];
                    if (pts[2 * k + 1] < y0) y0 = pts[2 * k + 1];
                    if (pts[2 * k + 1] > y1) y1 = pts[2 * k + 1];
                }
                if (x1 >= bx0 && x0 <= bx1 && y1 >= by0 && y0 <= by1) {
                    draw_poly(pts, m, poly->color);
                    gRStats.occluders++;
                }
            }
            p = poly->next;
        }
    }
}
#endif

/* ---------- frame ---------- */

void render_frame(u16 *page, const struct RCamera *cam, const struct RPlayer *player,
                  const struct RQuality *q) {
    s32 i, r;
    u32 t0, t1, t2, t3;

    sPage = (u8 *) page;
    sW = q->width;
    sH = q->height;
    sFar = q->farDist;
    sFs = sW * 40 / 3;
    sBucketScale = (R_NUM_BUCKETS << 16) / (3 * sFar);
    sFogScale = (6 << 16) / (3 * sFar);
    sCamX = cam->x;
    sCamY = cam->y;
    sCamZ = cam->z;
    sSinY = isin(cam->yaw);
    sCosY = icos(cam->yaw);
    sSinP = isin(cam->pitch);
    sCosP = icos(cam->pitch);

    if (++sFrame == 0) {
        for (i = 0; i < R_MAX_VERTS; i++) sStamp[i] = 0;
        sFrame = 1;
    }
    sNumPolys = 0;
    sClipUsed = 0;
    for (i = 0; i < R_NUM_BUCKETS; i++) sBucket[i] = 0xFFFF;
    gRStats.chunksDrawn = gRStats.trisConsidered = gRStats.trisDrawn = gRStats.clipped = 0;
    gRStats.vertsTransformed = 0;
    gRStats.marioTris = gRStats.occluders = gRStats.cyclesMario = 0;
#ifdef DEMO_IMPORTED
    sMarioOn = player->visible && player->frames != 0;
#else
    sMarioOn = 0;
#endif
    sNumOcc = 0;
    sMX = player->x;
    sMY = player->y + 80; /* body center: Mario is ~160 units tall */
    sMZ = player->z;
    if (gRMesh.numPits) {
        pits_frame();
    }
    t0 = cycles();

    /* Background: sky gradient above the horizon, the level's ground color
     * (heavily fogged) below it, so pinholes between ground
     * triangles (T-junctions in the collision mesh) don't flash sky. The
     * horizon is where a level direction projects: row H/2 - F tan(pitch). */
    {
        u8 *row = sPage;
        s32 step = (16 << 16) / sH, acc = 0;
        s32 horizon = sH / 2 - (s32) (((s64) sFs * isin(cam->pitch) / (icos(cam->pitch) | 1)) >> 4);
        u32 ground = q->groundColor;
        ground |= ground << 8;
        ground |= ground << 16;
        for (r = 0; r < sH; r++, row += 240, acc += step) {
            u32 c, *w = (u32 *) row;
            s32 nw = sW >> 2;
            if (r > horizon + 1) {
                c = ground;
            } else {
                c = PAL_SKY + (acc >> 16);
                c |= c << 8;
                c |= c << 16;
            }
            fill_words(w, c, nw);
        }
    }

    t1 = cycles();
    for (i = 0; i < R_MAX_SUPERS; i++) {
        s32 child;
        if (gRMesh.supers[i].radius < 0 || !chunk_visible(&gRMesh.supers[i])) continue;
        for (child = 0; child < 4; child++) {
            const struct RChunk *ch = &gRMesh.chunks[i * 4 + child];
            u32 t, end;
            if (ch->radius < 0 || !chunk_visible(ch)) continue;
            gRStats.chunksDrawn++;
            end = ch->first + ch->count;
            for (t = ch->first; t < end; t++) {
                const struct RTri *tri = &gRMesh.tris[t];
                s32 side = tri->n[0] * sCamX + tri->n[1] * sCamY + tri->n[2] * sCamZ + tri->d;
                u32 occ = 0;
                /* backface: camera on the front side of the plane (Q14) */
#ifdef DEMO_IMPORTED
                if (side <= 0 && !(tri->flags & R_TRI_DOUBLE)) continue;
#else
                if (side <= 0) continue;
#endif
                /* off-screen triangles stop here, before their vertices are transformed */
                if (!sphere_visible(tri->sc[0], tri->sc[1], tri->sc[2], tri->sr)) continue;
                gRStats.trisConsidered++;
#ifdef DEMO_IMPORTED
                if (sMarioOn) {
                    /* Mario on the other side of the plane from the camera:
                     * wherever this triangle overlaps him on screen it is in
                     * front of him (the plane separates them) */
                    s32 ms = tri->n[0] * sMX + tri->n[1] * sMY + tri->n[2] * sMZ + tri->d;
                    /* most of his body (center 50 units past the plane): a
                     * plane grazing him (sloped ground, a wall he leans on)
                     * must not paint over him */
                    if ((side > 0 && ms < -50 * 16384) || (side < 0 && ms > 50 * 16384)) occ = POLY_OCCLUDER;
                }
#endif
                level_triangle(t, occ);
            }
        }
    }

    t2 = cycles();
    /* draw far to near */
    for (i = R_NUM_BUCKETS - 1; i >= 0; i--) {
        u32 p = sBucket[i];
        while (p != 0xFFFF) {
            const struct RPoly *poly = &sPolys[p];
            if ((poly->n & 0x7F) == 0) {
                const struct RTri *t = &gRMesh.tris[poly->ref];
                s16 xy[6];
                xy[0] = sSX[t->v[0]]; xy[1] = sSY[t->v[0]];
                xy[2] = sSX[t->v[1]]; xy[3] = sSY[t->v[1]];
                xy[4] = sSX[t->v[2]]; xy[5] = sSY[t->v[2]];
                draw_poly(xy, 3, poly->color);
            } else {
                draw_poly(&sClip[2 * poly->ref], poly->n & 0x7F, poly->color);
            }
            p = poly->next;
        }
    }

    if (player->visible) {
        draw_player(player);
    }
#ifdef DEMO_IMPORTED
    if (sMarioOn) {
        u32 tm = cycles();
        draw_mario(player->frames);
        gRStats.cyclesMario = cycles() - tm;
    }
#endif
    t3 = cycles();
    gRStats.cyclesClear = t1 - t0;
    gRStats.cyclesSetup = t2 - t1;
    gRStats.cyclesDraw = t3 - t2;
}
