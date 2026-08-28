GBDK   ?= $(HOME)/toolchains/gbdk
LCC     = $(GBDK)/bin/lcc
PY     ?= .venv/bin/python
NAME    = orca-dmg

SRC  = src/main.c src/orca.c src/audio.c src/instr.c src/edit.c src/text.c src/font.c src/notes.c
HDRS = src/orca.h src/audio.h src/instr.h src/edit.h src/text.h src/vram.h src/demo.h
OBJ  = $(SRC:src/%.c=build/%.o)

# 0x1B = MBC5 + RAM + battery, so patches survive a power cycle.
CART = -Wm-yt0x1B -Wm-ya1 -Wm-yo4 -Wm-yn"ORCA DMG"

all: build/$(NAME).gb

build/$(NAME).gb: $(OBJ)
	$(LCC) $(CART) -Wl-m -o $@ $(OBJ)
	@$(GBDK)/bin/romusage $(@:.gb=.map) -g | head -4

build/%.o: src/%.c $(HDRS)
	@mkdir -p build
	$(LCC) -c -o $@ $<

# Two layers: the VM core is plain C and is proven on the host, where a
# failure is readable; the ROM itself is then driven in an emulator and
# checked against what the PPU and APU actually hold.
test: build/host_test build/$(NAME).gb
	./build/host_test
	$(PY) tools/emu_test.py
	$(PY) tools/vblank_test.py
	$(PY) tools/header_test.py

build/host_test: test/host_test.c src/orca.c $(HDRS)
	@mkdir -p build
	cc -O1 -Wall -Wextra -std=c99 -o $@ test/host_test.c src/orca.c

shots: build/$(NAME).gb
	$(PY) tools/shots.py

# Runs every pattern in patches/ inside the ROM and reports what it costs:
# whether the sequencer keeps its clock, and where the time goes.
loadtest: build/$(NAME).gb
	python3 tools/mkpatches.py
	$(PY) tools/loadtest.py

# Times orca_run() on real hardware timing: with an empty grid (pure scan
# cost) and with the boot pattern (scan plus operators).
bench: build/orca.o
	$(LCC) -c -o build/bench.o tools/bench.c
	$(LCC) -Wl-m -o build/bench.gb build/bench.o build/orca.o
	$(PY) tools/bench_run.py build/bench.gb 20000

# Regenerates the font tiles and the Game Boy pitch tables.
gen:
	python3 tools/makefont.py src/font.c
	python3 tools/makenotes.py src/notes.c

clean:
	rm -rf build

.PHONY: all test shots bench loadtest gen clean
