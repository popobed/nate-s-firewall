/*
 * signal_tracking.c
 *
 * See include/signal_tracking.h for the full design and honest-scope
 * notes. Implementation only; non-obvious decisions are explained there,
 * not duplicated here.
 */

#include "signal_tracking.h"

#include <limits.h>

/* ==================================================================== */
/* Part A -- I/Q noise-cleaning filter bank                              */
/* ==================================================================== */

void iq_filter_bank_init(iq_filter_bank_t *bank) {
    for (uint32_t i = 0; i < IQ_MAX_CHANNELS; i++) {
        iq_channel_state_t *ch = &bank->channels[i];
        ch->i_filtered_q8 = 0;
        ch->q_filtered_q8 = 0;
        ch->mag_sq_mean_q8 = 0u;
        ch->samples_processed = 0u;
        ch->outliers_rejected = 0u;
        ch->initialized = false;
    }
}

sigtrack_status_t iq_filter_sample(iq_filter_bank_t *bank, uint32_t channel_id, iq_sample_t sample,
                                    iq_sample_t *out_clean, bool *out_was_outlier) {
    if (bank == NULL || out_clean == NULL) {
        return SIGTRACK_ERR_INVALID;
    }
    if (channel_id >= IQ_MAX_CHANNELS) {
        return SIGTRACK_ERR_OUT_OF_RANGE;
    }
    iq_channel_state_t *ch = &bank->channels[channel_id];

    int64_t mag_sq = (int64_t)sample.i * (int64_t)sample.i + (int64_t)sample.q * (int64_t)sample.q;

    if (!ch->initialized) {
        /* Seed the filter and statistics from the very first sample --
         * there is no history yet to judge it an outlier against. */
        ch->i_filtered_q8 = (int32_t)sample.i << 8;
        ch->q_filtered_q8 = (int32_t)sample.q << 8;
        ch->mag_sq_mean_q8 = (uint64_t)mag_sq << 8;
        ch->initialized = true;
        ch->samples_processed = 1u;
        out_clean->i = sample.i;
        out_clean->q = sample.q;
        if (out_was_outlier != NULL) {
            *out_was_outlier = false;
        }
        return SIGTRACK_OK;
    }

    uint64_t mag_sq_q8 = (uint64_t)mag_sq << 8;
    uint64_t threshold_q8 = ch->mag_sq_mean_q8 * (uint64_t)(1u + IQ_OUTLIER_FACTOR);
    bool is_outlier = mag_sq_q8 > threshold_q8;

    if (is_outlier) {
        ch->outliers_rejected++;
        out_clean->i = (int16_t)(ch->i_filtered_q8 >> 8);
        out_clean->q = (int16_t)(ch->q_filtered_q8 >> 8);
    } else {
        /* Single-pole IIR: filtered += (sample - filtered) >> ALPHA_SHIFT,
         * applied independently to I and Q. */
        int32_t i_q8 = (int32_t)sample.i << 8;
        int32_t q_q8 = (int32_t)sample.q << 8;
        ch->i_filtered_q8 += (i_q8 - ch->i_filtered_q8) >> IQ_FILTER_ALPHA_SHIFT;
        ch->q_filtered_q8 += (q_q8 - ch->q_filtered_q8) >> IQ_FILTER_ALPHA_SHIFT;

        /* The running magnitude^2 mean updates the same way -- only ever
         * from non-outlier samples, so a rejected outlier never poisons
         * the very statistic used to detect the next one. */
        int64_t diff = (int64_t)mag_sq_q8 - (int64_t)ch->mag_sq_mean_q8;
        ch->mag_sq_mean_q8 = (uint64_t)((int64_t)ch->mag_sq_mean_q8 + (diff >> IQ_FILTER_ALPHA_SHIFT));

        out_clean->i = (int16_t)(ch->i_filtered_q8 >> 8);
        out_clean->q = (int16_t)(ch->q_filtered_q8 >> 8);
    }

    ch->samples_processed++;
    if (out_was_outlier != NULL) {
        *out_was_outlier = is_outlier;
    }
    return SIGTRACK_OK;
}

