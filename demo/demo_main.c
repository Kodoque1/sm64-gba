/*
 * Playable demo: Mario's movement core (the same bit-exact, optimized
 * objects the parity tests check) driven by the GBA's buttons, at SM64's
 * native 30 Hz, with the level's collision mesh rendered in software.
 *
 * Frame: read keys -> Mario update (exactly the harness's sequence) ->
 * camera -> render into the back page -> wait for the 2nd VBlank since the
 * last flip -> flip, update HUD sprites -> quality governor.
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
#include "surface_terrains.h"
#include "levels.h"
#include "render.h"
#include "hud.h"
#include "mannequin.h"
#ifdef DEMO_IMPORTED
#include "import.h"
#endif
#ifdef DEMO_BENCH
#include "bench.h"
#endif

void adjust_analog_stick(struct Controller *controller);
void copy_mario_state_to_object(void);
void mario_anim_render_step(struct Object *marioObj);
void setup_dma_table_list(struct DmaHandlerList *list, void *srcAddr, void *buffer);
void irq_handler(void);
void fill_words(u32 *dst, u32 value, s32 count); /* irq.s */
extern s32 gHarnessWarpRequest;
extern const s16 gSinQ14[4096];

struct MarioState gMarioStates[1];
volatile u32 gVBlankCount;

static struct Object sMarioObject;
static struct Area sArea;
static struct Camera sGameCamera;
static u8 sMarioAnimBuffer[64] __attribute__((aligned(16))); /* header only: ANIM_ZERO_COPY */

/* ---------- surface pools sized for these levels (frees ~70 KB of EWRAM) ---------- */

#define DEMO_MAX_NODES 5440
#define DEMO_MAX_SURFACES 1072
extern struct SurfaceNode *sSurfaceNodePool;
extern s16 sSurfacePoolSize;
void reset_red_coins_collected(void);

void __wrap_alloc_surface_pools(void) {
    sSurfacePoolSize = DEMO_MAX_SURFACES;
    sSurfaceNodePool = main_pool_alloc(DEMO_MAX_NODES * sizeof(struct SurfaceNode), MEMORY_POOL_LEFT);
    sSurfacePool = main_pool_alloc(DEMO_MAX_SURFACES * sizeof(struct Surface), MEMORY_POOL_LEFT);
    gCCMEnteredSlide = 0;
    reset_red_coins_collected();
}

/* ---------- level looks ---------- */

static const struct RLevelStyle kStyles[] = {
    /* bob: grass hills, earth walls */
    { { { 96, 176, 64 }, { 132, 150, 70 }, { 150, 118, 86 }, { 186, 186, 200 }, { 196, 182, 140 },
        { 220, 96, 40 }, { 86, 72, 66 } },
      { 64, 120, 232 }, { 176, 216, 248 } },
    /* wf: stone fortress, grassy tops */
    { { { 110, 170, 80 }, { 150, 160, 100 }, { 176, 168, 150 }, { 190, 190, 205 }, { 200, 196, 176 },
        { 220, 96, 40 }, { 96, 90, 86 } },
      { 72, 128, 232 }, { 184, 216, 248 } },
    /* ccm: snow */
    { { { 236, 240, 250 }, { 212, 222, 238 }, { 130, 128, 148 }, { 160, 200, 246 }, { 196, 196, 210 },
        { 220, 96, 40 }, { 70, 70, 90 } },
      { 96, 140, 220 }, { 210, 226, 246 } },
    /* bitdw: dark sky, purple stone */
    { { { 156, 146, 178 }, { 128, 118, 158 }, { 100, 88, 128 }, { 150, 170, 210 }, { 176, 160, 196 },
        { 220, 96, 40 }, { 60, 52, 80 } },
      { 10, 8, 36 }, { 70, 50, 110 } },
};
static const char *const kLevelLabels[] = { "BOB", "WF", "CCM", "BITDW" };
/* Background below the horizon: ground color, or sky for levels over a void. */
static const u8 kGroundFill[] = { 0 * 32 + 6 * 4 + 1, 0 * 32 + 6 * 4 + 1, 0 * 32 + 6 * 4 + 2, PAL_SKY + 15 };

/* ---------- quality steps, best first ---------- */

