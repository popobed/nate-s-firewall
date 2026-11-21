//! Index HNSW (Hierarchical Navigable Small World) : graphe multi-couches
//! permettant une recherche des k plus proches voisins approximative en
//! O(log N) en moyenne, au lieu du parcours exhaustif O(N).

use alloc::collections::{BTreeSet, BinaryHeap};
use alloc::vec::Vec;
use core::cmp::Ordering;

use crate::distance::{distance, DistanceMetric};
use crate::rng::Xorshift64;

/// Element de la frontiere d'exploration : ordonne pour que le
/// `BinaryHeap` (naturellement un tas MAX) se comporte comme un tas MIN
/// sur la distance (on inverse volontairement la comparaison).
#[derive(Clone, Copy)]
struct MinCandidate(f32, u32);

impl PartialEq for MinCandidate {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}
impl Eq for MinCandidate {}
impl PartialOrd for MinCandidate {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for MinCandidate {
    fn cmp(&self, other: &Self) -> Ordering {
        other.0.partial_cmp(&self.0).unwrap_or(Ordering::Equal)
    }
}

/// Ensemble des meilleurs candidats trouves : ordre naturel, donc un
/// `BinaryHeap` classique agit comme un tas MAX sur la distance -- son
/// sommet est le plus eloigne, celui qu'on evince en premier quand la
/// taille depasse `ef`.
#[derive(Clone, Copy)]
struct MaxCandidate(f32, u32);

impl PartialEq for MaxCandidate {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}
impl Eq for MaxCandidate {}
impl PartialOrd for MaxCandidate {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for MaxCandidate {
    fn cmp(&self, other: &Self) -> Ordering {
        self.0.partial_cmp(&other.0).unwrap_or(Ordering::Equal)
    }
}

struct Node {
    vector: Vec<f32>,
    neighbors: Vec<Vec<u32>>, // neighbors[layer] = identifiants des voisins a cette couche
}

pub struct HnswIndex {
    nodes: Vec<Node>,
    entry_point: Option<u32>,
    max_layer: usize,
    dim: usize,
    metric: DistanceMetric,
    m: usize,
    m0: usize,
    ef_construction: usize,
    level_lambda: f32,
    rng: Xorshift64,
}

impl HnswIndex {
    pub fn new(dim: usize, metric: DistanceMetric, m: usize, ef_construction: usize, seed: u64) -> Self {
        let m = m.max(2);
        HnswIndex {
            nodes: Vec::new(),
            entry_point: None,
            max_layer: 0,
            dim,
            metric,
            m,
            m0: m * 2,
            ef_construction: ef_construction.max(1),
            level_lambda: 1.0 / libm::logf(m as f32),
            rng: Xorshift64::new(seed),
        }
    }

    pub fn len(&self) -> usize {
        self.nodes.len()
    }

    pub fn dim(&self) -> usize {
        self.dim
    }

    pub fn vector_of(&self, id: u32) -> &[f32] {
        &self.nodes[id as usize].vector
    }

    fn random_level(&mut self) -> usize {
        let r = self.rng.next_f32().max(1.0e-9);
        let level = libm::floorf(-(libm::logf(r)) * self.level_lambda);
        if level < 0.0 {
            0
        } else {
            level as usize
        }
    }

    #[inline]
    fn dist_to_node(&self, query: &[f32], node_id: u32) -> f32 {
        distance(self.metric, query, &self.nodes[node_id as usize].vector)
    }

    fn search_layer(&self, query: &[f32], entry_points: &[u32], ef: usize, layer: usize) -> Vec<(f32, u32)> {
        let mut visited: BTreeSet<u32> = BTreeSet::new();
        let mut candidates: BinaryHeap<MinCandidate> = BinaryHeap::new();
        let mut found: BinaryHeap<MaxCandidate> = BinaryHeap::new();

        for &ep in entry_points {
            if visited.insert(ep) {
                let d = self.dist_to_node(query, ep);
                candidates.push(MinCandidate(d, ep));
                found.push(MaxCandidate(d, ep));
            }
        }

        while let Some(MinCandidate(current_dist, current_id)) = candidates.pop() {
            if let Some(&MaxCandidate(worst_dist, _)) = found.peek() {
                if found.len() >= ef && current_dist > worst_dist {
                    break; // aucune amelioration possible en continuant : arret anticipe
                }
            }

            let layer_neighbors = self.nodes[current_id as usize]
                .neighbors
                .get(layer)
                .cloned()
                .unwrap_or_default();

            for neighbor_id in layer_neighbors {
                if visited.insert(neighbor_id) {
                    let d = self.dist_to_node(query, neighbor_id);
                    let worst = found.peek().map(|c| c.0);
                    let should_add = found.len() < ef || worst.map_or(true, |w| d < w);

                    if should_add {
                        candidates.push(MinCandidate(d, neighbor_id));
                        found.push(MaxCandidate(d, neighbor_id));
                        if found.len() > ef {
                            found.pop();
                        }
                    }
                }
            }
        }

        let sorted = found.into_sorted_vec(); // ordre ascendant (Ord naturel de MaxCandidate)
        sorted.into_iter().map(|MaxCandidate(d, id)| (d, id)).collect()
    }

