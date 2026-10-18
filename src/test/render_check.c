/*
 * The demo's renderer (demo/render_iw.c, painter's algorithm) run on the
 * host and checked against a z-buffered reference of the same mesh, same
 * camera math and same projection.
 *
 *   render_check
 *       For every pit found by demo/render_build.c (a floor closed in by walls
 *       rising to a rim: BoB's cannon shaft and the like), renders views from
 *       all around it and above its rim, from cameras in open air with the
 *       opening in sight, and counts pixels (away from triangle edges) where
 *       the pit's triangles show although the reference has something else in
 *       front ("bleeding": the pit drawn over the ground around it), and
 *       pixels where the reference shows the pit but the painter's image
 *       doesn't ("missing"). Run once with the pit handling, which must stay
 *       under one wrong pixel per view on average, and once without it, which
 *       must fail.
 *
 *   render_check view <level> <x> <y> <z> <yaw> <pitch> <width> <out.ppm>
 *       Renders one view (a camera from the demo's "f" log lines: b e g i j,
 *       width 160/144/128/112/96) to a PPM, the demo's renderer on the left
 *       and the reference on the right, and lists the triangles that won the
 *       most pixels. "view0" does the same without the pit handling.
 *
 * The GBA registers the renderer reads (timers) and the palette are mapped
 * at their GBA addresses.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "engine/surface_collision.h"
#include "engine/surface_load.h"
#include "game/object_list_processor.h"
#include "levels.h"
#include "render.h"

int printf(const char *fmt, ...);
void *fopen(const char *path, const char *mode);
unsigned int fwrite(const void *p, unsigned int size, unsigned int n, void *f);
int fclose(void *f);
void *mmap(void *addr, unsigned int len, int prot, int flags, int fd, int off);

#define PROT_RW 3
#define MAP_PRIVATE_ANON_FIXED_NOREPLACE (0x02 | 0x20 | 0x100000)

struct MarioState gMarioStates[1];
extern const s16 gSinQ14[4096];

/* demo/irq.s */
void fill_words(u32 *dst, u32 value, s32 count) {
    while (count-- > 0) {
        *dst++ = value;
    }
}

static const struct RLevelStyle kStyle = {
    { { 96, 176, 64 }, { 132, 150, 70 }, { 150, 118, 86 }, { 186, 186, 200 }, { 196, 182, 140 },
      { 220, 96, 40 }, { 86, 72, 66 } },
    { 64, 120, 232 }, { 176, 216, 248 }
};
static const s32 kHeights[] = { 106, 106, 96, 85, 74, 64 };

static u16 sPage[240 * 160 / 2];
#define PAGE ((u8 *) sPage)
#define W_MAX 160
#define H_MAX 108
static float sZ[H_MAX][W_MAX];
static u16 sRefTri[H_MAX][W_MAX];
#define NO_TRI 0xFFFF

static s32 isin(u16 a) { return gSinQ14[a >> 4]; }
static s32 icos(u16 a) { return gSinQ14[(u16) (a + 0x4000) >> 4]; }

/* ---------- reference: z-buffer, same camera and projection ---------- */

struct View {
    s32 x, y, z;
    u16 yaw, pitch;
    s32 w, h;
};

static void to_camera(const struct View *v, s32 x, s32 y, s32 z, float *o) {
    s32 sy = isin(v->yaw), cy = icos(v->yaw), sp = isin(v->pitch), cp = icos(v->pitch);
    s32 dx = x - v->x, dy = y - v->y, dz = z - v->z;
    s32 zf = -((dx * sy + dz * cy) >> 14);
    o[0] = (float) ((dx * cy - dz * sy) >> 14);
    o[2] = (float) ((zf * cp - dy * sp) >> 14);
    o[1] = (float) ((zf * sp + dy * cp) >> 14);
}

