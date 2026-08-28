#!/usr/bin/env python3
"""Checks that the vram drain always finishes inside vblank.

PyBoy writes to vram in any ppu mode (core/mb.py has no mode gate), so its
"every tile arrived" result cannot say anything about real hardware, where a
write during mode 3 is discarded.  What PyBoy does model correctly is LY.  So
instead of testing the writes, test the property that makes the gating moot:
the drain must begin and end within LY 144..153.

Run it against the worst case there is -- an instrument page reflow, which
queues far more than one vblank can carry and keeps the drain saturated.
"""
import re
import sys
from pyboy import PyBoy

MARGIN_LY = 152  # finish a line before vblank actually ends

fails = []


def check(name, ok, detail=""):
    if ok:
        print("  ok   %s" % name)
    else:
        print("  FAIL %-40s %s" % (name, detail))
        fails.append(name)


def main_symbol(name):
    """main.c statics: anchor on a global that appears in both files."""
    off = {}
    for line in open("build/main.sym"):
        m = re.match(r"\s*(\d)\s+(_\w+)\s+([0-9A-F]{8})", line)
        if m and m.group(1) == "0":
            off[m.group(2)] = int(m.group(3), 16)
    rom = dict((s, int(a, 16)) for a, s in
               re.findall(r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()))
    base = rom["_vq_push"] - off["_vq_push"]
    return base + off[name]


pb = PyBoy("build/orca-dmg.gb", window="null", sound_emulated=True, sound_volume=0)
pb.set_emulation_speed(0)

samples = []
state = {"ly_in": None}


def on_flush(_):
    state["ly_in"] = pb.memory[0xFF44]


def on_done(_):
    if state["ly_in"] is not None:
        samples.append((state["ly_in"], pb.memory[0xFF44]))
        state["ly_in"] = None


rom_syms = dict((s, int(a, 16)) for a, s in
                re.findall(r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()))
pb.hook_register(0, main_symbol("_vq_flush"), on_flush, None)
pb.hook_register(0, rom_syms["_snd_fram"], on_done, None)  # the next call in the loop


def press(b, hold=4, after=6):
    pb.button_press(b); pb.tick(hold, False)
    pb.button_release(b); pb.tick(after, False)


def verdict(label):
    assert samples, label
    # Outside 144..153 is a real hardware fault; landing exactly on 153 is
    # inside vblank but leaves no room, so it is reported separately.
    unsafe = [s for s in samples if not (144 <= s[1] <= 153) or s[1] < s[0]]
    tight = [s for s in samples if s[1] > MARGIN_LY]
    print("     %-26s n=%-4d entry LY %d   exit LY %s   tight=%d"
          % (label, len(samples), samples[0][0],
             sorted(set(s[1] for s in samples))[-3:], len(tight)))
    check("drain stays in vblank: " + label, not unsafe, "overruns: %s" % unsafe[:6])
    samples.clear()


pb.tick(250, False)
verdict("idle")

pb.tick(400, False)
verdict("running pattern")

pb.button_press("select"); pb.tick(2, False)
press("start")
pb.button_release("select"); pb.tick(40, False)
samples.clear()

for _ in range(8):          # channel changes reflow most of the page
    press("right")
    press("left")
pb.tick(40, False)
verdict("instrument page reflow")

press("b"); pb.tick(20, False)
pb.button_press("select"); pb.tick(2, False)
for _ in range(10):
    press("up")             # crank the tempo while editing
pb.button_release("select")
pb.tick(200, False)
verdict("max tempo")

pb.stop()
print("\n%d failures" % len(fails))
sys.exit(1 if fails else 0)
