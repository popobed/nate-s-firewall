#include "wire_proto.h"

#include <string.h>

void wire_ring_init(wire_ring_t *ring) {
    atomic_store_explicit(&ring->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->head, 0, memory_order_relaxed);

    for (uint32_t i = 0; i < WIRE_RING_CAPACITY; i++) {
        atomic_store_explicit(&ring->ready[i], 0u, memory_order_relaxed);
        memset(&ring->slots[i], 0, sizeof ring->slots[i]);
    }

    atomic_store_explicit(&ring->posted_total, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->consumed_total, 0, memory_order_relaxed);
    atomic_store_explicit(&ring->rejected_full_total, 0, memory_order_relaxed);
}

wire_status_t wire_ring_reserve(wire_ring_t *ring, wire_msg_t **out_slot) {
    if (ring == NULL || out_slot == NULL) {
        return WIRE_ERR_INVALID;
    }

    const uint64_t mask = (uint64_t)(WIRE_RING_CAPACITY - 1u);
    uint64_t old_tail = atomic_load_explicit(&ring->tail, memory_order_relaxed);

    for (;;) {
        /* Acquire: synchronizes with the consumer's release-store to
         * `head` in wire_ring_try_pop(). "tail - head < CAPACITY" is true
         * iff the slot this reservation would target has already been
         * fully read by the consumer -- see wire_proto.h. */
        uint64_t h = atomic_load_explicit(&ring->head, memory_order_acquire);
        if (old_tail - h >= (uint64_t)WIRE_RING_CAPACITY) {
            atomic_fetch_add_explicit(&ring->rejected_full_total, 1, memory_order_relaxed);
            return WIRE_ERR_FULL; /* O(1) instant rejection: no spin, no wait */
        }

        if (atomic_compare_exchange_weak_explicit(&ring->tail, &old_tail, old_tail + 1u,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            uint32_t slot_idx = (uint32_t)(old_tail & mask);

            /* Security hardening (CWE-457, residual data across slot
             * reuse): wire_build_msg() always zeroes an unused payload
             * tail before a normal producer call, but the zero-copy path
             * this ring exists for (net_io.c's recv() writing DIRECTLY
             * into *out_slot, per this header's file comment) makes no
             * such guarantee -- a short/partial recv() would otherwise
             * leave whichever PREVIOUS tenant's message last occupied
             * this slot sitting in the unwritten tail, to be published
             * and read as if it belonged to the new message. Zeroing the
             * whole slot here, once, at the one point a slot changes
             * ownership, removes that dependency on every current and
             * future producer getting a full write right, instead of
             * assuming it. Fixed 64-byte memset -- the O(1) reservation
             * contract this function documents is unchanged. */
            memset(&ring->slots[slot_idx], 0, sizeof ring->slots[slot_idx]);

            *out_slot = &ring->slots[slot_idx];
            return WIRE_OK;
        }
        /* CAS lost the race: old_tail refreshed to the current tail by
         * the failed CAS; retry with a fresh fullness check against the
         * (different) slot it now points to. */
    }
}

void wire_ring_publish(wire_ring_t *ring, wire_msg_t *slot, uint8_t type) {
    slot->type = type;

    /* Release: publishes every byte the caller wrote into *slot (directly,
     * e.g. via recv()) to the consumer's matching acquire load of the
     * ready flag at this same slot's index. `ready` lives in its own
     * array, not inside the packed struct -- see wire_proto.h point 3. */
    uint32_t slot_idx = (uint32_t)(slot - ring->slots);
    atomic_store_explicit(&ring->ready[slot_idx], 1u, memory_order_release);
    atomic_fetch_add_explicit(&ring->posted_total, 1, memory_order_relaxed);
}

wire_status_t wire_ring_try_pop(wire_ring_t *ring, wire_msg_t *out) {
    if (ring == NULL || out == NULL) {
        return WIRE_ERR_INVALID;
    }

    const uint64_t mask = (uint64_t)(WIRE_RING_CAPACITY - 1u);
    uint64_t h = atomic_load_explicit(&ring->head, memory_order_relaxed); /* single consumer: relaxed self-read */
    uint32_t slot_idx = (uint32_t)(h & mask);

    /* Acquire: synchronizes with the producer's release store in
     * wire_ring_publish(), so every field written before that release is
     * guaranteed visible below -- never a torn or stale read. */
    if (atomic_load_explicit(&ring->ready[slot_idx], memory_order_acquire) == 0u) {
        return WIRE_ERR_EMPTY;
    }

    memcpy(out, &ring->slots[slot_idx], sizeof *out);
    atomic_store_explicit(&ring->ready[slot_idx], 0u, memory_order_relaxed);

    /* Release: the actual O(1) free of this slot for reuse. Synchronizes
     * with a producer's acquire load of `head` in wire_ring_reserve(): the
     * full read above is guaranteed complete before any producer can
     * conclude, from seeing this new head value, that it may overwrite
     * this slot. */
    atomic_store_explicit(&ring->head, h + 1u, memory_order_release);
    atomic_fetch_add_explicit(&ring->consumed_total, 1, memory_order_relaxed);
    return WIRE_OK;
}

size_t wire_ring_count(const wire_ring_t *ring) {
    uint64_t t = atomic_load_explicit((_Atomic uint64_t *)&ring->tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit((_Atomic uint64_t *)&ring->head, memory_order_relaxed);
    return (size_t)(t - h);
}

bool wire_ring_is_full(const wire_ring_t *ring) {
    return wire_ring_count(ring) >= (size_t)WIRE_RING_CAPACITY;
}

bool wire_ring_is_empty(const wire_ring_t *ring) {
    return wire_ring_count(ring) == 0u;
}

wire_status_t wire_build_msg(wire_msg_t *out, uint16_t tenant_id, uint8_t type,
                              const uint8_t *payload, uint16_t payload_len) {
    if (out == NULL || (payload == NULL && payload_len != 0u) || payload_len > WIRE_MSG_PAYLOAD_SIZE) {
        return WIRE_ERR_INVALID;
    }
    out->magic = WIRE_MAGIC;
    out->tenant_id = tenant_id;
    out->type = type;
    out->version = (uint8_t)WIRE_VERSION;
    out->payload_len = payload_len;
    if (payload_len != 0u) {
        memcpy(out->payload, payload, payload_len);
    }
    if (payload_len < WIRE_MSG_PAYLOAD_SIZE) {
        memset(out->payload + payload_len, 0, WIRE_MSG_PAYLOAD_SIZE - payload_len);
    }
    return WIRE_OK;
}
