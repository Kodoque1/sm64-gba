#ifndef DEMO_MANNEQUIN_H
#define DEMO_MANNEQUIN_H

#include "render.h"

struct Object;

void mannequin_init(void);   /* ball sprite tiles and palettes (once) */
void mannequin_hide(void);
void mannequin_update(struct Object *marioObj, const struct RCamera *cam); /* after Mario's frame */
void mannequin_commit(void); /* OAM writes, during VBlank */
s32 mannequin_pose(struct Object *marioObj);       /* skeleton only: 0 if no animation */
const struct RFrame *mannequin_frames(void);      /* the 20 posed parts */

#endif
