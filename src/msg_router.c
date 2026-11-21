#include "msg_router.h"

#include <stdatomic.h>
#include <string.h>

/* O(1) tenant -> actor routing table, direct-indexed by the wire
 * tenant_id field. 0 = unmapped; a registered tenant stores actor_id + 1
 * so actor 0 is never confused with "unmapped". Process-wide, matching
 * this project's established idiom (net_io.c's g_tenant_actor_map). */
static uint8_t g_router_tenant_map[ROUTER_MAX_TENANTS];

typedef struct {
    _Atomic uint64_t frames_routed;
    _Atomic uint64_t rejected_bad_frame;
    _Atomic uint64_t rejected_unknown_tenant;
    _Atomic uint64_t rejected_backpressure;
} router_stats_internal_t;

static router_stats_internal_t g_stats;

/* ================================================================== */
/* Little-endian field helpers (fixed wire format, no aliasing UB)     */
/* ================================================================== */

static uint16_t router_load_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t router_load_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void router_store_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void router_store_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ================================================================== */
/* Router                                                                */
/* ================================================================== */

void router_init(void) {
    for (uint32_t i = 0; i < ROUTER_MAX_TENANTS; i++) {
        g_router_tenant_map[i] = 0u;
    }
    atomic_store_explicit(&g_stats.frames_routed, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.rejected_bad_frame, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.rejected_unknown_tenant, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stats.rejected_backpressure, 0, memory_order_relaxed);
}

router_status_t router_register_tenant(uint16_t tenant_id, uint8_t actor_id) {
    if (tenant_id >= ROUTER_MAX_TENANTS || actor_id >= ACTOR_MAX_COUNT) {
        return ROUTER_ERR_INVALID;
    }
    g_router_tenant_map[tenant_id] = (uint8_t)(actor_id + 1u);
    return ROUTER_OK;
}

router_status_t router_dispatch(actor_system_t *sys, const uint8_t *frame, size_t frame_len) {
    if (sys == NULL || frame == NULL) {
        return ROUTER_ERR_INVALID;
    }

    if (frame_len < (size_t)ROUTER_HEADER_SIZE) {
        atomic_fetch_add_explicit(&g_stats.rejected_bad_frame, 1, memory_order_relaxed);
        return ROUTER_ERR_TRUNCATED;
    }

    uint32_t magic = router_load_u32le(frame + 0);
    uint8_t  version = frame[4];
    uint8_t  type = frame[5];
    uint16_t tenant_id = router_load_u16le(frame + 6);
    uint16_t payload_len = router_load_u16le(frame + 8);

    if (magic != ROUTER_MAGIC || version != ROUTER_VERSION) {
        atomic_fetch_add_explicit(&g_stats.rejected_bad_frame, 1, memory_order_relaxed);
        return ROUTER_ERR_BAD_MAGIC;
    }
    if (payload_len > ROUTER_MAX_PAYLOAD) {
        atomic_fetch_add_explicit(&g_stats.rejected_bad_frame, 1, memory_order_relaxed);
        return ROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    if (frame_len != (size_t)ROUTER_HEADER_SIZE + (size_t)payload_len) {
        atomic_fetch_add_explicit(&g_stats.rejected_bad_frame, 1, memory_order_relaxed);
        return ROUTER_ERR_TRUNCATED;
    }

    /* Strict Tenant ID filtering, O(1): a single bound check plus a
     * direct array read, no search -- and this happens BEFORE any actor
     * or mailbox is touched, so an invalid tenant_id can never affect any
     * actor's throughput or state. */
    if (tenant_id >= ROUTER_MAX_TENANTS) {
        atomic_fetch_add_explicit(&g_stats.rejected_unknown_tenant, 1, memory_order_relaxed);
        return ROUTER_ERR_UNKNOWN_TENANT;
    }
    uint8_t mapped = g_router_tenant_map[tenant_id];
    if (mapped == 0u) {
        atomic_fetch_add_explicit(&g_stats.rejected_unknown_tenant, 1, memory_order_relaxed);
        return ROUTER_ERR_UNKNOWN_TENANT;
    }

    uint8_t actor_id = (uint8_t)(mapped - 1u);
    actor_t *actor = actor_system_get(sys, actor_id);
    if (actor == NULL) {
        atomic_fetch_add_explicit(&g_stats.rejected_unknown_tenant, 1, memory_order_relaxed);
        return ROUTER_ERR_ACTOR_INACTIVE;
    }

    ring_msg_t *slot;
    if (actor_mailbox_reserve(actor, &slot) != ACTOR_OK) {
        atomic_fetch_add_explicit(&g_stats.rejected_backpressure, 1, memory_order_relaxed);
        return ROUTER_ERR_MAILBOX_FULL; /* O(1) instant rejection: no spin, no wait */
    }

    /* The ONE unavoidable copy: caller's frame -> the target actor's
     * mailbox slot, DIRECTLY. No staging buffer exists anywhere in
     * between. */
    const uint8_t *payload = frame + ROUTER_HEADER_SIZE;
    if (payload_len != 0u) {
        memcpy(slot->payload, payload, payload_len);
    }
    if (payload_len < ROUTER_MAX_PAYLOAD) {
        memset(slot->payload + payload_len, 0, ROUTER_MAX_PAYLOAD - payload_len);
    }

    actor_mailbox_publish(actor, slot, tenant_id, type);
    atomic_fetch_add_explicit(&g_stats.frames_routed, 1, memory_order_relaxed);
    return ROUTER_OK;
}

router_status_t router_build_frame(uint8_t *out, size_t out_cap, size_t *out_len,
                                    uint8_t type, uint16_t tenant_id,
                                    const uint8_t *payload, uint16_t payload_len) {
    if (out == NULL || out_len == NULL || (payload == NULL && payload_len != 0u)) {
        return ROUTER_ERR_INVALID;
    }
    if (payload_len > ROUTER_MAX_PAYLOAD) {
        return ROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t total = (size_t)ROUTER_HEADER_SIZE + (size_t)payload_len;
    if (out_cap < total) {
        return ROUTER_ERR_INVALID;
    }

    router_store_u32le(out + 0, ROUTER_MAGIC);
    out[4] = (uint8_t)ROUTER_VERSION;
    out[5] = type;
    router_store_u16le(out + 6, tenant_id);
    router_store_u16le(out + 8, payload_len);
    if (payload_len != 0u) {
        memcpy(out + ROUTER_HEADER_SIZE, payload, payload_len);
    }

    *out_len = total;
    return ROUTER_OK;
}

void router_get_stats(router_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->frames_routed = atomic_load_explicit(&g_stats.frames_routed, memory_order_relaxed);
    out->rejected_bad_frame = atomic_load_explicit(&g_stats.rejected_bad_frame, memory_order_relaxed);
    out->rejected_unknown_tenant =
        atomic_load_explicit(&g_stats.rejected_unknown_tenant, memory_order_relaxed);
    out->rejected_backpressure =
        atomic_load_explicit(&g_stats.rejected_backpressure, memory_order_relaxed);
}
