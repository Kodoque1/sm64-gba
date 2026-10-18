/*
 * The player as an animated mannequin: Mario's skeleton (joint hierarchy and
 * offsets from the decomp's mario_geo_body, see tools/gen_skeleton.py) posed
 * every frame by the game's own animation data at the frame the physics just
 * advanced, with each limb an ellipsoid drawn by one affine hardware sprite.
 *
 * Why ellipsoids: seen from any direction, an ellipsoid's outline is an
 * ellipse (exactly, under the weak-perspective approximation used per limb),
 * and an ellipse is a circle under a 2x2 linear map, which is exactly what
 * the GBA's affine sprites apply. One shaded-ball sprite gives every limb a
 * correct silhouette from any camera angle, drawn by the hardware at full
 * 240x160 resolution while the world renders at a lower one.
 *
 * The animation evaluation follows geo_set_animation_globals and
 * geo_process_animated_part (rendering_graph_node.c) and the matrix
 * conventions of mtxf_rotate_xyz_and_translate / mtxf_rotate_zxy_and_translate
 * (row vectors, child world = local * parent world), in fixed point.
 * Cosmetic GEO_ASM nodes (torso tilt, head turn, punch scaling) are skipped.
 *
 * The shapes and colors are a neutral wooden mannequin, not the character.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "game/level_update.h"
#include "render.h"
#include "mannequin.h"

extern const s16 gSinQ14[4096];
extern const u32 gRecipZ[16384];     /* 2^24 / z */
extern const u32 gRecipClip[32768];  /* 2^30 / d */

/* Hot code as ARM in IWRAM (budget checked with the stack high-water mark). */
#ifdef MANNEQUIN_TEST
#define FAST
#else
#define FAST __attribute__((section(".iwram"), noinline)) /* file is built -marm */
#endif
extern const signed char gSkelParent[20];
extern const short gSkelOffset[20][3];

#define NUM_PARTS 20
#define BALL_TILE 576       /* OBJ tile index of the 32x32 4bpp ball (after the HUD font) */
#define OAM_FIRST 48        /* OAM slots after the HUD's */
#define BALL_R 15           /* ball radius in texture pixels */
#define FOCAL 200           /* full-res focal length: 120 / tan(31 deg) as in the renderer */

/* Limb shapes in each part's local frame (model units; the bone to the child
 * joint runs along +x). radius < 0: no shape (joints with zero-length bones). */
struct Shape {
    s16 cx, cy, cz;
    s16 rx, ry, rz;
    u8 material; /* 0 limb wood, 1 joint wood (hands, feet), 2 head */
};

static const struct Shape kShapes[NUM_PARTS] = {
    /*  0 root      */ { 0, 0, 0, -1, -1, -1, 0 },
    /*  1 pelvis    */ { 18, -4, 0, 44, 48, 62, 0 },
    /*  2 torso     */ { 46, 0, 0, 62, 54, 60, 0 },
    /*  3 head      */ { 56, 0, 0, 56, 54, 54, 2 },
    /*  4 shoulder  */ { 0, 0, 0, -1, -1, -1, 0 },
    /*  5 upper arm */ { 32, 0, 0, 44, 26, 26, 0 },
    /*  6 forearm   */ { 30, 0, 0, 40, 24, 24, 0 },
    /*  7 hand      */ { 18, 0, 0, 28, 26, 26, 1 },
    /*  8 shoulder  */ { 0, 0, 0, -1, -1, -1, 0 },
    /*  9 upper arm */ { 32, 0, 0, 44, 26, 26, 0 },
    /* 10 forearm   */ { 30, 0, 0, 40, 24, 24, 0 },
    /* 11 hand      */ { 18, 0, 0, 28, 26, 26, 1 },
    /* 12 hip       */ { 0, 0, 0, -1, -1, -1, 0 },
    /* 13 thigh     */ { 44, 0, 0, 56, 32, 32, 0 },
    /* 14 shin      */ { 34, 0, 0, 44, 28, 28, 0 },
    /* 15 foot      */ { 32, 8, 0, 54, 26, 32, 1 },
    /* 16 hip       */ { 0, 0, 0, -1, -1, -1, 0 },
    /* 17 thigh     */ { 44, 0, 0, 56, 32, 32, 0 },
    /* 18 shin      */ { 34, 0, 0, 44, 28, 28, 0 },
    /* 19 foot      */ { 32, 8, 0, 54, 26, 32, 1 },
};

