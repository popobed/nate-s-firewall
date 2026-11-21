/*
 * test_actor_pipeline.c - Unit + real multi-thread stress test bench for
 * actor_pipeline.{h,c}: the additive actor_core.h <-> barry_storage.h
 * (real MPMC task queue) <-> telemetry_fabric.h (hash-chained audit log)
 * bridge. Portable threading (#if _WIN32 CreateThread / else pthread),
 * same pattern as tests/test_telemetry_fabric.c / tests/test_barry_storage.c.
 * No dynamic allocation anywhere in this file.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "actor_pipeline.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
#endif

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

/* ---------------------------------------------------------------- */
/* Portable thread launch/join (mirrors test_telemetry_fabric.c)      */
/* ---------------------------------------------------------------- */

#if defined(_WIN32)
typedef HANDLE thread_handle_t;
typedef DWORD (WINAPI *thread_fn_t)(LPVOID);
#define THREAD_FN(name) DWORD WINAPI name(LPVOID arg)
static thread_handle_t thread_start(thread_fn_t fn, void *arg) {
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}
static void thread_join(thread_handle_t h) {
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}
#else
typedef pthread_t thread_handle_t;
typedef void *(*thread_fn_t)(void *);
#define THREAD_FN(name) void *name(void *arg)
static thread_handle_t thread_start(thread_fn_t fn, void *arg) {
    pthread_t t;
    pthread_create(&t, NULL, fn, arg);
    return t;
}
static void thread_join(thread_handle_t h) {
    pthread_join(h, NULL);
}
#endif

/* ---------------------------------------------------------------- */
/* Part A: actor_task_queue_bridge_one -- basic round trip             */
/* ---------------------------------------------------------------- */

static void test_bridge_basic_roundtrip(void) {
    SECTION("actor_task_queue_bridge_one: barry_task_queue_t -> actor mailbox, zero corruption");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);

    const char msg[] = "hello-bridge";
    CHECK(barry_task_enqueue(&q, 42u, 7u, msg, (uint32_t)sizeof msg) == BARRY_OK);

    CHECK(actor_task_queue_bridge_one(actor, &q, NULL) == ACTOR_PIPELINE_OK);
    CHECK(actor_mailbox_count(actor) == 1u);

    ring_msg_t out = {0};
    CHECK(actor_process_one(actor, &out) == ACTOR_OK);
    CHECK(out.sender_id == 42u);
    CHECK(out.type == 7u);
    CHECK(memcmp(out.payload, msg, sizeof msg) == 0);
    /* Zero-padded tail beyond the copied payload. */
    for (uint32_t i = (uint32_t)sizeof msg; i < RING_MSG_PAYLOAD_SIZE; i++) {
        CHECK(out.payload[i] == 0u);
    }

    /* Queue now empty. */
    CHECK(actor_task_queue_bridge_one(actor, &q, NULL) == ACTOR_PIPELINE_ERR_QUEUE_EMPTY);
}

static void test_bridge_invalid_args(void) {
    SECTION("actor_task_queue_bridge_one: invalid arguments rejected");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);

    CHECK(actor_task_queue_bridge_one(NULL, &q, NULL) == ACTOR_PIPELINE_ERR_INVALID);
    CHECK(actor_task_queue_bridge_one(actor, NULL, NULL) == ACTOR_PIPELINE_ERR_INVALID);
}

static void test_bridge_payload_too_large(void) {
    SECTION("actor_task_queue_bridge_one: oversized task payload dropped, not silently truncated");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);

    /* BARRY_TASK_PAYLOAD_MAX (64) > RING_MSG_PAYLOAD_SIZE (48): a task
     * this large fits the broker but not one actor mailbox slot. */
    uint8_t big[BARRY_TASK_PAYLOAD_MAX];
    memset(big, 0xAB, sizeof big);
    CHECK(RING_MSG_PAYLOAD_SIZE < BARRY_TASK_PAYLOAD_MAX);
    CHECK(barry_task_enqueue(&q, 1u, 1u, big, RING_MSG_PAYLOAD_SIZE + 1u) == BARRY_OK);

    CHECK(actor_task_queue_bridge_one(actor, &q, NULL) == ACTOR_PIPELINE_ERR_PAYLOAD_TOO_LARGE);
    /* Dropped, not delivered: mailbox stays empty. */
    CHECK(actor_mailbox_count(actor) == 0u);
    /* And not requeued: the broker is now empty too. */
    CHECK(actor_task_queue_bridge_one(actor, &q, NULL) == ACTOR_PIPELINE_ERR_QUEUE_EMPTY);
}

