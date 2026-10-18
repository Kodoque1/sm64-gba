/*
 * Replay harness: drives Mario's movement core one frame at a time from an
 * input script and logs his full physical state as raw float bits.
 *
 * Input script (text):
 *   level <name>                 first non-comment line: bob | wf | ccm | bitdw
 *   <rawX> <rawY> <buttons> <camYaw>   one line per frame
 *     rawX/rawY : N64 stick, -128..127 (as read from the controller)
 *     buttons   : N64 button mask in hex (A=8000 B=4000 Z=2000 ...)
 *     camYaw    : camera yaw, s16 (decides which way "up" on the stick goes)
 *   Lines starting with '#' are comments.
 *
 * Output: one line per frame, every float printed as its IEEE bit pattern,
 * then "hash <fnv1a>" over all frame lines. Two builds agree bit for bit
 * iff their logs are identical.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "engine/graph_node.h"
#include "engine/surface_load.h"
#include "engine/surface_collision.h"
#include "game/area.h"
#include "game/camera.h"
#include "game/game_init.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/object_list_processor.h"
#include "levels.h"
#include "platform.h"


void adjust_analog_stick(struct Controller *controller);
void copy_mario_state_to_object(void);
void mario_anim_render_step(struct Object *marioObj);
void setup_dma_table_list(struct DmaHandlerList *list, void *srcAddr, void *buffer);

struct MarioState gMarioStates[1];

static struct Object sMarioObject;
static struct Area sArea;
static struct Camera sCamera;
/* The N64 DMAs each animation into a 16 KB buffer. Optimized builds copy only
 * the 24-byte header (ANIM_ZERO_COPY in anim.c) and shrink this to 64 bytes. */
#ifndef MARIO_ANIM_BUFFER_SIZE
#define MARIO_ANIM_BUFFER_SIZE 0x4000
#endif
static u8 sMarioAnimBuffer[MARIO_ANIM_BUFFER_SIZE] __attribute__((aligned(16)));

/* ---------- tiny formatting helpers (no libc in this translation unit) ---------- */

static char sLine[256];
static int sLen;
static u32 sHash = 0x811C9DC5u;

static void put_c(char c) {
    sLine[sLen++] = c;
}

static void put_s(const char *s) {
    while (*s) {
        put_c(*s++);
    }
}

static void put_hex(u32 v, int digits) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = digits - 1; i >= 0; i--) {
        put_c(hex[(v >> (i * 4)) & 0xF]);
    }
}

static void put_dec(s32 v) {
    char tmp[12];
    int n = 0;
    u32 u;
    if (v < 0) {
        put_c('-');
        u = (u32) (-(v + 1)) + 1;
    } else {
        u = (u32) v;
    }
    do {
        tmp[n++] = (char) ('0' + u % 10);
        u /= 10;
    } while (u != 0);
    while (n > 0) {
        put_c(tmp[--n]);
    }
}

static u32 f32_bits(f32 f) {
    union {
        f32 f;
        u32 u;
    } x;
    x.f = f;
    return x.u;
}

static void put_f(f32 f) {
    put_c(' ');
    put_hex(f32_bits(f), 8);
}

static void flush_line(int hashed) {
    int i;
    put_c('\n');
    if (hashed) {
        for (i = 0; i < sLen; i++) {
            sHash ^= (u8) sLine[i];
            sHash *= 0x01000193u;
        }
    }
    plat_write(sLine, sLen);
    sLen = 0;
}

static void err(const char *s) {
    int n = 0;
    while (s[n]) {
        n++;
    }
    plat_write_err(s, n);
}

/* ---------- parsing ---------- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static const char *parse_int(const char *p, s32 *out, int base) {
    s32 sign = 1;
    s32 v = 0;
    p = skip_ws(p);
    if (*p == '-') {
        sign = -1;
        p++;
    }
    for (;;) {
        s32 d;
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (base == 16 && *p >= 'a' && *p <= 'f') {
            d = *p - 'a' + 10;
        } else if (base == 16 && *p >= 'A' && *p <= 'F') {
            d = *p - 'A' + 10;
        } else {
            break;
        }
        v = v * base + d;
        p++;
    }
    *out = v * sign;
    return p;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* ---------- setup, mirroring what the level script does on area load ---------- */

