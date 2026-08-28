#include "orca.h"

u8 og_grid[GRID_N];
u8 og_mark[GRID_N];
u16 og_tick;

static u8 vars[36];
static u16 rng_state = 0x2545u;

/* Marks carry the port flags in the low three bits and a per-tick generation
 * in the top five, so one byte answers both "has this cell been spoken for"
 * and "what is it to the operator that spoke for it".  Stamping a generation
 * rather than clearing matters: wiping 512 bytes every tick cost more than
 * every operator in a busy pattern put together. */
#define MK_GEN OG_MK_GEN
u8 og_mark_gen;

u16 og_touch[OG_TOUCH_MAX];
u8 og_touch_n;
u8 og_touch_over;

static void touch(u16 o) {
  if (og_touch_n < OG_TOUCH_MAX)
    og_touch[og_touch_n++] = o;
  else
    og_touch_over = 1;
}

/* Set while re-deriving marks for the editor: everything runs, nothing is
 * allowed to change. */
static u8 dry_run;

/* Cursor of the cell currently being evaluated.  Kept in globals rather than
 * passed around: sdcc puts arguments on a software stack and every extra
 * parameter costs real cycles on a 4 MHz part. */
static u8 cy, cx;
static u8 cg;

static const u8 glyph_table[36] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b',
    'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};

static const u8 index_table[128] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  0,  0,  0,  0,  0,  0,
    0,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0,  0,  0,  0,  0,
    0,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0,  0,  0,  0,  0};

u8 orca_index_of(u8 g) { return index_table[g & 0x7f]; }
u8 orca_glyph_of(u8 v) { return glyph_table[v % 36u]; }

#define IDX(g) index_table[(g) & 0x7f]

/* ---------------------------------------------------------------- grid ---- */

static u8 peek(i16 y, i16 x) {
  if (y < 0 || x < 0 || y >= GRID_H || x >= GRID_W)
    return '.';
  return og_grid[(u16)y * GRID_W + (u16)x];
}

static void poke(i16 y, i16 x, u8 g) {
  u16 o;
  if (dry_run)
    return;
  if (y < 0 || x < 0 || y >= GRID_H || x >= GRID_W)
    return;
  o = (u16)y * GRID_W + (u16)x;
  if (og_grid[o] != g) {
    og_grid[o] = g;
    touch(o);
    orca_cell_changed(o, g);
  }
}

/* Marking a cell makes the scan skip it this tick, and records what the cell
 * is being used for.  orca-c keeps "sleep" separate from "lock"; the split
 * only drives its colouring, so both land here as OG_MK_LOCK. */
static void mark_at(i16 y, i16 x, u8 f) {
  u16 o;
  if (y < 0 || x < 0 || y >= GRID_H || x >= GRID_W)
    return;
  o = (u16)y * GRID_W + (u16)x;
  if ((og_mark[o] & MK_GEN) == og_mark_gen) {
    og_mark[o] |= f;
  } else {
    og_mark[o] = (u8)(og_mark_gen | f); /* first mark this tick: list it once */
    touch(o);
  }
}

u8 orca_mark_flags(u16 offs) {
  u8 m = og_mark[offs];
  return ((m & MK_GEN) == og_mark_gen) ? (u8)(m & 0x07u) : 0;
}

void orca_poke_abs(u8 y, u8 x, u8 g) {
  u16 o;
  if (dry_run)
    return;
  o = (u16)y * GRID_W + x;
  if (og_grid[o] != g) {
    og_grid[o] = g;
    touch(o);
    orca_cell_changed(o, g);
  }
}

#define PEEK(dy, dx) peek((i16)cy + (dy), (i16)cx + (dx))
#define POKE(dy, dx, g) poke((i16)cy + (dy), (i16)cx + (dx), (g))
#define LOCK(dy, dx) mark_at((i16)cy + (dy), (i16)cx + (dx), OG_MK_LOCK)
#define PIN(dy, dx)                                                            \
  mark_at((i16)cy + (dy), (i16)cx + (dx), (u8)(OG_MK_LOCK | OG_MK_IN))
#define POUT(dy, dx)                                                           \
  mark_at((i16)cy + (dy), (i16)cx + (dx), (u8)(OG_MK_LOCK | OG_MK_OUT))

