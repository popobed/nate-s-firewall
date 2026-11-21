/*
 * test_audio_wasm_integration.c - Phase 5/6 : filtrage acoustique
 * (audio_transcribe.h) et moteur graphique Direct3D12 (wasm_gpu_engine.h).
 * Simule l'ingestion d'un signal PCM continu (plusieurs trames successives
 * sur un meme canal) et la synchronisation d'une file de commandes
 * graphiques, de bout en bout.
 *
 * No dynamic allocation anywhere in this file: all storage is static or on
 * the stack, matching both modules' own zero-allocation constraint.
 *
 * GPU_ERR_NO_ADAPTER is treated as an expected, valid outcome (see
 * wasm_gpu_engine.h's honest-scope note): this build/test machine has no
 * D3D12-capable display driver exposed (confirmed: D3D12CreateDevice()
 * returns DXGI_ERROR_UNSUPPORTED here). Tests adapt rather than fail when
 * that happens, while still exercising every code path that needs no live
 * device for real.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_transcribe.h"
#include "swarm_dispatch.h"
#include "wasm_gpu_engine.h"

/* ---------------------------------------------------------------- */
/* Minimal test harness (same shape as this project's other test files) */
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
/* Layout                                                              */
/* ---------------------------------------------------------------- */

static void test_layout(void) {
    SECTION("layout: audio buffers and GPU structures 64-byte aligned, gpu_draw_cmd_t fixed-size");

    static audio_pool_t pool;
    CHECK(((uintptr_t)pool.frames[0].samples % AUDIO_CACHE_LINE_SIZE) == 0u);
    CHECK(((uintptr_t)pool.frames[0].filtered % AUDIO_CACHE_LINE_SIZE) == 0u);
    CHECK(((uintptr_t)pool.fir_state[0].tail % AUDIO_CACHE_LINE_SIZE) == 0u);

    CHECK(sizeof(gpu_draw_cmd_t) == 16u);

    static gpu_vertex_buffer_t vb;
    CHECK(((uintptr_t)vb.pos_x % GPU_CACHE_LINE_SIZE) == 0u);
    CHECK(((uintptr_t)vb.normal_y % GPU_CACHE_LINE_SIZE) == 0u);
    CHECK(((uintptr_t)vb.uv_v % GPU_CACHE_LINE_SIZE) == 0u);
}

/* ---------------------------------------------------------------- */
/* audio_transcribe: pool acquisition                                  */
/* ---------------------------------------------------------------- */

static audio_pool_t g_pool;

static void test_pool_acquire_release(void) {
    SECTION("audio_frame_acquire/_release: fixed-capacity pool, O(1) bounded scan");

    audio_pool_init(&g_pool);

    uint32_t indices[AUDIO_FRAME_POOL_SIZE];
    for (uint32_t i = 0; i < AUDIO_FRAME_POOL_SIZE; i++) {
        CHECK(audio_frame_acquire(&g_pool, &indices[i]) == AUDIO_OK);
    }
    uint32_t overflow_index;
    CHECK(audio_frame_acquire(&g_pool, &overflow_index) == AUDIO_ERR_POOL_EXHAUSTED);

    audio_frame_release(&g_pool, indices[3]);
    uint32_t reacquired;
    CHECK(audio_frame_acquire(&g_pool, &reacquired) == AUDIO_OK);
    CHECK(reacquired == indices[3]);

    CHECK(audio_frame_input(&g_pool, AUDIO_FRAME_POOL_SIZE) == NULL);   /* out of range */
    CHECK(audio_frame_output(&g_pool, AUDIO_FRAME_POOL_SIZE) == NULL);
    CHECK(audio_frame_acquire(NULL, &reacquired) == AUDIO_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* FIR: DC settle (unity gain) + cold-start vs warm-tail continuity     */
/* ---------------------------------------------------------------- */

static void fill_constant(audio_sample_t *buf, audio_sample_t value) {
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        buf[i] = value;
    }
}

static void test_fir_scalar_dc_settle(void) {
    SECTION("FIR (forced scalar): constant input settles to the exact same constant (unity DC gain)");

    audio_pool_init(&g_pool);
    uint32_t idx;
    CHECK(audio_frame_acquire(&g_pool, &idx) == AUDIO_OK);

    fill_constant(audio_frame_input(&g_pool, idx), 1000);
    CHECK(audio_fir_filter_frame_ex(&g_pool, idx, AUDIO_FIR_FORCE_SCALAR) == AUDIO_OK);

    const audio_sample_t *out = audio_frame_output(&g_pool, idx);
    /* Cold start: the first AUDIO_FIR_HISTORY outputs are pulled down by
     * the zero-padded history -- not yet steady state. */
    CHECK(out[0] < 1000);
    /* Once the window is entirely inside the constant input, the boxcar
     * (unity DC gain) reproduces the input exactly. */
    for (uint32_t i = AUDIO_FIR_HISTORY; i < AUDIO_FRAME_SAMPLES; i++) {
        CHECK(out[i] == 1000);
    }
}

static void test_fir_streaming_continuity(void) {
    SECTION("FIR: state carries across frames -- a warm tail starts already at steady state");

    audio_pool_init(&g_pool);
    uint32_t idx;
    CHECK(audio_frame_acquire(&g_pool, &idx) == AUDIO_OK);

    /* Frame A: cold start on a constant signal. */
    fill_constant(audio_frame_input(&g_pool, idx), 500);
    CHECK(audio_fir_filter_frame_ex(&g_pool, idx, AUDIO_FIR_FORCE_SCALAR) == AUDIO_OK);
    const audio_sample_t *out_a = audio_frame_output(&g_pool, idx);
    CHECK(out_a[0] < 500); /* cold start, zero history still in the window */

    /* Frame B: same channel, same constant signal -- the tail carried over
     * from frame A is already 500, so EVERY output (including the very
     * first) is exactly 500 from the start. This is the concrete,
     * checkable difference between "continuity across frames" and
     * "independent, cold-started frames". */
    fill_constant(audio_frame_input(&g_pool, idx), 500);
    CHECK(audio_fir_filter_frame_ex(&g_pool, idx, AUDIO_FIR_FORCE_SCALAR) == AUDIO_OK);
    const audio_sample_t *out_b = audio_frame_output(&g_pool, idx);
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        CHECK(out_b[i] == 500);
    }
}

