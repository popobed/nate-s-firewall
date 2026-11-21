/*
 * self_replicate.c
 *
 * See include/self_replicate.h for the full design and honest-scope
 * notes. Implementation only; non-obvious decisions are explained there,
 * not duplicated here.
 */

#include "self_replicate.h"

#include <string.h>

/* ==================================================================== */
/* Part A -- Sandbox health tracking + predictive auto-heal              */
/* ==================================================================== */

void self_replicate_sandbox_registry_init(self_replicate_sandbox_registry_t *reg) {
    for (uint32_t i = 0; i < SELF_REPLICATE_MAX_SANDBOXES; i++) {
        atomic_store_explicit(&reg->sandboxes[i].bounds_violation_count, 0u, memory_order_relaxed);
        atomic_store_explicit(&reg->sandboxes[i].last_latency_cycles, 0u, memory_order_relaxed);
        atomic_store_explicit(&reg->sandboxes[i].heals_total, 0u, memory_order_relaxed);
        reg->sandboxes[i].active = false;
    }
}

self_replicate_status_t self_replicate_sandbox_activate(self_replicate_sandbox_registry_t *reg, uint8_t actor_id) {
    if (reg == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }
    if (actor_id >= SELF_REPLICATE_MAX_SANDBOXES) {
        return SELF_REPLICATE_ERR_OUT_OF_RANGE;
    }
    self_replicate_sandbox_t *sb = &reg->sandboxes[actor_id];
    atomic_store_explicit(&sb->bounds_violation_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&sb->last_latency_cycles, 0u, memory_order_relaxed);
    sb->active = true;
    return SELF_REPLICATE_OK;
}

self_replicate_status_t self_replicate_sandbox_report_bounds_violation(self_replicate_sandbox_registry_t *reg, uint8_t actor_id) {
    if (reg == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }
    if (actor_id >= SELF_REPLICATE_MAX_SANDBOXES) {
        return SELF_REPLICATE_ERR_OUT_OF_RANGE;
    }
    atomic_fetch_add_explicit(&reg->sandboxes[actor_id].bounds_violation_count, 1u, memory_order_acq_rel);
    return SELF_REPLICATE_OK;
}

self_replicate_status_t self_replicate_sandbox_report_latency(self_replicate_sandbox_registry_t *reg, uint8_t actor_id, uint64_t observed_cycles) {
    if (reg == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }
    if (actor_id >= SELF_REPLICATE_MAX_SANDBOXES) {
        return SELF_REPLICATE_ERR_OUT_OF_RANGE;
    }
    atomic_store_explicit(&reg->sandboxes[actor_id].last_latency_cycles, observed_cycles, memory_order_release);
    return SELF_REPLICATE_OK;
}

self_replicate_status_t agn_predictive_auto_heal(self_replicate_sandbox_registry_t *reg, actor_system_t *sys, uint8_t actor_id) {
    if (reg == NULL || sys == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }
    if (actor_id >= SELF_REPLICATE_MAX_SANDBOXES) {
        return SELF_REPLICATE_ERR_OUT_OF_RANGE;
    }
    self_replicate_sandbox_t *sb = &reg->sandboxes[actor_id];

    uint32_t violations = atomic_load_explicit(&sb->bounds_violation_count, memory_order_acquire);
    uint64_t latency = atomic_load_explicit(&sb->last_latency_cycles, memory_order_acquire);
    bool trigger = (violations >= SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD) ||
                   (latency >= SELF_REPLICATE_LATENCY_THRESHOLD_CYCLES);
    if (!trigger) {
        return SELF_REPLICATE_OK; /* nothing to heal */
    }

    actor_t *actor = actor_system_get(sys, actor_id);
    if (actor == NULL) {
        return SELF_REPLICATE_ERR_NOT_FOUND;
    }

    /* The "binary erasure mask": one bounded, O(1) zero-fill of this
     * actor's entire SRAM-style private memory (a fixed compile-time
     * size), discarding whatever might be corrupted and resetting to a
     * known-safe zero state. */
    memset(actor->private_memory, 0, ACTOR_PRIVATE_MEM_SIZE);

    /* Drain and discard whatever is still in flight in its mailbox --
     * bounded by RING_BUFFER_CAPACITY, a small fixed compile-time
     * constant, never an unbounded wait. */
    ring_msg_t discard;
    while (actor_process_one(actor, &discard) == ACTOR_OK) {
        /* discard: this in-flight traffic belonged to the state we just
         * decided was untrustworthy */
    }

    atomic_store_explicit(&sb->bounds_violation_count, 0u, memory_order_release);
    atomic_store_explicit(&sb->last_latency_cycles, 0u, memory_order_release);
    atomic_fetch_add_explicit(&sb->heals_total, 1u, memory_order_relaxed);
    return SELF_REPLICATE_OK;
}

