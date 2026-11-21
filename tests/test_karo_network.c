/*
 * test_karo_network.c - Unit + true multi-thread stress test bench for
 * net_io.{h,c}'s Niveau 53 addition: per-session (wire tenant_id) chat
 * rate limiting (net_io_wire_admit_tenant_message(), token_bucket_t,
 * rate_limit.h, reused verbatim) and its telemetry_fabric.c audit trail.
 *
 * Two concerns are tested SEPARATELY, deliberately, rather than in one
 * conflated mega-test:
 *   - The rate limiter's own concurrency correctness: token_bucket_t's
 *     CAS loop uses memory_order_relaxed throughout (rate_limit.c) --
 *     correct and sufficient here because a token count is the ONLY
 *     state being synchronized, nothing else needs publishing through
 *     it. That is NOT a release/acquire pair, and this file does not
 *     mislabel it as one.
 *   - The downstream handoff an ADMITTED message takes (wire_ring_reserve()/
 *     wire_ring_publish()/wire_ring_try_pop(), wire_proto.c) IS where
 *     release/acquire genuinely applies -- already proven correct in
 *     tests/test_wire_proto.c in isolation; this file proves it still
 *     holds when driven through THIS session's new admit-gate -> reserve
 *     -> publish call sequence, under real concurrent producer/consumer
 *     load.
 *
 * Windows-only (net_io.h itself #errors on any other platform) -- same
 * posture as tests/test_net_io.c. No dynamic allocation anywhere in this
 * file: all storage is static or on the stack.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_io.h"
#include <windows.h>

/* ---------------------------------------------------------------- */
/* Minimal test harness (same shape as tests/test_net_io.c)           */
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

#define POLL_SPIN_LIMIT 20000000 /* bounded spin, no Sleep() -- see test_net_io.c's own note */

/* ================================================================== */
/* Unit tests: net_io_wire_admit_tenant_message()                       */
/* ================================================================== */

static void test_admit_rejects_invalid_args(void) {
    SECTION("net_io_wire_admit_tenant_message: rejects NULL srv and an out-of-range tenant_id");
    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);

    CHECK(net_io_wire_admit_tenant_message(NULL, 0u) == false);
    CHECK(net_io_wire_admit_tenant_message(&srv, (uint16_t)NET_IO_WIRE_MAX_TENANTS) == false);

    net_io_wire_shutdown(&srv);
}

