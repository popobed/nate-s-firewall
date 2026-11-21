/*
 * brain_core.c
 *
 * See include/brain_core.h for the full design and honest-scope notes.
 * Implementation only; non-obvious decisions are explained there, not
 * duplicated here.
 */

#include "brain_core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ==================================================================== */
/* Part A -- IAM                                                         */
/* ==================================================================== */

void brain_iam_init(brain_iam_registry_t *reg) {
    for (uint32_t i = 0; i < BRAIN_IAM_MAX_TOKENS; i++) {
        memset(reg->tokens[i].secret, 0, sizeof reg->tokens[i].secret);
        reg->tokens[i].scope_mask = 0u;
        reg->tokens[i].tenant_id = 0u;
        reg->tokens[i].active = false;
    }
}

brain_status_t brain_iam_register_token(brain_iam_registry_t *reg, uint16_t token_id, uint16_t tenant_id,
                                         const uint8_t secret[BRAIN_IAM_SECRET_LEN], uint32_t scope_mask) {
    if (reg == NULL || secret == NULL || token_id >= BRAIN_IAM_MAX_TOKENS) {
        return BRAIN_ERR_INVALID;
    }
    if (reg->tokens[token_id].active) {
        return BRAIN_ERR_ALREADY_REGISTERED;
    }
    memcpy(reg->tokens[token_id].secret, secret, BRAIN_IAM_SECRET_LEN);
    reg->tokens[token_id].scope_mask = scope_mask;
    reg->tokens[token_id].tenant_id = tenant_id;
    reg->tokens[token_id].active = true;
    return BRAIN_OK;
}

brain_status_t brain_iam_revoke_token(brain_iam_registry_t *reg, uint16_t token_id) {
    if (reg == NULL || token_id >= BRAIN_IAM_MAX_TOKENS) {
        return BRAIN_ERR_INVALID;
    }
    if (!reg->tokens[token_id].active) {
        return BRAIN_ERR_NOT_FOUND;
    }
    memset(reg->tokens[token_id].secret, 0, sizeof reg->tokens[token_id].secret);
    reg->tokens[token_id].scope_mask = 0u;
    reg->tokens[token_id].tenant_id = 0u;
    reg->tokens[token_id].active = false;
    return BRAIN_OK;
}

