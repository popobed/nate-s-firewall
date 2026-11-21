/*
 * test_karo_core.c - Unit + true multi-thread stress test bench for
 * karo_interface.{h,c}: the lock-free Gemini key pool (including the
 * mandated 50-key concurrent rotation-under-saturation stress test), the
 * owner-exclusive constant-time gate audited into telemetry_fabric.h, the
 * audio ingestion bridge onto audio_transcribe.c's existing pipeline, and
 * the translucent overlay SoA (including a real D3D12 submit round trip
 * on Windows, GPU_ERR_NO_ADAPTER treated as a valid outcome exactly like
 * this project's other GPU tests).
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching karo_interface.c's own zero-allocation
 * constraint. Portable threading (#if _WIN32 CreateThread / else
 * pthread), same pattern as tests/test_barry_storage.c.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "karo_interface.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <pthread.h>
#endif

/* ---------------------------------------------------------------- */
/* Minimal test harness (same shape as this project's other test files) */
/* ---------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_tests_run++;                                                     \
        if (!(cond)) {                                                     \
            g_tests_failed++;                                              \
            fprintf(stderr, "  [FAIL] %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                   \
    } while (0)

#define SECTION(name) printf("-- %s\n", (name))

/* ---------------------------------------------------------------- */
/* Portable thread launch/join (mirrors test_barry_storage.c)         */
/* ---------------------------------------------------------------- */

#if defined(_WIN32)
typedef HANDLE thread_handle_t;
typedef DWORD (WINAPI *thread_fn_t)(LPVOID);
#define THREAD_FN(name) DWORD WINAPI name(LPVOID arg)
static thread_handle_t thread_start(thread_fn_t fn, void *arg) {
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}
static void thread_join(thread_handle_t h) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}
#else
typedef pthread_t thread_handle_t;
typedef void *(*thread_fn_t)(void *);
#define THREAD_FN(name) void *name(void *arg)
static thread_handle_t thread_start(thread_fn_t fn, void *arg) {
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
}
static void thread_join(thread_handle_t h) {
    pthread_join(h, NULL);
}
#endif

#define MAX_THREADS 8

/* ---------------------------------------------------------------- */
/* Part A: Gemini key pool + CAS rotation                             */
/* ---------------------------------------------------------------- */

static void test_key_pool_provision_and_active_key(void) {
    SECTION("karo_key_pool_provision/active_key: round trip, double-provision rejected, empty pool");
    karo_key_pool_t pool;
    karo_key_pool_init(&pool);

    char out[KARO_KEY_MAX_LEN];
    uint32_t out_len = sizeof out;
    CHECK(karo_key_pool_active_key(&pool, out, &out_len) == KARO_ERR_NOT_FOUND); /* nothing provisioned yet */

    CHECK(karo_key_pool_provision(&pool, 0, "AIzaSy-fake-test-key-0", 22) == KARO_OK);
    CHECK(karo_key_pool_provision(&pool, 0, "AIzaSy-fake-test-key-0", 22) == KARO_ERR_ALREADY_PROVISIONED);

    out_len = sizeof out;
    CHECK(karo_key_pool_active_key(&pool, out, &out_len) == KARO_OK);
    CHECK(out_len == 22u);
    CHECK(memcmp(out, "AIzaSy-fake-test-key-0", 22) == 0);
}

