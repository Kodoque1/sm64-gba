#ifndef BENCH_H
#define BENCH_H

/* The bench ROM's record (make bench), kept in the cartridge's save RAM
 * (SRAM: 32 KB at 0x0E000000, byte access only), so it ends up in a .sav
 * file in emulators and on a flash cart's SD card. Read by
 * tools/bench_report.py. Little-endian.
 *
 * Header, 32 bytes:
 *    0  "SM64BNCH"
 *    8  version, record size
 *   10  frames recorded (u16), updated every frame
 *   12  state: BENCH_RUNNING, then BENCH_DONE after the last frame
 *   13  flags: bit 0 = mGBA's debug registers answered (an emulator: mGBA
 *       and NanoBoyAdvance have them, a GBA doesn't)
 *   16  ROM checksum: sum of the ROM image's 32-bit words
 *   20  VBlanks each level took to load (u16 x 4, by level)
 *
 * Then one record per game frame:
 *    0  VBlanks the frame took (2 = 30 fps)
 *    1  quality step used (bits 0-3), level (bits 4-7)
 *    2  stick x, stick y (s8) and buttons (N64 mask >> 8) fed to Mario
 *    5  unused
 *    6  camera yaw fed to Mario (s16)
 *    8  Mario update cycles (u24)
 *   11  render cycles (u24)
 *   14  unused
 *   16  FNV-1a hash of Mario's position (three floats, 12 bytes) */
#define BENCH_MAGIC "SM64BNCH"
#define BENCH_VERSION 1
#define BENCH_HEADER 32
#define BENCH_RECORD 20
#define BENCH_MAX_FRAMES ((32768 - BENCH_HEADER) / BENCH_RECORD)

#define BENCH_FRAMES_OFS 10
#define BENCH_STATE_OFS 12
#define BENCH_FLAGS_OFS 13
#define BENCH_CHECKSUM_OFS 16
#define BENCH_LOADS_OFS 20

#define BENCH_RUNNING 0x01
#define BENCH_DONE 0xD0

#endif
