/*
 * actor_core.h
 *
 * Phase 3 -- Isolated, multi-tenant-capable actors built on ring_buffer.h.
 *
 * Isolation model: each actor_t owns two things nothing else ever
 * touches -- a private memory block (private_memory, for the actor's own
 * state) and its own ring_buffer_t mailbox (an MPSC inbox where any
 * number of producers, typically msg_router.c, may post, but only this
 * actor's own processing call ever consumes). Nothing in this file lets
 * one actor read or write another's private_memory or mailbox: the only
 * cross-actor entry point is actor_mailbox_reserve()/_publish(), and it
 * always takes an explicit actor_t* naming exactly which actor's mailbox
 * is being targeted -- there is no ambient/implicit routing here (that
 * policy decision belongs to msg_router.c, one layer up).
 *
 * Zero dynamic allocation: actor_system_t embeds every actor_t (and thus
 * every actor's private memory and mailbox storage) directly. Declaring
 * one `static actor_system_t` is the entire allocation for the whole
 * actor population.
 *
 * ============================== Private heap, precisely =======================
 * private_memory's own _Alignas(64) already gives each actor a block that
 * starts on its own cache line, isolated from every other actor's block
 * (see the isolation model above) -- eliminating cache-bouncing between
 * actors touching adjacent memory was already true before this section
 * existed. What actor_arena_alloc()/actor_arena_reset() (below) ADD is
 * turning that block into an actual, usable "private fixed heap": a
 * bump/arena allocator with its own bounded cursor (arena_used), O(1)
 * alloc, never resized, never returned to a free list -- the same
 * "reset, don't free" discipline this project already applies to its
 * other arenas (barry_storage.h's shards, wire_proto.h's rings). This is
 * NOT a general-purpose heap (no free() of an individual allocation, no
 * splitting/coalescing, no growth beyond ACTOR_PRIVATE_MEM_SIZE) -- it is
 * the honest, zero-malloc-compatible meaning of "private heap" a fixed,
 * pre-reserved, per-actor arena can actually provide.
 *
 * ============================== Rate limiting, precisely =======================
 * Each actor_t embeds its own token_bucket_t (rate_limit.h) -- a
 * lock-free CAS token bucket, reused verbatim, not reimplemented. Gating
 * "trafic entrant" per actor (rather than one global limiter) means one
 * saturated actor's incoming load can never starve another actor's
 * allowance -- the same per-actor isolation posture private_memory/mailbox
 * already established, extended to rate limiting. Nothing in actor_core.c
 * refills the bucket automatically (no OS timer, ever) -- the caller
 * drives refill cadence explicitly, e.g. from chrono_tick.h's step
 * counter, exactly as that file's own header documents.
 */

#ifndef ACTOR_CORE_H
#define ACTOR_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ring_buffer.h"
#include "rate_limit.h"

#define ACTOR_MAX_COUNT        8u   /* fixed compile-time actor population */
#define ACTOR_PRIVATE_MEM_SIZE 256u /* private per-actor scratch/state block, doubling as its arena's backing storage */

typedef enum {
    ACTOR_OK = 0,
    ACTOR_ERR_INVALID       = -1,
    ACTOR_ERR_OUT_OF_RANGE  = -2, /* actor_id >= ACTOR_MAX_COUNT */
    ACTOR_ERR_INACTIVE      = -3, /* actor_id valid but never spawned */
    ACTOR_ERR_ALREADY_ACTIVE = -4,
    ACTOR_ERR_MAILBOX_FULL  = -5, /* O(1) backpressure rejection */
    ACTOR_ERR_EMPTY         = -6,
    ACTOR_ERR_ARENA_FULL    = -7  /* actor_arena_alloc(): request does not fit in the remaining private_memory */
} actor_status_t;

typedef struct {
    _Alignas(64) uint8_t private_memory[ACTOR_PRIVATE_MEM_SIZE]; /* isolated: only this actor touches it -- also this actor's private arena backing storage, see "Private heap, precisely" above */
    uint32_t      arena_used;      /* bump cursor into private_memory, 0..ACTOR_PRIVATE_MEM_SIZE -- owned solely by this actor's own thread, like private_memory itself */
    ring_buffer_t mailbox;         /* isolated inbox: this actor is the ONLY consumer */
    uint8_t       actor_id;
    bool          active;
    _Atomic uint64_t processed_total;
    token_bucket_t rate_limiter;   /* per-actor lock-free CAS token bucket -- see "Rate limiting, precisely" above */
} actor_t;

