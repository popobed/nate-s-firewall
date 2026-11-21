/*
 * brain_core.h
 *
 * Niveaux 21-30 -- "Cerveau Logique" : API gateway / IAM (constant-time
 * auth + O(1) OAuth2-style scopes + lock-free token-bucket rate limiting),
 * a secrets vault, a workflow/webhook orchestrator backed by
 * barry_storage.h's MPMC ring, a cron timing wheel driven by opaque cycle
 * counts (swarm_tick's own posture), per-tenant billing quotas in a real
 * SoA layout, and a data-driven business rule engine -- all static,
 * zero-malloc-after-init, built from primitives this project already has
 * (crypto_constant_time.h, barry_storage.h), not reimplemented.
 *
 * ============================== Honesty notes, up front =====================
 * The request names ten real, independently enormous systems (Kong/
 * KrakenD, Keycloak/Authelia, a DB-to-API generator, Temporal/n8n, a
 * distributed cron at "billions of events", HashiCorp Vault, Lago/
 * Kill Bill, Centrifugo, and a JSON/YAML-driven BRE). Same posture as
 * infra_mesh.h and barry_storage.h before it: none of those becomes a
 * few hundred lines of freestanding C11 in one process. What this file
 * actually builds, per item:
 *
 *   - Niveau 21 (Go/Rust microservice templates): NOT attempted -- this
 *     project is C11, and generating scaffolding in other languages is
 *     outside anything this file could honestly claim to BE, not just
 *     scope down.
 *   - Niveau 22 (Kong/KrakenD gateway) & Niveau 23 (Keycloak/Authelia
 *     IAM): `brain_iam_registry_t` is real, working bearer-token
 *     authentication (ct_verify_token(), crypto_constant_time.h, reused
 *     verbatim -- not reimplemented) plus an O(1) OAuth2-style scope
 *     bitmask check. No SSO protocol (SAML/OIDC handshakes), no MFA, no
 *     user-facing login UI, no TLS termination -- a token/scope
 *     verification primitive, the concrete part of "gateway security"
 *     this codebase can honestly build without an HTTP server or a
 *     crypto handshake stack.
 *   - Niveau 24 (DB table -> generated REST/GraphQL API): NOT attempted.
 *     There is no SQL database in this project to introspect (barry_storage.h
 *     is an in-memory KV/document store, not a relational engine with a
 *     schema catalog) -- generating REST/GraphQL from a table that does
 *     not exist here would be fabricating scope, not building it.
 *   - Niveau 25 (Temporal/n8n workflow + webhooks) & Niveau 26 (cron "at
 *     billions of events"): `brain_workflow_engine_t` is a real, working
 *     fixed-step workflow state machine (direct-indexed instances, O(1)
 *     advance) that enqueues each step's action onto a caller-owned
 *     `barry_task_queue_t` (barry_storage.h's MPMC ring, reused verbatim
 *     -- explicitly "adosse a" in the request, not reinvented). "Webhook"
 *     here means exactly that: a structured outbound event record placed
 *     on the queue for a caller-owned drainer to actually send -- this
 *     file does NOT open arbitrary outbound HTTP/TLS connections to
 *     third-party URLs (net_io.c's real Winsock reactor only ever speaks
 *     this project's own fixed wire_msg_t framing, not general HTTP; a
 *     general HTTP client needs DNS resolution, TLS, and an unbounded
 *     URL/header parser, all outside this project's static-allocation
 *     posture). `brain_cron_wheel_t` is a real, working single-level
 *     hashed timing wheel (BRAIN_CRON_WHEEL_SLOTS slots, bounded jobs per
 *     slot) advanced one opaque cycle at a time via brain_cron_tick() --
 *     the same "caller-driven, no wall clock" discipline swarm_dispatch.h's
 *     swarm_tick already established. "Milliards d'evenements": the
 *     wheel's slot-modulo indexing genuinely never runs out of FUTURE
 *     cycles to schedule against (due_cycle is a 64-bit tick), so
 *     unbounded event THROUGHPUT over time is real; what is NOT real is
 *     billions of simultaneously-pending distinct jobs resident in static
 *     RAM at once -- each slot's bounded depth (BRAIN_CRON_SLOT_DEPTH) is
 *     the honest, finite limit, the same "concrete mechanism vs.
 *     aspirational number" distinction wasm_gpu_engine.h already draws
 *     for "120 FPS constants".
 *   - Niveau 27 (HashiCorp Vault): `brain_vault_t` is a real, working
 *     isolated in-memory secret store, direct-indexed by secret_id (O(1)),
 *     with zero-residue revocation (brain_vault_revoke() zeroes the
 *     slot -- same SECURITY posture as net_gateway.c's ngw_slab_free()
 *     and wire_proto.c's wire_ring_reserve()). No disk persistence, no
 *     seal/unseal, no dynamic secret leasing/rotation, no encryption at
 *     rest (values live in plain process memory, same trust boundary as
 *     everything else in this project) -- an isolated secret table, not
 *     a standalone secrets-management server. Access control (WHO may
 *     call brain_vault_reveal() for WHICH secret_id) is deliberately the
 *     CALLER's responsibility to compose with brain_iam_authenticate(),
 *     the same layering discipline actor_core.h already applies to
 *     actor_mailbox_reserve() (no ambient/implicit routing here either).
 *   - Niveau 28 (Lago/Kill Bill billing): `brain_billing_ledger_t` is a
 *     real, working per-tenant usage-quota ledger in genuine
 *     Structure-of-Arrays form -- each column (usage_count[],
 *     quota_limit[], period_end_cycle[]) is its own `_Alignas(64)` array,
 *     the exact pattern infra_mesh.h's `lb_pool_t` already uses (a writer
 *     hammering usage_count[] never invalidates a reader's cache line for
 *     quota_limit[], which barely changes) -- NOT per-tenant padding
 *     inside a column (see the header's "SoA, precisely" note below for
 *     why). No invoice generation, no payment processor integration, no
 *     currency/tax logic -- a quota-tracking primitive, the concrete,
 *     achievable meaning of "billing" this codebase can build standalone.
 *   - Niveau 29 (Centrifugo/PocketBase realtime): NOT attempted as a
 *     WebSocket server -- net_io.c's real Winsock reactor already exists
 *     in this project and speaks this project's own wire_msg_t framing;
 *     building a SEPARATE WebSocket/HTTP-upgrade protocol stack here
 *     would duplicate net_io.c's role for a different wire format this
 *     project has no client for, not a legitimate new primitive.
 *   - Niveau 30 (JSON/YAML-driven BRE): `brain_bre_engine_t` is a real,
 *     working data-driven rule table (field/operator/threshold ->
 *     action_id), loaded and changed at RUNTIME via
 *     brain_bre_load_rule()/brain_bre_load_from_text() -- genuinely "no
 *     recompilation needed" to change business logic, the concrete part
 *     of the request. brain_bre_load_from_text() is explicitly NOT a
 *     JSON/YAML parser (writing a general, safe, bounded JSON/YAML parser
 *     is its own significant, security-sensitive undertaking, and
 *     silently claiming "YAML" for a hand-rolled subset would be exactly
 *     the kind of overclaim this codebase's own SESSION_NOTES.md already
 *     refuses elsewhere): it is a tiny, fixed-grammar line format,
 *     `<field_id> <op> <threshold> -> <action_id>` per line, documented
 *     at brain_bre_load_from_text()'s declaration. A caller that needs
 *     real JSON/YAML parses it with a real parser and calls
 *     brain_bre_load_rule() directly with the result -- the same
 *     boundary wire_proto.h already draws around itself (a wire struct,
 *     not a serialization framework).
 *
 * ============================== SoA, precisely ===============================
 * "Structure of Arrays... _Alignas(64)... no False Sharing" is applied
 * here at COLUMN granularity (one `_Alignas(64)` array per field), not
 * per-tenant-element granularity -- the SAME choice infra_mesh.h's
 * `lb_pool_t` already made and documented: false sharing is a
 * PERFORMANCE concern between DIFFERENT, independently-hot columns (a
 * billing charge hammering usage_count[] must never evict a mostly-
 * static quota_limit[] read from cache), not a correctness/isolation
 * one -- direct indexing by tenant_id (never a search, never shared
 * state between two different tenant_id values) is what actually
 * guarantees multi-tenant isolation, and that guarantee holds regardless
 * of whether two tenants' *counters* happen to share a 64-byte line.
 * Padding every individual tenant slot to its own cache line as well
 * would cost BRAIN_BILLING_MAX_TENANTS x 64 bytes per column for a
 * property (contention between DIFFERENT tenants hammering the SAME
 * column at the SAME moment) this project has no evidence is its actual
 * bottleneck versus the column-vs-column false sharing this pattern
 * already closes for real, tested cost.
 */

