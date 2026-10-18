/* Minimal GBA hardware definitions for the demo. */
#ifndef DEMO_GBA_H
#define DEMO_GBA_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
typedef long long s64;

#define REG8(a) (*(volatile u8 *) (a))
#define REG16(a) (*(volatile u16 *) (a))
#define REG32(a) (*(volatile u32 *) (a))

#define REG_DISPCNT REG16(0x04000000)
#define REG_DISPSTAT REG16(0x04000004)
#define REG_VCOUNT REG16(0x04000006)
#define REG_BG2PA REG16(0x04000020)
#define REG_BG2PB REG16(0x04000022)
#define REG_BG2PC REG16(0x04000024)
#define REG_BG2PD REG16(0x04000026)
#define REG_BG2X REG32(0x04000028)
#define REG_BG2Y REG32(0x0400002C)
#define REG_KEYINPUT REG16(0x04000130)
#define REG_IE REG16(0x04000200)
#define REG_IF REG16(0x04000202)
#define REG_WAITCNT REG16(0x04000204)
#define REG_IME REG16(0x04000208)
#define REG_TM2CNT_L REG16(0x04000108)
#define REG_TM2CNT_H REG16(0x0400010A)
#define REG_TM3CNT_L REG16(0x0400010C)
#define REG_TM3CNT_H REG16(0x0400010E)

#define IRQ_HANDLER_ADDR (*(volatile u32 *) 0x03007FFC)
#define BIOS_IF (*(volatile u16 *) 0x03007FF8)

#define VRAM ((volatile u16 *) 0x06000000)
#define VRAM_PAGE1_OFFSET 0xA000
#define OBJ_VRAM_BITMAP ((volatile u16 *) 0x06014000) /* OBJ tiles 512+ in bitmap modes */
#define PAL_BG ((volatile u16 *) 0x05000000)
#define PAL_OBJ ((volatile u16 *) 0x05000200)
#define OAM ((volatile u16 *) 0x07000000)

#define DCNT_MODE4 0x0004
#define DCNT_PAGE 0x0010
#define DCNT_OBJ_1D 0x0040
#define DCNT_BG2 0x0400
#define DCNT_OBJ 0x1000

#define KEY_A 0x0001
#define KEY_B 0x0002
#define KEY_SELECT 0x0004
#define KEY_START 0x0008
#define KEY_RIGHT 0x0010
#define KEY_LEFT 0x0020
#define KEY_UP 0x0040
#define KEY_DOWN 0x0080
#define KEY_R 0x0100
#define KEY_L 0x0200

#define RGB15(r, g, b) ((u16) (((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10)))

#define IWRAM_CODE __attribute__((section(".iwram"), long_call, target("arm")))
#define EWRAM_BSS __attribute__((section(".bss")))

#endif
