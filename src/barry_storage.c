/*
 * barry_storage.c
 *
 * See include/barry_storage.h for the full design and honest-scope notes
 * (sharding semantics, the seqlock read discipline, and exactly which
 * Niveau 11-20 items are and are not attempted). This file is the
 * implementation only; every non-obvious decision is explained there, not
 * duplicated here.
 */

#include "barry_storage.h"

#include <string.h>

/* Same standard FNV-1a/64 offset basis and prime infra_mesh.c's
 * mesh_fnv1a64() and net_gateway.c's ngw_fnv1a64() use -- reimplemented
 * locally rather than shared, the same "duplication for module
 * independence" posture infra_mesh.c already documents for its own copy. */
static uint64_t barry_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ==================================================================== */
/* Part A -- KV cache store                                              */
/* ==================================================================== */

void barry_kv_init(barry_kv_store_t *store) {
    for (uint32_t s = 0; s < BARRY_KV_SHARD_COUNT; s++) {
        for (uint32_t i = 0; i < BARRY_KV_SHARD_CAPACITY; i++) {
            barry_kv_slot_t *slot = &store->shards[s].slots[i];
            atomic_store_explicit(&slot->state, (uint32_t)BARRY_SLOT_EMPTY, memory_order_relaxed);
            atomic_store_explicit(&slot->version, 0u, memory_order_relaxed);
            slot->hash = 0u;
            slot->key_len = 0u;
            slot->val_len = 0u;
            memset(slot->key, 0, sizeof slot->key);
            memset(slot->val, 0, sizeof slot->val);
        }
        atomic_store_explicit(&store->shards[s].occupied_count, 0u, memory_order_relaxed);
    }
    atomic_store_explicit(&store->active_shard_count, 1u, memory_order_relaxed);
    atomic_store_explicit(&store->shard_promotions_total, 0u, memory_order_relaxed);
}

/* Locates an OCCUPIED slot matching (hash, key) within one shard, or NULL
 * if the key was never inserted along this shard's probe chain. Skips
 * TOMBSTONEs; gives a transient RESERVED/UPDATING slot a small bounded
 * chance to settle before treating it as "keep probing" -- see the
 * concurrency note in barry_storage.h. */
static barry_kv_slot_t *kv_shard_find(barry_kv_shard_t *shard, uint64_t hash,
                                       const uint8_t *key, uint32_t key_len) {
    uint32_t mask = BARRY_KV_SHARD_CAPACITY - 1u;
    uint32_t idx0 = (uint32_t)(hash & (uint64_t)mask);

    for (uint32_t probe = 0; probe < BARRY_KV_MAX_PROBE; probe++) {
        uint32_t idx = (idx0 + probe) & mask;
        barry_kv_slot_t *slot = &shard->slots[idx];
        uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);

        for (int spin = 0; spin < 64 &&
             (state == (uint32_t)BARRY_SLOT_RESERVED || state == (uint32_t)BARRY_SLOT_UPDATING);
             spin++) {
            state = atomic_load_explicit(&slot->state, memory_order_acquire);
        }

        if (state == (uint32_t)BARRY_SLOT_EMPTY) {
            return NULL;
        }
        if (state == (uint32_t)BARRY_SLOT_OCCUPIED && slot->hash == hash &&
            slot->key_len == key_len && memcmp(slot->key, key, key_len) == 0) {
            return slot;
        }
        /* TOMBSTONE, non-matching OCCUPIED, or still busy after the spin
         * bound: keep probing -- this key may live further down the chain. */
    }
    return NULL;
}

static void kv_maybe_promote(barry_kv_store_t *store, uint32_t active, uint32_t occupied_in_target) {
    if (active >= BARRY_KV_SHARD_COUNT) {
        return; /* every pre-reserved shard already activated */
    }
    /* occupied/CAPACITY >= WATERMARK_PCT/100, without floating point */
    uint64_t lhs = (uint64_t)occupied_in_target * 100u;
    uint64_t rhs = (uint64_t)BARRY_KV_SHARD_CAPACITY * BARRY_SHARD_WATERMARK_PCT;
    if (lhs < rhs) {
        return;
    }
    uint32_t expected = active;
    if (atomic_compare_exchange_strong_explicit(&store->active_shard_count, &expected, active + 1u,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&store->shard_promotions_total, 1u, memory_order_relaxed);
    }
    /* CAS loss just means another thread already promoted with the same
     * `active` snapshot -- no retry needed, the next put() call observes
     * the already-promoted count via a fresh atomic_load(). */
}

