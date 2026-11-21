/*
 * test_signal_core.c - Unit + concurrency test bench for
 * signal_core.{h,c}: PCM capture ring layout/backpressure, FIR decimation
 * determinism (including streaming-chunking equivalence), SRAM token
 * decode determinism, zero-copy emission into an actor_core.h mailbox,
 * CPUID feature reporting, and a simulated concurrent audio stream
 * (capture thread + DSP/inference thread) validated against a
 * single-threaded reference computation.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching signal_core's own zero-allocation constraint.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "signal_core.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <pthread.h>
#endif

/* ---------------------------------------------------------------- */
/* Minimal test harness                                              */
/* ---------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_tests_run++;                                                    \
        if (!(cond)) {                                                     \
            g_tests_failed++;                                              \
            fprintf(stderr, "  [FAIL] %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                   \
    } while (0)

#define SECTION(name) printf("-- %s\n", (name))

/* ---------------------------------------------------------------- */
/* Layout: 64-byte alignment                                          */
/* ---------------------------------------------------------------- */

static void test_layout(void) {
    SECTION("layout: PCM ring head/tail cache lines, FIR history, SRAM activation all 64-byte aligned");
    CHECK(offsetof(sig_pcm_ring_t, head) == 0u);
    CHECK(offsetof(sig_pcm_ring_t, tail) == SIG_CACHE_LINE_SIZE);
    CHECK((offsetof(sig_pcm_ring_t, samples) % SIG_CACHE_LINE_SIZE) == 0u);
    CHECK(_Alignof(sig_fir_state_t) >= SIG_CACHE_LINE_SIZE);
    CHECK(_Alignof(sig_sram_t) >= SIG_CACHE_LINE_SIZE);
}

/* ---------------------------------------------------------------- */
/* PCM ring: correctness + backpressure                               */
/* ---------------------------------------------------------------- */

static sig_pcm_ring_t g_pcm;

static void test_pcm_ring(void) {
    SECTION("PCM ring: FIFO order + deterministic backpressure when full");

    sig_pcm_ring_init(&g_pcm);
    CHECK(sig_pcm_is_empty(&g_pcm) == true);

    sig_sample_t in[8];
    for (uint32_t i = 0; i < 8u; i++) {
        in[i] = (sig_sample_t)i;
    }
    CHECK(sig_pcm_push(&g_pcm, in, 8u) == 8u);
    CHECK(sig_pcm_count(&g_pcm) == 8u);

    sig_sample_t out[8];
    CHECK(sig_pcm_pop(&g_pcm, out, 8u) == 8u);
    for (uint32_t i = 0; i < 8u; i++) {
        CHECK(out[i] == (sig_sample_t)i); /* strict FIFO order */
    }
    CHECK(sig_pcm_is_empty(&g_pcm) == true);

    /* Fill to capacity (one slot sacrificed): pushing more than that
     * returns fewer than requested -- deterministic backpressure, never a
     * wait. */
    static sig_sample_t big[SIG_PCM_CAPACITY + 16u];
    memset(big, 0, sizeof big);
    size_t pushed = sig_pcm_push(&g_pcm, big, SIG_PCM_CAPACITY + 16u);
    CHECK(pushed == (size_t)(SIG_PCM_CAPACITY - 1u));
    CHECK(sig_pcm_count(&g_pcm) == (size_t)(SIG_PCM_CAPACITY - 1u));
}

/* ---------------------------------------------------------------- */
/* FIR: determinism, steady state, and chunking equivalence            */
/* ---------------------------------------------------------------- */

static void test_fir_steady_state(void) {
    SECTION("FIR: constant input settles to the exact same constant output (unity DC gain)");

    sig_fir_state_t st;
    sig_fir_state_init(&st);

    static sig_sample_t in[256];
    for (uint32_t i = 0; i < 256u; i++) {
        in[i] = 1000;
    }
    static sig_sample_t out[256 / SIG_DECIMATION_FACTOR];
    size_t n = sig_fir_decimate(&st, in, 256u, out, sizeof out / sizeof out[0]);
    CHECK(n == 256u / SIG_DECIMATION_FACTOR);

    /* Once the filter's history is fully populated with 1000 (after the
     * first SIG_FIR_TAPS input samples), a unity-DC-gain boxcar filter
     * must reproduce 1000 EXACTLY -- no rounding loss, since
     * SIG_FIR_TAPS * 2048 == 32768 divides evenly. */
    for (size_t i = n - 4u; i < n; i++) {
        CHECK(out[i] == 1000);
    }
}

