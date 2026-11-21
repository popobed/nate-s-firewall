/*
 * chrono_storage.h
 *
 * CHRONO-BLACKBOX -- append-only, Merkle-sealed persistence for windows
 * of chrono_block_t entries.
 *
 * ============================== Format, precisely =============================
 * The file is a flat sequence of independent WINDOWS, back to back, each:
 *
 *     [ window header: CHRONO_STORAGE_WINDOW_HEADER_SIZE (48) bytes ]
 *     [ block 0, CHRONO_BLOCK_SERIALIZED_SIZE (116) bytes           ]
 *     [ block 1, 116 bytes                                          ]
 *     ...
 *     [ block (block_count - 1), 116 bytes                          ]
 *
 * Window header, in this exact field order (host-native byte order for
 * the integers, same disclosed tradeoff as wire_proto.h/chrono_ingest.h):
 *     magic              u32  -- CHRONO_STORAGE_MAGIC, a structural sanity tag
 *     block_count        u32  -- number of chrono_block_t serialized right after this header
 *     window_timestamp_ns u64 -- real wall-clock ns at flush time (chrono_wall_clock_ns())
 *     merkle_root        32 bytes -- chrono_merkle_compute_root() over this window's blocks
 *
 * Each block is chrono_block_serialize()'d (chrono_ingest.h) -- the same
 * canonical encoding chrono_merkle.c hashes as Merkle leaves, so the
 * bytes this file writes are EXACTLY the bytes both the Merkle root and
 * the block-hash chain were computed over; nothing is re-encoded or
 * re-interpreted between "sealed in RAM" and "written to disk".
 *
 * ============================== What is, and is not, cryptographically checked =
 * chrono_storage_verify_file() (below) recomputes and checks:
 *   - the Merkle root over each window's blocks, against that window's
 *     stored merkle_root;
 *   - each block's own block_hash (recomputed from its stored fields) and
 *     its prev_hash link to the PREVIOUS block's block_hash -- carried
 *     CONTINUOUSLY across window boundaries (chrono_ingest_verify_block_chain(),
 *     chrono_ingest.h), so windows are a storage/Merkle-batching unit
 *     only, never a break in the underlying hash chain's own continuity.
 * A single altered byte anywhere in a window's block data, or in its
 * stored merkle_root, is therefore always caught (SHA-256's avalanche
 * property: a one-bit input change flips roughly half of every
 * downstream hash's output bits, so a coincidental "corrupted but still
 * matches" collision is not a realistic outcome, not merely an unlikely
 * one).
 *
 * What is NOT cryptographically checked: block_count and
 * window_timestamp_ns are read and used to parse the file, but neither is
 * committed to by any hash in this format (the requirement specifies the
 * header as exactly "Timestamp + Merkle Root", not a header-covering
 * hash). block_count corruption is still, in practice, almost always
 * caught STRUCTURALLY -- it changes how many bytes get consumed, which
 * cascades into either a short/truncated read (CHRONO_STORAGE_ERR_TRUNCATED)
 * or a Merkle root computed over the wrong block set (CHRONO_STORAGE_ERR_TAMPERED)
 * -- but this is a structural consequence, not a hash guarantee.
 * window_timestamp_ns corruption is a genuine, disclosed blind spot: nothing
 * in this file will ever flag a tampered timestamp on its own. A future
 * format revision wanting that guarantee would fold window_timestamp_ns
 * into what the Merkle root (or a new header-hash field) commits to; this
 * revision does not, because the requirement's own header layout was
 * exactly two fields with no third "header hash" named.
 *
 * ============================== Append-only, precisely ========================
 * chrono_storage_flush_window() opens the file via the raw Win32
 * CreateFileA() with dwDesiredAccess = FILE_APPEND_DATA specifically (not
 * the C run-time's "a"/"ab" fopen() mode, whose CRT-level "seek to EOF
 * then write" is a weaker guarantee than the kernel-enforced "every write
 * lands atomically at the CURRENT end of file, even under a concurrent
 * writer" semantics FILE_APPEND_DATA actually provides on Windows) --
 * this is the literal, real primitive the requirement names, not an
 * abstraction over it. dwCreationDisposition = OPEN_ALWAYS: the file is
 * created on the first flush and appended to on every one after that.
 * There is no code path in this file that opens the file for writing in
 * any mode other than append, and no code path that seeks or truncates
 * it -- a flushed window's bytes are never rewritten or removed by this
 * module, only ever added to.
 */

