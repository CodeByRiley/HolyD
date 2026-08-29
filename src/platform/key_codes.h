/* Select the keycode ABI used by the active HolyD host. */
#ifndef HOLYD_PLATFORM_KEY_CODES_H
#define HOLYD_PLATFORM_KEY_CODES_H

#if defined(HOLYD_TARGET_TOS) && HOLYD_TARGET_TOS
#include <include/key_codes.h>
#else
#include "standalone/key_codes.h"
#endif

#endif
