#!/usr/bin/env bash
# Refresh the vendored TOS files from a TOS checkout.
#
#   tools/sync-from-tos.sh ../TOS
#
# Only vendor/tos/ is copied, and only in that direction. Everything else in
# this repository is upstream of TOS rather than downstream of it: TOS
# consumes this repository as a submodule at userspace/bin/holyd, so src/,
# samples/ and tests/ are edited HERE and TOS picks them up by bumping the
# submodule pointer.
#
# vendor/tos/ goes the other way because lib/gfx.c and lib/bmp.c are shared
# with winman and the rest of the TOS desktop. TOS owns them; this
# repository needs them to draw when it is built away from TOS.
#
# vendor/tos/lib/syscall.h is deliberately not in the list. It is a shim this
# repository owns , four prototypes out of TOS's whole kernel interface , not
# a copy of anything upstream.
set -euo pipefail

TOS="${1:-../TOS}"
cd "$(dirname "$0")/.."

if [ ! -f "$TOS/userspace/lib/gfx.c" ]; then
    echo "not a TOS checkout: $TOS" >&2
    echo "usage: tools/sync-from-tos.sh <path-to-TOS>" >&2
    exit 2
fi

changed=0
copy() {
    mkdir -p "$(dirname "$2")"
    if cmp -s "$TOS/$1" "$2" 2>/dev/null; then
        return
    fi
    cp "$TOS/$1" "$2"
    echo "  updated $2"
    changed=1
}

echo "syncing vendor/tos from $TOS"

copy userspace/lib/gfx.c               vendor/tos/lib/gfx.c
copy userspace/lib/gfx.h               vendor/tos/lib/gfx.h
copy userspace/lib/bmp.c               vendor/tos/lib/bmp.c
copy userspace/lib/bmp.h               vendor/tos/lib/bmp.h
copy userspace/include/key_codes.h     vendor/tos/include/key_codes.h
copy userspace/include/fonts/font8x8.h vendor/tos/include/fonts/font8x8.h

if [ "$changed" -eq 0 ]; then
    echo "  already up to date"
else
    echo "done. 'git diff' to see what moved, then rebuild."
fi
