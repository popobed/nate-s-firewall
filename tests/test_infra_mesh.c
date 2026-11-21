/*
 * test_infra_mesh.c - Unit + true multi-thread stress test bench for
 * infra_mesh.{h,c}: node registry (O(1) resolver), authenticated inter-
 * node frame dispatch, the SoA load balancer, and the local immutable
 * block store.
 *
 * Windows-only (see infra_mesh.h/Makefile): the block store uses the
 * Windows CRT low-level fd calls directly, matching net_io.c's own
 * Windows-only posture in this project.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching infra_mesh's own zero-allocation constraint.
 */

#include <errno.h>
#include <inttypes.h>
#include <io.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "infra_mesh.h"

/* ---------------------------------------------------------------- */
/* Minimal test harness (same shape as this project's other test files) */
/* ---------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_tests_run++;                                                     \
        if (!(cond)) {                                                     \
            g_tests_failed++;                                              \
            fprintf(stderr, "  [FAIL] %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                   \
    } while (0)

#define SECTION(name) printf("-- %s\n", (name))

static const uint8_t SECRET_A[MESH_SECRET_LEN] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10
};
static const uint8_t SECRET_B[MESH_SECRET_LEN] = {
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,0x20
};

/* ---------------------------------------------------------------- */
/* Part A: registry                                                   */
/* ---------------------------------------------------------------- */

static void test_registry_register_resolve(void) {
    SECTION("mesh_register_node + mesh_resolve: O(1) direct-indexed round trip");
    mesh_registry_t reg;
    mesh_registry_init(&reg);

    CHECK(mesh_resolve(&reg, 0) == NULL); /* nothing registered yet */
    CHECK(mesh_register_node(&reg, 0, 0x0A000001u, SECRET_A, 0u) == MESH_OK);

    const mesh_node_t *n = mesh_resolve(&reg, 0);
    CHECK(n != NULL);
    if (n != NULL) {
        CHECK(n->internal_ip == 0x0A000001u);
        CHECK(n->actor_id == 0u);
        CHECK(memcmp(n->secret, SECRET_A, MESH_SECRET_LEN) == 0);
    }
}

static void test_registry_rejects_duplicate_and_out_of_range(void) {
    SECTION("mesh_register_node: rejects a duplicate node_id and an out-of-range one");
    mesh_registry_t reg;
    mesh_registry_init(&reg);
    CHECK(mesh_register_node(&reg, 1, 0, SECRET_A, 0u) == MESH_OK);
    CHECK(mesh_register_node(&reg, 1, 0, SECRET_B, 1u) == MESH_ERR_DUPLICATE_NODE);
    CHECK(mesh_register_node(&reg, (uint8_t)MESH_MAX_NODES, 0, SECRET_A, 0u) == MESH_ERR_OUT_OF_RANGE);
    CHECK(mesh_resolve(&reg, (uint8_t)MESH_MAX_NODES) == NULL);
}

/* ---------------------------------------------------------------- */
/* Part B: authenticated frame dispatch                               */
/* ---------------------------------------------------------------- */

static void test_dispatch_happy_path(void) {
    SECTION("mesh_build_frame -> mesh_dispatch: authenticated frame delivers into the destination actor's mailbox");

    static actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    CHECK(actor_system_spawn(&sys, 1u) == ACTOR_OK);

    mesh_registry_t reg;
    mesh_registry_init(&reg);
    CHECK(mesh_register_node(&reg, 0, 0x0A000001u, SECRET_A, 0u) == MESH_OK);
    CHECK(mesh_register_node(&reg, 1, 0x0A000002u, SECRET_B, 1u) == MESH_OK);

    const uint8_t payload[] = "ping";
    mesh_frame_t frame;
    CHECK(mesh_build_frame(&frame, 0u, 1u, SECRET_A, payload, (uint16_t)sizeof payload) == MESH_OK);
    CHECK(frame.magic == MESH_MAGIC);

    actor_t *dst = actor_system_get(&sys, 1u);
    CHECK(actor_mailbox_count(dst) == 0u);
    CHECK(mesh_dispatch(&sys, &reg, &frame) == MESH_OK);
    CHECK(actor_mailbox_count(dst) == 1u);

    ring_msg_t out;
    CHECK(actor_process_one(dst, &out) == ACTOR_OK);
    CHECK(out.sender_id == 0u); /* src_node_id */
    CHECK(out.type == MESH_MSG_TYPE_FRAME);
    CHECK(memcmp(out.payload, payload, sizeof payload) == 0);
}