/* Fixed-point joint frame (struct RFrame): rotation rows in Q14
 * (row-vector convention), origin in world units * 16. */
#define Frame RFrame

static inline s32 qsin(s32 a) {
    return gSinQ14[((u32) a & 0xFFFF) >> 4];
}

static inline s32 qcos(s32 a) {
    return gSinQ14[((u32) (a + 0x4000) & 0xFFFF) >> 4];
}

/* mtxf_rotate_xyz_and_translate's rotation part, Q14 */
static void rot_xyz(s32 r[3][3], s32 ax, s32 ay, s32 az) {
    s32 sx = qsin(ax), cx = qcos(ax), sy = qsin(ay), cy = qcos(ay), sz = qsin(az), cz = qcos(az);
    s32 sxsy = (sx * sy) >> 14, cxsy = (cx * sy) >> 14;
    r[0][0] = (cy * cz) >> 14;
    r[0][1] = (cy * sz) >> 14;
    r[0][2] = -sy;
    r[1][0] = ((sxsy * cz) >> 14) - ((cx * sz) >> 14);
    r[1][1] = ((sxsy * sz) >> 14) + ((cx * cz) >> 14);
    r[1][2] = (sx * cy) >> 14;
    r[2][0] = ((cxsy * cz) >> 14) + ((sx * sz) >> 14);
    r[2][1] = ((cxsy * sz) >> 14) - ((sx * cz) >> 14);
    r[2][2] = (cx * cy) >> 14;
}

/* mtxf_rotate_zxy_and_translate's rotation part, Q14 (the object node) */
static void rot_zxy(s32 r[3][3], s32 ax, s32 ay, s32 az) {
    s32 sx = qsin(ax), cx = qcos(ax), sy = qsin(ay), cy = qcos(ay), sz = qsin(az), cz = qcos(az);
    s32 sxsy = (sx * sy) >> 14, sxcy = (sx * cy) >> 14;
    r[0][0] = ((cy * cz) >> 14) + ((sxsy * sz) >> 14);
    r[1][0] = -((cy * sz) >> 14) + ((sxsy * cz) >> 14);
    r[2][0] = (cx * sy) >> 14;
    r[0][1] = (cx * sz) >> 14;
    r[1][1] = (cx * cz) >> 14;
    r[2][1] = -sx;
    r[0][2] = -((sy * cz) >> 14) + ((sxcy * sz) >> 14);
    r[1][2] = ((sy * sz) >> 14) + ((sxcy * cz) >> 14);
    r[2][2] = (cx * cy) >> 14;
}

/* child = local * parent: rotation rows and the local offset (model units). */
static void compose(struct Frame *out, const s32 lr[3][3], const s32 lt[3], const struct Frame *p) {
    s32 i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            out->r[i][j] = (lr[i][0] * p->r[0][j] + lr[i][1] * p->r[1][j] + lr[i][2] * p->r[2][j]) >> 14;
        }
    }
    /* model units -> world * 16: * 0.25 (GEO_SCALE 16384) * 16 = * 4 */
    for (j = 0; j < 3; j++) {
        out->t[j] = p->t[j] + ((lt[0] * 4 * p->r[0][j] + lt[1] * 4 * p->r[1][j] + lt[2] * 4 * p->r[2][j]) >> 14);
    }
}

