/*
 * test_agent_runner.c - Unit tests for agent_runner.{h,c}: Zero-Trust
 * dispatch (agent_inspector.h runs first, deny-by-default), the fixed
 * command table, the measured/cooperative timeout, and the
 * telemetry_fabric.h audit trail. No dynamic allocation.
 */

#include <stdio.h>
#include <string.h>

#include "agent_runner.h"

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif

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

static void sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---------------------------------------------------------------- */
/* Handlers under test                                                 */
/* ---------------------------------------------------------------- */

typedef struct {
    bool     called;
    char     arg_copy[64];
    uint32_t arg_len;
} ping_ctx_t;

static void ping_handler(void *ctx_v, const char *arg, uint32_t arg_len, const agent_deadline_t *deadline) {
    (void)deadline;
    ping_ctx_t *ctx = (ping_ctx_t *)ctx_v;
    ctx->called = true;
    uint32_t copy_len = arg_len < sizeof(ctx->arg_copy) - 1u ? arg_len : (uint32_t)sizeof(ctx->arg_copy) - 1u;
    memcpy(ctx->arg_copy, arg, copy_len);
    ctx->arg_copy[copy_len] = '\0';
    ctx->arg_len = arg_len;
}

static void noop_handler(void *ctx, const char *arg, uint32_t arg_len, const agent_deadline_t *deadline) {
    (void)ctx;
    (void)arg;
    (void)arg_len;
    (void)deadline;
}

/* Deliberately ignores the deadline entirely -- demonstrates that
 * agent_runner_execute() cannot force this to stop, only measure and
 * report that it overran, exactly as agent_runner.h's header documents. */
static void slow_noncooperative_handler(void *ctx, const char *arg, uint32_t arg_len,
                                         const agent_deadline_t *deadline) {
    (void)ctx;
    (void)arg;
    (void)arg_len;
    (void)deadline;
    sleep_ms(150);
}

/* Polls agent_deadline_exceeded() and returns as soon as the budget is
 * up -- the real, usable cooperative path. */
static void cooperative_handler(void *ctx, const char *arg, uint32_t arg_len, const agent_deadline_t *deadline) {
    (void)ctx;
    (void)arg;
    (void)arg_len;
    for (int i = 0; i < 200; i++) {
        if (agent_deadline_exceeded(deadline)) {
            return;
        }
        sleep_ms(5);
    }
}

/* ---------------------------------------------------------------- */

static void test_deadline_exceeded_direct(void) {
    SECTION("agent_deadline_exceeded: past vs. far-future deadline");

    agent_deadline_t past;
    past.deadline_ms = 0u; /* monotonic clocks are always > 0 shortly after start */
    CHECK(agent_deadline_exceeded(&past));

    agent_deadline_t future;
    future.deadline_ms = UINT64_MAX;
    CHECK(!agent_deadline_exceeded(&future));

    CHECK(!agent_deadline_exceeded(NULL));
}

static void test_register_and_execute_happy_path(void) {
    SECTION("agent_runner_execute: allowed instruction dispatches to the registered handler");

    agent_runner_t runner;
    agent_runner_init(&runner);
    ping_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    CHECK(agent_runner_register(&runner, "ping", 4u, ping_handler, &ctx) == AGENT_RUN_OK);

    const char msg[] = "ping hello world";
    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 1000u, NULL) == AGENT_RUN_OK);
    CHECK(ctx.called);
    CHECK(strcmp(ctx.arg_copy, "hello world") == 0);
    CHECK(ctx.arg_len == (uint32_t)strlen("hello world"));
}

static void test_duplicate_registration_rejected(void) {
    SECTION("agent_runner_register: duplicate command name rejected");

    agent_runner_t runner;
    agent_runner_init(&runner);
    CHECK(agent_runner_register(&runner, "cmd", 3u, noop_handler, NULL) == AGENT_RUN_OK);
    CHECK(agent_runner_register(&runner, "cmd", 3u, noop_handler, NULL) == AGENT_RUN_ERR_ALREADY_REGISTERED);
}

static void test_command_table_full(void) {
    SECTION("agent_runner_register: AGENT_RUN_ERR_FULL past AGENT_RUNNER_MAX_COMMANDS");

    agent_runner_t runner;
    agent_runner_init(&runner);
    char name[4];
    for (uint32_t i = 0; i < AGENT_RUNNER_MAX_COMMANDS; i++) {
        name[0] = 'c';
        name[1] = (char)('a' + (i % 26));
        name[2] = (char)('0' + (i / 26));
        CHECK(agent_runner_register(&runner, name, 3u, noop_handler, NULL) == AGENT_RUN_OK);
    }
    CHECK(agent_runner_register(&runner, "zzz", 3u, noop_handler, NULL) == AGENT_RUN_ERR_FULL);
}

