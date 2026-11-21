/*
 * msg_router.h
 *
 * Phase 3 -- O(1) binary header parsing + strict Tenant ID filtering +
 * zero-copy dispatch into actor_core.h mailboxes, with deterministic
 * backpressure.
 *
 * ============================ Wire format ==================================
 * Fixed layout (bounded, no delimiter scan -- same "O(1)/bounded parsing"
 * posture as this project's other wire formats), little-endian:
 *
 *   offset 0 : magic       u32 = ROUTER_MAGIC
 *   offset 4 : version     u8  = ROUTER_VERSION
 *   offset 5 : type        u8  (opaque application message type)
 *   offset 6 : tenant_id   u16
 *   offset 8 : payload_len u16 (<= ROUTER_MAX_PAYLOAD)
 *   offset 10: payload     payload_len bytes
 *
 * ROUTER_MAX_PAYLOAD == RING_MSG_PAYLOAD_SIZE: a frame's payload always
 * fits in exactly one ring_msg_t, which is what makes true zero-copy
 * dispatch possible (see below) -- there is never a "spans multiple
 * slots" case to handle.
 *
 * ============================ Tenant filtering, O(1) =======================
 * g_router_tenant_map[ROUTER_MAX_TENANTS] (msg_router.c) is indexed
 * directly by the wire tenant_id field -- one array read, no search, same
 * idiom as this project's other O(1) routing tables (net_io.c's
 * g_tenant_actor_map). 0 means "unmapped"; a registered tenant stores
 * actor_id + 1. An unmapped or out-of-range tenant_id is rejected in O(1)
 * (ROUTER_ERR_UNKNOWN_TENANT, rejected_unknown_tenant++) BEFORE any
 * mailbox is touched -- by construction, a flood of invalid-tenant frames
 * can never reach, slow down, or fill any actor's mailbox: the rejection
 * happens strictly before actor_mailbox_reserve() is even called.
 *
 * ============================ Zero-copy dispatch ===========================
 * router_dispatch() reserves a slot directly in the TARGET actor's
 * mailbox (actor_mailbox_reserve()) and memcpy()s the payload straight
 * into that slot's own buffer -- there is no staging/intermediate buffer
 * anywhere in this path. That memcpy (frame -> mailbox slot) is the one,
 * unavoidable copy of bytes crossing from the caller's buffer into memory
 * this system owns; nothing downstream ever copies the payload again.
 *
 * ============================ Backpressure ==================================
 * If the target actor's mailbox is saturated, actor_mailbox_reserve()
 * fails and router_dispatch() returns ROUTER_ERR_MAILBOX_FULL immediately
 * -- O(1), no retry, no blocking, and (being reservation-gated on `head`,
 * see ring_buffer.h) no risk of overwriting a message that actor has not
 * yet processed.
 */

#ifndef MSG_ROUTER_H
#define MSG_ROUTER_H

#include <stddef.h>
#include <stdint.h>

#include "actor_core.h"

#define ROUTER_MAX_TENANTS 256u /* direct-indexed by the wire tenant_id: O(1) */

#define ROUTER_MAGIC       0x30525452u /* "RTR0" as bytes R,T,R,0 little-endian */
#define ROUTER_VERSION     1u
#define ROUTER_MAX_PAYLOAD RING_MSG_PAYLOAD_SIZE /* a frame's payload = exactly one ring_msg_t */
#define ROUTER_HEADER_SIZE (4u + 1u + 1u + 2u + 2u) /* = 10 */
#define ROUTER_MAX_FRAME_SIZE (ROUTER_HEADER_SIZE + ROUTER_MAX_PAYLOAD)

typedef enum {
    ROUTER_OK = 0,
    ROUTER_ERR_INVALID           = -1,
    ROUTER_ERR_TRUNCATED         = -2,
    ROUTER_ERR_BAD_MAGIC         = -3,
    ROUTER_ERR_PAYLOAD_TOO_LARGE = -4,
    ROUTER_ERR_UNKNOWN_TENANT    = -5, /* O(1) rejection, before any mailbox is touched */
    ROUTER_ERR_ACTOR_INACTIVE    = -6,
    ROUTER_ERR_MAILBOX_FULL      = -7  /* O(1) backpressure rejection */
} router_status_t;

typedef struct {
    uint64_t frames_routed;
    uint64_t rejected_bad_frame;      /* truncated / bad magic / oversized */
    uint64_t rejected_unknown_tenant;
    uint64_t rejected_backpressure;   /* target mailbox full */
} router_stats_t;

/* Resets the global tenant->actor map and stats. Not thread-safe with
 * concurrent dispatch; call before anything else touches the router. */
void router_init(void);

/* Maps tenant_id to actor_id -- O(1) direct write. */
router_status_t router_register_tenant(uint16_t tenant_id, uint8_t actor_id);

/*
 * Parses one frame (see "Wire format" above), applies O(1) Tenant ID
 * filtering, and dispatches -- zero-copy -- into the mapped actor's
 * mailbox in `sys`. Every rejection path is a deterministic, bounded-time
 * return; never a blocking wait, never a retry loop.
 */
router_status_t router_dispatch(actor_system_t *sys, const uint8_t *frame, size_t frame_len);

/* Builds a wire frame -- the counterpart to router_dispatch()'s parser,
 * used by callers/tests to construct valid frames. */
router_status_t router_build_frame(uint8_t *out, size_t out_cap, size_t *out_len,
                                    uint8_t type, uint16_t tenant_id,
                                    const uint8_t *payload, uint16_t payload_len);

void router_get_stats(router_stats_t *out);

#endif /* MSG_ROUTER_H */
