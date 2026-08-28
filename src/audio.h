#ifndef AUDIO_H
#define AUDIO_H
#include "orca.h"

#define INSTR_COUNT 36

/* One instrument slot, addressed by a base-36 glyph in the ':' operand.
 * The channel lives here rather than in the pattern, so a single glyph
 * carries both "which voice" and "what it sounds like". */
typedef struct {
  u8 chan;   /* 0-3 */
  u8 vol;    /* 0-15; on the wave channel this picks one of four levels */
  u8 pan;    /* 0 both, 1 left, 2 right, 3 silent */
  i8 env;    /* 0 hold, <0 decay, >0 swell; magnitude is the period */
  u8 tone;   /* pulse: duty 0-3 | wave: shape 0-7 | noise: 0 = 15-bit, 1 = 7-bit */
  i8 shape;  /* noise only: where the sweep starts, in clock-shift steps away
              * from the note's own value; it glides back to the note and
              * stops there.  <0 starts high and falls, >0 starts low and
              * rises, 0 is static. */
  i8 pitch;  /* tonal channels: how far the note slides, in semitones, and
              * which way -- negative starts high and falls, which is what a
              * kick is.  It lands on the note and stays there. */
  u8 pitchspd; /* 1 slow .. 15 immediate */
  u8 vibdep; /* 0-15 */
  u8 vibspd; /* 0-15 */
  i8 trsp;   /* transpose, -24..+24 semitones */
  u8 len;    /* gate in ticks (0 is treated as 1) */
  /* Appended rather than slotted in, so the offsets above stay put.  The
   * DMG's own sweep unit, which only channel 0 has: it keeps running for as
   * long as the note does, where the software slide lands and stops. */
  i8 sweep;  /* ch0 only: 0 off, sign is the direction, magnitude the shift */
  u8 swtime; /* ch0 only: 1-7; the hardware treats 0 as "sweep off" */
} Instrument;

extern Instrument instruments[INSTR_COUNT];

void snd_init(void);
void snd_defaults(void);

/* A note asks for an instrument, not a channel.  Requests are collected for
 * the whole tick and resolved in snd_dispatch().  len is in ticks; 0 takes
 * the instrument's own setting. */
void snd_request(u8 inst, u8 note, u8 vel, u8 len);
void snd_dispatch(void);

void snd_age(void);      /* one call per ORCA tick: expires finished notes */

/* The editor's TEST button has to work with the sequencer stopped, so its
 * note is timed in frames.  Tell the engine how long a tick currently is. */
void snd_set_tempo(u8 frames_per_tick);
void snd_frame(void);    /* one call per frame: vibrato */
void snd_audition(u8 inst);
void snd_all_off(void);

#endif
