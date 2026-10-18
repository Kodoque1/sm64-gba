/*
 * Engine stubs for the Mario movement core.
 *
 * Everything Mario's code reaches outside of movement + collision lands here.
 * Rule of thumb: anything that can change Mario's physical state is either
 * implemented faithfully (copied from the decomp, see anim.c) or deliberately
 * absent (objects, interactions, warps). Pure side effects (sound, HUD,
 * dialog, save file, camera) are no-ops.
 */
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "sm64.h"
#include "types.h"
#include "engine/graph_node.h"
#include "engine/surface_collision.h"
#include "game/area.h"
#include "game/camera.h"
#include "game/game_init.h"
#include "game/level_update.h"
#include "game/mario_misc.h"
#include "game/memory.h"
#include "game/object_list_processor.h"
#include "game/save_file.h"
#include "game/ingame_menu.h"
#include "audio/external.h"

/* ---------- globals normally owned by files we don't compile ---------- */

u16 gAreaUpdateCounter;
u32 gGlobalTimer;
u32 gAudioRandom;
struct MarioBodyState gBodyStates[2];
s16 gCCMEnteredSlide;
struct Camera *gCamera;
s16 gCameraMovementFlags;
s16 gCheckingSurfaceCollisionsForCamera;
struct Controller gControllers[3];
s16 gCurrAreaIndex;
struct CreditsEntry *gCurrCreditsEntry;
s16 gCurrLevelNum;
s16 gCurrSaveFileNum = 1;
struct Area *gCurrentArea;
struct Object *gCurrentObject;
struct Object *gCutsceneFocus;
s8 gDebugLevelSelect;
s32 gDialogResponse;
s32 gEnvironmentLevels[20];
TerrainData *gEnvironmentRegions;
s16 gFindFloorIncludeSurfaceIntangible;
f32 gGlobalSoundSource[3];
struct HudDisplay gHudDisplay;
u8 gLastCompletedCourseNum;
u8 gLastCompletedStarNum;
struct DmaHandlerList gMarioAnimsBuf;
struct Object *gMarioObject;
struct SpawnInfo *gMarioSpawnInfo;
struct MarioState *gMarioState;
s8 gNeverEnteredCastle;
struct NumTimesCalled gNumCalls;
s32 gNumFindFloorMisses;
s32 gNumStaticSurfaceNodes;
s32 gNumStaticSurfaces;
f32 gPaintingMarioYEntry;
struct Controller *gPlayer1Controller;
struct PlayerCameraState gPlayerCameraState[2];
struct SpawnInfo gPlayerSpawnInfos[1];
s16 gSaveOptSelectIndex;
s8 gShowDebugText;
u8 gSpecialTripleJump;
s32 gSurfaceNodesAllocated;
s32 gSurfacesAllocated;
u32 gTimeStopState;
Vec3f gVec3fZero = { 0.0f, 0.0f, 0.0f };

/* Special-object parsing (macro_special_objects.c) needs these. */
struct Object gMacroObjectDefaultParent;
struct Object *gMacroObjectDefaultParentPtr;

/* ---------- memory ---------- */

/* Surface pools: alloc_surface_pools() always takes 7000 nodes (8 B) and
 * 2300 surfaces (48 B) on a 32-bit target = 166,400 B. That is 65% of the
 * GBA's 256 KB EWRAM before anything else, so it is a budget to watch. */
#ifndef POOL_BYTES
#define POOL_BYTES (168 * 1024)
#endif
static u8 sMainPool[POOL_BYTES] __attribute__((aligned(16)));
static u32 sMainPoolUsed;
u32 gMainPoolHighWater;

void *main_pool_alloc(u32 size, UNUSED u32 side) {
    void *p;
    size = (size + 15) & ~15u;
    if (sMainPoolUsed + size > POOL_BYTES) {
        return NULL; /* surface_load would crash on this: size the pool up */
    }
    p = &sMainPool[sMainPoolUsed];
    sMainPoolUsed += size;
    gMainPoolHighWater = sMainPoolUsed;
    return p;
}

