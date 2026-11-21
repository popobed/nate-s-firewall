/*
 * karo_window.h
 *
 * KARO's floating overlay: a borderless, always-on-top, semi-transparent
 * Win32 window (WS_EX_TOPMOST | WS_EX_LAYERED), draggable anywhere on its
 * own background, with a system-prompt box, a query input, an explicit
 * "Attach Screen" button, a "Send" button, and a "Close" (hide) button.
 *
 * ============================== Honesty notes, up front =====================
 *   - Drag & Drop repositioning: WM_NCHITTEST is remapped from HTCLIENT to
 *     HTCAPTION over the window's own background, the standard Win32
 *     technique for making a borderless custom-chrome window draggable
 *     without a real caption bar. Windows itself still routes clicks over
 *     a CHILD control (the edit boxes/buttons) directly to that child --
 *     this file's WM_NCHITTEST handler is never even consulted for those
 *     areas, so the input field and buttons keep working normally.
 *   - This file owns UI ONLY. It never itself performs a screen capture
 *     or an AI request -- "Attach Screen" and "Send" only invoke the
 *     caller-supplied `on_attach`/`on_send` callbacks; karo_daemon.c wires
 *     those to karo_screen_capture.h and karo_ai_router.h respectively.
 *     Both callbacks fire ONLY on an explicit click -- there is no timer,
 *     no polling, nothing automatic in this file.
 */

#ifndef KARO_WINDOW_H
#define KARO_WINDOW_H

#if defined(_WIN32)

#include <stdbool.h>
#include <stddef.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define KARO_WINDOW_INPUT_MAX 256u

typedef void (*karo_window_send_fn)(void *user_data, const char *query_text);
typedef void (*karo_window_attach_fn)(void *user_data);

typedef struct {
    HWND hwnd;
    HWND edit_system_prompt;
    HWND edit_query;
    HWND edit_response;
    HWND btn_attach;
    HWND btn_send;
    HWND btn_close;
    bool created;
    bool visible;
    karo_window_send_fn   on_send;
    karo_window_attach_fn on_attach;
    void *user_data;
} karo_window_t;

typedef enum {
    KARO_WINDOW_OK = 0,
    KARO_WINDOW_ERR_INVALID = -1,
    KARO_WINDOW_ERR_CLASS_REGISTER_FAILED = -2,
    KARO_WINDOW_ERR_CREATE_FAILED = -3
} karo_window_status_t;

/*
 * Creates the (initially hidden) overlay window and its child controls.
 * `on_send`/`on_attach` are invoked from the window's own message
 * handling on a Send/Attach-Screen click (may be NULL to no-op that
 * button). Registers the window class once per process (idempotent).
 */
karo_window_status_t karo_window_create(karo_window_t *win, HINSTANCE hinst,
                                         karo_window_send_fn on_send, karo_window_attach_fn on_attach,
                                         void *user_data);

void karo_window_show(karo_window_t *win);
void karo_window_hide(karo_window_t *win);
void karo_window_toggle(karo_window_t *win); /* what the global show/hide hotkey calls */
void karo_window_destroy(karo_window_t *win);

/* O(1) bounded copy (out_cap includes the terminating NUL; truncates,
 * never overflows) -- reads/writes the system-prompt edit control text
 * directly, no separate storage. */
void karo_window_get_system_prompt(const karo_window_t *win, char *out, size_t out_cap);
void karo_window_set_system_prompt(karo_window_t *win, const char *text);

/* Sets the read-only "Response" box's text -- what karo_daemon.c calls
 * once karo_bridge_poll_response() (karo_bridge.h) reports a new answer
 * from karo_vision_bridge.py. This file never calls that poll itself. */
void karo_window_set_response_text(karo_window_t *win, const char *text);

#endif /* defined(_WIN32) */

#endif /* KARO_WINDOW_H */