static const struct RQuality kQuality[] = {
    { 160, 106, 8000 }, { 160, 106, 6000 }, { 144, 96, 5000 },
    { 128, 85, 4200 },  { 112, 74, 3600 },  { 96, 64, 3000 },
};
#define NUM_QUALITY ((s32) (sizeof(kQuality) / sizeof(kQuality[0])))

/* ---------- stack high-water mark ----------
 * The free IWRAM between the end of IWRAM code/data and the stack is filled
 * with a pattern at boot; the lowest overwritten word is the deepest the
 * stack has reached. Reported in the debug stats (w = bytes of stack used). */
extern u8 __iwram_end[];
#define STACK_TOP 0x03007F00u
#define STACK_PATTERN 0x5AC3A55Cu

static void stack_paint(void) {
    u32 *p = (u32 *) (((u32) __iwram_end + 3) & ~3u);
    u32 *limit;
    register u32 sp asm("sp");
    limit = (u32 *) (sp - 64);
    while (p < limit) *p++ = STACK_PATTERN;
}

static u32 stack_used(void) {
    u32 *p = (u32 *) (((u32) __iwram_end + 3) & ~3u);
    while (p < (u32 *) STACK_TOP && *p == STACK_PATTERN) p++;
    return STACK_TOP - (u32) p;
}

/* ---------- timing ---------- */

#define CYCLES_PER_VBLANK 280896
#define FRAME_BUDGET (2 * CYCLES_PER_VBLANK) /* SM64 runs at 30 Hz */

static void timer_start(void) {
    REG_TM2CNT_H = 0;
    REG_TM3CNT_H = 0;
    REG_TM2CNT_L = 0;
    REG_TM3CNT_L = 0;
    REG_TM3CNT_H = 0x0084; /* cascade */
    REG_TM2CNT_H = 0x0080;
}

static u32 cycles_now(void) {
    u32 hi, lo, hi2;
    do {
        hi = REG_TM3CNT_L;
        lo = REG_TM2CNT_L;
        hi2 = REG_TM3CNT_L;
    } while (hi != hi2);
    return (hi << 16) | lo;
}

/* mGBA debug output (ignored on hardware): one stats line per frame. */
#define REG_DEBUG_ENABLE REG16(0x04FFF780)
#define REG_DEBUG_FLAGS REG16(0x04FFF700)
#define DEBUG_STRING ((volatile char *) 0x04FFF600)
static s32 sDebug;

static void debug_line(const char *s) {
    s32 i = 0;
    if (!sDebug) return;
    for (; s[i] && i < 250; i++) DEBUG_STRING[i] = s[i];
    DEBUG_STRING[i] = 0;
    REG_DEBUG_FLAGS = 0x100 | 3;
}

#ifdef DEMO_BENCH
/* Cartridge save RAM: 8-bit bus, so byte stores only. */
#define SRAM ((volatile u8 *) 0x0E000000)

static void sram_put(u32 ofs, u32 value, u32 bytes) {
    while (bytes--) {
        SRAM[ofs++] = (u8) value;
        value >>= 8;
    }
}
#endif

/* ---------- camera ---------- */

static struct {
    s32 yaw;      /* binary angle, s32 to allow smooth wrap */
    s32 dist;
    s32 focusY16; /* smoothed focus height, x16 */
} sCam;

static s32 isin(s32 a) {
    return gSinQ14[((u32) a & 0xFFFF) >> 4];
}

static s32 icos(s32 a) {
    return gSinQ14[((u32) (a + 0x4000) & 0xFFFF) >> 4];
}

/* angle in [0, 0x4000] with tan = y / x, for x > 0, y >= 0 (binary search on sin/cos) */
static s32 iatan(s32 y, s32 x) {
    s32 lo = 0, hi = 0x4000;
    while (hi - lo > 16) {
        s32 mid = (lo + hi) / 2;
        if ((s64) y * icos(mid) > (s64) x * isin(mid)) lo = mid; else hi = mid;
    }
    return lo;
}

static void camera_reset(struct MarioState *m) {
    sCam.yaw = (u16) (m->faceAngle[1] + 0x8000);
    sCam.dist = 1000;
    sCam.focusY16 = ((s32) m->pos[1] + 125) * 16;
}

