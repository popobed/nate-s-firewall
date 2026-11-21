/*
 * self_replicate.h
 *
 * Niveaux 41-50 -- the Empire's capstone: per-actor sandbox health
 * tracking with a deterministic, threshold-triggered "predictive"
 * auto-heal (a real O(1) memory-erasure + mailbox-drain reset, not a
 * trained prediction model), an immutable compile-time SaaS-template
 * dictionary with a deterministic, hallucination-free generator, and
 * `agn_execute_pure_replication()` -- the orchestration capstone that
 * composes actor_core.h, infra_mesh.h, brain_core.h, telemetry_fabric.h,
 * karo_interface.h and signal_core.h's real CPUID detection into ONE
 * deterministic "deploy a SaaS instance" call. Static, zero-malloc-
 * after-init; every underlying mechanism is reused verbatim from a
 * module this project already built and tested -- this file is
 * deliberately mostly composition, not a new low-level primitive.
 *
 * ============================== Honesty notes, up front =====================
 *   - Niveau 46 (Firecracker/cgroups/WASM sandboxing) : NOT a new
 *     isolation kernel technology -- this project has no WASM
 *     interpreter/runtime (wasm_gpu_engine.c's "wasm-demo" target
 *     compiles a portable SUBSET TO wasm OBJECT CODE, it does not
 *     execute wasm, see that header's own note) and no container/VM
 *     technology. The real, honest "isolated logical instance" this
 *     codebase already has is actor_core.h's actor_t (private_memory a
 *     no-other-actor-ever-touches block + an isolated mailbox) --
 *     infra_mesh.h already established "one mesh node == one actor_t"
 *     on the same basis. `self_replicate_sandbox_t` adds per-actor
 *     HEALTH tracking on top of that existing isolation, it does not
 *     invent a second one.
 *   - Niveau 48/49 ("predictive" auto-heal, bound-check/latency
 *     monitoring) : real and working, but precisely scoped -- there is
 *     no statistical or trained prediction model anywhere in this
 *     codebase (consistent with audio_transcribe.h's own "NOT real
 *     phoneme recognition" honesty note). "Predictive" here means a
 *     simple, deterministic threshold rule: once a sandbox's caller-
 *     reported bounds-violation count or latency sample crosses a fixed
 *     compile-time threshold, `agn_predictive_auto_heal()` acts BEFORE
 *     the caller necessarily sees a hard failure -- the "prediction" is
 *     "this trend crossed a known danger line", not a forecast from
 *     learned data. The "auto-correction... masques d'effacement
 *     binaires... temps constant O(1)" is real: `memset()`ing the
 *     actor's entire fixed-size (ACTOR_PRIVATE_MEM_SIZE, a compile-time
 *     constant) private_memory to zero plus draining its mailbox
 *     (bounded by RING_BUFFER_CAPACITY) -- both genuinely O(1)-bounded,
 *     the same "zero-residue reset" discipline net_gateway.c's
 *     ngw_slab_free() and brain_core.h's brain_iam_revoke_token()
 *     already apply, generalized into a health-triggered action instead
 *     of an explicit revoke call.
 *   - Niveau 44/45/47 (Ollama-style INT8 inference, SaaS-config
 *     dictionaries, deterministic generator "eliminating hallucination") :
 *     audio_transcribe.h's `audio_phoneme_classifier_init()` already IS
 *     this project's honest INT8-quantized, SRAM-scratchpad, argmax
 *     inference primitive (fixed-seed weights, zero training, already
 *     documented there as NOT real language understanding) -- not
 *     reimplemented a second time here. What IS new: `g_saas_templates`,
 *     a genuinely immutable (`static const`, true read-only data -- this
 *     dictionary, unlike karo_interface.h's runtime-provisioned key pool,
 *     truly never changes after compilation) `_Alignas(64)` array of
 *     fixed SaaS archetypes, and `self_replicate_generate_saas()`, a
 *     PURE, deterministic function: template lookup + a caller-supplied
 *     config struct in, one fixed-layout `saas_manifest_t` out, always
 *     identical for identical input. "Eliminant toute hallucination de
 *     code" is trivially, honestly true here for the reason that matters:
 *     this function never generates prose or source text via any
 *     language model at all -- it is a table lookup and a few integer
 *     operations, the same "data, not code-generation" boundary
 *     brain_core.h's BRE rule table already draws.
 *   - Niveau 50 (API Gateway scaffolding, SoA graphics at "120 FPS",
 *     real Swarm mesh deployment, CPUID-adaptive kernels for AVX-512/
 *     AMX/NEON) : `agn_execute_pure_replication()` is real composition,
 *     not a new gateway/graphics/mesh engine: it calls
 *     brain_iam_register_token() (brain_core.h, the real IAM this
 *     project already has -- see that header's own honest scope for
 *     "API Gateway"), mesh_register_node() (infra_mesh.h -- see that
 *     header for why this registers a LOGICAL node in this process,
 *     never a physically separate machine), karo_overlay_push()
 *     (karo_interface.h -- see that header's own unchanged "120 FPS
 *     constants" limit, not re-claimed as achieved here), and
 *     actor_system_spawn() (actor_core.h). CPUID: sig_detect_cpu_features()
 *     (signal_core.h, reused verbatim) already performs the real AVX-512/
 *     AMX/NEON detection this requirement asks for -- honestly reported,
 *     as that header already documents, WITHOUT this codebase owning a
 *     compiled kernel variant for any of those three (only scalar and
 *     AVX2 kernels exist, in audio_transcribe.c) --
 *     `self_replicate_select_kernel_variant()` returns which of those
 *     TWO real, existing kernels applies, and separately reports the
 *     full raw CPUID feature set truthfully, never conflating "detected"
 *     with "this codebase has an optimized path for it".
 */

