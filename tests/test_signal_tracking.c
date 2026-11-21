/*
 * test_signal_tracking.c - Unit + true multi-thread stress test bench
 * for signal_tracking.{h,c}: the I/Q noise-cleaning filter bank
 * (single-pole IIR smoothing + statistical outlier gating), Q21.10
 * fixed-point arithmetic, the 3D kinetic trajectory tracker (velocity
 * derived from consecutive position reports, exact linear
 * extrapolation), and an 8-thread concurrent stress test bombarding both
 * subsystems at once -- proving exact trajectory conservation under
 * concurrent, noisy, per-thread-isolated load.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching signal_tracking.c's own zero-allocation
 * constraint. Portable threading (#if _WIN32 CreateThread / else
 * pthread), same pattern as tests/test_barry_storage.c.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "signal_tracking.h"
#include "self_replicate.h"      /* self_replicate_couple_signal_anomaly(): Part E coupling into Niveau 41 */
#include "crypto_constant_time.h" /* ct_equal_u64(): constant-time authenticity check in the stress test below */

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
/* Part A: I/Q noise-cleaning filter bank                             */
/* ---------------------------------------------------------------- */

static void test_iq_filter_seeds_from_first_sample(void) {
    SECTION("iq_filter_sample: the first sample seeds the filter exactly, never flagged an outlier");
    iq_filter_bank_t bank;
    iq_filter_bank_init(&bank);

    iq_sample_t in = { 1000, -500 };
    iq_sample_t out;
    bool was_outlier = true;
    CHECK(iq_filter_sample(&bank, 0, in, &out, &was_outlier) == SIGTRACK_OK);
    CHECK(was_outlier == false);
    CHECK(out.i == 1000 && out.q == -500);
    CHECK(bank.channels[0].samples_processed == 1u);
}

static void test_iq_filter_rejects_outlier_exactly(void) {
    SECTION("iq_filter_sample: an extreme outlier is rejected -- filter state stays exactly at the clean value");
    iq_filter_bank_t bank;
    iq_filter_bank_init(&bank);

    iq_sample_t clean = { 2000, 1500 };
    iq_sample_t out;
    bool was_outlier;

    CHECK(iq_filter_sample(&bank, 5, clean, &out, &was_outlier) == SIGTRACK_OK); /* seed */
    CHECK(out.i == 2000 && out.q == 1500);

    for (int i = 0; i < 20; i++) {
        CHECK(iq_filter_sample(&bank, 5, clean, &out, &was_outlier) == SIGTRACK_OK);
        CHECK(was_outlier == false);
        CHECK(out.i == 2000 && out.q == 1500); /* diff == 0 every time: an exact fixed point of the recursion */
    }
    CHECK(bank.channels[5].outliers_rejected == 0u);

    iq_sample_t spike = { 30000, 30000 }; /* wildly implausible relative to the running mean */
    CHECK(iq_filter_sample(&bank, 5, spike, &out, &was_outlier) == SIGTRACK_OK);
    CHECK(was_outlier == true);
    CHECK(out.i == 2000 && out.q == 1500); /* replaced by the preserved filtered estimate, not the spike */
    CHECK(bank.channels[5].outliers_rejected == 1u);

    /* the filter state was never touched by the outlier: the next clean
     * sample is still exactly at the fixed point */
    CHECK(iq_filter_sample(&bank, 5, clean, &out, &was_outlier) == SIGTRACK_OK);
    CHECK(was_outlier == false);
    CHECK(out.i == 2000 && out.q == 1500);
}

static void test_iq_filter_rejects_bad_args(void) {
    SECTION("iq_filter_sample: rejects NULL/out-of-range arguments");
    iq_filter_bank_t bank;
    iq_filter_bank_init(&bank);
    iq_sample_t s = { 0, 0 };
    iq_sample_t out;
    CHECK(iq_filter_sample(NULL, 0, s, &out, NULL) == SIGTRACK_ERR_INVALID);
    CHECK(iq_filter_sample(&bank, 0, s, NULL, NULL) == SIGTRACK_ERR_INVALID);
    CHECK(iq_filter_sample(&bank, (uint32_t)IQ_MAX_CHANNELS, s, &out, NULL) == SIGTRACK_ERR_OUT_OF_RANGE);
}