static void ref_polygon(const struct View *v, float (*p)[3], s32 n, u16 id) {
    float fs = (float) (v->w * 40 / 3) / 16.0f, sx[8], sy[8], iz[8];
    s32 i, k;
    for (i = 0; i < n; i++) {
        sx[i] = (float) v->w / 2.0f + p[i][0] / p[i][2] * fs;
        sy[i] = (float) v->h / 2.0f - p[i][1] / p[i][2] * fs;
        iz[i] = 1.0f / p[i][2];
    }
    for (k = 1; k + 1 < n; k++) {
        s32 a = 0, b = k, c = k + 1, x, y, x0, x1, y0, y1;
        float area = (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sx[c] - sx[a]) * (sy[b] - sy[a]);
        float mnx = sx[a], mxx = sx[a], mny = sy[a], mxy = sy[a];
        if (area == 0.0f) continue;
        if (sx[b] < mnx) mnx = sx[b];
        if (sx[c] < mnx) mnx = sx[c];
        if (sx[b] > mxx) mxx = sx[b];
        if (sx[c] > mxx) mxx = sx[c];
        if (sy[b] < mny) mny = sy[b];
        if (sy[c] < mny) mny = sy[c];
        if (sy[b] > mxy) mxy = sy[b];
        if (sy[c] > mxy) mxy = sy[c];
        x0 = mnx < 0.0f ? 0 : (s32) mnx;
        y0 = mny < 0.0f ? 0 : (s32) mny;
        x1 = mxx > (float) (v->w - 1) ? v->w - 1 : (s32) mxx;
        y1 = mxy > (float) (v->h - 1) ? v->h - 1 : (s32) mxy;
        for (y = y0; y <= y1; y++) {
            for (x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float wa = ((sx[b] - px) * (sy[c] - py) - (sx[c] - px) * (sy[b] - py)) / area;
                float wb = ((sx[c] - px) * (sy[a] - py) - (sx[a] - px) * (sy[c] - py)) / area;
                float wc = 1.0f - wa - wb, z;
                if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
                z = wa * iz[a] + wb * iz[b] + wc * iz[c];
                if (z > sZ[y][x]) {
                    sZ[y][x] = z;
                    sRefTri[y][x] = id;
                }
            }
        }
    }
}

static void ref_render(const struct View *v) {
    s32 i, k, x, y;
    for (y = 0; y < v->h; y++) {
        for (x = 0; x < v->w; x++) {
            sZ[y][x] = 0.0f;
            sRefTri[y][x] = NO_TRI;
        }
    }
    for (i = 0; i < gRMesh.numTris; i++) {
        const struct RTri *t = &gRMesh.tris[i];
        float in[3][3], out[8][3];
        s32 n = 0;
        if (t->n[0] * v->x + t->n[1] * v->y + t->n[2] * v->z + t->d <= 0) continue;
        for (k = 0; k < 3; k++) {
            const struct RVert *rv = &gRMesh.verts[t->v[k]];
            to_camera(v, rv->x, rv->y, rv->z, in[k]);
        }
        /* near plane z = 32, as the renderer */
        for (k = 0; k < 3; k++) {
            const float *a = in[k], *b = in[k == 2 ? 0 : k + 1];
            float da = a[2] - 32.0f, db = b[2] - 32.0f;
            if (da >= 0.0f) {
                out[n][0] = a[0]; out[n][1] = a[1]; out[n][2] = a[2];
                n++;
            }
            if ((da >= 0.0f) != (db >= 0.0f)) {
                float f = da / (da - db);
                out[n][0] = a[0] + (b[0] - a[0]) * f;
                out[n][1] = a[1] + (b[1] - a[1]) * f;
                out[n][2] = 32.0f;
                n++;
            }
        }
        if (n >= 3) ref_polygon(v, out, n, (u16) i);
    }
}

/* ---------- the demo's renderer ---------- */

static void game_render(const struct View *v) {
    struct RCamera cam;
    struct RPlayer player;
    struct RQuality q;
    cam.x = v->x;
    cam.y = v->y;
    cam.z = v->z;
    cam.yaw = v->yaw;
    cam.pitch = v->pitch;
    player.visible = 0;
    player.frames = NULL;
    player.x = player.y = player.z = 0;
    q.width = v->w;
    q.height = v->h;
    q.farDist = 8000;
    q.groundColor = 0xDC;
    render_frame(sPage, &cam, &player, &q);
}

/* Which triangle won each pixel of the painter's image: colors encode
 * triangle indices, 50 per pass (fog adds 0..3 to a color). */
static u16 sGameTri[H_MAX][W_MAX];