#ifndef BRAIN_CORE_H
#define BRAIN_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "crypto_constant_time.h" /* ct_verify_token: reused verbatim, not reimplemented */
#include "barry_storage.h"        /* barry_task_queue_t: reused verbatim as the workflow/webhook event bus */
#include "telemetry_fabric.h"     /* tf_log_t: reused verbatim by brain_billing_charge_audited() below */

#define BRAIN_CACHE_LINE_SIZE 64u

typedef enum {
    BRAIN_OK = 0,
    BRAIN_ERR_INVALID        = -1,
    BRAIN_ERR_NOT_FOUND      = -2,
    BRAIN_ERR_FULL           = -3, /* O(1) instant rejection, never a wait */
    BRAIN_ERR_OUT_OF_RANGE   = -4,
    BRAIN_ERR_UNAUTHORIZED   = -5,
    BRAIN_ERR_RATE_LIMITED   = -6,
    BRAIN_ERR_QUOTA_EXCEEDED = -7,
    BRAIN_ERR_ALREADY_REGISTERED = -8,
    BRAIN_ERR_INACTIVE       = -9
} brain_status_t;

/* ==================================================================== */
/* Part A -- IAM: constant-time bearer-token auth + O(1) OAuth2 scopes    */
/*           (Niveaux 22 & 23)                                           */
/* ==================================================================== */

