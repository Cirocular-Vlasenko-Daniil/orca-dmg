"""Renders the README's reference screenshots straight out of the emulator."""
import os
os.makedirs("docs", exist_ok=True)
from pyboy import PyBoy

pb = PyBoy("build/orca-dmg.gb", window="null", sound_emulated=False)
pb.set_emulation_speed(0)
pb.tick(160, False)
pb.tick(2, True)   # render, or screen.image stays blank
pb.screen.image.resize((480, 432), 0).save("docs/shot-grid.png")

def press(btn, hold=4, after=4):
    pb.button_press(btn); pb.tick(hold, False)
    pb.button_release(btn); pb.tick(after, False)

for _ in range(14):
    press("down")
for _ in range(6):
    press("right")
press("a")
pb.tick(10, False)
pb.tick(2, True)
pb.screen.image.resize((480, 432), 0).save("docs/shot-picker.png")
# Freshly placed operators, so the empty-port markers are visible next to a
# pattern that is actually running.
import re
GRID = dict((s_, int(a, 16)) for a, s_ in re.findall(
    r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()))["_og_grid"]
for y, x, ch in ((12, 3, "A"), (12, 9, ":"), (14, 5, "Q")):
    pb.memory[GRID + y * 32 + x] = ord(ch)
# opening and closing the instrument page forces a full repaint
pb.button_press("select"); pb.tick(2, False)
press("start")
pb.button_release("select"); pb.tick(20, False)
press("b")
pb.tick(60, False)
pb.tick(2, True)
pb.screen.image.resize((480, 432), 0).save("docs/shot-ports.png")

# a selection dragged with B held
pb.button_press("b"); pb.tick(4, False)
for _ in range(3):
    press("right")
press("down")
pb.tick(14, False)
pb.tick(2, True)
pb.screen.image.resize((480, 432), 0).save("docs/shot-select.png")
pb.button_release("b"); pb.tick(10, False)
press("left")            # drop the selection again
pb.tick(10, False)

# instrument page: SELECT+START, then step onto a parameter row
press("b")               # close the palette first -- it swallows START
pb.tick(6, False)
pb.button_press("select"); pb.tick(2, False)
press("start")
pb.button_release("select"); pb.tick(8, False)
press("left")            # slot 1 -> slot 0
# a kick setting, so the page shows the pitch slide doing something
INSTR = dict((s_, int(a, 16)) for a, s_ in re.findall(
    r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()))["_instrume"]
pb.memory[INSTR + 6] = (-14) & 0xFF      # instruments[0].pitch
pb.memory[INSTR + 3] = (-4) & 0xFF       # ...and a decay to go with it
for _ in range(6):
    press("down")        # rest on the PITCH row
pb.tick(20, False)
pb.tick(2, True)
pb.screen.image.resize((480, 432), 0).save("docs/shot-instr.png")

# the noise instrument: a different channel means a different row list
for _ in range(3):
    press("up")          # back to the slot row
for _ in range(3):
    press("right")       # slot 0 -> 3
for _ in range(6):
    press("down")        # down to SHAPE
for _ in range(3):
    press("left")        # -3: the falling sweep that makes a kick
pb.tick(20, False)
pb.tick(2, True)
pb.screen.image.resize((480, 432), 0).save("docs/shot-noise.png")

lcdc = pb.memory[0xFF40]
print("LCDC=%02X tiledata=%s" % (lcdc, "8000" if (lcdc >> 4) & 1 else "8800"))
print("tile 3 @0x8000:", [pb.memory[0x8000 + 3*16 + i] for i in range(4)])
print("tile 3 @0x9000:", [pb.memory[0x9000 + 3*16 + i] for i in range(4)])
print("BGP=%02X" % pb.memory[0xFF47])
pb.stop()
print("wrote docs/shot-grid.png docs/shot-picker.png docs/shot-instr.png docs/shot-noise.png docs/shot-ports.png docs/shot-select.png")
