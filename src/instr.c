/* Instrument page: parameter metadata plus the LSDJ-style editor screen.
 *
 * Which parameters exist depends on the channel -- the wave channel has no
 * hardware envelope and the noise channel has no period to bend -- so the row
 * list is per channel rather than a fixed table with dead entries in it.
 */
#pragma bank 1

#include <gb/gb.h>
#include "instr.h"
#include "audio.h"
#include "vram.h"

enum {
  P_CHAN, P_VOL, P_PAN, P_ENV, P_TONE, P_SHAPE, P_PITCH, P_PITCHSPD, P_SWEEP,
  P_SWTIME, P_VIBDEP, P_VIBSPD, P_TRSP, P_LEN
};

/* The pitch slide is software, so unlike the DMG's hardware sweep it is
 * available on every tonal channel -- which is the point: a kick should not
 * be something only pulse A can do. */
static const u8 rows_ch0[13] = {P_CHAN, P_VOL, P_PAN, P_ENV, P_TONE, P_PITCH,
                                P_PITCHSPD, P_SWEEP, P_SWTIME, P_VIBDEP,
                                P_VIBSPD, P_TRSP, P_LEN};
static const u8 rows_ch1[11] = {P_CHAN, P_VOL, P_PAN, P_ENV, P_TONE, P_PITCH,
                                P_PITCHSPD, P_VIBDEP, P_VIBSPD, P_TRSP, P_LEN};
static const u8 rows_ch2[10] = {P_CHAN, P_VOL, P_PAN, P_TONE, P_PITCH,
                                P_PITCHSPD, P_VIBDEP, P_VIBSPD, P_TRSP, P_LEN};
static const u8 rows_ch3[8] = {P_CHAN, P_VOL, P_PAN, P_ENV, P_TONE, P_SHAPE,
                               P_TRSP, P_LEN};
static const u8 row_counts[4] = {13, 11, 10, 8};

static const u8 *row_map(u8 chan) {
  switch (chan) {
  case 0: return rows_ch0;
  case 1: return rows_ch1;
  case 2: return rows_ch2;
  default: return rows_ch3;
  }
}

static u8 chan_of(u8 inst) { return (u8)(instruments[inst].chan & 3); }

static u8 id_of(u8 inst, u8 row) { return row_map(chan_of(inst))[row]; }

static u8 param_count_for(u8 inst) { return row_counts[chan_of(inst)]; }

static const char *param_label(u8 inst, u8 row) {
  u8 chan = chan_of(inst);
  switch (id_of(inst, row)) {
  case P_CHAN: return "CHAN";
  case P_VOL: return "VOL";
  case P_PAN: return "PAN";
  case P_ENV: return "ENV";
  case P_TONE: return chan == 2 ? "WAVE" : chan == 3 ? "NOISE" : "DUTY";
  case P_SHAPE: return "SHAPE";
  case P_PITCH: return "PITCH";
  case P_PITCHSPD: return "PIT SPD";
  case P_SWEEP: return "SWEEP";
  case P_SWTIME: return "SWP SPD";
  case P_VIBDEP: return "VIB DEP";
  case P_VIBSPD: return "VIB SPD";
  case P_TRSP: return "TRANSP";
  default: return "LEN";
  }
}

static i8 param_get(u8 inst, u8 row) {
  const Instrument *p = &instruments[inst];
  switch (id_of(inst, row)) {
  case P_CHAN: return (i8)p->chan;
  case P_VOL: return (i8)p->vol;
  case P_PAN: return (i8)p->pan;
  case P_ENV: return p->env;
  case P_TONE: return (i8)p->tone;
  case P_SHAPE: return p->shape;
  case P_PITCH: return p->pitch;
  case P_PITCHSPD: return (i8)p->pitchspd;
  case P_SWEEP: return p->sweep;
  case P_SWTIME: return (i8)p->swtime;
  case P_VIBDEP: return (i8)p->vibdep;
  case P_VIBSPD: return (i8)p->vibspd;
  case P_TRSP: return p->trsp;
  default: return (i8)p->len;
  }
}

