/*
 * barry_storage.h
 *
 * Niveaux 11-20 -- unified static, deterministic storage/flow module:
 * an in-memory KV cache, a documentary store, a single-field relation
 * index, an MPMC task queue / message broker, and a per-metric
 * time-series ring -- all backed by memory reserved once at program
 * start, zero dynamic allocation after that.
 *
 * ============================== Honesty notes, up front =====================
 *
 * The request this module answers names ten real, independently enormous
 * pieces of infrastructure software (PostgreSQL/Patroni, FerretDB/CouchDB,
 * Valkey/KeyDB, Kafka/Redpanda, Celery, Meilisearch/OpenSearch,
 * TimescaleDB/InfluxDB, Neo4j/Apache Age, Airbyte, and a generic
 * "sharding/scaling" tier) as inspiration for a single C module compiled
 * into one Windows process. None of those becomes this file -- a
 * replicated Patroni cluster, a Cypher-speaking graph engine, or a
 * connector framework like Airbyte is not something a few hundred lines
 * of freestanding C11 can honestly claim to BE. That posture is not new
 * to this module: infra_mesh.h already refuses the same kind of overclaim
 * for Kubernetes/WireGuard/MinIO/etc., and SESSION_NOTES.md documents
 * "net_core.c... non cree" for the same reason. This file follows the
 * same rule: build the concrete, static, zero-allocation primitive each
 * numbered requirement actually specifies, and say plainly, here, what
 * it is NOT.
 *
 *   - Niveau 11 (PostgreSQL/Patroni -- "source unique de verite,
 *     haute disponibilite") : NOT attempted. A replicated relational
 *     engine with WAL shipping and consensus-based leader election is a
 *     multi-process, multi-machine system; nothing in this module claims
 *     to be it. What overlaps honestly: the KV/document stores below ARE
 *     each process's single, static, deterministic source of truth for
 *     the data they hold.
 *   - Niveau 12 (FerretDB/CouchDB) & Niveau 16 (Meilisearch/OpenSearch) &
 *     Niveau 18 (Neo4j/Apache Age) : `barry_doc_store_t` is a real,
 *     working flat-schema document store (fixed-count key/value field
 *     sets, not arbitrary nested BSON/JSON) with O(1)-amortized
 *     hash-indexed lookup by document key -- the concrete, achievable
 *     meaning of "indexed documentary storage" here. `barry_relation_index_t`
 *     is a real, working SINGLE-HOP inverted index (one field value ->
 *     a bounded list of document keys) -- explicitly NOT a query
 *     language, NOT multi-hop graph traversal, NOT ACID transactions
 *     across documents. This is the same "field index, not a graph
 *     engine" scoping infra_mesh.h already applied to Neo4j.
 *   - Niveau 13 (Valkey/KeyDB) : `barry_kv_store_t` is a real, working
 *     in-memory key/value cache: lock-free open-addressed hash table,
 *     bounded linear probing, O(1)-amortized get/put/delete. No
 *     persistence, no replication, no pub/sub, no TTL/eviction policy --
 *     a cache primitive, not a Redis-protocol-compatible server.
 *   - Niveau 14 (Kafka/Redpanda) & Niveau 15 (Celery / custom Go/Rust
 *     workers) : `barry_task_queue_t` is a real, working bounded MPMC
 *     lock-free ring (the classic Vyukov per-slot-sequence design --
 *     deliberately NOT ring_buffer.h's MPSC ring reused verbatim, for the
 *     same reason wire_proto.h built its own ring instead of reusing
 *     net_gateway's: the concurrency contract is genuinely different
 *     here, multiple CONSUMERS, not one). No topics, no partitions
 *     replicated across brokers, no persisted log, no consumer-group
 *     rebalancing -- one bounded in-memory queue, real backpressure
 *     (instant O(1) rejection when full, never a wait), real atomic
 *     hardware synchronization (`<stdatomic.h>`), nothing pretending to
 *     be a distributed log.
 *   - Niveau 17 (TimescaleDB/InfluxDB) : `barry_ts_store_t` is a real,
 *     working fixed-capacity per-metric ring of (tick, value) points,
 *     lock-free single-writer-per-slot via `fetch_add`, an explicit
 *     per-point `ready` handoff flag (same discipline as
 *     ring_buffer.h's `ready`) so a reader never observes a torn write.
 *     `tick` is an OPAQUE caller-supplied counter (e.g. a cycle count or
 *     `processed_total`), never a wall clock -- the same "no calendar,
 *     caller owns the meaning of the counter" posture swarm_dispatch.h's
 *     `swarm_tick` already documents, for the same reason (no external
 *     time dependency anywhere in this project).
 *   - Niveau 19 (Airbyte) : NOT attempted as a connector framework
 *     (no external system connectors exist in this project to extract
 *     from). What IS real: `barry_ingest_wire_msg()` below is a genuine,
 *     working extract/enqueue step -- it takes an already-received
 *     `wire_msg_t` (wire_proto.h's existing 64-byte struct, reused
 *     verbatim) and enqueues it as one task onto `barry_task_queue_t`,
 *     the concrete "data enters the pipeline" primitive this module can
 *     honestly provide.
 *   - Niveau 20 (automatic sharding/scaling) : real and load-bearing, see
 *     "Sharding" below -- but "cloning a structure" under a strict
 *     zero-malloc-after-init constraint cannot mean allocating a new
 *     memory region at runtime. Every shard this module will ever use is
 *     declared statically, at compile time, as part of the store's own
 *     struct -- "cloning" means ACTIVATING the next pre-reserved,
 *     already-identically-shaped shard once the current write target
 *     crosses its watermark, not `malloc()`ing a new one. This is the
 *     honest, achievable meaning of "clone and fragment" without dynamic
 *     allocation; it is not a live-migrating, arbitrarily-growing cluster.
 *
 * ============================== Sharding, precisely ==========================
 * `barry_kv_store_t` and `barry_doc_store_t` each embed BARRY_*_SHARD_COUNT
 * identically-shaped shards from process start (zero allocation, ever).
 * Only shards[0 .. active_shard_count-1] are live; new keys always target
 * the newest active shard (index active_shard_count-1). When that shard's
 * occupancy crosses BARRY_SHARD_WATERMARK_PCT (80%), the store attempts a
 * one-shot CAS to promote the next pre-reserved shard into service
 * (active_shard_count += 1) -- an automatic, load-triggered fragmentation
 * of where new tenant data lands, exactly the mechanism Niveau 20 asks
 * for, bounded by a small compile-time constant so lookup/insert remain
 * O(1)-amortized (bounded probe length x bounded active shard count --
 * the same "bounded, not the strict textbook definition" precision
 * infra_mesh.h's `lb_pool_dispatch()` already applies to its own scan).
 * Existing keys are NEVER migrated to a newer shard (no live rehashing is
 * attempted): get()/delete() scan active shards newest-to-oldest, so a
 * key keeps working wherever it was originally placed. Growth is
 * one-way: active_shard_count never shrinks back down.
 *
 * There is a SECOND, independent promotion trigger, added after this
 * module's own concurrent stress test caught the gap: open addressing's
 * bounded probe chain (BARRY_KV_MAX_PROBE) is a probabilistic guarantee,
 * not an absolute one. Between a shard crossing the 80% watermark and
 * every producer thread noticing the resulting promotion on its own next
 * call, a burst of concurrent inserts can keep landing on that same
 * shard, pushing its real load factor into the high 80s/90s percent --
 * and at that load factor, a bounded linear-probe chain has a genuine,
 * measured chance of finding every slot it checks already occupied, even
 * though the shard is not literally 100% full. barry_kv_put()/
 * barry_doc_put() therefore treat probe exhaustion the same way they
 * treat the watermark: as a trigger to force the next pre-reserved shard
 * into service immediately and retry there, bounded by
 * BARRY_KV_SHARD_COUNT total attempts (still a small, fixed,
 * O(1)-amortized bound, never an unbounded retry). Only when every
 * pre-reserved shard is exhausted this way does put() return
 * BARRY_ERR_FULL -- a genuine capacity limit, not a probe-length
 * artifact.
 *
 * ============================== Concurrency, precisely ========================
 * Every store here is safe for concurrent callers across threads for
 * DIFFERENT keys/metrics/tasks. A PUT of the SAME key racing another PUT
 * of that SAME key is serialized through that one slot's atomic `state`
 * (a CAS claims it into a transient BARRY_SLOT_UPDATING state before the
 * value bytes are touched, and releases it back to BARRY_SLOT_OCCUPIED
 * afterwards) -- the one place this module uses a transient lock-like
 * state instead of being purely wait-free, because a genuinely lock-free
 * in-place update of a variable-length value without hazard
 * pointers/epoch reclamation is out of scope for this codebase. A
 * concurrent GET that lands on a slot mid-update performs a small,
 * bounded number of retries and otherwise treats it as "not resolved
 * yet" -- a documented, non-blocking race against a concurrent writer,
 * not a correctness violation of the store (the writer has not
 * linearized its update yet either way).
 *
 * Reading a value out of a slot that CAN be updated in place (KV/document
 * stores only -- the task queue and time-series ring never rewrite a
 * published slot's payload) additionally goes through a per-slot seqlock
 * (`version`, incremented odd-then-even around every write): a get() spins
 * a small bounded number of times until it observes a stable, even version
 * before and after copying the value out, so it never returns bytes torn
 * between an old and a new value. This is deliberately a DIFFERENT, extra
 * mechanism from `state`/BARRY_SLOT_UPDATING (which only prevents two
 * WRITERS from touching the same slot at once) -- `state` alone is not
 * enough to stop a READER from racing a writer, which is exactly the gap
 * `version` closes. Without it, a concurrent get()/put() pair on the same
 * key would be a real, unguarded data race (not merely "stale but valid"),
 * which this module treats as a correctness bug to fix, not a claim to
 * hand-wave past.
 *
 * One precise limit on that claim, stated plainly rather than glossed
 * over: like every seqlock (including, e.g., the Linux kernel's
 * seqlock_t, which this design otherwise mirrors), the field bytes
 * copied out between the two version loads are ordinary, non-atomic
 * reads racing an ordinary, non-atomic writer -- formally outside the
 * C11 memory model's guarantees during that window, not merely relaxed-
 * atomic. What the retry loop guarantees is that any inconsistent read
 * IS detected (v1 != v2) and discarded before it is ever returned to a
 * caller; it does not make the underlying byte-level race itself
 * standard-defined. This is the same practical-vs-formal tradeoff every
 * production seqlock accepts (no mainstream compiler tears or reorders
 * a plain load/store across the acquire/release version boundary in
 * practice), not a gap unique to this file.
 */

