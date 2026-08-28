/* ORCA virtual machine, portable C.
 *
 * Ported from hundredrabbits/orca-c (sim.c).  Same grid model, same port
 * model, same operator semantics; the differences are documented in README.
 * This file must stay free of Game Boy specifics so it can be unit-tested on
 * the host -- see test/host_test.c.
 */
#ifndef ORCA_H
#define ORCA_H

#define GRID_W 32
#define GRID_H 16
#define GRID_N (GRID_W * GRID_H)

typedef unsigned char u8;
typedef unsigned int u16;
typedef signed char i8;
typedef signed int i16;

extern u8 og_grid[GRID_N];
extern u8 og_mark[GRID_N]; /* per-tick flags; see orca_mark_flags() */

/* Port flags, so the renderer can show what an operator reads and what it
 * writes -- orca-c paints outputs in reverse video and this is the same
 * information. */
#define OG_MK_LOCK 0x01
#define OG_MK_IN 0x02
#define OG_MK_OUT 0x04

/* The generation currently stamped into og_mark.  A cell's flags are live
 * only when (og_mark[i] & OG_MK_GEN) == og_mark_gen.  The renderer reads this
 * directly: a function call per cell across 512 cells cost more than the rest
 * of the repaint put together. */
#define OG_MK_GEN 0xF8
extern u8 og_mark_gen;

/* Flags for a cell, or 0 if the mark is left over from an earlier tick. */
u8 orca_mark_flags(u16 offs);

/* Cells the tick touched -- marked as a port, or written to.  The renderer
 * needs this: a cell's tile depends on the marks, so a full 512-cell diff
 * would be needed every tick otherwise, and measured that cost more than the
 * VM itself.  Entries are unique per tick for marks, and og_touch_over says
 * the list filled up and everything has to be repainted the slow way. */
#define OG_TOUCH_MAX 96
extern u16 og_touch[OG_TOUCH_MAX];
extern u8 og_touch_n;
extern u8 og_touch_over;
extern u16 og_tick;

void orca_init(void);
void orca_run(void);

/* Re-derives the port flags without touching the grid, the clock or the
 * sound.  The editor needs the ports visible while the sequencer is stopped,
 * and they only exist as a side effect of running the operators. */
void orca_run_marks(void);

u8 orca_index_of(u8 g);  /* glyph -> 0..35 */
u8 orca_glyph_of(u8 v);  /* 0..35 -> glyph (lowercase) */

/* Writes go through here so the frontend can repaint just the cells that
 * actually changed.  offs is y*GRID_W+x. */
void orca_cell_changed(u16 offs, u8 g);

/* Emitted by ':'.  inst 0..35 selects an instrument slot, note is a MIDI note
 * number, vel 0..15, len in ticks (0 = the instrument's own setting). */
void orca_note_event(u8 inst, u8 note, u8 vel, u8 len);

/* Editor-side write: keeps the repaint hook and bounds in one place. */
void orca_poke_abs(u8 y, u8 x, u8 g);

#endif
