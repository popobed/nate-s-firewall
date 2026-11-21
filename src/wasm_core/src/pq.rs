//! Product Quantization (PQ) : reduit l'empreinte memoire d'un vecteur en
//! le decoupant en `num_subvectors` sous-espaces, chacun approxime par
//! l'un des `num_centroids` centroides d'un codebook entraine par
//! k-means (Lloyd). Un vecteur complet (dim * 4 octets en f32) est alors
//! represente par seulement `num_subvectors` octets de code.
//!
//! Toutes les distances internes sont des distances euclidiennes AU
//! CARRE : la somme des distances au carre de chaque sous-espace est
//! mathematiquement egale a la distance au carre du vecteur complet
//! (par le theoreme de Pythagore applique a des sous-espaces orthogonaux
//! disjoints), ce qui rend la table de distance asymetrique (ADC)
//! correcte pour le classement par plus proches voisins. Sommer des
//! racines carrees sous-espace par sous-espace, a l'inverse, ne serait
//! PAS egal a la racine de la somme et fausserait le classement.

use alloc::vec::Vec;

use crate::distance::squared_euclidean_distance_scalar;
use crate::rng::Xorshift64;

pub struct ProductQuantizer {
    pub dim: usize,
    pub num_subvectors: usize,
    pub subvector_dim: usize,
    pub num_centroids: usize,
    pub codebooks: Vec<f32>, // aplati : [subvector][centroid][subvector_dim]
    pub trained: bool,
}

impl ProductQuantizer {
    pub fn new(dim: usize, num_subvectors: usize, num_centroids: usize) -> Self {
        debug_assert!(dim % num_subvectors == 0, "dim doit etre divisible par num_subvectors");
        debug_assert!(num_centroids >= 1 && num_centroids <= 256, "un code doit tenir sur un octet");
        let subvector_dim = dim / num_subvectors;
        let mut codebooks = Vec::new();
        codebooks.resize(num_subvectors * num_centroids * subvector_dim, 0.0f32);

        ProductQuantizer {
            dim,
            num_subvectors,
            subvector_dim,
            num_centroids,
            codebooks,
            trained: false,
        }
    }

    #[inline]
    fn codebook_slice(&self, subvector_index: usize, centroid_index: usize) -> &[f32] {
        let start = (subvector_index * self.num_centroids + centroid_index) * self.subvector_dim;
        &self.codebooks[start..start + self.subvector_dim]
    }

    #[inline]
    fn codebook_slice_mut(&mut self, subvector_index: usize, centroid_index: usize) -> &mut [f32] {
        let start = (subvector_index * self.num_centroids + centroid_index) * self.subvector_dim;
        &mut self.codebooks[start..start + self.subvector_dim]
    }

