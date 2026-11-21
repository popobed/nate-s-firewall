/*
 * karo_ai_router.c
 *
 * See include/karo_ai_router.h for the full design and honest-scope
 * notes. Implementation only.
 */

#include "karo_ai_router.h"

#include <string.h>

void karo_ai_router_init(karo_ai_router_t *router) {
    if (router == NULL) {
        return;
    }
    for (uint32_t p = 0u; p < KARO_ROUTER_PROVIDER_COUNT; p++) {
        karo_key_pool_init(&router->pools[p]);
    }
    atomic_store_explicit(&router->active_provider, (uint32_t)KARO_ROUTER_PROVIDER_GEMINI, memory_order_relaxed);
    atomic_store_explicit(&router->provider_switches_total, 0u, memory_order_relaxed);
}

karo_status_t karo_ai_router_provision(karo_ai_router_t *router, karo_router_provider_t provider,
                                        uint32_t slot_index, const char *key, uint32_t len) {
    if (router == NULL) {
        return KARO_ERR_INVALID;
    }
    if ((uint32_t)provider >= KARO_ROUTER_PROVIDER_COUNT) {
        return KARO_ERR_OUT_OF_RANGE;
    }
    return karo_key_pool_provision(&router->pools[(uint32_t)provider], slot_index, key, len);
}

karo_status_t karo_ai_router_select_provider(karo_ai_router_t *router, karo_router_provider_t provider) {
    if (router == NULL) {
        return KARO_ERR_INVALID;
    }
    if ((uint32_t)provider >= KARO_ROUTER_PROVIDER_COUNT) {
        return KARO_ERR_OUT_OF_RANGE;
    }
    atomic_store_explicit(&router->active_provider, (uint32_t)provider, memory_order_release);
    return KARO_OK;
}

karo_router_provider_t karo_ai_router_active_provider(const karo_ai_router_t *router) {
    if (router == NULL) {
        return KARO_ROUTER_PROVIDER_GEMINI;
    }
    return (karo_router_provider_t)atomic_load_explicit(&router->active_provider, memory_order_acquire);
}

karo_status_t karo_ai_router_active_key(const karo_ai_router_t *router, char *out_key, uint32_t *inout_len) {
    if (router == NULL) {
        return KARO_ERR_INVALID;
    }
    uint32_t active = atomic_load_explicit(&router->active_provider, memory_order_acquire);
    return karo_key_pool_active_key(&router->pools[active], out_key, inout_len);
}

karo_status_t karo_ai_router_report_429(karo_ai_router_t *router) {
    if (router == NULL) {
        return KARO_ERR_INVALID;
    }

    uint32_t old_active = atomic_load_explicit(&router->active_provider, memory_order_acquire);
    karo_status_t rot = karo_rotate_key_cas(&router->pools[old_active], KARO_HTTP_STATUS_QUOTA_EXCEEDED);
    if (rot != KARO_ERR_FULL) {
        return rot; /* KARO_OK: rotated within the active provider (or a real error) */
    }

    /* Active provider has no provisioned key at all: scan the others once,
     * in a fixed bounded order (see header). */
    for (uint32_t step = 1u; step <= KARO_ROUTER_PROVIDER_COUNT; step++) {
        uint32_t candidate = (old_active + step) % KARO_ROUTER_PROVIDER_COUNT;
        char probe[KARO_KEY_MAX_LEN];
        uint32_t probe_len = (uint32_t)sizeof probe;
        if (karo_key_pool_active_key(&router->pools[candidate], probe, &probe_len) != KARO_OK) {
            continue; /* this provider has no active key either */
        }
        uint32_t expected = old_active;
        if (atomic_compare_exchange_strong_explicit(&router->active_provider, &expected, candidate,
                                                      memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&router->provider_switches_total, 1u, memory_order_relaxed);
            return KARO_OK;
        }
        /* Another thread already switched active_provider concurrently --
         * some valid provider is active now, which is what the caller
         * needs; see header's note on this best-effort posture. */
        return KARO_OK;
    }
    return KARO_ERR_FULL; /* every provider's pool is empty */
}
