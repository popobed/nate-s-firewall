/*
 * test_concurrent_core.c - Unit + true multi-thread, multi-tenant stress
 * test bench for ring_buffer.{h,c}, actor_core.{h,c} and msg_router.{h,c}:
 * layout/alignment, actor isolation, O(1) Tenant ID filtering,
 * zero-copy dispatch, deterministic backpressure, and concurrent
 * multi-tenant routing correctness under load (including a concurrent
 * flood of invalid-tenant traffic that must never affect valid traffic).
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching these modules' own zero-allocation constraint.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "actor_core.h"
#include "msg_router.h"
#include "ring_buffer.h"

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
/* Layout: 64-byte alignment / false-sharing                          */
/* ---------------------------------------------------------------- */

static void test_ring_msg_layout(void) {
    SECTION("ring_msg_t: exactly 64 bytes, 64-byte aligned");
    CHECK(sizeof(ring_msg_t) == 64u);
    CHECK(_Alignof(ring_msg_t) == 64u);
}

static void test_ring_buffer_layout(void) {
    SECTION("ring_buffer_t: tail/head each on their own cache line");
    CHECK(offsetof(ring_buffer_t, tail) == 0);
    CHECK(offsetof(ring_buffer_t, head) == RB_CACHE_LINE_SIZE);
    CHECK(offsetof(ring_buffer_t, slots) == 2u * RB_CACHE_LINE_SIZE);
    CHECK(_Alignof(ring_buffer_t) >= RB_CACHE_LINE_SIZE);
}

static void test_actor_layout(void) {
    SECTION("actor_t: private_memory and mailbox both 64-byte aligned");
    CHECK(_Alignof(actor_t) >= 64u);
    CHECK(offsetof(actor_t, private_memory) == 0);
    /* mailbox starts right after private_memory; ring_buffer_t's own
     * _Alignas(64) first member forces it onto a 64-byte boundary
     * regardless of exactly where that lands. */
    CHECK((offsetof(actor_t, mailbox) % 64u) == 0);
}

/* ---------------------------------------------------------------- */
/* Actor isolation: private memory blocks never alias                 */
/* ---------------------------------------------------------------- */

static actor_system_t g_sys;

static void test_actor_private_memory_isolation(void) {
    SECTION("actor isolation: private_memory blocks never alias each other");

    actor_system_init(&g_sys);
    CHECK(actor_system_spawn(&g_sys, 0u) == ACTOR_OK);
    CHECK(actor_system_spawn(&g_sys, 1u) == ACTOR_OK);

    actor_t *a0 = actor_system_get(&g_sys, 0u);
    actor_t *a1 = actor_system_get(&g_sys, 1u);
    CHECK(a0 != NULL && a1 != NULL);
    CHECK(a0 != a1);
    CHECK((const void *)a0->private_memory != (const void *)a1->private_memory);

    memset(a0->private_memory, 0xAA, sizeof a0->private_memory);
    memset(a1->private_memory, 0x55, sizeof a1->private_memory);
    CHECK(a0->private_memory[0] == 0xAAu);
    CHECK(a1->private_memory[0] == 0x55u);
    CHECK(a0->private_memory[ACTOR_PRIVATE_MEM_SIZE - 1u] == 0xAAu);
    CHECK(a1->private_memory[ACTOR_PRIVATE_MEM_SIZE - 1u] == 0x55u);

    /* Re-spawning an already-active actor is rejected, not silently
     * accepted (a double-spawn is almost always a bug). */
    CHECK(actor_system_spawn(&g_sys, 0u) == ACTOR_ERR_ALREADY_ACTIVE);

    /* Unspawned/out-of-range actors are inaccessible. */
    CHECK(actor_system_get(&g_sys, 2u) == NULL);
    CHECK(actor_system_get(&g_sys, ACTOR_MAX_COUNT) == NULL);
}

/* ---------------------------------------------------------------- */
/* Router: basic dispatch round trip                                  */
/* ---------------------------------------------------------------- */