static void test_admit_exact_capacity_then_rejects(void) {
    SECTION("net_io_wire_admit_tenant_message: admits exactly 500, the 501st is rejected, budget unchanged by the reject");
    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);

    const uint16_t tenant = 7u;
    uint32_t admitted = 0;
    for (uint32_t i = 0; i < (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN; i++) {
        if (net_io_wire_admit_tenant_message(&srv, tenant)) {
            admitted++;
        }
    }
    CHECK(admitted == (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN);
    CHECK(token_bucket_available(&srv.rate_limiters[tenant]) == 0);

    CHECK(net_io_wire_admit_tenant_message(&srv, tenant) == false); /* the 501st */
    CHECK(token_bucket_available(&srv.rate_limiters[tenant]) == 0); /* a rejection never touches the balance */

    net_io_wire_stats_t stats;
    net_io_wire_get_stats(&srv, &stats);
    CHECK(stats.rejected_rate_limited == 1u);

    /* A DIFFERENT tenant's bucket is untouched -- per-session, not global. */
    CHECK(token_bucket_available(&srv.rate_limiters[8u]) == (int64_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN);

    net_io_wire_shutdown(&srv);
}

static void test_refill_restores_budget_capped_at_capacity(void) {
    SECTION("net_io_wire_refill_rate_limits: restores budget, capped at NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN, per tenant");
    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);

    const uint16_t tenant = 3u;
    for (uint32_t i = 0; i < (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN; i++) {
        CHECK(net_io_wire_admit_tenant_message(&srv, tenant));
    }
    CHECK(net_io_wire_admit_tenant_message(&srv, tenant) == false);

    net_io_wire_refill_rate_limits(&srv, 10);
    CHECK(token_bucket_available(&srv.rate_limiters[tenant]) == 10);
    for (int i = 0; i < 10; i++) {
        CHECK(net_io_wire_admit_tenant_message(&srv, tenant));
    }
    CHECK(net_io_wire_admit_tenant_message(&srv, tenant) == false); /* exhausted again */

    /* Massive over-refill still caps at capacity, never grants a burst
     * beyond the configured 500/min ceiling. */
    net_io_wire_refill_rate_limits(&srv, 999999);
    CHECK(token_bucket_available(&srv.rate_limiters[tenant]) == (int64_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN);

    /* Refill is global (every tenant), not just the one exercised above. */
    CHECK(token_bucket_available(&srv.rate_limiters[42u]) == (int64_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN);

    net_io_wire_shutdown(&srv);
}

static void test_rejection_is_logged_to_telemetry_fnv1a(void) {
    SECTION("net_io_wire_admit_tenant_message: a rejection appends one FNV-1a-checksummed telemetry_fabric.c entry");
    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);
    tf_log_t audit_log;
    tf_log_init(&audit_log);
    net_io_wire_set_audit_log(&srv, &audit_log);

    const uint16_t tenant = 9u;
    for (uint32_t i = 0; i < (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN; i++) {
        CHECK(net_io_wire_admit_tenant_message(&srv, tenant));
    }
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 0u); /* nothing logged while under budget */

    CHECK(net_io_wire_admit_tenant_message(&srv, tenant) == false); /* 1st rejection */
    CHECK(net_io_wire_admit_tenant_message(&srv, tenant) == false); /* 2nd rejection */
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 2u);
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);

    net_io_wire_shutdown(&srv);
}

static void test_null_audit_log_is_accepted(void) {
    SECTION("net_io_wire_admit_tenant_message: NULL audit_log (the default) -- rejection still works, nothing crashes");
    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);
    /* audit_log defaults to NULL from net_io_wire_init() -- never set here. */

    const uint16_t tenant = 1u;
    for (uint32_t i = 0; i < (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN; i++) {
        CHECK(net_io_wire_admit_tenant_message(&srv, tenant));
    }
    CHECK(net_io_wire_admit_tenant_message(&srv, tenant) == false);

    net_io_wire_stats_t stats;
    net_io_wire_get_stats(&srv, &stats);
    CHECK(stats.rejected_rate_limited == 1u);

    net_io_wire_shutdown(&srv);
}

/* ================================================================== */
/* 8 threads x 50,000 messages on ONE session token: strict blocking,   */
/* telemetry log saturates gracefully, FNV-1a chain stays valid.        */
/* ================================================================== */

#define FLOOD_THREAD_COUNT       8u
#define FLOOD_MSGS_PER_THREAD    50000u
#define FLOOD_TOTAL_ATTEMPTS     (FLOOD_THREAD_COUNT * FLOOD_MSGS_PER_THREAD)
#define FLOOD_TENANT             55u

static net_io_wire_server_t *g_flood_srv;
static _Atomic uint32_t       g_flood_admitted;

static DWORD WINAPI flood_thread(LPVOID arg) {
    (void)arg;
    for (uint32_t i = 0; i < FLOOD_MSGS_PER_THREAD; i++) {
        if (net_io_wire_admit_tenant_message(g_flood_srv, FLOOD_TENANT)) {
            atomic_fetch_add_explicit(&g_flood_admitted, 1u, memory_order_relaxed);
        }
    }
    return 0;
}

#define MODERATE_REJECTS_PER_THREAD 400u /* tenant 60's budget is exhausted before these threads start: every attempt is a real rejection */

static DWORD WINAPI moderate_reject_thread(LPVOID arg) {
    (void)arg;
    for (uint32_t i = 0; i < MODERATE_REJECTS_PER_THREAD; i++) {
        (void)net_io_wire_admit_tenant_message(g_flood_srv, 60u);
    }
    return 0;
}

static void test_8_threads_50000_messages_saturating_flood(void) {
    SECTION("8 threads x 50000 messages on one session token: exactly 500 admitted, strict O(1) blocking, FNV-1a chain valid under maximal contention");

    /* audit_log IS attached here again, deliberately: this test's first
     * run (before the Niveau 54 fix) uncovered a real, pre-existing race
     * in telemetry_fabric.c's tf_log_append() -- a fixed-bound spin that,
     * on exhaustion, read the predecessor's checksum without ever having
     * synchronized with it (a genuine data race, not just a logical
     * inconsistency), which tf_log_verify() then correctly reported as a
     * false TF_ERR_TAMPERED under contention exactly this extreme. Fixed
     * in src/telemetry_fabric.c (see EMPIRE_LOGDOWN.md's Niveau 54 entry
     * and tests/test_telemetry_fabric.c's own dedicated 8x50000 test,
     * which targets tf_log_append() directly): the wait is now unbounded
     * and CAS-based, with no bypass that skips synchronization. Verified
     * stable across 10 consecutive reruns before re-enabling this
     * assertion here. */
    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);
    tf_log_t audit_log;
    tf_log_init(&audit_log);
    net_io_wire_set_audit_log(&srv, &audit_log);

    g_flood_srv = &srv;
    atomic_store_explicit(&g_flood_admitted, 0u, memory_order_relaxed);

    HANDLE threads[FLOOD_THREAD_COUNT];
    for (uint32_t t = 0; t < FLOOD_THREAD_COUNT; t++) {
        threads[t] = CreateThread(NULL, 0, flood_thread, NULL, 0, NULL);
    }
    WaitForMultipleObjects(FLOOD_THREAD_COUNT, threads, TRUE, INFINITE);
    for (uint32_t t = 0; t < FLOOD_THREAD_COUNT; t++) {
        CloseHandle(threads[t]);
    }

    uint32_t admitted = atomic_load_explicit(&g_flood_admitted, memory_order_relaxed);
    printf("   attempts=%u admitted=%u (expected exactly %u)\n",
           (unsigned)FLOOD_TOTAL_ATTEMPTS, (unsigned)admitted, (unsigned)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN);
    CHECK(admitted == (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN); /* strict blocking: no more, no less, under 8-way contention */
    CHECK(token_bucket_available(&srv.rate_limiters[FLOOD_TENANT]) == 0);

    net_io_wire_stats_t stats;
    net_io_wire_get_stats(&srv, &stats);
    uint64_t expected_rejections = (uint64_t)FLOOD_TOTAL_ATTEMPTS - admitted;
    printf("   rejected_rate_limited=%llu (expected %llu)\n",
           (unsigned long long)stats.rejected_rate_limited, (unsigned long long)expected_rejections);
    CHECK(stats.rejected_rate_limited == expected_rejections);

    /* The audit log (TF_LOG_CAPACITY=4096) saturates under a flood this
     * size -- expected, best-effort logging. The point of this assertion:
     * every entry that DID land is now provably untampered, even at
     * maximum 8-way concurrent tf_log_append() contention. */
    uint64_t log_entries = atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed);
    printf("   telemetry entries logged=%llu (TF_LOG_CAPACITY=4096)\n", (unsigned long long)log_entries);
    CHECK(log_entries == 4096u);
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);

    net_io_wire_shutdown(&srv);
}