/* retrieve_animation_index (graph_node.c) */
static s32 anim_index(s32 frame, const u16 **attr) {
    s32 r = frame < (*attr)[0] ? (*attr)[1] + frame : (*attr)[1] + (*attr)[0] - 1;
    *attr += 2;
    return r;
}

#ifdef MANNEQUIN_TEST
#define TESTABLE
#else
#define TESTABLE static
#endif
TESTABLE struct Frame sFrames[NUM_PARTS];

/* Pose the skeleton for Mario's current animation frame. Returns 0 if there
 * is no animation yet. */
FAST TESTABLE s32 pose(struct Object *o) {
    struct AnimInfo *ai = &o->header.gfx.animInfo;
    struct Animation *anim = ai->curAnim;
    const u16 *attr;
    const s16 *values;
    s32 frame, type, mulQ8, part;
    struct Frame obj;
    s32 zero[3] = { 0, 0, 0 };

    if (anim == NULL) return 0;
    attr = (const u16 *) anim->index;
    values = (const s16 *) anim->values;
    frame = ai->animFrame;

    /* geo_set_animation_globals */
    if (anim->flags & ANIM_FLAG_HOR_TRANS) type = 2;      /* vertical translation only */
    else if (anim->flags & ANIM_FLAG_VERT_TRANS) type = 1; /* lateral translation only */
    else if (anim->flags & ANIM_FLAG_6) type = 3;          /* no translation */
    else type = 0;                                         /* full translation */
    mulQ8 = anim->animYTransDivisor == 0 ? 256 : (ai->animYTrans * 256) / anim->animYTransDivisor;

    /* object node: gfx position and angle, as set by Mario's actions */
    rot_zxy(obj.r, o->header.gfx.angle[0], o->header.gfx.angle[1], o->header.gfx.angle[2]);
    obj.t[0] = (s32) (o->header.gfx.pos[0] * 16.0f);
    obj.t[1] = (s32) (o->header.gfx.pos[1] * 16.0f);
    obj.t[2] = (s32) (o->header.gfx.pos[2] * 16.0f);

    for (part = 0; part < NUM_PARTS; part++) {
        s32 lt[3], lr[3][3], rot[3] = { 0, 0, 0 };
        lt[0] = gSkelOffset[part][0] * 256; /* model units, Q8 */
        lt[1] = gSkelOffset[part][1] * 256;
        lt[2] = gSkelOffset[part][2] * 256;
        if (part == 0) {
            /* geo_process_animated_part: the first part takes the translation */
            if (type == 0) {
                lt[0] += values[anim_index(frame, &attr)] * mulQ8;
                lt[1] += values[anim_index(frame, &attr)] * mulQ8;
                lt[2] += values[anim_index(frame, &attr)] * mulQ8;
            } else if (type == 1) {
                lt[0] += values[anim_index(frame, &attr)] * mulQ8;
                attr += 2;
                lt[2] += values[anim_index(frame, &attr)] * mulQ8;
            } else if (type == 2) {
                attr += 2;
                lt[1] += values[anim_index(frame, &attr)] * mulQ8;
                attr += 2;
            } else {
                attr += 6;
            }
        }
        rot[0] = values[anim_index(frame, &attr)];
        rot[1] = values[anim_index(frame, &attr)];
        rot[2] = values[anim_index(frame, &attr)];
        rot_xyz(lr, rot[0], rot[1], rot[2]);
        /* offsets are Q8 model units: compose() expects model units, so
         * fold the >> 8 in by scaling the parent's rotation contribution */
        lt[0] >>= 8;
        lt[1] >>= 8;
        lt[2] >>= 8;
        compose(&sFrames[part], lr, lt, part == 0 ? &obj : &sFrames[gSkelParent[part]]);
    }
    (void) zero;
    return 1;
}

/* ---------- sprites ---------- */

struct Limb {
    s32 depth; /* camera z, world units * 16 */
    s16 x, y;  /* screen center */
    s16 pa, pb, pc, pd;
    u8 bank;
};

