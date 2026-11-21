/*
 * test_self_replicate.c - Unit + true multi-thread stress test bench for
 * self_replicate.{h,c}: sandbox health tracking + predictive auto-heal
 * (real memory erasure + mailbox drain), the immutable SaaS-template
 * dictionary + deterministic generator (including the mandated 8-thread,
 * 3-architecture concurrent generation stress test proving exact
 * structural conservation), CPUID-adaptive kernel selection, and
 * agn_execute_pure_replication()'s end-to-end composition of every
 * underlying module.
 *
 * No dynamic allocation anywhere in this file: all storage is static or
 * on the stack, matching self_replicate.c's own zero-allocation
 * constraint. Portable threading (#if _WIN32 CreateThread / else
 * pthread), same pattern as tests/test_barry_storage.c.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "self_replicate.h"

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
/* Part A: sandbox health tracking + predictive auto-heal              */
/* ---------------------------------------------------------------- */

static void test_auto_heal_noop_below_threshold(void) {
    SECTION("agn_predictive_auto_heal: a no-op while thresholds are not crossed");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 0u) == SELF_REPLICATE_OK);

    CHECK(self_replicate_sandbox_report_bounds_violation(&reg, 0u) == SELF_REPLICATE_OK);
    CHECK(self_replicate_sandbox_report_bounds_violation(&reg, 0u) == SELF_REPLICATE_OK); /* 2 < threshold (3) */

    CHECK(agn_predictive_auto_heal(&reg, &sys, 0u) == SELF_REPLICATE_OK);
    CHECK(atomic_load_explicit(&reg.sandboxes[0].heals_total, memory_order_relaxed) == 0u); /* never triggered */
}

static void test_auto_heal_triggers_on_bounds_violations(void) {
    SECTION("agn_predictive_auto_heal: bounds-violation threshold triggers a real memory erasure + mailbox drain");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 1u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 1u) == SELF_REPLICATE_OK);

    actor_t *actor = actor_system_get(&sys, 1u);
    CHECK(actor != NULL);
    memset(actor->private_memory, 0xAB, ACTOR_PRIVATE_MEM_SIZE); /* simulate "possibly corrupted" state */
    ring_msg_t *slot = NULL;
    CHECK(actor_mailbox_reserve(actor, &slot) == ACTOR_OK);
    actor_mailbox_publish(actor, slot, 42u, 1u); /* something in flight */
    CHECK(actor_mailbox_count(actor) == 1u);

    for (uint32_t i = 0; i < SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD; i++) {
        CHECK(self_replicate_sandbox_report_bounds_violation(&reg, 1u) == SELF_REPLICATE_OK);
    }
    CHECK(agn_predictive_auto_heal(&reg, &sys, 1u) == SELF_REPLICATE_OK);

    uint8_t zero[ACTOR_PRIVATE_MEM_SIZE];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(actor->private_memory, zero, ACTOR_PRIVATE_MEM_SIZE) == 0); /* the erasure mask */
    CHECK(actor_mailbox_count(actor) == 0u); /* drained */
    CHECK(atomic_load_explicit(&reg.sandboxes[1].bounds_violation_count, memory_order_relaxed) == 0u); /* reset */
    CHECK(atomic_load_explicit(&reg.sandboxes[1].heals_total, memory_order_relaxed) == 1u);
}

static void test_auto_heal_triggers_on_latency(void) {
    SECTION("agn_predictive_auto_heal: latency threshold triggers the same erasure");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 2u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 2u) == SELF_REPLICATE_OK);

    CHECK(self_replicate_sandbox_report_latency(&reg, 2u, SELF_REPLICATE_LATENCY_THRESHOLD_CYCLES) == SELF_REPLICATE_OK);
    CHECK(agn_predictive_auto_heal(&reg, &sys, 2u) == SELF_REPLICATE_OK);
    CHECK(atomic_load_explicit(&reg.sandboxes[2].heals_total, memory_order_relaxed) == 1u);
    CHECK(atomic_load_explicit(&reg.sandboxes[2].last_latency_cycles, memory_order_relaxed) == 0u); /* reset */
}

