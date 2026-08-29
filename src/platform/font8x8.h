/* Select TOS's font table or HolyD's standalone copy. */
#ifndef HOLYD_PLATFORM_FONT8X8_H
#define HOLYD_PLATFORM_FONT8X8_H

#if defined(HOLYD_TARGET_TOS) && HOLYD_TARGET_TOS
#include <include/fonts/font8x8.h>
#else
#include "standalone/font8x8.h"
#endif

#endif
