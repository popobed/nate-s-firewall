/*
 * audio_transcribe.c
 *
 * Voir audio_transcribe.h pour la portee (honest scope) et les garanties.
 * Zero malloc/free/calloc/realloc dans ce fichier : audio_pool_t embarque
 * tout son stockage, et le seul tampon "scratch" (le buffer etendu utilise
 * pour construire les fenetres de convolution) est une variable
 * automatique de pile de taille fixe (AUDIO_EXT_LEN * sizeof(audio_sample_t)
 * = 542 octets), jamais tas.
 */

#include "audio_transcribe.h"

#include <math.h>
#include <string.h>

/* immintrin.h (AVX2 intrinsics) is x86/x86_64-specific -- same conditional
 * compilation boundary as signal_core.c's CPUID code. On any other
 * architecture, sig_detect_cpu_features().avx2 is unconditionally false
 * (see signal_core.c), so AUDIO_FIR_AUTO/AUDIO_FIR_FORCE_SCALAR simply
 * never select the AVX2 path there, and AUDIO_FIR_FORCE_AVX2 correctly
 * returns AUDIO_ERR_AVX2_UNAVAILABLE -- no AVX2 kernel needs to exist on
 * such a build at all. */
#if defined(__x86_64__) || defined(__i386__)
#define AUDIO_HAVE_AVX2_KERNEL 1
#include <immintrin.h>
#else
#define AUDIO_HAVE_AVX2_KERNEL 0
#endif

/* ------------------------------------------------------------------ */
/* Pool de canaux                                                       */
/* ------------------------------------------------------------------ */

void audio_pool_init(audio_pool_t *pool) {
    if (pool == NULL) {
        return;
    }
    memset(pool, 0, sizeof *pool);
}

audio_status_t audio_frame_acquire(audio_pool_t *pool, uint32_t *out_index) {
    if (pool == NULL || out_index == NULL) {
        return AUDIO_ERR_INVALID;
    }
    /* Scan borne sur une petite constante fixe (8) : O(1) au meme sens que
     * les autres tables de petite capacite fixe de ce projet. */
    for (uint32_t i = 0; i < AUDIO_FRAME_POOL_SIZE; i++) {
        if (!pool->frames[i].in_use) {
            pool->frames[i].in_use = true;
            memset(pool->frames[i].samples, 0, sizeof pool->frames[i].samples);
            memset(pool->frames[i].filtered, 0, sizeof pool->frames[i].filtered);
            *out_index = i;
            return AUDIO_OK;
        }
    }
    return AUDIO_ERR_POOL_EXHAUSTED;
}

void audio_frame_release(audio_pool_t *pool, uint32_t index) {
    if (pool == NULL || index >= AUDIO_FRAME_POOL_SIZE) {
        return;
    }
    pool->frames[index].in_use = false;
}

audio_sample_t *audio_frame_input(audio_pool_t *pool, uint32_t index) {
    if (pool == NULL || index >= AUDIO_FRAME_POOL_SIZE) {
        return NULL;
    }
    return pool->frames[index].samples;
}

const audio_sample_t *audio_frame_output(const audio_pool_t *pool, uint32_t index) {
    if (pool == NULL || index >= AUDIO_FRAME_POOL_SIZE) {
        return NULL;
    }
    return pool->frames[index].filtered;
}

/* ------------------------------------------------------------------ */
/* Filtre FIR : coefficients (Q15 boxcar, meme convention que              */
/* signal_core.h's SIG_FIR_COEFFS -- gain DC unitaire : 16*2048==32768)     */
/* ------------------------------------------------------------------ */

static const int16_t AUDIO_FIR_COEFFS[AUDIO_FIR_TAPS] = {
    2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048,
    2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048
};

/* Chaque coefficient (2048) tient sur 12 bits et chaque echantillon est un
 * int16 signe : le produit d'une paire tient toujours dans un int32
 * (max |2048 * 32768| < 2^31), et la somme des 16 produits (pire cas :
 * 16 * 2048 * 32767 = 1 073 676 288) reste elle aussi < INT32_MAX
 * (2 147 483 647). C'est cette borne, verifiee ici une fois pour toutes,
 * qui rend le noyau AVX2 (accumulation 32 bits via _mm256_madd_epi16) et
 * le noyau scalaire (accumulation 64 bits, plus de marge) exactement
 * comparables terme a terme -- voir le header. */