static struct Limb sLimbs[NUM_PARTS];
static u16 sOam[NUM_PARTS][3];
static s16 sAffine[NUM_PARTS][4];
static s32 sNumLimbs;

/* Ellipses get at least this radius (pixels, squared, Q8): adding r^2 I to
 * the shape matrix dilates the ellipse by a disc of radius r, so thin limbs
 * far away stay 2-3 pixels wide instead of sampling between texels. */
#define MIN_RADIUS_Q8 ((13 * 13 * 256) / 100) /* 1.3 px */

static inline u32 isqrt32(u32 v) {
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* Isqrt for 64-bit values (floor), used once at init. */
static u32 isqrt64(u64 v) {
    u64 r = 0, b = (u64) 1 << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return (u32) r;
}

const struct RFrame *mannequin_frames(void) {
    return sFrames;
}

/* Pose only (the imported model is drawn by the renderer). */
FAST s32 mannequin_pose(struct Object *marioObj) {
    return pose(marioObj);
}

void mannequin_init(void) {
    volatile u16 *tiles = (volatile u16 *) (0x06010000 + BALL_TILE * 32);
    s32 x, y, i, m, d;
    /* 32x32 4bpp ball: radial shading, light from the upper left and front */
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x += 4) {
            u16 nib[4];
            for (i = 0; i < 4; i++) {
                s32 px = x + i, dx = px * 2 - 31, dy = y * 2 - 31; /* half-pixel units */
                s32 d2 = dx * dx + dy * dy, r2 = (BALL_R * 2) * (BALL_R * 2);
                if (d2 > r2) {
                    nib[i] = 0;
                } else {
                    /* radially symmetric shading (the affine matrix may rotate
                     * the texture): bright center, darker rim, like a sphere
                     * lit from the viewer */
                    s32 nz = isqrt64((u64) ((r2 - d2) * 256)); /* 0..16 * 2R */
                    s32 shade = 4 + nz * 11 / (16 * 2 * BALL_R);
                    nib[i] = (u16) (shade < 1 ? 1 : shade > 15 ? 15 : shade);
                }
            }
            /* 1D tile layout: tile (x/8, y/8), row y%8, 4 bytes per row */
            {
                s32 tile = (y / 8) * 4 + (x / 8);
                s32 off = tile * 16 + (y % 8) * 2 + ((x % 8) / 4);
                tiles[off] = (u16) (nib[0] | (nib[1] << 4) | (nib[2] << 8) | (nib[3] << 12));
            }
        }
    }
    /* palette banks 1..15: 3 materials x 5 depth levels (nearer = brighter) */
    {
        static const u8 base[3][3] = { { 214, 172, 120 }, { 156, 112, 76 }, { 232, 198, 150 } };
        for (m = 0; m < 3; m++) {
            for (d = 0; d < 5; d++) {
                for (i = 1; i < 16; i++) {
                    s32 c[3], k;
                    for (k = 0; k < 3; k++) {
                        c[k] = base[m][k] * i / 15 * (100 - d * 7) / 100;
                        if (i == 15) c[k] = c[k] + (255 - c[k]) / 4; /* specular-ish top */
                    }
                    PAL_OBJ[(1 + m * 5 + d) * 16 + i] = RGB15(c[0], c[1], c[2]);
                }
            }
        }
    }
    mannequin_hide();
}

void mannequin_hide(void) {
    s32 i;
    for (i = 0; i < NUM_PARTS; i++) {
        sOam[i][0] = 0x0200; /* disabled */
        sOam[i][1] = 0;
        sOam[i][2] = 0;
    }
    sNumLimbs = 0;
}

static s32 cmp_limb_depth(const struct Limb *a, const struct Limb *b) {
    return a->depth - b->depth;
}

