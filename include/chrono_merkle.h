/*
 * chrono_merkle.h
 *
 * CHRONO-BLACKBOX -- Merkle root over a bounded window of sealed
 * chrono_block_t entries, zero dynamic allocation.
 *
 * ============================== Scope, precisely =============================
 * One function that matters: chrono_merkle_compute_root(). Everything
 * else in this file exists to make that one function correct, bounded,
 * and auditable.
 *
 * Leaves: leaf_i = SHA256(chrono_block_serialize(blocks[i])) -- the leaf
 * commits to EVERY byte of the sealed block (sequence_id, timestamp_ns,
 * tenant_id, prev_hash, payload_hash, AND block_hash itself), not just
 * to the subset block_hash already covers. This is deliberate defense in
 * depth for the specifically ON-DISK copy: chrono_storage.c writes
 * exactly these serialized bytes, so the Merkle root this file computes
 * is a real, independent commitment to the exact bytes that end up on
 * disk, not merely a re-derivation of block_hash.
 *
 * Interior nodes: parent = SHA256(left || right), the standard
 * construction (the same shape Bitcoin's block Merkle trees and RFC
 * 6962/Certificate Transparency's tree use, though this file matches
 * neither standard's domain-separation prefix byte -- see "What this is
 * NOT", below).
 *
 * Imperfect trees (leaf count not a power of two): at every level with an
 * odd node count, the LAST hash is duplicated as its own sibling before
 * hashing up one level -- the requirement's own specified rule, and the
 * same rule Bitcoin's Merkle tree uses. (Certificate Transparency's RFC
 * 6962 instead promotes the lone unpaired node unchanged to the next
 * level, a different construction with a different security property
 * against certain tree-structure ambiguity attacks -- not what was asked
 * for here, and not what this file does.)
 *
 * ============================== What this is NOT ============================
 * No domain-separation prefix byte distinguishes a leaf hash from an
 * interior-node hash (unlike RFC 6962, which prepends 0x00/0x01 to defend
 * specifically against a "second-preimage by reinterpreting an interior
 * hash as leaf data" attack across DIFFERENT trees sharing leaf/node
 * hash spaces). Precisely why that gap is acceptable here: this project
 * builds and verifies each window's tree from a caller-owned, in-RAM (or
 * just-read-from-this-project's-own-file) chrono_block_t array, and
 * chrono_block_serialize()'s output is fixed at CHRONO_BLOCK_SERIALIZED_SIZE
 * (116) bytes while an interior node hash is always exactly
 * SHA256_DIGEST_SIZE (32) bytes -- the two inputs are never
 * confusable byte-for-byte within one call, so the specific ambiguity
 * RFC 6962's prefix defends against does not arise in this file's own
 * verifier. A general-purpose Merkle library exposed to untrusted,
 * cross-tree input SHOULD still add that prefix; this one, scoped to
 * this project's own closed block format, does not need to.
 *
 * ============================== Zero-malloc, precisely ========================
 * The only working storage is one fixed-capacity stack array,
 * uint8_t[CHRONO_MERKLE_MAX_LEAVES][SHA256_DIGEST_SIZE] -- sized for the
 * worst case this file accepts (CHRONO_MERKLE_MAX_LEAVES leaves) and
 * reduced level-by-level IN PLACE (level i's parent is written to index
 * i < 2*i, always behind the still-unread pair it was computed from --
 * the standard safe in-place Merkle reduction), so no second full-size
 * buffer is ever needed. count > CHRONO_MERKLE_MAX_LEAVES is rejected as
 * CHRONO_MERKLE_ERR_INVALID rather than silently truncated.
 */

#ifndef CHRONO_MERKLE_H
#define CHRONO_MERKLE_H

#include <stddef.h>
#include <stdint.h>

#include "chrono_ingest.h" /* chrono_block_t, chrono_block_serialize(), CHRONO_BLOCK_SERIALIZED_SIZE */
#include "sha256.h"

/* Fixed upper bound on leaves-per-Merkle-tree (== blocks-per-storage-
 * window, chrono_storage.h reuses this same constant). Deliberately NOT
 * the same as CHRONO_CHAIN_CAPACITY (chrono_ingest.h's 4096-block in-RAM
 * chain limit): a Merkle tree here is built per bounded TIME WINDOW (the
 * requirement's own wording), not over an entire chain at once, so a much
 * smaller bound keeps this file's one stack buffer a modest, fixed size
 * (512 * 32 bytes = 16KiB) well inside any real stack, instead of sizing
 * for the full chain "just in case". */
#define CHRONO_MERKLE_MAX_LEAVES 512u

typedef enum {
    CHRONO_MERKLE_OK          = 0,
    CHRONO_MERKLE_ERR_INVALID = -1 /* NULL blocks/out_root, count == 0, or count > CHRONO_MERKLE_MAX_LEAVES */
} chrono_merkle_status_t;

/*
 * Computes the Merkle root over blocks[0..count) into out_root -- see the
 * file header for the exact leaf/interior-node/odd-count construction.
 * count == 0 is CHRONO_MERKLE_ERR_INVALID (a Merkle root over zero leaves
 * is not a well-defined commitment to anything). count == 1: out_root is
 * simply that one leaf's hash (a single-leaf tree has no interior nodes
 * to build). Pure function: no allocation, no shared state, safe from any
 * thread (each call uses only its own stack frame).
 */
chrono_merkle_status_t chrono_merkle_compute_root(chrono_block_t *blocks, size_t count,
                                                   uint8_t out_root[SHA256_DIGEST_SIZE]);

#endif /* CHRONO_MERKLE_H */
