/*
 * chrono_ingest.h
 *
 * CHRONO-BLACKBOX -- asynchronous log ingestion module (ingestion ONLY).
 *
 * ============================== Scope, precisely =============================
 * Scoped to exactly what was asked: the ingestion stage of a hypothetical
 * "CHRONO-BLACKBOX" immutable log system, wired directly onto this
 * project's real, existing async network primitive (net_io.c's wire-mode
 * reactor: non-blocking Winsock, MSG_PEEK-routed, zero-copy recv()
 * straight into a wire_ring_t slot -- see net_io.h). It does NOT attempt
 * hash-chaining, Merkle proofs, Ed25519 signatures, PII sanitization, or
 * a CLI exporter -- those are separate concerns, explicitly out of scope
 * ("le seul module d'ingestion"). Building any of those on a network path
 * that has not first been shown to actually receive and durably queue a
 * byte, correctly, under real non-blocking I/O, would be exactly the kind
 * of unverified claim this project's other modules (barry_storage.h,
 * infra_mesh.h) already refuse to make. This module is that verified
 * foundation, nothing more.
 *
 * ============================== Design ========================================
 * Two real, existing primitives, composed -- not reimplemented:
 *   1. net_io_wire_server_t (net_io.c) -- the actual asynchronous
 *      ingestion: non-blocking accept()/recv(), one bounded poll pass per
 *      call, zero-copy straight into a wire_ring_t slot, O(1) tenant
 *      routing. Reused here verbatim.
 *   2. barry_task_queue_t + barry_ingest_wire_msg() (barry_storage.c) --
 *      the actual durable-ready sink: a bounded lock-free MPMC queue and
 *      its existing "extract/enqueue one already-received wire_msg_t"
 *      step. Reused here verbatim, not duplicated.
 *
 * chrono_ingest_poll_once() is the one new piece of logic this file adds:
 * it drains each registered stream's wire_ring_t (net_io_wire_server_t's
 * zero-copy landing slot) into the ONE shared append_queue. Because
 * append_queue's Vyukov MPMC ring is strictly FIFO (dequeue order ==
 * enqueue order, by construction of its per-slot sequence handoff -- see
 * barry_storage.h), draining every stream into one shared queue gives a
 * flat, gap-free ordering across tenants/streams for free, with no extra
 * bookkeeping needed on the write side. chrono_ingest_dequeue() assigns a
 * monotonic chrono_seq at READ time (dequeue order is already correct
 * append order, so numbering it there is sufficient and needs no extra
 * field smuggled into the queue payload).
 *
 * ============================== Block-linking (cryptographic sealing) ========
 * chrono_ingest_seal_next() (Part B, below) dequeues one durably-queued
 * entry and wraps it in a chrono_block_t sealed with real SHA-256
 * (sha256.h -- FIPS 180-4, not FNV-1a; see that file's own honesty notes
 * for why a STANDARD hash is being added here where telemetry_fabric.h
 * deliberately did not). Fields, exactly as specified: sequence_id,
 * timestamp_ns, tenant_id, prev_hash, payload_hash, block_hash.
 *
 * One disclosed, deliberate deviation from the literal formula
 * "block_hash = SHA256(seq||ts||tenant||prev_hash||payload)": chrono_block_t
 * (below) stores payload_hash but NOT the raw payload bytes -- by design,
 * a sealed block is meant to be small and self-contained, the same
 * "header commits to a digest, not the raw body" shape a real block
 * header uses. Under the LITERAL formula, chrono_ingest_verify_chain()
 * could not recompute block_hash from a stored block alone (the raw
 * payload would have to be supplied separately, from some other retained
 * copy, defeating the point of having payload_hash at all). This file
 * therefore computes
 *     block_hash = SHA256(sequence_id || timestamp_ns || tenant_id || prev_hash || payload_hash)
 * instead -- payload_hash still cryptographically commits to every byte
 * of the original payload (computed once, at seal time, from the real
 * payload -- see chrono_ingest_seal_next()), and block_hash now covers
 * every field actually stored in the block, making
 * chrono_ingest_verify_chain() fully self-contained: it recomputes and
 * checks every hash from ctx->chain alone, no external payload archive
 * required. A single bit flipped ANYWHERE in a sealed block -- including
 * payload_hash itself -- still breaks this recomputation, so "no byte
 * altered" (the requirement's own words) is still exactly what gets
 * verified; only the formula's second-order shape (digest-of-digest
 * instead of digest-of-raw-bytes for the header hash) changed, and only
 * because the specified struct layout requires it.
 *
 * Concatenation is host-native byte order for the fixed-width integer
 * fields (memcpy of each field's in-memory representation) -- the same
 * disclosed, single-machine-scope tradeoff wire_proto.h already makes for
 * its own wire format, for the same reason (see that file's endianness
 * note).
 *
 * timestamp_ns is a REAL wall-clock nanosecond count (Windows
 * GetSystemTimePreciseAsFileTime, converted from 100ns FILETIME ticks
 * since 1601-01-01 to ns since the Unix epoch) -- deliberately NOT the
 * "opaque caller-supplied tick, never a wall clock" posture
 * barry_storage.h's barry_ts_store_t uses elsewhere in this project. That
 * posture existed there to avoid a hidden external-time dependency for a
 * metric counter whose meaning the caller owns; here, the field is
 * explicitly named and specified as a nanosecond timestamp, which only a
 * real clock read can honestly provide. Precision note, stated plainly:
 * the OS API's real resolution is typically sub-microsecond, not a
 * guaranteed exact nanosecond -- "timestamp_ns" describes the field's
 * UNIT (nanoseconds since epoch), not a claim of true 1ns hardware
 * resolution.
 *
 * ============================== Backpressure, precisely ======================
 * chrono_ingest_poll_once() runs single-threaded by contract (below), so
 * the append_queue occupancy it observes via barry_task_queue_count()
 * cannot be raced UPWARD by another producer between that check and the
 * following barry_ingest_wire_msg() call -- there is no other producer.
 * A concurrent consumer (a different thread calling
 * chrono_ingest_dequeue()) can only make MORE room, never less. So the
 * count() check genuinely gates entry: if append_queue is at capacity,
 * the frame is left UNPOPPED in its wire_ring_t (wire_ring_try_pop() is
 * only called once room has already been confirmed) -- not lost, just
 * deferred to the next poll. The barry_ingest_wire_msg() return value is
 * still checked and counted as a defensive fallback, not an expected
 * path. Only the wire_ring_t itself (bounded, WIRE_RING_CAPACITY slots)
 * can apply real, counted backpressure all the way back to
 * net_io_wire_poll_once() -> wire_ring_reserve() ->
 * net_io_wire_stats_t.rejected_ring_full, exactly as net_io.c already
 * implements; this module does not need to touch that path.
 *
 * ============================== Concurrency & allocation ======================
 * chrono_ingest_poll_once() must be called from one thread only (it calls
 * net_io_wire_poll_once() and wire_ring_try_pop(), both single-consumer
 * by their own contracts). chrono_ingest_dequeue() remains safe to call
 * from a different thread than the poller (barry_task_queue_t's MPMC
 * contract, unaffected -- this is the intended shape: one ingestion
 * thread, any number of downstream consumer threads). Zero dynamic
 * allocation: every stream ring and the append_queue are plain fields of
 * chrono_ingest_ctx_t, sized at compile time.
 */

