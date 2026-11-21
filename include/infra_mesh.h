/*
 * infra_mesh.h
 *
 * Internal mesh routing between "virtual server" actors, a shared-secret-
 * authenticated inter-node frame format, a deterministic SoA load
 * balancer over the actor pool, and a local, immutable, content-hashed
 * block store -- reusing actor_core.h, crypto_constant_time.h, and
 * wire_proto.h's wire_msg_t as-is, not reimplementing any of them.
 *
 * ============================== Honesty notes, up front =========================
 *
 * The request this module answers named ten pieces of real, existing,
 * complete infrastructure software as a numbered list: Proxmox/XCP-ng,
 * a hardened Linux kernel, Kubernetes/Nomad, WireGuard/Nebula, Traefik/
 * Nginx + Let's Encrypt, CoreDNS/PowerDNS, MinIO/Ceph, Harbor, Keepalived
 * + HAProxy, BorgBackup. None of those become a C module added to one
 * Windows process in a single session -- each is a real, independently
 * enormous project (Kubernetes alone is several million lines across
 * dozens of components; WireGuard's Noise protocol handshake and cipher
 * suite is a from-scratch cryptographic engineering effort this codebase
 * has no primitives for at all). Claiming to deliver any of them as a
 * source file here would be exactly the kind of overclaim this
 * codebase's SESSION_NOTES.md already refuses elsewhere ("net_core.c...
 * non cree, ce nom ne correspond a aucun module de ce projet").
 *
 * What THIS module actually is, honestly: the specific, concrete
 * requirements the mission's own numbered EXIGENCES section spelled out,
 * built for real, reusing what already exists:
 *
 *   - "Chaque serveur mondial virtuel modelise comme un Acteur leger":
 *     literally true here -- a mesh node IS one actor_t (actor_core.h),
 *     nothing new invented for it. ACTOR_MAX_COUNT (8) is this project's
 *     existing, already-tested hard actor-population limit; MESH_MAX_NODES
 *     equals it rather than pretending a larger, independent capacity.
 *   - "Resolveur DNS local... O(1)... sans recherche sequentielle": a
 *     direct-indexed array (mesh_node_id -> descriptor), the exact same
 *     technique this project's own msg_router.h already uses for
 *     tenant_id -> actor_id (ROUTER_MAX_TENANTS, direct write/read, zero
 *     search) -- not a DNS wire-protocol server, because nothing here
 *     needs one: this resolves an INTERNAL small integer ID, not a
 *     hostname, and there is no separate machine on the other end to
 *     query over port 53.
 *   - "Echange de trames chiffrees... reutilisant crypto_constant_time.h":
 *     crypto_constant_time.h in this codebase provides constant-time
 *     COMPARISON (ct_equal*, ct_verify_token) -- no cipher, no AEAD, no
 *     key exchange primitive exists here at all, so there is nothing to
 *     reuse for actual encryption. What this module honestly builds
 *     instead: shared-secret AUTHENTICATION between mesh frames, using
 *     ct_verify_token() exactly the way net_gateway.c already
 *     authenticates tenants -- tamper-evident and forgery-resistant
 *     against a naive attacker, but the secret and payload travel in the
 *     clear (no confidentiality). A real encrypted tunnel needs a real
 *     AEAD cipher; building one from scratch to satisfy this checkbox
 *     would be introducing unvetted cryptography, which is worse than
 *     not claiming encryption at all. This module also does NOT open a
 *     new network transport -- net_io.c already owns real Winsock
 *     sockets; mesh_dispatch() below routes an already-received frame to
 *     the correct actor IN THIS PROCESS, modeling inter-"server" routing
 *     semantics without claiming to bridge physically separate machines.
 *   - "Load balancer... SoA... _Alignas(64)... backpressure cgroups-style":
 *     lb_pool_t below is genuinely Structure-of-Arrays and 64-byte
 *     aligned, distributes to actor_core.h's existing actor pool by
 *     least-loaded selection (actor_mailbox_count(), already O(1)), and
 *     enforces a PER-WORKER SOFT QUOTA tighter than the mailbox's own
 *     hard ring capacity -- a real resource-isolation mechanism, not
 *     literal Linux cgroups (which do not exist as a concept to invoke
 *     from a Windows process; this project targets Windows, per its own
 *     Makefile's -lws2_32).
 *   - "Stockage S3-style... Copy-on-Write... FNV-1a... descripteurs bas
 *     niveau": a real local (single-directory, single-process) content-
 *     addressed, IMMUTABLE block store for wire_msg_t (the exact 64-byte
 *     struct wire_proto.h already defines -- reused verbatim, per the
 *     mission's own wording), written via _open()/_write()/_close()
 *     (native low-level fd calls, no stdio buffering), each block's
 *     FNV-1a/64 hash computed before write and re-verified on every
 *     read. Immutability is enforced by the OS itself: writes use
 *     _O_CREAT|_O_EXCL, so overwriting an existing object_id fails
 *     atomically -- a new version needs a new object_id, never an
 *     in-place mutation, which is the honest, achievable meaning of
 *     "Copy-on-Write" for an application-level local store (a REAL CoW
 *     filesystem like ZFS/Btrfs does block-level copy-on-write with
 *     reference-counted extents; this does not attempt that). "S3-style"
 *     here means "immutable, content-hashed, object-addressed" -- NOT
 *     distributed, NOT replicated: one directory, one machine.
 */