/* ---------------------------------------------------------------- */
/* Part B: Q21.10 fixed-point arithmetic                              */
/* ---------------------------------------------------------------- */

static void test_fixed_point_arithmetic(void) {
    SECTION("fixed_from_int/fixed_to_int_trunc/fixed_mul: exact Q21.10 round trips");
    CHECK(fixed_from_int(0) == 0);
    CHECK(fixed_from_int(5) == 5 * FIXED_Q21_10_ONE);
    CHECK(fixed_to_int_trunc(fixed_from_int(42)) == 42);
    CHECK(fixed_to_int_trunc(fixed_from_int(-7)) == -7);

    fixed_q21_10_t two_point_five = (fixed_q21_10_t)(2 * FIXED_Q21_10_ONE + FIXED_Q21_10_ONE / 2); /* 2.5 */
    fixed_q21_10_t four = fixed_from_int(4);
    fixed_q21_10_t ten = fixed_from_int(10);
    CHECK(fixed_mul(two_point_five, four) == ten); /* 2.5 * 4.0 == 10.0, exactly */

    fixed_q21_10_t neg_three = fixed_from_int(-3);
    CHECK(fixed_mul(neg_three, four) == fixed_from_int(-12));
}

/* ---------------------------------------------------------------- */
/* Part C: 3D kinetic trajectory tracker                               */
/* ---------------------------------------------------------------- */

static void test_track_first_update_seeds_zero_velocity(void) {
    SECTION("track_update_position: the first report seeds position with zero velocity");
    track_registry_t reg;
    track_registry_init(&reg);

    CHECK(track_update_position(&reg, 0, fixed_from_int(10), fixed_from_int(20), fixed_from_int(30), 100u) == SIGTRACK_OK);
    CHECK(reg.vel_x[0] == 0 && reg.vel_y[0] == 0 && reg.vel_z[0] == 0);

    fixed_q21_10_t x, y, z;
    CHECK(track_predict_position(&reg, 0, 100u, &x, &y, &z) == SIGTRACK_OK);
    CHECK(x == fixed_from_int(10) && y == fixed_from_int(20) && z == fixed_from_int(30));
    CHECK(track_predict_position(&reg, 0, 500u, &x, &y, &z) == SIGTRACK_OK); /* zero velocity: stays put */
    CHECK(x == fixed_from_int(10) && y == fixed_from_int(20) && z == fixed_from_int(30));
}

static void test_track_velocity_derived_exactly(void) {
    SECTION("track_update_position: velocity DERIVED exactly from two position reports (data is positions only)");
    track_registry_t reg;
    track_registry_init(&reg);

    CHECK(track_update_position(&reg, 1, fixed_from_int(0), fixed_from_int(0), fixed_from_int(0), 0u) == SIGTRACK_OK);
    /* moves +50 in x, -20 in y, +5 in z over 10 opaque ticks */
    CHECK(track_update_position(&reg, 1, fixed_from_int(50), fixed_from_int(-20), fixed_from_int(5), 10u) == SIGTRACK_OK);

    CHECK(reg.vel_x[1] == fixed_from_int(5));       /* 50/10 == 5.0 */
    CHECK(reg.vel_y[1] == fixed_from_int(-2));      /* -20/10 == -2.0 */
    CHECK(reg.vel_z[1] == FIXED_Q21_10_ONE / 2);    /* 5/10 == 0.5 exactly representable in Q21.10 (512) */

    fixed_q21_10_t x, y, z;
    CHECK(track_predict_position(&reg, 1, 20u, &x, &y, &z) == SIGTRACK_OK); /* 10 ticks further */
    CHECK(x == fixed_from_int(100)); /* 50 + 5*10 */
    CHECK(y == fixed_from_int(-40)); /* -20 + (-2)*10 */

    /* backward extrapolation is equally exact and well-defined */
    CHECK(track_predict_position(&reg, 1, 0u, &x, &y, &z) == SIGTRACK_OK);
    CHECK(x == fixed_from_int(0) && y == fixed_from_int(0));
}

