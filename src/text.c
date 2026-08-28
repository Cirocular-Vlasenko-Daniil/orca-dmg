#pragma bank 1

#include "text.h"

static const char *const op_names[26] = {
    "ADD",     "SUBTRACT", "CLOCK",  "DELAY",  "EAST",     "IF",
    "GENERATE","HALT",     "INCR",   "JUMPER", "KONKAT",   "LESSER",
    "MULTIPLY","NORTH",    "READ",   "PUSH",   "QUERY",    "RANDOM",
    "SOUTH",   "TRACK",    "EUCLID", "VAR",    "WEST",     "WRITE",
    "YUMPER",  "LERP"};

const char *name_of(u8 g) BANKED {
  u8 up = (u8)(g & (u8)~0x20u);
  if (up >= 'A' && up <= 'Z')
    return op_names[up - 'A'];
  switch (g) {
  case '*': return "BANG";
  case '#': return "COMMENT";
  case ':': return "INS OCT NT VEL LEN"; /* the five operands, in order */
  case '.': return "";
  }
  if (g >= '0' && g <= '9')
    return "VALUE";
  return "";
}
