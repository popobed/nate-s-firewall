/**
 * Point d'entree public du paquet @synapse-mesh/core.
 */

export {
  KademliaRoutingTable,
  SynapseMeshNode,
  generatePeerId,
  type ControlMessage,
  type DhtFindNodeRequest,
  type DhtFindNodeResponse,
  type MeshTransportHandlers,
  type PeerId,
  type RoutablePeer,
  type SignalEnvelope,
  type SignalPayload,
} from "./network/p2p_mesh.js";

export {
  VectorOrSet,
  buildMerkleDag,
  diffMerkleSubtrees,
  resyncViaMerkleDag,
  type MerkleNode,
  type OperationId,
  type VectorOrSetSnapshot,
  type VectorRecord,
} from "./network/crdt_sync.js";

/**
 * Charge le noyau vectoriel WASM (HNSW + SIMD + Product Quantization,
 * `#![no_std]`, ABI C -- voir src/wasm_core/src/lib.rs) et retourne ses
 * exports typés. `wasmUrl` doit pointer vers le fichier
 * `synapse_mesh_core.wasm` compilé (`npm run build:wasm`).
 */
export async function loadSynapseMeshCore(wasmUrl: string | URL): Promise<SynapseMeshCoreExports> {
  const response = await fetch(wasmUrl);
  const bytes = await response.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(bytes, {});
  return instance.exports as unknown as SynapseMeshCoreExports;
}

export interface SynapseMeshCoreExports {
  readonly memory: WebAssembly.Memory;
  synapse_init_index(dim: number, metric: number, m: number, efConstruction: number, seed: bigint): void;
  synapse_alloc_buffer(len: number): number;
  synapse_free_buffer(ptr: number, len: number): void;
  synapse_insert_vector(ptr: number, len: number): bigint;
  synapse_search_knn(ptr: number, len: number, k: number, efSearch: number): number;
  synapse_result_ptr(): number;
  synapse_index_len(): number;
  synapse_get_vector(id: number): number;
  synapse_pq_train(numSubvectors: number, numCentroids: number, iterations: number, seed: bigint): number;
  synapse_pq_memory_reduction_ratio(): number;
  synapse_pq_encode(ptr: number, len: number): number;
  synapse_pq_search_knn(ptr: number, len: number, k: number): number;
  synapse_pq_decode(indexId: number): number;
  synapse_export_snapshot(): number;
  synapse_import_snapshot(
    ptr: number,
    len: number,
    metric: number,
    m: number,
    efConstruction: number,
    seed: bigint,
  ): number;
  synapse_debug_distance(ptrA: number, ptrB: number, len: number, metric: number, useSimd: number): number;
}