static void game_render_ids(const struct View *v) {
    static u8 saved[R_MAX_TRIS];
    s32 i, x, y, base;
    for (i = 0; i < gRMesh.numTris; i++) saved[i] = gRMesh.tris[i].color;
    for (y = 0; y < v->h; y++) {
        for (x = 0; x < v->w; x++) sGameTri[y][x] = NO_TRI;
    }
    for (base = 0; base < gRMesh.numTris; base += 50) {
        for (i = 0; i < gRMesh.numTris; i++) {
            gRMesh.tris[i].color = (u8) (i >= base && i < base + 50 ? 4 * (i - base + 1) : 0xD0);
        }
        game_render(v);
        for (y = 0; y < v->h; y++) {
            for (x = 0; x < v->w; x++) {
                u8 c = PAGE[y * 240 + x];
                if (c >= 4 && c < 204) sGameTri[y][x] = (u16) (base + c / 4 - 1);
            }
        }
    }
    for (i = 0; i < gRMesh.numTris; i++) gRMesh.tris[i].color = saved[i];
}

/* ---------- views ---------- */

static void look_at(struct View *v, s32 tx, s32 ty, s32 tz) {
    s32 dx = tx - v->x, dy = ty - v->y, dz = tz - v->z, a, best = -0x7FFFFFFF;
    s64 h2 = (s64) dx * dx + (s64) dz * dz;
    s32 hd = 0;
    while ((s64) (hd + 1) * (hd + 1) <= h2) hd++;
    for (a = 0; a < 0x10000; a += 16) {
        s32 f = -(dx * isin((u16) a) + dz * icos((u16) a));
        if (f > best) {
            best = f;
            v->yaw = (u16) a;
        }
    }
    best = -0x7FFFFFFF;
    for (a = -0x3FF0; a < 0x4000; a += 16) {
        s32 f = hd * icos((u16) a) - dy * isin((u16) a);
        if (f > best) {
            best = f;
            v->pitch = (u16) a;
        }
    }
}

/* Pit membership from the build (tags are cleared to render without the
 * pit handling). */
static u8 sMember[R_MAX_TRIS];
static struct RPit sPits[R_MAX_PITS];
static s32 sNumPits;

static s32 tri_pit(u16 t) {
    return t != NO_TRI && sMember[t];
}

/* Pixels where the painter's image and the reference disagree about the
 * pit, away from the reference's triangle edges (rasterization differs by a
 * pixel there). */
static void compare(const struct View *v, u32 *bleed, u32 *missing) {
    s32 x, y, dx, dy;
    for (y = 1; y + 1 < v->h; y++) {
        for (x = 1; x + 1 < v->w; x++) {
            u16 r = sRefTri[y][x];
            s32 interior = 1;
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    if (sRefTri[y + dy][x + dx] != r) interior = 0;
                }
            }
            if (!interior) continue;
            if (tri_pit(sGameTri[y][x]) && !tri_pit(r)) (*bleed)++;
            if (!tri_pit(sGameTri[y][x]) && tri_pit(r)) (*missing)++;
        }
    }
}

static void load_level(s32 l, s32 handling) {
    s32 i;
    load_area_terrain(0, (TerrainData *) gHarnessLevels[l].collision, NULL, NULL);
    render_build_level(&kStyle, gHarnessLevels[l].terrainType);
    sNumPits = gRMesh.numPits;
    for (i = 0; i < sNumPits; i++) sPits[i] = gRMesh.pits[i];
    for (i = 0; i < gRMesh.numTris; i++) {
        sMember[i] = (gRMesh.tris[i].flags >> R_TRI_PIT_SHIFT) != 0;
        if (!handling) gRMesh.tris[i].flags &= (1 << R_TRI_PIT_SHIFT) - 1;
    }
    if (!handling) gRMesh.numPits = 0;
}

/* Does the segment from p to q cross any collision surface? (Cameras must be
 * in open air with the pit's opening in sight, as the demo's camera, which
 * the game's collision keeps out of walls.) */