#ifndef SELF_REPLICATE_H
#define SELF_REPLICATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "actor_core.h"
#include "infra_mesh.h"       /* mesh_registry_t/mesh_register_node: reused verbatim */
#include "brain_core.h"       /* brain_iam_registry_t/brain_iam_register_token: reused verbatim */
#include "telemetry_fabric.h" /* tf_log_t: reused verbatim */
#include "karo_interface.h"   /* karo_overlay_t: reused verbatim */
#include "signal_core.h"      /* sig_detect_cpu_features(): reused verbatim */
#include "signal_tracking.h"  /* sigtrack_status_t: reused verbatim by Part E below (already transitively pulled in via infra_mesh.h; included directly here too since Part E names it in its own signature) */

#define SELF_REPLICATE_CACHE_LINE_SIZE 64u

typedef enum {
    SELF_REPLICATE_OK = 0,
    SELF_REPLICATE_ERR_INVALID       = -1,
    SELF_REPLICATE_ERR_OUT_OF_RANGE  = -2,
    SELF_REPLICATE_ERR_NOT_FOUND     = -3,
    SELF_REPLICATE_ERR_ALREADY_ACTIVE = -4,
    SELF_REPLICATE_ERR_SPAWN_FAILED  = -5,
    SELF_REPLICATE_ERR_MESH_FAILED   = -6,
    SELF_REPLICATE_ERR_IAM_FAILED    = -7
} self_replicate_status_t;

/* ==================================================================== */
/* Part A -- Sandbox health tracking + predictive auto-heal (Niveau 46/48/49) */
/* ==================================================================== */

#define SELF_REPLICATE_MAX_SANDBOXES ACTOR_MAX_COUNT /* one sandbox per actor -- see file header */
#define SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD 3u
#define SELF_REPLICATE_LATENCY_THRESHOLD_CYCLES   1000000ULL /* opaque cycle units -- caller-defined meaning, same posture as swarm_tick */

typedef struct {
    _Atomic uint32_t bounds_violation_count;
    _Atomic uint64_t last_latency_cycles;
    _Atomic uint32_t heals_total; /* diagnostics: how many times agn_predictive_auto_heal() actually acted */
    bool             active;
} self_replicate_sandbox_t;

typedef struct {
    self_replicate_sandbox_t sandboxes[SELF_REPLICATE_MAX_SANDBOXES]; /* direct-indexed by actor_id: O(1) */
} self_replicate_sandbox_registry_t;

void self_replicate_sandbox_registry_init(self_replicate_sandbox_registry_t *reg);

/* Boot-time/deploy-time: activates sandboxes[actor_id]'s health tracking
 * (idempotent: safe to call again on an already-active sandbox, e.g. on
 * redeploy -- resets its counters). */
self_replicate_status_t self_replicate_sandbox_activate(self_replicate_sandbox_registry_t *reg, uint8_t actor_id);