#define BRAIN_IAM_MAX_TOKENS  64u  /* fixed token population, same posture as ACTOR_MAX_COUNT */
#define BRAIN_IAM_SECRET_LEN  32u  /* bearer token / API key length */

typedef struct {
    uint8_t  secret[BRAIN_IAM_SECRET_LEN];
    uint32_t scope_mask; /* one bit per OAuth2-style scope; meaning is caller-defined */
    uint16_t tenant_id;
    bool     active;
} brain_iam_token_t;

typedef struct {
    brain_iam_token_t tokens[BRAIN_IAM_MAX_TOKENS]; /* direct-indexed by token_id: O(1), same technique as mesh_registry_t/router tables */
} brain_iam_registry_t;

void brain_iam_init(brain_iam_registry_t *reg);

/* Boot-time/admin-time only (like mesh_register_node()): not thread-safe
 * against concurrent registration or authenticate() calls. */
brain_status_t brain_iam_register_token(brain_iam_registry_t *reg, uint16_t token_id, uint16_t tenant_id,
                                         const uint8_t secret[BRAIN_IAM_SECRET_LEN], uint32_t scope_mask);

/* Zeroes the secret -- no residue survives revocation, same SECURITY
 * posture as ngw_slab_free()/wire_ring_reserve(). */
brain_status_t brain_iam_revoke_token(brain_iam_registry_t *reg, uint16_t token_id);

/* O(1) direct-index lookup + ct_verify_token() (constant-time content
 * comparison for the already-known secret length -- see
 * crypto_constant_time.h's own "Scope, precisely" note for exactly what
 * is and is not hidden) + an O(1) scope bitmask check
 * ((token.scope_mask & required_scope_mask) == required_scope_mask). On
 * BRAIN_OK, *out_tenant_id is filled. */
