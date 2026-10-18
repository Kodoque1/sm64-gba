/*
 * Checks the demo mannequin's fixed-point skeleton against SM64's own float
 * matrix code: for every one of Mario's animations (sampled frames), several
 * object yaws and pitches, the joint positions from demo/mannequin.c's pose()
 * must match the chain the game's renderer builds:
 *
 *   object = mtxf_rotate_zxy_and_translate(gfx.pos, gfx.angle), scaled by
 *   0.25 (mario_geo's GEO_SCALE 16384, mtxf_scale_vec3f), then per animated
 *   part: local = mtxf_rotate_xyz_and_translate(offset (+ root anim
 *   translation), anim rotation), world = local * parent (mtxf_mul).
 *
 * Tolerance: 1.5 world units (Q14 trig over a 20-joint chain; Mario's hitbox is 160
 * units tall).
 */
#define MANNEQUIN_TEST
#include "../../demo/mannequin.c"

#include "engine/math_util.h"
#include "game/mario.h"
#include "game/game_init.h"

int printf(const char *fmt, ...);

struct MarioState gMarioStates[1];
extern const signed char gSkelParent[20];
extern const short gSkelOffset[20][3];
void setup_dma_table_list(struct DmaHandlerList *list, void *srcAddr, void *buffer);

static u8 sAnimBuf[0x4000] __attribute__((aligned(16)));
static struct Object sObj;

/* The game's chain, in floats. */
static void reference(struct Object *o, f32 out[20][3]) {
    struct AnimInfo *ai = &o->header.gfx.animInfo;
    struct Animation *anim = ai->curAnim;
    const u16 *attr = (const u16 *) anim->index;
    const s16 *values = (const s16 *) anim->values;
    s32 frame = ai->animFrame, type, part;
    f32 mul = anim->animYTransDivisor == 0 ? 1.0f : (f32) ai->animYTrans / (f32) anim->animYTransDivisor;
    Mat4 objM, scaled, world[20];
    Vec3f pos, sc = { 0.25f, 0.25f, 0.25f };

    if (anim->flags & ANIM_FLAG_HOR_TRANS) type = 2;
    else if (anim->flags & ANIM_FLAG_VERT_TRANS) type = 1;
    else if (anim->flags & ANIM_FLAG_6) type = 3;
    else type = 0;

    vec3f_copy(pos, o->header.gfx.pos);
    mtxf_rotate_zxy_and_translate(objM, pos, o->header.gfx.angle);
    mtxf_scale_vec3f(scaled, objM, sc);

    for (part = 0; part < 20; part++) {
        Vec3f t;
        Vec3s r;
        Mat4 local;
        t[0] = gSkelOffset[part][0];
        t[1] = gSkelOffset[part][1];
        t[2] = gSkelOffset[part][2];
        if (part == 0) {
            if (type == 0) {
                t[0] += values[retrieve_animation_index(frame, (u16 **) &attr)] * mul;
                t[1] += values[retrieve_animation_index(frame, (u16 **) &attr)] * mul;
                t[2] += values[retrieve_animation_index(frame, (u16 **) &attr)] * mul;
            } else if (type == 1) {
                t[0] += values[retrieve_animation_index(frame, (u16 **) &attr)] * mul;
                attr += 2;
                t[2] += values[retrieve_animation_index(frame, (u16 **) &attr)] * mul;
            } else if (type == 2) {
                attr += 2;
                t[1] += values[retrieve_animation_index(frame, (u16 **) &attr)] * mul;
                attr += 2;
            } else {
                attr += 6;
            }
        }
        r[0] = values[retrieve_animation_index(frame, (u16 **) &attr)];
        r[1] = values[retrieve_animation_index(frame, (u16 **) &attr)];
        r[2] = values[retrieve_animation_index(frame, (u16 **) &attr)];
        mtxf_rotate_xyz_and_translate(local, t, r);
        mtxf_mul(world[part], local, part == 0 ? scaled : world[gSkelParent[part]]);
        out[part][0] = world[part][3][0];
        out[part][1] = world[part][3][1];
        out[part][2] = world[part][3][2];
    }
}

double fabs(double);

int main(void) {
    struct MarioState *m = &gMarioStates[0];
    s32 id, frame, yaw, pitch, part, checks = 0, fails = 0;
    double worst = 0;

    gMarioState = m;
    m->marioObj = &sObj;
    m->animList = &gMarioAnimsBuf;
    m->unkB0 = 0xBD;
    setup_dma_table_list(&gMarioAnimsBuf, (void *) gMarioAnims, sAnimBuf);
    sObj.header.gfx.pos[0] = 1234.5f;
    sObj.header.gfx.pos[1] = -321.25f;
    sObj.header.gfx.pos[2] = 777.0f;

    for (id = 0; id < 209; id++) {
        struct Animation *a;
        sObj.header.gfx.animInfo.animID = -1;
        set_mario_animation(m, id);
        a = sObj.header.gfx.animInfo.curAnim;
        for (frame = 0; frame < a->loopEnd + 2; frame += 5) {
            for (yaw = 0; yaw < 0x10000; yaw += 0x2A00) {
                for (pitch = -0x2000; pitch <= 0x2000; pitch += 0x2000) {
                    f32 ref[20][3];
                    sObj.header.gfx.animInfo.animFrame = (s16) frame;
                    sObj.header.gfx.angle[0] = (s16) pitch;
                    sObj.header.gfx.angle[1] = (s16) yaw;
                    sObj.header.gfx.angle[2] = 0;
                    reference(&sObj, ref);
                    pose(&sObj);
                    for (part = 0; part < 20; part++) {
                        s32 k;
                        for (k = 0; k < 3; k++) {
                            double d = fabs(sFrames[part].t[k] / 16.0 - ref[part][k]);
                            if (d > worst) worst = d;
                            if (d > 1.5 && fails++ < 8) {
                                printf("  FAIL anim %d frame %d yaw %x part %d axis %d: fixed %.2f float %.2f\n",
                                       id, frame, yaw, part, k, sFrames[part].t[k] / 16.0, ref[part][k]);
                            }
                        }
                        checks++;
                    }
                }
            }
        }
    }
    printf("%s: %d joint positions over 209 animations, worst error %.2f world units\n",
           fails ? "FAIL" : "PASS", checks, worst);
    return fails != 0;
}
