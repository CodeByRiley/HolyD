# HolyD , standalone build.
#
#   make            build holyd.exe
#   make test       run every script under tests/
#   make gui        build, then open the windowing sample
#   make compile HD=path/to/script.hd   build that script into an .exe
#   make run     HD=path/to/script.hd   build it and run it
#   make difftest   run tests/ on the VM and compiled, and diff them
#   make clean
#
# Windows only for now: the one host implementation here is ffi_win32.c.
# src/ffi_tos.c is the other one , winman over IPC , and is built inside the
# TOS tree rather than here. See README.md.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
BIN     ?= holyd.exe

# Platform adapters under src/platform select HolyD's standalone graphics,
# image, font, and keycode implementations. TOS defines HOLYD_TARGET_TOS and
# selects its own libtos interfaces instead.
INCLUDES := -I src

# What a transpiled program links against: value semantics, the natives, and
# the host. No lexer, parser or VM , generated C is already past those.
RUNTIME_SRCS := \
	src/runtime.c \
	src/eval.c \
	src/ffi.c \
	src/ffi_win32.c \
	src/platform/standalone/gfx.c \
	src/platform/standalone/bmp.c

SRCS := \
	src/main.c \
	src/compiler.c \
	src/emit_c.c \
	src/lexer/lexer.c \
	src/parser/parser.c \
	src/ast/ast.c \
	src/ast/type_syntax.c \
	$(RUNTIME_SRCS)

# gdi32 for the DIB section, user32 for the window, ws2_32 for UdpSocket.
LIBS := -lgdi32 -luser32 -lws2_32

.PHONY: all
all: $(BIN)

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LIBS)

# --test scans the tests/ directory and waits for each script, so everything
# in there has to finish on its own. Scripts that wait on a person or on the
# network live in samples/ and are run by path.
.PHONY: test
test: $(BIN)
	./$(BIN) --test tests

.PHONY: gui
gui: $(BIN)
	./$(BIN) samples/gui.hd

# Compile one script into a standalone executable through the C backend.
#
#   make compile HD=samples/gui.hd              -> build/gui.exe
#   make compile HD=samples/gui.hd OUT=g.exe    -> g.exe
#   make run     HD=tests/hello.hd              -> build it, then run it
#
# The generated C stays in build/ rather than being deleted: it is the thing
# to read when the backend does something surprising, and difftest compiles
# its own copy anyway.
BUILD_DIR ?= build
HD        ?= tests/hello.hd
HD_NAME    = $(basename $(notdir $(HD)))
GEN_C      = $(BUILD_DIR)/$(HD_NAME).c
OUT       ?= $(BUILD_DIR)/$(HD_NAME).exe

.PHONY: compile
compile: $(BIN)
	@test -f "$(HD)" || { echo "make compile: no such script: $(HD)"; exit 1; }
	@mkdir -p $(BUILD_DIR)
	./$(BIN) --emit-c $(HD) -o $(GEN_C)
	$(CC) $(CFLAGS) $(INCLUDES) $(GEN_C) $(RUNTIME_SRCS) -o $(OUT) $(LIBS)
	@echo "make compile: $(OUT)"

.PHONY: run
run: compile
	@./$(OUT)

# Runs every script under tests/ twice , once on the VM, once transpiled ,
# and fails if the two disagree on stdout or exit status.
.PHONY: difftest
difftest: $(BIN)
	./tools/difftest.sh

.PHONY: clean
clean:
	rm -f $(BIN) a.exe
	rm -rf $(BUILD_DIR)
	rm -f tests/*.hd.c samples/*.hd.c