#ifndef INFRA_MESH_H
#define INFRA_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actor_core.h"
#include "wire_proto.h" /* wire_msg_t: reused verbatim as the store's 64-byte block format */
#include "barry_storage.h" /* barry_kv_store_t/barry_ts_store_t: reused verbatim by mesh_dispatch_recorded() below */
#include "telemetry_fabric.h" /* tf_log_t: reused verbatim by mesh_dispatch_audited() below */
#include "signal_tracking.h" /* track_registry_t/fixed_q21_10_t: reused verbatim by the Part E spatial-perception functions below */

typedef enum {
    MESH_OK                    = 0,
    MESH_ERR_INVALID           = -1,
    MESH_ERR_OUT_OF_RANGE      = -2,  /* node_id/worker index >= its table's fixed capacity */
    MESH_ERR_DUPLICATE_NODE    = -3,
    MESH_ERR_UNKNOWN_NODE      = -4,  /* O(1) rejection: no such node_id registered */
    MESH_ERR_PAYLOAD_TOO_LARGE = -5,
    MESH_ERR_BAD_MAGIC         = -6,
    MESH_ERR_INTEGRITY         = -7,  /* frame content does not match its embedded FNV-1a hash */
    MESH_ERR_AUTH_FAILED       = -8,  /* shared-secret mismatch (constant-time, ct_verify_token) */
    MESH_ERR_ACTOR_SPAWN_FAILED = -9,
    MESH_ERR_BACKPRESSURE      = -10, /* every eligible worker/actor is at or above its quota -- O(1)-bounded rejection, never a wait */
    MESH_ERR_STORE_EXISTS      = -11, /* object_id already stored -- immutable: never overwritten */
    MESH_ERR_STORE_NOT_FOUND   = -12,
    MESH_ERR_STORE_CORRUPT     = -13, /* on-disk FNV-1a hash does not match the block's content */
    MESH_ERR_STORE_IO          = -14,
    MESH_ERR_TRACKING_FAILED   = -15  /* signal_tracking.h call rejected the report/query (bad tick order, unknown target) */
} mesh_status_t;

/* ==================================================================== */
/* Part A -- Node registry: O(1) direct-indexed resolver                 */
/* ==================================================================== */

#define MESH_MAX_NODES   ACTOR_MAX_COUNT /* one node == one actor_t; see header note */
#define MESH_SECRET_LEN  16u             /* shared-secret length, chosen so mesh_frame_t == 64 bytes exactly */

typedef struct {
    uint32_t internal_ip;               /* opaque internal "virtual IP" (e.g. a packed 10.x.x.x); never resolved over any real network */
    uint8_t  secret[MESH_SECRET_LEN];
    uint8_t  actor_id;                  /* this node's lightweight actor, in a caller-owned actor_system_t */
    uint8_t  registered;
} mesh_node_t;

typedef struct {
    mesh_node_t nodes[MESH_MAX_NODES];  /* direct-indexed by node_id: O(1), zero sequential search */
} mesh_registry_t;

void mesh_registry_init(mesh_registry_t *reg);

/* Boot-time-only (like msg_router's router_register_tenant()): registers
 * node_id (< MESH_MAX_NODES) with its internal IP, shared secret, and
 * backing actor_id. Not thread-safe against concurrent registration or
 * mesh_resolve()/mesh_dispatch() calls. */
mesh_status_t mesh_register_node(mesh_registry_t *reg, uint8_t node_id, uint32_t internal_ip,
                                  const uint8_t secret[MESH_SECRET_LEN], uint8_t actor_id);

/* O(1) direct-index lookup. Returns NULL if node_id is out of range or
 * was never registered -- the entire "DNS resolution" this module needs,
 * with no protocol, no query, no sequential search. */
const mesh_node_t *mesh_resolve(const mesh_registry_t *reg, uint8_t node_id);