static void test_dispatch_rejects_bad_magic(void) {
    SECTION("mesh_dispatch: bad magic rejected before any node is resolved");
    static actor_system_t sys;
    actor_system_init(&sys);
    mesh_registry_t reg;
    mesh_registry_init(&reg);

    mesh_frame_t frame;
    CHECK(mesh_build_frame(&frame, 0u, 1u, SECRET_A, NULL, 0u) == MESH_OK);
    frame.magic ^= 0xFFFFFFFFu;
    CHECK(mesh_dispatch(&sys, &reg, &frame) == MESH_ERR_BAD_MAGIC);
}

static void test_dispatch_rejects_corrupted_integrity(void) {
    SECTION("mesh_dispatch: a corrupted payload byte fails the FNV-1a integrity check");
    static actor_system_t sys;
    actor_system_init(&sys);
    mesh_registry_t reg;
    mesh_registry_init(&reg);
    CHECK(mesh_register_node(&reg, 0, 0, SECRET_A, 0u) == MESH_OK);

    const uint8_t payload[] = "integrity";
    mesh_frame_t frame;
    CHECK(mesh_build_frame(&frame, 0u, 0u, SECRET_A, payload, (uint16_t)sizeof payload) == MESH_OK);
    frame.payload[0] ^= 0x01u;
    CHECK(mesh_dispatch(&sys, &reg, &frame) == MESH_ERR_INTEGRITY);
}

static void test_dispatch_rejects_forged_secret(void) {
    SECTION("mesh_dispatch: THE authentication guarantee -- a frame claiming node A's identity "
            "but signed with node B's secret is rejected (constant-time, ct_verify_token)");
    static actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);

    mesh_registry_t reg;
    mesh_registry_init(&reg);
    CHECK(mesh_register_node(&reg, 0, 0, SECRET_A, 0u) == MESH_OK); /* node 0's REAL secret is A */

    mesh_frame_t forged;
    /* Claims src_node_id == 0, but signs with node B's secret. */
    CHECK(mesh_build_frame(&forged, 0u, 0u, SECRET_B, NULL, 0u) == MESH_OK);
    CHECK(mesh_dispatch(&sys, &reg, &forged) == MESH_ERR_AUTH_FAILED);
    CHECK(actor_mailbox_count(actor_system_get(&sys, 0u)) == 0u); /* never delivered */
}

static void test_dispatch_rejects_unknown_nodes(void) {
    SECTION("mesh_dispatch: unknown src/dst node_id rejected in O(1), before any actor is touched");
    static actor_system_t sys;
    actor_system_init(&sys);
    mesh_registry_t reg;
    mesh_registry_init(&reg);
    CHECK(mesh_register_node(&reg, 0, 0, SECRET_A, 0u) == MESH_OK);

    mesh_frame_t frame;
    CHECK(mesh_build_frame(&frame, 5u /* never registered */, 0u, SECRET_A, NULL, 0u) == MESH_OK);
    CHECK(mesh_dispatch(&sys, &reg, &frame) == MESH_ERR_UNKNOWN_NODE);

    CHECK(mesh_build_frame(&frame, 0u, 5u /* never registered */, SECRET_A, NULL, 0u) == MESH_OK);
    CHECK(mesh_dispatch(&sys, &reg, &frame) == MESH_ERR_UNKNOWN_NODE);
}

static void test_build_frame_rejects_oversized_payload(void) {
    SECTION("mesh_build_frame: rejects payload_len > MESH_PAYLOAD_LEN");
    uint8_t big[MESH_PAYLOAD_LEN + 1u];
    memset(big, 0x41, sizeof big);
    mesh_frame_t frame;
    CHECK(mesh_build_frame(&frame, 0u, 1u, SECRET_A, big, (uint16_t)sizeof big) == MESH_ERR_PAYLOAD_TOO_LARGE);
}

