/* userspace/bin/holyd/ffi.c , every native a HolyD script can call.
 *
 * Nothing in this file talks to an operating system. Windows arrive as a
 * pixel buffer from ffi_platform.h's host, and the drawing natives are
 * lib/gfx.c over that buffer, which is pure arithmetic , so the same code
 * serves heimdall on TOS and a DIB section on Windows. Sockets, input and
 * sleeping are the host's too. See ffi_tos.c and ffi_win32.c.
 *
 * Two conventions run through the natives:
 *
 *   , Arguments past the ones a call needs are optional and have defaults,
 *     so WinDrawText(w, x, y, s) works and the colour and scale can be added
 *     when a caller cares.
 *   , A failure is -1 rather than an abort. A script has no exceptions and
 *     no way to inspect an error, so the useful thing is a value it can
 *     compare against zero.
 */
#include "ffi.h"
#include "compiler.h"
#include "ffi_platform.h"

#include "platform/gfx.h"
#include "platform/key_codes.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward declarations
static HDValue native_udp_socket(int arg_count, HDValue *args);
static HDValue native_udp_bind(int arg_count, HDValue *args);
static HDValue native_udp_send(int arg_count, HDValue *args);
static HDValue native_udp_recv(int arg_count, HDValue *args);

static HDValue native_win_create(int arg_count, HDValue *args);
static HDValue native_win_destroy(int arg_count, HDValue *args);
static HDValue native_win_set_title(int arg_count, HDValue *args);
static HDValue native_win_set_status(int arg_count, HDValue *args);
static HDValue native_win_invalidate(int arg_count, HDValue *args);
static HDValue native_win_width(int arg_count, HDValue *args);
static HDValue native_win_height(int arg_count, HDValue *args);
static HDValue native_win_prompt(int arg_count, HDValue *args);
static HDValue native_win_prompt_text(int arg_count, HDValue *args);

static HDValue native_win_clear(int arg_count, HDValue *args);
static HDValue native_win_fill_rect(int arg_count, HDValue *args);
static HDValue native_win_draw_rect(int arg_count, HDValue *args);
static HDValue native_win_bevel(int arg_count, HDValue *args);
static HDValue native_win_draw_pixel(int arg_count, HDValue *args);
static HDValue native_win_hline(int arg_count, HDValue *args);
static HDValue native_win_vline(int arg_count, HDValue *args);
static HDValue native_win_draw_text(int arg_count, HDValue *args);
static HDValue native_win_text_width(int arg_count, HDValue *args);

static HDValue native_win_poll_event(int arg_count, HDValue *args);
static HDValue native_win_event_key(int arg_count, HDValue *args);
static HDValue native_win_event_x(int arg_count, HDValue *args);
static HDValue native_win_event_y(int arg_count, HDValue *args);
static HDValue native_win_event_w(int arg_count, HDValue *args);
static HDValue native_win_event_h(int arg_count, HDValue *args);

static HDValue native_str(int arg_count, HDValue *args);
static HDValue native_rgb(int arg_count, HDValue *args);
static HDValue native_sleep(int arg_count, HDValue *args);
static HDValue native_yield(int arg_count, HDValue *args);

// Private array mapping string names to C functions
static struct NativeDef {
  const char *name;
  NativeFn fn;
} native_functions[] = {
    /* Networking. */
    {"UdpReceive", native_udp_recv},
    {"UdpSocket", native_udp_socket},
    {"UdpBind", native_udp_bind},
    {"UdpSend", native_udp_send},

    /* Window lifecycle. */
    {"WinCreate", native_win_create},
    {"WinDestroy", native_win_destroy},
    {"WinSetTitle", native_win_set_title},
    {"WinSetStatus", native_win_set_status},
    {"WinInvalidate", native_win_invalidate},
    /* Same call under the name a script that thinks in frames reaches for
     * first. The host owns compositing, so "present this window" and "this
     * window's pixels changed" are the same request. */
    {"WinPresent", native_win_invalidate},
    {"WinWidth", native_win_width},
    {"WinHeight", native_win_height},
    {"WinPrompt", native_win_prompt},
    {"WinPromptText", native_win_prompt_text},

    /* Drawing. */
    {"WinClear", native_win_clear},
    {"WinFillRect", native_win_fill_rect},
    {"WinDrawRect", native_win_draw_rect},
    {"WinBevel", native_win_bevel},
    {"WinDrawPixel", native_win_draw_pixel},
    {"WinHLine", native_win_hline},
    {"WinVLine", native_win_vline},
    {"WinDrawText", native_win_draw_text},
    {"WinTextWidth", native_win_text_width},

    /* Events. */
    {"WinPollEvent", native_win_poll_event},
    {"WinEventKey", native_win_event_key},
    {"WinEventX", native_win_event_x},
    {"WinEventY", native_win_event_y},
    {"WinEventW", native_win_event_w},
    {"WinEventH", native_win_event_h},

    /* Odds and ends a GUI loop cannot do without. */
    {"Str", native_str},
    {"Rgb", native_rgb},
    {"Sleep", native_sleep},
    {"Yield", native_yield},

    {NULL, NULL}};