typedef struct {
    actor_t actors[ACTOR_MAX_COUNT];
} actor_system_t;

/* Resets every actor slot to inactive with a freshly rb_init()'d mailbox
 * and a zeroed private memory block. Not thread-safe with concurrent
 * producers/consumers; call before anything else touches actors[]. */
void actor_system_init(actor_system_t *sys);

/* Activates actors[actor_id]. Returns ACTOR_ERR_ALREADY_ACTIVE if it was
 * already spawned (idempotent registration is the caller's job, not
 * silently allowed here -- a double-spawn is almost always a bug). */
actor_status_t actor_system_spawn(actor_system_t *sys, uint8_t actor_id);

/* Returns the actor, or NULL if actor_id is out of range or inactive.
 * Safe to call from any thread (only reads `active`, itself only ever
 * written by actor_system_init()/_spawn() before concurrent use starts). */
actor_t *actor_system_get(actor_system_t *sys, uint8_t actor_id);

/*
 * Zero-copy producer API, scoped to one specific actor's isolated
 * mailbox -- thin wrappers over ring_buffer.h's rb_reserve()/rb_publish()
 * (any number of concurrent callers; see ring_buffer.h for the full
 * reservation contract, including why an aborted reservation MUST still
 * be published, with type == RING_MSG_TYPE_DISCARD if necessary).
 */
actor_status_t actor_mailbox_reserve(actor_t *actor, ring_msg_t **out_slot);
void           actor_mailbox_publish(actor_t *actor, ring_msg_t *slot, uint32_t sender_id, uint32_t type);

/* Single-consumer side: this actor's own processing step. Call from
 * exactly one thread per actor (that thread IS the actor, in effect).
 * Non-blocking; ACTOR_ERR_EMPTY is not a failure, just "nothing yet". */
actor_status_t actor_process_one(actor_t *actor, ring_msg_t *out);

size_t actor_mailbox_count(const actor_t *actor);

/* ------------------------------------------------------------------ */
/* Private heap (bump/arena allocator over private_memory)              */
/* ------------------------------------------------------------------ */

/*
 * O(1): carves `size` bytes, aligned to `align` (must be a power of two),
 * off the front of the remaining unused tail of `actor`'s private_memory
 * and returns a pointer to it. Returns NULL (ACTOR_ERR_ARENA_FULL,
 * distinguishable from a real allocation only by the NULL return -- there
 * is nothing else to report) if the aligned request does not fit in what
 * remains. Not thread-safe against concurrent callers on the SAME actor
 * (matches this project's "one thread IS the actor" posture for
 * actor_process_one()) -- safe across DIFFERENT actors, which never share
 * any memory. Never returns memory to a free list; call
 * actor_arena_reset() to reclaim the whole block at once.
 */
void *actor_arena_alloc(actor_t *actor, size_t size, size_t align);

/* O(1): resets the arena to empty (arena_used = 0) WITHOUT zeroing
 * private_memory's bytes (that would be an O(n) operation this bump
 * allocator's O(1) contract does not include) -- a caller relying on
 * zeroed storage must memset() what it allocates itself, the same
 * discipline malloc() vs calloc() already draws. Every pointer previously
 * returned by actor_arena_alloc() for this actor is invalidated the
 * instant this is called. */
void actor_arena_reset(actor_t *actor);

/* ------------------------------------------------------------------ */
/* Per-actor rate limiting (token_bucket_t, rate_limit.h)                */
/* ------------------------------------------------------------------ */

/* Boot-time-only, like actor_system_init(): not thread-safe against
 * concurrent refill/try_consume calls on this actor's bucket. */
void actor_rate_limit_init(actor_t *actor, int64_t capacity, int64_t initial_tokens);

/* O(1), lock-free CAS (rate_limit.h, reused verbatim) -- see that file
 * for the exact contract. Typical use: gate whether actor_process_one()
 * is even attempted for this poll iteration. */
bool actor_rate_limit_try_consume(actor_t *actor, int64_t cost);

/* O(1), lock-free CAS. The caller decides `amount` and when to call this
 * -- e.g. once per chrono_tick_advance() -- never an OS timer, see
 * rate_limit.h/chrono_tick.h. */
void actor_rate_limit_refill(actor_t *actor, int64_t amount);

#endif /* ACTOR_CORE_H */
