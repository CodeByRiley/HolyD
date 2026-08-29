# HolyD

A HolyC-flavoured language with a D accent, and a bytecode VM to run it.
Originally the scripting language for [TOS](../TOS); this repository is the
standalone build, which opens real windows.

```
make
./holyd.exe samples/gui.hd
```

![the windowing sample](docs/gui.png)

## What it is

A lexer, parser, AST, bytecode compiler and VM in about 2,300 lines of C,
plus an FFI that gives scripts windows, drawing, input and UDP sockets. It
runs the same `.hd` file on Windows and on TOS, unchanged.

```c
I64 win = WinCreate(420, 260, "HolyD GUI", WIN_STATUSBAR);

while (running == 1) {
    I64 ev = WinPollEvent(win);
    while (ev != EV_NONE) {
        if (ev == EV_KEY_DOWN && WinEventKey() == KEY_ESC) running = 0;
        ev = WinPollEvent(win);
    }

    WinClear(win, COLOR_DARK_GRAY);
    WinFillRect(win, bx, by, 40, 40, COLOR_BLUE);
    WinDrawText(win, 8, 8, "clicks: " ~ Str(clicks), COLOR_WHITE, 1);
    WinInvalidate(win);
    Sleep(16);
}
```

`samples/gui.hd` is the real thing, and predates `&&`, so it spells that
condition as a nested `if`.

## Building

Windows, with MSYS2 or any MinGW-w64 gcc:

| | |
|---|---|
| `make` | build `holyd.exe` |
| `make test` | run every script in `tests/` |
| `make gui` | build and open the windowing sample |

There are no dependencies beyond the C runtime and three system libraries:
`gdi32` for the DIB section a window draws into, `user32` for the window
itself, `ws2_32` for `UdpSocket`.

## Using it

```
holyd <source.hd>          run a script
holyd --test [dir]         run every .hd under dir (default holyd/tests/)
holyd -tokens <source.hd>  print the token stream
holyd --dump-bytecode ...  disassemble before running
holyd --interpret ...      walk the AST instead of running bytecode
holyd --emit-c <source.hd> translate to C instead of running it
holyd --emit-c ... -o out.c   where to write it
holyd --help
```

`--interpret` has no FFI: the tree walker predates it, so the `Win*` and
`Udp*` natives resolve only under the bytecode VM.

## Compiling to an executable

`--emit-c` writes a C translation unit that links against the same runtime
the VM uses, which gives you a real `.exe` that no longer needs `holyd`:

```
make compile HD=samples/gui.hd
```

That writes `build/gui.c` and `build/gui.exe`. The name comes from the
script; `OUT=` overrides it, and `make run HD=...` builds and then runs.

| | |
|---|---|
| `make compile HD=tests/hello.hd` | `build/hello.exe` |
| `make compile HD=samples/gui.hd OUT=gui.exe` | `gui.exe` |
| `make run HD=tests/hello.hd` | build it, then run it |

The generated C is kept rather than deleted: it is the thing to read when
the backend does something surprising. By hand, the same build is

```
holyd --emit-c samples/gui.hd -o gui.c
gcc -std=gnu11 -O2 -I src gui.c \
    src/runtime.c src/eval.c src/ffi.c src/ffi_win32.c \
    src/platform/standalone/gfx.c src/platform/standalone/bmp.c \
    -o gui.exe -lgdi32 -luser32 -lws2_32
```

It is a bootstrap backend, not a fast one. Control flow becomes real C
control flow and calls become direct C calls, but values stay boxed
`HDValue`s and variables stay in the runtime `Environment`, so the arithmetic
costs what it costs in the VM. That is deliberate: every semantic the VM has
comes along unchanged, which is what lets the two be diffed against each
other. Making it fast means resolving names to frame slots and giving the
language a real type system , `docs/roadmap.md` §10.

`make difftest` runs every script in `tests/` twice, once on the VM and once
transpiled and compiled, and fails if the two disagree on stdout or on exit
status. The VM is the oracle; run it after touching `src/runtime.c` or
`src/emit_c.c`.

## Examples

Everything in `tests/` finishes on its own and is checked by both execution
paths, so they double as the worked examples. Read them in roughly this
order:

| | |
|---|---|
| `tests/hello.hd` | printing, globals |
| `tests/fizzbuzz.hd` | `else if` chains, `%`, and truncating division |
| `tests/operators.hd` | arithmetic, bitwise, shifts, `^^`, `!`, and short-circuiting made visible |
| `tests/control_flow.hd` | what to write where `break`, `continue`, `switch` and `do`/`while` would go |
| `tests/recursion.hd` | recursion, mutual recursion, declaration order |
| `tests/sorting.hd` | arrays in place, and that a passed array aliases the caller's |
| `tests/life_text.hd` | a 2D grid in a flat array, and Life's rules checked against known patterns |