static void test_fir_chunking_equivalence(void) {
    SECTION("FIR: one big call vs many small calls (same state) give bit-identical output");

    static sig_sample_t waveform[2048];
    for (uint32_t i = 0; i < 2048u; i++) {
        waveform[i] = (sig_sample_t)(((int32_t)(i * 37u) % 2000) - 1000);
    }

    sig_fir_state_t st_whole;
    sig_fir_state_init(&st_whole);
    static sig_sample_t out_whole[2048 / SIG_DECIMATION_FACTOR];
    size_t n_whole = sig_fir_decimate(&st_whole, waveform, 2048u, out_whole,
                                       sizeof out_whole / sizeof out_whole[0]);

    sig_fir_state_t st_chunked;
    sig_fir_state_init(&st_chunked);
    static sig_sample_t out_chunked[2048 / SIG_DECIMATION_FACTOR];
    size_t total_out = 0;
    size_t processed = 0;
    const size_t chunk_sizes[] = {1, 3, 7, 13, 29, 50};
    size_t ci = 0;
    while (processed < 2048u) {
        size_t want = chunk_sizes[ci % (sizeof chunk_sizes / sizeof chunk_sizes[0])];
        if (want > 2048u - processed) {
            want = 2048u - processed;
        }
        total_out += sig_fir_decimate(&st_chunked, waveform + processed, want,
                                       out_chunked + total_out,
                                       (sizeof out_chunked / sizeof out_chunked[0]) - total_out);
        processed += want;
        ci++;
    }

    CHECK(total_out == n_whole);
    CHECK(memcmp(out_whole, out_chunked, n_whole * sizeof(sig_sample_t)) == 0);
}

/* ---------------------------------------------------------------- */
/* SRAM token decode: determinism + zero-copy emission                */
/* ---------------------------------------------------------------- */

static actor_system_t g_sys;

static void test_token_decode_and_emission(void) {
    SECTION("SRAM token decode is deterministic; emission is zero-copy into the actor mailbox");

    sig_sram_init();
    actor_system_init(&g_sys);
    CHECK(actor_system_spawn(&g_sys, 0u) == ACTOR_OK);
    actor_t *target = actor_system_get(&g_sys, 0u);
    CHECK(target != NULL);

    static sig_sample_t decimated[SIG_ACT_DIM];
    for (uint32_t i = 0; i < SIG_ACT_DIM; i++) {
        decimated[i] = (sig_sample_t)((i * 271u) % 4000) - 2000;
    }

    sig_sram_t sram_a, sram_b;
    sig_sram_fold(&sram_a, decimated, SIG_ACT_DIM);
    sig_sram_fold(&sram_b, decimated, SIG_ACT_DIM);
    uint8_t token_a = sig_infer_token(&sram_a);
    uint8_t token_b = sig_infer_token(&sram_b);
    CHECK(token_a == token_b); /* determinism: identical input -> identical token, always */

    CHECK(sig_emit_token(target, 77u, token_a) == ACTOR_OK);
    CHECK(actor_mailbox_count(target) == 1u);

    ring_msg_t msg;
    CHECK(actor_process_one(target, &msg) == ACTOR_OK);
    CHECK(msg.sender_id == 77u);
    CHECK(msg.type == SIG_MSG_TYPE_TOKEN);
    CHECK(msg.payload[0] == token_a);
}

/* ---------------------------------------------------------------- */
/* CPUID feature reporting                                            */
/* ---------------------------------------------------------------- */

