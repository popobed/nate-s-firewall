/*
 * karo_interface.h
 *
 * KARO -- the Empire's universal overlay assistant surface: a lock-free
 * Gemini API key pool with CAS-based quota-triggered rotation, an
 * owner-exclusive constant-time access gate audited into
 * telemetry_fabric.h's immutable log, a thin audio-ingestion bridge onto
 * audio_transcribe.c's existing AVX2 Goertzel pipeline, and a translucent
 * overlay SoA submitted through wasm_gpu_engine.c's existing real D3D12
 * backend. Static, zero-malloc-after-init; reuses everything this
 * project already has rather than reimplementing it.
 *
 * ============================== Honesty notes, up front =====================
 *   - Niveau 22/24/27 (Gemini key pool, "API generator", secrets) :
 *     `karo_key_pool_t` is real: a fixed pool of key SLOTS, each rotated
 *     to the next PROVISIONED slot via a bounded lock-free CAS loop on
 *     429 (quota exceeded), zero allocation, never blocking the calling
 *     inference loop. What this file deliberately does NOT do: ship any
 *     actual Gemini API key VALUE compiled into this source tree. This
 *     project has no real keys to embed, and hand-writing plausible-
 *     looking fake ones would be worse than useless (indistinguishable
 *     from a real leaked credential to anyone who later greps this repo)
 *     -- karo_key_pool_provision() loads real key material at RUNTIME,
 *     from wherever the deploying operator actually keeps it (an
 *     environment variable, telemetry_fabric.h's tenant key store, a
 *     brain_core.h barry_vault_t -- this file does not decide that for
 *     the caller). This is also why the pool cannot literally live in a
 *     linker `.rodata` section (true read-only data cannot be rotated/
 *     reprovisioned at runtime) -- it is `_Alignas(64)` static storage
 *     instead, the same "durci" meaning (fixed-size, zero dynamic
 *     allocation, never freed) barry_storage.h's own shards already
 *     established for the identical "section .rodata / BSS durcie"
 *     phrasing.
 *   - Niveau 23/33/44 (exclusive owner authentication, access logging) :
 *     `karo_verify_owner()` is real constant-time comparison
 *     (ct_equal_u32(), crypto_constant_time.h, reused verbatim) against
 *     ONE fixed, compile-time owner identifier, with every attempt
 *     logged into a caller-owned telemetry_fabric.h `tf_log_t` (FNV-1a-
 *     chained, tamper-evident). KARO_OWNER_ID is an opaque 32-bit
 *     identifier the deploying owner ("Mulet Obed", per this request)
 *     provisions and controls -- this file does not parse or hash a name
 *     string at authentication time; the constant itself, chosen once at
 *     build time, IS the credential, the same "a compile-time secret
 *     constant, not a runtime string comparison" posture net_gateway.c's
 *     shared-secret authentication already uses.
 *   - Niveau 16/35/37 (audio ingestion, AVX2 Goertzel) :
 *     `karo_audio_ingest_pcm()` is a thin, honest orchestration wrapper
 *     over audio_transcribe.c's EXISTING, REAL pipeline
 *     (audio_fir_filter_frame -> audio_extract_features ->
 *     audio_actor_store_features -> audio_emit_frame_index) -- seven
 *     lines of glue, not a second DSP engine. See audio_transcribe.h for
 *     the AVX2/CPUID gating and bit-identity guarantees it already
 *     documents; this file does not re-derive or re-claim any of them.
 *   - Niveau 34 ("floating translucent overlay at 120 FPS") :
 *     `karo_overlay_t` is a genuinely new Structure-of-Arrays
 *     (`_Alignas(64)` per column: pos_x[], pos_y[], opacity[],
 *     vertex_offset[]) submitted through wasm_gpu_engine.c's EXISTING
 *     real D3D12 device/queue/fence backend (gpu_backend_submit_frame(),
 *     reused verbatim -- this file creates no second device/queue/
 *     fence). "120 FPS constants": the same honest limit
 *     wasm_gpu_engine.h already states applies here unchanged -- no
 *     library can guarantee a display refresh cadence (driver/
 *     compositor/monitor dependent); what is real and verifiable is
 *     bounded, O(1)-per-command CPU submission plus a genuine GPU fence
 *     wait when a device exists. `opacity[]` is DATA an actual blend
 *     shader would consume; this project has no shader/PSO compiler (see
 *     wasm_gpu_engine.h's own "Honest scope" for exactly that same,
 *     already-declared limit) -- karo_overlay_submit() stages the data
 *     and the real GPU submission, not a rendered translucent pixel.
 */

