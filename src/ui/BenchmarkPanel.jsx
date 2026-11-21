import { useEffect, useRef, useState } from "react";

const LATENCY_TARGET_MS = 2.0;
const HISTORY_LENGTH = 60;

/**
 * Panneau de benchmark en direct : interroge periodiquement
 * `runBenchmarkTick` (fourni par l'application hote -- typiquement un
 * appel a `synapse_search_knn` sur une requete aleatoire) et affiche la
 * latence de recherche vectorielle, une mini-courbe (sparkline) des
 * dernieres mesures, la taille de l'index et le taux de reduction
 * memoire du Product Quantizer.
 */
export default function BenchmarkPanel({ runBenchmarkTick, intervalMs = 500 }) {
  const [history, setHistory] = useState([]);
  const [snapshot, setSnapshot] = useState({ latencyMs: 0, indexSize: 0, memoryReductionPct: null });
  const canvasRef = useRef(null);

  useEffect(() => {
    if (typeof runBenchmarkTick !== "function") {
      return undefined;
    }

    let cancelled = false;

    const tick = async () => {
      const result = await runBenchmarkTick();
      if (cancelled || !result) {
        return;
      }
      setSnapshot(result);
      setHistory((prev) => {
        const next = [...prev, result.latencyMs];
        return next.length > HISTORY_LENGTH ? next.slice(next.length - HISTORY_LENGTH) : next;
      });
    };

    tick();
    const timer = setInterval(tick, intervalMs);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [runBenchmarkTick, intervalMs]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return;
    }
    const ctx = canvas.getContext("2d");
    const { width, height } = canvas;
    ctx.clearRect(0, 0, width, height);

    if (history.length < 2) {
      return;
    }

    const maxValue = Math.max(LATENCY_TARGET_MS * 1.5, ...history);
    const stepX = width / (HISTORY_LENGTH - 1);
    const startIndex = HISTORY_LENGTH - history.length;

    // Ligne de seuil (2 ms).
    const thresholdY = height - (LATENCY_TARGET_MS / maxValue) * height;
    ctx.strokeStyle = "rgba(248, 113, 113, 0.5)";
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(0, thresholdY);
    ctx.lineTo(width, thresholdY);
    ctx.stroke();
    ctx.setLineDash([]);

    ctx.strokeStyle = "#7dd3fc";
    ctx.lineWidth = 2;
    ctx.beginPath();
    history.forEach((value, idx) => {
      const x = (startIndex + idx) * stepX;
      const y = height - (value / maxValue) * height;
      if (idx === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.stroke();
  }, [history]);

  const average = history.length > 0 ? history.reduce((sum, v) => sum + v, 0) / history.length : 0;
  const underTarget = snapshot.latencyMs > 0 && snapshot.latencyMs < LATENCY_TARGET_MS;

  return (
    <div
      style={{
        background: "#0b0f1a",
        color: "#e2e8f0",
        borderRadius: 12,
        padding: 20,
        fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace",
        minWidth: 320,
        boxShadow: "0 0 30px rgba(125, 211, 252, 0.08)",
      }}
    >
      <div style={{ fontSize: 13, letterSpacing: 1, color: "#94a3b8", marginBottom: 12 }}>
        SYNAPSE-MESH · BENCHMARK EN DIRECT
      </div>

      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline" }}>
        <span style={{ fontSize: 32, fontWeight: 700, color: underTarget ? "#4ade80" : "#f87171" }}>
          {snapshot.latencyMs.toFixed(3)} ms
        </span>
        <span style={{ fontSize: 12, color: "#64748b" }}>cible &lt; {LATENCY_TARGET_MS.toFixed(1)} ms</span>
      </div>

      <canvas ref={canvasRef} width={280} height={70} style={{ marginTop: 10, width: "100%", height: 70 }} />

      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8, marginTop: 14, fontSize: 12 }}>
        <Metric label="latence moyenne" value={`${average.toFixed(3)} ms`} />
        <Metric label="vecteurs indexes" value={snapshot.indexSize.toLocaleString("fr-FR")} />
        <Metric
          label="reduction memoire PQ"
          value={snapshot.memoryReductionPct == null ? "non entraine" : `${snapshot.memoryReductionPct.toFixed(1)}%`}
        />
        <Metric label="echantillons" value={`${history.length}/${HISTORY_LENGTH}`} />
      </div>
    </div>
  );
}

function Metric({ label, value }) {
  return (
    <div style={{ background: "#111827", borderRadius: 8, padding: "8px 10px" }}>
      <div style={{ color: "#64748b", fontSize: 10, textTransform: "uppercase", letterSpacing: 0.5 }}>{label}</div>
      <div style={{ color: "#e2e8f0", fontSize: 14, marginTop: 2 }}>{value}</div>
    </div>
  );
}
