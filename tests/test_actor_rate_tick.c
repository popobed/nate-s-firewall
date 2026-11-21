/*
 * test_actor_rate_tick.c - Unit + concurrency stress bench for the three
 * additions layered onto actor_core.c: the private-memory bump/arena
 * allocator, the per-actor lock-free CAS token bucket (rate_limit.h), and
 * the CHRONO-BLACKBOX O(1) step counter (chrono_tick.h).
 *
 * The concurrency sections are the point of this file: a token bucket or
 * a one-shot "is it due" check is trivial to get right single-threaded
 * and easy to get subtly wrong under real contention (double-consumption,
 * double-firing). Both are proven here with real OS threads hammering the
 * SAME shared instance, not just sequential calls from one thread.
 *
 * Windows-only build (actor_core.h has no such restriction itself, but
 * this file uses the Win32 thread API directly for its stress sections,
 * matching this project's other concurrency test benches, e.g.
 * test_concurrent_core.c). No dynamic allocation anywhere in this file.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "actor_core.h"
#include "chrono_tick.h"
#include <windows.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_tests_run++;                                                          \
        if (!(cond)) {                                                           \
            g_tests_failed++;                                                    \
            fprintf(stderr, "  [FAIL] %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                         \
    } while (0)

#define SECTION(name) printf("-- %s\n", (name))

/* ---------------------------------------------------------------- */
/* Private-memory arena allocator                                     */
/* ---------------------------------------------------------------- */

static void test_arena_alloc(void) {
    SECTION("actor_arena_alloc: bump allocation within private_memory, alignment honored");
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    actor_t *a = actor_system_get(&sys, 0u);
    CHECK(a != NULL);

    void *p1 = actor_arena_alloc(a, 10u, 1u);
    CHECK(p1 != NULL);
    CHECK((uint8_t *)p1 >= a->private_memory && (uint8_t *)p1 < a->private_memory + ACTOR_PRIVATE_MEM_SIZE);

    void *p2 = actor_arena_alloc(a, 16u, 16u);
    CHECK(p2 != NULL);
    CHECK(((uintptr_t)p2 % 16u) == 0u);
    CHECK((uint8_t *)p2 > (uint8_t *)p1); /* strictly after p1's allocation, bump allocator never rewinds */

    void *p3 = actor_arena_alloc(a, 32u, 32u);
    CHECK(p3 != NULL);
    CHECK(((uintptr_t)p3 % 32u) == 0u);

    SECTION("actor_arena_alloc: exhaustion returns NULL, never overruns private_memory");
    void *huge = actor_arena_alloc(a, ACTOR_PRIVATE_MEM_SIZE, 1u); /* already partially consumed above -- must not fit */
    CHECK(huge == NULL);

    void *exact = actor_arena_alloc(a, 1u, 1u);
    CHECK(exact != NULL); /* a single spare byte should still be grantable */

    SECTION("actor_arena_reset: reclaims the whole arena at once");
    actor_arena_reset(a);
    void *after_reset = actor_arena_alloc(a, ACTOR_PRIVATE_MEM_SIZE, 1u);
    CHECK(after_reset != NULL);
    CHECK((uint8_t *)after_reset == a->private_memory);

    SECTION("actor_arena_alloc: rejects a non-power-of-two alignment");
    CHECK(actor_arena_alloc(a, 4u, 3u) == NULL);
}

/* ---------------------------------------------------------------- */
/* Token bucket: single-threaded contract                             */
/* ---------------------------------------------------------------- */

static void test_token_bucket_basic(void) {
    SECTION("token_bucket: init/consume/refill/capacity clamp");
    token_bucket_t tb;
    token_bucket_init(&tb, 10, 3);
    CHECK(token_bucket_available(&tb) == 3);

    CHECK(token_bucket_try_consume(&tb, 3) == true);
    CHECK(token_bucket_available(&tb) == 0);
    CHECK(token_bucket_try_consume(&tb, 1) == false); /* exhausted: fail fast, unchanged */
    CHECK(token_bucket_available(&tb) == 0);

    token_bucket_refill(&tb, 100); /* far more than capacity -- must clamp */
    CHECK(token_bucket_available(&tb) == 10);

    CHECK(token_bucket_try_consume(&tb, 10) == true);
    CHECK(token_bucket_available(&tb) == 0);

    SECTION("actor_rate_limit_*: per-actor wrapper over token_bucket_t");
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 1u) == ACTOR_OK);
    actor_t *a = actor_system_get(&sys, 1u);
    CHECK(actor_rate_limit_try_consume(a, 1) == false); /* default-initialized to capacity 0 -- see actor_system_init() */
    actor_rate_limit_init(a, 5, 5);
    CHECK(actor_rate_limit_try_consume(a, 5) == true);
    CHECK(actor_rate_limit_try_consume(a, 1) == false);
    actor_rate_limit_refill(a, 2);
    CHECK(actor_rate_limit_try_consume(a, 2) == true);
}

/* ---------------------------------------------------------------- */
/* Token bucket: real concurrent contention                           */
/* ---------------------------------------------------------------- */

#define BUCKET_STRESS_THREADS   8u
#define BUCKET_STRESS_ATTEMPTS  50000u /* per thread -- far more than the bucket's capacity */
#define BUCKET_CAPACITY         10000

static token_bucket_t g_stress_bucket;
static _Atomic uint32_t g_stress_success_total = 0;

static DWORD WINAPI bucket_stress_thread(LPVOID arg) {
    (void)arg;
    for (uint32_t i = 0; i < BUCKET_STRESS_ATTEMPTS; i++) {
        if (token_bucket_try_consume(&g_stress_bucket, 1)) {
            atomic_fetch_add_explicit(&g_stress_success_total, 1u, memory_order_relaxed);
        }
    }
    return 0;
}