// Public lookup function
NativeFn ffi_lookup_native(const char *name, int len) {
  for (int i = 0; native_functions[i].name != NULL; i++) {
    int nlen = (int)strlen(native_functions[i].name);
    if (nlen == len && strncmp(name, native_functions[i].name, len) == 0) {
      return native_functions[i].fn;
    }
  }
  return NULL; // Not found
}

/* ---------------- Argument helpers --------------------------------------- */

/* HolyD strings are a pointer and a length into the source buffer, so they
 * are not NUL-terminated. Anything handed to a C API is copied first. */
static const char *cstr(HDValue value, char *buf, size_t cap) {
  if (cap == 0)
    return "";

  size_t n = 0;
  if (value.type == VAL_STRING && value.str_len > 0) {
    n = (size_t)value.str_len;
    if (n > cap - 1)
      n = cap - 1;
    memcpy(buf, value.str, n);
  }
  buf[n] = 0;
  return buf;
}

/* Argument `index` as an integer, or `fallback` when the script left it
 * off. Trailing optional arguments are how the drawing natives keep their
 * common form short: WinDrawText(w, x, y, s) with the colour implied. */
static long long arg_int(int arg_count, HDValue *args, int index,
                         long long fallback) {
  if (index >= arg_count)
    return fallback;
  if (args[index].type == VAL_FLOAT)
    return (long long)args[index].f64;
  return args[index].i64;
}

/* ---------------- Networking ---------------------------------------------- */

static HDValue native_udp_socket(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(hdp_udp_socket());
}

// UdpBind(I64 fd, I64 port) -> I64 result
static HDValue native_udp_bind(int arg_count, HDValue *args) {
  if (arg_count != 2)
    return int_value(-1);
  return int_value(hdp_udp_bind((int)args[0].i64,
                                (unsigned)arg_int(arg_count, args, 1, 0)));
}

// UdpSend(I64 fd, U8 *data, I64 ip[4], I64 port) -> I64 bytes sent
static HDValue native_udp_send(int arg_count, HDValue *args) {
  if (arg_count != 4)
    return int_value(-1);

  unsigned char ip[4] = {0, 0, 0, 0};
  if (args[2].type == VAL_ARRAY && args[2].array_len == 4) {
    for (int i = 0; i < 4; i++) {
      ip[i] = (unsigned char)args[2].elements[i].i64;
    }
  }

  return int_value(hdp_udp_send((int)args[0].i64, args[1].str, args[1].str_len,
                                ip, (unsigned)arg_int(arg_count, args, 3, 0)));
}

/* UdpReceive(I64 fd) -> U8 *datagram
 * Blocks. An empty string means the read failed, which a script can test
 * without a second return value. */
static HDValue native_udp_recv(int arg_count, HDValue *args) {
  if (arg_count != 1)
    return int_value(-1);

  char buf[1514];
  int bytes = hdp_udp_recv((int)args[0].i64, buf, (int)sizeof(buf));
  if (bytes < 0)
    return string_value("", 0);

  /* HDValue borrows its bytes, so the datagram has to outlive this frame. */
  char *heap_buf = malloc((size_t)bytes + 1);
  if (heap_buf == NULL)
    return string_value("", 0);

  memcpy(heap_buf, buf, (size_t)bytes);
  heap_buf[bytes] = 0;
  return string_value(heap_buf, bytes);
}

/* ---------------- Windowing ----------------------------------------------
 *
 * A script sees a window as an I64 handle and draws into it through the
 * Win* natives; the host's buffer and the gfx surface over it stay here.
 * Handles run 1..HD_MAX_WINDOWS and the table is keyed by handle - 1, so a
 * stale handle is a lookup miss rather than a write into another window.
 */

