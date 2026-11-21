/*
 * test_chrono_ingest.c - End-to-end bench for chrono_ingest.{h,c}: drives
 * a real loopback TCP client against the real net_io.c wire-mode reactor,
 * through chrono_ingest's drain, into the real barry_storage.c append
 * queue, and reads the durably-queued result back out.
 *
 * Windows-only (chrono_ingest.h #errors on any other platform, same as
 * net_io.h), so this file uses the Win32 thread API directly, same
 * posture as test_net_io.c. No dynamic allocation anywhere in this file.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "chrono_ingest.h"
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

/* Pure bounded spin, no Sleep()/SwitchToThread() -- same rationale as
 * test_net_io.c's POLL_SPIN_LIMIT (empirically more reliable on this
 * sandboxed VM than voluntary yielding). */
#define POLL_SPIN_LIMIT 20000000

typedef struct {
    uint16_t port;
    uint16_t tenant_id;
    const uint8_t *payload;
    uint16_t payload_len;
    bool fragmented;
    bool connected_ok;
} client_job_t;

static DWORD WINAPI client_thread_main(LPVOID arg) {
    client_job_t *job = (client_job_t *)arg;
    job->connected_ok = false;

    wire_msg_t frame;
    if (wire_build_msg(&frame, job->tenant_id, 1u, job->payload, job->payload_len) != WIRE_OK) {
        return 0;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(job->port);

    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        closesocket(s);
        WSACleanup();
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)&frame;
    size_t total = sizeof frame;
    size_t sent = 0;

    if (job->fragmented) {
        /* Send in small, deliberately awkward chunks to exercise net_io's
         * resume-at-offset path (a frame arriving across several recv()
         * calls), not just the single-shot case. */
        const size_t chunk_sizes[] = {3, 1, 10, 50, 0 /* remainder */};
        for (size_t i = 0; chunk_sizes[i] != 0 || sent < total; i++) {
            size_t chunk = (i < 4) ? chunk_sizes[i] : (total - sent);
            if (chunk == 0) {
                chunk = total - sent;
            }
            if (sent + chunk > total) {
                chunk = total - sent;
            }
            int n = send(s, (const char *)(bytes + sent), (int)chunk, 0);
            if (n <= 0) {
                break;
            }
            sent += (size_t)n;
            Sleep(5); /* client-side only: give the server a real chance to observe a partial frame */
            if (sent >= total) {
                break;
            }
        }
    } else {
        while (sent < total) {
            int n = send(s, (const char *)(bytes + sent), (int)(total - sent), 0);
            if (n <= 0) {
                break;
            }
            sent += (size_t)n;
        }
    }

    job->connected_ok = (sent == total);

    /* Give the server time to finish recv()ing before we tear the socket
     * down -- one frame per connection, by protocol (net_io.h). */
    Sleep(20);
    closesocket(s);
    WSACleanup();
    return 0;
}