/* ---------------------------------------------------------------- */
/* Part C: SoA load balancer                                          */
/* ---------------------------------------------------------------- */

static void test_lb_least_loaded_and_backpressure(void) {
    SECTION("lb_pool_dispatch: picks the least-loaded worker under quota, deterministic backpressure at quota");
    static actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    CHECK(actor_system_spawn(&sys, 1u) == ACTOR_OK);

    lb_pool_t lb;
    lb_pool_init(&lb);
    CHECK(lb_pool_add_worker(&lb, actor_system_get(&sys, 0u), 2u) == MESH_OK); /* soft quota: 2 */
    CHECK(lb_pool_add_worker(&lb, actor_system_get(&sys, 1u), 2u) == MESH_OK);

    const uint8_t p[] = "x";
    uint8_t chosen;

    /* First dispatch: both empty (load 0) -- picks worker 0 (first seen at the minimum). */
    CHECK(lb_pool_dispatch(&lb, 1u, 1u, p, sizeof p, &chosen) == MESH_OK);
    CHECK(chosen == 0u);

    /* Second dispatch: worker 0 now has load 1, worker 1 has load 0 -- picks worker 1. */
    CHECK(lb_pool_dispatch(&lb, 1u, 1u, p, sizeof p, &chosen) == MESH_OK);
    CHECK(chosen == 1u);

    /* Third: both at load 1 -- ties go to the first (worker 0), now reaching its quota of 2. */
    CHECK(lb_pool_dispatch(&lb, 1u, 1u, p, sizeof p, &chosen) == MESH_OK);
    CHECK(chosen == 0u);

    /* Fourth: worker 0 at quota (2/2) -- routes to worker 1, also reaching quota (2/2). */
    CHECK(lb_pool_dispatch(&lb, 1u, 1u, p, sizeof p, &chosen) == MESH_OK);
    CHECK(chosen == 1u);

    /* Both workers now at their soft quota of 2 -- deterministic backpressure,
     * even though each mailbox's HARD ring capacity (64) is nowhere near full. */
    CHECK(lb_pool_dispatch(&lb, 1u, 1u, p, sizeof p, &chosen) == MESH_ERR_BACKPRESSURE);
}

static void test_lb_rejects_oversized_payload(void) {
    SECTION("lb_pool_dispatch: rejects a payload larger than one mailbox slot can hold");
    static actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    lb_pool_t lb;
    lb_pool_init(&lb);
    CHECK(lb_pool_add_worker(&lb, actor_system_get(&sys, 0u), 10u) == MESH_OK);

    uint8_t big[RING_MSG_PAYLOAD_SIZE + 1u];
    memset(big, 0x42, sizeof big);
    CHECK(lb_pool_dispatch(&lb, 1u, 1u, big, sizeof big, NULL) == MESH_ERR_PAYLOAD_TOO_LARGE);
}

/* ---------------------------------------------------------------- */
/* Part D: local immutable block store                                */
/* ---------------------------------------------------------------- */

static const char *STORE_DIR = "build/test_infra_mesh_store";

static void test_store_put_get_roundtrip(void) {
    SECTION("mesh_store_put/_get: exact round trip, FNV-1a verified on read");
    CreateDirectoryA(STORE_DIR, NULL); /* ignore "already exists" -- idempotent setup */
    _unlink("build/test_infra_mesh_store/00000001.blk");

    wire_msg_t block;
    memset(&block, 0, sizeof block);
    CHECK(wire_build_msg(&block, 42u, 7u, (const uint8_t *)"stored-config", 14u) == WIRE_OK);

    CHECK(mesh_store_put(STORE_DIR, 1u, &block) == MESH_OK);

    wire_msg_t out;
    CHECK(mesh_store_get(STORE_DIR, 1u, &out) == MESH_OK);
    CHECK(memcmp(&out, &block, sizeof block) == 0);

    _unlink("build/test_infra_mesh_store/00000001.blk");
}

