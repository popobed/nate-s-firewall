/*
 * test_brain_core.c - Unit + true multi-thread stress test bench for
 * brain_core.{h,c} (Niveaux 21-30): IAM (constant-time auth + O(1)
 * scopes), the lock-free token-bucket rate limiter (including an 8-thread
 * contention stress test proving no over/under consumption), the secrets
 * vault (zero-residue revocation), the workflow orchestrator (backed by
 * a real barry_task_queue_t), the cron timing wheel, per-tenant billing
 * quotas (including an 8-thread concurrent-charge stress test), and the
 * business rule engine (programmatic + the minimal text loader).
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching brain_core.c's own zero-allocation constraint.
 * Portable threading (#if _WIN32 CreateThread / else pthread), same
 * pattern as tests/test_barry_storage.c.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "brain_core.h"

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
/* Portable thread launch/join (mirrors test_barry_storage.c)         */
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
/* Part A: IAM                                                        */
/* ---------------------------------------------------------------- */

static const uint8_t TOKEN_SECRET_A[BRAIN_IAM_SECRET_LEN] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
};
static const uint8_t TOKEN_SECRET_WRONG[BRAIN_IAM_SECRET_LEN] = {
    99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99,99
};

#define SCOPE_READ  0x1u
#define SCOPE_WRITE 0x2u
#define SCOPE_ADMIN 0x4u

static void test_iam_authenticate(void) {
    SECTION("brain_iam_register_token/authenticate: constant-time secret + O(1) scope check");
    brain_iam_registry_t reg;
    brain_iam_init(&reg);

    CHECK(brain_iam_register_token(&reg, 0, 7u, TOKEN_SECRET_A, SCOPE_READ | SCOPE_WRITE) == BRAIN_OK);
    CHECK(brain_iam_register_token(&reg, 0, 7u, TOKEN_SECRET_A, SCOPE_READ) == BRAIN_ERR_ALREADY_REGISTERED);

    uint16_t tenant = 0xFFFFu;
    CHECK(brain_iam_authenticate(&reg, 0, TOKEN_SECRET_A, BRAIN_IAM_SECRET_LEN, SCOPE_READ, &tenant) == BRAIN_OK);
    CHECK(tenant == 7u);

    tenant = 0xFFFFu;
    CHECK(brain_iam_authenticate(&reg, 0, TOKEN_SECRET_WRONG, BRAIN_IAM_SECRET_LEN, SCOPE_READ, &tenant) == BRAIN_ERR_UNAUTHORIZED);
    CHECK(tenant == 0xFFFFu); /* not written on failure */

    CHECK(brain_iam_authenticate(&reg, 0, TOKEN_SECRET_A, BRAIN_IAM_SECRET_LEN, SCOPE_ADMIN, &tenant) == BRAIN_ERR_UNAUTHORIZED);
    CHECK(brain_iam_authenticate(&reg, 0, TOKEN_SECRET_A, BRAIN_IAM_SECRET_LEN, SCOPE_READ | SCOPE_WRITE, &tenant) == BRAIN_OK);

    CHECK(brain_iam_authenticate(&reg, (uint16_t)BRAIN_IAM_MAX_TOKENS, TOKEN_SECRET_A, BRAIN_IAM_SECRET_LEN, 0u, &tenant) == BRAIN_ERR_OUT_OF_RANGE);
    CHECK(brain_iam_authenticate(&reg, 1, TOKEN_SECRET_A, BRAIN_IAM_SECRET_LEN, 0u, &tenant) == BRAIN_ERR_NOT_FOUND);
}

static void test_iam_revoke_zeroes_secret(void) {
    SECTION("brain_iam_revoke_token: zero-residue revocation, no ghost auth afterwards");
    brain_iam_registry_t reg;
    brain_iam_init(&reg);
    CHECK(brain_iam_register_token(&reg, 3, 1u, TOKEN_SECRET_A, SCOPE_READ) == BRAIN_OK);
    CHECK(brain_iam_revoke_token(&reg, 3) == BRAIN_OK);
    CHECK(brain_iam_revoke_token(&reg, 3) == BRAIN_ERR_NOT_FOUND); /* double revoke */

    uint16_t tenant;
    CHECK(brain_iam_authenticate(&reg, 3, TOKEN_SECRET_A, BRAIN_IAM_SECRET_LEN, 0u, &tenant) == BRAIN_ERR_NOT_FOUND);

    /* internal check: the raw secret bytes are actually zeroed, not just marked inactive */
    uint8_t zero[BRAIN_IAM_SECRET_LEN];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(reg.tokens[3].secret, zero, BRAIN_IAM_SECRET_LEN) == 0);

    /* re-registration after revoke succeeds (slot fully reusable) */
    CHECK(brain_iam_register_token(&reg, 3, 2u, TOKEN_SECRET_A, SCOPE_READ) == BRAIN_OK);
}

