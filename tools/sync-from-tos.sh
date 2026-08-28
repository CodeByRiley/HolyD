#!/usr/bin/env bash
# Refresh this repository from a TOS checkout.
#
#   tools/sync-from-tos.sh ../TOS
#
# TOS is upstream for every file this copies. That is not an accident of how
# the split happened: lib/gfx.c and lib/bmp.c are shared with winman and the
# rest of the desktop, so TOS has to keep its own copies, and the HolyD
# sources still build into the TOS image as userspace/bin/holyd. Until this
# repository is wired back into TOS as a submodule, the two trees are kept
# equal by running this.
#
# Nothing here is edited on this side. The files this repository owns , the
# Makefile, README, vendor/tos/lib/syscall.h, samples/, tests/, tools/ , are
# not touched, so a sync never clobbers local work.
set -euo pipefail

TOS="${1:-../TOS}"
cd "$(dirname "$0")/.."

if [ ! -d "$TOS/userspace/bin/holyd" ]; then
    echo "not a TOS checkout: $TOS" >&2
    echo "usage: tools/sync-from-tos.sh <path-to-TOS>" >&2
    exit 2
fi

copy() {
    # $1 source under $TOS, $2 destination here
    mkdir -p "$(dirname "$2")"
    if cmp -s "$TOS/$1" "$2" 2>/dev/null; then
        return
    fi
    cp "$TOS/$1" "$2"
    echo "  updated $2"
}

echo "syncing from $TOS"

# HolyD itself.
for f in main.c compiler.c compiler.h eval.c eval.h ffi.c ffi.h \
         ffi_platform.h ffi_tos.c ffi_win32.c; do
    copy "userspace/bin/holyd/$f" "src/$f"
done
copy userspace/bin/holyd/lexer/lexer.c  src/lexer/lexer.c
copy userspace/bin/holyd/lexer/lexer.h  src/lexer/lexer.h
copy userspace/bin/holyd/parser/parser.c src/parser/parser.c
copy userspace/bin/holyd/parser/parser.h src/parser/parser.h
copy userspace/bin/holyd/ast/ast.c      src/ast/ast.c
copy userspace/bin/holyd/ast/ast.h      src/ast/ast.h
copy userspace/bin/holyd/HOLYD.todo     docs/roadmap.md

# Drawing and input, shared with winman. vendor/tos/lib/syscall.h is NOT in
# this list: it is a shim this repository owns, not a copy of TOS's.
copy userspace/lib/gfx.c                vendor/tos/lib/gfx.c
copy userspace/lib/gfx.h                vendor/tos/lib/gfx.h
copy userspace/lib/bmp.c                vendor/tos/lib/bmp.c
copy userspace/lib/bmp.h                vendor/tos/lib/bmp.h
copy userspace/include/key_codes.h      vendor/tos/include/key_codes.h
copy userspace/include/fonts/font8x8.h  vendor/tos/include/fonts/font8x8.h

# Scripts. gui/window/net block on a person or on the network, so they are
# samples rather than tests , see the Makefile's test target.
for f in gui window net; do
    copy "rootfs/holyd/$f.hd" "samples/$f.hd"
done
for f in array conditionals functions hello holyc_d_style math no_semis strings; do
    copy "rootfs/holyd/$f.hd" "tests/$f.hd"
done

echo "done. 'git diff' to see what moved."