/* O(1) atomic increments/stores, any number of concurrent callers -- a
 * caller reports what IT already detected (an out-of-bounds access it
 * caught, an operation's observed cycle cost); this file does not
 * itself instrument or intercept anything. */
self_replicate_status_t self_replicate_sandbox_report_bounds_violation(self_replicate_sandbox_registry_t *reg, uint8_t actor_id);
self_replicate_status_t self_replicate_sandbox_report_latency(self_replicate_sandbox_registry_t *reg, uint8_t actor_id, uint64_t observed_cycles);

/*
 * O(1), bounded: if sandboxes[actor_id]'s bounds-violation count or last
 * reported latency has crossed its fixed threshold, applies the
 * "binary erasure mask" -- zeroes `sys`'s actor[actor_id].private_memory
 * in full (ACTOR_PRIVATE_MEM_SIZE, a compile-time constant) and drains
 * its mailbox (bounded by RING_BUFFER_CAPACITY, discarding whatever was
 * in flight) -- then resets the sandbox's counters to a clean state.
 * SELF_REPLICATE_OK and a no-op if neither threshold is crossed (nothing
 * to heal). See the file header for precisely what "predictive" does and
 * does not mean here.
 */
self_replicate_status_t agn_predictive_auto_heal(self_replicate_sandbox_registry_t *reg, actor_system_t *sys, uint8_t actor_id);

/*
 * Additive audit wrapper (Niveau 53) around agn_predictive_auto_heal():
 * calls it completely unchanged, then -- iff it actually performed a heal
 * on THIS call (detected by reading sandboxes[actor_id].heals_total
 * before and after, never by re-deriving "did it trigger" a second time)
 * -- appends one tf_log_t entry (telemetry_fabric.h) recording actor_id
 * and the sandbox's post-heal heals_total, via tf_log_append()'s own
 * FNV-1a/64 checksum. Same additive-wrapper posture as this file's own
 * agn_execute_pure_replication() (which audits deploy outcomes) and
 * telemetry_fabric.h's mesh_dispatch_audited()/brain_billing_charge_
 * audited() -- the underlying function's signature and behavior are
 * unchanged; nothing that already calls agn_predictive_auto_heal()
 * directly needs to change.
 *
 * audit_log may be NULL (heals without an audit trail -- identical to
 * calling agn_predictive_auto_heal() directly). A NULL/out-of-range reg/
 * actor_id is NOT re-validated here; it is passed straight through to
 * agn_predictive_auto_heal() so the exact same error code comes back,
 * never duplicated or drifted from that function's own validation.
 *
 * Returns whatever agn_predictive_auto_heal() returned.
 */
self_replicate_status_t agn_predictive_auto_heal_audited(self_replicate_sandbox_registry_t *reg, actor_system_t *sys,
                                                           uint8_t actor_id, tf_log_t *audit_log);

/* ==================================================================== */
/* Part B -- Immutable SaaS-template dictionary + deterministic generator */
/* (Niveau 44/45/47)                                                     */
/* ==================================================================== */

typedef enum {
    SAAS_TEMPLATE_CRM       = 0,
    SAAS_TEMPLATE_ECOMMERCE = 1,
    SAAS_TEMPLATE_BLOG_CMS  = 2,
    SAAS_TEMPLATE_HELPDESK  = 3,
    SAAS_TEMPLATE_BOOKING   = 4,
    SAAS_TEMPLATE_ANALYTICS = 5,
    SAAS_TEMPLATE_BILLING   = 6,
    SAAS_TEMPLATE_GENERIC   = 7
} saas_template_id_t;

#define SELF_REPLICATE_TEMPLATE_COUNT 8u
#define SELF_REPLICATE_TEMPLATE_NAME_MAX 32u

typedef struct {
    char     name[SELF_REPLICATE_TEMPLATE_NAME_MAX];
    uint32_t endpoint_count;
    uint32_t table_count;
    uint32_t default_scope_mask; /* brain_core.h-style OAuth2 scope bitmask this archetype grants by default */
} saas_template_t;

/* O(1) direct-index copy out of the immutable, compile-time template
 * dictionary (true `static const` -- never written to after
 * compilation, unlike karo_interface.h's runtime-provisioned key pool). */
self_replicate_status_t self_replicate_get_template(saas_template_id_t template_id, saas_template_t *out);

