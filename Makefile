GBDK   ?= $(HOME)/toolchains/gbdk
LCC     = $(GBDK)/bin/lcc
NAME    = orca-dmg

# Windows has no unix shell: make hands recipes to cmd.exe, which has no rm
# and whose mkdir reads "-p" as a directory name to create.  The ROM build
# needs exactly two shell commands, so they are switched here rather than
# making everyone install a unix environment.  Override PY/PYGEN/HOSTCC if
# your interpreters or compiler live somewhere else.
ifeq ($(OS),Windows_NT)
PY     ?= .venv/Scripts/python.exe
PYGEN  ?= python
RMBUILD = if exist build rmdir /S /Q build
EXE     = .exe
else
PY     ?= .venv/bin/python
PYGEN  ?= python3
RMBUILD = rm -rf build
EXE     =
endif
HOSTCC ?= cc

SRC  = src/main.c src/orca.c src/audio.c src/instr.c src/edit.c src/text.c src/font.c src/notes.c
HDRS = src/orca.h src/audio.h src/instr.h src/edit.h src/text.h src/vram.h src/demo.h
OBJ  = $(SRC:src/%.c=build/%.o)

# 0x1B = MBC5 + RAM + battery, so patches survive a power cycle.
# No space in the cartridge title, and so no quotes: sh and cmd.exe disagree
# about quoting, and there is no form that survives both -- under cmd.exe
# -Wm-yn"ORCA DMG" split in two and the linker went looking for a file
# called DMG.
CART = -Wm-yt0x1B -Wm-ya1 -Wm-yo4 -Wm-ynORCA-DMG

all: build/$(NAME).gb

# An order-only prerequisite (after the |): the directory is created once, when
# it is missing, and never counts as out of date afterwards.  Plain mkdir, so
# cmd.exe understands it too.
build:
	mkdir build

build/$(NAME).gb: $(OBJ) | build
	$(LCC) $(CART) -Wl-m -o $@ $(OBJ)
	@$(GBDK)/bin/romusage $(@:.gb=.map) -g

build/%.o: src/%.c $(HDRS) | build
	$(LCC) -c -o $@ $<

# Two layers: the VM core is plain C and is proven on the host, where a
# failure is readable; the ROM itself is then driven in an emulator and
# checked against what the PPU and APU actually hold.
# No leading ./ on the host test: a path with a slash runs fine under sh, and
# cmd.exe does not understand ./ at all.
test: build/host_test$(EXE) build/$(NAME).gb
	build/host_test$(EXE)
	$(PY) tools/emu_test.py
	$(PY) tools/vblank_test.py
	$(PY) tools/header_test.py

build/host_test$(EXE): test/host_test.c src/orca.c $(HDRS) | build
	$(HOSTCC) -O1 -Wall -Wextra -std=c99 -o $@ test/host_test.c src/orca.c

shots: build/$(NAME).gb
	$(PY) tools/shots.py

# Runs every pattern in patches/ inside the ROM and reports what it costs:
# whether the sequencer keeps its clock, and where the time goes.
loadtest: build/$(NAME).gb
	$(PYGEN) tools/mkpatches.py
	$(PY) tools/loadtest.py

# Times orca_run() on real hardware timing: with an empty grid (pure scan
# cost) and with the boot pattern (scan plus operators).
bench: build/orca.o
	$(LCC) -c -o build/bench.o tools/bench.c
	$(LCC) -Wl-m -o build/bench.gb build/bench.o build/orca.o
	$(PY) tools/bench_run.py build/bench.gb 20000

# Regenerates the font tiles and the Game Boy pitch tables.
gen:
	$(PYGEN) tools/makefont.py src/font.c
	$(PYGEN) tools/makenotes.py src/notes.c

clean:
	$(RMBUILD)

.PHONY: all test shots bench loadtest gen clean
