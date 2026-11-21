import { useState } from "react";

/** Champ de recherche en langage naturel sur les documents importes. */
export default function SearchBar({ onSearch, disabled, disabledReason }) {
  const [query, setQuery] = useState("");

  const submit = (event) => {
    event.preventDefault();
    const trimmed = query.trim();
    if (trimmed.length > 0 && !disabled) {
      onSearch(trimmed);
    }
  };

  return (
    <form onSubmit={submit} style={{ display: "flex", flexDirection: "column", gap: 6 }}>
      <div style={{ display: "flex", gap: 10 }}>
        <input
          type="text"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="Que voulez-vous chercher dans vos documents ?"
          disabled={disabled}
          style={{
            flex: 1,
            background: "#0f1420",
            color: "#e2e8f0",
            border: "1px solid #334155",
            borderRadius: 8,
            padding: "12px 16px",
            fontSize: 15,
            outline: "none",
          }}
        />
        <button
          type="submit"
          disabled={disabled || query.trim().length === 0}
          style={{
            background: "#38bdf8",
            color: "#05070d",
            border: "none",
            borderRadius: 8,
            padding: "12px 22px",
            fontWeight: 600,
            cursor: disabled ? "not-allowed" : "pointer",
            opacity: disabled ? 0.5 : 1,
          }}
        >
          Rechercher
        </button>
      </div>
      {disabled && disabledReason ? (
        <span style={{ color: "#94a3b8", fontSize: 12 }}>{disabledReason}</span>
      ) : null}
    </form>
  );
}
