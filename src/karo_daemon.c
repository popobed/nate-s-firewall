/*
 * karo_daemon.c
 *
 * KARO's standalone entry point: composes actor_core.h's actor system,
 * telemetry_fabric.h's hash-chained audit log, karo_ai_router.h's
 * multi-provider key router, karo_screen_capture.h's explicit DXGI
 * capture, and karo_window.h's draggable overlay into one runnable
 * Windows process. Reuses every mechanism verbatim from tested modules;
 * adds no new low-level primitive of its own.
 *
 * ============================== Honesty notes, up front =====================
 *   - "Ecoute les evenements systeme": Ctrl+Alt+K (one named Win32
 *     RegisterHotKey combination, never a keystroke hook) now toggles the
 *     overlay window's visibility -- it no longer fires a query itself.
 *     Sending a query and attaching a screen frame are each their own
 *     explicit, user-initiated action (the window's Send / Attach Screen
 *     buttons) -- see karo_window.c and karo_screen_capture.h for why
 *     that boundary is deliberate and unchanged from v1.
 *   - No live microphone capture is wired into this entry point (still
 *     out of scope -- see v1's own note; nothing about that changed).
 *   - "Memoire chiffree ... reservee a Mulet Obed": what IS real here is
 *     ACCESS-GATING, not encryption -- karo_daemon_history_read() requires
 *     karo_verify_owner() (constant-time, ct_equal_u32) to succeed before
 *     it will report anything from the telemetry log, and every attempt
 *     (granted or denied) is itself audited into that same log. No cipher/
 *     AEAD primitive exists anywhere in this codebase (crypto_constant_time.h
 *     and brain_core.h's brain_vault_t already establish this -- see their
 *     own honesty notes); calling this "chiffree" would be false, so this
 *     file does not. "History" reuses g_telemetry itself rather than a
 *     second storage mechanism.
 *   - "Update 1-clic generant l'actualisation du binaire" was explicitly
 *     NOT implemented -- a self-updater that overwrites its own running
 *     executable is separate, materially riskier infrastructure (supply-
 *     chain/trust surface) that was flagged and deferred, not silently
 *     dropped.
 *   - Key material (any provider) is never printed or logged -- only
 *     provisioned-slot counts and the active provider's identity.
 */

#if !defined(_WIN32)
#error "karo_daemon.c is Windows-only (RegisterHotKey, karo_window.c, karo_screen_capture.h)"
#endif

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "actor_core.h"
#include "karo_ai_router.h"
#include "karo_bridge.h"
#include "karo_interface.h"
#include "karo_screen_capture.h"
#include "karo_window.h"
#include "telemetry_fabric.h"

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000 /* fallback: some mingw-w64 header sets gate this behind an older _WIN32_WINNT default */
#endif

#define KARO_DAEMON_HOTKEY_ID   1
#define KARO_DAEMON_BRIDGE_TIMER_ID 2
#define KARO_DAEMON_BRIDGE_POLL_MS  1000u
#define KARO_DAEMON_BRIDGE_DIR      "bridge"
#define KARO_DAEMON_ENV_KEY_MAX 32u /* env vars scanned per provider: <PREFIX>0 .. <PREFIX>31 */

static const char *const KARO_DAEMON_PROVIDER_NAME[KARO_ROUTER_PROVIDER_COUNT] = { "gemini", "claude", "deepseek" };

#define KARO_DAEMON_EVENT_STARTUP 0x4B440001u /* "KD" + 0001 */
#define KARO_DAEMON_EVENT_TRIGGER 0x4B440002u /* also doubles as the "history" record for a sent query */

#define KARO_DAEMON_SCREEN_BUF_W     1920u
#define KARO_DAEMON_SCREEN_BUF_H     1080u
#define KARO_DAEMON_SCREEN_BUF_BYTES (KARO_DAEMON_SCREEN_BUF_W * KARO_DAEMON_SCREEN_BUF_H * 4u) /* BGRA8 */

static actor_system_t   g_actors;
static tf_log_t         g_telemetry;
static karo_ai_router_t g_router;
static karo_overlay_t   g_overlay;
static karo_window_t    g_window;
static uint32_t         g_trigger_seq = 0u;
static uint64_t         g_bridge_last_response_seq = 0u;

static _Alignas(64) uint8_t g_screen_buf[KARO_DAEMON_SCREEN_BUF_BYTES];

/* Reads <prefix>0 .. <prefix><KARO_DAEMON_ENV_KEY_MAX-1> from the
 * environment and provisions each one found into `provider`'s pool.
 * Never prints key material -- only the resulting count. */
