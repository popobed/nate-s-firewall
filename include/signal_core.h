/*
 * signal_core.h
 *
 * Signal acquisition + deterministic decimation + zero-copy token
 * emission into an actor_core.h mailbox.
 *
 * ============================ Honest scope ================================
 * "En francais" in the originating request does not change anything at
 * this layer: raw PCM capture and FIR decimation operate on waveforms,
 * not language. There is no speech-to-text model here. sig_infer_token()
 * is a DEMONSTRATION pipeline: its weight table is generated from a fixed
 * seed (deterministic, reproducible, never random) precisely so this
 * module can be tested and is honest about not being a trained acoustic
 * model of any language. It exists to exercise the real, load-bearing
 * parts of this request -- static SRAM-style activation storage, O(1)/
 * bounded-step token selection, and zero-copy delivery into an actor's
 * mailbox -- not to transcribe anything.
 *
 * What this file deliberately does NOT implement: a control-flow-graph
 * analyzer, a disassembler, or "polymorphic opcode" emission into the L1
 * instruction cache. Generating machine code at runtime specifically to
 * change a program's binary signature is, regardless of the framing
 * ("resilience", "efficiency"), the defining technique of a metamorphic/
 * polymorphic code engine -- the core evasion primitive of malware
 * designed to defeat signature-based detection. That does not become
 * something else by request. What IS implemented instead, honestly
 * covering the legitimate part of the same ask, is real CPUID-based
 * feature detection (sig_detect_cpu_features()) with no code generation
 * of any kind: it reports what the silicon supports so a caller could
 * dispatch between separately-*compiled* kernels (the pattern used
 * elsewhere in this project's sibling codebase), never write new
 * instructions into memory at runtime.
 *
 * ============================ Design ========================================
 * 1. sig_pcm_ring_t: a lock-free SPSC (one capture source, one DSP
 *    consumer) circular sample buffer, `_Alignas(64)`, entirely static
 *    storage -- the classic "sacrifice one slot to tell full from empty"
 *    technique, requiring no CAS at all (unlike this project's MPSC rings)
 *    since there is exactly one writer and one reader.
 *
 * 2. sig_fir_state_t: a stateful (i.e. correct across repeated calls on a
 *    stream, not just within one buffer) FIR low-pass + decimation
 *    filter. The convolution loop is explicitly unrolled by 4 (16 taps =
 *    4 groups of 4, no remainder handling needed) -- a fixed-trip-count,
 *    branch-free body that -O3 auto-vectorizes into whatever SIMD width
 *    the build target provides (AVX2/NEON) without hand-written
 *    intrinsics for a kernel this small; see net_gateway.h's sibling
 *    project (adaptive_engine.c) for the pattern of separately-compiled,
 *    explicitly-dispatched ISA-specific kernels, the natural next step if
 *    a dedicated -mavx2 translation unit is ever added for this one.
 *    The filter itself is a basic boxcar (moving-average) low-pass --
 *    sufficient to demonstrate correct streaming/decimation mechanics,
 *    not tuned for real acoustic stopband performance.
 *
 * 3. sig_sram_t: a `_Alignas(64)` INT8 activation scratchpad ("interdire
 *    la derive de phase": fixed static storage, never reallocated or
 *    moved, so its address and layout are stable for the process's
 *    lifetime). sig_infer_token() is a fixed VOCAB x DIM dot-product +
 *    argmax over that scratchpad: the same SIG_TOKEN_VOCAB * SIG_ACT_DIM
 *    multiply-adds run every call regardless of the INPUT VALUES -- the
 *    precise, bounded sense in which this decode step is constant-time
 *    throughout this project (see msg_router.h, crypto_constant_time.h).
 *
 * 4. sig_emit_token(): zero-copy in the same sense used throughout this
 *    project -- the token byte is written directly into a reserved
 *    ring_msg_t slot's payload (actor_mailbox_reserve()/_publish(),
 *    actor_core.h), never copied through an intermediate buffer.
 *
 * Zero dynamic allocation, ever: every struct here embeds its storage
 * directly.
 */

#ifndef SIGNAL_CORE_H
#define SIGNAL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "actor_core.h"

#define SIG_CACHE_LINE_SIZE 64u

/* ------------------------------------------------------------------ */
/* PCM capture ring (lock-free SPSC)                                   */
/* ------------------------------------------------------------------ */

#define SIG_PCM_CAPACITY 4096u /* power of two; one slot is sacrificed (full/empty distinction) */

_Static_assert(SIG_PCM_CAPACITY != 0u && (SIG_PCM_CAPACITY & (SIG_PCM_CAPACITY - 1u)) == 0u,
               "SIG_PCM_CAPACITY must be a power of two");

typedef int16_t sig_sample_t; /* linear PCM, 16-bit signed -- the common raw capture format */

typedef struct {
    _Alignas(SIG_CACHE_LINE_SIZE)
    _Atomic uint32_t head; /* producer-owned (capture) */
    uint8_t _pad_head[SIG_CACHE_LINE_SIZE - sizeof(_Atomic uint32_t)];

    _Alignas(SIG_CACHE_LINE_SIZE)
    _Atomic uint32_t tail; /* consumer-owned (DSP pipeline) */
    uint8_t _pad_tail[SIG_CACHE_LINE_SIZE - sizeof(_Atomic uint32_t)];

    _Alignas(SIG_CACHE_LINE_SIZE)
    sig_sample_t samples[SIG_PCM_CAPACITY]; /* embedded: zero dynamic allocation, ever */
} sig_pcm_ring_t;

