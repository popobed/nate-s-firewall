#include "chrono_ingest.h"

#include <string.h>
#include <windows.h> /* GetSystemTimePreciseAsFileTime -- net_io.h already defines WIN32_LEAN_AND_MEAN
                      * and pulls in winsock2.h, so this does not re-declare the old winsock.h
                      * behind our back (same note as test_net_io.c's). */

chrono_ingest_status_t chrono_ingest_init(chrono_ingest_ctx_t *ctx, uint16_t bind_port) {
    if (ctx == NULL) {
        return CHRONO_INGEST_ERR_INVALID;
    }
    memset(ctx, 0, sizeof *ctx);

    net_io_status_t st = net_io_wire_init(&ctx->netio, bind_port);
    if (st != NET_IO_OK) {
        ctx->last_net_status = st;
        return CHRONO_INGEST_ERR_NET;
    }

    for (uint32_t i = 0; i < CHRONO_INGEST_MAX_STREAMS; i++) {
        wire_ring_init(&ctx->streams[i]);
    }
    ctx->stream_count = 0u;

    barry_task_queue_init(&ctx->append_queue);

    ctx->next_dequeue_seq = 0u;
    ctx->frames_appended = 0u;
    ctx->append_backpressure = 0u;
    ctx->last_net_status = NET_IO_OK;
    return CHRONO_INGEST_OK;
}

uint16_t chrono_ingest_get_port(const chrono_ingest_ctx_t *ctx) {
    if (ctx == NULL) {
        return 0u;
    }
    return net_io_wire_get_port(&ctx->netio);
}

chrono_ingest_status_t chrono_ingest_register_stream(chrono_ingest_ctx_t *ctx, uint32_t *out_stream_id) {
    if (ctx == NULL || out_stream_id == NULL || ctx->stream_count >= CHRONO_INGEST_MAX_STREAMS) {
        return CHRONO_INGEST_ERR_INVALID;
    }
    uint32_t id = ctx->stream_count;
    net_io_status_t st = net_io_wire_register_actor(&ctx->netio, (uint8_t)id, &ctx->streams[id]);
    if (st != NET_IO_OK) {
        ctx->last_net_status = st;
        return CHRONO_INGEST_ERR_NET;
    }
    ctx->stream_count++;
    *out_stream_id = id;
    return CHRONO_INGEST_OK;
}

chrono_ingest_status_t chrono_ingest_register_tenant(uint16_t tenant_id, uint32_t stream_id) {
    if (stream_id >= CHRONO_INGEST_MAX_STREAMS) {
        return CHRONO_INGEST_ERR_INVALID;
    }
    net_io_status_t st = net_io_wire_register_tenant(tenant_id, (uint8_t)stream_id);
    return (st == NET_IO_OK) ? CHRONO_INGEST_OK : CHRONO_INGEST_ERR_INVALID;
}

/* Drains one stream's zero-copy landing ring into append_queue, gated by
 * backpressure -- see the file header's "Backpressure, precisely". */
static uint32_t chrono_ingest_drain_stream(chrono_ingest_ctx_t *ctx, uint32_t stream_id) {
    uint32_t appended = 0u;
    wire_ring_t *ring = &ctx->streams[stream_id];

    for (;;) {
        if (barry_task_queue_count(&ctx->append_queue) >= BARRY_TASK_QUEUE_CAPACITY) {
            ctx->append_backpressure++;
            break; /* durable sink at capacity: leave the frame in its ring, retry next poll */
        }

        wire_msg_t frame;
        if (wire_ring_try_pop(ring, &frame) != WIRE_OK) {
            break; /* nothing more ready in this stream right now */
        }
        if (frame.type == WIRE_MSG_TYPE_DISCARD) {
            continue; /* aborted mid-flight reservation (peer disconnect): not a log entry */
        }

        barry_status_t bst = barry_ingest_wire_msg(&ctx->append_queue, stream_id, &frame);
        if (bst != BARRY_OK) {
            /* Defensive fallback only -- see file header: single-threaded
             * poll_once() means the count() check above cannot be raced
             * upward by another producer, so this path is not expected
             * to trigger in normal operation. */
            ctx->append_backpressure++;
            break;
        }
        ctx->frames_appended++;
        appended++;
    }
    return appended;
}

uint32_t chrono_ingest_poll_once(chrono_ingest_ctx_t *ctx) {
    if (ctx == NULL) {
        return 0u;
    }

    net_io_wire_poll_once(&ctx->netio);

    uint32_t appended = 0u;
    for (uint32_t i = 0; i < ctx->stream_count; i++) {
        appended += chrono_ingest_drain_stream(ctx, i);
    }
    return appended;
}

barry_status_t chrono_ingest_dequeue(chrono_ingest_ctx_t *ctx, chrono_log_entry_t *out) {
    if (ctx == NULL || out == NULL) {
        return BARRY_ERR_INVALID;
    }
    barry_task_t task;
    barry_status_t st = barry_task_dequeue(&ctx->append_queue, &task);
    if (st != BARRY_OK) {
        return st;
    }
    if (task.type != BARRY_TASK_TYPE_WIRE_MSG || task.payload_len != (uint32_t)sizeof(wire_msg_t)) {
        return BARRY_ERR_INVALID;
    }
    out->chrono_seq = ctx->next_dequeue_seq++;
    out->stream_id = task.producer_id;
    memcpy(&out->frame, task.payload, sizeof out->frame);
    return BARRY_OK;
}

void chrono_ingest_get_stats(const chrono_ingest_ctx_t *ctx, chrono_ingest_stats_t *out) {
    if (ctx == NULL || out == NULL) {
        return;
    }
    out->frames_appended = ctx->frames_appended;
    out->append_backpressure = ctx->append_backpressure;
    net_io_wire_get_stats(&ctx->netio, &out->net);
    out->append_queue_depth = barry_task_queue_count(&ctx->append_queue);
}

void chrono_ingest_shutdown(chrono_ingest_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    net_io_wire_shutdown(&ctx->netio);
}

/* ==================================================================== */
/* Part B -- Block-linking: SHA-256-sealed, hash-chained log blocks      */
/* ==================================================================== */

const uint8_t CHRONO_GENESIS_HASH[SHA256_DIGEST_SIZE] = {0};

/* Real wall-clock nanoseconds since the Unix epoch -- see chrono_ingest.h's
 * "Block-linking" section for why this field is a real clock read rather
 * than this project's usual opaque-tick posture. GetSystemTimePreciseAsFileTime
 * (Windows 8+) reports 100ns-tick FILETIME (100ns ticks since 1601-01-01);
 * EPOCH_DIFF_100NS is the fixed, well-known offset between that epoch and
 * 1970-01-01 in the same 100ns units. Not static: chrono_storage.c reuses
 * this exact clock read for a window header's own timestamp -- see
 * chrono_ingest.h's Part C. */
uint64_t chrono_wall_clock_ns(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ticks;
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;

    const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    uint64_t unix_100ns = ticks.QuadPart - EPOCH_DIFF_100NS;
    return unix_100ns * 100ULL;
}

/* Builds the exact byte sequence chrono_ingest.h's "Block-linking" section
 * specifies for block_hash and hashes it -- shared by both
 * chrono_ingest_seal_next() (computing a NEW block_hash) and
 * chrono_ingest_verify_chain() (recomputing an EXISTING one), so the two
 * can never silently drift apart. */
static void chrono_compute_block_hash(uint64_t sequence_id, uint64_t timestamp_ns, uint32_t tenant_id,
                                       const uint8_t prev_hash[SHA256_DIGEST_SIZE],
                                       const uint8_t payload_hash[SHA256_DIGEST_SIZE],
                                       uint8_t out_block_hash[SHA256_DIGEST_SIZE]) {
    uint8_t preimage[sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) +
                      SHA256_DIGEST_SIZE + SHA256_DIGEST_SIZE];
    size_t off = 0u;

    memcpy(preimage + off, &sequence_id, sizeof sequence_id); off += sizeof sequence_id;
    memcpy(preimage + off, &timestamp_ns, sizeof timestamp_ns); off += sizeof timestamp_ns;
    memcpy(preimage + off, &tenant_id, sizeof tenant_id); off += sizeof tenant_id;
    memcpy(preimage + off, prev_hash, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
    memcpy(preimage + off, payload_hash, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;

    sha256_hash(preimage, off, out_block_hash);
}

chrono_ingest_status_t chrono_ingest_seal_next(chrono_ingest_ctx_t *ctx, chrono_block_t *out_block) {
    if (ctx == NULL) {
        return CHRONO_INGEST_ERR_INVALID;
    }
    if (ctx->chain_len >= CHRONO_CHAIN_CAPACITY) {
        return CHRONO_INGEST_ERR_CHAIN_FULL;
    }

    chrono_log_entry_t entry;
    if (chrono_ingest_dequeue(ctx, &entry) != BARRY_OK) {
        return CHRONO_INGEST_ERR_EMPTY; /* append_queue had nothing ready to seal */
    }

    chrono_block_t *blk = &ctx->chain[ctx->chain_len];
    blk->sequence_id = entry.chrono_seq;
    blk->timestamp_ns = chrono_wall_clock_ns();
    blk->tenant_id = (uint32_t)entry.frame.tenant_id;

    if (ctx->chain_len == 0u) {
        memcpy(blk->prev_hash, CHRONO_GENESIS_HASH, SHA256_DIGEST_SIZE);
    } else {
        memcpy(blk->prev_hash, ctx->chain[ctx->chain_len - 1u].block_hash, SHA256_DIGEST_SIZE);
    }

    /* payload_hash is computed from the REAL payload bytes -- the one
     * point in this file that still needs the raw wire_msg_t, exactly
     * once, at seal time. Not re-derivable later from the block alone,
     * by design -- see the file header. */
    sha256_hash(entry.frame.payload, entry.frame.payload_len, blk->payload_hash);

    chrono_compute_block_hash(blk->sequence_id, blk->timestamp_ns, blk->tenant_id,
                               blk->prev_hash, blk->payload_hash, blk->block_hash);

    if (out_block != NULL) {
        *out_block = *blk;
    }
    ctx->chain_len++;
    return CHRONO_INGEST_OK;
}

chrono_ingest_status_t chrono_ingest_verify_block_chain(const chrono_block_t *blocks, size_t count,
                                                          const uint8_t expected_first_prev_hash[SHA256_DIGEST_SIZE],
                                                          uint64_t *out_first_broken_index) {
    if (blocks == NULL || expected_first_prev_hash == NULL) {
        return CHRONO_INGEST_ERR_INVALID;
    }

    const uint8_t *expected_prev = expected_first_prev_hash;

    for (size_t i = 0; i < count; i++) {
        const chrono_block_t *blk = &blocks[i];

        if (memcmp(blk->prev_hash, expected_prev, SHA256_DIGEST_SIZE) != 0) {
            if (out_first_broken_index != NULL) {
                *out_first_broken_index = (uint64_t)i;
            }
            return CHRONO_INGEST_ERR_TAMPERED;
        }

        uint8_t recomputed[SHA256_DIGEST_SIZE];
        chrono_compute_block_hash(blk->sequence_id, blk->timestamp_ns, blk->tenant_id,
                                   blk->prev_hash, blk->payload_hash, recomputed);

        if (memcmp(recomputed, blk->block_hash, SHA256_DIGEST_SIZE) != 0) {
            if (out_first_broken_index != NULL) {
                *out_first_broken_index = (uint64_t)i;
            }
            return CHRONO_INGEST_ERR_TAMPERED;
        }

        expected_prev = blk->block_hash;
    }
    return CHRONO_INGEST_OK;
}

chrono_ingest_status_t chrono_ingest_verify_chain(const chrono_ingest_ctx_t *ctx, uint64_t *out_first_broken_index) {
    if (ctx == NULL) {
        return CHRONO_INGEST_ERR_INVALID;
    }
    return chrono_ingest_verify_block_chain(ctx->chain, (size_t)ctx->chain_len, CHRONO_GENESIS_HASH,
                                             out_first_broken_index);
}

/* ==================================================================== */
/* Part C -- Canonical on-disk / Merkle-leaf block encoding              */
/* ==================================================================== */

void chrono_block_serialize(const chrono_block_t *blk, uint8_t out[CHRONO_BLOCK_SERIALIZED_SIZE]) {
    size_t off = 0u;
    memcpy(out + off, &blk->sequence_id, sizeof blk->sequence_id); off += sizeof blk->sequence_id;
    memcpy(out + off, &blk->timestamp_ns, sizeof blk->timestamp_ns); off += sizeof blk->timestamp_ns;
    memcpy(out + off, &blk->tenant_id, sizeof blk->tenant_id); off += sizeof blk->tenant_id;
    memcpy(out + off, blk->prev_hash, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
    memcpy(out + off, blk->payload_hash, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
    memcpy(out + off, blk->block_hash, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
}

void chrono_block_deserialize(const uint8_t in[CHRONO_BLOCK_SERIALIZED_SIZE], chrono_block_t *out) {
    size_t off = 0u;
    memcpy(&out->sequence_id, in + off, sizeof out->sequence_id); off += sizeof out->sequence_id;
    memcpy(&out->timestamp_ns, in + off, sizeof out->timestamp_ns); off += sizeof out->timestamp_ns;
    memcpy(&out->tenant_id, in + off, sizeof out->tenant_id); off += sizeof out->tenant_id;
    memcpy(out->prev_hash, in + off, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
    memcpy(out->payload_hash, in + off, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
    memcpy(out->block_hash, in + off, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
}