static void test_store_immutable_rejects_overwrite(void) {
    SECTION("mesh_store_put: an existing object_id is NEVER overwritten (OS-enforced _O_EXCL)");
    CreateDirectoryA(STORE_DIR, NULL);
    _unlink("build/test_infra_mesh_store/00000002.blk");

    wire_msg_t block_v1, block_v2;
    memset(&block_v1, 0, sizeof block_v1);
    memset(&block_v2, 0, sizeof block_v2);
    CHECK(wire_build_msg(&block_v1, 1u, 1u, (const uint8_t *)"v1", 3u) == WIRE_OK);
    CHECK(wire_build_msg(&block_v2, 2u, 2u, (const uint8_t *)"v2", 3u) == WIRE_OK);

    CHECK(mesh_store_put(STORE_DIR, 2u, &block_v1) == MESH_OK);
    CHECK(mesh_store_put(STORE_DIR, 2u, &block_v2) == MESH_ERR_STORE_EXISTS);

    /* The original v1 content must be completely untouched by the
     * rejected v2 attempt. */
    wire_msg_t out;
    CHECK(mesh_store_get(STORE_DIR, 2u, &out) == MESH_OK);
    CHECK(memcmp(&out, &block_v1, sizeof block_v1) == 0);

    _unlink("build/test_infra_mesh_store/00000002.blk");
}

static void test_store_detects_on_disk_corruption(void) {
    SECTION("mesh_store_get: a bit-flipped byte on disk is detected as MESH_ERR_STORE_CORRUPT, never silently returned");
    CreateDirectoryA(STORE_DIR, NULL);
    const char *path = "build/test_infra_mesh_store/00000003.blk";
    _unlink(path);

    wire_msg_t block;
    memset(&block, 0, sizeof block);
    CHECK(wire_build_msg(&block, 9u, 9u, (const uint8_t *)"corrupt-me", 11u) == WIRE_OK);
    mesh_status_t put_st = mesh_store_put(STORE_DIR, 3u, &block);
    if (put_st != MESH_OK) {
        fprintf(stderr, "  DIAG: mesh_store_put returned %d, errno=%d (%s)\n", (int)put_st, errno, strerror(errno));
    }
    CHECK(put_st == MESH_OK);

    /* Flip one byte directly on disk, bypassing this module entirely --
     * simulates real bit rot / a corrupted sector. */
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f != NULL) {
        int c = fgetc(f);
        CHECK(c != EOF);
        fseek(f, 0, SEEK_SET);
        fputc(c ^ 0xFF, f);
        fclose(f);
    }

    wire_msg_t out;
    CHECK(mesh_store_get(STORE_DIR, 3u, &out) == MESH_ERR_STORE_CORRUPT);

    _unlink(path);
}

static void test_store_not_found(void) {
    SECTION("mesh_store_get: a never-stored object_id returns MESH_ERR_STORE_NOT_FOUND");
    CreateDirectoryA(STORE_DIR, NULL);
    wire_msg_t out;
    CHECK(mesh_store_get(STORE_DIR, 999999u, &out) == MESH_ERR_STORE_NOT_FOUND);
}

/* ---------------------------------------------------------------- */
/* Concurrent stress: multiple "server nodes", dynamic load balancing, */
/* corrupted-packet rejection, under real multi-thread contention.     */
/* ---------------------------------------------------------------- */

#define STRESS_NODES          (ACTOR_MAX_COUNT - 1u) /* leave node 0 as a dedicated hub */
#define STRESS_ITERS_PER_NODE 20000u

static actor_system_t g_stress_sys;
static mesh_registry_t g_stress_reg;
static _Atomic uint64_t g_delivered;
static _Atomic uint64_t g_auth_rejected;
static _Atomic uint64_t g_integrity_rejected;
static _Atomic uint64_t g_unexpected;
static _Atomic uint64_t g_backpressure;
static _Atomic uint64_t g_drained;
static _Atomic uint32_t g_producers_remaining;

static const uint8_t STRESS_SECRETS[STRESS_NODES][MESH_SECRET_LEN] = {
    { 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 },
    { 2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2 },
    { 3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3 },
    { 4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4 },
    { 5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5 },
    { 6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6 },
    { 7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7 },
};

typedef struct {
    uint8_t node_id; /* this thread simulates traffic FROM this node, to the hub (node 0) */
} stress_ctx_t;