static void camera_update(struct MarioState *m, u32 keys, struct RCamera *out) {
    const s32 pitchBase = 0x0E00; /* ~20 degrees down */
    s32 mx = (s32) m->pos[0], my = (s32) m->pos[1], mz = (s32) m->pos[2];
    s32 hd, vd, cx, cy, cz;

    if (keys & KEY_L) {
        if (keys & KEY_LEFT) sCam.yaw -= 0x380;
        if (keys & KEY_RIGHT) sCam.yaw += 0x380;
        if ((keys & KEY_UP) && sCam.dist > 600) sCam.dist -= 40;
        if ((keys & KEY_DOWN) && sCam.dist < 2000) sCam.dist += 40;
    } else if (m->forwardVel > 4.0f || m->forwardVel < -4.0f) {
        /* drift behind Mario while he runs, unless he runs at the camera */
        s32 diff = (s16) (u16) (m->faceAngle[1] + 0x8000 - sCam.yaw);
        if (diff > -0x5000 && diff < 0x5000) sCam.yaw += diff / 24;
    }
    sCam.yaw = (u16) sCam.yaw;

    /* focus height follows smoothly (less bob when jumping) */
    sCam.focusY16 += ((my + 125) * 16 - sCam.focusY16) / 5;

    hd = (sCam.dist * icos(pitchBase)) >> 14;
    vd = (sCam.dist * isin(pitchBase)) >> 14;
    cx = mx + ((hd * isin(sCam.yaw)) >> 14);
    cz = mz + ((hd * icos(sCam.yaw)) >> 14);
    cy = sCam.focusY16 / 16 + vd;

    /* Keep the camera out of the level geometry with the game's own queries
     * (they don't touch Mario's state): out of walls, under ceilings above
     * the focus point, above the ground. */
    {
        struct Surface *surf;
        struct WallCollisionData wall;
        f32 fy, ceilY;
        s32 focusY = sCam.focusY16 / 16;

        wall.x = (f32) cx;
        wall.y = (f32) cy;
        wall.z = (f32) cz;
        wall.offsetY = 0.0f;
        wall.radius = 80.0f;
        if (find_wall_collisions(&wall) > 0) {
            cx = (s32) wall.x;
            cz = (s32) wall.z;
        }
        ceilY = find_ceil((f32) cx, (f32) focusY, (f32) cz, &surf);
        if (surf != NULL && cy > (s32) ceilY - 60) {
            cy = (s32) ceilY - 60;
        }
        fy = find_floor((f32) cx, (f32) (cy + 300), (f32) cz, &surf);
        if (surf != NULL && cy < (s32) fy + 140) cy = (s32) fy + 140;
    }

    out->x = cx;
    out->y = cy;
    out->z = cz;
    out->yaw = (u16) sCam.yaw;
    /* look at the focus point */
    {
        s32 dy = cy - sCam.focusY16 / 16;
        out->pitch = (u16) (dy > 0 ? iatan(dy, hd > 0 ? hd : 1) : 0);
    }
    sGameCamera.yaw = (s16) sCam.yaw; /* Mario's stick directions are camera-relative */
}

/* ---------- level and Mario ---------- */

static s32 sLevel;

static void spawn_mario(void) {
    init_mario_from_save_file();
    init_mario();
    gHarnessWarpRequest = 0;
    camera_reset(gMarioState);
}

static void halt_with(const char *msg) {
    hud_clear();
    hud_text(8, 8, msg);
    while (1) {
        while (!(REG_DISPSTAT & 1)) {}
        hud_commit();
        while (REG_DISPSTAT & 1) {}
    }
}

/* Black screen with a sprite message while a level loads (~2 s). */
static void show_loading(s32 idx) {
    char msg[24], *p = msg;
    const char *t = "LOADING ";
    while (*t) *p++ = *t++;
    t = kLevelLabels[idx];
    while (*t) *p++ = *t++;
    *p = 0;
    hud_clear();
    hud_text(120 - (s32) (p - msg) * 5 / 2, 76, msg);
    while (REG_VCOUNT >= 160) {}
    while (REG_VCOUNT < 160) {}
    PAL_BG[0] = 0;
    REG_DISPCNT = DCNT_MODE4 | DCNT_OBJ | DCNT_OBJ_1D; /* backdrop + sprites only */
    hud_commit();
    mannequin_hide();
    mannequin_commit();
}