static void test_track_same_cycle_leaves_velocity_unchanged(void) {
    SECTION("track_update_position: dt == 0 (same opaque tick reported twice) leaves velocity untouched");
    track_registry_t reg;
    track_registry_init(&reg);
    CHECK(track_update_position(&reg, 2, fixed_from_int(0), fixed_from_int(0), fixed_from_int(0), 0u) == SIGTRACK_OK);
    CHECK(track_update_position(&reg, 2, fixed_from_int(100), fixed_from_int(0), fixed_from_int(0), 10u) == SIGTRACK_OK);
    fixed_q21_10_t vx_before = reg.vel_x[2];

    CHECK(track_update_position(&reg, 2, fixed_from_int(999), fixed_from_int(0), fixed_from_int(0), 10u) == SIGTRACK_OK);
    CHECK(reg.vel_x[2] == vx_before); /* unchanged despite the position jump: dt == 0 */
    CHECK(reg.pos_x[2] == fixed_from_int(999)); /* position itself is still updated */
}

static void test_track_update_overflow_leaves_state_unchanged(void) {
    SECTION("track_update_position: a Q21.10 velocity overflow is rejected, reg state left exactly as before");
    track_registry_t reg;
    track_registry_init(&reg);

    /* Seed at the most negative representable position. */
    CHECK(track_update_position(&reg, 10, INT32_MIN, 0, 0, 0u) == SIGTRACK_OK);
    fixed_q21_10_t pos_before = reg.pos_x[10];
    fixed_q21_10_t vel_before = reg.vel_x[10];
    uint64_t cycle_before = reg.last_update_cycle[10];

    /* Jump to the most positive representable position after a single
     * tick: the true delta (INT32_MAX - INT32_MIN) does not fit in
     * int32_t, so the derived velocity cannot either -- must be rejected,
     * not silently truncated. */
    CHECK(track_update_position(&reg, 10, INT32_MAX, 0, 0, 1u) == SIGTRACK_ERR_OVERFLOW);

    /* All-or-nothing: nothing about target 10 changed. */
    CHECK(reg.pos_x[10] == pos_before);
    CHECK(reg.vel_x[10] == vel_before);
    CHECK(reg.last_update_cycle[10] == cycle_before);
}

static void test_track_predict_overflow_leaves_outputs_untouched(void) {
    SECTION("track_predict_position: an extrapolation overflow is rejected, out params left untouched");
    track_registry_t reg;
    track_registry_init(&reg);

    /* A large but valid velocity, then a request far enough in the future
     * that pos + vel*dt cannot fit in int32_t -- dt (10) is kept small
     * enough that vel*dt itself stays well inside int64_t range, so only
     * the FINAL int32_t range check trips, never an int64_t overflow in
     * the intermediate multiplication. */
    CHECK(track_update_position(&reg, 11, fixed_from_int(0), fixed_from_int(0), fixed_from_int(0), 0u) == SIGTRACK_OK);
    CHECK(track_update_position(&reg, 11, fixed_from_int(1000000), fixed_from_int(0), fixed_from_int(0), 1u) == SIGTRACK_OK);

    fixed_q21_10_t x = 0x5A5A5A5A, y = 0x5A5A5A5A, z = 0x5A5A5A5A; /* sentinel */
    sigtrack_status_t st = track_predict_position(&reg, 11, 11u, &x, &y, &z);
    CHECK(st == SIGTRACK_ERR_OVERFLOW);
    CHECK(x == (fixed_q21_10_t)0x5A5A5A5A); /* untouched, not a partial/garbage write */
    CHECK(y == (fixed_q21_10_t)0x5A5A5A5A);
    CHECK(z == (fixed_q21_10_t)0x5A5A5A5A);
}

