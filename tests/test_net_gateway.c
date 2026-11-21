/*
 * test_net_gateway.c - Unit + true multi-thread stress test bench for
 * net_gateway.{h,c}: frame build/parse round trip, authentication
 * (constant-time, via crypto_constant_time), zero-copy routing into an
 * actor mailbox, and concurrent producer/consumer correctness.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching net_gateway's own zero-allocation constraint.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_gateway.h"

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

static const uint8_t KEY_A[NGW_API_KEY_LEN] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF
};
static const uint8_t KEY_WRONG[NGW_API_KEY_LEN] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

/* ---------------------------------------------------------------- */
/* Layout: cache-line compaction                                     */
/* ---------------------------------------------------------------- */

static void test_message_layout(void) {
    SECTION("ngw_message_t: exactly 64 bytes, 64-byte aligned");
    CHECK(sizeof(ngw_message_t) == 64u);
    CHECK(_Alignof(ngw_message_t) == 64u);
}

static void test_mailbox_layout(void) {
    SECTION("ngw_mailbox_t: tail/head each on their own cache line");
    CHECK(offsetof(ngw_mailbox_t, tail) == 0);
    CHECK(offsetof(ngw_mailbox_t, head) == NGW_CACHE_LINE_SIZE);
    CHECK(offsetof(ngw_mailbox_t, slots) == 2u * NGW_CACHE_LINE_SIZE);
    CHECK(_Alignof(ngw_mailbox_t) >= NGW_CACHE_LINE_SIZE);
}

static void test_descriptor_fits(void) {
    SECTION("ngw_descriptor_t fits in one mailbox message payload");
    CHECK(sizeof(ngw_descriptor_t) <= NGW_MESSAGE_PAYLOAD_SIZE);
}

/* ---------------------------------------------------------------- */
/* Frame build / parse round trip                                    */
/* ---------------------------------------------------------------- */

static ngw_gateway_t g_gw;
static ngw_mailbox_t g_mb0;
static ngw_mailbox_t g_mb1;

static void test_basic_ingest_roundtrip(void) {
    SECTION("build -> ingest -> mailbox -> decode -> release, happy path");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb0);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb0) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 100u, KEY_A, 0u) == NGW_OK);

    const uint8_t payload[] = "hello-gateway";
    uint8_t frame[NGW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_WEBHOOK, 100u, KEY_A,
                           payload, (uint16_t)(sizeof payload)) == NGW_OK);

    CHECK(ngw_mailbox_is_empty(&g_mb0) == true);
    ngw_status_t st = ngw_ingest_frame(&g_gw, frame, frame_len);
    CHECK(st == NGW_OK);
    CHECK(ngw_mailbox_count(&g_mb0) == 1u);

    ngw_message_t msg;
    CHECK(ngw_mailbox_consume(&g_mb0, &msg) == NGW_OK);
    CHECK(msg.type == (uint32_t)NGW_PROTO_WEBHOOK);
    CHECK(msg.sender_id == 100u); /* tenant_id, as posted by ngw_ingest_frame */

    ngw_descriptor_t desc;
    CHECK(ngw_decode_message(&msg, &desc) == NGW_OK);
    CHECK(desc.len == sizeof payload);
    CHECK(desc.tenant_id == 100u);
    CHECK(desc.protocol == (uint8_t)NGW_PROTO_WEBHOOK);
    CHECK(memcmp(desc.ptr, payload, sizeof payload) == 0);

    ngw_release_payload(&g_gw, desc.ptr);

    ngw_stats_t stats;
    ngw_get_stats(&g_gw, &stats);
    CHECK(stats.frames_ingested == 1u);
    CHECK(stats.frames_accepted == 1u);
}

/* ---------------------------------------------------------------- */
/* Rejection paths                                                    */
/* ---------------------------------------------------------------- */

