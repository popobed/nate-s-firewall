import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

// Resolu par rapport a l'emplacement de CE fichier, jamais par rapport
// au repertoire de travail courant (le script est invoque a la fois
// depuis src/wasm_core/ en local et depuis la racine du depot via
// `npm run test:wasm`).
const wasmPath = fileURLToPath(new URL("../target/wasm32-unknown-unknown/release/synapse_mesh_core.wasm", import.meta.url));
const bytes = await readFile(wasmPath);
const { instance } = await WebAssembly.instantiate(bytes, {});
const exp = instance.exports;
const memF32 = () => new Float32Array(exp.memory.buffer);
const memU8 = () => new Uint8Array(exp.memory.buffer);
const memU32 = () => new Uint32Array(exp.memory.buffer);

function writeVector(values) {
    const ptr = exp.synapse_alloc_buffer(values.length * 4);
    const view = new Float32Array(exp.memory.buffer, ptr, values.length);
    view.set(values);
    return ptr;
}

function assert(cond, msg) {
    if (!cond) throw new Error("ECHEC: " + msg);
    console.log("OK  ", msg);
}

// --- 1. synapse_ping n'existe plus (remplace) : verification croisee SIMD/scalaire ---
{
    const a = writeVector([1, 2, 3, 4, 5, 6, 7, 8, 9]);
    const b = writeVector([9, 1, 8, 2, 7, 3, 6, 4, 0.5]);
    const scalarEuclid = exp.synapse_debug_distance(a, b, 9, 0, 0);
    const simdEuclid = exp.synapse_debug_distance(a, b, 9, 0, 1);
    const scalarCos = exp.synapse_debug_distance(a, b, 9, 1, 0);
    const simdCos = exp.synapse_debug_distance(a, b, 9, 1, 1);
    console.log(`    euclid scalar=${scalarEuclid} simd=${simdEuclid}`);
    console.log(`    cosine scalar=${scalarCos} simd=${simdCos}`);
    assert(Math.abs(scalarEuclid - simdEuclid) < 1e-3, "distance euclidienne SIMD == scalaire (longueur non multiple de 4)");
    assert(Math.abs(scalarCos - simdCos) < 1e-3, "distance cosinus SIMD == scalaire (longueur non multiple de 4)");
}

// --- 2. HNSW : insertion + recherche exacte sur un petit jeu connu ---
{
    const dim = 4;
    exp.synapse_init_index(dim, 0 /* euclidienne */, 8, 32, 1234n);

    const points = [
        [0, 0, 0, 0],
        [10, 0, 0, 0],
        [0, 10, 0, 0],
        [5, 5, 5, 5],
        [100, 100, 100, 100],
        [1, 1, 1, 1],
        [9, 0, 0, 1],
        [50, 50, 0, 0],
    ];

    for (const p of points) {
        const ptr = writeVector(p);
        const id = exp.synapse_insert_vector(ptr, dim);
        assert(id >= 0, `insertion reussie pour ${JSON.stringify(p)} (id=${id})`);
    }

    assert(exp.synapse_index_len() === points.length, `index_len == ${points.length}`);

    // Requete proche de [1,1,1,1] (id 5) et [0,0,0,0] (id 0).
    const queryPtr = writeVector([0.9, 0.9, 0.9, 0.9]);
    const count = exp.synapse_search_knn(queryPtr, dim, 3, 32);
    assert(count === 3, "search_knn retourne k=3 resultats");

    const resultPtr = exp.synapse_result_ptr();
    const buf = new DataView(exp.memory.buffer, resultPtr, count * 8);
    const results = [];
    for (let i = 0; i < count; i++) {
        const id = buf.getUint32(i * 8, true);
        const dist = buf.getFloat32(i * 8 + 4, true);
        results.push({ id, dist });
    }
    console.log("    resultats k-NN:", results);
    assert(results[0].id === 5, "le plus proche voisin de (0.9,0.9,0.9,0.9) est bien le point [1,1,1,1] (id 5)");
    assert(results[0].dist < results[1].dist, "les resultats sont tries par distance croissante");
}

// --- 3. Recherche a plus grande echelle (verifie le log N / graphe multi-couches) ---
{
    const dim = 8;
    exp.synapse_init_index(dim, 0, 12, 64, 42n);
    const N = 500;
    const vectors = [];
    for (let i = 0; i < N; i++) {
        const v = Array.from({ length: dim }, () => Math.random() * 100);
        vectors.push(v);
        const ptr = writeVector(v);
        exp.synapse_insert_vector(ptr, dim);
    }
    assert(exp.synapse_index_len() === N, `index a grande echelle : ${N} vecteurs inseres`);

    // Verite terrain par recherche exhaustive cote JS.
    const query = Array.from({ length: dim }, () => Math.random() * 100);
    const bruteForce = vectors
        .map((v, id) => ({ id, dist: Math.sqrt(v.reduce((s, x, i) => s + (x - query[i]) ** 2, 0)) }))
        .sort((a, b) => a.dist - b.dist)
        .slice(0, 5);

    const qPtr = writeVector(query);
    const t0 = performance.now();
    const count = exp.synapse_search_knn(qPtr, dim, 5, 64);
    const t1 = performance.now();
    const resultPtr = exp.synapse_result_ptr();
    const buf = new DataView(exp.memory.buffer, resultPtr, count * 8);
    const approx = [];
    for (let i = 0; i < count; i++) {
        approx.push({ id: buf.getUint32(i * 8, true), dist: buf.getFloat32(i * 8 + 4, true) });
    }

    console.log("    verite terrain (brute force):", bruteForce.map((r) => r.id));
    console.log("    HNSW approx:                 ", approx.map((r) => r.id));
    console.log(`    latence recherche (${N} vecteurs, dim=${dim}) : ${(t1 - t0).toFixed(3)} ms`);

    assert(approx[0].id === bruteForce[0].id, "HNSW retrouve le VRAI plus proche voisin sur 500 points aleatoires");
    const overlap = approx.filter((r) => bruteForce.some((b) => b.id === r.id)).length;
    assert(overlap >= 4, `rappel HNSW eleve sur top-5 (${overlap}/5 en commun avec la verite terrain)`);
}

// --- 4. Product Quantization : entrainement + reduction memoire + recherche ADC ---
{
    const trainResult = exp.synapse_pq_train(4 /* num_subvectors */, 16 /* num_centroids */, 10 /* iterations */, 7n);
    assert(trainResult === 0, "entrainement PQ reussi (dim=8 divisible par 4 sous-vecteurs)");

    const ratio = exp.synapse_pq_memory_reduction_ratio();
    console.log(`    reduction memoire PQ : ${(ratio * 100).toFixed(2)}%`);
    assert(ratio > 0.7, "reduction memoire PQ superieure a 70% (cible >= 75% annoncee)");

    const query = writeVector(Array.from({ length: 8 }, () => Math.random() * 100));
    const pqCount = exp.synapse_pq_search_knn(query, 8, 5);
    assert(pqCount > 0, "recherche ADC sur codes quantifies retourne des resultats");
}

console.log("\nTOUS LES TESTS D'INTEGRATION WASM ONT REUSSI.");