/*
 * Forces the next pre-reserved shard into service RIGHT NOW, regardless of
 * the 80% watermark -- the fallback path for when a fresh insert's bounded
 * probe chain (BARRY_KV_MAX_PROBE) comes up empty on the current
 * write-target shard even though that shard is not literally full. Open
 * addressing's bounded probe length is a PROBABILISTIC guarantee, not an
 * absolute one: as a shard's load factor climbs into the high 80s/90s
 * percent (which a burst of concurrent inserts CAN reach in the window
 * between crossing the 80% watermark and every producer thread noticing
 * the resulting promotion on its own next call), the chance that some
 * particular key's whole probe chain lands on already-occupied slots
 * rises with it -- this is a real, measured failure mode of linear
 * probing under load, not a theoretical one (see the barry_kv_put() call
 * site: it does not treat this as best-effort, it treats it as the same
 * kind of load-triggered fragmentation trigger the watermark already is,
 * just a second, harder trigger for the case the first one was not
 * enough). Returns true iff active_shard_count is now > `active` --
 * whether because THIS call won the CAS, or because another thread
 * already promoted past `active` for the same reason. Returns false only
 * when every pre-reserved shard is already active (a genuine, final
 * capacity limit, not a probe-length artifact). */
static bool kv_force_promote(barry_kv_store_t *store, uint32_t active) {
    if (active >= BARRY_KV_SHARD_COUNT) {
        return false;
    }
    uint32_t expected = active;
    if (atomic_compare_exchange_strong_explicit(&store->active_shard_count, &expected, active + 1u,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&store->shard_promotions_total, 1u, memory_order_relaxed);
    }
    return atomic_load_explicit(&store->active_shard_count, memory_order_acquire) > active;
}

barry_status_t barry_kv_put(barry_kv_store_t *store, const void *key, uint32_t key_len,
                             const void *val, uint32_t val_len) {
    if (store == NULL || key == NULL || key_len == 0u || key_len > BARRY_KV_KEY_MAX ||
        val_len > BARRY_KV_VAL_MAX || (val_len > 0u && val == NULL)) {
        return BARRY_ERR_INVALID;
    }

    const uint8_t *key_bytes = (const uint8_t *)key;
    uint64_t hash = barry_fnv1a64(key_bytes, key_len);
    uint32_t active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);

    /* Update path: the key may already live in ANY active shard (no
     * migration ever moves it), so scan newest -> oldest. */
    for (uint32_t i = active; i > 0u; i--) {
        barry_kv_shard_t *shard = &store->shards[i - 1u];
        barry_kv_slot_t *slot = kv_shard_find(shard, hash, key_bytes, key_len);
        if (slot == NULL) {
            continue;
        }
        uint32_t expected = (uint32_t)BARRY_SLOT_OCCUPIED;
        if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_UPDATING,
                                                       memory_order_acq_rel, memory_order_acquire)) {
            continue; /* lost the race (concurrent delete/update) -- keep scanning */
        }
        atomic_fetch_add_explicit(&slot->version, 1u, memory_order_acq_rel); /* now odd: write in progress */
        if (val_len > 0u) {
            memcpy(slot->val, val, val_len);
        }
        if (val_len < BARRY_KV_VAL_MAX) {
            memset(slot->val + val_len, 0, BARRY_KV_VAL_MAX - val_len);
        }
        slot->val_len = val_len;
        atomic_fetch_add_explicit(&slot->version, 1u, memory_order_release); /* even again: stable */
        atomic_store_explicit(&slot->state, (uint32_t)BARRY_SLOT_OCCUPIED, memory_order_release);
        return BARRY_OK;
    }

    /* Fresh insert: targets the newest active (write-target) shard, and
     * retries on the NEXT pre-reserved shard (kv_force_promote()) if this
     * shard's bounded probe chain comes up empty -- bounded by
     * BARRY_KV_SHARD_COUNT attempts total (a small compile-time constant),
     * so this stays O(1)-amortized (bounded probe x bounded shard
     * fallback), never an unbounded retry. See kv_force_promote()'s
     * comment for why this second trigger is needed alongside the
     * watermark one. */
    uint32_t mask = BARRY_KV_SHARD_CAPACITY - 1u;
    uint32_t idx0 = (uint32_t)(hash & (uint64_t)mask);

    for (;;) {
        barry_kv_shard_t *target = &store->shards[active - 1u];
        bool inserted = false;

        for (uint32_t probe = 0; probe < BARRY_KV_MAX_PROBE; probe++) {
            uint32_t idx = (idx0 + probe) & mask;
            barry_kv_slot_t *slot = &target->slots[idx];

            uint32_t expected = (uint32_t)BARRY_SLOT_EMPTY;
            bool claimed = atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_RESERVED,
                                                                     memory_order_acq_rel, memory_order_acquire);
            if (!claimed) {
                expected = (uint32_t)BARRY_SLOT_TOMBSTONE;
                claimed = atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_RESERVED,
                                                                    memory_order_acq_rel, memory_order_acquire);
            }
            if (!claimed) {
                continue;
            }

            atomic_fetch_add_explicit(&slot->version, 1u, memory_order_acq_rel); /* odd: writing */
            slot->hash = hash;
            slot->key_len = key_len;
            memcpy(slot->key, key_bytes, key_len);
            if (key_len < BARRY_KV_KEY_MAX) {
                memset(slot->key + key_len, 0, BARRY_KV_KEY_MAX - key_len);
            }
            if (val_len > 0u) {
                memcpy(slot->val, val, val_len);
            }
            if (val_len < BARRY_KV_VAL_MAX) {
                memset(slot->val + val_len, 0, BARRY_KV_VAL_MAX - val_len);
            }
            slot->val_len = val_len;
            atomic_fetch_add_explicit(&slot->version, 1u, memory_order_release); /* even: stable */
            atomic_store_explicit(&slot->state, (uint32_t)BARRY_SLOT_OCCUPIED, memory_order_release);

            uint32_t occ = atomic_fetch_add_explicit(&target->occupied_count, 1u, memory_order_acq_rel) + 1u;
            kv_maybe_promote(store, active, occ);
            inserted = true;
            break;
        }

        if (inserted) {
            return BARRY_OK;
        }
        if (!kv_force_promote(store, active)) {
            return BARRY_ERR_FULL; /* every pre-reserved shard is genuinely exhausted */
        }
        active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);
    }
}