/* ==================================================================== */
/* Part B -- Authenticated (NOT encrypted -- see header) inter-node frame */
/* ==================================================================== */

#define MESH_MAGIC        0x4853454Du /* "MESH" as bytes M,E,S,H little-endian */
#define MESH_PAYLOAD_LEN  32u          /* sized so sizeof(mesh_frame_t) == 64 exactly */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  src_node_id;
    uint8_t  dst_node_id;
    uint16_t payload_len;
    uint8_t  secret[MESH_SECRET_LEN];  /* cleartext on the wire -- authentication, not confidentiality; see header */
    uint8_t  payload[MESH_PAYLOAD_LEN];
    uint64_t integrity_hash;           /* FNV-1a/64 over every field above */
} mesh_frame_t;

_Static_assert(sizeof(mesh_frame_t) == 64u, "mesh_frame_t must be exactly 64 bytes");

/* Builds one authenticated frame: fills every field, zeroes any unused
 * payload tail (so no stale byte from a reused mesh_frame_t survives),
 * and computes the integrity hash last, over the final content. */
mesh_status_t mesh_build_frame(mesh_frame_t *out, uint8_t src_node_id, uint8_t dst_node_id,
                                const uint8_t secret[MESH_SECRET_LEN],
                                const uint8_t *payload, uint16_t payload_len);

/*
 * Verifies frame (magic, integrity hash) and authenticates it against
 * the SENDING node's registered secret (ct_verify_token(), constant-
 * time -- crypto_constant_time.h, reused as-is) -- a forged src_node_id
 * claiming a secret it does not actually hold is rejected here, before
 * dst_node_id is even resolved. On success, routes the payload (O(1)
 * resolve of dst_node_id, then actor_mailbox_reserve()/_publish() into
 * that node's actor -- reusing actor_core.h's own zero-copy mailbox API,
 * not a new one) into the destination actor's mailbox, with sender_id
 * set to src_node_id and type fixed to MESH_MSG_TYPE_FRAME.
 */
#define MESH_MSG_TYPE_FRAME 1u

mesh_status_t mesh_dispatch(actor_system_t *sys, const mesh_registry_t *reg, const mesh_frame_t *frame);

/*
 * Additive coupling point to barry_storage.h (Niveaux 11-20): runs
 * mesh_dispatch() completely unchanged (identical validation, identical
 * return codes, identical actor routing) and, only on MESH_OK, optionally
 * records the delivery -- the destination node's most recent payload into
 * *kv_store (key "node:<dst_node_id>:last_frame") and a
 * (tick, payload_len) point into *ts_store under metric_id. Either store
 * pointer may be NULL to skip that half of the recording; mesh_dispatch()
 * itself, and every existing caller of it, is untouched by this function
 * existing. See barry_storage.h and src/infra_mesh.c for why `tick` is an
 * opaque caller-supplied counter, not a wall clock.
 */
mesh_status_t mesh_dispatch_recorded(actor_system_t *sys, const mesh_registry_t *reg,
                                      const mesh_frame_t *frame,
                                      barry_kv_store_t *kv_store, barry_ts_store_t *ts_store,
                                      uint32_t metric_id, uint64_t tick);

/*
 * Second additive coupling point (Niveaux 31-40): runs mesh_dispatch()
 * completely unchanged, then appends ONE tf_log_t entry (telemetry_fabric.h,
 * reused verbatim) recording src_node_id, dst_node_id and the resulting
 * mesh_status_t -- an immutable, hash-chained audit trail of every mesh
 * delivery attempt, successful or not. `audit_log` may be NULL to skip
 * logging. Distinct from mesh_dispatch_recorded() (which feeds
 * barry_storage.h's observability stores) so a caller can use either,
 * both, or neither without the two features being coupled to each other.
 */
mesh_status_t mesh_dispatch_audited(actor_system_t *sys, const mesh_registry_t *reg,
                                     const mesh_frame_t *frame, tf_log_t *audit_log);

/* ==================================================================== */
/* Part C -- Deterministic SoA load balancer over the actor pool         */
/* ==================================================================== */

#define LB_MAX_WORKERS ACTOR_MAX_COUNT

/* Structure of Arrays, each column its own cache line: a hot least-
 * loaded scan over `worker`/`capacity` never shares a line with, or
 * false-shares against, anything else. */
typedef struct {
    _Alignas(64) actor_t  *worker[LB_MAX_WORKERS];
    _Alignas(64) uint32_t  capacity[LB_MAX_WORKERS]; /* per-worker soft quota: a "cgroup-style" isolation limit, tighter than the mailbox's own hard ring capacity */
    uint32_t count;
} lb_pool_t;