static void test_rejections(void) {
    SECTION("rejection paths: auth, unknown tenant, corruption, malformed");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb0);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb0) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 100u, KEY_A, 0u) == NGW_OK);

    const uint8_t payload[] = "payload";
    uint8_t frame[NGW_MAX_FRAME_SIZE];
    size_t frame_len = 0;

    /* Wrong API key -> constant-time rejection, no message posted. */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_WRONG,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_ERR_AUTH_FAILED);
    CHECK(ngw_mailbox_is_empty(&g_mb0) == true);

    /* Unknown/unregistered tenant. */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 999u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_ERR_UNKNOWN_TENANT);

    /* Corrupted frame: flip one payload byte after building (hash no
     * longer matches -> integrity rejection, not silently accepted). */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    frame[NGW_HEADER_SIZE] ^= 0xFFu; /* first payload byte */
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_ERR_INTEGRITY);

    /* Truncated frame. */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len - 1u) == NGW_ERR_TRUNCATED);

    /* Bad magic. */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    frame[0] ^= 0xFFu;
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_ERR_BAD_MAGIC);

    /* Oversized payload rejected by the builder itself. */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_A,
                           payload, (uint16_t)(NGW_MAX_PAYLOAD + 1u)) == NGW_ERR_PAYLOAD_TOO_LARGE);

    /* Actor registered for the tenant but never actually attached. */
    ngw_gateway_t gw2;
    ngw_gateway_init(&gw2);
    CHECK(ngw_register_tenant(&gw2, 100u, KEY_A, 3u) == NGW_OK); /* actor_id 3 never registered */
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&gw2, frame, frame_len) == NGW_ERR_ACTOR_NOT_REGISTERED);

    ngw_stats_t stats;
    ngw_get_stats(&g_gw, &stats);
    CHECK(stats.frames_accepted == 0u);
    CHECK(stats.frames_rejected_auth >= 2u);   /* wrong key + unknown tenant */
    CHECK(stats.frames_rejected_bad_frame >= 3u); /* integrity + truncated + bad magic */
}

/* ---------------------------------------------------------------- */
/* Multi-tenant routing                                               */
/* ---------------------------------------------------------------- */

static void test_multi_tenant_routing(void) {
    SECTION("two tenants route to two different actor mailboxes");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb0);
    ngw_mailbox_init(&g_mb1);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb0) == NGW_OK);
    CHECK(ngw_register_actor(&g_gw, 1u, &g_mb1) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 10u, KEY_A, 0u) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 20u, KEY_A, 1u) == NGW_OK);

    uint8_t frame[NGW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    const uint8_t payload[] = "x";

    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_UDP, 10u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_OK);

    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_UDP, 20u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_OK);

    CHECK(ngw_mailbox_count(&g_mb0) == 1u);
    CHECK(ngw_mailbox_count(&g_mb1) == 1u);

    ngw_message_t msg;
    CHECK(ngw_mailbox_consume(&g_mb0, &msg) == NGW_OK);
    CHECK(msg.sender_id == 10u);
    ngw_descriptor_t d0;
    ngw_decode_message(&msg, &d0);
    ngw_release_payload(&g_gw, d0.ptr);

    CHECK(ngw_mailbox_consume(&g_mb1, &msg) == NGW_OK);
    CHECK(msg.sender_id == 20u);
    ngw_descriptor_t d1;
    ngw_decode_message(&msg, &d1);
    ngw_release_payload(&g_gw, d1.ptr);
}

/* ---------------------------------------------------------------- */
/* Capacity: mailbox full / slab exhausted are deterministic O(1)     */
/* rejections, and a rejected post never leaks the slab block it      */
/* provisionally allocated.                                           */
/* ---------------------------------------------------------------- */

static void test_capacity_rejection_no_leak(void) {
    SECTION("mailbox full: O(1) rejection, no slab leak");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb0);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb0) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 1u, KEY_A, 0u) == NGW_OK);

    uint8_t frame[NGW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    const uint8_t payload[] = "p";
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 1u, KEY_A,
                           payload, (uint16_t)sizeof payload) == NGW_OK);

    /* NGW_SLAB_CAPACITY == NGW_MAILBOX_CAPACITY here, and ngw_ingest_frame
     * allocates a slab block BEFORE attempting the mailbox post, so once
     * both are simultaneously saturated the rejection reason observed is
     * NGW_ERR_SLAB_EXHAUSTED, not NGW_ERR_MAILBOX_FULL -- both are
     * expected, capacity-driven O(1) rejections in this test. */
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < NGW_MAILBOX_CAPACITY + 8u; i++) {
        ngw_status_t st = ngw_ingest_frame(&g_gw, frame, frame_len);
        if (st == NGW_OK) {
            accepted++;
        } else {
            CHECK(st == NGW_ERR_MAILBOX_FULL || st == NGW_ERR_SLAB_EXHAUSTED);
        }
    }
    CHECK(accepted == (uint32_t)NGW_MAILBOX_CAPACITY);
    CHECK(ngw_mailbox_is_full(&g_mb0) == true);

    /* Drain everything; each accepted frame provisionally took one slab
     * block, so the rejected attempts above must NOT have consumed any --
     * confirm the pool is exactly back to full capacity once drained. */
    uint32_t drained = 0;
    ngw_message_t msg;
    while (ngw_mailbox_consume(&g_mb0, &msg) == NGW_OK) {
        ngw_descriptor_t d;
        ngw_decode_message(&msg, &d);
        ngw_release_payload(&g_gw, d.ptr);
        drained++;
    }
    CHECK(drained == (uint32_t)NGW_MAILBOX_CAPACITY);

    /* If any slab block had leaked, this second full-capacity round would
     * fall short of NGW_MAILBOX_CAPACITY accepted. */
    accepted = 0;
    for (uint32_t i = 0; i < NGW_MAILBOX_CAPACITY; i++) {
        if (ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_OK) {
            accepted++;
        }
    }
    CHECK(accepted == (uint32_t)NGW_MAILBOX_CAPACITY);
}