static void load_level(s32 idx) {
    const struct HarnessLevel *lvl = &gHarnessLevels[idx];
#ifdef DEMO_BENCH
    u32 v0 = gVBlankCount;
#endif

    show_loading(idx);
    load_area_terrain(0, (TerrainData *) lvl->collision, NULL, NULL);
    if (gSurfaceNodesAllocated > DEMO_MAX_NODES || gSurfacesAllocated > DEMO_MAX_SURFACES) {
        halt_with("SURFACE POOL TOO SMALL");
    }

    sArea.index = 1;
    sArea.terrainType = lvl->terrainType;
    sArea.camera = &sGameCamera;
    gCurrentArea = &sArea;
    gCurrAreaIndex = 1;
    gCamera = &sGameCamera;

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
    gMarioState = &gMarioStates[0];
    gPlayer1Controller = &gControllers[0];
    gCurrSaveFileNum = 1;

#ifdef DEMO_IMPORTED
    render_build_imported(&gImpLevels[idx]);
#else
    render_build_level(&kStyles[idx], lvl->terrainType);
#endif
    if (sDebug) {
        char line[64], *p = line;
        const char *t = "load tris ";
        while (*t) *p++ = *t++;
        p = hud_fmt_int(p, gRMesh.numTris, 1);
        *p++ = ' '; *p++ = 'v'; *p++ = ' ';
        p = hud_fmt_int(p, gRMesh.numVerts, 1);
        *p++ = ' '; *p++ = 't'; *p++ = ' ';
        p = hud_fmt_int(p, gRWallsTrimmed, 1);
        *p++ = ' '; *p++ = 'd'; *p++ = ' ';
        p = hud_fmt_int(p, gRWallsDropped, 1);
        *p++ = ' '; *p++ = 'p'; *p++ = ' ';
        p = hud_fmt_int(p, gRMesh.numPits, 1);
        *p = 0;
        debug_line(line);
    }
#ifdef DEMO_IMPORTED
    render_set_imported_palette(&gImpLevels[idx]);
#else
    render_set_palette(&kStyles[idx]);
#endif
    spawn_mario();
#ifdef DEMO_BENCH
    sram_put(BENCH_LOADS_OFS + 2 * idx, gVBlankCount - v0, 2);
#endif
}

/* The same per-frame sequence as the parity harness (src/harness.c). */
/* Inputs fed to Mario this frame, logged for the end-to-end physics check
 * (tools/demo_physics_check.py replays them through the reference harness). */
static s32 sLogRawX, sLogRawY, sLogButtons, sLogCamYaw;

static void mario_frame(u32 keys, u32 pressed) {
    struct Controller *c = gPlayer1Controller;
    s32 sx = 0, sy = 0;
    u16 buttons = 0;

    if (!(keys & KEY_L)) {
        s32 full = 80, diag = 57;
        s32 h = (keys & KEY_RIGHT) ? 1 : (keys & KEY_LEFT) ? -1 : 0;
        s32 v = (keys & KEY_UP) ? 1 : (keys & KEY_DOWN) ? -1 : 0;
        s32 mag = (h && v) ? diag : full;
        sx = h * mag;
        sy = v * mag;
    }
    if (keys & KEY_A) buttons |= A_BUTTON;
    if (keys & KEY_B) buttons |= B_BUTTON;
    if (keys & KEY_R) buttons |= Z_TRIG;
    (void) pressed;

    sLogRawX = sx;
    sLogRawY = sy;
    sLogButtons = buttons;
    sLogCamYaw = sGameCamera.yaw;
    c->rawStickX = (s16) sx;
    c->rawStickY = (s16) sy;
    c->buttonPressed = (u16) (buttons & (buttons ^ c->buttonDown));
    c->buttonDown = buttons;
    adjust_analog_stick(c);

    gGlobalTimer++;
    gAreaUpdateCounter++;
    gCurrentObject = gMarioObject;
    gCurrentObject->oMarioParticleFlags = execute_mario_action(gCurrentObject);
    copy_mario_state_to_object();
    mario_anim_render_step(gMarioObject);
}

/* Level logic the harness doesn't simulate: respawn after death, warps,
 * death planes (mario_handle_special_floors in the full game), or leaving
 * the level geometry. */