static DWORD WINAPI stress_worker(LPVOID arg) {
    stress_ctx_t *ctx = (stress_ctx_t *)arg;
    const uint8_t *real_secret = STRESS_SECRETS[ctx->node_id - 1u];

    for (uint32_t i = 0; i < STRESS_ITERS_PER_NODE; i++) {
        mesh_frame_t frame;
        const uint8_t payload[4] = { (uint8_t)i, (uint8_t)(i >> 8), (uint8_t)(i >> 16), (uint8_t)(i >> 24) };

        switch (i % 3u) {
            case 0: { /* legitimate traffic */
                CHECK(mesh_build_frame(&frame, ctx->node_id, 0u, real_secret, payload, sizeof payload) == MESH_OK);
                mesh_status_t st = mesh_dispatch(&g_stress_sys, &g_stress_reg, &frame);
                if (st == MESH_OK) {
                    atomic_fetch_add_explicit(&g_delivered, 1, memory_order_relaxed);
                } else if (st == MESH_ERR_BACKPRESSURE) {
                    /* A correct, expected outcome, not an anomaly: 7
                     * producer threads racing a single 64-slot mailbox
                     * against one consumer can legitimately outpace it --
                     * see this test's own comment above stress_consumer().
                     * The security-relevant checks (auth/integrity below)
                     * happen BEFORE the mailbox is ever touched, so THEY
                     * are never affected by this and stay held to an
                     * exact count. */
                    atomic_fetch_add_explicit(&g_backpressure, 1, memory_order_relaxed);
                } else {
                    atomic_fetch_add_explicit(&g_unexpected, 1, memory_order_relaxed);
                }
                break;
            }
            case 1: { /* forged: claims this node's identity, signs with a DIFFERENT node's secret */
                const uint8_t *wrong_secret = STRESS_SECRETS[(ctx->node_id) % STRESS_NODES];
                CHECK(mesh_build_frame(&frame, ctx->node_id, 0u, wrong_secret, payload, sizeof payload) == MESH_OK);
                mesh_status_t st = mesh_dispatch(&g_stress_sys, &g_stress_reg, &frame);
                if (st == MESH_ERR_AUTH_FAILED) {
                    atomic_fetch_add_explicit(&g_auth_rejected, 1, memory_order_relaxed);
                } else {
                    atomic_fetch_add_explicit(&g_unexpected, 1, memory_order_relaxed);
                }
                break;
            }
            default: { /* corrupted: legitimate secret, but a flipped payload byte */
                CHECK(mesh_build_frame(&frame, ctx->node_id, 0u, real_secret, payload, sizeof payload) == MESH_OK);
                frame.payload[0] ^= 0xFFu;
                mesh_status_t st = mesh_dispatch(&g_stress_sys, &g_stress_reg, &frame);
                if (st == MESH_ERR_INTEGRITY) {
                    atomic_fetch_add_explicit(&g_integrity_rejected, 1, memory_order_relaxed);
                } else {
                    atomic_fetch_add_explicit(&g_unexpected, 1, memory_order_relaxed);
                }
                break;
            }
        }
    }
    atomic_fetch_sub_explicit(&g_producers_remaining, 1, memory_order_release);
    return 0;
}

/* The hub's mailbox (RING_BUFFER_CAPACITY == 64) is far smaller than the
 * ~140000 legitimate-traffic attempts this stress test drives at it --
 * without a concurrent consumer, it fills almost instantly and every
 * later legitimate send would see MESH_ERR_BACKPRESSURE instead of
 * delivery, which is a correct outcome for an undrained mailbox but not
 * what this test exists to prove. A dedicated consumer thread, draining
 * concurrently exactly as actor_core.h's own "single consumer thread"
 * contract requires, is what makes "the mailbox stays realistically
 * drained under load" true here instead of assumed. */
