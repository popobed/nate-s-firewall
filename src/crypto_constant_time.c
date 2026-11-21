#include "crypto_constant_time.h"

int ct_equal(const void *a, const void *b, size_t len) {
    /* volatile: see the "Hardening technique" note in crypto_constant_time.h --
     * forces every load and every accumulator update to be a real, observable
     * memory access the compiler cannot fold, reorder past the loop, or prove
     * constant-away, at the cost of vectorization/registerization (an
     * accepted trade-off here: memory-bound by design). */
    volatile const uint8_t *pa = (volatile const uint8_t *)a;
    volatile const uint8_t *pb = (volatile const uint8_t *)b;
    volatile uint8_t diff = 0;

    for (size_t i = 0; i < len; i++) {
        diff = (uint8_t)(diff | (uint8_t)(pa[i] ^ pb[i]));
    }
    return diff == 0;
}

/* Taking the address of a parameter forces the compiler to give it a real,
 * addressable memory location (the standard requires this for `&x` to be
 * well-formed at all); ct_equal()'s own volatile-qualified reads of that
 * location then prevent any data-dependent short-circuiting of the
 * comparison loop itself -- no extra hardening needed at this call site. */
int ct_equal_u16(uint16_t a, uint16_t b) {
    return ct_equal(&a, &b, sizeof a);
}

int ct_equal_u32(uint32_t a, uint32_t b) {
    return ct_equal(&a, &b, sizeof a);
}

int ct_equal_u64(uint64_t a, uint64_t b) {
    return ct_equal(&a, &b, sizeof a);
}

int ct_verify_token(const uint8_t *provided, size_t provided_len,
                     const uint8_t *expected, size_t expected_len) {
    if (provided == NULL || expected == NULL) {
        return 0;
    }
    if (provided_len > CRYPTO_CT_MAX_LEN || expected_len > CRYPTO_CT_MAX_LEN) {
        return 0;
    }
    /* Ordinary (non-constant-time) branch: see "Scope, precisely" in the
     * header -- token length is a public protocol constant, not a secret. */
    if (provided_len != expected_len) {
        return 0;
    }
    return ct_equal(provided, expected, provided_len);
}
