/*
 * signal_tracking.h
 *
 * Niveaux 51-52 -- RF I/Q noise-cleaning filter bank and a fixed-point
 * Q21.10 3D kinetic trajectory tracker. Static, zero-malloc-after-init,
 * pure integer arithmetic throughout (no float anywhere in the hot
 * path) -- a standalone leaf module, no dependency on the rest of this
 * project's modules.
 *
 * ============================== Honesty notes, up front =====================
 *   - Niveau 51 ("dereceveur radiofrequence... deconvolution statistique
 *     du bruit de phase") : this project owns no RF/SDR hardware driver
 *     -- there is no real receiver to acquire from, the same posture
 *     audio_transcribe.h already takes for its "capteur audio local"
 *     (a caller-filled PCM buffer, not a real microphone driver).
 *     `iq_filter_sample()` operates on caller-supplied, already-
 *     digitized I/Q sample PAIRS (as if handed off by an ADC/SDR
 *     front-end this codebase does not own). What it does to them is
 *     real, working, and precisely NOT what "deconvolution" means in
 *     the strict signal-processing sense: true blind/Wiener
 *     deconvolution needs a known or estimated channel impulse response
 *     and frequency-domain (FFT) processing -- no FFT exists anywhere in
 *     this codebase, and building one now, from scratch, to satisfy a
 *     naming choice would be exactly the kind of unvetted, undertested
 *     new primitive this project's own crypto_constant_time.h and
 *     infra_mesh.h already refuse to introduce for cryptography. What IS
 *     real here: a single-pole IIR recursive smoothing filter on I and Q
 *     independently (a genuine, standard phase-noise-reduction
 *     technique, structurally the time-domain sibling of
 *     audio_transcribe.c's FIR filter -- IIR here because it is the
 *     right, honest tool for continuous per-sample smoothing with O(1)
 *     state, not a stylistic swap) PLUS magnitude-based statistical
 *     outlier gating (a running mean of magnitude-squared; a sample
 *     whose magnitude-squared exceeds a fixed multiple of that mean is
 *     rejected and replaced by the current filtered estimate rather than
 *     allowed to poison the filter state or the running statistics). All
 *     of it is real integer/fixed-point arithmetic, O(1) per sample,
 *     zero allocation, ever.
 *   - Niveau 52 (3D kinetic tracking, Q21.10 fixed point, "trajectoires
 *     a partir de donnees incompletes") : real and working, precisely
 *     scoped as linear dead-reckoning, NOT a Kalman filter. A full
 *     Kalman filter needs matrix inversion and a covariance model --
 *     genuinely more machinery than "data occasionally missing" needs
 *     to be handled honestly, and this codebase has no linear-algebra
 *     library to build one on safely. What IS real: `track_update_position()`
 *     DERIVES a velocity estimate from two position samples and the
 *     opaque tick delta between them (this is precisely "trajectoires a
 *     partir de donnees de signalisation incompletes": only positions
 *     ever arrive, velocity is inferred, never itself an input), and
 *     `track_predict_position()` linearly extrapolates position from the
 *     last known position/velocity for any requested tick -- exact,
 *     deterministic fixed-point arithmetic, the same "no wall clock,
 *     caller-supplied opaque tick" discipline as swarm_tick everywhere
 *     else in this project. Q21.10 (1 sign + 21 integer + 10 fractional
 *     bits in a plain int32_t) is used throughout instead of float,
 *     precisely to make every computation bit-reproducible across
 *     platforms/compilers/optimization levels -- the actual, concrete
 *     meaning of "eliminant l'indeterminisme des arrondis flottants"
 *     here, not a vague claim.
 */

#ifndef SIGNAL_TRACKING_H
#define SIGNAL_TRACKING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIGNAL_TRACKING_CACHE_LINE_SIZE 64u

typedef enum {
    SIGTRACK_OK = 0,
    SIGTRACK_ERR_INVALID      = -1,
    SIGTRACK_ERR_OUT_OF_RANGE = -2,
    SIGTRACK_ERR_NOT_FOUND    = -3,
    SIGTRACK_ERR_OVERFLOW     = -4 /* a Q21.10 result would not fit in int32_t -- see Part C */
} sigtrack_status_t;

/* ==================================================================== */
/* Part A -- I/Q noise-cleaning filter bank (Niveau 51)                  */
/* ==================================================================== */

#define IQ_MAX_CHANNELS       32u  /* one per receiver/tenant, direct-indexed: O(1) */
#define IQ_FILTER_ALPHA_SHIFT 3u   /* single-pole IIR smoothing factor alpha = 1/2^3 = 0.125, fixed-point, zero float */
#define IQ_OUTLIER_FACTOR     4u   /* a sample is an outlier if magnitude^2 > mean_magnitude^2 * (1 + IQ_OUTLIER_FACTOR) */

typedef struct {
    int16_t i;
    int16_t q;
} iq_sample_t;