static DWORD WINAPI stress_consumer(LPVOID arg) {
    (void)arg;
    actor_t *hub = actor_system_get(&g_stress_sys, 0u);
    ring_msg_t msg;
    for (;;) {
        if (actor_process_one(hub, &msg) == ACTOR_OK) {
            atomic_fetch_add_explicit(&g_drained, 1, memory_order_relaxed);
            continue;
        }
        /* Empty right now. Only stop once every producer has finished --
         * by the time g_producers_remaining reads 0 here (acquire), every
         * message any producer thread published is already visible
         * (release-acquire through this same atomic, C11 5.1.2.4). */
        if (atomic_load_explicit(&g_producers_remaining, memory_order_acquire) == 0u) {
            break;
        }
        SwitchToThread();
    }
    return 0;
}

static void test_concurrent_multi_node_stress(void) {
    SECTION("CONCURRENT STRESS: multiple server-node actors, real multi-thread traffic, forged "
            "identities and corrupted packets rejected deterministically, 0 unexpected outcomes");

    actor_system_init(&g_stress_sys);
    for (uint8_t a = 0; a < ACTOR_MAX_COUNT; a++) {
        CHECK(actor_system_spawn(&g_stress_sys, a) == ACTOR_OK);
    }
    mesh_registry_init(&g_stress_reg);
    CHECK(mesh_register_node(&g_stress_reg, 0u, 0x0A000000u, STRESS_SECRETS[0], 0u) == MESH_OK); /* hub itself, secret unused as sender here */
    for (uint8_t n = 1; n <= STRESS_NODES; n++) {
        CHECK(mesh_register_node(&g_stress_reg, n, 0x0A000000u + n, STRESS_SECRETS[n - 1u], n) == MESH_OK);
    }

    atomic_store_explicit(&g_delivered, 0, memory_order_relaxed);
    atomic_store_explicit(&g_auth_rejected, 0, memory_order_relaxed);
    atomic_store_explicit(&g_integrity_rejected, 0, memory_order_relaxed);
    atomic_store_explicit(&g_unexpected, 0, memory_order_relaxed);
    atomic_store_explicit(&g_backpressure, 0, memory_order_relaxed);
    atomic_store_explicit(&g_drained, 0, memory_order_relaxed);
    atomic_store_explicit(&g_producers_remaining, STRESS_NODES, memory_order_relaxed);

    /* Consumer FIRST, so the hub's mailbox is already being drained the
     * moment producers start posting to it. */
    HANDLE consumer_handle = CreateThread(NULL, 0, stress_consumer, NULL, 0, NULL);
    CHECK(consumer_handle != NULL);

    static stress_ctx_t ctxs[STRESS_NODES];
    HANDLE handles[STRESS_NODES];
    for (uint8_t n = 0; n < STRESS_NODES; n++) {
        ctxs[n].node_id = (uint8_t)(n + 1u);
        handles[n] = CreateThread(NULL, 0, stress_worker, &ctxs[n], 0, NULL);
        CHECK(handles[n] != NULL);
    }
    WaitForMultipleObjects(STRESS_NODES, handles, TRUE, INFINITE);
    for (uint8_t n = 0; n < STRESS_NODES; n++) {
        CloseHandle(handles[n]);
    }

    WaitForSingleObject(consumer_handle, INFINITE);
    CloseHandle(consumer_handle);

    uint64_t drained      = atomic_load_explicit(&g_drained, memory_order_relaxed);
    uint64_t delivered    = atomic_load_explicit(&g_delivered, memory_order_relaxed);
    uint64_t backpressure = atomic_load_explicit(&g_backpressure, memory_order_relaxed);
    uint64_t auth_rej     = atomic_load_explicit(&g_auth_rejected, memory_order_relaxed);
    uint64_t integ_rej    = atomic_load_explicit(&g_integrity_rejected, memory_order_relaxed);
    uint64_t unexpected   = atomic_load_explicit(&g_unexpected, memory_order_relaxed);

    /* The two SECURITY guarantees (a forged identity, a corrupted payload)
     * are checked before the mailbox is ever touched, so they must hold
     * EXACTLY, unaffected by load: */
    CHECK(unexpected == 0u);
    CHECK(auth_rej > 0u);
    CHECK(integ_rej > 0u);
    /* Legitimate traffic's split between "delivered" and "backpressure"
     * is throughput-dependent (7 producers vs. 1 consumer against a
     * 64-slot mailbox) -- both are correct outcomes; what must hold is
     * that every legitimate attempt landed in exactly one of the two,
     * and that delivery is real (matches what the consumer actually
     * drained, no lost/duplicated/phantom message). */
    CHECK(drained == delivered);
    CHECK(delivered > 0u);
    CHECK(backpressure < (uint64_t)STRESS_NODES * STRESS_ITERS_PER_NODE); /* sanity: not literally everything backpressured */
    CHECK(delivered + backpressure + auth_rej + integ_rej == (uint64_t)STRESS_NODES * STRESS_ITERS_PER_NODE);

    printf("   nodes=%u iters_per_node=%u delivered=%" PRIu64 " backpressure=%" PRIu64
           " auth_rejected=%" PRIu64 " integrity_rejected=%" PRIu64 " unexpected=%" PRIu64 "\n",
           STRESS_NODES, STRESS_ITERS_PER_NODE, delivered, backpressure, auth_rej, integ_rej, unexpected);
}

