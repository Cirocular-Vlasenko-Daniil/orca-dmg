/* The sound engine.
 *
 * ':' names an instrument, not a channel -- the channel is one of the
 * instrument's own settings.  That means a pattern glyph carries both "which
 * voice" and "what it sounds like", and re-voicing a part is one edit on the
 * instrument page instead of a rewrite of the pattern.
 *
 * Requests are collected for a whole tick and resolved in snd_dispatch():
 * when two instruments land on the same channel in the same tick, the lower
 * instrument number wins.  Deciding at the end of the tick rather than on the
 * fly is what makes that rule independent of where the ':' glyphs happen to
 * sit on the grid.
 */
#include <gb/gb.h>
#include <gb/hardware.h>
#include "audio.h"

extern const unsigned int note_period[128];
extern const unsigned char note_noise[128];
extern const unsigned char wave_shapes[128];

Instrument instruments[INSTR_COUNT];

typedef struct {
  u8 active;
  u8 inst;
  u16 period;
  u16 target;   /* where the pitch slide is heading */
  i16 step;     /* ...and how much of the way it covers each frame */
  u8 slide;     /* frames of slide left; 0 once it has arrived */
  u8 phase;
  u8 gate;
} Voice;

typedef struct {
  u8 used;
  u8 inst;
  u8 note;
  u8 vel;
  u8 len;
} Request;

static Voice voice[4];
static Request pending[4];
static u8 cur_wave = 0xFF; /* which shape is loaded in wave RAM */
static u8 nr51_shadow;     /* NR51 is write-only in practice, so mirror it */
static u8 tick_frames = 8; /* how many frames a tick currently lasts */

/* Auditions are timed in frames, not ticks: pressing TEST with the sequencer
 * stopped must still produce a note that ends.  On the wave channel this was
 * plainly audible, because it is the one voice with no envelope to fade it
 * out on its own. */
static u8 aud_ch, aud_timer;

/* Noise sweep state.  Only one channel can be making noise, so this lives
 * here rather than in the voice record.
 *
 * The note sets noise_base and that is where the sound comes to rest; SHAPE
 * only decides how far from it the note starts.  An unbounded sweep -- what
 * this was at first -- runs into the rail within a few frames and every note
 * ends up sounding the same, which throws away the one thing the note was
 * choosing. */
static u8 noise_shift; /* NR43 clock shift, 0-13; larger is lower */
static u8 noise_base;  /* the note's own shift: the resting value */
static u8 noise_low;   /* width bit and divisor, kept as written */
static u8 noise_acc;

#define NOISE_SWEEP_FRAMES 2 /* frames per clock-shift step */

/* A triangle, so vibrato costs one table lookup a frame. */
static const i8 vib_tab[16] = {0, 3, 6, 8, 8, 6, 3, 0, 0, -3, -6, -8, -8, -6, -3, 0};

/* ------------------------------------------------------------- defaults -- */

void snd_defaults(void) {
  u8 i;
  Instrument *p;
  for (i = 0; i < INSTR_COUNT; i++) {
    p = &instruments[i];
    p->chan = (u8)(i & 3);  /* 0..3 keep the classic channel-per-glyph layout */
    p->vol = 15;
    p->pan = 0;             /* both outputs */
    p->env = 0;
    p->tone = 2;            /* 50% duty / square wave */
    p->pitch = 0;
    p->pitchspd = 12;
    p->sweep = 0;
    p->swtime = 3;
    p->vibdep = 0;
    p->vibspd = 0;
    p->shape = 0;
    p->trsp = 0;
    p->len = 2;
  }
  /* Four voices worth reaching for straight off the boot pattern. */
  instruments[0].tone = 2;                              /* pulse A, plain */
  instruments[0].len = 4;                               /* ...used as a bass */
  instruments[1].tone = 1;                              /* pulse B, thinner */
  instruments[1].env = -2;                              /* ...and decaying */
  instruments[1].pan = 1;                               /* ...to the left */
  instruments[2].tone = 0;                              /* wave, triangle */
  instruments[2].pan = 2;                               /* ...to the right */
  instruments[3].env = -3;                              /* noise, percussive */
  instruments[3].tone = 0;
  instruments[3].len = 1;
}

/* ------------------------------------------------------------- playback -- */

static void chan_off(u8 c) {
  switch (c) {
  case 0: NR12_REG = 0x00; NR14_REG = 0x80; break; /* kill the DAC */
  case 1: NR22_REG = 0x00; NR24_REG = 0x80; break;
  case 2: NR30_REG = 0x00; break;
  case 3: NR42_REG = 0x00; NR44_REG = 0x80; break;
  }
  voice[c].active = 0;
}

void snd_all_off(void) {
  u8 c;
  for (c = 0; c < 4; c++) {
    voice[c].gate = 0;
    pending[c].used = 0;
    chan_off(c);
  }
}