void sig_pcm_ring_init(sig_pcm_ring_t *r);

/* Producer side (one capture source). Pushes as many of `n` samples as
 * currently fit; returns the count actually pushed (< n means the ring
 * filled up mid-write -- deterministic backpressure, never a wait). */
size_t sig_pcm_push(sig_pcm_ring_t *r, const sig_sample_t *samples, size_t n);

/* Consumer side (one DSP thread). Pops up to max_n samples; returns the
 * count actually popped (0 is not an error, just "nothing new yet"). */
size_t sig_pcm_pop(sig_pcm_ring_t *r, sig_sample_t *out, size_t max_n);

size_t sig_pcm_count(const sig_pcm_ring_t *r);
bool   sig_pcm_is_empty(const sig_pcm_ring_t *r);

/* ------------------------------------------------------------------ */
/* FIR decimation filter (stateful, streaming-correct)                 */
/* ------------------------------------------------------------------ */

#define SIG_FIR_TAPS          16u /* multiple of 4: exact 4x unroll, no remainder handling */
#define SIG_DECIMATION_FACTOR 4u  /* power of two: phase tracked with a bitmask, no modulo */

_Static_assert((SIG_FIR_TAPS % 4u) == 0u, "SIG_FIR_TAPS must be a multiple of 4 for the explicit unroll");
_Static_assert(SIG_DECIMATION_FACTOR != 0u &&
               (SIG_DECIMATION_FACTOR & (SIG_DECIMATION_FACTOR - 1u)) == 0u,
               "SIG_DECIMATION_FACTOR must be a power of two");

/* Q15 fixed-point boxcar (moving-average) low-pass coefficients, unity DC
 * gain (they sum to exactly 32768 = 2^15): a basic anti-aliasing filter,
 * not tuned for stopband performance -- see the file header. */
extern const int32_t SIG_FIR_COEFFS[SIG_FIR_TAPS];

typedef struct {
    _Alignas(SIG_CACHE_LINE_SIZE) sig_sample_t history[SIG_FIR_TAPS]; /* [0]=oldest .. [TAPS-1]=newest */
    uint32_t decim_phase; /* persists across calls: correct streaming decimation */
} sig_fir_state_t;

void sig_fir_state_init(sig_fir_state_t *st);

/* Filters + decimates `n_in` input samples, writing at most out_cap
 * output samples to `out`. Returns the number of output samples actually
 * produced. Deterministic and stateful: calling this repeatedly on
 * successive chunks of one continuous stream gives bit-identical results
 * to calling it once on the whole concatenated stream (verified in
 * tests/test_signal_core.c). O(n_in * SIG_FIR_TAPS): a fixed, bounded
 * amount of work per input sample, independent of the sample VALUES. */
size_t sig_fir_decimate(sig_fir_state_t *st, const sig_sample_t *in, size_t n_in,
                         sig_sample_t *out, size_t out_cap);

/* ------------------------------------------------------------------ */
/* SRAM-style INT8 activation scratchpad + deterministic token decode   */
/* ------------------------------------------------------------------ */

#define SIG_ACT_DIM     64u  /* one cache line of INT8 activations */
#define SIG_TOKEN_VOCAB 256u /* byte-level output token space */

typedef struct {
    _Alignas(SIG_CACHE_LINE_SIZE) int8_t activation[SIG_ACT_DIM];
} sig_sram_t;

/* Generates the fixed, deterministic (seeded, never random) weight table
 * used by sig_infer_token(). Call once before any inference. */
void sig_sram_init(void);

/* Folds `n` decimated samples into sram->activation (bucketed
 * average, rescaled to INT8) -- O(n), bounded by the caller-supplied n. */
void sig_sram_fold(sig_sram_t *sram, const sig_sample_t *decimated, size_t n);

/* Fixed VOCAB x DIM dot-product + argmax over sram->activation. The same
 * SIG_TOKEN_VOCAB * SIG_ACT_DIM multiply-adds run every call regardless
 * of the activation VALUES -- see the file header's constant-time note. */
uint8_t sig_infer_token(const sig_sram_t *sram);

#define SIG_MSG_TYPE_TOKEN 1u

/* Zero-copy emission: reserves a slot directly in `target`'s mailbox
 * (actor_core.h) and writes `token` straight into it -- no intermediate
 * buffer. sender_id on the published message is `tenant_id`. */
actor_status_t sig_emit_token(actor_t *target, uint16_t tenant_id, uint8_t token);

/* ------------------------------------------------------------------ */
/* CPUID-based feature detection (reporting only -- no code generation) */
/* ------------------------------------------------------------------ */

typedef struct {
    bool sse2;      /* x86_64 baseline: architecturally guaranteed, checked anyway for completeness */
    bool avx2;
    bool avx512f;
    bool amx_tile;  /* CPUID leaf 7, sub-leaf 0, EDX bit 24 per the Intel SDM -- not verifiable on
                      * hardware without AMX; reported as detected purely from the CPUID bit. */
    bool neon;      /* AArch64: NEON (Advanced SIMD) is mandatory in the base architecture -- this
                      * is an architectural fact stated at compile time, not a runtime CPUID-style
                      * probe (there is no user-space equivalent on ARM). */
} sig_cpu_features_t;

/* Pure feature reporting: reads CPUID (x86/x86_64) or the architectural
 * guarantee (AArch64). Never generates, modifies, or executes any code
 * based on the result -- dispatch onto separately-compiled kernels is the
 * caller's decision, out of scope for this function. */
sig_cpu_features_t sig_detect_cpu_features(void);

#endif /* SIGNAL_CORE_H */