/* Camera space as in the renderer: x right, y up, z forward. */
static void cam_vec(const struct RCamera *cam, s32 v[3], s32 out[3], s32 sinY, s32 cosY, s32 sinP, s32 cosP) {
    s32 zf = -((v[0] * sinY + v[2] * cosY) >> 14);
    (void) cam;
    out[0] = (v[0] * cosY - v[2] * sinY) >> 14;
    out[2] = (zf * cosP - v[1] * sinP) >> 14;
    out[1] = (zf * sinP + v[1] * cosP) >> 14;
}

#ifdef DEMO_IMPORTED
#define FAST_SPRITES /* unused with the imported model: keep IWRAM for the renderer */
#else
#define FAST_SPRITES FAST
#endif
FAST_SPRITES void mannequin_update(struct Object *marioObj, const struct RCamera *cam) {
    s32 sinY = qsin(cam->yaw), cosY = qcos(cam->yaw), sinP = qsin(cam->pitch), cosP = qcos(cam->pitch);
    s32 part, n = 0, i, j, centerDepth = 0;

    if (!pose(marioObj)) {
        mannequin_hide();
        return;
    }

    for (part = 0; part < NUM_PARTS; part++) {
        const struct Shape *sh = &kShapes[part];
        const struct Frame *f = &sFrames[part];
        struct Limb *l;
        s32 c[3], cc[3], k, z, s[3], rz;
        if (sh->rx < 0) continue;
        /* center in world * 16, relative to the camera */
        for (k = 0; k < 3; k++) {
            c[k] = f->t[k] + ((sh->cx * 4 * f->r[0][k] + sh->cy * 4 * f->r[1][k] + sh->cz * 4 * f->r[2][k]) >> 14);
        }
        c[0] -= cam->x * 16;
        c[1] -= cam->y * 16;
        c[2] -= cam->z * 16;
        cam_vec(cam, c, cc, sinY, cosY, sinP, cosP);
        z = cc[2];
        if (z < 32 * 16) continue; /* behind or too close to the camera */
        /* semi-axes: rows of the part rotation * radius (model) * 4 -> world * 16;
         * projected with weak perspective at the center's depth, in pixels Q8 */
        s[0] = sh->rx;
        s[1] = sh->ry;
        s[2] = sh->rz;
        /* 2^30 / z (z in world * 16) from the 2^24 / z_world table: no division */
        rz = (s32) gRecipZ[(z >> 4) > 16383 ? 16383 : (z >> 4)] << 2;
        l = &sLimbs[n];
        l->depth = z;
        l->x = (s16) (120 + (s32) (((s64) (cc[0] * FOCAL)) * rz >> 30));
        l->y = (s16) (80 - (s32) (((s64) (cc[1] * FOCAL)) * rz >> 30));
        l->bank = (u8) (1 + sh->material * 5);
        {
            s32 sxx = MIN_RADIUS_Q8, sxy = 0, syy = MIN_RADIUS_Q8; /* S + r_min^2 I */
            s32 l11, l21, l22, t2, pa, pc, pd, inv11, inv22;
            for (k = 0; k < 3; k++) {
                s32 v[3], vc[3], ux, uy;
                v[0] = (f->r[k][0] * s[k] * 4) >> 14;
                v[1] = (f->r[k][1] * s[k] * 4) >> 14;
                v[2] = (f->r[k][2] * s[k] * 4) >> 14;
                cam_vec(cam, v, vc, sinY, cosY, sinP, cosP);
                /* semi-axis on screen, pixels Q4 (weak perspective at the center) */
                ux = (s32) (((s64) (vc[0] * FOCAL * 16)) * rz >> 30);
                uy = (s32) (((s64) (-vc[1] * FOCAL * 16)) * rz >> 30);
                if (ux > 1024) ux = 1024; if (ux < -1024) ux = -1024; /* 64 px: sprite box */
                if (uy > 1024) uy = 1024; if (uy < -1024) uy = -1024;
                sxx += ux * ux; /* Q8 */
                sxy += ux * uy;
                syy += uy * uy;
            }
            /* S = L L^T (Cholesky), P = R L^-1: P^T P = R^2 S^-1, so the ball
             * texture covers exactly the ellipse (the ball is shaded radially,
             * so the rotation this choice of P adds is invisible). */
            l11 = isqrt32((u32) sxx);              /* Q4 */
            if (l11 == 0 || l11 > 32767) continue;
            inv11 = (s32) gRecipClip[l11];         /* 2^30 / l11 */
            l21 = (s32) (((s64) sxy * inv11) >> 30); /* Q4 */
            t2 = syy - l21 * l21;
            if (t2 <= 0) continue;
            l22 = isqrt32((u32) t2);               /* Q4 */
            if (l22 == 0 || l22 > 32767) continue;
            inv22 = (s32) gRecipClip[l22];
            pa = (s32) (((s64) (BALL_R * 4096) * inv11) >> 30); /* 8.8: R / l11_px */
            pd = (s32) (((s64) (BALL_R * 4096) * inv22) >> 30);
            pc = -(s32) (((s64) pa * l21 * inv22) >> 30);       /* -R l21 / (l11 l22) */
            l->pa = (s16) (pa > 32767 ? 32767 : pa);
            l->pb = 0;
            l->pc = (s16) (pc > 32767 ? 32767 : pc < -32767 ? -32767 : pc);
            l->pd = (s16) (pd > 32767 ? 32767 : pd);
        }
        centerDepth += z;
        n++;
    }

    /* nearest first: lower OAM index draws on top */
    for (i = 1; i < n; i++) {
        struct Limb t = sLimbs[i];
        for (j = i - 1; j >= 0 && cmp_limb_depth(&sLimbs[j], &t) > 0; j--) sLimbs[j + 1] = sLimbs[j];
        sLimbs[j + 1] = t;
    }
    if (n > 0) centerDepth /= n;

    for (i = 0; i < NUM_PARTS; i++) {
        if (i < n) {
            const struct Limb *l = &sLimbs[i];
            s32 dl = (l->depth - centerDepth) / (12 * 16) + 2; /* depth shading level */
            s32 x = l->x - 32, y = l->y - 32;                    /* double-size box: 64x64 */
            dl = dl < 0 ? 0 : dl > 4 ? 4 : dl;
            if (x < -64 || x > 240 || y < -64 || y > 160) {
                sOam[i][0] = 0x0200;
                continue;
            }
            sOam[i][0] = (u16) ((y & 0xFF) | 0x0100 | 0x0200);           /* affine, double size */
            sOam[i][1] = (u16) ((x & 0x1FF) | (i << 9) | (2 << 14));     /* matrix i, 32x32 */
            sOam[i][2] = (u16) (BALL_TILE | ((l->bank + dl) << 12));
            sAffine[i][0] = l->pa;
            sAffine[i][1] = l->pb;
            sAffine[i][2] = l->pc;
            sAffine[i][3] = l->pd;
        } else {
            sOam[i][0] = 0x0200;
        }
    }
    sNumLimbs = n;
}

/* Copy to OAM during VBlank: attributes in slots OAM_FIRST.., affine
 * matrices 0..19 (the 4th halfword of each group of 4 OAM entries). */
void mannequin_commit(void) {
    s32 i;
    for (i = 0; i < NUM_PARTS; i++) {
        OAM[(OAM_FIRST + i) * 4 + 0] = sOam[i][0];
        OAM[(OAM_FIRST + i) * 4 + 1] = sOam[i][1];
        OAM[(OAM_FIRST + i) * 4 + 2] = sOam[i][2];
        OAM[i * 16 + 3] = (u16) sAffine[i][0];
        OAM[i * 16 + 7] = (u16) sAffine[i][1];
        OAM[i * 16 + 11] = (u16) sAffine[i][2];
        OAM[i * 16 + 15] = (u16) sAffine[i][3];
    }
}