brain_status_t brain_iam_authenticate(const brain_iam_registry_t *reg, uint16_t token_id,
                                       const uint8_t *provided_secret, size_t provided_len,
                                       uint32_t required_scope_mask, uint16_t *out_tenant_id);

/* ==================================================================== */
/* Part B -- Token-bucket rate limiting, O(1), lock-free (Niveau 22)     */
/* ==================================================================== */

#define BRAIN_RATE_MAX_BUCKETS BRAIN_IAM_MAX_TOKENS

typedef struct {
    _Atomic uint32_t tokens;
    _Atomic uint64_t last_refill_tick;
    uint32_t         capacity;         /* boot-time config, read-only after brain_rate_configure() */
    uint32_t         refill_per_tick;  /* tokens credited per elapsed opaque tick */
} brain_rate_bucket_t;

typedef struct {
    brain_rate_bucket_t buckets[BRAIN_RATE_MAX_BUCKETS];
} brain_rate_limiter_t;

void brain_rate_init(brain_rate_limiter_t *rl);

/* Boot-time only. Starts the bucket full (tokens = capacity). */
brain_status_t brain_rate_configure(brain_rate_limiter_t *rl, uint16_t bucket_id,
                                     uint32_t capacity, uint32_t refill_per_tick);

/* O(1): lazily refills (elapsed opaque ticks x refill_per_tick, capped at
 * capacity, via a single CAS-guarded "who gets credit for this refill"
 * step so concurrent callers never double-credit the same elapsed
 * window) then attempts to atomically consume `cost` tokens via a
 * bounded CAS retry loop (contention only among callers of the SAME
 * bucket_id -- same posture as ring_buffer.h's own CAS loops). Instant
 * BRAIN_ERR_RATE_LIMITED rejection when insufficient, never a wait.
 * `current_tick` is an OPAQUE caller-supplied counter -- never a wall
 * clock, same posture as swarm_tick's current_cycle. */
brain_status_t brain_rate_try_consume(brain_rate_limiter_t *rl, uint16_t bucket_id,
                                       uint64_t current_tick, uint32_t cost);

/* ==================================================================== */
/* Part C -- Secrets vault (Niveau 27, honest scope)                     */
/* ==================================================================== */

#define BRAIN_VAULT_MAX_SECRETS 64u
#define BRAIN_VAULT_SECRET_MAX  64u

typedef struct {
    uint8_t  value[BRAIN_VAULT_SECRET_MAX];
    uint32_t len;
    bool     present;
} brain_vault_entry_t;

typedef struct {
    brain_vault_entry_t entries[BRAIN_VAULT_MAX_SECRETS]; /* direct-indexed by secret_id: O(1) */
} brain_vault_t;

void brain_vault_init(brain_vault_t *vault);
brain_status_t brain_vault_store(brain_vault_t *vault, uint32_t secret_id, const void *value, uint32_t len);
brain_status_t brain_vault_reveal(const brain_vault_t *vault, uint32_t secret_id, void *out_value, uint32_t *inout_len);

/* Zeroes the slot's full value buffer -- no residue survives revocation
 * (same SECURITY posture as brain_iam_revoke_token()). */
brain_status_t brain_vault_revoke(brain_vault_t *vault, uint32_t secret_id);

/* ==================================================================== */
/* Part D -- Workflow orchestrator + webhook event bus (Niveau 25)       */
/* ==================================================================== */

#define BRAIN_WORKFLOW_MAX_DEFS      16u
#define BRAIN_WORKFLOW_MAX_STEPS     8u
#define BRAIN_WORKFLOW_MAX_INSTANCES 64u

typedef struct {
    uint32_t step_action_id[BRAIN_WORKFLOW_MAX_STEPS]; /* opaque, caller-defined action ids -- same "caller owns the meaning" posture as swarm_tick's cycle counters */
    uint32_t step_count;
    bool     defined;
} brain_workflow_def_t;

typedef struct {
    brain_workflow_def_t defs[BRAIN_WORKFLOW_MAX_DEFS]; /* direct-indexed by workflow_def_id: O(1) */
} brain_workflow_registry_t;

