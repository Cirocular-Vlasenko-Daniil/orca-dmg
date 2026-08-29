#!/usr/bin/env python3
"""Drives the real ROM in an emulator and asserts on what the PPU actually
holds.  Reading the tilemap beats eyeballing a screenshot: tile == glyph-32,
so the assertions are written in ORCA glyphs."""
import re
import sys
from pyboy import PyBoy


def symbol(name):
    """sdld truncates names in the map file, so match on the prefix."""
    for addr, sym in re.findall(r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()):
        if name.startswith(sym):
            return int(addr, 16)
    raise KeyError(name)

ROM = "build/orca-dmg.gb"
fails = []


def check(name, got, want):
    if got != want:
        fails.append("%-34s got %r want %r" % (name, got, want))
        print("  FAIL %-32s got %r want %r" % (name, got, want))
    else:
        print("  ok   %s" % name)


# Tiles 0..95 are the glyphs, 96..191 the same glyphs in reverse video (an
# output port), and 192 marks an input port that is still empty.
def glyph(t):
    t &= 0xFF
    if t == 192:
        return "."
    if 96 <= t < 192:
        t -= 96
    return chr(t + 32)


def is_output_port(pb, x, y):
    return 96 <= (pb.tilemap_background[x, y] & 0xFF) < 192


def is_empty_input_port(pb, x, y):
    return (pb.tilemap_background[x, y] & 0xFF) == 192


def bg(pb, x, y):
    return glyph(pb.tilemap_background[x, y])


def bg_row(pb, y, x=0, n=32):
    return "".join(bg(pb, x + i, y) for i in range(n))


def win_row(pb, y, x=0, n=20):
    return "".join(glyph(pb.tilemap_window[x + i, y]) for i in range(n))


def press(pb, btn, hold=4, after=4):
    pb.button_press(btn)
    pb.tick(hold, False)
    pb.button_release(btn)
    pb.tick(after, False)


pb = PyBoy(ROM, window="null", sound_emulated=True, sound_volume=0)
pb.set_emulation_speed(0)

# --- boots and paints ------------------------------------------------------
pb.tick(120, False)   # gbdk's crt0 alone costs ~66 frames before main()
check("cartridge title", pb.cartridge_title, "ORCA-DMG")
check("comment row rendered", bg_row(pb, 8, 0, 14), "#.A.ORCA.DMG.#")
check("clock row rendered", bg_row(pb, 4, 0, 4), ".1C4")
check("status line", win_row(pb, 0)[:7], "BPM120 ")
check("transport shows playing", win_row(pb, 0)[18], ">")

# --- the sequencer is actually running ------------------------------------
f0 = win_row(pb, 0)[7:12]
pb.tick(90, False)
f1 = win_row(pb, 0)[7:12]
check("frame counter advances", f0 != f1, True)

# D at (0,1) drives a bang into (1,1); over a few ticks it must appear.
seen_bang = False
for _ in range(120):
    pb.tick(1, False)
    if bg(pb, 1, 1) == "*":
        seen_bang = True
        break
check("delay emits a bang", seen_bang, True)

# T at (5,4) writes the arpeggio note into the ':' note port at (6,4).
notes = set()
for _ in range(360):
    pb.tick(1, False)
    notes.add(bg(pb, 4, 6))
check("track cycles the arpeggio", sorted(notes - {"."}), ["A", "C", "E", "G"])

# --- the APU is being driven ----------------------------------------------
touched = set()
for _ in range(240):
    pb.tick(1, False)
    if pb.memory[0xFF12]: touched.add("ch1")
    if pb.memory[0xFF17]: touched.add("ch2")
    if pb.memory[0xFF1A]: touched.add("ch3")
    if pb.memory[0xFF21]: touched.add("ch4")
check("channels receive notes", sorted(touched), ["ch1", "ch2", "ch3", "ch4"])

# --- tempo accuracy -------------------------------------------------------
# A busy pattern pushes a tick past one frame; the clock must still land on
# 3600/(bpm*4) frames per tick rather than drifting with the workload.
TICK = symbol("_og_tick")


def tickno(pb):
    return pb.memory[TICK] | (pb.memory[TICK + 1] << 8)


start_tick, start_frame = tickno(pb), pb.frame_count
while tickno(pb) - start_tick < 48:
    pb.tick(1, False)
