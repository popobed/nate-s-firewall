import { afterEach, describe, expect, it, vi } from "vitest";
import { chunkText, DocumentStore } from "./storage_manager.js";
import type { SynapseMeshCoreExports } from "../index.js";

// storage_manager.ts n'importe que `embedText` de ce module : on le
// remplace par un plongement deterministe (hachage du texte), afin de
// tester l'ORCHESTRATION (decoupage, insertion, persistance, recherche,
// resolution des metadonnees) independamment du modele transformers.js
// reel, qui ne peut pas s'executer dans cet environnement Node (voir
// embedder.ts : execution navigateur uniquement).
vi.mock("./embedder.js", () => ({
  embedText: vi.fn(async (text: string) => deterministicEmbedding(text, FAKE_DIM)),
}));

const FAKE_DIM = 8;

function deterministicEmbedding(text: string, dim: number): Float32Array {
  const vector = new Float32Array(dim);
  let hash = 2166136261;
  for (let i = 0; i < text.length; i += 1) {
    hash = (hash ^ text.charCodeAt(i)) * 16777619;
    const slot = i % dim;
    vector[slot] = vector[slot]! + ((hash >>> 0) % 1000);
  }
  let norm = 0;
  for (let i = 0; i < dim; i += 1) norm += vector[i]! * vector[i]!;
  norm = Math.sqrt(norm) || 1;
  for (let i = 0; i < dim; i += 1) vector[i] = vector[i]! / norm;
  return vector;
}

/**
 * Double de test du noyau Wasm : implemente la MEME interface
 * (SynapseMeshCoreExports) avec une logique en JavaScript pur (brute
 * force, pas de HNSW), suffisante pour verifier que storage_manager.ts
 * appelle les bonnes fonctions, dans le bon ordre, avec les bonnes
 * tailles de tampon. La correction de l'algorithme HNSW/SIMD/PQ lui-meme
 * est verifiee separement dans src/wasm_core/tests/integration.mjs
 * contre le vrai binaire compile.
 */
function createFakeWasmExports(): SynapseMeshCoreExports {
  const memory = new WebAssembly.Memory({ initial: 16 });
  let bumpOffset = 8;
  let lastResultPtr = 0;
  const vectors: Float32Array[] = [];

  function alloc(len: number): number {
    const needed = bumpOffset + len;
    if (needed > memory.buffer.byteLength) {
      const pages = Math.ceil((needed - memory.buffer.byteLength) / 65536);
      memory.grow(pages);
    }
    const ptr = bumpOffset;
    bumpOffset += len;
    return ptr;
  }

  function cosineDistance(a: Float32Array, b: Float32Array): number {
    let dot = 0;
    let normA = 0;
    let normB = 0;
    for (let i = 0; i < a.length; i += 1) {
      dot += a[i]! * b[i]!;
      normA += a[i]! * a[i]!;
      normB += b[i]! * b[i]!;
    }
    if (normA === 0 || normB === 0) return 1;
    return 1 - dot / (Math.sqrt(normA) * Math.sqrt(normB));
  }

  return {
    memory,
    synapse_init_index: () => {
      vectors.length = 0;
    },
    synapse_alloc_buffer: (len) => alloc(len),
    synapse_free_buffer: () => {},
    synapse_insert_vector: (ptr, len) => {
      const vector = new Float32Array(memory.buffer, ptr, len).slice();
      vectors.push(vector);
      return BigInt(vectors.length - 1);
    },
    synapse_search_knn: (ptr, len, k) => {
      const query = new Float32Array(memory.buffer, ptr, len).slice();
      const scored = vectors.map((vector, id) => ({ id, distance: cosineDistance(query, vector) }));
      scored.sort((a, b) => a.distance - b.distance);
      const top = scored.slice(0, k);

      const resultPtr = alloc(top.length * 8);
      const view = new DataView(memory.buffer, resultPtr, top.length * 8);
      top.forEach((entry, i) => {
        view.setUint32(i * 8, entry.id, true);
        view.setFloat32(i * 8 + 4, entry.distance, true);
      });
      lastResultPtr = resultPtr;
      return top.length;
    },
    synapse_result_ptr: () => lastResultPtr,
    synapse_index_len: () => vectors.length,
    synapse_get_vector: () => 0,
    synapse_pq_train: () => -1,
    synapse_pq_memory_reduction_ratio: () => -1,
    synapse_pq_encode: () => 0,
    synapse_pq_search_knn: () => 0,
    synapse_pq_decode: () => 0,
    synapse_export_snapshot: () => {
      const count = vectors.length;
      const dim = count > 0 ? vectors[0]!.length : FAKE_DIM;
      const bufLen = 8 + count * dim * 4;
      const ptr = alloc(bufLen);
      const view = new DataView(memory.buffer, ptr, bufLen);
      view.setUint32(0, count, true);
      view.setUint32(4, dim, true);
      let offset = 8;
      for (const vector of vectors) {
        for (let i = 0; i < dim; i += 1) {
          view.setFloat32(offset, vector[i]!, true);
          offset += 4;
        }
      }
      lastResultPtr = ptr;
      return bufLen;
    },
    synapse_import_snapshot: (ptr, len) => {
      const view = new DataView(memory.buffer, ptr, len);
      const count = view.getUint32(0, true);
      const dim = view.getUint32(4, true);
      vectors.length = 0;
      let offset = 8;
      for (let i = 0; i < count; i += 1) {
        const vector = new Float32Array(dim);
        for (let d = 0; d < dim; d += 1) {
          vector[d] = view.getFloat32(offset, true);
          offset += 4;
        }
        vectors.push(vector);
      }
      return count;
    },
    synapse_debug_distance: () => 0,
  };
}