#define SELF_REPLICATE_TF_EVENT_SELF_HEAL 0x48454C00u /* "HEL\0" as bytes */

typedef struct {
    uint8_t  actor_id;
    uint32_t heals_total;
} self_replicate_tf_heal_payload_t;

self_replicate_status_t agn_predictive_auto_heal_audited(self_replicate_sandbox_registry_t *reg, actor_system_t *sys,
                                                           uint8_t actor_id, tf_log_t *audit_log) {
    if (reg == NULL || actor_id >= SELF_REPLICATE_MAX_SANDBOXES) {
        return agn_predictive_auto_heal(reg, sys, actor_id); /* precise error code, not duplicated here */
    }

    /* Diagnostic-only read, same memory_order_relaxed treatment
     * heals_total already gets everywhere else in this file -- this
     * counter is never used to synchronize any other data. */
    uint32_t heals_before = atomic_load_explicit(&reg->sandboxes[actor_id].heals_total, memory_order_relaxed);
    self_replicate_status_t st = agn_predictive_auto_heal(reg, sys, actor_id);
    if (st != SELF_REPLICATE_OK || audit_log == NULL) {
        return st;
    }

    uint32_t heals_after = atomic_load_explicit(&reg->sandboxes[actor_id].heals_total, memory_order_relaxed);
    if (heals_after != heals_before) {
        self_replicate_tf_heal_payload_t payload;
        payload.actor_id = actor_id;
        payload.heals_total = heals_after;
        /* Best-effort: a full audit log (TF_ERR_FULL) must never block or
         * fail a real heal that already happened -- same posture as
         * agn_execute_pure_replication()'s own audit_log call below. */
        (void)tf_log_append(audit_log, SELF_REPLICATE_TF_EVENT_SELF_HEAL, &payload, (uint32_t)sizeof payload);
    }
    return st;
}

/* ==================================================================== */
/* Part B -- Immutable SaaS-template dictionary + deterministic generator */
/* ==================================================================== */

/* True `static const` -- read-only, compile-time data, never written to
 * after compilation (unlike karo_interface.h's runtime-provisioned key
 * pool, which cannot honestly make the same claim -- see that header). */
static const _Alignas(SELF_REPLICATE_CACHE_LINE_SIZE) saas_template_t g_saas_templates[SELF_REPLICATE_TEMPLATE_COUNT] = {
    { "crm",       12u, 6u,  0x7u },
    { "ecommerce", 20u, 10u, 0x7u },
    { "blog-cms",   8u, 4u,  0x3u },
    { "helpdesk",  14u, 7u,  0x7u },
    { "booking",   16u, 8u,  0x7u },
    { "analytics", 10u, 5u,  0x3u },
    { "billing",    9u, 5u,  0x7u },
    { "generic",    6u, 3u,  0x1u }
};

self_replicate_status_t self_replicate_get_template(saas_template_id_t template_id, saas_template_t *out) {
    if (out == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }
    if ((unsigned)template_id >= SELF_REPLICATE_TEMPLATE_COUNT) {
        return SELF_REPLICATE_ERR_OUT_OF_RANGE;
    }
    *out = g_saas_templates[template_id];
    return SELF_REPLICATE_OK;
}

