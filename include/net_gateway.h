/*
 * net_gateway.h
 *
 * Phase 5 (Niveau 29) -- High-throughput network gateway: OS-bypass-style
 * frame ingestion, O(1) fixed-layout header parsing, and zero-copy routing
 * into per-actor MPSC mailboxes. Self-contained (no dependency on any
 * other project) -- this file owns its own mailbox/slab primitives rather
 * than importing them.
 *
 * ============================ Honest scope ================================
 * "Bypass Abstraction": true kernel-bypass I/O (DPDK, AF_XDP, io_uring
 * fixed buffers) is privileged, OS- and NIC-driver-specific, and not
 * expressible in portable ISO C11 -- it cannot live in this file. What
 * DOES live here is the abstraction boundary such a layer would hand off
 * to: ngw_ingest_frame() takes a raw byte buffer exactly as if it were a
 * DMA ring slot/mbuf/io_uring buffer already delivered by that privileged
 * layer, and does everything from there (parse, authenticate, route)
 * without assuming, or requiring, any particular OS transport underneath.
 * Wiring a real bypass driver into that boundary is deployment-specific
 * plumbing, deliberately out of scope for a portable library.
 *
 * "O(1) HTTP/Webhook/UDP header parsing": this does NOT parse raw HTTP/1.1
 * text (arbitrary header lines, folding, chunked encoding...) -- doing
 * that safely fundamentally requires scanning for delimiters, which is not
 * a bounded-step operation. Instead, `protocol` is a provenance tag on a
 * single FIXED, versioned binary wire format (see "Wire format" below),
 * modeling frames already normalized by an edge terminator (reverse
 * proxy, webhook relay, UDP demultiplexer) before they reach this
 * ultra-low-latency path -- the same "fixed frame, not general parsing"
 * choice this project's bin_parser-equivalent code makes elsewhere.
 * "O(1)" here means what it means throughout this codebase family: a
 * fixed, small number of field reads plus ONE bounded copy of length <=
 * NGW_MAX_PAYLOAD -- no unbounded loop, no delimiter scan -- not "zero
 * cost regardless of payload size" (impossible for any parser that reads
 * the payload at all).
 *
 * "Zero-copy... sans copie intermediaire": the payload is copied exactly
 * ONCE per frame -- from the caller-supplied wire buffer (the bypass
 * layer's buffer) into a gateway-owned slab block. From there on, only a
 * 16-byte {ptr,len,tenant_id,protocol} descriptor moves: it is what gets
 * posted into the target actor's mailbox, never the payload bytes
 * themselves. The actor reads the payload in place via that pointer and
 * calls ngw_release_payload() when done. Copying the very first time
 * bytes cross from an external buffer into memory this module owns is
 * unavoidable (the source buffer's lifetime is the caller's, not ours);
 * "zero-copy" describes every hop after that.
 *
 * ============================ Wire format ==================================
 * All multi-byte integers little-endian. Total length = NGW_HEADER_SIZE +
 * payload_len + NGW_HASH_SIZE.
 *
 *   offset  0 : magic        u32  = NGW_MAGIC ("NGWF")
 *   offset  4 : protocol     u8   (ngw_protocol_t)
 *   offset  5 : version      u8   = NGW_VERSION
 *   offset  6 : tenant_id    u16
 *   offset  8 : api_key      NGW_API_KEY_LEN bytes
 *   offset 40 : payload_len  u16  (<= NGW_MAX_PAYLOAD)
 *   offset 42 : payload      payload_len bytes
 *   offset 42+payload_len : hash u64 = FNV-1a/64 over bytes [0, 42+payload_len)
 *
 * api_key is verified against the tenant's registered key with
 * ct_verify_token() (crypto_constant_time.h) -- Niveau 30 of this phase.
 * The integrity hash is also compared with ct_equal() for uniformity, even
 * though (being an unkeyed checksum, not a MAC) its own timing carries no
 * exploitable secret.
 *
 * ============================ Determinism ==================================
 *   - Fixed compile-time capacities everywhere (mailbox slots, slab
 *     blocks, tenant/actor tables): zero dynamic allocation, ever, after
 *     any ngw_*_init() call -- indeed there is no allocation AT init
 *     either, since every struct here embeds its storage directly.
 *   - ngw_mailbox_t is the same corrected MPSC design as this project's
 *     prior actor-mailbox work: producers reserve a slot with a CAS loop
 *     on `tail`, gated by "tail - head < CAPACITY" (head read with
 *     memory_order_acquire, matching the consumer's memory_order_release
 *     store) -- NOT by a per-slot flag set after the write, which would
 *     reopen a reservation-before-publish race window on wraparound (a
 *     real bug found and fixed in this project's earlier actor-mailbox
 *     module; this file starts from the corrected design directly).
 *     ready/1 is published with memory_order_release; the consumer checks
 *     it with memory_order_acquire before reading anything else.
 *   - ngw_slab_t is a lock-free Treiber-stack free list (32-bit index +
 *     32-bit ABA generation packed into one 64-bit CAS target) -- alloc/
 *     free are each a single bounded-retry CAS, true O(1), and unlike the
 *     mailbox's reserve-then-publish sequence there is no window between
 *     "own the block" and "safe to use it": one CAS transfers the whole
 *     block atomically.
 *   - Cross-tenant residual-data hardening (CWE-457): ngw_slab_free()
 *     zeroes a block's full NGW_MAX_PAYLOAD bytes before it re-enters the
 *     free list, not just the 4 bytes the intrusive free-list link
 *     itself needs. ngw_gateway_ingest() only ever writes `payload_len`
 *     bytes into a freshly allocated block, which is normally far
 *     smaller than NGW_MAX_PAYLOAD -- without this, a different tenant's
 *     leftover bytes from that block's previous occupant would still be
 *     sitting there, and correctness against reading them would rest
 *     entirely on every consumer, present and future, strictly
 *     respecting ngw_descriptor_t.len. Zeroed at the one point ownership
 *     actually changes hands instead.
 *   - 64-byte alignment: ngw_message_t is exactly 64 bytes (one cache
 *     line); the mailbox's tail/head cache lines are padded so producers
 *     and the consumer never false-share.
 */