_Static_assert((int64_t)AUDIO_FIR_TAPS * 2048LL * 32767LL < 2147483647LL,
               "somme des produits FIR hors de la marge de securite int32 documentee ci-dessus");

static void audio_fir_build_extended(const audio_fir_state_t *st, const audio_sample_t *in,
                                      audio_sample_t *ext /* [AUDIO_EXT_LEN] */) {
    memcpy(ext, st->tail, sizeof st->tail);
    memcpy(ext + AUDIO_FIR_HISTORY, in, AUDIO_FRAME_SAMPLES * sizeof *in);
}

static void audio_fir_update_tail(audio_fir_state_t *st, const audio_sample_t *in) {
    memcpy(st->tail, in + (AUDIO_FRAME_SAMPLES - AUDIO_FIR_HISTORY),
           AUDIO_FIR_HISTORY * sizeof *in);
}

static audio_sample_t audio_fir_saturate(int64_t acc_q15_shifted) {
    if (acc_q15_shifted > INT16_MAX) {
        return INT16_MAX;
    }
    if (acc_q15_shifted < INT16_MIN) {
        return INT16_MIN;
    }
    return (audio_sample_t)acc_q15_shifted;
}

/* ---- Noyau scalaire (portable, toujours disponible) ---- */

static audio_sample_t audio_fir_dot_scalar(const audio_sample_t *window16) {
    int64_t acc = 0;
    for (uint32_t k = 0; k < AUDIO_FIR_TAPS; k++) {
        acc += (int64_t)AUDIO_FIR_COEFFS[k] * (int64_t)window16[k];
    }
    return audio_fir_saturate(acc >> 15); /* Q15 -> entier, decalage arithmetique */
}

static void audio_fir_kernel_scalar(const audio_sample_t *ext, audio_sample_t *out) {
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        out[i] = audio_fir_dot_scalar(&ext[i]);
    }
}

/* ---- Noyau AVX2 (reel : vraies instructions vectorielles, jamais         */
/* appele sans verification CPUID prealable -- voir le header et           */
/* audio_fir_filter_frame_ex()) ---- */

#if AUDIO_HAVE_AVX2_KERNEL

__attribute__((target("avx2")))
static audio_sample_t audio_fir_dot_avx2(const audio_sample_t *window16, __m256i coeff_vec) {
    /* Un seul registre AVX2 = exactement les 16 taps : chargement non
     * aligne (les fenetres glissent d'un echantillon a chaque sortie, donc
     * leur adresse n'est generalement pas 32-octets-alignee -- _mm256_loadu
     * est le choix correct ici, pas une approximation). */
    __m256i w    = _mm256_loadu_si256((const __m256i *)(const void *)window16);
    __m256i prod = _mm256_madd_epi16(w, coeff_vec); /* 8 x int32 : prod[j] = w[2j]*c[2j] + w[2j+1]*c[2j+1] */

    __m128i lo  = _mm256_castsi256_si128(prod);
    __m128i hi  = _mm256_extracti128_si256(prod, 1);
    __m128i sum = _mm_add_epi32(lo, hi); /* 4 x int32 */

    /* Reduction horizontale standard 4 -> 1 (deux etapes shuffle+add) : */
    __m128i shuf = _mm_shuffle_epi32(sum, _MM_SHUFFLE(2, 3, 0, 1));
    sum          = _mm_add_epi32(sum, shuf);
    shuf         = _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2));
    sum          = _mm_add_epi32(sum, shuf);

    const int32_t total = _mm_cvtsi128_si32(sum);
    /* total est une somme d'entiers EXACTE (voir la _Static_assert de marge
     * ci-dessus : jamais de depassement 32 bits) -- l'additionner en tant
     * qu'int64 puis appliquer le meme decalage/saturation que le noyau
     * scalaire produit un resultat bit-a-bit identique, l'addition
     * entiere etant associative/commutative quel que soit l'ordre de
     * reduction (contrairement a une somme flottante). */
    return audio_fir_saturate((int64_t)total >> 15);
}