barry_status_t barry_kv_get(barry_kv_store_t *store, const void *key, uint32_t key_len,
                             void *out_val, uint32_t *inout_val_len) {
    if (store == NULL || key == NULL || key_len == 0u || key_len > BARRY_KV_KEY_MAX ||
        inout_val_len == NULL) {
        return BARRY_ERR_INVALID;
    }

    const uint8_t *key_bytes = (const uint8_t *)key;
    uint64_t hash = barry_fnv1a64(key_bytes, key_len);
    uint32_t active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);

    for (uint32_t i = active; i > 0u; i--) {
        barry_kv_shard_t *shard = &store->shards[i - 1u];
        barry_kv_slot_t *slot = kv_shard_find(shard, hash, key_bytes, key_len);
        if (slot == NULL) {
            continue;
        }

        uint8_t  tmp[BARRY_KV_VAL_MAX];
        uint32_t tmp_len = 0u;
        bool     consistent = false;

        for (int attempt = 0; attempt < 32 && !consistent; attempt++) {
            uint32_t v1 = atomic_load_explicit(&slot->version, memory_order_acquire);
            if (v1 & 1u) {
                continue; /* writer mid-flight: retry */
            }
            tmp_len = slot->val_len;
            if (tmp_len > BARRY_KV_VAL_MAX) {
                continue; /* torn read of val_len itself: retry */
            }
            memcpy(tmp, slot->val, tmp_len);
            uint32_t v2 = atomic_load_explicit(&slot->version, memory_order_acquire);
            consistent = (v1 == v2);
        }

        if (!consistent) {
            /* Persistent contention on this exact slot across every bounded
             * retry -- documented best-effort limit, see barry_storage.h.
             * Report "not found" rather than risk handing back a torn
             * value; the caller's next get() will very likely succeed. */
            return BARRY_ERR_NOT_FOUND;
        }
        if (tmp_len > *inout_val_len) {
            return BARRY_ERR_INVALID; /* caller's out_val buffer too small */
        }
        if (out_val != NULL && tmp_len > 0u) {
            memcpy(out_val, tmp, tmp_len);
        }
        *inout_val_len = tmp_len;
        return BARRY_OK;
    }

    return BARRY_ERR_NOT_FOUND;
}

barry_status_t barry_kv_delete(barry_kv_store_t *store, const void *key, uint32_t key_len) {
    if (store == NULL || key == NULL || key_len == 0u || key_len > BARRY_KV_KEY_MAX) {
        return BARRY_ERR_INVALID;
    }

    const uint8_t *key_bytes = (const uint8_t *)key;
    uint64_t hash = barry_fnv1a64(key_bytes, key_len);
    uint32_t active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);

    for (uint32_t i = active; i > 0u; i--) {
        barry_kv_shard_t *shard = &store->shards[i - 1u];
        barry_kv_slot_t *slot = kv_shard_find(shard, hash, key_bytes, key_len);
        if (slot == NULL) {
            continue;
        }
        uint32_t expected = (uint32_t)BARRY_SLOT_OCCUPIED;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_TOMBSTONE,
                                                      memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_sub_explicit(&shard->occupied_count, 1u, memory_order_acq_rel);
            return BARRY_OK;
        }
        /* lost the race (concurrent update/delete) -- another caller
         * resolved this key first; keep scanning older shards in case a
         * duplicate somehow existed (it should not, but this stays
         * correct even if it did). */
    }
    return BARRY_ERR_NOT_FOUND;
}