#ifndef CHRONO_INGEST_H
#define CHRONO_INGEST_H

#if !defined(_WIN32)
#error "chrono_ingest.h wraps net_io.c's wire-mode server, which is Winsock-specific -- see net_io.h."
#endif

#include <stddef.h>
#include <stdint.h>

#include "net_io.h"
#include "wire_proto.h"
#include "barry_storage.h"
#include "sha256.h"

#define CHRONO_INGEST_MAX_STREAMS NET_IO_WIRE_MAX_ACTORS

typedef enum {
    CHRONO_INGEST_OK              = 0,
    CHRONO_INGEST_ERR_INVALID     = -1,
    CHRONO_INGEST_ERR_NET         = -2, /* net_io_wire_init()/register call failed -- see ctx->last_net_status */
    CHRONO_INGEST_ERR_EMPTY       = -3, /* chrono_ingest_seal_next(): append_queue had nothing to seal */
    CHRONO_INGEST_ERR_CHAIN_FULL  = -4, /* chrono_ingest_seal_next(): CHRONO_CHAIN_CAPACITY reached */
    CHRONO_INGEST_ERR_TAMPERED    = -5  /* chrono_ingest_verify_chain(): a stored hash no longer matches its recomputation */
} chrono_ingest_status_t;

/* Assembled at dequeue time from a durably-queued barry_task_t -- see
 * "Design" above for why chrono_seq needs no storage in the queue itself. */