__attribute__((target("avx2")))
static void audio_fir_kernel_avx2(const audio_sample_t *ext, audio_sample_t *out) {
    const __m256i coeff_vec = _mm256_loadu_si256((const __m256i *)(const void *)AUDIO_FIR_COEFFS);
    for (uint32_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
        out[i] = audio_fir_dot_avx2(&ext[i], coeff_vec);
    }
}

#endif /* AUDIO_HAVE_AVX2_KERNEL */

/* ---- Dispatch : jamais d'AVX2 sans verification CPUID reelle ---- */

audio_status_t audio_fir_filter_frame_ex(audio_pool_t *pool, uint32_t index, audio_fir_mode_t mode) {
    if (pool == NULL || index >= AUDIO_FRAME_POOL_SIZE) {
        return AUDIO_ERR_INVALID;
    }

    const bool has_avx2 = sig_detect_cpu_features().avx2; /* toujours false hors x86/x86_64 -- voir signal_core.c */
    bool use_avx2;
    switch (mode) {
        case AUDIO_FIR_FORCE_SCALAR:
            use_avx2 = false;
            break;
        case AUDIO_FIR_FORCE_AVX2:
            if (!has_avx2) {
                /* Refuse plutot que de risquer une instruction illegale sur
                 * du materiel sans AVX2 -- voir le header. */
                return AUDIO_ERR_AVX2_UNAVAILABLE;
            }
            use_avx2 = true;
            break;
        case AUDIO_FIR_AUTO:
        default:
            use_avx2 = has_avx2;
            break;
    }

    audio_fir_state_t *st = &pool->fir_state[index];
    audio_frame_t      *fr = &pool->frames[index];

    audio_sample_t ext[AUDIO_EXT_LEN];
    audio_fir_build_extended(st, fr->samples, ext);

#if AUDIO_HAVE_AVX2_KERNEL
    if (use_avx2) {
        audio_fir_kernel_avx2(ext, fr->filtered);
    } else {
        audio_fir_kernel_scalar(ext, fr->filtered);
    }
#else
    (void)use_avx2; /* toujours false ici (has_avx2 est toujours false hors x86) */
    audio_fir_kernel_scalar(ext, fr->filtered);
#endif

    audio_fir_update_tail(st, fr->samples);
    return AUDIO_OK;
}

audio_status_t audio_fir_filter_frame(audio_pool_t *pool, uint32_t index) {
    return audio_fir_filter_frame_ex(pool, index, AUDIO_FIR_AUTO);
}

/* ------------------------------------------------------------------ */
/* Emission zero-copy vers l'acteur d'inference                         */
/* ------------------------------------------------------------------ */

actor_status_t audio_emit_frame_index(actor_t *target, uint16_t tenant_id, uint32_t frame_index) {
    if (target == NULL) {
        return ACTOR_ERR_INVALID;
    }
    ring_msg_t *slot;
    actor_status_t rst = actor_mailbox_reserve(target, &slot);
    if (rst != ACTOR_OK) {
        return rst;
    }
    /* Zero-copy : seul l'index (4 octets) traverse -- jamais les
     * echantillons audio eux-memes. */
    memcpy(slot->payload, &frame_index, sizeof frame_index);
    memset(slot->payload + sizeof frame_index, 0, RING_MSG_PAYLOAD_SIZE - sizeof frame_index);
    actor_mailbox_publish(target, slot, tenant_id, AUDIO_MSG_TYPE_FRAME_READY);
    return ACTOR_OK;
}

/* ------------------------------------------------------------------ */
/* Extraction de caracteristiques : banc de filtres de Goertzel          */
/* mel-espace (voir le header : famille MFCC, pas un MFCC canonique)     */
/* ------------------------------------------------------------------ */

