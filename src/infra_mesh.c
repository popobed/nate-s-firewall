#include "infra_mesh.h"

#include <errno.h>
#include <io.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "crypto_constant_time.h" /* ct_equal, ct_verify_token -- reused, not reimplemented */

/* Same standard FNV-1a/64 offset basis and prime net_gateway.c's
 * ngw_fnv1a64() uses -- reimplemented locally (it is `static` there, and
 * this is a small, well-known, non-secret checksum, not a primitive that
 * needs a single shared, audited implementation the way the constant-
 * time comparisons above do) rather than widening net_gateway.h's public
 * API for an unrelated module's benefit. */
static uint64_t mesh_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static uint64_t mesh_frame_hash(const mesh_frame_t *f) {
    /* Every field except integrity_hash itself -- offsetof is exact and
     * well-defined here because mesh_frame_t is packed. */
    return mesh_fnv1a64((const uint8_t *)f, offsetof(mesh_frame_t, integrity_hash));
}

/* ==================================================================== */
/* Part A -- Node registry                                               */
/* ==================================================================== */

void mesh_registry_init(mesh_registry_t *reg) {
    if (reg == NULL) {
        return;
    }
    memset(reg, 0, sizeof(*reg));
}

mesh_status_t mesh_register_node(mesh_registry_t *reg, uint8_t node_id, uint32_t internal_ip,
                                  const uint8_t secret[MESH_SECRET_LEN], uint8_t actor_id) {
    if (reg == NULL || secret == NULL) {
        return MESH_ERR_INVALID;
    }
    if (node_id >= MESH_MAX_NODES) {
        return MESH_ERR_OUT_OF_RANGE;
    }
    if (reg->nodes[node_id].registered) {
        return MESH_ERR_DUPLICATE_NODE;
    }

    reg->nodes[node_id].internal_ip = internal_ip;
    memcpy(reg->nodes[node_id].secret, secret, MESH_SECRET_LEN);
    reg->nodes[node_id].actor_id   = actor_id;
    reg->nodes[node_id].registered = 1u;
    return MESH_OK;
}

const mesh_node_t *mesh_resolve(const mesh_registry_t *reg, uint8_t node_id) {
    if (reg == NULL || node_id >= MESH_MAX_NODES || !reg->nodes[node_id].registered) {
        return NULL;
    }
    return &reg->nodes[node_id];
}

/* ==================================================================== */
/* Part B -- Authenticated inter-node frame                              */
/* ==================================================================== */