static void test_rejection_logging_stays_valid_under_moderate_concurrency(void) {
    SECTION("8 threads x 400 rejections concurrently: FNV-1a audit chain stays exactly valid under real (non-pathological) contention");

    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);
    tf_log_t audit_log;
    tf_log_init(&audit_log);
    net_io_wire_set_audit_log(&srv, &audit_log);

    /* Exhaust the tenant's budget first (single-threaded, deterministic)
     * so every one of the concurrent attempts below is a genuine
     * rejection, not a race against the last few admits. */
    const uint16_t tenant = 60u;
    for (uint32_t i = 0; i < (uint32_t)NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN; i++) {
        CHECK(net_io_wire_admit_tenant_message(&srv, tenant));
    }

    g_flood_srv = &srv;
    HANDLE threads[FLOOD_THREAD_COUNT];
    for (uint32_t t = 0; t < FLOOD_THREAD_COUNT; t++) {
        threads[t] = CreateThread(NULL, 0, moderate_reject_thread, NULL, 0, NULL);
    }
    WaitForMultipleObjects(FLOOD_THREAD_COUNT, threads, TRUE, INFINITE);
    for (uint32_t t = 0; t < FLOOD_THREAD_COUNT; t++) {
        CloseHandle(threads[t]);
    }

    uint64_t expected_entries = (uint64_t)FLOOD_THREAD_COUNT * MODERATE_REJECTS_PER_THREAD;
    uint64_t log_entries = atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed);
    printf("   concurrent rejections logged=%llu (expected %llu)\n",
           (unsigned long long)log_entries, (unsigned long long)expected_entries);
    CHECK(log_entries == expected_entries);
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);

    net_io_wire_stats_t stats;
    net_io_wire_get_stats(&srv, &stats);
    CHECK(stats.rejected_rate_limited == expected_entries);

    net_io_wire_shutdown(&srv);
}

