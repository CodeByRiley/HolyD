/* userspace/bin/holyd/ffi_tos.c , the TOS host for HolyD's natives.
 *
 * Windows are winman's, reached over the IPC handshake in lib/wm.c, and the
 * pixels are a page-aligned buffer winman maps into this process. Sockets
 * are the kernel's, through musl's POSIX wrappers.
 *
 * The only thing here that is not a thin wrapper is the handle table.
 * Winman's handle space is its own business and has changed size before, so
 * a script never sees a winman handle , it gets an index into `slots`, and
 * this file translates. That keeps ffi.c's table bounded by HD_MAX_WINDOWS
 * no matter what winman does.
 */
#include "ffi_platform.h"

#include <assert.h>
#include <errno.h>
#include <lib/syscall.h>
#include <lib/wm.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* A script comparing against EV_KEY_DOWN is comparing against a number that
 * came off winman's wire. If these ever diverge the events would decode as
 * each other, which is the kind of bug that looks like bad input handling. */
static_assert((int)HD_EV_NONE == (int)WM_EV_NONE, "event codes must match winman");
static_assert((int)HD_EV_KEY_DOWN == (int)WM_EV_KEY_DOWN, "event codes must match winman");
static_assert((int)HD_EV_KEY_UP == (int)WM_EV_KEY_UP, "event codes must match winman");
static_assert((int)HD_EV_MOUSE_MOVE == (int)WM_EV_MOUSE_MOVE, "event codes must match winman");
static_assert((int)HD_EV_MOUSE_DOWN == (int)WM_EV_MOUSE_DOWN, "event codes must match winman");
static_assert((int)HD_EV_MOUSE_UP == (int)WM_EV_MOUSE_UP, "event codes must match winman");
static_assert((int)HD_EV_RESIZE == (int)WM_EV_RESIZE, "event codes must match winman");
static_assert((int)HD_EV_QUIT == (int)WM_EV_QUIT, "event codes must match winman");
static_assert((int)HD_PROMPT_MESSAGE == (int)WM_PROMPT_MESSAGE, "prompt kinds must match winman");
static_assert((int)HD_PROMPT_CONFIRM == (int)WM_PROMPT_CONFIRM, "prompt kinds must match winman");
static_assert((int)HD_PROMPT_TEXT == (int)WM_PROMPT_TEXT, "prompt kinds must match winman");
static_assert((int)HD_PROMPT_CANCEL == (int)WM_PROMPT_CANCEL, "prompt answers must match winman");
static_assert((int)HD_PROMPT_OK == (int)WM_PROMPT_OK, "prompt answers must match winman");
static_assert((int)HD_PROMPT_NO == (int)WM_PROMPT_NO, "prompt answers must match winman");
static_assert((int)HD_CREATE_STATUSBAR == (int)WM_CREATE_STATUSBAR, "flags must match winman");

/* slots[i] holds the winman handle for HolyD handle i + 1, or 0 when free. */
static int slots[HD_MAX_WINDOWS];

static int to_wm(int handle) {
    if (handle < 1 || handle > HD_MAX_WINDOWS) return 0;
    return slots[handle - 1];
}

static void fill_surface(struct hdp_surface *out, uint64_t va, int w, int h,
                         uint32_t pitch) {
    out->pixels = (uint32_t *)(uintptr_t)va;
    out->w      = w;
    out->h      = h;
    out->stride = (int)(pitch / 4);
}

int hdp_window_open(int w, int h, const char *title, unsigned flags,
                    int *out_handle, struct hdp_surface *out) {
    int slot = -1;
    for (int i = 0; i < HD_MAX_WINDOWS; i++) {
        if (slots[i] == 0) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct wm_window win;
    if (wm_window_create_ex(w, h, title, flags, &win) != 0) return -1;

    slots[slot]  = win.handle;
    *out_handle  = slot + 1;
    fill_surface(out, win.surface_va, win.w, win.h, win.pitch);
    return 0;
}

int hdp_window_close(int handle) {
    int wm = to_wm(handle);
    if (wm == 0) return -1;
    int result = wm_window_destroy(wm);
    slots[handle - 1] = 0;
    return result;
}

int hdp_window_set_title(int handle, const char *title) {
    int wm = to_wm(handle);
    return wm == 0 ? -1 : wm_window_set_title(wm, title);
}

int hdp_window_set_status(int handle, const char *text) {
    int wm = to_wm(handle);
    return wm == 0 ? -1 : wm_window_set_status(wm, text);
}

int hdp_window_present(int handle) {
    int wm = to_wm(handle);
    return wm == 0 ? -1 : wm_window_invalidate(wm);
}

int hdp_poll_event(int handle, struct hdp_event *out) {
    (void)handle; /* winman's queue is per-process, not per-window. */

    struct wm_event ev;
    if (!wm_poll_event(&ev)) return 0;

    memset(out, 0, sizeof(*out));
    out->type  = ev.type;
    out->param = ev.param;
    out->x     = ev.x;
    out->y     = ev.y;

    if (ev.type == WM_EV_RESIZE) {
        fill_surface(&out->surface, ev.surface_va, ev.w, ev.h, ev.pitch);
    }
    return 1;
}

int hdp_prompt(int handle, int kind, const char *message, char *out,
               size_t cap) {
    int wm = to_wm(handle);
    if (wm == 0) return HD_PROMPT_CANCEL;
    return wm_prompt(wm, kind, message, out, cap);
}

/* The unit is a scheduler tick, nominally a millisecond. IRQ0 currently
 * fires well below the rate the PIT is programmed for, so this overshoots
 * by roughly 15x , a lower bound, not a frame budget. */
int hdp_sleep_ms(long long ms) {
    if (ms < 0) ms = 0;
    return (int)sleep_ticks((unsigned long)ms);
}

int hdp_yield(void) { return (int)yield(); }

/* ---------------- Sockets ------------------------------------------------ */

int hdp_udp_socket(void) {
    return socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
}

int hdp_udp_bind(int fd, unsigned port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    return bind(fd, (struct sockaddr *)&addr, sizeof(addr));
}

int hdp_udp_send(int fd, const void *buf, int len,
                 const unsigned char ip[4], unsigned port) {
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((uint16_t)port);
    memcpy(&dest.sin_addr.s_addr, ip, 4);

    return (int)sendto(fd, buf, (size_t)len, 0, (struct sockaddr *)&dest,
                       sizeof(dest));
}

int hdp_udp_recv(int fd, void *buf, int cap) {
    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    socklen_t src_len = sizeof(src);

    int bytes;
    do {
        bytes = (int)recvfrom(fd, buf, (size_t)cap, 0,
                              (struct sockaddr *)&src, &src_len);
    } while (bytes < 0 && errno == EINTR);

    if (bytes < 0) {
        printf("[UDP] recvfrom failed: fd=%d errno=%d (%s)\n", fd, errno,
               strerror(errno));
    }
    return bytes;
}
