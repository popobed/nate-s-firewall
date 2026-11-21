/*
 * tools/wasm_shim/string.h
 *
 * This environment has no wasi-libc (or any libc) installed for the
 * wasm32 target -- <string.h> itself does not exist there. This shim
 * provides ONLY the two prototypes the portable subset of
 * wasm_gpu_engine.c/ring_buffer.c actually calls (memcpy/memset),
 * matching their standard ISO C signatures exactly. It declares, it
 * does not define: the resulting .o keeps memcpy/memset as ordinary
 * unresolved external symbols, exactly like any C translation unit
 * before being linked against a real libc (wasi-libc, Emscripten, ...).
 *
 * Used ONLY by `make wasm-demo` (see the Makefile) to verify that the
 * portable geometry/command-queue module in wasm_gpu_engine.h really
 * compiles to WebAssembly object code -- it is not part of, and never
 * reaches, the real Windows/D3D12 build.
 */
#ifndef WASM_SHIM_STRING_H
#define WASM_SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *restrict dst, const void *restrict src, size_t n);
void *memset(void *dst, int c, size_t n);
#endif