static void test_track_rejects_backward_ticks_and_bad_args(void) {
    SECTION("track_update_position/predict_position: rejects decreasing ticks, out-of-range/unknown targets");
    track_registry_t reg;
    track_registry_init(&reg);
    CHECK(track_update_position(&reg, 3, fixed_from_int(0), 0, 0, 10u) == SIGTRACK_OK);
    CHECK(track_update_position(&reg, 3, fixed_from_int(1), 0, 0, 5u) == SIGTRACK_ERR_INVALID); /* 5 < 10 */

    CHECK(track_update_position(&reg, (uint32_t)TRACK_MAX_TARGETS, 0, 0, 0, 0u) == SIGTRACK_ERR_OUT_OF_RANGE);

    fixed_q21_10_t x, y, z;
    CHECK(track_predict_position(&reg, 4, 0u, &x, &y, &z) == SIGTRACK_ERR_NOT_FOUND); /* never updated */
    CHECK(track_predict_position(&reg, (uint32_t)TRACK_MAX_TARGETS, 0u, &x, &y, &z) == SIGTRACK_ERR_OUT_OF_RANGE);
}

/* ---------------------------------------------------------------- */
/* 8-thread concurrent stress: noisy I/Q + per-thread-isolated         */
/* trajectories, proving exact conservation under concurrent load      */
/* ---------------------------------------------------------------- */

#define TARGETS_PER_THREAD  (TRACK_MAX_TARGETS / MAX_THREADS)
#define TRACK_STEPS         200u
#define TRACK_STEP_TICKS    4u
#define IQ_ITERATIONS       2000u
#define IQ_OUTLIER_PERIOD   17u

typedef struct {
    int thread_id;
    _Atomic uint32_t *mismatch_count;
    track_registry_t *tracks;
    iq_filter_bank_t  *iq_bank;
} stress_args_t;

/* Constant-time equality check for a computed fixed_q21_10_t against its
 * expected value -- reuses crypto_constant_time.h's ct_equal_u64() (this
 * codebase's one audited word-equality primitive) instead of a bespoke
 * `==`, the same "every identifier/secret comparison goes through the
 * same audited primitive" posture that header documents for itself.
 * There is no secret here (both operands are derived from this test's own
 * known inputs) -- this proves the primitive composes cleanly as the
 * authenticity/integrity check for trajectory results computed under
 * concurrent, per-thread-isolated load, which is what this stress test is
 * for. Sign-extending to int64_t before the uint64_t cast preserves value
 * (not just bit pattern) equality for negative fixed_q21_10_t values. */
static bool fixed_ct_equal(fixed_q21_10_t actual, fixed_q21_10_t expected) {
    return ct_equal_u64((uint64_t)(int64_t)actual, (uint64_t)(int64_t)expected) != 0;
}

