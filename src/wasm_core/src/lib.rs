//! SYNAPSE-MESH CORE : moteur vectoriel HNSW + SIMD + Product
//! Quantization, `#![no_std]`, compile en WebAssembly pur (ABI C, sans
//! wasm-bindgen -- voir le commentaire d'architecture dans le depot pour
//! la justification : wasm-bindgen/web-sys tirent `std` transitivement,
//! ce qui entrerait en conflit avec notre allocateur et notre gestionnaire
//! de panique `no_std`). L'acces IndexedDB se fait depuis la couche
//! TypeScript (src/wasm_core/bindings.ts), qui appelle ces exports pour
//! obtenir/fournir des octets serialises.

#![no_std]

extern crate alloc;

mod allocator;
mod codec;
mod distance;
mod hnsw;
mod pq;
mod rng;
mod state;

use alloc::vec::Vec;

use allocator::BumpAllocator;
use distance::{cosine_distance_scalar, cosine_distance_simd, euclidean_distance_scalar, euclidean_distance_simd, DistanceMetric};
use hnsw::HnswIndex;
use pq::ProductQuantizer;
use state::SingleThreaded;

#[global_allocator]
static ALLOCATOR: BumpAllocator = BumpAllocator::new();

#[panic_handler]
fn empire_panic_handler(_info: &core::panic::PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}

// Etat global unique : ce module n'est instancie qu'une fois par
// contexte JavaScript (une instance WebAssembly = un thread logique
// unique, sans SharedArrayBuffer ici). Voir state.rs pour la
// justification de l'unsafe centralise.
static GLOBAL_INDEX: SingleThreaded<Option<HnswIndex>> = SingleThreaded::new(None);
static GLOBAL_PQ: SingleThreaded<Option<ProductQuantizer>> = SingleThreaded::new(None);
static GLOBAL_PQ_CODES: SingleThreaded<Vec<u8>> = SingleThreaded::new(Vec::new());
static RESULT_BUFFER: SingleThreaded<Vec<u8>> = SingleThreaded::new(Vec::new());

/// Exports de verification croisee : confirment, depuis l'hote
/// JavaScript/Node, que le noyau SIMD128 et le noyau scalaire de
/// reference produisent la meme distance sur les memes entrees. Ne fait
/// partie d'aucun chemin d'insertion/recherche de production.
#[no_mangle]
pub extern "C" fn synapse_debug_distance(ptr_a: *const f32, ptr_b: *const f32, len: u32, metric: u32, use_simd: u32) -> f32 {
    let a = unsafe { core::slice::from_raw_parts(ptr_a, len as usize) };
    let b = unsafe { core::slice::from_raw_parts(ptr_b, len as usize) };

    match (metric_from_code(metric), use_simd != 0) {
        (DistanceMetric::Euclidean, true) => euclidean_distance_simd(a, b),
        (DistanceMetric::Euclidean, false) => euclidean_distance_scalar(a, b),
        (DistanceMetric::Cosine, true) => cosine_distance_simd(a, b),
        (DistanceMetric::Cosine, false) => cosine_distance_scalar(a, b),
    }
}

fn metric_from_code(code: u32) -> DistanceMetric {
    if code == 0 {
        DistanceMetric::Euclidean
    } else {
        DistanceMetric::Cosine
    }
}

/// Initialise (ou reinitialise) l'index HNSW global.
/// metric : 0 = euclidienne, 1 = cosinus.
#[no_mangle]
pub extern "C" fn synapse_init_index(dim: u32, metric: u32, m: u32, ef_construction: u32, seed: u64) {
    *GLOBAL_INDEX.get() = Some(HnswIndex::new(
        dim as usize,
        metric_from_code(metric),
        m as usize,
        ef_construction as usize,
        seed,
    ));
}

/// Alloue un tampon lineaire que l'appelant JS remplit (ex : composantes
/// f32 d'un vecteur) avant de rappeler une fonction de traitement.
/// Retourne un pointeur nul si l'allocation echoue.
#[no_mangle]
pub extern "C" fn synapse_alloc_buffer(len: u32) -> *mut u8 {
    let mut buffer: Vec<u8> = Vec::new();
    buffer.resize(len as usize, 0u8);
    let ptr = buffer.as_mut_ptr();
    core::mem::forget(buffer);
    ptr
}

/// Libere un tampon precedemment obtenu via `synapse_alloc_buffer`.
/// (L'allocateur bump ne recupere pas reellement la memoire -- voir
/// allocator.rs -- mais cette fonction reste necessaire pour executer
/// correctement le destructeur logique du cote Rust.)
#[no_mangle]
pub extern "C" fn synapse_free_buffer(ptr: *mut u8, len: u32) {
    unsafe {
        let _ = Vec::from_raw_parts(ptr, len as usize, len as usize);
    }
}