static void test_bridge_mailbox_full(void) {
    SECTION("actor_task_queue_bridge_one: destination mailbox backpressure is O(1), never a wait");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);

    for (uint32_t i = 0; i < RING_BUFFER_CAPACITY + 1u; i++) {
        CHECK(barry_task_enqueue(&q, 1u, i, "x", 1u) == BARRY_OK);
    }

    uint32_t ok_count = 0, full_count = 0;
    for (uint32_t i = 0; i < RING_BUFFER_CAPACITY + 1u; i++) {
        actor_pipeline_status_t st = actor_task_queue_bridge_one(actor, &q, NULL);
        if (st == ACTOR_PIPELINE_OK) {
            ok_count++;
        } else if (st == ACTOR_PIPELINE_ERR_MAILBOX_FULL) {
            full_count++;
        } else {
            CHECK(0 && "unexpected status while filling mailbox to capacity");
        }
    }
    CHECK(ok_count == RING_BUFFER_CAPACITY);
    CHECK(full_count == 1u);
    /* The rejected task was still removed from the broker (dropped, same
     * disclosed posture as the oversized-payload case). */
    CHECK(actor_task_queue_bridge_one(actor, &q, NULL) == ACTOR_PIPELINE_ERR_QUEUE_EMPTY);
}

/* ---------------------------------------------------------------- */
/* Part B: actor_process_one_audited                                   */
/* ---------------------------------------------------------------- */

static void test_process_one_audited(void) {
    SECTION("actor_process_one_audited: unchanged processing + hash-chained audit trail");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 2) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 2);

    tf_log_t log;
    tf_log_init(&log);

    ring_msg_t *slot = NULL;
    CHECK(actor_mailbox_reserve(actor, &slot) == ACTOR_OK);
    actor_mailbox_publish(actor, slot, 99u, 3u);

    ring_msg_t out = {0};
    CHECK(actor_process_one_audited(actor, &out, &log) == ACTOR_OK);
    CHECK(out.sender_id == 99u && out.type == 3u);

    /* Empty mailbox now -- ACTOR_ERR_EMPTY is not a failure, still logged. */
    CHECK(actor_process_one_audited(actor, &out, &log) == ACTOR_ERR_EMPTY);

    uint64_t broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
    CHECK(log.entries[0].event_type == ACTOR_PIPELINE_TF_EVENT_PROCESS);
    CHECK(log.entries[1].event_type == ACTOR_PIPELINE_TF_EVENT_PROCESS);

    actor_pipeline_tf_audit_payload_t p0, p1;
    memcpy(&p0, log.entries[0].payload, sizeof p0);
    memcpy(&p1, log.entries[1].payload, sizeof p1);
    CHECK(p0.actor_id == 2u && p0.producer_id == 99u && p0.type == 3u && p0.status == (int16_t)ACTOR_OK);
    CHECK(p1.actor_id == 2u && p1.status == (int16_t)ACTOR_ERR_EMPTY);
}

static void test_process_one_audited_null_log_matches_unaudited(void) {
    SECTION("actor_process_one_audited: audit_log == NULL behaves exactly like actor_process_one");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    ring_msg_t *slot = NULL;
    CHECK(actor_mailbox_reserve(actor, &slot) == ACTOR_OK);
    actor_mailbox_publish(actor, slot, 5u, 6u);

    ring_msg_t out = {0};
    CHECK(actor_process_one_audited(actor, &out, NULL) == ACTOR_OK);
    CHECK(out.sender_id == 5u && out.type == 6u);
    CHECK(actor_process_one_audited(actor, &out, NULL) == ACTOR_ERR_EMPTY);
}

