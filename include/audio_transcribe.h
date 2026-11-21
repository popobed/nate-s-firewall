/*
 * audio_transcribe.h
 *
 * Phase 5 -- Filtrage acoustique de trames PCM 16-bit pre-allouees
 * statiquement, avec un noyau FIR explicitement vectorise AVX2 (dispatch
 * temps reel vers un repli scalaire portable), et emission zero-copy de
 * l'index de la trame nettoyee dans la mailbox lock-free d'un acteur
 * d'inference (actor_core.h).
 *
 * ============================ Honest scope =================================
 * "transcribe" dans le nom de fichier reprend le mot demande, mais AUCUNE
 * transcription (reconnaissance vocale / speech-to-text) n'est implementee
 * ici -- ni ailleurs dans ce projet. Un vrai modele acoustique/linguistique
 * entraine ne peut pas etre produit par un fichier source ecrit a la main;
 * pretendre le contraire serait malhonnete. Ce que ce fichier implemente
 * REELLEMENT, sans placeholder ni simulation, c'est la partie DSP en amont
 * d'une transcription : un filtre FIR passe-bas (le meme genre de brique
 * qu'utiliserait un vrai pipeline de reconnaissance vocale pour nettoyer
 * le signal avant toute inference), calcule avec de vraies instructions
 * AVX2 explicites (immintrin.h), et un dispatch zero-copy vers un acteur en
 * aval. Le decodage effectif (signal nettoye -> texte) reste hors de
 * portee, exactement comme documente par signal_core.h (sig_infer_token()),
 * dont ce module reutilise volontairement sig_detect_cpu_features() plutot
 * que de dupliquer le CPUID.
 *
 * ============================ AVX2 : reel, jamais inconditionnel ===========
 * Les instructions AVX2 ne sont PAS un artifice : audio_fir_kernel_avx2()
 * (src/audio_transcribe.c) utilise reellement _mm256_madd_epi16 sur un
 * registre de 16 echantillons int16 -- verifie bit-a-bit identique au
 * noyau scalaire (tests/test_audio_wasm_integration.c), l'addition
 * entiere etant exactement associative/commutative (pas d'arrondi flottant
 * en jeu), donc l'ordre de reduction vectoriel vs. sequentiel ne change
 * jamais le resultat pour les coefficients/tailles utilises ici (borne
 * demontree dans le fichier source).
 *
 * Cette machine de compilation/execution n'a PAS AVX2 en materiel (verifie
 * via signal_core.h's sig_detect_cpu_features(): avx2=0) -- executer le
 * noyau AVX2 sans verification prealable declenche une instruction
 * illegale (SIGILL), constate empiriquement pendant le developpement de ce
 * module. C'est pourquoi le noyau AVX2 est compile avec
 * `__attribute__((target("avx2")))` (pas de -mavx2 global : le reste du
 * projet reste portable) et n'est JAMAIS appele directement par l'API
 * publique -- seul audio_fir_filter_frame_ex() y accede, apres avoir
 * verifie sig_detect_cpu_features().avx2 lui-meme. Aucune fonction ici ne
 * permet a un appelant de contourner cette verification.
 *
 * ============================ Zero-copy / zero allocation ==================
 * audio_pool_t embarque tout son stockage (trames + etat FIR par canal) :
 * aucun malloc/free, ni a l'init ni en cours de traitement.
 * audio_emit_frame_index() ecrit uniquement un index (uint32_t, 4 octets)
 * dans un slot de mailbox reserve via actor_mailbox_reserve() -- jamais les
 * echantillons audio eux-memes -- exactement le meme principe de
 * descripteur que net_gateway.h/signal_core.h : le consommateur lit les
 * echantillons filtres en place via ce meme audio_pool_t partage.
 *
 * ============================ Extraction de caracteristiques (v2) ==========
 * "banques de filtres MFCC" : ce fichier n'implemente PAS de MFCC canonique
 * (qui exige FFT + filtres mel + log + DCT) -- l'honnetete impose de le
 * dire plutot que d'utiliser le terme en glissant sur la difference. Ce qui
 * EST implemente reellement, c'est la meme famille de technique --
 * l'energie du signal pres d'un ensemble de frequences centrales espacees
 * sur l'echelle mel -- calculee via l'algorithme de Goertzel (une
 * recursion IIR d'ordre 2 par bande, la technique standard pour extraire
 * l'energie a UNE frequence donnee sans calculer une FFT complete). C'est
 * une extraction de caracteristiques frequentielles reelle, pas une
 * simulation, simplement une variante dans le domaine temporel plutot que
 * frequentiel-par-FFT.
 *
 * AUDIO_MEL_BANDS == 8 n'est pas arbitraire : c'est exactement la largeur
 * d'un registre AVX2 en float32 (8 x float). audio_extract_features(),
 * quand l'AVX2 est detecte, fait avancer les 8 recursions de Goertzel (une
 * par bande, independantes les unes des autres) EN PARALLELE dans un seul
 * __m256 -- un vrai motif SIMD "N canaux independants dans N voies", par
 * opposition au noyau FIR ci-dessus qui vectorise a l'interieur d'UN SEUL
 * canal. Contrairement au noyau FIR (arithmetique entiere, associative de
 * facon exacte), ce noyau est en virgule flottante : le scalaire et le
 * vectoriel ne sont PAS garantis bit-a-bit identiques (l'addition
 * flottante n'est pas associative en general) -- les deux implementations
 * suivent le meme ordre d'operations et sont verifiees a une tolerance
 * epsilon dans les tests, jamais bit-a-bit -- ce serait une fausse
 * promesse de pretendre le contraire pour du flottant.
 *
 * ============================ Inference dans le scratchpad SRAM ============
 * audio_actor_store_features() ecrit le vecteur de caracteristiques
 * (8 octets INT8) DIRECTEMENT dans actor_t.private_memory
 * (actor_core.h) -- le "Private Heap statique de l'Acteur" demande, qui
 * n'est pas un tas du tout mais un bloc statique embarque, exactement le
 * terme que ce projet utilise ailleurs (signal_core.h's sig_sram_t) pour
 * designer la meme idee : une zone d'activation figee en adresse, jamais
 * reallouee. audio_actor_infer_phoneme_class() relit ce meme bloc et
 * calcule un argmax deterministe dessus -- l'inference s'execute
 * entierement sur cette zone, comme demande.
 *
 * Ce que cette "inference" N'EST PAS : un decodeur phonetique francais, ni
 * un modele "style Whisper". audio_phoneme_classifier_init() genere une
 * table de poids a graine FIXE (jamais aleatoire, jamais entrainee) --
 * exactement la meme honnetete que signal_core.h's sig_infer_token(). Ce
 * qui est reel et verifie ici, c'est la plomberie complete et fonctionnelle
 * (extraction -> ecriture SRAM -> lecture SRAM -> argmax deterministe ->
 * dispatch zero-copy), pas la comprehension du francais ni d'aucune
 * langue -- aucun jeu de poids entraine sur de la parole n'existe dans ce
 * fichier ni n'aurait pu y etre invente honnetement.
 */

