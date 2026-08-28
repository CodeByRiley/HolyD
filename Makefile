# HolyD , standalone build.
#
#   make            build holyd.exe
#   make test       run every script under tests/
#   make gui        build, then open the windowing sample
#   make clean
#
# Windows only for now: the one host implementation here is ffi_win32.c.
# src/ffi_tos.c is the other one , winman over IPC , and is built inside the
# TOS tree rather than here. See README.md.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
BIN     ?= holyd.exe

# vendor/tos mirrors TOS's userspace/ layout, so the vendored sources resolve
# <lib/gfx.h> and <include/key_codes.h> exactly as they do upstream.
INCLUDES := -I src -I vendor/tos -I vendor/tos/lib

SRCS := \
	src/main.c \
	src/compiler.c \
	src/eval.c \
	src/ffi.c \
	src/ffi_win32.c \
	src/lexer/lexer.c \
	src/parser/parser.c \
	src/ast/ast.c \
	vendor/tos/lib/gfx.c \
	vendor/tos/lib/bmp.c

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

.PHONY: clean
clean:
	rm -f $(BIN)
