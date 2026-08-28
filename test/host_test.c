/* Host-side tests for the VM core.  Nothing here touches the Game Boy, so a
 * failure is a porting bug in orca.c and not a hardware/timing problem. */
#include "../src/orca.h"
#include <stdio.h>
#include <string.h>
#include "../src/demo.h"

static int fails, checks;

typedef struct { u8 inst, note, vel, len; } Note;
static Note notes[32];
static int nnotes;

void orca_cell_changed(u16 offs, u8 g) { (void)offs; (void)g; }
void orca_note_event(u8 c, u8 n, u8 v, u8 l) {
  if (nnotes < 32) { notes[nnotes].inst = c; notes[nnotes].note = n;
                     notes[nnotes].vel = v; notes[nnotes].len = l; }
  nnotes++;
}

static void row(int y, int x, const char *s) {
  while (*s) og_grid[y * GRID_W + x++] = (u8)*s++;
}

static char cellat(int y, int x) { return (char)og_grid[y * GRID_W + x]; }

static void getrow(int y, int x, int n, char *out) {
  int i;
  for (i = 0; i < n; i++) out[i] = cellat(y, x + i);
  out[n] = 0;
}

static void expect_row(const char *what, int y, int x, int n, const char *want) {
  char got[64];
  checks++;
  getrow(y, x, n, got);
  if (strcmp(got, want)) {
    printf("  FAIL %-28s row%d+%d: got \"%s\" want \"%s\"\n", what, y, x, got, want);
    fails++;
  }
}

static void expect_int(const char *what, long got, long want) {
  checks++;
  if (got != want) { printf("  FAIL %-28s got %ld want %ld\n", what, got, want); fails++; }
}

static void reset(void) { orca_init(); nnotes = 0; }
static void ticks(int n) { while (n--) orca_run(); }

