/* Selection and clipboard.
 *
 * Lives in bank 1: none of it runs per frame, only when a button is pressed,
 * so the banked-call trampoline costs nothing that matters.
 */
#pragma bank 1

#include "edit.h"

u8 sel_active;
u8 sel_x0, sel_y0, sel_x1, sel_y1;
u8 sel_ax, sel_ay;
u8 clip_w, clip_h;

static u8 clip[CLIP_W * CLIP_H];

void edit_reset(void) BANKED {
  sel_active = 0;
  clip_w = 0;
  clip_h = 0;
}

void edit_clear(void) BANKED { sel_active = 0; }

void edit_anchor(u8 x, u8 y) BANKED {
  sel_ax = x;
  sel_ay = y;
  sel_x0 = sel_x1 = x;
  sel_y0 = sel_y1 = y;
  sel_active = 1;
}

void edit_extend(u8 x, u8 y) BANKED {
  if (!sel_active) {
    edit_anchor(x, y);
    return;
  }
  if (x < sel_ax) {
    sel_x0 = x;
    sel_x1 = sel_ax;
  } else {
    sel_x0 = sel_ax;
    sel_x1 = x;
  }
  if (y < sel_ay) {
    sel_y0 = y;
    sel_y1 = sel_ay;
  } else {
    sel_y0 = sel_ay;
    sel_y1 = y;
  }
}

void edit_copy(u8 cx, u8 cy) BANKED {
  u8 x0, y0, w, h, x, y;
  if (sel_active) {
    x0 = sel_x0;
    y0 = sel_y0;
    w = (u8)(sel_x1 - sel_x0 + 1);
    h = (u8)(sel_y1 - sel_y0 + 1);
  } else {
    x0 = cx;
    y0 = cy;
    w = 1;
    h = 1;
  }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      clip[y * CLIP_W + x] = og_grid[(u16)(y0 + y) * GRID_W + x0 + x];
  clip_w = w;
  clip_h = h;
}

void edit_cut(u8 cx, u8 cy) BANKED {
  u8 x, y;
  edit_copy(cx, cy);
  if (sel_active) {
    for (y = sel_y0; y <= sel_y1; y++)
      for (x = sel_x0; x <= sel_x1; x++)
        orca_poke_abs(y, x, '.');
  } else {
    orca_poke_abs(cy, cx, '.');
  }
}

u8 edit_paste(u8 cx, u8 cy) BANKED {
  u8 x, y;
  if (!clip_w)
    return 0;
  for (y = 0; y < clip_h; y++) {
    if (cy + y >= GRID_H)
      break;
    for (x = 0; x < clip_w; x++) {
      if (cx + x >= GRID_W)
        break;
      orca_poke_abs((u8)(cy + y), (u8)(cx + x), clip[y * CLIP_W + x]);
    }
  }
  return 1;
}
