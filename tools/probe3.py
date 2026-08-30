import re, sys
from pyboy import PyBoy
def syms():
    off={}
    for line in open("build/main.sym"):
        m=re.match(r"\s*(\d)\s+(_\w+)\s+([0-9A-F]{8})", line)
        if m and m.group(1)=="0": off[m.group(2)]=int(m.group(3),16)
    rom={}
    for a,s in re.findall(r"([0-9A-F]{8})\s+(_\w+)", open("build/orca-dmg.map").read()):
        rom.setdefault(s,int(a,16))
    return dict((k,rom["_vq_push"]-off["_vq_push"]+v) for k,v in off.items()), rom
M,R=syms()
pb=PyBoy("build/orca-dmg.gb", window="null", sound_emulated=True, sound_volume=0)
pb.set_emulation_speed(0)
GRID=R["_og_grid"]
n={"paint":0,"vm":0}
pb.hook_register(0, M["_paint_grid"], lambda c: n.__setitem__("paint", n["paint"]+1), None)
pb.hook_register(0, R["_orca_run"],   lambda c: n.__setitem__("vm", n["vm"]+1), None)
def press(b,h=4,a=6):
    pb.button_press(b); pb.tick(h,False); pb.button_release(b); pb.tick(a,False)
# heavy pattern so the difference is visible
pb.tick(200,False)
for i in range(512): pb.memory[GRID+i]=ord(".")
for y in range(0,16,2):
    for i,ch in enumerate("XE"): pb.memory[GRID+y*32+i]=ord(ch)
pb.tick(120,False)

def sample(label, frames=300):
    n["paint"]=0; n["vm"]=0
    f0=pb.frame_count
    pb.tick(frames,False)
    print("  %-22s paint_grid=%-4d orca_run=%-4d  SCY=%d" % (label, n["paint"], n["vm"], pb.memory[0xFF42]))

sample("на сетке")
pb.button_press("select"); pb.tick(2,False); press("start"); pb.button_release("select"); pb.tick(30,False)
sample("на стр. инструментов")
press("b"); pb.tick(30,False)
sample("вернулись на сетку")
pb.stop()