measured = (pb.frame_count - start_frame) / float(tickno(pb) - start_tick)
check("tempo within 5%% of 7.50 frames/tick (got %.2f)" % measured,
      abs(measured - 7.5) < 0.375, True)

# --- transport ------------------------------------------------------------
press(pb, "start")
pb.tick(4, False)
check("start pauses", win_row(pb, 0)[18], "|")
f0 = win_row(pb, 0)[7:12]
pb.tick(60, False)
check("paused clock is frozen", win_row(pb, 0)[7:12], f0)
press(pb, "start")
pb.tick(4, False)
check("start resumes", win_row(pb, 0)[18], ">")

# --- editing: move to an empty cell, open the picker, insert a glyph -------
for _ in range(12):
    press(pb, "down")
for _ in range(3):
    press(pb, "right")
pb.tick(4, False)
check("cursor position readout", win_row(pb, 0)[13:16], "@3c")

press(pb, "a")                       # open picker
pb.tick(6, False)
check("picker palette row 0", win_row(pb, 2), "0123456789ABCDEFGHIJ")
check("picker palette row 3", win_row(pb, 5)[:6], "uvwxyz")
# The picker opens on whatever glyph the cursor is already sitting on -- an
# empty cell, so on '.' at row 1 col 16.  One row up from there is 'G'.
check("picker preselects current glyph", (pb.memory[0xFF4A], win_row(pb, 3)[16]), (96, "."))
press(pb, "up")
press(pb, "a")                       # commit 'G'
pb.tick(6, False)
check("picker inserted glyph", bg(pb, 3, 12), "G")

press(pb, "b")                       # erase it again
pb.tick(6, False)
check("b erases", bg(pb, 3, 12), ".")

# --- the picker must not hide the cell being edited ---------------------
for _ in range(3):
    press(pb, "down")            # cursor to row 15
pb.tick(4, False)
check("cursor at last row", win_row(pb, 0)[13:16], "@3f")
press(pb, "a")
pb.tick(8, False)
check("grid scrolls out from under the picker", pb.memory[0xFF42], 32)
cursor_y = pb.memory[0xFE00]     # OAM entry 0: y + 16
check("cursor sprite stays above the palette", cursor_y - 16 < 96, True)
press(pb, "b")
pb.tick(8, False)
check("closing the picker restores scroll", pb.memory[0xFF42], 0)

# --- bpm ------------------------------------------------------------------
pb.button_press("select")
pb.tick(2, False)
press(pb, "up")
pb.button_release("select")
pb.tick(4, False)
check("select+up raises bpm", win_row(pb, 0)[:6], "BPM130")

# Worst case: at the top of the range a tick period is only ~3.6 frames,
# barely more than the tick itself costs.
pb.button_press("select")
pb.tick(2, False)
for _ in range(12):
    press(pb, "up")
pb.button_release("select")
pb.tick(4, False)
check("bpm reaches the top of the range", win_row(pb, 0)[:6], "BPM250")
start_tick, start_frame = tickno(pb), pb.frame_count
while tickno(pb) - start_tick < 48:
    pb.tick(1, False)
measured = (pb.frame_count - start_frame) / float(tickno(pb) - start_tick)
check("tempo holds at max bpm (%.2f vs 3.60 frames/tick)" % measured,
      abs(measured - 3.6) < 0.4, True)

# --- the instrument page --------------------------------------------------
# It lives in background rows 16-31, so opening it is a write to SCY and the
# grid stays exactly where it was.
pb.button_press("select")
pb.tick(2, False)
press(pb, "start")
pb.button_release("select")
pb.tick(8, False)
check("instrument page scrolls into view", pb.memory[0xFF42], 128)
check("page header", bg_row(pb, 16, 0, 13), " INSTRUMENT 0")
check("first parameter row", bg_row(pb, 18, 0, 20), " CHAN      0 PULSE A")
check("status shows the slot", win_row(pb, 0)[13:16], "I0 ")

press(pb, "down")                    # onto CHAN
press(pb, "right")                   # -> channel 1
pb.tick(20, False)
check("channel edit takes effect", bg_row(pb, 18, 0, 20), " CHAN      1 PULSE B")
check("row list follows the channel", bg_row(pb, 22, 1, 4), "DUTY")

