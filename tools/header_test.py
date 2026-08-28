#!/usr/bin/env python3
"""Checks the cartridge header the way a real DMG does.

The DMG boot ROM compares the Nintendo logo at 0x104 byte for byte and
verifies the header checksum at 0x14D; either mismatch locks the machine up on
the boot screen.  PyBoy skips the boot ROM by default, so a broken header is
invisible in every other test here -- and fatal on hardware.  This test runs
the real boot ROM as well, which is the closest thing to plugging it in.
"""
import os
import sys
import pyboy
from pyboy import PyBoy

ROM = "build/orca-dmg.gb"
LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
    0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
    0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E])

fails = []


def check(name, got, want):
    if got == want:
        print("  ok   %s" % name)
    else:
        print("  FAIL %-38s got %r want %r" % (name, got, want))
        fails.append(name)


# --- the link map has to fit the banks it claims -------------------------
# romusage reports free bytes for a bank as a total, so an area that starts
# near the top and runs past 0x4000 shows up as "there is room".  What
# actually happens is that the code carries on into the switchable bank: the
# ROM boots, most of it works, and the startup routine that spilled over --
# the one that copies initialised variables out of ROM -- silently executes
# whatever bank 1 has at that address.
import re as _re

map_text = open("build/orca-dmg.map").read()
for name, start, length in _re.findall(
        r"^(_\w+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})", map_text, _re.M):
    s0, ln = int(start, 16), int(length, 16)
    if ln and s0 < 0x4000:
        check("%s fits inside bank 0" % name, s0 + ln <= 0x4000, True)

rom = open(ROM, "rb").read()

check("nintendo logo intact", rom[0x104:0x134], LOGO)

chk = 0
for b in rom[0x134:0x14D]:
    chk = (chk - b - 1) & 0xFF
check("header checksum", rom[0x14D], chk)

glob = (sum(rom) - rom[0x14E] - rom[0x14F]) & 0xFFFF
check("global checksum", (rom[0x14E] << 8) | rom[0x14F], glob)

check("cartridge type is MBC5+RAM+BATTERY", rom[0x147], 0x1B)
check("declared rom size matches the file", 0x8000 << rom[0x148], len(rom))
check("ram size is one 8K bank", rom[0x149], 0x02)
check("runs on DMG (no CGB-only flag)", rom[0x143] & 0x80, 0)

# Now boot it the way the console does.
bootrom = os.path.join(os.path.dirname(pyboy.__file__), "core", "bootrom_dmg.bin")
if not os.path.exists(bootrom):
    print("  --   no dmg boot rom available, skipping the live boot")
else:
    pb = PyBoy(ROM, window="null", sound_emulated=False, bootrom=bootrom)
    pb.set_emulation_speed(0)
    # A locked-up boot rom never hands over, so "the pattern is on screen" is
    # the honest end-to-end signal -- no need to guess an entry address.
    booted_at = None
    for f in range(900):
        pb.tick(1, False)
        row = "".join(chr((pb.tilemap_background[i, 8] & 0xFF) + 32) for i in range(14))
        if row == "#.A.ORCA.DMG.#":
            booted_at = f
            break
    check("boots and renders through the real DMG boot rom", booted_at is not None, True)
    if booted_at is not None:
        print("     pattern on screen %d frames after power-on" % booted_at)
    pb.stop()

print("\n%d failures" % len(fails))
sys.exit(1 if fails else 0)
