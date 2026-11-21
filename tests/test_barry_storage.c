/*
 * test_barry_storage.c - Unit + true multi-thread stress test bench for
 * barry_storage.{h,c} (Niveaux 11-20): the KV cache store (including
 * automatic watermark-triggered sharding under concurrent load), the
 * documentary store + single-field relation index, the bounded MPMC task
 * queue/message broker (including strict backpressure and a real
 * wire_msg_t ingest round trip), and the per-metric time-series ring
 * (including simultaneous injection from 8 producer threads).
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching barry_storage.c's own zero-allocation
 * constraint. Portable threading (#if _WIN32 CreateThread / else
 * pthread), same pattern as tests/test_concurrent_core.c -- this test is
 * built and run on every OS the Makefile targets, not gated to
 * Windows-only like test_infra_mesh.c (barry_storage.c itself has no
 * Windows-specific dependency).
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "barry_storage.h"
#include "wire_proto.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <pthread.h>
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
/* Portable thread launch/join (mirrors test_concurrent_core.c)       */
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

#define MAX_THREADS 8

/* ---------------------------------------------------------------- */
/* Part A: KV cache store                                            */
/* ---------------------------------------------------------------- */

static void test_kv_put_get_roundtrip(void) {
    SECTION("barry_kv_put/get: basic round trip, not-found, delete");
    barry_kv_store_t store;
    barry_kv_init(&store);

    uint8_t buf[BARRY_KV_VAL_MAX];
    uint32_t len = sizeof buf;

    CHECK(barry_kv_get(&store, "missing", 7, buf, &len) == BARRY_ERR_NOT_FOUND);

    CHECK(barry_kv_put(&store, "counter:tenant7", 15, "42", 2) == BARRY_OK);
    len = sizeof buf;
    CHECK(barry_kv_get(&store, "counter:tenant7", 15, buf, &len) == BARRY_OK);
    CHECK(len == 2);
    CHECK(memcmp(buf, "42", 2) == 0);

    /* update in place: same key, new value */
    CHECK(barry_kv_put(&store, "counter:tenant7", 15, "43", 2) == BARRY_OK);
    len = sizeof buf;
    CHECK(barry_kv_get(&store, "counter:tenant7", 15, buf, &len) == BARRY_OK);
    CHECK(len == 2);
    CHECK(memcmp(buf, "43", 2) == 0);
    CHECK(barry_kv_active_shard_count(&store) == 1u); /* one key never triggers a promotion */

    CHECK(barry_kv_delete(&store, "counter:tenant7", 15) == BARRY_OK);
    len = sizeof buf;
    CHECK(barry_kv_get(&store, "counter:tenant7", 15, buf, &len) == BARRY_ERR_NOT_FOUND);
    CHECK(barry_kv_delete(&store, "counter:tenant7", 15) == BARRY_ERR_NOT_FOUND); /* double-delete */
}