press(pb, "left")                    # back to channel 0
pb.tick(40, False)
# A channel change reflows most of the page.  Comparing every row is the
# direct test for queued writes going missing: a dropped tile is recorded in
# the page's shadow and would never be retried.
check("page reflows completely", [bg_row(pb, 16 + i, 0, 20) for i in range(2, 15)], [
    " CHAN      0 PULSE A",
    " VOL       f        ",
    " PAN       0 L+R    ",
    " ENV     +00 HOLD   ",
    " DUTY      2 50%    ",
    " PITCH   +00 STATIC ",
    " PIT SPD   c        ",
    " SWEEP   +00 OFF    ",
    " SWP SPD   3        ",
    " VIB DEP   0        ",
    " VIB SPD   0        ",
    " TRANSP  +00        ",
    " LEN       4 TICKS  ",
])

press(pb, "a")                       # audition
pb.tick(4, False)
check("audition drives the channel", pb.memory[0xFF12] != 0, True)

# --- instruments survive a save/load round trip ---------------------------
INSTR = symbol("_instruments")
ISIZE = 14
F_VOL, F_PAN, F_TONE, F_SHAPE = 1, 2, 4, 5
F_PITCH, F_PITCHSPD, F_VIBDEP, F_VIBSPD, F_LEN = 6, 7, 8, 9, 11
F_SWEEP, F_SWTIME = 12, 13


def ifield(slot, field):
    return INSTR + slot * ISIZE + field


VOL = ifield(5, F_VOL)
pb.memory[VOL] = 9
pb.button_press("select")
pb.tick(2, False)
press(pb, "a")                       # save
pb.button_release("select")
pb.tick(8, False)
pb.memory[VOL] = 2                   # scribble over it
pb.button_press("select")
pb.tick(2, False)
press(pb, "b")                       # load
pb.button_release("select")
pb.tick(20, False)
check("instruments round-trip through sram", pb.memory[VOL], 9)

press(pb, "b")                       # back to the grid
pb.tick(20, False)
check("leaving the page restores the grid", pb.memory[0xFF42], 0)

# --- the priority rule ----------------------------------------------------
# Instruments 0 and 4 both default to channel 0.  When both fire in the same
# tick the lower number must win, whichever one the grid scan reaches first.
GRID = symbol("_og_grid")


def clear_grid():
    for i in range(512):
        pb.memory[GRID + i] = ord(".")


def poke(y, x, text):
    for i, c in enumerate(text):
        pb.memory[GRID + y * 32 + x + i] = ord(c)


def envelopes_seen(frames=150):
    pb.tick(30, False)   # let the previous note's gate expire first
    seen = set()
    for _ in range(frames):
        pb.tick(1, False)
        v = pb.memory[0xFF12]
        if v:
            seen.add(v)
    return seen


pb.memory[ifield(4, F_VOL)] = 8    # instrument 4: half volume -> NR12 0x80
                                     # instrument 0: full        -> NR12 0xF0
clear_grid()
poke(0, 0, "1D1")
poke(0, 11, "1D1")
poke(2, 1, ":05Cf2")                 # instrument 0, scanned first
poke(2, 12, ":45Ef2")                # instrument 4, scanned second
check("lower instrument wins (0 before 4)", envelopes_seen(), {0xF0})

clear_grid()
poke(0, 0, "1D1")
poke(0, 11, "1D1")
poke(2, 1, ":45Ef2")                 # now instrument 4 is scanned first
poke(2, 12, ":05Cf2")
check("lower instrument wins (4 before 0)", envelopes_seen(), {0xF0})

clear_grid()                          # control: alone, instrument 4 is heard
poke(0, 0, "1D1")
poke(2, 1, ":45Ef2")
check("instrument 4 plays when unopposed", envelopes_seen(), {0x80})

# --- vibrato and the instrument's own note length -------------------------
# NR13/NR14 are write-only on real hardware, so the period cannot be read
# back; the voice state in wram is the honest observable.
def audio_static(name):
    offs = {}
    for line in open("build/audio.sym"):
        m = re.match(r"\s*(\d)\s+(_\w+)\s+([0-9A-F]{8})", line)
        if m:
            offs[m.group(2)] = (int(m.group(1)), int(m.group(3), 16))
    area, off = offs[name]
    anchor_area, anchor_off = offs["_instruments"]
    assert area == anchor_area, name
    return symbol("_instruments") - anchor_off + off


# Voice: {active, inst, period:2, target:2, step:2, slide, phase, gate}
VOICE0 = audio_static("_voice")
VOICE0_PHASE = VOICE0 + 9


def voice0_period():
    return pb.memory[VOICE0 + 2] | (pb.memory[VOICE0 + 3] << 8)


