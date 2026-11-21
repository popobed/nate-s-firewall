/*
 * test_net_io.c - Unit + true multi-client stress test bench for
 * net_io.{h,c}: non-blocking Winsock ingestion, O(1) tenant routing via
 * g_tenant_actor_map, and zero-copy recv()-straight-into-mailbox delivery.
 *
 * Windows-only (net_io.h itself #errors on any other platform), so this
 * file always uses the Win32 thread API directly -- no POSIX branch.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching net_io's own zero-allocation constraint.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net_io.h"
#include <windows.h> /* CreateThread/HANDLE et al. -- net_io.h only pulls in winsock2.h/ws2tcpip.h;
                      * WIN32_LEAN_AND_MEAN is already defined by the time this is reached, so
                      * windows.h does not re-declare the old winsock.h behind our back. */

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

/* Pure bounded spin, deliberately no Sleep()/SwitchToThread(): see this
 * project's actor-mailbox precedent (net_gateway.h's design note, and
 * net_io.h's own "Determinism" note) -- on this sandboxed VM, frequent
 * voluntary yielding across concurrently active threads was empirically
 * far less reliable than a tight bounded spin. */
#define POLL_SPIN_LIMIT 20000000

/* ---------------------------------------------------------------- */
/* Client helper: connects, sends tenant_id + a fixed-size payload,     */
/* optionally fragmented across several small send() calls.            */
/* ---------------------------------------------------------------- */

static bool client_send_frame(uint16_t port, uint8_t tenant_id,
                               const uint8_t *payload, size_t payload_len, bool fragmented) {
    if (payload_len != NET_IO_PAYLOAD_LEN) {
        return false;
    }

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

    bool ok = true;
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        ok = false;
    }

    if (ok) {
        uint8_t frame[1 + NET_IO_PAYLOAD_LEN];
        frame[0] = tenant_id;
        memcpy(frame + 1, payload, payload_len);
        size_t total = sizeof frame;

        if (!fragmented) {
            if (send(s, (const char *)frame, (int)total, 0) != (int)total) {
                ok = false;
            }
        } else {
            /* Send in small, deliberately awkward chunks to exercise the
             * server's multi-poll accumulation path (a TCP frame is not
             * guaranteed to arrive in one recv() -- this proves net_io
             * handles that correctly rather than assuming one-shot). */
            size_t sent = 0;
            const size_t chunk_sizes[] = {1, 3, 5, 7, 11, 13, 17};
            size_t chunk_i = 0;
            while (sent < total && ok) {
                size_t want = chunk_sizes[chunk_i % (sizeof chunk_sizes / sizeof chunk_sizes[0])];
                if (want > total - sent) {
                    want = total - sent;
                }
                int n = send(s, (const char *)(frame + sent), (int)want, 0);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                sent += (size_t)n;
                chunk_i++;
            }
        }
    }

    closesocket(s);
    WSACleanup();
    return ok;
}

/* ---------------------------------------------------------------- */
/* Basic round trip                                                   */
/* ---------------------------------------------------------------- */

static net_io_server_t g_srv;
static ngw_mailbox_t   g_mb0;

static uint32_t poll_until(net_io_server_t *srv, uint32_t target_frames) {
    uint32_t total = 0;
    for (int i = 0; i < POLL_SPIN_LIMIT && total < target_frames; i++) {
        total += net_io_poll_once(srv);
    }
    return total;
}