/* ---------------------------------------------------------------- */
/* True multi-producer / single-consumer concurrency stress           */
/* ---------------------------------------------------------------- */

#define STRESS_PRODUCERS       4u
#define STRESS_N_PER_PRODUCER  ((uint64_t)20000)
#define STRESS_TENANT_ID       42u

static ngw_gateway_t g_stress_gw;
static ngw_mailbox_t g_stress_mb;

typedef struct {
    uint32_t sender_id;
} stress_producer_ctx_t;

typedef struct {
    uint64_t total_consumed;
    uint64_t checksum;
    uint64_t expected_seq[STRESS_PRODUCERS];
    int      order_ok;
    int      content_ok;
} stress_consumer_ctx_t;

/* Deliberately a pure bounded spin, no SwitchToThread()/sched_yield():
 * see this project's actor-mailbox precedent (net_gateway.h's design
 * note) -- on this sandboxed VM, frequent voluntary yielding across
 * several CPU-bound threads was empirically far LESS reliable than
 * letting the OS's ordinary preemptive time-slicing handle fairness. */

#if defined(_WIN32)
static DWORD WINAPI stress_producer_thread(LPVOID arg) {
    stress_producer_ctx_t *ctx = (stress_producer_ctx_t *)arg;
    uint8_t frame[NGW_MAX_FRAME_SIZE];

    for (uint64_t seq = 0; seq < STRESS_N_PER_PRODUCER; seq++) {
        uint8_t payload[8];
        memcpy(payload, &seq, sizeof seq);

        size_t frame_len = 0;
        ngw_status_t bst = ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP,
                                            STRESS_TENANT_ID, KEY_A, payload, sizeof payload);
        (void)bst;

        for (;;) {
            ngw_status_t st = ngw_ingest_frame(&g_stress_gw, frame, frame_len);
            if (st == NGW_OK) {
                break;
            }
            /* NGW_ERR_MAILBOX_FULL or NGW_ERR_SLAB_EXHAUSTED: transient
             * backpressure while the consumer catches up. Pure spin. */
        }
    }
    (void)ctx;
    return 0;
}

static DWORD WINAPI stress_consumer_thread(LPVOID arg) {
    stress_consumer_ctx_t *ctx = (stress_consumer_ctx_t *)arg;
    const uint64_t target = (uint64_t)STRESS_PRODUCERS * STRESS_N_PER_PRODUCER;
    ngw_message_t msg;

    while (ctx->total_consumed < target) {
        if (ngw_mailbox_consume(&g_stress_mb, &msg) == NGW_OK) {
            ngw_descriptor_t d;
            ngw_decode_message(&msg, &d);

            uint64_t seq = 0xFFFFFFFFFFFFFFFFULL;
            if (d.len == sizeof seq) {
                memcpy(&seq, d.ptr, sizeof seq);
            } else {
                ctx->content_ok = 0;
            }
            if (d.tenant_id != STRESS_TENANT_ID) {
                ctx->content_ok = 0;
            }

            uint32_t sender = msg.sender_id; /* == tenant_id, all producers share one tenant here */
            (void)sender;

            ngw_release_payload(&g_stress_gw, d.ptr);

            ctx->checksum += seq;
            ctx->total_consumed++;
        }
    }
    return 0;
}
#else
static void *stress_producer_thread(void *arg) {
    stress_producer_ctx_t *ctx = (stress_producer_ctx_t *)arg;
    uint8_t frame[NGW_MAX_FRAME_SIZE];

    for (uint64_t seq = 0; seq < STRESS_N_PER_PRODUCER; seq++) {
        uint8_t payload[8];
        memcpy(payload, &seq, sizeof seq);

        size_t frame_len = 0;
        ngw_status_t bst = ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP,
                                            STRESS_TENANT_ID, KEY_A, payload, sizeof payload);
        (void)bst;

        for (;;) {
            ngw_status_t st = ngw_ingest_frame(&g_stress_gw, frame, frame_len);
            if (st == NGW_OK) {
                break;
            }
        }
    }
    (void)ctx;
    return NULL;
}