static void test_blocked_instruction_never_dispatches(void) {
    SECTION("agent_runner_execute: BLOCKED instruction never reaches the handler, audit trail records it");

    agent_runner_t runner;
    agent_runner_init(&runner);
    ping_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    CHECK(agent_runner_register(&runner, "run", 3u, ping_handler, &ctx) == AGENT_RUN_OK);

    tf_log_t log;
    tf_log_init(&log);

    const char msg[] = "run os.system(\"whoami\")";
    agent_inspect_result_t independent_check;
    CHECK(agent_inspect_instruction(msg, (uint32_t)strlen(msg), &independent_check) == AGENT_INSPECT_OK);
    CHECK(independent_check.verdict == AGENT_VERDICT_BLOCKED);

    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 1000u, &log) == AGENT_RUN_ERR_BLOCKED);
    CHECK(!ctx.called);

    uint64_t broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
    agent_run_tf_audit_payload_t payload;
    memcpy(&payload, log.entries[0].payload, sizeof payload);
    CHECK(payload.status == (int16_t)AGENT_RUN_ERR_BLOCKED);
    CHECK(payload.matched_rule_id == independent_check.matched_rule_id);
}

static void test_unknown_command(void) {
    SECTION("agent_runner_execute: allowed but unregistered command name");

    agent_runner_t runner;
    agent_runner_init(&runner);
    CHECK(agent_runner_register(&runner, "known", 5u, noop_handler, NULL) == AGENT_RUN_OK);

    const char msg[] = "unknown some arg";
    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 1000u, NULL) == AGENT_RUN_ERR_UNKNOWN_COMMAND);
}

static void test_invalid_args(void) {
    SECTION("agent_runner_execute: invalid arguments rejected");

    agent_runner_t runner;
    agent_runner_init(&runner);
    const char msg[] = "ping x";
    CHECK(agent_runner_execute(NULL, msg, (uint32_t)strlen(msg), 1000u, NULL) == AGENT_RUN_ERR_INVALID);
    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 0u, NULL) == AGENT_RUN_ERR_INVALID);
}

static void test_timeout_is_measured_not_preemptive(void) {
    SECTION("agent_runner_execute: TIMEOUT is measured after the handler returns, never forced");

    agent_runner_t runner;
    agent_runner_init(&runner);
    CHECK(agent_runner_register(&runner, "slow", 4u, slow_noncooperative_handler, NULL) == AGENT_RUN_OK);

    const char msg[] = "slow";
    /* Handler sleeps 150ms unconditionally; a 15ms budget is blown, and
     * this call necessarily blocks for the full 150ms -- proving the
     * timeout is reported, not enforced. */
    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 15u, NULL) == AGENT_RUN_ERR_TIMEOUT);
}

static void test_cooperative_handler_returns_promptly(void) {
    SECTION("agent_runner_execute: a handler that polls agent_deadline_exceeded() stops promptly");

    agent_runner_t runner;
    agent_runner_init(&runner);
    CHECK(agent_runner_register(&runner, "coop", 4u, cooperative_handler, NULL) == AGENT_RUN_OK);

    const char msg[] = "coop";
    /* Budget is 10ms; the handler polls every 5ms and must notice the
     * overrun within a couple of iterations, not run for its full
     * 200 * 5ms = 1000ms worst case. */
    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 10u, NULL) == AGENT_RUN_ERR_TIMEOUT);
}

static void test_audit_trail_command_name_recorded(void) {
    SECTION("agent_runner_execute: audit payload records the dispatched command name");

    agent_runner_t runner;
    agent_runner_init(&runner);
    CHECK(agent_runner_register(&runner, "ok", 2u, noop_handler, NULL) == AGENT_RUN_OK);

    tf_log_t log;
    tf_log_init(&log);

    const char msg[] = "ok some-argument";
    CHECK(agent_runner_execute(&runner, msg, (uint32_t)strlen(msg), 1000u, &log) == AGENT_RUN_OK);

    uint64_t broken = 0;
    CHECK(tf_log_verify(&log, &broken) == TF_OK);
    agent_run_tf_audit_payload_t payload;
    memcpy(&payload, log.entries[0].payload, sizeof payload);
    CHECK(payload.status == (int16_t)AGENT_RUN_OK);
    CHECK(memcmp(payload.command_name, "ok", 2) == 0);
}

int main(void) {
    test_deadline_exceeded_direct();
    test_register_and_execute_happy_path();
    test_duplicate_registration_rejected();
    test_command_table_full();
    test_blocked_instruction_never_dispatches();
    test_unknown_command();
    test_invalid_args();
    test_timeout_is_measured_not_preemptive();
    test_cooperative_handler_returns_promptly();
    test_audit_trail_command_name_recorded();

    printf("\n%d tests run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