static void poke_stunned(i16 dy, i16 dx, u8 g) {
  poke((i16)cy + dy, (i16)cx + dx, g);
  mark_at((i16)cy + dy, (i16)cx + dx, (u8)(OG_MK_LOCK | OG_MK_OUT));
}

/* Output takes the case of the right-hand operand -- this is what makes
 * `2A5` produce `7` but `2Ae` produce a lowercase result. */
static u8 with_case(u8 g, u8 caser) {
  return (u8)((g & (u8)~0x20u) | (u8)((~g & 0x40u) >> 1) | (u8)(caser & 0x20u));
}

static u8 banged(void) {
  if (PEEK(0, 1) == '*')
    return 1;
  if (PEEK(0, -1) == '*')
    return 1;
  if (PEEK(1, 0) == '*')
    return 1;
  if (PEEK(-1, 0) == '*')
    return 1;
  return 0;
}

/* Returns 255 when the glyph is not a note. */
static u8 note_number_of(u8 g) {
  static const u8 semis[7] = {0, 2, 4, 5, 7, 9, 11};
  u8 sharp = (u8)((g & 0x20u) >> 5);
  u8 deg;
  g &= (u8)~0x20u;
  if (g < 'A' || g > 'Z')
    return 255;
  /* C=0; A and B sit above G, exactly as in orca-c. */
  deg = (g <= 'B') ? (u8)('G' - 'B' + g - 'A') : (u8)(g - 'C');
  return (u8)(deg / 7u * 12u + semis[deg % 7u] + sharp);
}

/* ----------------------------------------------------------- operators ---- */

#define REQUIRE_BANG_IF_LOWER                                                  \
  if ((cg & 0x20u) && !banged())                                               \
  return

static void op_add(void) {
  u8 a, b;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  a = PEEK(0, -1);
  b = PEEK(0, 1);
  POKE(1, 0, with_case(glyph_table[(IDX(a) + IDX(b)) % 36u], b));
}

static void op_subtract(void) {
  u8 a, b;
  i16 v;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  a = PEEK(0, -1);
  b = PEEK(0, 1);
  v = (i16)IDX(b) - (i16)IDX(a);
  if (v < 0)
    v = -v;
  POKE(1, 0, with_case(glyph_table[v], b));
}

static void op_clock(void) {
  u8 b, rate, m;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  b = PEEK(0, 1);
  rate = IDX(PEEK(0, -1));
  m = IDX(b);
  if (rate == 0)
    rate = 1;
  if (m == 0)
    m = 8;
  POKE(1, 0, with_case(glyph_table[(og_tick / rate) % m], b));
}

static void op_delay(void) {
  u8 rate, m;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  rate = IDX(PEEK(0, -1));
  m = IDX(PEEK(0, 1));
  if (rate == 0)
    rate = 1;
  if (m == 0)
    m = 8;
  POKE(1, 0, (u8)((og_tick % ((u16)rate * m)) == 0 ? '*' : '.'));
}

static void op_if(void) {
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  POKE(1, 0, (u8)(PEEK(0, -1) == PEEK(0, 1) ? '*' : '.'));
}

static void op_movement(void) {
  i16 dy = 0, dx = 0;
  i16 y0, x0;
  u8 up = (u8)(cg & (u8)~0x20u);
  REQUIRE_BANG_IF_LOWER;
  switch (up) {
  case 'N': dy = -1; break;
  case 'E': dx = 1; break;
  case 'S': dy = 1; break;
  case 'W': dx = -1; break;
  }
  y0 = (i16)cy + dy;
  x0 = (i16)cx + dx;
  if (y0 < 0 || x0 < 0 || y0 >= GRID_H || x0 >= GRID_W) {
    orca_poke_abs(cy, cx, '*');
    return;
  }
  if (peek(y0, x0) == '.') {
    poke(y0, x0, cg);
    orca_poke_abs(cy, cx, '.');
    mark_at(y0, x0, OG_MK_LOCK);
  } else {
    orca_poke_abs(cy, cx, '*');
  }
}

