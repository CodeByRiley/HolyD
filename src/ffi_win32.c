/* userspace/bin/holyd/ffi_win32.c , the Windows host for HolyD's natives.
 *
 * A window is a plain CreateWindowEx over a top-down 32-bit DIB section, and
 * the DIB's bits are handed to ffi.c as the drawing surface. That is the
 * whole trick: heimdall also hands out a raw BGRA buffer, so lib/gfx.c draws
 * into a Win32 window through exactly the same code path it uses on TOS, and
 * a .hd script does not know the difference.
 *
 * Keyboard codes go through the scan code in lParam rather than the virtual
 * key. Linux input codes , which include/key_codes.h is , were derived from
 * XT set 1, so for the main block the hardware scan code IS the KEY_* value
 * and needs no table at all. Only the extended keys diverge, and those are
 * the short table below.
 *
 * holyd is a console program that can also open windows, so this file never
 * takes over the process: no WinMain, no modal loop except inside a prompt,
 * and the message pump runs from hdp_poll_event and hdp_sleep_ms.
 */
#include "ffi_platform.h"

#include "platform/gfx.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h> /* GET_X_LPARAM / GET_Y_LPARAM */
#include <winsock2.h>

#include <stdio.h>
#include <string.h>

#define HD_STATUS_H  18        /* font8x8 with room to breathe            */
#define HD_EVENT_CAP 256

#define STATUS_FACE  0x00C0C0C0u
#define STATUS_EDGE  0x00808080u
#define STATUS_TEXT  0x00202020u

struct win32_window {
    HWND      hwnd;
    HBITMAP   dib;
    HDC       memdc;
    uint32_t *pixels;      /* w x (h + status_h), top-down               */
    int       w, h;        /* client area the script draws into          */
    int       status_h;    /* 0 unless HD_CREATE_STATUSBAR               */
    char      status[64];
    int       in_use;
};

static struct win32_window windows[HD_MAX_WINDOWS];
static int                 class_registered;

/* One queue for every window, like heimdall's per-process queue. Entries carry
 * the handle so a resize can name the window it belongs to. */
static struct hdp_event events[HD_EVENT_CAP];
static int              event_head, event_count;

static const char *HD_WINDOW_CLASS = "HolyDWindow";

/* ---------------- Event queue -------------------------------------------- */

static void push_event(const struct hdp_event *ev) {
    if (event_count == HD_EVENT_CAP) {
        /* Drop the oldest. A script that stops polling is already broken;
         * growing without bound would just make it fail later and worse. */
        event_head = (event_head + 1) % HD_EVENT_CAP;
        event_count--;
    }
    events[(event_head + event_count) % HD_EVENT_CAP] = *ev;
    event_count++;
}

static int pop_event(struct hdp_event *out) {
    if (event_count == 0) return 0;
    *out = events[event_head];
    event_head = (event_head + 1) % HD_EVENT_CAP;
    event_count--;
    return 1;
}

static void push_simple(int handle, int type, int param, int x, int y) {
    struct hdp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type   = type;
    ev.param  = param;
    ev.x      = x;
    ev.y      = y;
    ev.window = handle;
    push_event(&ev);
}

/* ---------------- Keyboard ------------------------------------------------ */

/* Extended (0xE0-prefixed) scan codes, whose Linux input codes are not the
 * scan code. Everything else passes through untouched. */
static int extended_key(int scan) {
    switch (scan) {
    case 0x1C: return 96;  /* KEY_KPENTER   */
    case 0x1D: return 97;  /* KEY_RIGHTCTRL */
    case 0x35: return 98;  /* KEY_KPSLASH   */
    case 0x38: return 100; /* KEY_RIGHTALT  */
    case 0x47: return 102; /* KEY_HOME      */
    case 0x48: return 103; /* KEY_UP        */
    case 0x49: return 104; /* KEY_PAGEUP    */
    case 0x4B: return 105; /* KEY_LEFT      */
    case 0x4D: return 106; /* KEY_RIGHT     */
    case 0x4F: return 107; /* KEY_END       */
    case 0x50: return 108; /* KEY_DOWN      */
    case 0x51: return 109; /* KEY_PAGEDOWN  */
    case 0x52: return 110; /* KEY_INSERT    */
    case 0x53: return 111; /* KEY_DELETE    */
    default:   return 0;
    }
}

static int keycode_from_lparam(LPARAM lparam) {
    int scan     = (int)((lparam >> 16) & 0xFF);
    int extended = (int)((lparam >> 24) & 1);

    if (extended) {
        int mapped = extended_key(scan);
        if (mapped != 0) return mapped;
    }
    return scan;
}

/* ---------------- Surfaces ------------------------------------------------ */