static void test_kv_rejects_oversized(void) {
    SECTION("barry_kv_put: rejects oversized key/value as BARRY_ERR_INVALID");
    barry_kv_store_t store;
    barry_kv_init(&store);

    uint8_t oversized_key[BARRY_KV_KEY_MAX + 1];
    memset(oversized_key, 'k', sizeof oversized_key);
    CHECK(barry_kv_put(&store, oversized_key, sizeof oversized_key, "v", 1) == BARRY_ERR_INVALID);

    uint8_t oversized_val[BARRY_KV_VAL_MAX + 1];
    memset(oversized_val, 'v', sizeof oversized_val);
    CHECK(barry_kv_put(&store, "k", 1, oversized_val, sizeof oversized_val) == BARRY_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* Part B: documentary store + relation index                        */
/* ---------------------------------------------------------------- */

static barry_document_t make_tenant_doc(const char *plan, const char *region) {
    barry_document_t doc;
    memset(&doc, 0, sizeof doc);
    doc.field_count = 2;
    strncpy(doc.fields[0].key, "plan", BARRY_DOC_FIELD_KEY_MAX - 1);
    strncpy(doc.fields[0].val, plan, BARRY_DOC_FIELD_VAL_MAX - 1);
    strncpy(doc.fields[1].key, "region", BARRY_DOC_FIELD_KEY_MAX - 1);
    strncpy(doc.fields[1].val, region, BARRY_DOC_FIELD_VAL_MAX - 1);
    return doc;
}

static void test_doc_put_get_fields(void) {
    SECTION("barry_doc_put/get: documentary store round trip with real fields");
    barry_doc_store_t store;
    barry_doc_init(&store);

    barry_document_t in = make_tenant_doc("pro", "eu-west");
    CHECK(barry_doc_put(&store, "tenant:7", 8, &in) == BARRY_OK);

    barry_document_t out;
    CHECK(barry_doc_get(&store, "tenant:7", 8, &out) == BARRY_OK);
    CHECK(out.field_count == 2u);
    CHECK(strcmp(out.fields[0].key, "plan") == 0);
    CHECK(strcmp(out.fields[0].val, "pro") == 0);
    CHECK(strcmp(out.fields[1].key, "region") == 0);
    CHECK(strcmp(out.fields[1].val, "eu-west") == 0);

    CHECK(barry_doc_delete(&store, "tenant:7", 8) == BARRY_OK);
    CHECK(barry_doc_get(&store, "tenant:7", 8, &out) == BARRY_ERR_NOT_FOUND);
}

static void test_doc_rejects_bad_field_count(void) {
    SECTION("barry_doc_put: rejects field_count > BARRY_DOC_MAX_FIELDS");
    barry_doc_store_t store;
    barry_doc_init(&store);
    barry_document_t bad;
    memset(&bad, 0, sizeof bad);
    bad.field_count = BARRY_DOC_MAX_FIELDS + 1u;
    CHECK(barry_doc_put(&store, "x", 1, &bad) == BARRY_ERR_INVALID);
}

static void test_relation_index(void) {
    SECTION("barry_index_link/lookup: single-hop relation index, O(1) bucket, bounded depth");
    barry_relation_index_t idx;
    barry_index_init(&idx);

    CHECK(barry_index_link(&idx, "eu-west", 7, "tenant:1", 8) == BARRY_OK);
    CHECK(barry_index_link(&idx, "eu-west", 7, "tenant:2", 8) == BARRY_OK);
    CHECK(barry_index_link(&idx, "eu-west", 7, "tenant:3", 8) == BARRY_OK);

    uint8_t keys[BARRY_INDEX_BUCKET_DEPTH][BARRY_DOC_KEY_MAX];
    uint32_t lens[BARRY_INDEX_BUCKET_DEPTH];
    uint32_t count = 0;
    CHECK(barry_index_lookup(&idx, "eu-west", 7, keys, lens, BARRY_INDEX_BUCKET_DEPTH, &count) == BARRY_OK);
    CHECK(count == 3u);
    CHECK(lens[0] == 8u && memcmp(keys[0], "tenant:1", 8) == 0);
    CHECK(lens[1] == 8u && memcmp(keys[1], "tenant:2", 8) == 0);
    CHECK(lens[2] == 8u && memcmp(keys[2], "tenant:3", 8) == 0);

    /* a different field value hashes into a (very likely) different
     * bucket and must never see "eu-west"'s entries */
    count = 12345u;
    CHECK(barry_index_lookup(&idx, "us-east", 7, keys, lens, BARRY_INDEX_BUCKET_DEPTH, &count) == BARRY_OK);
    CHECK(count == 0u);
}

static void test_relation_index_bounded_depth(void) {
    SECTION("barry_index_link: rejects past BARRY_INDEX_BUCKET_DEPTH with BARRY_ERR_FULL");
    barry_relation_index_t idx;
    barry_index_init(&idx);

    char field[] = "shared-field";
    int ok_count = 0;
    for (uint32_t i = 0; i < BARRY_INDEX_BUCKET_DEPTH + 4u; i++) {
        char key[BARRY_DOC_KEY_MAX];
        snprintf(key, sizeof key, "doc:%u", i);
        barry_status_t st = barry_index_link(&idx, field, (uint32_t)strlen(field),
                                              key, (uint32_t)strlen(key));
        if (st == BARRY_OK) {
            ok_count++;
        } else {
            CHECK(st == BARRY_ERR_FULL);
        }
    }
    CHECK(ok_count == (int)BARRY_INDEX_BUCKET_DEPTH);
}

/* ---------------------------------------------------------------- */
/* Part C: MPMC task queue / message broker                          */
/* ---------------------------------------------------------------- */

static barry_task_queue_t g_layout_check_queue;

static void test_task_queue_slots_are_64byte_aligned(void) {
    SECTION("barry_task_slot_t: each slot in the MPMC ring is individually 64-byte aligned, pre-allocated, zero dynamic allocation");

    CHECK(sizeof(barry_task_slot_t) % 64u == 0u);
    CHECK(_Alignof(barry_task_slot_t) == 64u);

    /* Real address arithmetic on the actual embedded array -- not just
     * the type's own sizeof/alignof in isolation -- proves slots[] as
     * laid out inside barry_task_queue_t really does keep every entry on
     * its own 64-byte boundary, statically embedded (no malloc/new
     * anywhere in barry_storage.c). */
    uintptr_t base = (uintptr_t)&g_layout_check_queue.slots[0];
    CHECK(base % 64u == 0u);
    for (uint32_t i = 0; i < 8u; i++) {
        uintptr_t addr = (uintptr_t)&g_layout_check_queue.slots[i];
        CHECK(addr % 64u == 0u);
        CHECK(addr - base == (uintptr_t)i * sizeof(barry_task_slot_t));
    }
}

static void test_task_queue_fifo_and_backpressure(void) {
    SECTION("barry_task_enqueue/dequeue: FIFO ordering, strict O(1) backpressure when full");
    barry_task_queue_t q;
    barry_task_queue_init(&q);

    for (uint32_t i = 0; i < 5; i++) {
        CHECK(barry_task_enqueue(&q, 1u, i, &i, sizeof i) == BARRY_OK);
    }
    CHECK(barry_task_queue_count(&q) == 5u);

    for (uint32_t i = 0; i < 5; i++) {
        barry_task_t task;
        CHECK(barry_task_dequeue(&q, &task) == BARRY_OK);
        CHECK(task.type == i); /* FIFO order preserved */
        uint32_t val;
        memcpy(&val, task.payload, sizeof val);
        CHECK(val == i);
    }

    barry_task_t empty_task;
    CHECK(barry_task_dequeue(&q, &empty_task) == BARRY_ERR_EMPTY);

    /* fill to capacity, then verify instant, counted rejection */
    for (uint32_t i = 0; i < BARRY_TASK_QUEUE_CAPACITY; i++) {
        CHECK(barry_task_enqueue(&q, 1u, i, NULL, 0) == BARRY_OK);
    }
    CHECK(barry_task_queue_count(&q) == BARRY_TASK_QUEUE_CAPACITY);
    CHECK(barry_task_enqueue(&q, 1u, 999u, NULL, 0) == BARRY_ERR_FULL);
    CHECK(atomic_load_explicit(&q.rejected_full_total, memory_order_relaxed) >= 1u);
}

static uint64_t g_worker_processed_sum = 0;
static void sum_handler(void *ctx, const barry_task_t *task) {
    (void)ctx;
    uint64_t *sum = &g_worker_processed_sum;
    *sum += task->type;
}

static void test_worker_process_one(void) {
    SECTION("barry_worker_process_one: dequeues and invokes handler exactly once per call");
    barry_task_queue_t q;
    barry_task_queue_init(&q);
    g_worker_processed_sum = 0;

    for (uint32_t i = 1; i <= 4; i++) {
        CHECK(barry_task_enqueue(&q, 1u, i, NULL, 0) == BARRY_OK);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(barry_worker_process_one(&q, sum_handler, NULL) == BARRY_OK);
    }
    CHECK(g_worker_processed_sum == 10u); /* 1+2+3+4 */
    CHECK(barry_worker_process_one(&q, sum_handler, NULL) == BARRY_ERR_EMPTY);
}

static void test_ingest_wire_msg_roundtrip(void) {
    SECTION("barry_ingest_wire_msg: real wire_msg_t enqueued and dequeued byte-for-byte");
    barry_task_queue_t q;
    barry_task_queue_init(&q);

    wire_msg_t msg;
    uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(wire_build_msg(&msg, 42u, 7u, payload, sizeof payload) == WIRE_OK);

    CHECK(barry_ingest_wire_msg(&q, 99u, &msg) == BARRY_OK);

    barry_task_t task;
    CHECK(barry_task_dequeue(&q, &task) == BARRY_OK);
    CHECK(task.producer_id == 99u);
    CHECK(task.type == BARRY_TASK_TYPE_WIRE_MSG);
    CHECK(task.payload_len == sizeof(wire_msg_t));
    CHECK(memcmp(task.payload, &msg, sizeof(wire_msg_t)) == 0);
}

/* ---------------------------------------------------------------- */
/* Part D: time-series metrics                                       */
/* ---------------------------------------------------------------- */

static void test_ts_record_and_query(void) {
    SECTION("barry_ts_record/query_latest: single-threaded ordering and windowing");
    barry_ts_store_t store;
    barry_ts_init(&store);

    CHECK(barry_ts_register_metric(&store, 0u) == BARRY_OK);
    CHECK(barry_ts_register_metric(&store, 0u) == BARRY_ERR_ALREADY_REGISTERED);
    CHECK(barry_ts_record(&store, 5u /* never registered */, 1u, 1.0) == BARRY_ERR_OUT_OF_RANGE);

    for (uint64_t i = 0; i < 10; i++) {
        CHECK(barry_ts_record(&store, 0u, i, (double)i * 2.0) == BARRY_OK);
    }

    barry_ts_point_t out[10];
    uint32_t count = 0;
    CHECK(barry_ts_query_latest(&store, 0u, out, 10, &count) == BARRY_OK);
    CHECK(count == 10u);
    for (uint32_t i = 0; i < 10; i++) {
        CHECK(out[i].tick == i);            /* oldest of the window first */
        CHECK(out[i].value == (double)i * 2.0);
    }

    /* asking for fewer than were recorded returns only the most recent window */
    count = 0;
    CHECK(barry_ts_query_latest(&store, 0u, out, 3, &count) == BARRY_OK);
    CHECK(count == 3u);
    CHECK(out[0].tick == 7u && out[1].tick == 8u && out[2].tick == 9u);
}

/* ---------------------------------------------------------------- */
/* Multi-thread stress: 8 producer threads into a shared time-series  */
/* metric, simultaneously with 8 producer threads inserting unique    */
/* keys into a shared KV store until automatic sharding triggers.     */
/* ---------------------------------------------------------------- */

#define TS_POINTS_PER_THREAD  200u
#define KV_KEYS_PER_THREAD    113u /* 8 * 113 = 904 > 80% of one 1024-slot shard: guarantees at least one promotion */

typedef struct {
    barry_ts_store_t *ts_store;
    uint32_t          metric_id;
    int               thread_id;
} ts_thread_args_t;

typedef struct {
    barry_kv_store_t *kv_store;
    int               thread_id;
} kv_thread_args_t;

static THREAD_FN(ts_producer_thread) {
    ts_thread_args_t *args = (ts_thread_args_t *)arg;
    for (uint32_t i = 0; i < TS_POINTS_PER_THREAD; i++) {
        /* opaque, collision-free tick per producer -- not a wall clock,
         * see barry_storage.h's honest-scope note on `tick` */
        uint64_t tick = ((uint64_t)args->thread_id * 1000000u) + i;
        barry_ts_record(args->ts_store, args->metric_id, tick, (double)args->thread_id);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static THREAD_FN(kv_producer_thread) {
    kv_thread_args_t *args = (kv_thread_args_t *)arg;
    for (uint32_t i = 0; i < KV_KEYS_PER_THREAD; i++) {
        char key[BARRY_KV_KEY_MAX];
        int n = snprintf(key, sizeof key, "t%d:k%u", args->thread_id, i);
        char val[8];
        int vn = snprintf(val, sizeof val, "%u", i);
        barry_kv_put(args->kv_store, key, (uint32_t)n, val, (uint32_t)vn);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_timeseries_and_sharding_under_load(void) {
    SECTION("8 producer threads: simultaneous time-series injection + KV auto-sharding under load");

    static barry_ts_store_t g_ts_store;
    static barry_kv_store_t g_kv_store;
    barry_ts_init(&g_ts_store);
    barry_kv_init(&g_kv_store);
    CHECK(barry_ts_register_metric(&g_ts_store, 0u) == BARRY_OK);

    ts_thread_args_t ts_args[MAX_THREADS];
    kv_thread_args_t kv_args[MAX_THREADS];
    thread_handle_t  ts_threads[MAX_THREADS];
    thread_handle_t  kv_threads[MAX_THREADS];

    for (int t = 0; t < MAX_THREADS; t++) {
        ts_args[t].ts_store = &g_ts_store;
        ts_args[t].metric_id = 0u;
        ts_args[t].thread_id = t;
        kv_args[t].kv_store = &g_kv_store;
        kv_args[t].thread_id = t;
    }

    /* launch both swarms interleaved so they genuinely race each other,
     * not just their own kind */
    for (int t = 0; t < MAX_THREADS; t++) {
        ts_threads[t] = thread_start(ts_producer_thread, &ts_args[t]);
        kv_threads[t] = thread_start(kv_producer_thread, &kv_args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(ts_threads[t]);
        thread_join(kv_threads[t]);
    }

    /* --- time-series: every point from every producer must be
     * accounted for in the ring's write cursor (fetch_add gives each
     * producer a distinct slot -- no lost writes under concurrency) --- */
    uint32_t expected_total = MAX_THREADS * TS_POINTS_PER_THREAD;
    barry_ts_point_t latest[BARRY_TS_CAPACITY];
    uint32_t got = 0;
    CHECK(barry_ts_query_latest(&g_ts_store, 0u, latest, BARRY_TS_CAPACITY, &got) == BARRY_OK);
    /* the ring only retains the most recent BARRY_TS_CAPACITY points --
     * expected_total (1600) exceeds that (1024), so the window is full
     * and holds exactly the newest BARRY_TS_CAPACITY of them */
    CHECK(expected_total > BARRY_TS_CAPACITY);
    CHECK(got == BARRY_TS_CAPACITY);

    /* --- KV: automatic sharding must have triggered under this load --- */
    uint32_t active = barry_kv_active_shard_count(&g_kv_store);
    CHECK(active > 1u);
    CHECK(active <= BARRY_KV_SHARD_COUNT);
    CHECK(barry_kv_shard_promotions_total(&g_kv_store) >= 1u);

    /* every single one of the 904 concurrently-inserted keys must still
     * be correctly retrievable afterwards, regardless of which shard it
     * ultimately landed in (sharding must never lose or corrupt data) */
    uint32_t verified = 0;
    for (int t = 0; t < MAX_THREADS; t++) {
        for (uint32_t i = 0; i < KV_KEYS_PER_THREAD; i++) {
            char key[BARRY_KV_KEY_MAX];
            int n = snprintf(key, sizeof key, "t%d:k%u", t, i);
            char expected_val[8];
            int vn = snprintf(expected_val, sizeof expected_val, "%u", i);

            uint8_t out_val[BARRY_KV_VAL_MAX];
            uint32_t out_len = sizeof out_val;
            barry_status_t st = barry_kv_get(&g_kv_store, key, (uint32_t)n, out_val, &out_len);
            if (st == BARRY_OK && out_len == (uint32_t)vn && memcmp(out_val, expected_val, (size_t)vn) == 0) {
                verified++;
            }
        }
    }
    CHECK(verified == MAX_THREADS * KV_KEYS_PER_THREAD);
}

static _Atomic uint32_t g_shared_writer_success = 0;

typedef struct {
    barry_kv_store_t *store;
    int               thread_id;
} contend_args_t;

static THREAD_FN(kv_same_key_writer_thread) {
    contend_args_t *args = (contend_args_t *)arg;
    for (int i = 0; i < 500; i++) {
        char val[16];
        snprintf(val, sizeof val, "w%d-i%d", args->thread_id, i);
        if (barry_kv_put(args->store, "shared:key", 10, val, (uint32_t)strlen(val)) == BARRY_OK) {
            atomic_fetch_add_explicit(&g_shared_writer_success, 1u, memory_order_relaxed);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_same_key_updates_stay_consistent(void) {
    SECTION("8 threads racing PUT on the SAME key: every get() afterwards is a whole, never a torn, value");
    barry_kv_store_t store;
    barry_kv_init(&store);
    atomic_store_explicit(&g_shared_writer_success, 0u, memory_order_relaxed);

    contend_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].store = &store;
        args[t].thread_id = t;
        threads[t] = thread_start(kv_same_key_writer_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }
    CHECK(atomic_load_explicit(&g_shared_writer_success, memory_order_relaxed) == MAX_THREADS * 500u);

    uint8_t out_val[BARRY_KV_VAL_MAX];
    uint32_t out_len = sizeof out_val;
    CHECK(barry_kv_get(&store, "shared:key", 10, out_val, &out_len) == BARRY_OK);
    /* the final value must be exactly one whole "w<thread>-i<iter>" string
     * written by SOME thread -- never a torn splice of two writers'
     * bytes. Validate its shape precisely rather than just its length. */
    int thread_id = -1, iter = -1;
    char terminator = 0;
    int parsed = sscanf((const char *)out_val, "w%d-i%d%c", &thread_id, &iter, &terminator);
    CHECK(parsed == 2); /* exactly "w<n>-i<n>", nothing trailing, no garbage byte */
    CHECK(thread_id >= 0 && thread_id < MAX_THREADS);
    CHECK(iter >= 0 && iter < 500);
}

/* ---------------------------------------------------------------- */

int main(void) {
    test_kv_put_get_roundtrip();
    test_kv_rejects_oversized();

    test_doc_put_get_fields();
    test_doc_rejects_bad_field_count();
    test_relation_index();
    test_relation_index_bounded_depth();

    test_task_queue_slots_are_64byte_aligned();
    test_task_queue_fifo_and_backpressure();
    test_worker_process_one();
    test_ingest_wire_msg_roundtrip();

    test_ts_record_and_query();

    test_concurrent_timeseries_and_sharding_under_load();
    test_concurrent_same_key_updates_stay_consistent();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