#ifndef NET_GATEWAY_H
#define NET_GATEWAY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "crypto_constant_time.h"

#define NGW_CACHE_LINE_SIZE 64u

/* ------------------------------------------------------------------ */
/* Actor mailbox (MPSC, self-contained)                                */
/* ------------------------------------------------------------------ */

#define NGW_MAILBOX_CAPACITY     64u /* power of two */
#define NGW_MESSAGE_PAYLOAD_SIZE 48u /* sizeof(ngw_message_t) == 64 exactly */

_Static_assert(NGW_MAILBOX_CAPACITY != 0u &&
               (NGW_MAILBOX_CAPACITY & (NGW_MAILBOX_CAPACITY - 1u)) == 0u,
               "NGW_MAILBOX_CAPACITY must be a power of two");

typedef struct {
    _Alignas(NGW_CACHE_LINE_SIZE)
    _Atomic uint32_t ready;
    uint32_t         sender_id;
    uint32_t         type;
    uint32_t         _reserved;
    uint8_t          payload[NGW_MESSAGE_PAYLOAD_SIZE];
} ngw_message_t;

_Static_assert(sizeof(ngw_message_t) == 64u, "ngw_message_t must be exactly 64 bytes");
_Static_assert(_Alignof(ngw_message_t) == 64u, "ngw_message_t must be 64-byte aligned");

