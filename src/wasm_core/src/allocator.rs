//! Allocateur global "bump" pour WebAssembly, sans systeme d'exploitation
//! sous-jacent : le tas grandit via `memory.grow` et n'est jamais
//! compacte ni libere individuellement (coherent avec la philosophie
//! "Zero Stop-The-World" du noyau C11 de l'Empire d'Automatisation).

use core::alloc::{GlobalAlloc, Layout};
use core::sync::atomic::{AtomicUsize, Ordering};

const WASM_PAGE_BYTES: usize = 0x10000; // 64 KiB, taille fixe d'une page WebAssembly

pub struct BumpAllocator {
    /// Prochaine adresse libre. 0 = tas pas encore initialise.
    next_free: AtomicUsize,
    /// Borne haute actuelle du tas (fin de la derniere page allouee).
    heap_limit: AtomicUsize,
}

// Le module s'execute dans un contexte a thread unique (pas de
// `wasm-threads`/SharedArrayBuffer ici) ; les operations atomiques
// servent de garde-fou de coherence, pas d'une garantie multi-thread
// complete.
unsafe impl Sync for BumpAllocator {}

impl BumpAllocator {
    pub const fn new() -> Self {
        BumpAllocator {
            next_free: AtomicUsize::new(0),
            heap_limit: AtomicUsize::new(0),
        }
    }
}

unsafe impl GlobalAlloc for BumpAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        loop {
            let mut current = self.next_free.load(Ordering::Relaxed);

            if current == 0 {
                // Initialisation paresseuse : le tas commence juste apres
                // toute la memoire lineaire deja occupee par les segments
                // statiques et la pile au demarrage du module (on
                // interroge la taille actuelle sans la modifier via un
                // memory_grow de delta nul).
                let current_pages = core::arch::wasm32::memory_grow(0, 0);
                let start = current_pages * WASM_PAGE_BYTES;
                self.heap_limit.store(start, Ordering::Relaxed);

                current = match self.next_free.compare_exchange(
                    0,
                    start,
                    Ordering::Relaxed,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => start,
                    Err(actual) => actual,
                };
            }

            let align = layout.align();
            let aligned = (current + align - 1) & !(align - 1);

            let new_free = match aligned.checked_add(layout.size()) {
                Some(v) => v,
                None => return core::ptr::null_mut(),
            };

            let limit = self.heap_limit.load(Ordering::Relaxed);

            if new_free > limit {
                let needed = new_free - limit;
                let pages_needed = (needed + WASM_PAGE_BYTES - 1) / WASM_PAGE_BYTES;
                let previous_pages = core::arch::wasm32::memory_grow(0, pages_needed);

                if previous_pages == usize::MAX {
                    return core::ptr::null_mut(); // croissance memoire refusee par l'hote
                }

                let new_limit = limit + pages_needed * WASM_PAGE_BYTES;
                self.heap_limit.store(new_limit, Ordering::Relaxed);
                continue; // reessaye l'allocation avec la nouvelle limite
            }

            if self
                .next_free
                .compare_exchange(current, new_free, Ordering::Relaxed, Ordering::Relaxed)
                .is_ok()
            {
                return aligned as *mut u8;
            }
            // CAS perdu : un autre appelant a deja avance next_free, on reboucle.
        }
    }

    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: Layout) {
        // Allocateur bump : aucune liberation individuelle. La memoire
        // n'est recuperee qu'en detruisant l'instance WebAssembly elle-meme.
    }
}
