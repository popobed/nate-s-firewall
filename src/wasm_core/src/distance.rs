//! Fonctions de distance vectorielle : version scalaire (reference,
//! toujours correcte) et version vectorisee WebAssembly SIMD128 (4 f32
//! par cycle). Le module entier est compile avec
//! `target-feature=+simd128` (voir .cargo/config.toml) : sur WebAssembly,
//! contrairement au x86 natif, il n'existe pas de detection de capacite
//! a l'execution -- l'hote (navigateur) supporte SIMD128 ou refuse
//! d'instancier le module. Le repli scalaire ci-dessous reste neanmoins
//! utilise pour la fraction (< 4 elements) qui ne remplit pas un
//! registre de 128 bits.

use core::arch::wasm32::*;

#[inline]
pub fn squared_euclidean_distance_scalar(a: &[f32], b: &[f32]) -> f32 {
    debug_assert_eq!(a.len(), b.len());
    let mut sum = 0.0f32;
    for i in 0..a.len() {
        let d = a[i] - b[i];
        sum += d * d;
    }
    sum
}

#[inline]
pub fn euclidean_distance_scalar(a: &[f32], b: &[f32]) -> f32 {
    libm::sqrtf(squared_euclidean_distance_scalar(a, b))
}

#[inline]
pub fn cosine_distance_scalar(a: &[f32], b: &[f32]) -> f32 {
    debug_assert_eq!(a.len(), b.len());
    let mut dot = 0.0f32;
    let mut norm_a = 0.0f32;
    let mut norm_b = 0.0f32;
    for i in 0..a.len() {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if norm_a == 0.0 || norm_b == 0.0 {
        return 1.0;
    }
    1.0 - (dot / (libm::sqrtf(norm_a) * libm::sqrtf(norm_b)))
}

#[inline]
fn horizontal_sum(v: v128) -> f32 {
    f32x4_extract_lane::<0>(v)
        + f32x4_extract_lane::<1>(v)
        + f32x4_extract_lane::<2>(v)
        + f32x4_extract_lane::<3>(v)
}

pub fn euclidean_distance_simd(a: &[f32], b: &[f32]) -> f32 {
    debug_assert_eq!(a.len(), b.len());
    let len = a.len();
    let simd_len = len - (len % 4);
    let mut acc = f32x4_splat(0.0);
    let mut i = 0usize;

    while i < simd_len {
        unsafe {
            let va = v128_load(a.as_ptr().add(i) as *const v128);
            let vb = v128_load(b.as_ptr().add(i) as *const v128);
            let diff = f32x4_sub(va, vb);
            acc = f32x4_add(acc, f32x4_mul(diff, diff));
        }
        i += 4;
    }

    let mut sum = horizontal_sum(acc);
    while i < len {
        let d = a[i] - b[i];
        sum += d * d;
        i += 1;
    }

    libm::sqrtf(sum)
}

pub fn cosine_distance_simd(a: &[f32], b: &[f32]) -> f32 {
    debug_assert_eq!(a.len(), b.len());
    let len = a.len();
    let simd_len = len - (len % 4);
    let mut dot_acc = f32x4_splat(0.0);
    let mut norm_a_acc = f32x4_splat(0.0);
    let mut norm_b_acc = f32x4_splat(0.0);
    let mut i = 0usize;

    while i < simd_len {
        unsafe {
            let va = v128_load(a.as_ptr().add(i) as *const v128);
            let vb = v128_load(b.as_ptr().add(i) as *const v128);
            dot_acc = f32x4_add(dot_acc, f32x4_mul(va, vb));
            norm_a_acc = f32x4_add(norm_a_acc, f32x4_mul(va, va));
            norm_b_acc = f32x4_add(norm_b_acc, f32x4_mul(vb, vb));
        }
        i += 4;
    }

    let mut dot = horizontal_sum(dot_acc);
    let mut norm_a = horizontal_sum(norm_a_acc);
    let mut norm_b = horizontal_sum(norm_b_acc);

    while i < len {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
        i += 1;
    }

    if norm_a == 0.0 || norm_b == 0.0 {
        return 1.0;
    }
    1.0 - (dot / (libm::sqrtf(norm_a) * libm::sqrtf(norm_b)))
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum DistanceMetric {
    Euclidean,
    Cosine,
}

#[inline]
pub fn distance(metric: DistanceMetric, a: &[f32], b: &[f32]) -> f32 {
    match metric {
        DistanceMetric::Euclidean => euclidean_distance_simd(a, b),
        DistanceMetric::Cosine => cosine_distance_simd(a, b),
    }
}