/// Insere un vecteur de `len` f32 (pointeur vers memoire lineaire WASM,
/// generalement obtenue via `synapse_alloc_buffer`) dans l'index.
/// Retourne l'identifiant du nouveau noeud, ou -1/-2 en cas d'erreur.
#[no_mangle]
pub extern "C" fn synapse_insert_vector(ptr: *const f32, len: u32) -> i64 {
    let slice = unsafe { core::slice::from_raw_parts(ptr, len as usize) };
    match GLOBAL_INDEX.get().as_mut() {
        Some(index) => {
            if slice.len() != index.dim() {
                -1
            } else {
                index.insert(slice.to_vec()) as i64
            }
        }
        None => -2,
    }
}

/// Recherche les `k` plus proches voisins de la requete pointee par
/// `ptr`/`len`. Ecrit les resultats (id:u32, distance:f32) dans un
/// tampon interne lisible via `synapse_result_ptr`, et retourne le
/// nombre de resultats trouves.
#[no_mangle]
pub extern "C" fn synapse_search_knn(ptr: *const f32, len: u32, k: u32, ef_search: u32) -> u32 {
    let slice = unsafe { core::slice::from_raw_parts(ptr, len as usize) };

    let results = match GLOBAL_INDEX.get().as_ref() {
        Some(index) => index.search(slice, k as usize, ef_search as usize),
        None => Vec::new(),
    };

    let mut buffer = Vec::with_capacity(results.len() * 8);
    for (id, dist) in &results {
        buffer.extend_from_slice(&id.to_le_bytes());
        buffer.extend_from_slice(&dist.to_le_bytes());
    }

    let count = results.len() as u32;
    *RESULT_BUFFER.get() = buffer;
    count
}

#[no_mangle]
pub extern "C" fn synapse_result_ptr() -> *const u8 {
    RESULT_BUFFER.get().as_ptr()
}

#[no_mangle]
pub extern "C" fn synapse_index_len() -> u32 {
    GLOBAL_INDEX.get().as_ref().map(|i| i.len() as u32).unwrap_or(0)
}

/// Entraine le Product Quantizer sur l'ensemble des vecteurs actuellement
/// indexes. `num_subvectors` doit diviser la dimension de l'index.
#[no_mangle]
pub extern "C" fn synapse_pq_train(num_subvectors: u32, num_centroids: u32, iterations: u32, seed: u64) -> i32 {
    let index = match GLOBAL_INDEX.get().as_ref() {
        Some(i) => i,
        None => return -1,
    };

    if index.dim() % (num_subvectors as usize) != 0 {
        return -2;
    }

    let (ids, flat) = index.export_all();
    let mut pq = ProductQuantizer::new(index.dim(), num_subvectors as usize, num_centroids as usize);
    pq.train(&flat, iterations as usize, seed);

    // Encode chaque vecteur actuellement indexe : ces codes alimentent
    // synapse_pq_search_knn (recherche approximative par ADC, sans jamais
    // decoder ni toucher au graphe HNSW).
    let mut codes_flat = Vec::with_capacity(ids.len() * pq.num_subvectors);
    for i in 0..ids.len() {
        let base = i * index.dim();
        codes_flat.extend_from_slice(&pq.encode(&flat[base..base + index.dim()]));
    }
    *GLOBAL_PQ_CODES.get() = codes_flat;
    *GLOBAL_PQ.get() = Some(pq);
    0
}

/// Recherche approximative des `k` plus proches voisins par calcul de
/// distance asymetrique (ADC) sur les codes Product Quantization,
/// balayage lineaire sur l'ensemble des vecteurs quantifies encodes lors
/// du dernier `synapse_pq_train` (independant du graphe HNSW). Illustre
/// directement le gain memoire/vitesse de la quantization : chaque
/// candidat ne coute que `num_subvectors` lectures de table, jamais un
/// decodage complet.
#[no_mangle]
pub extern "C" fn synapse_pq_search_knn(ptr: *const f32, len: u32, k: u32) -> u32 {
    let slice = unsafe { core::slice::from_raw_parts(ptr, len as usize) };

    let pq = match GLOBAL_PQ.get().as_ref() {
        Some(pq) if pq.trained && slice.len() == pq.dim => pq,
        _ => return 0,
    };

    let codes_flat = GLOBAL_PQ_CODES.get();
    let num_subvectors = pq.num_subvectors;
    let count = if num_subvectors == 0 { 0 } else { codes_flat.len() / num_subvectors };
    if count == 0 {
        return 0;
    }

    let table = pq.build_adc_table(slice);
    let mut scored: Vec<(f32, u32)> = Vec::with_capacity(count);
    for i in 0..count {
        let codes = &codes_flat[i * num_subvectors..(i + 1) * num_subvectors];
        scored.push((pq.adc_squared_distance(&table, codes), i as u32));
    }
    scored.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(core::cmp::Ordering::Equal));
    scored.truncate(k as usize);

    let mut buffer = Vec::with_capacity(scored.len() * 8);
    for (dist, id) in &scored {
        buffer.extend_from_slice(&id.to_le_bytes());
        buffer.extend_from_slice(&dist.to_le_bytes());
    }
    let n = scored.len() as u32;
    *RESULT_BUFFER.get() = buffer;
    n
}

