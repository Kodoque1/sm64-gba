/*
 * Collision cost profiler for the GBA build (linked with -Wl,--wrap=...).
 *
 * Times every collision query from Mario's code with timers 0+1 cascaded
 * (full CPU clock), separately from the per-frame timer on 2+3. Nested
 * queries are only timed at the outermost level. platform_gba.c, built with
 * -DGBA_PROF, prints the per-frame totals next to the frame's cycle count.
 */
typedef unsigned short u16;
typedef unsigned int u32;

#define REG16(a) (*(volatile u16 *) (a))
#define REG_TM0CNT_L REG16(0x04000100)
#define REG_TM0CNT_H REG16(0x04000102)
#define REG_TM1CNT_L REG16(0x04000104)
#define REG_TM1CNT_H REG16(0x04000106)

enum { K_WALL, K_FLOOR, K_CEIL, K_WATER, K_COUNT };

u32 gProfCycles[K_COUNT];
u32 gProfCalls[K_COUNT];
static int sDepth;

static void t_start(void) {
    if (sDepth++ == 0) {
        REG_TM0CNT_H = 0;
        REG_TM1CNT_H = 0;
        REG_TM0CNT_L = 0;
        REG_TM1CNT_L = 0;
        REG_TM1CNT_H = 0x0084;
        REG_TM0CNT_H = 0x0080;
    }
}

static void t_stop(int kind) {
    if (--sDepth == 0) {
        REG_TM0CNT_H = 0;
        gProfCycles[kind] += ((u32) REG_TM1CNT_L << 16) | REG_TM0CNT_L;
        gProfCalls[kind]++;
    }
}

struct Surface;
struct WallCollisionData;

int __real_f32_find_wall_collision(float *x, float *y, float *z, float offsetY, float radius);
int __wrap_f32_find_wall_collision(float *x, float *y, float *z, float offsetY, float radius) {
    int r;
    t_start();
    r = __real_f32_find_wall_collision(x, y, z, offsetY, radius);
    t_stop(K_WALL);
    return r;
}

int __real_find_wall_collisions(struct WallCollisionData *d);
int __wrap_find_wall_collisions(struct WallCollisionData *d) {
    int r;
    t_start();
    r = __real_find_wall_collisions(d);
    t_stop(K_WALL);
    return r;
}

float __real_find_floor(float x, float y, float z, struct Surface **f);
float __wrap_find_floor(float x, float y, float z, struct Surface **f) {
    float r;
    t_start();
    r = __real_find_floor(x, y, z, f);
    t_stop(K_FLOOR);
    return r;
}

float __real_find_floor_height(float x, float y, float z);
float __wrap_find_floor_height(float x, float y, float z) {
    float r;
    t_start();
    r = __real_find_floor_height(x, y, z);
    t_stop(K_FLOOR);
    return r;
}

float __real_find_ceil(float x, float y, float z, struct Surface **c);
float __wrap_find_ceil(float x, float y, float z, struct Surface **c) {
    float r;
    t_start();
    r = __real_find_ceil(x, y, z, c);
    t_stop(K_CEIL);
    return r;
}

float __real_find_water_level(float x, float z);
float __wrap_find_water_level(float x, float z) {
    float r;
    t_start();
    r = __real_find_water_level(x, z);
    t_stop(K_WATER);
    return r;
}

/* Writes " wall floor ceil water" cycle totals and call counts, then resets. */
int prof_frame_report(char *out) {
    int n = 0, i;
    for (i = 0; i < K_COUNT * 2; i++) {
        u32 v = i < K_COUNT ? gProfCycles[i] : gProfCalls[i - K_COUNT];
        char tmp[12];
        int k = 0;
        out[n++] = ' ';
        do {
            tmp[k++] = (char) ('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (k > 0) {
            out[n++] = tmp[--k];
        }
    }
    for (i = 0; i < K_COUNT; i++) {
        gProfCycles[i] = 0;
        gProfCalls[i] = 0;
    }
    return n;
}