static void test_router_basic_roundtrip(void) {
    SECTION("router: build -> dispatch -> actor mailbox -> process, happy path");

    actor_system_init(&g_sys);
    CHECK(actor_system_spawn(&g_sys, 0u) == ACTOR_OK);
    router_init();
    CHECK(router_register_tenant(100u, 0u) == ROUTER_OK);

    const uint8_t payload[] = "hello-actor-system";
    uint8_t frame[ROUTER_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    CHECK(router_build_frame(frame, sizeof frame, &frame_len, 7u, 100u,
                              payload, (uint16_t)sizeof payload) == ROUTER_OK);

    actor_t *a0 = actor_system_get(&g_sys, 0u);
    CHECK(actor_mailbox_count(a0) == 0u);
    CHECK(router_dispatch(&g_sys, frame, frame_len) == ROUTER_OK);
    CHECK(actor_mailbox_count(a0) == 1u);

    ring_msg_t msg;
    CHECK(actor_process_one(a0, &msg) == ACTOR_OK);
    CHECK(msg.sender_id == 100u); /* tenant_id, as published by router_dispatch */
    CHECK(msg.type == 7u);
    CHECK(memcmp(msg.payload, payload, sizeof payload) == 0);

    router_stats_t stats;
    router_get_stats(&stats);
    CHECK(stats.frames_routed == 1u);
    CHECK(stats.rejected_unknown_tenant == 0u);
}

/* ---------------------------------------------------------------- */
/* Strict Tenant ID filtering: O(1) rejection, zero impact elsewhere   */
/* ---------------------------------------------------------------- */

static void test_tenant_filtering_isolation(void) {
    SECTION("unknown Tenant ID rejected O(1), no actor mailbox is ever touched");

    actor_system_init(&g_sys);
    CHECK(actor_system_spawn(&g_sys, 0u) == ACTOR_OK);
    CHECK(actor_system_spawn(&g_sys, 1u) == ACTOR_OK);
    router_init();
    CHECK(router_register_tenant(1u, 0u) == ROUTER_OK);
    CHECK(router_register_tenant(2u, 1u) == ROUTER_OK);

    const uint8_t payload[] = "x";
    uint8_t frame[ROUTER_MAX_FRAME_SIZE];
    size_t frame_len = 0;

    /* Tenant 999 was never registered. */
    CHECK(router_build_frame(frame, sizeof frame, &frame_len, 1u, 999u,
                              payload, (uint16_t)sizeof payload) == ROUTER_OK);
    CHECK(router_dispatch(&g_sys, frame, frame_len) == ROUTER_ERR_UNKNOWN_TENANT);

    actor_t *a0 = actor_system_get(&g_sys, 0u);
    actor_t *a1 = actor_system_get(&g_sys, 1u);
    CHECK(actor_mailbox_count(a0) == 0u);
    CHECK(actor_mailbox_count(a1) == 0u);

    /* Malformed frames (bad magic, truncated, oversized) are likewise
     * rejected before any mailbox is touched. */
    CHECK(router_build_frame(frame, sizeof frame, &frame_len, 1u, 1u,
                              payload, (uint16_t)sizeof payload) == ROUTER_OK);
    frame[0] ^= 0xFFu;
    CHECK(router_dispatch(&g_sys, frame, frame_len) == ROUTER_ERR_BAD_MAGIC);
    CHECK(router_dispatch(&g_sys, frame, 3) == ROUTER_ERR_TRUNCATED);
    CHECK(router_build_frame(frame, sizeof frame, &frame_len, 1u, 1u, payload,
                              (uint16_t)(ROUTER_MAX_PAYLOAD + 1u)) == ROUTER_ERR_PAYLOAD_TOO_LARGE);

    CHECK(actor_mailbox_count(a0) == 0u);
    CHECK(actor_mailbox_count(a1) == 0u);

    router_stats_t stats;
    router_get_stats(&stats);
    CHECK(stats.rejected_unknown_tenant == 1u);
    CHECK(stats.rejected_bad_frame >= 2u); /* bad magic + truncated */
    CHECK(stats.frames_routed == 0u);
}

/* ---------------------------------------------------------------- */
/* Backpressure: saturating one actor never affects another            */
/* ---------------------------------------------------------------- */

static void test_backpressure_no_cross_actor_impact(void) {
    SECTION("backpressure: one actor's full mailbox never affects another actor");

    actor_system_init(&g_sys);
    CHECK(actor_system_spawn(&g_sys, 0u) == ACTOR_OK);
    CHECK(actor_system_spawn(&g_sys, 1u) == ACTOR_OK);
    router_init();
    CHECK(router_register_tenant(1u, 0u) == ROUTER_OK);
    CHECK(router_register_tenant(2u, 1u) == ROUTER_OK);

    uint8_t frame1[ROUTER_MAX_FRAME_SIZE];
    size_t len1 = 0;
    const uint8_t p1[] = "a";
    CHECK(router_build_frame(frame1, sizeof frame1, &len1, 1u, 1u, p1, (uint16_t)sizeof p1) == ROUTER_OK);

    uint32_t accepted = 0;
    for (uint32_t i = 0; i < RING_BUFFER_CAPACITY + 8u; i++) {
        router_status_t st = router_dispatch(&g_sys, frame1, len1);
        if (st == ROUTER_OK) {
            accepted++;
        } else {
            CHECK(st == ROUTER_ERR_MAILBOX_FULL);
        }
    }
    CHECK(accepted == (uint32_t)RING_BUFFER_CAPACITY);

    actor_t *a0 = actor_system_get(&g_sys, 0u);
    actor_t *a1 = actor_system_get(&g_sys, 1u);
    CHECK(actor_mailbox_count(a0) == (size_t)RING_BUFFER_CAPACITY);

    /* Actor 1 (a different tenant) must be completely unaffected: it can
     * still receive normally while actor 0 is fully saturated. */
    uint8_t frame2[ROUTER_MAX_FRAME_SIZE];
    size_t len2 = 0;
    const uint8_t p2[] = "b";
    CHECK(router_build_frame(frame2, sizeof frame2, &len2, 1u, 2u, p2, (uint16_t)sizeof p2) == ROUTER_OK);
    CHECK(router_dispatch(&g_sys, frame2, len2) == ROUTER_OK);
    CHECK(actor_mailbox_count(a1) == 1u);

    router_stats_t stats;
    router_get_stats(&stats);
    CHECK(stats.rejected_backpressure == 8u);
}

/* ---------------------------------------------------------------- */
/* True multi-thread, multi-tenant concurrency stress                 */
/* ---------------------------------------------------------------- */

#define STRESS_PRODUCERS_PER_TENANT 2u
#define STRESS_N_PER_PRODUCER       ((uint64_t)20000)
#define STRESS_TENANT_A             10u
#define STRESS_TENANT_B             20u
#define STRESS_ACTOR_A              0u
#define STRESS_ACTOR_B              1u
#define STRESS_INVALID_TENANT       250u

static actor_system_t g_stress_sys;

typedef struct {
    uint16_t tenant_id;
} producer_ctx_t;

typedef struct {
    uint8_t  actor_id;
    uint16_t expected_tenant_id;
    uint64_t consumed;
    uint64_t checksum;
    int      tenant_ok;
} consumer_ctx_t;

typedef struct {
    _Atomic int *stop_flag; /* set once all valid producers are done */
    uint64_t sent;
} flood_ctx_t;

#if defined(_WIN32)
static DWORD WINAPI stress_producer_thread(LPVOID arg) {
#else
static void *stress_producer_thread(void *arg) {
#endif
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    uint8_t frame[ROUTER_MAX_FRAME_SIZE];

    for (uint64_t seq = 0; seq < STRESS_N_PER_PRODUCER; seq++) {
        uint8_t payload[8];
        memcpy(payload, &seq, sizeof seq);
        size_t frame_len = 0;
        router_build_frame(frame, sizeof frame, &frame_len, 0u, ctx->tenant_id, payload, sizeof payload);

        for (;;) { /* pure bounded spin: see net_gateway/net_io precedent in this project */
            if (router_dispatch(&g_stress_sys, frame, frame_len) == ROUTER_OK) {
                break;
            }
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI stress_consumer_thread(LPVOID arg) {
#else
static void *stress_consumer_thread(void *arg) {
#endif
    consumer_ctx_t *ctx = (consumer_ctx_t *)arg;
    const uint64_t target = (uint64_t)STRESS_PRODUCERS_PER_TENANT * STRESS_N_PER_PRODUCER;
    actor_t *a = actor_system_get(&g_stress_sys, ctx->actor_id);
    ring_msg_t msg;

    while (ctx->consumed < target) {
        if (actor_process_one(a, &msg) == ACTOR_OK) {
            if (msg.sender_id != ctx->expected_tenant_id) {
                ctx->tenant_ok = 0;
            }
            uint64_t seq = 0xFFFFFFFFFFFFFFFFULL;
            memcpy(&seq, msg.payload, sizeof seq);
            ctx->checksum += seq;
            ctx->consumed++;
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#if defined(_WIN32)
static DWORD WINAPI stress_flood_thread(LPVOID arg) {
#else
static void *stress_flood_thread(void *arg) {
#endif
    flood_ctx_t *ctx = (flood_ctx_t *)arg;
    uint8_t frame[ROUTER_MAX_FRAME_SIZE];
    const uint8_t payload[] = "invalid-tenant-flood";
    size_t frame_len = 0;
    router_build_frame(frame, sizeof frame, &frame_len, 0u, STRESS_INVALID_TENANT,
                        payload, (uint16_t)sizeof payload);

    while (atomic_load_explicit(ctx->stop_flag, memory_order_relaxed) == 0) {
        router_status_t st = router_dispatch(&g_stress_sys, frame, frame_len);
        if (st == ROUTER_ERR_UNKNOWN_TENANT) {
            ctx->sent++;
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_multi_tenant_stress(void) {
    SECTION("true multi-thread, multi-tenant stress: two tenants, two actors, "
            "plus a concurrent invalid-tenant flood that must never leak in");

    actor_system_init(&g_stress_sys);
    CHECK(actor_system_spawn(&g_stress_sys, STRESS_ACTOR_A) == ACTOR_OK);
    CHECK(actor_system_spawn(&g_stress_sys, STRESS_ACTOR_B) == ACTOR_OK);
    router_init();
    CHECK(router_register_tenant(STRESS_TENANT_A, STRESS_ACTOR_A) == ROUTER_OK);
    CHECK(router_register_tenant(STRESS_TENANT_B, STRESS_ACTOR_B) == ROUTER_OK);
    /* STRESS_INVALID_TENANT is deliberately never registered. */

    producer_ctx_t pctx[2u * STRESS_PRODUCERS_PER_TENANT];
    consumer_ctx_t cctx_a, cctx_b;
    memset(&cctx_a, 0, sizeof cctx_a);
    memset(&cctx_b, 0, sizeof cctx_b);
    cctx_a.actor_id = STRESS_ACTOR_A;
    cctx_a.expected_tenant_id = STRESS_TENANT_A;
    cctx_a.tenant_ok = 1;
    cctx_b.actor_id = STRESS_ACTOR_B;
    cctx_b.expected_tenant_id = STRESS_TENANT_B;
    cctx_b.tenant_ok = 1;

    _Atomic int stop_flag = 0;
    flood_ctx_t flood_ctx;
    flood_ctx.stop_flag = &stop_flag;
    flood_ctx.sent = 0;

#if defined(_WIN32)
    HANDLE producers[2u * STRESS_PRODUCERS_PER_TENANT];
    HANDLE consumers[2];
    HANDLE flood;

    for (uint32_t i = 0; i < STRESS_PRODUCERS_PER_TENANT; i++) {
        pctx[i].tenant_id = STRESS_TENANT_A;
        producers[i] = CreateThread(NULL, 0, stress_producer_thread, &pctx[i], 0, NULL);
        CHECK(producers[i] != NULL);
    }
    for (uint32_t i = 0; i < STRESS_PRODUCERS_PER_TENANT; i++) {
        uint32_t idx = STRESS_PRODUCERS_PER_TENANT + i;
        pctx[idx].tenant_id = STRESS_TENANT_B;
        producers[idx] = CreateThread(NULL, 0, stress_producer_thread, &pctx[idx], 0, NULL);
        CHECK(producers[idx] != NULL);
    }
    consumers[0] = CreateThread(NULL, 0, stress_consumer_thread, &cctx_a, 0, NULL);
    consumers[1] = CreateThread(NULL, 0, stress_consumer_thread, &cctx_b, 0, NULL);
    CHECK(consumers[0] != NULL && consumers[1] != NULL);
    flood = CreateThread(NULL, 0, stress_flood_thread, &flood_ctx, 0, NULL);
    CHECK(flood != NULL);

    WaitForMultipleObjects(2u * STRESS_PRODUCERS_PER_TENANT, producers, TRUE, INFINITE);
    WaitForMultipleObjects(2, consumers, TRUE, INFINITE);
    atomic_store_explicit(&stop_flag, 1, memory_order_relaxed);
    WaitForSingleObject(flood, INFINITE);

    for (uint32_t i = 0; i < 2u * STRESS_PRODUCERS_PER_TENANT; i++) {
        CloseHandle(producers[i]);
    }
    CloseHandle(consumers[0]);
    CloseHandle(consumers[1]);
    CloseHandle(flood);
#else
    pthread_t producers[2u * STRESS_PRODUCERS_PER_TENANT];
    pthread_t consumers[2];
    pthread_t flood;

    for (uint32_t i = 0; i < STRESS_PRODUCERS_PER_TENANT; i++) {
        pctx[i].tenant_id = STRESS_TENANT_A;
        CHECK(pthread_create(&producers[i], NULL, stress_producer_thread, &pctx[i]) == 0);
    }
    for (uint32_t i = 0; i < STRESS_PRODUCERS_PER_TENANT; i++) {
        uint32_t idx = STRESS_PRODUCERS_PER_TENANT + i;
        pctx[idx].tenant_id = STRESS_TENANT_B;
        CHECK(pthread_create(&producers[idx], NULL, stress_producer_thread, &pctx[idx]) == 0);
    }
    CHECK(pthread_create(&consumers[0], NULL, stress_consumer_thread, &cctx_a) == 0);
    CHECK(pthread_create(&consumers[1], NULL, stress_consumer_thread, &cctx_b) == 0);
    CHECK(pthread_create(&flood, NULL, stress_flood_thread, &flood_ctx) == 0);

    for (uint32_t i = 0; i < 2u * STRESS_PRODUCERS_PER_TENANT; i++) {
        pthread_join(producers[i], NULL);
    }
    pthread_join(consumers[0], NULL);
    pthread_join(consumers[1], NULL);
    atomic_store_explicit(&stop_flag, 1, memory_order_relaxed);
    pthread_join(flood, NULL);
#endif

    const uint64_t per_tenant_total = (uint64_t)STRESS_PRODUCERS_PER_TENANT * STRESS_N_PER_PRODUCER;
    const uint64_t per_producer_sum = (STRESS_N_PER_PRODUCER * (STRESS_N_PER_PRODUCER - 1)) / 2;
    const uint64_t expected_checksum = per_producer_sum * STRESS_PRODUCERS_PER_TENANT;

    CHECK(cctx_a.consumed == per_tenant_total);
    CHECK(cctx_b.consumed == per_tenant_total);
    CHECK(cctx_a.checksum == expected_checksum);
    CHECK(cctx_b.checksum == expected_checksum);
    CHECK(cctx_a.tenant_ok != 0); /* actor A NEVER saw a message from tenant B, or vice versa */
    CHECK(cctx_b.tenant_ok != 0);

    actor_t *a0 = actor_system_get(&g_stress_sys, STRESS_ACTOR_A);
    actor_t *a1 = actor_system_get(&g_stress_sys, STRESS_ACTOR_B);
    CHECK(rb_is_empty(&a0->mailbox) == true);
    CHECK(rb_is_empty(&a1->mailbox) == true);

    router_stats_t stats;
    router_get_stats(&stats);
    CHECK(stats.frames_routed == 2u * per_tenant_total);
    CHECK(stats.rejected_unknown_tenant == flood_ctx.sent);
    CHECK(flood_ctx.sent > 0u); /* the flood thread actually ran concurrently */

    printf("   tenants=2 producers/tenant=%u per_producer=%" PRIu64
           " consumed_A=%" PRIu64 " consumed_B=%" PRIu64 "\n",
           STRESS_PRODUCERS_PER_TENANT, STRESS_N_PER_PRODUCER, cctx_a.consumed, cctx_b.consumed);
    printf("   checksum_A=%" PRIu64 " checksum_B=%" PRIu64 " (expected %" PRIu64 " each)\n",
           cctx_a.checksum, cctx_b.checksum, expected_checksum);
    printf("   invalid-tenant flood: %" PRIu64 " frames rejected concurrently, zero leaked in\n",
           flood_ctx.sent);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_concurrent_core: ring_buffer + actor_core + msg_router ===\n");

    test_ring_msg_layout();
    test_ring_buffer_layout();
    test_actor_layout();
    test_actor_private_memory_isolation();
    test_router_basic_roundtrip();
    test_tenant_filtering_isolation();
    test_backpressure_no_cross_actor_impact();
    test_concurrent_multi_tenant_stress();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