static void test_cpu_features(void) {
    SECTION("CPUID feature reporting (no code generation)");
    sig_cpu_features_t f = sig_detect_cpu_features();
#if defined(__x86_64__)
    CHECK(f.sse2 == true); /* architecturally guaranteed baseline on x86_64 */
#elif defined(__aarch64__)
    CHECK(f.neon == true); /* architecturally guaranteed baseline on AArch64 */
#endif
    printf("   sse2=%d avx2=%d avx512f=%d amx_tile=%d neon=%d\n",
           (int)f.sse2, (int)f.avx2, (int)f.avx512f, (int)f.amx_tile, (int)f.neon);
}

/* ---------------------------------------------------------------- */
/* Simulated incoming audio stream: concurrent capture + DSP/inference, */
/* validated against a single-threaded reference computation            */
/* ---------------------------------------------------------------- */

/* Kept comfortably under RING_BUFFER_CAPACITY (actor_core.h) so the
 * mailbox never needs to be drained concurrently for this test to stay
 * correct -- a deliberate simplification, not an oversight: draining
 * happens once after both threads join. */
#define SIG_TEST_TOKENS  60u
#define SIG_TEST_SAMPLES (SIG_TEST_TOKENS * SIG_DECIMATION_FACTOR * SIG_ACT_DIM)

static sig_sample_t   g_stream_waveform[SIG_TEST_SAMPLES];
static sig_pcm_ring_t g_stream_pcm;
static actor_system_t g_stream_sys;

typedef struct {
    size_t pushed_total;
} stream_producer_ctx_t;

typedef struct {
    actor_t *target;
    uint16_t tenant_id;
    size_t   tokens_emitted;
    int      emit_ok;
} stream_dsp_ctx_t;

static void generate_waveform(sig_sample_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] = (sig_sample_t)(((int32_t)((i * 37u) ^ (i >> 3)) % 2000) - 1000);
    }
}

