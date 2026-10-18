/*
 * Engine pieces that DO affect Mario's physical state, copied from the
 * decomp with only the hardware parts (DMA, graph rendering) replaced.
 *
 * Animation matters for movement: is_anim_at_end() gates many action
 * transitions (landings, idles), and update_mario_pos_for_anim() moves
 * Mario by the animation's root translation in some actions. On the N64
 * the animation frame advances during the render pass, so the harness
 * calls mario_anim_render_step() once per frame after the update.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "engine/graph_node.h"
#include "game/game_init.h"
#include "game/level_update.h"
#include "game/memory.h"
#include "game/object_list_processor.h"

/* memory.c: the N64 DMAs the requested animation from ROM into a 16 KB
 * buffer. The fixup in set_mario_animation() relies on that copy, so keep it. */
static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    while (n--) {
        *dst++ = *src++;
    }
}

s32 load_patchable_table(struct DmaHandlerList *list, s32 index) {
    s32 ret = FALSE;
    struct DmaTable *table = list->dmaTable;

    if ((u32) index < table->count) {
        /* The original reads table->srcAddr, which setup_dma_table_list()
         * writes into the table at boot. Using the table's own address
         * instead lets the table stay const, i.e. in cartridge ROM. */
        u8 *addr = (u8 *) table + table->anim[index].offset;
        s32 size = table->anim[index].size;

        if (addr != list->currentAddr) {
#ifdef ANIM_ZERO_COPY
            /* Optimized builds: copy only the 24-byte Animation header and
             * pre-bias its index/values offsets so that the fixup in
             * set_mario_animation() (buffer + offset) lands on the original
             * data in ROM. Same bytes read, no 16 KB copy (~250k cycles on
             * the GBA, done on every animation change). */
            struct Animation *hdr = (struct Animation *) list->bufTarget;
            const struct Animation *src = (const struct Animation *) addr;
            u8 *buf = (u8 *) list->bufTarget;
            (void) size;
            copy_bytes((u8 *) hdr, (const u8 *) src, sizeof(struct Animation));
            hdr->values = (const s16 *) (addr + (uintptr_t) src->values - (uintptr_t) buf);
            hdr->index = (const u16 *) (addr + (uintptr_t) src->index - (uintptr_t) buf);
#else
            copy_bytes(list->bufTarget, addr, size);
#endif
            list->currentAddr = addr;
            ret = TRUE;
        }
    }
    return ret;
}

void setup_dma_table_list(struct DmaHandlerList *list, void *srcAddr, void *buffer) {
    if (srcAddr != NULL) {
        list->dmaTable = (struct DmaTable *) srcAddr;
    }
    list->currentAddr = NULL;
    list->bufTarget = buffer;
}

/* graph_node.c */
s32 retrieve_animation_index(s32 frame, u16 **attributes) {
    s32 result;

    if (frame < (*attributes)[0]) {
        result = (*attributes)[1] + frame;
    } else {
        result = (*attributes)[1] + (*attributes)[0] - 1;
    }

    *attributes += 2;

    return result;
}

/* graph_node.c */
s16 geo_update_animation_frame(struct AnimInfo *obj, s32 *accelAssist) {
    s32 result;
    struct Animation *anim = obj->curAnim;

    if (obj->animTimer == gAreaUpdateCounter || anim->flags & ANIM_FLAG_2) {
        if (accelAssist != NULL) {
            accelAssist[0] = obj->animFrameAccelAssist;
        }

        return obj->animFrame;
    }

    if (anim->flags & ANIM_FLAG_BACKWARD) {
        if (obj->animAccel != 0) {
            result = obj->animFrameAccelAssist - obj->animAccel;
        } else {
            result = (obj->animFrame - 1) << 16;
        }

        if (GET_HIGH_S16_OF_32(result) < anim->loopStart) {
            if (anim->flags & ANIM_FLAG_NOLOOP) {
                SET_HIGH_S16_OF_32(result, anim->loopStart);
            } else {
                SET_HIGH_S16_OF_32(result, anim->loopEnd - 1);
            }
        }
    } else {
        if (obj->animAccel != 0) {
            result = obj->animFrameAccelAssist + obj->animAccel;
        } else {
            result = (obj->animFrame + 1) << 16;
        }

        if (GET_HIGH_S16_OF_32(result) >= anim->loopEnd) {
            if (anim->flags & ANIM_FLAG_NOLOOP) {
                SET_HIGH_S16_OF_32(result, anim->loopEnd - 1);
            } else {
                SET_HIGH_S16_OF_32(result, anim->loopStart);
            }
        }
    }

    if (accelAssist != 0) {
        accelAssist[0] = result;
    }

    return GET_HIGH_S16_OF_32(result);
}

