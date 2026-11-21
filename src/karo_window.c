/*
 * karo_window.c
 *
 * See include/karo_window.h for the full design and honest-scope notes.
 */

#include "karo_window.h"

#if defined(_WIN32)

#include <string.h>

#define ID_EDIT_PROMPT   1001
#define ID_EDIT_QUERY    1002
#define ID_BTN_ATTACH    1003
#define ID_BTN_SEND      1004
#define ID_BTN_CLOSE     1005
#define ID_EDIT_RESPONSE 1006

static const char *KARO_WINDOW_CLASS_NAME = "KaroOverlayWindowClass";
static ATOM g_wndclass_atom = 0;

static LRESULT CALLBACK karo_window_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    karo_window_t *win = (karo_window_t *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_NCHITTEST: {
        /* Remap the window's own background to HTCAPTION so it can be
         * dragged anywhere -- see the file header. Children (edit boxes,
         * buttons) never reach this: Windows routes their hit-testing to
         * the child HWND directly. */
        LRESULT hit = DefWindowProc(hwnd, msg, wp, lp);
        return (hit == HTCLIENT) ? (LRESULT)HTCAPTION : hit;
    }
    case WM_COMMAND:
        if (win != NULL && HIWORD(wp) == BN_CLICKED) {
            switch (LOWORD(wp)) {
            case ID_BTN_CLOSE:
                ShowWindow(hwnd, SW_HIDE);
                win->visible = false;
                return 0;
            case ID_BTN_ATTACH:
                if (win->on_attach != NULL) {
                    win->on_attach(win->user_data);
                }
                return 0;
            case ID_BTN_SEND: {
                char buf[KARO_WINDOW_INPUT_MAX];
                GetWindowText(win->edit_query, buf, (int)sizeof buf);
                if (win->on_send != NULL) {
                    win->on_send(win->user_data, buf);
                }
                SetWindowText(win->edit_query, "");
                return 0;
            }
            default:
                break;
            }
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        if (win != NULL) {
            win->visible = false;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

karo_window_status_t karo_window_create(karo_window_t *win, HINSTANCE hinst,
                                         karo_window_send_fn on_send, karo_window_attach_fn on_attach,
                                         void *user_data) {
    if (win == NULL) {
        return KARO_WINDOW_ERR_INVALID;
    }
    memset(win, 0, sizeof *win);
    win->on_send = on_send;
    win->on_attach = on_attach;
    win->user_data = user_data;

    if (g_wndclass_atom == 0) {
        WNDCLASSEX wc;
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = karo_window_wndproc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = KARO_WINDOW_CLASS_NAME;
        g_wndclass_atom = RegisterClassEx(&wc);
        if (g_wndclass_atom == 0) {
            return KARO_WINDOW_ERR_CLASS_REGISTER_FAILED;
        }
    }

    HWND hwnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                                KARO_WINDOW_CLASS_NAME, "KARO",
                                WS_POPUP | WS_BORDER,
                                100, 100, 420, 400,
                                NULL, NULL, hinst, win);
    if (hwnd == NULL) {
        return KARO_WINDOW_ERR_CREATE_FAILED;
    }
    win->hwnd = hwnd;
    SetLayeredWindowAttributes(hwnd, 0, 235, LWA_ALPHA);

    win->edit_system_prompt = CreateWindowEx(0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
        10, 10, 400, 80, hwnd, (HMENU)(UINT_PTR)ID_EDIT_PROMPT, hinst, NULL);

    win->edit_query = CreateWindowEx(0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_BORDER,
        10, 100, 300, 24, hwnd, (HMENU)(UINT_PTR)ID_EDIT_QUERY, hinst, NULL);

    win->btn_attach = CreateWindowEx(0, "BUTTON", "Attach Screen",
        WS_CHILD | WS_VISIBLE, 10, 135, 130, 26, hwnd, (HMENU)(UINT_PTR)ID_BTN_ATTACH, hinst, NULL);

    win->btn_send = CreateWindowEx(0, "BUTTON", "Send",
        WS_CHILD | WS_VISIBLE, 150, 135, 90, 26, hwnd, (HMENU)(UINT_PTR)ID_BTN_SEND, hinst, NULL);

    win->btn_close = CreateWindowEx(0, "BUTTON", "Close",
        WS_CHILD | WS_VISIBLE, 320, 10, 80, 24, hwnd, (HMENU)(UINT_PTR)ID_BTN_CLOSE, hinst, NULL);

    win->edit_response = CreateWindowEx(0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        10, 170, 400, 110, hwnd, (HMENU)(UINT_PTR)ID_EDIT_RESPONSE, hinst, NULL);

    win->created = true;
    win->visible = false;
    ShowWindow(hwnd, SW_HIDE);
    return KARO_WINDOW_OK;
}

void karo_window_show(karo_window_t *win) {
    if (win == NULL || !win->created) {
        return;
    }
    ShowWindow(win->hwnd, SW_SHOW);
    SetForegroundWindow(win->hwnd);
    win->visible = true;
}

void karo_window_hide(karo_window_t *win) {
    if (win == NULL || !win->created) {
        return;
    }
    ShowWindow(win->hwnd, SW_HIDE);
    win->visible = false;
}

void karo_window_toggle(karo_window_t *win) {
    if (win == NULL || !win->created) {
        return;
    }
    if (win->visible) {
        karo_window_hide(win);
    } else {
        karo_window_show(win);
    }
}

void karo_window_destroy(karo_window_t *win) {
    if (win == NULL || !win->created) {
        return;
    }
    DestroyWindow(win->hwnd);
    win->created = false;
    win->visible = false;
}

void karo_window_get_system_prompt(const karo_window_t *win, char *out, size_t out_cap) {
    if (out == NULL || out_cap == 0u) {
        return;
    }
    if (win == NULL || !win->created) {
        out[0] = '\0';
        return;
    }
    GetWindowText(win->edit_system_prompt, out, (int)out_cap);
}

void karo_window_set_system_prompt(karo_window_t *win, const char *text) {
    if (win == NULL || !win->created) {
        return;
    }
    SetWindowText(win->edit_system_prompt, text != NULL ? text : "");
}

void karo_window_set_response_text(karo_window_t *win, const char *text) {
    if (win == NULL || !win->created) {
        return;
    }
    SetWindowText(win->edit_response, text != NULL ? text : "");
}

#endif /* defined(_WIN32) */
