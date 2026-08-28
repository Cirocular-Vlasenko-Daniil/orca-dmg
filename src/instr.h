#ifndef INSTR_H
#define INSTR_H
#include <gb/gb.h>
#include "orca.h"

/* The instrument page lives in background map rows 16-31, which the 32x16
 * grid never uses.  Switching to it is a write to SCY -- nothing is redrawn,
 * the two pages simply coexist in the tilemap. */
#define INSTR_PAGE_SCY 128

void instr_page_init(void) BANKED;
void instr_page_invalidate(void) BANKED; /* after the tilemap was cleared underneath */
void instr_page_open(u8 slot) BANKED;
u8 instr_page_draw(void) BANKED;      /* incremental; 0 = queue was full */
void instr_page_repaint(void) BANKED; /* full paint, lcd off */
void instr_page_input(u8 pressed, u8 act) BANKED;
u8 instr_page_slot(void) BANKED;
u8 instr_page_cursor_row(void) BANKED; /* page row the selection sits on */
const char *instr_page_status(void) BANKED;

#endif