struct hd_window {
  int                in_use;
  struct gfx_surface surf;
};

static struct hd_window windows[HD_MAX_WINDOWS];
static int              window_count;

/* The event WinPollEvent last returned. HolyD has no structs, so its
 * fields are read back through WinEventKey/X/Y/W/H rather than returned. */
static struct hdp_event last_event;

static struct hd_window *win_by_handle(long long handle) {
  if (handle < 1 || handle > HD_MAX_WINDOWS)
    return NULL;
  struct hd_window *slot = &windows[handle - 1];
  return slot->in_use ? slot : NULL;
}

static struct hd_window *win_slot(HDValue value) {
  if (value.type != VAL_INT)
    return NULL;
  return win_by_handle(value.i64);
}

/* Every Win* native takes the handle first, so this is the one lookup. */
static struct hd_window *win_arg(int arg_count, HDValue *args) {
  return arg_count >= 1 ? win_slot(args[0]) : NULL;
}

/* The single open window, or NULL when there is more than one. It is the
 * last resort for attributing a resize on a host that cannot name the
 * window the event belongs to. */
static struct hd_window *win_only(void) {
  if (window_count != 1)
    return NULL;
  for (int i = 0; i < HD_MAX_WINDOWS; i++) {
    if (windows[i].in_use)
      return &windows[i];
  }
  return NULL;
}

static void bind_surface(struct hd_window *slot,
                         const struct hdp_surface *from) {
  gfx_surface_init(&slot->surf, from->pixels, from->w, from->h, from->stride);
}

/* Four consecutive arguments as x, y, w, h. */
static struct gfx_rect rect_args(int arg_count, HDValue *args, int first) {
  return gfx_rect_make((int)arg_int(arg_count, args, first + 0, 0),
                       (int)arg_int(arg_count, args, first + 1, 0),
                       (int)arg_int(arg_count, args, first + 2, 0),
                       (int)arg_int(arg_count, args, first + 3, 0));
}

#define WIN_DEFAULT_FG 0x00FFFFFFu

/* ---------------- Lifecycle ---------------------------------------------- */

/* WinCreate(I64 w, I64 h [, U8 *title [, I64 flags]]) -> I64 handle
 *
 * Returns -1 when the host has no window server or refuses the request, so
 * a script can test the handle before drawing. `flags` takes WIN_STATUSBAR. */
static HDValue native_win_create(int arg_count, HDValue *args) {
  if (arg_count < 2 || arg_count > 4)
    return int_value(-1);

  int w = (int)arg_int(arg_count, args, 0, 0);
  int h = (int)arg_int(arg_count, args, 1, 0);
  if (w <= 0 || h <= 0)
    return int_value(-1);

  char title[64];
  if (arg_count >= 3) {
    cstr(args[2], title, sizeof(title));
  } else {
    memcpy(title, "HolyD", sizeof("HolyD"));
  }

  int handle = -1;
  struct hdp_surface surface;
  if (hdp_window_open(w, h, title, (unsigned)arg_int(arg_count, args, 3, 0),
                      &handle, &surface) != 0) {
    return int_value(-1);
  }

  if (handle < 1 || handle > HD_MAX_WINDOWS) {
    /* A host that ignores its own handle range would corrupt the table.
     * Give the window straight back rather than tracking it wrong. */
    printf("holyd: host returned handle %d, outside 1..%d\n", handle,
           HD_MAX_WINDOWS);
    hdp_window_close(handle);
    return int_value(-1);
  }

  struct hd_window *slot = &windows[handle - 1];
  if (!slot->in_use)
    window_count++;
  slot->in_use = 1;
  bind_surface(slot, &surface);

  return int_value((long long)handle);
}

// WinDestroy(I64 handle) -> I64 result
static HDValue native_win_destroy(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot)
    return int_value(-1);

  int result = hdp_window_close((int)args[0].i64);
  slot->in_use = 0;
  window_count--;
  return int_value(result);
}

// WinSetTitle(I64 handle, U8 *title) -> I64 result
static HDValue native_win_set_title(int arg_count, HDValue *args) {
  if (!win_arg(arg_count, args) || arg_count != 2)
    return int_value(-1);

  char title[64];
  return int_value(hdp_window_set_title((int)args[0].i64,
                                        cstr(args[1], title, sizeof(title))));
}

