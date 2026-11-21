/*
 * telemetry_fabric.c
 *
 * See include/telemetry_fabric.h for the full design and honest-scope
 * notes. Implementation only; non-obvious decisions are explained there,
 * not duplicated here.
 */

#include "telemetry_fabric.h"

#include <string.h>

/* Portable cooperative yield for the Niveau 54 CAS wait below -- never a
 * blocking wait (no mutex/futex/Sleep()), just a hint that lets a
 * genuinely contended CPU reschedule the predecessor's writer thread
 * sooner. telemetry_fabric.c is built on every platform this project
 * targets (unlike net_io.c, which is Windows-only), so this stays
 * conditional rather than assuming Windows. */
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #define TF_YIELD() SwitchToThread()
#else
    #include <sched.h>
    #define TF_YIELD() sched_yield()
#endif

/* Same standard FNV-1a/64 offset basis and prime infra_mesh.c's
 * mesh_fnv1a64() / barry_storage.c's barry_fnv1a64() use -- reimplemented
 * locally, the same "duplication for module independence" posture those
 * files already document for their own copies. */
static uint64_t tf_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Checksum over every tf_log_entry_t field except `ready` (a pure
 * concurrency-handoff flag, never part of the tamper-evident content) and
 * `checksum` itself (computed last, over everything before it). Hashes
 * each field's raw bytes in a fixed, documented order rather than the
 * struct's in-memory layout directly, so this function's meaning never
 * silently shifts if the struct is ever reordered/padded differently. */
static uint64_t tf_compute_entry_checksum(const tf_log_entry_t *e) {
    uint8_t buf[8u + 8u + 4u + 4u + TF_LOG_MSG_MAX];
    size_t off = 0u;

    memcpy(buf + off, &e->seq, sizeof e->seq); off += sizeof e->seq;
    memcpy(buf + off, &e->prev_checksum, sizeof e->prev_checksum); off += sizeof e->prev_checksum;
    memcpy(buf + off, &e->event_type, sizeof e->event_type); off += sizeof e->event_type;
    memcpy(buf + off, &e->payload_len, sizeof e->payload_len); off += sizeof e->payload_len;
    memcpy(buf + off, e->payload, TF_LOG_MSG_MAX); off += TF_LOG_MSG_MAX;

    return tf_fnv1a64(buf, off);
}

/* ==================================================================== */
/* Part A -- Immutable, hash-chained write-ahead log                     */
/* ==================================================================== */

void tf_log_init(tf_log_t *log) {
    atomic_store_explicit(&log->write_pos, 0u, memory_order_relaxed);
    for (uint32_t i = 0; i < TF_LOG_CAPACITY; i++) {
        tf_log_entry_t *e = &log->entries[i];
        atomic_store_explicit(&e->ready, 0u, memory_order_relaxed);
        e->seq = 0u;
        e->prev_checksum = 0u;
        e->event_type = 0u;
        e->payload_len = 0u;
        memset(e->payload, 0, sizeof e->payload);
        e->checksum = 0u;
    }
    atomic_store_explicit(&log->tamper_flag, false, memory_order_relaxed);
}

tf_status_t tf_log_append(tf_log_t *log, uint32_t event_type, const void *payload, uint32_t payload_len) {
    if (log == NULL || payload_len > TF_LOG_MSG_MAX || (payload_len > 0u && payload == NULL)) {
        return TF_ERR_INVALID;
    }

    uint64_t pos = atomic_fetch_add_explicit(&log->write_pos, 1u, memory_order_acq_rel);
    if (pos >= TF_LOG_CAPACITY) {
        atomic_fetch_sub_explicit(&log->write_pos, 1u, memory_order_acq_rel); /* O(1) rollback: never leaves a permanent gap for a rejected append */
        return TF_ERR_FULL;
    }
    tf_log_entry_t *entry = &log->entries[pos];

    /* CWE-457: zero the WHOLE entry -- including any compiler-inserted
     * padding between fields, which the per-field assignment below never
     * touches by name -- synchronously, before a single field is set and
     * before the checksum is computed. This slot is claimed exactly once
     * (write_pos only ever grows; a position is never reused/recycled),
     * so no other thread can observe or race this write: a plain
     * (non-atomic) write to an _Atomic object is well-defined here
     * precisely because there is provably no concurrent access to it,
     * the same "exclusive ownership until published" invariant every
     * other module's own init path already relies on. Still followed by
     * an explicit atomic_store below for `ready` specifically, matching
     * this codebase's own established convention (barry_storage.c's
     * barry_task_queue_init(), ring_buffer.c's rb_init()) of never
     * relying on memset alone for a field the atomic API also touches. */
    memset(entry, 0, sizeof *entry);
    atomic_store_explicit(&entry->ready, 0u, memory_order_relaxed);

    uint64_t prev_checksum = TF_LOG_GENESIS_CHECKSUM;
    if (pos > 0u) {
        tf_log_entry_t *prev_entry = &log->entries[pos - 1u];

        /* Niveau 54: unbounded, non-blocking active wait via CAS -- see
         * this function's declaration (telemetry_fabric.h) for exactly
         * why the previous fixed-bound "give up and read anyway" design
         * was a real data-race bug, not just a theoretical one. The CAS
         * only ever "succeeds" once prev_entry->ready is genuinely 1
         * (comparing against the only value that can legitimately follow
         * 0); on any failure -- spurious or real -- the standard writes
         * the actually-observed value into `expected`, so it is reset to
         * 1u before the next attempt. Success carries memory_order_
         * acq_rel, which is what makes the plain (non-atomic) read of
         * prev_entry->checksum below safe: it happens-after the
         * predecessor's release-store of its own `ready`. */
        uint32_t expected = 1u;
        uint32_t spins = 0u;
        while (!atomic_compare_exchange_weak_explicit(&prev_entry->ready, &expected, 1u,
                                                        memory_order_acq_rel, memory_order_acquire)) {
            expected = 1u;
            spins++;
            if ((spins % TF_LOG_SPIN_BOUND) == 0u) {
                TF_YIELD(); /* cooperative only -- never a blocking wait */
            }
        }
        prev_checksum = prev_entry->checksum; /* safe: happens-after the CAS above actually observed ready == 1 */
    }

    entry->seq = pos;
    entry->prev_checksum = prev_checksum;
    entry->event_type = event_type;
    entry->payload_len = payload_len;
    if (payload_len > 0u) {
        memcpy(entry->payload, payload, payload_len);
    }
    /* payload's unused tail (and every other unset byte, padding
     * included) is already zero from the whole-entry memset above --
     * no separate tail-zeroing needed here anymore. */
    entry->checksum = tf_compute_entry_checksum(entry);
    atomic_store_explicit(&entry->ready, 1u, memory_order_release);
    return TF_OK;
}

