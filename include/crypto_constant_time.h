/*
 * crypto_constant_time.h
 *
 * Constant-time comparison primitives for authentication secrets (Tenant
 * ID lookups, API key / bearer token verification) -- Phase 5 (Niveau 30)
 * of the Empire d'Automatisation, Gateway Reseau & Protection Temps
 * Constant.
 *
 * The attack this module closes: a naive `memcmp`/`==`-based comparison of
 * a secret against attacker-supplied input returns as soon as it finds the
 * first differing byte. An attacker who can measure response time (even
 * over a network, with enough samples to average out noise -- this is a
 * well-published, practical attack class, not a theoretical one) can
 * therefore recover a secret one byte at a time: try every possible first
 * byte, keep the one that takes measurably longer, then move on to the
 * second byte, and so on. ct_equal() below always inspects every byte of
 * both inputs, with no data-dependent branch and no early exit, so neither
 * the elapsed time nor the branch-predictor state can reveal *where*, or
 * even *whether*, two buffers differ.
 *
 * Hardening technique: every pointer AND the accumulator are `volatile`-
 * qualified, the same technique used by libsodium's sodium_memcmp(). This
 * is not about a branch-free, fixed-trip-count OR-accumulate loop being
 * unsafe without it in practice (no mainstream compiler turns such a loop
 * into an early-exit one) -- it forecloses a different, harder-to-audit
 * risk: aggressive inlining/LTO proving something about the operands at a
 * specific call site (e.g. a constant-folded comparison against a literal)
 * and short-circuiting the whole function. `volatile` makes every load and
 * the accumulator a real, observable memory access the compiler cannot
 * reason away, at the cost of preventing vectorization/registerization --
 * a deliberate, expected trade-off for security-critical code: this loop
 * is memory-bound by design, not tuned for throughput.
 *
 * ============================== Scope, precisely =========================
 * What IS guaranteed: for two buffers of the SAME, already-known length,
 * comparing their CONTENT takes time that depends only on that length,
 * never on where (or whether) they differ.
 *
 * What is NOT attempted: hiding the LENGTH of a secret. ct_verify_token()
 * below rejects a length mismatch (provided_len != expected_len) with an
 * ordinary early-return branch, not a constant-time one. This is standard,
 * intentional practice (the same posture as libsodium's crypto_verify_*
 * family, which requires equal, fixed-size buffers and explicitly leaves
 * variable-length comparison out of scope): token/key lengths are protocol
 * constants an API documents publicly (e.g. "API keys are 32 bytes"), not
 * a secret derived from the key's content. Trying to iterate a fixed
 * CRYPTO_CT_MAX_LEN over a shorter real buffer to "hide" this would mean
 * reading past the end of that buffer -- undefined behavior, and a real
 * bug, in exchange for closing a channel that essentially never carries a
 * secret in practice. If a protocol genuinely must hide a secret's length,
 * the standard fix is to compare a FIXED-length MAC/hash of it instead
 * (ct_equal() over two hash outputs, for instance), never a bespoke
 * length-hiding buffer trick.
 *
 * Also out of scope, like any pure-software constant-time primitive: cache-
 * timing and power/EM side channels below the instruction level. This
 * closes the classic remote byte-at-a-time timing oracle, not every
 * conceivable side channel on every microarchitecture.
 *
 * Zero allocation, no I/O, no locks: pure functions over caller-owned
 * memory, safe to call from any thread and from the hardest-real-time
 * critical path.
 */

#ifndef CRYPTO_CONSTANT_TIME_H
#define CRYPTO_CONSTANT_TIME_H

#include <stddef.h>
#include <stdint.h>

/* Upper bound accepted by ct_verify_token(); large enough for any
 * realistic API key/bearer token while keeping the comparison loop's
 * worst-case cost a small, fixed, auditable constant. */
#define CRYPTO_CT_MAX_LEN 256u

/* Constant-time comparison of exactly `len` bytes starting at a and b.
 * Returns 1 if equal, 0 otherwise. Time and control flow depend only on
 * `len` (a caller-known, non-secret call-time value), never on the byte
 * VALUES of a or b. len == 0 is well-defined and always returns 1 without
 * dereferencing a or b. */
int ct_equal(const void *a, const void *b, size_t len);

/* Constant-time equality of two identifiers, without a plain `==` at the
 * call site. For a single machine word this closes a defense-in-depth /
 * consistency gap more than a practical one -- native integer comparison
 * is already effectively constant-time on real hardware -- but it keeps
 * every identifier/secret comparison in a codebase going through the same
 * audited primitive instead of requiring case-by-case judgment calls about
 * which `==` are "safe". */
int ct_equal_u16(uint16_t a, uint16_t b);
int ct_equal_u32(uint32_t a, uint32_t b);
int ct_equal_u64(uint64_t a, uint64_t b);

/*
 * Verifies a caller-provided token/API key against the real, expected
 * secret. Returns 1 iff provided_len == expected_len AND every byte
 * matches; 0 otherwise (including provided/expected == NULL, or either
 * length exceeding CRYPTO_CT_MAX_LEN).
 *
 * See the file header ("Scope, precisely") for exactly what is and is not
 * constant-time here: byte CONTENT comparison for matching lengths is;
 * the length check and the CRYPTO_CT_MAX_LEN bound check are ordinary,
 * fast-reject branches, by design.
 */
int ct_verify_token(const uint8_t *provided, size_t provided_len,
                     const uint8_t *expected, size_t expected_len);

#endif /* CRYPTO_CONSTANT_TIME_H */
