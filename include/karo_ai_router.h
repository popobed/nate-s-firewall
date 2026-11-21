/*
 * karo_ai_router.h
 *
 * KARO's multi-provider selector: three independent karo_key_pool_t
 * instances (karo_interface.h Part A, reused verbatim -- not
 * reimplemented) behind one active-provider selector, with the SAME
 * bounded lock-free CAS rotation on HTTP 429 already tested there, plus
 * ONE additive behavior: if the currently active provider's own pool is
 * entirely unprovisioned, fall over to the next provider that actually
 * has a key, via a second bounded CAS loop over active_provider.
 *
 * ============================== Honesty notes, up front =====================
 *   - This is NOT an HTTP client and does not call Gemini, Claude, or
 *     DeepSeek (or anything else) over the network -- exactly like
 *     karo_interface.h's own Part A, no HTTP client exists anywhere in
 *     this codebase. karo_ai_router_active_key() hands the caller's own
 *     HTTP client the key+provider to use; karo_ai_router_report_429()
 *     is what that caller calls back after IT observes a 429 from
 *     whichever provider it actually called.
 *   - "DeepSeek/Alternatives" is modeled as ONE additional named provider
 *     slot (KARO_ROUTER_PROVIDER_DEEPSEEK) -- the request named three
 *     concrete choices, so this file provisions exactly three, not an
 *     open-ended plugin system nobody asked for.
 */

#ifndef KARO_AI_ROUTER_H
#define KARO_AI_ROUTER_H

#include <stdatomic.h>
#include <stdint.h>

#include "karo_interface.h" /* karo_key_pool_t and its init/provision/active_key/rotate_key_cas: reused verbatim */

typedef enum {
    KARO_ROUTER_PROVIDER_GEMINI   = 0,
    KARO_ROUTER_PROVIDER_CLAUDE   = 1,
    KARO_ROUTER_PROVIDER_DEEPSEEK = 2
} karo_router_provider_t;

#define KARO_ROUTER_PROVIDER_COUNT 3u

typedef struct {
    karo_key_pool_t   pools[KARO_ROUTER_PROVIDER_COUNT]; /* one karo_key_pool_t per provider, reused verbatim */
    _Atomic uint32_t  active_provider;                    /* karo_router_provider_t, CAS-rotated */
    _Atomic uint64_t  provider_switches_total;             /* diagnostics: cross-provider fallbacks only */
} karo_ai_router_t;

void karo_ai_router_init(karo_ai_router_t *router);

/* Boot-time/admin-time only, like karo_key_pool_provision() itself. */
karo_status_t karo_ai_router_provision(karo_ai_router_t *router, karo_router_provider_t provider,
                                        uint32_t slot_index, const char *key, uint32_t len);

/* Explicit user selection (the config UI switching providers by hand). */
karo_status_t karo_ai_router_select_provider(karo_ai_router_t *router, karo_router_provider_t provider);

karo_router_provider_t karo_ai_router_active_provider(const karo_ai_router_t *router);

/* O(1) direct read of the active provider's active key -- same contract
 * as karo_key_pool_active_key() (karo_interface.h), one layer up. */
karo_status_t karo_ai_router_active_key(const karo_ai_router_t *router, char *out_key, uint32_t *inout_len);

/*
 * On a real 429 (KARO_HTTP_STATUS_QUOTA_EXCEEDED, karo_interface.h):
 * first tries karo_rotate_key_cas() within the active provider's own pool
 * (same <100us bounded CAS loop, reused verbatim, see karo_interface.h).
 * If that pool has literally no provisioned key at all (KARO_ERR_FULL),
 * scans the other KARO_ROUTER_PROVIDER_COUNT-1 providers ONCE, in a fixed
 * bounded order, and CAS-switches active_provider to the first one that
 * has an active key. Returns KARO_OK if either a same-provider rotation
 * or a cross-provider switch happened, KARO_ERR_FULL only if every
 * provider's pool is empty. Under concurrent callers reporting 429s from
 * different providers at once, a lost CAS race can return KARO_OK for a
 * switch a different thread already made -- the same best-effort-under-
 * contention posture karo_rotate_key_cas() itself already documents, not
 * a new guarantee this file invents.
 */
karo_status_t karo_ai_router_report_429(karo_ai_router_t *router);

#endif /* KARO_AI_ROUTER_H */
