"""Cost of one operator, measured rather than guessed: fill the grid with 64
copies of it and time orca_run() against an empty grid."""
import re
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
spans=[]; st={}
pb.hook_register(0, R["_orca_run"], lambda c: st.__setitem__("t",(pb.frame_count,pb.memory[0xFF44])), None)
def close(c):
    if "t" in st:
        f0,ly0=st.pop("t")
        spans.append((pb.frame_count-f0)*154+(pb.memory[0xFF44]-ly0))
pb.hook_register(0, M["_paint_grid"], close, None)  # bank 0, runs right after the tick

def fill(op):
    for i in range(512): pb.memory[GRID+i]=ord(".")
    if op:
        for y in range(0,16,2):
            for g in range(8):
                for i,ch in enumerate("1"+op+"z."):
                    pb.memory[GRID+y*32+g*4+i]=ord(ch)

def measure(op, frames=400):
    fill(op)
    pb.tick(90,False)
    spans.clear()
    pb.tick(frames,False)
    v=sorted(spans)
    return v[len(v)//2] if v else 0

pb.tick(200,False)
base=measure("")
print("пустая сетка (только обход 512 клеток): %d строк = %.2f кадра" % (base, base/154.0))
print()
print("оператор   строк   кадра   на оператор (тактов)")
for op in "ARIFCUMLB":
    t=measure(op)
    per=(t-base)*114.0/64.0     # 114 машинных тактов в строке
    print("   %s      %5d   %5.2f   %6.0f" % (op, t, t/154.0, per))
pb.stop()