/* "Un unique fichier de configuration structurel compact": a fixed
 * 12-byte struct, not a parsed text/JSON format -- the same
 * fixed-layout-over-a-generic-parser boundary wire_proto.h already
 * draws for its own wire format. */
typedef struct {
    saas_template_id_t template_id;
    uint16_t           tenant_id;
    uint32_t           feature_flags; /* opaque, caller-defined bits layered on top of the template's default_scope_mask */
} saas_config_t;

/* The deterministically assembled result -- "les blocs binaires" this
 * generator "assembles" is precisely this fixed-layout struct, composed
 * from the template dictionary plus the caller's config: zero text/code
 * generation, hence zero hallucination surface, per the file header. */
typedef struct {
    saas_template_id_t template_id;
    uint16_t           tenant_id;
    uint32_t           endpoint_count;
    uint32_t           table_count;
    uint32_t           scope_mask;
} saas_manifest_t;

/*
 * Pure and deterministic: identical `*config` always produces a
 * byte-identical `*out_manifest`, from any thread, with no shared
 * mutable state touched (the template dictionary is read-only). O(1):
 * one bounds-checked array lookup plus a few integer operations.
 */
self_replicate_status_t self_replicate_generate_saas(const saas_config_t *config, saas_manifest_t *out_manifest);

/* ==================================================================== */
/* Part C -- CPUID-adaptive kernel selection (Niveau 50)                 */
/* ==================================================================== */

typedef enum {
    SELF_REPLICATE_KERNEL_SCALAR = 0,
    SELF_REPLICATE_KERNEL_AVX2   = 1
} self_replicate_kernel_variant_t;

/* Reuses sig_detect_cpu_features() (signal_core.h) verbatim -- real
 * CPUID/architectural detection, no code generation. Returns which of
 * this codebase's two ACTUALLY EXISTING compiled kernels
 * (audio_transcribe.c's scalar/AVX2 FIR+Goertzel) applies; if
 * `out_features_detected` is non-NULL, the FULL raw feature set
 * (including avx512f/amx_tile/neon) is copied out honestly, even though
 * no dedicated kernel variant for those three exists in this codebase
 * yet -- see the file header. */
self_replicate_kernel_variant_t self_replicate_select_kernel_variant(sig_cpu_features_t *out_features_detected);

/* ==================================================================== */
/* Part D -- agn_execute_pure_replication: the orchestration capstone    */
/* (Niveau 50)                                                           */
/* ==================================================================== */

/* "Flux de commande binaire" -- a fixed-size struct, per the same
 * fixed-layout posture as saas_config_t above. */
typedef struct {
    saas_config_t saas_config;
    uint8_t       node_id;                       /* actor_id AND mesh node_id -- one-to-one, per infra_mesh.h's own "one mesh node == one actor_t" */
    uint32_t      mesh_internal_ip;
    uint8_t       mesh_secret[MESH_SECRET_LEN];
    uint16_t      iam_token_id;
    uint8_t       iam_secret[BRAIN_IAM_SECRET_LEN];
} agn_replication_command_t;

typedef struct {
    saas_manifest_t                 manifest;
    uint8_t                         node_id;
    uint16_t                        iam_token_id;
    self_replicate_kernel_variant_t kernel_variant;
    bool                            overlay_staged;
} agn_replication_result_t;

/*
 * Composes, in order: self_replicate_generate_saas() (Part B),
 * actor_system_spawn() (actor_core.h -- ACTOR_ERR_ALREADY_ACTIVE is
 * treated as "already deployed", not a failure: idempotent redeploy),
 * mesh_register_node() (infra_mesh.h -- MESH_ERR_DUPLICATE_NODE
 * likewise treated as idempotent), brain_iam_register_token()
 * (brain_core.h -- BRAIN_ERR_ALREADY_REGISTERED likewise),
 * self_replicate_sandbox_activate() (Part A),
 * self_replicate_select_kernel_variant() (Part C), and, if `overlay` is
 * non-NULL, ONE karo_overlay_push() staging this deployment's dashboard
 * tile (karo_interface.h -- "120 FPS" caveat unchanged, see file
 * header). If `audit_log` is non-NULL, appends one tf_log_t entry
 * (telemetry_fabric.h) summarizing the outcome. Any real failure (not
 * one of the three idempotent cases above) stops immediately and is
 * returned as-is; already-completed earlier steps are NOT rolled back
 * (each one is independently idempotent to retry, so a caller can
 * safely call this again with the same command after fixing whatever
 * failed).
 */
