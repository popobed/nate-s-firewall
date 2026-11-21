//! Codec binaire compact pour la persistance IndexedDB.
//!
//! Format volontairement simple et fixe -- l'equivalent fonctionnel de
//! ce que produirait `bincode` pour cette structure de donnees precise,
//! sans tirer la dependance externe (bincode/serde ne garantissent pas
//! tous une compatibilite no_std stricte ; ce codec, ecrit a la main,
//! est verifiable de bout en bout dans cet environnement).
//!
//! Disposition : [magic:u32][count:u32][dim:u32] puis, pour chaque
//! vecteur, [id:u32][dim * f32 little-endian].

use alloc::vec::Vec;

pub const RECORD_HEADER_MAGIC: u32 = 0x4D45_5348; // "MESH" en ASCII

pub fn encode_vectors(ids: &[u32], flat_vectors: &[f32], dim: usize) -> Vec<u8> {
    let count = ids.len();
    let mut out = Vec::with_capacity(12 + count * (4 + dim * 4));

    out.extend_from_slice(&RECORD_HEADER_MAGIC.to_le_bytes());
    out.extend_from_slice(&(count as u32).to_le_bytes());
    out.extend_from_slice(&(dim as u32).to_le_bytes());

    for i in 0..count {
        out.extend_from_slice(&ids[i].to_le_bytes());
        let base = i * dim;
        for d in 0..dim {
            out.extend_from_slice(&flat_vectors[base + d].to_le_bytes());
        }
    }

    out
}

pub struct DecodedVectors {
    pub ids: Vec<u32>,
    pub flat_vectors: Vec<f32>,
    pub dim: usize,
}

pub fn decode_vectors(buffer: &[u8]) -> Option<DecodedVectors> {
    if buffer.len() < 12 {
        return None;
    }

    let magic = u32::from_le_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]);
    if magic != RECORD_HEADER_MAGIC {
        return None;
    }

    let count = u32::from_le_bytes([buffer[4], buffer[5], buffer[6], buffer[7]]) as usize;
    let dim = u32::from_le_bytes([buffer[8], buffer[9], buffer[10], buffer[11]]) as usize;

    let expected_len = 12usize
        .checked_add(count.checked_mul(4usize.checked_add(dim.checked_mul(4)?)?)?)?;
    if buffer.len() < expected_len {
        return None;
    }

    let mut ids = Vec::with_capacity(count);
    let mut flat_vectors = Vec::with_capacity(count * dim);
    let mut offset = 12usize;

    for _ in 0..count {
        let id = u32::from_le_bytes([buffer[offset], buffer[offset + 1], buffer[offset + 2], buffer[offset + 3]]);
        ids.push(id);
        offset += 4;

        for _ in 0..dim {
            let v = f32::from_le_bytes([buffer[offset], buffer[offset + 1], buffer[offset + 2], buffer[offset + 3]]);
            flat_vectors.push(v);
            offset += 4;
        }
    }

    Some(DecodedVectors { ids, flat_vectors, dim })
}
