#include "net_gateway.h"

#include <string.h>

/* ================================================================== */
/* Little-endian field helpers (fixed wire format, no aliasing UB)     */
/* ================================================================== */

static uint16_t ngw_load_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ngw_load_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ngw_load_u64le(const uint8_t *p) {
    uint64_t lo = ngw_load_u32le(p);
    uint64_t hi = ngw_load_u32le(p + 4);
    return lo | (hi << 32);
}

static void ngw_store_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ngw_store_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void ngw_store_u64le(uint8_t *p, uint64_t v) {
    ngw_store_u32le(p, (uint32_t)(v & 0xFFFFFFFFu));
    ngw_store_u32le(p + 4, (uint32_t)(v >> 32));
}

/* ================================================================== */
/* FNV-1a/64 frame integrity checksum (unkeyed: tamper/corruption       */
/* detection, not a secret-dependent MAC -- see net_gateway.h)          */
/* ================================================================== */

#define NGW_FNV64_OFFSET_BASIS 0xcbf29ce484222325ULL
#define NGW_FNV64_PRIME        0x100000001b3ULL

static uint64_t ngw_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t state = NGW_FNV64_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        state ^= (uint64_t)data[i];
        state *= NGW_FNV64_PRIME;
    }
    return state;
}

/* ================================================================== */
/* Actor mailbox (MPSC) -- corrected design from the start:            */
/* fullness gated on tail-vs-head (release/acquire), not on a per-slot */
/* flag set after the write (see net_gateway.h for why that would      */
/* reopen a reservation-before-publish race on wraparound).             */
/* ================================================================== */

void ngw_mailbox_init(ngw_mailbox_t *mb) {
    atomic_store_explicit(&mb->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&mb->head, 0, memory_order_relaxed);

    for (uint32_t i = 0; i < NGW_MAILBOX_CAPACITY; i++) {
        ngw_message_t *slot = &mb->slots[i];
        atomic_store_explicit(&slot->ready, 0u, memory_order_relaxed);
        slot->sender_id = 0u;
        slot->type = 0u;
        slot->_reserved = 0u;
        memset(slot->payload, 0, sizeof slot->payload);
    }

    atomic_store_explicit(&mb->posted_total, 0, memory_order_relaxed);
    atomic_store_explicit(&mb->consumed_total, 0, memory_order_relaxed);
    atomic_store_explicit(&mb->rejected_full_total, 0, memory_order_relaxed);
}