typedef struct {
    int32_t  i_filtered_q8;    /* Q8 fixed-point (scale 256) smoothed I */
    int32_t  q_filtered_q8;    /* Q8 fixed-point smoothed Q */
    uint64_t mag_sq_mean_q8;   /* running mean of magnitude^2, Q8 fixed-point, non-outlier samples only */
    uint32_t samples_processed;
    uint32_t outliers_rejected;
    bool     initialized;      /* false until the first sample seeds the filter/statistics */
} iq_channel_state_t;

typedef struct {
    iq_channel_state_t channels[IQ_MAX_CHANNELS]; /* every channel exists from process start: zero allocation, ever */
} iq_filter_bank_t;

void iq_filter_bank_init(iq_filter_bank_t *bank);

/*
 * O(1) per sample, fixed compile-time cost, zero allocation. Single-
 * writer-per-channel (any number of concurrent callers across DIFFERENT
 * channel_id values; concurrent calls on the SAME channel_id race each
 * other's state, the same single-consumer-per-stream discipline
 * ring_buffer.h already applies on its consumer side -- one acquisition
 * thread per RF channel is the intended real-world shape here).
 *
 * First seeds the filter/statistics from the very first sample (never
 * flagged as an outlier -- there is no history yet to compare against).
 * Afterwards: computes magnitude^2 = i^2+q^2; if it exceeds the running
 * mean by more than IQ_OUTLIER_FACTOR, the sample is REJECTED
 * (outliers_rejected++, *out_was_outlier = true if non-NULL) and
 * *out_clean is set to the current filtered estimate instead of the raw
 * (statistically implausible) sample, without updating the filter state
 * or statistics from it. Otherwise, applies the single-pole IIR update
 * to I and Q and to the running magnitude^2 mean, and *out_clean is the
 * newly filtered I/Q pair.
 */
sigtrack_status_t iq_filter_sample(iq_filter_bank_t *bank, uint32_t channel_id, iq_sample_t sample,
                                    iq_sample_t *out_clean, bool *out_was_outlier);

/*
 * Batch entry point: filters ALL IQ_MAX_CHANNELS in one call, one sample
 * per channel, `samples[k]` / `out_clean[k]` / `out_was_outlier[k]`
 * corresponding to channel_id == k. Internally unrolled in strides of 4
 * (IQ_MAX_CHANNELS == 32 is an exact multiple of 4, so there is no
 * remainder tail to special-case).
 *
 * Honest scope on "vectorization": the single-pole IIR recursion inside
 * iq_filter_sample() is a genuine sequential dependency ACROSS TIME for
 * one channel (filtered[n] depends on filtered[n-1]) -- unrolling that
 * axis would not be valid vectorization, it would be a lie. What IS
 * embarrassingly parallel is the CHANNEL axis: every channel_id owns its
 * own iq_channel_state_t, so four consecutive channels processed in the
 * same unrolled iteration touch four disjoint cache lines with zero
 * loop-carried dependency between them -- exactly the "N independent
 * channels across N lanes" pattern this codebase's own
 * audio_transcribe.c Goertzel filterbank already established (see
 * SESSION_NOTES.md Sec. 4.2), applied here to whichever receiver/tenant
 * channels this codebase already has (IQ_MAX_CHANNELS), not a new SIMD
 * kernel. This gives the compiler four independent, alignment-friendly
 * iq_filter_sample() bodies per unrolled iteration to schedule/vectorize
 * at -O3 -- a real, inspectable opportunity, not a guaranteed instruction
 * count (this project makes no unverifiable claims about what a specific
 * compiler backend chooses to emit).
 */
void iq_filter_bank_process_all(iq_filter_bank_t *bank,
                                 const iq_sample_t samples[IQ_MAX_CHANNELS],
                                 iq_sample_t out_clean[IQ_MAX_CHANNELS],
                                 bool out_was_outlier[IQ_MAX_CHANNELS]);

/* ==================================================================== */
/* Part B -- Q21.10 fixed-point arithmetic                               */
/* ==================================================================== */

/* Q21.10: a plain int32_t, 1 sign bit + 21 integer bits + 10 fractional
 * bits. Range: approx. +/-2,097,151.999. Chosen so every kinematic
 * computation below is exact, deterministic integer arithmetic -- no
 * float anywhere in this file's hot path. */
typedef int32_t fixed_q21_10_t;

#define FIXED_Q21_10_SHIFT 10
#define FIXED_Q21_10_ONE   (1 << FIXED_Q21_10_SHIFT)

static inline fixed_q21_10_t fixed_from_int(int32_t v) {
    return (fixed_q21_10_t)(v << FIXED_Q21_10_SHIFT);
}
static inline int32_t fixed_to_int_trunc(fixed_q21_10_t v) {
    return v >> FIXED_Q21_10_SHIFT;
}
/* 64-bit intermediate: the product of two Q21.10 values needs up to 64
 * bits before rescaling back down, or it silently overflows int32_t. */
