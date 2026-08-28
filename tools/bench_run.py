import sys
from pyboy import PyBoy
rom = sys.argv[1]
budget = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
pb = PyBoy(rom, window="null", sound_emulated=False)
pb.set_emulation_speed(0)
MARK = 0xC0B1
prev, seen = 0, {}
for f in range(budget):
    pb.tick(1, False)
    m = pb.memory[MARK]
    if m != prev:
        seen[m] = f; prev = m
        if m == 5: break
name = rom.split('/')[-1]
if {3, 4, 5} <= set(seen):
    print("%-14s demo grid %5.2f fr/tick   empty grid %5.2f fr/tick"
          % (name, (seen[4]-seen[3])/60.0, (seen[5]-seen[4])/60.0))
else:
    print("%-14s INCOMPLETE after %d frames, markers: %s" % (name, budget, seen))
pb.stop()
