#include "rate_limit.h"

void token_bucket_init(token_bucket_t *tb, int64_t capacity, int64_t initial_tokens) {
    tb->capacity = capacity;
    int64_t clamped = initial_tokens;
    if (clamped > capacity) {
        clamped = capacity;
    }
    if (clamped < 0) {
        clamped = 0;
    }
    atomic_store_explicit(&tb->tokens, clamped, memory_order_relaxed);
}

void token_bucket_refill(token_bucket_t *tb, int64_t amount) {
    if (amount <= 0) {
        return;
    }
    int64_t old = atomic_load_explicit(&tb->tokens, memory_order_relaxed);
    for (;;) {
        int64_t new_val = old + amount;
        if (new_val > tb->capacity) {
            new_val = tb->capacity;
        }
        if (atomic_compare_exchange_weak_explicit(&tb->tokens, &old, new_val,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            return;
        }
        /* CAS failure refreshes `old` to the current value; retry against it. */
    }
}

bool token_bucket_try_consume(token_bucket_t *tb, int64_t cost) {
    if (cost <= 0) {
        return true;
    }
    int64_t old = atomic_load_explicit(&tb->tokens, memory_order_relaxed);
    for (;;) {
        if (old < cost) {
            return false; /* insufficient balance: fail fast, never loop on this path */
        }
        int64_t new_val = old - cost;
        if (atomic_compare_exchange_weak_explicit(&tb->tokens, &old, new_val,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            return true;
        }
        /* CAS lost the race against a concurrent consumer/refill: `old` was
         * refreshed to the current value; retry the balance check against it. */
    }
}

int64_t token_bucket_available(const token_bucket_t *tb) {
    return atomic_load_explicit((_Atomic int64_t *)&tb->tokens, memory_order_relaxed);
}
