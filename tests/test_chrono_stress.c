/*
 * test_chrono_stress.c - Concurrent load bench for chrono_ingest.c: one
 * background thread runs the real net_io.c wire-mode reactor
 * (chrono_ingest_poll_once()), a second background thread continuously
 * seals durably-queued entries into the hash chain
 * (chrono_ingest_seal_next()), and 8 client threads each open 1000
 * separate loopback TCP connections as fast as possible (8000 frames
 * total) -- real concurrent I/O against the real reactor, not a
 * single-threaded simulation.
 *
 * ============================== Threading contracts, precisely ===============
 * chrono_ingest_poll_once() and chrono_ingest_seal_next() each require a
 * SINGLE caller thread (chrono_ingest.h) -- satisfied here: exactly one
 * dedicated thread calls each, for the whole run. They are safe to run
 * CONCURRENTLY with each other (poll_once() only ever touches
 * ctx->netio/streams/append_queue; seal_next() only ever touches
 * ctx->append_queue/chain/chain_len -- disjoint field sets except for the
 * MPMC-safe append_queue both go through). The 8 client threads never
 * touch ctx at all -- they only open their own sockets.
 *
 * The one remaining hazard this file is careful about: chrono_ingest_ctx_t's
 * own bookkeeping fields (frames_appended, append_backpressure, chain,
 * chain_len) are PLAIN, non-atomic fields, written only by the poll/seal
 * threads. Reading them from main() is only safe AFTER both threads have
 * been joined (a real synchronization point, not merely "probably done").
 * While they are still running, this file's main() polls quiescence using
 * ONLY the fields that genuinely are atomic and documented safe for
 * concurrent reads: net_io_wire_stats_t's counters (net_io.c: `_Atomic
 * uint64_t` throughout) and barry_task_queue_count() (barry_storage.h:
 * "Safe from any thread"), plus two small atomics this file owns itself
 * (g_sealed_count, g_chain_became_full). No other cross-thread read in
 * this file touches a non-atomic ctx field before the corresponding
 * thread has been joined.
 *
 * ============================== The 4096 < 8000 capacity mismatch =============
 * CHRONO_CHAIN_CAPACITY (chrono_ingest.h) is 4096 -- a fixed, disclosed,
 * zero-malloc bound on the in-RAM sealed chain, unchanged here rather
 * than bumped for this one test's convenience (bumping it would grow
 * every chrono_ingest_ctx_t in the project, including the three already-
 * validated test binaries, for a single stress scenario's sake). 8000
 * requested frames therefore CANNOT all fit in one chain: once 4096 are
 * sealed, chrono_ingest_seal_next() starts returning
 * CHRONO_INGEST_ERR_CHAIN_FULL, the seal thread stops, and the still-full
 * append_queue (256 capacity) and wire rings (64 capacity each, 8 of
 * them) then apply exactly the bounded, deterministic backpressure this
 * architecture is designed to produce under sustained overload -- a real,
 * honest, and arguably MORE interesting stress-test outcome than quietly
 * quadrupling a shared capacity constant to make the numbers fit. This
 * file measures and reports whichever outcome actually occurs, including
 * whether the chain saturated.
 */

#include <inttypes.h>
#include <stdbool.h>
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

#define CLIENT_THREAD_COUNT 8u
#define FRAMES_PER_CLIENT   1000u
#define TOTAL_FRAMES        (CLIENT_THREAD_COUNT * FRAMES_PER_CLIENT)

/* ---------------------------------------------------------------- */
/* Background reactor thread (network -> append_queue)                */
/* ---------------------------------------------------------------- */

static _Atomic int g_stop_polling = 0;