static void check_respawn(void) {
    struct MarioState *m = gMarioState;
    if (gHarnessWarpRequest || m->floor == NULL || m->pos[1] < -10500.0f
        || (m->floor->type == SURFACE_DEATH_PLANE && m->pos[1] < m->floorHeight + 2048.0f)) {
        spawn_mario();
    }
}

static void player_from_mario(struct RPlayer *p) {
    struct MarioState *m = gMarioState;
    u32 act = m->action;
    p->x = (s32) m->pos[0];
    p->y = (s32) m->pos[1];
    p->z = (s32) m->pos[2];
    p->yaw = (u16) m->faceAngle[1];
    p->height = 150;
    if (act & ACT_FLAG_SHORT_HITBOX) p->height = 80;   /* crouch, crawl, slides */
    else if (act & ACT_FLAG_AIR) p->height = 170;      /* stretch in the air */
    p->lean = (s32) (m->forwardVel * 1.2f);
    /* a little step bob while walking or running on the ground */
    if ((act & ACT_GROUP_MASK) == ACT_GROUP_MOVING && m->forwardVel > 2.0f) {
        s32 ph = gSinQ14[((u32) gGlobalTimer * 0x1A00 & 0xFFFF) >> 4];
        p->y += (ph < 0 ? -ph : ph) * 7 >> 14;
    }
    if (p->lean > 48) p->lean = 48;
    if (p->lean < -24) p->lean = -24;
    p->shadowY = (s32) m->floorHeight;
    p->visible = 1;
    p->drawBody = 0; /* the animated mannequin sprites draw the body */
}

/* ---------- bench (make bench) ----------
 * The bench ROM plays a fixed tour from a key script indexed by game frame
 * (build/demo/bench_keys.c, from demo/keys/bench.keys), so every emulator
 * and every GBA renders the same scene on the same frame, and records each
 * frame in the cartridge's save RAM (demo/bench.h). */
#ifdef DEMO_BENCH
extern const u16 gBenchKeys[][3]; /* first frame, last frame, keys */
extern const u32 gNumBenchKeys, gBenchFrames;
extern const u8 __data_lma[], __data_start[], __data_end[];

/* flash carts and emulators pick the save type by finding this string */
__attribute__((used, aligned(4))) static const char kSaveType[] = "SRAM_V113";

static u32 sBenchAt30;

static u32 bench_keys(u32 frame) {
    u32 keys = 0, i;
    for (i = 0; i < gNumBenchKeys; i++) {
        if (frame >= gBenchKeys[i][0] && frame <= gBenchKeys[i][1]) keys |= gBenchKeys[i][2];
    }
    return keys;
}

/* sum of the ROM image's words (.data's load copy is the last thing in it),
 * so a report can tell which build made a .sav */
static u32 rom_checksum(void) {
    const u32 *p = (const u32 *) 0x08000000;
    const u32 *end = (const u32 *) (__data_lma + (__data_end - __data_start));
    u32 sum = 0;
    while (p < end) sum += *p++;
    return sum;
}

static void bench_start(void) {
    u32 i;
    for (i = 0; i < 8; i++) sram_put(i, (u8) BENCH_MAGIC[i], 1);
    sram_put(8, BENCH_VERSION, 1);
    sram_put(9, BENCH_RECORD, 1);
    sram_put(BENCH_FRAMES_OFS, 0, 2);
    sram_put(BENCH_STATE_OFS, BENCH_RUNNING, 1);
    sram_put(BENCH_FLAGS_OFS, sDebug ? 1 : 0, 1);
    sram_put(14, 0, 2);
    sram_put(BENCH_CHECKSUM_OFS, rom_checksum(), 4);
    for (i = BENCH_LOADS_OFS; i < BENCH_HEADER; i++) sram_put(i, 0, 1);
}

static u32 pos_hash(void) {
    u32 h = 2166136261u, k, j;
    for (k = 0; k < 3; k++) {
        union { f32 f; u32 u; } b;
        b.f = gMarioState->pos[k];
        for (j = 0; j < 32; j += 8) {
            h ^= (b.u >> j) & 0xFF;
            h *= 16777619u;
        }
    }
    return h;
}

