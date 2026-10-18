/*
 * Headless mGBA runner for the harness ROMs.
 *
 * Boots a .gba with skipBios, runs it as fast as the host allows, and routes
 * the ROM's debug prints: INFO lines (the parity log) to stdout, WARN lines
 * (cycle counts, diagnostics) to stderr. Exits when the ROM prints __done__.
 *
 *   mgba_runner rom.gba > log.txt 2> cycles.txt
 *
 * Sampling profiler: with MGBA_PROFILE=<file>, every MGBA_PROFILE_PERIOD
 * cycles (default 97, prime to avoid locking onto loops) the runner records
 * the CPU's PC and LR, but only while GBA timer 2 is running, i.e. only
 * inside Mario's measured frame update. Output: "pc lr count" lines in hex,
 * which tools/gba_pcprof.py maps to symbols.
 *
 * Demo options (after the ROM path, in any order):
 *   --frames N        run N GBA frames (60 Hz) then exit (the demo never ends)
 *   --keys FILE       button script: lines "first last KEYS", frames inclusive,
 *                     KEYS from A B L R S(start) E(select) < > ^ v
 *   --capture DIR K   write every K-th frame as DIR/NNNNNN.ppm
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/log.h>
#include <mgba/core/timing.h>
#include <mgba/internal/arm/arm.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sDone;

/* ---------- sampling profiler ---------- */

#define PROF_SLOTS (1u << 20)
static uint32_t *sProfKey; /* pc, lr pairs; pc == 0 marks an empty slot */
static uint32_t *sProfCount;
static struct mCore *sProfCore;
static struct mTimingEvent sProfEvent;
static int32_t sProfPeriod = 97;
static uint64_t sProfSamples;
static long sProfSkipFrames; /* MGBA_PROFILE_SKIP: start sampling after this many frames */
static long sFrameCounter;

static void prof_record(uint32_t pc, uint32_t lr) {
    uint32_t h = (pc * 2654435761u) ^ (lr * 40503u);
    for (;;) {
        uint32_t i = h & (PROF_SLOTS - 1);
        if (sProfKey[2 * i] == pc && sProfKey[2 * i + 1] == lr) {
            sProfCount[i]++;
            return;
        }
        if (sProfKey[2 * i] == 0) {
            sProfKey[2 * i] = pc;
            sProfKey[2 * i + 1] = lr;
            sProfCount[i] = 1;
            return;
        }
        h++;
    }
}

static void prof_tick(struct mTiming *timing, void *context, uint32_t late) {
    struct ARMCore *cpu = sProfCore->cpu;
    (void) context;
    /* TM2CNT_H bit 7: the frame timer is running. */
    if (sFrameCounter >= sProfSkipFrames && (sProfCore->rawRead16(sProfCore, 0x0400010A, -1) & 0x80)) {
        uint32_t pc = (uint32_t) cpu->gprs[15] - (cpu->executionMode == MODE_THUMB ? 4 : 8);
        prof_record(pc, (uint32_t) cpu->gprs[14]);
        sProfSamples++;
    }
    mTimingSchedule(timing, &sProfEvent, sProfPeriod - (int32_t) late);
}

static void prof_dump(const char *path) {
    FILE *f = fopen(path, "w");
    uint32_t i;
    if (f == NULL) {
        return;
    }
    fprintf(f, "# period %d samples %llu\n", sProfPeriod, (unsigned long long) sProfSamples);
    for (i = 0; i < PROF_SLOTS; i++) {
        if (sProfKey[2 * i] != 0) {
            fprintf(f, "%08x %08x %u\n", sProfKey[2 * i], sProfKey[2 * i + 1], sProfCount[i]);
        }
    }
    fclose(f);
}

static void on_log(struct mLogger *logger, int category, enum mLogLevel level, const char *format,
                   va_list args) {
    char buf[512];
    const char *name = mLogCategoryName(category);
    (void) logger;

    if (name == NULL || strcmp(name, "GBA Debug") != 0) {
        return;
    }
    vsnprintf(buf, sizeof(buf), format, args);
    if (level == mLOG_INFO) {
        fputs(buf, stdout);
        fputc('\n', stdout);
    } else if (strcmp(buf, "__done__") == 0) {
        sDone = 1;
    } else {
        fputs(buf, stderr);
        fputc('\n', stderr);
    }
}

/* ---------- key scripts and capture ---------- */

#define MAX_KEY_RANGES 4096
static struct { long first, last; unsigned keys; } sKeyRanges[MAX_KEY_RANGES];
static int sNumKeyRanges;