typedef struct {
    uint64_t   chrono_seq;  /* monotonic, gap-free across every stream: append_queue's FIFO dequeue order */
    uint32_t   stream_id;   /* which registered stream produced this frame (barry_task_t.producer_id) */
    wire_msg_t frame;       /* raw, already zero-copy-ingested wire_msg_t -- byte-identical to what net_io.c received */
} chrono_log_entry_t;

typedef struct {
    uint64_t             frames_appended;     /* wire frame -> append_queue, successfully, this process */
    uint64_t             append_backpressure; /* append_queue was at capacity at least once when a frame was ready to drain */
    net_io_wire_stats_t  net;                 /* passthrough: net_io.c's own real counters */
    size_t                append_queue_depth;  /* O(1) snapshot, may be stale by the time it's read -- see barry_task_queue_count() */
} chrono_ingest_stats_t;

/* ==================================================================== */
/* Part B -- Block-linking: SHA-256-sealed, hash-chained log blocks      */
/* ==================================================================== */

#define CHRONO_CHAIN_CAPACITY 4096u /* fixed, pre-allocated block count -- never grows, same posture as tf_log_t (telemetry_fabric.h) */

/* GENESIS_HASH: prev_hash for block 0 -- there is no predecessor. All-
 * zero, same "there is no valid real hash that collides with the
 * all-zero sentinel by chance" posture as TF_LOG_GENESIS_CHECKSUM (0) in
 * telemetry_fabric.h, just 32 bytes of it instead of a single 0. */
extern const uint8_t CHRONO_GENESIS_HASH[SHA256_DIGEST_SIZE];

typedef struct {
    uint64_t sequence_id;                      /* == the sealed entry's chrono_seq */
    uint64_t timestamp_ns;                     /* real wall-clock ns since Unix epoch -- see file header */
    uint32_t tenant_id;                        /* the wire frame's tenant_id, widened */
    uint8_t  prev_hash[SHA256_DIGEST_SIZE];    /* previous block's block_hash, or CHRONO_GENESIS_HASH for block 0 */
    uint8_t  payload_hash[SHA256_DIGEST_SIZE]; /* SHA256(payload[0..payload_len)) -- the real payload bytes, at seal time */
    uint8_t  block_hash[SHA256_DIGEST_SIZE];   /* SHA256(sequence_id||timestamp_ns||tenant_id||prev_hash||payload_hash) -- see file header for why payload_hash, not raw payload */
} chrono_block_t;

typedef struct {
    net_io_wire_server_t netio;                                /* real net_io.c async reactor, wire mode */
    wire_ring_t           streams[CHRONO_INGEST_MAX_STREAMS];   /* one zero-copy landing ring per registered log stream */
    uint32_t              stream_count;
    barry_task_queue_t     append_queue;                        /* durable-ready sink, shared across every stream */

    uint64_t               next_dequeue_seq;
    uint64_t               frames_appended;
    uint64_t               append_backpressure;
    net_io_status_t        last_net_status;

    chrono_block_t          chain[CHRONO_CHAIN_CAPACITY]; /* embedded: zero dynamic allocation, ever */
    uint64_t                chain_len;                     /* number of sealed blocks, <= CHRONO_CHAIN_CAPACITY */
} chrono_ingest_ctx_t;

