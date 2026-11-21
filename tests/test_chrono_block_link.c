/*
 * test_chrono_block_link.c - Validates the SHA-256 engine (sha256.h)
 * against NIST's own known-answer test vectors, then exercises
 * chrono_ingest.c's block-linking layer (chrono_ingest_seal_next() /
 * chrono_ingest_verify_chain()) end-to-end: real network ingestion into
 * real sealed, hash-chained blocks, a clean verify, a single-bit
 * corruption injection, a failed verify pinpointing the tampered block,
 * then a restore proving the check is precise (not just "always fails").
 *
 * Windows-only (chrono_ingest.h #errors on any other platform), same
 * posture as test_chrono_ingest.c. No dynamic allocation anywhere in
 * this file.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "chrono_ingest.h"
#include "sha256.h"
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

/* ---------------------------------------------------------------- */
/* Part 1: SHA-256 known-answer tests (NIST/FIPS 180-4)               */
/* ---------------------------------------------------------------- */

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", buf[i]);
    }
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) {
            return false;
        }
        out[i] = (uint8_t)byte;
    }
    return true;
}

static void check_kat(const char *label, const void *msg, size_t msg_len, const char *expected_hex) {
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE];

    sha256_hash(msg, msg_len, digest);
    CHECK(hex_to_bytes(expected_hex, expected, SHA256_DIGEST_SIZE));
    CHECK(memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0);

    printf("  %-28s = ", label);
    print_hex(digest, SHA256_DIGEST_SIZE);
    printf("\n");
}

static void test_sha256_kats(void) {
    SECTION("SHA-256 known-answer tests (NIST CAVP / FIPS 180-4)");
    check_kat("SHA256(\"\")", "", 0,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_kat("SHA256(\"abc\")", "abc", 3,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        check_kat("SHA256(56-char 2-block msg)", msg, strlen(msg),
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }
}

/* ---------------------------------------------------------------- */
/* Part 2: real network ingestion feeding the block chain             */
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

int main(void) {
    printf("=== chrono_ingest block-linking bench ===\n");

    test_sha256_kats();

    SECTION("init + ingest 4 real frames across 2 tenants");
    chrono_ingest_ctx_t ctx;
    CHECK(chrono_ingest_init(&ctx, 0u) == CHRONO_INGEST_OK);

    uint32_t stream_a = UINT32_MAX, stream_b = UINT32_MAX;
    CHECK(chrono_ingest_register_stream(&ctx, &stream_a) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_stream(&ctx, &stream_b) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_tenant(42u, stream_a) == CHRONO_INGEST_OK);
    CHECK(chrono_ingest_register_tenant(7u, stream_b) == CHRONO_INGEST_OK);

    uint16_t port = chrono_ingest_get_port(&ctx);
    printf("[server] listening on 127.0.0.1:%u\n", (unsigned)port);

    const char *messages[4] = {
        "block 0 -- audit: user_login ok",
        "block 1 -- audit: config_change applied",
        "block 2 -- audit: user_logout ok",
        "block 3 -- audit: privilege_escalation DETECTED"
    };
    uint16_t tenants[4] = {42u, 7u, 42u, 7u};

    for (int i = 0; i < 4; i++) {
        CHECK(run_client(port, tenants[i], messages[i]));
        CHECK(poll_until(&ctx, 1u) == 1u);
    }

    chrono_ingest_stats_t stats;
    chrono_ingest_get_stats(&ctx, &stats);
    CHECK(stats.frames_appended == 4u);
    CHECK(stats.append_queue_depth == 4u);

    SECTION("seal all 4 entries into the hash chain");
    for (int i = 0; i < 4; i++) {
        chrono_block_t blk;
        chrono_ingest_status_t st = chrono_ingest_seal_next(&ctx, &blk);
        CHECK(st == CHRONO_INGEST_OK);
        printf("  [block %" PRIu64 "] tenant=%u payload_hash=", blk.sequence_id, blk.tenant_id);
        print_hex(blk.payload_hash, SHA256_DIGEST_SIZE);
        printf("\n                block_hash  =");
        print_hex(blk.block_hash, SHA256_DIGEST_SIZE);
        printf("\n");
    }
    CHECK(ctx.chain_len == 4u);

    /* One more seal attempt: append_queue is now empty. */
    chrono_block_t nothing;
    CHECK(chrono_ingest_seal_next(&ctx, &nothing) == CHRONO_INGEST_ERR_EMPTY);

    SECTION("chain structure: genesis link + sequential prev_hash links");
    CHECK(memcmp(ctx.chain[0].prev_hash, CHRONO_GENESIS_HASH, SHA256_DIGEST_SIZE) == 0);
    for (int i = 1; i < 4; i++) {
        CHECK(memcmp(ctx.chain[i].prev_hash, ctx.chain[i - 1].block_hash, SHA256_DIGEST_SIZE) == 0);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(ctx.chain[i].sequence_id == (uint64_t)i);
    }

    SECTION("verify_chain: clean chain must pass");
    uint64_t broken_idx = 0xFFFFFFFFFFFFFFFFULL;
    chrono_ingest_status_t vst = chrono_ingest_verify_chain(&ctx, &broken_idx);
    printf("  verify_chain() -> %d (CHRONO_INGEST_OK == %d)\n", (int)vst, (int)CHRONO_INGEST_OK);
    CHECK(vst == CHRONO_INGEST_OK);

    SECTION("corruption injection: flip 1 bit in block 2's payload_hash");
    uint8_t original_byte = ctx.chain[2].payload_hash[5];
    ctx.chain[2].payload_hash[5] ^= 0x01u; /* flip exactly one bit */
    CHECK(ctx.chain[2].payload_hash[5] != original_byte);

    broken_idx = 0xFFFFFFFFFFFFFFFFULL;
    vst = chrono_ingest_verify_chain(&ctx, &broken_idx);
    printf("  verify_chain() after 1-bit flip -> %d (CHRONO_INGEST_ERR_TAMPERED == %d), first_broken_index=%" PRIu64 "\n",
           (int)vst, (int)CHRONO_INGEST_ERR_TAMPERED, broken_idx);
    CHECK(vst == CHRONO_INGEST_ERR_TAMPERED);
    CHECK(broken_idx == 2u);

    SECTION("restore the flipped bit: verify_chain must pass again");
    ctx.chain[2].payload_hash[5] = original_byte;
    broken_idx = 0xFFFFFFFFFFFFFFFFULL;
    vst = chrono_ingest_verify_chain(&ctx, &broken_idx);
    CHECK(vst == CHRONO_INGEST_OK);

    SECTION("corruption injection #2: flip 1 bit in block 0's prev_hash (the genesis link itself)");
    uint8_t original_genesis_byte = ctx.chain[0].prev_hash[0];
    ctx.chain[0].prev_hash[0] ^= 0x80u;
    broken_idx = 0xFFFFFFFFFFFFFFFFULL;
    vst = chrono_ingest_verify_chain(&ctx, &broken_idx);
    printf("  verify_chain() after genesis-link flip -> %d, first_broken_index=%" PRIu64 "\n", (int)vst, broken_idx);
    CHECK(vst == CHRONO_INGEST_ERR_TAMPERED);
    CHECK(broken_idx == 0u);
    ctx.chain[0].prev_hash[0] = original_genesis_byte; /* restore -- leave the ctx in a known-good state */

    chrono_ingest_shutdown(&ctx);

    printf("=== %d checks run, %d failed ===\n", g_tests_run, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
