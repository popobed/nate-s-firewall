/**
 * Ingestion documentaire et persistance IndexedDB.
 *
 * Decoupe un document en fragments ("chunks"), les vectorise via
 * `embedder.ts`, les insere dans le graphe HNSW Wasm (SYNAPSE-MESH
 * core), et persiste a la fois les metadonnees de chunk (texte,
 * document d'origine) et un instantane binaire complet de l'index dans
 * IndexedDB, pour une reprise sans perte entre deux sessions du
 * navigateur (aucune reindexation necessaire au rechargement).
 */

import { embedText } from "./embedder.js";
import type { SynapseMeshCoreExports } from "../index.js";

const DB_NAME = "synapse-mesh-rag";
const DB_VERSION = 1;
const STORE_DOCUMENTS = "documents";
const STORE_CHUNKS = "chunks";
const STORE_INDEX_SNAPSHOT = "index_snapshot";
const SNAPSHOT_KEY = "current";

export interface ChunkingOptions {
  readonly maxChars?: number;
  readonly overlapChars?: number;
}

/**
 * Decoupe un texte en fragments de taille bornee, en essayant de couper
 * sur une frontiere de paragraphe ou de mot proche de la limite plutot
 * qu'au milieu d'un mot, avec un chevauchement entre fragments
 * consecutifs pour ne jamais perdre le contexte a une frontiere de
 * coupe. Termine toujours (la position de depart progresse strictement
 * a chaque iteration, quels que soient les parametres).
 */
export function chunkText(text: string, options: ChunkingOptions = {}): string[] {
  const maxChars = Math.max(options.maxChars ?? 800, 50);
  const overlapChars = Math.min(Math.max(options.overlapChars ?? 150, 0), Math.floor(maxChars / 2));

  const normalized = text.replace(/\r\n/g, "\n").trim();
  if (normalized.length === 0) {
    return [];
  }
  if (normalized.length <= maxChars) {
    return [normalized];
  }

  const chunks: string[] = [];
  let start = 0;

  while (start < normalized.length) {
    const hardEnd = Math.min(start + maxChars, normalized.length);
    let end = hardEnd;

    if (hardEnd < normalized.length) {
      const searchZoneStart = start + Math.floor(maxChars * 0.5);
      const paragraphBoundary = normalized.lastIndexOf("\n\n", hardEnd);
      const spaceBoundary = normalized.lastIndexOf(" ", hardEnd);
      const bestBoundary = Math.max(paragraphBoundary, spaceBoundary);
      if (bestBoundary > searchZoneStart) {
        end = bestBoundary;
      }
    }

    const chunk = normalized.slice(start, end).trim();
    if (chunk.length > 0) {
      chunks.push(chunk);
    }

    if (end >= normalized.length) {
      break;
    }

    const nextStart = end - overlapChars;
    start = nextStart > start ? nextStart : end; // progression stricte garantie
  }

  return chunks;
}

export interface DocumentRecord {
  readonly documentId: string;
  readonly filename: string;
  readonly importedAtMs: number;
  readonly chunkCount: number;
}

export interface ChunkRecord {
  readonly chunkId: string;
  readonly documentId: string;
  readonly filename: string;
  readonly text: string;
  readonly vectorNodeId: number;
}

export interface SearchResult {
  readonly chunkId: string;
  readonly documentId: string;
  readonly filename: string;
  readonly text: string;
  readonly similarity: number; // cosinus, dans approximativement [-1, 1] ; 1 = identique
}

export interface SearchOutcome {
  readonly results: readonly SearchResult[];
  readonly latencyMs: number;
}

function promisifyRequest<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

function transactionDone(tx: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

function openDatabase(databaseName: string): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(databaseName, DB_VERSION);

    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(STORE_DOCUMENTS)) {
        db.createObjectStore(STORE_DOCUMENTS, { keyPath: "documentId" });
      }
      if (!db.objectStoreNames.contains(STORE_CHUNKS)) {
        const chunkStore = db.createObjectStore(STORE_CHUNKS, { keyPath: "chunkId" });
        chunkStore.createIndex("byDocumentId", "documentId", { unique: false });
      }
      if (!db.objectStoreNames.contains(STORE_INDEX_SNAPSHOT)) {
        db.createObjectStore(STORE_INDEX_SNAPSHOT);
      }
    };

    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
}

function cosineDistanceToSimilarity(distance: number): number {
  // Le noyau Wasm definit cosine_distance = 1 - cos(theta) ; la
  // similarite cosinus standard est donc simplement (1 - distance).
  return 1 - distance;
}