static void test_basic_zero_copy_roundtrip(void) {
    SECTION("connect -> send -> poll -> zero-copy mailbox delivery");

    CHECK(net_io_init(&g_srv, 0u) == NET_IO_OK);
    ngw_mailbox_init(&g_mb0);
    CHECK(net_io_register_actor(&g_srv, 0u, &g_mb0) == NET_IO_OK);
    CHECK(net_io_register_tenant(7u, 0u) == NET_IO_OK);

    uint16_t port = net_io_get_port(&g_srv);
    CHECK(port != 0u);

    uint8_t payload[NET_IO_PAYLOAD_LEN];
    memset(payload, 0, sizeof payload);
    memcpy(payload, "hello-net-io", 12);

    CHECK(client_send_frame(port, 7u, payload, sizeof payload, false) == true);

    uint32_t got = poll_until(&g_srv, 1u);
    CHECK(got == 1u);

    ngw_message_t msg;
    CHECK(ngw_mailbox_consume(&g_mb0, &msg) == NGW_OK);
    CHECK(msg.sender_id == 7u); /* tenant_id, as published by net_io */
    CHECK(msg.type == 0u);
    CHECK(memcmp(msg.payload, payload, sizeof payload) == 0);

    net_io_stats_t stats;
    net_io_get_stats(&g_srv, &stats);
    CHECK(stats.frames_ingested_zero_copy == 1u);
    CHECK(stats.rejected_total == 0u);
    CHECK(stats.connections_accepted == 1u);
    CHECK(stats.connections_closed == 1u);

    net_io_shutdown(&g_srv);
}

/* ---------------------------------------------------------------- */
/* Invalid Tenant ID: O(1) rejection, socket cut, no mailbox message   */
/* ---------------------------------------------------------------- */

static void test_invalid_tenant_rejected(void) {
    SECTION("invalid Tenant ID: immediate rejection, closesocket(), rejected_total++");

    CHECK(net_io_init(&g_srv, 0u) == NET_IO_OK);
    ngw_mailbox_init(&g_mb0);
    CHECK(net_io_register_actor(&g_srv, 0u, &g_mb0) == NET_IO_OK);
    CHECK(net_io_register_tenant(7u, 0u) == NET_IO_OK); /* tenant 7 valid; 200 is not registered */

    uint16_t port = net_io_get_port(&g_srv);
    uint8_t payload[NET_IO_PAYLOAD_LEN];
    memset(payload, 0xAB, sizeof payload);

    CHECK(client_send_frame(port, 200u, payload, sizeof payload, false) == true);

    /* Give the server a bounded number of polls to react; no frame should
     * ever appear (the rejection happens before any payload byte is read). */
    for (int i = 0; i < POLL_SPIN_LIMIT / 100; i++) {
        net_io_poll_once(&g_srv);
    }

    CHECK(ngw_mailbox_is_empty(&g_mb0) == true);

    net_io_stats_t stats;
    net_io_get_stats(&g_srv, &stats);
    CHECK(stats.frames_ingested_zero_copy == 0u);
    CHECK(stats.rejected_total == 1u);

    /* The mailbox itself must never have been touched: no reservation is
     * made before the Tenant ID check passes. */
    CHECK(ngw_mailbox_count(&g_mb0) == 0u);

    net_io_shutdown(&g_srv);
}

/* ---------------------------------------------------------------- */
/* Fragmented TCP delivery: multi-poll accumulation into the SAME slot */
/* ---------------------------------------------------------------- */

static void test_fragmented_delivery(void) {
    SECTION("frame split across many small send() calls still lands intact");

    CHECK(net_io_init(&g_srv, 0u) == NET_IO_OK);
    ngw_mailbox_init(&g_mb0);
    CHECK(net_io_register_actor(&g_srv, 0u, &g_mb0) == NET_IO_OK);
    CHECK(net_io_register_tenant(3u, 0u) == NET_IO_OK);

    uint16_t port = net_io_get_port(&g_srv);
    uint8_t payload[NET_IO_PAYLOAD_LEN];
    for (size_t i = 0; i < sizeof payload; i++) {
        payload[i] = (uint8_t)(i * 7u + 1u);
    }

    CHECK(client_send_frame(port, 3u, payload, sizeof payload, true) == true);

    uint32_t got = poll_until(&g_srv, 1u);
    CHECK(got == 1u);

    ngw_message_t msg;
    CHECK(ngw_mailbox_consume(&g_mb0, &msg) == NGW_OK);
    CHECK(memcmp(msg.payload, payload, sizeof payload) == 0);

    net_io_shutdown(&g_srv);
}