static void load_keys(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    if (f == NULL) {
        fprintf(stderr, "mgba_runner: cannot read %s\n", path);
        exit(2);
    }
    while (fgets(line, sizeof(line), f) && sNumKeyRanges < MAX_KEY_RANGES) {
        long a, b;
        char k[64] = "";
        unsigned keys = 0;
        char *c;
        if (line[0] == '#' || sscanf(line, "%ld %ld %63s", &a, &b, k) < 2) continue;
        for (c = k; *c; c++) {
            switch (*c) {
                case 'A': keys |= 0x001; break;
                case 'B': keys |= 0x002; break;
                case 'E': keys |= 0x004; break;
                case 'S': keys |= 0x008; break;
                case '>': keys |= 0x010; break;
                case '<': keys |= 0x020; break;
                case '^': keys |= 0x040; break;
                case 'v': keys |= 0x080; break;
                case 'R': keys |= 0x100; break;
                case 'L': keys |= 0x200; break;
            }
        }
        sKeyRanges[sNumKeyRanges].first = a;
        sKeyRanges[sNumKeyRanges].last = b;
        sKeyRanges[sNumKeyRanges].keys = keys;
        sNumKeyRanges++;
    }
    fclose(f);
}

static unsigned keys_for_frame(long frame) {
    unsigned keys = 0;
    int i;
    for (i = 0; i < sNumKeyRanges; i++) {
        if (frame >= sKeyRanges[i].first && frame <= sKeyRanges[i].last) keys |= sKeyRanges[i].keys;
    }
    return keys;
}

static void write_ppm(const char *dir, long frame, const color_t *video, unsigned w, unsigned h) {
    char path[512];
    FILE *f;
    unsigned x, y;
    snprintf(path, sizeof(path), "%s/%06ld.ppm", dir, frame);
    f = fopen(path, "wb");
    if (f == NULL) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c = (uint32_t) video[y * w + x];
            unsigned char rgb[3] = { (unsigned char) (c & 0xFF), (unsigned char) ((c >> 8) & 0xFF),
                                     (unsigned char) ((c >> 16) & 0xFF) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

int main(int argc, char **argv) {
    static struct mLogger logger = { .log = on_log };
    struct mCore *core;
    unsigned width, height;
    color_t *video;
    long maxFrames = 20000000;
    long frames = 0;
    const char *captureDir = NULL;
    long captureEvery = 1;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s rom.gba [max_gba_frames] [--frames N] [--keys F] [--capture DIR K]\n", argv[0]);
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            maxFrames = atol(argv[++i]);
        } else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            load_keys(argv[++i]);
        } else if (strcmp(argv[i], "--capture") == 0 && i + 2 < argc) {
            captureDir = argv[++i];
            captureEvery = atol(argv[++i]);
        } else {
            maxFrames = atol(argv[i]);
        }
    }

    mLogSetDefaultLogger(&logger);

    core = mCoreFind(argv[1]);
    if (core == NULL || !core->init(core)) {
        fprintf(stderr, "mgba_runner: cannot create core for %s\n", argv[1]);
        return 1;
    }
    mCoreInitConfig(core, NULL);
    core->opts.skipBios = true;
    core->opts.useBios = false;
    mCoreLoadConfig(core);

    core->desiredVideoDimensions(core, &width, &height);
    video = calloc((size_t) width * height, sizeof(color_t));
    core->setVideoBuffer(core, video, width);

    if (!mCoreLoadFile(core, argv[1])) {
        fprintf(stderr, "mgba_runner: cannot load %s\n", argv[1]);
        return 1;
    }
    core->reset(core);

    if (getenv("MGBA_PROFILE") != NULL) {
        if (getenv("MGBA_PROFILE_PERIOD") != NULL) {
            sProfPeriod = atoi(getenv("MGBA_PROFILE_PERIOD"));
        }
        if (getenv("MGBA_PROFILE_SKIP") != NULL) {
            sProfSkipFrames = atol(getenv("MGBA_PROFILE_SKIP"));
        }
        sProfKey = calloc(2 * PROF_SLOTS, sizeof(uint32_t));
        sProfCount = calloc(PROF_SLOTS, sizeof(uint32_t));
        sProfCore = core;
        sProfEvent.context = NULL;
        sProfEvent.callback = prof_tick;
        sProfEvent.name = "harness profiler";
        sProfEvent.priority = 0x100;
        mTimingSchedule(core->timing, &sProfEvent, sProfPeriod);
    }

    while (!sDone && frames < maxFrames) {
        if (sNumKeyRanges > 0) {
            core->setKeys(core, keys_for_frame(frames));
        }
        core->runFrame(core);
        sFrameCounter++;
        if (captureDir != NULL && frames % captureEvery == 0) {
            write_ppm(captureDir, frames, video, width, height);
        }
        frames++;
    }
    if (captureDir != NULL || sNumKeyRanges > 0) {
        sDone = 1; /* demo runs end on the frame limit */
    }

    fflush(stdout);
    if (sProfKey != NULL) {
        prof_dump(getenv("MGBA_PROFILE"));
    }
    core->deinit(core);
    free(video);
    if (!sDone) {
        fprintf(stderr, "mgba_runner: ROM did not finish within %ld frames\n", maxFrames);
        return 3;
    }
    return 0;
}
