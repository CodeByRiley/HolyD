/* userspace/bin/holyd/ffi_platform.h , what a HolyD host has to provide.
 *
 * ffi.c holds every native the language sees. Almost all of it is portable:
 * the drawing calls only ever touch a struct gfx_surface, and gfx.c is pure
 * arithmetic over a pixel buffer. What is not portable is the handful of
 * things below , getting a window on the screen, reading input, sleeping,
 * and sockets , so those are the only functions a new host implements.
 *
 * Two hosts exist:
 *   ffi_tos.c    , heimdall over IPC, TOS syscalls, the kernel's sockets
 *   ffi_win32.c  , CreateWindowEx over a DIB section, winsock
 *
 * The contract is deliberately narrow. A host hands back a pixel buffer and
 * a stride and is then out of the drawing business until hdp_window_present.
 */
#ifndef HOLYD_FFI_PLATFORM_H
#define HOLYD_FFI_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Windows a script may hold open at once. Handles run 1..HD_MAX_WINDOWS so
 * that 0 is never a valid one and -1 can mean failure. */
#define HD_MAX_WINDOWS 16

/* Creation flags, passed through from WinCreate's fourth argument. */
#define HD_CREATE_STATUSBAR 0x1u

/* Event codes. These are the numbers a script compares against EV_*, and
 * they match heimdall's WM_EV_* so the TOS host can pass them through
 * unchanged; ffi_tos.c static-asserts that. */
enum {
    HD_EV_NONE       = 0,
    HD_EV_KEY_DOWN   = 1,
    HD_EV_KEY_UP     = 2,
    HD_EV_MOUSE_MOVE = 3,
    HD_EV_MOUSE_DOWN = 4,
    HD_EV_MOUSE_UP   = 5,
    HD_EV_RESIZE     = 100,
    HD_EV_QUIT       = 101,
};

/* Prompt kinds and answers, matching heimdall's WM_PROMPT_*. */
enum {
    HD_PROMPT_MESSAGE = 0,
    HD_PROMPT_CONFIRM = 1,
    HD_PROMPT_TEXT    = 2,
};
enum {
    HD_PROMPT_CANCEL = 0,
    HD_PROMPT_OK     = 1,
    HD_PROMPT_NO     = 2,
};

/* Mouse button mask in hdp_event.param on a button event. */
#define HD_MOUSE_LEFT   0x01
#define HD_MOUSE_RIGHT  0x02
#define HD_MOUSE_MIDDLE 0x04

/* Where a window's pixels live. `stride` is in pixels, not bytes, because
 * that is what gfx_surface_init wants. A host may hand back a buffer wider
 * or taller than w x h , a status strip below the client area, say , and
 * the surface is built over the top-left w x h of it. */
struct hdp_surface {
    uint32_t *pixels;
    int       w, h;
    int       stride;
};

struct hdp_event {
    int                type;    /* HD_EV_*                                */
    int                param;   /* keycode, or button mask                */
    int                x, y;    /* pointer position, in client pixels     */
    int                window;  /* handle it belongs to, or 0 if unknown  */
    struct hdp_surface surface; /* on HD_EV_RESIZE: the replacement       */
};

/* ---------------- Host interface ---------------------------------------- */
/* Unless noted, 0 means success and -1 means failure. */

/* Open a window with a `w` x `h` client area. On success *out_handle is in
 * 1..HD_MAX_WINDOWS and *out describes the pixels to draw into. */
int hdp_window_open(int w, int h, const char *title, unsigned flags,
                    int *out_handle, struct hdp_surface *out);

int hdp_window_close(int handle);
int hdp_window_set_title(int handle, const char *title);

/* No-op unless the window was opened with HD_CREATE_STATUSBAR. */
int hdp_window_set_status(int handle, const char *text);

/* Push what has been drawn to the screen. */
int hdp_window_present(int handle);

/* Non-blocking. Returns 1 when *out was filled, 0 when nothing is queued.
 * `handle` is the window the script named, or 0 for "any"; a host that can
 * attribute an event to a window should honour it.
 *
 * On HD_EV_RESIZE the host fills out->surface with the replacement pixels,
 * because a resize is exactly when the old buffer stops being valid. */
int hdp_poll_event(int handle, struct hdp_event *out);

/* Modal dialog owned by `handle`. Returns HD_PROMPT_OK / _NO / _CANCEL; a
 * host that cannot show one returns HD_PROMPT_CANCEL, so callers only have
 * to handle "the user did not say yes". For HD_PROMPT_TEXT the reply is
 * copied into `out` (NUL-terminated, truncated to `cap`). */
int hdp_prompt(int handle, int kind, const char *message, char *out,
               size_t cap);

/* Sleep at least `ms` milliseconds, and give up the rest of a timeslice.
 * A host with a message queue must keep pumping it inside hdp_sleep_ms, or
 * its windows stop responding while a script is between frames. */
int hdp_sleep_ms(long long ms);
int hdp_yield(void);

/* ---------------- Sockets ------------------------------------------------ */
/* A datagram socket, or -1. The returned value is the descriptor a script
 * passes back in; it is opaque to ffi.c. */
int hdp_udp_socket(void);
int hdp_udp_bind(int fd, unsigned port);
int hdp_udp_send(int fd, const void *buf, int len,
                 const unsigned char ip[4], unsigned port);

/* Bytes read, or -1. Blocks until a datagram arrives. */
int hdp_udp_recv(int fd, void *buf, int cap);

#endif
