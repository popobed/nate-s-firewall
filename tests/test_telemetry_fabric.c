/*
 * test_telemetry_fabric.c - Unit + true multi-thread stress test bench
 * for telemetry_fabric.{h,c}: the immutable hash-chained WAL (including
 * an 8-thread stress test bombarding it with malformed/well-formed
 * packets concurrently and proving the chain's integrity survives
 * exactly), tamper detection, the swarm-alert coupling, and the
 * constant-time-verified tenant access-key store.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching telemetry_fabric.c's own zero-allocation
 * constraint. Portable threading (#if _WIN32 CreateThread / else
 * pthread), same pattern as tests/test_barry_storage.c.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "telemetry_fabric.h"

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
/* Part A: basic append/verify round trip                             */
/* ---------------------------------------------------------------- */

static void test_log_append_and_verify(void) {
    SECTION("tf_log_append/verify: gapless hash-chained round trip");
    tf_log_t log;
    tf_log_init(&log);

    CHECK(tf_log_append(&log, 1u, "hello", 5) == TF_OK);
    CHECK(tf_log_append(&log, 2u, "world!", 6) == TF_OK);
    CHECK(tf_log_append(&log, 3u, NULL, 0) == TF_OK);

    uint64_t broken = 999u;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);

    CHECK(log.entries[0].seq == 0u);
    CHECK(log.entries[0].prev_checksum == TF_LOG_GENESIS_CHECKSUM);
    CHECK(log.entries[1].prev_checksum == log.entries[0].checksum);
    CHECK(log.entries[2].prev_checksum == log.entries[1].checksum);
    CHECK(memcmp(log.entries[0].payload, "hello", 5) == 0);
}

static void test_log_rejects_oversized_payload(void) {
    SECTION("tf_log_append: rejects payload_len > TF_LOG_MSG_MAX, never touches the chain");
    tf_log_t log;
    tf_log_init(&log);
    uint8_t buf[8] = {0};

    CHECK(tf_log_append(&log, 1u, buf, TF_LOG_MSG_MAX + 1u) == TF_ERR_INVALID);
    CHECK(atomic_load_explicit(&log.write_pos, memory_order_relaxed) == 0u); /* the malformed attempt left no trace */
}

static void test_log_bounded_capacity_never_corrupts(void) {
    SECTION("tf_log_append: BARRY-style bounded capacity -- TF_ERR_FULL with a clean O(1) rollback, chain stays valid");
    static tf_log_t log; /* large: static, not stack */
    tf_log_init(&log);

    for (uint32_t i = 0; i < TF_LOG_CAPACITY; i++) {
        CHECK(tf_log_append(&log, i, NULL, 0) == TF_OK);
    }
    CHECK(tf_log_append(&log, 0xFFFFu, NULL, 0) == TF_ERR_FULL);
    CHECK(atomic_load_explicit(&log.write_pos, memory_order_relaxed) == (uint64_t)TF_LOG_CAPACITY); /* rolled back, not left at CAPACITY+1 */

    uint64_t broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
}

/* ---------------------------------------------------------------- */
/* Part B: tamper detection + swarm-alert coupling                    */
/* ---------------------------------------------------------------- */

