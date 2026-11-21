#include "actor_pipeline.h"

#include <string.h>

/* Shared by every _audited()/_inspected() function below: always fully
 * zeroes the payload first, so an added-later field (e.g.
 * matched_rule_id) is never left uninitialized for a call site that
 * predates it -- see actor_pipeline.h's header note on why this project
 * treats that as a correctness requirement, not a style preference. */
static void audit_append(tf_log_t *audit_log, uint32_t event_type, uint8_t actor_id,
                          uint32_t producer_id, uint32_t type, int16_t status,
                          uint32_t matched_rule_id) {
    if (audit_log == NULL) {
        return;
    }
    actor_pipeline_tf_audit_payload_t payload;
    memset(&payload, 0, sizeof payload);
    payload.actor_id = actor_id;
    payload.producer_id = producer_id;
    payload.type = type;
    payload.status = status;
    payload.matched_rule_id = matched_rule_id;
    (void)tf_log_append(audit_log, event_type, &payload, (uint32_t)sizeof payload);
}

actor_pipeline_status_t actor_task_queue_bridge_one(actor_t *actor, barry_task_queue_t *q,
                                                     tf_log_t *audit_log) {
    if (actor == NULL || q == NULL) {
        return ACTOR_PIPELINE_ERR_INVALID;
    }

    barry_task_t task;
    barry_status_t dst = barry_task_dequeue(q, &task);
    if (dst == BARRY_ERR_EMPTY) {
        return ACTOR_PIPELINE_ERR_QUEUE_EMPTY;
    }
    if (dst != BARRY_OK) {
        return ACTOR_PIPELINE_ERR_INVALID;
    }

    actor_pipeline_status_t result;

    if (task.payload_len > RING_MSG_PAYLOAD_SIZE) {
        /* Already removed from the broker -- dropped, not requeued. See
         * this file's header for why requeueing would not be meaningful. */
        result = ACTOR_PIPELINE_ERR_PAYLOAD_TOO_LARGE;
    } else {
        ring_msg_t *slot;
        actor_status_t ast = actor_mailbox_reserve(actor, &slot);
        if (ast == ACTOR_ERR_MAILBOX_FULL) {
            result = ACTOR_PIPELINE_ERR_MAILBOX_FULL;
        } else if (ast != ACTOR_OK) {
            result = ACTOR_PIPELINE_ERR_INVALID;
        } else {
            memcpy(slot->payload, task.payload, task.payload_len);
            if (task.payload_len < RING_MSG_PAYLOAD_SIZE) {
                memset(slot->payload + task.payload_len, 0, RING_MSG_PAYLOAD_SIZE - task.payload_len);
            }
            actor_mailbox_publish(actor, slot, task.producer_id, task.type);
            result = ACTOR_PIPELINE_OK;
        }
    }

    audit_append(audit_log, ACTOR_PIPELINE_TF_EVENT_DEQUEUE, actor->actor_id,
                 task.producer_id, task.type, (int16_t)result, 0u);
    return result;
}

actor_pipeline_status_t actor_task_queue_bridge_one_inspected(actor_t *actor, barry_task_queue_t *q,
                                                                tf_log_t *audit_log) {
    if (actor == NULL || q == NULL) {
        return ACTOR_PIPELINE_ERR_INVALID;
    }

    barry_task_t task;
    barry_status_t dst = barry_task_dequeue(q, &task);
    if (dst == BARRY_ERR_EMPTY) {
        return ACTOR_PIPELINE_ERR_QUEUE_EMPTY;
    }
    if (dst != BARRY_OK) {
        return ACTOR_PIPELINE_ERR_INVALID;
    }

    actor_pipeline_status_t result;
    uint32_t matched_rule_id = 0u;
    uint32_t event = ACTOR_PIPELINE_TF_EVENT_DEQUEUE;

    agent_inspect_result_t inspect_result;
    agent_inspect_status_t inspect_st =
        agent_inspect_instruction((const char *)task.payload, task.payload_len, &inspect_result);

    if (inspect_st != AGENT_INSPECT_OK) {
        /* task.payload_len (<= BARRY_TASK_PAYLOAD_MAX == 64) is always
         * <= AGENT_INSTR_MAX_LEN (256), so this branch is defensive only. */
        result = ACTOR_PIPELINE_ERR_INVALID;
    } else if (inspect_result.verdict == AGENT_VERDICT_BLOCKED) {
        /* Already removed from the broker -- dropped, never reaches the
         * actor's mailbox or private memory. See file header. */
        result = ACTOR_PIPELINE_ERR_BLOCKED;
        matched_rule_id = inspect_result.matched_rule_id;
        event = ACTOR_PIPELINE_TF_EVENT_DEQUEUE_BLOCKED;
    } else if (task.payload_len > RING_MSG_PAYLOAD_SIZE) {
        result = ACTOR_PIPELINE_ERR_PAYLOAD_TOO_LARGE;
    } else {
        ring_msg_t *slot;
        actor_status_t ast = actor_mailbox_reserve(actor, &slot);
        if (ast == ACTOR_ERR_MAILBOX_FULL) {
            result = ACTOR_PIPELINE_ERR_MAILBOX_FULL;
        } else if (ast != ACTOR_OK) {
            result = ACTOR_PIPELINE_ERR_INVALID;
        } else {
            memcpy(slot->payload, task.payload, task.payload_len);
            if (task.payload_len < RING_MSG_PAYLOAD_SIZE) {
                memset(slot->payload + task.payload_len, 0, RING_MSG_PAYLOAD_SIZE - task.payload_len);
            }
            actor_mailbox_publish(actor, slot, task.producer_id, task.type);
            result = ACTOR_PIPELINE_OK;
        }
    }

    audit_append(audit_log, event, actor->actor_id, task.producer_id, task.type,
                 (int16_t)result, matched_rule_id);
    return result;
}

actor_status_t actor_process_one_audited(actor_t *actor, ring_msg_t *out, tf_log_t *audit_log) {
    actor_status_t st = actor_process_one(actor, out);

    if (actor != NULL) {
        uint32_t producer_id = (st == ACTOR_OK && out != NULL) ? out->sender_id : 0u;
        uint32_t type = (st == ACTOR_OK && out != NULL) ? out->type : 0u;
        audit_append(audit_log, ACTOR_PIPELINE_TF_EVENT_PROCESS, actor->actor_id,
                     producer_id, type, (int16_t)st, 0u);
    }

    return st;
}
