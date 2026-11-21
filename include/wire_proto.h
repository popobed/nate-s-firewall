/*
 * wire_proto.h
 *
 * Compact native binary protocol (Option 1 -- no gRPC/Protobuf: this
 * project stays zero-external-dependency, and a real gRPC stack needs
 * HTTP/2 + the Protobuf runtime, neither of which is compatible with
 * "zero dynamic allocation, no external library").
 *
 * ============================ Design ========================================
 * 1. wire_msg_t is `__attribute__((packed))`: every field sits at a fixed,
 *    compiler-independent byte offset with NO inserted padding, so its
 *    in-memory layout is BYTE-IDENTICAL to what arrives over the wire.
 *    sizeof(wire_msg_t) == 64 exactly (verified by _Static_assert and by
 *    a runtime test) -- one cache line, no more, no less.
 *
 *    Packing has a real, checked consequence: _Alignof(wire_msg_t) == 1
 *    (a packed struct carries no natural alignment requirement of its
 *    own -- confirmed empirically before writing this file). That is
 *    exactly why requirement 3 below is a SEPARATE concern: 64-byte
 *    alignment comes from `_Alignas(64)` on the CONTAINER (wire_ring_t's
 *    slots array), not from the packed struct itself. Because
 *    sizeof(wire_msg_t) is an exact multiple of 64, a 64-aligned array
 *    base keeps every element 64-aligned automatically.
 *
 * 2. Zero-copy ingestion: net_io.c's wire-mode server recv()s the raw
 *    frame bytes DIRECTLY into a reserved slot's memory (a contiguous,
 *    in-place write -- see wire_ring_reserve()) with NO decode/hydration
 *    function: once all 64 bytes have arrived, the slot's memory already
 *    IS a valid wire_msg_t, simply because the wire layout and the struct
 *    layout are the same layout. The one exception, and it is
 *    deliberate: routing needs `tenant_id` (bytes 4-5) BEFORE the target
 *    slot can even be chosen, so net_io.c peeks those 6 bytes
 *    (MSG_PEEK, non-destructive) to pick the destination ring first --
 *    that is a routing-metadata read, not a decode of the message itself
 *    (the same distinction a switch reading a VLAN tag without touching
 *    the frame's payload makes). See net_io.h.
 *
 *    Honesty note: with no decode step, the wire format is read at
 *    whatever endianness the host CPU uses (host-native, no htons/ntohs
 *    conversion) -- correct and sufficient for this project's scope (one
 *    machine, loopback sockets), but NOT safe to use as-is between
 *    machines of different endianness. A real cross-architecture
 *    deployment would need an explicit byte-order conversion step, which
 *    would reintroduce exactly the "decode function" this design
 *    deliberately avoids -- a real trade-off, stated plainly rather than
 *    silently assumed.
 *
 * 3. wire_ring_t is the same corrected lock-free MPSC design already
 *    validated repeatedly in this project (ring_buffer.h, net_gateway.h):
 *    fullness gated on "tail - head < CAPACITY" (head read with
 *    memory_order_acquire, paired with the consumer's memory_order_release
 *    store to it), NOT a per-slot flag set after the write -- that
 *    ordering is what closes the reservation-before-publish race window
 *    on wraparound found and fixed earlier in this project's history.
 *    The per-slot "ready" handoff flag lives in a SEPARATE array
 *    (wire_ring_t.ready[]), deliberately not folded into the packed
 *    struct: an _Atomic field needs its natural alignment to compile to
 *    real lock-free atomic instructions on every target, and `packed`
 *    explicitly discards alignment guarantees -- mixing the two in one
 *    struct would risk silently falling back to a slower (or, on a
 *    stricter target, incorrect) non-native atomic access. Keeping
 *    `ready` out of the packed struct sidesteps that risk entirely rather
 *    than relying on it happening to work on this compiler/target.
 */

#ifndef WIRE_PROTO_H
#define WIRE_PROTO_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIRE_CACHE_LINE_SIZE   64u
#define WIRE_RING_CAPACITY     64u /* power of two */
#define WIRE_MSG_PAYLOAD_SIZE  54u /* sized so sizeof(wire_msg_t) == 64 exactly */

_Static_assert(WIRE_RING_CAPACITY != 0u && (WIRE_RING_CAPACITY & (WIRE_RING_CAPACITY - 1u)) == 0u,
               "WIRE_RING_CAPACITY must be a power of two");

typedef enum {
    WIRE_OK = 0,
    WIRE_ERR_INVALID = -1,
    WIRE_ERR_EMPTY   = -2,
    WIRE_ERR_FULL    = -3 /* O(1) instant rejection: backpressure, never a wait */
} wire_status_t;

#define WIRE_MAGIC   0x30575752u /* "RWW0" as bytes R,W,W,0 host-native order */
#define WIRE_VERSION 1u