function generateId(): string {
  if (typeof crypto.randomUUID === "function") {
    return crypto.randomUUID();
  }
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

/**
 * Orchestration complete : ingestion (chunk -> embed -> insertion HNSW
 * -> persistance IndexedDB) et recherche semantique. `wasm` est
 * injecte (typiquement obtenu via `loadSynapseMeshCore`) pour rester
 * testable independamment du binaire Wasm reel (voir storage_manager.test.ts).
 */
export class DocumentStore {
  private readonly dbPromise: Promise<IDBDatabase>;

  /**
   * `databaseName` est parametrable (par defaut, la base partagee de
   * l'application) afin qu'une suite de tests puisse instancier
   * plusieurs `DocumentStore` totalement isoles les uns des autres, sans
   * qu'une connexion jamais fermee d'une instance precedente ne bloque
   * indefiniment l'ouverture ou la suppression de la base d'une autre.
   */
  constructor(
    private readonly wasm: SynapseMeshCoreExports,
    databaseName: string = DB_NAME,
  ) {
    this.dbPromise = openDatabase(databaseName);
  }

  private db(): Promise<IDBDatabase> {
    return this.dbPromise;
  }

  /** Ferme la connexion IndexedDB sous-jacente. A appeler quand l'instance n'est plus utilisee (ex : teardown de test). */
  async close(): Promise<void> {
    (await this.dbPromise).close();
  }

  async ingestDocument(filename: string, content: string, options?: ChunkingOptions): Promise<DocumentRecord> {
    const chunks = chunkText(content, options);
    if (chunks.length === 0) {
      throw new Error(`Le document "${filename}" ne contient aucun texte exploitable.`);
    }

    const documentId = generateId();
    const chunkRecords: ChunkRecord[] = [];

    for (let i = 0; i < chunks.length; i += 1) {
      const chunkContent = chunks[i]!;
      const vector = await embedText(chunkContent);

      const ptr = this.wasm.synapse_alloc_buffer(vector.length * 4);
      new Float32Array(this.wasm.memory.buffer, ptr, vector.length).set(vector);
      const nodeId = this.wasm.synapse_insert_vector(ptr, vector.length);
      this.wasm.synapse_free_buffer(ptr, vector.length * 4);

      if (nodeId < 0) {
        throw new Error(`Insertion vectorielle refusee (code ${nodeId}) pour un extrait de "${filename}".`);
      }

      chunkRecords.push({
        chunkId: `${documentId}:${i}`,
        documentId,
        filename,
        text: chunkContent,
        vectorNodeId: Number(nodeId),
      });
    }

    const documentRecord: DocumentRecord = {
      documentId,
      filename,
      importedAtMs: Date.now(),
      chunkCount: chunkRecords.length,
    };

    const db = await this.db();
    const tx = db.transaction([STORE_DOCUMENTS, STORE_CHUNKS], "readwrite");
    tx.objectStore(STORE_DOCUMENTS).put(documentRecord);
    const chunkStore = tx.objectStore(STORE_CHUNKS);
    for (const record of chunkRecords) {
      chunkStore.put(record);
    }
    await transactionDone(tx);

    await this.persistIndexSnapshot();
    return documentRecord;
  }

  /** Sauvegarde l'instantane binaire complet de l'index HNSW dans IndexedDB. */
  async persistIndexSnapshot(): Promise<void> {
    const len = this.wasm.synapse_export_snapshot();
    if (len === 0) {
      return;
    }

    const ptr = this.wasm.synapse_result_ptr();
    // Copie hors de la memoire lineaire Wasm : celle-ci peut etre
    // reallouee (memory.grow) par un appel ulterieur, ce qui invaliderait
    // toute vue non copiee.
    const bytes = new Uint8Array(this.wasm.memory.buffer, ptr, len).slice();

    const db = await this.db();
    const tx = db.transaction(STORE_INDEX_SNAPSHOT, "readwrite");
    tx.objectStore(STORE_INDEX_SNAPSHOT).put(bytes, SNAPSHOT_KEY);
    await transactionDone(tx);
  }

  /**
   * Reconstruit l'index HNSW Wasm a partir de l'instantane persiste.
   * A appeler au demarrage de l'application, avant toute recherche.
   * Retourne le nombre de vecteurs restaures (0 si aucun instantane).
   */
  async restoreIndexSnapshot(metric = 1, m = 12, efConstruction = 64, seed = 1n): Promise<number> {
    const db = await this.db();
    const tx = db.transaction(STORE_INDEX_SNAPSHOT, "readonly");
    const snapshot = await promisifyRequest<Uint8Array | undefined>(
      tx.objectStore(STORE_INDEX_SNAPSHOT).get(SNAPSHOT_KEY),
    );
    await transactionDone(tx);

    if (!snapshot || snapshot.length === 0) {
      return 0;
    }

    const ptr = this.wasm.synapse_alloc_buffer(snapshot.length);
    new Uint8Array(this.wasm.memory.buffer, ptr, snapshot.length).set(snapshot);
    const count = this.wasm.synapse_import_snapshot(ptr, snapshot.length, metric, m, efConstruction, seed);
    this.wasm.synapse_free_buffer(ptr, snapshot.length);
    return count;
  }

  async listDocuments(): Promise<DocumentRecord[]> {
    const db = await this.db();
    const tx = db.transaction(STORE_DOCUMENTS, "readonly");
    const records = await promisifyRequest<DocumentRecord[]>(tx.objectStore(STORE_DOCUMENTS).getAll());
    await transactionDone(tx);
    return records;
  }

  /** Tous les fragments actuellement indexes, toutes sources confondues (utilise notamment pour la visualisation 3D). */
  async listAllChunks(): Promise<ChunkRecord[]> {
    const db = await this.db();
    const tx = db.transaction(STORE_CHUNKS, "readonly");
    const records = await promisifyRequest<ChunkRecord[]>(tx.objectStore(STORE_CHUNKS).getAll());
    await transactionDone(tx);
    return records;
  }

  /** Vecteur brut d'un fragment, tel que stocke dans le graphe HNSW Wasm. */
  getChunkVector(vectorNodeId: number): Float32Array | null {
    const len = this.wasm.synapse_get_vector(vectorNodeId);
    if (len === 0) {
      return null;
    }
    const ptr = this.wasm.synapse_result_ptr();
    return new Float32Array(this.wasm.memory.buffer, ptr, len).slice();
  }

  private async getChunksByVectorNodeIds(vectorNodeIds: readonly number[]): Promise<Map<number, ChunkRecord>> {
    const db = await this.db();
    const tx = db.transaction(STORE_CHUNKS, "readonly");
    const all = await promisifyRequest<ChunkRecord[]>(tx.objectStore(STORE_CHUNKS).getAll());
    await transactionDone(tx);

    const wanted = new Set(vectorNodeIds);
    const map = new Map<number, ChunkRecord>();
    for (const record of all) {
      if (wanted.has(record.vectorNodeId)) {
        map.set(record.vectorNodeId, record);
      }
    }
    return map;
  }

  /** Recherche semantique en langage naturel : encode la requete, interroge le graphe HNSW, resout les extraits correspondants. */
  async search(queryText: string, k = 5, efSearch = 64): Promise<SearchOutcome> {
    const startedAt = performance.now();

    const queryVector = await embedText(queryText);
    const ptr = this.wasm.synapse_alloc_buffer(queryVector.length * 4);
    new Float32Array(this.wasm.memory.buffer, ptr, queryVector.length).set(queryVector);

    const count = this.wasm.synapse_search_knn(ptr, queryVector.length, k, efSearch);
    this.wasm.synapse_free_buffer(ptr, queryVector.length * 4);

    const resultPtr = this.wasm.synapse_result_ptr();
    const resultView = new DataView(this.wasm.memory.buffer, resultPtr, count * 8);
    const rawResults: Array<{ nodeId: number; distance: number }> = [];
    for (let i = 0; i < count; i += 1) {
      rawResults.push({
        nodeId: resultView.getUint32(i * 8, true),
        distance: resultView.getFloat32(i * 8 + 4, true),
      });
    }

    const chunkMap = await this.getChunksByVectorNodeIds(rawResults.map((r) => r.nodeId));
    const latencyMs = performance.now() - startedAt;

    const results: SearchResult[] = [];
    for (const raw of rawResults) {
      const chunk = chunkMap.get(raw.nodeId);
      if (!chunk) {
        continue; // instantane restaure sans les metadonnees de chunk correspondantes (incoherence, ignoree)
      }
      results.push({
        chunkId: chunk.chunkId,
        documentId: chunk.documentId,
        filename: chunk.filename,
        text: chunk.text,
        similarity: cosineDistanceToSimilarity(raw.distance),
      });
    }

    return { results, latencyMs };
  }
}