def voice0_target():
    return pb.memory[VOICE0 + 4] | (pb.memory[VOICE0 + 5] << 8)


def phases_seen(frames=90):
    seen = set()
    for _ in range(frames):
        pb.tick(1, False)
        seen.add(pb.memory[VOICE0_PHASE])
    return seen


clear_grid()
pb.memory[ifield(0, F_VOL)] = 15          # instrument 0 back to full volume
pb.memory[ifield(0, F_VIBDEP)] = 12          # vibdep
pb.memory[ifield(0, F_VIBSPD)] = 6           # vibspd
poke(0, 0, "1D8")
poke(2, 1, ":05Cfz")                        # long gate, so the voice stays up
pb.tick(60, False)
check("vibrato advances the voice phase", len(phases_seen()) > 4, True)

pb.memory[ifield(0, F_VIBDEP)] = 0           # vibrato off
pb.tick(90, False)
check("no vibrato leaves the phase alone", len(phases_seen()), 1)

# With no length operand the instrument's own default decides the gate.
clear_grid()
pb.memory[ifield(0, F_LEN)] = 6           # instruments[0].len
poke(0, 0, "1De")                           # bang every 14 ticks
poke(2, 1, ":05Cf")                         # no length operand
on = off = 0
for _ in range(400):
    pb.tick(1, False)
    if pb.memory[0xFF12]:
        on += 1
    else:
        off += 1
check("instrument length gates the note", on > 20 and off > 20, True)

# --- the port masks -------------------------------------------------------
# orca-c paints output ports in reverse video and leaves plain cells alone;
# here an input port that is still empty also gets its own marker, so the
# cells an operator is waiting on are visible before anything is typed in.
check("palette is inverted (darkest is the background)", pb.memory[0xFF47], 0x1B)


def force_full_repaint():
    """Opening and closing the instrument page invalidates the grid."""
    pb.button_press("select"); pb.tick(2, False)
    press(pb, "start")
    pb.button_release("select"); pb.tick(20, False)
    press(pb, "b")
    pb.tick(40, False)


clear_grid()
poke(3, 5, "A")        # a bare add: both inputs and the output still empty
poke(6, 8, "Q")        # query takes three parameters to its west
force_full_repaint()

check("empty west input is marked", is_empty_input_port(pb, 4, 3), True)
check("empty east input is marked", is_empty_input_port(pb, 6, 3), True)
check("the output below is reverse video", is_output_port(pb, 5, 4), True)
check("the operator itself is left plain", glyph(pb.tilemap_background[5, 3]), "A")
check("an unrelated cell stays a dot", pb.tilemap_background[20, 3] & 0xFF,
      ord(".") - 32)
check("all three query parameters are marked",
      [is_empty_input_port(pb, x, 6) for x in (5, 6, 7)], [True, True, True])

# Filling a port must clear its marker but keep the cell readable.
poke(3, 4, "3")
force_full_repaint()
check("a filled input drops the marker", glyph(pb.tilemap_background[4, 3]), "3")
check("...and is no longer a port dot", is_empty_input_port(pb, 4, 3), False)

# The ports have to be visible with the sequencer stopped -- that is when you
# are typing, and they only exist as a side effect of running the operators.
if win_row(pb, 0)[18] != "|":
    press(pb, "start")
pb.tick(10, False)
clear_grid()
poke(8, 4, "M")
press(pb, "b")         # any edit re-derives the marks
pb.tick(40, False)
check("sequencer is stopped for the port check", win_row(pb, 0)[18], "|")
check("ports show while stopped", is_empty_input_port(pb, 3, 8), True)
check("output shows while stopped", is_output_port(pb, 4, 9), True)
press(pb, "start")
pb.tick(10, False)

# --- panning --------------------------------------------------------------
# NR51 is one register for the whole APU: high nibble left, low nibble right,
# one bit per channel.  Instrument 0 is on channel 0, so its two bits are
# 0x10 (left) and 0x01 (right).  Pan lives on the instrument; the fifth
# operand is note length again.
def pan_bits(instrument_pan):
    clear_grid()
    pb.memory[ifield(0, F_PAN)] = instrument_pan
    pb.memory[ifield(0, F_LEN)] = 8
    poke(0, 0, "1D1")
    poke(2, 1, ":05Cf0")
    pb.tick(80, False)
    return pb.memory[0xFF25] & 0x11


