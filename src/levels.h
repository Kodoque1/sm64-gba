#ifndef HARNESS_LEVELS_H
#define HARNESS_LEVELS_H

#include <PR/ultratypes.h>
#include "types.h"

struct HarnessLevel {
    const char *name;
    const Collision *collision;
    s16 spawnPos[3];
    s16 spawnYawDegrees;
    s16 terrainType;
};

extern const struct HarnessLevel gHarnessLevels[];
extern const s32 gNumHarnessLevels;

#endif