static void param_set(u8 inst, u8 row, i8 v) {
  Instrument *p = &instruments[inst];
  switch (id_of(inst, row)) {
  case P_CHAN:
    p->chan = (u8)v;
    /* Duty has four settings, the wave shapes eight, the noise width two --
     * clamp so a channel change cannot leave a stale out-of-range tone. */
    if (v == 2) { if (p->tone > 7) p->tone = 7; }
    else if (v == 3) { if (p->tone > 1) p->tone = 1; }
    else if (p->tone > 3) p->tone = 3;
    break;
  case P_VOL: p->vol = (u8)v; break;
  case P_PAN: p->pan = (u8)v; break;
  case P_ENV: p->env = v; break;
  case P_TONE: p->tone = (u8)v; break;
  case P_SHAPE: p->shape = v; break;
  case P_PITCH: p->pitch = v; break;
  case P_PITCHSPD: p->pitchspd = (u8)v; break;
  case P_SWEEP: p->sweep = v; break;
  case P_SWTIME: p->swtime = (u8)v; break;
  case P_VIBDEP: p->vibdep = (u8)v; break;
  case P_VIBSPD: p->vibspd = (u8)v; break;
  case P_TRSP: p->trsp = v; break;
  default: p->len = (u8)v; break;
  }
}

static i8 param_min(u8 inst, u8 row) {
  switch (id_of(inst, row)) {
  case P_ENV: return -7;
  case P_SHAPE: return -7;
  case P_PITCH: return -24;
  case P_SWEEP: return -7;
  case P_TRSP: return -24;
  case P_PITCHSPD: return 1;
  case P_SWTIME: return 1; /* the hardware reads 0 as "sweep off" */
  case P_LEN: return 1; /* a note has to last at least one tick */
  default: return 0;
  }
}

static i8 param_max(u8 inst, u8 row) {
  u8 chan = chan_of(inst);
  switch (id_of(inst, row)) {
  case P_CHAN: return 3;
  case P_VOL: return 15;
  case P_PAN: return 3;
  case P_ENV: return 7;
  case P_TONE: return chan == 2 ? 7 : chan == 3 ? 1 : 3;
  case P_SHAPE: return 7;
  case P_PITCH: return 24;
  case P_PITCHSPD: return 15;
  case P_SWEEP: return 7;
  case P_SWTIME: return 7;
  case P_VIBDEP: return 15;
  case P_VIBSPD: return 15;
  case P_TRSP: return 24;
  default: return 35;
  }
}

static const char *const chan_names[4] = {"PULSE A", "PULSE B", "WAVE", "NOISE"};
static const char *const duty_names[4] = {"12%", "25%", "50%", "75%"};
static const char *const wave_names[8] = {"TRI", "SAW", "SQR", "PUL",
                                          "SIN", "ORG", "RND", "RMP"};
static const char *const pan_names[4] = {"L+R", "LEFT", "RIGHT", "MUTE"};

static const char *param_text(u8 inst, u8 row) {
  const Instrument *p = &instruments[inst];
  u8 chan = chan_of(inst);
  switch (id_of(inst, row)) {
  case P_CHAN: return chan_names[chan];
  case P_PAN: return pan_names[p->pan & 3];
  case P_TONE:
    if (chan == 2) return wave_names[p->tone & 7];
    if (chan == 3) return p->tone ? "7 BIT" : "15 BIT";
    return duty_names[p->tone & 3];
  case P_ENV: return p->env == 0 ? "HOLD" : p->env < 0 ? "DECAY" : "SWELL";
  case P_SHAPE:
    return p->shape == 0 ? "STATIC" : p->shape < 0 ? "FALL TO" : "RISE TO";
  case P_PITCH:
    return p->pitch == 0 ? "STATIC" : p->pitch < 0 ? "FALL TO" : "RISE TO";
  case P_SWEEP:
    return p->sweep == 0 ? "OFF" : p->sweep < 0 ? "FALLING" : "RISING";
  case P_LEN: return "TICKS";
  default: return "";
  }
}

/* ---------------------------------------------------------------- page --- */

#define PAGE_ROWS 16
#define PAGE_COLS 20

static u8 shadow[PAGE_ROWS * PAGE_COLS];
static u8 slot;
static u8 cursor; /* 0 = the slot selector, 1..n = parameter rows */

static u8 direct;        /* set while painting with the lcd off */
static u8 page_complete; /* cleared when the queue refused a tile */