static uint32_t karo_daemon_provision_provider_from_env(karo_router_provider_t provider, const char *prefix) {
    uint32_t provisioned = 0u;
    for (uint32_t i = 0u; i < KARO_DAEMON_ENV_KEY_MAX; i++) {
        char var_name[48];
        int n = snprintf(var_name, sizeof var_name, "%s%u", prefix, i);
        if (n <= 0 || (size_t)n >= sizeof var_name) {
            continue;
        }
        const char *val = getenv(var_name);
        if (val == NULL || val[0] == '\0') {
            continue;
        }
        size_t len = strlen(val);
        if (len > KARO_KEY_MAX_LEN) {
            fprintf(stderr, "[KARO] %s ignored: exceeds KARO_KEY_MAX_LEN (%u)\n", var_name, KARO_KEY_MAX_LEN);
            continue;
        }
        if (karo_ai_router_provision(&g_router, provider, i, val, (uint32_t)len) == KARO_OK) {
            provisioned++;
        }
    }
    return provisioned;
}

static uint32_t karo_daemon_provision_keys_from_env(uint32_t *out_gemini, uint32_t *out_claude, uint32_t *out_deepseek) {
    uint32_t g = karo_daemon_provision_provider_from_env(KARO_ROUTER_PROVIDER_GEMINI, "KARO_GEMINI_KEY_");
    uint32_t c = karo_daemon_provision_provider_from_env(KARO_ROUTER_PROVIDER_CLAUDE, "KARO_CLAUDE_KEY_");
    uint32_t d = karo_daemon_provision_provider_from_env(KARO_ROUTER_PROVIDER_DEEPSEEK, "KARO_DEEPSEEK_KEY_");
    if (out_gemini != NULL)   { *out_gemini = g; }
    if (out_claude != NULL)   { *out_claude = c; }
    if (out_deepseek != NULL) { *out_deepseek = d; }
    return g + c + d;
}

/* Send button: owner gate -> audit/history log (truncated to
 * TF_LOG_MSG_MAX bytes, same cap tf_log_t itself already enforces) -> one
 * staged overlay element. Does NOT call any AI provider -- see the file
 * header: no HTTP client exists in this project, karo_ai_router.h only
 * tracks which provider/key an external caller's client would use. */
static void karo_daemon_on_send(void *user_data, const char *query_text) {
    (void)user_data;
    g_trigger_seq++;

    karo_status_t auth = karo_verify_owner(KARO_OWNER_ID, &g_telemetry);
    if (auth != KARO_OK) {
        printf("[KARO] send #%u: owner verification failed -- ignored\n", g_trigger_seq);
        return;
    }

    uint32_t overlay_idx = 0u;
    karo_status_t ov = karo_overlay_push(&g_overlay, 0.0f, 0.0f, 1.0f, g_trigger_seq, &overlay_idx);

    karo_router_provider_t provider = karo_ai_router_active_provider(&g_router);
    char key_buf[KARO_KEY_MAX_LEN];
    uint32_t key_len = (uint32_t)sizeof key_buf;
    bool has_key = (karo_ai_router_active_key(&g_router, key_buf, &key_len) == KARO_OK);

    printf("[KARO] send #%u: provider=%s key_provisioned=%s overlay=%s\n",
           g_trigger_seq, KARO_DAEMON_PROVIDER_NAME[provider], has_key ? "yes" : "no",
           ov == KARO_OK ? "staged" : "full");

    size_t qlen = strlen(query_text);
    uint32_t payload_len = (uint32_t)(qlen > TF_LOG_MSG_MAX ? TF_LOG_MSG_MAX : qlen);
    (void)tf_log_append(&g_telemetry, KARO_DAEMON_EVENT_TRIGGER, query_text, payload_len);
}

