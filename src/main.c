/* ORCA/DMG -- the editor, the screen and the clock.
 *
 * Layout: the 32x16 grid is mapped 1:1 onto the background tilemap, which is
 * itself 32x32 tiles, so horizontal scrolling is free -- SCX and nothing else.
 * Rows 16-31 of that same tilemap hold the instrument page, so switching
 * pages is a write to SCY and never a redraw.  The bottom two rows of the
 * screen are the window layer, which also carries the (static) glyph palette
 * just off-screen; opening the picker is a single write to WY.
 */
#include <gb/gb.h>
#include <gb/hardware.h>
#include "orca.h"
#include "audio.h"
#include "instr.h"
#include "edit.h"
#include "text.h"
#include "vram.h"
#include "demo.h"

/* The tile set lives in bank 1: 193 tiles no longer fit alongside the code
 * in bank 0, and it is only read once, at init. */
#define FONT_BANK 1
#define TILE_COUNT 193
#define TILE_REVERSE 96  /* base tile + this = the same glyph, reverse video */
#define TILE_PORT_DOT 192
/* The cursor lives above the font: in 0x8000 addressing the background and
 * the sprites share one tile space. */
#define TILE_CURSOR 193

extern const unsigned char font_tiles[TILE_COUNT * 16];
extern const unsigned char cursor_tile_data[16];

#define BGMAP ((u8 *)0x9800)
#define WINMAP ((u8 *)0x9C00)
#define SRAM ((u8 *)0xA000)

#define STATUS_WY 128
#define PICKER_WY 96

#define PAGE_GRID 0
#define PAGE_INSTR 1

/* --------------------------------------------------------- vram queue ---- */
/* VRAM may only be touched during vblank, so writes are queued here and
 * drained there.
 *
 * The drain loop is assembly because the budget is real, and the budget is
 * smaller than the whole of vblank.  Measured on the running ROM: the vblank
 * interrupt fires at LY 144, gbdk's own handler (mostly the OAM DMA) hands
 * over at LY 146, and this drain gets going at LY 147 -- so a little under
 * seven scanlines, about 800 machine cycles, is what there actually is.
 * Anything written past LY 153 lands while the ppu is drawing and is silently
 * swallowed on real hardware, which an emulator that does not gate vram will
 * happily hide.  sdcc's version of this loop cost 67 cycles an entry; the one
 * below costs 15, and the cap is calibrated against the measurement in
 * tools/vblank_probe.py rather than guessed.
 *
 * Records are four bytes (address low, high, value, padding) so that stepping
 * is a plain increment and the offset of a record is a shift.  Head and tail
 * are record indices rather than byte offsets: that keeps every bit of the
 * arithmetic around the loop eight-bit, which measurably mattered -- the C
 * wrapper was costing more than the assembly it wrapped. */
/* 96 records rather than 160: wram is the scarce resource here, and a full
 * queue is not a failure -- every producer keeps its shadow un-updated and
 * retries, so a burst just takes an extra frame or two to land. */
#define VQ_RECORDS 96
#define VQ_FLUSH_MAX 20

static u8 vq_buf[VQ_RECORDS * 4];
static u8 vq_head, vq_tail;

static u8 *vq_run_ptr;
static u8 vq_run_n;

