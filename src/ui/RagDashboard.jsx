import { useCallback, useEffect, useRef, useState } from "react";
import { loadSynapseMeshCore } from "../index.ts";
import { EMBEDDING_DIM, preloadEmbedder } from "../core/embedder.ts";
import { DocumentStore } from "../core/storage_manager.ts";
import DocumentImporter from "./DocumentImporter.jsx";
import ResultsPanel from "./ResultsPanel.jsx";
import SearchBar from "./SearchBar.jsx";
import VectorMesh3D from "./VectorMesh3D.jsx";

const WASM_URL = new URL(
  "../wasm_core/target/wasm32-unknown-unknown/release/synapse_mesh_core.wasm",
  import.meta.url,
);

const COSINE_METRIC = 1;

/**
 * Tableau de bord metier SYNAPSE-MESH : recherche semantique locale
 * (RAG) sur des documents importes par l'utilisateur, sans jamais
 * envoyer ni le contenu ni les requetes hors du navigateur.
 */
export default function RagDashboard() {
  const storeRef = useRef(null);
  const wasmRef = useRef(null);
  const [status, setStatus] = useState("loading-core"); // loading-core -> loading-model -> ready
  const [modelProgress, setModelProgress] = useState(null);
  const [documents, setDocuments] = useState([]);
  const [importing, setImporting] = useState(false);
  const [importError, setImportError] = useState(null);
  const [results, setResults] = useState([]);
  const [lastQuery, setLastQuery] = useState(null);
  const [lastLatencyMs, setLastLatencyMs] = useState(null);
  const [chunkVectors, setChunkVectors] = useState([]);

  const refreshVisualization = useCallback(async () => {
    const store = storeRef.current;
    if (!store) return;
    const chunks = await store.listAllChunks();
    const vectors = chunks
      .map((chunk) => store.getChunkVector(chunk.vectorNodeId))
      .filter((vector) => vector !== null);
    setChunkVectors(vectors);
  }, []);

  useEffect(() => {
    let cancelled = false;

    (async () => {
      const wasmExports = await loadSynapseMeshCore(WASM_URL);
      if (cancelled) return;

      wasmExports.synapse_init_index(EMBEDDING_DIM, COSINE_METRIC, 16, 96, BigInt(Date.now()));
      const store = new DocumentStore(wasmExports);
      const restoredCount = await store.restoreIndexSnapshot(COSINE_METRIC, 16, 96, 1n);
      if (cancelled) return;

      storeRef.current = store;
      wasmRef.current = wasmExports;
      if (restoredCount > 0) {
        setDocuments(await store.listDocuments());
        await refreshVisualization();
      }
      setStatus("loading-model");

      await preloadEmbedder((progress) => {
        if (!cancelled) setModelProgress(progress);
      });
      if (cancelled) return;

      setStatus("ready");
    })().catch((error) => {
      console.error("Echec d'initialisation de SYNAPSE-MESH", error);
      if (!cancelled) setStatus("error");
    });

    return () => {
      cancelled = true;
    };
  }, []);

  const handleImportFiles = useCallback(async (entries) => {
    const store = storeRef.current;
    if (!store) return;

    setImporting(true);
    setImportError(null);
    try {
      for (const entry of entries) {
        await store.ingestDocument(entry.filename, entry.content);
      }
      setDocuments(await store.listDocuments());
      await refreshVisualization();
    } catch (error) {
      setImportError(error instanceof Error ? error.message : String(error));
    } finally {
      setImporting(false);
    }
  }, []);

  const handleSearch = useCallback(async (query) => {
    const store = storeRef.current;
    if (!store) return;

    setLastQuery(query);
    const outcome = await store.search(query, 5, 96);
    setResults(outcome.results);
    setLastLatencyMs(outcome.latencyMs);
  }, []);

  const isReady = status === "ready";
  const disabledReason =
    status === "loading-core"
      ? "Initialisation du noyau vectoriel..."
      : status === "loading-model"
        ? modelProgress?.status
          ? `Chargement du modele d'embeddings (${modelProgress.status})...`
          : "Chargement du modele d'embeddings..."
        : status === "error"
          ? "Une erreur est survenue au demarrage. Voir la console."
          : null;

  return (
    <div style={{ display: "flex", gap: 24, padding: 24, background: "#05070d", minHeight: "100vh" }}>
      <div style={{ flex: "1 1 640px", display: "flex", flexDirection: "column", gap: 20, maxWidth: 760 }}>
        <header>
          <h1 style={{ color: "#e2e8f0", fontSize: 22, margin: 0 }}>SYNAPSE-MESH · Recherche semantique locale</h1>
          <p style={{ color: "#64748b", fontSize: 13, margin: "4px 0 0" }}>
            Vos documents et vos recherches restent sur cet appareil : embeddings et index vectoriel s'executent
            entierement dans le navigateur.
          </p>
        </header>

        <Panel title="Documents">
          <DocumentImporter onImportFiles={handleImportFiles} importing={importing} documents={documents} />
          {importError ? <p style={{ color: "#f87171", fontSize: 12, marginTop: 8 }}>{importError}</p> : null}
        </Panel>

        <Panel title="Recherche">
          <SearchBar onSearch={handleSearch} disabled={!isReady} disabledReason={disabledReason} />
        </Panel>

        <Panel title="Resultats">
          <ResultsPanel results={results} latencyMs={lastLatencyMs} lastQuery={lastQuery} />
        </Panel>
      </div>

      <div style={{ flex: "0 0 auto" }}>
        <VectorMesh3D vectors={chunkVectors} dim={EMBEDDING_DIM} peerIds={[]} activeConnections={[]} width={420} height={420} />
        <p style={{ color: "#475569", fontSize: 11, marginTop: 8, maxWidth: 420 }}>
          Nebuleuse HNSW : projection PCA de l'espace semantique des extraits indexes.
        </p>
      </div>
    </div>
  );
}

function Panel({ title, children }) {
  return (
    <section style={{ background: "#0b0f1a", border: "1px solid #1e293b", borderRadius: 12, padding: 18 }}>
      <h2 style={{ color: "#94a3b8", fontSize: 12, letterSpacing: 1, textTransform: "uppercase", margin: "0 0 12px" }}>
        {title}
      </h2>
      {children}
    </section>
  );
}