#if defined(_WIN32)
static DWORD WINAPI stream_producer_thread(LPVOID arg) {
#else
static void *stream_producer_thread(void *arg) {
#endif
    stream_producer_ctx_t *ctx = (stream_producer_ctx_t *)arg;
    while (ctx->pushed_total < SIG_TEST_SAMPLES) {
        size_t chunk = 97u; /* deliberately not a multiple of the decimation factor or block size */
        if (chunk > SIG_TEST_SAMPLES - ctx->pushed_total) {
            chunk = SIG_TEST_SAMPLES - ctx->pushed_total;
        }
        size_t pushed = sig_pcm_push(&g_stream_pcm, g_stream_waveform + ctx->pushed_total, chunk);
        ctx->pushed_total += pushed; /* pushed < chunk (ring momentarily full) is retried next iteration */
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI stream_dsp_thread(LPVOID arg) {
#else
static void *stream_dsp_thread(void *arg) {
#endif
    stream_dsp_ctx_t *ctx = (stream_dsp_ctx_t *)arg;
    sig_fir_state_t fir;
    sig_fir_state_init(&fir);

    sig_sample_t accum[SIG_ACT_DIM * 2u];
    size_t accum_len = 0;

    while (ctx->tokens_emitted < SIG_TEST_TOKENS) {
        sig_sample_t raw[256];
        size_t n = sig_pcm_pop(&g_stream_pcm, raw, sizeof raw / sizeof raw[0]);
        if (n == 0u) {
            continue; /* pure bounded spin: see this project's established precedent */
        }

        sig_sample_t decim_out[256u / SIG_DECIMATION_FACTOR];
        size_t n_decim = sig_fir_decimate(&fir, raw, n, decim_out, sizeof decim_out / sizeof decim_out[0]);

        memcpy(accum + accum_len, decim_out, n_decim * sizeof(sig_sample_t));
        accum_len += n_decim;

        while (accum_len >= SIG_ACT_DIM) {
            sig_sram_t sram;
            sig_sram_fold(&sram, accum, SIG_ACT_DIM);
            uint8_t token = sig_infer_token(&sram);
            if (sig_emit_token(ctx->target, ctx->tenant_id, token) != ACTOR_OK) {
                ctx->emit_ok = 0;
            }
            ctx->tokens_emitted++;

            size_t remaining = accum_len - SIG_ACT_DIM;
            memmove(accum, accum + SIG_ACT_DIM, remaining * sizeof(sig_sample_t));
            accum_len = remaining;
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_simulated_audio_stream(void) {
    SECTION("simulated incoming audio stream: concurrent capture+DSP matches a single-threaded reference, exactly");

    generate_waveform(g_stream_waveform, SIG_TEST_SAMPLES);

    /* Single-threaded reference: one direct pass, no chunking, no threads. */
    sig_fir_state_t ref_fir;
    sig_fir_state_init(&ref_fir);
    static sig_sample_t ref_decimated[SIG_TEST_SAMPLES / SIG_DECIMATION_FACTOR];
    size_t ref_n_decim = sig_fir_decimate(&ref_fir, g_stream_waveform, SIG_TEST_SAMPLES,
                                           ref_decimated, sizeof ref_decimated / sizeof ref_decimated[0]);
    CHECK(ref_n_decim == (size_t)(SIG_TEST_SAMPLES / SIG_DECIMATION_FACTOR));

    static uint8_t ref_tokens[SIG_TEST_TOKENS];
    for (size_t b = 0; b < SIG_TEST_TOKENS; b++) {
        sig_sram_t sram;
        sig_sram_fold(&sram, ref_decimated + b * SIG_ACT_DIM, SIG_ACT_DIM);
        ref_tokens[b] = sig_infer_token(&sram);
    }

    /* Concurrent pipeline: real capture thread + real DSP/inference
     * thread, communicating only through sig_pcm_ring_t and (for the
     * result) the target actor's mailbox. */
    sig_pcm_ring_init(&g_stream_pcm);
    actor_system_init(&g_stream_sys);
    CHECK(actor_system_spawn(&g_stream_sys, 0u) == ACTOR_OK);
    actor_t *target = actor_system_get(&g_stream_sys, 0u);

    stream_producer_ctx_t pctx;
    pctx.pushed_total = 0;
    stream_dsp_ctx_t dctx;
    dctx.target = target;
    dctx.tenant_id = 55u;
    dctx.tokens_emitted = 0;
    dctx.emit_ok = 1;

#if defined(_WIN32)
    HANDLE hp = CreateThread(NULL, 0, stream_producer_thread, &pctx, 0, NULL);
    HANDLE hd = CreateThread(NULL, 0, stream_dsp_thread, &dctx, 0, NULL);
    CHECK(hp != NULL && hd != NULL);
    WaitForSingleObject(hp, INFINITE);
    WaitForSingleObject(hd, INFINITE);
    CloseHandle(hp);
    CloseHandle(hd);
#else
    pthread_t tp, td;
    CHECK(pthread_create(&tp, NULL, stream_producer_thread, &pctx) == 0);
    CHECK(pthread_create(&td, NULL, stream_dsp_thread, &dctx) == 0);
    pthread_join(tp, NULL);
    pthread_join(td, NULL);
#endif

    CHECK(dctx.emit_ok != 0);
    CHECK(dctx.tokens_emitted == SIG_TEST_TOKENS);
    CHECK(actor_mailbox_count(target) == SIG_TEST_TOKENS);

    /* Drain and compare against the reference, in order. */
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < SIG_TEST_TOKENS; i++) {
        ring_msg_t msg;
        CHECK(actor_process_one(target, &msg) == ACTOR_OK);
        CHECK(msg.sender_id == dctx.tenant_id);
        if (msg.payload[0] != ref_tokens[i]) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0u);

    printf("   samples=%u tokens=%u mismatches=%u (concurrent pipeline vs. single-threaded reference)\n",
           (unsigned)SIG_TEST_SAMPLES, (unsigned)SIG_TEST_TOKENS, mismatches);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_signal_core ===\n");

    test_layout();
    test_pcm_ring();
    test_fir_steady_state();
    test_fir_chunking_equivalence();
    test_token_decode_and_emission();
    test_cpu_features();
    test_simulated_audio_stream();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