check("pan 0 feeds both outputs", pan_bits(0), 0x11)
check("pan 1 is left only", pan_bits(1), 0x10)
check("pan 2 is right only", pan_bits(2), 0x01)
check("pan 3 silences both", pan_bits(3), 0x00)

# A muted note still wins its channel and still runs -- it is a note you
# cannot hear, not a note that never happened.
check("a muted note still drives the channel", pb.memory[0xFF12] != 0, True)
pb.memory[ifield(0, F_PAN)] = 0

# --- the fifth operand is note length again -------------------------------
def frames_sounding(operand, frames=300):
    clear_grid()
    pb.memory[ifield(0, F_LEN)] = 20     # the instrument's own, deliberately long
    poke(0, 0, "1De")                    # a bang every 14 ticks
    poke(2, 1, ":05Cf" + operand)
    pb.tick(40, False)
    return sum(1 for _ in range(frames)
               if (pb.tick(1, False) or True) and pb.memory[0xFF12])


short = frames_sounding("2")             # two ticks
defaulted = frames_sounding("")          # no operand: the instrument decides
check("the length operand shortens the note", short < defaulted, True)
check("...and an absent one defers to the instrument", defaulted > 200, True)

# --- the pitch slide ------------------------------------------------------
# Software, so unlike the DMG's hardware sweep it works on every tonal
# channel.  It starts the note away from its pitch and lands it back on it.
def slide_periods(pitch, spd=8, frames=40):
    clear_grid()
    pb.memory[ifield(0, F_PITCH)] = pitch & 0xFF
    pb.memory[ifield(0, F_PITCHSPD)] = spd
    pb.memory[ifield(0, F_LEN)] = 20
    poke(0, 0, "1Dk")
    poke(2, 1, ":05Cf0")
    for _ in range(600):
        pb.tick(1, False)
        if pb.memory[0xFF12] == 0:
            break
    for _ in range(600):
        pb.tick(1, False)
        if pb.memory[0xFF12] != 0:
            break
    return [voice0_period() for _ in range(frames) if (pb.tick(1, False) or True)]


static = slide_periods(0)
check("no slide means a static period", len(set(static)), 1)
rest = static[-1]

falling = slide_periods(-12)             # starts an octave up and drops
check("a falling slide starts above the note", falling[0] > rest, True)
check("...arrives exactly on the note", falling[-1], rest)
check("...and then stays there", len(set(falling[-10:])), 1)
check("...moving only one way", falling == sorted(falling, reverse=True), True)

rising = slide_periods(12)
check("a rising slide starts below the note", rising[0] < rest, True)
check("...and arrives on it too", rising[-1], rest)

# --- the hardware sweep is back, on the one channel that has one ----------
# Channel 0's sweep unit keeps going for as long as the note lasts, where the
# software slide lands and stops.  Both write the frequency registers, so when
# the sweep is on the slide has to stand aside rather than fight it.
clear_grid()
pb.memory[ifield(0, F_SWEEP)] = (-3) & 0xFF
pb.memory[ifield(0, F_SWTIME)] = 4
pb.memory[ifield(0, F_PITCH)] = (-12) & 0xFF   # deliberately also set
pb.memory[ifield(0, F_LEN)] = 8
poke(0, 0, "1D1")
poke(2, 1, ":05Cf0")
pb.tick(80, False)
check("sweep programs NR10", pb.memory[0xFF10] & 0x7F, (4 << 4) | 0x08 | 3)
check("the software slide stands aside", pb.memory[VOICE0 + 8], 0)

pb.memory[ifield(0, F_SWEEP)] = 0              # ...and gives way when off
pb.memory[ifield(0, F_LEN)] = 20
clear_grid()
poke(0, 0, "1D1")
poke(2, 1, ":05Cf0")
pb.tick(80, False)
check("no sweep leaves NR10 clear", pb.memory[0xFF10] & 0x7F, 0)
pb.memory[ifield(0, F_PITCH)] = 0

fast = slide_periods(-12, spd=15)
slow = slide_periods(-12, spd=4)


def settle_frame(seq, target):
    for i, v in enumerate(seq):
        if v == target:
            return i
    return len(seq)


check("a higher speed lands sooner",
      settle_frame(fast, rest) < settle_frame(slow, rest), True)

