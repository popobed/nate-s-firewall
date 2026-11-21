#include "signal_core.h"

#include <string.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

/* ================================================================== */
/* PCM capture ring: lock-free SPSC, no CAS needed (exactly one         */
/* writer, one reader) -- one slot sacrificed to distinguish full from  */
/* empty using only two indices, the classic SPSC ring technique.       */
/* ================================================================== */

static uint32_t sig_ring_next(uint32_t idx) {
    return (idx + 1u) & (SIG_PCM_CAPACITY - 1u);
}

void sig_pcm_ring_init(sig_pcm_ring_t *r) {
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    memset(r->samples, 0, sizeof r->samples);
}

size_t sig_pcm_push(sig_pcm_ring_t *r, const sig_sample_t *samples, size_t n) {
    size_t pushed = 0;
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);

    while (pushed < n) {
        uint32_t next = sig_ring_next(head);
        /* Acquire: synchronizes with the consumer's release-store to
         * `tail`, so we see its up-to-date position before deciding the
         * ring is full. */
        uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (next == tail) {
            break; /* full: deterministic backpressure, caller sees pushed < n */
        }
        r->samples[head] = samples[pushed];
        head = next;
        pushed++;
    }

    if (pushed != 0u) {
        /* Release: publishes both the new head and every sample written
         * above to the consumer thread. */
        atomic_store_explicit(&r->head, head, memory_order_release);
    }
    return pushed;
}

size_t sig_pcm_pop(sig_pcm_ring_t *r, sig_sample_t *out, size_t max_n) {
    size_t popped = 0;
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

    while (popped < max_n) {
        /* Acquire: synchronizes with the producer's release-store to
         * `head`, so we see the samples it just wrote. */
        uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);
        if (tail == head) {
            break; /* empty */
        }
        out[popped] = r->samples[tail];
        tail = sig_ring_next(tail);
        popped++;
    }

    if (popped != 0u) {
        atomic_store_explicit(&r->tail, tail, memory_order_release);
    }
    return popped;
}

size_t sig_pcm_count(const sig_pcm_ring_t *r) {
    uint32_t head = atomic_load_explicit((_Atomic uint32_t *)&r->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit((_Atomic uint32_t *)&r->tail, memory_order_acquire);
    return (size_t)((head - tail) & (SIG_PCM_CAPACITY - 1u));
}

bool sig_pcm_is_empty(const sig_pcm_ring_t *r) {
    return sig_pcm_count(r) == 0u;
}

/* ================================================================== */
/* FIR decimation filter                                                */
/* ================================================================== */

const int32_t SIG_FIR_COEFFS[SIG_FIR_TAPS] = {
    2048, 2048, 2048, 2048,
    2048, 2048, 2048, 2048,
    2048, 2048, 2048, 2048,
    2048, 2048, 2048, 2048
}; /* Q15 boxcar low-pass, unity DC gain: 16 * 2048 == 32768 == 2^15 exactly */

void sig_fir_state_init(sig_fir_state_t *st) {
    memset(st->history, 0, sizeof st->history);
    st->decim_phase = 0u;
}

static void sig_fir_push_sample(sig_fir_state_t *st, sig_sample_t sample) {
    /* TAPS is small (16): O(TAPS) per sample, same order as the
     * convolution itself -- shifting keeps the convolution loop below
     * free of any modular-index arithmetic. */
    for (uint32_t i = 0; i + 1u < SIG_FIR_TAPS; i++) {
        st->history[i] = st->history[i + 1u];
    }
    st->history[SIG_FIR_TAPS - 1u] = sample;
}

static sig_sample_t sig_fir_apply(const sig_fir_state_t *st) {
    int64_t acc = 0;
    /* Explicit 4x unroll: SIG_FIR_TAPS (16) is a compile-time multiple of
     * 4, so this is exact -- no remainder handling. Fixed trip count, no
     * data-dependent branch: -O3 auto-vectorizes this into whatever SIMD
     * width the build target provides. */
    for (uint32_t k = 0; k < SIG_FIR_TAPS; k += 4u) {
        acc += (int64_t)SIG_FIR_COEFFS[k + 0u] * (int64_t)st->history[k + 0u];
        acc += (int64_t)SIG_FIR_COEFFS[k + 1u] * (int64_t)st->history[k + 1u];
        acc += (int64_t)SIG_FIR_COEFFS[k + 2u] * (int64_t)st->history[k + 2u];
        acc += (int64_t)SIG_FIR_COEFFS[k + 3u] * (int64_t)st->history[k + 3u];
    }
    int64_t scaled = acc >> 15; /* Q15 -> integer; arithmetic (sign-extending) shift, standard fixed-point practice */
    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
    }
    return (sig_sample_t)scaled;
}