static s32 blocked(const float *p, const float *q) {
    s32 i, k;
    float d[3];
    for (k = 0; k < 3; k++) d[k] = q[k] - p[k];
    for (i = 0; i < gSurfacesAllocated; i++) {
        const struct Surface *s = &sSurfacePool[i];
        float a[3], e1[3], e2[3], h[3], sv[3], qv[3], det, u, w, t;
        for (k = 0; k < 3; k++) {
            a[k] = s->vertex1[k];
            e1[k] = s->vertex2[k] - a[k];
            e2[k] = s->vertex3[k] - a[k];
        }
        h[0] = d[1] * e2[2] - d[2] * e2[1];
        h[1] = d[2] * e2[0] - d[0] * e2[2];
        h[2] = d[0] * e2[1] - d[1] * e2[0];
        det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
        if (det > -1e-6f && det < 1e-6f) continue;
        for (k = 0; k < 3; k++) sv[k] = p[k] - a[k];
        u = (sv[0] * h[0] + sv[1] * h[1] + sv[2] * h[2]) / det;
        if (u < 0.0f || u > 1.0f) continue;
        qv[0] = sv[1] * e1[2] - sv[2] * e1[1];
        qv[1] = sv[2] * e1[0] - sv[0] * e1[2];
        qv[2] = sv[0] * e1[1] - sv[1] * e1[0];
        w = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
        if (w < 0.0f || u + w > 1.0f) continue;
        t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
        if (t > 0.0f && t < 1.0f) return 1;
    }
    return 0;
}

static struct View sWorst;
static s32 sLevel, sWorstLevel;

/* Views around every pit of the loaded level; returns the views tested. */
static u32 check_pits(u32 *bleed, u32 *missing, u32 *worst) {
    static const s32 kDist[] = { 250, 500, 900, 1500, 2400 };
    static const s32 kHeight[] = { 30, 60, 120, 200, 450, 900, 1600 };
    u32 views = 0;
    s32 p, d, h, a;
    for (p = 0; p < sNumPits; p++) {
        const struct RPit *pit = &sPits[p];
        for (d = 0; d < 5; d++) {
            for (h = 0; h < 7; h++) {
                for (a = 0; a < 16; a++) {
                    struct View v;
                    struct Surface *floor;
                    f32 fy;
                    u32 b = 0, m = 0;
                    v.x = pit->cx + ((kDist[d] * isin((u16) (a * 0x1000 + 0x300))) >> 14);
                    v.z = pit->cz + ((kDist[d] * icos((u16) (a * 0x1000 + 0x300))) >> 14);
                    v.y = pit->rimY + kHeight[h];
                    /* only cameras over ground and in open air with the
                     * opening in sight, like the demo's camera */
                    fy = find_floor((f32) v.x, (f32) v.y, (f32) v.z, &floor);
                    if (floor == NULL || fy > (f32) (v.y - 40)) continue;
                    {
                        float from[3] = { (float) v.x, (float) v.y, (float) v.z };
                        float to[3] = { (float) pit->cx, (float) pit->rimY + 8.0f, (float) pit->cz };
                        if (blocked(from, to)) continue;
                    }
                    v.w = 160;
                    v.h = 106;
                    look_at(&v, pit->cx, pit->rimY, pit->cz);
                    ref_render(&v);
                    game_render_ids(&v);
                    compare(&v, &b, &m);
                    *bleed += b;
                    *missing += m;
                    if (b + m > *worst) {
                        *worst = b + m;
                        sWorst = v;
                        sWorstLevel = sLevel;
                    }
                    views++;
                }
            }
        }
    }
    return views;
}

static void map_gba(void) {
    if (mmap((void *) 0x04000000, 0x1000, PROT_RW, MAP_PRIVATE_ANON_FIXED_NOREPLACE, -1, 0) != (void *) 0x04000000
        || mmap((void *) 0x05000000, 0x1000, PROT_RW, MAP_PRIVATE_ANON_FIXED_NOREPLACE, -1, 0) != (void *) 0x05000000) {
        printf("can't map the GBA I/O and palette addresses\n");
        *(volatile int *) 0 = 0;
    }
}

static s32 parse(const char *s) {
    s32 v = 0, neg = *s == '-';
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
    return neg ? -v : v;
}

