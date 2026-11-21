import { useCallback, useRef, useState } from "react";

const ACCEPTED_EXTENSIONS = [".txt", ".md", ".markdown", ".json"];

function isAcceptedFile(file) {
  const lowerName = file.name.toLowerCase();
  return ACCEPTED_EXTENSIONS.some((ext) => lowerName.endsWith(ext));
}

/** Extrait le texte exploitable d'un fichier importe (texte brut, Markdown ou JSON mis en forme lisible). */
async function readFileAsText(file) {
  const raw = await file.text();
  if (!file.name.toLowerCase().endsWith(".json")) {
    return raw;
  }
  try {
    return JSON.stringify(JSON.parse(raw), null, 2);
  } catch {
    return raw; // JSON invalide : on indexe malgre tout le contenu brut plutot que de rejeter le fichier
  }
}

/** Panneau d'importation de documents (glisser-deposer ou selection de fichiers) : .txt, .md, .json. */
export default function DocumentImporter({ onImportFiles, importing, documents }) {
  const [isDragOver, setIsDragOver] = useState(false);
  const fileInputRef = useRef(null);

  const processFiles = useCallback(
    async (fileList) => {
      const files = Array.from(fileList).filter(isAcceptedFile);
      if (files.length === 0) {
        return;
      }
      const entries = await Promise.all(
        files.map(async (file) => ({ filename: file.name, content: await readFileAsText(file) })),
      );
      onImportFiles(entries);
    },
    [onImportFiles],
  );

  const onDrop = useCallback(
    (event) => {
      event.preventDefault();
      setIsDragOver(false);
      void processFiles(event.dataTransfer.files);
    },
    [processFiles],
  );

  const onFilePicked = useCallback(
    (event) => {
      if (event.target.files) {
        void processFiles(event.target.files);
      }
      event.target.value = "";
    },
    [processFiles],
  );

  return (
    <div>
      <div
        onDragOver={(event) => {
          event.preventDefault();
          setIsDragOver(true);
        }}
        onDragLeave={() => setIsDragOver(false)}
        onDrop={onDrop}
        onClick={() => fileInputRef.current?.click()}
        style={{
          border: `2px dashed ${isDragOver ? "#38bdf8" : "#334155"}`,
          borderRadius: 10,
          padding: "22px 16px",
          textAlign: "center",
          cursor: "pointer",
          background: isDragOver ? "rgba(56, 189, 248, 0.06)" : "transparent",
          transition: "background 0.15s, border-color 0.15s",
        }}
      >
        <div style={{ color: "#94a3b8", fontSize: 14 }}>
          {importing ? "Importation en cours..." : "Glissez-deposez vos documents ici, ou cliquez pour choisir des fichiers"}
        </div>
        <div style={{ color: "#475569", fontSize: 12, marginTop: 4 }}>Formats acceptes : .txt, .md, .json</div>
        <input
          ref={fileInputRef}
          type="file"
          multiple
          accept={ACCEPTED_EXTENSIONS.join(",")}
          onChange={onFilePicked}
          style={{ display: "none" }}
        />
      </div>

      {documents.length > 0 ? (
        <ul style={{ listStyle: "none", margin: "12px 0 0", padding: 0, display: "flex", flexDirection: "column", gap: 4 }}>
          {documents.map((doc) => (
            <li
              key={doc.documentId}
              style={{
                display: "flex",
                justifyContent: "space-between",
                fontSize: 12,
                color: "#94a3b8",
                background: "#0f1420",
                borderRadius: 6,
                padding: "6px 10px",
              }}
            >
              <span>{doc.filename}</span>
              <span>{doc.chunkCount} extrait{doc.chunkCount > 1 ? "s" : ""}</span>
            </li>
          ))}
        </ul>
      ) : null}
    </div>
  );
}
