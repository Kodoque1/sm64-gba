/* Host and ARM (newlib + semihosting under qemu) share this: stdin in, stdout out. */
#include <stdio.h>
#include <string.h>
#include "platform.h"

int plat_read_line(char *buf, int max) {
    int n;
    if (fgets(buf, max, stdin) == NULL) {
        return -1;
    }
    n = (int) strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    return n;
}

void plat_write(const char *s, int n) {
    fwrite(s, 1, (size_t) n, stdout);
}

void plat_write_err(const char *s, int n) {
    fwrite(s, 1, (size_t) n, stderr);
}

void plat_init(void) {}
void plat_frame_begin(void) {}
void plat_frame_end(void) {}

#ifndef FP_PROFILE
void plat_finish(void) {
    fflush(stdout);
}
#endif