describe("chunkText", () => {
  it("returns an empty array for empty input", () => {
    expect(chunkText("   ")).toEqual([]);
  });

  it("returns a single chunk when the text fits within maxChars", () => {
    expect(chunkText("bonjour le monde", { maxChars: 100 })).toEqual(["bonjour le monde"]);
  });

  it("splits long text into multiple overlapping chunks and always terminates", () => {
    const longText = Array.from({ length: 50 }, (_, i) => `phrase numero ${i} avec un peu de texte`).join(" ");
    const chunks = chunkText(longText, { maxChars: 120, overlapChars: 30 });

    expect(chunks.length).toBeGreaterThan(1);
    for (const chunk of chunks) {
      expect(chunk.length).toBeLessThanOrEqual(120 + 30); // marge pour la recherche de frontiere de mot
    }
    // Le texte reconstitue doit contenir l'integralite du contenu original (le chevauchement duplique, jamais n'omet).
    expect(chunks.join(" ")).toContain("phrase numero 0");
    expect(chunks.join(" ")).toContain("phrase numero 49");
  });

  it("never loops forever even with pathological options", () => {
    const text = "x".repeat(5000);
    const chunks = chunkText(text, { maxChars: 60, overlapChars: 1000 }); // overlap > maxChars, doit etre borne en interne
    expect(chunks.length).toBeGreaterThan(1);
    expect(chunks.length).toBeLessThan(500); // progression garantie -> nombre de chunks borne
  });
});

describe("DocumentStore", () => {
  // Chaque test recoit sa propre base IndexedDB nommee de maniere unique :
  // une instance DocumentStore ne ferme jamais sa connexion de son propre
  // chef (par design -- l'application la garde ouverte tout au long de la
  // session), donc reutiliser un nom fixe entre tests ferait s'accumuler
  // des connexions jamais fermees, ce qui bloque indefiniment tout
  // `deleteDatabase`/`open` ulterieur sur ce meme nom.
  let dbCounter = 0;
  function freshDatabaseName(): string {
    dbCounter += 1;
    return `synapse-mesh-rag-test-${dbCounter}`;
  }

  const openStores: DocumentStore[] = [];
  function trackedStore(wasm: SynapseMeshCoreExports, databaseName: string): DocumentStore {
    const store = new DocumentStore(wasm, databaseName);
    openStores.push(store);
    return store;
  }

  afterEach(async () => {
    await Promise.all(openStores.map((store) => store.close()));
    openStores.length = 0;
  });

  it("ingests a document, persists it, and finds it back by semantic search", async () => {
    const wasm = createFakeWasmExports();
    const store = trackedStore(wasm, freshDatabaseName());

    const doc = await store.ingestDocument(
      "notes.md",
      "Le chat dort sur le canape. Les taux d'interet europeens augmentent fortement.",
      { maxChars: 40, overlapChars: 5 },
    );

    expect(doc.chunkCount).toBeGreaterThan(0);
    expect(wasm.synapse_index_len()).toBe(doc.chunkCount);

    const documents = await store.listDocuments();
    expect(documents).toHaveLength(1);
    expect(documents[0]!.filename).toBe("notes.md");

    // Requete proche d'un chunk ingere (le decoupage peut avoir legerement
    // modifie ses limites exactes) : avec un plongement deterministe,
    // le document source doit malgre tout ressortir en tout premier, avec
    // une similarite elevee.
    const { results, latencyMs } = await store.search("Le chat dort sur le canape.", 3);

    expect(results.length).toBeGreaterThan(0);
    expect(results[0]!.filename).toBe("notes.md");
    expect(results[0]!.similarity).toBeGreaterThan(0.9);
    expect(latencyMs).toBeGreaterThanOrEqual(0);
  });

  it("survives a full snapshot round-trip through IndexedDB (persistence across sessions)", async () => {
    const databaseName = freshDatabaseName();

    const wasmA = createFakeWasmExports();
    const storeA = trackedStore(wasmA, databaseName);
    await storeA.ingestDocument("a.txt", "Contenu unique pour verifier la persistance.", { maxChars: 500 });
    await storeA.close();

    // Nouvelle "session" : nouveau noyau Wasm vierge, nouvelle instance
    // DocumentStore, mais la MEME base IndexedDB (meme nom).
    const wasmB = createFakeWasmExports();
    const storeB = trackedStore(wasmB, databaseName);
    const restoredCount = await storeB.restoreIndexSnapshot();

    expect(restoredCount).toBe(1);
    expect(wasmB.synapse_index_len()).toBe(1);

    const { results } = await storeB.search("Contenu unique pour verifier la persistance.", 1);
    expect(results[0]!.filename).toBe("a.txt");
  });

  it("rejects a document with no exploitable text", async () => {
    const wasm = createFakeWasmExports();
    const store = trackedStore(wasm, freshDatabaseName());
    await expect(store.ingestDocument("vide.txt", "   \n  ")).rejects.toThrow();
  });
});