#ifndef BARRY_STORAGE_H
#define BARRY_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#include "wire_proto.h" /* wire_msg_t: reused verbatim for barry_ingest_wire_msg() */

#define BARRY_CACHE_LINE_SIZE 64u

typedef enum {
    BARRY_OK = 0,
    BARRY_ERR_INVALID            = -1,
    BARRY_ERR_NOT_FOUND          = -2,
    BARRY_ERR_FULL               = -3, /* O(1) instant backpressure rejection, never a wait */
    BARRY_ERR_OUT_OF_RANGE       = -4,
    BARRY_ERR_ALREADY_REGISTERED = -5,
    BARRY_ERR_EMPTY              = -6
} barry_status_t;

/* ==================================================================== */
/* Part A -- KV cache store (Niveau 13: Valkey/KeyDB-style)              */
/* ==================================================================== */

#define BARRY_KV_SHARD_COUNT         4u    /* power of two; every shard exists from process start */
#define BARRY_KV_SHARD_CAPACITY      1024u /* power of two: slots per shard */
#define BARRY_KV_KEY_MAX             32u
#define BARRY_KV_VAL_MAX             64u
#define BARRY_KV_MAX_PROBE           16u   /* bounded open-addressing probe chain length */
#define BARRY_SHARD_WATERMARK_PCT    80u   /* occupancy percent that triggers next-shard activation */