static THREAD_FN(signal_stress_thread) {
    stress_args_t *args = (stress_args_t *)arg;
    uint32_t base_target = (uint32_t)args->thread_id * TARGETS_PER_THREAD;
    uint32_t channel_id = (uint32_t)args->thread_id;

    /* --- trajectory side: each target in this thread's own disjoint
     * range moves at a distinct, exactly-representable constant velocity --- */
    for (uint32_t local = 0; local < TARGETS_PER_THREAD; local++) {
        uint32_t target_id = base_target + local;
        int32_t vx = (int32_t)(target_id + 1u);
        int32_t vy = (int32_t)(target_id + 2u);
        int32_t vz = (int32_t)(target_id + 3u);

        for (uint32_t step = 0; step <= TRACK_STEPS; step++) {
            uint64_t cycle = (uint64_t)step * TRACK_STEP_TICKS;
            fixed_q21_10_t x = fixed_from_int(vx * (int32_t)cycle);
            fixed_q21_10_t y = fixed_from_int(vy * (int32_t)cycle);
            fixed_q21_10_t z = fixed_from_int(vz * (int32_t)cycle);
            if (track_update_position(args->tracks, target_id, x, y, z, cycle) != SIGTRACK_OK) {
                atomic_fetch_add_explicit(args->mismatch_count, 1u, memory_order_relaxed);
            }
        }

        if (!fixed_ct_equal(args->tracks->vel_x[target_id], fixed_from_int(vx)) ||
            !fixed_ct_equal(args->tracks->vel_y[target_id], fixed_from_int(vy)) ||
            !fixed_ct_equal(args->tracks->vel_z[target_id], fixed_from_int(vz))) {
            atomic_fetch_add_explicit(args->mismatch_count, 1u, memory_order_relaxed);
        }

        uint64_t future_cycle = (uint64_t)(TRACK_STEPS + 5u) * TRACK_STEP_TICKS;
        fixed_q21_10_t px, py, pz;
        if (track_predict_position(args->tracks, target_id, future_cycle, &px, &py, &pz) != SIGTRACK_OK ||
            !fixed_ct_equal(px, fixed_from_int(vx * (int32_t)future_cycle)) ||
            !fixed_ct_equal(py, fixed_from_int(vy * (int32_t)future_cycle)) ||
            !fixed_ct_equal(pz, fixed_from_int(vz * (int32_t)future_cycle))) {
            atomic_fetch_add_explicit(args->mismatch_count, 1u, memory_order_relaxed);
        }
    }

    /* --- I/Q side: this thread's own channel, bombarded with a
     * constant clean signal plus periodic extreme outliers --- */
    iq_sample_t clean = { (int16_t)(100 + args->thread_id), (int16_t)(-100 - args->thread_id) };
    uint32_t expected_outliers = 0u;
    for (uint32_t i = 0; i < IQ_ITERATIONS; i++) {
        iq_sample_t in = clean;
        bool inject_outlier = (i % IQ_OUTLIER_PERIOD == 0u) && (i > 0u);
        if (inject_outlier) {
            in.i = 32000;
            in.q = -32000;
            expected_outliers++;
        }
        iq_sample_t out;
        bool was_outlier;
        if (iq_filter_sample(args->iq_bank, channel_id, in, &out, &was_outlier) != SIGTRACK_OK ||
            was_outlier != inject_outlier ||
            out.i != clean.i || out.q != clean.q) {
            atomic_fetch_add_explicit(args->mismatch_count, 1u, memory_order_relaxed);
        }
    }
    if (args->iq_bank->channels[channel_id].outliers_rejected != expected_outliers) {
        atomic_fetch_add_explicit(args->mismatch_count, 1u, memory_order_relaxed);
    }

#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_signal_bombardment_conserves_trajectories(void) {
    SECTION("8 threads bombarding noisy I/Q + isolated trajectories concurrently: exact conservation, 0 mismatch");
    static track_registry_t tracks; /* large-ish: static, not stack */
    static iq_filter_bank_t iq_bank;
    track_registry_init(&tracks);
    iq_filter_bank_init(&iq_bank);

    _Atomic uint32_t mismatch_count = 0;
    stress_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].thread_id = t;
        args[t].mismatch_count = &mismatch_count;
        args[t].tracks = &tracks;
        args[t].iq_bank = &iq_bank;
        threads[t] = thread_start(signal_stress_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    CHECK(atomic_load_explicit(&mismatch_count, memory_order_relaxed) == 0u);
}

/* ---------------------------------------------------------------- */
/* Batch entry point: iq_filter_bank_process_all() (unrolled x4 over the  */
/* independent channel axis)                                              */
/* ---------------------------------------------------------------- */