static void view(char **argv) {
    struct View v;
    static u32 count[R_MAX_TRIS];
    void *f;
    s32 x, y, i;
    load_level(parse(argv[2]), argv[1][4] != '0');
    v.x = parse(argv[3]);
    v.y = parse(argv[4]);
    v.z = parse(argv[5]);
    v.yaw = (u16) parse(argv[6]);
    v.pitch = (u16) parse(argv[7]);
    v.w = parse(argv[8]);
    v.h = kHeights[(160 - v.w) / 16];
    render_set_palette(&kStyle);
    game_render_ids(&v);
    ref_render(&v);
    game_render(&v);
    f = fopen(argv[9], "wb");
    {
        static char hdr[] = "P6\n320 108\n255\n";
        static u8 rgb[2 * 160 * 3];
        hdr[3] = (char) ('0' + 2 * v.w / 100);
        hdr[4] = (char) ('0' + 2 * v.w / 10 % 10);
        hdr[5] = (char) ('0' + 2 * v.w % 10);
        hdr[7] = (char) ('0' + v.h / 100);
        hdr[8] = (char) ('0' + v.h / 10 % 10);
        hdr[9] = (char) ('0' + v.h % 10);
        fwrite(hdr, 1, sizeof(hdr) - 1, f);
        for (y = 0; y < v.h; y++) {
            for (x = 0; x < 2 * v.w; x++) {
                /* left: the demo's renderer; right: the z-buffered reference */
                u32 idx = PAGE[y * 240 + x % v.w];
                u16 c;
                if (x >= v.w && sRefTri[y][x - v.w] != NO_TRI) {
                    const struct RTri *t = &gRMesh.tris[sRefTri[y][x - v.w]];
                    float zs[3];
                    s32 k, depth = 0, fog;
                    for (k = 0; k < 3; k++) {
                        const struct RVert *rv = &gRMesh.verts[t->v[k]];
                        to_camera(&v, rv->x, rv->y, rv->z, zs);
                        depth += (s32) zs[2];
                    }
                    fog = ((depth * ((6 << 16) / (3 * 8000))) >> 16) - 2;
                    idx = t->color + (fog < 0 ? 0 : fog > 3 ? 3 : fog);
                }
                c = PAL_BG[idx];
                rgb[3 * x] = (u8) ((c & 31) << 3);
                rgb[3 * x + 1] = (u8) (((c >> 5) & 31) << 3);
                rgb[3 * x + 2] = (u8) (((c >> 10) & 31) << 3);
            }
            fwrite(rgb, 1, (unsigned int) (6 * v.w), f);
        }
    }
    fclose(f);
    for (y = 0; y < v.h; y++) {
        for (x = 0; x < v.w; x++) {
            if (sGameTri[y][x] != NO_TRI) count[sGameTri[y][x]]++;
        }
    }
    printf("pits in level: %d\n", gRMesh.numPits);
    for (i = 0; i < gRMesh.numTris; i++) {
        const struct RTri *t = &gRMesh.tris[i];
        const struct RVert *a = &gRMesh.verts[t->v[0]], *b = &gRMesh.verts[t->v[1]], *c = &gRMesh.verts[t->v[2]];
        if (count[i] < 40) continue;
        printf("tri %4d  %5u px  material %d light %d  pit %d  (%d %d %d) (%d %d %d) (%d %d %d)\n", i, count[i],
               t->color >> 5, (t->color >> 2) & 7, t->flags >> R_TRI_PIT_SHIFT, a->x, a->y, a->z, b->x, b->y,
               b->z, c->x, c->y, c->z);
    }
}

int main(int argc, char **argv) {
    s32 l, pass;
    u32 failed = 0;

    map_gba();
    alloc_surface_pools();
    gCurrentObject = NULL;
    if (argc == 10 && argv[1][0] == 'v') {
        view(argv);
        return 0;
    }
    for (pass = 0; pass < 2; pass++) {
        u32 views = 0, bleed = 0, missing = 0, worst = 0;
        for (l = 0; l < gNumHarnessLevels; l++) {
            sLevel = l;
            load_level(l, pass == 0);
            if (pass == 0) printf("%-6s %d pits\n", gHarnessLevels[l].name, sNumPits);
            views += check_pits(&bleed, &missing, &worst);
        }
        printf("%s %u views, %u bleeding pixels, %u missing pixels, worst view %u\n",
               pass == 0 ? "with pit handling:   " : "without (must fail): ", views, bleed, missing, worst);
        printf("  worst view: level %d, camera %d %d %d, yaw %d pitch %d\n", sWorstLevel, sWorst.x, sWorst.y, sWorst.z,
               sWorst.yaw, sWorst.pitch);
        if (pass == 0) failed |= bleed + missing > views; /* under 1 pixel per view */
        else failed |= bleed < views;
    }
    printf(failed ? "FAIL\n" : "PASS\n");
    return failed ? 1 : 0;
}