static void test_sandbox_out_of_range(void) {
    SECTION("sandbox functions: reject an out-of-range actor_id");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    uint8_t bad = (uint8_t)SELF_REPLICATE_MAX_SANDBOXES;
    CHECK(self_replicate_sandbox_activate(&reg, bad) == SELF_REPLICATE_ERR_OUT_OF_RANGE);
    CHECK(self_replicate_sandbox_report_bounds_violation(&reg, bad) == SELF_REPLICATE_ERR_OUT_OF_RANGE);
    CHECK(self_replicate_sandbox_report_latency(&reg, bad, 0u) == SELF_REPLICATE_ERR_OUT_OF_RANGE);
    CHECK(agn_predictive_auto_heal(&reg, &sys, bad) == SELF_REPLICATE_ERR_OUT_OF_RANGE);
}

/* ---------------------------------------------------------------- */
/* Part E: Signal anomaly -> self-heal coupling, plain and telemetry-    */
/* audited (Niveau 53). Previously untested in this file.               */
/* ---------------------------------------------------------------- */

static void test_couple_signal_anomaly_noop_on_benign_report(void) {
    SECTION("self_replicate_couple_signal_anomaly: a single benign report is a no-op, nothing escalated");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 0u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 0u) == SELF_REPLICATE_OK);

    CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 0u, false, SIGTRACK_OK) == SELF_REPLICATE_OK);
    CHECK(atomic_load_explicit(&reg.sandboxes[0].bounds_violation_count, memory_order_relaxed) == 0u);
    CHECK(atomic_load_explicit(&reg.sandboxes[0].heals_total, memory_order_relaxed) == 0u);
}

static void test_couple_signal_anomaly_escalates_on_repeated_anomaly(void) {
    SECTION("self_replicate_couple_signal_anomaly: repeated I/Q outliers + a Q21.10 overflow cross the threshold and heal");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    CHECK(actor_system_spawn(&sys, 1u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 1u) == SELF_REPLICATE_OK);

    actor_t *actor = actor_system_get(&sys, 1u);
    CHECK(actor != NULL);
    memset(actor->private_memory, 0xCD, ACTOR_PRIVATE_MEM_SIZE); /* simulate corrupted state */

    CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 1u, true, SIGTRACK_OK) == SELF_REPLICATE_OK);          /* 1: I/Q outlier */
    CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 1u, false, SIGTRACK_ERR_OVERFLOW) == SELF_REPLICATE_OK); /* 2: Q21.10 overflow */
    CHECK(atomic_load_explicit(&reg.sandboxes[1].heals_total, memory_order_relaxed) == 0u); /* 2 < threshold (3) */

    CHECK(self_replicate_couple_signal_anomaly(&reg, &sys, 1u, true, SIGTRACK_OK) == SELF_REPLICATE_OK); /* 3: crosses threshold */

    uint8_t zero[ACTOR_PRIVATE_MEM_SIZE];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(actor->private_memory, zero, ACTOR_PRIVATE_MEM_SIZE) == 0); /* the erasure mask fired */
    CHECK(atomic_load_explicit(&reg.sandboxes[1].heals_total, memory_order_relaxed) == 1u);
}