static void bench_record(u32 frame, u32 vblanks, s32 quality, u32 physCycles, u32 renderCycles) {
    u32 at = BENCH_HEADER + frame * BENCH_RECORD;
    sram_put(at + 0, vblanks, 1);
    sram_put(at + 1, (u32) quality | (u32) sLevel << 4, 1);
    sram_put(at + 2, (u32) sLogRawX, 1);
    sram_put(at + 3, (u32) sLogRawY, 1);
    sram_put(at + 4, (u32) sLogButtons >> 8, 1);
    sram_put(at + 5, 0, 1);
    sram_put(at + 6, (u32) sLogCamYaw, 2);
    sram_put(at + 8, physCycles, 3);
    sram_put(at + 11, renderCycles, 3);
    sram_put(at + 14, 0, 2);
    sram_put(at + 16, pos_hash(), 4);
    sram_put(BENCH_FRAMES_OFS, frame + 1, 2);
    if (vblanks == 2) sBenchAt30++;
}

/* The end: the result on a black screen, and the record marked complete. */
static void bench_done(u32 frames) {
    char line[32], *p;
    const char *t;
    u32 permille = sBenchAt30 * 1000 / frames;
    sram_put(BENCH_STATE_OFS, BENCH_DONE, 1);
    while (REG_VCOUNT >= 160) {}
    while (REG_VCOUNT < 160) {}
    PAL_BG[0] = 0;
    REG_DISPCNT = DCNT_MODE4 | DCNT_OBJ | DCNT_OBJ_1D;
    mannequin_hide();
    mannequin_commit();
    hud_clear();
    hud_text(8, 8, "BENCH DONE");
    p = hud_fmt_int(line, (s32) frames, 1);
    t = " FRAMES";
    while (*t) *p++ = *t++;
    *p = 0;
    hud_text(8, 20, line);
    p = hud_fmt_int(line, (s32) (permille / 10), 1);
    *p++ = '.';
    p = hud_fmt_int(p, (s32) (permille % 10), 1);
    t = "% AT 30 FPS";
    while (*t) *p++ = *t++;
    *p = 0;
    hud_text(8, 32, line);
    hud_text(8, 44, "RESULTS IN SAVE RAM");
    while (1) {
        while (!(REG_DISPSTAT & 1)) {}
        hud_commit();
        while (REG_DISPSTAT & 1) {}
    }
}
#endif

/* ---------- main ---------- */

