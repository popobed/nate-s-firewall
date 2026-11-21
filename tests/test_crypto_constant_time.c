/*
 * test_crypto_constant_time.c - Unit + statistical-timing test bench for
 * crypto_constant_time.{h,c}.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching the module's own zero-allocation constraint.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "crypto_constant_time.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
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
/* ct_equal                                                           */
/* ---------------------------------------------------------------- */

static void test_ct_equal_basic(void) {
    SECTION("ct_equal: basic equal/unequal cases");

    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(ct_equal(a, b, sizeof a) == 1);

    b[7] = 9; /* mismatch at the last byte */
    CHECK(ct_equal(a, b, sizeof a) == 0);

    b[7] = 8;
    b[0] = 0; /* mismatch at the first byte */
    CHECK(ct_equal(a, b, sizeof a) == 0);

    CHECK(ct_equal(a, b, 0) == 1); /* zero length: always equal, no deref */
    CHECK(ct_equal(NULL, NULL, 0) == 1);

    uint8_t same[64];
    memset(same, 0xAB, sizeof same);
    uint8_t same2[64];
    memset(same2, 0xAB, sizeof same2);
    CHECK(ct_equal(same, same2, sizeof same) == 1);
    same2[63] ^= 0x01u;
    CHECK(ct_equal(same, same2, sizeof same) == 0);
}

static void test_ct_equal_u_variants(void) {
    SECTION("ct_equal_u16/u32/u64");

    CHECK(ct_equal_u16(0x1234u, 0x1234u) == 1);
    CHECK(ct_equal_u16(0x1234u, 0x1235u) == 0);
    CHECK(ct_equal_u16(0u, 0u) == 1);
    CHECK(ct_equal_u16(0xFFFFu, 0x0000u) == 0);

    CHECK(ct_equal_u32(0xDEADBEEFu, 0xDEADBEEFu) == 1);
    CHECK(ct_equal_u32(0xDEADBEEFu, 0xDEADBEEEu) == 0);
    CHECK(ct_equal_u32(0u, 0xFFFFFFFFu) == 0);

    CHECK(ct_equal_u64(0x0123456789ABCDEFULL, 0x0123456789ABCDEFULL) == 1);
    CHECK(ct_equal_u64(0x0123456789ABCDEFULL, 0x0123456789ABCDEEULL) == 0);
    CHECK(ct_equal_u64(0ULL, 0ULL) == 1);
}

/* ---------------------------------------------------------------- */
/* ct_verify_token                                                    */
/* ---------------------------------------------------------------- */

static void test_ct_verify_token(void) {
    SECTION("ct_verify_token");

    const uint8_t expected[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20
    };

    uint8_t provided[32];
    memcpy(provided, expected, sizeof expected);
    CHECK(ct_verify_token(provided, sizeof provided, expected, sizeof expected) == 1);

    provided[0] ^= 0xFFu; /* wrong content, first byte */
    CHECK(ct_verify_token(provided, sizeof provided, expected, sizeof expected) == 0);

    memcpy(provided, expected, sizeof expected);
    provided[31] ^= 0xFFu; /* wrong content, last byte */
    CHECK(ct_verify_token(provided, sizeof provided, expected, sizeof expected) == 0);

    CHECK(ct_verify_token(provided, 16, expected, 32) == 0); /* length mismatch */
    CHECK(ct_verify_token(NULL, 32, expected, 32) == 0);
    CHECK(ct_verify_token(provided, 32, NULL, 32) == 0);

    uint8_t oversized[CRYPTO_CT_MAX_LEN + 1];
    memset(oversized, 0, sizeof oversized);
    CHECK(ct_verify_token(oversized, sizeof oversized, oversized, sizeof oversized) == 0);

    /* Boundary: exactly CRYPTO_CT_MAX_LEN is accepted. */
    uint8_t maxbuf_a[CRYPTO_CT_MAX_LEN];
    uint8_t maxbuf_b[CRYPTO_CT_MAX_LEN];
    memset(maxbuf_a, 0x7Au, sizeof maxbuf_a);
    memset(maxbuf_b, 0x7Au, sizeof maxbuf_b);
    CHECK(ct_verify_token(maxbuf_a, sizeof maxbuf_a, maxbuf_b, sizeof maxbuf_b) == 1);
}

/* ---------------------------------------------------------------- */
/* Statistical timing sanity check (diagnostic, not a hard gate --    */
/* wall-clock timing on a shared/virtualized CI host is inherently    */
/* noisy; this reports the spread rather than asserting a threshold   */
/* that would make the suite flaky for reasons unrelated to           */
/* correctness).                                                      */
/* ---------------------------------------------------------------- */

static double now_seconds(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    LARGE_INTEGER counter;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

#define TIMING_ITERATIONS 200000
#define TIMING_KEY_LEN    32

static double time_scenario(const uint8_t *a, const uint8_t *b, size_t len) {
    volatile int sink = 0;
    double t0 = now_seconds();
    for (int i = 0; i < TIMING_ITERATIONS; i++) {
        sink ^= ct_equal(a, b, len);
    }
    double t1 = now_seconds();
    (void)sink;
    return (t1 - t0) / (double)TIMING_ITERATIONS * 1e9; /* ns/call */
}

static void test_timing_diagnostic(void) {
    SECTION("ct_equal: timing spread across mismatch position (diagnostic)");

    uint8_t a[TIMING_KEY_LEN];
    uint8_t b_equal[TIMING_KEY_LEN];
    uint8_t b_diff_first[TIMING_KEY_LEN];
    uint8_t b_diff_last[TIMING_KEY_LEN];

    memset(a, 0x42, sizeof a);
    memcpy(b_equal, a, sizeof a);
    memcpy(b_diff_first, a, sizeof a);
    b_diff_first[0] ^= 0xFFu;
    memcpy(b_diff_last, a, sizeof a);
    b_diff_last[TIMING_KEY_LEN - 1] ^= 0xFFu;

    double ns_equal = time_scenario(a, b_equal, TIMING_KEY_LEN);
    double ns_first  = time_scenario(a, b_diff_first, TIMING_KEY_LEN);
    double ns_last   = time_scenario(a, b_diff_last, TIMING_KEY_LEN);

    printf("   ns/call (%d-byte compare, %d iterations): equal=%.2f diff_first_byte=%.2f diff_last_byte=%.2f\n",
           TIMING_KEY_LEN, TIMING_ITERATIONS, ns_equal, ns_first, ns_last);
    printf("   (informational: a byte-at-a-time early-exit comparator would show diff_first_byte\n"
           "    measurably faster than diff_last_byte/equal here; ct_equal's cost is dominated by\n"
           "    the fixed-length loop regardless of where -- or whether -- the buffers differ)\n");

    /* Correctness, not timing, is the hard assertion. */
    CHECK(ct_equal(a, b_equal, TIMING_KEY_LEN) == 1);
    CHECK(ct_equal(a, b_diff_first, TIMING_KEY_LEN) == 0);
    CHECK(ct_equal(a, b_diff_last, TIMING_KEY_LEN) == 0);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_crypto_constant_time ===\n");

    test_ct_equal_basic();
    test_ct_equal_u_variants();
    test_ct_verify_token();
    test_timing_diagnostic();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