static void test_auto_heal_audited_logs_only_on_real_heal(void) {
    SECTION("agn_predictive_auto_heal_audited: appends one FNV-1a-checksummed telemetry entry iff a heal actually occurred");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    tf_log_t audit_log;
    tf_log_init(&audit_log);
    CHECK(actor_system_spawn(&sys, 2u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 2u) == SELF_REPLICATE_OK);

    /* Below threshold: a real no-op, no log entry. */
    CHECK(self_replicate_sandbox_report_bounds_violation(&reg, 2u) == SELF_REPLICATE_OK);
    CHECK(agn_predictive_auto_heal_audited(&reg, &sys, 2u, &audit_log) == SELF_REPLICATE_OK);
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 0u);

    /* Cross the threshold: a real heal, exactly one audit entry, and the
     * FNV-1a hash chain over it is genuinely valid, not just present. */
    for (uint32_t i = 0; i < SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD; i++) {
        CHECK(self_replicate_sandbox_report_bounds_violation(&reg, 2u) == SELF_REPLICATE_OK);
    }
    CHECK(agn_predictive_auto_heal_audited(&reg, &sys, 2u, &audit_log) == SELF_REPLICATE_OK);
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 1u);
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);
    CHECK(audit_log.entries[0].event_type == 0x48454C00u);

    /* NULL audit_log is accepted: heal proceeds, no entry appended. */
    for (uint32_t i = 0; i < SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD; i++) {
        CHECK(self_replicate_sandbox_report_bounds_violation(&reg, 2u) == SELF_REPLICATE_OK);
    }
    CHECK(agn_predictive_auto_heal_audited(&reg, &sys, 2u, NULL) == SELF_REPLICATE_OK);
    CHECK(atomic_load_explicit(&reg.sandboxes[2].heals_total, memory_order_relaxed) == 2u);
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 1u); /* unchanged */
}

static void test_couple_signal_anomaly_audited_logs_the_heal(void) {
    SECTION("self_replicate_couple_signal_anomaly_audited: a signal-triggered heal is logged to telemetry_fabric.c");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    tf_log_t audit_log;
    tf_log_init(&audit_log);
    CHECK(actor_system_spawn(&sys, 3u) == ACTOR_OK);
    CHECK(self_replicate_sandbox_activate(&reg, 3u) == SELF_REPLICATE_OK);

    for (uint32_t i = 0; i < SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD; i++) {
        CHECK(self_replicate_couple_signal_anomaly_audited(&reg, &sys, 3u, true, SIGTRACK_OK, &audit_log) == SELF_REPLICATE_OK);
    }
    CHECK(atomic_load_explicit(&reg.sandboxes[3].heals_total, memory_order_relaxed) == 1u);
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 1u);
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);
}

/* ---------------------------------------------------------------- */
/* 8-thread stress: massive concurrent noisy-packet injection, one      */
/* actor/sandbox/RF-channel per thread (MAX_THREADS == ACTOR_MAX_COUNT  */
/* == 8) -- proves the signal-anomaly -> self-heal -> telemetry_fabric  */
/* coupling stays exact, and that the FNV-1a hash-chained audit log     */
/* stays provably untampered, under real concurrent append pressure.    */
/* ---------------------------------------------------------------- */

#define STRESS_PACKETS_PER_THREAD 200u
#define STRESS_ANOMALY_PERIOD     20u /* anomaly on i % 20 == 0, i > 0 -- i in [0,200): 20,40,...,180 = 9 anomalies/thread */
#define STRESS_HEALS_PER_THREAD   3u  /* 9 anomalies / SELF_REPLICATE_BOUNDS_VIOLATION_THRESHOLD (3) */

typedef struct {
    self_replicate_sandbox_registry_t *reg;
    actor_system_t                    *sys;
    tf_log_t                          *audit_log;
    uint8_t                             actor_id;
} anomaly_stress_args_t;