int main(void) {
    struct RCamera cam;
    struct RPlayer player;
    s32 quality = 0, showHud = 1;
    u32 prevKeys = 0, lastFlip, frame = 0;
    u32 windowMax = 0, windowCount = 0, goodWindows = 0, missed = 0;
    u32 fpsFlips = 0, fpsStart, fps = 30;
    u32 physCycles = 0, renderCycles = 0, mannequinCycles = 0, stackUsed = 0;
    u32 i;
    char line[256];

    REG_WAITCNT = 0x4317; /* ROM 3/1 wait states, prefetch on */
    stack_paint();
    REG_DEBUG_ENABLE = 0xC0DE;
    sDebug = REG_DEBUG_ENABLE == 0x1DEA;
#ifdef DEMO_BENCH
    bench_start();
    sDebug = 0; /* no debug lines: emulators run the same code as a GBA */
#endif

    REG_DISPCNT = 0x0080; /* forced blank while setting up */
    /* Video memory holds junk at power-on (emulators zero it): hide all 128
     * sprites (the HUD and the mannequin only ever write their own slots) and
     * clear both bitmap pages (the first is shown before it's drawn to) */
    for (i = 0; i < 128 * 4; i++) OAM[i] = (i & 3) == 0 ? 0x0200 : 0;
    fill_words((u32 *) 0x06000000, 0, 2 * 0xA000 / 4);
    hud_init();
    mannequin_init();

    /* VBlank interrupt for frame pacing */
    REG_IME = 0;
    IRQ_HANDLER_ADDR = (u32) irq_handler;
    REG_DISPSTAT |= 0x0008;
    REG_IE = 0x0001;
    REG_IME = 1;

    timer_start();

    setup_dma_table_list(&gMarioAnimsBuf, (void *) gMarioAnims, sMarioAnimBuffer);
    alloc_surface_pools();
    sLevel = 0;
    load_level(sLevel);

    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
    hud_clear();
    lastFlip = gVBlankCount;
    fpsStart = gVBlankCount;

    for (;;) {
#ifdef DEMO_BENCH
        u32 keys = frame < gBenchFrames ? bench_keys(frame) : 0;
#else
        u32 keys = ~REG_KEYINPUT & 0x3FF;
#endif
        u32 pressed = keys & ~prevKeys;
        struct RQuality qv = kQuality[quality];
        const struct RQuality *q = &qv;
        u16 *back = (u16 *) ((REG_DISPCNT & DCNT_PAGE) ? 0x06000000 : 0x0600A000);
        u32 t0, t1, t2, tm, work, vblanks;
        prevKeys = keys;
#ifdef DEMO_BENCH
        if (frame == gBenchFrames) bench_done(frame);
#endif

        if (pressed & KEY_START) {
            sLevel = (sLevel + 1) % gNumHarnessLevels;
            load_level(sLevel);
            REG_DISPCNT = DCNT_MODE4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
            hud_clear();
            quality = 0;
            lastFlip = gVBlankCount;
            fpsStart = gVBlankCount;
            fpsFlips = 0;
            windowMax = windowCount = 0;
            continue;
        }
        if (pressed & KEY_SELECT) showHud = !showHud;

        t0 = cycles_now();
        mario_frame(keys, pressed);
        check_respawn();
        t1 = cycles_now();
        camera_update(gMarioState, keys, &cam);
        player_from_mario(&player);
#ifdef DEMO_IMPORTED
        /* Mario's own model, posed by the game's animation and drawn by the
         * renderer inside the scene */
        tm = cycles_now();
        player.frames = mannequin_pose(gMarioObject) ? mannequin_frames() : 0;
        mannequinCycles = cycles_now() - tm;
        /* the visual meshes have no T-junction pinholes to hide (spans are
         * widened), and below the horizon may be open sky (WF is an island):
         * continue the sky's horizon color */
        qv.groundColor = PAL_SKY + 15;
        render_frame(back, &cam, &player, q);
        t2 = cycles_now();
#else
        player.frames = 0;
        qv.groundColor = kGroundFill[sLevel];
        render_frame(back, &cam, &player, q);
        tm = cycles_now();
        mannequin_update(gMarioObject, &cam);
        t2 = cycles_now();
        mannequinCycles = t2 - tm;
#endif
        physCycles = t1 - t0;
        renderCycles = t2 - t1;
        work = t2 - t0;

        /* 30 Hz: flip on the 2nd VBlank after the last flip, or on the next
         * one if this frame ran long */
        if (gVBlankCount - lastFlip >= 2) {
            u32 c = gVBlankCount;
            while (gVBlankCount == c) {}
            missed++;
        } else {
            while (gVBlankCount - lastFlip < 2) {}
        }
        vblanks = gVBlankCount - lastFlip;
        lastFlip = gVBlankCount;
        REG_DISPCNT ^= DCNT_PAGE;
        REG_BG2PA = (u16) ((q->width << 8) / 240);
        REG_BG2PB = 0;
        REG_BG2PC = 0;
        REG_BG2PD = (u16) ((q->height << 8) / 160);
        REG_BG2X = 0;
        REG_BG2Y = 0;
        hud_commit();
        mannequin_commit();
#ifdef DEMO_BENCH
        bench_record(frame, vblanks, quality, physCycles, renderCycles);
#endif

        /* fps over the last second */
        fpsFlips++;
        if (gVBlankCount - fpsStart >= 60) {
            fps = fpsFlips * 60 / (gVBlankCount - fpsStart);
            fpsFlips = 0;
            fpsStart = gVBlankCount;
        }

        /* Governor: any frame over budget steps quality down right away; it
         * steps back up only after 45 frames (1.5 s) whose worst frame left
         * 35% headroom, so the next step up (~25% more work) still fits. */
        if (work > FRAME_BUDGET - FRAME_BUDGET / 32) {
            if (quality < NUM_QUALITY - 1) quality++;
            windowMax = 0;
            windowCount = 0;
        } else {
            if (work > windowMax) windowMax = work;
            if (++windowCount == 45) {
                if (windowMax < FRAME_BUDGET * 65 / 100 && quality > 0) quality--;
                windowMax = 0;
                windowCount = 0;
            }
        }
        (void) goodWindows;

        /* HUD for the next flip (text refreshed every 8 frames) */
        if ((frame & 7) == 0 || !showHud) hud_clear();
        if (showHud && (frame & 7) == 0) {
            char *p = line;
            p = hud_fmt_int(p, (s32) fps, 2);
            *p++ = 'F'; *p++ = 'P'; *p++ = 'S'; *p++ = ' '; *p++ = 'Q';
            p = hud_fmt_int(p, NUM_QUALITY - quality, 1);
            *p++ = ' ';
            p = hud_fmt_int(p, q->width, 3);
            *p = 0;
            hud_text(2, 2, line);
            p = line;
            *p++ = 'M';
            p = hud_fmt_int(p, (s32) (physCycles * 100 / FRAME_BUDGET), 3);
            *p++ = '%'; *p++ = ' '; *p++ = 'R';
            p = hud_fmt_int(p, (s32) (renderCycles * 100 / FRAME_BUDGET), 3);
            *p++ = '%';
            *p = 0;
            hud_text(2, 10, line);
            hud_text(240 - 5 * 5 - 2, 2, kLevelLabels[sLevel]);
        }

        /* stats for the mGBA runner, in hex (no divisions): "key value" pairs */
        if (sDebug) {
            static const char hex[] = "0123456789abcdef";
            const struct { char key; u32 value; } kv[] = {
                { 'f', frame }, { 'v', vblanks }, { 'p', physCycles }, { 'r', renderCycles },
                { 'q', (u32) quality }, { 't', gRStats.trisDrawn }, { 'a', gMarioState->action },
                { 'l', (u32) sLevel }, { 'c', gRStats.cyclesClear }, { 's', gRStats.cyclesSetup },
                { 'd', gRStats.cyclesDraw }, { 'k', gRStats.trisConsidered }, { 'h', gRStats.chunksDrawn },
                { 'x', gRStats.vertsTransformed }, { 'y', gRStats.clipped }, { 'm', mannequinCycles }, { 'w', stackUsed },
                { 'n', gRStats.marioTris }, { 'o', gRStats.occluders }, { 'u', gRStats.cyclesMario },
                /* the camera, to replay a frame's view (src/test/render_check.c) */
                { 'b', (u32) cam.x }, { 'e', (u32) cam.y }, { 'g', (u32) cam.z },
                { 'i', cam.yaw }, { 'j', cam.pitch },
                /* the keys read this frame (demo/keys/bench.keys was made from them) */
                { 'z', keys },
            };
            u32 k;
            s32 j;
            char *p = line;
            for (k = 0; k < sizeof(kv) / sizeof(kv[0]); k++) {
                if (k) *p++ = ' ';
                *p++ = kv[k].key;
                *p++ = ' ';
                for (j = 28; j > 0 && !(kv[k].value >> j); j -= 4) {}
                for (; j >= 0; j -= 4) *p++ = hex[(kv[k].value >> j) & 15];
            }
            *p = 0;
            debug_line(line);
            /* "i rawX rawY buttons camYaw | posX posY posZ": inputs fed to Mario
             * (decimal stick and yaw, hex buttons) and his position (float bits) */
            {
                s32 jj;
                p = line;
                *p++ = 'i'; *p++ = ' ';
                p = hud_fmt_int(p, sLogRawX, 1); *p++ = ' ';
                p = hud_fmt_int(p, sLogRawY, 1); *p++ = ' ';
                for (jj = 12; jj >= 0; jj -= 4) *p++ = hex[(sLogButtons >> jj) & 15];
                *p++ = ' ';
                p = hud_fmt_int(p, sLogCamYaw, 1);
                *p++ = ' '; *p++ = '|';
                for (k = 0; k < 3; k++) {
                    union { f32 f; u32 u; } b;
                    b.f = gMarioState->pos[k];
                    *p++ = ' ';
                    for (jj = 28; jj >= 0; jj -= 4) *p++ = hex[(b.u >> jj) & 15];
                }
                *p = 0;
                debug_line(line);
            }
        }
        if ((frame & 63) == 0) stackUsed = stack_used();
        frame++;
        (void) missed;
    }
}
