const LATENCY_TARGET_MS = 2.0;

/** Resultats de recherche semantique, classes par score de similarite cosinus exact. */
export default function ResultsPanel({ results, latencyMs, lastQuery }) {
  if (lastQuery == null) {
    return (
      <div style={emptyStateStyle}>
        Importez des documents puis lancez une recherche pour voir apparaitre les extraits les plus pertinents.
      </div>
    );
  }

  if (results.length === 0) {
    return (
      <div style={emptyStateStyle}>
        Aucun resultat pour « {lastQuery} ». Importez davantage de documents ou reformulez votre recherche.
      </div>
    );
  }

  const underTarget = latencyMs != null && latencyMs < LATENCY_TARGET_MS;

  return (
    <div>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline", marginBottom: 12 }}>
        <span style={{ color: "#94a3b8", fontSize: 13 }}>
          {results.length} resultat{results.length > 1 ? "s" : ""} pour « {lastQuery} »
        </span>
        {latencyMs != null ? (
          <span
            style={{
              fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace",
              fontSize: 12,
              color: underTarget ? "#4ade80" : "#f87171",
            }}
          >
            {latencyMs.toFixed(3)} ms
          </span>
        ) : null}
      </div>

      <div style={{ display: "flex", flexDirection: "column", gap: 10 }}>
        {results.map((result) => (
          <ResultCard key={result.chunkId} result={result} />
        ))}
      </div>
    </div>
  );
}

function ResultCard({ result }) {
  const percent = Math.max(0, Math.min(1, result.similarity)) * 100;
  return (
    <div
      style={{
        background: "#0f1420",
        border: "1px solid #1e293b",
        borderRadius: 10,
        padding: "14px 16px",
      }}
    >
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 8 }}>
        <span style={{ color: "#7dd3fc", fontSize: 13, fontWeight: 600 }}>{result.filename}</span>
        <span
          style={{
            fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace",
            fontSize: 12,
            color: "#4ade80",
            background: "rgba(74, 222, 128, 0.1)",
            borderRadius: 6,
            padding: "2px 8px",
          }}
        >
          {percent.toFixed(1)}% pertinent
        </span>
      </div>
      <p style={{ color: "#cbd5e1", fontSize: 14, lineHeight: 1.5, margin: 0, whiteSpace: "pre-wrap" }}>
        {result.text}
      </p>
    </div>
  );
}

const emptyStateStyle = {
  color: "#64748b",
  fontSize: 14,
  padding: "24px 8px",
  textAlign: "center",
};
