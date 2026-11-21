//! Cellule d'etat global pour un contexte a thread unique.
//!
//! Une instance WebAssembly sans `wasm-threads`/`SharedArrayBuffer`
//! s'execute integralement sur un seul thread : il ne peut jamais y
//! avoir de reentrance concurrente entre deux appels exportes. Ce
//! wrapper centralise donc l'unique `unsafe` necessaire pour exposer un
//! etat global mutable, plutot que de disperser des `static mut` bruts
//! (deconseilles depuis l'edition 2024 via le lint `static_mut_refs`,
//! car ils autorisent la coexistence de plusieurs references mutables).

use core::cell::UnsafeCell;

pub struct SingleThreaded<T> {
    inner: UnsafeCell<T>,
}

unsafe impl<T> Sync for SingleThreaded<T> {}

impl<T> SingleThreaded<T> {
    pub const fn new(value: T) -> Self {
        SingleThreaded { inner: UnsafeCell::new(value) }
    }

    /// Acces mutable exclusif. Valide uniquement sous l'hypothese de
    /// thread unique documentee ci-dessus.
    #[allow(clippy::mut_from_ref)]
    pub fn get(&self) -> &mut T {
        unsafe { &mut *self.inner.get() }
    }
}