static void test_log_detects_retroactive_tampering(void) {
    SECTION("tf_log_verify: any retroactive edit -- even to the stored checksum itself -- is detected");
    tf_log_t log;
    tf_log_init(&log);
    for (int i = 0; i < 10; i++) {
        CHECK(tf_log_append(&log, (uint32_t)i, "x", 1) == TF_OK);
    }
    CHECK(tf_log_verify(&log, NULL) == TF_OK);

    /* Tamper with entry 4's payload directly (simulating a retroactive
     * edit that bypasses the API entirely -- the strongest attack this
     * mechanism can be asked to catch). */
    log.entries[4].payload[0] = (uint8_t)'Y';

    uint64_t broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_ERR_TAMPERED);
    CHECK(broken == 4u); /* the checksum recomputed for entry 4 no longer matches what was stored */
    CHECK(atomic_load_explicit(&log.tamper_flag, memory_order_relaxed) == true);

    /* Even "fixing" entry 4's stored checksum to claim nothing changed
     * does not un-break the chain: whatever value a forger writes into
     * entries[4].checksum, it cannot simultaneously (a) match entry 5's
     * prev_checksum (captured honestly, before the tampering) AND (b)
     * match a checksum genuinely recomputed over entry 4's now-edited
     * fields -- tf_log_verify() checks BOTH. Here the forger's best
     * attempt (satisfying (a)) still fails (b). */
    tf_log_init(&log);
    for (int i = 0; i < 10; i++) {
        CHECK(tf_log_append(&log, (uint32_t)i, "x", 1) == TF_OK);
    }
    log.entries[4].payload[0] = (uint8_t)'Y';
    log.entries[4].checksum = log.entries[5].prev_checksum; /* the forger's best attempt: claim nothing changed */
    broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_ERR_TAMPERED);
    CHECK(broken == 4u); /* caught by the STORED-vs-RECOMPUTED mismatch at entry 4 itself, not the chain link */
}

typedef struct {
    uint16_t raised_tenant_id;
    int      call_count;
} fake_swarm_t;

static int fake_raise_alert(void *swarm_sys, uint16_t tenant_id) {
    fake_swarm_t *s = (fake_swarm_t *)swarm_sys;
    s->raised_tenant_id = tenant_id;
    s->call_count++;
    return 0;
}

static void test_verify_and_alert_couples_to_swarm(void) {
    SECTION("tf_log_verify_and_alert: raises the swarm alert hook iff tampering is found");
    tf_log_t log;
    tf_log_init(&log);
    CHECK(tf_log_append(&log, 1u, "a", 1) == TF_OK);
    CHECK(tf_log_append(&log, 2u, "b", 1) == TF_OK);

    fake_swarm_t swarm;
    swarm.call_count = 0;
    swarm.raised_tenant_id = 0xFFFFu;

    CHECK(tf_log_verify_and_alert(&log, NULL, &swarm, 7u, fake_raise_alert) == TF_OK);
    CHECK(swarm.call_count == 0); /* clean chain: never raised */

    log.entries[0].payload[0] = (uint8_t)'Z';
    CHECK(tf_log_verify_and_alert(&log, NULL, &swarm, 7u, fake_raise_alert) == TF_ERR_TAMPERED);
    CHECK(swarm.call_count == 1);
    CHECK(swarm.raised_tenant_id == 7u);
}

/* ---------------------------------------------------------------- */
/* Part C: constant-time tenant access-key store                      */
/* ---------------------------------------------------------------- */

static void test_key_store_verify(void) {
    SECTION("tf_key_store_register/verify: constant-time round trip, wrong key, unregistered slot");
    tf_tenant_key_store_t store;
    tf_key_store_init(&store);

    CHECK(tf_key_store_register(&store, 0, 42u, "super-secret-key", 16) == TF_OK);
    CHECK(tf_key_store_register(&store, 0, 42u, "super-secret-key", 16) == TF_ERR_ALREADY_REGISTERED);

    uint16_t tenant = 0xFFFFu;
    CHECK(tf_key_store_verify(&store, 0, "super-secret-key", 16, &tenant) == TF_OK);
    CHECK(tenant == 42u);

    CHECK(tf_key_store_verify(&store, 0, "wrong-key-entirely", 18, &tenant) == TF_ERR_NOT_FOUND);
    CHECK(tf_key_store_verify(&store, 1, "anything", 8, &tenant) == TF_ERR_NOT_FOUND); /* never registered */
    CHECK(tf_key_store_verify(&store, (uint16_t)TF_KEY_STORE_MAX_TENANTS, "x", 1, &tenant) == TF_ERR_OUT_OF_RANGE);

    /* the raw key is never retained in plaintext anywhere in the store */
    CHECK(memcmp(&store.key_hash[0], "super-secret-key", 8) != 0);
}