/* WinSetStatus(I64 handle, U8 *text) -> I64 result
 * A no-op unless the window was created with WIN_STATUSBAR. */
static HDValue native_win_set_status(int arg_count, HDValue *args) {
  if (!win_arg(arg_count, args) || arg_count != 2)
    return int_value(-1);

  char text[64];
  return int_value(hdp_window_set_status((int)args[0].i64,
                                         cstr(args[1], text, sizeof(text))));
}

// WinInvalidate(I64 handle) -> I64 result. Also registered as WinPresent.
static HDValue native_win_invalidate(int arg_count, HDValue *args) {
  if (!win_arg(arg_count, args))
    return int_value(-1);
  return int_value(hdp_window_present((int)args[0].i64));
}

/* WinWidth / WinHeight report the client area, which is what the drawing
 * calls are measured in. Both follow a resize. */
static HDValue native_win_width(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  return int_value(slot ? slot->surf.w : -1);
}

static HDValue native_win_height(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  return int_value(slot ? slot->surf.h : -1);
}

/* WinPrompt(I64 handle, I64 kind, U8 *message) -> I64 answer
 *
 * Blocks until the user answers. `kind` is PROMPT_MESSAGE or
 * PROMPT_CONFIRM; the answer is PROMPT_OK, PROMPT_NO or PROMPT_CANCEL. */
static HDValue native_win_prompt(int arg_count, HDValue *args) {
  if (!win_arg(arg_count, args) || arg_count != 3)
    return int_value(HD_PROMPT_CANCEL);

  char message[192];
  cstr(args[2], message, sizeof(message));

  int kind = (int)arg_int(arg_count, args, 1, HD_PROMPT_MESSAGE);
  return int_value(hdp_prompt((int)args[0].i64, kind, message, NULL, 0));
}

/* WinPromptText(I64 handle, U8 *message) -> U8 *reply
 *
 * The text the user typed, or the empty string if they cancelled , so the
 * caller only has to test for emptiness rather than juggle two returns. */
static HDValue native_win_prompt_text(int arg_count, HDValue *args) {
  if (!win_arg(arg_count, args) || arg_count != 2)
    return string_value("", 0);

  char message[192];
  cstr(args[1], message, sizeof(message));

  char reply[128];
  reply[0] = 0;
  if (hdp_prompt((int)args[0].i64, HD_PROMPT_TEXT, message, reply,
                 sizeof(reply)) != HD_PROMPT_OK) {
    return string_value("", 0);
  }

  /* HDValue borrows its bytes, so the reply has to outlive this frame. */
  size_t len = strlen(reply);
  char *heap = malloc(len + 1);
  if (heap == NULL)
    return string_value("", 0);
  memcpy(heap, reply, len + 1);
  return string_value(heap, (int)len);
}

/* ---------------- Drawing -------------------------------------------------
 *
 * Nothing here reaches the host: these write into the shared surface, and
 * the pixels only appear on screen at the next WinInvalidate. Colours are
 * 0x00RRGGBB, which the lexer's hex literals write directly, or Rgb().
 * Every gfx call clips, so coordinates outside the window are harmless.
 */

// WinClear(I64 handle, I64 color) -> I64 result
static HDValue native_win_clear(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot)
    return int_value(-1);

  gfx_clear(&slot->surf, (uint32_t)arg_int(arg_count, args, 1, 0));
  return int_value(0);
}

// WinFillRect(I64 handle, I64 x, I64 y, I64 w, I64 h, I64 color) -> I64 result
static HDValue native_win_fill_rect(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 6)
    return int_value(-1);

  gfx_fill(&slot->surf, rect_args(arg_count, args, 1),
           (uint32_t)arg_int(arg_count, args, 5, WIN_DEFAULT_FG));
  return int_value(0);
}

/* WinDrawRect(I64 handle, I64 x, I64 y, I64 w, I64 h, I64 color
 *             [, I64 thickness]) -> I64 result
 * The outline of the rectangle, drawn inside it. WinFillRect fills. */
static HDValue native_win_draw_rect(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 6)
    return int_value(-1);

  gfx_frame(&slot->surf, rect_args(arg_count, args, 1),
            (uint32_t)arg_int(arg_count, args, 5, WIN_DEFAULT_FG),
            (int)arg_int(arg_count, args, 6, 1));
  return int_value(0);
}

