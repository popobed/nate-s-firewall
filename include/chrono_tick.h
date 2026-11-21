/*
 * chrono_tick.h
 *
 * CHRONO-BLACKBOX -- O(1) software step counter for pacing Merkle-window
 * flush and chain-purge scheduling, with zero OS-clock dependency.
 *
 * ============================== Scope, precisely =============================
 * Deliberately NOT swarm_dispatch.h's swarm_tick() -- that function is a
 * different, already-shipped, semantically distinct thing: a per-tenant
 * "is a scheduled followup due" check driven by a caller-supplied
 * current_cycle. Reusing its name here for a process-wide step counter
 * would conflate two different concepts under one identifier; this file
 * uses its own name and its own, separate counter instead.
 *
 * chrono_tick_counter_t is a single monotonically increasing step count,
 * advanced explicitly by the caller (chrono_tick_advance(), typically
 * once per reactor poll iteration or once per sealed block -- the
 * caller's own choice of what one "tick" means, the same "opaque,
 * caller-owned counter" posture barry_storage.h's barry_ts_store_t and
 * swarm_dispatch.h's current_cycle already established). There is no
 * internal thread, no OS timer, no QueryPerformanceCounter/RTC read
 * anywhere in this file -- advancing the counter is the ONLY way its
 * value ever changes, so pacing decisions built on it are exactly as
 * deterministic and replayable as the caller's own call pattern.
 *
 * chrono_tick_due() is the one scheduling primitive this file provides:
 * an O(1) comparison ("has at least `interval` ticks elapsed since the
 * last time this fired") plus, on success, an atomic one-shot update of
 * the caller-owned `*last_fire_step` -- the same one-shot-then-rearm
 * shape swarm_dispatch.h's followup_due_cycle already uses, generalized
 * to any number of independent schedules (a window-flush cadence and a
 * chain-purge cadence can each own their own `last_fire_step`, ticking
 * off the SAME shared counter without interfering with each other).
 *
 * ============================== What this is NOT ============================
 * This file does not itself flush a window or purge a chain -- it only
 * answers "is it time yet?". Wiring chrono_tick_due() into an always-on
 * daemon loop that actually calls chrono_storage_flush_window() and some
 * chain-purge routine on a real cadence is the caller's job (this
 * project's build has no long-running daemon process for CHRONO-BLACKBOX
 * yet -- see chrono_cli.c for the offline, one-shot verification tool
 * that exists today). Building an unrequested daemon loop here would be
 * exactly the kind of unbuilt, unverified claim this project's other
 * modules already refuse to make.
 */

#ifndef CHRONO_TICK_H
#define CHRONO_TICK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

typedef struct {
    _Atomic uint64_t step;
} chrono_tick_counter_t;

void chrono_tick_init(chrono_tick_counter_t *tc);

/* O(1), lock-free, any number of concurrent callers (fetch_add): advances
 * the counter by exactly 1 and returns the NEW value. */
uint64_t chrono_tick_advance(chrono_tick_counter_t *tc);

/* O(1) atomic read; may be stale by the time the caller observes it under
 * concurrent chrono_tick_advance() calls, same caveat as this project's
 * other live counters. */
uint64_t chrono_tick_current(const chrono_tick_counter_t *tc);

/*
 * O(1): true iff (chrono_tick_current(tc) - *last_fire_step) >= interval.
 * On true, ATOMICALLY sets *last_fire_step = the step value observed at
 * that instant before returning (one-shot: the very next call, absent
 * further chrono_tick_advance() calls, returns false) -- a compare-and-
 * swap loop, not a plain load-then-store, so two threads racing to check
 * the SAME schedule can never both observe true for the same interval
 * window. interval == 0 always returns true (fires on every call) and
 * still updates *last_fire_step, matching the literal "always due" edge
 * case rather than treating it as an error.
 */
bool chrono_tick_due(const chrono_tick_counter_t *tc, uint64_t interval, _Atomic uint64_t *last_fire_step);

#endif /* CHRONO_TICK_H */
