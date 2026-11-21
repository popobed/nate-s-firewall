/*
 * sha256.h
 *
 * Minimal, standard SHA-256 (FIPS 180-4), one-shot, zero-allocation.
 *
 * ============================== Honesty notes, up front =====================
 * This is the one real cryptographic HASH primitive this project did not
 * previously have: crypto_constant_time.h documents explicitly that no
 * cipher/AEAD/hash-beyond-FNV-1a exists anywhere in this codebase, and
 * telemetry_fabric.h's tf_log_t deliberately used FNV-1a instead of a
 * cryptographic hash for exactly that reason ("inventing one here would
 * be... unvetted cryptography, worse than not claiming it").
 *
 * What makes this file different from that refusal, and why it is safe to
 * add now: SHA-256 is not a novel or bespoke primitive being invented for
 * this project -- it is the standard, publicly specified algorithm (FIPS
 * 180-4 / RFC 6234), implemented here bit-for-bit from that published
 * pseudocode, with no shortcuts or simplifications. "Unvetted" is a
 * property of NOVEL cryptographic design, not of a straightforward
 * from-spec implementation of an algorithm that has been reviewed by the
 * entire field for over two decades. This implementation's OWN
 * correctness is not asserted on faith either: tests/test_chrono_block_link.c
 * checks it against NIST's own official known-answer test vectors (the
 * empty string, "abc", and the standard two-block 56-character message)
 * before it is trusted for anything else -- verified, not assumed.
 *
 * What is NOT claimed: this is a plain, portable, from-spec SHA-256 core,
 * not a hardened cryptographic library. It is not written to be constant-
 * time against cache-timing/microarchitectural side channels (unlike
 * crypto_constant_time.h's ct_equal(), which exists for exactly that
 * property) -- hashing a block's own fields to seal it is not a secret-
 * dependent-branch scenario the way comparing a token against a secret
 * is, so that hardening is out of scope here, consistent with how this
 * project draws that line elsewhere (crypto_constant_time.h's own "Scope,
 * precisely" section). No HMAC, no signature scheme, no key derivation --
 * a hash function only.
 *
 * Zero allocation, no I/O, no locks, no global state: every function
 * either is pure over caller-owned memory or uses only its own stack
 * frame, safe to call from any thread and from the hardest-real-time
 * critical path -- same posture as crypto_constant_time.h.
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32u
#define SHA256_BLOCK_SIZE  64u

/*
 * One-shot: hashes exactly `len` bytes at `data` into
 * out[SHA256_DIGEST_SIZE], per FIPS 180-4. Pure function: no allocation,
 * no shared state, safe from any thread. len == 0 is well-defined (the
 * standard SHA-256 digest of the empty string, e3b0c442...). `data` may
 * be NULL only if len == 0. `out` must be non-NULL.
 */
void sha256_hash(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

#endif /* SHA256_H */
