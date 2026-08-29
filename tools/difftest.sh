#!/bin/sh
# Runs every script under tests/ twice: once on the bytecode VM, once
# transpiled to C and compiled. Fails if the two disagree on stdout or on
# exit status.
#
# This is the only thing that makes the C backend trustworthy. The VM is the
# oracle; the backend is correct exactly insofar as it cannot be told apart
# from it. Run it after any change to src/runtime.c or src/emit_c.c.
#
#   ./tools/difftest.sh [dir]      default: tests/

set -u

DIR="${1:-tests}"
BIN="${BIN:-./holyd.exe}"
CC="${CC:-gcc}"
CFLAGS="${CFLAGS:--std=gnu11 -O2}"
LIBS="-lgdi32 -luser32 -lws2_32"

RUNTIME="src/runtime.c src/eval.c src/ffi.c src/ffi_win32.c \
         src/platform/standalone/gfx.c src/platform/standalone/bmp.c"

# Repo-local rather than mktemp: under MSYS2, mktemp -d hands back an MSYS
# path like /tmp/tmp.XXXX, which the shell understands but holyd.exe does
# not , it is a native Windows binary and cannot open it. A relative path
# works for both, since they share a working directory.
WORK="./.difftest"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT INT TERM

# gcc writes assembler and linker scratch files into TMP. Some Windows
# shells hand down a TMP the toolchain cannot write to, which surfaces as a
# bare "ld returned 1 exit status" with nothing to explain it. We already
# have a directory of our own, so point the toolchain at that.
TMPDIR="$WORK"
TMP="$WORK"
TEMP="$WORK"
export TMPDIR TMP TEMP

# A missing holyd, or a compiler that cannot run at all, would otherwise show
# up as every script "differing" , which points at the backend when the
# problem is the build. Check both once, up front, and say so plainly.
if [ ! -x "$BIN" ]; then
    echo "difftest: no usable '$BIN' , run make first."
    exit 2
fi

printf 'int main(void){return 0;}\n' >"$WORK/preflight.c"
if ! $CC $CFLAGS "$WORK/preflight.c" -o "$WORK/preflight.exe" \
        >"$WORK/preflight.log" 2>&1
then
    echo "difftest: cannot compile anything with '$CC' , not a backend failure."
    sed 's/^/       /' "$WORK/preflight.log" | head -10
    exit 2
fi

pass=0
fail=0

for script in "$DIR"/*.hd; do
    [ -e "$script" ] || continue
    name="$(basename "$script" .hd)"

    "$BIN" "$script" >"$WORK/$name.vm.out" 2>&1
    vm_status=$?

    if ! "$BIN" --emit-c "$script" -o "$WORK/$name.c" >"$WORK/$name.emit" 2>&1
    then
        echo "FAIL $script , could not transpile"
        sed 's/^/       /' "$WORK/$name.emit"
        fail=$((fail + 1))
        continue
    fi

    # shellcheck disable=SC2086
    if ! $CC $CFLAGS -I src "$WORK/$name.c" $RUNTIME \
            -o "$WORK/$name.exe" $LIBS >"$WORK/$name.cc" 2>&1
    then
        echo "FAIL $script , generated C did not compile"
        sed 's/^/       /' "$WORK/$name.cc" | head -40
        fail=$((fail + 1))
        continue
    fi

    "$WORK/$name.exe" >"$WORK/$name.c.out" 2>&1
    c_status=$?

    if [ "$vm_status" != "$c_status" ]; then
        echo "FAIL $script , exit status: VM $vm_status, C $c_status"
        fail=$((fail + 1))
        continue
    fi

    if ! diff -u "$WORK/$name.vm.out" "$WORK/$name.c.out" \
            >"$WORK/$name.diff" 2>&1
    then
        echo "FAIL $script , output differs"
        sed 's/^/       /' "$WORK/$name.diff" | head -30
        fail=$((fail + 1))
        continue
    fi

    echo "ok   $script"
    pass=$((pass + 1))
done

echo
echo "difftest: $pass matched, $fail differed"
[ "$fail" -eq 0 ]