/* ---------------------------------------------------------------- */
/* FIR: AVX2 kernel -- real hardware dispatch, never unconditional      */
/* ---------------------------------------------------------------- */

static void test_fir_avx2_dispatch_is_safe_and_correct(void) {
    SECTION("FIR: AVX2 path is gated by real CPUID -- refuses cleanly when unavailable, bit-identical when available");

    const sig_cpu_features_t feat = sig_detect_cpu_features();
    printf("   sig_detect_cpu_features().avx2 = %d on this build/test machine\n", (int)feat.avx2);

    audio_pool_t pool_scalar, pool_avx2;
    audio_pool_init(&pool_scalar);
    audio_pool_init(&pool_avx2);

    uint32_t idx_s, idx_v;
    CHECK(audio_frame_acquire(&pool_scalar, &idx_s) == AUDIO_OK);
    CHECK(audio_frame_acquire(&pool_avx2, &idx_v) == AUDIO_OK);

    /* Same non-trivial (non-constant) input on both channels. */
    audio_sample_t *in_s = audio_frame_input(&pool_scalar, idx_s);
    audio_sample_t *in_v = audio_frame_input(&pool_avx2, idx_v);
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        const audio_sample_t v = (audio_sample_t)(((i * 733u) % 4001u) - 2000);
        in_s[i] = v;
        in_v[i] = v;
    }

    audio_status_t scalar_st = audio_fir_filter_frame_ex(&pool_scalar, idx_s, AUDIO_FIR_FORCE_SCALAR);
    CHECK(scalar_st == AUDIO_OK);

    audio_status_t avx2_st = audio_fir_filter_frame_ex(&pool_avx2, idx_v, AUDIO_FIR_FORCE_AVX2);

    if (!feat.avx2) {
        /* This build/test machine has no AVX2 hardware (confirmed above):
         * the dispatcher MUST refuse rather than execute an illegal
         * instruction. This is the safety property that matters most --
         * verified for real, on real hardware, every time this suite
         * runs here. */
        CHECK(avx2_st == AUDIO_ERR_AVX2_UNAVAILABLE);
        printf("   (AVX2 hardware absent here: AUDIO_FIR_FORCE_AVX2 correctly refused instead of crashing)\n");
    } else {
        /* Real AVX2 hardware is present: the vector kernel must actually
         * run and produce a bit-identical result to the scalar reference
         * (see audio_transcribe.c for why integer accumulation guarantees
         * this exactly, not just approximately). */
        CHECK(avx2_st == AUDIO_OK);
        const audio_sample_t *out_s = audio_frame_output(&pool_scalar, idx_s);
        const audio_sample_t *out_v = audio_frame_output(&pool_avx2, idx_v);
        CHECK(memcmp(out_s, out_v, sizeof(audio_sample_t) * AUDIO_FRAME_SAMPLES) == 0);
        printf("   (AVX2 hardware present: vector kernel output verified bit-identical to scalar)\n");
    }
}

/* ---------------------------------------------------------------- */
/* Feature extraction: real Goertzel mel-spaced filterbank              */
/* ---------------------------------------------------------------- */