uint32_t barry_kv_active_shard_count(const barry_kv_store_t *store) {
    return atomic_load_explicit(&store->active_shard_count, memory_order_acquire);
}

uint64_t barry_kv_shard_promotions_total(const barry_kv_store_t *store) {
    return atomic_load_explicit(&store->shard_promotions_total, memory_order_acquire);
}

/* ==================================================================== */
/* Part B -- Documentary store + relation index                          */
/* ==================================================================== */

void barry_doc_init(barry_doc_store_t *store) {
    for (uint32_t s = 0; s < BARRY_DOC_SHARD_COUNT; s++) {
        for (uint32_t i = 0; i < BARRY_DOC_SHARD_CAPACITY; i++) {
            barry_doc_slot_t *slot = &store->shards[s].slots[i];
            atomic_store_explicit(&slot->state, (uint32_t)BARRY_SLOT_EMPTY, memory_order_relaxed);
            atomic_store_explicit(&slot->version, 0u, memory_order_relaxed);
            slot->hash = 0u;
            slot->key_len = 0u;
            memset(slot->key, 0, sizeof slot->key);
            memset(&slot->doc, 0, sizeof slot->doc);
        }
        atomic_store_explicit(&store->shards[s].occupied_count, 0u, memory_order_relaxed);
    }
    atomic_store_explicit(&store->active_shard_count, 1u, memory_order_relaxed);
    atomic_store_explicit(&store->shard_promotions_total, 0u, memory_order_relaxed);
}

static barry_doc_slot_t *doc_shard_find(barry_doc_shard_t *shard, uint64_t hash,
                                         const uint8_t *key, uint32_t key_len) {
    uint32_t mask = BARRY_DOC_SHARD_CAPACITY - 1u;
    uint32_t idx0 = (uint32_t)(hash & (uint64_t)mask);

    for (uint32_t probe = 0; probe < BARRY_DOC_MAX_PROBE; probe++) {
        uint32_t idx = (idx0 + probe) & mask;
        barry_doc_slot_t *slot = &shard->slots[idx];
        uint32_t state = atomic_load_explicit(&slot->state, memory_order_acquire);

        for (int spin = 0; spin < 64 &&
             (state == (uint32_t)BARRY_SLOT_RESERVED || state == (uint32_t)BARRY_SLOT_UPDATING);
             spin++) {
            state = atomic_load_explicit(&slot->state, memory_order_acquire);
        }

        if (state == (uint32_t)BARRY_SLOT_EMPTY) {
            return NULL;
        }
        if (state == (uint32_t)BARRY_SLOT_OCCUPIED && slot->hash == hash &&
            slot->key_len == key_len && memcmp(slot->key, key, key_len) == 0) {
            return slot;
        }
    }
    return NULL;
}

static void doc_maybe_promote(barry_doc_store_t *store, uint32_t active, uint32_t occupied_in_target) {
    if (active >= BARRY_DOC_SHARD_COUNT) {
        return;
    }
    uint64_t lhs = (uint64_t)occupied_in_target * 100u;
    uint64_t rhs = (uint64_t)BARRY_DOC_SHARD_CAPACITY * BARRY_SHARD_WATERMARK_PCT;
    if (lhs < rhs) {
        return;
    }
    uint32_t expected = active;
    if (atomic_compare_exchange_strong_explicit(&store->active_shard_count, &expected, active + 1u,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&store->shard_promotions_total, 1u, memory_order_relaxed);
    }
}

/* Same fallback as kv_force_promote() (see its comment for why this is
 * needed alongside the watermark trigger): forces the next pre-reserved
 * shard into service when a fresh insert's bounded probe chain comes up
 * empty on the current write-target shard. */
static bool doc_force_promote(barry_doc_store_t *store, uint32_t active) {
    if (active >= BARRY_DOC_SHARD_COUNT) {
        return false;
    }
    uint32_t expected = active;
    if (atomic_compare_exchange_strong_explicit(&store->active_shard_count, &expected, active + 1u,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&store->shard_promotions_total, 1u, memory_order_relaxed);
    }
    return atomic_load_explicit(&store->active_shard_count, memory_order_acquire) > active;
}

