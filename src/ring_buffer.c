#include "ring_buffer.h"

#include <string.h>

void rb_init(ring_buffer_t *rb) {
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);

    for (uint32_t i = 0; i < RING_BUFFER_CAPACITY; i++) {
        ring_msg_t *slot = &rb->slots[i];
        atomic_store_explicit(&slot->ready, 0u, memory_order_relaxed);
        slot->sender_id = 0u;
        slot->type = 0u;
        slot->_reserved = 0u;
        memset(slot->payload, 0, sizeof slot->payload);
    }

    atomic_store_explicit(&rb->posted_total, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->consumed_total, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->rejected_full_total, 0, memory_order_relaxed);
}

rb_status_t rb_reserve(ring_buffer_t *rb, ring_msg_t **out_slot) {
    if (rb == NULL || out_slot == NULL) {
        return RB_ERR_INVALID;
    }

    const uint64_t mask = (uint64_t)(RING_BUFFER_CAPACITY - 1u);
    uint64_t old_tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    for (;;) {
        /* Acquire: synchronizes with the consumer's release-store to
         * `head` in rb_try_pop(). "tail - head < CAPACITY" is true iff
         * the slot this reservation would target has already been fully
         * read by the consumer -- see ring_buffer.h. */
        uint64_t h = atomic_load_explicit(&rb->head, memory_order_acquire);
        if (old_tail - h >= (uint64_t)RING_BUFFER_CAPACITY) {
            atomic_fetch_add_explicit(&rb->rejected_full_total, 1, memory_order_relaxed);
            return RB_ERR_FULL; /* O(1) instant rejection: no spin, no wait */
        }

        if (atomic_compare_exchange_weak_explicit(&rb->tail, &old_tail, old_tail + 1u,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            uint32_t slot_idx = (uint32_t)(old_tail & mask);
            *out_slot = &rb->slots[slot_idx];
            return RB_OK;
        }
        /* CAS lost the race: old_tail refreshed to the current tail by
         * the failed CAS; retry with a fresh fullness check against the
         * (different) slot it now points to. */
    }
}

void rb_publish(ring_buffer_t *rb, ring_msg_t *slot, uint32_t sender_id, uint32_t type) {
    slot->sender_id = sender_id;
    slot->type = type;

    /* Release: publishes sender_id/type/payload (whatever the caller
     * already wrote into slot->payload) to the consumer's matching
     * acquire load of `ready`. */
    atomic_store_explicit(&slot->ready, 1u, memory_order_release);
    atomic_fetch_add_explicit(&rb->posted_total, 1, memory_order_relaxed);
}

rb_status_t rb_try_push(ring_buffer_t *rb, uint32_t sender_id, uint32_t type,
                         const void *data, size_t len) {
    if (rb == NULL || (data == NULL && len != 0u) || len > RING_MSG_PAYLOAD_SIZE) {
        return RB_ERR_INVALID;
    }

    ring_msg_t *slot;
    rb_status_t rst = rb_reserve(rb, &slot);
    if (rst != RB_OK) {
        return rst;
    }

    if (len != 0u) {
        memcpy(slot->payload, data, len);
    }
    if (len < RING_MSG_PAYLOAD_SIZE) {
        memset(slot->payload + len, 0, RING_MSG_PAYLOAD_SIZE - len);
    }

    rb_publish(rb, slot, sender_id, type);
    return RB_OK;
}

rb_status_t rb_try_pop(ring_buffer_t *rb, ring_msg_t *out) {
    if (rb == NULL || out == NULL) {
        return RB_ERR_INVALID;
    }

    const uint64_t mask = (uint64_t)(RING_BUFFER_CAPACITY - 1u);
    uint64_t h = atomic_load_explicit(&rb->head, memory_order_relaxed); /* single consumer: relaxed self-read */
    uint32_t slot_idx = (uint32_t)(h & mask);
    ring_msg_t *slot = &rb->slots[slot_idx];

    /* Acquire: synchronizes with the producer's release store in
     * rb_publish(), so every field written before that release is
     * guaranteed visible below -- never a torn or stale read. */
    if (atomic_load_explicit(&slot->ready, memory_order_acquire) == 0u) {
        return RB_ERR_EMPTY;
    }

    out->sender_id = slot->sender_id;
    out->type = slot->type;
    out->_reserved = 0u;
    memcpy(out->payload, slot->payload, sizeof slot->payload);
    atomic_store_explicit(&out->ready, 1u, memory_order_relaxed); /* informational on the caller's own copy */

    atomic_store_explicit(&slot->ready, 0u, memory_order_relaxed);

    /* Release: the actual O(1) free of this slot for reuse. Synchronizes
     * with a producer's acquire load of `head` in rb_reserve(): the full
     * read above is guaranteed complete before any producer can conclude,
     * from seeing this new head value, that it may overwrite this slot. */
    atomic_store_explicit(&rb->head, h + 1u, memory_order_release);
    atomic_fetch_add_explicit(&rb->consumed_total, 1, memory_order_relaxed);
    return RB_OK;
}

size_t rb_count(const ring_buffer_t *rb) {
    uint64_t t = atomic_load_explicit((_Atomic uint64_t *)&rb->tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit((_Atomic uint64_t *)&rb->head, memory_order_relaxed);
    return (size_t)(t - h);
}

bool rb_is_full(const ring_buffer_t *rb) {
    return rb_count(rb) >= (size_t)RING_BUFFER_CAPACITY;
}

bool rb_is_empty(const ring_buffer_t *rb) {
    return rb_count(rb) == 0u;
}