    fn connect(&mut self, from: u32, to: u32, layer: usize) {
        let neighbors = &mut self.nodes[from as usize].neighbors[layer];
        if !neighbors.contains(&to) {
            neighbors.push(to);
        }
    }

    fn prune_neighbors(&mut self, node_id: u32, layer: usize, max_connections: usize) {
        if self.nodes[node_id as usize].neighbors[layer].len() <= max_connections {
            return;
        }

        let node_vector = self.nodes[node_id as usize].vector.clone();
        let mut scored: Vec<(f32, u32)> = self.nodes[node_id as usize]
            .neighbors[layer]
            .iter()
            .map(|&nid| (distance(self.metric, &node_vector, &self.nodes[nid as usize].vector), nid))
            .collect();

        scored.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(Ordering::Equal));
        scored.truncate(max_connections);

        self.nodes[node_id as usize].neighbors[layer] = scored.into_iter().map(|(_, nid)| nid).collect();
    }

    /// Insere un nouveau vecteur dans l'index et retourne son identifiant.
    pub fn insert(&mut self, vector: Vec<f32>) -> u32 {
        debug_assert_eq!(vector.len(), self.dim);

        let new_id = self.nodes.len() as u32;
        let level = self.random_level();

        let mut own_layers = Vec::with_capacity(level + 1);
        for _ in 0..=level {
            own_layers.push(Vec::new());
        }
        self.nodes.push(Node { vector: vector.clone(), neighbors: own_layers });

        let current_entry = match self.entry_point {
            None => {
                self.entry_point = Some(new_id);
                self.max_layer = level;
                return new_id;
            }
            Some(ep) => ep,
        };

        let mut entry = current_entry;
        let mut entry_dist = self.dist_to_node(&vector, entry);

        for layer in ((level + 1)..=self.max_layer).rev() {
            loop {
                let mut improved = false;
                let layer_neighbors = self.nodes[entry as usize].neighbors.get(layer).cloned().unwrap_or_default();
                for neighbor_id in layer_neighbors {
                    let d = self.dist_to_node(&vector, neighbor_id);
                    if d < entry_dist {
                        entry_dist = d;
                        entry = neighbor_id;
                        improved = true;
                    }
                }
                if !improved {
                    break;
                }
            }
        }

        let mut entry_points_for_layer = alloc::vec![entry];
        let top_layer_to_connect = level.min(self.max_layer);

        for layer in (0..=top_layer_to_connect).rev() {
            let candidates = self.search_layer(&vector, &entry_points_for_layer, self.ef_construction, layer);
            let max_connections = if layer == 0 { self.m0 } else { self.m };

            let selected: Vec<u32> = candidates.iter().take(max_connections).map(|c| c.1).collect();

            for &neighbor_id in &selected {
                self.connect(new_id, neighbor_id, layer);
                self.connect(neighbor_id, new_id, layer);
                self.prune_neighbors(neighbor_id, layer, max_connections);
            }

            entry_points_for_layer = if candidates.is_empty() {
                alloc::vec![entry]
            } else {
                candidates.iter().map(|c| c.1).collect()
            };
        }

        if level > self.max_layer {
            self.max_layer = level;
            self.entry_point = Some(new_id);
        }

        new_id
    }

    /// Recherche des `k` plus proches voisins de `query`. `ef_search`
    /// controle le compromis rappel/vitesse (>= k, plus grand = plus
    /// precis mais plus lent).
    pub fn search(&self, query: &[f32], k: usize, ef_search: usize) -> Vec<(u32, f32)> {
        let entry_point = match self.entry_point {
            Some(ep) => ep,
            None => return Vec::new(),
        };

        let mut entry = entry_point;
        let mut entry_dist = self.dist_to_node(query, entry);

        for layer in (1..=self.max_layer).rev() {
            loop {
                let mut improved = false;
                let layer_neighbors = self.nodes[entry as usize].neighbors.get(layer).cloned().unwrap_or_default();
                for neighbor_id in layer_neighbors {
                    let d = self.dist_to_node(query, neighbor_id);
                    if d < entry_dist {
                        entry_dist = d;
                        entry = neighbor_id;
                        improved = true;
                    }
                }
                if !improved {
                    break;
                }
            }
        }

        let ef = ef_search.max(k);
        let candidates = self.search_layer(query, &[entry], ef, 0);
        candidates.into_iter().take(k).map(|(d, id)| (id, d)).collect()
    }

    /// Exporte l'ensemble des identifiants et vecteurs bruts (aplatis),
    /// utilise pour l'entrainement du Product Quantizer et pour la
    /// persistance (le graphe HNSW lui-meme est reconstruit a partir des
    /// vecteurs bruts au rechargement, plutot que serialise directement).
    pub fn export_all(&self) -> (Vec<u32>, Vec<f32>) {
        let mut ids = Vec::with_capacity(self.nodes.len());
        let mut flat = Vec::with_capacity(self.nodes.len() * self.dim);
        for (i, node) in self.nodes.iter().enumerate() {
            ids.push(i as u32);
            flat.extend_from_slice(&node.vector);
        }
        (ids, flat)
    }
}