# --- TEST has to end on its own# --- TEST has to end on its own, with the sequencer stopped ---------------
# The audition used to be gated in ticks, so with playback paused it never
# expired.  The wave channel made it obvious: it is the one voice with no
# envelope to fade it out by itself.
def cursor_xy():
    row = win_row(pb, 0)
    return int(row[14], 36), int(row[15], 36)


def open_slot(slot):
    """open_instr_page() jumps to the slot named by the glyph under the
    cursor, so writing that glyph is faster than walking the slot row."""
    x, y = cursor_xy()
    pb.memory[GRID + y * 32 + x] = ord("0123456789abcdefghijklmnopqrstuvwxyz"[slot])
    pb.button_press("select")
    pb.tick(2, False)
    press(pb, "start")
    pb.button_release("select")
    pb.tick(10, False)


def wave_dac_on():
    return (pb.memory[0xFF1A] & 0x80) != 0


def audition_frames(limit=400):
    press(pb, "a")
    n = 0
    while n < limit and wave_dac_on():
        pb.tick(1, False)
        n += 1
    return n


clear_grid()                        # nothing left to retrigger the channel
if win_row(pb, 0)[18] != "|":
    press(pb, "start")              # stop the sequencer
pb.tick(10, False)
check("sequencer is stopped", win_row(pb, 0)[18], "|")

pb.memory[ifield(2, F_LEN)] = 2     # instrument 2 is the wave voice
open_slot(2)
check("page opened on the wave instrument", bg_row(pb, 16, 0, 13), " INSTRUMENT 2")
short = audition_frames()
check("test note ends while paused", short < 400, True)

pb.memory[ifield(2, F_LEN)] = 24
pb.tick(20, False)
long_ = audition_frames()
check("test note follows the instrument length", long_ > short + 8, True)

# --- noise shape ----------------------------------------------------------
# NR43 keeps the clock shift in its high nibble; a larger shift is a lower
# pitch.  The note decides where the sound comes to rest, and SHAPE only says
# how far from that it starts -- so the sweep has to converge and stop rather
# than run to the rail.
def noise_shifts(shape, frames=30, note="5C"):
    """Samples one note from its trigger onward, waiting for the trigger
    rather than assuming where the delay's phase happens to be."""
    clear_grid()
    pb.memory[ifield(3, F_SHAPE)] = shape & 0xFF
    pb.memory[ifield(3, F_LEN)] = 10   # shorter than the bang period, so the
    poke(0, 0, "1Dk")                  # channel really does fall silent
    poke(2, 1, ":3" + note + "f0")
    if win_row(pb, 0)[18] != ">":
        press(pb, "start")
    for _ in range(600):
        pb.tick(1, False)
        if pb.memory[0xFF21] == 0:
            break
    for _ in range(600):
        pb.tick(1, False)
        if pb.memory[0xFF21] != 0:
            break
    out = []
    for _ in range(frames):
        pb.tick(1, False)
        out.append(pb.memory[0xFF22] >> 4)
    return out


steady = noise_shifts(0)
rest = steady[-1]
check("shape 0 is static from the first frame", len(set(steady)), 1)

falling = noise_shifts(-3)
check("a falling shape starts below the note's shift", falling[0] < rest, True)
check("...climbs no further than the note", max(falling) <= rest, True)
check("...settles on the note", falling[-1], rest)
check("...and then holds it", len(set(falling[-12:])), 1)

rising = noise_shifts(3)
check("a rising shape starts above the note's shift", rising[0] > rest, True)
check("...and settles on the note too", rising[-1], rest)
check("...holding it as well", len(set(rising[-12:])), 1)

# The point of all this: the note, not the sweep, chooses the timbre.
high = noise_shifts(0, note="7C")
check("a different note rests somewhere else", high[-1] != rest, True)
check("the offset does not change where it rests", noise_shifts(-3, note="7C")[-1],
      high[-1])

# --- selection and the clipboard ------------------------------------------
# B is a modifier while it is held: B+direction drags a rectangle, B+A cuts.
# On its own it is still the plain erase tap.
if win_row(pb, 0)[18] != "|":
    press(pb, "start")               # stop, so the grid holds still
pb.tick(10, False)


def cursor_home():
    pb.button_press("left"); pb.tick(200, False); pb.button_release("left")
    pb.tick(6, False)
    pb.button_press("up"); pb.tick(140, False); pb.button_release("up")
    pb.tick(6, False)


def cursor_to(x, y):
    cursor_home()
    for _ in range(x):
        press(pb, "right", 3, 3)
    for _ in range(y):
        press(pb, "down", 3, 3)
    pb.tick(8, False)