mesh_status_t mesh_build_frame(mesh_frame_t *out, uint8_t src_node_id, uint8_t dst_node_id,
                                const uint8_t secret[MESH_SECRET_LEN],
                                const uint8_t *payload, uint16_t payload_len) {
    if (out == NULL || secret == NULL || (payload == NULL && payload_len != 0u)) {
        return MESH_ERR_INVALID;
    }
    if (payload_len > MESH_PAYLOAD_LEN) {
        return MESH_ERR_PAYLOAD_TOO_LARGE;
    }

    memset(out, 0, sizeof(*out)); /* no stale byte anywhere, including any unused payload tail */
    out->magic       = MESH_MAGIC;
    out->src_node_id = src_node_id;
    out->dst_node_id = dst_node_id;
    out->payload_len = payload_len;
    memcpy(out->secret, secret, MESH_SECRET_LEN);
    if (payload_len != 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    out->integrity_hash = mesh_frame_hash(out);
    return MESH_OK;
}

mesh_status_t mesh_dispatch(actor_system_t *sys, const mesh_registry_t *reg, const mesh_frame_t *frame) {
    if (sys == NULL || reg == NULL || frame == NULL) {
        return MESH_ERR_INVALID;
    }
    if (frame->magic != MESH_MAGIC) {
        return MESH_ERR_BAD_MAGIC;
    }
    if (frame->payload_len > MESH_PAYLOAD_LEN) {
        return MESH_ERR_PAYLOAD_TOO_LARGE;
    }

    /* Integrity first (cheap, no registry lookup needed): a corrupted
     * frame is rejected before either node_id is even resolved. Compared
     * via ct_equal for the same "uniformity" reasoning net_gateway.c
     * documents for its own hash check -- this is a checksum, not a
     * secret, so its own timing carries nothing exploitable, but every
     * comparison in this path goes through the one audited primitive
     * rather than a case-by-case judgment call about which `==` are safe. */
    uint64_t computed = mesh_frame_hash(frame);
    if (!ct_equal(&computed, &frame->integrity_hash, sizeof(uint64_t))) {
        return MESH_ERR_INTEGRITY;
    }

    const mesh_node_t *src = mesh_resolve(reg, frame->src_node_id);
    if (src == NULL) {
        return MESH_ERR_UNKNOWN_NODE;
    }
    /* Constant-time: a forged src_node_id claiming a secret it does not
     * hold must not leak, via comparison timing, how close the forgery
     * came to the real one. */
    if (!ct_verify_token(frame->secret, MESH_SECRET_LEN, src->secret, MESH_SECRET_LEN)) {
        return MESH_ERR_AUTH_FAILED;
    }

    const mesh_node_t *dst = mesh_resolve(reg, frame->dst_node_id);
    if (dst == NULL) {
        return MESH_ERR_UNKNOWN_NODE;
    }
    actor_t *actor = actor_system_get(sys, dst->actor_id);
    if (actor == NULL) {
        return MESH_ERR_UNKNOWN_NODE; /* registered node, but its backing actor was never spawned */
    }

    ring_msg_t *slot;
    actor_status_t ast = actor_mailbox_reserve(actor, &slot);
    if (ast == ACTOR_ERR_MAILBOX_FULL) {
        return MESH_ERR_BACKPRESSURE;
    }
    if (ast != ACTOR_OK) {
        return MESH_ERR_INVALID;
    }

    memcpy(slot->payload, frame->payload, frame->payload_len);
    if (frame->payload_len < RING_MSG_PAYLOAD_SIZE) {
        memset(slot->payload + frame->payload_len, 0, RING_MSG_PAYLOAD_SIZE - frame->payload_len);
    }
    actor_mailbox_publish(actor, slot, (uint32_t)frame->src_node_id, MESH_MSG_TYPE_FRAME);
    return MESH_OK;
}

/* Additive wrapper: runs mesh_dispatch() unchanged (same validation, same
 * return codes, same actor routing -- nothing above this function
 * changes), then, only on MESH_OK and only for stores the caller actually
 * passed, records the delivery for observability into barry_storage.h's
 * static KV cache and time-series ring. Either store pointer may be NULL
 * to skip that recording -- existing callers of mesh_dispatch() itself
 * are entirely unaffected by this function's existence. metric_id must
 * already be registered by the caller (barry_ts_register_metric(), a
 * boot-time-only call, same posture as mesh_register_node()); an
 * unregistered metric_id simply means the time-series side of the
 * recording is skipped (the frame was still dispatched and, if ts_store
 * is NULL, is by definition considered untracked here), never a reason to
 * fail a frame that mesh_dispatch() itself already accepted.
 *
 * `tick` is an OPAQUE caller-supplied counter, exactly like
 * swarm_dispatch.h's `swarm_tick` (current_cycle) -- this function does
 * not invent a meaning for it (no wall clock, no dst_node_id repurposed
 * as a fake timestamp); the caller owns what "tick" means for its own
 * call rate, the same posture already established for swarm_tick. */
mesh_status_t mesh_dispatch_recorded(actor_system_t *sys, const mesh_registry_t *reg,
                                      const mesh_frame_t *frame,
                                      barry_kv_store_t *kv_store, barry_ts_store_t *ts_store,
                                      uint32_t metric_id, uint64_t tick) {
    mesh_status_t st = mesh_dispatch(sys, reg, frame);
    if (st != MESH_OK) {
        return st;
    }

    if (kv_store != NULL) {
        char key[24];
        int n = snprintf(key, sizeof key, "node:%u:last_frame", (unsigned)frame->dst_node_id);
        if (n > 0 && (size_t)n < (int)sizeof key) {
            (void)barry_kv_put(kv_store, key, (uint32_t)n, frame->payload, frame->payload_len);
        }
    }
    if (ts_store != NULL) {
        (void)barry_ts_record(ts_store, metric_id, tick, (double)frame->payload_len);
    }
    return MESH_OK;
}

/* ==================================================================== */
/* Part E -- Spatial perception: mesh nodes <-> signal_tracking.h        */
/* ==================================================================== */

mesh_status_t mesh_node_report_position(const mesh_registry_t *reg, track_registry_t *tracks, uint8_t node_id,
                                         fixed_q21_10_t x, fixed_q21_10_t y, fixed_q21_10_t z,
                                         uint64_t current_cycle) {
    if (reg == NULL || tracks == NULL) {
        return MESH_ERR_INVALID;
    }
    if (mesh_resolve(reg, node_id) == NULL) {
        return MESH_ERR_UNKNOWN_NODE; /* resolve first, act second -- same discipline as mesh_dispatch() */
    }
    sigtrack_status_t st = track_update_position(tracks, (uint32_t)node_id, x, y, z, current_cycle);
    return st == SIGTRACK_OK ? MESH_OK : MESH_ERR_TRACKING_FAILED;
}

mesh_status_t mesh_node_predict_position(const mesh_registry_t *reg, const track_registry_t *tracks, uint8_t node_id,
                                          uint64_t current_cycle,
                                          fixed_q21_10_t *out_x, fixed_q21_10_t *out_y, fixed_q21_10_t *out_z) {
    if (reg == NULL || tracks == NULL) {
        return MESH_ERR_INVALID;
    }
    if (mesh_resolve(reg, node_id) == NULL) {
        return MESH_ERR_UNKNOWN_NODE;
    }
    sigtrack_status_t st = track_predict_position(tracks, (uint32_t)node_id, current_cycle, out_x, out_y, out_z);
    return st == SIGTRACK_OK ? MESH_OK : MESH_ERR_TRACKING_FAILED;
}

#define MESH_TF_EVENT_DISPATCH 0x4D455348u /* "MESH" as bytes, distinct from wire-level tags elsewhere in this project */

typedef struct {
    uint8_t src_node_id;
    uint8_t dst_node_id;
    int16_t status; /* mesh_status_t, narrowed -- every value in that enum fits int16_t */
} mesh_tf_audit_payload_t;

mesh_status_t mesh_dispatch_audited(actor_system_t *sys, const mesh_registry_t *reg,
                                     const mesh_frame_t *frame, tf_log_t *audit_log) {
    mesh_status_t st = mesh_dispatch(sys, reg, frame);

    if (audit_log != NULL && frame != NULL) {
        mesh_tf_audit_payload_t payload;
        payload.src_node_id = frame->src_node_id;
        payload.dst_node_id = frame->dst_node_id;
        payload.status = (int16_t)st;
        (void)tf_log_append(audit_log, MESH_TF_EVENT_DISPATCH, &payload, (uint32_t)sizeof payload);
    }
    return st;
}

/* ==================================================================== */
/* Part C -- SoA load balancer                                           */
/* ==================================================================== */

void lb_pool_init(lb_pool_t *lb) {
    if (lb == NULL) {
        return;
    }
    memset(lb, 0, sizeof(*lb));
}

mesh_status_t lb_pool_add_worker(lb_pool_t *lb, actor_t *worker, uint32_t capacity) {
    if (lb == NULL || worker == NULL) {
        return MESH_ERR_INVALID;
    }
    if (lb->count >= LB_MAX_WORKERS) {
        return MESH_ERR_OUT_OF_RANGE;
    }
    lb->worker[lb->count]   = worker;
    lb->capacity[lb->count] = capacity;
    lb->count++;
    return MESH_OK;
}

mesh_status_t lb_pool_dispatch(lb_pool_t *lb, uint32_t sender_id, uint32_t type,
                                const void *payload, size_t payload_len,
                                uint8_t *out_worker_index) {
    if (lb == NULL || (payload == NULL && payload_len != 0u)) {
        return MESH_ERR_INVALID;
    }
    if (payload_len > RING_MSG_PAYLOAD_SIZE) {
        return MESH_ERR_PAYLOAD_TOO_LARGE;
    }

    /* Least-loaded among workers strictly under their own quota --
     * bounded by LB_MAX_WORKERS (a small fixed constant), not literal
     * O(1); see the header's precision note. */
    int    best      = -1;
    size_t best_load = 0;
    for (uint32_t i = 0; i < lb->count; i++) {
        size_t load = actor_mailbox_count(lb->worker[i]);
        if (load < lb->capacity[i] && (best < 0 || load < best_load)) {
            best = (int)i;
            best_load = load;
        }
    }
    if (best < 0) {
        return MESH_ERR_BACKPRESSURE; /* every worker at or above its quota -- deterministic, immediate */
    }

    ring_msg_t *slot;
    actor_status_t ast = actor_mailbox_reserve(lb->worker[best], &slot);
    if (ast == ACTOR_ERR_MAILBOX_FULL) {
        /* Raced with another producer between the load check above and
         * this reservation -- report it the same way, still O(1). */
        return MESH_ERR_BACKPRESSURE;
    }
    if (ast != ACTOR_OK) {
        return MESH_ERR_INVALID;
    }

    memcpy(slot->payload, payload, payload_len);
    if (payload_len < RING_MSG_PAYLOAD_SIZE) {
        memset(slot->payload + payload_len, 0, RING_MSG_PAYLOAD_SIZE - payload_len);
    }
    actor_mailbox_publish(lb->worker[best], slot, sender_id, type);

    if (out_worker_index != NULL) {
        *out_worker_index = (uint8_t)best;
    }
    return MESH_OK;
}

/* ==================================================================== */
/* Part D -- Local immutable, content-hashed block store                 */
/* ==================================================================== */

static void mesh_store_path(char *out, size_t out_cap, const char *dest_dir, uint32_t object_id) {
    snprintf(out, out_cap, "%s/%08x.blk", dest_dir, object_id);
}

mesh_status_t mesh_store_put(const char *dest_dir, uint32_t object_id, const wire_msg_t *block) {
    if (dest_dir == NULL || dest_dir[0] == '\0' || block == NULL) {
        return MESH_ERR_INVALID;
    }

    /* Hash validated (computed) BEFORE the write, over the exact bytes
     * about to be written -- "valider leur integrite... avant d'ecrire",
     * per the mission text, taken literally. */
    uint8_t buf[sizeof(wire_msg_t) + sizeof(uint64_t)];
    memcpy(buf, block, sizeof(wire_msg_t));
    uint64_t hash = mesh_fnv1a64((const uint8_t *)block, sizeof(wire_msg_t));
    memcpy(buf + sizeof(wire_msg_t), &hash, sizeof hash);

    char path[512];
    mesh_store_path(path, sizeof path, dest_dir, object_id);

    /* _O_EXCL: the OS itself refuses to open a file that already exists,
     * atomically -- immutability enforced by the kernel, not by a
     * check-then-create race this process could lose. */
    int fd = _open(path, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return (errno == EEXIST) ? MESH_ERR_STORE_EXISTS : MESH_ERR_STORE_IO;
    }

    size_t written = 0;
    while (written < sizeof buf) {
        int n = _write(fd, buf + written, (unsigned int)(sizeof buf - written));
        if (n <= 0) {
            _close(fd);
            return MESH_ERR_STORE_IO;
        }
        written += (size_t)n;
    }
    if (_close(fd) != 0) {
        return MESH_ERR_STORE_IO;
    }
    return MESH_OK;
}

mesh_status_t mesh_store_get(const char *dest_dir, uint32_t object_id, wire_msg_t *out) {
    if (dest_dir == NULL || dest_dir[0] == '\0' || out == NULL) {
        return MESH_ERR_INVALID;
    }

    char path[512];
    mesh_store_path(path, sizeof path, dest_dir, object_id);

    int fd = _open(path, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        return (errno == ENOENT) ? MESH_ERR_STORE_NOT_FOUND : MESH_ERR_STORE_IO;
    }

    uint8_t buf[sizeof(wire_msg_t) + sizeof(uint64_t)];
    size_t total = 0;
    while (total < sizeof buf) {
        int n = _read(fd, buf + total, (unsigned int)(sizeof buf - total));
        if (n < 0) {
            _close(fd);
            return MESH_ERR_STORE_IO;
        }
        if (n == 0) {
            break; /* short/truncated file */
        }
        total += (size_t)n;
    }
    _close(fd);

    if (total != sizeof buf) {
        return MESH_ERR_STORE_CORRUPT; /* wrong size on disk -- treated as corruption, not a distinct case */
    }

    uint64_t stored_hash;
    memcpy(&stored_hash, buf + sizeof(wire_msg_t), sizeof stored_hash);
    uint64_t computed = mesh_fnv1a64(buf, sizeof(wire_msg_t));
    if (computed != stored_hash) {
        return MESH_ERR_STORE_CORRUPT;
    }

    memcpy(out, buf, sizeof(wire_msg_t));
    return MESH_OK;
}
