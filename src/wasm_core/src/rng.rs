//! Generateur pseudo-aleatoire xorshift64*, deterministe et sans
//! dependance externe (pas d'acces a une source d'entropie systeme en
//! no_std). Utilise pour l'initialisation des centroides du Product
//! Quantizer et pour l'echantillonnage du niveau d'insertion HNSW.

pub struct Xorshift64 {
    state: u64,
}

impl Xorshift64 {
    pub fn new(seed: u64) -> Self {
        Xorshift64 {
            state: if seed == 0 { 0x9E3779B97F4A7C15 } else { seed },
        }
    }

    #[inline]
    pub fn next_u64(&mut self) -> u64 {
        let mut x = self.state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.state = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }

    /// Flottant uniforme dans [0, 1).
    #[inline]
    pub fn next_f32(&mut self) -> f32 {
        // 24 bits de mantisse suffisent pour un f32 uniforme correct.
        let bits = (self.next_u64() >> 40) as u32;
        (bits as f32) / (1u32 << 24) as f32
    }

    #[inline]
    pub fn next_usize_below(&mut self, bound: usize) -> usize {
        if bound == 0 {
            return 0;
        }
        (self.next_u64() % (bound as u64)) as usize
    }
}