typedef enum {
    BRAIN_WF_RUNNING   = 0,
    BRAIN_WF_COMPLETED = 1
} brain_workflow_run_state_t;

typedef struct {
    uint32_t workflow_def_id;
    uint32_t current_step;
    uint16_t tenant_id;
    brain_workflow_run_state_t run_state;
    bool     active;
} brain_workflow_instance_t;

typedef struct {
    brain_workflow_instance_t instances[BRAIN_WORKFLOW_MAX_INSTANCES]; /* direct-indexed by instance_id: O(1) */
} brain_workflow_engine_t;

void brain_workflow_registry_init(brain_workflow_registry_t *reg);
brain_status_t brain_workflow_define(brain_workflow_registry_t *reg, uint32_t workflow_def_id,
                                      const uint32_t *step_action_ids, uint32_t step_count);

void brain_workflow_engine_init(brain_workflow_engine_t *eng);
brain_status_t brain_workflow_start(brain_workflow_engine_t *eng, const brain_workflow_registry_t *reg,
                                     uint32_t instance_id, uint32_t workflow_def_id, uint16_t tenant_id);

/* Task type stamped on every step enqueued by brain_workflow_advance(). */
#define BRAIN_TASK_TYPE_WORKFLOW_STEP 0xB4A10001u

/*
 * O(1): if the instance is RUNNING, enqueues its CURRENT step's
 * action_id (packed as a uint32_t payload, sender_id = instance_id) onto
 * `queue` (a caller-owned barry_task_queue_t -- barry_storage.h's MPMC
 * ring, reused verbatim, exactly the "adosse a des Ring Buffers MPMC"
 * the mission asked for) as the honest, concrete meaning of "fire this
 * webhook/step" -- draining the queue and actually acting on a step
 * (sending an HTTP request, updating a CRM, etc.) is the caller's
 * responsibility, the same layering discipline swarm_dispatch.h's
 * script_engine already applies. Then advances current_step; once past
 * the last step, run_state becomes BRAIN_WF_COMPLETED (a terminal
 * state -- calling advance() again on a completed instance is a no-op
 * returning BRAIN_ERR_INACTIVE, never re-fires the last step).
 * BARRY_ERR_FULL from the underlying enqueue is propagated as-is
 * (BRAIN_ERR_FULL) without advancing current_step -- backpressure on the
 * event bus must never silently skip a step.
 */
brain_status_t brain_workflow_advance(brain_workflow_engine_t *eng, const brain_workflow_registry_t *reg,
                                       barry_task_queue_t *queue, uint32_t instance_id);

/* ==================================================================== */
/* Part E -- Cron timing wheel (Niveau 26)                                */
/* ==================================================================== */

#define BRAIN_CRON_WHEEL_SLOTS 1024u /* power of two: slot = due_cycle & (SLOTS-1) */
#define BRAIN_CRON_SLOT_DEPTH  16u   /* bounded pending jobs per slot */

_Static_assert(BRAIN_CRON_WHEEL_SLOTS != 0u &&
               (BRAIN_CRON_WHEEL_SLOTS & (BRAIN_CRON_WHEEL_SLOTS - 1u)) == 0u,
               "BRAIN_CRON_WHEEL_SLOTS must be a power of two");

typedef struct {
    uint32_t          job_id[BRAIN_CRON_SLOT_DEPTH];
    uint64_t          due_cycle[BRAIN_CRON_SLOT_DEPTH]; /* exact due tick -- disambiguates jobs that share a slot from different wheel rotations */
    _Atomic uint32_t  count; /* also the next free index; claimed via fetch_add, same pattern as barry_index_link() */
} brain_cron_slot_t;

typedef struct {
    brain_cron_slot_t slots[BRAIN_CRON_WHEEL_SLOTS];
} brain_cron_wheel_t;

void brain_cron_init(brain_cron_wheel_t *wheel);

