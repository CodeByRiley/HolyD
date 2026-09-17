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
# src/ffi_tos.c is the other one , heimdall over IPC , and is built inside the
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
	src/resolve.c \
	src/typecheck.c \
	src/compiler.c \
	src/emit_c.c \
	src/emit_asm.c \
	src/emit_pe.c \
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

# Compile one script into a standalone executable, through either backend.
#
#   make compile HD=samples/gui.hd              -> build/gui.exe
#   make compile HD=samples/gui.hd OUT=g.exe    -> g.exe
#   make compile HD=tests/hello.hd BACKEND=asm  -> via x86-64 assembly
#   make run     HD=tests/hello.hd              -> build it, then run it
#
# BACKEND=c writes C and hands it to the C compiler; BACKEND=asm writes
# x86-64 assembly and hands it to the assembler. Both link the same runtime
# and produce the same program , see docs/roadmap.md section 10 for why the
# assembly one exists and why it is not the faster of the two.
#
# The generated file stays in build/ rather than being deleted: it is the
# thing to read when a backend does something surprising, and difftest
# compiles its own copy anyway.
BUILD_DIR ?= build
HD        ?= tests/hello.hd
BACKEND   ?= c
HD_NAME    = $(basename $(notdir $(HD)))
OUT       ?= $(BUILD_DIR)/$(HD_NAME).exe

ifeq ($(BACKEND),asm)
EMIT_FLAG  = --emit-asm
GEN_SRC    = $(BUILD_DIR)/$(HD_NAME).s
else
EMIT_FLAG  = --emit-c
GEN_SRC    = $(BUILD_DIR)/$(HD_NAME).c
endif

.PHONY: compile
compile: $(BIN)
	@test -f "$(HD)" || { echo "make compile: no such script: $(HD)"; exit 1; }
	@mkdir -p $(BUILD_DIR)
	./$(BIN) $(EMIT_FLAG) $(HD) -o $(GEN_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(GEN_SRC) $(RUNTIME_SRCS) -o $(OUT) $(LIBS)
	@echo "make compile: $(OUT)"

.PHONY: run
run: compile
	@./$(OUT)

# Runs every script under tests/ twice , once on the VM, once compiled , and
# fails if the two disagree on stdout or exit status. `difftest-asm` does the
# same through the assembly backend, and `difftest-all` runs both, which is
# what tells a codegen bug apart from a runtime one.
.PHONY: difftest
difftest: $(BIN)
	./tools/difftest.sh

.PHONY: difftest-asm
difftest-asm: $(BIN)
	BACKEND=asm ./tools/difftest.sh

.PHONY: difftest-all
difftest-all: difftest difftest-asm

.PHONY: clean
clean:
	rm -f $(BIN) a.exe
	rm -rf $(BUILD_DIR)
	rm -f tests/*.hd.c samples/*.hd.c tests/*.hd.s samples/*.hd.s
