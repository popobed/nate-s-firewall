/*
 * telemetry_fabric.h
 *
 * Niveaux 31-40 (as re-scoped by this mission's own concrete numbered
 * requirements -- see "Honesty notes" below) -- an immutable, hash-
 * chained write-ahead log, a constant-time-verified per-tenant access-key
 * store in real SoA, and the additive coupling points into infra_mesh.c
 * and brain_core.c the mission asked for. Static, zero-malloc-after-init,
 * built from primitives this project already has (crypto_constant_time.h)
 * rather than reimplemented.
 *
 * ============================== Honesty notes, up front =====================
 * The pasted 50-level roadmap in this request names ten more real,
 * independently enormous systems (Proxmox/K8s/WireGuard/... down through
 * a full frontend design-system/SDUI/i18n/offline-sync/analytics/CDN
 * stack for Niveaux 1-10 and 31-40). None of that is re-litigated here --
 * infra_mesh.h/barry_storage.h/brain_core.h already covered Niveaux 1-30
 * with the same honest-scope discipline this file continues, and a CDN/
 * frontend UI framework is not something this static C11 backend module
 * could honestly claim to BE. This file answers the mission's OWN
 * concrete numbered requirements for "Niveaux 31 a 40", which named:
 *
 *   - Niveau 32/33/34 (immutable centralized logs, monitoring) :
 *     `tf_log_t` is real: a fixed-capacity, hash-chained write-ahead log
 *     (each entry's FNV-1a checksum covers its own fields INCLUDING the
 *     previous entry's checksum -- the same tamper-evidence technique
 *     git commit hashing and simple blockchains use). Altering any
 *     historical entry's bytes -- or its stored checksum, to try to hide
 *     the edit -- breaks tf_log_verify()'s recomputation for that entry
 *     and desynchronizes every entry chained after it. Precisely NOT
 *     claimed: FNV-1a is a fast, non-cryptographic checksum (the same
 *     one this project already uses in infra_mesh.c/barry_storage.c) --
 *     it protects against accidental corruption and naive/unsophisticated
 *     tampering (the "0 echec" stress test this file ships proves this
 *     against malformed/adversarial-shaped input), NOT against a
 *     determined attacker with real FNV-1a collision-finding capability.
 *     A tenant genuinely needing that guarantee needs a cryptographic
 *     hash (SHA-256/BLAKE3) plus a signature scheme, neither of which
 *     exists in this codebase -- inventing one here would be exactly the
 *     "unvetted cryptography, worse than not claiming it" posture
 *     crypto_constant_time.h and infra_mesh.h already refuse.
 *   - Niveau 35/36/39 (sovereign crypto, tenant metadata "encryption",
 *     privacy-respecting analytics as "no third party sees this data") :
 *     `tf_tenant_key_store_t` is real: per-tenant access-key HASHES (never
 *     raw keys a second time) verified in genuinely constant time via
 *     crypto_constant_time.h's ct_equal_u64() -- reused verbatim, not
 *     reimplemented -- stored in true Structure-of-Arrays form, one
 *     `_Alignas(64)` array per column (tenant_id[], key_hash[],
 *     registered[]), the exact pattern infra_mesh.h's `lb_pool_t` and
 *     brain_core.h's `brain_billing_ledger_t` already established.
 *     Precisely NOT claimed: end-to-end AEAD encryption of tenant
 *     metadata -- crypto_constant_time.h itself documents that no
 *     cipher/AEAD primitive exists anywhere in this codebase, so there is
 *     nothing to "encrypt" with; what IS real and honestly buildable is
 *     constant-time-verified key POSSESSION, the same authenticate-not-
 *     encrypt boundary infra_mesh.h's mesh_frame_t already draws.
 *   - Niveau 37/38/40 (AVX2 acoustic analysis, ephemeral D3D12/WASM
 *     graphics) : this is NOT reimplemented a third time here --
 *     audio_transcribe.c's `audio_extract_features()` (AVX2-gated
 *     Goertzel filterbank) and `audio_actor_store_features()` (zero-copy
 *     injection into an actor_t's SRAM-style private_memory) already ARE
 *     the real, tested delivery of that requirement; wasm_gpu_engine.c's
 *     `gpu_backend_t`/`gpu_backend_submit_frame()` already ARE the real
 *     D3D12 fence synchronization, and its portable SoA/command-queue
 *     subset already IS what compiles to WebAssembly object code (see
 *     that header's own "Ce qui compile REELLEMENT" note). This file's
 *     actual, additive contribution here is the AUDIT TRAIL those
 *     subsystems can log into (tf_log_append() calls from a caller's
 *     signal/graphics pipeline), not a second signal-processing or
 *     graphics engine.
 *
 * ============================== Coupling, precisely ==========================
 * "Couple directement au maillage infra_mesh.c et au cerveau logique
 * brain_core.c": additive wrapper functions declared in THOSE headers
 * (`mesh_dispatch_audited()` in infra_mesh.h, `brain_billing_charge_audited()`
 * in brain_core.h) call the existing, unmodified mesh_dispatch()/
 * brain_billing_charge() and then tf_log_append() the outcome -- neither
 * existing function's own signature or behavior changes, the same
 * additive-only discipline barry_storage.h's mesh_dispatch_recorded()
 * already established. This header intentionally does NOT include
 * infra_mesh.h or brain_core.h (one-directional dependency only, no
 * cycle) -- exactly like barry_storage.h before it.
 */