/* NR51 packs both outputs into one register: high nibble is left, low nibble
 * is right, one bit per channel.  Panning is therefore a global register that
 * every voice shares, which is why it is mirrored and edited bit by bit. */
static void set_pan(u8 ch, u8 pan) {
  u8 l = (u8)(0x10u << ch);
  u8 r = (u8)(0x01u << ch);
  nr51_shadow &= (u8)~(l | r);
  if (pan == 0)
    nr51_shadow |= (u8)(l | r);
  else if (pan == 1)
    nr51_shadow |= l;
  else if (pan == 2)
    nr51_shadow |= r;
  NR51_REG = nr51_shadow;
}

void snd_set_tempo(u8 frames_per_tick) {
  tick_frames = frames_per_tick ? frames_per_tick : 1;
}

void snd_init(void) {
  NR52_REG = 0x80; /* APU on */
  nr51_shadow = 0xFF;
  NR51_REG = 0xFF; /* every channel to both outputs until a note says otherwise */
  NR50_REG = 0x77; /* full volume, no VIN */
  cur_wave = 0xFF;
  snd_all_off();
}

/* NRx2: volume, envelope direction and period in one byte. */
static u8 env_byte(u8 vol, i8 env) {
  u8 b = (u8)(vol << 4);
  if (env > 0)
    b |= (u8)(0x08 | (u8)env); /* swell */
  else if (env < 0)
    b |= (u8)(-env);           /* decay */
  return b;
}

static void load_wave(u8 shape) {
  u8 i;
  if (shape == cur_wave)
    return;
  cur_wave = shape;
  NR30_REG = 0x00; /* the DAC has to be off while wave RAM is rewritten */
  for (i = 0; i < 16; i++)
    AUD3WAVE[i] = wave_shapes[(u16)shape * 16u + i];
}

/* CH3 runs an octave below the pulses for the same period value, so the
 * conversion has to happen wherever a note becomes a period. */
static u16 period_for(u8 ch, u8 note) {
  u16 p = note_period[note];
  if (ch == 2)
    p = (u16)(2048u - ((2048u - p) >> 1));
  return p;
}

static void write_period(u8 ch, u16 p, u8 trigger) {
  u8 hi = (u8)((p >> 8) & 7);
  if (trigger)
    hi |= 0x80;
  switch (ch) {
  case 0: NR13_REG = (u8)p; NR14_REG = hi; break;
  case 1: NR23_REG = (u8)p; NR24_REG = hi; break;
  case 2: NR33_REG = (u8)p; NR34_REG = hi; break;
  }
}

static void trigger(u8 ch, u8 inst, u8 note, u8 vel) {
  const Instrument *p = &instruments[inst];
  u16 period;
  u8 vol, frames, hw_sweep;
  i16 from_note;

  set_pan(ch, p->pan);
  /* Channel 0 is the only one with a sweep unit.  When it is switched on it
   * owns the frequency registers, so the software slide stands aside rather
   * than the two of them writing over each other every frame. */
  hw_sweep = (u8)(ch == 0 && p->sweep != 0);

  /* ORCA's velocity scales the instrument's own level rather than replacing
   * it, so one instrument can be played soft without being re-edited. */
  vol = (u8)(((u16)p->vol * vel + 7u) / 15u);
  if (vol > 15)
    vol = 15;

  voice[ch].active = 1;
  voice[ch].inst = inst;
  voice[ch].phase = 0;

  if (ch == 3) {
    /* note_noise packs the clock shift in the high nibble and the divisor in
     * the low three bits, leaving bit 3 for the LFSR width. */
    i16 start;
    noise_base = (u8)(note_noise[note] >> 4);
    noise_low = (u8)((note_noise[note] & 0x07u) | (p->tone ? 0x08u : 0x00u));
    start = (i16)noise_base + p->shape; /* the offset is applied once, here */
    if (start < 0)
      start = 0;
    if (start > 13)
      start = 13;
    noise_shift = (u8)start;
    noise_acc = 0;
    NR41_REG = 0x00;
    NR42_REG = env_byte(vol, p->env);
    NR43_REG = (u8)((noise_shift << 4) | noise_low);
    NR44_REG = 0x80;
    return;
  }

  /* The slide is set up once, here: it starts p->pitch semitones away from
   * the note and walks back onto it.  Doing it in period units rather than
   * in semitones keeps it smooth instead of chromatic. */
  period = period_for(ch, note);
  voice[ch].target = period;
  if (p->pitch && !hw_sweep) {
    from_note = (i16)note - p->pitch; /* sign is the direction of travel */
    if (from_note < 0)
      from_note = 0;
    if (from_note > 127)
      from_note = 127;
    frames = (u8)((16u - (p->pitchspd > 15 ? 15 : p->pitchspd)) * 2u);
    if (frames == 0)
      frames = 1;
    voice[ch].period = period_for(ch, (u8)from_note);
    voice[ch].step = (i16)(((i16)period - (i16)voice[ch].period) / (i16)frames);
    voice[ch].slide = frames;
    period = voice[ch].period;
  } else {
    voice[ch].period = period;
    voice[ch].slide = 0;
  }

  switch (ch) {
  case 0:
    NR10_REG = hw_sweep
                   ? (u8)(((u8)(p->swtime & 7) << 4) |
                          (p->sweep < 0 ? 0x08 : 0x00) |
                          (u8)(p->sweep < 0 ? -p->sweep : p->sweep))
                   : 0x00;
    NR11_REG = (u8)((p->tone & 3) << 6);
    NR12_REG = env_byte(vol, p->env);
    break;
  case 1:
    NR21_REG = (u8)((p->tone & 3) << 6);
    NR22_REG = env_byte(vol, p->env);
    break;
  case 2:
    load_wave((u8)(p->tone & 7));
    NR30_REG = 0x80;
    NR31_REG = 0x00;
    /* NR32 has four steps only: mute / 100% / 50% / 25%. */
    NR32_REG = (u8)(vol == 0 ? 0x00 : vol > 10 ? 0x20 : vol > 5 ? 0x40 : 0x60);
    break;
  }
  write_period(ch, period, 1);
}

