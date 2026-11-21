/*
 * rate_limit.h
 *
 * Lock-free CAS token bucket -- a generic, reusable rate-limiting
 * primitive (not CHRONO-BLACKBOX-specific; actor_core.h is its first
 * caller, one per actor_t).
 *
 * ============================== Design, precisely =============================
 * Classic lazy token bucket, but refilled by the CALLER's own cadence
 * (token_bucket_refill(), driven by e.g. chrono_tick.h's step counter or
 * any other caller-owned counter), never by an OS timer -- consistent
 * with this project's "no calendar, no RTC dependency" posture everywhere
 * else (wire_proto.h, barry_storage.h's barry_ts_store_t, swarm_dispatch.h's
 * current_cycle). A bucket that is never refilled simply never grants more
 * tokens than it started with -- deterministic, not silently time-based.
 *
 * token_bucket_try_consume() is a single CAS loop: load current tokens,
 * fail fast (false, unchanged) if insufficient, otherwise attempt to swap
 * in tokens-cost, retrying only on a LOST race against another consumer
 * (never on insufficient balance -- that path never loops). No mutex, no
 * spinlock, no blocking wait: O(1) amortized, deterministic instant
 * rejection when exhausted, the same backpressure posture as every other
 * bounded structure in this project (barry_task_queue_t, wire_ring_t,
 * ring_buffer_t).
 *
 * token_bucket_refill() is its own separate CAS loop (capped at
 * `capacity`, never overflowing the configured burst size) -- safe to
 * call concurrently with any number of token_bucket_try_consume() callers
 * from other threads.
 *
 * Zero allocation, no locks, no shared global state: one token_bucket_t
 * per caller-owned instance, embedded wherever it is needed (actor_t,
 * here).
 */

#ifndef RATE_LIMIT_H
#define RATE_LIMIT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct {
    _Atomic int64_t tokens;
    int64_t         capacity; /* max burst size -- refill() never exceeds this; set once at init, never mutated after */
} token_bucket_t;

/* Boot-time-only (like this project's other *_init() calls): not
 * thread-safe against concurrent refill()/try_consume() calls. */
void token_bucket_init(token_bucket_t *tb, int64_t capacity, int64_t initial_tokens);

/* O(1), lock-free, any number of concurrent callers: adds `amount`
 * tokens, capped at `capacity`. amount <= 0 is a no-op (never removes
 * tokens -- use try_consume() for that). */
void token_bucket_refill(token_bucket_t *tb, int64_t amount);

/* O(1), lock-free, any number of concurrent callers: atomically consumes
 * `cost` tokens and returns true iff the bucket held at least that many;
 * otherwise returns false and leaves the bucket UNCHANGED (no partial
 * consumption). cost <= 0 always succeeds without changing the bucket. */
bool token_bucket_try_consume(token_bucket_t *tb, int64_t cost);

/* O(1) diagnostic snapshot; may be stale by the time the caller observes
 * it under concurrent refill()/try_consume(), same caveat as this
 * project's other live counters (e.g. ring_buffer.h's rb_count()). */
int64_t token_bucket_available(const token_bucket_t *tb);

#endif /* RATE_LIMIT_H */
