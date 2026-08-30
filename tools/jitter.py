"""Does pressing buttons disturb the clock?  Measures the gap between ticks."""
import re
from pyboy import PyBoy
rom={}
for a,s in re.findall(r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()):
    rom.setdefault(s,int(a,16))
pb=PyBoy("build/orca-dmg.gb", window="null", sound_emulated=True, sound_volume=0)
pb.set_emulation_speed(0)
TICK=rom["_og_tick"]; GRID=rom["_og_grid"]
def tickno(): return pb.memory[TICK]|(pb.memory[TICK+1]<<8)
def load(name):
    text=[l.rstrip("\n") for l in open("patches/%s.orca"%name) if l.strip("\n")]
    for i in range(512): pb.memory[GRID+i]=ord(".")
    for y,row in enumerate(text):
        for x,ch in enumerate(row): pb.memory[GRID+y*32+x]=ord(ch)
def gaps(frames, hold=None):
    last=tickno(); at=pb.frame_count; out=[]
    if hold: pb.button_press(hold)
    for _ in range(frames):
        pb.tick(1,False)
        t=tickno()
        if t!=last:
            out.append(pb.frame_count-at); at=pb.frame_count; last=t
    if hold: pb.button_release(hold); pb.tick(10,False)
    return out
pb.tick(200,False); load("euclid"); pb.tick(120,False)
for label,hold in (("покой", None), ("зажат вправо", "right"), ("зажата A (палитра)", "a")):
    g=gaps(600, hold)
    if g:
        print("  %-20s тиков=%-3d  интервал: мин %d, макс %d, разброс %d кадра"
              % (label, len(g), min(g), max(g), max(g)-min(g)))
    pb.tick(60,False)
pb.stop()
