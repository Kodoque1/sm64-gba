/* Soft-float call counter for the ARM build (linked with -Wl,--wrap=...).
 * Generated from src/fp_wraps.txt by hand-kept signatures. */
#include <stdio.h>
#include "platform.h"

static int sInCollision;
static unsigned long sCollisionCalls;
#define NOTE() (sCollisionCalls += sInCollision > 0)

static unsigned long c_fadd;
float __real___aeabi_fadd(float a, float b);
float __wrap___aeabi_fadd(float a, float b) { c_fadd++; NOTE(); return __real___aeabi_fadd(a, b); }
static unsigned long c_fsub;
float __real___aeabi_fsub(float a, float b);
float __wrap___aeabi_fsub(float a, float b) { c_fsub++; NOTE(); return __real___aeabi_fsub(a, b); }
static unsigned long c_fmul;
float __real___aeabi_fmul(float a, float b);
float __wrap___aeabi_fmul(float a, float b) { c_fmul++; NOTE(); return __real___aeabi_fmul(a, b); }
static unsigned long c_fdiv;
float __real___aeabi_fdiv(float a, float b);
float __wrap___aeabi_fdiv(float a, float b) { c_fdiv++; NOTE(); return __real___aeabi_fdiv(a, b); }
static unsigned long c_fcmpeq;
int __real___aeabi_fcmpeq(float a, float b);
int __wrap___aeabi_fcmpeq(float a, float b) { c_fcmpeq++; NOTE(); return __real___aeabi_fcmpeq(a, b); }
static unsigned long c_fcmplt;
int __real___aeabi_fcmplt(float a, float b);
int __wrap___aeabi_fcmplt(float a, float b) { c_fcmplt++; NOTE(); return __real___aeabi_fcmplt(a, b); }
static unsigned long c_fcmple;
int __real___aeabi_fcmple(float a, float b);
int __wrap___aeabi_fcmple(float a, float b) { c_fcmple++; NOTE(); return __real___aeabi_fcmple(a, b); }
static unsigned long c_fcmpgt;
int __real___aeabi_fcmpgt(float a, float b);
int __wrap___aeabi_fcmpgt(float a, float b) { c_fcmpgt++; NOTE(); return __real___aeabi_fcmpgt(a, b); }
static unsigned long c_fcmpge;
int __real___aeabi_fcmpge(float a, float b);
int __wrap___aeabi_fcmpge(float a, float b) { c_fcmpge++; NOTE(); return __real___aeabi_fcmpge(a, b); }
static unsigned long c_i2f;
float __real___aeabi_i2f(int a);
float __wrap___aeabi_i2f(int a) { c_i2f++; NOTE(); return __real___aeabi_i2f(a); }
static unsigned long c_ui2f;
float __real___aeabi_ui2f(unsigned a);
float __wrap___aeabi_ui2f(unsigned a) { c_ui2f++; NOTE(); return __real___aeabi_ui2f(a); }
static unsigned long c_f2iz;
int __real___aeabi_f2iz(float a);
int __wrap___aeabi_f2iz(float a) { c_f2iz++; NOTE(); return __real___aeabi_f2iz(a); }
static unsigned long c_f2d;
double __real___aeabi_f2d(float a);
double __wrap___aeabi_f2d(float a) { c_f2d++; NOTE(); return __real___aeabi_f2d(a); }
static unsigned long c_d2f;
float __real___aeabi_d2f(double a);
float __wrap___aeabi_d2f(double a) { c_d2f++; NOTE(); return __real___aeabi_d2f(a); }
static unsigned long c_dadd;
double __real___aeabi_dadd(double a, double b);
double __wrap___aeabi_dadd(double a, double b) { c_dadd++; NOTE(); return __real___aeabi_dadd(a, b); }
static unsigned long c_dsub;
double __real___aeabi_dsub(double a, double b);
double __wrap___aeabi_dsub(double a, double b) { c_dsub++; NOTE(); return __real___aeabi_dsub(a, b); }
static unsigned long c_dmul;
double __real___aeabi_dmul(double a, double b);
double __wrap___aeabi_dmul(double a, double b) { c_dmul++; NOTE(); return __real___aeabi_dmul(a, b); }
static unsigned long c_ddiv;
double __real___aeabi_ddiv(double a, double b);
double __wrap___aeabi_ddiv(double a, double b) { c_ddiv++; NOTE(); return __real___aeabi_ddiv(a, b); }
static unsigned long c_dcmplt;
int __real___aeabi_dcmplt(double a, double b);
int __wrap___aeabi_dcmplt(double a, double b) { c_dcmplt++; NOTE(); return __real___aeabi_dcmplt(a, b); }
static unsigned long c_dcmpgt;
int __real___aeabi_dcmpgt(double a, double b);
int __wrap___aeabi_dcmpgt(double a, double b) { c_dcmpgt++; NOTE(); return __real___aeabi_dcmpgt(a, b); }
static unsigned long c_sqrtf;
float __real_sqrtf(float a);
float __wrap_sqrtf(float a) { c_sqrtf++; NOTE(); return __real_sqrtf(a); }
static unsigned long c_idiv;
int __real___aeabi_idiv(int a, int b);
int __wrap___aeabi_idiv(int a, int b) { c_idiv++; NOTE(); return __real___aeabi_idiv(a, b); }
static unsigned long c_idivmod;
unsigned long long __real___aeabi_idivmod(int a, int b);
unsigned long long __wrap___aeabi_idivmod(int a, int b) { c_idivmod++; NOTE(); return __real___aeabi_idivmod(a, b); }