static void test_token_bucket_concurrent(void) {
    SECTION("token_bucket: 8 threads racing to consume a bucket of exactly 10000 -- must grant EXACTLY 10000");
    token_bucket_init(&g_stress_bucket, BUCKET_CAPACITY, BUCKET_CAPACITY);
    atomic_store_explicit(&g_stress_success_total, 0u, memory_order_relaxed);

    HANDLE handles[BUCKET_STRESS_THREADS];
    for (uint32_t i = 0; i < BUCKET_STRESS_THREADS; i++) {
        handles[i] = CreateThread(NULL, 0, bucket_stress_thread, NULL, 0, NULL);
        CHECK(handles[i] != NULL);
    }
    WaitForMultipleObjects(BUCKET_STRESS_THREADS, handles, TRUE, INFINITE);
    for (uint32_t i = 0; i < BUCKET_STRESS_THREADS; i++) {
        CloseHandle(handles[i]);
    }

    uint32_t granted = atomic_load_explicit(&g_stress_success_total, memory_order_relaxed);
    printf("  attempts=%u (%u threads x %u), granted=%u, remaining=%" PRId64 "\n",
           (unsigned)(BUCKET_STRESS_THREADS * BUCKET_STRESS_ATTEMPTS), (unsigned)BUCKET_STRESS_THREADS,
           (unsigned)BUCKET_STRESS_ATTEMPTS, (unsigned)granted, token_bucket_available(&g_stress_bucket));
    CHECK(granted == (uint32_t)BUCKET_CAPACITY);
    CHECK(token_bucket_available(&g_stress_bucket) == 0);
}

/* ---------------------------------------------------------------- */
/* chrono_tick: single-threaded contract                               */
/* ---------------------------------------------------------------- */

static void test_chrono_tick_basic(void) {
    SECTION("chrono_tick: advance/current, no OS clock involved");
    chrono_tick_counter_t tc;
    chrono_tick_init(&tc);
    CHECK(chrono_tick_current(&tc) == 0u);
    CHECK(chrono_tick_advance(&tc) == 1u);
    CHECK(chrono_tick_advance(&tc) == 2u);
    CHECK(chrono_tick_current(&tc) == 2u);

    SECTION("chrono_tick_due: one-shot per interval, interval==0 always fires");
    _Atomic uint64_t last_fire = 0;
    CHECK(chrono_tick_due(&tc, 10u, &last_fire) == false); /* only 2 ticks elapsed, interval is 10 */
    for (int i = 0; i < 8; i++) {
        chrono_tick_advance(&tc); /* now at 10 */
    }
    CHECK(chrono_tick_current(&tc) == 10u);
    CHECK(chrono_tick_due(&tc, 10u, &last_fire) == true);  /* exactly due */
    CHECK(chrono_tick_due(&tc, 10u, &last_fire) == false); /* one-shot: immediately re-checking must NOT fire again */

    _Atomic uint64_t last_fire_zero = 0;
    CHECK(chrono_tick_due(&tc, 0u, &last_fire_zero) == true);
    CHECK(chrono_tick_due(&tc, 0u, &last_fire_zero) == true); /* interval 0: always due, every call */
}

/* ---------------------------------------------------------------- */
/* chrono_tick_due: real concurrent contention                        */
/* ---------------------------------------------------------------- */

#define TICK_STRESS_THREADS 8u

static chrono_tick_counter_t g_stress_tick;
static _Atomic uint64_t g_stress_last_fire = 0;
static _Atomic uint32_t g_stress_fire_count = 0;

static DWORD WINAPI tick_due_stress_thread(LPVOID arg) {
    (void)arg;
    if (chrono_tick_due(&g_stress_tick, 100u, &g_stress_last_fire)) {
        atomic_fetch_add_explicit(&g_stress_fire_count, 1u, memory_order_relaxed);
    }
    return 0;
}

static void test_chrono_tick_due_concurrent(void) {
    SECTION("chrono_tick_due: 8 threads racing on the SAME schedule at the exact due instant -- exactly one must fire");
    chrono_tick_init(&g_stress_tick);
    for (int i = 0; i < 100; i++) {
        chrono_tick_advance(&g_stress_tick);
    }
    atomic_store_explicit(&g_stress_last_fire, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_stress_fire_count, 0u, memory_order_relaxed);

    HANDLE handles[TICK_STRESS_THREADS];
    for (uint32_t i = 0; i < TICK_STRESS_THREADS; i++) {
        handles[i] = CreateThread(NULL, 0, tick_due_stress_thread, NULL, 0, NULL);
        CHECK(handles[i] != NULL);
    }
    WaitForMultipleObjects(TICK_STRESS_THREADS, handles, TRUE, INFINITE);
    for (uint32_t i = 0; i < TICK_STRESS_THREADS; i++) {
        CloseHandle(handles[i]);
    }

    uint32_t fires = atomic_load_explicit(&g_stress_fire_count, memory_order_relaxed);
    printf("  %u threads raced chrono_tick_due() on one schedule -> %u fired\n",
           (unsigned)TICK_STRESS_THREADS, (unsigned)fires);
    CHECK(fires == 1u);
}

int main(void) {
    printf("=== actor_core private-heap / rate-limit / chrono_tick bench ===\n");

    test_arena_alloc();
    test_token_bucket_basic();
    test_token_bucket_concurrent();
    test_chrono_tick_basic();
    test_chrono_tick_due_concurrent();

    printf("=== %d checks run, %d failed ===\n", g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