#ifndef KARO_INTERFACE_H
#define KARO_INTERFACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "crypto_constant_time.h" /* ct_equal_u32: reused verbatim */
#include "telemetry_fabric.h"     /* tf_log_t/tf_log_append: reused verbatim, owner-auth audit trail */
#include "audio_transcribe.h"     /* audio_pool_t/audio_extract_features/etc: reused verbatim, not reimplemented */
#include "ring_buffer.h"          /* ring_buffer_t: the existing draw-command queue primitive, reused as-is */

#if defined(_WIN32)
#include "wasm_gpu_engine.h" /* gpu_backend_t/gpu_backend_submit_frame: reused verbatim, Windows-only, same posture as that header */
#endif

#define KARO_CACHE_LINE_SIZE 64u

typedef enum {
    KARO_OK = 0,
    KARO_ERR_INVALID      = -1,
    KARO_ERR_FULL          = -2,
    KARO_ERR_NOT_FOUND     = -3,
    KARO_ERR_OUT_OF_RANGE  = -4,
    KARO_ERR_UNAUTHORIZED  = -5,
    KARO_ERR_ALREADY_PROVISIONED = -6
} karo_status_t;

/* ==================================================================== */
/* Part A -- Gemini API key pool, lock-free CAS rotation (Niveau 22/24/27)*/
/* ==================================================================== */

#define KARO_KEY_POOL_SIZE 64u  /* power of two; comfortably covers this file's 50-key stress test */
#define KARO_KEY_MAX_LEN   128u

_Static_assert(KARO_KEY_POOL_SIZE != 0u && (KARO_KEY_POOL_SIZE & (KARO_KEY_POOL_SIZE - 1u)) == 0u,
               "KARO_KEY_POOL_SIZE must be a power of two");

typedef struct {
    _Alignas(KARO_CACHE_LINE_SIZE) char key[KARO_KEY_MAX_LEN];
    uint32_t len;
    bool     provisioned;
} karo_key_slot_t;

typedef struct {
    karo_key_slot_t  slots[KARO_KEY_POOL_SIZE]; /* every slot exists from process start -- zero allocation, ever */
    _Atomic uint32_t active_index;
    _Atomic uint64_t rotations_total; /* diagnostics: how many times karo_rotate_key_cas() actually rotated */
} karo_key_pool_t;

void karo_key_pool_init(karo_key_pool_t *pool);

/* Boot-time/admin-time only (like mesh_register_node()): loads real key
 * material supplied by the caller at runtime -- see the file header for
 * why this file never compiles one in. Not thread-safe against
 * concurrent provisioning or rotation. */
karo_status_t karo_key_pool_provision(karo_key_pool_t *pool, uint32_t slot_index, const char *key, uint32_t len);

/* O(1) direct read of the currently active slot. KARO_ERR_NOT_FOUND if
 * that slot was never provisioned (e.g. right after karo_key_pool_init(),
 * before any karo_key_pool_provision() call). */
karo_status_t karo_key_pool_active_key(const karo_key_pool_t *pool, char *out_key, uint32_t *inout_len);

#define KARO_HTTP_STATUS_QUOTA_EXCEEDED 429u /* the one status this file reacts to -- it does not parse HTTP itself, no HTTP client exists in this project */

/*
 * A no-op (KARO_OK) unless http_status == KARO_HTTP_STATUS_QUOTA_EXCEEDED.
 * On a real quota rejection: advances active_index to the next
 * PROVISIONED slot via a bounded lock-free CAS loop (bounded by
 * KARO_KEY_POOL_SIZE outer retries x KARO_KEY_POOL_SIZE inner scan -- a
 * small, fixed compile-time bound, never an unbounded spin) -- zero
 * allocation, never blocks or interrupts a caller's inference loop, the
 * exact "sans interruption" requirement this file answers. Any number of
 * concurrent callers. KARO_ERR_FULL only if literally no slot in the
 * whole pool is provisioned.
 */
karo_status_t karo_rotate_key_cas(karo_key_pool_t *pool, uint32_t http_status);

/* ==================================================================== */
/* Part B -- Owner-exclusive constant-time gate + audit trail (Niveau 23/33/44) */
/* ==================================================================== */

/* Opaque owner identifier -- see the file header: a compile-time
 * credential the deploying owner controls, not a name parsed/hashed at
 * request time. Change this value (and only this value) to change who
 * KARO answers to. */