static void put(u8 r, u8 c, char ch) {
  u16 i = (u16)r * PAGE_COLS + c;
  u8 t = TILE(ch);
  u16 addr;
  if (shadow[i] == t)
    return;
  addr = (u16)(BGMAP_ADDR + (((u16)(16 + r)) << 5) + c);
  if (direct) {
    *(u8 *)addr = t;
    shadow[i] = t;
  } else if (vq_push(addr, t)) {
    shadow[i] = t; /* only once it is really on its way */
  } else {
    page_complete = 0;
  }
}

static void put_str(u8 r, u8 c, const char *s, u8 w) {
  u8 i;
  for (i = 0; i < w; i++) {
    put(r, (u8)(c + i), *s ? *s : ' ');
    if (*s)
      s++;
  }
}

static void put_val(u8 r, i8 v, i8 lo) {
  u8 a;
  if (lo < 0) {
    a = (u8)(v < 0 ? -v : v);
    put(r, 9, v < 0 ? '-' : '+');
    put(r, 10, (char)('0' + a / 10u));
    put(r, 11, (char)('0' + a % 10u));
  } else {
    put(r, 9, ' ');
    put(r, 10, ' ');
    put(r, 11, (char)orca_glyph_of((u8)v));
  }
}

void instr_page_init(void) BANKED {
  u16 i;
  for (i = 0; i < PAGE_ROWS * PAGE_COLS; i++)
    shadow[i] = 0;
  slot = 0;
  cursor = 0;
}

void instr_page_invalidate(void) BANKED {
  u16 i;
  for (i = 0; i < PAGE_ROWS * PAGE_COLS; i++)
    shadow[i] = 0;
}

void instr_page_open(u8 s) BANKED {
  if (s < INSTR_COUNT)
    slot = s;
  if (cursor > param_count_for(slot))
    cursor = 0;
}

u8 instr_page_slot(void) BANKED { return slot; }

u8 instr_page_cursor_row(void) BANKED { return cursor == 0 ? 0 : (u8)(cursor + 1); }

const char *instr_page_status(void) BANKED { return "A TEST  B BACK"; }

u8 instr_page_draw(void) BANKED {
  u8 n = param_count_for(slot);
  u8 i, r;

  page_complete = 1;

  put_str(0, 0, " INSTRUMENT", 11);
  put(0, 11, ' ');
  put(0, 12, (char)orca_glyph_of(slot));
  put_str(0, 13, "", 7);
  for (i = 0; i < PAGE_COLS; i++)
    put(1, i, '-');

  for (i = 0; i < n; i++) {
    r = (u8)(2 + i);
    put(r, 0, ' ');
    put_str(r, 1, param_label(slot, i), 8);
    put_val(r, param_get(slot, i), param_min(slot, i));
    put(r, 12, ' ');
    put_str(r, 13, param_text(slot, i), 7);
  }
  /* Thirteen parameter rows leave no room for a help line, and the channel
   * name in the status bar was only repeating the CHAN row anyway -- so the
   * hint goes there instead. */
  for (i = n; i < 14; i++)
    put_str((u8)(2 + i), 0, "", PAGE_COLS);
  return page_complete;
}

void instr_page_repaint(void) BANKED {
  DISPLAY_OFF;
  direct = 1;
  instr_page_draw();
  direct = 0;
  DISPLAY_ON;
}

void instr_page_input(u8 pressed, u8 act) BANKED {
  u8 n = param_count_for(slot);
  u8 p;
  i8 v, lo, hi;

  if ((act & J_UP) && cursor)
    cursor--;
  if ((act & J_DOWN) && cursor < n)
    cursor++;

  if (cursor == 0) {
    if ((act & J_LEFT) && slot)
      slot--;
    if ((act & J_RIGHT) && slot < INSTR_COUNT - 1)
      slot++;
  } else {
    p = (u8)(cursor - 1);
    v = param_get(slot, p);
    lo = param_min(slot, p);
    hi = param_max(slot, p);
    if ((act & J_LEFT) && v > lo)
      param_set(slot, p, (i8)(v - 1));
    if ((act & J_RIGHT) && v < hi)
      param_set(slot, p, (i8)(v + 1));
  }

  /* A channel change rewrites the row list under the cursor. */
  n = param_count_for(slot);
  if (cursor > n)
    cursor = n;

  if (pressed & J_A)
    snd_audition(slot);
}