static void test_feature_extraction_dispatch_safety(void) {
    SECTION("audio_extract_features: real Goertzel filterbank, AVX2 gated by CPUID, epsilon-verified when available");

    audio_filterbank_init();

    audio_pool_init(&g_pool);
    uint32_t idx;
    CHECK(audio_frame_acquire(&g_pool, &idx) == AUDIO_OK);
    audio_sample_t *in = audio_frame_input(&g_pool, idx);
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        in[i] = (audio_sample_t)(((i * 613u) % 6001u) - 3000);
    }
    CHECK(audio_fir_filter_frame(&g_pool, idx) == AUDIO_OK);
    const audio_sample_t *filtered = audio_frame_output(&g_pool, idx);

    audio_feature_vec_t feat_scalar;
    CHECK(audio_extract_features(filtered, &feat_scalar, AUDIO_FIR_FORCE_SCALAR) == AUDIO_OK);

    const sig_cpu_features_t feat_cpu = sig_detect_cpu_features();
    audio_feature_vec_t feat_avx2;
    const audio_status_t avx2_st = audio_extract_features(filtered, &feat_avx2, AUDIO_FIR_FORCE_AVX2);
    if (!feat_cpu.avx2) {
        /* Meme garantie de securite que le noyau FIR : jamais d'instruction
         * illegale sur du materiel sans AVX2, verifie pour de vrai ici. */
        CHECK(avx2_st == AUDIO_ERR_AVX2_UNAVAILABLE);
    } else {
        CHECK(avx2_st == AUDIO_OK);
        /* Flottant, pas entier (voir le header) : tolerance epsilon apres
         * quantification INT8, jamais une egalite bit-a-bit. */
        for (uint32_t k = 0; k < AUDIO_MEL_BANDS; k++) {
            const int diff = (int)feat_scalar.bands[k] - (int)feat_avx2.bands[k];
            CHECK(diff >= -1 && diff <= 1);
        }
    }

    /* Determinisme : meme entree -> meme sortie, toujours (aucun alea). */
    audio_feature_vec_t feat_scalar_2;
    CHECK(audio_extract_features(filtered, &feat_scalar_2, AUDIO_FIR_FORCE_SCALAR) == AUDIO_OK);
    CHECK(memcmp(&feat_scalar, &feat_scalar_2, sizeof feat_scalar) == 0);

    CHECK(audio_extract_features(NULL, &feat_scalar, AUDIO_FIR_AUTO) == AUDIO_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* Goertzel scalar kernel: unrolled-by-4 vs. pre-unroll reference        */
/* ---------------------------------------------------------------- */

#if defined(_WIN32)
static double now_ns(void) {
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1e9 / (double)freq.QuadPart;
}
#else
static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

static void test_goertzel_unroll_bit_exact_and_timing(void) {
    SECTION("Goertzel scalar kernel: unrolled-by-4 production kernel is BIT-EXACT vs. the "
            "pre-unroll reference, across varied inputs -- measured, not assumed, timing delta");

    /* Several distinct coefficients (spanning the real range 2*cos(theta)
     * can take, theta in [0,pi]) and frame contents -- not just one lucky
     * input. */
    static const float coeffs[] = { -1.9f, -0.7f, 0.0f, 0.31415f, 1.0f, 1.9998f };

    for (size_t c = 0; c < sizeof(coeffs) / sizeof(coeffs[0]); c++) {
        for (int pattern = 0; pattern < 3; pattern++) {
            static audio_sample_t frame[AUDIO_FRAME_SAMPLES];
            for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                switch (pattern) {
                    case 0: frame[i] = (audio_sample_t)(((i * 613u) % 6001u) - 3000); break;
                    case 1: frame[i] = (audio_sample_t)((i % 2u == 0u) ? 3000 : -3000); break;
                    default: frame[i] = 0; break;
                }
            }

            float unrolled  = audio_goertzel_power_scalar(frame, coeffs[c]);
            float reference = audio_goertzel_power_scalar_reference(frame, coeffs[c]);

            /* Bit-exact: reinterpret as the raw bit pattern so even a
             * NaN/inf edge case (none expected here, but the comparison
             * should not silently pass one via float `==`'s NaN
             * semantics) is checked honestly. */
            uint32_t bits_u, bits_r;
            memcpy(&bits_u, &unrolled, sizeof bits_u);
            memcpy(&bits_r, &reference, sizeof bits_r);
            CHECK(bits_u == bits_r);
        }
    }

    /* Measured timing delta -- this codebase's standing rule (see
     * SESSION_NOTES.md) is verified, not assumed. Printed honestly
     * either way: unrolling a serial IIR recursion's loop overhead is a
     * modest effect, not a register-saturation win (see the header/impl
     * comments), so a large measured speedup here would itself be a sign
     * something else changed, not a claim this test tries to force. */
    static audio_sample_t timing_frame[AUDIO_FRAME_SAMPLES];
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        timing_frame[i] = (audio_sample_t)(((i * 991u) % 6001u) - 3000);
    }
    const int reps = 20000;
    volatile float sink = 0.0f;

    double t0 = now_ns();
    for (int r = 0; r < reps; r++) {
        sink += audio_goertzel_power_scalar(timing_frame, 0.87f);
    }
    double t1 = now_ns();
    for (int r = 0; r < reps; r++) {
        sink += audio_goertzel_power_scalar_reference(timing_frame, 0.87f);
    }
    double t2 = now_ns();
    (void)sink;

    double unrolled_ns_per_call  = (t1 - t0) / (double)reps;
    double reference_ns_per_call = (t2 - t1) / (double)reps;
    printf("   unrolled=%.1f ns/call  reference=%.1f ns/call  (delta=%.1f%%)\n",
           unrolled_ns_per_call, reference_ns_per_call,
           100.0 * (reference_ns_per_call - unrolled_ns_per_call) / reference_ns_per_call);
}

/* ---------------------------------------------------------------- */
/* Inference dans le scratchpad SRAM (actor_t.private_memory)           */
/* ---------------------------------------------------------------- */