/* O(1): any number of concurrent producers. BRAIN_ERR_FULL if this
 * due_cycle's slot has already reached BRAIN_CRON_SLOT_DEPTH pending
 * jobs (a real, documented, bounded limit -- see the file header's
 * "Milliards d'evenements" note). */
brain_status_t brain_cron_schedule(brain_cron_wheel_t *wheel, uint32_t job_id, uint64_t due_cycle);

/*
 * O(1) (bounded by BRAIN_CRON_SLOT_DEPTH): processes EXACTLY the one
 * slot for `current_cycle` (slot = current_cycle & (SLOTS-1)), draining
 * every job whose due_cycle == current_cycle exactly into
 * out_fired_job_ids[] (up to out_max) and resetting the slot's count to
 * 0. Jobs in that slot whose due_cycle does NOT match current_cycle
 * (i.e. scheduled for a future wheel rotation sharing this slot) are
 * re-appended for their real rotation -- never fired early, never lost.
 * Caller-driven, one opaque cycle at a time, in non-decreasing order --
 * the same discipline as swarm_tick(): this file has no wall clock and
 * does not advance itself. Pass out_max >= BRAIN_CRON_SLOT_DEPTH to
 * guarantee every job actually due this cycle is reported: due jobs are
 * removed from the wheel unconditionally (they fired), so a smaller
 * out_max would mean some of them are consumed without being
 * represented in out_fired_job_ids for this call.
 *
 * Not safe to call concurrently with brain_cron_schedule() calls that
 * could target THIS SAME slot (i.e. any due_cycle congruent to
 * current_cycle mod BRAIN_CRON_WHEEL_SLOTS) -- same single-driver
 * caveat as ring_buffer.h's single-consumer rb_try_pop(). Concurrent
 * schedule() calls targeting any OTHER slot are unaffected.
 */
brain_status_t brain_cron_tick(brain_cron_wheel_t *wheel, uint64_t current_cycle,
                                uint32_t *out_fired_job_ids, uint32_t out_max, uint32_t *out_count);

/* ==================================================================== */
/* Part F -- Billing quotas, real SoA, _Alignas(64) per column (Niveau 28)*/
/* ==================================================================== */

#define BRAIN_BILLING_MAX_TENANTS 128u

typedef struct {
    _Alignas(BRAIN_CACHE_LINE_SIZE) _Atomic uint32_t usage_count[BRAIN_BILLING_MAX_TENANTS];
    _Alignas(BRAIN_CACHE_LINE_SIZE) uint32_t         quota_limit[BRAIN_BILLING_MAX_TENANTS];
    _Alignas(BRAIN_CACHE_LINE_SIZE) uint64_t         period_end_cycle[BRAIN_BILLING_MAX_TENANTS];
    _Alignas(BRAIN_CACHE_LINE_SIZE) bool             registered[BRAIN_BILLING_MAX_TENANTS];
} brain_billing_ledger_t;

void brain_billing_init(brain_billing_ledger_t *ledger);

/* Boot-time/admin-time only. period_end_cycle is the opaque tick at
 * which brain_billing_charge() next resets usage_count to 0 for this
 * tenant (caller re-arms it by calling this again with a new
 * period_end_cycle -- this file does not compute calendar billing
 * periods, same "caller owns the meaning of the counter" posture as
 * everywhere else in this project). */
brain_status_t brain_billing_register_tenant(brain_billing_ledger_t *ledger, uint16_t tenant_id,
                                              uint32_t quota_limit, uint64_t period_end_cycle);

/*
 * O(1), direct-indexed by tenant_id: if current_cycle >=
 * period_end_cycle[tenant_id], resets usage_count to 0 first (a lazy,
 * caller-driven period rollover -- consistent with the rest of this
 * project having no wall clock; the CALLER is responsible for supplying
 * a new period_end_cycle via brain_billing_register_tenant() to truly
 * start a new period, otherwise every subsequent call keeps resetting).
 * Then atomically attempts usage_count += amount via a bounded CAS
 * retry loop, rejecting with BRAIN_ERR_QUOTA_EXCEEDED (an O(1) instant
 * rejection, never a partial charge) if that would exceed quota_limit.
 */