self_replicate_status_t self_replicate_generate_saas(const saas_config_t *config, saas_manifest_t *out_manifest) {
    if (config == NULL || out_manifest == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }
    if ((unsigned)config->template_id >= SELF_REPLICATE_TEMPLATE_COUNT) {
        return SELF_REPLICATE_ERR_OUT_OF_RANGE;
    }
    const saas_template_t *tmpl = &g_saas_templates[config->template_id];

    /* Zero the WHOLE struct first, padding included, before filling the
     * named fields below -- saas_manifest_t has a real gap between
     * tenant_id (uint16_t) and endpoint_count (uint32_t) that field-by-
     * field assignment alone never touches. Left uninitialized, that gap
     * holds whatever garbage happened to be on the caller's stack, which
     * makes a whole-struct memcmp() (or hash) of two otherwise-identical
     * manifests non-deterministic -- the exact bug class this project's
     * own SESSION_NOTES.md already documents once for audio_feature_vec_t,
     * fixed the same way there: make the full byte representation
     * deterministic, not just the named fields. */
    memset(out_manifest, 0, sizeof *out_manifest);

    out_manifest->template_id = config->template_id;
    out_manifest->tenant_id = config->tenant_id;
    out_manifest->endpoint_count = tmpl->endpoint_count;
    out_manifest->table_count = tmpl->table_count;
    out_manifest->scope_mask = tmpl->default_scope_mask | config->feature_flags;
    return SELF_REPLICATE_OK;
}

/* ==================================================================== */
/* Part C -- CPUID-adaptive kernel selection                             */
/* ==================================================================== */

self_replicate_kernel_variant_t self_replicate_select_kernel_variant(sig_cpu_features_t *out_features_detected) {
    sig_cpu_features_t feats = sig_detect_cpu_features();
    if (out_features_detected != NULL) {
        *out_features_detected = feats;
    }
    return feats.avx2 ? SELF_REPLICATE_KERNEL_AVX2 : SELF_REPLICATE_KERNEL_SCALAR;
}

/* ==================================================================== */
/* Part D -- agn_execute_pure_replication: the orchestration capstone    */
/* ==================================================================== */

#define SELF_REPLICATE_TF_EVENT_DEPLOY 0x41474E00u /* "AGN\0" as bytes */

typedef struct {
    uint8_t  node_id;
    uint16_t tenant_id;
    uint16_t iam_token_id;
    int16_t  status;
} self_replicate_tf_audit_payload_t;