static void test_sram_inference(void) {
    SECTION("SRAM scratchpad: features stored/read from actor_t.private_memory, deterministic argmax -- NOT real phoneme recognition");

    audio_phoneme_classifier_init();
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0u);
    CHECK(actor != NULL);

    audio_feature_vec_t feat;
    for (uint32_t k = 0; k < AUDIO_MEL_BANDS; k++) {
        feat.bands[k] = (int8_t)((int32_t)k * 7 - 20);
    }

    CHECK(audio_actor_store_features(actor, &feat) == ACTOR_OK);

    audio_feature_vec_t loaded;
    CHECK(audio_actor_load_features(actor, &loaded) == AUDIO_OK);
    CHECK(memcmp(&feat, &loaded, sizeof feat) == 0); /* aller-retour SRAM exact */

    const uint8_t class_a = audio_actor_infer_phoneme_class(actor);
    const uint8_t class_b = audio_actor_infer_phoneme_class(actor);
    CHECK(class_a == class_b); /* determinisme : meme scratchpad -> meme classe, toujours */
    CHECK(class_a < AUDIO_PHONEME_CLASS_COUNT);

    CHECK(audio_actor_store_features(NULL, &feat) == ACTOR_ERR_INVALID);
    CHECK(audio_actor_load_features(NULL, &loaded) == AUDIO_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* Composition : injection zero-copy dans le repartiteur FSM +          */
/* alerte critique + relance deterministe a compteur de cycles          */
/* ---------------------------------------------------------------- */

static int      g_followup_fired = 0;
static uint16_t g_followup_tenant = 0;
static uint64_t g_followup_cycle = 0;

static void test_followup_hook(uint16_t tenant_id, agent_state_t state, uint64_t current_cycle) {
    (void)state;
    g_followup_fired++;
    g_followup_tenant = tenant_id;
    g_followup_cycle = current_cycle;
}

static void test_transcribed_frame_into_fsm_dispatch(void) {
    SECTION("composition: SRAM-inferred result -> zero-copy injection into swarm_dispatch's FSM, plus cycle-counter follow-up");

    /* Mappage de demonstration UNIQUEMENT (local a ce test, arbitraire) :
     * il ne pretend a AUCUNE comprehension linguistique -- il existe pour
     * prouver que la plomberie "resultat d'inference -> ingestion FSM"
     * fonctionne de bout en bout, exactement ce que la demande appelle
     * "injection en Zero-Copy dans le repartiteur de taches de l'automate
     * a etats finis". */
    swarm_system_t swarm;
    swarm_system_init(&swarm);
    swarm_script_engine_init(); /* reinitialise aussi le hook de relance (voir swarm_dispatch.c) */
    swarm_followup_register(test_followup_hook);
    g_followup_fired = 0;

    const uint16_t tenant = 42u;
    const uint8_t  simulated_intent_code = 4u; /* SWARM_INTENT_HUMAN_REQUEST -- comme si la classe inferee y correspondait */

    uint8_t lead_payload[SWARM_LEAD_MAX_FRAME];
    const uint32_t lead_id = 9001u;
    memcpy(lead_payload, &lead_id, sizeof lead_id);
    lead_payload[4] = simulated_intent_code;
    lead_payload[5] = 0u;
    lead_payload[6] = 0u;
    const size_t lead_len = SWARM_LEAD_HEADER_SIZE;

    /* Compteurs de cycles opaques (voir swarm_dispatch.h) -- typiquement
     * atomic_load(&actor->processed_total) en pratique ; ici des valeurs
     * fixes pour un test reproductible. */
    const uint64_t current_cycle          = 1000u;
    const uint64_t followup_delay_cycles  = 500u; /* traduire "7 jours" en cycles est la responsabilite de l'appelant */

    CHECK(swarm_ingest_ex(&swarm, tenant, lead_payload, lead_len, current_cycle, followup_delay_cycles) == SWARM_OK);

    const swarm_agent_t *agent = swarm_get_agent(&swarm, tenant);
    CHECK(agent != NULL);
    CHECK(agent->alert_flag == true);
    CHECK(agent->followup_due_cycle == current_cycle + followup_delay_cycles);

    /* Pas encore echu : le hook ne doit pas se declencher. */
    CHECK(swarm_tick(&swarm, tenant, current_cycle + followup_delay_cycles - 1u) == SWARM_OK);
    CHECK(g_followup_fired == 0);

    /* Echeance atteinte : declenchement exactement une fois (one-shot). */
    CHECK(swarm_tick(&swarm, tenant, current_cycle + followup_delay_cycles) == SWARM_OK);
    CHECK(g_followup_fired == 1);
    CHECK(g_followup_tenant == tenant);
    CHECK(g_followup_cycle == current_cycle + followup_delay_cycles);
    CHECK(swarm_get_agent(&swarm, tenant)->followup_due_cycle == 0u);

    /* Un tick ulterieur ne redeclenche pas (one-shot deja consomme). */
    CHECK(swarm_tick(&swarm, tenant, current_cycle + followup_delay_cycles + 1000u) == SWARM_OK);
    CHECK(g_followup_fired == 1);

    CHECK(swarm_agent_ack_alert(&swarm, tenant) == SWARM_OK);
    CHECK(swarm_get_agent(&swarm, tenant)->alert_flag == false);
}

/* ---------------------------------------------------------------- */
/* Zero-copy emission into the inference actor's mailbox                */
/* ---------------------------------------------------------------- */

static actor_system_t g_actor_sys;

static void test_zero_copy_emission(void) {
    SECTION("audio_emit_frame_index: only the index crosses into the mailbox, never the samples");

    audio_pool_init(&g_pool);
    actor_system_init(&g_actor_sys);
    CHECK(actor_system_spawn(&g_actor_sys, 0u) == ACTOR_OK);
    actor_t *inference_actor = actor_system_get(&g_actor_sys, 0u);
    CHECK(inference_actor != NULL);

    uint32_t idx;
    CHECK(audio_frame_acquire(&g_pool, &idx) == AUDIO_OK);
    fill_constant(audio_frame_input(&g_pool, idx), 42);
    CHECK(audio_fir_filter_frame(&g_pool, idx) == AUDIO_OK);

    const uint16_t tenant_id = 314u;
    CHECK(audio_emit_frame_index(inference_actor, tenant_id, idx) == ACTOR_OK);
    CHECK(actor_mailbox_count(inference_actor) == 1u);

    ring_msg_t msg;
    CHECK(actor_process_one(inference_actor, &msg) == ACTOR_OK);
    CHECK(msg.sender_id == tenant_id);
    CHECK(msg.type == AUDIO_MSG_TYPE_FRAME_READY);

    uint32_t decoded_index;
    memcpy(&decoded_index, msg.payload, sizeof decoded_index);
    CHECK(decoded_index == idx);
    for (size_t i = sizeof decoded_index; i < RING_MSG_PAYLOAD_SIZE; i++) {
        CHECK(msg.payload[i] == 0u); /* nothing beyond the index ever crossed */
    }

    /* The consumer reads the filtered samples in place via the shared pool. */
    const audio_sample_t *filtered = audio_frame_output(&g_pool, decoded_index);
    CHECK(filtered[AUDIO_FRAME_SAMPLES - 1u] == 42);

    CHECK(audio_emit_frame_index(NULL, tenant_id, idx) == ACTOR_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* wasm_gpu_engine: SoA vertex buffer                                   */
/* ---------------------------------------------------------------- */

static gpu_vertex_buffer_t g_vb;

static void test_vertex_buffer_soa(void) {
    SECTION("gpu_vertex_buffer_push: SoA layout, O(1) bounded append, capacity rejection");

    gpu_vertex_buffer_init(&g_vb);

    uint32_t idx0, idx1;
    CHECK(gpu_vertex_buffer_push(&g_vb, 1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f, &idx0) == GPU_OK);
    CHECK(idx0 == 0u);
    CHECK(gpu_vertex_buffer_push(&g_vb, 4.0f, 5.0f, 6.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.75f, &idx1) == GPU_OK);
    CHECK(idx1 == 1u);
    CHECK(g_vb.count == 2u);

    /* Each channel lives in its own array -- verify no cross-talk between
     * logically different vertices/channels. */
    CHECK(g_vb.pos_x[0] == 1.0f && g_vb.pos_y[0] == 2.0f && g_vb.pos_z[0] == 3.0f);
    CHECK(g_vb.pos_x[1] == 4.0f && g_vb.pos_y[1] == 5.0f && g_vb.pos_z[1] == 6.0f);
    CHECK(g_vb.uv_u[0] == 0.5f && g_vb.uv_v[1] == 0.75f);

    for (uint32_t i = g_vb.count; i < GPU_MAX_VERTICES; i++) {
        uint32_t out_index;
        CHECK(gpu_vertex_buffer_push(&g_vb, 0, 0, 0, 0, 0, 0, 0, 0, &out_index) == GPU_OK);
    }
    CHECK(g_vb.count == GPU_MAX_VERTICES);

    uint32_t overflow_index;
    CHECK(gpu_vertex_buffer_push(&g_vb, 0, 0, 0, 0, 0, 0, 0, 0, &overflow_index) == GPU_ERR_CAPACITY);
    CHECK(gpu_vertex_buffer_push(NULL, 0, 0, 0, 0, 0, 0, 0, 0, &overflow_index) == GPU_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* wasm_gpu_engine: command queue (built on ring_buffer.h)              */
/* ---------------------------------------------------------------- */

static ring_buffer_t g_cmd_ring;

static void test_cmd_queue_roundtrip(void) {
    SECTION("gpu_cmd_queue_push/_pop: zero-copy round trip + O(1) backpressure on ring_buffer.h");

    rb_init(&g_cmd_ring);

    gpu_draw_cmd_t cmd = {.vertex_offset = 10u, .vertex_count = 3u, .transform_id = 7u, ._reserved = 0u};
    CHECK(gpu_cmd_queue_push(&g_cmd_ring, 1u, &cmd) == GPU_OK);

    gpu_draw_cmd_t out;
    memset(&out, 0xFF, sizeof out);
    CHECK(gpu_cmd_queue_pop(&g_cmd_ring, &out) == GPU_OK);
    CHECK(out.vertex_offset == 10u && out.vertex_count == 3u && out.transform_id == 7u);

    CHECK(gpu_cmd_queue_pop(&g_cmd_ring, &out) == GPU_ERR_QUEUE_EMPTY);
    CHECK(gpu_cmd_queue_push(NULL, 1u, &cmd) == GPU_ERR_INVALID);

    /* O(1) backpressure: RING_BUFFER_CAPACITY commands fit, the next one
     * is rejected immediately, never blocks. */
    for (uint32_t i = 0; i < RING_BUFFER_CAPACITY; i++) {
        cmd.transform_id = i;
        CHECK(gpu_cmd_queue_push(&g_cmd_ring, 1u, &cmd) == GPU_OK);
    }
    CHECK(gpu_cmd_queue_push(&g_cmd_ring, 1u, &cmd) == GPU_ERR_QUEUE_FULL);

    for (uint32_t i = 0; i < RING_BUFFER_CAPACITY; i++) {
        CHECK(gpu_cmd_queue_pop(&g_cmd_ring, &out) == GPU_OK);
        CHECK(out.transform_id == i);
    }
    CHECK(gpu_cmd_queue_pop(&g_cmd_ring, &out) == GPU_ERR_QUEUE_EMPTY);
}

/* ---------------------------------------------------------------- */
/* wasm_gpu_engine: Rust/WASM (verio) FFI bridge -- portable ISO C11,   */
/* private static storage. See wasm_gpu_engine.h's "Pont Rust/WASM      */
/* (verio), precisement" note: this proves the C side of that bridge    */
/* compiles and behaves correctly under this project's normal strict    */
/* native build; it does not itself exercise the wasm32/Rust link step  */
/* (see verio/build.rs for that half, gated behind the disabled-by-     */
/* default `gpu-bridge` Cargo feature).                                 */
/* ---------------------------------------------------------------- */

static void test_wasm_bridge_vertex_and_cmd_roundtrip(void) {
    SECTION("gpu_wasm_bridge_*: scalar-only FFI surface, private static storage, real round trip");

    gpu_wasm_bridge_init();
    CHECK(gpu_wasm_bridge_vertex_count() == 0u);

    uint32_t idx0, idx1;
    CHECK(gpu_wasm_bridge_push_vertex(1.0f, 2.0f, 3.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f, &idx0) == GPU_OK);
    CHECK(idx0 == 0u);
    CHECK(gpu_wasm_bridge_push_vertex(4.0f, 5.0f, 6.0f, 0.0f, 0.0f, 1.0f, 0.25f, 0.75f, &idx1) == GPU_OK);
    CHECK(idx1 == 1u);
    CHECK(gpu_wasm_bridge_vertex_count() == 2u);
    CHECK(gpu_wasm_bridge_push_vertex(0, 0, 0, 0, 0, 0, 0, 0, NULL) == GPU_OK);
    CHECK(gpu_wasm_bridge_vertex_count() == 3u);

    CHECK(gpu_wasm_bridge_push_draw_cmd(1u, 10u, 3u, 7u) == GPU_OK);

    uint32_t offset = 0, count = 0, transform = 0;
    CHECK(gpu_wasm_bridge_pop_draw_cmd(&offset, &count, &transform) == GPU_OK);
    CHECK(offset == 10u && count == 3u && transform == 7u);
    CHECK(gpu_wasm_bridge_pop_draw_cmd(&offset, &count, &transform) == GPU_ERR_QUEUE_EMPTY);
    /* NULL out-params are all individually optional, like gpu_cmd_queue_pop's own callers. */
    CHECK(gpu_wasm_bridge_push_draw_cmd(1u, 11u, 4u, 8u) == GPU_OK);
    CHECK(gpu_wasm_bridge_pop_draw_cmd(NULL, NULL, NULL) == GPU_OK);

    /* gpu_wasm_bridge_init() is a real reset, not just a first-time init. */
    gpu_wasm_bridge_init();
    CHECK(gpu_wasm_bridge_vertex_count() == 0u);
    CHECK(gpu_wasm_bridge_pop_draw_cmd(&offset, &count, &transform) == GPU_ERR_QUEUE_EMPTY);
}

/* ---------------------------------------------------------------- */
/* wasm_gpu_engine: real Direct3D12 backend (device/queue/fence)        */
/* ---------------------------------------------------------------- */

static void test_gpu_backend_and_frame_sync(void) {
    SECTION("gpu_backend_init/_submit_frame: real D3D12 calls; GPU_ERR_NO_ADAPTER is a valid, handled outcome");

    gpu_backend_t backend;
    gpu_status_t init_st = gpu_backend_init(&backend);

    rb_init(&g_cmd_ring);
    gpu_draw_cmd_t cmd = {.vertex_offset = 0u, .vertex_count = 3u, .transform_id = 1u, ._reserved = 0u};
    CHECK(gpu_cmd_queue_push(&g_cmd_ring, 1u, &cmd) == GPU_OK);
    CHECK(gpu_cmd_queue_push(&g_cmd_ring, 1u, &cmd) == GPU_OK);

    uint32_t submitted = 0;
    uint64_t fence_value = 0;

    if (init_st == GPU_OK) {
        printf("   (a real D3D12-capable display driver is present -- exercising the live fence round trip)\n");
        CHECK(gpu_backend_submit_frame(&backend, &g_cmd_ring, 8u, &submitted, &fence_value) == GPU_OK);
        CHECK(submitted == 2u); /* exactly the 2 real commands queued above -- bounded, deterministic */
        CHECK(fence_value == 1u);

        /* A second frame: the fence value is real and monotonically
         * increasing -- the same primitive production frame pacing uses. */
        uint64_t fence_value_2 = 0;
        CHECK(gpu_backend_submit_frame(&backend, &g_cmd_ring, 8u, &submitted, &fence_value_2) == GPU_OK);
        CHECK(submitted == 0u); /* queue was already drained */
        CHECK(fence_value_2 == 2u);
    } else {
        CHECK(init_st == GPU_ERR_NO_ADAPTER); /* the only other outcome expected in this sandbox -- see the header */
        printf("   (no D3D12-capable display driver exposed here -- GPU_ERR_NO_ADAPTER, expected in this "
               "headless/VM environment; see wasm_gpu_engine.h)\n");

        /* Even with no live device, the CPU-side queue drain is real and
         * bounded -- verified without needing any GPU at all. */
        CHECK(gpu_backend_submit_frame(&backend, &g_cmd_ring, 8u, &submitted, &fence_value) == GPU_OK);
        CHECK(submitted == 2u);
        CHECK(fence_value == 0u); /* no device to synchronize with */
    }

    CHECK(gpu_backend_submit_frame(NULL, &g_cmd_ring, 8u, &submitted, &fence_value) == GPU_ERR_INVALID);

    gpu_backend_shutdown(&backend);
    gpu_backend_shutdown(&backend); /* idempotent: must not double-free/crash */
}

/* ---------------------------------------------------------------- */
/* Combined: continuous PCM ingestion driving both the inference actor  */
/* and the graphics command queue, one simulated tick at a time         */
/* ---------------------------------------------------------------- */

#define STREAM_TICKS 32u

/* Tick a la mediane du flux ou l'on simule une interaction critique
 * detectee (demande d'humain) -- voir la boucle ci-dessous. */
#define STREAM_HUMAN_REQUEST_TICK 15u
#define STREAM_FOLLOWUP_DELAY_CYCLES 3u /* petit expres : doit echoir avant la fin de STREAM_TICKS */

static void test_continuous_audio_and_graphics_pipeline(void) {
    SECTION("continuous simulated PCM stream: local vectorized \"transcription\" (filter+features+SRAM) "
            "+ zero-copy FSM dispatch + graphics queue sync, end to end");

    audio_filterbank_init();
    audio_phoneme_classifier_init();

    audio_pool_init(&g_pool);
    actor_system_init(&g_actor_sys);
    CHECK(actor_system_spawn(&g_actor_sys, 0u) == ACTOR_OK);
    actor_t *inference_actor = actor_system_get(&g_actor_sys, 0u);
    CHECK(inference_actor != NULL);

    swarm_system_t swarm;
    swarm_system_init(&swarm);
    swarm_script_engine_init();
    swarm_followup_register(test_followup_hook);
    g_followup_fired = 0;
    const uint16_t fsm_tenant = 7u;

    rb_init(&g_cmd_ring);

    gpu_backend_t backend;
    gpu_status_t init_st = gpu_backend_init(&backend);
    CHECK(init_st == GPU_OK || init_st == GPU_ERR_NO_ADAPTER);

    uint32_t idx;
    CHECK(audio_frame_acquire(&g_pool, &idx) == AUDIO_OK); /* one continuous channel across all ticks */

    uint32_t frames_emitted = 0;
    uint32_t draws_submitted_total = 0;
    uint32_t classifications_total = 0;

    for (uint32_t tick = 0; tick < STREAM_TICKS; tick++) {
        /* Simulate a continuous incoming raw PCM signal: a bounded
         * triangle wave, different every tick, never the same frame
         * twice -- exercises the FIR's cross-frame state on genuinely
         * changing input, not just a static constant. */
        audio_sample_t *in = audio_frame_input(&g_pool, idx);
        for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            const uint32_t phase = (tick * AUDIO_FRAME_SAMPLES + i) % 512u;
            const int32_t  tri   = (phase < 256u) ? (int32_t)phase : (int32_t)(511u - phase);
            in[i] = (audio_sample_t)((tri - 128) * 50); /* bounded, well within int16 range */
        }

        CHECK(audio_fir_filter_frame(&g_pool, idx) == AUDIO_OK); /* real dispatch: AVX2 if available, else scalar */

        /* "Transcription vectorielle locale", honnetement scopee (voir
         * audio_transcribe.h) : extraction Goertzel reelle -> SRAM de
         * l'acteur -> argmax deterministe. Aucune comprehension du
         * francais ni d'aucune langue n'est produite ici. */
        audio_feature_vec_t features;
        CHECK(audio_extract_features(audio_frame_output(&g_pool, idx), &features, AUDIO_FIR_AUTO) == AUDIO_OK);
        CHECK(audio_actor_store_features(inference_actor, &features) == ACTOR_OK);
        const uint8_t phoneme_class = audio_actor_infer_phoneme_class(inference_actor);
        CHECK(phoneme_class < AUDIO_PHONEME_CLASS_COUNT);
        classifications_total++;

        CHECK(audio_emit_frame_index(inference_actor, (uint16_t)tick, idx) == ACTOR_OK);
        frames_emitted++;

        /* One draw command per tick, tied to this frame's identity. */
        gpu_draw_cmd_t cmd = {.vertex_offset = tick * 3u, .vertex_count = 3u, .transform_id = tick, ._reserved = 0u};
        CHECK(gpu_cmd_queue_push(&g_cmd_ring, tick, &cmd) == GPU_OK);

        uint32_t submitted = 0;
        uint64_t fence_value = 0;
        CHECK(gpu_backend_submit_frame(&backend, &g_cmd_ring, 4u, &submitted, &fence_value) == GPU_OK);
        CHECK(submitted == 1u); /* exactly the one command pushed this tick -- queue never accumulates */
        draws_submitted_total += submitted;

        /* Drain the inference mailbox this tick too, mirroring a real
         * consumer keeping pace with the producer -- never let it grow
         * unbounded across ticks. */
        ring_msg_t msg;
        CHECK(actor_process_one(inference_actor, &msg) == ACTOR_OK);
        CHECK(msg.type == AUDIO_MSG_TYPE_FRAME_READY);
        uint32_t decoded_index;
        memcpy(&decoded_index, msg.payload, sizeof decoded_index);
        CHECK(decoded_index == idx);

        /* Injection zero-copy dans le repartiteur FSM (swarm_dispatch.h) :
         * mappage de demonstration UNIQUEMENT (arbitraire, local a ce
         * test) de la classe inferee vers un intent_code -- au tick
         * STREAM_HUMAN_REQUEST_TICK on simule explicitement la detection
         * d'une interaction critique, comme demande. `current_cycle` est
         * le VRAI compteur de cycles de cet acteur (actor_core.h),
         * lecture atomique implicite d'un uint64_t _Atomic. */
        /* La classe inferee elle-meme n'a pas de sens linguistique (voir le
         * header) -- le tick de declenchement est fixe explicitement ici
         * pour que ce test soit reproductible, plutot que de dependre
         * d'une classe particuliere sortant du classifieur de demo. */
        const uint8_t intent_code = (tick == STREAM_HUMAN_REQUEST_TICK) ? 4u /* SWARM_INTENT_HUMAN_REQUEST */
                                                                          : 1u /* SWARM_INTENT_INTEREST */;
        uint8_t lead_payload[SWARM_LEAD_HEADER_SIZE];
        memcpy(lead_payload, &tick, sizeof tick);
        lead_payload[4] = intent_code;
        lead_payload[5] = 0u;
        lead_payload[6] = 0u;
        const uint64_t current_cycle = inference_actor->processed_total;
        CHECK(swarm_ingest_ex(&swarm, fsm_tenant, lead_payload, sizeof lead_payload,
                               current_cycle, STREAM_FOLLOWUP_DELAY_CYCLES) == SWARM_OK);
        CHECK(swarm_tick(&swarm, fsm_tenant, current_cycle) == SWARM_OK);
    }

    CHECK(frames_emitted == STREAM_TICKS);
    CHECK(classifications_total == STREAM_TICKS);
    CHECK(draws_submitted_total == STREAM_TICKS);
    CHECK(actor_mailbox_count(inference_actor) == 0u); /* fully drained, tick by tick */
    CHECK(rb_is_empty(&g_cmd_ring) == true);

    /* Le drapeau d'alerte a bien ete leve par le tick "demande d'humain",
     * et le rappel programme (delai court expres) a fini par echoir et
     * declencher le hook UNE FOIS pendant la boucle -- one-shot, jamais
     * redeclenche par les ticks suivants. */
    CHECK(swarm_get_agent(&swarm, fsm_tenant)->alert_flag == true);
    CHECK(g_followup_fired == 1);

    printf("   ticks=%u frames_emitted=%u classifications=%u draws_submitted=%u "
           "alert_flag=%d followup_fired=%d (backend %s)\n",
           STREAM_TICKS, frames_emitted, classifications_total, draws_submitted_total,
           (int)swarm_get_agent(&swarm, fsm_tenant)->alert_flag, g_followup_fired,
           init_st == GPU_OK ? "live" : "no-adapter");

    gpu_backend_shutdown(&backend);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_audio_wasm_integration ===\n");

    test_layout();
    test_pool_acquire_release();
    test_fir_scalar_dc_settle();
    test_fir_streaming_continuity();
    test_fir_avx2_dispatch_is_safe_and_correct();
    test_feature_extraction_dispatch_safety();
    test_goertzel_unroll_bit_exact_and_timing();
    test_sram_inference();
    test_transcribed_frame_into_fsm_dispatch();
    test_zero_copy_emission();
    test_vertex_buffer_soa();
    test_cmd_queue_roundtrip();
    test_wasm_bridge_vertex_and_cmd_roundtrip();
    test_gpu_backend_and_frame_sync();
    test_continuous_audio_and_graphics_pipeline();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
