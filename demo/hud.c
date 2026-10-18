/*
 * Performance HUD: text drawn with hardware sprites (8x8, 4bpp) so it costs
 * the renderer nothing. 3x5 pixel glyphs with a dark drop shadow.
 */
#include "gba.h"
#include "hud.h"

#define HUD_TILE_BASE 512 /* first OBJ tile usable in bitmap modes */
#define HUD_MAX_SPRITES 48

static const char kGlyphChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%/:.- ";
/* 5 rows of 3 pixels per glyph, '#' = set */
static const char *const kGlyphs[] = {
    "###", "#.#", "#.#", "#.#", "###", /* 0 */
    ".#.", "##.", ".#.", ".#.", "###", /* 1 */
    "###", "..#", "###", "#..", "###", /* 2 */
    "###", "..#", ".##", "..#", "###", /* 3 */
    "#.#", "#.#", "###", "..#", "..#", /* 4 */
    "###", "#..", "###", "..#", "###", /* 5 */
    "###", "#..", "###", "#.#", "###", /* 6 */
    "###", "..#", "..#", ".#.", ".#.", /* 7 */
    "###", "#.#", "###", "#.#", "###", /* 8 */
    "###", "#.#", "###", "..#", "###", /* 9 */
    ".#.", "#.#", "###", "#.#", "#.#", /* A */
    "##.", "#.#", "##.", "#.#", "##.", /* B */
    ".##", "#..", "#..", "#..", ".##", /* C */
    "##.", "#.#", "#.#", "#.#", "##.", /* D */
    "###", "#..", "##.", "#..", "###", /* E */
    "###", "#..", "##.", "#..", "#..", /* F */
    ".##", "#..", "#.#", "#.#", ".##", /* G */
    "#.#", "#.#", "###", "#.#", "#.#", /* H */
    "###", ".#.", ".#.", ".#.", "###", /* I */
    "..#", "..#", "..#", "#.#", ".#.", /* J */
    "#.#", "#.#", "##.", "#.#", "#.#", /* K */
    "#..", "#..", "#..", "#..", "###", /* L */
    "#.#", "###", "###", "#.#", "#.#", /* M */
    "##.", "#.#", "#.#", "#.#", "#.#", /* N */
    ".#.", "#.#", "#.#", "#.#", ".#.", /* O */
    "##.", "#.#", "##.", "#..", "#..", /* P */
    ".#.", "#.#", "#.#", "##.", ".##", /* Q */
    "##.", "#.#", "##.", "#.#", "#.#", /* R */
    ".##", "#..", ".#.", "..#", "##.", /* S */
    "###", ".#.", ".#.", ".#.", ".#.", /* T */
    "#.#", "#.#", "#.#", "#.#", "###", /* U */
    "#.#", "#.#", "#.#", "#.#", ".#.", /* V */
    "#.#", "#.#", "###", "###", "#.#", /* W */
    "#.#", "#.#", ".#.", "#.#", "#.#", /* X */
    "#.#", "#.#", ".#.", ".#.", ".#.", /* Y */
    "###", "..#", ".#.", "#..", "###", /* Z */
    "#.#", "..#", ".#.", "#..", "#.#", /* % */
    "..#", "..#", ".#.", "#..", "#..", /* / */
    "...", ".#.", "...", ".#.", "...", /* : */
    "...", "...", "...", "...", ".#.", /* . */
    "...", "...", "###", "...", "...", /* - */
    "...", "...", "...", "...", "...", /* space */
};

static s32 glyph_index(char c) {
    s32 i;
    if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A');
    for (i = 0; kGlyphChars[i]; i++) {
        if (kGlyphChars[i] == c) return i;
    }
    return (s32) sizeof(kGlyphChars) - 2; /* space */
}

static u16 sOamShadow[HUD_MAX_SPRITES * 4];
static s32 sUsed;

void hud_init(void) {
    s32 g, row, col, i;
    volatile u16 *tiles = OBJ_VRAM_BITMAP;
    /* 4bpp tile: 8 rows of 4 bytes, low nibble = left pixel. Glyph at (2,1),
     * shadow one pixel down-right. Color 1 = text, 2 = shadow. */
    for (g = 0; kGlyphChars[g]; g++) {
        u8 pix[8][8];
        for (row = 0; row < 8; row++)
            for (col = 0; col < 8; col++) pix[row][col] = 0;
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++)
                if (kGlyphs[g * 5 + row][col] == '#') pix[row + 2][col + 3] = 2;
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++)
                if (kGlyphs[g * 5 + row][col] == '#') pix[row + 1][col + 2] = 1;
        for (row = 0; row < 8; row++) {
            for (i = 0; i < 2; i++) {
                u16 v = (u16) (pix[row][i * 4] | (pix[row][i * 4 + 1] << 4) | (pix[row][i * 4 + 2] << 8)
                               | (pix[row][i * 4 + 3] << 12));
                tiles[g * 16 + row * 2 + i] = v;
            }
        }
    }
    PAL_OBJ[1] = RGB15(255, 255, 240);
    PAL_OBJ[2] = RGB15(16, 16, 24);
    hud_clear();
    hud_commit();
}

void hud_clear(void) {
    s32 i;
    for (i = 0; i < HUD_MAX_SPRITES; i++) {
        sOamShadow[i * 4 + 0] = 0x0200; /* hidden */
        sOamShadow[i * 4 + 1] = 0;
        sOamShadow[i * 4 + 2] = 0;
    }
    sUsed = 0;
}

void hud_text(s32 x, s32 y, const char *s) {
    for (; *s; s++, x += 5) {
        if (*s == ' ') continue;
        if (sUsed >= HUD_MAX_SPRITES) return;
        sOamShadow[sUsed * 4 + 0] = (u16) (y & 0xFF);          /* square, 4bpp */
        sOamShadow[sUsed * 4 + 1] = (u16) (x & 0x1FF);         /* 8x8 */
        sOamShadow[sUsed * 4 + 2] = (u16) (HUD_TILE_BASE + glyph_index(*s));
        sUsed++;
    }
}

/* Copy to OAM: call during VBlank. */
void hud_commit(void) {
    s32 i;
    for (i = 0; i < HUD_MAX_SPRITES * 4; i++) {
        if ((i & 3) == 3) continue; /* affine slots */
        OAM[i] = sOamShadow[i];
    }
}

/* Small integer formatting without libc. */
char *hud_fmt_int(char *out, s32 v, s32 width) {
    char tmp[12];
    s32 n = 0, neg = v < 0;
    u32 u = neg ? (u32) (-v) : (u32) v;
    do {
        tmp[n++] = (char) ('0' + u % 10);
        u /= 10;
    } while (u && n < 11);
    if (neg) tmp[n++] = '-';
    while (n < width) tmp[n++] = ' ';
    while (n > 0) *out++ = tmp[--n];
    *out = 0;
    return out;
}
