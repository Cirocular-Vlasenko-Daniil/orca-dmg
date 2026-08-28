#ifndef VRAM_H
#define VRAM_H
#include "orca.h"

/* Queued VRAM writes, drained during vblank.  Implemented in main.c.
 * Returns 0 when the queue is full: a caller keeping a shadow of what it
 * believes is on screen must not record the write in that case, or the tile
 * is lost for good. */
u8 vq_push(u16 addr, u8 v);

#define BGMAP_ADDR 0x9800u
#define TILE(g) ((u8)((g) - 32))

#endif