brain_status_t brain_billing_charge(brain_billing_ledger_t *ledger, uint16_t tenant_id,
                                     uint64_t current_cycle, uint32_t amount);

uint32_t brain_billing_usage(const brain_billing_ledger_t *ledger, uint16_t tenant_id);

/*
 * Additive coupling point (Niveaux 31-40): runs brain_billing_charge()
 * completely unchanged, then appends ONE tf_log_t entry (telemetry_fabric.h,
 * reused verbatim) recording tenant_id, amount and the resulting
 * brain_status_t -- an immutable, hash-chained billing audit trail.
 * `audit_log` may be NULL to skip logging; every existing caller of
 * brain_billing_charge() itself is unaffected.
 */
brain_status_t brain_billing_charge_audited(brain_billing_ledger_t *ledger, uint16_t tenant_id,
                                             uint64_t current_cycle, uint32_t amount, tf_log_t *audit_log);

/* ==================================================================== */
/* Part G -- Business Rule Engine: data-driven, no recompilation (Niveau 30) */
/* ==================================================================== */

#define BRAIN_BRE_MAX_RULES 64u

typedef enum {
    BRAIN_BRE_OP_EQ  = 0,
    BRAIN_BRE_OP_NE  = 1,
    BRAIN_BRE_OP_LT  = 2,
    BRAIN_BRE_OP_LTE = 3,
    BRAIN_BRE_OP_GT  = 4,
    BRAIN_BRE_OP_GTE = 5
} brain_bre_op_t;

typedef struct {
    uint32_t       field_id;  /* opaque, caller-defined field identifier */
    brain_bre_op_t op;
    int64_t        threshold;
    uint32_t       action_id; /* opaque, caller-defined action identifier, fired when the rule matches */
    bool           active;
} brain_bre_rule_t;

typedef struct {
    brain_bre_rule_t rules[BRAIN_BRE_MAX_RULES];
    uint32_t         rule_count;
} brain_bre_engine_t;

void brain_bre_init(brain_bre_engine_t *eng);

/* Loads/replaces ONE rule at `index` -- rules are DATA the caller can
 * change at any time without recompiling this file, the concrete,
 * achievable meaning of "config-driven business logic" here. */
brain_status_t brain_bre_load_rule(brain_bre_engine_t *eng, uint32_t index, uint32_t field_id,
                                    brain_bre_op_t op, int64_t threshold, uint32_t action_id);

/* O(rule_count), bounded by BRAIN_BRE_MAX_RULES (a small, fixed compile-
 * time constant -- same "bounded, not the strict textbook O(1)"
 * precision infra_mesh.h's lb_pool_dispatch() already applies): every
 * ACTIVE rule whose field_id matches is evaluated against `value`;
 * every match appends its action_id to out_actions[] (up to out_max).
 * *out_count is how many actually fired. */
brain_status_t brain_bre_evaluate(const brain_bre_engine_t *eng, uint32_t field_id, int64_t value,
                                   uint32_t *out_actions, uint32_t out_max, uint32_t *out_count);

/*
 * Minimal, fixed-grammar textual rule loader -- explicitly NOT a JSON or
 * YAML parser (see the file header's honesty note for why). Grammar, one
 * rule per line, blank lines and lines starting with '#' ignored:
 *
 *     <field_id> <op> <threshold> -> <action_id>
 *
 * where <op> is one of ==, !=, <, <=, >, >= and every other token is a
 * decimal integer (threshold may be negative). Parses into `eng`
 * starting at rule index 0, overwriting whatever was loaded before, up
 * to BRAIN_BRE_MAX_RULES lines -- BRAIN_ERR_FULL if the text has more
 * rule lines than that. A malformed line is BRAIN_ERR_INVALID and stops
 * parsing immediately (no partial application of a bad file: on error,
 * `eng` is left exactly as brain_bre_init() left it).
 */
brain_status_t brain_bre_load_from_text(brain_bre_engine_t *eng, const char *text, size_t text_len);

#endif /* BRAIN_CORE_H */