static void *stress_consumer_thread(void *arg) {
    stress_consumer_ctx_t *ctx = (stress_consumer_ctx_t *)arg;
    const uint64_t target = (uint64_t)STRESS_PRODUCERS * STRESS_N_PER_PRODUCER;
    ngw_message_t msg;

    while (ctx->total_consumed < target) {
        if (ngw_mailbox_consume(&g_stress_mb, &msg) == NGW_OK) {
            ngw_descriptor_t d;
            ngw_decode_message(&msg, &d);

            uint64_t seq = 0xFFFFFFFFFFFFFFFFULL;
            if (d.len == sizeof seq) {
                memcpy(&seq, d.ptr, sizeof seq);
            } else {
                ctx->content_ok = 0;
            }
            if (d.tenant_id != STRESS_TENANT_ID) {
                ctx->content_ok = 0;
            }

            ngw_release_payload(&g_stress_gw, d.ptr);

            ctx->checksum += seq;
            ctx->total_consumed++;
        }
    }
    return NULL;
}
#endif

static void test_concurrent_stress(void) {
    SECTION("true multi-producer/single-consumer stress (net_gateway end-to-end)");

    ngw_gateway_init(&g_stress_gw);
    ngw_mailbox_init(&g_stress_mb);
    CHECK(ngw_register_actor(&g_stress_gw, 0u, &g_stress_mb) == NGW_OK);
    CHECK(ngw_register_tenant(&g_stress_gw, STRESS_TENANT_ID, KEY_A, 0u) == NGW_OK);

    stress_producer_ctx_t pctx[STRESS_PRODUCERS];
    stress_consumer_ctx_t cctx;
    memset(&cctx, 0, sizeof cctx);
    cctx.order_ok = 1;
    cctx.content_ok = 1;

#if defined(_WIN32)
    HANDLE hp[STRESS_PRODUCERS];
    for (uint32_t i = 0; i < STRESS_PRODUCERS; i++) {
        pctx[i].sender_id = i;
        hp[i] = CreateThread(NULL, 0, stress_producer_thread, &pctx[i], 0, NULL);
        CHECK(hp[i] != NULL);
    }
    HANDLE hc = CreateThread(NULL, 0, stress_consumer_thread, &cctx, 0, NULL);
    CHECK(hc != NULL);
    WaitForMultipleObjects(STRESS_PRODUCERS, hp, TRUE, INFINITE);
    WaitForSingleObject(hc, INFINITE);
    for (uint32_t i = 0; i < STRESS_PRODUCERS; i++) {
        CloseHandle(hp[i]);
    }
    CloseHandle(hc);
#else
    pthread_t tp[STRESS_PRODUCERS];
    pthread_t tc;
    for (uint32_t i = 0; i < STRESS_PRODUCERS; i++) {
        pctx[i].sender_id = i;
        CHECK(pthread_create(&tp[i], NULL, stress_producer_thread, &pctx[i]) == 0);
    }
    CHECK(pthread_create(&tc, NULL, stress_consumer_thread, &cctx) == 0);
    for (uint32_t i = 0; i < STRESS_PRODUCERS; i++) {
        pthread_join(tp[i], NULL);
    }
    pthread_join(tc, NULL);
#endif

    const uint64_t total = (uint64_t)STRESS_PRODUCERS * STRESS_N_PER_PRODUCER;
    /* All STRESS_PRODUCERS producers post sequences 0..N-1 under the SAME
     * tenant, so the aggregate checksum is STRESS_PRODUCERS independent
     * copies of the same 0..N-1 sum -- this does not check per-producer
     * ordering (unlike this project's actor-mailbox precedent, multiple
     * distinct producer identities are not distinguishable once routed
     * through a single tenant), only that every posted sequence value
     * was received exactly once in aggregate and with intact content. */
    const uint64_t per_producer_sum = (STRESS_N_PER_PRODUCER * (STRESS_N_PER_PRODUCER - 1)) / 2;
    const uint64_t expected_checksum = per_producer_sum * STRESS_PRODUCERS;

    CHECK(cctx.total_consumed == total);
    CHECK(cctx.checksum == expected_checksum);
    CHECK(cctx.content_ok != 0);
    CHECK(ngw_mailbox_is_empty(&g_stress_mb) == true);

    ngw_stats_t stats;
    ngw_get_stats(&g_stress_gw, &stats);
    CHECK(stats.frames_accepted == total);
    CHECK(stats.frames_rejected_bad_frame == 0u);
    CHECK(stats.frames_rejected_auth == 0u);

    printf("   producers=%u per_producer=%" PRIu64 " total_consumed=%" PRIu64
           " checksum=%" PRIu64 " (expected %" PRIu64 ")\n",
           STRESS_PRODUCERS, STRESS_N_PER_PRODUCER, cctx.total_consumed, cctx.checksum, expected_checksum);
}

