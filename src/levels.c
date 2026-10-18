/*
 * Level collision, straight from the decomp's levels/ directory (these files
 * are in the decomp repo as source; no ROM extraction needed). Spawn points
 * and terrain types are copied from each level's script.c.
 */
#include <PR/ultratypes.h>
#include "sm64.h"
#include "types.h"
#include "surface_terrains.h"
#include "level_misc_macros.h"
#include "macro_presets.h"
#include "special_presets.h"
#include "levels.h"

#include "levels/bob/areas/1/collision.inc.c"
#include "levels/wf/areas/1/collision.inc.c"
#include "levels/ccm/areas/1/collision.inc.c"
#include "levels/bitdw/areas/1/collision.inc.c"

const struct HarnessLevel gHarnessLevels[] = {
    /* name     collision                     spawn x, y, z          yaw  terrain */
    { "bob",   bob_seg7_collision_level,    { -6558,     0,  6464 }, 135, TERRAIN_GRASS },
    { "wf",    wf_seg7_collision_070102D8,  {  2600,   256,  5120 },  90, TERRAIN_STONE },
    { "ccm",   ccm_seg7_area_1_collision,   { -1512,  2560, -2305 }, 140, TERRAIN_SNOW },
    { "bitdw", bitdw_seg7_collision_level,  { -7443, -3153,  3886 },  90, TERRAIN_STONE },
};

const s32 gNumHarnessLevels = sizeof(gHarnessLevels) / sizeof(gHarnessLevels[0]);