static THREAD_FN(anomaly_stress_thread) {
    anomaly_stress_args_t *args = (anomaly_stress_args_t *)arg;
    for (uint32_t i = 0; i < STRESS_PACKETS_PER_THREAD; i++) {
        /* Mostly clean traffic ("injection massive de paquets"), a real
         * anomaly every STRESS_ANOMALY_PERIOD packets, alternating which
         * of the two independent anomaly sources (I/Q outlier vs. Q21.10
         * overflow) fires so both paths are exercised under contention. */
        bool is_anomaly = (i % STRESS_ANOMALY_PERIOD == 0u) && (i > 0u);
        bool iq_outlier = is_anomaly && (((i / STRESS_ANOMALY_PERIOD) % 2u) == 0u);
        sigtrack_status_t track_status = (is_anomaly && !iq_outlier) ? SIGTRACK_ERR_OVERFLOW : SIGTRACK_OK;
        (void)self_replicate_couple_signal_anomaly_audited(args->reg, args->sys, args->actor_id,
                                                             iq_outlier, track_status, args->audit_log);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_noisy_packet_injection_self_heals_and_audits(void) {
    SECTION("8 threads x massive noisy-packet injection: exact heal counts, FNV-1a audit chain stays valid under contention");
    self_replicate_sandbox_registry_t reg;
    self_replicate_sandbox_registry_init(&reg);
    actor_system_t sys;
    actor_system_init(&sys);
    tf_log_t audit_log;
    tf_log_init(&audit_log);

    anomaly_stress_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        uint8_t actor_id = (uint8_t)t; /* MAX_THREADS == ACTOR_MAX_COUNT == 8: every actor gets its own thread */
        CHECK(actor_system_spawn(&sys, actor_id) == ACTOR_OK);
        CHECK(self_replicate_sandbox_activate(&reg, actor_id) == SELF_REPLICATE_OK);
        args[t].reg = &reg;
        args[t].sys = &sys;
        args[t].audit_log = &audit_log;
        args[t].actor_id = actor_id;
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        threads[t] = thread_start(anomaly_stress_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    uint64_t total_heals = 0;
    for (int t = 0; t < MAX_THREADS; t++) {
        uint32_t heals = atomic_load_explicit(&reg.sandboxes[t].heals_total, memory_order_relaxed);
        CHECK(heals == STRESS_HEALS_PER_THREAD);
        total_heals += heals;
    }
    CHECK(total_heals == (uint64_t)MAX_THREADS * STRESS_HEALS_PER_THREAD);
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == total_heals);
    /* The real point of this test: under 8-way concurrent tf_log_append()
     * pressure (one WAL, disjoint actors, genuinely overlapping in time),
     * the FNV-1a hash chain is still provably untampered end to end. */
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);
}

/* ---------------------------------------------------------------- */
/* Part B: SaaS-template dictionary + deterministic generator          */
/* ---------------------------------------------------------------- */

static void test_get_template(void) {
    SECTION("self_replicate_get_template: O(1) copy from the immutable dictionary, bounds-checked");
    saas_template_t tmpl;
    CHECK(self_replicate_get_template(SAAS_TEMPLATE_CRM, &tmpl) == SELF_REPLICATE_OK);
    CHECK(strcmp(tmpl.name, "crm") == 0);
    CHECK(tmpl.endpoint_count == 12u);
    CHECK(tmpl.table_count == 6u);

    CHECK(self_replicate_get_template(SAAS_TEMPLATE_GENERIC, &tmpl) == SELF_REPLICATE_OK);
    CHECK(strcmp(tmpl.name, "generic") == 0);

    CHECK(self_replicate_get_template((saas_template_id_t)SELF_REPLICATE_TEMPLATE_COUNT, &tmpl) == SELF_REPLICATE_ERR_OUT_OF_RANGE);
}

static void test_generate_saas_deterministic(void) {
    SECTION("self_replicate_generate_saas: pure and deterministic -- identical config, byte-identical manifest");
    saas_config_t config;
    config.template_id = SAAS_TEMPLATE_ECOMMERCE;
    config.tenant_id = 100u;
    config.feature_flags = 0x8u;

    saas_manifest_t m1, m2;
    CHECK(self_replicate_generate_saas(&config, &m1) == SELF_REPLICATE_OK);
    CHECK(self_replicate_generate_saas(&config, &m2) == SELF_REPLICATE_OK);
    CHECK(memcmp(&m1, &m2, sizeof m1) == 0);

    CHECK(m1.endpoint_count == 20u);
    CHECK(m1.table_count == 10u);
    CHECK(m1.scope_mask == (0x7u | 0x8u)); /* template default | feature_flags */
    CHECK(m1.tenant_id == 100u);
}

static void test_generate_saas_rejects_bad_template(void) {
    SECTION("self_replicate_generate_saas: rejects an out-of-range template_id, leaves *out_manifest untouched");
    saas_config_t config;
    config.template_id = (saas_template_id_t)SELF_REPLICATE_TEMPLATE_COUNT;
    config.tenant_id = 1u;
    config.feature_flags = 0u;

    saas_manifest_t manifest;
    memset(&manifest, 0xEE, sizeof manifest); /* sentinel */
    CHECK(self_replicate_generate_saas(&config, &manifest) == SELF_REPLICATE_ERR_OUT_OF_RANGE);
    uint8_t sentinel[sizeof manifest];
    memset(sentinel, 0xEE, sizeof sentinel);
    CHECK(memcmp(&manifest, sentinel, sizeof manifest) == 0); /* untouched on error */
}

/* ---------------------------------------------------------------- */
/* Part C: CPUID-adaptive kernel selection                             */
/* ---------------------------------------------------------------- */

static void test_select_kernel_variant(void) {
    SECTION("self_replicate_select_kernel_variant: reuses real CPUID detection, reports the full feature set honestly");
    sig_cpu_features_t feats;
    self_replicate_kernel_variant_t variant = self_replicate_select_kernel_variant(&feats);
    CHECK(variant == SELF_REPLICATE_KERNEL_AVX2 || variant == SELF_REPLICATE_KERNEL_SCALAR);
    CHECK((variant == SELF_REPLICATE_KERNEL_AVX2) == feats.avx2);
    printf("   detected: sse2=%d avx2=%d avx512f=%d amx_tile=%d neon=%d -> kernel=%s\n",
           (int)feats.sse2, (int)feats.avx2, (int)feats.avx512f, (int)feats.amx_tile, (int)feats.neon,
           variant == SELF_REPLICATE_KERNEL_AVX2 ? "AVX2" : "SCALAR");

    CHECK(self_replicate_select_kernel_variant(NULL) == variant); /* NULL out-param is optional, same result */
}

/* ---------------------------------------------------------------- */
/* Part D: agn_execute_pure_replication end-to-end                     */
/* ---------------------------------------------------------------- */

static agn_replication_command_t make_command(uint8_t node_id, uint16_t tenant_id, uint16_t iam_token_id) {
    agn_replication_command_t cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.saas_config.template_id = SAAS_TEMPLATE_HELPDESK;
    cmd.saas_config.tenant_id = tenant_id;
    cmd.saas_config.feature_flags = 0x10u;
    cmd.node_id = node_id;
    cmd.mesh_internal_ip = 0x0A000001u + node_id;
    memset(cmd.mesh_secret, (int)(0x40u + node_id), MESH_SECRET_LEN);
    cmd.iam_token_id = iam_token_id;
    memset(cmd.iam_secret, (int)(0x80u + node_id), BRAIN_IAM_SECRET_LEN);
    return cmd;
}

static void test_execute_pure_replication_end_to_end(void) {
    SECTION("agn_execute_pure_replication: composes actor_core+infra_mesh+brain_core+telemetry_fabric+karo_interface end to end");
    actor_system_t sys;
    mesh_registry_t mesh_reg;
    brain_iam_registry_t iam_reg;
    self_replicate_sandbox_registry_t sandbox_reg;
    tf_log_t audit_log;
    karo_overlay_t overlay;

    actor_system_init(&sys);
    mesh_registry_init(&mesh_reg);
    brain_iam_init(&iam_reg);
    self_replicate_sandbox_registry_init(&sandbox_reg);
    tf_log_init(&audit_log);
    karo_overlay_init(&overlay);

    agn_replication_command_t cmd = make_command(3u, 777u, 5u);
    agn_replication_result_t result;
    memset(&result, 0, sizeof result);

    CHECK(agn_execute_pure_replication(&sys, &mesh_reg, &iam_reg, &sandbox_reg, &audit_log, &overlay,
                                        &cmd, &result) == SELF_REPLICATE_OK);

    CHECK(result.node_id == 3u);
    CHECK(result.iam_token_id == 5u);
    CHECK(result.manifest.tenant_id == 777u);
    CHECK(result.manifest.endpoint_count == 14u); /* helpdesk template */
    CHECK(result.overlay_staged == true);
    CHECK(overlay.count == 1u);

    /* the actor was really spawned */
    CHECK(actor_system_get(&sys, 3u) != NULL);

    /* the mesh node was really registered, with the right secret */
    const mesh_node_t *node = mesh_resolve(&mesh_reg, 3u);
    CHECK(node != NULL);
    CHECK(node->internal_ip == cmd.mesh_internal_ip);
    CHECK(node->actor_id == 3u);

    /* the IAM token was really registered and authenticates */
    uint16_t auth_tenant = 0xFFFFu;
    CHECK(brain_iam_authenticate(&iam_reg, 5u, cmd.iam_secret, BRAIN_IAM_SECRET_LEN, 0u, &auth_tenant) == BRAIN_OK);
    CHECK(auth_tenant == 3u); /* tenant_id == node_id in this scheme */

    /* the sandbox was activated */
    CHECK(sandbox_reg.sandboxes[3].active == true);

    /* one audit entry landed in the (still perfectly valid) telemetry chain */
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 1u);
    CHECK(audit_log.entries[0].event_type == 0x41474E00u);
    CHECK(tf_log_verify(&audit_log, NULL) == TF_OK);

    /* idempotent redeploy: calling again with the SAME command succeeds
     * (every underlying registration is treated as "already deployed",
     * not a failure) and still deterministically regenerates the same
     * manifest */
    agn_replication_result_t result2;
    memset(&result2, 0, sizeof result2);
    CHECK(agn_execute_pure_replication(&sys, &mesh_reg, &iam_reg, &sandbox_reg, &audit_log, &overlay,
                                        &cmd, &result2) == SELF_REPLICATE_OK);
    CHECK(memcmp(&result2.manifest, &result.manifest, sizeof result.manifest) == 0);
    CHECK(overlay.count == 2u); /* a second dashboard tile staged */
    CHECK(atomic_load_explicit(&audit_log.write_pos, memory_order_relaxed) == 2u);
}

