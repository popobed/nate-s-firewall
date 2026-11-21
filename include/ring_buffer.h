/*
 * ring_buffer.h
 *
 * Phase 3 -- Generic lock-free MPSC ring buffer, the foundation primitive
 * for actor_core.h's per-actor mailboxes.
 *
 * This is the SAME corrected MPSC design already validated twice in this
 * project (net_gateway.h's ngw_mailbox_t, and net_io.c's usage of it),
 * reimplemented here as its own standalone, reusable module rather than
 * imported from net_gateway.h -- actor_core/msg_router are a deliberately
 * separate architectural layer.
 *
 * ============================ Design ========================================
 *   - Power-of-two capacity, mask-based slot indexing (index = position &
 *     (RING_BUFFER_CAPACITY - 1)) -- no modulo, no branch.
 *   - Fullness/backpressure: gated on "tail - head < CAPACITY", NOT on a
 *     per-slot flag set after the producer finishes writing. `head` is
 *     read with memory_order_acquire (paired with the consumer's
 *     memory_order_release store to it) BEFORE a producer's CAS attempt.
 *     A per-slot "occupied" flag set only after the write would reopen a
 *     reservation-before-publish race window on wraparound: between
 *     winning the tail CAS and marking the slot occupied, CAPACITY more
 *     reservations (which do not wait on any single slot's write) could
 *     let a second producer see the same slot as free and overwrite live,
 *     unread data. This is a REAL bug that was found, reproduced, and
 *     fixed earlier in this project's history (its first actor-mailbox
 *     module) -- this file starts from the corrected design directly.
 *   - `ready` per slot (release on publish / acquire on consume) is the
 *     data-ready handoff, orthogonal to the fullness gate above: it tells
 *     the consumer "this slot's fields are safe to read now", not "this
 *     slot is free to reuse".
 *   - reserve()/publish() are split so a caller whose data source can
 *     write directly into memory (recv() into a buffer, a DMA engine) can
 *     target the slot's payload with zero intermediate copies. try_push()
 *     is the convenience wrapper for callers that already have an
 *     assembled buffer to copy from.
 *   - _Alignas(64) throughout: ring_msg_t is exactly one cache line, and
 *     tail/head each get their own padded cache line so producers and the
 *     single consumer never false-share.
 *   - Zero dynamic allocation, ever: ring_buffer_t embeds its slot storage
 *     directly.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_CACHE_LINE_SIZE    64u
#define RING_BUFFER_CAPACITY  64u /* power of two */
#define RING_MSG_PAYLOAD_SIZE 48u /* sizeof(ring_msg_t) == 64 exactly */

_Static_assert(RING_BUFFER_CAPACITY != 0u &&
               (RING_BUFFER_CAPACITY & (RING_BUFFER_CAPACITY - 1u)) == 0u,
               "RING_BUFFER_CAPACITY must be a power of two");

typedef enum {
    RB_OK = 0,
    RB_ERR_INVALID = -1,
    RB_ERR_EMPTY   = -2,
    RB_ERR_FULL    = -3  /* O(1) instant rejection: backpressure, never a wait */
} rb_status_t;

/* Reserved ring_msg_t.type sentinel: published on an aborted reservation
 * (the source never finished writing its slot) so a consumer can tell
 * "nothing real here" apart from real data and skip it -- see
 * rb_publish()'s contract below. Never a valid `type` for real data. */
#define RING_MSG_TYPE_DISCARD 0xFFFFFFFFu

typedef struct {
    _Alignas(RB_CACHE_LINE_SIZE)
    _Atomic uint32_t ready;
    uint32_t         sender_id;
    uint32_t         type;
    uint32_t         _reserved;
    uint8_t          payload[RING_MSG_PAYLOAD_SIZE];
} ring_msg_t;

_Static_assert(sizeof(ring_msg_t) == 64u, "ring_msg_t must be exactly 64 bytes");
_Static_assert(_Alignof(ring_msg_t) == 64u, "ring_msg_t must be 64-byte aligned");

typedef struct {
    _Alignas(RB_CACHE_LINE_SIZE)
    _Atomic uint64_t tail; /* producers CAS this to reserve the next slot */
    uint8_t _pad_tail[RB_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    _Alignas(RB_CACHE_LINE_SIZE)
    _Atomic uint64_t head; /* consumer-owned; advanced after each consume */
    uint8_t _pad_head[RB_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    ring_msg_t slots[RING_BUFFER_CAPACITY]; /* embedded: zero dynamic allocation, ever */

    _Atomic uint64_t posted_total;
    _Atomic uint64_t consumed_total;
    _Atomic uint64_t rejected_full_total;
} ring_buffer_t;

void rb_init(ring_buffer_t *rb);

/*
 * Zero-copy producer API. Any number of concurrent callers. Reserves a
 * slot with a CAS loop on `tail`, gated by the head-synchronized fullness
 * check described above; never blocks, returns RB_ERR_FULL immediately
 * (O(1)) instead of retrying. On RB_OK, *out_slot points directly at the
 * slot's own payload buffer -- write into it directly, then call
 * rb_publish() (with real data, or RING_MSG_TYPE_DISCARD if the write was
 * aborted -- an un-published reservation permanently stalls every slot
 * behind it, not just this one, since `head` can never advance past it).
 */
rb_status_t rb_reserve(ring_buffer_t *rb, ring_msg_t **out_slot);
void        rb_publish(ring_buffer_t *rb, ring_msg_t *slot, uint32_t sender_id, uint32_t type);

/* Convenience wrapper for callers that already have an assembled buffer:
 * reserve + memcpy (zero-padding the remainder) + publish. */
rb_status_t rb_try_push(ring_buffer_t *rb, uint32_t sender_id, uint32_t type,
                         const void *data, size_t len);

/* Single consumer thread only. RB_ERR_EMPTY is not a failure, just
 * "nothing ready yet". */
rb_status_t rb_try_pop(ring_buffer_t *rb, ring_msg_t *out);

/* O(1) diagnostics (tail - head). Safe from any thread; may be stale by
 * the time the caller observes it, as with any live concurrent queue. */
size_t rb_count(const ring_buffer_t *rb);
bool   rb_is_full(const ring_buffer_t *rb);
bool   rb_is_empty(const ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
