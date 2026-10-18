/*
 * Property tests for the exactness arguments in src/opt/surface_collision.c.
 *
 * The replay corpus only tests the inputs Mario happens to produce. These
 * tests attack each rewrite directly, on the host reference build (strict
 * IEEE single precision), with millions of adversarial cases:
 *
 *   floor/ceil   f32_floor_s32 / f32_ceil_s32 against the libm result
 *   y-range      (y < L) == (floor(y) < L), (y > U) == (ceil(y) > U), all s16 L, U
 *   compares     f32_key ordering, F32_GT0 / F32_LT0 against IEEE compares
 *   plane        for every wall of every level, points placed within a few
 *                units of the radius boundary: whenever the integer filter
 *                rejects, the original float test must reject too
 *   edges        wall_edge_sign against the original float edge expression:
 *                random cases, plus adversarial ones built so the integer
 *                estimate sits just past its threshold while the exact value
 *                is within float rounding of zero (large coordinates,
 *                fractional parts just below 1)
 *
 * Exit code 0 only if every property holds.
 */
#include "opt/surface_collision.c"

#include "engine/surface_load.h"
#include "levels.h"

int printf(const char *fmt, ...);
double floor(double);

struct MarioState gMarioStates[1]; /* normally in harness.c */

static u32 sRng = 0x12345678u;

static u32 rnd(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static f32 bits_f32(u32 b) {
    union { u32 u; f32 f; } x;
    x.u = b;
    return x.f;
}

/* Floats spread across every magnitude, plus the awkward ones. */
static f32 rnd_f32(void) {
    static const u32 special[] = { 0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000,
                                   0xBF000000, 0x00000001, 0x80000001, 0x4B000000, 0xCB000000,
                                   0x4AFFFFFF, 0xCAFFFFFF, 0x3F7FFFFF, 0xBF7FFFFF };
    u32 b;
    if ((rnd() & 15) == 0) {
        return bits_f32(special[rnd() % (sizeof(special) / sizeof(special[0]))]);
    }
    do {
        b = rnd();
        if (rnd() & 1) { /* bias toward |f| < 2^17, where the game lives */
            b = (b & 0x807FFFFF) | ((u32) (100 + rnd() % 44) << 23);
        }
    } while (((b >> 23) & 0xFF) == 0xFF); /* no NaN / Inf */
    return bits_f32(b);
}

static s32 ref_floor(f32 f) {
    double d = floor((double) f);
    if (d > (double) (1 << 30)) return 1 << 30;
    if (d < -(double) (1 << 30)) return -(1 << 30);
    return (s32) d;
}

static int sFailures;

static void fail(const char *what, u32 a, u32 b) {
    if (sFailures++ < 10) {
        printf("  FAIL %s: %08x %08x\n", what, a, b);
    }
}

static void test_floor_ceil(u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        f32 f = rnd_f32();
        s32 fl = f32_floor_s32(f);
        s32 ce = f32_ceil_s32(f);
        s32 rf = ref_floor(f);
        s32 rc = -ref_floor(-f);
        /* Beyond 2^30 both sides clamp; only the s16 comparisons matter there. */
        if (fl != rf) fail("floor", f32_bits(f), (u32) fl);
        if (ce != rc) fail("ceil", f32_bits(f), (u32) ce);
    }
    printf("floor/ceil      %u floats\n", n);
}

static void test_yrange(u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        f32 y = (rnd() & 1) ? rnd_f32() : (f32) (s16) rnd() + bits_f32(0x3F000000 + (rnd() & 0x7FFFFF)) - 1.0f;
        s16 L = (s16) rnd();
        if ((y < L) != (f32_floor_s32(y) < L)) fail("y<L", f32_bits(y), (u32) L);
        if ((y > L) != (f32_ceil_s32(y) > L)) fail("y>U", f32_bits(y), (u32) L);
    }
    printf("y-range         %u (y, bound) pairs\n", n);
}

static void test_compares(u32 n) {
    u32 i;
    for (i = 0; i < n; i++) {
        f32 a = rnd_f32();
        f32 b = (rnd() & 3) ? rnd_f32() : bits_f32(f32_bits(a) + (rnd() % 5) - 2);
        if (((f32_bits(b) >> 23) & 0xFF) == 0xFF) continue;
        if ((a < b) != (f32_key(a) < f32_key(b))) fail("key<", f32_bits(a), f32_bits(b));
        if ((a > b) != (f32_key(a) > f32_key(b))) fail("key>", f32_bits(a), f32_bits(b));
        if ((a > 0.0f) != F32_GT0(a)) fail("gt0", f32_bits(a), 0);
        if ((a < 0.0f) != F32_LT0(a)) fail("lt0", f32_bits(a), 0);
    }
    printf("compares        %u pairs\n", n);
}

/* The original test, verbatim: TRUE if the wall is rejected by distance. */
static int float_rejects(struct Surface *surf, f32 x, f32 y, f32 z, f32 radius) {
    f32 offset = surf->normal.x * x + surf->normal.y * y + surf->normal.z * z + surf->originOffset;
    return offset < -radius || offset > radius;
}