_Static_assert(BARRY_KV_SHARD_COUNT != 0u &&
               (BARRY_KV_SHARD_COUNT & (BARRY_KV_SHARD_COUNT - 1u)) == 0u,
               "BARRY_KV_SHARD_COUNT must be a power of two");
_Static_assert(BARRY_KV_SHARD_CAPACITY != 0u &&
               (BARRY_KV_SHARD_CAPACITY & (BARRY_KV_SHARD_CAPACITY - 1u)) == 0u,
               "BARRY_KV_SHARD_CAPACITY must be a power of two");

/* Slot lifecycle shared by the KV and document stores (identical state
 * machine, deliberately duplicated per store rather than shared through a
 * generic helper -- this project already has a standing precedent for
 * that choice: infra_mesh.c re-implements net_gateway.c's FNV-1a locally
 * "because it is static there", i.e. duplication for module independence
 * over a shared indirection). EMPTY and TOMBSTONE both mean "no live
 * value here" for occupancy purposes, but only EMPTY lets an open-
 * addressing probe chain stop early: a TOMBSTONE must be skipped over
 * (the key it once held may still be found further down the same chain).
 */
typedef enum {
    BARRY_SLOT_EMPTY     = 0,
    BARRY_SLOT_RESERVED  = 1, /* claimed for a fresh insert, fields being written */
    BARRY_SLOT_OCCUPIED  = 2,
    BARRY_SLOT_TOMBSTONE = 3,
    BARRY_SLOT_UPDATING  = 4  /* claimed to overwrite an existing key's value in place */
} barry_slot_state_t;