chrono_ingest_status_t chrono_ingest_init(chrono_ingest_ctx_t *ctx, uint16_t bind_port);
uint16_t                chrono_ingest_get_port(const chrono_ingest_ctx_t *ctx);

/* Registers a new, bounded log stream (< CHRONO_INGEST_MAX_STREAMS) and
 * returns its stream_id via *out_stream_id. Boot-time-only, like this
 * project's other *_register_* calls. */
chrono_ingest_status_t chrono_ingest_register_stream(chrono_ingest_ctx_t *ctx, uint32_t *out_stream_id);

/* Maps a wire tenant_id to a previously registered stream_id -- O(1),
 * delegates to net_io_wire_register_tenant(). */
chrono_ingest_status_t chrono_ingest_register_tenant(uint16_t tenant_id, uint32_t stream_id);

/* One non-blocking pass: net_io_wire_poll_once() (accept + recv, zero-copy
 * into the target stream ring), then drains every stream ring into
 * append_queue, bounded by backpressure (see file header). Returns the
 * number of frames actually appended during this call. Single-threaded
 * caller only -- see "Concurrency & allocation" above. */
uint32_t chrono_ingest_poll_once(chrono_ingest_ctx_t *ctx);

/* Dequeues at most one durably-queued log entry. Safe to call from a
 * different thread than the one driving chrono_ingest_poll_once().
 * BARRY_ERR_EMPTY if nothing is queued (not a failure). */
barry_status_t chrono_ingest_dequeue(chrono_ingest_ctx_t *ctx, chrono_log_entry_t *out);

void chrono_ingest_get_stats(const chrono_ingest_ctx_t *ctx, chrono_ingest_stats_t *out);
void chrono_ingest_shutdown(chrono_ingest_ctx_t *ctx);

/*
 * Dequeues one durably-queued entry (chrono_ingest_dequeue(), internally)
 * and seals it as the next block of ctx->chain: prev_hash is the current
 * chain tip's block_hash (or CHRONO_GENESIS_HASH if ctx->chain_len == 0),
 * payload_hash is computed from the REAL payload bytes
 * (entry.frame.payload[0..payload_len)), and block_hash is computed per
 * the formula in the file header. On success (CHRONO_INGEST_OK), the
 * sealed block is appended to ctx->chain and also copied to *out_block
 * (optional -- pass NULL to ignore). CHRONO_INGEST_ERR_EMPTY if
 * append_queue had nothing to seal (not a failure); CHRONO_INGEST_ERR_CHAIN_FULL
 * once CHRONO_CHAIN_CAPACITY sealed blocks already exist. Same
 * single-threaded-caller contract as chrono_ingest_poll_once() (it calls
 * chrono_ingest_dequeue() and then mutates ctx->chain/chain_len, neither
 * of which is safe for concurrent callers of THIS function).
 */
chrono_ingest_status_t chrono_ingest_seal_next(chrono_ingest_ctx_t *ctx, chrono_block_t *out_block);

/*
 * Walks ctx->chain[0..chain_len) in order. For each block: recomputes
 * block_hash from that block's OWN stored fields (sequence_id,
 * timestamp_ns, tenant_id, prev_hash, payload_hash) and confirms it
 * matches the stored block_hash; confirms prev_hash equals the PREVIOUS
 * block's block_hash (or CHRONO_GENESIS_HASH for block 0). On the first
 * mismatch of either check, sets *out_first_broken_index (if non-NULL) to
 * that block's index and returns CHRONO_INGEST_ERR_TAMPERED.
 * CHRONO_INGEST_OK means every sealed block is provably byte-identical to
 * what chrono_ingest_seal_next() produced. Read-only: safe to call from
 * any thread concurrently with chrono_ingest_seal_next() in the sense
 * that it never crashes, but a verify racing a concurrent seal may
 * observe chain_len mid-update (not a torn READ of any individual
 * block -- blocks already counted in chain_len are complete and are never
 * modified afterward) -- for a guaranteed-quiescent check, call this
 * after ingestion has stopped, as the accompanying test does.
 */