barry_status_t barry_doc_put(barry_doc_store_t *store, const void *key, uint32_t key_len,
                              const barry_document_t *doc) {
    if (store == NULL || key == NULL || key_len == 0u || key_len > BARRY_DOC_KEY_MAX ||
        doc == NULL || doc->field_count > BARRY_DOC_MAX_FIELDS) {
        return BARRY_ERR_INVALID;
    }

    const uint8_t *key_bytes = (const uint8_t *)key;
    uint64_t hash = barry_fnv1a64(key_bytes, key_len);
    uint32_t active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);

    for (uint32_t i = active; i > 0u; i--) {
        barry_doc_shard_t *shard = &store->shards[i - 1u];
        barry_doc_slot_t *slot = doc_shard_find(shard, hash, key_bytes, key_len);
        if (slot == NULL) {
            continue;
        }
        uint32_t expected = (uint32_t)BARRY_SLOT_OCCUPIED;
        if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_UPDATING,
                                                       memory_order_acq_rel, memory_order_acquire)) {
            continue;
        }
        atomic_fetch_add_explicit(&slot->version, 1u, memory_order_acq_rel);
        slot->doc = *doc;
        atomic_fetch_add_explicit(&slot->version, 1u, memory_order_release);
        atomic_store_explicit(&slot->state, (uint32_t)BARRY_SLOT_OCCUPIED, memory_order_release);
        return BARRY_OK;
    }

    /* Same bounded shard-fallback retry as barry_kv_put() -- see
     * kv_force_promote()'s comment for why probe exhaustion on a
     * not-literally-full shard is a real failure mode, not theoretical. */
    uint32_t mask = BARRY_DOC_SHARD_CAPACITY - 1u;
    uint32_t idx0 = (uint32_t)(hash & (uint64_t)mask);

    for (;;) {
        barry_doc_shard_t *target = &store->shards[active - 1u];
        bool inserted = false;

        for (uint32_t probe = 0; probe < BARRY_DOC_MAX_PROBE; probe++) {
            uint32_t idx = (idx0 + probe) & mask;
            barry_doc_slot_t *slot = &target->slots[idx];

            uint32_t expected = (uint32_t)BARRY_SLOT_EMPTY;
            bool claimed = atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_RESERVED,
                                                                     memory_order_acq_rel, memory_order_acquire);
            if (!claimed) {
                expected = (uint32_t)BARRY_SLOT_TOMBSTONE;
                claimed = atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_RESERVED,
                                                                    memory_order_acq_rel, memory_order_acquire);
            }
            if (!claimed) {
                continue;
            }

            atomic_fetch_add_explicit(&slot->version, 1u, memory_order_acq_rel);
            slot->hash = hash;
            slot->key_len = key_len;
            memcpy(slot->key, key_bytes, key_len);
            if (key_len < BARRY_DOC_KEY_MAX) {
                memset(slot->key + key_len, 0, BARRY_DOC_KEY_MAX - key_len);
            }
            slot->doc = *doc;
            atomic_fetch_add_explicit(&slot->version, 1u, memory_order_release);
            atomic_store_explicit(&slot->state, (uint32_t)BARRY_SLOT_OCCUPIED, memory_order_release);

            uint32_t occ = atomic_fetch_add_explicit(&target->occupied_count, 1u, memory_order_acq_rel) + 1u;
            doc_maybe_promote(store, active, occ);
            inserted = true;
            break;
        }

        if (inserted) {
            return BARRY_OK;
        }
        if (!doc_force_promote(store, active)) {
            return BARRY_ERR_FULL;
        }
        active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);
    }
}

barry_status_t barry_doc_get(barry_doc_store_t *store, const void *key, uint32_t key_len,
                              barry_document_t *out_doc) {
    if (store == NULL || key == NULL || key_len == 0u || key_len > BARRY_DOC_KEY_MAX ||
        out_doc == NULL) {
        return BARRY_ERR_INVALID;
    }

    const uint8_t *key_bytes = (const uint8_t *)key;
    uint64_t hash = barry_fnv1a64(key_bytes, key_len);
    uint32_t active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);

    for (uint32_t i = active; i > 0u; i--) {
        barry_doc_shard_t *shard = &store->shards[i - 1u];
        barry_doc_slot_t *slot = doc_shard_find(shard, hash, key_bytes, key_len);
        if (slot == NULL) {
            continue;
        }

        barry_document_t tmp;
        bool consistent = false;

        for (int attempt = 0; attempt < 32 && !consistent; attempt++) {
            uint32_t v1 = atomic_load_explicit(&slot->version, memory_order_acquire);
            if (v1 & 1u) {
                continue;
            }
            tmp = slot->doc;
            uint32_t v2 = atomic_load_explicit(&slot->version, memory_order_acquire);
            consistent = (v1 == v2);
        }

        if (!consistent) {
            return BARRY_ERR_NOT_FOUND; /* see barry_kv_get()'s identical documented limit */
        }
        *out_doc = tmp;
        return BARRY_OK;
    }

    return BARRY_ERR_NOT_FOUND;
}