#ifndef CHRONO_STORAGE_H
#define CHRONO_STORAGE_H

#if !defined(_WIN32)
#error "chrono_storage.h's flush path uses Win32 CreateFileA(..., FILE_APPEND_DATA, ...) directly -- see the file header."
#endif

#include <stddef.h>
#include <stdint.h>

#include "chrono_merkle.h" /* chrono_block_t, CHRONO_MERKLE_MAX_LEAVES, chrono_block_serialize()/deserialize() */

/* A storage window's block_count is bounded by CHRONO_MERKLE_MAX_LEAVES:
 * every window is, by construction, exactly one Merkle tree's worth of
 * blocks (chrono_merkle_compute_root() could not have produced its
 * merkle_root otherwise). */
#define CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW CHRONO_MERKLE_MAX_LEAVES

/* "BLK1" as bytes B,L,K,'1', host-native order -- same magic-number
 * comment convention as wire_proto.h's WIRE_MAGIC. */
#define CHRONO_STORAGE_MAGIC 0x314B4C42u

#define CHRONO_STORAGE_WINDOW_HEADER_SIZE (4u + 4u + 8u + SHA256_DIGEST_SIZE) /* magic + block_count + window_timestamp_ns + merkle_root == 48 */

typedef enum {
    CHRONO_STORAGE_OK            = 0,
    CHRONO_STORAGE_ERR_INVALID   = -1, /* bad argument: NULL path/blocks/root, count == 0, or count > CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW */
    CHRONO_STORAGE_ERR_IO        = -2, /* CreateFileA/WriteFile/fopen/fread failed */
    CHRONO_STORAGE_ERR_TRUNCATED = -3, /* verify: file ends mid-header or mid-block -- an incomplete write, not necessarily malicious */
    CHRONO_STORAGE_ERR_TAMPERED  = -4  /* verify: a window's magic is wrong, its Merkle root does not match a recomputation over its own stored blocks, or the block-hash chain no longer links */
} chrono_storage_status_t;

/*
 * Appends exactly one window (header + count serialized blocks) to
 * filepath, opened strictly in FILE_APPEND_DATA mode (see file header).
 * merkle_root is the caller's own chrono_merkle_compute_root(blocks,
 * count, merkle_root) result -- this function does not recompute it, it
 * only writes what it is given (the caller-facing contract the
 * requirement's own signature specifies). Rejects count == 0 or count >
 * CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW as CHRONO_STORAGE_ERR_INVALID
 * without opening the file at all. On any I/O failure partway through a
 * window's bytes, the already-written prefix remains on disk (append-only
 * means this function never rewinds to undo a partial write) --
 * chrono_storage_verify_file() detects such a partial window as
 * CHRONO_STORAGE_ERR_TRUNCATED, not silent data loss.
 */
chrono_storage_status_t chrono_storage_flush_window(const char *filepath, chrono_block_t *blocks, size_t count,
                                                      uint8_t merkle_root[SHA256_DIGEST_SIZE]);

/*
 * Reads filepath from disk window by window (plain buffered fopen("rb")
 * -- no append semantics needed for a pure read), recomputing and
 * checking each window's Merkle root and the block-hash chain's
 * continuity across every block in the file (see file header for exactly
 * what is and is not covered). Bounded, streaming memory use: at most
 * CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW chrono_block_t are ever held at
 * once, regardless of total file size or window count. An empty (zero-
 * byte) file is CHRONO_STORAGE_OK (vacuously nothing to falsify, the same
 * posture chrono_ingest_verify_chain() takes for an empty in-RAM chain).
 */
chrono_storage_status_t chrono_storage_verify_file(const char *filepath);

#endif /* CHRONO_STORAGE_H */
