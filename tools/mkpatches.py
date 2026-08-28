#!/usr/bin/env python3
"""Builds the test patches from explicit coordinates.

Writing ORCA by counting dots in a string literal is how you end up with an
operator whose ports land one cell off and a pattern that quietly does
nothing.  Everything here is placed by (row, col) and the result is checked
for size; tools/loadtest.py then proves each one actually runs.
"""
import os

W, H = 32, 16


def blank():
    return [["."] * W for _ in range(H)]


def put(g, y, x, s):
    for i, ch in enumerate(s):
        assert 0 <= y < H and 0 <= x + i < W, (y, x + i, s)
        g[y][x + i] = ch


def render(g):
    return "\n".join("".join(row) for row in g) + "\n"


P = {}

# --- euclid: four euclidean voices, the classic ORCA drum machine ---------
g = blank()
for x, steps, mx, note in ((1, "3", "8", ":33Cf1"),   # noise, sparse
                           (9, "5", "8", ":04Df1"),   # pulse A
                           (17, "2", "8", ":25Cf2"),  # wave
                           (25, "7", "g", ":16Ea2")):  # pulse B, dense
    put(g, 0, x - 1, steps + "U" + mx)   # U bangs into (1, x)
    put(g, 2, x, note)                   # ':' hangs under the bang
put(g, 7, 0, "#.EUCLID.#")
P["euclid"] = g

# --- arp: clock -> track -> note, twice, over a slow bass -----------------
g = blank()
for base, mod, ln, notes, head, tail in (
        (0, "2", "4", "CEGA", ":15", "f2"),
        (16, "3", "6", "CEGAce", ":26", "82")):
    put(g, 2, base + 1, "1C" + ("4" if base == 0 else "6"))  # clock -> (3,base+2)
    put(g, 3, base, "D" + mod)                # bang -> (4, base) beside the ':'
    put(g, 3, base + 3, ln + "T" + notes)     # track -> (4, base+4) = the note
    put(g, 4, base + 1, head)                 # ':' inst + octave
    put(g, 4, base + 5, tail)                 # velocity + length
put(g, 7, 0, "2D8")
put(g, 9, 1, ":03Ca4")
put(g, 11, 0, "#.ARPEGGIOS.#")
P["arp"] = g

# --- movers: a new mover injected every tick, eight rows of them ----------
# X writes 'E' back into column 0 each tick, so the rows keep filling with
# movers that march right and explode into bangs at the wall.  This is the
# heaviest thing the grid can be made to do.
g = blank()
for y in range(0, 16, 2):
    put(g, y, 0, "XE")
P["movers"] = g

# --- churn: sixty-four values that change every single tick ---------------
g = blank()
for y in range(0, 16, 2):
    op = "R" if (y // 2) % 2 == 0 else "I"
    put(g, y, 0, ("1" + op + "z.") * 8)
P["churn"] = g

# --- bulk: five generators, each writing a whole row every tick -----------
g = blank()
vals = "abcdefghijklmnopqrstuvwxyz0123456789"
for y in (0, 3, 6, 9, 12):
    put(g, y, 0, "00zG" + vals[:W - 4])
P["bulk"] = g

os.makedirs("patches", exist_ok=True)
for name, grid in P.items():
    text = render(grid)
    rows = text.rstrip("\n").split("\n")
    assert len(rows) == H, (name, len(rows))
    for r in rows:
        assert len(r) == W, (name, len(r), r)
    open("patches/%s.orca" % name, "w").write(text)
    print("patches/%s.orca  %dx%d ok" % (name, W, H))
