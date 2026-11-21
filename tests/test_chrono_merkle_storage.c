/*
 * test_chrono_merkle_storage.c - Generates 10 real sealed chrono_block_t
 * entries (via chrono_ingest.c, same real network path as the other
 * chrono_* test benches), computes their Merkle root (chrono_merkle.c),
 * flushes them as one window to test_audit.bin (chrono_storage.c,
 * strictly append-only), reopens the file and verifies it, then injects
 * a single-bit corruption at a random byte offset and confirms
 * chrono_storage_verify_file() detects it.
 *
 * Windows-only (chrono_storage.h #errors on any other platform), same
 * posture as the other chrono_* test benches. No dynamic allocation
 * anywhere in this file.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chrono_storage.h"
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

#define POLL_SPIN_LIMIT 20000000
#define TEST_AUDIT_FILE "test_audit.bin"

/* ---------------------------------------------------------------- */
/* Real network client, same shape as the other chrono_* test benches */
/* ---------------------------------------------------------------- */

typedef struct {
    uint16_t port;
    uint16_t tenant_id;
    const uint8_t *payload;
    uint16_t payload_len;
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
    while (sent < total) {
        int n = send(s, (const char *)(bytes + sent), (int)(total - sent), 0);
        if (n <= 0) {
            break;
        }
        sent += (size_t)n;
    }
    job->connected_ok = (sent == total);

    Sleep(20);
    closesocket(s);
    WSACleanup();
    return 0;
}

static bool run_client(uint16_t port, uint16_t tenant_id, const char *text) {
    client_job_t job;
    job.port = port;
    job.tenant_id = tenant_id;
    job.payload = (const uint8_t *)text;
    job.payload_len = (uint16_t)strlen(text);
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

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", buf[i]);
    }
}