/* ---------------------------------------------------------------- */
/* Part C: real concurrency stress -- competing actor consumers over    */
/* ONE shared barry_task_queue_t, exactly the capability gap this file  */
/* bridges (ring_buffer.h alone is single-consumer).                    */
/* ---------------------------------------------------------------- */

#define STRESS_PRODUCER_THREADS 4
#define STRESS_TASKS_PER_PRODUCER 10
#define STRESS_TOTAL_TASKS (STRESS_PRODUCER_THREADS * STRESS_TASKS_PER_PRODUCER)
#define STRESS_ACTOR_COUNT 4

_Static_assert(STRESS_TOTAL_TASKS < RING_BUFFER_CAPACITY,
               "keep total below one mailbox's capacity so worst-case scheduling skew never hits MAILBOX_FULL");

static barry_task_queue_t g_stress_queue;
static actor_system_t     g_stress_sys;
static tf_log_t           g_stress_log;

typedef struct {
    uint32_t producer_id;
} producer_arg_t;

static THREAD_FN(stress_producer) {
    producer_arg_t *a = (producer_arg_t *)arg;
    for (uint32_t i = 0; i < STRESS_TASKS_PER_PRODUCER; i++) {
        /* marker = globally unique task id, encoded in the payload */
        uint32_t marker = a->producer_id * STRESS_TASKS_PER_PRODUCER + i;
        barry_status_t st = barry_task_enqueue(&g_stress_queue, a->producer_id, marker, &marker, (uint32_t)sizeof marker);
        (void)st; /* queue is sized well above STRESS_TOTAL_TASKS; not expected to reject here */
    }
    return 0;
}

typedef struct {
    uint8_t  actor_id;
    uint32_t bridged_ok;
} consumer_arg_t;

static THREAD_FN(stress_consumer) {
    consumer_arg_t *a = (consumer_arg_t *)arg;
    actor_t *actor = actor_system_get(&g_stress_sys, a->actor_id);
    for (;;) {
        actor_pipeline_status_t st = actor_task_queue_bridge_one(actor, &g_stress_queue, &g_stress_log);
        if (st == ACTOR_PIPELINE_ERR_QUEUE_EMPTY) {
            break;
        }
        CHECK(st == ACTOR_PIPELINE_OK); /* STRESS_TOTAL_TASKS < RING_BUFFER_CAPACITY: never MAILBOX_FULL/TOO_LARGE here */
        a->bridged_ok++;
    }
    return 0;
}