static void test_plane(u32 perWall) {
    s32 lvl, cx, cz;
    u32 walls = 0, cases = 0, rejected = 0, closeCalls = 0;

    for (lvl = 0; lvl < gNumHarnessLevels; lvl++) {
        {
            static int pooled;
            if (!pooled) {
                alloc_surface_pools();
                pooled = 1;
            }
        }
        load_area_terrain(0, (TerrainData *) gHarnessLevels[lvl].collision, NULL, NULL);

        for (cz = 0; cz < NUM_CELLS; cz++) {
            for (cx = 0; cx < NUM_CELLS; cx++) {
                struct SurfaceNode *node = gStaticSurfacePartition[cz][cx][SPATIAL_PARTITION_WALLS].next;
                for (; node != NULL; node = node->next) {
                    struct Surface *s = node->surface;
                    u32 k;
                    walls++;
                    for (k = 0; k < perWall; k++) {
                        /* A point on the wall's plane near its first vertex... */
                        f32 radius = (rnd() & 1) ? (f32) (rnd() % 201) : bits_f32(0x41000000 + rnd() % 0x2800000);
                        f32 target, x, y, z, off;
                        s32 thr, xF, yF, zF;
                        if (radius > 200.0f) radius = 200.0f;
                        x = s->vertex1[0] + (f32) ((s32) (rnd() % 2001) - 1000) * 0.37f;
                        z = s->vertex1[2] + (f32) ((s32) (rnd() % 2001) - 1000) * 0.37f;
                        y = (f32) s->lowerY + (f32) (rnd() % (u32) (s->upperY - s->lowerY + 1))
                            + (f32) (rnd() & 1023) / 1024.0f;
                        /* ...pushed along the normal to just around the radius boundary. */
                        off = s->normal.x * x + s->normal.y * y + s->normal.z * z + s->originOffset;
                        target = ((rnd() & 1) ? 1.0f : -1.0f)
                                 * (radius + (f32) ((s32) (rnd() % 4001) - 2000) / 100.0f);
                        x += s->normal.x * (target - off);
                        z += s->normal.z * (target - off);
                        y += s->normal.y * (target - off);

                        if (x <= -8192.0f || x >= 8192.0f || z <= -8192.0f || z >= 8192.0f) continue;
                        yF = f32_floor_s32(y);
                        if (yF < s->lowerY || f32_ceil_s32(y) > s->upperY) continue;
                        xF = f32_floor_s32(x);
                        zF = f32_floor_s32(z);
                        thr = wall_plane_reject_threshold(radius);
                        cases++;
                        if (wall_plane_surely_beyond(s, xF, yF, zF, thr)) {
                            rejected++;
                            if (!float_rejects(s, x, y, z, radius)) {
                                fail("plane filter rejected a wall the float test keeps",
                                     f32_bits(x), f32_bits(radius));
                            }
                        } else if (float_rejects(s, x, y, z, radius)) {
                            closeCalls++; /* left to the float path: fine, just slower */
                        }
                    }
                }
            }
        }
    }
    printf("plane filter    %u walls, %u near-boundary cases, %u rejected early, "
           "%u passed on to the float test\n", walls, cases, rejected, closeCalls);
}

/* The original edge expression, evaluated in single precision exactly as in
 * find_wall_collisions_from_list (operands are integer-valued floats). */
static f32 float_edge(s32 yi, s32 wi, s32 yj, s32 wj, f32 y, f32 q) {
    f32 fyi = yi, fwi = wi, fyj = yj, fwj = wj;
    return (fyi - y) * (fwj - fwi) - (fwi - q) * (fyj - fyi);
}

/* Extended Euclid on positive a, b: returns g = gcd, with a*x + b*y = g. */
static s32 egcd(s32 a, s32 b, s64 *x, s64 *y) {
    s64 x0 = 1, y0 = 0, x1 = 0, y1 = 1;
    while (b != 0) {
        s32 qt = a / b, t = a - qt * b;
        s64 tx = x0 - qt * x1, ty = y0 - qt * y1;
        a = b; b = t;
        x0 = x1; y0 = y1; x1 = tx; y1 = ty;
    }
    *x = x0;
    *y = y0;
    return a;
}