`samples/` needs a person or a network:

| | |
|---|---|
| `samples/gui.hd` | the windowing FFI end to end |
| `samples/window.hd` | the smallest window that draws something |
| `samples/life.hd` | `life_text.hd` again, drawn , space pauses, `r` reseeds, `s` steps |
| `samples/net.hd` | UDP |

`tests/control_flow.hd` is the one to read first if the missing statements
are making the language feel smaller than it is.

## Layout

```
src/            the language, and the natives
  runtime.c       what a HolyD value is and what the operators do. Shared:
                  the VM and transpiled C both run on this, so there is one
                  definition of each operator rather than two.
  emit_c.c        the C backend , AST to a C translation unit.
  ffi.c           every native a script can call. Portable: the drawing
                  calls only ever touch a gfx_surface.
  ffi_platform.h  what a host has to provide , about fourteen functions
  ffi_win32.c     CreateWindowEx over a top-down DIB section
  ffi_tos.c       the other host: winman over IPC. Built inside the TOS
                  tree, not here.
  platform/       adapters for gfx, BMP, keycodes, and font8x8
    standalone/   HolyD-owned implementations used away from TOS
samples/        scripts that open a window or wait on the network
tests/          scripts that finish on their own , what --test runs
tools/difftest.sh  runs tests/ on both paths and diffs them
docs/roadmap.md what the language is still missing
```

The reason one `ffi.c` serves both hosts is that `lib/gfx.c` is pure
arithmetic over a pixel buffer with no system calls in it. winman hands a
client a raw BGRA buffer; a DIB section is also a raw BGRA buffer. So the
drawing natives never needed porting , only window creation, input, sleeping
and sockets did, and those are `ffi_platform.h`.

## Relationship to TOS

**This repository is upstream.** TOS consumes it as a submodule at
`userspace/bin/holyd`, and builds `src/` into the OS image with `ffi_tos.c`
as the host. Changing the language means committing here, then bumping the
pointer in TOS:

```
# after committing and pushing the HolyD checkout, in the TOS tree
git -C userspace/bin/holyd fetch origin
git -C userspace/bin/holyd checkout <new-holyd-commit>
git add userspace/bin/holyd && git commit -m "holyd: bump"
```

TOS's samples on the image come from `samples/` and `tests/` here, so those
have one home too. There is no reverse source sync.

The headers under `src/platform/` make the graphics boundary explicit. TOS
builds HolyD with `HOLYD_TARGET_TOS=1`, so the adapters use TOS's public
`gfx`, `bmp`, `key_codes`, and `font8x8` headers and the executable links
their implementations from `libtos`. A standalone build leaves that macro
unset and selects the compatible implementations under
`src/platform/standalone/` instead. Those fallbacks belong to HolyD; they are
not refreshed by copying files out of TOS.

Because it is a submodule, a fresh TOS clone needs the extra step:

```
git clone --recursive https://github.com/CodeByRiley/TOS.git
# or, in an existing clone
git submodule update --init
```

## Language

HolyC with D borrowings, and unfinished in ways worth knowing before writing
much:

- `~` joins strings. `Str()` turns an I64 into one; there is no other way.
- No `break`, `continue`, `switch`, or `do`/`while`. `&&`, `||`, `!`, `%`,
  the bitwise and shift operators, `^^`, and the compound assignments all
  work; `tests/operators.hd` is the tour.
- `else if` and brace-less bodies do work. So do hex literals and floats.
- `main()` runs on its own unless the top level already calls it. A `main`
  that declares parameters is never called automatically.
- No structs. That is why `WinPollEvent` returns a code and the rest of the
  event is read back through `WinEventKey()`, `WinEventX()` and friends.

`docs/roadmap.md` is the full list of what is missing, roughly in the order
it is worth adding.

## Natives

Windows: `WinCreate` `WinDestroy` `WinSetTitle` `WinSetStatus` `WinInvalidate`
`WinPresent` `WinWidth` `WinHeight` `WinPrompt` `WinPromptText`

Drawing: `WinClear` `WinFillRect` `WinDrawRect` `WinBevel` `WinDrawPixel`
`WinHLine` `WinVLine` `WinDrawText` `WinTextWidth`

Events: `WinPollEvent` `WinEventKey` `WinEventX` `WinEventY` `WinEventW`
`WinEventH`

Other: `Str` `Rgb` `Sleep` `Yield` `UdpSocket` `UdpBind` `UdpSend`
`UdpReceive`

Constants arrive as ordinary globals, because the language has no enums:
`EV_*`, `KEY_*`, `COLOR_*`, `MOUSE_*`, `PROMPT_*`, `WIN_STATUSBAR`.
