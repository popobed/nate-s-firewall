#include "chrono_tick.h"

void chrono_tick_init(chrono_tick_counter_t *tc) {
    atomic_store_explicit(&tc->step, 0u, memory_order_relaxed);
}

uint64_t chrono_tick_advance(chrono_tick_counter_t *tc) {
    return atomic_fetch_add_explicit(&tc->step, 1u, memory_order_relaxed) + 1u;
}

uint64_t chrono_tick_current(const chrono_tick_counter_t *tc) {
    return atomic_load_explicit((_Atomic uint64_t *)&tc->step, memory_order_relaxed);
}

bool chrono_tick_due(const chrono_tick_counter_t *tc, uint64_t interval, _Atomic uint64_t *last_fire_step) {
    uint64_t last = atomic_load_explicit(last_fire_step, memory_order_relaxed);
    for (;;) {
        uint64_t now = chrono_tick_current(tc);
        if (now - last < interval) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(last_fire_step, &last, now,
                                                    memory_order_relaxed, memory_order_relaxed)) {
            return true;
        }
        /* CAS lost the race against another thread checking the same
         * schedule: `last` was refreshed to its current value; retry the
         * interval check against it. */
    }
}