void *segmented_to_virtual(const void *addr) { return (void *) addr; }
void *virtual_to_segmented(UNUSED u32 segment, const void *addr) { return (void *) addr; }

/* ---------- objects: spawned into a sink, never simulated ---------- */

static struct Object sObjectSink;

struct Object *spawn_object(UNUSED struct Object *parent, UNUSED s32 model,
                            UNUSED const BehaviorScript *behavior) {
    return &sObjectSink;
}

struct Object *spawn_object_abs_with_rot(UNUSED struct Object *parent, UNUSED s16 uselessArg,
                                         UNUSED u32 model, UNUSED const BehaviorScript *behavior,
                                         UNUSED s16 x, UNUSED s16 y, UNUSED s16 z, UNUSED s16 rx,
                                         UNUSED s16 ry, UNUSED s16 rz) {
    return &sObjectSink;
}

void obj_mark_for_deletion(UNUSED struct Object *obj) {}
void obj_scale(UNUSED struct Object *obj, UNUSED f32 scale) {}
void obj_apply_scale_to_matrix(UNUSED struct Object *obj, UNUSED Mat4 dst, UNUSED Mat4 src) {}
void obj_build_transform_from_pos_and_angle(UNUSED struct Object *obj, UNUSED s16 posIndex,
                                            UNUSED s16 angleIndex) {}
s32 cur_obj_check_if_near_animation_end(void) { return 0; }
void cur_obj_init_animation_with_sound(UNUSED s32 animIndex) {}
void enable_time_stop(void) {}
void disable_time_stop(void) {}
void spawn_wind_particles(UNUSED s16 pitch, UNUSED s16 yaw) {}

f32 dist_between_objects(UNUSED struct Object *obj1, UNUSED struct Object *obj2) { return 0.0f; }

/* ---------- interactions: no objects, so nothing to interact with ---------- */

void mario_blow_off_cap(UNUSED struct MarioState *m, UNUSED f32 capSpeed) {}
u32 mario_check_object_grab(UNUSED struct MarioState *m) { return FALSE; }
void mario_drop_held_object(UNUSED struct MarioState *m) {}
struct Object *mario_get_collided_object(UNUSED struct MarioState *m, UNUSED u32 interactType) {
    return NULL;
}
void mario_grab_used_object(UNUSED struct MarioState *m) {}
void mario_handle_special_floors(UNUSED struct MarioState *m) {}
s16 mario_obj_angle_to_object(UNUSED struct MarioState *m, UNUSED struct Object *o) { return 0; }
void mario_process_interactions(UNUSED struct MarioState *m) {}
void mario_stop_riding_and_holding(UNUSED struct MarioState *m) {}
void mario_stop_riding_object(UNUSED struct MarioState *m) {}
void mario_throw_held_object(UNUSED struct MarioState *m) {}
u32 get_door_save_file_flag(UNUSED struct Object *door) { return 0; }

/* ---------- camera ---------- */

/* Copied from camera.c: pure math, kept exact in case a cutscene path uses it. */
f32 camera_approach_f32_symmetric(f32 current, f32 target, f32 increment) {
    f32 dist = target - current;

    if (increment < 0) {
        increment = -1 * increment;
    }
    if (dist > 0) {
        dist -= increment;
        if (dist > 0) {
            current = target - dist;
        } else {
            current = target;
        }
    } else {
        dist += increment;
        if (dist < 0) {
            current = target - dist;
        } else {
            current = target;
        }
    }
    return current;
}

void set_camera_mode(UNUSED struct Camera *c, UNUSED s16 mode, UNUSED s16 frames) {}
void set_camera_shake_from_hit(UNUSED s16 shake) {}
s32 trigger_cutscene_dialog(UNUSED s32 trigger) { return 0; }

/* ---------- level flow, warps, save file ---------- */

void fade_into_special_warp(UNUSED u32 arg, UNUSED u32 color) {}
/* Warps (death, level exits) aren't simulated. The demo reads this to respawn
 * Mario; the parity harness ignores it. Nothing in Mario's code reads it. */