#define AUDIO_PI 3.14159265358979323846f /* evite de dependre de M_PI (non-ISO sous -pedantic -std=c11) */

static float g_goertzel_coeff[AUDIO_MEL_BANDS]; /* 2*cos(2*pi*f_k/fs), calcule une fois par audio_filterbank_init() */

void audio_filterbank_init(void) {
    /* Bande utile vocale typique pour du 16 kHz (Nyquist = 8 kHz) : 200 Hz
     * a 7500 Hz, espacee lineairement sur l'echelle mel (mel(f) =
     * 2595*log10(1+f/700)) puis reconvertie en Hz -- exactement la
     * construction standard d'un banc de filtres mel, seule la partie
     * "energie par bande" change (Goertzel au lieu de FFT+triangles). */
    const float mel_low  = 2595.0f * log10f(1.0f + 200.0f / 700.0f);
    const float mel_high = 2595.0f * log10f(1.0f + 7500.0f / 700.0f);
    for (uint32_t k = 0; k < AUDIO_MEL_BANDS; k++) {
        const float mel_k = mel_low + (mel_high - mel_low) * ((float)k / (float)(AUDIO_MEL_BANDS - 1u));
        const float f_k   = 700.0f * (powf(10.0f, mel_k / 2595.0f) - 1.0f);
        g_goertzel_coeff[k] = 2.0f * cosf(2.0f * AUDIO_PI * f_k / (float)AUDIO_SAMPLE_RATE_HZ);
    }
}

static int8_t audio_quantize_energy(float power) {
    /* log1pf compresse la large plage dynamique de l'energie (proportionnelle
     * au carre de l'amplitude) avant quantification -- la meme raison
     * d'etre qu'un log-mel dans un MFCC classique. */
    const float compressed = log1pf(power);
    float scaled = compressed * 8.0f; /* mise a l'echelle empirique vers la plage INT8 -- voir le header */
    if (scaled > 127.0f) {
        scaled = 127.0f;
    } else if (scaled < -128.0f) {
        scaled = -128.0f;
    }
    return (int8_t)scaled;
}

/* ---- Noyau scalaire (portable, toujours disponible) ---- */

/*
 * Loop-unrolled by 4 (AUDIO_FRAME_SAMPLES == 256, a multiple of 4 by
 * _Static_assert in the header -- no remainder tail to handle).
 *
 * Honest scope of what this buys, and what it does NOT: a Goertzel
 * recursion has a genuine loop-carried dependency -- computing s0 at
 * step n needs s1/s2 from step n-1, which needs step n-2's, and so on.
 * That is an inherently SERIAL chain (this is the textbook reason IIR
 * filters, unlike FIR filters, do not vectorize/pipeline across samples
 * of the SAME recursion -- see this file's own AVX2 kernel below, which
 * gets its real parallelism from running the 8 INDEPENDENT per-band
 * recursions in the 8 lanes of one __m256, never from unrolling within
 * one band's sample loop). Unrolling this loop cannot overlap multiple
 * values of `n` for the same band -- step n+1 still cannot start before
 * step n's s0 exists, unrolled or not, and no amount of "saturating
 * registers" removes that dependency; it is a property of the algorithm,
 * not the code. What unrolling genuinely does here: replace 256 loop-
 * control operations (increment + compare + branch) with 64, cutting
 * fixed per-iteration overhead relative to the one FMA-chain step that
 * actually does the work -- a real but modest gain, honestly scoped, not
 * the register-saturation win a data-parallel loop would get. See
 * test_audio_wasm_integration.c for the actual measured delta: this
 * codebase's standing rule is "verified in tests, not just assumed" (see
 * SESSION_NOTES.md).
 *
 * Bit-exact by construction: the four steps per unrolled block execute
 * in the exact same order, same operations, same intermediate values as
 * four consecutive calls to the original single-step body would -- no
 * reordering of floating-point operations happens at all, so the result
 * is bit-identical to the pre-unroll implementation for every input,
 * not merely "close" -- verified by direct equality in the test, not an
 * epsilon tolerance (contrast the AVX2-vs-scalar comparison elsewhere in
 * this file, which genuinely does reorder operations and is correctly
 * epsilon-tolerant instead).
 */