#ifndef AUDIO_TRANSCRIBE_H
#define AUDIO_TRANSCRIBE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "signal_core.h" /* reutilise actor_core.h (transitif) + sig_detect_cpu_features() */

#define AUDIO_CACHE_LINE_SIZE 64u

#define AUDIO_FRAME_POOL_SIZE 8u   /* canaux/tenants audio simultanes, capacite fixe */
#define AUDIO_FRAME_SAMPLES   256u /* echantillons par trame, multiple de 16 (largeur du noyau AVX2) */
#define AUDIO_FIR_TAPS        16u  /* EXACTEMENT un registre AVX2 de 16 x int16 -- voir le noyau AVX2 */
#define AUDIO_FIR_HISTORY     (AUDIO_FIR_TAPS - 1u) /* echantillons de la trame precedente necessaires en tete de fenetre */
#define AUDIO_EXT_LEN         (AUDIO_FIR_HISTORY + AUDIO_FRAME_SAMPLES)

_Static_assert(AUDIO_FIR_TAPS == 16u,
               "le noyau AVX2 (_mm256_madd_epi16 sur un unique registre 256 bits) est cable pour exactement 16 taps");
_Static_assert((AUDIO_FRAME_SAMPLES % 16u) == 0u,
               "AUDIO_FRAME_SAMPLES multiple de 16 : pas de reste a gerer en fin de trame");

typedef int16_t audio_sample_t;