/* ---------------------------------------------------------------- */
/* Security hardening: no residual cross-tenant data on slab reuse   */
/* ---------------------------------------------------------------- */

static void test_slab_free_zeroes_residual_tenant_data(void) {
    SECTION("SECURITY: ngw_slab_free() zeroes the ENTIRE block on release -- no residual "
            "cross-tenant payload bytes survive into the next allocation of the same block");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb0);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb0) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 100u, KEY_A, 0u) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 200u, KEY_A, 0u) == NGW_OK);

    /* Tenant A sends a large, distinctive payload filling most of one
     * slab block. */
    static uint8_t big_payload[NGW_MAX_PAYLOAD - 8u];
    memset(big_payload, 0xA5, sizeof big_payload);

    uint8_t frame[NGW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 100u, KEY_A,
                           big_payload, (uint16_t)sizeof big_payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_OK);

    ngw_message_t msg1;
    CHECK(ngw_mailbox_consume(&g_mb0, &msg1) == NGW_OK);
    ngw_descriptor_t desc1;
    CHECK(ngw_decode_message(&msg1, &desc1) == NGW_OK);
    CHECK(desc1.len == sizeof big_payload);
    uint8_t *first_block = desc1.ptr;
    CHECK(memcmp(first_block, big_payload, sizeof big_payload) == 0);

    /* Tenant A releases it -- the (now-hardened) slab free path must zero
     * the whole block, not just the 4-byte free-list link. */
    ngw_release_payload(&g_gw, desc1.ptr);

    /* Tenant B sends a small payload. The Treiber-stack free list is LIFO
     * and this test is single-threaded, so this allocation deterministically
     * reuses the exact block just freed. */
    const uint8_t small_payload[] = "hi";
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_HTTP, 200u, KEY_A,
                           small_payload, (uint16_t)sizeof small_payload) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_OK);

    ngw_message_t msg2;
    CHECK(ngw_mailbox_consume(&g_mb0, &msg2) == NGW_OK);
    ngw_descriptor_t desc2;
    CHECK(ngw_decode_message(&msg2, &desc2) == NGW_OK);
    CHECK(desc2.len == sizeof small_payload);

    CHECK(desc2.ptr == first_block); /* confirms this test actually exercises reuse of the SAME block */

    /* The defining assertion: every byte beyond tenant B's own small
     * payload must be zero -- none of tenant A's 0xA5 bytes may survive. */
    CHECK(memcmp(desc2.ptr, small_payload, sizeof small_payload) == 0);
    for (size_t i = sizeof small_payload; i < NGW_MAX_PAYLOAD; i++) {
        CHECK(desc2.ptr[i] == 0u);
    }

    ngw_release_payload(&g_gw, desc2.ptr);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_net_gateway ===\n");

    test_message_layout();
    test_mailbox_layout();
    test_descriptor_fits();
    test_basic_ingest_roundtrip();
    test_rejections();
    test_multi_tenant_routing();
    test_capacity_rejection_no_leak();
    test_slab_free_zeroes_residual_tenant_data();
    test_concurrent_stress();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
