/*
 * karo_screen_capture.h
 *
 * KARO's explicit screen-context capture: ONE synchronous DXGI Desktop
 * Duplication frame grab per call (BGRA8), Windows-only.
 *
 * ============================== Honesty notes, up front =====================
 *   - Explicit trigger ONLY: karo_screen_capture_attach() is called
 *     exactly once per "Attach Screen" button press in karo_window.c --
 *     nothing in this file polls, loops, or runs in the background. There
 *     is no automatic/continuous capture anywhere in this codebase; that
 *     was a deliberate, negotiated scope boundary (see karo_window.c and
 *     karo_daemon.c's own notes) and this file does not quietly reopen it.
 *   - Real Desktop Duplication (D3D11 device -> IDXGIOutputDuplication),
 *     not a simulation -- same "real primitive, honestly bounded" posture
 *     as wasm_gpu_engine.h's D3D12 backend. Three outcomes that are
 *     EXPECTED, not bugs, and reported as distinct honest statuses rather
 *     than folded into one generic failure:
 *       - KARO_SCREEN_ERR_NO_DEVICE / _NO_OUTPUT: no compatible D3D11
 *         adapter/output exposed to this process (headless/no-GPU VM --
 *         this exact machine already showed the same class of outcome for
 *         D3D12 in wasm_gpu_engine.h's own tests).
 *       - KARO_SCREEN_ERR_UNSUPPORTED: DuplicateOutput() refused (common
 *         over RDP or without an active local desktop session).
 *       - KARO_SCREEN_ERR_TIMEOUT: AcquireNextFrame() only signals when
 *         the screen content has actually changed since duplication
 *         started -- no change within `timeout_ms` is not a failure.
 *   - Zero dynamic allocation in this file's own code; the D3D11/DXGI
 *     COM objects it creates are reference-counted by the driver (Release()),
 *     not malloc/free, same accounting wasm_gpu_engine.h already draws.
 *     The caller supplies the destination pixel buffer (typically a
 *     static, fixed-capacity buffer, e.g. in karo_daemon.c) -- this file
 *     never allocates one itself.
 */

#ifndef KARO_SCREEN_CAPTURE_H
#define KARO_SCREEN_CAPTURE_H

#if defined(_WIN32)

#include <stddef.h>
#include <stdint.h>

typedef enum {
    KARO_SCREEN_OK                    = 0,
    KARO_SCREEN_ERR_INVALID           = -1,
    KARO_SCREEN_ERR_NO_DEVICE         = -2, /* D3D11CreateDevice failed -- see honest scope */
    KARO_SCREEN_ERR_NO_OUTPUT         = -3, /* no DXGI output enumerated */
    KARO_SCREEN_ERR_UNSUPPORTED       = -4, /* DuplicateOutput refused (e.g. RDP/no active desktop) */
    KARO_SCREEN_ERR_TIMEOUT           = -5, /* no screen change within timeout_ms -- not a failure */
    KARO_SCREEN_ERR_ACQUIRE_FAILED    = -6,
    KARO_SCREEN_ERR_MAP_FAILED        = -7,
    KARO_SCREEN_ERR_BUFFER_TOO_SMALL  = -8  /* out_frame->bytes_written is set to the REQUIRED size -- same "report real size" pattern as karo_key_pool_active_key() */
} karo_screen_status_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;         /* bytes per row, == width*4 (BGRA8) */
    uint32_t bytes_written;  /* on KARO_SCREEN_OK: bytes actually copied into out_buf; on KARO_SCREEN_ERR_BUFFER_TOO_SMALL: bytes REQUIRED */
} karo_screen_frame_t;

/*
 * One explicit, synchronous capture of the primary display output's
 * current frame (BGRA8) into out_buf (capacity out_cap bytes). See the
 * file header for exactly what each non-OK status means and why every
 * one of them is a real, expected outcome rather than a generic failure.
 * timeout_ms bounds the wait for AcquireNextFrame.
 */
karo_screen_status_t karo_screen_capture_attach(uint8_t *out_buf, size_t out_cap,
                                                 uint32_t timeout_ms, karo_screen_frame_t *out_frame);

#endif /* defined(_WIN32) */

#endif /* KARO_SCREEN_CAPTURE_H */