static void test_concurrent_competing_actor_consumers(void) {
    SECTION("stress: N actors as competing consumers over ONE shared MPMC broker queue");

    barry_task_queue_init(&g_stress_queue);
    actor_system_init(&g_stress_sys);
    tf_log_init(&g_stress_log);
    for (uint8_t i = 0; i < STRESS_ACTOR_COUNT; i++) {
        CHECK(actor_system_spawn(&g_stress_sys, i) == ACTOR_OK);
    }

    producer_arg_t producer_args[STRESS_PRODUCER_THREADS];
    thread_handle_t producers[STRESS_PRODUCER_THREADS];
    for (uint32_t i = 0; i < STRESS_PRODUCER_THREADS; i++) {
        producer_args[i].producer_id = i;
        producers[i] = thread_start(stress_producer, &producer_args[i]);
    }
    for (uint32_t i = 0; i < STRESS_PRODUCER_THREADS; i++) {
        thread_join(producers[i]);
    }
    CHECK(barry_task_queue_count(&g_stress_queue) == STRESS_TOTAL_TASKS);

    consumer_arg_t consumer_args[STRESS_ACTOR_COUNT];
    thread_handle_t consumers[STRESS_ACTOR_COUNT];
    for (uint32_t i = 0; i < STRESS_ACTOR_COUNT; i++) {
        consumer_args[i].actor_id    = (uint8_t)i;
        consumer_args[i].bridged_ok  = 0;
        consumers[i] = thread_start(stress_consumer, &consumer_args[i]);
    }
    for (uint32_t i = 0; i < STRESS_ACTOR_COUNT; i++) {
        thread_join(consumers[i]);
    }

    uint32_t total_bridged = 0;
    for (uint32_t i = 0; i < STRESS_ACTOR_COUNT; i++) {
        total_bridged += consumer_args[i].bridged_ok;
    }
    CHECK(total_bridged == STRESS_TOTAL_TASKS);
    CHECK(barry_task_queue_count(&g_stress_queue) == 0u);

    /* Every marker in [0, STRESS_TOTAL_TASKS) was delivered to exactly
     * one actor's mailbox -- no duplication, no loss, no corruption. */
    bool seen[STRESS_TOTAL_TASKS];
    memset(seen, 0, sizeof seen);
    uint32_t drained = 0;
    for (uint8_t i = 0; i < STRESS_ACTOR_COUNT; i++) {
        actor_t *actor = actor_system_get(&g_stress_sys, i);
        ring_msg_t out = {0};
        while (actor_process_one(actor, &out) == ACTOR_OK) {
            uint32_t marker;
            memcpy(&marker, out.payload, sizeof marker);
            CHECK(marker < STRESS_TOTAL_TASKS);
            CHECK(out.type == marker); /* enqueue used type == marker */
            if (marker < STRESS_TOTAL_TASKS) {
                CHECK(!seen[marker]);
                seen[marker] = true;
            }
            drained++;
        }
    }
    CHECK(drained == STRESS_TOTAL_TASKS);
    for (uint32_t i = 0; i < STRESS_TOTAL_TASKS; i++) {
        CHECK(seen[i]);
    }

    /* The audit trail written concurrently by every consumer thread
     * (tf_log_append()'s own bounded-spin sequential-WAL discipline,
     * see telemetry_fabric.h) is still a provably untampered chain. */
    uint64_t broken = 0;
    CHECK(tf_log_verify(&g_stress_log, &broken) == TF_OK);
}

/* ---------------------------------------------------------------- */
/* Part D: actor_task_queue_bridge_one_inspected() -- Zero-Trust        */
/* interception wired into the actor pipeline, plus real (measured,     */
/* not assumed) latency profiling of the O(1)-bounded inspection path.  */
/* ---------------------------------------------------------------- */

static void test_inspected_bridge_allows_benign_payload(void) {
    SECTION("actor_task_queue_bridge_one_inspected: benign payload reaches the actor unchanged");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);
    const char msg[] = "check status";
    CHECK(barry_task_enqueue(&q, 7u, 1u, msg, (uint32_t)sizeof msg) == BARRY_OK);

    CHECK(actor_task_queue_bridge_one_inspected(actor, &q, NULL) == ACTOR_PIPELINE_OK);
    CHECK(actor_mailbox_count(actor) == 1u);

    ring_msg_t out = {0};
    CHECK(actor_process_one(actor, &out) == ACTOR_OK);
    CHECK(memcmp(out.payload, msg, sizeof msg) == 0);
}

static void test_inspected_bridge_blocks_dangerous_payload(void) {
    SECTION("actor_task_queue_bridge_one_inspected: dangerous payload dropped BEFORE reaching the actor, audited");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);
    const char msg[] = "run os.system(\"whoami\")";
    CHECK(barry_task_enqueue(&q, 7u, 1u, msg, (uint32_t)sizeof msg) == BARRY_OK);

    tf_log_t log;
    tf_log_init(&log);

    agent_inspect_result_t independent_check;
    CHECK(agent_inspect_instruction(msg, (uint32_t)sizeof msg, &independent_check) == AGENT_INSPECT_OK);
    CHECK(independent_check.verdict == AGENT_VERDICT_BLOCKED);

    CHECK(actor_task_queue_bridge_one_inspected(actor, &q, &log) == ACTOR_PIPELINE_ERR_BLOCKED);
    /* Never touched the actor's mailbox or private memory. */
    CHECK(actor_mailbox_count(actor) == 0u);
    /* Dropped, not requeued -- same disclosed posture as an oversized payload. */
    CHECK(actor_task_queue_bridge_one_inspected(actor, &q, NULL) == ACTOR_PIPELINE_ERR_QUEUE_EMPTY);

    uint64_t broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
    actor_pipeline_tf_audit_payload_t payload;
    memcpy(&payload, log.entries[0].payload, sizeof payload);
    CHECK(payload.status == (int16_t)ACTOR_PIPELINE_ERR_BLOCKED);
    CHECK(payload.matched_rule_id == independent_check.matched_rule_id);
}