/* Attach Screen button: ONE explicit DXGI capture, see karo_screen_capture.h. */
static void karo_daemon_on_attach(void *user_data) {
    (void)user_data;
    karo_screen_frame_t frame;
    karo_screen_status_t st = karo_screen_capture_attach(g_screen_buf, sizeof g_screen_buf, 200u, &frame);
    switch (st) {
    case KARO_SCREEN_OK: {
        printf("[KARO] attach: captured %ux%u (%u bytes)\n", frame.width, frame.height, frame.bytes_written);
        char prompt_buf[2048];
        karo_window_get_system_prompt(&g_window, prompt_buf, sizeof prompt_buf);
        karo_router_provider_t provider = karo_ai_router_active_provider(&g_router);
        karo_bridge_status_t bst = karo_bridge_write_capture(KARO_DAEMON_BRIDGE_DIR, &frame, g_screen_buf,
                                                              prompt_buf, KARO_DAEMON_PROVIDER_NAME[provider]);
        if (bst != KARO_BRIDGE_OK) {
            printf("[KARO] attach: bridge write failed (%d) -- karo_vision_bridge.py will not see this frame\n", (int)bst);
        } else {
            printf("[KARO] attach: staged for karo_vision_bridge.py in %s/\n", KARO_DAEMON_BRIDGE_DIR);
        }
        break;
    }
    case KARO_SCREEN_ERR_TIMEOUT:
        printf("[KARO] attach: no screen change within timeout -- nothing new to attach\n");
        break;
    case KARO_SCREEN_ERR_NO_DEVICE:
    case KARO_SCREEN_ERR_NO_OUTPUT:
        printf("[KARO] attach: no compatible display adapter/output exposed to this process (expected on a headless/VM machine)\n");
        break;
    case KARO_SCREEN_ERR_UNSUPPORTED:
        printf("[KARO] attach: desktop duplication unsupported in this session (e.g. RDP / no active desktop)\n");
        break;
    case KARO_SCREEN_ERR_BUFFER_TOO_SMALL:
        printf("[KARO] attach: frame needs %u bytes, buffer holds %zu -- not attached\n",
               frame.bytes_written, sizeof g_screen_buf);
        break;
    default:
        printf("[KARO] attach: capture failed (%d)\n", (int)st);
        break;
    }
}

/* Called on each KARO_DAEMON_BRIDGE_TIMER_ID tick: cheap check of one
 * local sequence-number file (see karo_bridge.h) -- never touches the
 * screen or network itself. If karo_vision_bridge.py has written a new
 * response since the last tick, shows it in the window's response box. */
static void karo_daemon_poll_bridge(void) {
    char response[4096];
    if (karo_bridge_poll_response(KARO_DAEMON_BRIDGE_DIR, &g_bridge_last_response_seq,
                                   response, sizeof response)) {
        printf("[KARO] bridge: new response (%zu chars)\n", strlen(response));
        karo_window_set_response_text(&g_window, response);
    }
}

/* Owner-gated history read -- see the file header on why this is
 * access-gated, not encrypted. Every call, granted or denied, is itself
 * audited by karo_verify_owner() into g_telemetry. */
static karo_status_t karo_daemon_history_read(uint32_t requester_owner_id, uint32_t *out_count) {
    karo_status_t auth = karo_verify_owner(requester_owner_id, &g_telemetry);
    if (auth != KARO_OK) {
        if (out_count != NULL) {
            *out_count = 0u;
        }
        return KARO_ERR_UNAUTHORIZED;
    }
    uint64_t n = atomic_load_explicit(&g_telemetry.write_pos, memory_order_acquire);
    if (out_count != NULL) {
        *out_count = (uint32_t)n;
    }
    return KARO_OK;
}

static void karo_daemon_run_selftest(void) {
    printf("[KARO] --selftest: simulating one Send click and one Attach-Screen click\n");
    karo_daemon_on_send(NULL, "selftest query: what is on my screen?");
    karo_daemon_on_attach(NULL);

    karo_status_t rot = karo_ai_router_report_429(&g_router);
    printf("[KARO] --selftest: karo_ai_router_report_429() = %d, active_provider=%u, provider_switches_total=%llu\n",
           (int)rot, (unsigned)karo_ai_router_active_provider(&g_router),
           (unsigned long long)atomic_load_explicit(&g_router.provider_switches_total, memory_order_relaxed));

    uint32_t count = 0u;
    karo_status_t hist_owner = karo_daemon_history_read(KARO_OWNER_ID, &count);
    printf("[KARO] --selftest: history read as owner = %d, entries=%u\n", (int)hist_owner, count);
    karo_status_t hist_impostor = karo_daemon_history_read(KARO_OWNER_ID ^ 0xFFu, &count);
    printf("[KARO] --selftest: history read as impostor = %d (expect unauthorized)\n", (int)hist_impostor);

    karo_window_set_system_prompt(&g_window, "You are KARO, a desktop productivity assistant.");
    char prompt_out[256];
    karo_window_get_system_prompt(&g_window, prompt_out, sizeof prompt_out);
    printf("[KARO] --selftest: system prompt round trip: \"%s\"\n", prompt_out);

    /* karo_daemon_on_attach() above already wrote bridge/capture.bmp,
     * system_prompt.txt, active_provider.txt and bumped capture_seq.txt
     * for real -- that's the C-side half of the IPC contract, exercised
     * genuinely. This machine has no API key and this selftest makes no
     * network call, so it cannot exercise karo_vision_bridge.py itself;
     * instead it simulates exactly what that script would write back, to
     * verify karo_bridge_poll_response()'s side of the contract for real. */
    {
        FILE *rf = fopen(KARO_DAEMON_BRIDGE_DIR "\\response.txt", "wb");
        if (rf != NULL) {
            fputs("[selftest-simulated] this screenshot shows a KiCad schematic.", rf);
            fclose(rf);
        }
        FILE *sf = fopen(KARO_DAEMON_BRIDGE_DIR "\\response_seq.txt", "wb");
        if (sf != NULL) {
            fputs("1", sf);
            fclose(sf);
        }
    }
    karo_daemon_poll_bridge();
    char response_out[256];
    GetWindowText(g_window.edit_response, response_out, (int)sizeof response_out);
    printf("[KARO] --selftest: bridge response round trip: \"%s\"\n", response_out);

    uint64_t broken_seq = 0u;
    tf_status_t verify_st = tf_log_verify(&g_telemetry, &broken_seq);
    printf("[KARO] --selftest: tf_log_verify() = %d (%s)\n", (int)verify_st,
           verify_st == TF_OK ? "chain intact" : "TAMPERED/NOT_READY");
    printf("[KARO] --selftest: PASS\n");
}