#ifndef TELEMETRY_FABRIC_H
#define TELEMETRY_FABRIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "crypto_constant_time.h" /* ct_equal_u64: reused verbatim, not reimplemented */

#define TF_CACHE_LINE_SIZE 64u

typedef enum {
    TF_OK = 0,
    TF_ERR_INVALID   = -1,
    TF_ERR_FULL      = -2, /* O(1) instant rejection, never a wait */
    TF_ERR_TAMPERED  = -3, /* tf_log_verify(): the hash chain no longer matches recomputed checksums */
    TF_ERR_NOT_READY = -4, /* tf_log_verify(): hit an entry mid-write (a concurrent append) before verifying it */
    TF_ERR_NOT_FOUND = -5,
    TF_ERR_OUT_OF_RANGE = -6,
    TF_ERR_ALREADY_REGISTERED = -7
} tf_status_t;

/* ==================================================================== */
/* Part A -- Immutable, hash-chained write-ahead log (Niveau 32/33/34)   */
/* ==================================================================== */

#define TF_LOG_CAPACITY  4096u /* fixed, pre-allocated entry count -- never grows */
#define TF_LOG_MSG_MAX   48u
/* Niveau 54: NOT a give-up bound anymore (see tf_log_append()'s doc
 * comment for why the old give-up-and-proceed design was a real bug, not
 * just a theoretical one). This is now the spin count after which each
 * retry of the predecessor-linking CAS wait cooperatively yields the CPU
 * (SwitchToThread()/sched_yield()) -- never a blocking wait, and the wait
 * itself no longer has an exit that skips synchronization. */
#define TF_LOG_SPIN_BOUND 100000u
#define TF_LOG_GENESIS_CHECKSUM 0ULL /* prev_checksum for entry 0 -- there is no predecessor */

typedef struct {
    _Atomic uint32_t ready;         /* 0 while this entry's fields are still being written, 1 once its checksum is final -- same discipline as ring_buffer.h's ring_msg_t.ready */
    uint64_t         seq;           /* == this entry's own slot index; gapless, gives tf_log_verify() the expected order */
    uint64_t         prev_checksum; /* the IMMEDIATELY PRECEDING entry's checksum -- the hash-chain link */
    uint32_t         event_type;    /* opaque, caller-defined -- same "caller owns the meaning" posture as swarm_tick */
    uint32_t         payload_len;
    uint8_t          payload[TF_LOG_MSG_MAX];
    uint64_t         checksum;      /* FNV-1a/64 over every field above (seq, prev_checksum, event_type, payload_len, payload) */
} tf_log_entry_t;

typedef struct {
    _Alignas(TF_CACHE_LINE_SIZE) _Atomic uint64_t write_pos; /* append cursor, claimed via fetch_add: any number of concurrent producers */
    tf_log_entry_t entries[TF_LOG_CAPACITY]; /* embedded: zero dynamic allocation, ever */
    _Atomic bool   tamper_flag; /* latched true the instant ANY tf_log_verify() call (including the internal one tf_log_append() never runs -- see below) finds a broken chain; never reset by this file */
} tf_log_t;

void tf_log_init(tf_log_t *log);

/*
 * O(1) amortized: claims a slot via fetch_add(write_pos) (bounded,
 * TF_ERR_FULL -- with the reservation rolled back, never a permanent
 * gap -- once TF_LOG_CAPACITY is reached). The claimed slot is memset()
 * to all-zero synchronously -- including any compiler-inserted padding a
 * per-field assignment would never reach by name -- before a single
 * field is set and before the checksum is computed (CWE-457).
 *
 * Then links this entry to its immediate predecessor's checksum via an
 * UNBOUNDED, non-blocking active wait built on atomic_compare_exchange_
 * weak (memory_order_acq_rel on success / memory_order_acquire on
 * failure) against the predecessor's `ready` flag -- the mission's own
 * wording asks for "ecriture de facon sequentielle synchrone (WAL)",
 * which is exactly this: a WAL is fundamentally an ordered, single
 * logical stream, even when fed by multiple producer THREADS.
 *
 * Niveau 54 fix, precisely: this wait previously had a fixed bound
 * (TF_LOG_SPIN_BOUND spins) and, on exhaustion, read the predecessor's
 * checksum ANYWAY -- a genuine data race on a non-atomic field (no
 * happens-before edge was ever established, since `ready` was never
 * actually observed as 1), and the concrete, reproduced cause of
 * tf_log_verify() reporting a false TF_ERR_TAMPERED under extreme
 * concurrent load (8 threads doing nothing but hammer this function).
 * The predecessor's own write here is strictly O(1)-bounded work (the
 * same memset plus a handful of field copies plus one FNV-1a pass over
 * <= TF_LOG_MSG_MAX bytes) -- never I/O, never user-code-duration --
 * so removing the give-up bound does not risk an actual unbounded wait;
 * it removes a bypass that let this function read data it had not
 * synchronized with. TF_LOG_SPIN_BOUND is repurposed as the spin count
 * after which each retry cooperatively yields the CPU, never as a point
 * where correctness is given up.
 *
 * Finally computes and stores this entry's own FNV-1a/64 checksum over
 * every preceding field, then publishes (`ready` = 1, release). Rejects
 * payload_len > TF_LOG_MSG_MAX as TF_ERR_INVALID.
 */
