/*
 * GBA platform layer.
 *   input  : the .inp script, embedded in ROM as a string (build/gba/<name>_input.c)
 *   output : mGBA's debug-print registers; INFO = the parity log, WARN = side channel
 *   timing : timers 2+3 cascaded at the full 16.78 MHz clock, around each frame
 */
#include "platform.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define REG16(a) (*(volatile u16 *) (a))
#define REG_WAITCNT REG16(0x04000204)
#define REG_TM2CNT_L REG16(0x04000108)
#define REG_TM2CNT_H REG16(0x0400010A)
#define REG_TM3CNT_L REG16(0x0400010C)
#define REG_TM3CNT_H REG16(0x0400010E)

#define REG_DEBUG_ENABLE REG16(0x04FFF780)
#define REG_DEBUG_FLAGS REG16(0x04FFF700)
#define DEBUG_STRING ((volatile char *) 0x04FFF600)
#define DEBUG_LEVEL_WARN 2
#define DEBUG_LEVEL_INFO 3

extern const char gInputScript[];
static const char *sInputPos = gInputScript;

static char sOut[256];
static int sOutLen;

static void debug_emit(const char *s, int n, int level) {
    int i;
    if (n > 255) {
        n = 255;
    }
    for (i = 0; i < n; i++) {
        DEBUG_STRING[i] = s[i];
    }
    DEBUG_STRING[n] = '\0';
    REG_DEBUG_FLAGS = (u16) (level | 0x100);
}

void plat_init(void) {
    /* Commercial-game setting: ROM 3/1 waitstates, prefetch on. */
    REG_WAITCNT = 0x4317;
    REG_DEBUG_ENABLE = 0xC0DE;
}

int plat_read_line(char *buf, int max) {
    int n = 0;
    if (*sInputPos == '\0') {
        return -1;
    }
    while (*sInputPos != '\0' && *sInputPos != '\n') {
        if (n < max - 1) {
            buf[n++] = *sInputPos;
        }
        sInputPos++;
    }
    if (*sInputPos == '\n') {
        sInputPos++;
    }
    buf[n] = '\0';
    return n;
}

void plat_write(const char *s, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\n') {
            debug_emit(sOut, sOutLen, DEBUG_LEVEL_INFO);
            sOutLen = 0;
        } else if (sOutLen < (int) sizeof(sOut) - 1) {
            sOut[sOutLen++] = s[i];
        }
    }
}

void plat_write_err(const char *s, int n) {
    while (n > 0 && s[n - 1] == '\n') {
        n--;
    }
    debug_emit(s, n, DEBUG_LEVEL_WARN);
}

void plat_frame_begin(void) {
    REG_TM2CNT_H = 0;
    REG_TM3CNT_H = 0;
    REG_TM2CNT_L = 0;
    REG_TM3CNT_L = 0;
    REG_TM3CNT_H = 0x0084; /* enable, cascade from timer 2 */
    REG_TM2CNT_H = 0x0080; /* enable, prescaler 1 = every CPU cycle */
}

void plat_frame_end(void) {
    u32 cycles;
    char msg[128];
    int n = 0;
    char tmp[12];
    int k = 0;

    REG_TM2CNT_H = 0; /* stop: both counters freeze */
    cycles = ((u32) REG_TM3CNT_L << 16) | REG_TM2CNT_L;

    msg[n++] = 'c';
    msg[n++] = ' ';
    do {
        tmp[k++] = (char) ('0' + cycles % 10);
        cycles /= 10;
    } while (cycles != 0);
    while (k > 0) {
        msg[n++] = tmp[--k];
    }
#ifdef GBA_PROF
    {
        /* gba/prof_gba.c: collision cycles and calls for this frame */
        extern int prof_frame_report(char *out);
        n += prof_frame_report(msg + n);
    }
#endif
    debug_emit(msg, n, DEBUG_LEVEL_WARN);
}

void plat_finish(void) {
    debug_emit("__done__", 8, DEBUG_LEVEL_WARN);
}