/* ================================================================== */
/* release/acquire: admit -> wire_ring_reserve -> publish -> pop,       */
/* driven concurrently by real producer + consumer threads, exactly    */
/* net_io.c's own real call sequence for an admitted message.           */
/* ================================================================== */

#define PIPE_PRODUCER_THREADS  4u
#define PIPE_MSGS_PER_THREAD   100u /* 4*100 = 400 < 500: every attempt should be admitted */
#define PIPE_TOTAL_MSGS        (PIPE_PRODUCER_THREADS * PIPE_MSGS_PER_THREAD)
#define PIPE_TENANT            11u

static net_io_wire_server_t *g_pipe_srv;
static wire_ring_t          *g_pipe_ring;
static _Atomic uint32_t       g_pipe_admitted;
static _Atomic uint32_t       g_pipe_reserve_failures;
static _Atomic uint64_t       g_pipe_produced_checksum;
static _Atomic uint64_t       g_pipe_consumed_checksum;
static _Atomic uint32_t       g_pipe_consumed_count;
static _Atomic bool           g_pipe_producers_done;

typedef struct {
    uint32_t thread_id;
} pipe_thread_args_t;

static DWORD WINAPI pipe_producer_thread(LPVOID arg) {
    pipe_thread_args_t *args = (pipe_thread_args_t *)arg;
    for (uint32_t i = 0; i < PIPE_MSGS_PER_THREAD; i++) {
        if (!net_io_wire_admit_tenant_message(g_pipe_srv, PIPE_TENANT)) {
            continue; /* should not happen at this volume, but never treated as fatal here */
        }
        atomic_fetch_add_explicit(&g_pipe_admitted, 1u, memory_order_relaxed);

        uint32_t tag = args->thread_id * 1000u + i;

        wire_msg_t *slot = NULL;
        int spins = 0;
        wire_status_t rst;
        do {
            rst = wire_ring_reserve(g_pipe_ring, &slot);
            spins++;
            /* A raw busy-spin with no yield can starve the consumer
             * thread of CPU time on a loaded machine (this run's own
             * observed flake: 4 producers vs. 1 consumer on a box also
             * running other CPU-heavy test binaries back to back) --
             * this affects only how fast the ring drains (liveness),
             * never the correctness this test actually asserts (which
             * depends solely on exact counts/checksums, not on timing). */
            if (rst == WIRE_ERR_FULL && (spins % 4096) == 0) {
                SwitchToThread();
            }
        } while (rst == WIRE_ERR_FULL && spins < POLL_SPIN_LIMIT);

        if (rst != WIRE_OK) {
            atomic_fetch_add_explicit(&g_pipe_reserve_failures, 1u, memory_order_relaxed);
            continue;
        }

        /* Zero-copy-style direct slot write, exactly like net_io.c's real
         * reactor -- slot is already fully zeroed by wire_ring_reserve(). */
        memcpy(slot->payload, &tag, sizeof tag);
        wire_ring_publish(g_pipe_ring, slot, 1u);

        atomic_fetch_add_explicit(&g_pipe_produced_checksum, (uint64_t)tag, memory_order_relaxed);
    }
    return 0;
}

static DWORD WINAPI pipe_consumer_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        wire_msg_t out;
        wire_status_t rst = wire_ring_try_pop(g_pipe_ring, &out);
        if (rst == WIRE_OK) {
            if (out.type != WIRE_MSG_TYPE_DISCARD) {
                uint32_t tag;
                memcpy(&tag, out.payload, sizeof tag);
                atomic_fetch_add_explicit(&g_pipe_consumed_checksum, (uint64_t)tag, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_pipe_consumed_count, 1u, memory_order_relaxed);
            }
            continue;
        }
        /* WIRE_ERR_EMPTY: stop only once every producer is done AND the
         * ring is confirmed empty -- otherwise this is just a momentary
         * gap between producer publishes. */
        if (atomic_load_explicit(&g_pipe_producers_done, memory_order_acquire) && wire_ring_is_empty(g_pipe_ring)) {
            break;
        }
    }
    return 0;
}