chrono_ingest_status_t chrono_ingest_verify_chain(const chrono_ingest_ctx_t *ctx, uint64_t *out_first_broken_index);

/* ==================================================================== */
/* Part C -- Canonical on-disk / Merkle-leaf block encoding              */
/* ==================================================================== */

/* Exact, explicit-field-order byte size of a serialized chrono_block_t --
 * NOT sizeof(chrono_block_t) (whose real size includes compiler-inserted
 * trailing padding: 8+8+4+32+32+32 = 116 real field bytes, padded by the
 * compiler up to a multiple of the struct's own 8-byte alignment, i.e.
 * 120 -- see chrono_block_serialize()'s own comment for why this project
 * never hashes/writes sizeof(chrono_block_t) raw bytes). chrono_merkle.c
 * (leaf hashing) and chrono_storage.c (the on-disk format) both use this
 * one canonical encoding, so neither can silently drift from the other. */
#define CHRONO_BLOCK_SERIALIZED_SIZE (8u + 8u + 4u + SHA256_DIGEST_SIZE * 3u)

/*
 * Flattens *blk into out[CHRONO_BLOCK_SERIALIZED_SIZE], field by field, in
 * a fixed, explicit order (sequence_id, timestamp_ns, tenant_id,
 * prev_hash, payload_hash, block_hash) -- deliberately NOT a raw memcpy of
 * sizeof(chrono_block_t) bytes: that size includes compiler-inserted
 * trailing padding whose CONTENT the C standard never guarantees (two
 * otherwise-identical blocks could differ in unused padding bytes
 * depending on how they were constructed, which would make hashing or
 * on-disk-comparing the raw struct nondeterministic -- exactly the hidden
 * footgun this explicit field-by-field approach avoids). Host-native byte
 * order for the integer fields, the same disclosed, single-machine-scope
 * tradeoff wire_proto.h and this file's own block_hash preimage already
 * make.
 */
void chrono_block_serialize(const chrono_block_t *blk, uint8_t out[CHRONO_BLOCK_SERIALIZED_SIZE]);

/* Inverse of chrono_block_serialize(): reconstructs *out from
 * in[CHRONO_BLOCK_SERIALIZED_SIZE] (e.g. bytes just read back from disk).
 * Same host-native-order contract as the serializer. */
void chrono_block_deserialize(const uint8_t in[CHRONO_BLOCK_SERIALIZED_SIZE], chrono_block_t *out);

/* Real wall-clock nanoseconds since the Unix epoch -- the same clock read
 * chrono_ingest_seal_next() uses for a block's own timestamp_ns, exposed
 * here so chrono_storage.c can stamp a window header's timestamp with the
 * SAME clock rather than reimplementing the FILETIME conversion a second
 * time. See "Block-linking" above for the precision/wall-clock-vs-opaque-
 * tick disclosure. */
uint64_t chrono_wall_clock_ns(void);

/*
 * Verifies a flat array of blocks[0..count) as one hash chain, given the
 * hash the FIRST block's prev_hash must equal (CHRONO_GENESIS_HASH for a
 * chain's true beginning; a previous window's last block_hash when
 * resuming verification across a storage window boundary -- see
 * chrono_storage.c). chrono_ingest_verify_chain() above is a thin
 * wrapper over this exact function (ctx->chain, ctx->chain_len,
 * CHRONO_GENESIS_HASH) -- sharing one implementation so the in-RAM check
 * and the on-disk check can never silently diverge.
 */
chrono_ingest_status_t chrono_ingest_verify_block_chain(const chrono_block_t *blocks, size_t count,
                                                          const uint8_t expected_first_prev_hash[SHA256_DIGEST_SIZE],
                                                          uint64_t *out_first_broken_index);

#endif /* CHRONO_INGEST_H */