typedef struct {
    _Atomic uint32_t state;
    _Atomic uint32_t version; /* seqlock: odd while a writer is inside, even when stable -- see file header */
    uint64_t         hash;
    uint32_t         key_len;
    uint32_t         val_len;
    uint8_t          key[BARRY_KV_KEY_MAX];
    uint8_t          val[BARRY_KV_VAL_MAX];
} barry_kv_slot_t;

typedef struct {
    barry_kv_slot_t  slots[BARRY_KV_SHARD_CAPACITY];
    _Atomic uint32_t occupied_count; /* O(1) watermark check, never a scan */
} barry_kv_shard_t;

typedef struct {
    barry_kv_shard_t shards[BARRY_KV_SHARD_COUNT]; /* all pre-reserved; "cloning" == activating the next one, never malloc() -- see file header */
    _Atomic uint32_t active_shard_count;           /* 1..BARRY_KV_SHARD_COUNT; monotonically non-decreasing */
    _Atomic uint64_t shard_promotions_total;        /* diagnostics: how many times auto-sharding actually triggered */
} barry_kv_store_t;

void barry_kv_init(barry_kv_store_t *store);

/* O(1)-amortized (bounded probe x bounded active shard count). Inserts a
 * fresh key into the current write-target shard (the newest active one),
 * or updates an existing key wherever it already lives (see "Sharding,
 * precisely" above -- no migration). May trigger automatic shard
 * promotion as a side effect of a fresh insert. Rejects key_len >
 * BARRY_KV_KEY_MAX or val_len > BARRY_KV_VAL_MAX as BARRY_ERR_INVALID,
 * and a full write-target shard (every slot occupied across the whole
 * bounded probe chain, and no promotion possible) as BARRY_ERR_FULL. */
barry_status_t barry_kv_put(barry_kv_store_t *store, const void *key, uint32_t key_len,
                             const void *val, uint32_t val_len);

/* *inout_val_len is the caller's out_val capacity on entry, the actual
 * value length on BARRY_OK. BARRY_ERR_NOT_FOUND if the key was never
 * inserted (or was deleted). */
barry_status_t barry_kv_get(barry_kv_store_t *store, const void *key, uint32_t key_len,
                             void *out_val, uint32_t *inout_val_len);

barry_status_t barry_kv_delete(barry_kv_store_t *store, const void *key, uint32_t key_len);

uint32_t barry_kv_active_shard_count(const barry_kv_store_t *store);
uint64_t barry_kv_shard_promotions_total(const barry_kv_store_t *store);

/* ==================================================================== */
/* Part B -- Documentary store (Niveau 12: FerretDB/CouchDB-style) +     */
/*           single-field relation index (Niveau 18, honest scope)       */
/* ==================================================================== */

#define BARRY_DOC_SHARD_COUNT     BARRY_KV_SHARD_COUNT
#define BARRY_DOC_SHARD_CAPACITY  256u  /* power of two: documents per shard */
#define BARRY_DOC_KEY_MAX         BARRY_KV_KEY_MAX
#define BARRY_DOC_MAX_PROBE       BARRY_KV_MAX_PROBE
#define BARRY_DOC_MAX_FIELDS      8u
#define BARRY_DOC_FIELD_KEY_MAX   16u
#define BARRY_DOC_FIELD_VAL_MAX   32u

_Static_assert(BARRY_DOC_SHARD_CAPACITY != 0u &&
               (BARRY_DOC_SHARD_CAPACITY & (BARRY_DOC_SHARD_CAPACITY - 1u)) == 0u,
               "BARRY_DOC_SHARD_CAPACITY must be a power of two");