static void test_rotate_key_cas_basic(void) {
    SECTION("karo_rotate_key_cas: no-op on non-429, advances to next provisioned slot on 429");
    karo_key_pool_t pool;
    karo_key_pool_init(&pool);
    CHECK(karo_key_pool_provision(&pool, 0, "key-a", 5) == KARO_OK);
    CHECK(karo_key_pool_provision(&pool, 3, "key-b", 5) == KARO_OK);

    CHECK(karo_rotate_key_cas(&pool, 200u) == KARO_OK); /* not a quota error: no-op */
    CHECK(atomic_load_explicit(&pool.active_index, memory_order_relaxed) == 0u);
    CHECK(atomic_load_explicit(&pool.rotations_total, memory_order_relaxed) == 0u);

    CHECK(karo_rotate_key_cas(&pool, KARO_HTTP_STATUS_QUOTA_EXCEEDED) == KARO_OK);
    CHECK(atomic_load_explicit(&pool.active_index, memory_order_relaxed) == 3u); /* skipped unprovisioned slots 1,2 */
    CHECK(atomic_load_explicit(&pool.rotations_total, memory_order_relaxed) == 1u);

    CHECK(karo_rotate_key_cas(&pool, KARO_HTTP_STATUS_QUOTA_EXCEEDED) == KARO_OK);
    CHECK(atomic_load_explicit(&pool.active_index, memory_order_relaxed) == 0u); /* wraps back around */
}

static void test_rotate_key_cas_empty_pool(void) {
    SECTION("karo_rotate_key_cas: KARO_ERR_FULL when literally no slot is provisioned");
    karo_key_pool_t pool;
    karo_key_pool_init(&pool);
    CHECK(karo_rotate_key_cas(&pool, KARO_HTTP_STATUS_QUOTA_EXCEEDED) == KARO_ERR_FULL);
}

static _Atomic uint32_t g_rotate_ok_count = 0;

typedef struct {
    karo_key_pool_t *pool;
    uint32_t         attempts;
} rotate_thread_args_t;