static void op_generator(void) {
  i16 ox, oy, len, i;
  REQUIRE_BANG_IF_LOWER;
  ox = (i16)IDX(PEEK(0, -3));
  oy = (i16)IDX(PEEK(0, -2)) + 1;
  len = (i16)IDX(PEEK(0, -1));
  PIN(0, -3); PIN(0, -2); PIN(0, -1);
  for (i = 0; i < len; i++) {
    PIN(0, i + 1);
    poke_stunned(oy, ox + i, PEEK(0, i + 1));
  }
}

static void op_halt(void) {
  REQUIRE_BANG_IF_LOWER;
  PIN(1, 0);
}

static void op_increment(void) {
  u8 ga, gb, rate, max, val;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  ga = PEEK(0, -1);
  gb = PEEK(0, 1);
  rate = 1;
  if (ga != '.' && ga != '*')
    rate = IDX(ga);
  max = IDX(gb);
  val = IDX(PEEK(1, 0));
  if (max == 0)
    max = 36;
  val = (u8)((val + rate) % max);
  POKE(1, 0, with_case(glyph_table[val], gb));
}

static void op_jump(void) {
  u8 g;
  i16 i;
  REQUIRE_BANG_IF_LOWER;
  g = PEEK(-1, 0);
  if (g == 'J')
    return;
  PIN(-1, 0);
  for (i = 1; i < GRID_H; i++) {
    if (PEEK(i, 0) != cg) {
      POUT(i, 0);
      POKE(i, 0, g);
      break;
    }
    LOCK(i, 0);
  }
}

static void op_konkat(void) {
  i16 len, i;
  u8 var;
  REQUIRE_BANG_IF_LOWER;
  len = (i16)IDX(PEEK(0, -1));
  if (len == 0)
    len = 1;
  PIN(0, -1);
  for (i = 0; i < len; i++) {
    PIN(0, i + 1);
    var = PEEK(0, i + 1);
    if (var != '.') {
      POUT(1, i + 1);
      POKE(1, i + 1, vars[IDX(var)]);
    }
  }
}

static void op_lesser(void) {
  u8 ga, gb, ia, ib;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  ga = PEEK(0, -1);
  gb = PEEK(0, 1);
  if (ga == '.' || gb == '.') {
    POKE(1, 0, '.');
  } else {
    ia = IDX(ga);
    ib = IDX(gb);
    POKE(1, 0, with_case(glyph_table[ia < ib ? ia : ib], gb));
  }
}

static void op_multiply(void) {
  u8 a, b;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  a = PEEK(0, -1);
  b = PEEK(0, 1);
  POKE(1, 0, with_case(glyph_table[((u16)IDX(a) * IDX(b)) % 36u], b));
}

static void op_offset(void) {
  i16 ix, iy;
  REQUIRE_BANG_IF_LOWER;
  ix = (i16)IDX(PEEK(0, -2)) + 1;
  iy = (i16)IDX(PEEK(0, -1));
  PIN(0, -1); PIN(0, -2);
  PIN(iy, ix); POUT(1, 0);
  POKE(1, 0, PEEK(iy, ix));
}

static void op_push(void) {
  u8 key, len;
  i16 ox, i;
  REQUIRE_BANG_IF_LOWER;
  key = IDX(PEEK(0, -2));
  len = IDX(PEEK(0, -1));
  PIN(0, -1); PIN(0, -2); PIN(0, 1);
  if (len == 0)
    return;
  ox = (i16)(key % len);
  for (i = 0; i < len; i++)
    LOCK(1, i);
  POUT(1, ox);
  POKE(1, ox, PEEK(0, 1));
}

static void op_query(void) {
  i16 ix, iy, len, ox, i;
  REQUIRE_BANG_IF_LOWER;
  ix = (i16)IDX(PEEK(0, -3)) + 1;
  iy = (i16)IDX(PEEK(0, -2));
  len = (i16)IDX(PEEK(0, -1));
  ox = 1 - len;
  PIN(0, -3); PIN(0, -2); PIN(0, -1);
  for (i = 0; i < len; i++) {
    PIN(iy, ix + i);
    POUT(1, ox + i);
    POKE(1, ox + i, PEEK(iy, ix + i));
  }
}

/* orca-c hashes a 32-bit key; on this CPU that is a lot of shifting for a
 * value nobody can tell apart from a cheap xorshift.  Position and tick still
 * feed the state so two R's on one row do not lock together. */