barry_status_t barry_doc_delete(barry_doc_store_t *store, const void *key, uint32_t key_len) {
    if (store == NULL || key == NULL || key_len == 0u || key_len > BARRY_DOC_KEY_MAX) {
        return BARRY_ERR_INVALID;
    }

    const uint8_t *key_bytes = (const uint8_t *)key;
    uint64_t hash = barry_fnv1a64(key_bytes, key_len);
    uint32_t active = atomic_load_explicit(&store->active_shard_count, memory_order_acquire);

    for (uint32_t i = active; i > 0u; i--) {
        barry_doc_shard_t *shard = &store->shards[i - 1u];
        barry_doc_slot_t *slot = doc_shard_find(shard, hash, key_bytes, key_len);
        if (slot == NULL) {
            continue;
        }
        uint32_t expected = (uint32_t)BARRY_SLOT_OCCUPIED;
        if (atomic_compare_exchange_strong_explicit(&slot->state, &expected, (uint32_t)BARRY_SLOT_TOMBSTONE,
                                                      memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_sub_explicit(&shard->occupied_count, 1u, memory_order_acq_rel);
            return BARRY_OK;
        }
    }
    return BARRY_ERR_NOT_FOUND;
}

uint32_t barry_doc_active_shard_count(const barry_doc_store_t *store) {
    return atomic_load_explicit(&store->active_shard_count, memory_order_acquire);
}

void barry_index_init(barry_relation_index_t *idx) {
    for (uint32_t b = 0; b < BARRY_INDEX_BUCKETS; b++) {
        atomic_store_explicit(&idx->buckets[b].count, 0u, memory_order_relaxed);
        memset(idx->buckets[b].entries, 0, sizeof idx->buckets[b].entries);
    }
}

barry_status_t barry_index_link(barry_relation_index_t *idx,
                                 const void *field_val, uint32_t field_val_len,
                                 const void *doc_key, uint32_t doc_key_len) {
    if (idx == NULL || field_val == NULL || field_val_len == 0u ||
        doc_key == NULL || doc_key_len == 0u || doc_key_len > BARRY_DOC_KEY_MAX) {
        return BARRY_ERR_INVALID;
    }

    uint64_t hash = barry_fnv1a64((const uint8_t *)field_val, field_val_len);
    uint32_t bucket_idx = (uint32_t)(hash & (uint64_t)(BARRY_INDEX_BUCKETS - 1u));
    barry_index_bucket_t *bucket = &idx->buckets[bucket_idx];

    /* Each concurrent caller claims a distinct slot via fetch_add -- no
     * CAS retry loop needed, since two callers never claim the same
     * index. A claim past the bounded depth is put back and rejected. */
    uint32_t slot_idx = atomic_fetch_add_explicit(&bucket->count, 1u, memory_order_acq_rel);
    if (slot_idx >= BARRY_INDEX_BUCKET_DEPTH) {
        atomic_fetch_sub_explicit(&bucket->count, 1u, memory_order_acq_rel);
        return BARRY_ERR_FULL;
    }

    barry_index_entry_t *entry = &bucket->entries[slot_idx];
    entry->doc_key_len = doc_key_len;
    memcpy(entry->doc_key, doc_key, doc_key_len);
    if (doc_key_len < BARRY_DOC_KEY_MAX) {
        memset(entry->doc_key + doc_key_len, 0, BARRY_DOC_KEY_MAX - doc_key_len);
    }
    return BARRY_OK;
}

barry_status_t barry_index_lookup(const barry_relation_index_t *idx,
                                   const void *field_val, uint32_t field_val_len,
                                   uint8_t out_doc_keys[][BARRY_DOC_KEY_MAX],
                                   uint32_t *out_doc_key_lens, uint32_t out_max,
                                   uint32_t *out_count) {
    if (idx == NULL || field_val == NULL || field_val_len == 0u || out_count == NULL) {
        return BARRY_ERR_INVALID;
    }

    uint64_t hash = barry_fnv1a64((const uint8_t *)field_val, field_val_len);
    uint32_t bucket_idx = (uint32_t)(hash & (uint64_t)(BARRY_INDEX_BUCKETS - 1u));
    const barry_index_bucket_t *bucket = &idx->buckets[bucket_idx];

    uint32_t count = atomic_load_explicit(&bucket->count, memory_order_acquire);
    if (count > BARRY_INDEX_BUCKET_DEPTH) {
        count = BARRY_INDEX_BUCKET_DEPTH; /* a fetch_add race past the bound is already rejected by the writer; this just bounds a stale read */
    }

    uint32_t n = count < out_max ? count : out_max;
    for (uint32_t i = 0; i < n; i++) {
        if (out_doc_keys != NULL) {
            memcpy(out_doc_keys[i], bucket->entries[i].doc_key, BARRY_DOC_KEY_MAX);
        }
        if (out_doc_key_lens != NULL) {
            out_doc_key_lens[i] = bucket->entries[i].doc_key_len;
        }
    }
    *out_count = n;
    return BARRY_OK;
}

/* ==================================================================== */
/* Part C -- MPMC task queue / message broker                            */
/* ==================================================================== */

void barry_task_queue_init(barry_task_queue_t *q) {
    for (uint64_t i = 0; i < BARRY_TASK_QUEUE_CAPACITY; i++) {
        barry_task_slot_t *slot = &q->slots[i];
        atomic_store_explicit(&slot->sequence, i, memory_order_relaxed);
        slot->producer_id = 0u;
        slot->type = 0u;
        slot->payload_len = 0u;
        memset(slot->payload, 0, sizeof slot->payload);
    }
    atomic_store_explicit(&q->enqueue_pos, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->dequeue_pos, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->posted_total, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->consumed_total, 0u, memory_order_relaxed);
    atomic_store_explicit(&q->rejected_full_total, 0u, memory_order_relaxed);
}

barry_status_t barry_task_enqueue(barry_task_queue_t *q, uint32_t producer_id, uint32_t type,
                                   const void *payload, uint32_t payload_len) {
    if (q == NULL || payload_len > BARRY_TASK_PAYLOAD_MAX || (payload_len > 0u && payload == NULL)) {
        return BARRY_ERR_INVALID;
    }

    uint32_t mask = BARRY_TASK_QUEUE_CAPACITY - 1u;
    uint64_t pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);

    for (;;) {
        barry_task_slot_t *slot = &q->slots[pos & mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->enqueue_pos, &pos, pos + 1u,
                                                        memory_order_relaxed, memory_order_relaxed)) {
                slot->producer_id = producer_id;
                slot->type = type;
                slot->payload_len = payload_len;
                if (payload_len > 0u) {
                    memcpy(slot->payload, payload, payload_len);
                }
                if (payload_len < BARRY_TASK_PAYLOAD_MAX) {
                    memset(slot->payload + payload_len, 0, BARRY_TASK_PAYLOAD_MAX - payload_len);
                }
                atomic_store_explicit(&slot->sequence, pos + 1u, memory_order_release);
                atomic_fetch_add_explicit(&q->posted_total, 1u, memory_order_relaxed);
                return BARRY_OK;
            }
            /* another producer won this slot; pos was refreshed by the
             * failed CAS, retry the loop with the new snapshot */
        } else if (diff < 0) {
            atomic_fetch_add_explicit(&q->rejected_full_total, 1u, memory_order_relaxed);
            return BARRY_ERR_FULL; /* queue full: instant O(1) backpressure rejection, never a wait */
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }
}