static void surface_of(const struct win32_window *w, struct hdp_surface *out) {
    out->pixels = w->pixels;
    out->w      = w->w;
    out->h      = w->h;
    out->stride = w->w; /* the DIB is exactly w wide; the strip is extra rows */
}

static void release_dib(struct win32_window *w) {
    if (w->memdc) { DeleteDC(w->memdc); w->memdc = NULL; }
    if (w->dib)   { DeleteObject(w->dib); w->dib = NULL; }
    w->pixels = NULL;
}

/* Build (or rebuild) the DIB behind a window at its current size. */
static int make_dib(struct win32_window *w) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w->w;
    bi.bmiHeader.biHeight      = -(w->h + w->status_h); /* negative: top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (dib == NULL || bits == NULL) {
        if (dib) DeleteObject(dib);
        return -1;
    }

    HDC memdc = CreateCompatibleDC(NULL);
    if (memdc == NULL) { DeleteObject(dib); return -1; }
    SelectObject(memdc, dib);

    w->dib    = dib;
    w->memdc  = memdc;
    w->pixels = (uint32_t *)bits;
    memset(bits, 0, (size_t)w->w * (size_t)(w->h + w->status_h) * 4);
    return 0;
}

/* Paint the status strip into the rows below the client area. Heimdall draws
 * its own strip, so on this side the host owns those rows too , the script's
 * surface stops at w->h and never sees them. */
static void draw_status(struct win32_window *w) {
    if (w->status_h == 0 || w->pixels == NULL) return;

    struct gfx_surface s;
    gfx_surface_init(&s, w->pixels, w->w, w->h + w->status_h, w->w);

    struct gfx_rect strip = gfx_rect_make(0, w->h, w->w, w->status_h);
    gfx_fill(&s, strip, STATUS_FACE);
    gfx_hline(&s, 0, w->h, w->w, STATUS_EDGE);
    gfx_text(&s, 4, w->h + (w->status_h - GFX_GLYPH_H) / 2, w->status,
             STATUS_TEXT, 1);
}

/* ---------------- Window procedure ---------------------------------------- */

static struct win32_window *window_of(HWND hwnd) {
    LONG_PTR slot = GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (slot < 1 || slot > HD_MAX_WINDOWS) return NULL;
    struct win32_window *w = &windows[slot - 1];
    return w->in_use ? w : NULL;
}

