/**
 * Projection lineaire de vecteurs de haute dimension vers R^3 par PCA
 * (Analyse en Composantes Principales) calculee via iteration de
 * puissance + deflation -- suffisant pour n'extraire que les 3
 * premieres composantes principales sans jamais diagonaliser toute la
 * matrice de covariance. Utilise pour positionner les noeuds HNSW dans
 * la nebuleuse 3D lorsque la dimension d'embedding depasse 3.
 */

function subtractMean(vectors, dim) {
  const mean = new Float64Array(dim);
  for (const v of vectors) {
    for (let i = 0; i < dim; i += 1) mean[i] += v[i];
  }
  for (let i = 0; i < dim; i += 1) mean[i] /= vectors.length;

  return vectors.map((v) => v.map((x, i) => x - mean[i]));
}

function covarianceMatvec(centered, dim, vec) {
  // Applique la matrice de covariance C = (1/N) * X^T X a `vec` sans
  // jamais materialiser C explicitement (couteux en O(dim^2)) : on
  // calcule X * vec (projection scalaire par ligne), puis X^T * (X*vec).
  const projections = centered.map((row) => {
    let sum = 0;
    for (let i = 0; i < dim; i += 1) sum += row[i] * vec[i];
    return sum;
  });

  const result = new Float64Array(dim);
  for (let i = 0; i < centered.length; i += 1) {
    const row = centered[i];
    const p = projections[i];
    for (let d = 0; d < dim; d += 1) result[d] += row[d] * p;
  }
  for (let d = 0; d < dim; d += 1) result[d] /= centered.length;
  return result;
}

function normalize(vec) {
  let norm = 0;
  for (const x of vec) norm += x * x;
  norm = Math.sqrt(norm) || 1;
  return vec.map((x) => x / norm);
}

function powerIterationTopComponent(centered, dim, iterations, rng) {
  let vec = Array.from({ length: dim }, () => rng() - 0.5);
  vec = normalize(vec);

  for (let iter = 0; iter < iterations; iter += 1) {
    const next = covarianceMatvec(centered, dim, vec);
    vec = normalize(Array.from(next));
  }
  return vec;
}

function deflate(centered, dim, component) {
  // Retire la projection sur `component` de chaque vecteur, pour que
  // l'iteration de puissance suivante converge vers la composante
  // principale SUIVANTE plutot que de retrouver la meme.
  return centered.map((row) => {
    let dot = 0;
    for (let i = 0; i < dim; i += 1) dot += row[i] * component[i];
    return row.map((x, i) => x - dot * component[i]);
  });
}

/**
 * Projette un tableau de vecteurs (memes dimensions) vers 3 composantes
 * principales. Si `dim <= 3`, retourne les vecteurs tels quels
 * (complete avec des zeros si dim < 3).
 */
export function projectTo3D(vectors, dim, options = {}) {
  const iterations = options.iterations ?? 24;
  const seed = options.seed ?? 1;

  if (vectors.length === 0) {
    return [];
  }

  if (dim <= 3) {
    return vectors.map((v) => [v[0] ?? 0, v[1] ?? 0, v[2] ?? 0]);
  }

  let state = seed >>> 0 || 1;
  const rng = () => {
    // xorshift32, deterministe : la disposition de la nebuleuse est
    // stable entre deux rendus pour le meme jeu de donnees.
    state ^= state << 13;
    state ^= state >>> 17;
    state ^= state << 5;
    return ((state >>> 0) % 1000000) / 1000000;
  };

  let centered = subtractMean(vectors, dim);
  const components = [];

  for (let c = 0; c < 3; c += 1) {
    const component = powerIterationTopComponent(centered, dim, iterations, rng);
    components.push(component);
    centered = deflate(centered, dim, component);
  }

  return centered.map((_, idx) => {
    const original = vectors[idx];
    return components.map((component) => {
      let dot = 0;
      for (let i = 0; i < dim; i += 1) dot += original[i] * component[i];
      return dot;
    });
  });
}

/** Position deterministe pseudo-aleatoire sur une sphere, derivee d'un identifiant de pair (hex). */
export function peerIdToSpherePosition(peerIdHex, radius = 8) {
  let hash = 0;
  for (let i = 0; i < peerIdHex.length; i += 1) {
    hash = (hash * 31 + peerIdHex.charCodeAt(i)) >>> 0;
  }
  const theta = (hash % 1000) / 1000 * Math.PI * 2;
  const phi = ((hash >>> 10) % 1000) / 1000 * Math.PI;
  return [
    radius * Math.sin(phi) * Math.cos(theta),
    radius * Math.sin(phi) * Math.sin(theta),
    radius * Math.cos(phi),
  ];
}
