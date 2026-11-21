/*
 * test_wire_proto.c - Unit + concurrency stress test bench for
 * wire_proto.{h,c} (the compact __attribute__((packed)) 64-byte binary
 * protocol and its dedicated lock-free ring), plus -- on Windows -- a
 * true Winsock round-trip exercising net_io.c's wire-mode zero-copy
 * ingestion (net_io_wire_*).
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching wire_proto's own zero-allocation constraint.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wire_proto.h"

#if defined(_WIN32)
  #include "net_io.h"
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
/* Layout: packed wire format + 64-byte-aligned slots                 */
/* ---------------------------------------------------------------- */

static void test_wire_msg_layout(void) {
    SECTION("wire_msg_t: exactly 64 bytes, packed, no inserted padding");
    CHECK(sizeof(wire_msg_t) == 64u);
    CHECK(offsetof(wire_msg_t, magic) == 0u);
    CHECK(offsetof(wire_msg_t, tenant_id) == 4u);
    CHECK(offsetof(wire_msg_t, type) == 6u);
    CHECK(offsetof(wire_msg_t, version) == 7u);
    CHECK(offsetof(wire_msg_t, payload_len) == 8u);
    CHECK(offsetof(wire_msg_t, payload) == 10u);
    /* A packed struct carries no natural alignment requirement of its
     * own (confirmed empirically before this file was written) -- 64-
     * byte alignment is a property of the CONTAINER, checked next. */
    CHECK(_Alignof(wire_msg_t) == 1u);
}

static wire_ring_t g_ring;

static void test_wire_ring_layout(void) {
    SECTION("wire_ring_t: tail/head/ready/slots all 64-byte aligned, slots contiguous 64B apart");
    CHECK(offsetof(wire_ring_t, tail) == 0u);
    CHECK(offsetof(wire_ring_t, head) == WIRE_CACHE_LINE_SIZE);
    CHECK((offsetof(wire_ring_t, ready) % WIRE_CACHE_LINE_SIZE) == 0u);
    CHECK((offsetof(wire_ring_t, slots) % WIRE_CACHE_LINE_SIZE) == 0u);
    CHECK(_Alignof(wire_ring_t) >= WIRE_CACHE_LINE_SIZE);

    wire_ring_init(&g_ring);
    const uint8_t *s0 = (const uint8_t *)&g_ring.slots[0];
    const uint8_t *s1 = (const uint8_t *)&g_ring.slots[1];
    CHECK((size_t)(s1 - s0) == 64u); /* each slot is exactly one cache line from the next */
}

/* ---------------------------------------------------------------- */
/* Basic reserve/publish/pop round trip                               */
/* ---------------------------------------------------------------- */

static void test_basic_roundtrip(void) {
    SECTION("wire_build_msg -> reserve -> zero-copy write -> publish -> pop");

    wire_ring_init(&g_ring);
    CHECK(wire_ring_is_empty(&g_ring) == true);

    wire_msg_t built;
    const uint8_t payload[] = "hello-wire-proto";
    CHECK(wire_build_msg(&built, 42u, 7u, payload, (uint16_t)sizeof payload) == WIRE_OK);
    CHECK(built.magic == WIRE_MAGIC);

    wire_msg_t *slot = NULL;
    CHECK(wire_ring_reserve(&g_ring, &slot) == WIRE_OK);
    memcpy(slot, &built, sizeof built); /* simulates a zero-copy recv() writing the full 64 bytes directly */
    wire_ring_publish(&g_ring, slot, slot->type);

    CHECK(wire_ring_count(&g_ring) == 1u);

    wire_msg_t out = {0};
    CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_OK);
    CHECK(out.tenant_id == 42u);
    CHECK(out.type == 7u);
    CHECK(out.payload_len == (uint16_t)sizeof payload);
    CHECK(memcmp(out.payload, payload, sizeof payload) == 0);
    CHECK(wire_ring_is_empty(&g_ring) == true);
    CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_ERR_EMPTY);
}