/* WinBevel(I64 handle, I64 x, I64 y, I64 w, I64 h, I64 light, I64 dark
 *          [, I64 thickness]) -> I64 result
 * Swap light and dark for a pressed-in look. */
static HDValue native_win_bevel(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 7)
    return int_value(-1);

  gfx_bevel(&slot->surf, rect_args(arg_count, args, 1),
            (uint32_t)arg_int(arg_count, args, 5, WIN_DEFAULT_FG),
            (uint32_t)arg_int(arg_count, args, 6, 0),
            (int)arg_int(arg_count, args, 7, 1));
  return int_value(0);
}

// WinDrawPixel(I64 handle, I64 x, I64 y, I64 color) -> I64 result
static HDValue native_win_draw_pixel(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 4)
    return int_value(-1);

  gfx_pixel(&slot->surf, (int)arg_int(arg_count, args, 1, 0),
            (int)arg_int(arg_count, args, 2, 0),
            (uint32_t)arg_int(arg_count, args, 3, WIN_DEFAULT_FG));
  return int_value(0);
}

// WinHLine(I64 handle, I64 x, I64 y, I64 w, I64 color) -> I64 result
static HDValue native_win_hline(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 5)
    return int_value(-1);

  gfx_hline(&slot->surf, (int)arg_int(arg_count, args, 1, 0),
            (int)arg_int(arg_count, args, 2, 0),
            (int)arg_int(arg_count, args, 3, 0),
            (uint32_t)arg_int(arg_count, args, 4, WIN_DEFAULT_FG));
  return int_value(0);
}

// WinVLine(I64 handle, I64 x, I64 y, I64 h, I64 color) -> I64 result
static HDValue native_win_vline(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 5)
    return int_value(-1);

  gfx_vline(&slot->surf, (int)arg_int(arg_count, args, 1, 0),
            (int)arg_int(arg_count, args, 2, 0),
            (int)arg_int(arg_count, args, 3, 0),
            (uint32_t)arg_int(arg_count, args, 4, WIN_DEFAULT_FG));
  return int_value(0);
}

/* WinDrawText(I64 handle, I64 x, I64 y, U8 *text [, I64 color [, I64 scale]])
 *     -> I64 result
 *
 * font8x8, so a cell is 8x8 pixels before `scale` magnifies it. There is no
 * wrapping and no newline handling: one call draws one line. */
static HDValue native_win_draw_text(int arg_count, HDValue *args) {
  struct hd_window *slot = win_arg(arg_count, args);
  if (!slot || arg_count < 4)
    return int_value(-1);

  char text[256];
  cstr(args[3], text, sizeof(text));

  int scale = (int)arg_int(arg_count, args, 5, 1);
  if (scale < 1)
    scale = 1;

  gfx_text(&slot->surf, (int)arg_int(arg_count, args, 1, 0),
           (int)arg_int(arg_count, args, 2, 0), text,
           (uint32_t)arg_int(arg_count, args, 4, WIN_DEFAULT_FG), scale);
  return int_value(0);
}

/* WinTextWidth(U8 *text [, I64 scale]) -> I64 pixels
 * Needs no window: font8x8 is fixed width. Use it to centre a label. */
static HDValue native_win_text_width(int arg_count, HDValue *args) {
  if (arg_count < 1)
    return int_value(0);

  char text[256];
  cstr(args[0], text, sizeof(text));

  int scale = (int)arg_int(arg_count, args, 1, 1);
  if (scale < 1)
    scale = 1;

  int w = 0;
  gfx_text_size(text, scale, &w, NULL);
  return int_value(w);
}

/* ---------------- Events --------------------------------------------------
 *
 * WinPollEvent returns the event code and stashes the rest; the fields come
 * back through the WinEvent* readers. Splitting it that way is what lets a
 * language with no structs still see a mouse position.
 */

/* WinPollEvent([I64 handle]) -> I64 event, EV_NONE when nothing is queued.
 *
 * The handle is only needed for EV_RESIZE, and only on a host whose resize
 * notification cannot name the window it belongs to , heimdall's cannot. The
 * window is picked in that order: what the event says, then what the caller
 * named, then the only open window. If none of those resolve, the event is
 * still reported and the surface keeps pointing at the old backing, so
 * drawing into it will not be seen. */