static void op_random(void) {
  u8 gb, a, b, lo, hi;
  u16 r;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  gb = PEEK(0, 1);
  a = IDX(PEEK(0, -1));
  b = IDX(gb);
  if (b == 0)
    b = 36;
  if (a == b) {
    POKE(1, 0, glyph_table[a]);
    return;
  }
  if (a < b) { lo = a; hi = b; } else { lo = b; hi = a; }
  r = rng_state;
  r ^= (u16)(r << 7);
  r ^= (u16)(r >> 9);
  r ^= (u16)(r << 8);
  r += (u16)((u16)cy * GRID_W + cx) + og_tick;
  rng_state = r;
  POKE(1, 0, with_case(glyph_table[lo + (r % (u16)(hi - lo))], gb));
}

static void op_track(void) {
  u8 key, len;
  i16 rx, i;
  REQUIRE_BANG_IF_LOWER;
  key = IDX(PEEK(0, -2));
  len = IDX(PEEK(0, -1));
  PIN(0, -2); PIN(0, -1);
  if (len == 0)
    return;
  rx = (i16)(key % len) + 1;
  for (i = 0; i < len; i++)
    LOCK(0, i + 1);
  PIN(0, rx); POUT(1, 0);
  POKE(1, 0, PEEK(0, rx));
}

static void op_uclid(void) {
  u8 left, steps, max;
  u16 bucket;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  left = PEEK(0, -1);
  steps = 1;
  if (left != '.' && left != '*')
    steps = IDX(left);
  max = IDX(PEEK(0, 1));
  if (max == 0)
    max = 8;
  bucket = (u16)(((u16)steps * (og_tick + max - 1u)) % max) + steps;
  POKE(1, 0, (u8)(bucket >= max ? '*' : '.'));
}

static void op_variable(void) {
  u8 left, right;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1);
  left = PEEK(0, -1);
  right = PEEK(0, 1);
  if (left != '.') {
    vars[IDX(left)] = right;
  } else if (right != '.') {
    POUT(1, 0);
    POKE(1, 0, vars[IDX(right)]);
  }
}

static void op_teleport(void) {
  i16 ox, oy;
  REQUIRE_BANG_IF_LOWER;
  ox = (i16)IDX(PEEK(0, -2));
  oy = (i16)IDX(PEEK(0, -1)) + 1;
  PIN(0, -2); PIN(0, -1); PIN(0, 1);
  poke_stunned(oy, ox, PEEK(0, 1));
}

static void op_yump(void) {
  u8 g;
  i16 i;
  REQUIRE_BANG_IF_LOWER;
  g = PEEK(0, -1);
  if (g == 'Y')
    return;
  PIN(0, -1);
  for (i = 1; i < GRID_W; i++) {
    if (PEEK(0, i) != cg) {
      POUT(0, i);
      POKE(0, i, g);
      break;
    }
    LOCK(0, i);
  }
}

static void op_lerp(void) {
  u8 g, b;
  i16 rate, goal, val, mod;
  REQUIRE_BANG_IF_LOWER;
  PIN(0, -1); PIN(0, 1); POUT(1, 0);
  g = PEEK(0, -1);
  b = PEEK(0, 1);
  rate = (g == '.' || g == '*') ? 1 : (i16)IDX(g);
  goal = (i16)IDX(b);
  val = (i16)IDX(PEEK(1, 0));
  mod = (val <= goal - rate) ? rate : (val >= goal + rate) ? -rate : (goal - val);
  POKE(1, 0, with_case(glyph_table[val + mod], b));
}

static void op_comment(void) {
  u16 row = (u16)cy * GRID_W;
  u8 x0;
  for (x0 = (u8)(cx + 1); x0 < GRID_W; x0++) {
    mark_at((i16)cy, (i16)x0, OG_MK_LOCK);
    if (og_grid[row + x0] == '#')
      break;
  }
}

/* ':' -- the first operand names one of the 36 instrument slots.  Which
 * channel it reaches, and what it sounds like, is the instrument's business;
 * this only resolves pitch and velocity and hands the request over. */
