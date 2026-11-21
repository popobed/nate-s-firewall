/*
 * karo_bridge.h
 *
 * File-based IPC between karo_daemon.exe and the separate
 * karo_vision_bridge.py process: writes an explicitly-captured frame
 * (karo_screen_capture.h) out as a BMP, and polls back for the text
 * response that script writes once it has one.
 *
 * ============================== Honesty notes, up front =====================
 *   - This file does NOT call any AI API -- see karo_vision_bridge.py
 *     (a separate Python process) for that, consistent with this project
 *     never embedding an HTTP client (karo_ai_router.h's own note).
 *   - Every write here happens exactly once per "Attach Screen" click --
 *     the SAME explicit trigger karo_screen_capture_attach() itself
 *     already requires. Nothing in this file polls or watches the
 *     screen; karo_daemon.c's own WM_TIMER only polls one tiny local
 *     sequence-number text file (response_seq.txt) to notice when the
 *     Python script has written a new response -- not the screen, not
 *     the microphone, nothing continuous or automatic about capture
 *     itself.
 *   - BMP output is a real, standard 32bpp BGRA bottom-up bitmap (no
 *     external image library needed on the C side) -- not a placeholder.
 */

#ifndef KARO_BRIDGE_H
#define KARO_BRIDGE_H

#if defined(_WIN32)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "karo_screen_capture.h"

typedef enum {
    KARO_BRIDGE_OK = 0,
    KARO_BRIDGE_ERR_INVALID = -1,
    KARO_BRIDGE_ERR_IO      = -2
} karo_bridge_status_t;

/*
 * Writes, into `bridge_dir` (created if it does not exist):
 *   - capture.bmp          : `frame`/`pixels` (as produced by
 *                             karo_screen_capture_attach()) as a standard
 *                             bottom-up 32bpp BGRA BMP.
 *   - system_prompt.txt    : `system_prompt` verbatim (may be NULL/empty).
 *   - active_provider.txt  : `provider_name` verbatim (e.g. "gemini").
 *   - capture_seq.txt      : incremented by 1 -- the one file
 *                             karo_vision_bridge.py polls to notice a new
 *                             capture is ready.
 */
karo_bridge_status_t karo_bridge_write_capture(const char *bridge_dir, const karo_screen_frame_t *frame,
                                                const uint8_t *pixels, const char *system_prompt,
                                                const char *provider_name);

/*
 * O(1): if <bridge_dir>/response_seq.txt has changed since
 * `*inout_last_seen_seq`, reads <bridge_dir>/response.txt (truncated to
 * out_cap-1 bytes, NUL-terminated) into `out`, updates
 * `*inout_last_seen_seq`, and returns true. Returns false (out
 * untouched) if there is nothing new -- the normal "no response yet"
 * outcome, not a failure.
 */
bool karo_bridge_poll_response(const char *bridge_dir, uint64_t *inout_last_seen_seq,
                                char *out, size_t out_cap);

#endif /* defined(_WIN32) */

#endif /* KARO_BRIDGE_H */