/* ---------------------------------------------------------------- */
/* Part B: token-bucket rate limiting                                 */
/* ---------------------------------------------------------------- */

static void test_rate_limit_basic(void) {
    SECTION("brain_rate_try_consume: capacity enforced, O(1) instant rejection when empty");
    brain_rate_limiter_t rl;
    brain_rate_init(&rl);
    CHECK(brain_rate_configure(&rl, 0, 5u, 0u) == BRAIN_OK);

    for (int i = 0; i < 5; i++) {
        CHECK(brain_rate_try_consume(&rl, 0, 0u, 1u) == BRAIN_OK);
    }
    CHECK(brain_rate_try_consume(&rl, 0, 0u, 1u) == BRAIN_ERR_RATE_LIMITED);

    CHECK(brain_rate_try_consume(&rl, (uint16_t)BRAIN_RATE_MAX_BUCKETS, 0u, 1u) == BRAIN_ERR_OUT_OF_RANGE);
    CHECK(brain_rate_try_consume(&rl, 1, 0u, 1u) == BRAIN_ERR_NOT_FOUND); /* never configured */
}

static void test_rate_limit_refill_over_ticks(void) {
    SECTION("brain_rate_try_consume: lazy refill scales with opaque elapsed ticks, capped at capacity");
    brain_rate_limiter_t rl;
    brain_rate_init(&rl);
    CHECK(brain_rate_configure(&rl, 0, 10u, 2u) == BRAIN_OK); /* capacity 10, +2 tokens/tick */

    for (int i = 0; i < 10; i++) {
        CHECK(brain_rate_try_consume(&rl, 0, 0u, 1u) == BRAIN_OK);
    }
    CHECK(brain_rate_try_consume(&rl, 0, 0u, 1u) == BRAIN_ERR_RATE_LIMITED);

    /* 3 ticks elapsed -> +6 tokens (capped well under capacity) */
    for (int i = 0; i < 6; i++) {
        CHECK(brain_rate_try_consume(&rl, 0, 3u, 1u) == BRAIN_OK);
    }
    CHECK(brain_rate_try_consume(&rl, 0, 3u, 1u) == BRAIN_ERR_RATE_LIMITED);

    /* far in the future -> refill caps at capacity, never overflows/wraps */
    CHECK(brain_rate_try_consume(&rl, 0, 1000000u, 10u) == BRAIN_OK);
    CHECK(brain_rate_try_consume(&rl, 0, 1000000u, 1u) == BRAIN_ERR_RATE_LIMITED);
}

static _Atomic uint32_t g_rate_success_count = 0;

typedef struct {
    brain_rate_limiter_t *rl;
    uint16_t              bucket_id;
    uint32_t              attempts;
} rate_thread_args_t;