void iq_filter_bank_process_all(iq_filter_bank_t *bank,
                                 const iq_sample_t samples[IQ_MAX_CHANNELS],
                                 iq_sample_t out_clean[IQ_MAX_CHANNELS],
                                 bool out_was_outlier[IQ_MAX_CHANNELS]) {
    /* IQ_MAX_CHANNELS (32) is a compile-time multiple of 4 -- no
     * remainder tail loop needed. Four independent iq_filter_sample()
     * calls per iteration, each on its own channel_id (own cache lines,
     * own recursion state): see the header for why this is the
     * channel axis, never the time axis, being unrolled. */
    for (uint32_t base = 0; base < IQ_MAX_CHANNELS; base += 4u) {
        (void)iq_filter_sample(bank, base + 0u, samples[base + 0u], &out_clean[base + 0u], &out_was_outlier[base + 0u]);
        (void)iq_filter_sample(bank, base + 1u, samples[base + 1u], &out_clean[base + 1u], &out_was_outlier[base + 1u]);
        (void)iq_filter_sample(bank, base + 2u, samples[base + 2u], &out_clean[base + 2u], &out_was_outlier[base + 2u]);
        (void)iq_filter_sample(bank, base + 3u, samples[base + 3u], &out_clean[base + 3u], &out_was_outlier[base + 3u]);
    }
}

/* ==================================================================== */
/* Part C -- 3D kinetic trajectory tracker                                */
/* ==================================================================== */

void track_registry_init(track_registry_t *reg) {
    for (uint32_t i = 0; i < TRACK_MAX_TARGETS; i++) {
        reg->pos_x[i] = 0;
        reg->pos_y[i] = 0;
        reg->pos_z[i] = 0;
        reg->vel_x[i] = 0;
        reg->vel_y[i] = 0;
        reg->vel_z[i] = 0;
        reg->last_update_cycle[i] = 0u;
        reg->active[i] = false;
    }
}