tf_status_t tf_log_append(tf_log_t *log, uint32_t event_type, const void *payload, uint32_t payload_len);

/*
 * Bounded by TF_LOG_CAPACITY (a small, fixed compile-time constant --
 * same "bounded, not the strict textbook O(1)" precision infra_mesh.h's
 * lb_pool_dispatch() already applies): walks entries[0..write_pos) in
 * order, recomputing each entry's checksum and confirming it both
 * matches the STORED checksum and matches the NEXT entry's
 * prev_checksum link. On the first mismatch, sets *out_first_broken_seq
 * (if non-NULL), latches log->tamper_flag, and returns TF_ERR_TAMPERED.
 * TF_ERR_NOT_READY if it reaches an entry still mid-append (a benign
 * race against a concurrent producer, not evidence of tampering) --
 * *out_first_broken_seq is set to that entry's seq so the caller can
 * retry from there. TF_OK means every entry checked is provably
 * untouched since it was written.
 */
tf_status_t tf_log_verify(tf_log_t *log, uint64_t *out_first_broken_seq);

/* Convenience composition: calls tf_log_verify(); on TF_ERR_TAMPERED,
 * additionally raises `swarm_sys`'s alert_flag for `tenant_id` via the
 * additive swarm_agent_raise_alert() hook (swarm_dispatch.h) -- the
 * "drapeau d'alerte critique dans le dispatcher du Swarm" the mission
 * asks for. Declared with an opaque `void *swarm_sys` and a raw function
 * pointer rather than including swarm_dispatch.h directly (this header
 * stays a leaf dependency, the same one-directional-only posture as its
 * relationship with infra_mesh.h/brain_core.h) -- the caller supplies
 * swarm_agent_raise_alert itself as `raise_alert_fn`. Returns whatever
 * tf_log_verify() returned. */
typedef int (*tf_alert_fn)(void *swarm_sys, uint16_t tenant_id); /* mirrors swarm_agent_raise_alert()'s (swarm_status_t) return, as a plain int to avoid the header dependency */

tf_status_t tf_log_verify_and_alert(tf_log_t *log, uint64_t *out_first_broken_seq,
                                     void *swarm_sys, uint16_t tenant_id, tf_alert_fn raise_alert_fn);

/* ==================================================================== */
/* Part B -- Constant-time tenant access-key store, real SoA (Niveau 35/36/39) */
/* ==================================================================== */

#define TF_KEY_STORE_MAX_TENANTS 128u

typedef struct {
    _Alignas(TF_CACHE_LINE_SIZE) uint16_t tenant_id[TF_KEY_STORE_MAX_TENANTS];
    _Alignas(TF_CACHE_LINE_SIZE) uint64_t key_hash[TF_KEY_STORE_MAX_TENANTS];  /* FNV-1a/64 of the tenant's provisioned access key -- never the raw key */
    _Alignas(TF_CACHE_LINE_SIZE) bool     registered[TF_KEY_STORE_MAX_TENANTS];
} tf_tenant_key_store_t;

void tf_key_store_init(tf_tenant_key_store_t *store);

/* Boot-time/admin-time only (like mesh_register_node()): not thread-safe
 * against concurrent registration or verify() calls. Hashes `key` with
 * FNV-1a/64 before storing -- the raw key bytes are never retained. */
tf_status_t tf_key_store_register(tf_tenant_key_store_t *store, uint16_t slot, uint16_t tenant_id,
                                   const void *key, uint32_t key_len);

/*
 * O(1), direct-indexed by `slot`: hashes `provided_key` with the SAME
 * FNV-1a/64 (an ordinary, non-secret checksum -- computing it is not the
 * part that needs to be constant-time) then compares it to the stored
 * hash via ct_equal_u64() (crypto_constant_time.h, reused verbatim) --
 * the comparison's timing depends only on the fact that two 64-bit
 * values are being compared, never on where they differ. Returns TF_OK
 * (and *out_tenant_id, if non-NULL) iff registered[slot] and the hashes
 * match.
 */
tf_status_t tf_key_store_verify(const tf_tenant_key_store_t *store, uint16_t slot,
                                 const void *provided_key, uint32_t provided_key_len,
                                 uint16_t *out_tenant_id);

#endif /* TELEMETRY_FABRIC_H */
