import re, sys
from pyboy import PyBoy

def syms():
    off = {}
    for line in open("build/main.sym"):
        m = re.match(r"\s*(\d)\s+(_\w+)\s+([0-9A-F]{8})", line)
        if m and m.group(1) == "0":
            off[m.group(2)] = int(m.group(3), 16)
    rom = dict((s, int(a, 16)) for a, s in
               re.findall(r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()))
    base = rom["_vq_push"] - off["_vq_push"]
    return dict((k, base + v) for k, v in off.items()), rom

M, ROM = syms()
pb = PyBoy("build/orca-dmg.gb", window="null", sound_emulated=True, sound_volume=0)
pb.set_emulation_speed(0)

spans = {}
open_at = {}

pairs = {}

def mark(name, addr, bank=0):
    pairs.setdefault(addr, []).append(("open", name))

def close(name, addr, bank=0):
    pairs.setdefault(addr, []).append(("close", name))

def install():
    for addr, acts in pairs.items():
        def cb(_c, acts=acts):
            now = (pb.frame_count, pb.memory[0xFF44])
            for kind, n in acts:
                if kind == "close" and n in open_at:
                    f0, ly0 = open_at.pop(n)
                    spans.setdefault(n, []).append((now[0]-f0)*154 + (now[1]-ly0))
            for kind, n in acts:
                if kind == "open":
                    open_at[n] = now
        pb.hook_register(0, addr, cb, None)

# each pair: start of the routine, and the routine called right after it
mark("orca_run", ROM["_orca_run"]);          close("orca_run", ROM["_snd_disp"])
mark("paint_grid", M["_paint_grid"]);        close("paint_grid", M["_update_view"])
# whole iteration: from the drain at the top of the loop to the next one
mark("iteration", M["_vq_flush"]);           close("iteration", M["_vq_flush"])

install()
pb.tick(600, False)
for k, v in spans.items():
    v = [x for x in v if 0 <= x < 2000]
    if v:
        v.sort()
        print("%-12s n=%-4d median %5.1f lines (%.2f frames)   max %d lines (%.2f frames)"
              % (k, len(v), v[len(v)//2], v[len(v)//2]/154.0, v[-1], v[-1]/154.0))
pb.stop()