static void load_level(const struct HarnessLevel *lvl) {
    alloc_surface_pools();
    load_area_terrain(0, (TerrainData *) lvl->collision, NULL, NULL);

    sArea.index = 1;
    sArea.terrainType = lvl->terrainType;
    sArea.camera = &sCamera;
    gCurrentArea = &sArea;
    gCurrAreaIndex = 1;
    gCamera = &sCamera;

    gMarioSpawnInfo = &gPlayerSpawnInfos[0];
    gMarioSpawnInfo->areaIndex = 1;
    gMarioSpawnInfo->startPos[0] = lvl->spawnPos[0];
    gMarioSpawnInfo->startPos[1] = lvl->spawnPos[1];
    gMarioSpawnInfo->startPos[2] = lvl->spawnPos[2];
    gMarioSpawnInfo->startAngle[0] = 0;
    gMarioSpawnInfo->startAngle[1] = lvl->spawnYawDegrees * 0x8000 / 180;
    gMarioSpawnInfo->startAngle[2] = 0;

    sMarioObject.header.gfx.node.flags = GRAPH_RENDER_ACTIVE | GRAPH_RENDER_HAS_ANIMATION;
    sMarioObject.header.gfx.areaIndex = 1;
    sMarioObject.header.gfx.activeAreaIndex = 1;
    gMarioObject = &sMarioObject;
    gCurrentObject = gMarioObject;

    setup_dma_table_list(&gMarioAnimsBuf, (void *) &gMarioAnims, sMarioAnimBuffer);

    gMarioState = &gMarioStates[0];
    gPlayer1Controller = &gControllers[0];
    gCurrSaveFileNum = 1;
    init_mario_from_save_file();
    init_mario();
}

static void log_frame(s32 frame) {
    struct MarioState *m = gMarioState;
    struct AnimInfo *anim = &m->marioObj->header.gfx.animInfo;

    put_dec(frame);
    put_c(' ');
    put_hex(m->action, 8);
    put_c(' ');
    put_hex((u16) m->actionState, 4);
    put_c(' ');
    put_hex((u16) m->actionTimer, 4);
    put_f(m->pos[0]);
    put_f(m->pos[1]);
    put_f(m->pos[2]);
    put_f(m->vel[0]);
    put_f(m->vel[1]);
    put_f(m->vel[2]);
    put_f(m->forwardVel);
    put_c(' ');
    put_hex((u16) m->faceAngle[0], 4);
    put_c(' ');
    put_hex((u16) m->faceAngle[1], 4);
    put_c(' ');
    put_hex((u16) m->faceAngle[2], 4);
    put_f(m->floorHeight);
    put_c(' ');
    put_hex((u16) anim->animID, 4);
    put_c(' ');
    put_hex((u16) anim->animFrame, 4);
    flush_line(1);
}

int main(void) {
    static char buf[256];
    const struct HarnessLevel *lvl = NULL;
    s32 frame = 0;
    s32 i;
    int n;

    plat_init();

    /* Header: first non-comment line names the level. */
    while ((n = plat_read_line(buf, sizeof(buf))) >= 0) {
        const char *p = skip_ws(buf);
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (p[0] == 'l' && p[1] == 'e' && p[2] == 'v' && p[3] == 'e' && p[4] == 'l') {
            p = skip_ws(p + 5);
            for (i = 0; i < gNumHarnessLevels; i++) {
                if (str_eq(p, gHarnessLevels[i].name)) {
                    lvl = &gHarnessLevels[i];
                }
            }
        }
        break;
    }
    if (lvl == NULL) {
        err("harness: first line must be 'level <bob|wf|ccm|bitdw>'\n");
        return 2;
    }

    load_level(lvl);

    put_s("level ");
    put_s(lvl->name);
    flush_line(0);
    log_frame(0);

    while ((n = plat_read_line(buf, sizeof(buf))) >= 0) {
        const char *p = skip_ws(buf);
        s32 rawX, rawY, buttons, camYaw;
        struct Controller *c = gPlayer1Controller;

        if (*p == '\0' || *p == '#') {
            continue;
        }
        p = parse_int(p, &rawX, 10);
        p = parse_int(p, &rawY, 10);
        p = parse_int(p, &buttons, 16);
        p = parse_int(p, &camYaw, 10);

        plat_frame_begin();

        /* game_init.c: read_controller_inputs() */
        c->rawStickX = (s16) rawX;
        c->rawStickY = (s16) rawY;
        c->buttonPressed = (u16) (buttons & (buttons ^ c->buttonDown));
        c->buttonDown = (u16) buttons;
        adjust_analog_stick(c);
        sCamera.yaw = (s16) camYaw;

        /* One game frame: area update, then Mario's behavior, then render. */
        gGlobalTimer++;
        gAreaUpdateCounter++;
        gCurrentObject = gMarioObject;
        gCurrentObject->oMarioParticleFlags = execute_mario_action(gCurrentObject);
        copy_mario_state_to_object();
        mario_anim_render_step(gMarioObject);

        plat_frame_end();

        frame++;
        log_frame(frame);
    }

    {
        /* Memory side channel (stderr), not part of the compared log. */
        extern u32 harness_pool_used(void);
        u32 used = harness_pool_used();
        sLen = 0;
        put_s("pool ");
        put_dec((s32) used);
        put_c('\n');
        plat_write_err(sLine, sLen);
        sLen = 0;
    }

    put_s("hash ");
    put_hex(sHash, 8);
    flush_line(0);
    plat_finish();
    return 0;
}