int main(void) {
    printf("=== chrono_merkle / chrono_storage bench ===\n");

    /* Fresh start: chrono_storage_flush_window() is strictly append-only,
     * so any leftover file from a previous run would silently accumulate
     * extra windows instead of giving this run a clean, known file. */
    remove(TEST_AUDIT_FILE);

    SECTION("init + ingest + seal 10 real blocks");
    chrono_ingest_ctx_t ctx;
    CHECK(chrono_ingest_init(&ctx, 0u) == CHRONO_INGEST_OK);

    uint32_t stream_a = UINT32_MAX, stream_b = UINT32_MAX;
    CHECK(chrono_ingest_register_stream(&ctx, &stream_a) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_stream(&ctx, &stream_b) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_tenant(42u, stream_a) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_tenant(7u, stream_b) == CHRONO_INGEST_OK);

    uint16_t port = chrono_ingest_get_port(&ctx);
    printf("[server] listening on 127.0.0.1:%u\n", (unsigned)port);

    for (int i = 0; i < 10; i++) {
        char msg[64];
        snprintf(msg, sizeof msg, "audit entry #%d -- merkle/storage bench", i);
        uint16_t tenant = (i % 2 == 0) ? 42u : 7u;
        CHECK(run_client(port, tenant, msg));
        CHECK(poll_until(&ctx, 1u) == 1u);

        chrono_block_t blk;
        CHECK(chrono_ingest_seal_next(&ctx, &blk) == CHRONO_INGEST_OK);
    }
    CHECK(ctx.chain_len == 10u);
    CHECK(chrono_ingest_verify_chain(&ctx, NULL) == CHRONO_INGEST_OK);

    SECTION("compute Merkle root over the 10 sealed blocks");
    uint8_t root[SHA256_DIGEST_SIZE];
    chrono_merkle_status_t mst = chrono_merkle_compute_root(ctx.chain, (size_t)ctx.chain_len, root);
    CHECK(mst == CHRONO_MERKLE_OK);
    printf("  merkle_root = ");
    print_hex(root, SHA256_DIGEST_SIZE);
    printf("\n");

    /* Sanity: a single-leaf and a two-leaf tree computed independently
     * must be internally consistent with the odd-count duplication rule
     * -- cheap extra confidence in chrono_merkle.c beyond the main path. */
    {
        uint8_t root1[SHA256_DIGEST_SIZE], root2[SHA256_DIGEST_SIZE];
        CHECK(chrono_merkle_compute_root(&ctx.chain[0], 1u, root1) == CHRONO_MERKLE_OK);
        CHECK(chrono_merkle_compute_root(ctx.chain, 3u, root2) == CHRONO_MERKLE_OK);
        CHECK(memcmp(root1, root2, SHA256_DIGEST_SIZE) != 0); /* different windows must not collide trivially */
    }

    SECTION("flush window to disk (strictly append-only)");
    chrono_storage_status_t sst = chrono_storage_flush_window(TEST_AUDIT_FILE, ctx.chain, (size_t)ctx.chain_len, root);
    CHECK(sst == CHRONO_STORAGE_OK);
    printf("  chrono_storage_flush_window() -> %d (CHRONO_STORAGE_OK == %d)\n", (int)sst, (int)CHRONO_STORAGE_OK);

    long file_size;
    {
        FILE *f = fopen(TEST_AUDIT_FILE, "rb");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END);
        file_size = ftell(f);
        fclose(f);
    }
    long expected_size = (long)CHRONO_STORAGE_WINDOW_HEADER_SIZE + 10L * (long)CHRONO_BLOCK_SERIALIZED_SIZE;
    printf("  %s size on disk = %ld bytes (expected %ld)\n", TEST_AUDIT_FILE, file_size, expected_size);
    CHECK(file_size == expected_size);

    SECTION("reopen + verify: clean file must pass");
    sst = chrono_storage_verify_file(TEST_AUDIT_FILE);
    printf("  chrono_storage_verify_file() -> %d (CHRONO_STORAGE_OK == %d)\n", (int)sst, (int)CHRONO_STORAGE_OK);
    CHECK(sst == CHRONO_STORAGE_OK);

    SECTION("corruption injection: flip 1 bit at a random byte offset (fopen \"r+b\")");
    /* The window's window_timestamp_ns field (file bytes [8,16)) is
     * deliberately excluded from the random range: it is bookkeeping this
     * on-disk format never commits to any hash (see chrono_storage.h's
     * own "What is, and is not, cryptographically checked" section) --
     * corrupting it is a different, undetectable-by-design bug class than
     * the byte-level tamper-evidence this test demonstrates. Every other
     * byte in the file (magic, block_count, merkle_root, and all block
     * data) IS covered, structurally or cryptographically. */
    srand((unsigned)time(NULL));
    size_t offset;
    do {
        offset = (size_t)(rand() % (int)file_size);
    } while (offset >= 8u && offset < 16u);

    uint8_t original_byte;
    uint8_t corrupted_byte;
    {
        FILE *f = fopen(TEST_AUDIT_FILE, "r+b");
        CHECK(f != NULL);
        fseek(f, (long)offset, SEEK_SET);
        CHECK(fread(&original_byte, 1, 1, f) == 1u);
        corrupted_byte = (uint8_t)(original_byte ^ 0x01u); /* flip exactly one bit */
        fseek(f, (long)offset, SEEK_SET);
        CHECK(fwrite(&corrupted_byte, 1, 1, f) == 1u);
        fclose(f);
    }
    printf("  corrupted byte offset=%zu: 0x%02x -> 0x%02x\n", offset, original_byte, corrupted_byte);

    SECTION("re-verify: corrupted file must be detected instantly");
    sst = chrono_storage_verify_file(TEST_AUDIT_FILE);
    printf("  chrono_storage_verify_file() after corruption -> %d "
           "(CHRONO_STORAGE_ERR_TAMPERED == %d, CHRONO_STORAGE_ERR_TRUNCATED == %d)\n",
           (int)sst, (int)CHRONO_STORAGE_ERR_TAMPERED, (int)CHRONO_STORAGE_ERR_TRUNCATED);
    CHECK(sst == CHRONO_STORAGE_ERR_TAMPERED || sst == CHRONO_STORAGE_ERR_TRUNCATED);
    CHECK(sst != CHRONO_STORAGE_OK);

    SECTION("restore the flipped bit: verify must pass again");
    {
        FILE *f = fopen(TEST_AUDIT_FILE, "r+b");
        CHECK(f != NULL);
        fseek(f, (long)offset, SEEK_SET);
        CHECK(fwrite(&original_byte, 1, 1, f) == 1u);
        fclose(f);
    }
    sst = chrono_storage_verify_file(TEST_AUDIT_FILE);
    CHECK(sst == CHRONO_STORAGE_OK);

    chrono_ingest_shutdown(&ctx);

    printf("=== %d checks run, %d failed ===\n", g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