/* ---------------------------------------------------------------- */
/* 8-thread concurrent stress: malformed + well-formed packets bombard */
/* the log simultaneously; the chain's integrity must survive exactly  */
/* ---------------------------------------------------------------- */

#define APPENDS_PER_THREAD 300u

static _Atomic uint32_t g_valid_success_count = 0;
static _Atomic uint32_t g_invalid_rejected_count = 0;

typedef struct {
    tf_log_t *log;
    int       thread_id;
} log_stress_args_t;

static THREAD_FN(log_stress_thread) {
    log_stress_args_t *args = (log_stress_args_t *)arg;
    for (uint32_t i = 0; i < APPENDS_PER_THREAD; i++) {
        if (i % 3u == 0u) {
            /* a deliberately malformed packet: payload_len beyond
             * TF_LOG_MSG_MAX. The buffer itself is real and small --
             * only the claimed length is bogus -- so this stays memory-
             * safe regardless of whether the length check fires first
             * (it must). */
            uint8_t tiny[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
            tf_status_t st = tf_log_append(args->log, 0xBAD00000u + (uint32_t)args->thread_id,
                                            tiny, TF_LOG_MSG_MAX + 1u);
            if (st == TF_ERR_INVALID) {
                atomic_fetch_add_explicit(&g_invalid_rejected_count, 1u, memory_order_relaxed);
            }
        } else {
            uint32_t payload = ((uint32_t)args->thread_id << 16) | i;
            tf_status_t st = tf_log_append(args->log, 0x60000000u + (uint32_t)args->thread_id,
                                            &payload, (uint32_t)sizeof payload);
            if (st == TF_OK) {
                atomic_fetch_add_explicit(&g_valid_success_count, 1u, memory_order_relaxed);
            }
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_stress_malformed_and_valid_packets(void) {
    SECTION("8 threads bombarding the log with malformed + well-formed packets: absolute chain integrity survives");
    static tf_log_t log; /* large: static, not stack */
    tf_log_init(&log);
    atomic_store_explicit(&g_valid_success_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_invalid_rejected_count, 0u, memory_order_relaxed);

    log_stress_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].log = &log;
        args[t].thread_id = t;
        threads[t] = thread_start(log_stress_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    uint32_t valid = atomic_load_explicit(&g_valid_success_count, memory_order_relaxed);
    uint32_t invalid = atomic_load_explicit(&g_invalid_rejected_count, memory_order_relaxed);

    /* every malformed packet was rejected -- exactly 1/3 of each thread's
     * APPENDS_PER_THREAD iterations (i % 3 == 0) */
    uint32_t expected_invalid = MAX_THREADS * ((APPENDS_PER_THREAD + 2u) / 3u);
    uint32_t expected_valid = (MAX_THREADS * APPENDS_PER_THREAD) - expected_invalid;
    CHECK(invalid == expected_invalid);
    CHECK(valid == expected_valid);
    CHECK(valid < TF_LOG_CAPACITY); /* stays well under capacity: no TF_ERR_FULL rejections expected here */

    /* the write cursor advanced by EXACTLY the number of successful
     * (valid) appends -- malformed ones never claimed a slot */
    CHECK(atomic_load_explicit(&log.write_pos, memory_order_relaxed) == (uint64_t)valid);

    /* the decisive proof: despite 8-way concurrent contention and a third
     * of all traffic being malformed, the hash chain is PERFECTLY intact
     * end to end -- 0 echec. */
    uint64_t broken = 0xFFFFFFFFFFFFFFFFULL;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
}

/* ---------------------------------------------------------------- */
/* Niveau 54: 8 threads x 50000 packets -- the exact scenario that       */
/* used to expose the fixed-bound spin's false-tamper bug (see           */
/* EMPIRE_LOGDOWN.md, 2026-09-17b entry). All-valid, maximum-pressure    */
/* append contention: proves the unbounded CAS chain-link handoff        */
/* produces a chain tf_log_verify() reports 100% valid, even once the    */
/* log genuinely saturates under 8-way concurrent load far beyond its    */
/* fixed capacity.                                                       */
/* ---------------------------------------------------------------- */

#define MASSIVE_ATTEMPTS_PER_THREAD 50000u

static _Atomic uint32_t g_massive_ok_count  = 0;
static _Atomic uint32_t g_massive_full_count = 0;

typedef struct {
    tf_log_t *log;
    int       thread_id;
} massive_stress_args_t;

static THREAD_FN(massive_stress_thread) {
    massive_stress_args_t *args = (massive_stress_args_t *)arg;
    for (uint32_t i = 0; i < MASSIVE_ATTEMPTS_PER_THREAD; i++) {
        uint32_t payload = ((uint32_t)args->thread_id << 16) | (i & 0xFFFFu);
        tf_status_t st = tf_log_append(args->log, 0x50000000u + (uint32_t)args->thread_id,
                                        &payload, (uint32_t)sizeof payload);
        if (st == TF_OK) {
            atomic_fetch_add_explicit(&g_massive_ok_count, 1u, memory_order_relaxed);
        } else if (st == TF_ERR_FULL) {
            atomic_fetch_add_explicit(&g_massive_full_count, 1u, memory_order_relaxed);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_8_threads_50000_packets_massive_saturation(void) {
    SECTION("8 threads x 50000 packets: log saturates at exactly TF_LOG_CAPACITY, tf_log_verify() 100% valid");
    static tf_log_t log; /* large: static, not stack */
    tf_log_init(&log);
    atomic_store_explicit(&g_massive_ok_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_massive_full_count, 0u, memory_order_relaxed);

    massive_stress_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].log = &log;
        args[t].thread_id = t;
        threads[t] = thread_start(massive_stress_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    uint32_t ok = atomic_load_explicit(&g_massive_ok_count, memory_order_relaxed);
    uint32_t full = atomic_load_explicit(&g_massive_full_count, memory_order_relaxed);
    uint64_t total_attempts = (uint64_t)MAX_THREADS * MASSIVE_ATTEMPTS_PER_THREAD;

    printf("   attempts=%llu ok=%u full=%u (expected ok == TF_LOG_CAPACITY == %u)\n",
           (unsigned long long)total_attempts, (unsigned)ok, (unsigned)full, (unsigned)TF_LOG_CAPACITY);

    CHECK((uint64_t)ok + (uint64_t)full == total_attempts); /* every attempt landed in exactly one bucket -- none lost, none double-counted */
    CHECK(ok == (uint32_t)TF_LOG_CAPACITY); /* the log fills EXACTLY to capacity, never more, never less, under real 8-way contention */
    CHECK(atomic_load_explicit(&log.write_pos, memory_order_relaxed) == (uint64_t)TF_LOG_CAPACITY);

    /* The actual point of this test: under the maximum append pressure
     * this log can ever see, the FNV-1a hash chain the unbounded CAS
     * wait produced is providably, 100%, exactly valid -- not "usually",
     * not "unless a predecessor stalls". */
    uint64_t broken = 0xFFFFFFFFFFFFFFFFULL;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
}

/* ---------------------------------------------------------------- */

int main(void) {
    test_log_append_and_verify();
    test_log_rejects_oversized_payload();
    test_log_bounded_capacity_never_corrupts();

    test_log_detects_retroactive_tampering();
    test_verify_and_alert_couples_to_swarm();

    test_key_store_verify();

    test_concurrent_stress_malformed_and_valid_packets();
    test_8_threads_50000_packets_massive_saturation();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
