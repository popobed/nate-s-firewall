#include "agent_runner.h"

#include <string.h>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <time.h>
#endif

/* Real monotonic clock, milliseconds -- used ONLY to measure a real
 * execution-time budget (see file header for why this is a different
 * concern from swarm_tick/barry_ts.h's "no wall clock" business-time
 * posture). */
static uint64_t agent_now_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

bool agent_deadline_exceeded(const agent_deadline_t *deadline) {
    if (deadline == NULL) {
        return false;
    }
    return agent_now_ms() >= deadline->deadline_ms;
}

void agent_runner_init(agent_runner_t *runner) {
    if (runner == NULL) {
        return;
    }
    memset(runner, 0, sizeof *runner);
}

agent_run_status_t agent_runner_register(agent_runner_t *runner, const char *name, uint32_t name_len,
                                          agent_command_handler_fn handler, void *ctx) {
    if (runner == NULL || name == NULL || handler == NULL ||
        name_len == 0u || name_len > AGENT_CMD_NAME_MAX) {
        return AGENT_RUN_ERR_INVALID;
    }

    for (uint32_t i = 0; i < runner->command_count; i++) {
        agent_command_slot_t *slot = &runner->commands[i];
        if (slot->name_len == name_len && memcmp(slot->name, name, name_len) == 0) {
            return AGENT_RUN_ERR_ALREADY_REGISTERED;
        }
    }

    if (runner->command_count >= AGENT_RUNNER_MAX_COMMANDS) {
        return AGENT_RUN_ERR_FULL;
    }

    agent_command_slot_t *slot = &runner->commands[runner->command_count];
    memset(slot->name, 0, sizeof slot->name);
    memcpy(slot->name, name, name_len);
    slot->name_len = name_len;
    slot->handler = handler;
    slot->ctx = ctx;
    slot->registered = true;
    runner->command_count++;

    return AGENT_RUN_OK;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t';
}

/* Splits `payload[0..len)` into a leading command token and the
 * remainder (leading whitespace after the token skipped). Never fails --
 * an all-whitespace or empty payload yields an empty token. */
static void split_command(const char *payload, uint32_t len,
                           const char **out_name, uint32_t *out_name_len,
                           const char **out_arg, uint32_t *out_arg_len) {
    uint32_t i = 0;
    while (i < len && !is_ws(payload[i])) {
        i++;
    }
    *out_name = payload;
    *out_name_len = i;

    uint32_t j = i;
    while (j < len && is_ws(payload[j])) {
        j++;
    }
    *out_arg = payload + j;
    *out_arg_len = len - j;
}

static const agent_command_slot_t *find_command(const agent_runner_t *runner,
                                                  const char *name, uint32_t name_len) {
    for (uint32_t i = 0; i < runner->command_count; i++) {
        const agent_command_slot_t *slot = &runner->commands[i];
        if (slot->name_len == name_len && memcmp(slot->name, name, name_len) == 0) {
            return slot;
        }
    }
    return NULL;
}

static void audit(tf_log_t *audit_log, agent_run_status_t status, uint32_t matched_rule_id,
                   uint32_t payload_len, const char *command_name, uint32_t command_name_len) {
    if (audit_log == NULL) {
        return;
    }
    agent_run_tf_audit_payload_t p;
    memset(&p, 0, sizeof p);
    p.status = (int16_t)status;
    p.matched_rule_id = matched_rule_id;
    p.payload_len = payload_len;
    if (command_name != NULL) {
        uint32_t copy_len = command_name_len < AGENT_CMD_NAME_MAX ? command_name_len : AGENT_CMD_NAME_MAX;
        memcpy(p.command_name, command_name, copy_len);
    }
    (void)tf_log_append(audit_log, AGENT_RUN_TF_EVENT, &p, (uint32_t)sizeof p);
}

agent_run_status_t agent_runner_execute(agent_runner_t *runner, const char *payload, uint32_t len,
                                         uint32_t timeout_ms, tf_log_t *audit_log) {
    if (runner == NULL || (payload == NULL && len != 0u) || timeout_ms == 0u) {
        return AGENT_RUN_ERR_INVALID;
    }

    agent_inspect_result_t inspect_result;
    agent_inspect_status_t inspect_st = agent_inspect_instruction(payload, len, &inspect_result);
    if (inspect_st != AGENT_INSPECT_OK) {
        audit(audit_log, AGENT_RUN_ERR_INVALID, 0u, len, NULL, 0u);
        return AGENT_RUN_ERR_INVALID;
    }
    if (inspect_result.verdict == AGENT_VERDICT_BLOCKED) {
        audit(audit_log, AGENT_RUN_ERR_BLOCKED, inspect_result.matched_rule_id, len, NULL, 0u);
        return AGENT_RUN_ERR_BLOCKED;
    }

    const char *name;
    uint32_t name_len;
    const char *arg;
    uint32_t arg_len;
    split_command(payload, len, &name, &name_len, &arg, &arg_len);

    const agent_command_slot_t *slot = find_command(runner, name, name_len);
    if (slot == NULL) {
        audit(audit_log, AGENT_RUN_ERR_UNKNOWN_COMMAND, 0u, len, name, name_len);
        return AGENT_RUN_ERR_UNKNOWN_COMMAND;
    }

    agent_deadline_t deadline;
    uint64_t start = agent_now_ms();
    deadline.deadline_ms = start + (uint64_t)timeout_ms;

    slot->handler(slot->ctx, arg, arg_len, &deadline);

    uint64_t elapsed = agent_now_ms() - start;
    agent_run_status_t result = (elapsed > (uint64_t)timeout_ms) ? AGENT_RUN_ERR_TIMEOUT : AGENT_RUN_OK;
    audit(audit_log, result, 0u, len, name, name_len);
    return result;
}