ngw_status_t ngw_mailbox_reserve(ngw_mailbox_t *mb, ngw_message_t **out_slot) {
    if (mb == NULL || out_slot == NULL) {
        return NGW_ERR_INVALID;
    }

    const uint64_t mask = (uint64_t)(NGW_MAILBOX_CAPACITY - 1u);
    uint64_t old_tail = atomic_load_explicit(&mb->tail, memory_order_relaxed);

    for (;;) {
        /* Acquire: synchronizes with the consumer's release-store to
         * `head` in ngw_mailbox_consume(). "tail - head < CAPACITY" is
         * true iff the slot this reservation would target has already
         * been fully read by the consumer -- see net_gateway.h. */
        uint64_t h = atomic_load_explicit(&mb->head, memory_order_acquire);
        if (old_tail - h >= (uint64_t)NGW_MAILBOX_CAPACITY) {
            atomic_fetch_add_explicit(&mb->rejected_full_total, 1, memory_order_relaxed);
            return NGW_ERR_MAILBOX_FULL; /* O(1) instant rejection: no spin, no wait */
        }

        if (atomic_compare_exchange_weak_explicit(&mb->tail, &old_tail, old_tail + 1u,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            uint32_t slot_idx = (uint32_t)(old_tail & mask);
            *out_slot = &mb->slots[slot_idx];
            return NGW_OK;
        }
        /* CAS lost the race: old_tail refreshed by the failed CAS; retry
         * with a fresh fullness check against the (different) slot. */
    }
}

void ngw_mailbox_publish(ngw_mailbox_t *mb, ngw_message_t *slot, uint32_t sender_id, uint32_t type) {
    slot->sender_id = sender_id;
    slot->type = type;

    /* Release: publishes sender_id/type/payload (whatever the caller
     * already wrote into slot->payload, directly or via memcpy) to the
     * consumer's matching acquire load of `ready`. */
    atomic_store_explicit(&slot->ready, 1u, memory_order_release);
    atomic_fetch_add_explicit(&mb->posted_total, 1, memory_order_relaxed);
}

ngw_status_t ngw_mailbox_post(ngw_mailbox_t *mb, uint32_t sender_id, uint32_t type,
                               const void *payload, size_t payload_len) {
    if (mb == NULL || (payload == NULL && payload_len != 0u) ||
        payload_len > NGW_MESSAGE_PAYLOAD_SIZE) {
        return NGW_ERR_INVALID;
    }

    ngw_message_t *slot;
    ngw_status_t rst = ngw_mailbox_reserve(mb, &slot);
    if (rst != NGW_OK) {
        return rst;
    }

    if (payload_len != 0u) {
        memcpy(slot->payload, payload, payload_len);
    }
    if (payload_len < NGW_MESSAGE_PAYLOAD_SIZE) {
        memset(slot->payload + payload_len, 0, NGW_MESSAGE_PAYLOAD_SIZE - payload_len);
    }

    ngw_mailbox_publish(mb, slot, sender_id, type);
    return NGW_OK;
}

ngw_status_t ngw_mailbox_consume(ngw_mailbox_t *mb, ngw_message_t *out) {
    if (mb == NULL || out == NULL) {
        return NGW_ERR_INVALID;
    }

    const uint64_t mask = (uint64_t)(NGW_MAILBOX_CAPACITY - 1u);
    uint64_t h = atomic_load_explicit(&mb->head, memory_order_relaxed); /* single consumer: relaxed self-read */
    uint32_t slot_idx = (uint32_t)(h & mask);
    ngw_message_t *msg = &mb->slots[slot_idx];

    /* Acquire: synchronizes with the producer's release store above, so
     * every field written before that release is guaranteed visible
     * below -- never a torn or stale read. */
    if (atomic_load_explicit(&msg->ready, memory_order_acquire) == 0u) {
        return NGW_ERR_EMPTY;
    }

    out->sender_id = msg->sender_id;
    out->type = msg->type;
    out->_reserved = 0u;
    memcpy(out->payload, msg->payload, sizeof msg->payload);
    atomic_store_explicit(&out->ready, 1u, memory_order_relaxed); /* informational on the caller's own copy */

    atomic_store_explicit(&msg->ready, 0u, memory_order_relaxed);

    /* Release: the actual O(1) free of this slot for reuse. Synchronizes
     * with a producer's acquire load of `head`: everything above (the
     * full read of msg->sender_id/type/payload) is guaranteed complete
     * before any producer can conclude, from seeing this new head value,
     * that it may overwrite this slot. */
    atomic_store_explicit(&mb->head, h + 1u, memory_order_release);
    atomic_fetch_add_explicit(&mb->consumed_total, 1, memory_order_relaxed);
    return NGW_OK;
}

size_t ngw_mailbox_count(const ngw_mailbox_t *mb) {
    uint64_t t = atomic_load_explicit((_Atomic uint64_t *)&mb->tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit((_Atomic uint64_t *)&mb->head, memory_order_relaxed);
    return (size_t)(t - h);
}

bool ngw_mailbox_is_full(const ngw_mailbox_t *mb) {
    return ngw_mailbox_count(mb) >= (size_t)NGW_MAILBOX_CAPACITY;
}

bool ngw_mailbox_is_empty(const ngw_mailbox_t *mb) {
    return ngw_mailbox_count(mb) == 0u;
}

/* ================================================================== */
/* Slab (lock-free Treiber-stack free list, single-CAS ownership        */
/* transfer -- no reservation/publish window, unlike the mailbox above) */
/* ================================================================== */

#define NGW_SLAB_NIL 0xFFFFFFFFu

static uint32_t ngw_slab_index_of(const ngw_slab_t *slab, const uint8_t *ptr) {
    return (uint32_t)((size_t)(ptr - &slab->storage[0][0]) / NGW_MAX_PAYLOAD);
}

static void ngw_slab_init(ngw_slab_t *slab) {
    for (uint32_t i = 0; i < NGW_SLAB_CAPACITY; i++) {
        uint32_t next = (i + 1u < NGW_SLAB_CAPACITY) ? (i + 1u) : NGW_SLAB_NIL;
        memcpy(&slab->storage[i][0], &next, sizeof next);
    }
    uint64_t initial_head = (NGW_SLAB_CAPACITY != 0u) ? 0u /* generation 0, index 0 */ : (uint64_t)NGW_SLAB_NIL;
    atomic_store_explicit(&slab->free_head, initial_head, memory_order_relaxed);
}

static uint8_t *ngw_slab_alloc(ngw_slab_t *slab) {
    uint64_t old_head = atomic_load_explicit(&slab->free_head, memory_order_acquire);
    for (;;) {
        uint32_t idx = (uint32_t)(old_head & 0xFFFFFFFFu);
        if (idx == NGW_SLAB_NIL) {
            return NULL; /* exhausted: deterministic, no growth */
        }

        uint8_t *block = &slab->storage[idx][0];
        uint32_t next_idx;
        memcpy(&next_idx, block, sizeof next_idx);

        uint32_t generation = (uint32_t)(old_head >> 32);
        uint64_t new_head = ((uint64_t)(generation + 1u) << 32) | next_idx;

        if (atomic_compare_exchange_weak_explicit(&slab->free_head, &old_head, new_head,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            return block;
        }
    }
}

static void ngw_slab_free(ngw_slab_t *slab, uint8_t *block) {
    uint32_t idx = ngw_slab_index_of(slab, block);

    /* Security hardening (CWE-457, residual data in a reused buffer):
     * ngw_gateway_ingest() only ever memcpy()s `payload_len` bytes into
     * a freshly allocated block (see there) -- bytes [payload_len,
     * NGW_MAX_PAYLOAD) from whatever this block's PREVIOUS occupant
     * (a different tenant, in general) left behind were never touched.
     * As long as every consumer strictly respects the tracked length in
     * ngw_descriptor_t.len, those stale bytes are never actually read --
     * but that makes correctness depend on every current AND FUTURE
     * consumer getting that right, which is exactly the kind of implicit
     * cross-tenant leak this codebase's own crypto_constant_time.h module
     * exists to avoid leaving to chance elsewhere. Zeroing the entire
     * block here, once, at the one place a block changes ownership,
     * removes the stale bytes deterministically regardless of what any
     * consumer does or will ever do -- the caller-visible contract
     * (O(1), single CAS retry loop) is unchanged; this is a fixed,
     * bounded NGW_MAX_PAYLOAD-byte memset, not a new loop over anything
     * unbounded.
     *
     * Ordering matters: the block's first 4 bytes double as the
     * intrusive free-list "next index" while it sits on the free list
     * (see ngw_slab_alloc()) -- the memset must happen BEFORE that link
     * is written, not after, or it would erase the very free-list
     * pointer this function is about to publish. The CAS loop below may
     * retry on contention; each retry only rewrites those same first 4
     * bytes with the (possibly updated) next_idx, so bytes [4,
     * NGW_MAX_PAYLOAD) stay zero across every retry, not just the first
     * attempt. */
    memset(block, 0, NGW_MAX_PAYLOAD);

    uint64_t old_head = atomic_load_explicit(&slab->free_head, memory_order_acquire);
    for (;;) {
        uint32_t next_idx = (uint32_t)(old_head & 0xFFFFFFFFu);
        memcpy(block, &next_idx, sizeof next_idx);

        uint32_t generation = (uint32_t)(old_head >> 32);
        uint64_t new_head = ((uint64_t)(generation + 1u) << 32) | idx;

        if (atomic_compare_exchange_weak_explicit(&slab->free_head, &old_head, new_head,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

/* ================================================================== */
/* Gateway                                                               */
/* ================================================================== */

void ngw_gateway_init(ngw_gateway_t *gw) {
    ngw_slab_init(&gw->slab);

    for (uint32_t i = 0; i < NGW_MAX_TENANTS; i++) {
        memset(&gw->tenants[i], 0, sizeof gw->tenants[i]);
    }
    for (uint32_t i = 0; i < NGW_MAX_ACTORS; i++) {
        gw->actors[i] = NULL;
    }

    atomic_store_explicit(&gw->frames_ingested, 0, memory_order_relaxed);
    atomic_store_explicit(&gw->frames_accepted, 0, memory_order_relaxed);
    atomic_store_explicit(&gw->frames_rejected_bad_frame, 0, memory_order_relaxed);
    atomic_store_explicit(&gw->frames_rejected_auth, 0, memory_order_relaxed);
    atomic_store_explicit(&gw->frames_rejected_capacity, 0, memory_order_relaxed);
}

ngw_status_t ngw_register_actor(ngw_gateway_t *gw, uint8_t actor_id, ngw_mailbox_t *mailbox) {
    if (gw == NULL || mailbox == NULL || actor_id >= NGW_MAX_ACTORS) {
        return NGW_ERR_INVALID;
    }
    gw->actors[actor_id] = mailbox;
    return NGW_OK;
}

ngw_status_t ngw_register_tenant(ngw_gateway_t *gw, uint16_t tenant_id,
                                  const uint8_t api_key[NGW_API_KEY_LEN], uint8_t actor_id) {
    if (gw == NULL || api_key == NULL || tenant_id >= NGW_MAX_TENANTS || actor_id >= NGW_MAX_ACTORS) {
        return NGW_ERR_INVALID;
    }
    ngw_tenant_t *t = &gw->tenants[tenant_id];
    memcpy(t->api_key, api_key, NGW_API_KEY_LEN);
    t->actor_id = actor_id;
    t->registered = 1u;
    return NGW_OK;
}

ngw_status_t ngw_ingest_frame(ngw_gateway_t *gw, const uint8_t *frame, size_t frame_len) {
    if (gw == NULL || frame == NULL) {
        return NGW_ERR_INVALID;
    }
    atomic_fetch_add_explicit(&gw->frames_ingested, 1, memory_order_relaxed);

    if (frame_len < (size_t)(NGW_HEADER_SIZE + NGW_HASH_SIZE)) {
        atomic_fetch_add_explicit(&gw->frames_rejected_bad_frame, 1, memory_order_relaxed);
        return NGW_ERR_TRUNCATED;
    }

    uint32_t magic = ngw_load_u32le(frame + 0);
    uint8_t  protocol = frame[4];
    uint8_t  version = frame[5];
    uint16_t tenant_id = ngw_load_u16le(frame + 6);
    const uint8_t *api_key = frame + 8;
    uint16_t payload_len = ngw_load_u16le(frame + 40);

    if (magic != NGW_MAGIC || version != NGW_VERSION) {
        atomic_fetch_add_explicit(&gw->frames_rejected_bad_frame, 1, memory_order_relaxed);
        return NGW_ERR_BAD_MAGIC;
    }
    if (protocol != (uint8_t)NGW_PROTO_HTTP && protocol != (uint8_t)NGW_PROTO_WEBHOOK &&
        protocol != (uint8_t)NGW_PROTO_UDP) {
        atomic_fetch_add_explicit(&gw->frames_rejected_bad_frame, 1, memory_order_relaxed);
        return NGW_ERR_BAD_MAGIC;
    }
    if (payload_len > NGW_MAX_PAYLOAD) {
        atomic_fetch_add_explicit(&gw->frames_rejected_bad_frame, 1, memory_order_relaxed);
        return NGW_ERR_PAYLOAD_TOO_LARGE;
    }
    if (frame_len != (size_t)NGW_HEADER_SIZE + (size_t)payload_len + (size_t)NGW_HASH_SIZE) {
        atomic_fetch_add_explicit(&gw->frames_rejected_bad_frame, 1, memory_order_relaxed);
        return NGW_ERR_TRUNCATED;
    }

    const uint8_t *payload = frame + NGW_HEADER_SIZE;
    uint64_t hash_received = ngw_load_u64le(frame + NGW_HEADER_SIZE + payload_len);
    uint64_t hash_computed = ngw_fnv1a64(frame, (size_t)NGW_HEADER_SIZE + payload_len);
    if (!ct_equal(&hash_received, &hash_computed, sizeof hash_received)) {
        atomic_fetch_add_explicit(&gw->frames_rejected_bad_frame, 1, memory_order_relaxed);
        return NGW_ERR_INTEGRITY;
    }

    if (tenant_id >= NGW_MAX_TENANTS) {
        atomic_fetch_add_explicit(&gw->frames_rejected_auth, 1, memory_order_relaxed);
        return NGW_ERR_UNKNOWN_TENANT;
    }
    ngw_tenant_t *tenant = &gw->tenants[tenant_id];
    if (!tenant->registered) {
        atomic_fetch_add_explicit(&gw->frames_rejected_auth, 1, memory_order_relaxed);
        return NGW_ERR_UNKNOWN_TENANT;
    }
    if (!ct_verify_token(api_key, NGW_API_KEY_LEN, tenant->api_key, NGW_API_KEY_LEN)) {
        atomic_fetch_add_explicit(&gw->frames_rejected_auth, 1, memory_order_relaxed);
        return NGW_ERR_AUTH_FAILED;
    }

    uint8_t actor_id = tenant->actor_id;
    if (actor_id >= NGW_MAX_ACTORS || gw->actors[actor_id] == NULL) {
        atomic_fetch_add_explicit(&gw->frames_rejected_capacity, 1, memory_order_relaxed);
        return NGW_ERR_ACTOR_NOT_REGISTERED;
    }

    uint8_t *buf = ngw_slab_alloc(&gw->slab);
    if (buf == NULL) {
        atomic_fetch_add_explicit(&gw->frames_rejected_capacity, 1, memory_order_relaxed);
        return NGW_ERR_SLAB_EXHAUSTED;
    }
    if (payload_len != 0u) {
        memcpy(buf, payload, payload_len); /* the ONE copy: caller's frame -> gateway-owned storage */
    }

    ngw_descriptor_t desc;
    desc.ptr = buf;
    desc.len = payload_len;
    desc.tenant_id = tenant_id;
    desc.protocol = protocol;
    memset(desc._pad, 0, sizeof desc._pad);

    ngw_status_t post_status = ngw_mailbox_post(gw->actors[actor_id], tenant_id, (uint32_t)protocol,
                                                 &desc, sizeof desc);
    if (post_status != NGW_OK) {
        ngw_slab_free(&gw->slab, buf); /* undo the allocation: never leak a slab block on rejection */
        atomic_fetch_add_explicit(&gw->frames_rejected_capacity, 1, memory_order_relaxed);
        return NGW_ERR_MAILBOX_FULL;
    }

    atomic_fetch_add_explicit(&gw->frames_accepted, 1, memory_order_relaxed);
    return NGW_OK;
}

ngw_status_t ngw_decode_message(const ngw_message_t *msg, ngw_descriptor_t *out) {
    if (msg == NULL || out == NULL) {
        return NGW_ERR_INVALID;
    }
    memcpy(out, msg->payload, sizeof *out);
    return NGW_OK;
}

void ngw_release_payload(ngw_gateway_t *gw, uint8_t *ptr) {
    if (gw == NULL || ptr == NULL) {
        return;
    }
    ngw_slab_free(&gw->slab, ptr);
}

ngw_status_t ngw_build_frame(uint8_t *out, size_t out_cap, size_t *out_len,
                              ngw_protocol_t protocol, uint16_t tenant_id,
                              const uint8_t api_key[NGW_API_KEY_LEN],
                              const uint8_t *payload, uint16_t payload_len) {
    if (out == NULL || out_len == NULL || api_key == NULL || (payload == NULL && payload_len != 0u)) {
        return NGW_ERR_INVALID;
    }
    if (payload_len > NGW_MAX_PAYLOAD) {
        return NGW_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t total = (size_t)NGW_HEADER_SIZE + (size_t)payload_len + (size_t)NGW_HASH_SIZE;
    if (out_cap < total) {
        return NGW_ERR_INVALID;
    }

    ngw_store_u32le(out + 0, NGW_MAGIC);
    out[4] = (uint8_t)protocol;
    out[5] = (uint8_t)NGW_VERSION;
    ngw_store_u16le(out + 6, tenant_id);
    memcpy(out + 8, api_key, NGW_API_KEY_LEN);
    ngw_store_u16le(out + 40, payload_len);
    if (payload_len != 0u) {
        memcpy(out + NGW_HEADER_SIZE, payload, payload_len);
    }

    uint64_t hash = ngw_fnv1a64(out, (size_t)NGW_HEADER_SIZE + payload_len);
    ngw_store_u64le(out + NGW_HEADER_SIZE + payload_len, hash);

    *out_len = total;
    return NGW_OK;
}

void ngw_get_stats(const ngw_gateway_t *gw, ngw_stats_t *out) {
    if (gw == NULL || out == NULL) {
        return;
    }
    out->frames_ingested = atomic_load_explicit((_Atomic uint64_t *)&gw->frames_ingested, memory_order_relaxed);
    out->frames_accepted = atomic_load_explicit((_Atomic uint64_t *)&gw->frames_accepted, memory_order_relaxed);
    out->frames_rejected_bad_frame =
        atomic_load_explicit((_Atomic uint64_t *)&gw->frames_rejected_bad_frame, memory_order_relaxed);
    out->frames_rejected_auth =
        atomic_load_explicit((_Atomic uint64_t *)&gw->frames_rejected_auth, memory_order_relaxed);
    out->frames_rejected_capacity =
        atomic_load_explicit((_Atomic uint64_t *)&gw->frames_rejected_capacity, memory_order_relaxed);
}