static void test_iq_filter_bank_process_all_matches_per_channel_calls(void) {
    SECTION("iq_filter_bank_process_all: unrolled x4 batch is bit-identical to calling iq_filter_sample() per channel");

    /* Reference bank: one iq_filter_sample() call per channel, in order. */
    iq_filter_bank_t reference_bank;
    iq_filter_bank_init(&reference_bank);
    iq_sample_t samples[IQ_MAX_CHANNELS];
    iq_sample_t reference_clean[IQ_MAX_CHANNELS];
    bool reference_outlier[IQ_MAX_CHANNELS];
    for (uint32_t ch = 0; ch < IQ_MAX_CHANNELS; ch++) {
        /* Every 5th channel gets a wildly implausible sample; the rest get
         * a distinct, per-channel clean value -- exercises both the
         * smoothing path and the outlier-gating path across the batch. */
        bool make_outlier = (ch % 5u == 0u) && (ch != 0u); /* channel 0 seeds first: never an outlier */
        samples[ch].i = make_outlier ? (int16_t)30000 : (int16_t)(100 + (int16_t)ch);
        samples[ch].q = make_outlier ? (int16_t)(-30000) : (int16_t)(-50 - (int16_t)ch);
        CHECK(iq_filter_sample(&reference_bank, ch, samples[ch], &reference_clean[ch], &reference_outlier[ch]) == SIGTRACK_OK);
    }

    /* Batch bank: identical inputs, one iq_filter_bank_process_all() call. */
    iq_filter_bank_t batch_bank;
    iq_filter_bank_init(&batch_bank);
    iq_sample_t batch_clean[IQ_MAX_CHANNELS];
    bool batch_outlier[IQ_MAX_CHANNELS];
    iq_filter_bank_process_all(&batch_bank, samples, batch_clean, batch_outlier);

    for (uint32_t ch = 0; ch < IQ_MAX_CHANNELS; ch++) {
        CHECK(batch_clean[ch].i == reference_clean[ch].i);
        CHECK(batch_clean[ch].q == reference_clean[ch].q);
        CHECK(batch_outlier[ch] == reference_outlier[ch]);

        /* Compare internal per-channel state field-by-field, not via a
         * whole-struct memcmp: iq_channel_state_t has real trailing
         * padding after `initialized` (int32+int32+uint64+uint32+uint32+
         * bool does not sum to a multiple of the struct's own 8-byte
         * alignment), and iq_filter_bank_init() only sets the named
         * fields -- that padding is indeterminate stack content on two
         * separately-declared local banks, exactly the
         * SESSION_NOTES.md-documented audio_feature_vec_t class of bug a
         * whole-struct memcmp would silently reintroduce here. */
        const iq_channel_state_t *b = &batch_bank.channels[ch];
        const iq_channel_state_t *r = &reference_bank.channels[ch];
        CHECK(b->i_filtered_q8 == r->i_filtered_q8);
        CHECK(b->q_filtered_q8 == r->q_filtered_q8);
        CHECK(b->mag_sq_mean_q8 == r->mag_sq_mean_q8);
        CHECK(b->samples_processed == r->samples_processed);
        CHECK(b->outliers_rejected == r->outliers_rejected);
        CHECK(b->initialized == r->initialized);
    }
}

/* ---------------------------------------------------------------- */
/* Part E (self_replicate.h): signal anomaly -> self-heal coupling        */
/* ---------------------------------------------------------------- */

static void test_couple_signal_anomaly_noop_without_anomaly(void) {
    SECTION("self_replicate_couple_signal_anomaly: no anomaly reported -> no-op, private_memory untouched");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 6u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 6u) == SELF_REPLICATE_OK);

    actor_t *actor = actor_system_get(&sys, 6u);
    CHECK(actor != NULL);
    memset(actor->private_memory, 0xCD, ACTOR_PRIVATE_MEM_SIZE);

    CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 6u, false, SIGTRACK_OK) == SELF_REPLICATE_OK);

    uint8_t marker[ACTOR_PRIVATE_MEM_SIZE];
    memset(marker, 0xCD, sizeof marker);
    CHECK(memcmp(actor->private_memory, marker, ACTOR_PRIVATE_MEM_SIZE) == 0); /* untouched */
    CHECK(atomic_load_explicit(&reg.sandboxes[6].bounds_violation_count, memory_order_relaxed) == 0u);
    CHECK(atomic_load_explicit(&reg.sandboxes[6].heals_total, memory_order_relaxed) == 0u);
}

