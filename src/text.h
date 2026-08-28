#ifndef TEXT_H
#define TEXT_H
#include <gb/gb.h>
#include "orca.h"

/* The operator names live in bank 1: they are a few hundred bytes of strings
 * that the status bar reads once a tick, and bank 0 has no room to spare. */
const char *name_of(u8 g) BANKED;

#endif