static void vq_run(void) {
__asm
  ld  a, (#_vq_run_n)
  or  a, a
  ret Z
  ld  b, a
  ld  hl, #_vq_run_ptr
  ld  a, (hl+)
  ld  h, (hl)
  ld  l, a
1$:
  ld  a, (hl+)
  ld  e, a
  ld  a, (hl+)
  ld  d, a
  ld  a, (hl+)
  ld  (de), a
  inc hl
  dec b
  jr  NZ, 1$
__endasm;
}

u8 vq_push(u16 addr, u8 v) {
  u8 next = (u8)(vq_tail + 1);
  u16 o;
  if (next >= VQ_RECORDS)
    next = 0;
  if (next == vq_head)
    return 0;
  o = (u16)vq_tail << 2;
  vq_buf[o] = (u8)addr;
  vq_buf[o + 1] = (u8)(addr >> 8);
  vq_buf[o + 2] = v;
  vq_tail = next;
  return 1;
}

static void vq_flush(void) {
  u8 budget = VQ_FLUSH_MAX;
  u8 run;
  /* One contiguous run at a time -- the asm loop carries no wrap check. */
  while (vq_head != vq_tail && budget) {
    run = (vq_tail > vq_head) ? (u8)(vq_tail - vq_head)
                              : (u8)(VQ_RECORDS - vq_head);
    if (run > budget)
      run = budget;
    vq_run_ptr = &vq_buf[(u16)vq_head << 2];
    vq_run_n = run;
    vq_run();
    budget = (u8)(budget - run);
    vq_head = (u8)(vq_head + run);
    if (vq_head >= VQ_RECORDS)
      vq_head = 0;
  }
}

/* A cell's tile depends on the port marks as well as the glyph, and the marks
 * move without any glyph changing, so the grid is repainted by diffing
 * against a shadow rather than by reacting to individual writes. */
static u8 grid_repaint = 1;

void orca_cell_changed(u16 offs, u8 g) {
  (void)offs;
  (void)g;
  grid_repaint = 1;
}

/* The first ':' operand is an instrument, not a channel: which voice it lands
 * on is the instrument's business, and so is the timbre. */
void orca_note_event(u8 inst, u8 note, u8 vel, u8 len) {
  snd_request(inst, note, vel, len);
}

/* -------------------------------------------------------------- state ---- */
static u8 cur_x, cur_y;
static u8 scroll_x;
static u8 bpm = 120;
static u8 playing = 1;
static u16 tick_acc;
static u16 last_time;
static u8 picker_open, pick_x, pick_y;
static u8 page = PAGE_GRID;
static u8 page_dirty;
static u8 status_dirty = 1;
static u8 msg_timer;
static const char *msg;

static const char palette[81] =
    "0123456789ABCDEFGHIJ"
    "KLMNOPQRSTUVWXYZ.*#:"
    "abcdefghijklmnopqrst"
    "uvwxyz              ";

/* ------------------------------------------------------------ drawing ---- */

static u8 status_shadow[40];

static u8 tile_shadow[GRID_N];

/* orca-c paints output ports in reverse video (tui_main.c); an input port
 * that is still empty gets a marker instead of a grid dot, so the cells an
 * operator is waiting on are visible before anything is typed into them. */
static u8 in_selection(u16 i) {
  u8 x, y;
  if (!sel_active)
    return 0;
  x = (u8)i & 31u;
  y = (u8)(i >> 5);
  return (u8)(x >= sel_x0 && x <= sel_x1 && y >= sel_y0 && y <= sel_y1);
}

static u8 tile_for(u16 i) {
  u8 g = og_grid[i];
  u8 m = og_mark[i];
  u8 t = TILE(g);
  if (in_selection(i))
    return (u8)(t + TILE_REVERSE);
  if ((m & OG_MK_GEN) == og_mark_gen) {
    if (m & OG_MK_OUT)
      t = (u8)(t + TILE_REVERSE);
    else if ((m & OG_MK_IN) && g == '.')
      t = TILE_PORT_DOT;
  }
  return t;
}

/* The search is a leaf function on purpose, and it is not a micro-optimisation:
 * with vq_push() inline in the loop, sdcc spills the counter to the stack and
 * every one of the 512 cells costs a pop/push pair to index three arrays.
 * Measured, that shape cost 3.7 frames a tick -- more than the whole VM -- and
 * pushed the sequencer off its clock at high tempo. */
/* Cells the previous tick marked: they have to be revisited even when nothing
 * about them changed, because losing a mark changes the tile back. */
static u16 paint_prev[OG_TOUCH_MAX];
static u8 paint_prev_n;
static u8 paint_full = 1;

/* Returns 0 if the queue was full.  tile_for() is written out rather than
 * called: this runs once per touched cell per tick, and on this CPU the call
 * is a large part of the work. */
static u8 paint_cell(u16 i) {
  u8 g = og_grid[i];
  u8 m = og_mark[i];
  u8 t = (u8)(g - 32);
  if (sel_active && in_selection(i)) {
    t = (u8)(t + TILE_REVERSE);
  } else if ((m & OG_MK_GEN) == og_mark_gen) {
    if (m & OG_MK_OUT)
      t = (u8)(t + TILE_REVERSE);
    else if ((m & OG_MK_IN) && g == '.')
      t = TILE_PORT_DOT;
  }
  if (tile_shadow[i] == t)
    return 1;
  if (!vq_push((u16)0x9800u + i, t))
    return 0;
  tile_shadow[i] = t;
  return 1;
}

static void paint_grid(void) {
  u16 i;
  u8 k, ok = 1;
  grid_repaint = 0;

  if (paint_full || og_touch_over) {
    for (i = 0; i < GRID_N; i++)
      if (!paint_cell(i)) {
        ok = 0;
        break;
      }
  } else {
    for (k = 0; k < paint_prev_n && ok; k++)
      ok = paint_cell(paint_prev[k]);
    for (k = 0; k < og_touch_n && ok; k++)
      ok = paint_cell(og_touch[k]);
  }

  if (!ok) {
    /* The queue filled up mid-pass.  A tick may replace the touch list before
     * the next attempt, so fall back to the whole grid rather than risk
     * leaving a cell behind. */
    grid_repaint = 1;
    paint_full = 1;
    return;
  }
  paint_full = 0;
  paint_prev_n = og_touch_n;
  for (k = 0; k < og_touch_n; k++)
    paint_prev[k] = og_touch[k];
}

static void redraw_all(void) {
  u16 i;
  DISPLAY_OFF;
  for (i = 0; i < GRID_N; i++)
    BGMAP[i] = tile_shadow[i] = tile_for(i);
  paint_full = 0;
  paint_prev_n = 0;
  for (i = GRID_N; i < 1024u; i++)
    BGMAP[i] = 0;
  /* Dropping the queue also drops writes the status shadow already counted
   * as done, so that shadow has to be forgotten along with it. */
  vq_head = vq_tail = 0;
  for (i = 0; i < 40; i++)
    status_shadow[i] = 0xFF; /* no real tile, so every cell is rewritten */
  status_dirty = 1;
  DISPLAY_ON;
  instr_page_invalidate(); /* rows 16-31 went blank underneath the page */
  if (page == PAGE_INSTR)
    instr_page_repaint();
}

static void gfx_init(void) {
  u8 i, r, c;
  DISPLAY_OFF;
  /* Inverted: index 0 is the darkest, so the grid is light on black the way
   * ORCA itself looks.  1 dots, 2 values, 3 operators. */
  BGP_REG = 0x1B;
  OBP0_REG = 0x1B;
  /* 0x8000 addressing, because 0x8800 only reaches 128 background tiles and
   * the reverse-video set puts the count at 193 -- the tiles past 128 were
   * landing on top of the tilemaps. */
  LCDC_REG |= LCDCF_BG8000;
  SWITCH_ROM(FONT_BANK);
  set_bkg_data(0, TILE_COUNT, font_tiles);
  set_sprite_data(TILE_CURSOR, 1, cursor_tile_data);
  set_sprite_tile(0, TILE_CURSOR);
  set_sprite_tile(1, TILE_CURSOR);

  /* Window: rows 0-1 status, rows 2-5 the palette.  The palette never
   * changes, so it is written once here and simply scrolled into view. */
  for (i = 0; i < 32; i++) {
    WINMAP[i] = 0;
    WINMAP[32 + i] = 0;
  }
  for (r = 0; r < 4; r++)
    for (c = 0; c < 20; c++)
      WINMAP[32u * (2 + r) + c] = TILE(palette[r * 20 + c]);

  SCX_REG = 0;
  SCY_REG = 0;
  WX_REG = 7;
  WY_REG = STATUS_WY;
  LCDC_REG |= LCDCF_WIN9C00;
  SHOW_BKG;
  SHOW_WIN;
  SHOW_SPRITES;
  DISPLAY_ON;
}

/* Only the bytes that actually changed are queued -- the tick counter alone
 * would otherwise push 40 writes a tick for no reason. */
static u8 status_complete;

static void put_status(u8 i, char ch) {
  u8 t = TILE(ch);
  if (status_shadow[i] == t)
    return;
  if (vq_push((u16)0x9C00u + (i < 20 ? i : (u16)(32 + i - 20)), t))
    status_shadow[i] = t;
  else
    status_complete = 0; /* try again next frame rather than lose the tile */
}

static void put_status_str(u8 from, const char *s) {
  u8 i;
  for (i = from; i < 40; i++) {
    put_status(i, *s ? *s : ' ');
    if (*s)
      s++;
  }
}

static void draw_status(void) {
  u8 g;
  u16 t = og_tick;

  status_complete = 1;

  put_status(0, 'B');
  put_status(1, 'P');
  put_status(2, 'M');
  put_status(3, (char)('0' + bpm / 100u));
  put_status(4, (char)('0' + (bpm / 10u) % 10u));
  put_status(5, (char)('0' + bpm % 10u));
  put_status(6, ' ');
  put_status(7, 'F');
  put_status(8, (char)orca_glyph_of((u8)((t / 46656u) % 36u)));
  put_status(9, (char)orca_glyph_of((u8)((t / 1296u) % 36u)));
  put_status(10, (char)orca_glyph_of((u8)((t / 36u) % 36u)));
  put_status(11, (char)orca_glyph_of((u8)(t % 36u)));
  put_status(12, ' ');
  if (page == PAGE_INSTR) {
    put_status(13, 'I');
    put_status(14, (char)orca_glyph_of(instr_page_slot()));
    put_status(15, ' ');
  } else {
    put_status(13, '@');
    put_status(14, (char)orca_glyph_of(cur_x));
    put_status(15, (char)orca_glyph_of(cur_y));
  }
  put_status(16, ' ');
  put_status(17, ' ');
  put_status(18, playing ? '>' : '|');
  put_status(19, ' ');

  if (msg_timer) {
    put_status_str(20, msg);
    return;
  }
  if (page == PAGE_INSTR) {
    put_status_str(20, instr_page_status());
    return;
  }

  if (sel_active) {
    put_status(20, 'S');
    put_status(21, 'E');
    put_status(22, 'L');
    put_status(23, ' ');
    put_status(24, (char)orca_glyph_of((u8)(sel_x1 - sel_x0 + 1)));
    put_status(25, 'x');
    put_status(26, (char)orca_glyph_of((u8)(sel_y1 - sel_y0 + 1)));
    put_status_str(27, "");
    return;
  }
  g = og_grid[(u16)cur_y * GRID_W + cur_x];
  put_status(20, (char)g);
  put_status(21, ' ');
  put_status_str(22, name_of(g));
}

static void update_view(void) {
  i16 w;
  u8 scroll_y = 0;

  if (page == PAGE_INSTR) {
    SCX_REG = 0;
    SCY_REG = INSTR_PAGE_SCY;
    WY_REG = STATUS_WY;
    move_sprite(0, 0, 0);
    move_sprite(1, 8, (u8)(instr_page_cursor_row() * 8u + 16u));
    return;
  }

  w = (i16)cur_x * 8 - 76;
  if (w < 0)
    w = 0;
  if (w > 96)
    w = 96;
  scroll_x = (u8)w;

  if (picker_open) {
    /* The palette eats the bottom six rows, leaving twelve of the sixteen
     * grid rows on screen -- scroll so the edited cell is never one of the
     * four that got covered. */
    i16 v = (i16)cur_y * 8 - 40;
    if (v < 0)
      v = 0;
    if (v > 32)
      v = 32;
    scroll_y = (u8)v;
    WY_REG = PICKER_WY;
    move_sprite(1, (u8)(pick_x * 8u + 8u), (u8)(128u + pick_y * 8u));
  } else {
    WY_REG = STATUS_WY;
    move_sprite(1, 0, 0);
  }

  SCX_REG = scroll_x;
  SCY_REG = scroll_y;
  move_sprite(0, (u8)((u8)(cur_x * 8u) - scroll_x + 8u),
              (u8)((u8)(cur_y * 8u) - scroll_y + 16u));
}

/* Tempo changes have to reach the sound engine too: TEST times its note in
 * frames, and a frame is only meaningful against the current tick length. */
static void set_bpm(u8 v) {
  if (v < 20)
    v = 20;
  if (v > 250)
    v = 250;
  bpm = v;
  snd_set_tempo((u8)(3600u / ((u16)v * 4u)));
  status_dirty = 1;
}

static void flash(const char *m) {
  msg = m;
  msg_timer = 90;
  status_dirty = 1;
}

/* ---------------------------------------------------------- persistence -- */
/* 'ORC5' -- the instrument record keeps changing shape as parameters come and
 * go, so older saves are rejected rather than half-loaded. */

#define SAVE_GRID 5
#define SAVE_INSTR (SAVE_GRID + GRID_N)

static void sram_save(void) {
  u8 *ip = (u8 *)instruments;
  u16 i;
  ENABLE_RAM;
  SWITCH_RAM(0);
  SRAM[0] = 'O'; SRAM[1] = 'R'; SRAM[2] = 'C'; SRAM[3] = '6';
  SRAM[4] = bpm;
  for (i = 0; i < GRID_N; i++)
    SRAM[SAVE_GRID + i] = og_grid[i];
  for (i = 0; i < sizeof(instruments); i++)
    SRAM[SAVE_INSTR + i] = ip[i];
  DISABLE_RAM;
  flash("SAVED");
}

static void sram_load(void) {
  u8 *ip = (u8 *)instruments;
  u16 i;
  ENABLE_RAM;
  SWITCH_RAM(0);
  if (SRAM[0] != 'O' || SRAM[1] != 'R' || SRAM[2] != 'C' || SRAM[3] != '6') {
    DISABLE_RAM;
    flash("NO SAVE");
    return;
  }
  set_bpm(SRAM[4]);
  for (i = 0; i < GRID_N; i++)
    og_grid[i] = SRAM[SAVE_GRID + i];
  for (i = 0; i < sizeof(instruments); i++)
    ip[i] = SRAM[SAVE_INSTR + i];
  DISABLE_RAM;
  orca_run_marks();
  redraw_all();
  flash("LOADED");
}

static void load_demo(void) {
  u8 y, x;
  for (y = 0; y < GRID_H; y++)
    for (x = 0; x < GRID_W; x++)
      og_grid[(u16)y * GRID_W + x] = (u8)demo_pattern[y][x];
}

/* ---------------------------------------------------------------- input -- */

static void grid_edited(void) {
  if (!playing)
    orca_run_marks(); /* the ports would otherwise be a tick out of date */
  paint_full = 1;     /* an edit is rare; just repaint the lot */
  grid_repaint = 1;
}

static void picker_sync_to_cursor(void) {
  u8 g = og_grid[(u16)cur_y * GRID_W + cur_x];
  u8 i;
  for (i = 0; i < 80; i++) {
    if ((u8)palette[i] == g) {
      pick_x = (u8)(i % 20u);
      pick_y = (u8)(i / 20u);
      return;
    }
  }
}

/* Opening the instrument page from a value glyph jumps straight to that
 * slot, which is usually the operand the cursor was already sitting on. */
static void open_instr_page(void) {
  u8 g = og_grid[(u16)cur_y * GRID_W + cur_x];
  if ((g >= '0' && g <= '9') || (g >= 'a' && g <= 'z'))
    instr_page_open(orca_index_of(g));
  else
    instr_page_open(instr_page_slot());
  page = PAGE_INSTR;
  /* A first paint is 320 tiles -- far more than a vblank's worth -- so it
   * goes straight to vram with the lcd off.  Later edits are a handful of
   * tiles and go through the queue like everything else. */
  instr_page_repaint();
  status_dirty = 1;
}

/* The pad is sampled in the vblank interrupt, not in the main loop.  A tick
 * can push one loop iteration past three or four frames, and a button pressed
 * and released inside that gap would otherwise never be seen at all. */
/* k_mods records what else was held at the moment a button went down.  The
 * edge latch alone is not enough: a heavy pattern can stretch one loop
 * iteration across several frames, and by the time the main loop looks, the
 * modifier may already be back up -- which turned SELECT+B (copy) into a bare
 * B (erase the cell).  A modifier counts as held if it is down now or was
 * down when the press happened. */
static volatile u8 k_cur, k_edge, k_isr_prev, k_mods;

static void vbl_handler(void) {
  u8 k = joypad();
  u8 e = (u8)(k & ~k_isr_prev);
  if (e) {
    k_edge |= e;
    k_mods = k;
  }
  k_isr_prev = k;
  k_cur = k;
}

static u8 prev_keys;
static u8 rep_timer;
static u8 b_used; /* set when B has acted as a modifier, so the release is
                   * not also read as the plain "erase this cell" tap */

static u8 within(u8 v, u8 anchor, u8 span) {
  return (u8)((v > anchor ? (u8)(v - anchor) : (u8)(anchor - v)) < span);
}

/* Dragging a selection also moves the cursor, but not out of the copy
 * window: the limit is easier to understand while you can see it. */
static void sel_move(u8 act) {
  if (!sel_active)
    edit_anchor(cur_x, cur_y);
  if ((act & J_LEFT) && cur_x && within((u8)(cur_x - 1), sel_ax, CLIP_W))
    cur_x--;
  if ((act & J_RIGHT) && cur_x < GRID_W - 1 &&
      within((u8)(cur_x + 1), sel_ax, CLIP_W))
    cur_x++;
  if ((act & J_UP) && cur_y && within((u8)(cur_y - 1), sel_ay, CLIP_H))
    cur_y--;
  if ((act & J_DOWN) && cur_y < GRID_H - 1 &&
      within((u8)(cur_y + 1), sel_ay, CLIP_H))
    cur_y++;
  edit_extend(cur_x, cur_y);
  paint_full = 1;
  grid_repaint = 1;
  status_dirty = 1;
}

static void handle_input(void) {
  u8 keys, pressed, released, dp, dnew, act, hold;

  disable_interrupts();
  keys = k_cur;
  pressed = k_edge;
  hold = (u8)(keys | (pressed ? k_mods : 0));
  k_edge = 0;
  enable_interrupts();

  released = (u8)(prev_keys & ~keys);
  prev_keys = keys;

  dp = (u8)(keys & (J_LEFT | J_RIGHT | J_UP | J_DOWN));
  dnew = (u8)(pressed & (J_LEFT | J_RIGHT | J_UP | J_DOWN));

  /* Edge first: a tap that began and ended between two polls still counts. */
  if (dnew) {
    act = dnew;
    rep_timer = 16;
  } else if (dp == 0) {
    rep_timer = 0;
    act = 0;
  } else if (rep_timer > 1) {
    rep_timer--;
    act = 0;
  } else {
    act = dp;
    rep_timer = 4;
  }

  if (picker_open) {
    if ((act & J_LEFT) && pick_x) pick_x--;
    else if ((act & J_LEFT) && pick_x == 0) pick_x = 19;
    if ((act & J_RIGHT) && pick_x < 19) pick_x++;
    else if ((act & J_RIGHT) && pick_x >= 19) pick_x = 0;
    if ((act & J_UP) && pick_y) pick_y--;
    else if ((act & J_UP) && pick_y == 0) pick_y = 3;
    if ((act & J_DOWN) && pick_y < 3) pick_y++;
    else if ((act & J_DOWN) && pick_y >= 3) pick_y = 0;
    if (released & J_A) {
      u8 g = (u8)palette[pick_y * 20u + pick_x];
      if (g != ' ') {
        orca_poke_abs(cur_y, cur_x, g);
        grid_edited();
      }
      picker_open = 0;
      status_dirty = 1;
    }
    if (pressed & (J_B | J_START)) {
      picker_open = 0;
      status_dirty = 1;
    }
    return;
  }

  if (page == PAGE_INSTR) {
    if (hold & J_SELECT) {
      /* The clipboard is a grid idea, so on this page SELECT+A/B are free --
       * and this is where saving belongs anyway. */
      if (pressed & J_START) {
        page = PAGE_GRID;
        grid_repaint = 1;
        status_dirty = 1;
      }
      if (pressed & J_A) sram_save();
      if (pressed & J_B) sram_load();
      if ((act & J_LEFT) && bpm > 20) set_bpm((u8)(bpm - 1));
      if ((act & J_RIGHT) && bpm < 250) set_bpm((u8)(bpm + 1));
      if ((act & J_UP) && bpm <= 240) set_bpm((u8)(bpm + 10));
      if ((act & J_DOWN) && bpm >= 30) set_bpm((u8)(bpm - 10));
      return;
    }
    if (pressed || act) {
      instr_page_input(pressed, act);
      page_dirty = 1;
      status_dirty = 1;
    }
    if (pressed & J_B) {
      page = PAGE_GRID;
      grid_repaint = 1;
    }
    if (pressed & J_START) {
      playing = playing ? 0 : 1;
      if (!playing)
        snd_all_off();
    }
    return;
  }

  /* ------------------------------------------------------------ the grid -- */

  /* B released without having modified anything is the plain erase tap. */
  if (released & J_B) {
    if (!b_used) {
      orca_poke_abs(cur_y, cur_x, '.');
      grid_edited();
      status_dirty = 1;
    }
  }
  if (pressed & J_B)
    b_used = 0;

  if (hold & J_SELECT) {
    if (pressed & J_B)
      b_used = 1; /* SELECT owns it; do not erase when it comes back up */
    if (pressed & J_START) {
      open_instr_page();
      return;
    }
    if (pressed & J_A) {
      if (edit_paste(cur_x, cur_y)) {
        grid_edited();
        flash("PASTED");
      } else {
        flash("CLIPBOARD EMPTY");
      }
    }
    if (pressed & J_B) {
      edit_copy(cur_x, cur_y);
      flash("COPIED");
    }
    if ((act & J_LEFT) && bpm > 20) set_bpm((u8)(bpm - 1));
    if ((act & J_RIGHT) && bpm < 250) set_bpm((u8)(bpm + 1));
    if ((act & J_UP) && bpm <= 240) set_bpm((u8)(bpm + 10));
    if ((act & J_DOWN) && bpm >= 30) set_bpm((u8)(bpm - 10));
    return;
  }

  /* B modifies only when there is something to modify, so a bare tap still
   * falls through to the erase below. */
  if ((hold & J_B) && (act || (pressed & J_A))) {
    if (act) {
      sel_move(act);
      b_used = 1;
    }
    if (pressed & J_A) {
      edit_cut(cur_x, cur_y);
      edit_clear();
      grid_edited();
      flash("CUT");
      b_used = 1;
    }
    return;
  }
  if (keys & J_B)
    return; /* still held, waiting to see what it modifies */

  /* A tap that began and ended inside one poll never shows up as a release. */
  if ((pressed & J_B) && !(keys & J_B) && !dnew && !(pressed & J_A)) {
    orca_poke_abs(cur_y, cur_x, '.');
    grid_edited();
    status_dirty = 1;
  }

  if (act) {
    if ((act & J_LEFT) && cur_x) cur_x--;
    if ((act & J_RIGHT) && cur_x < GRID_W - 1) cur_x++;
    if ((act & J_UP) && cur_y) cur_y--;
    if ((act & J_DOWN) && cur_y < GRID_H - 1) cur_y++;
    if (sel_active) { /* moving without B lets the selection go */
      edit_clear();
      paint_full = 1;
      grid_repaint = 1;
    }
    status_dirty = 1;
  }

  if (pressed & J_A) {
    picker_sync_to_cursor();
    picker_open = 1;
  }
  if (pressed & J_START) {
    playing = playing ? 0 : 1;
    if (!playing) {
      snd_all_off();
      orca_run_marks(); /* keep the ports on screen while stopped */
      grid_repaint = 1;
    }
    status_dirty = 1;
  }
}


/* ----------------------------------------------------------------- main -- */

void main(void) {
  orca_init();
  load_demo();
  orca_run_marks(); /* ports visible before the first tick */
  edit_reset();
  snd_defaults();
  instr_page_init();
  gfx_init();
  redraw_all();
  snd_init();
  set_bpm(bpm);
  last_time = sys_time;
  add_VBL(vbl_handler);

  while (1) {
    vsync();
    vq_flush(); /* first thing in vblank, while vram is ours */
    snd_frame();

    handle_input();

    /* Tempo is driven by elapsed vblanks, not by loop iterations: a busy
     * pattern can push orca_run() past a single frame, and counting
     * iterations would quietly drag the tempo down with it. */
    {
      u16 now = sys_time;
      u8 elapsed = (u8)(now - last_time);
      last_time = now;
      if (elapsed > 8)
        elapsed = 8;
      if (playing) {
        u8 budget = 2; /* never chase more than two ticks in one pass */
        tick_acc += (u16)bpm * 4u * elapsed; /* four ticks to the beat */
        while (tick_acc >= 3600u && budget) {
          tick_acc -= 3600u;
          budget--;
          snd_age();
          orca_run();
          snd_dispatch(); /* resolve the tick's competing instruments */
          grid_repaint = 1;
          status_dirty = 1;
        }
        if (tick_acc >= 3600u)
          tick_acc = 0; /* fell behind: resync rather than accumulate debt */
      }
    }

    if (msg_timer && --msg_timer == 0)
      status_dirty = 1;

    /* A draw that could not queue everything keeps its dirty flag: the
     * shadow only records tiles that actually made it into the queue, so
     * without this the remainder would never be retried. */
    if (page_dirty)
      page_dirty = (u8)!instr_page_draw();
    if (status_dirty) {
      draw_status();
      status_dirty = (u8)!status_complete;
    }
    /* The grid is off-screen while the instrument page is up, so repainting
     * it there would only take drain budget away from the page. */
    if (page == PAGE_INSTR)
      paint_full = 1;
    else if (grid_repaint)
      paint_grid();
    update_view();
  }
}