typedef struct {
    char key[BARRY_DOC_FIELD_KEY_MAX];
    char val[BARRY_DOC_FIELD_VAL_MAX];
} barry_doc_field_t;

typedef struct {
    uint32_t           field_count; /* <= BARRY_DOC_MAX_FIELDS */
    barry_doc_field_t  fields[BARRY_DOC_MAX_FIELDS];
} barry_document_t;

typedef struct {
    _Atomic uint32_t  state;   /* barry_slot_state_t */
    _Atomic uint32_t  version; /* seqlock, same discipline as barry_kv_slot_t.version */
    uint64_t          hash;
    uint32_t          key_len;
    uint8_t           key[BARRY_DOC_KEY_MAX];
    barry_document_t  doc;
} barry_doc_slot_t;

typedef struct {
    barry_doc_slot_t  slots[BARRY_DOC_SHARD_CAPACITY];
    _Atomic uint32_t  occupied_count;
} barry_doc_shard_t;

typedef struct {
    barry_doc_shard_t shards[BARRY_DOC_SHARD_COUNT];
    _Atomic uint32_t  active_shard_count;
    _Atomic uint64_t  shard_promotions_total;
} barry_doc_store_t;

void barry_doc_init(barry_doc_store_t *store);

/* Same O(1)-amortized/sharding contract as barry_kv_put(). Rejects
 * key_len > BARRY_DOC_KEY_MAX or doc->field_count > BARRY_DOC_MAX_FIELDS
 * as BARRY_ERR_INVALID. */
barry_status_t barry_doc_put(barry_doc_store_t *store, const void *key, uint32_t key_len,
                              const barry_document_t *doc);
barry_status_t barry_doc_get(barry_doc_store_t *store, const void *key, uint32_t key_len,
                              barry_document_t *out_doc);
barry_status_t barry_doc_delete(barry_doc_store_t *store, const void *key, uint32_t key_len);

uint32_t barry_doc_active_shard_count(const barry_doc_store_t *store);

/* Single-hop inverted index: field VALUE -> bounded list of document
 * keys carrying it. Deliberately NOT a graph traversal engine (no
 * multi-hop query, no Cypher) -- the same "field index, not Neo4j"
 * scoping already applied above. */
#define BARRY_INDEX_BUCKETS      256u /* power of two */
#define BARRY_INDEX_BUCKET_DEPTH 8u   /* bounded chain per bucket */

_Static_assert(BARRY_INDEX_BUCKETS != 0u &&
               (BARRY_INDEX_BUCKETS & (BARRY_INDEX_BUCKETS - 1u)) == 0u,
               "BARRY_INDEX_BUCKETS must be a power of two");

typedef struct {
    uint32_t doc_key_len;
    uint8_t  doc_key[BARRY_DOC_KEY_MAX];
} barry_index_entry_t;

typedef struct {
    _Atomic uint32_t     count; /* <= BARRY_INDEX_BUCKET_DEPTH; also the next free slot */
    barry_index_entry_t  entries[BARRY_INDEX_BUCKET_DEPTH];
} barry_index_bucket_t;

typedef struct {
    barry_index_bucket_t buckets[BARRY_INDEX_BUCKETS];
} barry_relation_index_t;

void barry_index_init(barry_relation_index_t *idx);

/* O(1): hashes field_val into one bucket and atomically claims the next
 * free entry in it (fetch_add on the bucket's own counter -- each
 * concurrent caller gets a distinct slot, no CAS retry loop needed).
 * BARRY_ERR_FULL if that bucket's bounded depth is already exhausted
 * (an O(1)-bounded rejection, documented, never a resize). */
barry_status_t barry_index_link(barry_relation_index_t *idx,
                                 const void *field_val, uint32_t field_val_len,
                                 const void *doc_key, uint32_t doc_key_len);

/* O(1): copies up to out_max document keys linked to field_val into
 * out_doc_keys[i] (each BARRY_DOC_KEY_MAX bytes) / out_doc_key_lens[i].
 * *out_count is how many were actually copied. */
barry_status_t barry_index_lookup(const barry_relation_index_t *idx,
                                   const void *field_val, uint32_t field_val_len,
                                   uint8_t out_doc_keys[][BARRY_DOC_KEY_MAX],
                                   uint32_t *out_doc_key_lens, uint32_t out_max,
                                   uint32_t *out_count);

/* ==================================================================== */
/* Part C -- MPMC task queue / message broker (Niveau 14 & 15)           */
/* ==================================================================== */