/* Reserved wire_msg_t.type sentinel: written by wire_ring_publish() on an
 * aborted reservation so a consumer can skip it -- never a valid `type`
 * for real data. See the reservation contract on wire_ring_reserve(). */
#define WIRE_MSG_TYPE_DISCARD 0xFFu

/*
 * The compact wire format itself -- __attribute__((packed)): no compiler-
 * inserted padding anywhere, so this layout IS the wire layout, byte for
 * byte. Field order matches the offsets documented in wire_proto.h's file
 * header and mirrored in net_io.h's wire-mode ingestion:
 *   offset 0 : magic       u32
 *   offset 4 : tenant_id   u16
 *   offset 6 : type        u8
 *   offset 7 : version     u8
 *   offset 8 : payload_len u16
 *   offset 10: payload     WIRE_MSG_PAYLOAD_SIZE bytes
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t tenant_id;
    uint8_t  type;
    uint8_t  version;
    uint16_t payload_len;
    uint8_t  payload[WIRE_MSG_PAYLOAD_SIZE];
} wire_msg_t;

_Static_assert(sizeof(wire_msg_t) == 64u, "wire_msg_t must be exactly 64 bytes");
_Static_assert(offsetof(wire_msg_t, tenant_id) == 4u, "tenant_id offset is relied on by net_io.c's MSG_PEEK routing read");

typedef struct {
    _Alignas(WIRE_CACHE_LINE_SIZE)
    _Atomic uint64_t tail; /* producers CAS this to reserve the next slot */
    uint8_t _pad_tail[WIRE_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    _Alignas(WIRE_CACHE_LINE_SIZE)
    _Atomic uint64_t head; /* consumer-owned; advanced after each consume */
    uint8_t _pad_head[WIRE_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    /* Data-ready handoff flags, kept OUT of the packed wire_msg_t itself
     * (see the file header's point 3) -- one per slot, same index. */
    _Alignas(WIRE_CACHE_LINE_SIZE)
    _Atomic uint8_t ready[WIRE_RING_CAPACITY];

    _Alignas(WIRE_CACHE_LINE_SIZE)
    wire_msg_t slots[WIRE_RING_CAPACITY]; /* embedded: zero dynamic allocation, ever */

    _Atomic uint64_t posted_total;
    _Atomic uint64_t consumed_total;
    _Atomic uint64_t rejected_full_total;
} wire_ring_t;

void wire_ring_init(wire_ring_t *ring);

/*
 * Zero-copy producer API. Any number of concurrent callers. Reserves a
 * slot with a CAS loop on `tail`, gated by the head-synchronized fullness
 * check described in the file header; never blocks, returns WIRE_ERR_FULL
 * immediately (O(1)) instead of retrying. On WIRE_OK, *out_slot points
 * DIRECTLY at the slot's raw memory -- for net_io.c's wire-mode server,
 * that means recv() may target *out_slot as its buffer with no
 * intermediate copy at all. Call wire_ring_publish() once the slot's
 * bytes are valid (or with type == WIRE_MSG_TYPE_DISCARD if the source
 * aborted mid-frame -- an un-published reservation permanently stalls
 * every slot behind it, not just this one, since `head` can never
 * advance past it).
 *
 * Security hardening (CWE-457): the slot is fully zeroed (fixed 64 bytes)
 * before this function returns it -- so a short/partial write by the
 * caller (recv() returning fewer bytes than a full frame, say) leaves a
 * deterministic zero tail rather than whatever the slot's PREVIOUS
 * occupant (a different tenant's message, in general) left behind.
 */
wire_status_t wire_ring_reserve(wire_ring_t *ring, wire_msg_t **out_slot);
void          wire_ring_publish(wire_ring_t *ring, wire_msg_t *slot, uint8_t type);

/* Single consumer thread only. WIRE_ERR_EMPTY is not a failure, just
 * "nothing ready yet". */
wire_status_t wire_ring_try_pop(wire_ring_t *ring, wire_msg_t *out);

/* O(1) diagnostics (tail - head). Safe from any thread; may be stale by
 * the time the caller observes it, as with any live concurrent queue. */
size_t wire_ring_count(const wire_ring_t *ring);
bool   wire_ring_is_full(const wire_ring_t *ring);
bool   wire_ring_is_empty(const wire_ring_t *ring);

/* Builds a valid wire_msg_t frame into `out` (host-native layout, see the
 * endianness note in the file header). Returns WIRE_ERR_INVALID if
 * payload_len exceeds WIRE_MSG_PAYLOAD_SIZE. */
wire_status_t wire_build_msg(wire_msg_t *out, uint16_t tenant_id, uint8_t type,
                              const uint8_t *payload, uint16_t payload_len);

#endif /* WIRE_PROTO_H */
