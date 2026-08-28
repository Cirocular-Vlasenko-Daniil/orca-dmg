/* Throwaway ROM: splits the tick cost into "scanning 512 empty cells" and
 * "actually running the demo's operators". */
#include <gb/gb.h>
#include "../src/orca.h"
#include "../src/demo.h"

void orca_cell_changed(u16 o, u8 g) { (void)o; (void)g; }
void orca_note_event(u8 a, u8 b, u8 c, u8 d) { (void)a; (void)b; (void)c; (void)d; }

volatile u8 bench_mark;

void main(void) {
  u8 y, x, i;
  bench_mark = 1;
  orca_init();
  for (y = 0; y < GRID_H; y++)
    for (x = 0; x < GRID_W; x++)
      og_grid[(u16)y * GRID_W + x] = (u8)demo_pattern[y][x];
  bench_mark = 3;
  for (i = 0; i < 60; i++)
    orca_run();
  orca_init(); /* empty grid */
  bench_mark = 4;
  for (i = 0; i < 60; i++)
    orca_run();
  bench_mark = 5;
  while (1)
    ;
}