barry_status_t barry_task_dequeue(barry_task_queue_t *q, barry_task_t *out) {
    if (q == NULL || out == NULL) {
        return BARRY_ERR_INVALID;
    }

    uint32_t mask = BARRY_TASK_QUEUE_CAPACITY - 1u;
    uint64_t pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);

    for (;;) {
        barry_task_slot_t *slot = &q->slots[pos & mask];
        uint64_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)(pos + 1u);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->dequeue_pos, &pos, pos + 1u,
                                                        memory_order_relaxed, memory_order_relaxed)) {
                out->producer_id = slot->producer_id;
                out->type = slot->type;
                out->payload_len = slot->payload_len;
                memcpy(out->payload, slot->payload, BARRY_TASK_PAYLOAD_MAX);
                atomic_store_explicit(&slot->sequence, pos + mask + 1u, memory_order_release);
                atomic_fetch_add_explicit(&q->consumed_total, 1u, memory_order_relaxed);
                return BARRY_OK;
            }
        } else if (diff < 0) {
            return BARRY_ERR_EMPTY;
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }
}

size_t barry_task_queue_count(const barry_task_queue_t *q) {
    uint64_t enq = atomic_load_explicit(&q->enqueue_pos, memory_order_acquire);
    uint64_t deq = atomic_load_explicit(&q->dequeue_pos, memory_order_acquire);
    return (enq > deq) ? (size_t)(enq - deq) : 0u;
}