self_replicate_status_t agn_execute_pure_replication(actor_system_t *sys, mesh_registry_t *mesh_reg,
                                                       brain_iam_registry_t *iam_reg,
                                                       self_replicate_sandbox_registry_t *sandbox_reg,
                                                       tf_log_t *audit_log, karo_overlay_t *overlay,
                                                       const agn_replication_command_t *cmd,
                                                       agn_replication_result_t *out_result) {
    if (sys == NULL || mesh_reg == NULL || iam_reg == NULL || sandbox_reg == NULL ||
        cmd == NULL || out_result == NULL) {
        return SELF_REPLICATE_ERR_INVALID;
    }

    self_replicate_status_t final_status = SELF_REPLICATE_OK;

    self_replicate_status_t gen_st = self_replicate_generate_saas(&cmd->saas_config, &out_result->manifest);
    if (gen_st != SELF_REPLICATE_OK) {
        return gen_st;
    }

    actor_status_t spawn_st = actor_system_spawn(sys, cmd->node_id);
    if (spawn_st != ACTOR_OK && spawn_st != ACTOR_ERR_ALREADY_ACTIVE) {
        final_status = SELF_REPLICATE_ERR_SPAWN_FAILED;
    }

    if (final_status == SELF_REPLICATE_OK) {
        mesh_status_t mst = mesh_register_node(mesh_reg, cmd->node_id, cmd->mesh_internal_ip,
                                                cmd->mesh_secret, cmd->node_id);
        if (mst != MESH_OK && mst != MESH_ERR_DUPLICATE_NODE) {
            final_status = SELF_REPLICATE_ERR_MESH_FAILED;
        }
    }

    if (final_status == SELF_REPLICATE_OK) {
        brain_status_t bst = brain_iam_register_token(iam_reg, cmd->iam_token_id, (uint16_t)cmd->node_id,
                                                       cmd->iam_secret, out_result->manifest.scope_mask);
        if (bst != BRAIN_OK && bst != BRAIN_ERR_ALREADY_REGISTERED) {
            final_status = SELF_REPLICATE_ERR_IAM_FAILED;
        }
    }

    if (final_status == SELF_REPLICATE_OK) {
        (void)self_replicate_sandbox_activate(sandbox_reg, cmd->node_id);
        out_result->node_id = cmd->node_id;
        out_result->iam_token_id = cmd->iam_token_id;
        out_result->kernel_variant = self_replicate_select_kernel_variant(NULL);

        out_result->overlay_staged = false;
        if (overlay != NULL) {
            karo_status_t ost = karo_overlay_push(overlay, (float)cmd->saas_config.tenant_id, 0.0f, 1.0f,
                                                   0u, NULL);
            out_result->overlay_staged = (ost == KARO_OK);
        }
    }

    if (audit_log != NULL) {
        self_replicate_tf_audit_payload_t payload;
        payload.node_id = cmd->node_id;
        payload.tenant_id = cmd->saas_config.tenant_id;
        payload.iam_token_id = cmd->iam_token_id;
        payload.status = (int16_t)final_status;
        (void)tf_log_append(audit_log, SELF_REPLICATE_TF_EVENT_DEPLOY, &payload, (uint32_t)sizeof payload);
    }

    return final_status;
}

/* ==================================================================== */
/* Part E -- Signal anomaly -> self-heal coupling                        */
/* ==================================================================== */

self_replicate_status_t self_replicate_couple_signal_anomaly(self_replicate_sandbox_registry_t *reg,
                                                               actor_system_t *sys, uint8_t actor_id,
                                                               bool iq_was_outlier,
                                                               sigtrack_status_t track_status) {
    /* NULL/out-of-range args: let the calls below produce the usual
     * SELF_REPLICATE_ERR_INVALID/_OUT_OF_RANGE rather than duplicating
     * that validation here. */
    bool anomaly = iq_was_outlier || (track_status == SIGTRACK_ERR_OVERFLOW);
    if (!anomaly) {
        return SELF_REPLICATE_OK; /* one benign sample/report: nothing to escalate */
    }

    self_replicate_status_t report_st = self_replicate_sandbox_report_bounds_violation(reg, actor_id);
    if (report_st != SELF_REPLICATE_OK) {
        return report_st;
    }

    /* No-op unless/until SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD is
     * actually crossed -- see agn_predictive_auto_heal() above. */
    return agn_predictive_auto_heal(reg, sys, actor_id);
}

self_replicate_status_t self_replicate_couple_signal_anomaly_audited(self_replicate_sandbox_registry_t *reg,
                                                                       actor_system_t *sys, uint8_t actor_id,
                                                                       bool iq_was_outlier,
                                                                       sigtrack_status_t track_status,
                                                                       tf_log_t *audit_log) {
    bool anomaly = iq_was_outlier || (track_status == SIGTRACK_ERR_OVERFLOW);
    if (!anomaly) {
        return SELF_REPLICATE_OK; /* one benign sample/report: nothing to escalate */
    }

    self_replicate_status_t report_st = self_replicate_sandbox_report_bounds_violation(reg, actor_id);
    if (report_st != SELF_REPLICATE_OK) {
        return report_st;
    }

    /* The one difference from the un-audited function: a heal that
     * actually fires here is logged to telemetry_fabric.c. */
    return agn_predictive_auto_heal_audited(reg, sys, actor_id, audit_log);
}