/* ---------------------------------------------------------------- */
/* True multi-client concurrency stress                                */
/* ---------------------------------------------------------------- */

#define STRESS_CLIENTS 8u
#define STRESS_TENANT  9u

static net_io_server_t g_stress_srv;
static ngw_mailbox_t   g_stress_mb;
static uint16_t        g_stress_port;

typedef struct {
    uint32_t client_index;
    bool     send_ok;
} stress_client_ctx_t;

static DWORD WINAPI stress_client_thread(LPVOID arg) {
    stress_client_ctx_t *ctx = (stress_client_ctx_t *)arg;
    uint8_t payload[NET_IO_PAYLOAD_LEN];
    memset(payload, 0, sizeof payload);
    uint64_t seq = ctx->client_index;
    memcpy(payload, &seq, sizeof seq);

    ctx->send_ok = client_send_frame(g_stress_port, STRESS_TENANT, payload, sizeof payload, false);
    return 0;
}

static void test_concurrent_clients(void) {
    SECTION("true multi-client stress: concurrent connects + zero-copy delivery");

    CHECK(net_io_init(&g_stress_srv, 0u) == NET_IO_OK);
    ngw_mailbox_init(&g_stress_mb);
    CHECK(net_io_register_actor(&g_stress_srv, 0u, &g_stress_mb) == NET_IO_OK);
    CHECK(net_io_register_tenant(STRESS_TENANT, 0u) == NET_IO_OK);

    g_stress_port = net_io_get_port(&g_stress_srv);
    CHECK(g_stress_port != 0u);

    stress_client_ctx_t ctx[STRESS_CLIENTS];
    HANDLE threads[STRESS_CLIENTS];
    for (uint32_t i = 0; i < STRESS_CLIENTS; i++) {
        ctx[i].client_index = i;
        ctx[i].send_ok = false;
        threads[i] = CreateThread(NULL, 0, stress_client_thread, &ctx[i], 0, NULL);
        CHECK(threads[i] != NULL);
    }

    /* The server itself stays single-threaded, polled from here -- exactly
     * the "no intermediary thread" requirement -- while multiple real
     * client threads connect and send concurrently. */
    uint32_t got = poll_until(&g_stress_srv, STRESS_CLIENTS);
    CHECK(got == STRESS_CLIENTS);

    WaitForMultipleObjects(STRESS_CLIENTS, threads, TRUE, INFINITE);
    for (uint32_t i = 0; i < STRESS_CLIENTS; i++) {
        CHECK(ctx[i].send_ok == true);
        CloseHandle(threads[i]);
    }

    uint64_t checksum = 0;
    uint32_t consumed = 0;
    ngw_message_t msg;
    while (ngw_mailbox_consume(&g_stress_mb, &msg) == NGW_OK) {
        CHECK(msg.sender_id == STRESS_TENANT);
        uint64_t seq = 0xFFFFFFFFFFFFFFFFULL;
        memcpy(&seq, msg.payload, sizeof seq);
        checksum += seq;
        consumed++;
    }
    CHECK(consumed == STRESS_CLIENTS);

    uint64_t expected_checksum = 0;
    for (uint32_t i = 0; i < STRESS_CLIENTS; i++) {
        expected_checksum += i;
    }
    CHECK(checksum == expected_checksum);

    net_io_stats_t stats;
    net_io_get_stats(&g_stress_srv, &stats);
    CHECK(stats.frames_ingested_zero_copy == STRESS_CLIENTS);
    CHECK(stats.rejected_total == 0u);

    printf("   clients=%u consumed=%u checksum=%" PRIu64 " (expected %" PRIu64 ")\n",
           STRESS_CLIENTS, consumed, checksum, expected_checksum);

    net_io_shutdown(&g_stress_srv);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_net_io ===\n");

    test_basic_zero_copy_roundtrip();
    test_invalid_tenant_rejected();
    test_fragmented_delivery();
    test_concurrent_clients();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
