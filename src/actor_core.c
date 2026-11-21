#include "actor_core.h"

#include <string.h>

void actor_system_init(actor_system_t *sys) {
    for (uint32_t i = 0; i < ACTOR_MAX_COUNT; i++) {
        actor_t *a = &sys->actors[i];
        memset(a->private_memory, 0, sizeof a->private_memory);
        a->arena_used = 0u;
        rb_init(&a->mailbox);
        a->actor_id = (uint8_t)i;
        a->active = false;
        atomic_store_explicit(&a->processed_total, 0, memory_order_relaxed);
        /* No traffic allowed until the caller opts in via
         * actor_rate_limit_init() -- capacity 0 rejects every
         * try_consume() rather than silently granting unlimited tokens. */
        token_bucket_init(&a->rate_limiter, 0, 0);
    }
}

actor_status_t actor_system_spawn(actor_system_t *sys, uint8_t actor_id) {
    if (sys == NULL || actor_id >= ACTOR_MAX_COUNT) {
        return ACTOR_ERR_OUT_OF_RANGE;
    }
    actor_t *a = &sys->actors[actor_id];
    if (a->active) {
        return ACTOR_ERR_ALREADY_ACTIVE;
    }
    a->active = true;
    return ACTOR_OK;
}

actor_t *actor_system_get(actor_system_t *sys, uint8_t actor_id) {
    if (sys == NULL || actor_id >= ACTOR_MAX_COUNT) {
        return NULL;
    }
    actor_t *a = &sys->actors[actor_id];
    return a->active ? a : NULL;
}

actor_status_t actor_mailbox_reserve(actor_t *actor, ring_msg_t **out_slot) {
    if (actor == NULL || out_slot == NULL) {
        return ACTOR_ERR_INVALID;
    }
    rb_status_t rst = rb_reserve(&actor->mailbox, out_slot);
    if (rst == RB_ERR_FULL) {
        return ACTOR_ERR_MAILBOX_FULL;
    }
    if (rst != RB_OK) {
        return ACTOR_ERR_INVALID;
    }
    return ACTOR_OK;
}

void actor_mailbox_publish(actor_t *actor, ring_msg_t *slot, uint32_t sender_id, uint32_t type) {
    rb_publish(&actor->mailbox, slot, sender_id, type);
}

actor_status_t actor_process_one(actor_t *actor, ring_msg_t *out) {
    if (actor == NULL || out == NULL) {
        return ACTOR_ERR_INVALID;
    }
    rb_status_t rst = rb_try_pop(&actor->mailbox, out);
    if (rst == RB_ERR_EMPTY) {
        return ACTOR_ERR_EMPTY;
    }
    if (rst != RB_OK) {
        return ACTOR_ERR_INVALID;
    }
    atomic_fetch_add_explicit(&actor->processed_total, 1, memory_order_relaxed);
    return ACTOR_OK;
}

size_t actor_mailbox_count(const actor_t *actor) {
    return rb_count(&actor->mailbox);
}

/* ------------------------------------------------------------------ */
/* Private heap (bump/arena allocator over private_memory)              */
/* ------------------------------------------------------------------ */

void *actor_arena_alloc(actor_t *actor, size_t size, size_t align) {
    if (actor == NULL || size == 0u || align == 0u || (align & (align - 1u)) != 0u) {
        return NULL; /* align must be a power of two */
    }

    uint8_t *base = actor->private_memory + actor->arena_used;
    uintptr_t addr = (uintptr_t)base;
    uintptr_t aligned_addr = (addr + (align - 1u)) & ~(uintptr_t)(align - 1u);
    size_t padding = (size_t)(aligned_addr - addr);
    size_t needed = padding + size;

    if ((size_t)actor->arena_used + needed > (size_t)ACTOR_PRIVATE_MEM_SIZE) {
        return NULL; /* ACTOR_ERR_ARENA_FULL: does not fit in what remains */
    }

    actor->arena_used += (uint32_t)needed;
    return (void *)aligned_addr;
}

void actor_arena_reset(actor_t *actor) {
    if (actor == NULL) {
        return;
    }
    actor->arena_used = 0u;
}

/* ------------------------------------------------------------------ */
/* Per-actor rate limiting (token_bucket_t, rate_limit.h)                */
/* ------------------------------------------------------------------ */

void actor_rate_limit_init(actor_t *actor, int64_t capacity, int64_t initial_tokens) {
    if (actor == NULL) {
        return;
    }
    token_bucket_init(&actor->rate_limiter, capacity, initial_tokens);
}

bool actor_rate_limit_try_consume(actor_t *actor, int64_t cost) {
    if (actor == NULL) {
        return false;
    }
    return token_bucket_try_consume(&actor->rate_limiter, cost);
}

void actor_rate_limit_refill(actor_t *actor, int64_t amount) {
    if (actor == NULL) {
        return;
    }
    token_bucket_refill(&actor->rate_limiter, amount);
}