static void test_inspected_bridge_still_enforces_existing_boundaries(void) {
    SECTION("actor_task_queue_bridge_one_inspected: payload-too-large/mailbox-full behavior unchanged from the uninspected bridge");

    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);

    barry_task_queue_t q;
    barry_task_queue_init(&q);
    uint8_t big[BARRY_TASK_PAYLOAD_MAX];
    memset(big, 0x41, sizeof big); /* benign bytes -- 'A' repeated, no denied pattern */
    CHECK(barry_task_enqueue(&q, 1u, 1u, big, RING_MSG_PAYLOAD_SIZE + 1u) == BARRY_OK);

    CHECK(actor_task_queue_bridge_one_inspected(actor, &q, NULL) == ACTOR_PIPELINE_ERR_PAYLOAD_TOO_LARGE);
    CHECK(actor_mailbox_count(actor) == 0u);
}

#if defined(_WIN32)
static double now_seconds(void) {
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    LARGE_INTEGER counter;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}
#else
static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

#define PROFILE_ITERATIONS 50000
#define PROFILE_MAX_NS_PER_CALL (5.0 * 1000000.0) /* 5ms, this session's requested ceiling */

static void test_inspection_latency_profiling(void) {
    SECTION("profiling: agent_inspect_instruction() and the inspected bridge stay well under 5ms/call (measured, not assumed)");

    const char benign[] = "process the incoming request and report status";

    double t0 = now_seconds();
    for (int i = 0; i < PROFILE_ITERATIONS; i++) {
        agent_inspect_result_t result;
        (void)agent_inspect_instruction(benign, (uint32_t)sizeof benign, &result);
    }
    double t1 = now_seconds();
    double inspect_ns = (t1 - t0) / (double)PROFILE_ITERATIONS * 1e9;

    printf("   agent_inspect_instruction: %.1f ns/call over %d iterations (ceiling: %.0f ns / 5ms)\n",
           inspect_ns, PROFILE_ITERATIONS, PROFILE_MAX_NS_PER_CALL);
    CHECK(inspect_ns < PROFILE_MAX_NS_PER_CALL);

    /* End-to-end: enqueue -> inspected bridge -> mailbox drain, the real
     * path a caller actually exercises, not just the inspector alone. */
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0) == ACTOR_OK);
    actor_t *actor = actor_system_get(&sys, 0);
    barry_task_queue_t q;
    barry_task_queue_init(&q);

    t0 = now_seconds();
    for (int i = 0; i < PROFILE_ITERATIONS; i++) {
        CHECK(barry_task_enqueue(&q, 1u, 1u, benign, (uint32_t)sizeof benign) == BARRY_OK);
        CHECK(actor_task_queue_bridge_one_inspected(actor, &q, NULL) == ACTOR_PIPELINE_OK);
        ring_msg_t out = {0};
        CHECK(actor_process_one(actor, &out) == ACTOR_OK);
    }
    t1 = now_seconds();
    double bridge_ns = (t1 - t0) / (double)PROFILE_ITERATIONS * 1e9;

    printf("   enqueue -> actor_task_queue_bridge_one_inspected -> drain: %.1f ns/call over %d iterations\n",
           bridge_ns, PROFILE_ITERATIONS);
    CHECK(bridge_ns < PROFILE_MAX_NS_PER_CALL);
}

/* ---------------------------------------------------------------- */

int main(void) {
    test_bridge_basic_roundtrip();
    test_bridge_invalid_args();
    test_bridge_payload_too_large();
    test_bridge_mailbox_full();
    test_process_one_audited();
    test_process_one_audited_null_log_matches_unaudited();
    test_concurrent_competing_actor_consumers();
    test_inspected_bridge_allows_benign_payload();
    test_inspected_bridge_blocks_dangerous_payload();
    test_inspected_bridge_still_enforces_existing_boundaries();
    test_inspection_latency_profiling();

    printf("\n%d tests run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