#define KARO_OWNER_ID 0x4B41524Fu /* "KARO" as bytes, chosen as a fixed, documented, non-secret-by-obscurity placeholder -- a real deployment overrides this with its own provisioned constant */

#define KARO_TF_EVENT_OWNER_AUTH_OK     0x4B410001u
#define KARO_TF_EVENT_OWNER_AUTH_DENIED 0x4B410002u

/*
 * Constant-time comparison (ct_equal_u32(), crypto_constant_time.h,
 * reused verbatim) of `provided_owner_id` against KARO_OWNER_ID. Every
 * call -- success or failure -- is logged into `access_log` (a
 * caller-owned telemetry_fabric.h tf_log_t; may be NULL to skip logging,
 * though a real deployment never would) via tf_log_append(), payload =
 * provided_owner_id, so a denied attempt is preserved in the immutable
 * chain exactly like a granted one -- "journalisation des acces signee
 * par un checksum FNV-1a" via tf_log_t's own hash-chained checksums,
 * reused, not a second logging mechanism. Returns KARO_OK iff the IDs
 * match, KARO_ERR_UNAUTHORIZED otherwise.
 */
karo_status_t karo_verify_owner(uint32_t provided_owner_id, tf_log_t *access_log);

/* ==================================================================== */
/* Part C -- Audio ingestion bridge onto audio_transcribe.c (Niveau 16/35/37) */
/* ==================================================================== */

/*
 * Thin orchestration over audio_transcribe.h's existing, real pipeline
 * for the frame already held at `pool`'s channel `frame_index` (acquired
 * and filled by the caller via audio_frame_acquire()/audio_frame_input(),
 * exactly as audio_transcribe.h itself documents): FIR-filters it
 * (audio_fir_filter_frame(), AVX2-gated by real CPUID), extracts its
 * Goertzel mel-band features into *out_features (audio_extract_features()),
 * stores them zero-copy into `inference_actor`'s SRAM-style
 * private_memory (audio_actor_store_features()), then emits just the
 * frame index (never the samples) into `inference_actor`'s mailbox
 * (audio_emit_frame_index()). Reuses every step verbatim; adds no new
 * signal-processing code.
 */
karo_status_t karo_audio_ingest_pcm(audio_pool_t *pool, uint32_t frame_index,
                                     actor_t *inference_actor, uint16_t tenant_id,
                                     audio_feature_vec_t *out_features);

/* ==================================================================== */
/* Part D -- Translucent overlay SoA over the existing D3D12 fence backend (Niveau 34) */
/* ==================================================================== */

#define KARO_OVERLAY_MAX_ELEMENTS 256u

typedef struct {
    _Alignas(KARO_CACHE_LINE_SIZE) float    pos_x[KARO_OVERLAY_MAX_ELEMENTS];
    _Alignas(KARO_CACHE_LINE_SIZE) float    pos_y[KARO_OVERLAY_MAX_ELEMENTS];
    _Alignas(KARO_CACHE_LINE_SIZE) float    opacity[KARO_OVERLAY_MAX_ELEMENTS];       /* 0.0 (invisible) .. 1.0 (opaque) -- data for a caller-owned blend shader, see file header */
    _Alignas(KARO_CACHE_LINE_SIZE) uint32_t vertex_offset[KARO_OVERLAY_MAX_ELEMENTS]; /* into a caller-owned gpu_vertex_buffer_t */
    uint32_t count;
} karo_overlay_t;

void karo_overlay_init(karo_overlay_t *overlay);

/* O(1), bounded by KARO_OVERLAY_MAX_ELEMENTS, zero allocation. */
karo_status_t karo_overlay_push(karo_overlay_t *overlay, float x, float y, float opacity,
                                 uint32_t vertex_offset, uint32_t *out_index);

#if defined(_WIN32)
/*
 * Encodes each queued overlay element as one gpu_draw_cmd_t
 * (wasm_gpu_engine.h, reused verbatim) and pushes it onto `queue` (an
 * existing ring_buffer_t) via gpu_cmd_queue_push(), then calls
 * gpu_backend_submit_frame() -- wasm_gpu_engine.c's real D3D12 fence
 * synchronization, unchanged. This file creates no second device/queue/
 * fence. GPU_ERR_NO_ADAPTER is a valid, expected outcome in a headless
 * environment (see wasm_gpu_engine.h), not a failure of this function.
 */
gpu_status_t karo_overlay_submit(karo_overlay_t *overlay, gpu_backend_t *backend, ring_buffer_t *queue,
                                  uint32_t *out_submitted, uint64_t *out_fence_value);
#endif

#endif /* KARO_INTERFACE_H */