static bool run_client(uint16_t port, uint16_t tenant_id, const char *text, bool fragmented) {
    client_job_t job;
    job.port = port;
    job.tenant_id = tenant_id;
    job.payload = (const uint8_t *)text;
    job.payload_len = (uint16_t)strlen(text);
    job.fragmented = fragmented;
    job.connected_ok = false;

    HANDLE h = CreateThread(NULL, 0, client_thread_main, &job, 0, NULL);
    if (h == NULL) {
        return false;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return job.connected_ok;
}

static uint32_t poll_until(chrono_ingest_ctx_t *ctx, uint32_t want_total) {
    uint32_t total = 0;
    for (long i = 0; i < POLL_SPIN_LIMIT && total < want_total; i++) {
        total += chrono_ingest_poll_once(ctx);
    }
    return total;
}

int main(void) {
    printf("=== chrono_ingest end-to-end bench ===\n");

    chrono_ingest_ctx_t ctx;
    SECTION("init");
    CHECK(chrono_ingest_init(&ctx, 0u) == CHRONO_INGEST_OK);

    uint32_t stream_a = UINT32_MAX, stream_b = UINT32_MAX;
    CHECK(chrono_ingest_register_stream(&ctx, &stream_a) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_stream(&ctx, &stream_b) == CHRONO_INGEST_OK);
    CHECK(stream_a == 0u && stream_b == 1u);

    CHECK(chrono_ingest_register_tenant(42u, stream_a) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_tenant(7u, stream_b) == CHRONO_INGEST_OK);
    /* tenant_id 99 is deliberately left UNMAPPED, to exercise rejection. */

    uint16_t port = chrono_ingest_get_port(&ctx);
    printf("[server] listening on 127.0.0.1:%u\n", (unsigned)port);
    CHECK(port != 0u);

    SECTION("ingest: single-shot frame, tenant 42 -> stream A");
    CHECK(run_client(port, 42u, "CHRONO log entry #1 -- audit: user_login ok", false));
    uint32_t got1 = poll_until(&ctx, 1u);
    CHECK(got1 == 1u);

    SECTION("ingest: fragmented frame (multi-recv reassembly), tenant 7 -> stream B");
    CHECK(run_client(port, 7u, "CHRONO log entry #2 -- audit: config_change applied", true));
    uint32_t got2 = poll_until(&ctx, 1u);
    CHECK(got2 == 1u);

    SECTION("ingest: second frame on stream A, to prove ordering across polls");
    CHECK(run_client(port, 42u, "CHRONO log entry #3 -- audit: user_logout ok", false));
    uint32_t got3 = poll_until(&ctx, 1u);
    CHECK(got3 == 1u);

    SECTION("reject: unmapped tenant_id 99 (no payload ever read for it)");
    CHECK(run_client(port, 99u, "should never be stored", false));
    /* A rejection produces no frame to wait for; a few idle polls are
     * enough for net_io.c to observe and cut the connection. */
    for (int i = 0; i < 200000; i++) {
        chrono_ingest_poll_once(&ctx);
    }

    chrono_ingest_stats_t stats;
    chrono_ingest_get_stats(&ctx, &stats);

    SECTION("stats (real counters from net_io.c + chrono_ingest.c)");
    printf("  net.frames_ingested_zero_copy = %" PRIu64 "\n", stats.net.frames_ingested_zero_copy);
    printf("  net.rejected_total            = %" PRIu64 "\n", stats.net.rejected_total);
    printf("  net.connections_accepted      = %" PRIu64 "\n", stats.net.connections_accepted);
    printf("  net.connections_closed        = %" PRIu64 "\n", stats.net.connections_closed);
    printf("  chrono.frames_appended        = %" PRIu64 "\n", stats.frames_appended);
    printf("  chrono.append_backpressure    = %" PRIu64 "\n", stats.append_backpressure);
    printf("  chrono.append_queue_depth     = %zu\n", stats.append_queue_depth);

    CHECK(stats.net.frames_ingested_zero_copy == 3u);
    CHECK(stats.net.rejected_total == 1u);
    CHECK(stats.frames_appended == 3u);
    CHECK(stats.append_queue_depth == 3u);

    SECTION("dequeue: read back every durably-queued entry, verify content + order");
    for (int i = 0; i < 3; i++) {
        chrono_log_entry_t entry;
        barry_status_t st = chrono_ingest_dequeue(&ctx, &entry);
        CHECK(st == BARRY_OK);
        if (st == BARRY_OK) {
            printf("  [seq %" PRIu64 "] stream=%u tenant=%u payload=\"%.*s\"\n",
                   entry.chrono_seq, entry.stream_id, (unsigned)entry.frame.tenant_id,
                   (int)entry.frame.payload_len, (const char *)entry.frame.payload);
            CHECK(entry.chrono_seq == (uint64_t)i);
            CHECK(entry.frame.magic == WIRE_MAGIC);
        }
    }

    chrono_log_entry_t empty_check;
    CHECK(chrono_ingest_dequeue(&ctx, &empty_check) == BARRY_ERR_EMPTY);

    chrono_ingest_shutdown(&ctx);

    printf("=== %d checks run, %d failed ===\n", g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