clear_grid()
poke(4, 3, "AB")
poke(5, 3, "CD")
force_full_repaint()
cursor_to(3, 4)
check("cursor parked on the block", win_row(pb, 0)[13:16], "@34")

pb.button_press("b")
pb.tick(4, False)
press(pb, "right")
press(pb, "down")
pb.tick(12, False)
check("B and a direction drag a selection", win_row(pb, 1)[:7], "SEL 2x2")
check("the selection is drawn inverted", is_output_port(pb, 3, 4), True)
pb.button_release("b")
pb.tick(12, False)
check("releasing B after a drag does not erase", glyph(pb.tilemap_background[3, 4]), "A")

pb.button_press("select"); pb.tick(3, False)
press(pb, "b")                       # copy
pb.button_release("select"); pb.tick(20, False)

cursor_to(10, 8)                     # moving without B drops the selection
pb.button_press("select"); pb.tick(3, False)
press(pb, "a")                       # paste
pb.button_release("select"); pb.tick(40, False)
check("paste reproduces the block",
      [bg_row(pb, 8, 10, 2), bg_row(pb, 9, 10, 2)], ["AB", "CD"])

cursor_to(3, 4)
pb.button_press("b"); pb.tick(4, False)
press(pb, "right")
press(pb, "down")
press(pb, "a")                       # cut the selection
pb.button_release("b"); pb.tick(40, False)
check("cut clears the source",
      [bg_row(pb, 4, 3, 2), bg_row(pb, 5, 3, 2)], ["..", ".."])

cursor_to(14, 12)
pb.button_press("select"); pb.tick(3, False)
press(pb, "a")
pb.button_release("select"); pb.tick(40, False)
check("cut kept what it removed",
      [bg_row(pb, 12, 14, 2), bg_row(pb, 13, 14, 2)], ["AB", "CD"])

cursor_to(14, 12)
press(pb, "b")                       # a bare tap is still the eraser
pb.tick(30, False)
check("a bare B tap still erases one cell", glyph(pb.tilemap_background[14, 12]), ".")

pb.button_press("b"); pb.tick(4, False)
press(pb, "right")
pb.button_release("b"); pb.tick(12, False)
check("the selection is showing", win_row(pb, 1)[:3], "SEL")
press(pb, "left")
pb.tick(20, False)
check("moving without B drops the selection", win_row(pb, 1)[:3] != "SEL", True)

# --- a modifier has to survive a slow frame -------------------------------
# A heavy pattern at the top of the tempo range stretches one loop iteration
# across several frames.  The edge latch alone was not enough for that: SELECT
# could be back up before the main loop looked, so SELECT+START read as a bare
# START (transport toggled instead of opening the page) and, worse, SELECT+B
# read as a bare B and erased the cell instead of copying it.
clear_grid()
for y in range(0, 16, 2):
    poke(y, 0, "XE")           # a fresh mover injected into eight rows a tick
poke(15, 20, "Z")              # a witness glyph, away from the traffic
for _ in range(60):            # give the movers time to fill the grid
    pb.tick(1, False)
check("tempo is at the top for the load test", win_row(pb, 0)[:6], "BPM250")

transport = win_row(pb, 0)[18]
pb.button_press("select")
pb.tick(2, False)
press(pb, "start")
pb.button_release("select")
pb.tick(30, False)
check("SELECT+START opens the page under load", pb.memory[0xFF42], 128)
check("...and does not toggle the transport", win_row(pb, 0)[18], transport)
press(pb, "b")                 # back to the grid
pb.tick(30, False)

# Park the cursor on the witness and copy it; a misread would erase it.
while cursor_xy() != (20, 15):
    x, y = cursor_xy()
    if y < 15:
        press(pb, "down", 2, 2)
    elif x < 20:
        press(pb, "right", 2, 2)
    else:
        break
pb.tick(20, False)
check("cursor reached the witness", bg(pb, 20, 15), "Z")
pb.button_press("select")
pb.tick(2, False)
press(pb, "b")
pb.button_release("select")
pb.tick(30, False)
check("SELECT+B copies rather than erasing under load", bg(pb, 20, 15), "Z")

pb.screen.image.resize((480, 432), 0).save("build/screen.png")
pb.stop()

print("\n%d failures" % len(fails))
sys.exit(1 if fails else 0)
