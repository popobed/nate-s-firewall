/*
 * actor_pipeline.h
 *
 * Additive coupling module: extends actor_core.h's isolated,
 * single-consumer actor mailboxes into barry_storage.h's real MPMC task
 * queue and telemetry_fabric.h's hash-chained audit log -- the same
 * "wrap the existing function unchanged, then log" discipline
 * infra_mesh.h's mesh_dispatch_audited() and brain_core.h's
 * brain_billing_charge_audited() already established, applied to
 * actor_core.h. Does NOT modify actor_core.h/.c, barry_storage.h/.c or
 * telemetry_fabric.h/.c themselves; every existing caller of those files
 * is unaffected.
 *
 * ============================== Honesty notes, up front =====================
 *
 * - "Ring Buffers MPMC" : ring_buffer.h is, by its own documented design,
 *   single-consumer (MPSC) -- see that header's own comment, and reused
 *   as-is everywhere else in this project. The real multi-producer/
 *   multi-CONSUMER primitive already in this codebase is
 *   barry_storage.h's barry_task_queue_t (a Vyukov bounded MPMC ring),
 *   built there specifically BECAUSE ring_buffer.h is the wrong shape for
 *   multiple consumers (barry_storage.h's own header explains this
 *   choice). This file does not invent a second MPMC primitive; it
 *   bridges actor_core.h's isolated mailboxes to that existing one, so
 *   any number of actors can act as competing consumers pulling
 *   fan-out work items off ONE shared broker queue -- a capability
 *   neither module offers alone.
 *
 * - actor_task_queue_bridge_one(): dequeues at most one barry_task_t from
 *   a shared barry_task_queue_t (barry_task_dequeue() is genuinely safe
 *   for any number of concurrent callers) and republishes it into one
 *   specific actor's own isolated ring_buffer_t mailbox. A task is
 *   removed from the broker the instant it is dequeued -- that is
 *   barry_task_dequeue()'s own contract, not something this file can
 *   change. If its payload does not fit the destination mailbox slot
 *   (BARRY_TASK_PAYLOAD_MAX == 64 bytes can exceed RING_MSG_PAYLOAD_SIZE
 *   == 48 bytes), this function does NOT silently truncate it: it
 *   returns ACTOR_PIPELINE_ERR_PAYLOAD_TOO_LARGE and the task is dropped,
 *   never requeued (the broker gives no ordering guarantee that would
 *   make requeueing behind whatever else was enqueued meanwhile
 *   meaningful). A genuine, disclosed boundary condition, not hidden
 *   data loss -- `audit_log`, when supplied, still records the drop.
 *
 * - actor_process_one_audited(): runs actor_process_one() completely
 *   unchanged, then appends ONE tf_log_t entry (telemetry_fabric.h,
 *   reused verbatim) recording actor_id, the popped message's
 *   sender_id/type, and the resulting actor_status_t -- an immutable,
 *   hash-chained audit trail of actor message processing. `audit_log`
 *   may be NULL to skip logging, exactly like the existing _audited()
 *   functions in infra_mesh.h/brain_core.h; when NULL, this call is
 *   exactly actor_process_one().
 *
 * - actor_task_queue_bridge_one_inspected(): added during a later
 *   hardening pass, additive alongside actor_task_queue_bridge_one()
 *   (which is unchanged and still available for a caller that does not
 *   want inspection). Runs agent_inspect_instruction() (agent_inspector.h,
 *   reused verbatim) against the dequeued task's payload BEFORE it is
 *   ever copied into the destination actor's mailbox -- deny-by-default,
 *   same posture as agent_runner.h's Zero-Trust dispatch. A BLOCKED
 *   verdict returns ACTOR_PIPELINE_ERR_BLOCKED and the task is dropped
 *   (already removed from the broker -- same disclosed-drop posture as
 *   the oversized-payload case above), never reaching the actor's
 *   private memory or mailbox. Bounded, zero-allocation: the inspection
 *   itself is O(1) in the same precise sense this project already uses
 *   elsewhere (bounded by the fixed compile-time constants
 *   AGENT_INSTR_MAX_LEN and the deny-list's fixed size, not by any
 *   value that grows at runtime -- see agent_inspector.h).
 */

#ifndef ACTOR_PIPELINE_H
#define ACTOR_PIPELINE_H