static int handle_of(const struct win32_window *w) {
    return (int)(w - windows) + 1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                 LPARAM lparam) {
    struct win32_window *w = window_of(hwnd);
    if (w == NULL) return DefWindowProc(hwnd, msg, wparam, lparam);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (w->memdc) {
            BitBlt(dc, 0, 0, w->w, w->h + w->status_h, w->memdc, 0, 0, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; /* the DIB covers every pixel; erasing first only flickers */

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        push_simple(handle_of(w), HD_EV_KEY_DOWN, keycode_from_lparam(lparam),
                    0, 0);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        push_simple(handle_of(w), HD_EV_KEY_UP, keycode_from_lparam(lparam),
                    0, 0);
        return 0;

    case WM_MOUSEMOVE:
        push_simple(handle_of(w), HD_EV_MOUSE_MOVE, 0,
                    GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        int button = msg == WM_LBUTTONDOWN   ? HD_MOUSE_LEFT
                     : msg == WM_RBUTTONDOWN ? HD_MOUSE_RIGHT
                                             : HD_MOUSE_MIDDLE;
        SetCapture(hwnd);
        push_simple(handle_of(w), HD_EV_MOUSE_DOWN, button,
                    GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    }

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: {
        int button = msg == WM_LBUTTONUP   ? HD_MOUSE_LEFT
                     : msg == WM_RBUTTONUP ? HD_MOUSE_RIGHT
                                           : HD_MOUSE_MIDDLE;
        ReleaseCapture();
        push_simple(handle_of(w), HD_EV_MOUSE_UP, button,
                    GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    }

    case WM_SIZE: {
        int new_w = LOWORD(lparam);
        int new_h = HIWORD(lparam) - w->status_h;
        if (wparam == SIZE_MINIMIZED || new_w <= 0 || new_h <= 0) return 0;
        if (new_w == w->w && new_h == w->h) return 0;

        release_dib(w);
        w->w = new_w;
        w->h = new_h;
        if (make_dib(w) != 0) return 0;
        draw_status(w);

        struct hdp_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type   = HD_EV_RESIZE;
        ev.x      = new_w;
        ev.y      = new_h;
        ev.window = handle_of(w);
        surface_of(w, &ev.surface);
        push_event(&ev);
        return 0;
    }

    case WM_CLOSE:
        /* Report it and let the script decide. Destroying the window here
         * would pull the surface out from under a frame already in flight. */
        push_simple(handle_of(w), HD_EV_QUIT, 0, 0, 0);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }
}

static void pump(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

/* ---------------- Host interface ------------------------------------------ */

int hdp_window_open(int w, int h, const char *title, unsigned flags,
                    int *out_handle, struct hdp_surface *out) {
    if (!class_registered) {
        WNDCLASSEX wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = wnd_proc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = HD_WINDOW_CLASS;
        if (RegisterClassEx(&wc) == 0) return -1;
        class_registered = 1;
    }

    int slot = -1;
    for (int i = 0; i < HD_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    struct win32_window *win = &windows[slot];
    memset(win, 0, sizeof(*win));
    win->w        = w;
    win->h        = h;
    win->status_h = (flags & HD_CREATE_STATUSBAR) ? HD_STATUS_H : 0;
    win->in_use   = 1;

    if (make_dib(win) != 0) { win->in_use = 0; return -1; }

    /* Size the frame so the client area comes out exactly as asked. */
    RECT r = {0, 0, w, h + win->status_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    win->hwnd = CreateWindowEx(0, HD_WINDOW_CLASS, title ? title : "HolyD",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, r.right - r.left,
                               r.bottom - r.top, NULL, NULL,
                               GetModuleHandle(NULL), NULL);
    if (win->hwnd == NULL) {
        release_dib(win);
        win->in_use = 0;
        return -1;
    }

    /* Set before the first paint, so wnd_proc can find the window. */
    SetWindowLongPtr(win->hwnd, GWLP_USERDATA, (LONG_PTR)(slot + 1));
    draw_status(win);
    ShowWindow(win->hwnd, SW_SHOW);
    UpdateWindow(win->hwnd);
    pump();

    *out_handle = slot + 1;
    surface_of(win, out);
    return 0;
}

static struct win32_window *lookup(int handle) {
    if (handle < 1 || handle > HD_MAX_WINDOWS) return NULL;
    struct win32_window *w = &windows[handle - 1];
    return w->in_use ? w : NULL;
}

int hdp_window_close(int handle) {
    struct win32_window *w = lookup(handle);
    if (w == NULL) return -1;

    if (w->hwnd) {
        SetWindowLongPtr(w->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(w->hwnd);
    }
    release_dib(w);
    memset(w, 0, sizeof(*w));
    pump();
    return 0;
}

int hdp_window_set_title(int handle, const char *title) {
    struct win32_window *w = lookup(handle);
    if (w == NULL) return -1;
    return SetWindowText(w->hwnd, title ? title : "") ? 0 : -1;
}

int hdp_window_set_status(int handle, const char *text) {
    struct win32_window *w = lookup(handle);
    if (w == NULL) return -1;
    if (w->status_h == 0) return 0; /* no strip: the same no-op heimdall is */

    snprintf(w->status, sizeof(w->status), "%s", text ? text : "");
    draw_status(w);
    InvalidateRect(w->hwnd, NULL, FALSE);
    return 0;
}

int hdp_window_present(int handle) {
    struct win32_window *w = lookup(handle);
    if (w == NULL) return -1;
    InvalidateRect(w->hwnd, NULL, FALSE);
    UpdateWindow(w->hwnd);
    return 0;
}

int hdp_poll_event(int handle, struct hdp_event *out) {
    (void)handle; /* one queue, like heimdall's; entries name their window */
    pump();
    return pop_event(out);
}

/* ---------------- Prompts -------------------------------------------------- */

/* A text prompt has no stock Win32 dialog, so this is a small popup with an
 * edit control and its own message loop. The owner is disabled for the
 * duration, which is what makes it modal without a dialog template. */
struct text_prompt {
    HWND  edit;
    char *out;
    size_t cap;
    int   answer;
    int   done;
};

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
    struct text_prompt *p =
        (struct text_prompt *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_COMMAND:
        if (p == NULL) break;
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            if (LOWORD(wparam) == IDOK) {
                if (p->out && p->cap)
                    GetWindowText(p->edit, p->out, (int)p->cap);
                p->answer = HD_PROMPT_OK;
            } else {
                p->answer = HD_PROMPT_CANCEL;
            }
            p->done = 1;
            return 0;
        }
        break;

    case WM_CLOSE:
        if (p) { p->answer = HD_PROMPT_CANCEL; p->done = 1; }
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static int prompt_text(HWND owner, const char *message, char *out,
                       size_t cap) {
    static int registered;
    static const char *cls = "HolyDPrompt";
    if (!registered) {
        WNDCLASSEX wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = prompt_proc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        if (RegisterClassEx(&wc) == 0) return HD_PROMPT_CANCEL;
        registered = 1;
    }

    RECT r = {0, 0, 320, 120};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    HWND dlg = CreateWindowEx(WS_EX_DLGMODALFRAME, cls, "HolyD",
                              WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                              CW_USEDEFAULT, r.right - r.left,
                              r.bottom - r.top, owner, NULL,
                              GetModuleHandle(NULL), NULL);
    if (dlg == NULL) return HD_PROMPT_CANCEL;

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND label = CreateWindowEx(0, "STATIC", message ? message : "",
                                WS_CHILD | WS_VISIBLE, 12, 10, 296, 32, dlg,
                                NULL, NULL, NULL);
    struct text_prompt p;
    memset(&p, 0, sizeof(p));
    p.out = out;
    p.cap = cap;
    p.answer = HD_PROMPT_CANCEL;
    p.edit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                            12, 46, 296, 22, dlg, NULL, NULL, NULL);
    HWND ok = CreateWindowEx(0, "BUTTON", "OK",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                             150, 80, 74, 26, dlg, (HMENU)IDOK, NULL, NULL);
    HWND cancel = CreateWindowEx(0, "BUTTON", "Cancel",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 234, 80, 74, 26, dlg, (HMENU)IDCANCEL, NULL,
                                 NULL);

    SendMessage(label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(p.edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(ok, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(cancel, WM_SETFONT, (WPARAM)font, TRUE);

    SetWindowLongPtr(dlg, GWLP_USERDATA, (LONG_PTR)&p);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(p.edit);

    MSG msg;
    while (!p.done && GetMessage(&msg, NULL, 0, 0) > 0) {
        /* IsDialogMessage gives Tab, Enter and Escape their dialog meanings
         * without a dialog template. */
        if (!IsDialogMessage(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    if (owner) SetForegroundWindow(owner);
    return p.answer;
}

int hdp_prompt(int handle, int kind, const char *message, char *out,
               size_t cap) {
    struct win32_window *w = lookup(handle);
    HWND owner = w ? w->hwnd : NULL;

    if (kind == HD_PROMPT_TEXT) {
        if (out && cap) out[0] = 0;
        return prompt_text(owner, message, out, cap);
    }

    UINT style = kind == HD_PROMPT_CONFIRM ? MB_YESNOCANCEL : MB_OK;
    int answer = MessageBox(owner, message ? message : "", "HolyD", style);
    if (answer == IDOK || answer == IDYES) return HD_PROMPT_OK;
    if (answer == IDNO) return HD_PROMPT_NO;
    return HD_PROMPT_CANCEL;
}

/* ---------------- Time ----------------------------------------------------- */

/* Pumping inside the sleep is not optional: a script spends most of every
 * frame in here, and a window that is not dispatching messages is the one
 * Windows greys out and calls "not responding". */
int hdp_sleep_ms(long long ms) {
    if (ms < 0) ms = 0;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)ms;
    for (;;) {
        pump();
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        ULONGLONG left = deadline - now;
        Sleep(left > 4 ? 4 : (DWORD)left);
    }
    return 0;
}

int hdp_yield(void) {
    pump();
    Sleep(0);
    return 0;
}

/* ---------------- Sockets --------------------------------------------------- */

static int winsock_ready(void) {
    static int started;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        started = 1;
    }
    return 1;
}

int hdp_udp_socket(void) {
    if (!winsock_ready()) return -1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    /* SOCKET is an unsigned handle on Windows, not a small descriptor. It
     * still fits an int in practice, and int is what the language can hold. */
    return s == INVALID_SOCKET ? -1 : (int)s;
}

int hdp_udp_bind(int fd, unsigned port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    return bind((SOCKET)fd, (struct sockaddr *)&addr, sizeof(addr)) ==
                   SOCKET_ERROR
               ? -1
               : 0;
}

int hdp_udp_send(int fd, const void *buf, int len,
                 const unsigned char ip[4], unsigned port) {
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((unsigned short)port);
    memcpy(&dest.sin_addr.s_addr, ip, 4);

    int sent = sendto((SOCKET)fd, (const char *)buf, len, 0,
                      (struct sockaddr *)&dest, sizeof(dest));
    return sent == SOCKET_ERROR ? -1 : sent;
}

int hdp_udp_recv(int fd, void *buf, int cap) {
    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    int src_len = sizeof(src);

    int bytes = recvfrom((SOCKET)fd, (char *)buf, cap, 0,
                         (struct sockaddr *)&src, &src_len);
    if (bytes == SOCKET_ERROR) {
        printf("[UDP] recvfrom failed: fd=%d error=%d\n", fd, WSAGetLastError());
        return -1;
    }
    return bytes;
}