static void test_admit_reserve_publish_pop_pipeline_under_concurrency(void) {
    SECTION("release/acquire: 4 producers (admit->reserve->publish) + 1 consumer (pop), byte-exact, 0 mismatch");

    net_io_wire_server_t srv;
    CHECK(net_io_wire_init(&srv, 0) == NET_IO_OK);
    static wire_ring_t ring; /* large-ish: static, not stack */
    wire_ring_init(&ring);

    g_pipe_srv = &srv;
    g_pipe_ring = &ring;
    atomic_store_explicit(&g_pipe_admitted, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_pipe_reserve_failures, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_pipe_produced_checksum, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_pipe_consumed_checksum, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_pipe_consumed_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_pipe_producers_done, false, memory_order_relaxed);

    HANDLE consumer = CreateThread(NULL, 0, pipe_consumer_thread, NULL, 0, NULL);

    pipe_thread_args_t args[PIPE_PRODUCER_THREADS];
    HANDLE producers[PIPE_PRODUCER_THREADS];
    for (uint32_t t = 0; t < PIPE_PRODUCER_THREADS; t++) {
        args[t].thread_id = t;
        producers[t] = CreateThread(NULL, 0, pipe_producer_thread, &args[t], 0, NULL);
    }
    WaitForMultipleObjects(PIPE_PRODUCER_THREADS, producers, TRUE, INFINITE);
    for (uint32_t t = 0; t < PIPE_PRODUCER_THREADS; t++) {
        CloseHandle(producers[t]);
    }

    atomic_store_explicit(&g_pipe_producers_done, true, memory_order_release);
    WaitForSingleObject(consumer, INFINITE);
    CloseHandle(consumer);

    uint32_t admitted = atomic_load_explicit(&g_pipe_admitted, memory_order_relaxed);
    uint32_t reserve_failures = atomic_load_explicit(&g_pipe_reserve_failures, memory_order_relaxed);
    uint32_t consumed_count = atomic_load_explicit(&g_pipe_consumed_count, memory_order_relaxed);
    uint64_t produced_checksum = atomic_load_explicit(&g_pipe_produced_checksum, memory_order_relaxed);
    uint64_t consumed_checksum = atomic_load_explicit(&g_pipe_consumed_checksum, memory_order_relaxed);

    printf("   admitted=%u reserve_failures=%u consumed=%u produced_checksum=%llu consumed_checksum=%llu\n",
           (unsigned)admitted, (unsigned)reserve_failures, (unsigned)consumed_count,
           (unsigned long long)produced_checksum, (unsigned long long)consumed_checksum);

    CHECK(admitted == (uint32_t)PIPE_TOTAL_MSGS);       /* 400 < 500: every attempt admitted */
    CHECK(reserve_failures == 0u);                       /* continuous drain: the 64-slot ring never permanently stalls a producer */
    CHECK(consumed_count == admitted);                   /* every admitted, published message was drained -- none lost */
    CHECK(consumed_checksum == produced_checksum);        /* byte-exact: no torn/stale/duplicated read, across real concurrency */
    CHECK(consumed_checksum != 0u);                       /* sanity: the checksum isn't trivially zero */

    net_io_wire_shutdown(&srv);
}

/* ================================================================== */

int main(void) {
    printf("=== test_karo_network: net_io.c Niveau 53 chat rate limiting + telemetry audit ===\n");

    test_admit_rejects_invalid_args();
    test_admit_exact_capacity_then_rejects();
    test_refill_restores_budget_capped_at_capacity();
    test_rejection_is_logged_to_telemetry_fnv1a();
    test_null_audit_log_is_accepted();

    test_8_threads_50000_messages_saturating_flood();
    test_rejection_logging_stays_valid_under_moderate_concurrency();
    test_admit_reserve_publish_pop_pipeline_under_concurrency();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