#include <stdint.h>

#include "actor_core.h"
#include "agent_inspector.h"
#include "barry_storage.h"
#include "telemetry_fabric.h"

typedef enum {
    ACTOR_PIPELINE_OK = 0,
    ACTOR_PIPELINE_ERR_INVALID           = -1,
    ACTOR_PIPELINE_ERR_QUEUE_EMPTY       = -2, /* not a failure: barry_task_queue_t had nothing ready */
    ACTOR_PIPELINE_ERR_MAILBOX_FULL      = -3, /* O(1) backpressure rejection on the destination mailbox */
    ACTOR_PIPELINE_ERR_PAYLOAD_TOO_LARGE = -4, /* task WAS dequeued (and is dropped) -- see file header */
    ACTOR_PIPELINE_ERR_BLOCKED           = -5  /* agent_inspect_instruction() denied it -- see file header, _inspected() only */
} actor_pipeline_status_t;

/* tf_log_t event_type tags this file uses -- opaque, caller-facing constants. */
#define ACTOR_PIPELINE_TF_EVENT_DEQUEUE         0xA9100001u /* actor_task_queue_bridge_one() */
#define ACTOR_PIPELINE_TF_EVENT_PROCESS         0xA9100002u /* actor_process_one_audited() */
#define ACTOR_PIPELINE_TF_EVENT_DEQUEUE_BLOCKED 0xA9100003u /* actor_task_queue_bridge_one_inspected(): denied */

/* Audit payload this file encodes into tf_log_t entries -- well under
 * TF_LOG_MSG_MAX (48 bytes). Always fully zeroed before use (every field
 * meaningful in every case, no uninitialized trailing bytes ever logged
 * -- see telemetry_fabric.h/SESSION_NOTES.md for why this project treats
 * that as a correctness requirement, not a style preference). */
typedef struct {
    uint8_t  actor_id;
    uint32_t producer_id;     /* DEQUEUE(_BLOCKED): barry_task_t.producer_id. PROCESS: the popped ring_msg_t's sender_id. */
    uint32_t type;             /* DEQUEUE(_BLOCKED): barry_task_t.type.        PROCESS: the popped ring_msg_t's type. */
    int16_t  status;           /* actor_pipeline_status_t or actor_status_t, cast to int16_t */
    uint32_t matched_rule_id;  /* meaningful iff status == ACTOR_PIPELINE_ERR_BLOCKED, else 0 -- agent_inspector.h's rule index */
} actor_pipeline_tf_audit_payload_t;

_Static_assert(sizeof(actor_pipeline_tf_audit_payload_t) <= TF_LOG_MSG_MAX,
               "actor_pipeline_tf_audit_payload_t must fit tf_log_t's fixed payload");

/*
 * Dequeues at most one task from `q` (any number of concurrent callers)
 * and republishes it into `actor`'s own isolated mailbox via
 * actor_mailbox_reserve()/_publish(). ACTOR_PIPELINE_ERR_QUEUE_EMPTY is
 * not a failure. See file header for the payload-size boundary.
 * `audit_log` may be NULL to skip logging.
 */
actor_pipeline_status_t actor_task_queue_bridge_one(actor_t *actor, barry_task_queue_t *q,
                                                     tf_log_t *audit_log);

/*
 * Same contract as actor_task_queue_bridge_one(), PLUS a mandatory
 * Zero-Trust inspection of the dequeued task's payload before it is
 * copied anywhere -- see file header. ACTOR_PIPELINE_ERR_BLOCKED on a
 * denied payload; otherwise identical behavior (including the
 * QUEUE_EMPTY/MAILBOX_FULL/PAYLOAD_TOO_LARGE outcomes) to
 * actor_task_queue_bridge_one(). `audit_log` may be NULL to skip logging.
 */
actor_pipeline_status_t actor_task_queue_bridge_one_inspected(actor_t *actor, barry_task_queue_t *q,
                                                                tf_log_t *audit_log);

/*
 * Runs actor_process_one() completely unchanged, then appends ONE
 * tf_log_t entry recording the outcome. `audit_log` may be NULL to skip
 * logging.
 */
actor_status_t actor_process_one_audited(actor_t *actor, ring_msg_t *out, tf_log_t *audit_log);

#endif /* ACTOR_PIPELINE_H */