static THREAD_FN(rotate_stress_thread) {
    rotate_thread_args_t *args = (rotate_thread_args_t *)arg;
    for (uint32_t i = 0; i < args->attempts; i++) {
        if (karo_rotate_key_cas(args->pool, KARO_HTTP_STATUS_QUOTA_EXCEEDED) == KARO_OK) {
            atomic_fetch_add_explicit(&g_rotate_ok_count, 1u, memory_order_relaxed);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_rotate_key_cas_50_keys_concurrent_stress(void) {
    SECTION("8 threads rotating 50 saturated (429) keys concurrently: never blocks, always lands on a provisioned slot");
    karo_key_pool_t pool;
    karo_key_pool_init(&pool);

    const uint32_t provisioned_count = 50u; /* the mission's own number, comfortably < KARO_KEY_POOL_SIZE (64) */
    for (uint32_t i = 0; i < provisioned_count; i++) {
        char key[16];
        int n = snprintf(key, sizeof key, "key-%u", i);
        CHECK(karo_key_pool_provision(&pool, i, key, (uint32_t)n) == KARO_OK);
    }
    atomic_store_explicit(&g_rotate_ok_count, 0u, memory_order_relaxed);

    rotate_thread_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    const uint32_t attempts_per_thread = 2000u;
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].pool = &pool;
        args[t].attempts = attempts_per_thread;
        threads[t] = thread_start(rotate_stress_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    /* with 50/64 slots provisioned and only 8 threads racing (a
     * KARO_KEY_POOL_SIZE-bounded retry budget per call), every single
     * rotation attempt succeeds -- never a hang, never a lost update */
    CHECK(atomic_load_explicit(&g_rotate_ok_count, memory_order_relaxed) == MAX_THREADS * attempts_per_thread);
    CHECK(atomic_load_explicit(&pool.rotations_total, memory_order_relaxed) == MAX_THREADS * attempts_per_thread);

    uint32_t final_active = atomic_load_explicit(&pool.active_index, memory_order_relaxed);
    CHECK(final_active < KARO_KEY_POOL_SIZE);
    CHECK(pool.slots[final_active].provisioned); /* rotation NEVER lands on an unprovisioned slot */
}

/* ---------------------------------------------------------------- */
/* Part B: owner-exclusive constant-time gate + audit trail            */
/* ---------------------------------------------------------------- */

static void test_verify_owner(void) {
    SECTION("karo_verify_owner: constant-time gate, every attempt audited into telemetry_fabric.h");
    tf_log_t log;
    tf_log_init(&log);

    CHECK(karo_verify_owner(KARO_OWNER_ID, &log) == KARO_OK);
    CHECK(karo_verify_owner(KARO_OWNER_ID + 1u, &log) == KARO_ERR_UNAUTHORIZED);
    CHECK(karo_verify_owner(0u, NULL) == KARO_ERR_UNAUTHORIZED); /* NULL log: skips logging, still gates correctly */

    CHECK(atomic_load_explicit(&log.write_pos, memory_order_relaxed) == 2u); /* the NULL-log call left no trace */
    CHECK(log.entries[0].event_type == KARO_TF_EVENT_OWNER_AUTH_OK);
    CHECK(log.entries[1].event_type == KARO_TF_EVENT_OWNER_AUTH_DENIED);

    uint32_t logged_id;
    memcpy(&logged_id, log.entries[0].payload, sizeof logged_id);
    CHECK(logged_id == (uint32_t)KARO_OWNER_ID);
    memcpy(&logged_id, log.entries[1].payload, sizeof logged_id);
    CHECK(logged_id == (uint32_t)KARO_OWNER_ID + 1u);

    /* the audit trail itself stays a valid, verifiable chain */
    CHECK(tf_log_verify(&log, NULL) == TF_OK);
}

/* ---------------------------------------------------------------- */
/* Part C: audio ingestion bridge onto audio_transcribe.c              */
/* ---------------------------------------------------------------- */

static void fill_constant(audio_sample_t *dst, audio_sample_t value) {
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        dst[i] = value;
    }
}

static void test_audio_ingest_pcm_bridge(void) {
    SECTION("karo_audio_ingest_pcm: thin bridge -- FIR filter -> Goertzel features -> SRAM store -> zero-copy mailbox emit");
    audio_filterbank_init();

    audio_pool_t pool;
    audio_pool_init(&pool);
    actor_system_t actor_sys;
    actor_system_init(&actor_sys);
    CHECK(actor_system_spawn(&actor_sys, 0u) == ACTOR_OK);
    actor_t *inference_actor = actor_system_get(&actor_sys, 0u);
    CHECK(inference_actor != NULL);

    uint32_t idx;
    CHECK(audio_frame_acquire(&pool, &idx) == AUDIO_OK);
    fill_constant(audio_frame_input(&pool, idx), 1000);

    audio_feature_vec_t features;
    uint16_t tenant_id = 55u;
    CHECK(karo_audio_ingest_pcm(&pool, idx, inference_actor, tenant_id, &features) == KARO_OK);

    CHECK(actor_mailbox_count(inference_actor) == 1u);
    ring_msg_t msg;
    CHECK(actor_process_one(inference_actor, &msg) == ACTOR_OK);
    CHECK(msg.sender_id == tenant_id);
    CHECK(msg.type == AUDIO_MSG_TYPE_FRAME_READY);
    uint32_t decoded_index;
    memcpy(&decoded_index, msg.payload, sizeof decoded_index);
    CHECK(decoded_index == idx);

    /* the features this call returned are exactly what got stored into
     * the actor's SRAM-style private memory */
    audio_feature_vec_t stored;
    CHECK(audio_actor_load_features(inference_actor, &stored) == AUDIO_OK);
    CHECK(memcmp(&stored, &features, sizeof stored) == 0);
}

static void test_audio_ingest_pcm_rejects_invalid(void) {
    SECTION("karo_audio_ingest_pcm: rejects NULL arguments and a bad frame_index cleanly");
    audio_pool_t pool;
    audio_pool_init(&pool);
    actor_system_t actor_sys;
    actor_system_init(&actor_sys);
    CHECK(actor_system_spawn(&actor_sys, 0u) == ACTOR_OK);
    actor_t *inference_actor = actor_system_get(&actor_sys, 0u);

    audio_feature_vec_t features;
    CHECK(karo_audio_ingest_pcm(NULL, 0u, inference_actor, 1u, &features) == KARO_ERR_INVALID);
    CHECK(karo_audio_ingest_pcm(&pool, 0u, NULL, 1u, &features) == KARO_ERR_INVALID);
    CHECK(karo_audio_ingest_pcm(&pool, 0u, inference_actor, 1u, NULL) == KARO_ERR_INVALID);
    CHECK(karo_audio_ingest_pcm(&pool, AUDIO_FRAME_POOL_SIZE, inference_actor, 1u, &features) == KARO_ERR_INVALID); /* out-of-range channel */
}

/* ---------------------------------------------------------------- */
/* Part D: translucent overlay SoA                                     */
/* ---------------------------------------------------------------- */

static void test_overlay_push_and_capacity(void) {
    SECTION("karo_overlay_push: O(1) SoA append, bounded capacity rejection");
    karo_overlay_t overlay;
    karo_overlay_init(&overlay);

    uint32_t idx0;
    CHECK(karo_overlay_push(&overlay, 10.0f, 20.0f, 0.5f, 7u, &idx0) == KARO_OK);
    CHECK(idx0 == 0u);
    CHECK(overlay.pos_x[0] == 10.0f && overlay.pos_y[0] == 20.0f);
    CHECK(overlay.opacity[0] == 0.5f);
    CHECK(overlay.vertex_offset[0] == 7u);
    CHECK(overlay.count == 1u);

    int ok = 0;
    for (uint32_t i = 0; i < KARO_OVERLAY_MAX_ELEMENTS + 4u; i++) {
        karo_status_t st = karo_overlay_push(&overlay, 0.0f, 0.0f, 1.0f, i, NULL);
        if (st == KARO_OK) {
            ok++;
        } else {
            CHECK(st == KARO_ERR_FULL);
        }
    }
    CHECK(ok == (int)KARO_OVERLAY_MAX_ELEMENTS - 1); /* -1: idx0 above already used one slot */
}

#if defined(_WIN32)
static void test_overlay_submit_real_d3d12(void) {
    SECTION("karo_overlay_submit: stages overlay elements as real gpu_draw_cmd_t, submits through the existing D3D12 fence backend");
    karo_overlay_t overlay;
    karo_overlay_init(&overlay);
    for (uint32_t i = 0; i < 4u; i++) {
        CHECK(karo_overlay_push(&overlay, (float)i, (float)i, 0.75f, i * 3u, NULL) == KARO_OK);
    }

    static ring_buffer_t cmd_queue;
    rb_init(&cmd_queue);

    gpu_backend_t backend;
    gpu_status_t init_st = gpu_backend_init(&backend);
    CHECK(init_st == GPU_OK || init_st == GPU_ERR_NO_ADAPTER); /* GPU_ERR_NO_ADAPTER is a valid, expected outcome here -- see wasm_gpu_engine.h */

    uint32_t submitted = 0;
    uint64_t fence_value = 0;
    CHECK(karo_overlay_submit(&overlay, &backend, &cmd_queue, &submitted, &fence_value) == GPU_OK);
    CHECK(submitted == 4u);

    gpu_backend_shutdown(&backend);
    CHECK(karo_overlay_submit(NULL, &backend, &cmd_queue, &submitted, &fence_value) == GPU_ERR_INVALID);
}
#endif

/* ---------------------------------------------------------------- */

int main(void) {
    test_key_pool_provision_and_active_key();
    test_rotate_key_cas_basic();
    test_rotate_key_cas_empty_pool();
    test_rotate_key_cas_50_keys_concurrent_stress();

    test_verify_owner();

    test_audio_ingest_pcm_bridge();
    test_audio_ingest_pcm_rejects_invalid();

    test_overlay_push_and_capacity();
#if defined(_WIN32)
    test_overlay_submit_real_d3d12();
#endif

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
