#ifndef EDIT_H
#define EDIT_H
#include <gb/gb.h>
#include "orca.h"

/* The clipboard is capped by wram, not by taste: 16x16 is what is left over
 * once the grid, the marks, the tile shadow and the vram queue have had
 * theirs.  The selection is clamped to the same box so the limit is visible
 * while you are dragging rather than discovered at the paste. */
#define CLIP_W 16
#define CLIP_H 16

extern u8 sel_active;
extern u8 sel_x0, sel_y0, sel_x1, sel_y1; /* inclusive, already normalised */
extern u8 sel_ax, sel_ay;                 /* the corner the drag started from */
extern u8 clip_w, clip_h;                 /* 0 x 0 until something is copied */

void edit_reset(void) BANKED;
void edit_anchor(u8 x, u8 y) BANKED;
void edit_extend(u8 x, u8 y) BANKED;
void edit_clear(void) BANKED;
void edit_copy(u8 cx, u8 cy) BANKED;  /* the selection, or the cell under it */
void edit_cut(u8 cx, u8 cy) BANKED;
u8 edit_paste(u8 cx, u8 cy) BANKED;   /* 0 when there is nothing to paste */

#endif
