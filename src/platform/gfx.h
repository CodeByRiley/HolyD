/*
 * HolyD's graphics API adapter.
 *
 * TOS provides gfx as part of libtos. Standalone builds use the compatible
 * implementation owned by HolyD. TOS's build sets HOLYD_TARGET_TOS=1
 * explicitly; guessing from available headers would make the selected ABI
 * depend on the host's include path.
 */
#ifndef HOLYD_PLATFORM_GFX_H
#define HOLYD_PLATFORM_GFX_H

#if defined(HOLYD_TARGET_TOS) && HOLYD_TARGET_TOS
#include <lib/gfx.h>
#else
#include "standalone/gfx.h"
#endif

#endif