static HDValue native_win_poll_event(int arg_count, HDValue *args) {
  struct hdp_event ev;
  if (!hdp_poll_event(arg_count >= 1 ? (int)args[0].i64 : 0, &ev)) {
    last_event.type = HD_EV_NONE;
    return int_value(HD_EV_NONE);
  }
  last_event = ev;

  if (ev.type == HD_EV_RESIZE) {
    struct hd_window *slot = win_by_handle(ev.window);
    if (slot == NULL && arg_count >= 1)
      slot = win_slot(args[0]);
    if (slot == NULL)
      slot = win_only();
    if (slot != NULL)
      bind_surface(slot, &ev.surface);
  }

  return int_value(ev.type);
}

/* Keycode on EV_KEY_DOWN/EV_KEY_UP, button mask on EV_MOUSE_DOWN/UP. */
static HDValue native_win_event_key(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(last_event.param);
}

static HDValue native_win_event_x(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(last_event.x);
}

static HDValue native_win_event_y(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(last_event.y);
}

/* The new client size, on EV_RESIZE. Zero on every other event. */
static HDValue native_win_event_w(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(last_event.type == HD_EV_RESIZE ? last_event.surface.w : 0);
}

static HDValue native_win_event_h(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(last_event.type == HD_EV_RESIZE ? last_event.surface.h : 0);
}

/* ---------------- Odds and ends ------------------------------------------ */

/* Str(I64 value) -> U8 *text
 *
 * The bridge from a number to something drawable: WinDrawText takes a
 * string, `~` joins two strings, and nothing else in the language turns an
 * I64 into one. A string argument passes straight through, so Str() is safe
 * to wrap anything in. */
static HDValue native_str(int arg_count, HDValue *args) {
  if (arg_count < 1)
    return string_value("", 0);
  if (args[0].type == VAL_STRING)
    return args[0];

  long long value = arg_int(arg_count, args, 0, 0);
  int negative = value < 0;

  /* Negating through the unsigned side: -LLONG_MIN is not representable. */
  unsigned long long u = negative ? (unsigned long long)(-(value + 1)) + 1ULL
                                  : (unsigned long long)value;

  char digits[24];
  int n = 0;
  do {
    digits[n++] = (char)('0' + (int)(u % 10));
    u /= 10;
  } while (u != 0);
  if (negative)
    digits[n++] = '-';

  /* HDValue borrows its bytes, so this has to outlive the call. */
  char *out = malloc((size_t)n + 1);
  if (out == NULL)
    return string_value("", 0);
  for (int i = 0; i < n; i++)
    out[i] = digits[n - 1 - i];
  out[n] = 0;
  return string_value(out, n);
}

/* Rgb(I64 r, I64 g, I64 b) -> I64 color
 * Components are clamped to 0..255, so arithmetic that overshoots a fade
 * saturates instead of wrapping into a different hue. */
static HDValue native_rgb(int arg_count, HDValue *args) {
  long long c[3];
  for (int i = 0; i < 3; i++) {
    c[i] = arg_int(arg_count, args, i, 0);
    if (c[i] < 0)
      c[i] = 0;
    if (c[i] > 255)
      c[i] = 255;
  }
  return int_value((c[0] << 16) | (c[1] << 8) | c[2]);
}

/* Sleep(I64 ms) -> I64 result
 *
 * A frame loop needs this: without it a `while (true)` body spins, and on
 * either host that starves the thing that has to composite what was drawn.
 * How closely the wait matches the request is the host's business , on TOS
 * it currently overshoots badly. Treat it as a lower bound. */
static HDValue native_sleep(int arg_count, HDValue *args) {
  return int_value(hdp_sleep_ms(arg_int(arg_count, args, 0, 0)));
}

/* Yield() -> I64 result. Give up the rest of this timeslice. */
static HDValue native_yield(int arg_count, HDValue *args) {
  (void)arg_count;
  (void)args;
  return int_value(hdp_yield());
}

/* ---------------- Script-visible constants --------------------------------
 *
 * HolyD has no enums and no preprocessor, so the names a windowing script
 * needs , event codes, keycodes, prompt kinds , arrive as ordinary I64
 * globals defined before the program runs. A script is free to assign over
 * them; nothing here reads them back.
 */