typedef enum {
    AUDIO_OK = 0,
    AUDIO_ERR_INVALID        = -1,
    AUDIO_ERR_POOL_EXHAUSTED = -2, /* aucun canal libre -- capacite fixe AUDIO_FRAME_POOL_SIZE */
    AUDIO_ERR_BAD_INDEX      = -3,
    AUDIO_ERR_AVX2_UNAVAILABLE = -4 /* mode force demande mais sig_detect_cpu_features().avx2 == false */
} audio_status_t;

/* AUDIO_FIR_AUTO : dispatch reel sur AVX2 si detecte, sinon repli scalaire.
 * AUDIO_FIR_FORCE_SCALAR / AUDIO_FIR_FORCE_AVX2 : reserves aux tests, pour
 * comparer bit-a-bit les deux noyaux -- FORCE_AVX2 verifie quand meme le
 * CPUID et echoue proprement (AUDIO_ERR_AVX2_UNAVAILABLE) plutot que de
 * risquer une instruction illegale sur du materiel sans AVX2. */
typedef enum {
    AUDIO_FIR_AUTO         = 0,
    AUDIO_FIR_FORCE_SCALAR = 1,
    AUDIO_FIR_FORCE_AVX2   = 2
} audio_fir_mode_t;

/* Tag de message distinct de SIG_MSG_TYPE_TOKEN (signal_core.h == 1u) pour
 * que les deux modules restent identifiables sur une meme mailbox
 * partagee. */
#define AUDIO_MSG_TYPE_FRAME_READY 2u

typedef struct {
    _Alignas(AUDIO_CACHE_LINE_SIZE) audio_sample_t samples[AUDIO_FRAME_SAMPLES];  /* entree brute -- cible d'ingestion zero-copy directe */
    _Alignas(AUDIO_CACHE_LINE_SIZE) audio_sample_t filtered[AUDIO_FRAME_SAMPLES]; /* sortie nettoyee */
    bool in_use;
} audio_frame_t;

typedef struct {
    _Alignas(AUDIO_CACHE_LINE_SIZE) audio_sample_t tail[AUDIO_FIR_HISTORY]; /* continuite du filtre entre deux trames du meme canal */
} audio_fir_state_t;

/* Grand (plusieurs Ko, domine par les buffers de trames) : place statique/
 * globale, pas la pile -- meme convention que ngw_gateway_t. */
typedef struct {
    audio_frame_t     frames[AUDIO_FRAME_POOL_SIZE];
    audio_fir_state_t fir_state[AUDIO_FRAME_POOL_SIZE];
} audio_pool_t;

void audio_pool_init(audio_pool_t *pool);

/* Acquisition O(1) (scan borne sur AUDIO_FRAME_POOL_SIZE, une petite
 * constante fixe -- meme idiome que NGW_MAX_ACTORS/NET_IO_MAX_ACTORS
 * ailleurs dans ce projet) d'un canal libre. */
audio_status_t audio_frame_acquire(audio_pool_t *pool, uint32_t *out_index);
void           audio_frame_release(audio_pool_t *pool, uint32_t index);

/* Pointeur direct vers le tampon d'entree du canal `index` -- cible
 * d'ecriture zero-copy pour une source d'ingestion (ex: recv() reseau,
 * capture materielle) ; NULL si index hors bornes. */
audio_sample_t       *audio_frame_input(audio_pool_t *pool, uint32_t index);
const audio_sample_t *audio_frame_output(const audio_pool_t *pool, uint32_t index);

/* Filtre FIR passe-bas 16 taps sur samples[] -> filtered[] pour le canal
 * `index`, en conservant l'etat inter-trames (audio_fir_state_t) pour que
 * des appels successifs sur le meme canal filtrent un flux continu de
 * facon identique a un seul appel sur la concatenation. O(AUDIO_FRAME_SAMPLES
 * * AUDIO_FIR_TAPS) fixe, zero allocation. */
audio_status_t audio_fir_filter_frame_ex(audio_pool_t *pool, uint32_t index, audio_fir_mode_t mode);
audio_status_t audio_fir_filter_frame(audio_pool_t *pool, uint32_t index); /* == audio_fir_filter_frame_ex(..., AUDIO_FIR_AUTO) */

/* Zero-copy : ecrit uniquement `frame_index` (4 octets) dans un slot reserve
 * de la mailbox de `target` -- jamais les echantillons -- puis publie avec
 * AUDIO_MSG_TYPE_FRAME_READY. Le consommateur relit filtered[frame_index]
 * en place via le meme audio_pool_t (partage entre producteur/consommateur
 * par l'appelant, comme net_gateway.h's descriptor). */