static inline fixed_q21_10_t fixed_mul(fixed_q21_10_t a, fixed_q21_10_t b) {
    return (fixed_q21_10_t)(((int64_t)a * (int64_t)b) >> FIXED_Q21_10_SHIFT);
}

/* ==================================================================== */
/* Part C -- 3D kinetic trajectory tracker, Q21.10 SoA (Niveau 52)       */
/* ==================================================================== */

#define TRACK_MAX_TARGETS 64u /* direct-indexed by target_id: O(1) */

/* Structure of Arrays, one `_Alignas(64)` array per column -- the exact
 * pattern infra_mesh.h's `lb_pool_t` and brain_core.h's
 * `brain_billing_ledger_t` already established: a writer hammering one
 * target's position never invalidates a reader's cache line for a
 * DIFFERENT column that changes far less often. */
typedef struct {
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) fixed_q21_10_t pos_x[TRACK_MAX_TARGETS];
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) fixed_q21_10_t pos_y[TRACK_MAX_TARGETS];
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) fixed_q21_10_t pos_z[TRACK_MAX_TARGETS];
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) fixed_q21_10_t vel_x[TRACK_MAX_TARGETS]; /* distance-per-opaque-tick, Q21.10 */
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) fixed_q21_10_t vel_y[TRACK_MAX_TARGETS];
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) fixed_q21_10_t vel_z[TRACK_MAX_TARGETS];
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) uint64_t       last_update_cycle[TRACK_MAX_TARGETS]; /* opaque, caller-supplied tick -- never a wall clock, same posture as swarm_tick */
    _Alignas(SIGNAL_TRACKING_CACHE_LINE_SIZE) bool           active[TRACK_MAX_TARGETS];
} track_registry_t;

void track_registry_init(track_registry_t *reg);

/*
 * O(1), direct-indexed by target_id. On the FIRST update for a
 * target_id, seeds its position (velocity starts at zero -- there is no
 * prior sample to derive it from). On every SUBSEQUENT update, first
 * DERIVES a velocity estimate from the delta between the new position
 * and the previously stored one, divided by the elapsed opaque-tick
 * delta (dt = current_cycle - last_update_cycle) -- this is the file
 * header's "trajectoires a partir de donnees incompletes": velocity is
 * never an input, only ever inferred from consecutive position reports,
 * exactly matching real signaling data that reports positions
 * intermittently, never velocity directly. dt == 0 (two updates stamped
 * with the same opaque tick) leaves the existing velocity estimate
 * unchanged rather than dividing by zero. current_cycle going backwards
 * relative to the stored last_update_cycle is rejected as
 * SIGTRACK_ERR_INVALID (a caller bug -- ticks must be non-decreasing per
 * target, the same "caller-driven, in order" discipline
 * brain_core.h's cron wheel already requires of its own ticks).
 *
 * Overflow: the derived velocity is checked against int32_t range (the
 * plain integer backing fixed_q21_10_t) BEFORE anything is written --
 * either all three axes fit, and pos_x/y/z + vel_x/y/z +
 * last_update_cycle are updated together, or none of reg's state changes
 * and SIGTRACK_ERR_OVERFLOW is returned. This is the "overflow ou
 * corruption detectee" signal self_replicate.h's Part E
 * (self_replicate_couple_signal_anomaly()) escalates to
 * agn_predictive_auto_heal()'s memory erasure -- see that header.
 */
sigtrack_status_t track_update_position(track_registry_t *reg, uint32_t target_id,
                                         fixed_q21_10_t x, fixed_q21_10_t y, fixed_q21_10_t z,
                                         uint64_t current_cycle);

/*
 * O(1): linearly extrapolates target_id's position to `current_cycle`
 * from its last known position and derived velocity:
 * predicted = pos + vel * (current_cycle - last_update_cycle). Exact,
 * deterministic Q21.10 fixed-point arithmetic (64-bit intermediate
 * products, no overflow for any dt/velocity combination this file's
 * types can represent). Passing the SAME current_cycle as the last
 * update returns exactly the last known position (dt == 0). Passing an
 * EARLIER cycle than the last update still computes the (now negative
 * dt) linear extrapolation backwards -- a valid, well-defined
 * computation, not an error: reconstructing where a target likely WAS
 * between two known reports is as legitimate a use as predicting ahead.
 * SIGTRACK_ERR_NOT_FOUND if target_id was never updated.
 *
 * Overflow: each axis's 64-bit extrapolation is range-checked against
 * int32_t before any `out_*` is written -- on overflow, ALL THREE out
 * parameters are left untouched and SIGTRACK_ERR_OVERFLOW is returned
 * (never a partially-written result on two axes and garbage on the
 * third). Same self-heal escalation path as track_update_position()
 * above.
 */
sigtrack_status_t track_predict_position(const track_registry_t *reg, uint32_t target_id, uint64_t current_cycle,
                                          fixed_q21_10_t *out_x, fixed_q21_10_t *out_y, fixed_q21_10_t *out_z);

#endif /* SIGNAL_TRACKING_H */