tf_status_t tf_log_verify(tf_log_t *log, uint64_t *out_first_broken_seq) {
    if (log == NULL) {
        return TF_ERR_INVALID;
    }
    uint64_t count = atomic_load_explicit(&log->write_pos, memory_order_acquire);
    if (count > TF_LOG_CAPACITY) {
        count = TF_LOG_CAPACITY; /* defensive clamp on a stale/racy read, same posture as barry_index_lookup() */
    }

    uint64_t prev_checksum = TF_LOG_GENESIS_CHECKSUM;
    for (uint64_t i = 0u; i < count; i++) {
        const tf_log_entry_t *e = &log->entries[i];
        if (atomic_load_explicit(&e->ready, memory_order_acquire) == 0u) {
            if (out_first_broken_seq != NULL) {
                *out_first_broken_seq = i;
            }
            return TF_ERR_NOT_READY; /* a concurrent append in flight -- benign, not tampering */
        }
        if (e->seq != i || e->prev_checksum != prev_checksum) {
            if (out_first_broken_seq != NULL) {
                *out_first_broken_seq = i;
            }
            atomic_store_explicit(&log->tamper_flag, true, memory_order_relaxed);
            return TF_ERR_TAMPERED;
        }
        uint64_t recomputed = tf_compute_entry_checksum(e);
        if (recomputed != e->checksum) {
            if (out_first_broken_seq != NULL) {
                *out_first_broken_seq = i;
            }
            atomic_store_explicit(&log->tamper_flag, true, memory_order_relaxed);
            return TF_ERR_TAMPERED;
        }
        prev_checksum = e->checksum;
    }
    return TF_OK;
}

tf_status_t tf_log_verify_and_alert(tf_log_t *log, uint64_t *out_first_broken_seq,
                                     void *swarm_sys, uint16_t tenant_id, tf_alert_fn raise_alert_fn) {
    tf_status_t st = tf_log_verify(log, out_first_broken_seq);
    if (st == TF_ERR_TAMPERED && raise_alert_fn != NULL) {
        (void)raise_alert_fn(swarm_sys, tenant_id);
    }
    return st;
}

/* ==================================================================== */
/* Part B -- Constant-time tenant access-key store                       */
/* ==================================================================== */

void tf_key_store_init(tf_tenant_key_store_t *store) {
    for (uint32_t i = 0; i < TF_KEY_STORE_MAX_TENANTS; i++) {
        store->tenant_id[i] = 0u;
        store->key_hash[i] = 0u;
        store->registered[i] = false;
    }
}

tf_status_t tf_key_store_register(tf_tenant_key_store_t *store, uint16_t slot, uint16_t tenant_id,
                                   const void *key, uint32_t key_len) {
    if (store == NULL || key == NULL || key_len == 0u) {
        return TF_ERR_INVALID;
    }
    if (slot >= TF_KEY_STORE_MAX_TENANTS) {
        return TF_ERR_OUT_OF_RANGE;
    }
    if (store->registered[slot]) {
        return TF_ERR_ALREADY_REGISTERED;
    }
    store->tenant_id[slot] = tenant_id;
    store->key_hash[slot] = tf_fnv1a64((const uint8_t *)key, key_len);
    store->registered[slot] = true;
    return TF_OK;
}

tf_status_t tf_key_store_verify(const tf_tenant_key_store_t *store, uint16_t slot,
                                 const void *provided_key, uint32_t provided_key_len,
                                 uint16_t *out_tenant_id) {
    if (store == NULL || provided_key == NULL) {
        return TF_ERR_INVALID;
    }
    if (slot >= TF_KEY_STORE_MAX_TENANTS) {
        return TF_ERR_OUT_OF_RANGE;
    }
    if (!store->registered[slot]) {
        return TF_ERR_NOT_FOUND;
    }
    uint64_t provided_hash = tf_fnv1a64((const uint8_t *)provided_key, provided_key_len);
    if (!ct_equal_u64(provided_hash, store->key_hash[slot])) {
        return TF_ERR_NOT_FOUND; /* same "no distinct unauthorized-vs-not-found signal" posture as a lookup miss */
    }
    if (out_tenant_id != NULL) {
        *out_tenant_id = store->tenant_id[slot];
    }
    return TF_OK;
}