sigtrack_status_t track_update_position(track_registry_t *reg, uint32_t target_id,
                                         fixed_q21_10_t x, fixed_q21_10_t y, fixed_q21_10_t z,
                                         uint64_t current_cycle) {
    if (reg == NULL) {
        return SIGTRACK_ERR_INVALID;
    }
    if (target_id >= TRACK_MAX_TARGETS) {
        return SIGTRACK_ERR_OUT_OF_RANGE;
    }

    if (!reg->active[target_id]) {
        /* First report for this target: seed its position, velocity
         * starts at zero (nothing to derive it from yet). */
        reg->pos_x[target_id] = x;
        reg->pos_y[target_id] = y;
        reg->pos_z[target_id] = z;
        reg->vel_x[target_id] = 0;
        reg->vel_y[target_id] = 0;
        reg->vel_z[target_id] = 0;
        reg->last_update_cycle[target_id] = current_cycle;
        reg->active[target_id] = true;
        return SIGTRACK_OK;
    }

    uint64_t prev_cycle = reg->last_update_cycle[target_id];
    if (current_cycle < prev_cycle) {
        return SIGTRACK_ERR_INVALID; /* opaque ticks must be non-decreasing per target */
    }

    uint64_t dt = current_cycle - prev_cycle;
    fixed_q21_10_t new_vel_x = reg->vel_x[target_id];
    fixed_q21_10_t new_vel_y = reg->vel_y[target_id];
    fixed_q21_10_t new_vel_z = reg->vel_z[target_id];
    if (dt > 0u) {
        /* velocity (Q21.10, distance per opaque tick) = delta_position
         * (already Q21.10) / dt -- dt is a plain, dimensionless integer
         * tick count, so dividing the fixed-point delta by it directly
         * yields a correctly-scaled fixed-point velocity, no rescale
         * needed. Integer division truncates toward zero, deterministic
         * and reproducible -- never float rounding.
         *
         * The subtraction itself is done in int64_t, not fixed_q21_10_t
         * (== int32_t): two opposite-extreme int32_t positions (e.g. x
         * == INT32_MAX, stored pos_x == INT32_MIN) subtract to a value
         * that does not fit in int32_t -- computing that in int32_t
         * would be signed-integer-overflow undefined behavior, not just
         * an inconvenient result. int64_t has ample headroom for the
         * difference of any two int32_t values. */
        int64_t dx64 = (int64_t)x - (int64_t)reg->pos_x[target_id];
        int64_t dy64 = (int64_t)y - (int64_t)reg->pos_y[target_id];
        int64_t dz64 = (int64_t)z - (int64_t)reg->pos_z[target_id];
        int64_t vx64 = dx64 / (int64_t)dt;
        int64_t vy64 = dy64 / (int64_t)dt;
        int64_t vz64 = dz64 / (int64_t)dt;

        /* All-or-nothing range check against int32_t (fixed_q21_10_t's
         * plain integer backing) BEFORE any write to reg -- an overflow
         * on any single axis must never leave the other two axes updated
         * and the third silently truncated. */
        if (vx64 > INT32_MAX || vx64 < INT32_MIN ||
            vy64 > INT32_MAX || vy64 < INT32_MIN ||
            vz64 > INT32_MAX || vz64 < INT32_MIN) {
            return SIGTRACK_ERR_OVERFLOW;
        }
        new_vel_x = (fixed_q21_10_t)vx64;
        new_vel_y = (fixed_q21_10_t)vy64;
        new_vel_z = (fixed_q21_10_t)vz64;
    }
    /* dt == 0 (two reports stamped with the same opaque tick): leave the
     * existing velocity estimate unchanged, per the header's contract. */

    reg->vel_x[target_id] = new_vel_x;
    reg->vel_y[target_id] = new_vel_y;
    reg->vel_z[target_id] = new_vel_z;
    reg->pos_x[target_id] = x;
    reg->pos_y[target_id] = y;
    reg->pos_z[target_id] = z;
    reg->last_update_cycle[target_id] = current_cycle;
    return SIGTRACK_OK;
}

sigtrack_status_t track_predict_position(const track_registry_t *reg, uint32_t target_id, uint64_t current_cycle,
                                          fixed_q21_10_t *out_x, fixed_q21_10_t *out_y, fixed_q21_10_t *out_z) {
    if (reg == NULL || out_x == NULL || out_y == NULL || out_z == NULL) {
        return SIGTRACK_ERR_INVALID;
    }
    if (target_id >= TRACK_MAX_TARGETS) {
        return SIGTRACK_ERR_OUT_OF_RANGE;
    }
    if (!reg->active[target_id]) {
        return SIGTRACK_ERR_NOT_FOUND;
    }

    /* May be negative (a request for an earlier cycle than the last
     * update) -- a valid, well-defined backward extrapolation, not an
     * error; see the header. */
    int64_t dt = (int64_t)current_cycle - (int64_t)reg->last_update_cycle[target_id];

    int64_t px64 = (int64_t)reg->pos_x[target_id] + (int64_t)reg->vel_x[target_id] * dt;
    int64_t py64 = (int64_t)reg->pos_y[target_id] + (int64_t)reg->vel_y[target_id] * dt;
    int64_t pz64 = (int64_t)reg->pos_z[target_id] + (int64_t)reg->vel_z[target_id] * dt;

    /* All-or-nothing: an overflow on any single axis must never leave
     * out_x/out_y written and out_z holding indeterminate stack garbage
     * from the caller's uninitialized local. */
    if (px64 > INT32_MAX || px64 < INT32_MIN ||
        py64 > INT32_MAX || py64 < INT32_MIN ||
        pz64 > INT32_MAX || pz64 < INT32_MIN) {
        return SIGTRACK_ERR_OVERFLOW;
    }

    *out_x = (fixed_q21_10_t)px64;
    *out_y = (fixed_q21_10_t)py64;
    *out_z = (fixed_q21_10_t)pz64;
    return SIGTRACK_OK;
}