void lb_pool_init(lb_pool_t *lb);

/* Boot-time-only. Registers an already-spawned actor as a worker with a
 * soft in-flight quota (must be <= RING_BUFFER_CAPACITY to mean anything
 * -- a quota above the mailbox's own hard capacity would never trigger). */
mesh_status_t lb_pool_add_worker(lb_pool_t *lb, actor_t *worker, uint32_t capacity);

/*
 * Deterministic least-loaded dispatch: scans every registered worker
 * (bounded by LB_MAX_WORKERS, a small fixed compile-time constant -- not
 * claimed as literal O(1), the same "bounded, not O(1) in the strict
 * sense" precision this codebase already applies elsewhere), picks the
 * one with the fewest messages currently in its mailbox
 * (actor_mailbox_count(), itself O(1)) among those STRICTLY UNDER their
 * own capacity[], and posts into it. If every worker is at or above its
 * quota, returns MESH_ERR_BACKPRESSURE immediately -- never blocks,
 * never spins, never silently exceeds a quota to force a delivery
 * through. *out_worker_index (if non-NULL) receives which worker index
 * was chosen, on success.
 */
mesh_status_t lb_pool_dispatch(lb_pool_t *lb, uint32_t sender_id, uint32_t type,
                                const void *payload, size_t payload_len,
                                uint8_t *out_worker_index);

/* ==================================================================== */
/* Part D -- Local immutable, content-hashed block store                 */
/* ==================================================================== */

/*
 * Stores *block (wire_msg_t, wire_proto.h's existing 64-byte packed
 * struct -- reused verbatim, never redefined) under dest_dir as
 * object_id. Computes FNV-1a/64 over the block's 64 bytes and writes it
 * appended after them (72 bytes on disk total), via _open()/_write()/
 * _close() -- no stdio buffering. Opens with the OS-enforced exclusive-
 * create flag: if object_id already exists, this call fails with
 * MESH_ERR_STORE_EXISTS and writes NOTHING -- objects are immutable by
 * construction (enforced by the filesystem itself, not by a check-then-
 * write race), a new version needs a new object_id.
 */
mesh_status_t mesh_store_put(const char *dest_dir, uint32_t object_id, const wire_msg_t *block);

/* Reads object_id back, re-verifying its FNV-1a/64 hash before returning
 * it -- MESH_ERR_STORE_CORRUPT (not a silently wrong value) if the bytes
 * on disk no longer match the hash stored alongside them. */
mesh_status_t mesh_store_get(const char *dest_dir, uint32_t object_id, wire_msg_t *out);

/* ==================================================================== */
/* Part E -- Spatial perception: mesh nodes <-> signal_tracking.h        */
/* ==================================================================== */

/*
 * "Give spatial perception to our virtual workers": a mesh node's 3D
 * trajectory is tracked in a caller-owned track_registry_t
 * (signal_tracking.h, reused verbatim -- no new storage, no second
 * tracking mechanism) under target_id == node_id directly. MESH_MAX_NODES
 * (== ACTOR_MAX_COUNT, 8) is always comfortably within TRACK_MAX_TARGETS
 * (64), so this reuse needs no remapping. The only thing this function
 * ADDS over calling track_update_position() directly is the mesh
 * registry check: a position report for a node_id that was never
 * mesh_register_node()'d is rejected as MESH_ERR_UNKNOWN_NODE before it
 * ever reaches signal_tracking.h -- a worker's spatial trajectory only
 * exists once the worker itself is a real, registered mesh node, the
 * same "resolve first, act second" discipline mesh_dispatch() already
 * applies to frame delivery. Any other signal_tracking.h rejection
 * (backward ticks) surfaces as MESH_ERR_TRACKING_FAILED.
 */
mesh_status_t mesh_node_report_position(const mesh_registry_t *reg, track_registry_t *tracks, uint8_t node_id,
                                         fixed_q21_10_t x, fixed_q21_10_t y, fixed_q21_10_t z,
                                         uint64_t current_cycle);

/* Same registry-gated reuse as mesh_node_report_position(), for reading
 * a node's linearly-extrapolated position instead of writing one.
 * MESH_ERR_TRACKING_FAILED if the node is registered but has never had
 * its position reported yet (SIGTRACK_ERR_NOT_FOUND from
 * track_predict_position()). */
mesh_status_t mesh_node_predict_position(const mesh_registry_t *reg, const track_registry_t *tracks, uint8_t node_id,
                                          uint64_t current_cycle,
                                          fixed_q21_10_t *out_x, fixed_q21_10_t *out_y, fixed_q21_10_t *out_z);

#endif /* INFRA_MESH_H */
