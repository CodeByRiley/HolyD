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

(That example uses `&&`, which the parser does not have yet. `samples/gui.hd`
is the real thing.)

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
holyd --help
```

`--interpret` has no FFI: the tree walker predates it, so the `Win*` and
`Udp*` natives resolve only under the bytecode VM.

## Layout

```
src/            the language, and the natives
  ffi.c           every native a script can call. Portable: the drawing
                  calls only ever touch a gfx_surface.
  ffi_platform.h  what a host has to provide , about fourteen functions
  ffi_win32.c     CreateWindowEx over a top-down DIB section
  ffi_tos.c       the other host: winman over IPC. Built inside the TOS
                  tree, not here.
vendor/tos/     drawing and input, copied from TOS (see below)
samples/        scripts that open a window or wait on the network
tests/          scripts that finish on their own , what --test runs
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
# in the TOS tree
cd userspace/bin/holyd
git commit -am "..." && git push
cd ../../..
git add userspace/bin/holyd && git commit -m "holyd: bump"
```

TOS's samples on the image come from `samples/` and `tests/` here, so those
have one home too.

`vendor/tos/` runs the other way: `lib/gfx.c` and `lib/bmp.c` are shared with
winman and the rest of the TOS desktop, so TOS owns them and this repository
needs a copy to draw when built on its own. `tools/sync-from-tos.sh ../TOS`
refreshes those six files and nothing else.

`vendor/tos/lib/syscall.h` is the exception to the exception , this
repository owns it. TOS's real one is the whole kernel interface; `bmp.c`
wants four calls out of it, and that file is those four.

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
- No `&&`, `||`, unary `!`, `break`, `continue`, `switch`, `+=`, or `%`.
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