barry_status_t barry_worker_process_one(barry_task_queue_t *q, barry_task_handler_fn handler, void *ctx) {
    if (q == NULL || handler == NULL) {
        return BARRY_ERR_INVALID;
    }
    barry_task_t task;
    barry_status_t st = barry_task_dequeue(q, &task);
    if (st != BARRY_OK) {
        return st;
    }
    handler(ctx, &task);
    return BARRY_OK;
}

barry_status_t barry_ingest_wire_msg(barry_task_queue_t *q, uint32_t producer_id, const wire_msg_t *msg) {
    if (q == NULL || msg == NULL) {
        return BARRY_ERR_INVALID;
    }
    _Static_assert(sizeof(wire_msg_t) <= BARRY_TASK_PAYLOAD_MAX,
                   "wire_msg_t must fit in one task payload for zero-copy-sized enqueue");
    return barry_task_enqueue(q, producer_id, BARRY_TASK_TYPE_WIRE_MSG, msg, (uint32_t)sizeof(*msg));
}

/* ==================================================================== */
/* Part D -- Time-series metrics                                         */
/* ==================================================================== */

void barry_ts_init(barry_ts_store_t *store) {
    for (uint32_t m = 0; m < BARRY_TS_MAX_METRICS; m++) {
        atomic_store_explicit(&store->series[m].write_pos, 0u, memory_order_relaxed);
        for (uint32_t i = 0; i < BARRY_TS_CAPACITY; i++) {
            barry_ts_point_t *point = &store->series[m].points[i];
            atomic_store_explicit(&point->ready, 0u, memory_order_relaxed);
            point->tick = 0u;
            point->value = 0.0;
        }
        store->registered[m] = false;
    }
}

barry_status_t barry_ts_register_metric(barry_ts_store_t *store, uint32_t metric_id) {
    if (store == NULL || metric_id >= BARRY_TS_MAX_METRICS) {
        return BARRY_ERR_OUT_OF_RANGE;
    }
    if (store->registered[metric_id]) {
        return BARRY_ERR_ALREADY_REGISTERED;
    }
    store->registered[metric_id] = true;
    return BARRY_OK;
}

barry_status_t barry_ts_record(barry_ts_store_t *store, uint32_t metric_id, uint64_t tick, double value) {
    if (store == NULL || metric_id >= BARRY_TS_MAX_METRICS || !store->registered[metric_id]) {
        return BARRY_ERR_OUT_OF_RANGE;
    }

    barry_ts_series_t *series = &store->series[metric_id];
    uint64_t pos = atomic_fetch_add_explicit(&series->write_pos, 1u, memory_order_relaxed);
    barry_ts_point_t *point = &series->points[pos & (BARRY_TS_CAPACITY - 1u)];

    atomic_store_explicit(&point->ready, 0u, memory_order_relaxed); /* claim: not readable mid-write */
    point->tick = tick;
    point->value = value;
    atomic_store_explicit(&point->ready, 1u, memory_order_release);
    return BARRY_OK;
}

barry_status_t barry_ts_query_latest(const barry_ts_store_t *store, uint32_t metric_id,
                                      barry_ts_point_t *out_points, uint32_t out_max,
                                      uint32_t *out_count) {
    if (store == NULL || metric_id >= BARRY_TS_MAX_METRICS || !store->registered[metric_id] ||
        out_count == NULL) {
        return BARRY_ERR_OUT_OF_RANGE;
    }

    const barry_ts_series_t *series = &store->series[metric_id];
    uint64_t written = atomic_load_explicit(&series->write_pos, memory_order_acquire);
    uint32_t window = written < BARRY_TS_CAPACITY ? (uint32_t)written : BARRY_TS_CAPACITY;
    uint32_t want = window < out_max ? window : out_max;

    uint32_t produced = 0;
    for (uint32_t back = want; back > 0u && produced < out_max; back--) {
        uint64_t pos = written - back;
        const barry_ts_point_t *point = &series->points[pos & (BARRY_TS_CAPACITY - 1u)];
        if (atomic_load_explicit(&point->ready, memory_order_acquire) == 0u) {
            continue; /* overwritten or not yet published by the time we got here -- skip, not an error */
        }
        if (out_points != NULL) {
            out_points[produced].tick  = point->tick;
            out_points[produced].value = point->value;
            atomic_store_explicit(&out_points[produced].ready, 1u, memory_order_relaxed); /* informational on the caller's own copy, same convention as rb_try_pop() */
        }
        produced++;
    }
    *out_count = produced;
    return BARRY_OK;
}