s32 gHarnessWarpRequest;
s16 level_trigger_warp(UNUSED struct MarioState *m, s32 warpOp) {
    gHarnessWarpRequest = warpOp | 0x10000;
    return 0;
}
void load_level_init_text(UNUSED u32 arg) {}
void play_transition(UNUSED s16 transType, UNUSED s16 time, UNUSED u8 red, UNUSED u8 green,
                     UNUSED u8 blue) {}
void override_viewport_and_clip(UNUSED Vp *a, UNUSED Vp *b, UNUSED u8 c, UNUSED u8 d,
                                UNUSED u8 e) {}
void reset_red_coins_collected(void) {}

void save_file_clear_flags(UNUSED u32 flags) {}
void save_file_do_save(UNUSED s32 fileIndex) {}
s32 save_file_get_cap_pos(UNUSED Vec3s capPos) { return FALSE; }
u32 save_file_get_flags(void) { return 0; }
s32 save_file_get_total_star_count(UNUSED s32 fileIndex, UNUSED s32 minCourse,
                                   UNUSED s32 maxCourse) {
    return 0;
}
void save_file_set_flags(UNUSED u32 flags) {}

/* ---------- dialog, HUD, debug text ---------- */

void create_dialog_box(UNUSED s16 dialog) {}
void create_dialog_box_with_response(UNUSED s16 dialog) {}
void create_dialog_box_with_var(UNUSED s16 dialog, UNUSED s32 dialogVar) {}
void create_dialog_inverted_box(UNUSED s16 dialog) {}
s16 get_dialog_id(void) { return -1; }
void dl_rgba16_begin_cutscene_msg_fade(void) {}
void dl_rgba16_stop_cutscene_msg_fade(void) {}
void reset_cutscene_msg_fade(void) {}
void set_cutscene_message(UNUSED s16 xOffset, UNUSED s16 yOffset, UNUSED s16 msgIndex,
                          UNUSED s16 msgDuration) {}
void set_menu_mode(UNUSED s16 mode) {}
void print_credits_str_ascii(UNUSED s16 x, UNUSED s16 y, UNUSED const char *str) {}
void print_debug_top_down_mapinfo(UNUSED const char *str, UNUSED s32 number) {}
void print_text_fmt_int(UNUSED s32 x, UNUSED s32 y, UNUSED const char *str, UNUSED s32 n) {}
void set_text_array_x_y(UNUSED s32 xOffset, UNUSED s32 yOffset) {}
void guMtxF2L(UNUSED float mf[4][4], UNUSED Mtx *m) {}

/* ---------- audio ---------- */

void play_sound(UNUSED s32 soundBits, UNUSED f32 *pos) {}
void stop_sound(UNUSED u32 soundBits, UNUSED f32 *pos) {}
void play_music(UNUSED u8 player, UNUSED u16 seqArgs, UNUSED u16 fadeTimer) {}
void play_course_clear(void) {}
void play_peachs_jingle(void) {}
void play_cutscene_music(UNUSED u16 seqArgs) {}
void play_infinite_stairs_music(void) {}
void play_shell_music(void) {}
void stop_shell_music(void) {}
void stop_cap_music(void) {}
void fadeout_cap_music(void) {}
void enable_background_sound(void) {}
void disable_background_sound(void) {}
void lower_background_noise(UNUSED s32 a) {}
void raise_background_noise(UNUSED s32 a) {}
void seq_player_lower_volume(UNUSED u8 player, UNUSED u16 fadeDuration, UNUSED u8 percentage) {}
void seq_player_unlower_volume(UNUSED u8 player, UNUSED u16 fadeDuration) {}
void set_sound_moving_speed(UNUSED u8 bank, UNUSED u8 speed) {}
void sound_banks_enable(UNUSED u8 player, UNUSED u16 bankMask) {}

/* Memory report for sizing the GBA build. */
u32 harness_pool_used(void) { return sMainPoolUsed; }