struct Surface;
struct WallCollisionData;
int __real_f32_find_wall_collision(float *x, float *y, float *z, float offsetY, float radius);
int __wrap_f32_find_wall_collision(float *x, float *y, float *z, float offsetY, float radius) {
    int r; sInCollision++; r = __real_f32_find_wall_collision(x, y, z, offsetY, radius); sInCollision--; return r;
}
int __real_find_wall_collisions(struct WallCollisionData *d);
int __wrap_find_wall_collisions(struct WallCollisionData *d) {
    int r; sInCollision++; r = __real_find_wall_collisions(d); sInCollision--; return r;
}
float __real_find_ceil(float x, float y, float z, struct Surface **c);
float __wrap_find_ceil(float x, float y, float z, struct Surface **c) {
    float r; sInCollision++; r = __real_find_ceil(x, y, z, c); sInCollision--; return r;
}
float __real_find_floor(float x, float y, float z, struct Surface **f);
float __wrap_find_floor(float x, float y, float z, struct Surface **f) {
    float r; sInCollision++; r = __real_find_floor(x, y, z, f); sInCollision--; return r;
}
float __real_find_floor_height(float x, float y, float z);
float __wrap_find_floor_height(float x, float y, float z) {
    float r; sInCollision++; r = __real_find_floor_height(x, y, z); sInCollision--; return r;
}
float __real_find_water_level(float x, float z);
float __wrap_find_water_level(float x, float z) {
    float r; sInCollision++; r = __real_find_water_level(x, z); sInCollision--; return r;
}

void plat_finish(void) {
    unsigned long total = 0;
    total += c_fadd;
    total += c_fsub;
    total += c_fmul;
    total += c_fdiv;
    total += c_fcmpeq;
    total += c_fcmplt;
    total += c_fcmple;
    total += c_fcmpgt;
    total += c_fcmpge;
    total += c_i2f;
    total += c_ui2f;
    total += c_f2iz;
    total += c_f2d;
    total += c_d2f;
    total += c_dadd;
    total += c_dsub;
    total += c_dmul;
    total += c_ddiv;
    total += c_dcmplt;
    total += c_dcmpgt;
    total += c_sqrtf;
    total += c_idiv;
    total += c_idivmod;
    fflush(stdout);
    if (c_fadd) fprintf(stderr, "  %-8s %10lu\n", "fadd", c_fadd);
    if (c_fsub) fprintf(stderr, "  %-8s %10lu\n", "fsub", c_fsub);
    if (c_fmul) fprintf(stderr, "  %-8s %10lu\n", "fmul", c_fmul);
    if (c_fdiv) fprintf(stderr, "  %-8s %10lu\n", "fdiv", c_fdiv);
    if (c_fcmpeq) fprintf(stderr, "  %-8s %10lu\n", "fcmpeq", c_fcmpeq);
    if (c_fcmplt) fprintf(stderr, "  %-8s %10lu\n", "fcmplt", c_fcmplt);
    if (c_fcmple) fprintf(stderr, "  %-8s %10lu\n", "fcmple", c_fcmple);
    if (c_fcmpgt) fprintf(stderr, "  %-8s %10lu\n", "fcmpgt", c_fcmpgt);
    if (c_fcmpge) fprintf(stderr, "  %-8s %10lu\n", "fcmpge", c_fcmpge);
    if (c_i2f) fprintf(stderr, "  %-8s %10lu\n", "i2f", c_i2f);
    if (c_ui2f) fprintf(stderr, "  %-8s %10lu\n", "ui2f", c_ui2f);
    if (c_f2iz) fprintf(stderr, "  %-8s %10lu\n", "f2iz", c_f2iz);
    if (c_f2d) fprintf(stderr, "  %-8s %10lu\n", "f2d", c_f2d);
    if (c_d2f) fprintf(stderr, "  %-8s %10lu\n", "d2f", c_d2f);
    if (c_dadd) fprintf(stderr, "  %-8s %10lu\n", "dadd", c_dadd);
    if (c_dsub) fprintf(stderr, "  %-8s %10lu\n", "dsub", c_dsub);
    if (c_dmul) fprintf(stderr, "  %-8s %10lu\n", "dmul", c_dmul);
    if (c_ddiv) fprintf(stderr, "  %-8s %10lu\n", "ddiv", c_ddiv);
    if (c_dcmplt) fprintf(stderr, "  %-8s %10lu\n", "dcmplt", c_dcmplt);
    if (c_dcmpgt) fprintf(stderr, "  %-8s %10lu\n", "dcmpgt", c_dcmpgt);
    if (c_sqrtf) fprintf(stderr, "  %-8s %10lu\n", "sqrtf", c_sqrtf);
    if (c_idiv) fprintf(stderr, "  %-8s %10lu\n", "idiv", c_idiv);
    if (c_idivmod) fprintf(stderr, "  %-8s %10lu\n", "idivmod", c_idivmod);
    fprintf(stderr, "  total    %10lu\n", total);
    fprintf(stderr, "  inside collision queries: %lu (%.0f%%)\n", sCollisionCalls, total ? 100.0 * sCollisionCalls / total : 0.0);
}