/* The animation half of rendering_graph_node.c: geo_set_animation_globals(),
 * as reached from geo_process_object() for Mario's object every frame.
 * Runs regardless of visibility, exactly like the original. */
void mario_anim_render_step(struct Object *marioObj) {
    struct AnimInfo *node = &marioObj->header.gfx.animInfo;

    if (node->curAnim == NULL) {
        return;
    }
    if (marioObj->header.gfx.node.flags & GRAPH_RENDER_HAS_ANIMATION) {
        node->animFrame = geo_update_animation_frame(node, &node->animFrameAccelAssist);
    }
    node->animTimer = gAreaUpdateCounter;
}

/* object_list_processor.c (single-player path). */
extern struct MarioState gMarioStates[];

void copy_mario_state_to_object(void) {
    s32 i = 0;
    if (gCurrentObject != gMarioObject) {
        i++;
    }

    gCurrentObject->oVelX = gMarioStates[i].vel[0];
    gCurrentObject->oVelY = gMarioStates[i].vel[1];
    gCurrentObject->oVelZ = gMarioStates[i].vel[2];

    gCurrentObject->oPosX = gMarioStates[i].pos[0];
    gCurrentObject->oPosY = gMarioStates[i].pos[1];
    gCurrentObject->oPosZ = gMarioStates[i].pos[2];

    gCurrentObject->oMoveAnglePitch = gCurrentObject->header.gfx.angle[0];
    gCurrentObject->oMoveAngleYaw = gCurrentObject->header.gfx.angle[1];
    gCurrentObject->oMoveAngleRoll = gCurrentObject->header.gfx.angle[2];

    gCurrentObject->oFaceAnglePitch = gCurrentObject->header.gfx.angle[0];
    gCurrentObject->oFaceAngleYaw = gCurrentObject->header.gfx.angle[1];
    gCurrentObject->oFaceAngleRoll = gCurrentObject->header.gfx.angle[2];

    gCurrentObject->oAngleVelPitch = gMarioStates[i].angleVel[0];
    gCurrentObject->oAngleVelYaw = gMarioStates[i].angleVel[1];
    gCurrentObject->oAngleVelRoll = gMarioStates[i].angleVel[2];
}

/* game_init.c */
void adjust_analog_stick(struct Controller *controller) {
    controller->stickX = 0;
    controller->stickY = 0;

    if (controller->rawStickX <= -8) {
        controller->stickX = controller->rawStickX + 6;
    }
    if (controller->rawStickX >= 8) {
        controller->stickX = controller->rawStickX - 6;
    }
    if (controller->rawStickY <= -8) {
        controller->stickY = controller->rawStickY + 6;
    }
    if (controller->rawStickY >= 8) {
        controller->stickY = controller->rawStickY - 6;
    }

    controller->stickMag =
        sqrtf(controller->stickX * controller->stickX + controller->stickY * controller->stickY);

    if (controller->stickMag > 64) {
        controller->stickX *= 64 / controller->stickMag;
        controller->stickY *= 64 / controller->stickMag;
        controller->stickMag = 64;
    }
}
