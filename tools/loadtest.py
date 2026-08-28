#!/usr/bin/env python3
"""Runs each patch in patches/ inside the real ROM and reports what it costs.

Three things matter and they fail differently:
  * does the sequencer still keep its clock, or is it falling behind
  * what the VM and the repaint each cost, in scanlines
  * whether the vram drain still finishes inside vblank -- the one property
    that decides whether this works on hardware at all
"""
import glob
import os
import re
import sys
from pyboy import PyBoy

ROM = "build/orca-dmg.gb"


def symbols():
    off = {}
    for line in open("build/main.sym"):
        m = re.match(r"\s*(\d)\s+(_\w+)\s+([0-9A-F]{8})", line)
        if m and m.group(1) == "0":
            off[m.group(2)] = int(m.group(3), 16)
    # sdld truncates names to nine characters, so orca_run and orca_run_marks
    # collide -- keep the first, which is the one defined first.
    rom = {}
    for a, sym in re.findall(r"([0-9A-F]{8})\s+(_\w+)",
                             open("build/orca-dmg.map").read()):
        rom.setdefault(sym, int(a, 16))
    base = rom["_vq_push"] - off["_vq_push"]
    main = dict((k, base + v) for k, v in off.items())
    return main, rom


MAIN, ROM_SYM = symbols()
GRID = ROM_SYM["_og_grid"]
TICK = ROM_SYM["_og_tick"]


pb = PyBoy(ROM, window="null", sound_emulated=True, sound_volume=0)
pb.set_emulation_speed(0)

spans = {}
open_at = {}
pairs = {}


def at(addr, kind, name):
    pairs.setdefault(addr, []).append((kind, name))


at(ROM_SYM["_orca_run"], "open", "vm")
at(ROM_SYM["_snd_disp"], "close", "vm")
at(MAIN["_paint_grid"], "open", "paint")
at(MAIN["_update_view"], "close", "paint")
at(MAIN["_vq_flush"], "open", "drain")
at(ROM_SYM["_snd_fram"], "close", "drain")

for addr, acts in pairs.items():
    def cb(_c, acts=acts):
        now = (pb.frame_count, pb.memory[0xFF44])
        for kind, n in acts:
            if kind == "close" and n in open_at:
                f0, ly0 = open_at.pop(n)
                spans.setdefault(n, []).append(
                    ((now[0] - f0) * 154 + (now[1] - ly0), ly0, now[1]))
        for kind, n in acts:
            if kind == "open":
                open_at[n] = now
    pb.hook_register(0, addr, cb, None)


def press(b, hold=4, after=6):
    pb.button_press(b); pb.tick(hold, False)
    pb.button_release(b); pb.tick(after, False)


def read_bpm():
    return int("".join(chr((pb.tilemap_window[3 + i, 0] & 0xFF) + 32) for i in range(3)))


def set_bpm(target):
    """Driven through the buttons rather than by poking the variable: bpm is
    static, and this exercises the path the instrument actually uses."""
    for _ in range(80):
        cur = read_bpm()
        if cur == target:
            return
        pb.button_press("select")
        pb.tick(2, False)
        press("up" if cur < target else "down")
        pb.button_release("select")
        pb.tick(6, False)
    raise AssertionError("could not reach %d bpm (stuck at %d)" % (target, read_bpm()))


def tickno():
    return pb.memory[TICK] | (pb.memory[TICK + 1] << 8)


def load(path):
    text = [l.rstrip("\n") for l in open(path) if l.strip("\n")]
    for i in range(512):
        pb.memory[GRID + i] = ord(".")
    for y, row in enumerate(text):
        for x, ch in enumerate(row):
            pb.memory[GRID + y * 32 + x] = ord(ch)
    # opening and closing the instrument page forces a full repaint
    pb.button_press("select"); pb.tick(2, False)
    press("start")
    pb.button_release("select"); pb.tick(20, False)
    press("b")
    pb.tick(60, False)
    press("start")   # pause: silences whatever the last patch left ringing
    pb.tick(10, False)
    press("start")
    pb.tick(30, False)


def win0():
    return "".join(chr((pb.tilemap_window[i, 0] & 0xFF) + 32) for i in range(20))


def measure(bpm, frames=600):
    set_bpm(bpm)
    assert win0()[18] == ">", "the sequencer stopped: %r" % win0()
    pb.tick(60, False)
    spans.clear()
    open_at.clear()
    voices = set()
    t0, f0 = tickno(), pb.frame_count
    for _ in range(frames):
        pb.tick(1, False)
        # NR30 reads back with its unused bits set, so 0x7F means "off" --
        # only bit 7 says whether the wave DAC is actually on.
        if pb.memory[0xFF12]: voices.add(0)
        if pb.memory[0xFF17]: voices.add(1)
        if pb.memory[0xFF1A] & 0x80: voices.add(2)
        if pb.memory[0xFF21]: voices.add(3)
    ticks = tickno() - t0
    got = (pb.frame_count - f0) / float(ticks) if ticks else 0
    want = 3600.0 / (bpm * 4)
    return got, want, voices


def med(name):
    v = sorted(x[0] for x in spans.get(name, []))
    return v[len(v) // 2] if v else 0


def drain_ok():
    return all(144 <= e <= 153 and e >= s for _, s, e in spans.get("drain", []))


pb.tick(200, False)
print("%-9s %-14s  %-15s %-15s %s" %
      ("patch", "voices", "120 bpm", "250 bpm", "vm / paint (lines)"))
print("-" * 78)

fails = []
for path in sorted(glob.glob("patches/*.orca")):
    name = os.path.basename(path)[:-5]
    load(path)

    got120, want120, voices = measure(120)
    vm, paint = med("vm"), med("paint")
    ok120 = drain_ok()

    got250, want250, _ = measure(250)
    ok250 = drain_ok()

    def fmt(got, want):
        flag = "" if got <= want * 1.10 else "  LATE"
        return "%.2f/%.2f%s" % (got, want, flag)

    print("%-9s %-14s  %-15s %-15s %d / %d%s" %
          (name, ("ch " + "".join(str(v) for v in sorted(voices))) if voices else "silent",
           fmt(got120, want120), fmt(got250, want250), vm, paint,
           "" if (ok120 and ok250) else "   DRAIN OVERRUN"))
    if not (ok120 and ok250):
        fails.append(name + ": drain left vblank")
    if got120 > want120 * 1.10:
        fails.append(name + ": behind the clock at 120 bpm")

pb.stop()
print()
for f in fails:
    print("  !! " + f)
print("%d problems" % len(fails))
