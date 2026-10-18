#ifndef HARNESS_PLATFORM_H
#define HARNESS_PLATFORM_H

/* The movement core is compiled against the decomp's own libc headers
 * (-nostdinc), so all real I/O lives behind this small interface,
 * implemented in platform_stdio.c with the toolchain's libc. */

/* Read one line (without the newline). Returns its length, or -1 at EOF. */
int plat_read_line(char *buf, int max);

/* Write raw bytes to the output log. */
void plat_write(const char *s, int n);

/* Write to stderr (diagnostics, never part of the compared log). */
void plat_write_err(const char *s, int n);

/* Called once at startup (GBA: wait states, debug output). */
void plat_init(void);

/* Bracket the per-frame game update; the GBA build times it in CPU cycles. */
void plat_frame_begin(void);
void plat_frame_end(void);

/* Called once at the end; profile builds print their counters here. */
void plat_finish(void);

#endif