#define BARRY_TASK_QUEUE_CAPACITY 256u /* power of two */
#define BARRY_TASK_PAYLOAD_MAX    64u  /* >= sizeof(wire_msg_t), so barry_ingest_wire_msg() below never truncates */

_Static_assert(BARRY_TASK_QUEUE_CAPACITY != 0u &&
               (BARRY_TASK_QUEUE_CAPACITY & (BARRY_TASK_QUEUE_CAPACITY - 1u)) == 0u,
               "BARRY_TASK_QUEUE_CAPACITY must be a power of two");

/* Classic Vyukov bounded MPMC ring: each slot carries its own sequence
 * counter instead of a global head/tail-derived "ready" flag, which is
 * what makes MULTIPLE consumers safe here (ring_buffer.h's MPSC ring is
 * single-consumer only by design -- reused as-is everywhere else in this
 * project, but genuinely the wrong primitive for this requirement, the
 * same reasoning wire_proto.h already applied when it built its own ring
 * instead of reusing net_gateway.c's).
 *
 * _Alignas(BARRY_CACHE_LINE_SIZE) here, added during a later hardening
 * pass: verified (not assumed) that WITHOUT it, this struct's natural
 * size is 84 bytes (8 + 4 + 4 + 4 + 64), not a multiple of 64 -- meaning
 * consecutive slots[] entries did NOT each start on their own cache
 * line, so two threads touching ADJACENT slot indices concurrently
 * (a real, common access pattern for a bounded MPMC ring under load)
 * could false-share. _Alignas(64) on the struct itself makes the
 * compiler round sizeof up to the next multiple of 64 (128 bytes here,
 * 44 of it trailing padding) -- verified below via _Static_assert rather
 * than left as an unchecked assumption, same discipline as ring_buffer.h's
 * own sizeof/alignof asserts on ring_msg_t. The memory cost (256 slots x
 * 128B = 32KB vs. 21KB before) is the deliberate, disclosed trade for
 * eliminating that false-sharing window -- not a free change, and not
 * hidden as one. */
typedef struct {
    _Alignas(BARRY_CACHE_LINE_SIZE)
    _Atomic uint64_t sequence;
    uint32_t         producer_id;
    uint32_t         type;
    uint32_t         payload_len;
    uint8_t          payload[BARRY_TASK_PAYLOAD_MAX];
} barry_task_slot_t;

_Static_assert(sizeof(barry_task_slot_t) % BARRY_CACHE_LINE_SIZE == 0u,
               "barry_task_slot_t must be a whole multiple of the cache line size -- see comment above");
_Static_assert(_Alignof(barry_task_slot_t) == BARRY_CACHE_LINE_SIZE,
               "barry_task_slot_t must itself be 64-byte aligned so slots[] stays 64-byte aligned per entry");

/* Plain (non-atomic) copy of one task, handed to callers by
 * barry_task_dequeue()/the worker handler -- deliberately a distinct type
 * from barry_task_slot_t (which owns the ring's internal handoff
 * `sequence` counter, an implementation detail no caller needs or should
 * touch): a caller-owned barry_task_t is safe to keep on the stack,
 * memset(), or memcmp() as an ordinary object. */
typedef struct {
    uint32_t producer_id;
    uint32_t type;
    uint32_t payload_len;
    uint8_t  payload[BARRY_TASK_PAYLOAD_MAX];
} barry_task_t;