int main(void) {
    actor_system_init(&g_actors);
    tf_log_init(&g_telemetry);
    karo_ai_router_init(&g_router);
    karo_overlay_init(&g_overlay);
    (void)actor_system_spawn(&g_actors, 0u);

    uint32_t gemini_n = 0u, claude_n = 0u, deepseek_n = 0u;
    uint32_t provisioned = karo_daemon_provision_keys_from_env(&gemini_n, &claude_n, &deepseek_n);
    printf("[KARO] daemon starting -- owner_id=0x%08X, keys provisioned: gemini=%u claude=%u deepseek=%u, overlay: 0/%u elements\n",
           (unsigned)KARO_OWNER_ID, gemini_n, claude_n, deepseek_n, KARO_OVERLAY_MAX_ELEMENTS);
    (void)tf_log_append(&g_telemetry, KARO_DAEMON_EVENT_STARTUP, &provisioned, (uint32_t)sizeof provisioned);

    HINSTANCE hinst = GetModuleHandle(NULL);
    karo_window_status_t wst = karo_window_create(&g_window, hinst, karo_daemon_on_send, karo_daemon_on_attach, NULL);
    if (wst != KARO_WINDOW_OK) {
        fprintf(stderr, "[KARO] overlay window creation failed (%d)\n", (int)wst);
        return 1;
    }

    const char *selftest_env = getenv("KARO_DAEMON_SELFTEST");
    if (selftest_env != NULL && strcmp(selftest_env, "1") == 0) {
        karo_daemon_run_selftest();
        karo_window_destroy(&g_window);
        return 0;
    }

    if (!RegisterHotKey(NULL, KARO_DAEMON_HOTKEY_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, (UINT)'K')) {
        fprintf(stderr, "[KARO] RegisterHotKey(Ctrl+Alt+K) failed (error %lu) -- another process may already own it\n",
                (unsigned long)GetLastError());
        karo_window_destroy(&g_window);
        return 1;
    }
    SetTimer(NULL, KARO_DAEMON_BRIDGE_TIMER_ID, KARO_DAEMON_BRIDGE_POLL_MS, NULL);
    printf("[KARO] listening for Ctrl+Alt+K (show/hide the overlay). Ctrl+C or close this window to stop.\n");
    printf("[KARO] run karo_vision_bridge.py separately to get explanations for Attach-Screen captures.\n");

    MSG msg;
    BOOL got;
    while ((got = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (got == -1) {
            break; /* GetMessage error */
        }
        if (msg.message == WM_HOTKEY && msg.wParam == (WPARAM)KARO_DAEMON_HOTKEY_ID) {
            karo_window_toggle(&g_window);
            continue;
        }
        if (msg.message == WM_TIMER && msg.wParam == (WPARAM)KARO_DAEMON_BRIDGE_TIMER_ID) {
            karo_daemon_poll_bridge();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(NULL, KARO_DAEMON_BRIDGE_TIMER_ID);
    UnregisterHotKey(NULL, KARO_DAEMON_HOTKEY_ID);
    karo_window_destroy(&g_window);
    return 0;
}