int main(void) {
  char buf[64];

  /* --- base36 maths, and the "output takes the case of the right operand"
   *     rule that separates operators from values ------------------------- */
  reset();
  row(0, 0, "1A2");
  row(2, 0, "zA1");
  row(4, 0, "2Ae");
  ticks(1);
  expect_row("add 1A2", 1, 1, 1, "3");
  expect_row("add wraps mod 36", 3, 1, 1, "0");   /* 35+1 == 36 -> 0 */
  expect_row("add keeps rhs case", 5, 1, 1, "g"); /* 2+14 == 16 -> 'g' */

  reset();
  row(0, 0, "5B2");
  ticks(1);
  expect_row("subtract is absolute", 1, 1, 1, "3");

  reset();
  row(0, 0, "3M4");
  ticks(1);
  expect_row("multiply", 1, 1, 1, "c"); /* 12 */

  reset();
  row(0, 0, "5L3");
  ticks(1);
  expect_row("lesser", 1, 1, 1, "3");

  /* --- clock counts ticks, delay bangs on them ------------------------- */
  reset();
  row(0, 0, "1C4");
  ticks(1); expect_row("clock t0", 1, 1, 1, "0");
  ticks(1); expect_row("clock t1", 1, 1, 1, "1");
  ticks(2); expect_row("clock t3", 1, 1, 1, "3");
  ticks(1); expect_row("clock wraps at mod", 1, 1, 1, "0");

  reset();
  row(0, 0, "1D4");
  ticks(1); expect_row("delay on beat", 1, 1, 1, "*");
  ticks(1); expect_row("delay off beat", 1, 1, 1, ".");
  ticks(2); expect_row("delay still off", 1, 1, 1, ".");
  ticks(1); expect_row("delay fires every 4", 1, 1, 1, "*");

  /* --- the bang/case contract ----------------------------------------- */
  reset();
  row(0, 0, "1a2");   /* lowercase add, nothing banging it */
  ticks(1);
  expect_row("lowercase idles unbanged", 1, 1, 1, ".");

  reset();
  row(0, 0, "1D1");   /* fires every tick */
  row(1, 0, ".*.");   /* delay writes its bang here */
  row(2, 0, "1a2");   /* ... which is directly north of this lowercase add */
  ticks(1);
  expect_row("lowercase runs when banged", 3, 1, 1, "3");

  /* A bang that no operator wrote is consumed by the scan itself. */
  reset();
  row(3, 3, "*");
  ticks(1);
  expect_row("stray bang is eaten", 3, 3, 1, ".");

  /* --- if, increment, lerp -------------------------------------------- */
  reset();
  row(0, 0, "4F4");
  row(2, 0, "4F5");
  ticks(1);
  expect_row("if equal bangs", 1, 1, 1, "*");
  expect_row("if unequal is silent", 3, 1, 1, ".");

  reset();
  row(0, 0, "1I3");
  ticks(1); expect_row("increment 1", 1, 1, 1, "1");
  ticks(1); expect_row("increment 2", 1, 1, 1, "2");
  ticks(1); expect_row("increment wraps", 1, 1, 1, "0");

  reset();
  row(0, 0, "2Z8");
  ticks(1); expect_row("lerp step 1", 1, 1, 1, "2");
  ticks(1); expect_row("lerp step 2", 1, 1, 1, "4");
  ticks(3); expect_row("lerp settles on goal", 1, 1, 1, "8");

  /* --- euclidean rhythm ------------------------------------------------ */
  reset();
  row(0, 0, "3U8");
  {
    int t; char pat[9];
    for (t = 0; t < 8; t++) { orca_run(); pat[t] = cellat(1, 1); }
    pat[8] = 0;
    checks++;
    if (strcmp(pat, "*..*..*.")) { printf("  FAIL uclid 3/8: got \"%s\"\n", pat); fails++; }
  }

  /* --- spatial operators ----------------------------------------------- */
  reset();
  row(0, 0, "..E");
  ticks(1); expect_row("east moves", 0, 0, 5, "...E.");
  ticks(1); expect_row("east keeps moving", 0, 0, 5, "....E");

  reset();
  row(0, 0, "E");
  row(0, 1, "#");   /* blocked */
  ticks(1);
  expect_row("blocked mover explodes", 0, 0, 2, "*#");

  reset();
  row(0, 0, "7");
  row(1, 0, "J");
  row(2, 0, "J");
  ticks(1);
  expect_row("J carries value past a chain", 3, 0, 1, "7");

  reset();
  row(0, 0, "7YY");
  ticks(1);
  expect_row("Y carries value east", 0, 0, 4, "7YY7");

  reset();
  row(0, 0, "aV5");   /* write 5 into var a */
  row(1, 0, ".Va");   /* read it back */
  ticks(1);
  expect_row("variable read/write", 2, 1, 1, "5");

  reset();
  row(0, 0, "123G");   /* x=1 y=2 len=3 */
  row(0, 4, "abc");
  ticks(1);
  /* operands land at (x, y+1) measured from G at (0,3) */
  expect_row("generator writes at offset", 3, 4, 3, "abc");

  reset();
  row(0, 0, "23X");   /* x=2 y=3, X sits at (0,2) */
  row(0, 3, "q");
  ticks(1);
  expect_row("teleport writes at offset", 4, 4, 1, "q");

  reset();
  row(0, 0, "12O");   /* reads (x+1, y) == (+2,+2) from O at (0,2) */
  row(2, 4, "k");
  ticks(1);
  expect_row("offset reads at x+1,y", 1, 2, 1, "k");

  reset();
  row(0, 0, "123Q");  /* x=1 y=2 len=3, Q at (0,3) */
  row(2, 5, "wxy");   /* reads (x+1, y) == (+2,+2) .. (+4,+2) */
  ticks(1);
  expect_row("query reads a window", 1, 1, 3, "wxy");

  reset();
  row(0, 0, "13T");
  row(0, 3, "pqr");
  ticks(1);
  expect_row("track picks index key%len", 1, 2, 1, "q");

  reset();
  row(0, 0, "13P");
  row(0, 3, "z");
  ticks(1);
  expect_row("push writes to key%len", 1, 2, 3, ".z.");

  /* --- H halts the cell below, # comments out the rest of the line ------ */
  reset();
  row(0, 1, "H");     /* H halts the cell directly below it */
  row(1, 0, "1A2");
  ticks(1);
  expect_row("halt suppresses cell below", 2, 1, 1, ".");

  reset();
  row(0, 0, "#1A2#");
  row(0, 6, "1A2");
  ticks(1);
  expect_row("comment disables operators", 1, 1, 1, ".");
  expect_row("comment ends at second #", 1, 7, 1, "3");

  /* --- the sound port --------------------------------------------------- */
  reset();
  row(0, 0, "1D1");     /* D at (0,1) bangs (1,1) every tick */
  row(2, 1, ":25C4");   /* instrument 2, octave 5, note C, velocity 4 */
  ticks(1);
  expect_int("note events emitted", nnotes, 1);
  expect_int("note instrument", notes[0].inst, 2);
  expect_int("note pitch (oct*12+C)", notes[0].note, 60);
  expect_int("note velocity", notes[0].vel, 4);

  reset();
  row(0, 0, "1D1");
  row(2, 1, ":25c4");   /* lowercase note == sharp */
  ticks(1);
  expect_int("lowercase note is sharp", notes[0].note, 61);

  /* The fifth operand is note length, in ticks; 0 hands the decision to the
   * instrument. */
  reset();
  row(0, 0, "1D1");
  row(2, 1, ":25C4");
  ticks(1);
  expect_int("absent length defers to instrument", notes[0].len, 0);

  reset();
  row(0, 0, "1D1");
  row(2, 1, ":25C46");
  ticks(1);
  expect_int("length operand passes through", notes[0].len, 6);

  reset();
  row(0, 0, "1D1");
  row(2, 1, ":25C4z");
  ticks(1);
  expect_int("length spans base 36", notes[0].len, 35);

  /* All 36 slots are addressable: the operand is base 36, not a channel. */
  reset();
  row(0, 0, "1D1");
  row(2, 1, ":z5C4");
  ticks(1);
  expect_int("instrument operand spans base 36", notes[0].inst, 35);

  /* --- ports lock their cells so values are never re-evaluated ---------- */
  reset();
  row(0, 0, "2A2");
  row(1, 1, ".");
  ticks(1);
  getrow(1, 1, 1, buf);
  checks++;
  if (buf[0] != '4') { printf("  FAIL output cell: got \"%s\"\n", buf); fails++; }
  ticks(1); /* the '4' below A must not be treated as an operator */
  expect_row("output stays put", 1, 1, 1, "4");

  /* --- port flags: what the renderer paints the masks from -------------- */
  reset();
  row(0, 0, "1A2");
  ticks(1);
  /* "1A2" sits at offsets 0,1,2 of row 0. */
  expect_int("left operand is an input", orca_mark_flags(0) & OG_MK_IN, OG_MK_IN);
  expect_int("right operand is an input", orca_mark_flags(2) & OG_MK_IN, OG_MK_IN);
  expect_int("the cell below is an output",
             orca_mark_flags(GRID_W + 1) & OG_MK_OUT, OG_MK_OUT);
  expect_int("the operator itself is not a port", orca_mark_flags(1), 0);
  expect_int("an untouched cell has no flags", orca_mark_flags(GRID_W * 5), 0);

  reset();
  row(0, 0, ":25C40");
  row(1, 1, "*");
  ticks(1);
  expect_int("note operands are inputs", orca_mark_flags(1) & OG_MK_IN, OG_MK_IN);
  expect_int("note operands are inputs to the end",
             orca_mark_flags(5) & OG_MK_IN, OG_MK_IN);

  /* Marks live only for the tick that made them, so a stale one must not be
   * mistaken for a port. */
  reset();
  row(0, 0, "1A2");
  ticks(1);
  og_grid[1] = '.';
  og_grid[2] = '.';
  og_grid[3] = '.';
  ticks(1);
  expect_int("marks expire with their tick", orca_mark_flags(GRID_W + 1), 0);

  /* --- the dry run: ports without side effects --------------------------- */
  reset();
  row(0, 0, "1A2");
  row(2, 0, "1D1");
  orca_run_marks();
  expect_row("dry run leaves the grid alone", 1, 1, 1, ".");
  expect_row("dry run writes no bangs", 3, 1, 1, ".");
  expect_int("dry run does not advance the clock", (long)og_tick, 0);
  expect_int("dry run still finds the output",
             orca_mark_flags(GRID_W + 1) & OG_MK_OUT, OG_MK_OUT);

  reset();
  row(0, 0, "1D1");
  row(2, 1, ":25C40");
  orca_run_marks();
  expect_int("dry run makes no sound", nnotes, 0);

  /* --- the shipped boot pattern must actually play, on all four voices --- */
  reset();
  {
    int y, t, ch = 0;
    for (y = 0; y < GRID_H; y++)
      row(y, 0, demo_pattern[y]);
    for (t = 0; t < 64; t++)
      orca_run();
    for (t = 0; t < nnotes && t < 32; t++)
      ch |= 1 << notes[t].inst;
    expect_int("demo emits notes", nnotes > 8, 1);
    expect_int("demo uses instruments 0-3", ch, 0x0F);
  }

  printf("%d checks, %d failures\n", checks, fails);
  return fails != 0;
}