typedef struct {
    _Alignas(BARRY_CACHE_LINE_SIZE) _Atomic uint64_t enqueue_pos;
    uint8_t _pad_enq[BARRY_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    _Alignas(BARRY_CACHE_LINE_SIZE) _Atomic uint64_t dequeue_pos;
    uint8_t _pad_deq[BARRY_CACHE_LINE_SIZE - sizeof(_Atomic uint64_t)];

    barry_task_slot_t slots[BARRY_TASK_QUEUE_CAPACITY];

    _Atomic uint64_t posted_total;
    _Atomic uint64_t consumed_total;
    _Atomic uint64_t rejected_full_total; /* backpressure counter */
} barry_task_queue_t;

void barry_task_queue_init(barry_task_queue_t *q);

/* Any number of concurrent producers. O(1): never blocks, never spins
 * unboundedly -- returns BARRY_ERR_FULL immediately (strict backpressure)
 * instead of waiting for room. */
barry_status_t barry_task_enqueue(barry_task_queue_t *q, uint32_t producer_id, uint32_t type,
                                   const void *payload, uint32_t payload_len);

/* Any number of concurrent consumers ("workers"). BARRY_ERR_EMPTY is not
 * a failure, just "nothing ready yet". *out is fully, deterministically
 * written on BARRY_OK (including zero-padding any unused payload tail),
 * left untouched on any non-OK return. */
barry_status_t barry_task_dequeue(barry_task_queue_t *q, barry_task_t *out);

/* O(1) diagnostics; may be stale by the time the caller observes it, as
 * with any live concurrent queue (same caveat as ring_buffer.h's
 * rb_count()). */
size_t barry_task_queue_count(const barry_task_queue_t *q);

/* Worker-side convenience: dequeues at most one task and, if one was
 * available, invokes handler(ctx, task) before returning BARRY_OK.
 * BARRY_ERR_EMPTY if the queue had nothing -- a worker loop calls this
 * repeatedly, never blocking, exactly the "asynchronous background
 * worker regulated by backpressure" Niveau 15 asks for. */
typedef void (*barry_task_handler_fn)(void *ctx, const barry_task_t *task);

barry_status_t barry_worker_process_one(barry_task_queue_t *q, barry_task_handler_fn handler, void *ctx);

/* Extract/ingest step (honest-scope Niveau 19 stand-in, see file header):
 * enqueues an already-received wire_msg_t (wire_proto.h, reused verbatim)
 * as one task, type == BARRY_TASK_TYPE_WIRE_MSG. */
#define BARRY_TASK_TYPE_WIRE_MSG 0xB17E0001u

barry_status_t barry_ingest_wire_msg(barry_task_queue_t *q, uint32_t producer_id,
                                      const wire_msg_t *msg);

/* ==================================================================== */
/* Part D -- Time-series metrics (Niveau 17)                             */
/* ==================================================================== */

#define BARRY_TS_MAX_METRICS 16u
#define BARRY_TS_CAPACITY    1024u /* power of two: points retained per metric */

_Static_assert(BARRY_TS_CAPACITY != 0u &&
               (BARRY_TS_CAPACITY & (BARRY_TS_CAPACITY - 1u)) == 0u,
               "BARRY_TS_CAPACITY must be a power of two");

typedef struct {
    _Atomic uint32_t ready; /* same producer/consumer handoff discipline as ring_buffer.h's ring_msg_t.ready */
    uint64_t         tick;  /* OPAQUE caller-supplied counter -- never a wall clock, see file header */
    double           value;
} barry_ts_point_t;

typedef struct {
    _Alignas(BARRY_CACHE_LINE_SIZE) _Atomic uint64_t write_pos;
    barry_ts_point_t points[BARRY_TS_CAPACITY];
} barry_ts_series_t;

typedef struct {
    barry_ts_series_t series[BARRY_TS_MAX_METRICS];
    bool              registered[BARRY_TS_MAX_METRICS]; /* boot-time-only, same posture as mesh_registry_t/router tables */
} barry_ts_store_t;

void barry_ts_init(barry_ts_store_t *store);

/* Boot-time-only, like mesh_register_node()/router_register_tenant():
 * not thread-safe against concurrent registration or record()/query(). */
barry_status_t barry_ts_register_metric(barry_ts_store_t *store, uint32_t metric_id);

/* O(1): any number of concurrent producers (each gets a distinct slot via
 * fetch_add), a fixed-capacity ring per metric -- the oldest point is
 * silently overwritten once the ring wraps, the standard, honest meaning
 * of a bounded retention window (real TimescaleDB/InfluxDB deployments
 * apply the same kind of retention policy, just configurable and
 * disk-backed instead of fixed and in RAM). */
barry_status_t barry_ts_record(barry_ts_store_t *store, uint32_t metric_id, uint64_t tick, double value);

/* Copies up to out_max of the most recently written, fully-published
 * points (oldest of the returned window first) into out_points.
 * *out_count is how many were actually copied -- may be less than
 * out_max if fewer than out_max points have ever been recorded. */
barry_status_t barry_ts_query_latest(const barry_ts_store_t *store, uint32_t metric_id,
                                      barry_ts_point_t *out_points, uint32_t out_max,
                                      uint32_t *out_count);

#endif /* BARRY_STORAGE_H */