static void test_reserve_zeroes_residual_data_on_reuse(void) {
    SECTION("SECURITY: wire_ring_reserve() zeroes a slot's full 64 bytes before reuse -- "
            "no residual bytes survive from whichever message last occupied it");

    wire_ring_init(&g_ring);

    wire_msg_t *slot = NULL;
    CHECK(wire_ring_reserve(&g_ring, &slot) == WIRE_OK); /* index 0 */
    /* Simulate a zero-copy recv() writing a full, distinctive 64-byte
     * frame directly into the slot. */
    memset(slot, 0xA5, sizeof *slot);
    wire_ring_publish(&g_ring, slot, 1u);

    wire_msg_t out = {0};
    CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_OK); /* frees index 0 for reuse */

    /* Advance all the way around the ring so the NEXT reservation targets
     * the exact same physical slot (index 0) again -- true reuse, not a
     * fresh, never-touched slot. */
    for (uint32_t i = 1; i < WIRE_RING_CAPACITY; i++) {
        CHECK(wire_ring_reserve(&g_ring, &slot) == WIRE_OK);
        wire_ring_publish(&g_ring, slot, WIRE_MSG_TYPE_DISCARD);
        CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_OK);
    }

    CHECK(wire_ring_reserve(&g_ring, &slot) == WIRE_OK);
    CHECK((size_t)(slot - g_ring.slots) == 0u); /* confirms this really is index 0 again */

    /* Before this hardening, a producer that only partially wrote the
     * slot (a short recv(), say) would still see 0xA5 here. Even without
     * writing anything at all, the slot must already read back as fully
     * zero the instant it is reserved. */
    const uint8_t *raw = (const uint8_t *)slot;
    for (size_t i = 0; i < sizeof(*slot); i++) {
        CHECK(raw[i] == 0u);
    }

    wire_ring_publish(&g_ring, slot, WIRE_MSG_TYPE_DISCARD);
    CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_OK);
}

static void test_discard_sentinel_never_stalls_ring(void) {
    SECTION("an aborted reservation, published as DISCARD, frees its slot normally");

    wire_ring_init(&g_ring);
    wire_msg_t *slot = NULL;
    CHECK(wire_ring_reserve(&g_ring, &slot) == WIRE_OK);
    /* Simulate a connection that died mid-frame: never write real data,
     * publish the discard sentinel instead. */
    wire_ring_publish(&g_ring, slot, WIRE_MSG_TYPE_DISCARD);

    wire_msg_t out = {0};
    CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_OK);
    CHECK(out.type == WIRE_MSG_TYPE_DISCARD);
    CHECK(wire_ring_is_empty(&g_ring) == true);

    /* The slot is fully reusable afterwards -- no permanent stall. */
    CHECK(wire_ring_reserve(&g_ring, &slot) == WIRE_OK);
    wire_ring_publish(&g_ring, slot, 1u);
    CHECK(wire_ring_try_pop(&g_ring, &out) == WIRE_OK);
    CHECK(out.type == 1u);
}

static void test_backpressure(void) {
    SECTION("O(1) backpressure: full ring rejects immediately, drains and refills cleanly");

    wire_ring_init(&g_ring);
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < WIRE_RING_CAPACITY + 8u; i++) {
        wire_msg_t *slot;
        wire_status_t st = wire_ring_reserve(&g_ring, &slot);
        if (st == WIRE_OK) {
            wire_ring_publish(&g_ring, slot, 0u);
            accepted++;
        } else {
            CHECK(st == WIRE_ERR_FULL);
        }
    }
    CHECK(accepted == (uint32_t)WIRE_RING_CAPACITY);
    CHECK(wire_ring_is_full(&g_ring) == true);

    uint32_t drained = 0;
    wire_msg_t out = {0};
    while (wire_ring_try_pop(&g_ring, &out) == WIRE_OK) {
        drained++;
    }
    CHECK(drained == (uint32_t)WIRE_RING_CAPACITY);
    CHECK(wire_ring_is_empty(&g_ring) == true);
}

/* ---------------------------------------------------------------- */
/* True multi-producer / single-consumer concurrency stress            */
/* (ring only -- no sockets involved)                                 */
/* ---------------------------------------------------------------- */

#define RING_STRESS_PRODUCERS      4u
#define RING_STRESS_N_PER_PRODUCER ((uint64_t)25000)

static wire_ring_t g_stress_ring;

typedef struct {
    uint16_t tenant_id;
} ring_producer_ctx_t;

typedef struct {
    uint64_t consumed;
    uint64_t checksum;
} ring_consumer_ctx_t;