typedef struct {
    _Alignas(NGW_CACHE_LINE_SIZE)
    _Atomic uint64_t tail;
    uint8_t _pad_tail[NGW_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    _Alignas(NGW_CACHE_LINE_SIZE)
    _Atomic uint64_t head;
    uint8_t _pad_head[NGW_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    ngw_message_t slots[NGW_MAILBOX_CAPACITY];

    _Atomic uint64_t posted_total;
    _Atomic uint64_t consumed_total;
    _Atomic uint64_t rejected_full_total;
} ngw_mailbox_t;

/* ------------------------------------------------------------------ */
/* Gateway status / protocol tags                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    NGW_OK = 0,
    NGW_ERR_INVALID              = -1,  /* NULL/malformed arguments */
    NGW_ERR_EMPTY                 = -2,  /* mailbox: nothing ready to consume */
    NGW_ERR_TRUNCATED            = -3,  /* frame shorter than its declared length */
    NGW_ERR_BAD_MAGIC            = -4,
    NGW_ERR_PAYLOAD_TOO_LARGE    = -5,
    NGW_ERR_INTEGRITY            = -6,  /* hash mismatch: frame corrupted/tampered */
    NGW_ERR_UNKNOWN_TENANT       = -7,
    NGW_ERR_AUTH_FAILED          = -8,  /* api_key did not match (constant-time) */
    NGW_ERR_SLAB_EXHAUSTED       = -9,
    NGW_ERR_MAILBOX_FULL         = -10, /* O(1) instant rejection, no spin/wait */
    NGW_ERR_ACTOR_NOT_REGISTERED = -11
} ngw_status_t;

typedef enum {
    NGW_PROTO_HTTP    = 1,
    NGW_PROTO_WEBHOOK = 2,
    NGW_PROTO_UDP     = 3
} ngw_protocol_t;

/* Reserved ngw_message_t.type sentinel: published by ngw_mailbox_publish()
 * on an aborted reservation (see the note there) so a consumer can tell
 * "this slot was reserved but its source never completed" apart from any
 * real message and simply skip it -- never a valid `type` for real data. */
#define NGW_MSG_TYPE_DISCARD 0xFFFFFFFFu

/* ------------------------------------------------------------------ */
/* Wire format constants                                               */
/* ------------------------------------------------------------------ */

#define NGW_MAGIC       0x4657474Eu /* "NGWF" as bytes N,G,W,F little-endian */
#define NGW_VERSION     1u
#define NGW_API_KEY_LEN 32u
#define NGW_MAX_PAYLOAD 1024u
#define NGW_HEADER_SIZE (4u + 1u + 1u + 2u + NGW_API_KEY_LEN + 2u) /* = 42 */
#define NGW_HASH_SIZE   8u
#define NGW_MAX_FRAME_SIZE (NGW_HEADER_SIZE + NGW_MAX_PAYLOAD + NGW_HASH_SIZE)

/* Zero-copy descriptor posted into a mailbox -- the payload bytes
 * themselves never travel through the mailbox, only this. */
typedef struct {
    uint8_t *ptr;        /* gateway-slab-owned payload bytes, valid until ngw_release_payload() */
    uint16_t len;
    uint16_t tenant_id;
    uint8_t  protocol;   /* ngw_protocol_t */
    uint8_t  _pad[3];
} ngw_descriptor_t;

_Static_assert(sizeof(ngw_descriptor_t) <= NGW_MESSAGE_PAYLOAD_SIZE,
               "ngw_descriptor_t must fit in one mailbox message payload");

/* ------------------------------------------------------------------ */
/* Gateway state                                                       */
/* ------------------------------------------------------------------ */

#define NGW_SLAB_CAPACITY 64u  /* in-flight payload buffers */
#define NGW_MAX_TENANTS  256u  /* direct-indexed by tenant_id: O(1) lookup */
#define NGW_MAX_ACTORS     8u  /* registered mailboxes the gateway can route into */

typedef struct {
    _Alignas(NGW_CACHE_LINE_SIZE)
    _Atomic uint64_t free_head; /* hi32 = ABA generation, lo32 = free block index or NGW_SLAB_NIL */
    uint8_t _pad[NGW_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    uint8_t storage[NGW_SLAB_CAPACITY][NGW_MAX_PAYLOAD];
} ngw_slab_t;

typedef struct {
    uint8_t api_key[NGW_API_KEY_LEN];
    uint8_t actor_id;
    uint8_t registered;
} ngw_tenant_t;

typedef struct {
    uint64_t frames_ingested;
    uint64_t frames_accepted;
    uint64_t frames_rejected_bad_frame; /* truncated / bad magic / oversized / integrity */
    uint64_t frames_rejected_auth;      /* unknown tenant or api_key mismatch */
    uint64_t frames_rejected_capacity;  /* slab or mailbox exhausted */
} ngw_stats_t;

/* Large (tens of KB, dominated by the slab): intended for static/global
 * placement, not the stack. */
typedef struct {
    ngw_slab_t    slab;
    ngw_tenant_t  tenants[NGW_MAX_TENANTS];
    ngw_mailbox_t *actors[NGW_MAX_ACTORS]; /* caller-owned mailboxes */

    _Atomic uint64_t frames_ingested;
    _Atomic uint64_t frames_accepted;
    _Atomic uint64_t frames_rejected_bad_frame;
    _Atomic uint64_t frames_rejected_auth;
    _Atomic uint64_t frames_rejected_capacity;
} ngw_gateway_t;

/* ------------------------------------------------------------------ */
/* Mailbox API                                                         */
/* ------------------------------------------------------------------ */

void ngw_mailbox_init(ngw_mailbox_t *mb);

/* Any number of concurrent producer threads. Reserves a slot with a CAS
 * loop on `tail`, gated by the head-synchronized fullness check described
 * above; never blocks or retries past a full mailbox -- returns
 * NGW_ERR_MAILBOX_FULL immediately (O(1)) instead. */
ngw_status_t ngw_mailbox_post(ngw_mailbox_t *mb, uint32_t sender_id, uint32_t type,
                               const void *payload, size_t payload_len);

/* Single consumer thread only. NGW_ERR_EMPTY is not a failure, just
 * "nothing ready yet". */
ngw_status_t ngw_mailbox_consume(ngw_mailbox_t *mb, ngw_message_t *out);

/*
 * Low-level zero-copy producer API: reserve a slot and get a pointer to
 * ITS payload buffer, fill that buffer DIRECTLY from the true data source
 * (e.g. recv() straight into slot->payload, with no intermediate buffer
 * at all), then publish. ngw_mailbox_post() above copies from an
 * already-assembled buffer -- correct when the caller necessarily has one
 * already, but not truly zero-copy when the ORIGINAL source (a socket, a
 * DMA ring) could have written into the mailbox slot itself. This is the
 * pair net_io.c uses for recv()-direct-into-mailbox ingestion.
 *
 * Same CAS-on-tail/head-gated-fullness reservation as ngw_mailbox_post();
 * ngw_mailbox_publish() only sets sender_id/type and the release-store of
 * `ready` -- the payload bytes are the CALLER's responsibility (they were
 * the one writing directly into them).
 *
 * IMPORTANT: once ngw_mailbox_reserve() succeeds, the reservation MUST be
 * published -- with real data via ngw_mailbox_publish(), or, if the
 * source aborts partway through (e.g. a socket closes mid-frame), with
 * type == NGW_MSG_TYPE_DISCARD. `tail` is a monotonically increasing
 * counter shared by every producer: an un-published reservation leaves a
 * gap `head` can never advance past, permanently stalling every slot
 * behind it, not just this one. Publishing a discard sentinel instead of
 * silently dropping the reservation is what keeps the ring alive.
 */
ngw_status_t ngw_mailbox_reserve(ngw_mailbox_t *mb, ngw_message_t **out_slot);
void         ngw_mailbox_publish(ngw_mailbox_t *mb, ngw_message_t *slot,
                                  uint32_t sender_id, uint32_t type);

/* O(1) diagnostics (tail - head). Safe from any thread; may be stale by
 * the time the caller observes it, as with any live concurrent queue. */
size_t ngw_mailbox_count(const ngw_mailbox_t *mb);
bool   ngw_mailbox_is_full(const ngw_mailbox_t *mb);
bool   ngw_mailbox_is_empty(const ngw_mailbox_t *mb);

/* ------------------------------------------------------------------ */
/* Gateway API                                                         */
/* ------------------------------------------------------------------ */

void ngw_gateway_init(ngw_gateway_t *gw);

/* Registers `mailbox` (caller-owned, already ngw_mailbox_init()'d) as the
 * routing target for actor_id (< NGW_MAX_ACTORS). */
ngw_status_t ngw_register_actor(ngw_gateway_t *gw, uint8_t actor_id, ngw_mailbox_t *mailbox);

/* Registers tenant_id's authentication secret and which actor its traffic
 * routes to. api_key must be exactly NGW_API_KEY_LEN bytes. */
ngw_status_t ngw_register_tenant(ngw_gateway_t *gw, uint16_t tenant_id,
                                  const uint8_t api_key[NGW_API_KEY_LEN], uint8_t actor_id);

/*
 * Ingests one raw wire frame (see "Wire format" above): O(1) fixed-layout
 * parse, integrity check, constant-time tenant/api_key authentication
 * (crypto_constant_time.h), one slab allocation + the single payload copy,
 * then posts a zero-copy descriptor to the tenant's registered actor
 * mailbox. Every rejection path is a deterministic, bounded-time return --
 * never a blocking wait, never a retry loop.
 */
ngw_status_t ngw_ingest_frame(ngw_gateway_t *gw, const uint8_t *frame, size_t frame_len);

/* Decodes a mailbox message produced by ngw_ingest_frame() back into its
 * descriptor. Pure struct copy out of msg->payload, O(1). */
ngw_status_t ngw_decode_message(const ngw_message_t *msg, ngw_descriptor_t *out);

/* Returns a payload buffer (ngw_descriptor_t.ptr) to the slab pool. Call
 * exactly once per successfully ingested frame, after the consumer is
 * done reading it, or the slab will exhaust. */
void ngw_release_payload(ngw_gateway_t *gw, uint8_t *ptr);

/* Builds a wire frame (see "Wire format" above), including the integrity
 * hash -- the counterpart to ngw_ingest_frame(), used by callers/tests to
 * construct valid frames. Returns NGW_ERR_PAYLOAD_TOO_LARGE if payload_len
 * exceeds NGW_MAX_PAYLOAD, NGW_ERR_INVALID if out_cap is too small for the
 * resulting frame. */
ngw_status_t ngw_build_frame(uint8_t *out, size_t out_cap, size_t *out_len,
                              ngw_protocol_t protocol, uint16_t tenant_id,
                              const uint8_t api_key[NGW_API_KEY_LEN],
                              const uint8_t *payload, uint16_t payload_len);

void ngw_get_stats(const ngw_gateway_t *gw, ngw_stats_t *out);

#endif /* NET_GATEWAY_H */