    #[inline]
    fn subvector_of<'a>(&self, flat_vectors: &'a [f32], vector_index: usize, subvector_index: usize) -> &'a [f32] {
        let base = vector_index * self.dim + subvector_index * self.subvector_dim;
        &flat_vectors[base..base + self.subvector_dim]
    }

    /// Entrainement independant d'un codebook par sous-espace, par
    /// k-means (Lloyd). `training_vectors` est un tableau aplati de n
    /// vecteurs de dimension `self.dim`.
    pub fn train(&mut self, training_vectors: &[f32], iterations: usize, seed: u64) {
        let n = training_vectors.len() / self.dim;
        if n == 0 {
            return;
        }
        let mut rng = Xorshift64::new(seed);
        let effective_k = self.num_centroids.min(n);

        for sub in 0..self.num_subvectors {
            for c in 0..effective_k {
                let sample_index = rng.next_usize_below(n);
                let sample = self.subvector_of(training_vectors, sample_index, sub).to_vec();
                self.codebook_slice_mut(sub, c).copy_from_slice(&sample);
            }

            let mut assignment = Vec::new();
            assignment.resize(n, 0usize);

            for _ in 0..iterations {
                for i in 0..n {
                    let sample = self.subvector_of(training_vectors, i, sub);
                    let mut best_centroid = 0usize;
                    let mut best_distance = f32::MAX;
                    for c in 0..effective_k {
                        let d = squared_euclidean_distance_scalar(sample, self.codebook_slice(sub, c));
                        if d < best_distance {
                            best_distance = d;
                            best_centroid = c;
                        }
                    }
                    assignment[i] = best_centroid;
                }

                let mut sums = Vec::new();
                sums.resize(self.num_centroids * self.subvector_dim, 0.0f32);
                let mut counts = Vec::new();
                counts.resize(self.num_centroids, 0usize);

                for i in 0..n {
                    let sample = self.subvector_of(training_vectors, i, sub);
                    let c = assignment[i];
                    counts[c] += 1;
                    for d in 0..self.subvector_dim {
                        sums[c * self.subvector_dim + d] += sample[d];
                    }
                }

                let subvector_dim = self.subvector_dim;
                for c in 0..effective_k {
                    if counts[c] == 0 {
                        continue; // centroide orphelin : conserve sa derniere position
                    }
                    let denom = counts[c] as f32;
                    let target = self.codebook_slice_mut(sub, c);
                    for d in 0..subvector_dim {
                        target[d] = sums[c * subvector_dim + d] / denom;
                    }
                }
            }
        }

        self.trained = true;
    }

    /// Encode un vecteur complet en `num_subvectors` codes d'un octet.
    pub fn encode(&self, vector: &[f32]) -> Vec<u8> {
        let mut codes = Vec::with_capacity(self.num_subvectors);
        for sub in 0..self.num_subvectors {
            let sample = &vector[sub * self.subvector_dim..(sub + 1) * self.subvector_dim];
            let mut best_centroid = 0u8;
            let mut best_distance = f32::MAX;
            for c in 0..self.num_centroids {
                let d = squared_euclidean_distance_scalar(sample, self.codebook_slice(sub, c));
                if d < best_distance {
                    best_distance = d;
                    best_centroid = c as u8;
                }
            }
            codes.push(best_centroid);
        }
        codes
    }

    /// Reconstruction approximative d'un vecteur a partir de ses codes.
    pub fn decode(&self, codes: &[u8]) -> Vec<f32> {
        let mut out = Vec::with_capacity(self.dim);
        for (sub, &code) in codes.iter().enumerate() {
            out.extend_from_slice(self.codebook_slice(sub, code as usize));
        }
        out
    }

    /// Precalcule, pour une requete donnee, la distance au carre entre
    /// chaque sous-vecteur de la requete et chacun des centroides du
    /// sous-espace correspondant. Permet ensuite un calcul de distance
    /// approximatif en O(num_subvectors) par candidat, sans decodage.
    pub fn build_adc_table(&self, query: &[f32]) -> Vec<f32> {
        let mut table = Vec::new();
        table.resize(self.num_subvectors * self.num_centroids, 0.0f32);
        for sub in 0..self.num_subvectors {
            let sample = &query[sub * self.subvector_dim..(sub + 1) * self.subvector_dim];
            for c in 0..self.num_centroids {
                table[sub * self.num_centroids + c] =
                    squared_euclidean_distance_scalar(sample, self.codebook_slice(sub, c));
            }
        }
        table
    }

    #[inline]
    pub fn adc_squared_distance(&self, table: &[f32], codes: &[u8]) -> f32 {
        let mut sum = 0.0f32;
        for (sub, &code) in codes.iter().enumerate() {
            sum += table[sub * self.num_centroids + code as usize];
        }
        sum
    }

    /// Fraction de memoire economisee par rapport au stockage f32 brut.
    pub fn memory_reduction_ratio(&self) -> f32 {
        let original_bytes = (self.dim * core::mem::size_of::<f32>()) as f32;
        let quantized_bytes = self.num_subvectors as f32;
        1.0 - (quantized_bytes / original_bytes)
    }
}
