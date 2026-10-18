#ifndef DEMO_HUD_H
#define DEMO_HUD_H

#include "gba.h"

void hud_init(void);
void hud_clear(void);
void hud_text(s32 x, s32 y, const char *s);
void hud_commit(void);
char *hud_fmt_int(char *out, s32 v, s32 width);

#endif
