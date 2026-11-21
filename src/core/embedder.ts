/**
 * Pipeline d'embeddings de texte 100% client-side (transformers.js /
 * @huggingface/transformers), execute localement dans le navigateur via
 * ONNX Runtime Web (WASM/WebGPU) : aucun texte ne quitte jamais le
 * poste de l'utilisateur, condition necessaire pour indexer des
 * documents d'entreprise sensibles.
 *
 * Modele : Xenova/all-MiniLM-L6-v2 -- 384 dimensions, pooling moyen,
 * normalise (le produit scalaire entre deux embeddings normalises est
 * alors directement leur similarite cosinus, ce qui correspond a la
 * metrique "cosine" du noyau HNSW Wasm ; voir p2p/wasm_core DistanceMetric).
 */

import { pipeline, type FeatureExtractionPipeline, type ProgressInfo } from "@huggingface/transformers";

export const EMBEDDING_MODEL_ID = "Xenova/all-MiniLM-L6-v2";
export const EMBEDDING_DIM = 384;

export type EmbedderLoadProgress = ProgressInfo;

let pipelinePromise: Promise<FeatureExtractionPipeline> | null = null;

function loadPipeline(onProgress?: (progress: EmbedderLoadProgress) => void): Promise<FeatureExtractionPipeline> {
  if (!pipelinePromise) {
    pipelinePromise = pipeline("feature-extraction", EMBEDDING_MODEL_ID, {
      progress_callback: onProgress,
    });
  }
  return pipelinePromise;
}

/**
 * Precharge le modele (poids ONNX ~ quelques dizaines de Mo, mis en
 * cache par le navigateur apres le premier chargement). A appeler tot
 * (ex. au montage de l'application) pour eviter que la latence de
 * telechargement retombe sur la premiere recherche utilisateur.
 */
export async function preloadEmbedder(onProgress?: (progress: EmbedderLoadProgress) => void): Promise<void> {
  await loadPipeline(onProgress);
}

/** Encode un texte en un vecteur de EMBEDDING_DIM flottants, normalise (norme L2 = 1). */
export async function embedText(text: string): Promise<Float32Array> {
  const extractor = await loadPipeline();
  const output = await extractor(text, { pooling: "mean", normalize: true });
  return Float32Array.from(output.data as Float32Array);
}

/**
 * Encode plusieurs textes. Implementee en appels sequentiels plutot
 * qu'en un unique forward-pass batch : plus simple et plus robuste (pas
 * de reconstruction manuelle des dimensions du Tensor par lot), au prix
 * d'un debit legerement moindre -- acceptable pour l'ingestion de
 * documents, qui n'est pas sur le chemin critique de latence de
 * recherche.
 */
export async function embedBatch(
  texts: readonly string[],
  onEachDone?: (index: number, total: number) => void,
): Promise<Float32Array[]> {
  const extractor = await loadPipeline();
  const vectors: Float32Array[] = [];

  for (let i = 0; i < texts.length; i += 1) {
    const output = await extractor(texts[i]!, { pooling: "mean", normalize: true });
    vectors.push(Float32Array.from(output.data as Float32Array));
    onEachDone?.(i + 1, texts.length);
  }

  return vectors;
}