static void op_midi(void) {
  u8 i, cg_, og_, ng_, vg_, lg_, oct, note, inst, vel;
  for (i = 1; i < 6; i++)
    PIN(0, i);
  if (!banged())
    return;
  cg_ = PEEK(0, 1);
  og_ = PEEK(0, 2);
  ng_ = PEEK(0, 3);
  vg_ = PEEK(0, 4);
  lg_ = PEEK(0, 5);
  if (og_ == '.')
    return;
  oct = IDX(og_);
  if (oct > 9)
    oct = 9;
  note = note_number_of(ng_);
  if (note == 255)
    return;
  inst = IDX(cg_); /* 0..35, one slot per base-36 glyph */
  if (vg_ == '.') {
    vel = 15;
  } else {
    vel = IDX(vg_);
    if (vel == 0)
      return;
    if (vel > 15)
      vel = 15;
  }
  POUT(0, 0);
  if (dry_run)
    return; /* the ports are all we came for */
  note = (u8)(oct * 12u + note);
  if (note > 127)
    note = 127;
  orca_note_event(inst, note, vel, IDX(lg_)); /* 0 defers to the instrument */
}

/* ---------------------------------------------------------------- run ----- */

void orca_init(void) {
  u16 i;
  for (i = 0; i < GRID_N; i++) {
    og_grid[i] = '.';
    og_mark[i] = 0;
  }
  for (i = 0; i < 36; i++)
    vars[i] = '.';
  og_tick = 0;
  og_mark_gen = 0;
  og_touch_n = 0;
  og_touch_over = 1; /* nothing painted yet */
  dry_run = 0;
  rng_state = 0x2545u;
}

/* The scan lives in its own leaf function on purpose.  With the dispatch
 * switch inline, sdcc has to spill the loop counter to the stack across every
 * call, and each empty cell then costs a pop/push pair -- which on a 512-cell
 * grid dwarfs the operators themselves.  With no calls in here, the counter
 * and the cursor both stay in registers. */
static u16 next_cell(u16 start) {
  u16 i;
  for (i = start; i < GRID_N; i++)
    if (og_grid[i] != '.' &&
        !((og_mark[i] & MK_GEN) == og_mark_gen && (og_mark[i] & OG_MK_LOCK)))
      break;
  return i;
}

void orca_run(void) {
  u16 i;
  u8 g, up;

  og_touch_n = 0;
  og_touch_over = 0;

  /* Advance the generation; only on wrap do we pay for a real clear. */
  og_mark_gen = (u8)(og_mark_gen + 8u);
  if ((og_mark_gen & MK_GEN) == 0) {
    for (i = 0; i < GRID_N; i++)
      og_mark[i] = 0;
    og_mark_gen = 8;
  }
  for (i = 0; i < 36; i++)
    vars[i] = '.';

  for (i = next_cell(0); i < GRID_N; i = next_cell((u16)(i + 1))) {
    g = og_grid[i];
    cy = (u8)(i >> 5);
    cx = (u8)i & 31u;
    cg = g;
    if (g == '#') {
      op_comment();
      continue;
    }
    if (g == '*') {
      orca_poke_abs(cy, cx, '.');
      continue;
    }
    if (g == ':') {
      op_midi();
      continue;
    }
    up = (u8)(g & (u8)~0x20u);
    if (up < 'A' || up > 'Z')
      continue;
    switch (up) {
    case 'A': op_add(); break;
    case 'B': op_subtract(); break;
    case 'C': op_clock(); break;
    case 'D': op_delay(); break;
    case 'E': op_movement(); break;
    case 'F': op_if(); break;
    case 'G': op_generator(); break;
    case 'H': op_halt(); break;
    case 'I': op_increment(); break;
    case 'J': op_jump(); break;
    case 'K': op_konkat(); break;
    case 'L': op_lesser(); break;
    case 'M': op_multiply(); break;
    case 'N': op_movement(); break;
    case 'O': op_offset(); break;
    case 'P': op_push(); break;
    case 'Q': op_query(); break;
    case 'R': op_random(); break;
    case 'S': op_movement(); break;
    case 'T': op_track(); break;
    case 'U': op_uclid(); break;
    case 'V': op_variable(); break;
    case 'W': op_movement(); break;
    case 'X': op_teleport(); break;
    case 'Y': op_yump(); break;
    case 'Z': op_lerp(); break;
    }
  }
  if (!dry_run)
    og_tick++;
}

void orca_run_marks(void) {
  dry_run = 1;
  orca_run();
  dry_run = 0;
}