brain_status_t brain_iam_authenticate(const brain_iam_registry_t *reg, uint16_t token_id,
                                       const uint8_t *provided_secret, size_t provided_len,
                                       uint32_t required_scope_mask, uint16_t *out_tenant_id) {
    if (reg == NULL || out_tenant_id == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (token_id >= BRAIN_IAM_MAX_TOKENS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    const brain_iam_token_t *tok = &reg->tokens[token_id];
    if (!tok->active) {
        return BRAIN_ERR_NOT_FOUND;
    }
    if (!ct_verify_token(provided_secret, provided_len, tok->secret, BRAIN_IAM_SECRET_LEN)) {
        return BRAIN_ERR_UNAUTHORIZED;
    }
    if ((tok->scope_mask & required_scope_mask) != required_scope_mask) {
        return BRAIN_ERR_UNAUTHORIZED;
    }
    *out_tenant_id = tok->tenant_id;
    return BRAIN_OK;
}

/* ==================================================================== */
/* Part B -- Token-bucket rate limiting                                  */
/* ==================================================================== */

void brain_rate_init(brain_rate_limiter_t *rl) {
    for (uint32_t i = 0; i < BRAIN_RATE_MAX_BUCKETS; i++) {
        atomic_store_explicit(&rl->buckets[i].tokens, 0u, memory_order_relaxed);
        atomic_store_explicit(&rl->buckets[i].last_refill_tick, 0u, memory_order_relaxed);
        rl->buckets[i].capacity = 0u;
        rl->buckets[i].refill_per_tick = 0u;
    }
}

brain_status_t brain_rate_configure(brain_rate_limiter_t *rl, uint16_t bucket_id,
                                     uint32_t capacity, uint32_t refill_per_tick) {
    if (rl == NULL || bucket_id >= BRAIN_RATE_MAX_BUCKETS || capacity == 0u) {
        return BRAIN_ERR_INVALID;
    }
    rl->buckets[bucket_id].capacity = capacity;
    rl->buckets[bucket_id].refill_per_tick = refill_per_tick;
    atomic_store_explicit(&rl->buckets[bucket_id].tokens, capacity, memory_order_relaxed);
    atomic_store_explicit(&rl->buckets[bucket_id].last_refill_tick, 0u, memory_order_relaxed);
    return BRAIN_OK;
}

brain_status_t brain_rate_try_consume(brain_rate_limiter_t *rl, uint16_t bucket_id,
                                       uint64_t current_tick, uint32_t cost) {
    if (rl == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (bucket_id >= BRAIN_RATE_MAX_BUCKETS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    brain_rate_bucket_t *bucket = &rl->buckets[bucket_id];
    if (bucket->capacity == 0u) {
        return BRAIN_ERR_NOT_FOUND; /* never configured */
    }

    uint64_t last = atomic_load_explicit(&bucket->last_refill_tick, memory_order_acquire);
    if (current_tick > last) {
        /* Only the thread that wins this CAS credits the elapsed window
         * -- prevents concurrent callers from double-crediting the same
         * ticks (see brain_rate_try_consume()'s header comment). */
        if (atomic_compare_exchange_strong_explicit(&bucket->last_refill_tick, &last, current_tick,
                                                      memory_order_acq_rel, memory_order_acquire)) {
            uint64_t elapsed = current_tick - last;
            uint64_t add64 = 0u;
            if (bucket->refill_per_tick != 0u && elapsed != 0u) {
                if (elapsed > UINT64_MAX / (uint64_t)bucket->refill_per_tick) {
                    add64 = (uint64_t)bucket->capacity; /* overflow-safe saturating clamp */
                } else {
                    add64 = elapsed * (uint64_t)bucket->refill_per_tick;
                }
            }
            uint32_t add = add64 > (uint64_t)bucket->capacity ? bucket->capacity : (uint32_t)add64;

            uint32_t old_tok = atomic_load_explicit(&bucket->tokens, memory_order_relaxed);
            uint32_t new_tok;
            do {
                uint64_t sum = (uint64_t)old_tok + add;
                new_tok = sum > (uint64_t)bucket->capacity ? bucket->capacity : (uint32_t)sum;
            } while (!atomic_compare_exchange_weak_explicit(&bucket->tokens, &old_tok, new_tok,
                                                              memory_order_relaxed, memory_order_relaxed));
        }
        /* CAS loss: another thread already advanced last_refill_tick and
         * credited this window -- correct to skip, not a lost refill. */
    }

    uint32_t cur = atomic_load_explicit(&bucket->tokens, memory_order_acquire);
    for (;;) {
        if (cur < cost) {
            return BRAIN_ERR_RATE_LIMITED; /* O(1) instant rejection, never a wait */
        }
        uint32_t next = cur - cost;
        if (atomic_compare_exchange_weak_explicit(&bucket->tokens, &cur, next,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            return BRAIN_OK;
        }
    }
}

/* ==================================================================== */
/* Part C -- Secrets vault                                               */
/* ==================================================================== */

void brain_vault_init(brain_vault_t *vault) {
    for (uint32_t i = 0; i < BRAIN_VAULT_MAX_SECRETS; i++) {
        memset(vault->entries[i].value, 0, sizeof vault->entries[i].value);
        vault->entries[i].len = 0u;
        vault->entries[i].present = false;
    }
}

brain_status_t brain_vault_store(brain_vault_t *vault, uint32_t secret_id, const void *value, uint32_t len) {
    if (vault == NULL || (len > 0u && value == NULL) || len > BRAIN_VAULT_SECRET_MAX) {
        return BRAIN_ERR_INVALID;
    }
    if (secret_id >= BRAIN_VAULT_MAX_SECRETS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    brain_vault_entry_t *e = &vault->entries[secret_id];
    if (len > 0u) {
        memcpy(e->value, value, len);
    }
    if (len < BRAIN_VAULT_SECRET_MAX) {
        memset(e->value + len, 0, BRAIN_VAULT_SECRET_MAX - len);
    }
    e->len = len;
    e->present = true;
    return BRAIN_OK;
}

brain_status_t brain_vault_reveal(const brain_vault_t *vault, uint32_t secret_id, void *out_value, uint32_t *inout_len) {
    if (vault == NULL || inout_len == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (secret_id >= BRAIN_VAULT_MAX_SECRETS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    const brain_vault_entry_t *e = &vault->entries[secret_id];
    if (!e->present) {
        return BRAIN_ERR_NOT_FOUND;
    }
    if (e->len > *inout_len) {
        return BRAIN_ERR_INVALID; /* caller's out_value buffer too small */
    }
    if (out_value != NULL && e->len > 0u) {
        memcpy(out_value, e->value, e->len);
    }
    *inout_len = e->len;
    return BRAIN_OK;
}

brain_status_t brain_vault_revoke(brain_vault_t *vault, uint32_t secret_id) {
    if (vault == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (secret_id >= BRAIN_VAULT_MAX_SECRETS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    brain_vault_entry_t *e = &vault->entries[secret_id];
    if (!e->present) {
        return BRAIN_ERR_NOT_FOUND;
    }
    memset(e->value, 0, sizeof e->value);
    e->len = 0u;
    e->present = false;
    return BRAIN_OK;
}

/* ==================================================================== */
/* Part D -- Workflow orchestrator                                       */
/* ==================================================================== */

void brain_workflow_registry_init(brain_workflow_registry_t *reg) {
    for (uint32_t i = 0; i < BRAIN_WORKFLOW_MAX_DEFS; i++) {
        memset(reg->defs[i].step_action_id, 0, sizeof reg->defs[i].step_action_id);
        reg->defs[i].step_count = 0u;
        reg->defs[i].defined = false;
    }
}

brain_status_t brain_workflow_define(brain_workflow_registry_t *reg, uint32_t workflow_def_id,
                                      const uint32_t *step_action_ids, uint32_t step_count) {
    if (reg == NULL || step_action_ids == NULL || step_count == 0u || step_count > BRAIN_WORKFLOW_MAX_STEPS) {
        return BRAIN_ERR_INVALID;
    }
    if (workflow_def_id >= BRAIN_WORKFLOW_MAX_DEFS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    brain_workflow_def_t *def = &reg->defs[workflow_def_id];
    memcpy(def->step_action_id, step_action_ids, (size_t)step_count * sizeof(uint32_t));
    if (step_count < BRAIN_WORKFLOW_MAX_STEPS) {
        memset(&def->step_action_id[step_count], 0, (size_t)(BRAIN_WORKFLOW_MAX_STEPS - step_count) * sizeof(uint32_t));
    }
    def->step_count = step_count;
    def->defined = true;
    return BRAIN_OK;
}

void brain_workflow_engine_init(brain_workflow_engine_t *eng) {
    for (uint32_t i = 0; i < BRAIN_WORKFLOW_MAX_INSTANCES; i++) {
        eng->instances[i].workflow_def_id = 0u;
        eng->instances[i].current_step = 0u;
        eng->instances[i].tenant_id = 0u;
        eng->instances[i].run_state = BRAIN_WF_RUNNING;
        eng->instances[i].active = false;
    }
}

brain_status_t brain_workflow_start(brain_workflow_engine_t *eng, const brain_workflow_registry_t *reg,
                                     uint32_t instance_id, uint32_t workflow_def_id, uint16_t tenant_id) {
    if (eng == NULL || reg == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (instance_id >= BRAIN_WORKFLOW_MAX_INSTANCES) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    if (workflow_def_id >= BRAIN_WORKFLOW_MAX_DEFS || !reg->defs[workflow_def_id].defined) {
        return BRAIN_ERR_NOT_FOUND;
    }
    brain_workflow_instance_t *inst = &eng->instances[instance_id];
    inst->workflow_def_id = workflow_def_id;
    inst->current_step = 0u;
    inst->tenant_id = tenant_id;
    inst->run_state = BRAIN_WF_RUNNING;
    inst->active = true;
    return BRAIN_OK;
}

brain_status_t brain_workflow_advance(brain_workflow_engine_t *eng, const brain_workflow_registry_t *reg,
                                       barry_task_queue_t *queue, uint32_t instance_id) {
    if (eng == NULL || reg == NULL || queue == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (instance_id >= BRAIN_WORKFLOW_MAX_INSTANCES) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    brain_workflow_instance_t *inst = &eng->instances[instance_id];
    if (!inst->active || inst->run_state != BRAIN_WF_RUNNING) {
        return BRAIN_ERR_INACTIVE;
    }
    const brain_workflow_def_t *def = &reg->defs[inst->workflow_def_id];
    if (!def->defined || inst->current_step >= def->step_count) {
        inst->run_state = BRAIN_WF_COMPLETED; /* defensive: should not happen via the public API alone */
        return BRAIN_ERR_INACTIVE;
    }

    uint32_t action_id = def->step_action_id[inst->current_step];
    barry_status_t bst = barry_task_enqueue(queue, instance_id, BRAIN_TASK_TYPE_WORKFLOW_STEP,
                                             &action_id, (uint32_t)sizeof action_id);
    if (bst == BARRY_ERR_FULL) {
        return BRAIN_ERR_FULL; /* backpressure: current_step NOT advanced, this step never silently skipped */
    }
    if (bst != BARRY_OK) {
        return BRAIN_ERR_INVALID;
    }

    inst->current_step++;
    if (inst->current_step >= def->step_count) {
        inst->run_state = BRAIN_WF_COMPLETED;
    }
    return BRAIN_OK;
}

/* ==================================================================== */
/* Part E -- Cron timing wheel                                           */
/* ==================================================================== */

void brain_cron_init(brain_cron_wheel_t *wheel) {
    for (uint32_t s = 0; s < BRAIN_CRON_WHEEL_SLOTS; s++) {
        memset(wheel->slots[s].job_id, 0, sizeof wheel->slots[s].job_id);
        memset(wheel->slots[s].due_cycle, 0, sizeof wheel->slots[s].due_cycle);
        atomic_store_explicit(&wheel->slots[s].count, 0u, memory_order_relaxed);
    }
}

brain_status_t brain_cron_schedule(brain_cron_wheel_t *wheel, uint32_t job_id, uint64_t due_cycle) {
    if (wheel == NULL) {
        return BRAIN_ERR_INVALID;
    }
    uint32_t idx = (uint32_t)(due_cycle & (uint64_t)(BRAIN_CRON_WHEEL_SLOTS - 1u));
    brain_cron_slot_t *slot = &wheel->slots[idx];

    uint32_t pos = atomic_fetch_add_explicit(&slot->count, 1u, memory_order_acq_rel);
    if (pos >= BRAIN_CRON_SLOT_DEPTH) {
        atomic_fetch_sub_explicit(&slot->count, 1u, memory_order_acq_rel);
        return BRAIN_ERR_FULL;
    }
    slot->job_id[pos] = job_id;
    slot->due_cycle[pos] = due_cycle;
    return BRAIN_OK;
}

brain_status_t brain_cron_tick(brain_cron_wheel_t *wheel, uint64_t current_cycle,
                                uint32_t *out_fired_job_ids, uint32_t out_max, uint32_t *out_count) {
    if (wheel == NULL || out_count == NULL) {
        return BRAIN_ERR_INVALID;
    }
    uint32_t idx = (uint32_t)(current_cycle & (uint64_t)(BRAIN_CRON_WHEEL_SLOTS - 1u));
    brain_cron_slot_t *slot = &wheel->slots[idx];

    uint32_t count = atomic_load_explicit(&slot->count, memory_order_acquire);
    if (count > BRAIN_CRON_SLOT_DEPTH) {
        count = BRAIN_CRON_SLOT_DEPTH; /* defensive clamp on a stale/racy read, same posture as barry_index_lookup() */
    }

    uint32_t fired = 0u;
    uint32_t requeue_job[BRAIN_CRON_SLOT_DEPTH];
    uint64_t requeue_due[BRAIN_CRON_SLOT_DEPTH];
    uint32_t requeue_count = 0u;

    for (uint32_t i = 0; i < count; i++) {
        if (slot->due_cycle[i] == current_cycle) {
            if (fired < out_max && out_fired_job_ids != NULL) {
                out_fired_job_ids[fired] = slot->job_id[i];
            }
            fired++;
        } else {
            requeue_job[requeue_count] = slot->job_id[i];
            requeue_due[requeue_count] = slot->due_cycle[i];
            requeue_count++;
        }
    }

    for (uint32_t i = 0; i < requeue_count; i++) {
        slot->job_id[i] = requeue_job[i];
        slot->due_cycle[i] = requeue_due[i];
    }
    for (uint32_t i = requeue_count; i < count; i++) {
        slot->job_id[i] = 0u;
        slot->due_cycle[i] = 0u;
    }
    atomic_store_explicit(&slot->count, requeue_count, memory_order_release);

    *out_count = fired < out_max ? fired : out_max;
    return BRAIN_OK;
}

/* ==================================================================== */
/* Part F -- Billing quotas                                              */
/* ==================================================================== */

void brain_billing_init(brain_billing_ledger_t *ledger) {
    for (uint32_t i = 0; i < BRAIN_BILLING_MAX_TENANTS; i++) {
        atomic_store_explicit(&ledger->usage_count[i], 0u, memory_order_relaxed);
        ledger->quota_limit[i] = 0u;
        ledger->period_end_cycle[i] = 0u;
        ledger->registered[i] = false;
    }
}

brain_status_t brain_billing_register_tenant(brain_billing_ledger_t *ledger, uint16_t tenant_id,
                                              uint32_t quota_limit, uint64_t period_end_cycle) {
    if (ledger == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (tenant_id >= BRAIN_BILLING_MAX_TENANTS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    ledger->quota_limit[tenant_id] = quota_limit;
    ledger->period_end_cycle[tenant_id] = period_end_cycle;
    atomic_store_explicit(&ledger->usage_count[tenant_id], 0u, memory_order_relaxed);
    ledger->registered[tenant_id] = true;
    return BRAIN_OK;
}

brain_status_t brain_billing_charge(brain_billing_ledger_t *ledger, uint16_t tenant_id,
                                     uint64_t current_cycle, uint32_t amount) {
    if (ledger == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (tenant_id >= BRAIN_BILLING_MAX_TENANTS) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    if (!ledger->registered[tenant_id]) {
        return BRAIN_ERR_NOT_FOUND;
    }
    if (current_cycle >= ledger->period_end_cycle[tenant_id]) {
        atomic_store_explicit(&ledger->usage_count[tenant_id], 0u, memory_order_relaxed); /* lazy, caller-driven period rollover */
    }

    uint32_t limit = ledger->quota_limit[tenant_id];
    uint32_t old = atomic_load_explicit(&ledger->usage_count[tenant_id], memory_order_acquire);
    for (;;) {
        uint64_t sum = (uint64_t)old + amount;
        if (sum > (uint64_t)limit) {
            return BRAIN_ERR_QUOTA_EXCEEDED; /* O(1) instant rejection, never a partial charge */
        }
        uint32_t new_val = (uint32_t)sum;
        if (atomic_compare_exchange_weak_explicit(&ledger->usage_count[tenant_id], &old, new_val,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            return BRAIN_OK;
        }
    }
}

uint32_t brain_billing_usage(const brain_billing_ledger_t *ledger, uint16_t tenant_id) {
    if (ledger == NULL || tenant_id >= BRAIN_BILLING_MAX_TENANTS) {
        return 0u; /* documented: an invalid tenant_id reads as zero usage, not a distinct error -- this is a plain accessor, not a validating call */
    }
    return atomic_load_explicit(&ledger->usage_count[tenant_id], memory_order_acquire);
}

#define BRAIN_TF_EVENT_BILLING_CHARGE 0xB4A20001u

typedef struct {
    uint16_t tenant_id;
    int16_t  status; /* brain_status_t, narrowed -- every value in that enum fits int16_t */
    uint32_t amount;
} brain_tf_audit_payload_t;

brain_status_t brain_billing_charge_audited(brain_billing_ledger_t *ledger, uint16_t tenant_id,
                                             uint64_t current_cycle, uint32_t amount, tf_log_t *audit_log) {
    brain_status_t st = brain_billing_charge(ledger, tenant_id, current_cycle, amount);

    if (audit_log != NULL) {
        brain_tf_audit_payload_t payload;
        payload.tenant_id = tenant_id;
        payload.status = (int16_t)st;
        payload.amount = amount;
        (void)tf_log_append(audit_log, BRAIN_TF_EVENT_BILLING_CHARGE, &payload, (uint32_t)sizeof payload);
    }
    return st;
}

/* ==================================================================== */
/* Part G -- Business Rule Engine                                        */
/* ==================================================================== */

void brain_bre_init(brain_bre_engine_t *eng) {
    for (uint32_t i = 0; i < BRAIN_BRE_MAX_RULES; i++) {
        eng->rules[i].field_id = 0u;
        eng->rules[i].op = BRAIN_BRE_OP_EQ;
        eng->rules[i].threshold = 0;
        eng->rules[i].action_id = 0u;
        eng->rules[i].active = false;
    }
    eng->rule_count = 0u;
}

brain_status_t brain_bre_load_rule(brain_bre_engine_t *eng, uint32_t index, uint32_t field_id,
                                    brain_bre_op_t op, int64_t threshold, uint32_t action_id) {
    if (eng == NULL) {
        return BRAIN_ERR_INVALID;
    }
    if (index >= BRAIN_BRE_MAX_RULES) {
        return BRAIN_ERR_OUT_OF_RANGE;
    }
    if ((unsigned)op > (unsigned)BRAIN_BRE_OP_GTE) {
        return BRAIN_ERR_INVALID;
    }
    eng->rules[index].field_id = field_id;
    eng->rules[index].op = op;
    eng->rules[index].threshold = threshold;
    eng->rules[index].action_id = action_id;
    eng->rules[index].active = true;
    if (index + 1u > eng->rule_count) {
        eng->rule_count = index + 1u;
    }
    return BRAIN_OK;
}

brain_status_t brain_bre_evaluate(const brain_bre_engine_t *eng, uint32_t field_id, int64_t value,
                                   uint32_t *out_actions, uint32_t out_max, uint32_t *out_count) {
    if (eng == NULL || out_count == NULL) {
        return BRAIN_ERR_INVALID;
    }
    uint32_t fired = 0u;
    uint32_t limit = eng->rule_count < BRAIN_BRE_MAX_RULES ? eng->rule_count : BRAIN_BRE_MAX_RULES;

    for (uint32_t i = 0; i < limit; i++) {
        const brain_bre_rule_t *r = &eng->rules[i];
        if (!r->active || r->field_id != field_id) {
            continue;
        }
        bool match;
        switch (r->op) {
            case BRAIN_BRE_OP_EQ:  match = (value == r->threshold); break;
            case BRAIN_BRE_OP_NE:  match = (value != r->threshold); break;
            case BRAIN_BRE_OP_LT:  match = (value <  r->threshold); break;
            case BRAIN_BRE_OP_LTE: match = (value <= r->threshold); break;
            case BRAIN_BRE_OP_GT:  match = (value >  r->threshold); break;
            case BRAIN_BRE_OP_GTE: match = (value >= r->threshold); break;
            default:               match = false; break;
        }
        if (match) {
            if (fired < out_max && out_actions != NULL) {
                out_actions[fired] = r->action_id;
            }
            fired++;
        }
    }
    *out_count = fired < out_max ? fired : out_max;
    return BRAIN_OK;
}

#define BRAIN_BRE_LINE_MAX 128u

brain_status_t brain_bre_load_from_text(brain_bre_engine_t *eng, const char *text, size_t text_len) {
    if (eng == NULL || (text == NULL && text_len > 0u)) {
        return BRAIN_ERR_INVALID;
    }
    brain_bre_init(eng);

    uint32_t rule_index = 0u;
    size_t pos = 0u;

    while (pos < text_len) {
        char line[BRAIN_BRE_LINE_MAX];
        size_t line_len = 0u;

        while (pos < text_len && text[pos] != '\n') {
            if (line_len >= BRAIN_BRE_LINE_MAX - 1u) {
                brain_bre_init(eng);
                return BRAIN_ERR_INVALID;
            }
            line[line_len++] = text[pos++];
        }
        if (pos < text_len) {
            pos++; /* skip the '\n' */
        }
        if (line_len > 0u && line[line_len - 1u] == '\r') {
            line_len--; /* CRLF-tolerant */
        }
        line[line_len] = '\0';

        size_t start = 0u;
        while (start < line_len && (line[start] == ' ' || line[start] == '\t')) {
            start++;
        }
        if (start == line_len || line[start] == '#') {
            continue; /* blank or comment line */
        }

        const char *p = line + start;
        char *endp = NULL;

        unsigned long field_id_ul = strtoul(p, &endp, 10);
        if (endp == p) {
            brain_bre_init(eng);
            return BRAIN_ERR_INVALID;
        }
        p = endp;
        while (*p == ' ' || *p == '\t') { p++; }

        brain_bre_op_t op;
        size_t op_len;
        if (strncmp(p, "==", 2) == 0)      { op = BRAIN_BRE_OP_EQ;  op_len = 2u; }
        else if (strncmp(p, "!=", 2) == 0) { op = BRAIN_BRE_OP_NE;  op_len = 2u; }
        else if (strncmp(p, "<=", 2) == 0) { op = BRAIN_BRE_OP_LTE; op_len = 2u; }
        else if (strncmp(p, ">=", 2) == 0) { op = BRAIN_BRE_OP_GTE; op_len = 2u; }
        else if (p[0] == '<')              { op = BRAIN_BRE_OP_LT;  op_len = 1u; }
        else if (p[0] == '>')              { op = BRAIN_BRE_OP_GT;  op_len = 1u; }
        else {
            brain_bre_init(eng);
            return BRAIN_ERR_INVALID;
        }
        p += op_len;
        while (*p == ' ' || *p == '\t') { p++; }

        long long threshold_ll = strtoll(p, &endp, 10);
        if (endp == p) {
            brain_bre_init(eng);
            return BRAIN_ERR_INVALID;
        }
        p = endp;
        while (*p == ' ' || *p == '\t') { p++; }

        if (p[0] != '-' || p[1] != '>') {
            brain_bre_init(eng);
            return BRAIN_ERR_INVALID;
        }
        p += 2;
        while (*p == ' ' || *p == '\t') { p++; }

        unsigned long action_id_ul = strtoul(p, &endp, 10);
        if (endp == p) {
            brain_bre_init(eng);
            return BRAIN_ERR_INVALID;
        }
        p = endp;
        while (*p == ' ' || *p == '\t') { p++; }
        if (*p != '\0') {
            brain_bre_init(eng);
            return BRAIN_ERR_INVALID; /* trailing garbage on the line */
        }

        if (rule_index >= BRAIN_BRE_MAX_RULES) {
            brain_bre_init(eng);
            return BRAIN_ERR_FULL;
        }
        brain_status_t st = brain_bre_load_rule(eng, rule_index, (uint32_t)field_id_ul, op,
                                                 (int64_t)threshold_ll, (uint32_t)action_id_ul);
        if (st != BRAIN_OK) {
            brain_bre_init(eng);
            return st;
        }
        rule_index++;
    }
    return BRAIN_OK;
}
