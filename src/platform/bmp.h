/* Select TOS's BMP API or HolyD's standalone-compatible implementation. */
#ifndef HOLYD_PLATFORM_BMP_H
#define HOLYD_PLATFORM_BMP_H

#if defined(HOLYD_TARGET_TOS) && HOLYD_TARGET_TOS
#include <lib/bmp.h>
#else
#include "standalone/bmp.h"
#endif

#endif