static void test_couple_signal_anomaly_iq_outlier_heals_after_threshold(void) {
    SECTION("self_replicate_couple_signal_anomaly: repeated I/Q outliers cross the threshold -> real memset erasure");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 7u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 7u) == SELF_REPLICATE_OK);

    actor_t *actor = actor_system_get(&sys, 7u);
    CHECK(actor != NULL);
    memset(actor->private_memory, 0xEF, ACTOR_PRIVATE_MEM_SIZE); /* "possibly corrupted" state to erase */

    /* Below threshold: still a no-op. */
    for (uint32_t i = 0; i + 1u < SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD; i++) {
        CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 7u, true, SIGTRACK_OK) == SELF_REPLICATE_OK);
    }
    CHECK(atomic_load_explicit(&reg.sandboxes[7].heals_total, memory_order_relaxed) == 0u);

    /* The threshold-crossing call actually heals. */
    CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 7u, true, SIGTRACK_OK) == SELF_REPLICATE_OK);

    uint8_t zero[ACTOR_PRIVATE_MEM_SIZE];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(actor->private_memory, zero, ACTOR_PRIVATE_MEM_SIZE) == 0); /* real structural erasure */
    CHECK(atomic_load_explicit(&reg.sandboxes[7].heals_total, memory_order_relaxed) == 1u);
    CHECK(atomic_load_explicit(&reg.sandboxes[7].bounds_violation_count, memory_order_relaxed) == 0u); /* reset */
}

static void test_couple_signal_anomaly_track_overflow_heals_after_threshold(void) {
    SECTION("self_replicate_couple_signal_anomaly: repeated Q21.10 overflows cross the threshold -> real memset erasure");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 5u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 5u) == SELF_REPLICATE_OK);

    actor_t *actor = actor_system_get(&sys, 5u);
    CHECK(actor != NULL);
    memset(actor->private_memory, 0x11, ACTOR_PRIVATE_MEM_SIZE);

    for (uint32_t i = 0; i < SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD; i++) {
        CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 5u, false, SIGTRACK_ERR_OVERFLOW) == SELF_REPLICATE_OK);
    }

    uint8_t zero[ACTOR_PRIVATE_MEM_SIZE];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(actor->private_memory, zero, ACTOR_PRIVATE_MEM_SIZE) == 0);
    CHECK(atomic_load_explicit(&reg.sandboxes[5].heals_total, memory_order_relaxed) == 1u);
}

/* ---------------------------------------------------------------- */

int main(void) {
    test_iq_filter_seeds_from_first_sample();
    test_iq_filter_rejects_outlier_exactly();
    test_iq_filter_rejects_bad_args();

    test_fixed_point_arithmetic();

    test_track_first_update_seeds_zero_velocity();
    test_track_velocity_derived_exactly();
    test_track_same_cycle_leaves_velocity_unchanged();
    test_track_update_overflow_leaves_state_unchanged();
    test_track_predict_overflow_leaves_outputs_untouched();
    test_track_rejects_backward_ticks_and_bad_args();

    test_concurrent_signal_bombardment_conserves_trajectories();

    test_iq_filter_bank_process_all_matches_per_channel_calls();

    test_couple_signal_anomaly_noop_without_anomaly();
    test_couple_signal_anomaly_iq_outlier_heals_after_threshold();
    test_couple_signal_anomaly_track_overflow_heals_after_threshold();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