#if defined(_WIN32)
static DWORD WINAPI ring_stress_producer(LPVOID arg) {
#else
static void *ring_stress_producer(void *arg) {
#endif
    ring_producer_ctx_t *ctx = (ring_producer_ctx_t *)arg;
    for (uint64_t seq = 0; seq < RING_STRESS_N_PER_PRODUCER; seq++) {
        for (;;) { /* pure bounded spin: see this project's established precedent */
            wire_msg_t *slot;
            if (wire_ring_reserve(&g_stress_ring, &slot) == WIRE_OK) {
                slot->magic = WIRE_MAGIC;
                slot->tenant_id = ctx->tenant_id;
                slot->version = (uint8_t)WIRE_VERSION;
                slot->payload_len = (uint16_t)sizeof seq;
                memcpy(slot->payload, &seq, sizeof seq);
                memset(slot->payload + sizeof seq, 0, WIRE_MSG_PAYLOAD_SIZE - sizeof seq);
                wire_ring_publish(&g_stress_ring, slot, 0u);
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
static DWORD WINAPI ring_stress_consumer(LPVOID arg) {
#else
static void *ring_stress_consumer(void *arg) {
#endif
    ring_consumer_ctx_t *ctx = (ring_consumer_ctx_t *)arg;
    const uint64_t target = (uint64_t)RING_STRESS_PRODUCERS * RING_STRESS_N_PER_PRODUCER;
    wire_msg_t msg;
    while (ctx->consumed < target) {
        if (wire_ring_try_pop(&g_stress_ring, &msg) == WIRE_OK) {
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

static void test_ring_concurrent_stress(void) {
    SECTION("true multi-producer/single-consumer stress on wire_ring_t directly");

    wire_ring_init(&g_stress_ring);
    ring_producer_ctx_t pctx[RING_STRESS_PRODUCERS];
    ring_consumer_ctx_t cctx;
    memset(&cctx, 0, sizeof cctx);

#if defined(_WIN32)
    HANDLE producers[RING_STRESS_PRODUCERS];
    HANDLE consumer;
    for (uint32_t i = 0; i < RING_STRESS_PRODUCERS; i++) {
        pctx[i].tenant_id = (uint16_t)i;
        producers[i] = CreateThread(NULL, 0, ring_stress_producer, &pctx[i], 0, NULL);
        CHECK(producers[i] != NULL);
    }
    consumer = CreateThread(NULL, 0, ring_stress_consumer, &cctx, 0, NULL);
    CHECK(consumer != NULL);
    WaitForMultipleObjects(RING_STRESS_PRODUCERS, producers, TRUE, INFINITE);
    WaitForSingleObject(consumer, INFINITE);
    for (uint32_t i = 0; i < RING_STRESS_PRODUCERS; i++) {
        CloseHandle(producers[i]);
    }
    CloseHandle(consumer);
#else
    pthread_t producers[RING_STRESS_PRODUCERS];
    pthread_t consumer;
    for (uint32_t i = 0; i < RING_STRESS_PRODUCERS; i++) {
        pctx[i].tenant_id = (uint16_t)i;
        CHECK(pthread_create(&producers[i], NULL, ring_stress_producer, &pctx[i]) == 0);
    }
    CHECK(pthread_create(&consumer, NULL, ring_stress_consumer, &cctx) == 0);
    for (uint32_t i = 0; i < RING_STRESS_PRODUCERS; i++) {
        pthread_join(producers[i], NULL);
    }
    pthread_join(consumer, NULL);
#endif

    const uint64_t total = (uint64_t)RING_STRESS_PRODUCERS * RING_STRESS_N_PER_PRODUCER;
    const uint64_t per_producer_sum = (RING_STRESS_N_PER_PRODUCER * (RING_STRESS_N_PER_PRODUCER - 1)) / 2;
    const uint64_t expected_checksum = per_producer_sum * RING_STRESS_PRODUCERS;

    CHECK(cctx.consumed == total);
    CHECK(cctx.checksum == expected_checksum);
    CHECK(wire_ring_is_empty(&g_stress_ring) == true);

    printf("   producers=%u per_producer=%" PRIu64 " consumed=%" PRIu64
           " checksum=%" PRIu64 " (expected %" PRIu64 ")\n",
           RING_STRESS_PRODUCERS, RING_STRESS_N_PER_PRODUCER, cctx.consumed, cctx.checksum, expected_checksum);
}

/* ================================================================== */
/* Windows-only: real Winsock zero-copy round trip (net_io.c wire-mode) */
/* ================================================================== */

#if defined(_WIN32)

#define WIO_POLL_SPIN_LIMIT 20000000

static net_io_wire_server_t g_wsrv;
static wire_ring_t          g_wring;

static bool wio_send_raw(uint16_t port, const uint8_t *bytes, size_t len, bool fragmented) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    bool ok = (connect(s, (struct sockaddr *)&addr, sizeof addr) != SOCKET_ERROR);
    if (ok) {
        if (!fragmented) {
            ok = (send(s, (const char *)bytes, (int)len, 0) == (int)len);
        } else {
            const size_t chunk_sizes[] = {1, 4, 9, 13, 20};
            size_t sent = 0, ci = 0;
            while (sent < len && ok) {
                size_t want = chunk_sizes[ci % (sizeof chunk_sizes / sizeof chunk_sizes[0])];
                if (want > len - sent) {
                    want = len - sent;
                }
                int n = send(s, (const char *)(bytes + sent), (int)want, 0);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                sent += (size_t)n;
                ci++;
            }
        }
    }
    closesocket(s);
    WSACleanup();
    return ok;
}

static uint32_t wio_poll_until(uint32_t target) {
    uint32_t total = 0;
    for (int i = 0; i < WIO_POLL_SPIN_LIMIT && total < target; i++) {
        total += net_io_wire_poll_once(&g_wsrv);
    }
    return total;
}

static void test_winsock_zero_copy_roundtrip(void) {
    SECTION("Winsock: recv() straight into a wire_ring_t slot, no decode step");

    CHECK(net_io_wire_init(&g_wsrv, 0u) == NET_IO_OK);
    wire_ring_init(&g_wring);
    CHECK(net_io_wire_register_actor(&g_wsrv, 0u, &g_wring) == NET_IO_OK);
    CHECK(net_io_wire_register_tenant(5u, 0u) == NET_IO_OK);

    uint16_t port = net_io_wire_get_port(&g_wsrv);
    CHECK(port != 0u);

    wire_msg_t frame;
    const uint8_t payload[] = "zero-copy-native-frame";
    CHECK(wire_build_msg(&frame, 5u, 3u, payload, (uint16_t)sizeof payload) == WIRE_OK);

    CHECK(wio_send_raw(port, (const uint8_t *)&frame, sizeof frame, false) == true);
    CHECK(wio_poll_until(1u) == 1u);

    wire_msg_t out = {0};
    CHECK(wire_ring_try_pop(&g_wring, &out) == WIRE_OK);
    CHECK(out.tenant_id == 5u);
    CHECK(out.type == 3u);
    CHECK(memcmp(out.payload, payload, sizeof payload) == 0);

    net_io_wire_stats_t stats;
    net_io_wire_get_stats(&g_wsrv, &stats);
    CHECK(stats.frames_ingested_zero_copy == 1u);
    CHECK(stats.rejected_total == 0u);

    net_io_wire_shutdown(&g_wsrv);
}

static void test_winsock_fragmented_and_rejections(void) {
    SECTION("Winsock: fragmented 64-byte frame still lands intact; bad magic / unknown tenant rejected");

    CHECK(net_io_wire_init(&g_wsrv, 0u) == NET_IO_OK);
    wire_ring_init(&g_wring);
    CHECK(net_io_wire_register_actor(&g_wsrv, 0u, &g_wring) == NET_IO_OK);
    CHECK(net_io_wire_register_tenant(11u, 0u) == NET_IO_OK);
    uint16_t port = net_io_wire_get_port(&g_wsrv);

    wire_msg_t good;
    const uint8_t p[] = "fragmented-ok";
    CHECK(wire_build_msg(&good, 11u, 1u, p, (uint16_t)sizeof p) == WIRE_OK);
    CHECK(wio_send_raw(port, (const uint8_t *)&good, sizeof good, true) == true);
    CHECK(wio_poll_until(1u) == 1u);

    wire_msg_t out = {0};
    CHECK(wire_ring_try_pop(&g_wring, &out) == WIRE_OK);
    CHECK(memcmp(out.payload, p, sizeof p) == 0);

    /* Bad magic: rejected before any ring is touched. */
    wire_msg_t bad_magic;
    CHECK(wire_build_msg(&bad_magic, 11u, 1u, p, (uint16_t)sizeof p) == WIRE_OK);
    bad_magic.magic ^= 0xFFFFFFFFu;
    CHECK(wio_send_raw(port, (const uint8_t *)&bad_magic, sizeof bad_magic, false) == true);
    for (int i = 0; i < WIO_POLL_SPIN_LIMIT / 200; i++) {
        net_io_wire_poll_once(&g_wsrv);
    }
    CHECK(wire_ring_is_empty(&g_wring) == true);

    /* Unknown tenant: also rejected before any ring is touched. */
    wire_msg_t bad_tenant;
    CHECK(wire_build_msg(&bad_tenant, 9999u, 1u, p, (uint16_t)sizeof p) == WIRE_OK);
    CHECK(wio_send_raw(port, (const uint8_t *)&bad_tenant, sizeof bad_tenant, false) == true);
    for (int i = 0; i < WIO_POLL_SPIN_LIMIT / 200; i++) {
        net_io_wire_poll_once(&g_wsrv);
    }
    CHECK(wire_ring_is_empty(&g_wring) == true);

    net_io_wire_stats_t stats;
    net_io_wire_get_stats(&g_wsrv, &stats);
    CHECK(stats.frames_ingested_zero_copy == 1u);
    CHECK(stats.rejected_total == 2u);

    net_io_wire_shutdown(&g_wsrv);
}

#endif /* _WIN32 */

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_wire_proto ===\n");

    test_wire_msg_layout();
    test_wire_ring_layout();
    test_basic_roundtrip();
    test_reserve_zeroes_residual_data_on_reuse();
    test_discard_sentinel_never_stalls_ring();
    test_backpressure();
    test_ring_concurrent_stress();
#if defined(_WIN32)
    test_winsock_zero_copy_roundtrip();
    test_winsock_fragmented_and_rejections();
#endif

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