/* ------------------------------------------------------------- requests -- */

void snd_request(u8 inst, u8 note, u8 vel, u8 len) {
  const Instrument *p;
  u8 ch;
  i16 n;

  if (inst >= INSTR_COUNT)
    return;
  p = &instruments[inst];
  ch = (u8)(p->chan & 3);

  /* Lowest instrument number wins the channel for this tick. */
  if (pending[ch].used && pending[ch].inst <= inst)
    return;

  n = (i16)note + p->trsp;
  if (n < 0)
    n = 0;
  if (n > 127)
    n = 127;

  pending[ch].used = 1;
  pending[ch].inst = inst;
  pending[ch].note = (u8)n;
  pending[ch].vel = vel;
  pending[ch].len = len ? len : p->len;
}

void snd_dispatch(void) {
  u8 ch;
  for (ch = 0; ch < 4; ch++) {
    if (!pending[ch].used)
      continue;
    pending[ch].used = 0;
    if (aud_timer && ch == aud_ch)
      aud_timer = 0; /* the pattern has taken the channel back */
    voice[ch].gate = pending[ch].len ? pending[ch].len : 1;
    trigger(ch, pending[ch].inst, pending[ch].note, pending[ch].vel);
  }
}

void snd_audition(u8 inst) {
  const Instrument *p;
  u16 frames;
  u8 ch;
  if (inst >= INSTR_COUNT)
    return;
  p = &instruments[inst];
  ch = (u8)(p->chan & 3);

  /* Play for the instrument's own length at the current tempo, but count it
   * down in frames so it ends whether or not the sequencer is running. */
  frames = (u16)tick_frames * (p->len ? p->len : 1);
  if (frames < 6)
    frames = 6;
  if (frames > 240)
    frames = 240;
  aud_ch = ch;
  aud_timer = (u8)frames;

  voice[ch].gate = 0; /* not the sequencer's note to expire */
  trigger(ch, inst, 60, 15); /* middle C */
}

void snd_age(void) {
  u8 c;
  for (c = 0; c < 4; c++)
    if (voice[c].gate && --voice[c].gate == 0)
      chan_off(c);
}

void snd_frame(void) {
  const Instrument *p;
  u8 c;
  i16 off;

  if (aud_timer && --aud_timer == 0)
    chan_off(aud_ch);

  /* Noise shape: glide back to the note's own value and stop there, so the
   * note still decides what the tail of the sound is. */
  if (voice[3].active && noise_shift != noise_base) {
    noise_acc++;
    if (noise_acc >= NOISE_SWEEP_FRAMES) {
      noise_acc = 0;
      if (noise_shift < noise_base)
        noise_shift++;
      else
        noise_shift--;
      NR43_REG = (u8)((noise_shift << 4) | noise_low);
    }
  }

  for (c = 0; c < 3; c++) { /* noise has no period to bend */
    u8 moved = 0;
    if (!voice[c].active)
      continue;
    p = &instruments[voice[c].inst];
    if (c == 0 && p->sweep)
      continue; /* the sweep unit is driving this channel's frequency */
    off = 0;

    if (voice[c].slide) {
      voice[c].slide--;
      /* Land exactly on the note rather than wherever the division left us. */
      voice[c].period = voice[c].slide ? (u16)((i16)voice[c].period + voice[c].step)
                                       : voice[c].target;
      moved = 1;
    }
    if (p->vibdep) {
      voice[c].phase = (u8)(voice[c].phase + p->vibspd);
      off = ((i16)vib_tab[(voice[c].phase >> 4) & 15] * (i16)p->vibdep) >> 3;
      moved = 1;
    }
    if (moved)
      write_period(c, (u16)((i16)voice[c].period + off), 0);
  }
}