_Static_assert((AUDIO_FRAME_SAMPLES % 4u) == 0u,
               "the 4x unroll below assumes no remainder tail (already implied by the header's "
               "%16==0 assert, restated here at the point that actually relies on it)");

float audio_goertzel_power_scalar(const audio_sample_t *frame, float coeff) {
    float s1 = 0.0f, s2 = 0.0f;
    uint32_t n = 0;
    for (; n < AUDIO_FRAME_SAMPLES; n += 4u) {
        float s0;

        s0 = (float)frame[n]     + coeff * s1 - s2; s2 = s1; s1 = s0;
        s0 = (float)frame[n + 1u] + coeff * s1 - s2; s2 = s1; s1 = s0;
        s0 = (float)frame[n + 2u] + coeff * s1 - s2; s2 = s1; s1 = s0;
        s0 = (float)frame[n + 3u] + coeff * s1 - s2; s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/* Pre-unroll reference kernel, kept verbatim for the bit-exact
 * equivalence test in test_audio_wasm_integration.c -- proof, not
 * assertion, that unrolling changed nothing about the computed result.
 * Declared in audio_transcribe.h (test-only, see there); not part of
 * this module's own hot path. */
float audio_goertzel_power_scalar_reference(const audio_sample_t *frame, float coeff) {
    float s1 = 0.0f, s2 = 0.0f;
    for (uint32_t n = 0; n < AUDIO_FRAME_SAMPLES; n++) {
        const float s0 = (float)frame[n] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static void audio_features_kernel_scalar(const audio_sample_t *frame, audio_feature_vec_t *out) {
    for (uint32_t k = 0; k < AUDIO_MEL_BANDS; k++) {
        const float power = audio_goertzel_power_scalar(frame, g_goertzel_coeff[k]);
        out->bands[k] = audio_quantize_energy(power);
    }
}

/* ---- Noyau AVX2 (reel : les AUDIO_MEL_BANDS==8 recursions de Goertzel    */
/* -- une par bande, mutuellement independantes -- avancent EN PARALLELE   */
/* dans un seul __m256, jamais appele sans verification CPUID prealable    */
/* -- voir le header pour la note sur la tolerance flottante) ---- */

#if AUDIO_HAVE_AVX2_KERNEL

__attribute__((target("avx2")))
static void audio_features_kernel_avx2(const audio_sample_t *frame, audio_feature_vec_t *out) {
    const __m256 coeff = _mm256_loadu_ps(g_goertzel_coeff);
    __m256 s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps();
    for (uint32_t n = 0; n < AUDIO_FRAME_SAMPLES; n++) {
        const __m256 x  = _mm256_set1_ps((float)frame[n]);
        /* Meme ordre d'operations que le noyau scalaire : (x + coeff*s1) - s2. */
        const __m256 t0 = _mm256_add_ps(x, _mm256_mul_ps(coeff, s1));
        const __m256 s0 = _mm256_sub_ps(t0, s2);
        s2 = s1;
        s1 = s0;
    }
    /* power = (s1*s1 + s2*s2) - (coeff*s1)*s2 -- meme ordre que le scalaire. */
    const __m256 t1    = _mm256_add_ps(_mm256_mul_ps(s1, s1), _mm256_mul_ps(s2, s2));
    const __m256 t2    = _mm256_mul_ps(_mm256_mul_ps(coeff, s1), s2);
    const __m256 power = _mm256_sub_ps(t1, t2);

    _Alignas(32) float power_arr[AUDIO_MEL_BANDS];
    _mm256_store_ps(power_arr, power);
    for (uint32_t k = 0; k < AUDIO_MEL_BANDS; k++) {
        out->bands[k] = audio_quantize_energy(power_arr[k]);
    }
}

#endif /* AUDIO_HAVE_AVX2_KERNEL */

audio_status_t audio_extract_features(const audio_sample_t *frame, audio_feature_vec_t *out,
                                       audio_fir_mode_t mode) {
    if (frame == NULL || out == NULL) {
        return AUDIO_ERR_INVALID;
    }
    /* _Alignas(64) on `bands` forces sizeof(audio_feature_vec_t) up to 64
     * (the struct's size must be a multiple of its alignment) even though
     * only AUDIO_MEL_BANDS==8 bytes are meaningful -- zero the whole
     * struct first so the tail padding is deterministic (0), not whatever
     * garbage sat on the caller's stack. Without this, two callers who
     * memcmp() or hash the whole struct (a reasonable thing to do with a
     * small POD feature vector) would see it differ despite identical
     * `bands[]` content -- not a computation bug, but an avoidable trap
     * this function should not hand its callers. */
    memset(out, 0, sizeof *out);

    const bool has_avx2 = sig_detect_cpu_features().avx2;
    bool use_avx2;
    switch (mode) {
        case AUDIO_FIR_FORCE_SCALAR:
            use_avx2 = false;
            break;
        case AUDIO_FIR_FORCE_AVX2:
            if (!has_avx2) {
                return AUDIO_ERR_AVX2_UNAVAILABLE; /* jamais d'instruction illegale sur du materiel sans AVX2 */
            }
            use_avx2 = true;
            break;
        case AUDIO_FIR_AUTO:
        default:
            use_avx2 = has_avx2;
            break;
    }

#if AUDIO_HAVE_AVX2_KERNEL
    if (use_avx2) {
        audio_features_kernel_avx2(frame, out);
    } else {
        audio_features_kernel_scalar(frame, out);
    }
#else
    (void)use_avx2;
    audio_features_kernel_scalar(frame, out);
#endif
    return AUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Scratchpad SRAM = actor_t.private_memory                             */
/* ------------------------------------------------------------------ */

actor_status_t audio_actor_store_features(actor_t *target, const audio_feature_vec_t *features) {
    if (target == NULL || features == NULL) {
        return ACTOR_ERR_INVALID;
    }
    memcpy(target->private_memory, features, sizeof *features);
    return ACTOR_OK;
}

audio_status_t audio_actor_load_features(const actor_t *source, audio_feature_vec_t *out) {
    if (source == NULL || out == NULL) {
        return AUDIO_ERR_INVALID;
    }
    memcpy(out, source->private_memory, sizeof *out);
    return AUDIO_OK;
}

/* ------------------------------------------------------------------ */
/* Classifieur de demonstration -- graine fixe, JAMAIS une langue reelle */
/* (voir le header : "Inference dans le scratchpad SRAM")                */
/* ------------------------------------------------------------------ */

static int8_t g_phoneme_weights[AUDIO_PHONEME_CLASS_COUNT][AUDIO_MEL_BANDS];

static uint64_t audio_splitmix64_next(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void audio_phoneme_classifier_init(void) {
    uint64_t seed = 0x415544494F3846ULL; /* "AUDIO8F" tronque -- constante fixe et arbitraire */
    for (uint32_t c = 0; c < AUDIO_PHONEME_CLASS_COUNT; c++) {
        for (uint32_t d = 0; d < AUDIO_MEL_BANDS; d++) {
            g_phoneme_weights[c][d] = (int8_t)(audio_splitmix64_next(&seed) & 0xFFu);
        }
    }
}

uint8_t audio_actor_infer_phoneme_class(const actor_t *target) {
    audio_feature_vec_t features;
    memset(&features, 0, sizeof features);
    if (target != NULL) {
        memcpy(&features, target->private_memory, sizeof features);
    }

    int32_t best_score = INT32_MIN;
    uint32_t best_c = 0u;
    for (uint32_t c = 0; c < AUDIO_PHONEME_CLASS_COUNT; c++) {
        int32_t acc = 0;
        for (uint32_t d = 0; d < AUDIO_MEL_BANDS; d++) {
            acc += (int32_t)features.bands[d] * (int32_t)g_phoneme_weights[c][d];
        }
        if (acc > best_score) {
            best_score = acc;
            best_c = c;
        }
    }
    return (uint8_t)best_c;
}