size_t sig_fir_decimate(sig_fir_state_t *st, const sig_sample_t *in, size_t n_in,
                         sig_sample_t *out, size_t out_cap) {
    size_t out_count = 0;
    for (size_t i = 0; i < n_in; i++) {
        sig_fir_push_sample(st, in[i]);
        st->decim_phase = (st->decim_phase + 1u) & (SIG_DECIMATION_FACTOR - 1u);
        if (st->decim_phase == 0u) {
            if (out_count >= out_cap) {
                break; /* bounded, deterministic: stop rather than overflow the caller's buffer */
            }
            out[out_count++] = sig_fir_apply(st);
        }
    }
    return out_count;
}

/* ================================================================== */
/* SRAM-style INT8 activation scratchpad + deterministic token decode   */
/* ================================================================== */

_Alignas(64) static int8_t g_sig_weights[SIG_TOKEN_VOCAB][SIG_ACT_DIM];

/* Deterministic, fixed-seed weight generation -- see signal_core.h: NOT a
 * trained acoustic model, reproducible on purpose (never random). */
static uint64_t sig_splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void sig_sram_init(void) {
    uint64_t seed = 0x5349474E414C3F31ULL; /* fixed, arbitrary constant */
    for (uint32_t v = 0; v < SIG_TOKEN_VOCAB; v++) {
        for (uint32_t d = 0; d < SIG_ACT_DIM; d++) {
            g_sig_weights[v][d] = (int8_t)(sig_splitmix64_next(&seed) & 0xFFu);
        }
    }
}

void sig_sram_fold(sig_sram_t *sram, const sig_sample_t *decimated, size_t n) {
    if (n == 0u) {
        memset(sram->activation, 0, sizeof sram->activation);
        return;
    }
    for (uint32_t d = 0; d < SIG_ACT_DIM; d++) {
        size_t lo = ((size_t)d * n) / SIG_ACT_DIM;
        size_t hi = ((size_t)(d + 1u) * n) / SIG_ACT_DIM;
        if (hi <= lo) {
            hi = lo + 1u;
            if (hi > n) {
                hi = n;
            }
        }
        int64_t sum = 0;
        for (size_t i = lo; i < hi; i++) {
            sum += decimated[i];
        }
        size_t count = hi - lo;
        int64_t avg = (count != 0u) ? (sum / (int64_t)count) : 0;
        int64_t scaled = avg >> 8; /* rescale int16 sample range down to int8, sign-preserving */
        if (scaled > 127) {
            scaled = 127;
        } else if (scaled < -128) {
            scaled = -128;
        }
        sram->activation[d] = (int8_t)scaled;
    }
}

uint8_t sig_infer_token(const sig_sram_t *sram) {
    int32_t best_score = INT32_MIN;
    uint32_t best_v = 0u;
    for (uint32_t v = 0; v < SIG_TOKEN_VOCAB; v++) {
        int32_t acc = 0;
        for (uint32_t d = 0; d < SIG_ACT_DIM; d++) {
            acc += (int32_t)sram->activation[d] * (int32_t)g_sig_weights[v][d];
        }
        if (acc > best_score) {
            best_score = acc;
            best_v = v;
        }
    }
    return (uint8_t)best_v;
}

actor_status_t sig_emit_token(actor_t *target, uint16_t tenant_id, uint8_t token) {
    if (target == NULL) {
        return ACTOR_ERR_INVALID;
    }
    ring_msg_t *slot;
    actor_status_t rst = actor_mailbox_reserve(target, &slot);
    if (rst != ACTOR_OK) {
        return rst;
    }
    /* Zero-copy: the token is written directly into the reserved slot's
     * own payload buffer -- no intermediate buffer anywhere. */
    slot->payload[0] = token;
    memset(slot->payload + 1, 0, RING_MSG_PAYLOAD_SIZE - 1u);
    actor_mailbox_publish(target, slot, tenant_id, SIG_MSG_TYPE_TOKEN);
    return ACTOR_OK;
}

/* ================================================================== */
/* CPUID-based feature detection -- reporting only, see signal_core.h   */
/* ================================================================== */

sig_cpu_features_t sig_detect_cpu_features(void) {
    sig_cpu_features_t f;
    memset(&f, 0, sizeof f);

#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    unsigned int max_leaf = __get_cpuid_max(0, NULL);

    if (max_leaf >= 1u) {
        __get_cpuid(1, &eax, &ebx, &ecx, &edx);
        f.sse2 = (edx & (1u << 26)) != 0u;
    }
    if (max_leaf >= 7u) {
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
        f.avx2 = (ebx & (1u << 5)) != 0u;
        f.avx512f = (ebx & (1u << 16)) != 0u;
        f.amx_tile = (edx & (1u << 24)) != 0u;
    }
    /* eax/ecx are required output parameters of __get_cpuid[_count]() but
     * their values are not needed here -- only ebx/edx feature bits are. */
    (void)eax;
    (void)ecx;
#elif defined(__aarch64__)
    f.neon = true; /* NEON is mandatory in the AArch64 base architecture: an architectural fact, not a probe */
#endif

    return f;
}