self_replicate_status_t agn_execute_pure_replication(actor_system_t *sys, mesh_registry_t *mesh_reg,
                                                       brain_iam_registry_t *iam_reg,
                                                       self_replicate_sandbox_registry_t *sandbox_reg,
                                                       tf_log_t *audit_log, karo_overlay_t *overlay,
                                                       const agn_replication_command_t *cmd,
                                                       agn_replication_result_t *out_result);

/* ==================================================================== */
/* Part E -- Signal anomaly -> self-heal coupling (Niveaux 51/52 x 41)   */
/* ==================================================================== */

/*
 * The one place this codebase wires signal_tracking.h's per-sample
 * anomaly indicators into Part A's threshold-triggered auto-heal. This
 * function does not reimplement anything from either side: it is a thin
 * caller-driven bridge, the same "compose, never re-derive" posture as
 * agn_execute_pure_replication() above.
 *
 * `iq_was_outlier`: pass the `*out_was_outlier` iq_filter_sample() (or
 * iq_filter_bank_process_all()) already produced for this actor_id's RF
 * channel this cycle.
 * `track_status`: pass the sigtrack_status_t track_update_position() or
 * track_predict_position() already returned for this actor_id's target
 * this cycle.
 *
 * If NEITHER indicates an anomaly (iq_was_outlier == false AND
 * track_status != SIGTRACK_ERR_OVERFLOW), this is a no-op returning
 * SELF_REPLICATE_OK -- exactly one benign sample/report is never enough
 * to heal on its own, by design (see self_replicate_sandbox_report_
 * bounds_violation()'s SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD).
 *
 * If EITHER indicates an anomaly, this calls
 * self_replicate_sandbox_report_bounds_violation() once (folding "a
 * statistically implausible I/Q sample" and "a Q21.10 result that would
 * not fit in int32_t" into the SAME existing bounds-violation counter --
 * both are, precisely, a caller-detected value outside the bounds this
 * codebase can safely represent or trust, which is exactly what that
 * counter already means for Part A, not a new concept), then calls
 * agn_predictive_auto_heal() unconditionally, which is itself a no-op
 * unless/until the threshold is actually crossed. Repeated anomalies
 * (>= SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD) therefore culminate in
 * the real, existing O(1) memset()-based "remise a zero structurelle" of
 * actor_id's private_memory plus a mailbox drain -- precisely the
 * "overflow ou corruption detectee -> memset en RAM" coupling requested,
 * built entirely out of Part A's already-tested mechanism.
 *
 * Returns whatever agn_predictive_auto_heal() returns (SELF_REPLICATE_OK
 * on a no-op OR a successful heal), or the bounds-violation report call's
 * own error (SELF_REPLICATE_ERR_INVALID/_OUT_OF_RANGE) if that failed
 * first.
 */
self_replicate_status_t self_replicate_couple_signal_anomaly(self_replicate_sandbox_registry_t *reg,
                                                               actor_system_t *sys, uint8_t actor_id,
                                                               bool iq_was_outlier,
                                                               sigtrack_status_t track_status);

/*
 * Additive audit wrapper (Niveau 53) around self_replicate_couple_signal_
 * anomaly(): identical anomaly logic (a single benign report is still a
 * no-op), but the escalation call is agn_predictive_auto_heal_audited()
 * instead of the bare function -- so a signal-anomaly-triggered
 * structural erasure (the misaligned-frame/I-Q-outlier/Q21.10-overflow
 * case this mission's Part 2 describes) is logged to telemetry_fabric.c
 * exactly like any other self-heal, via the SAME FNV-1a-checksummed
 * tf_log_append() call, not a second logging mechanism. audit_log may be
 * NULL. Returns whatever the escalation call returned (or the bounds-
 * violation report's own error, same as the un-audited function).
 */
self_replicate_status_t self_replicate_couple_signal_anomaly_audited(self_replicate_sandbox_registry_t *reg,
                                                                       actor_system_t *sys, uint8_t actor_id,
                                                                       bool iq_was_outlier,
                                                                       sigtrack_status_t track_status,
                                                                       tf_log_t *audit_log);

#endif /* SELF_REPLICATE_H */