static s32 clamp16(s32 v) {
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static void test_edges(u32 n) {
    u32 i, decided = 0, adversarialNear = 0;
    for (i = 0; i < n; i++) {
        s32 yi, wi, yj, wj, yF, qF, sign;
        f32 y, q, v;
        if (i & 1) {
            /* random: vertices anywhere, point near the edge's span */
            yi = (s16) rnd(); yj = clamp16(yi + (s32) (rnd() % 8001) - 4000);
            wi = (s16) rnd(); wj = clamp16(wi + (s32) (rnd() % 8001) - 4000);
            y = (f32) (yi + (s32) (rnd() % 6001) - 3000) + (f32) (rnd() & 0xFFFF) / 65536.0f;
            q = (f32) (wi + (s32) (rnd() % 6001) - 3000) + (f32) (rnd() & 0xFFFF) / 65536.0f;
            if (y > 32768.0f || y < -32768.0f) continue;
        } else {
            /* Adversarial. Solve a*d1 - c*d2 = E exactly (extended Euclid) with
             * E just above |d1|+|d2|, so the integer estimate barely clears its
             * floor-error bound, then pick fractional parts that cancel almost
             * all of E: the exact value R ends up around 1 while the operands
             * are ~10^8, where single-precision rounding is worth tens. y and q
             * sit near 0 so their fractions can be fine-grained. */
            s32 d1 = (s32) (rnd() % 30000) + 500, d2 = (s32) (rnd() % 30000) + 500;
            s32 target, g, k;
            s64 a, c, x0, y0;
            f32 fy, fq;
            if (rnd() & 1) d1 = -d1;
            if (rnd() & 1) d2 = -d2;
            g = egcd(d1 < 0 ? -d1 : d1, d2 < 0 ? -d2 : d2, &x0, &y0);
            if (g != 1) continue;
            target = (d1 < 0 ? -d1 : d1) + (d2 < 0 ? -d2 : d2) + 1 + (s32) (rnd() % 3);
            if (rnd() & 1) target = -target;
            /* |d1| x0 + |d2| y0 = 1  ->  a d1 - c d2 = target */
            a = x0 * target * (d1 < 0 ? -1 : 1);
            c = -y0 * target * (d2 < 0 ? -1 : 1);
            /* general solution (a + k d2, c + k d1); aim |a| around 15000 */
            k = (s32) ((((s64) (rnd() % 30000) - 15000) - a) / d2);
            a += (s64) k * d2;
            c += (s64) k * d1;
            if (a > 30000 || a < -30000 || c > 30000 || c < -30000) continue;
            yF = (s32) (rnd() % 64) - 32;
            qF = (s32) (rnd() % 64) - 32;
            yi = yF + (s32) a;
            wi = qF + (s32) c;
            yj = yi + d2;
            wj = wi + d1;
            if (yi != clamp16(yi) || wi != clamp16(wi) || yj != clamp16(yj) || wj != clamp16(wj)) continue;
            /* R = E - fy*d1 + fq*d2: push both fractions toward the side that
             * shrinks |R|, leaving ~1 */
            fy = 1.0f - (f32) (rnd() % 64 + 1) / 4194304.0f;
            fq = 1.0f - (f32) (rnd() % 64 + 1) / 4194304.0f;
            if ((target > 0) != (d1 > 0)) fy = 1.0f - fy;
            if ((target > 0) != (d2 < 0)) fq = 1.0f - fq;
            y = (f32) yF + fy;
            q = (f32) qF + fq;
            if (f32_floor_s32(y) != yF || f32_floor_s32(q) != qF) continue;
            adversarialNear++;
        }
        yF = f32_floor_s32(y);
        qF = f32_floor_s32(q);
        sign = wall_edge_sign(yi, wi, yj, wj, yF, qF);
        if (sign == 0) continue;
        decided++;
        v = float_edge(yi, wi, yj, wj, y, q);
        if ((sign > 0 && !(v > 0.0f)) || (sign < 0 && !(v < 0.0f))) {
            fail("edge sign decided wrongly", f32_bits(y), f32_bits(q));
        }
    }
    printf("edges           %u cases (%u adversarial), %u decided in integers\n", n,
           adversarialNear, decided);
}

/* f32_to_s32_libgcc against the real __aeabi_f2iz. Only meaningful on ARM:
 * on x86 the reference cast is cvttss2si, which differs above 2^31. Sweeps
 * the whole 32-bit pattern space with a prime stride, plus every exponent
 * with boundary mantissas, NaNs and infinities. */
static void test_f2iz(u32 stride) {
#ifdef __arm__
    u32 n = 0, b, e;
    volatile f32 vf;
    for (b = 0; b < 0xFFFFFFFFu - stride; b += stride, n++) {
        vf = bits_f32(b);
        if (f32_to_s32_libgcc(vf) != (s32) vf) fail("f2iz", b, (u32) f32_to_s32_libgcc(vf));
    }
    for (e = 0; e < 512; e++) {
        static const u32 mant[] = { 0, 1, 0x400000, 0x7FFFFE, 0x7FFFFF };
        u32 k;
        for (k = 0; k < 5; k++, n++) {
            b = (e << 23) | mant[k];
            vf = bits_f32(b);
            if (f32_to_s32_libgcc(vf) != (s32) vf) fail("f2iz", b, (u32) f32_to_s32_libgcc(vf));
        }
    }
    printf("f2iz            %u bit patterns vs __aeabi_f2iz\n", n);
#else
    (void) stride;
#endif
}

#ifndef PROPS_SCALE
#define PROPS_SCALE 1
#endif

int main(void) {
#ifdef __arm__
    test_f2iz(97);
#endif
    test_edges(8000000 / PROPS_SCALE);
    test_floor_ceil(4000000 / PROPS_SCALE);
    test_yrange(4000000 / PROPS_SCALE);
    test_compares(4000000 / PROPS_SCALE);
    test_plane(400 / PROPS_SCALE);
    printf("%s (%d failures)\n", sFailures ? "FAIL" : "PASS", sFailures);
    return sFailures != 0;
}