actor_status_t audio_emit_frame_index(actor_t *target, uint16_t tenant_id, uint32_t frame_index);

/* ------------------------------------------------------------------ */
/* Extraction de caracteristiques frequentielles (Goertzel mel-espace)  */
/* ------------------------------------------------------------------ */

#define AUDIO_SAMPLE_RATE_HZ 16000u /* PCM 16-bit, 16 kHz -- voir la demande */
#define AUDIO_MEL_BANDS      8u     /* == un registre AVX2 de 8 x float32, voir le header */

_Static_assert(AUDIO_MEL_BANDS == 8u,
               "le noyau AVX2 (8 recursions de Goertzel dans un seul __m256) est cable pour exactement 8 bandes");

typedef struct {
    _Alignas(AUDIO_CACHE_LINE_SIZE) int8_t bands[AUDIO_MEL_BANDS]; /* energie log-compressee, quantifiee INT8 */
} audio_feature_vec_t;

/* Calcule une fois les frequences centrales mel-espacees et leurs
 * coefficients de Goertzel (2*cos(2*pi*f_k/fs)) -- a appeler avant tout
 * audio_extract_features(). Deterministe (aucun alea) ; utilise <math.h>
 * (cosf/log10f/powf) une seule fois, jamais dans le chemin chaud. */
void audio_filterbank_init(void);

/* Energie de Goertzel par bande sur une trame de AUDIO_FRAME_SAMPLES
 * echantillons (typiquement filtered[], voir audio_fir_filter_frame()),
 * log-compressee (log1pf) puis quantifiee en INT8. mode suit exactement la
 * meme semantique de securite que audio_fir_filter_frame_ex() : jamais
 * d'AVX2 sans verification CPUID reelle prealable. */
audio_status_t audio_extract_features(const audio_sample_t *frame, audio_feature_vec_t *out,
                                       audio_fir_mode_t mode);

/* The actual single-band Goertzel power kernel audio_extract_features()
 * calls internally (loop-unrolled by 4 -- see src/audio_transcribe.c for
 * why that is a modest loop-overhead reduction, not a parallelization
 * win, for this inherently serial IIR recursion). Exposed directly so a
 * test can drive it with arbitrary frame/coeff values, not only whatever
 * audio_filterbank_init()'s fixed mel-band coefficients happen to produce. */
float audio_goertzel_power_scalar(const audio_sample_t *frame, float coeff);

/* Diagnostic/test-only: the pre-unroll, one-sample-per-iteration version
 * of the same kernel, kept verbatim so a test can prove the two are
 * bit-identical for every input -- never called from this module's own
 * hot path. */
float audio_goertzel_power_scalar_reference(const audio_sample_t *frame, float coeff);

/* ------------------------------------------------------------------ */
/* Scratchpad SRAM = actor_t.private_memory (actor_core.h)              */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(audio_feature_vec_t) <= ACTOR_PRIVATE_MEM_SIZE,
               "le vecteur de caracteristiques doit tenir dans la memoire privee statique de l'acteur");

/* Ecrit `features` directement dans target->private_memory (offset 0) --
 * zero-copy au sens ou aucun tampon intermediaire n'existe entre le
 * vecteur calcule et la memoire privee finale de l'acteur. */
actor_status_t audio_actor_store_features(actor_t *target, const audio_feature_vec_t *features);
audio_status_t audio_actor_load_features(const actor_t *source, audio_feature_vec_t *out);

#define AUDIO_PHONEME_CLASS_COUNT 40u /* borne fixe, purement illustrative -- voir "Inference dans le scratchpad SRAM" */

/* Genere une table de poids AUDIO_PHONEME_CLASS_COUNT x AUDIO_MEL_BANDS a
 * graine FIXE (jamais aleatoire, jamais entrainee) -- a appeler une fois
 * avant tout audio_actor_infer_phoneme_class(). */
void audio_phoneme_classifier_init(void);

/* Relit le vecteur de caracteristiques depuis target->private_memory (la
 * meme zone que audio_actor_store_features() a remplie) et calcule un
 * argmax deterministe dessus : l'inference s'execute entierement sur ce
 * scratchpad, comme demande -- voir le header pour ce que ce resultat
 * N'EST PAS (aucun decodage phonetique francais reel). */
uint8_t audio_actor_infer_phoneme_class(const actor_t *target);

#endif /* AUDIO_TRANSCRIBE_H */