static DWORD WINAPI poll_thread_main(LPVOID arg) {
    chrono_ingest_ctx_t *ctx = (chrono_ingest_ctx_t *)arg;
    for (;;) {
        chrono_ingest_poll_once(ctx);
        if (atomic_load_explicit(&g_stop_polling, memory_order_relaxed)) {
            break;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Background sealing thread (append_queue -> hash chain)             */
/* ---------------------------------------------------------------- */

static _Atomic int g_stop_sealing = 0;
static _Atomic uint32_t g_sealed_count = 0;
static _Atomic int g_chain_became_full = 0;

static DWORD WINAPI seal_thread_main(LPVOID arg) {
    chrono_ingest_ctx_t *ctx = (chrono_ingest_ctx_t *)arg;
    for (;;) {
        bool stop_requested = atomic_load_explicit(&g_stop_sealing, memory_order_relaxed) != 0;

        chrono_block_t blk;
        chrono_ingest_status_t st = chrono_ingest_seal_next(ctx, &blk);

        if (st == CHRONO_INGEST_OK) {
            atomic_fetch_add_explicit(&g_sealed_count, 1u, memory_order_relaxed);
            continue; /* keep draining eagerly while anything is ready */
        }
        if (st == CHRONO_INGEST_ERR_CHAIN_FULL) {
            atomic_store_explicit(&g_chain_became_full, 1, memory_order_relaxed);
            break; /* CHRONO_CHAIN_CAPACITY reached -- nothing more this thread can ever do, see file header */
        }
        /* st == CHRONO_INGEST_ERR_EMPTY: nothing ready right now. */
        if (stop_requested) {
            break; /* stop requested AND genuinely nothing left to seal -- done */
        }
        /* else: still running, just nothing to seal yet -- loop again (tight spin, no Sleep()). */
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Client threads: 1000 independent loopback connections each          */
/* ---------------------------------------------------------------- */

typedef struct {
    uint16_t port;
    uint16_t tenant_id;
    uint32_t frames_to_send;
    uint32_t send_ok;
    uint32_t send_failed;
} client_thread_arg_t;

static DWORD WINAPI client_thread_main(LPVOID arg_ptr) {
    client_thread_arg_t *arg = (client_thread_arg_t *)arg_ptr;
    arg->send_ok = 0;
    arg->send_failed = 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        arg->send_failed = arg->frames_to_send;
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(arg->port);

    for (uint32_t i = 0; i < arg->frames_to_send; i++) {
        char payload[32];
        int plen = snprintf(payload, sizeof payload, "stress-t%u-#%u", (unsigned)arg->tenant_id, (unsigned)i);

        wire_msg_t frame;
        if (plen < 0 || wire_build_msg(&frame, arg->tenant_id, 1u, (const uint8_t *)payload, (uint16_t)plen) != WIRE_OK) {
            arg->send_failed++;
            continue;
        }

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            arg->send_failed++;
            continue;
        }

        if (connect(s, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
            closesocket(s);
            arg->send_failed++;
            continue;
        }

        const uint8_t *bytes = (const uint8_t *)&frame;
        size_t total = sizeof frame;
        size_t sent = 0;
        bool ok = true;
        while (sent < total) {
            int n = send(s, (const char *)(bytes + sent), (int)(total - sent), 0);
            if (n <= 0) {
                ok = false;
                break;
            }
            sent += (size_t)n;
        }
        closesocket(s);

        if (ok) {
            arg->send_ok++;
        } else {
            arg->send_failed++;
        }
    }

    WSACleanup();
    return 0;
}

/* ---------------------------------------------------------------- */

static double qpc_elapsed_seconds(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq) {
    return (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

int main(void) {
    printf("=== chrono_ingest concurrent stress bench ===\n");
    printf("  %u client threads x %u frames = %u total frames\n",
           (unsigned)CLIENT_THREAD_COUNT, (unsigned)FRAMES_PER_CLIENT, (unsigned)TOTAL_FRAMES);

    SECTION("init: 1 server, 8 streams/tenants (one per client thread)");
    chrono_ingest_ctx_t ctx; /* ~550KB: too large for a safe local on some configurations if
                              * CHRONO_CHAIN_CAPACITY ever grows, but fine at its current fixed
                              * size within the default 1MB thread stack, same as every other
                              * chrono_* test bench. */
    CHECK(chrono_ingest_init(&ctx, 0u) == CHRONO_INGEST_OK);

    uint16_t tenant_of_stream[CLIENT_THREAD_COUNT];
    for (uint32_t i = 0; i < CLIENT_THREAD_COUNT; i++) {
        uint32_t stream_id = UINT32_MAX;
        CHECK(chrono_ingest_register_stream(&ctx, &stream_id) == CHRONO_INGEST_OK);
        CHECK(stream_id == i);
        uint16_t tenant_id = (uint16_t)(10u + i); /* NET_IO_WIRE_MAX_TENANTS is 256 -- stay well under it */
        CHECK(chrono_ingest_register_tenant(tenant_id, stream_id) == CHRONO_INGEST_OK);
        tenant_of_stream[i] = tenant_id;
    }

    uint16_t port = chrono_ingest_get_port(&ctx);
    printf("[server] listening on 127.0.0.1:%u\n", (unsigned)port);
    CHECK(port != 0u);

    SECTION("launch background reactor + sealer threads");
    HANDLE poll_handle = CreateThread(NULL, 0, poll_thread_main, &ctx, 0, NULL);
    HANDLE seal_handle = CreateThread(NULL, 0, seal_thread_main, &ctx, 0, NULL);
    CHECK(poll_handle != NULL);
    CHECK(seal_handle != NULL);

    SECTION("launch 8 client threads, 1000 frames each, concurrently");
    client_thread_arg_t client_args[CLIENT_THREAD_COUNT];
    HANDLE client_handles[CLIENT_THREAD_COUNT];
    for (uint32_t i = 0; i < CLIENT_THREAD_COUNT; i++) {
        client_args[i].port = port;
        client_args[i].tenant_id = tenant_of_stream[i];
        client_args[i].frames_to_send = FRAMES_PER_CLIENT;
        client_args[i].send_ok = 0;
        client_args[i].send_failed = 0;
    }

    LARGE_INTEGER freq, t_start, t_end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_start);

    for (uint32_t i = 0; i < CLIENT_THREAD_COUNT; i++) {
        client_handles[i] = CreateThread(NULL, 0, client_thread_main, &client_args[i], 0, NULL);
        CHECK(client_handles[i] != NULL);
    }
    WaitForMultipleObjects(CLIENT_THREAD_COUNT, client_handles, TRUE, INFINITE);

    QueryPerformanceCounter(&t_end);
    double load_gen_seconds = qpc_elapsed_seconds(t_start, t_end, freq);

    for (uint32_t i = 0; i < CLIENT_THREAD_COUNT; i++) {
        CloseHandle(client_handles[i]);
    }

    uint32_t total_send_ok = 0, total_send_failed = 0;
    for (uint32_t i = 0; i < CLIENT_THREAD_COUNT; i++) {
        total_send_ok += client_args[i].send_ok;
        total_send_failed += client_args[i].send_failed;
    }
    CHECK(total_send_ok + total_send_failed == TOTAL_FRAMES);

    SECTION("drain quiescence: wait for the reactor/sealer to finish processing what was sent");
    /* Only atomic-backed reads here -- see file header's threading-
     * contract note. Declares quiescence once connections_closed, the
     * queue depth, and the sealed count have all been unchanged for 20
     * consecutive 10ms ticks (~200ms of genuine silence), or the chain
     * hits its hard capacity (nothing more will ever change after that). */
    uint64_t prev_closed = (uint64_t)-1;
    size_t prev_qdepth = (size_t)-1;
    uint32_t prev_sealed = (uint32_t)-1;
    int stable_ticks = 0;
    const int STABLE_TARGET = 20;

    for (;;) {
        Sleep(10);
        net_io_wire_stats_t nstats;
        net_io_wire_get_stats(&ctx.netio, &nstats);
        size_t qdepth = barry_task_queue_count(&ctx.append_queue);
        uint32_t sealed = atomic_load_explicit(&g_sealed_count, memory_order_relaxed);

        if (atomic_load_explicit(&g_chain_became_full, memory_order_relaxed)) {
            break; /* sealing is permanently done -- nothing left to wait for on that front */
        }
        if (nstats.connections_closed == prev_closed && qdepth == prev_qdepth && sealed == prev_sealed) {
            stable_ticks++;
            if (stable_ticks >= STABLE_TARGET) {
                break;
            }
        } else {
            stable_ticks = 0;
            prev_closed = nstats.connections_closed;
            prev_qdepth = qdepth;
            prev_sealed = sealed;
        }
    }

    atomic_store_explicit(&g_stop_polling, 1, memory_order_relaxed);
    atomic_store_explicit(&g_stop_sealing, 1, memory_order_relaxed);
    WaitForSingleObject(poll_handle, INFINITE);
    WaitForSingleObject(seal_handle, INFINITE);
    CloseHandle(poll_handle);
    CloseHandle(seal_handle);
    /* Both background threads are now joined: every ctx field below is
     * safe to read directly, no race possible. */

    chrono_ingest_stats_t stats;
    chrono_ingest_get_stats(&ctx, &stats);
    uint32_t sealed_final = atomic_load_explicit(&g_sealed_count, memory_order_relaxed);
    bool chain_full = atomic_load_explicit(&g_chain_became_full, memory_order_relaxed) != 0;

    uint64_t accounted = stats.net.frames_ingested_zero_copy + stats.net.rejected_total + stats.net.rejected_ring_full;
    uint64_t unaccounted = (stats.net.connections_accepted > accounted) ? (stats.net.connections_accepted - accounted) : 0u;

    SECTION("results: client-side view (this process' own sockets)");
    printf("  frames attempted        = %u\n", (unsigned)TOTAL_FRAMES);
    printf("  client send_ok          = %u\n", (unsigned)total_send_ok);
    printf("  client send_failed      = %u\n", (unsigned)total_send_failed);

    SECTION("results: server-side ground truth (net_io.c real counters)");
    printf("  connections_accepted    = %" PRIu64 "\n", stats.net.connections_accepted);
    printf("  connections_closed      = %" PRIu64 "\n", stats.net.connections_closed);
    printf("  frames_ingested_zero_copy (accepted) = %" PRIu64 "\n", stats.net.frames_ingested_zero_copy);
    printf("  rejected_total (bad tenant)          = %" PRIu64 "\n", stats.net.rejected_total);
    printf("  rejected_ring_full (backpressure)    = %" PRIu64 "\n", stats.net.rejected_ring_full);
    printf("  unaccounted (client/socket aborts)   = %" PRIu64 "\n", unaccounted);

    SECTION("results: chrono_ingest pipeline (queue -> sealed chain)");
    printf("  append_backpressure (queue-full events) = %" PRIu64 "\n", stats.append_backpressure);
    printf("  final append_queue depth                = %zu\n", stats.append_queue_depth);
    printf("  sealed_count                            = %u\n", (unsigned)sealed_final);
    printf("  chain_len                               = %" PRIu64 "\n", ctx.chain_len);
    printf("  chain reached CHRONO_CHAIN_CAPACITY?     = %s\n", chain_full ? "yes" : "no");
    CHECK(sealed_final == (uint32_t)ctx.chain_len);

    SECTION("performance metrics (real, measured)");
    double frames_per_sec = (load_gen_seconds > 0.0) ? ((double)stats.net.frames_ingested_zero_copy / load_gen_seconds) : 0.0;
    printf("  load-generation wall time = %.4f s (8 threads, %u connections total attempted)\n",
           load_gen_seconds, (unsigned)TOTAL_FRAMES);
    printf("  accepted frames / sec     = %.1f frames/sec\n", frames_per_sec);
    printf("  attempted frames / sec    = %.1f frames/sec (client-side, includes rejected attempts)\n",
           (load_gen_seconds > 0.0) ? ((double)TOTAL_FRAMES / load_gen_seconds) : 0.0);

    SECTION("final chain integrity check (chrono_ingest_verify_block_chain)");
    uint64_t broken_index = (uint64_t)-1;
    chrono_ingest_status_t vst = chrono_ingest_verify_block_chain(ctx.chain, (size_t)ctx.chain_len,
                                                                    CHRONO_GENESIS_HASH, &broken_index);
    printf("  chrono_ingest_verify_block_chain() over %" PRIu64 " sealed blocks -> %d (CHRONO_INGEST_OK == %d)\n",
           ctx.chain_len, (int)vst, (int)CHRONO_INGEST_OK);
    CHECK(vst == CHRONO_INGEST_OK);
    CHECK(ctx.chain_len > 0u);

    chrono_ingest_shutdown(&ctx);

    printf("=== %d checks run, %d failed ===\n", g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