/* ---------------------------------------------------------------- */
/* Part E: spatial perception -- mesh nodes <-> signal_tracking.h      */
/* ---------------------------------------------------------------- */

static void test_node_position_report_and_predict(void) {
    SECTION("mesh_node_report_position/predict_position: registry-gated reuse of signal_tracking.h, O(1)");
    mesh_registry_t reg;
    mesh_registry_init(&reg);
    track_registry_t tracks;
    track_registry_init(&tracks);

    /* an unregistered node has no spatial perception yet */
    CHECK(mesh_node_report_position(&reg, &tracks, 2, fixed_from_int(1), 0, 0, 0u) == MESH_ERR_UNKNOWN_NODE);
    fixed_q21_10_t x, y, z;
    CHECK(mesh_node_predict_position(&reg, &tracks, 2, 0u, &x, &y, &z) == MESH_ERR_UNKNOWN_NODE);

    CHECK(mesh_register_node(&reg, 2, 0x0A000002u, SECRET_A, 0u) == MESH_OK);

    CHECK(mesh_node_report_position(&reg, &tracks, 2, fixed_from_int(10), fixed_from_int(20), fixed_from_int(30), 0u) == MESH_OK);
    CHECK(mesh_node_report_position(&reg, &tracks, 2, fixed_from_int(20), fixed_from_int(20), fixed_from_int(30), 10u) == MESH_OK);

    /* the underlying track_registry_t was genuinely updated under
     * target_id == node_id, not a separate/shadow storage */
    CHECK(tracks.active[2] == true);
    CHECK(tracks.vel_x[2] == fixed_from_int(1)); /* moved +10 over 10 ticks */

    CHECK(mesh_node_predict_position(&reg, &tracks, 2, 30u, &x, &y, &z) == MESH_OK);
    CHECK(x == fixed_from_int(40)); /* 20 + 1.0*20 more ticks */
    CHECK(y == fixed_from_int(20) && z == fixed_from_int(30));

    /* backward ticks are rejected the same way signal_tracking.h itself rejects them */
    CHECK(mesh_node_report_position(&reg, &tracks, 2, fixed_from_int(0), 0, 0, 5u) == MESH_ERR_TRACKING_FAILED);

    CHECK(mesh_node_report_position(NULL, &tracks, 2, 0, 0, 0, 0u) == MESH_ERR_INVALID);
    CHECK(mesh_node_predict_position(&reg, NULL, 2, 0u, &x, &y, &z) == MESH_ERR_INVALID);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_infra_mesh ===\n");

    test_registry_register_resolve();
    test_registry_rejects_duplicate_and_out_of_range();

    test_dispatch_happy_path();
    test_dispatch_rejects_bad_magic();
    test_dispatch_rejects_corrupted_integrity();
    test_dispatch_rejects_forged_secret();
    test_dispatch_rejects_unknown_nodes();
    test_build_frame_rejects_oversized_payload();

    test_lb_least_loaded_and_backpressure();
    test_lb_rejects_oversized_payload();

    test_store_put_get_roundtrip();
    test_store_immutable_rejects_overwrite();
    test_store_detects_on_disk_corruption();
    test_store_not_found();

    test_concurrent_multi_node_stress();

    test_node_position_report_and_predict();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