static const struct {
  const char *name;
  long long   value;
} ffi_constants[] = {
    /* WinPollEvent results. */
    {"EV_NONE", HD_EV_NONE},
    {"EV_KEY_DOWN", HD_EV_KEY_DOWN},
    {"EV_KEY_UP", HD_EV_KEY_UP},
    {"EV_MOUSE_MOVE", HD_EV_MOUSE_MOVE},
    {"EV_MOUSE_DOWN", HD_EV_MOUSE_DOWN},
    {"EV_MOUSE_UP", HD_EV_MOUSE_UP},
    {"EV_RESIZE", HD_EV_RESIZE},
    {"EV_QUIT", HD_EV_QUIT},

    /* WinEventKey() on a mouse event. */
    {"MOUSE_LEFT", HD_MOUSE_LEFT},
    {"MOUSE_RIGHT", HD_MOUSE_RIGHT},
    {"MOUSE_MIDDLE", HD_MOUSE_MIDDLE},

    /* WinCreate flags, and the WinPrompt vocabulary. */
    {"WIN_STATUSBAR", HD_CREATE_STATUSBAR},
    {"PROMPT_MESSAGE", HD_PROMPT_MESSAGE},
    {"PROMPT_CONFIRM", HD_PROMPT_CONFIRM},
    {"PROMPT_CANCEL", HD_PROMPT_CANCEL},
    {"PROMPT_OK", HD_PROMPT_OK},
    {"PROMPT_NO", HD_PROMPT_NO},

    /* Enough of a palette to draw something without reaching for hex. */
    {"COLOR_BLACK", 0x00000000},
    {"COLOR_WHITE", 0x00FFFFFF},
    {"COLOR_RED", 0x00FF0000},
    {"COLOR_GREEN", 0x0000FF00},
    {"COLOR_BLUE", 0x000000FF},
    {"COLOR_YELLOW", 0x00FFFF00},
    {"COLOR_CYAN", 0x0000FFFF},
    {"COLOR_MAGENTA", 0x00FF00FF},
    {"COLOR_GRAY", 0x00808080},
    {"COLOR_DARK_GRAY", 0x00404040},

    /* Keycodes. The letters and digits are here because a script that
     * reacts to the keyboard needs them by name; the rest of
     * include/key_codes.h is a raw number away. */
    {"KEY_ESC", KEY_ESC},
    {"KEY_ENTER", KEY_ENTER},
    {"KEY_SPACE", KEY_SPACE},
    {"KEY_TAB", KEY_TAB},
    {"KEY_BACKSPACE", KEY_BACKSPACE},
    {"KEY_UP", KEY_UP},
    {"KEY_DOWN", KEY_DOWN},
    {"KEY_LEFT", KEY_LEFT},
    {"KEY_RIGHT", KEY_RIGHT},
    {"KEY_A", KEY_A}, {"KEY_B", KEY_B}, {"KEY_C", KEY_C}, {"KEY_D", KEY_D},
    {"KEY_E", KEY_E}, {"KEY_F", KEY_F}, {"KEY_G", KEY_G}, {"KEY_H", KEY_H},
    {"KEY_I", KEY_I}, {"KEY_J", KEY_J}, {"KEY_K", KEY_K}, {"KEY_L", KEY_L},
    {"KEY_M", KEY_M}, {"KEY_N", KEY_N}, {"KEY_O", KEY_O}, {"KEY_P", KEY_P},
    {"KEY_Q", KEY_Q}, {"KEY_R", KEY_R}, {"KEY_S", KEY_S}, {"KEY_T", KEY_T},
    {"KEY_U", KEY_U}, {"KEY_V", KEY_V}, {"KEY_W", KEY_W}, {"KEY_X", KEY_X},
    {"KEY_Y", KEY_Y}, {"KEY_Z", KEY_Z},
    {"KEY_0", KEY_0}, {"KEY_1", KEY_1}, {"KEY_2", KEY_2}, {"KEY_3", KEY_3},
    {"KEY_4", KEY_4}, {"KEY_5", KEY_5}, {"KEY_6", KEY_6}, {"KEY_7", KEY_7},
    {"KEY_8", KEY_8}, {"KEY_9", KEY_9},

    {NULL, 0},
};

void ffi_define_globals(Environment *env) {
  if (env == NULL)
    return;
  for (int i = 0; ffi_constants[i].name != NULL; i++) {
    EnvDefine(env, ffi_constants[i].name, strlen(ffi_constants[i].name),
              int_value(ffi_constants[i].value));
  }
}