static THREAD_FN(rate_consumer_thread) {
    rate_thread_args_t *args = (rate_thread_args_t *)arg;
    for (uint32_t i = 0; i < args->attempts; i++) {
        if (brain_rate_try_consume(args->rl, args->bucket_id, 0u, 1u) == BRAIN_OK) {
            atomic_fetch_add_explicit(&g_rate_success_count, 1u, memory_order_relaxed);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_rate_limit_concurrent_stress(void) {
    SECTION("8 threads hammering ONE bucket: total successful consumes == exact fixed capacity, never more, never less");
    brain_rate_limiter_t rl;
    brain_rate_init(&rl);
    const uint32_t capacity = 1000u;
    CHECK(brain_rate_configure(&rl, 0, capacity, 0u) == BRAIN_OK); /* no refill: a fixed, exhaustible budget */
    atomic_store_explicit(&g_rate_success_count, 0u, memory_order_relaxed);

    rate_thread_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].rl = &rl;
        args[t].bucket_id = 0;
        args[t].attempts = 500u; /* 8*500 = 4000 attempts against a budget of 1000 */
        threads[t] = thread_start(rate_consumer_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    CHECK(atomic_load_explicit(&g_rate_success_count, memory_order_relaxed) == capacity);
}

/* ---------------------------------------------------------------- */
/* Part C: secrets vault                                              */
/* ---------------------------------------------------------------- */

static void test_vault_store_reveal_revoke(void) {
    SECTION("brain_vault_store/reveal/revoke: round trip, buffer-too-small, zero-residue revoke");
    brain_vault_t vault;
    brain_vault_init(&vault);

    CHECK(brain_vault_store(&vault, 5, "s3cr3t-api-key", 15) == BRAIN_OK);

    uint8_t out[BRAIN_VAULT_SECRET_MAX];
    uint32_t out_len = sizeof out;
    CHECK(brain_vault_reveal(&vault, 5, out, &out_len) == BRAIN_OK);
    CHECK(out_len == 15u);
    CHECK(memcmp(out, "s3cr3t-api-key", 15) == 0);

    uint8_t tiny[4];
    uint32_t tiny_len = sizeof tiny;
    CHECK(brain_vault_reveal(&vault, 5, tiny, &tiny_len) == BRAIN_ERR_INVALID);

    CHECK(brain_vault_reveal(&vault, 6, out, &out_len) == BRAIN_ERR_NOT_FOUND);
    CHECK(brain_vault_reveal(&vault, (uint32_t)BRAIN_VAULT_MAX_SECRETS, out, &out_len) == BRAIN_ERR_OUT_OF_RANGE);

    CHECK(brain_vault_revoke(&vault, 5) == BRAIN_OK);
    CHECK(brain_vault_revoke(&vault, 5) == BRAIN_ERR_NOT_FOUND);
    out_len = sizeof out;
    CHECK(brain_vault_reveal(&vault, 5, out, &out_len) == BRAIN_ERR_NOT_FOUND);

    uint8_t zero[BRAIN_VAULT_SECRET_MAX];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(vault.entries[5].value, zero, BRAIN_VAULT_SECRET_MAX) == 0);
}

/* ---------------------------------------------------------------- */
/* Part D: workflow orchestrator                                      */
/* ---------------------------------------------------------------- */

static void test_workflow_advance_through_completion(void) {
    SECTION("brain_workflow_advance: enqueues each step onto a real barry_task_queue_t, completes cleanly");
    brain_workflow_registry_t reg;
    brain_workflow_engine_t eng;
    barry_task_queue_t queue;
    brain_workflow_registry_init(&reg);
    brain_workflow_engine_init(&eng);
    barry_task_queue_init(&queue);

    uint32_t steps[3] = { 111u, 222u, 333u };
    CHECK(brain_workflow_define(&reg, 0, steps, 3) == BRAIN_OK);
    CHECK(brain_workflow_start(&eng, &reg, 0, 0, 42u) == BRAIN_OK);

    for (int i = 0; i < 3; i++) {
        CHECK(brain_workflow_advance(&eng, &reg, &queue, 0) == BRAIN_OK);
    }
    CHECK(eng.instances[0].run_state == BRAIN_WF_COMPLETED);
    CHECK(brain_workflow_advance(&eng, &reg, &queue, 0) == BRAIN_ERR_INACTIVE);

    for (int i = 0; i < 3; i++) {
        barry_task_t task;
        CHECK(barry_task_dequeue(&queue, &task) == BARRY_OK);
        CHECK(task.producer_id == 0u); /* instance_id */
        CHECK(task.type == BRAIN_TASK_TYPE_WORKFLOW_STEP);
        uint32_t action_id;
        memcpy(&action_id, task.payload, sizeof action_id);
        CHECK(action_id == steps[i]);
    }
}

static void test_workflow_start_unknown_def_rejected(void) {
    SECTION("brain_workflow_start: rejects an undefined workflow_def_id");
    brain_workflow_registry_t reg;
    brain_workflow_engine_t eng;
    brain_workflow_registry_init(&reg);
    brain_workflow_engine_init(&eng);
    CHECK(brain_workflow_start(&eng, &reg, 0, 5, 1u) == BRAIN_ERR_NOT_FOUND);
}

static void test_workflow_backpressure_never_skips_step(void) {
    SECTION("brain_workflow_advance: a full event bus propagates BRAIN_ERR_FULL without advancing the step");
    brain_workflow_registry_t reg;
    brain_workflow_engine_t eng;
    barry_task_queue_t queue;
    brain_workflow_registry_init(&reg);
    brain_workflow_engine_init(&eng);
    barry_task_queue_init(&queue);

    uint32_t steps[1] = { 7u };
    CHECK(brain_workflow_define(&reg, 0, steps, 1) == BRAIN_OK);
    CHECK(brain_workflow_start(&eng, &reg, 0, 0, 1u) == BRAIN_OK);

    for (uint32_t i = 0; i < BARRY_TASK_QUEUE_CAPACITY; i++) {
        CHECK(barry_task_enqueue(&queue, 9u, 0u, NULL, 0) == BARRY_OK);
    }
    CHECK(brain_workflow_advance(&eng, &reg, &queue, 0) == BRAIN_ERR_FULL);
    CHECK(eng.instances[0].current_step == 0u); /* not advanced */
    CHECK(eng.instances[0].run_state == BRAIN_WF_RUNNING);
}

/* ---------------------------------------------------------------- */
/* Part E: cron timing wheel                                          */
/* ---------------------------------------------------------------- */

static void test_cron_fires_exactly_at_due_cycle(void) {
    SECTION("brain_cron_schedule/tick: fires exactly at due_cycle, never before, never after");
    brain_cron_wheel_t wheel;
    brain_cron_init(&wheel);

    CHECK(brain_cron_schedule(&wheel, 100u, 5u) == BRAIN_OK);
    CHECK(brain_cron_schedule(&wheel, 200u, 5u) == BRAIN_OK);
    CHECK(brain_cron_schedule(&wheel, 300u, 9u) == BRAIN_OK);

    uint32_t fired[BRAIN_CRON_SLOT_DEPTH];
    uint32_t count;

    for (uint64_t cycle = 0; cycle < 5u; cycle++) {
        CHECK(brain_cron_tick(&wheel, cycle, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
        CHECK(count == 0u); /* nothing due yet */
    }

    CHECK(brain_cron_tick(&wheel, 5u, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
    CHECK(count == 2u);
    CHECK((fired[0] == 100u && fired[1] == 200u) || (fired[0] == 200u && fired[1] == 100u));

    CHECK(brain_cron_tick(&wheel, 5u, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
    CHECK(count == 0u); /* already fired, not re-fired */

    for (uint64_t cycle = 6; cycle < 9u; cycle++) {
        CHECK(brain_cron_tick(&wheel, cycle, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
        CHECK(count == 0u);
    }
    CHECK(brain_cron_tick(&wheel, 9u, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
    CHECK(count == 1u);
    CHECK(fired[0] == 300u);
}

static void test_cron_same_slot_future_rotation_not_fired_early(void) {
    SECTION("brain_cron_schedule/tick: a job sharing a slot but due on a LATER wheel rotation never fires early");
    brain_cron_wheel_t wheel;
    brain_cron_init(&wheel);

    uint64_t near_due = 3u;
    uint64_t far_due   = 3u + (uint64_t)BRAIN_CRON_WHEEL_SLOTS; /* same slot index, later rotation */
    CHECK(brain_cron_schedule(&wheel, 1u, near_due) == BRAIN_OK);
    CHECK(brain_cron_schedule(&wheel, 2u, far_due) == BRAIN_OK);

    uint32_t fired[BRAIN_CRON_SLOT_DEPTH];
    uint32_t count;
    CHECK(brain_cron_tick(&wheel, near_due, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
    CHECK(count == 1u);
    CHECK(fired[0] == 1u); /* only the near one -- the far one is requeued, not fired */

    CHECK(brain_cron_tick(&wheel, far_due, fired, BRAIN_CRON_SLOT_DEPTH, &count) == BRAIN_OK);
    CHECK(count == 1u);
    CHECK(fired[0] == 2u);
}

static void test_cron_slot_bounded_depth(void) {
    SECTION("brain_cron_schedule: rejects past BRAIN_CRON_SLOT_DEPTH with BRAIN_ERR_FULL");
    brain_cron_wheel_t wheel;
    brain_cron_init(&wheel);

    int ok = 0;
    for (uint32_t i = 0; i < BRAIN_CRON_SLOT_DEPTH + 3u; i++) {
        brain_status_t st = brain_cron_schedule(&wheel, i, 42u);
        if (st == BRAIN_OK) {
            ok++;
        } else {
            CHECK(st == BRAIN_ERR_FULL);
        }
    }
    CHECK(ok == (int)BRAIN_CRON_SLOT_DEPTH);
}

/* ---------------------------------------------------------------- */
/* Part F: billing quotas                                             */
/* ---------------------------------------------------------------- */

static void test_billing_charge_and_quota(void) {
    SECTION("brain_billing_charge: quota enforced exactly, never a partial/over charge");
    brain_billing_ledger_t ledger;
    brain_billing_init(&ledger);
    CHECK(brain_billing_register_tenant(&ledger, 3, 100u, 1000u) == BRAIN_OK);

    CHECK(brain_billing_charge(&ledger, 3, 0u, 50u) == BRAIN_OK);
    CHECK(brain_billing_usage(&ledger, 3) == 50u);

    CHECK(brain_billing_charge(&ledger, 3, 0u, 60u) == BRAIN_ERR_QUOTA_EXCEEDED); /* 50+60 > 100 */
    CHECK(brain_billing_usage(&ledger, 3) == 50u); /* unchanged: never a partial charge */

    CHECK(brain_billing_charge(&ledger, 3, 0u, 50u) == BRAIN_OK);
    CHECK(brain_billing_usage(&ledger, 3) == 100u);
    CHECK(brain_billing_charge(&ledger, 3, 0u, 1u) == BRAIN_ERR_QUOTA_EXCEEDED);

    CHECK(brain_billing_charge(&ledger, 4, 0u, 1u) == BRAIN_ERR_NOT_FOUND); /* never registered */
    CHECK(brain_billing_usage(&ledger, (uint16_t)BRAIN_BILLING_MAX_TENANTS) == 0u);
}

static void test_billing_period_rollover(void) {
    SECTION("brain_billing_charge: lazy period rollover resets usage once current_cycle reaches period_end_cycle");
    brain_billing_ledger_t ledger;
    brain_billing_init(&ledger);
    CHECK(brain_billing_register_tenant(&ledger, 1, 10u, 100u) == BRAIN_OK);
    CHECK(brain_billing_charge(&ledger, 1, 0u, 10u) == BRAIN_OK);
    CHECK(brain_billing_charge(&ledger, 1, 50u, 1u) == BRAIN_ERR_QUOTA_EXCEEDED); /* still within period */

    CHECK(brain_billing_charge(&ledger, 1, 100u, 5u) == BRAIN_OK); /* rolled over: fresh quota */
    CHECK(brain_billing_usage(&ledger, 1) == 5u);
}

static _Atomic uint32_t g_billing_success_count = 0;

typedef struct {
    brain_billing_ledger_t *ledger;
    uint16_t                tenant_id;
    uint32_t                attempts;
} billing_thread_args_t;

static THREAD_FN(billing_charge_thread) {
    billing_thread_args_t *args = (billing_thread_args_t *)arg;
    for (uint32_t i = 0; i < args->attempts; i++) {
        if (brain_billing_charge(args->ledger, args->tenant_id, 0u, 1u) == BRAIN_OK) {
            atomic_fetch_add_explicit(&g_billing_success_count, 1u, memory_order_relaxed);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_billing_concurrent_charge_stress(void) {
    SECTION("8 threads charging the SAME tenant: usage lands exactly at quota, never exceeds it");
    brain_billing_ledger_t ledger;
    brain_billing_init(&ledger);
    const uint32_t quota = 1000u;
    CHECK(brain_billing_register_tenant(&ledger, 0, quota, 0xFFFFFFFFFFFFFFFFULL) == BRAIN_OK);
    atomic_store_explicit(&g_billing_success_count, 0u, memory_order_relaxed);

    billing_thread_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].ledger = &ledger;
        args[t].tenant_id = 0;
        args[t].attempts = 500u; /* 8*500 = 4000 attempts against a quota of 1000 */
        threads[t] = thread_start(billing_charge_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    CHECK(atomic_load_explicit(&g_billing_success_count, memory_order_relaxed) == quota);
    CHECK(brain_billing_usage(&ledger, 0) == quota);
}

/* ---------------------------------------------------------------- */
/* Part G: business rule engine                                       */
/* ---------------------------------------------------------------- */

static void test_bre_programmatic_rules(void) {
    SECTION("brain_bre_load_rule/evaluate: every comparison operator, multi-rule firing, out_max truncation");
    brain_bre_engine_t eng;
    brain_bre_init(&eng);

    CHECK(brain_bre_load_rule(&eng, 0, 1u, BRAIN_BRE_OP_GTE, 100, 900u) == BRAIN_OK); /* usage >= 100 -> alert */
    CHECK(brain_bre_load_rule(&eng, 1, 1u, BRAIN_BRE_OP_GTE, 200, 901u) == BRAIN_OK); /* usage >= 200 -> suspend */
    CHECK(brain_bre_load_rule(&eng, 2, 2u, BRAIN_BRE_OP_EQ, 42, 902u) == BRAIN_OK);   /* different field, must not fire */

    uint32_t actions[4];
    uint32_t count = 0;

    CHECK(brain_bre_evaluate(&eng, 1u, 50, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 0u); /* below both thresholds */

    CHECK(brain_bre_evaluate(&eng, 1u, 150, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 1u && actions[0] == 900u);

    CHECK(brain_bre_evaluate(&eng, 1u, 250, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 2u && actions[0] == 900u && actions[1] == 901u);

    /* out_max smaller than the number of matches: truncated, not overflowed */
    CHECK(brain_bre_evaluate(&eng, 1u, 250, actions, 1, &count) == BRAIN_OK);
    CHECK(count == 1u && actions[0] == 900u);

    CHECK(brain_bre_evaluate(&eng, 2u, 42, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 1u && actions[0] == 902u);
}

static void test_bre_load_from_text_valid(void) {
    SECTION("brain_bre_load_from_text: minimal fixed-grammar loader, comments/blank lines ignored");
    brain_bre_engine_t eng;
    const char *text =
        "# billing alert thresholds\n"
        "\n"
        "1 >= 100 -> 900\n"
        "1 >= 200 -> 901\n"
        "2 == 42 -> 902\n"
        "3 != 0 -> 903\n"
        "4 < 10 -> 904\n"
        "5 <= 10 -> 905\n";

    CHECK(brain_bre_load_from_text(&eng, text, strlen(text)) == BRAIN_OK);
    CHECK(eng.rule_count == 6u);

    uint32_t actions[4];
    uint32_t count = 0;
    CHECK(brain_bre_evaluate(&eng, 1u, 250, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 2u);
    CHECK(brain_bre_evaluate(&eng, 4u, 5, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 1u && actions[0] == 904u);
    CHECK(brain_bre_evaluate(&eng, 5u, 10, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 1u && actions[0] == 905u);
}

static void test_bre_load_from_text_malformed_leaves_engine_clean(void) {
    SECTION("brain_bre_load_from_text: a malformed line rejects the whole load, no partial rule set");
    brain_bre_engine_t eng;
    const char *text = "1 >= 100 -> 900\nthis is not a rule\n2 == 1 -> 901\n";

    CHECK(brain_bre_load_from_text(&eng, text, strlen(text)) == BRAIN_ERR_INVALID);
    CHECK(eng.rule_count == 0u);

    uint32_t actions[4];
    uint32_t count = 123u;
    CHECK(brain_bre_evaluate(&eng, 1u, 250, actions, 4, &count) == BRAIN_OK);
    CHECK(count == 0u); /* nothing survived the failed parse */
}

static void test_bre_load_from_text_too_many_rules(void) {
    SECTION("brain_bre_load_from_text: more rule lines than BRAIN_BRE_MAX_RULES is BRAIN_ERR_FULL");
    char text[BRAIN_BRE_MAX_RULES * 24u + 64u];
    size_t off = 0;
    for (uint32_t i = 0; i < BRAIN_BRE_MAX_RULES + 2u; i++) {
        int n = snprintf(text + off, sizeof(text) - off, "%u == 1 -> %u\n", i, i);
        off += (size_t)n;
    }
    brain_bre_engine_t eng;
    CHECK(brain_bre_load_from_text(&eng, text, off) == BRAIN_ERR_FULL);
    CHECK(eng.rule_count == 0u);
}

/* ---------------------------------------------------------------- */

int main(void) {
    test_iam_authenticate();
    test_iam_revoke_zeroes_secret();

    test_rate_limit_basic();
    test_rate_limit_refill_over_ticks();
    test_rate_limit_concurrent_stress();

    test_vault_store_reveal_revoke();

    test_workflow_advance_through_completion();
    test_workflow_start_unknown_def_rejected();
    test_workflow_backpressure_never_skips_step();

    test_cron_fires_exactly_at_due_cycle();
    test_cron_same_slot_future_rotation_not_fired_early();
    test_cron_slot_bounded_depth();

    test_billing_charge_and_quota();
    test_billing_period_rollover();
    test_billing_concurrent_charge_stress();

    test_bre_programmatic_rules();
    test_bre_load_from_text_valid();
    test_bre_load_from_text_malformed_leaves_engine_clean();
    test_bre_load_from_text_too_many_rules();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