/// Decode approximativement le vecteur quantifie d'identifiant `index_id`
/// (reconstruction a partir des centroides du codebook) dans le tampon de
/// resultat interne. Retourne la dimension decodee, ou 0 si indisponible.
#[no_mangle]
pub extern "C" fn synapse_pq_decode(index_id: u32) -> u32 {
    let pq = match GLOBAL_PQ.get().as_ref() {
        Some(pq) if pq.trained => pq,
        _ => return 0,
    };

    let codes_flat = GLOBAL_PQ_CODES.get();
    let start = index_id as usize * pq.num_subvectors;
    if start + pq.num_subvectors > codes_flat.len() {
        return 0;
    }

    let decoded = pq.decode(&codes_flat[start..start + pq.num_subvectors]);
    let mut buffer = Vec::with_capacity(decoded.len() * 4);
    for v in &decoded {
        buffer.extend_from_slice(&v.to_le_bytes());
    }
    let n = decoded.len() as u32;
    *RESULT_BUFFER.get() = buffer;
    n
}

/// Copie le vecteur brut d'identifiant `id` dans le tampon de resultat
/// interne (utilise notamment par le rendu 3D pour positionner les
/// noeuds de la nebuleuse HNSW). Retourne la dimension, ou 0 si l'index
/// n'est pas initialise ou si `id` est hors bornes.
#[no_mangle]
pub extern "C" fn synapse_get_vector(id: u32) -> u32 {
    match GLOBAL_INDEX.get().as_ref() {
        Some(index) if (id as usize) < index.len() => {
            let vector = index.vector_of(id);
            let mut buffer = Vec::with_capacity(vector.len() * 4);
            for v in vector {
                buffer.extend_from_slice(&v.to_le_bytes());
            }
            let n = vector.len() as u32;
            *RESULT_BUFFER.get() = buffer;
            n
        }
        _ => 0,
    }
}

/// Fraction de reduction memoire obtenue par le Product Quantizer entraine
/// (par exemple 0.9375 pour une reduction de 93,75 %). Retourne -1.0 si
/// aucun quantizer n'est encore entraine.
#[no_mangle]
pub extern "C" fn synapse_pq_memory_reduction_ratio() -> f32 {
    match GLOBAL_PQ.get().as_ref() {
        Some(pq) if pq.trained => pq.memory_reduction_ratio(),
        _ => -1.0,
    }
}

/// Encode le vecteur pointe par `ptr`/`len` avec le Product Quantizer
/// entraine. Ecrit les codes (un octet par sous-vecteur) dans le tampon
/// de resultat interne et retourne leur nombre, ou 0 si aucun quantizer
/// n'est entraine.
#[no_mangle]
pub extern "C" fn synapse_pq_encode(ptr: *const f32, len: u32) -> u32 {
    let slice = unsafe { core::slice::from_raw_parts(ptr, len as usize) };
    match GLOBAL_PQ.get().as_ref() {
        Some(pq) if pq.trained && slice.len() == pq.dim => {
            let codes = pq.encode(slice);
            let count = codes.len() as u32;
            *RESULT_BUFFER.get() = codes;
            count
        }
        _ => 0,
    }
}

/// Exporte tous les vecteurs actuellement indexes au format binaire
/// compact (codec.rs), destine a etre persiste par la couche JS dans
/// IndexedDB. Retourne la taille du tampon exporte (0 si index vide ou
/// non initialise).
#[no_mangle]
pub extern "C" fn synapse_export_snapshot() -> u32 {
    match GLOBAL_INDEX.get().as_ref() {
        Some(index) => {
            let (ids, flat) = index.export_all();
            let encoded = codec::encode_vectors(&ids, &flat, index.dim());
            let len = encoded.len() as u32;
            *RESULT_BUFFER.get() = encoded;
            len
        }
        None => 0,
    }
}

/// Reconstruit l'index HNSW a partir d'un instantane precedemment
/// exporte (typiquement relu depuis IndexedDB par la couche JS et copie
/// dans la memoire lineaire WASM via `synapse_alloc_buffer`). Les
/// vecteurs sont reinseres dans l'ordre : le graphe est reconstruit,
/// pas serialise/deserialise directement.
#[no_mangle]
pub extern "C" fn synapse_import_snapshot(
    ptr: *const u8,
    len: u32,
    metric: u32,
    m: u32,
    ef_construction: u32,
    seed: u64,
) -> i32 {
    let slice = unsafe { core::slice::from_raw_parts(ptr, len as usize) };
    let decoded = match codec::decode_vectors(slice) {
        Some(d) => d,
        None => return -1,
    };

    let mut index = HnswIndex::new(decoded.dim, metric_from_code(metric), m as usize, ef_construction as usize, seed);
    let count = decoded.ids.len();
    for i in 0..count {
        let base = i * decoded.dim;
        index.insert(decoded.flat_vectors[base..base + decoded.dim].to_vec());
    }

    *GLOBAL_INDEX.get() = Some(index);
    count as i32
}