static void test_execute_pure_replication_rejects_null(void) {
    SECTION("agn_execute_pure_replication: rejects NULL required arguments");
    actor_system_t sys;
    mesh_registry_t mesh_reg;
    brain_iam_registry_t iam_reg;
    self_replicate_sandbox_registry_t sandbox_reg;
    actor_system_init(&sys);
    mesh_registry_init(&mesh_reg);
    brain_iam_init(&iam_reg);
    self_replicate_sandbox_registry_init(&sandbox_reg);

    agn_replication_command_t cmd = make_command(0u, 1u, 0u);
    agn_replication_result_t result;
    CHECK(agn_execute_pure_replication(NULL, &mesh_reg, &iam_reg, &sandbox_reg, NULL, NULL, &cmd, &result) == SELF_REPLICATE_ERR_INVALID);
    CHECK(agn_execute_pure_replication(&sys, &mesh_reg, &iam_reg, &sandbox_reg, NULL, NULL, NULL, &result) == SELF_REPLICATE_ERR_INVALID);
    CHECK(agn_execute_pure_replication(&sys, &mesh_reg, &iam_reg, &sandbox_reg, NULL, NULL, &cmd, NULL) == SELF_REPLICATE_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* 8-thread stress: 3 distinct SaaS architectures generated           */
/* concurrently, proving exact structural conservation                */
/* ---------------------------------------------------------------- */

#define GEN_ITERATIONS_PER_THREAD 3000u

typedef struct {
    saas_config_t   config;
    saas_manifest_t expected;
} archetype_case_t;

static _Atomic uint32_t g_mismatch_count = 0;
static _Atomic uint32_t g_generated_count = 0;

typedef struct {
    const archetype_case_t *cases;
    uint32_t                case_count;
    int                     thread_id;
} gen_thread_args_t;

static THREAD_FN(gen_stress_thread) {
    gen_thread_args_t *args = (gen_thread_args_t *)arg;
    for (uint32_t i = 0; i < GEN_ITERATIONS_PER_THREAD; i++) {
        const archetype_case_t *c = &args->cases[(i + (uint32_t)args->thread_id) % args->case_count];
        saas_manifest_t got;
        if (self_replicate_generate_saas(&c->config, &got) != SELF_REPLICATE_OK ||
            memcmp(&got, &c->expected, sizeof got) != 0) {
            atomic_fetch_add_explicit(&g_mismatch_count, 1u, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_generated_count, 1u, memory_order_relaxed);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void test_concurrent_generation_three_architectures(void) {
    SECTION("8 threads generating 3 distinct SaaS architectures concurrently: exact structural conservation, 0 mismatch");

    archetype_case_t cases[3];
    cases[0].config = (saas_config_t){ .template_id = SAAS_TEMPLATE_CRM, .tenant_id = 10u, .feature_flags = 0x1u };
    cases[1].config = (saas_config_t){ .template_id = SAAS_TEMPLATE_ECOMMERCE, .tenant_id = 20u, .feature_flags = 0x2u };
    cases[2].config = (saas_config_t){ .template_id = SAAS_TEMPLATE_BILLING, .tenant_id = 30u, .feature_flags = 0x4u };
    for (int i = 0; i < 3; i++) {
        CHECK(self_replicate_generate_saas(&cases[i].config, &cases[i].expected) == SELF_REPLICATE_OK);
    }
    /* the three expected manifests are genuinely distinct architectures */
    CHECK(memcmp(&cases[0].expected, &cases[1].expected, sizeof(saas_manifest_t)) != 0);
    CHECK(memcmp(&cases[1].expected, &cases[2].expected, sizeof(saas_manifest_t)) != 0);
    CHECK(memcmp(&cases[0].expected, &cases[2].expected, sizeof(saas_manifest_t)) != 0);

    atomic_store_explicit(&g_mismatch_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_generated_count, 0u, memory_order_relaxed);

    gen_thread_args_t args[MAX_THREADS];
    thread_handle_t threads[MAX_THREADS];
    for (int t = 0; t < MAX_THREADS; t++) {
        args[t].cases = cases;
        args[t].case_count = 3u;
        args[t].thread_id = t;
        threads[t] = thread_start(gen_stress_thread, &args[t]);
    }
    for (int t = 0; t < MAX_THREADS; t++) {
        thread_join(threads[t]);
    }

    CHECK(atomic_load_explicit(&g_mismatch_count, memory_order_relaxed) == 0u);
    CHECK(atomic_load_explicit(&g_generated_count, memory_order_relaxed) == MAX_THREADS * GEN_ITERATIONS_PER_THREAD);
}

/* ---------------------------------------------------------------- */

int main(void) {
    test_auto_heal_noop_below_threshold();
    test_auto_heal_triggers_on_bounds_violations();
    test_auto_heal_triggers_on_latency();
    test_sandbox_out_of_range();

    test_couple_signal_anomaly_noop_on_benign_report();
    test_couple_signal_anomaly_escalates_on_repeated_anomaly();
    test_auto_heal_audited_logs_only_on_real_heal();
    test_couple_signal_anomaly_audited_logs_the_heal();
    test_concurrent_noisy_packet_injection_self_heals_and_audits();

    test_get_template();
    test_generate_saas_deterministic();
    test_generate_saas_rejects_bad_template();

    test_select_kernel_variant();

    test_execute_pure_replication_end_to_end();
    test_execute_pure_replication_rejects_null();

    test_concurrent_generation_three_architectures();

    printf("\n%d checks run, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
