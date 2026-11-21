/*
 * net_io.h
 *
 * Phase 2 -- Native asynchronous, zero-copy network ingestion (Winsock).
 *
 * ============================ Honest scope ================================
 * This module is Winsock-specific by design (WSAStartup/ioctlsocket/
 * closesocket have no portable equivalent) -- there is no POSIX fallback
 * path, unlike net_gateway.c. Building it on a non-Windows target is a
 * compile-time error (see the #error below), not a silent no-op.
 *
 * "Asynchrone" here means what the requirement's own wording describes:
 * non-blocking sockets (ioctlsocket(..., FIONBIO, ...)) polled in a tight
 * reactor loop (net_io_poll_once()), with no blocking syscall and no
 * dedicated I/O thread -- a manual readiness-based reactor. This is NOT
 * Windows' overlapped I/O / IOCP (true OS-driven async completion); that
 * is a materially different API (WSARecv with OVERLAPPED structures,
 * completion ports) which was not requested and is not implemented here.
 * Being precise about which "async" this is matters: overlapped I/O and
 * readiness polling have different failure modes and different callers.
 *
 * ============================ Wire protocol ================================
 * Fixed-size, framed-by-length datagrams over TCP (deliberately, not
 * arbitrary-length or delimiter-based -- consistent with this project's
 * "O(1)/bounded parsing" posture elsewhere): each client connection sends
 * exactly
 *     tenant_id (1 byte) | payload (NET_IO_PAYLOAD_LEN bytes)
 * then the server closes the connection once that frame is fully
 * received. One connection = one frame; this keeps per-connection state
 * (net_io_conn_t) trivial and the accounting in the tests exact.
 *
 * ============================ Zero-copy ingestion ==========================
 * The tenant_id byte is read with a real, tiny (1-byte) recv() -- there is
 * no way to route to the correct mailbox before knowing it, and 1 byte of
 * transport framing metadata is not "the payload". Once routed, the
 * PAYLOAD bytes are recv()'d directly into the reserved ngw_message_t
 * slot's own payload buffer (ngw_mailbox_reserve() / recv() straight into
 * slot->payload / ngw_mailbox_publish(), net_gateway.h) -- no staging
 * buffer of any kind sits between the socket and the mailbox for the
 * payload itself, and TCP's stream nature (a frame may arrive across
 * several recv() calls) is handled by resuming the SAME direct write at
 * an increasing offset into that SAME slot on each poll, never by
 * buffering elsewhere first.
 *
 * ============================ Tenant routing, O(1) =========================
 * g_tenant_actor_map[256] (net_io.c) is indexed directly by the wire
 * tenant_id byte -- one comparison-free array read, exactly like this
 * project's other O(1) routing tables. 0 means "unmapped"; a registered
 * tenant stores actor_id+1, so index 0 is never confused with a valid
 * actor 0. An unmapped tenant_id is rejected immediately: the connection
 * is closesocket()'d (no payload is ever read for it) and
 * net_io_stats_t.rejected_total is incremented -- O(1), no retry.
 *
 * ============================ Determinism ==================================
 * Fixed compile-time capacities (NET_IO_MAX_CLIENTS in-flight connections,
 * one recv() attempt per connection per poll call): zero dynamic
 * allocation, ever. net_io_poll_once() never blocks and does a bounded
 * amount of work per call (one accept() attempt + at most
 * NET_IO_MAX_CLIENTS recv() attempts) regardless of how much data is
 * actually available.
 */

#ifndef NET_IO_H
#define NET_IO_H

#if !defined(_WIN32)
#error "net_io.h is Winsock-specific (WSAStartup/ioctlsocket/closesocket); there is no POSIX path. See the file header."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "net_gateway.h"
#include "wire_proto.h"
#include "rate_limit.h"      /* token_bucket_t: reused verbatim for per-tenant chat rate limiting */
#include "telemetry_fabric.h" /* tf_log_t: reused verbatim for the rate-limit audit trail */

#define NET_IO_MAX_CLIENTS 16u  /* in-flight connections, fixed compile-time capacity */
#define NET_IO_PAYLOAD_LEN NGW_MESSAGE_PAYLOAD_SIZE /* recv() target is the WHOLE mailbox payload */
#define NET_IO_MAX_TENANTS 256u /* tenant_id is one wire byte: the full range */
#define NET_IO_MAX_ACTORS  8u

typedef enum {
    NET_IO_OK = 0,
    NET_IO_ERR_INVALID      = -1,
    NET_IO_ERR_WSA_INIT     = -2,
    NET_IO_ERR_SOCKET       = -3,
    NET_IO_ERR_BIND         = -4,
    NET_IO_ERR_LISTEN       = -5,
    NET_IO_ERR_GETSOCKNAME  = -6
} net_io_status_t;

typedef enum {
    NET_IO_CONN_FREE = 0,
    NET_IO_CONN_AWAITING_TENANT,   /* accepted, tenant_id byte not read yet */
    NET_IO_CONN_RECEIVING_PAYLOAD  /* tenant validated, slot reserved, recv()ing directly into it */
} net_io_conn_state_t;

typedef struct {
    SOCKET               fd;
    net_io_conn_state_t  state;
    uint8_t              tenant_id;
    ngw_mailbox_t        *mailbox; /* target, resolved via g_tenant_actor_map once tenant_id is known */
    ngw_message_t        *slot;    /* reserved mailbox slot: the zero-copy recv() target */
    uint32_t              bytes_received; /* into slot->payload so far, < NET_IO_PAYLOAD_LEN */
} net_io_conn_t;

typedef struct {
    uint64_t frames_ingested_zero_copy; /* fully received and published */
    uint64_t rejected_total;            /* invalid tenant_id: socket cut immediately */
    uint64_t rejected_mailbox_full;     /* valid tenant, target mailbox saturated */
    uint64_t connections_accepted;
    uint64_t connections_closed;
} net_io_stats_t;

typedef struct {
    SOCKET        listen_fd;
    bool          wsa_started;
    net_io_conn_t conns[NET_IO_MAX_CLIENTS];
    ngw_mailbox_t *actors[NET_IO_MAX_ACTORS]; /* caller-owned mailboxes */

    _Atomic uint64_t frames_ingested_zero_copy;
    _Atomic uint64_t rejected_total;
    _Atomic uint64_t rejected_mailbox_full;
    _Atomic uint64_t connections_accepted;
    _Atomic uint64_t connections_closed;
} net_io_server_t;

/*
 * Initializes Winsock (WSAStartup) and a non-blocking TCP listening
 * socket (ioctlsocket(..., FIONBIO, ...)) bound to 127.0.0.1:bind_port.
 * bind_port == 0 asks the OS for any free port -- call net_io_get_port()
 * afterwards to discover it (avoids hardcoding a port that might already
 * be in use, the standard way to make a test suite immune to port
 * collisions). Returns NET_IO_OK on success; on any failure, WSAStartup
 * (if it had succeeded) is rolled back with WSACleanup().
 */
net_io_status_t net_io_init(net_io_server_t *srv, uint16_t bind_port);

/* The actual bound port (host byte order), useful when bind_port was 0.
 * Returns 0 on failure. */
uint16_t net_io_get_port(const net_io_server_t *srv);

/* Registers `mailbox` (caller-owned, already ngw_mailbox_init()'d) as the
 * routing target for actor_id (< NET_IO_MAX_ACTORS). */
net_io_status_t net_io_register_actor(net_io_server_t *srv, uint8_t actor_id, ngw_mailbox_t *mailbox);

/* Maps tenant_id (a single wire byte) to actor_id in g_tenant_actor_map --
 * O(1) direct write, process-wide (see net_io.h/.c "Tenant routing"). */
net_io_status_t net_io_register_tenant(uint8_t tenant_id, uint8_t actor_id);

/*
 * One non-blocking polling pass: at most one accept() attempt, then at
 * most one recv() attempt per active connection. Never blocks, never
 * spawns a thread. Returns the number of frames fully received and
 * zero-copy-published into a mailbox during this call (0 is the normal,
 * expected outcome of most calls -- it just means nothing new was ready).
 */
uint32_t net_io_poll_once(net_io_server_t *srv);

void net_io_get_stats(const net_io_server_t *srv, net_io_stats_t *out);

/* Closes every open socket (listening + in-flight connections) and calls
 * WSACleanup() if net_io_init() had called WSAStartup(). */
void net_io_shutdown(net_io_server_t *srv);

/* ================================================================== */
/* Compact wire-mode ingestion (wire_proto.h)                          */
/*                                                                       */
/* Same reactor (non-blocking, polled, no thread) as above, but ingests */
/* directly into wire_ring_t slots (wire_proto.h): once a connection's  */
/* target ring is known, recv() writes STRAIGHT into the reserved        */
/* wire_msg_t slot's raw memory with NO decode/hydration step -- the    */
/* slot's bytes ARE a valid wire_msg_t the instant all 64 of them have   */
/* arrived, by construction (see wire_proto.h).                         */
/*                                                                       */
/* The one exception, and it is deliberate: the target ring cannot be   */
/* chosen before the tenant_id field (bytes 4-5) is known, so each new  */
/* connection is first peeked (recv(..., MSG_PEEK), non-destructive) for*/
/* magic+tenant_id (6 bytes) -- a routing-metadata read, not a decode of*/
/* the message itself. Those bytes are never removed from the socket's  */
/* receive buffer, so the subsequent real recv() re-reads them as part  */
/* of the same 64-byte frame; nothing is read twice from the wire, and  */
/* nothing is copied twice in memory.                                   */
/* ================================================================== */

#define NET_IO_WIRE_MAX_CLIENTS 16u  /* in-flight connections, fixed compile-time capacity */
#define NET_IO_WIRE_MAX_TENANTS 256u /* wire tenant_id is u16; only this many are routable -- see net_io.c */
#define NET_IO_WIRE_MAX_ACTORS  8u

/* Bytes needed to peek magic (u32) + tenant_id (u16) -- see wire_proto.h's
 * field layout. Kept as its own name so the intent (a bounded routing-
 * metadata peek) is visible independently of wire_msg_t's exact layout. */
#define NET_IO_WIRE_PEEK_LEN (4u + 2u)

/* Niveau 53: strict per-session (= per wire tenant_id) chat throughput
 * cap. Enforced as a plain token_bucket_t (rate_limit.h) per tenant_id --
 * reused verbatim, not reimplemented -- with `capacity` doubling as the
 * per-minute quota. This module owns NO clock/timer (consistent with
 * every other module here -- see chrono_tick.h's own "no RTC dependency"
 * posture): net_io_wire_refill_rate_limits() is the mechanism, and the
 * CALLER drives its cadence, typically once per real minute with
 * amount == NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN for a strict fixed-window
 * reset (the literal, simplest reading of "500 messages/minute" -- no
 * fractional-token smoothing scheme is invented here). */
#define NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN 500

typedef enum {
    NET_IO_WIRE_CONN_FREE = 0,
    NET_IO_WIRE_CONN_AWAITING_HEADER, /* accepted; peeking magic+tenant_id to pick the target ring */
    NET_IO_WIRE_CONN_RECEIVING        /* target ring reserved; recv()ing the full 64 bytes directly into the slot */
} net_io_wire_conn_state_t;

typedef struct {
    SOCKET                    fd;
    net_io_wire_conn_state_t  state;
    wire_ring_t              *ring; /* resolved once the header is peeked */
    wire_msg_t               *slot; /* reserved slot: the zero-copy recv() target */
    uint32_t                  bytes_received; /* into *slot, < sizeof(wire_msg_t) */
} net_io_wire_conn_t;

typedef struct {
    uint64_t frames_ingested_zero_copy;
    uint64_t rejected_total;        /* bad magic or invalid/unmapped tenant_id: socket cut immediately */
    uint64_t rejected_ring_full;    /* valid tenant, target ring saturated */
    uint64_t rejected_rate_limited; /* valid tenant, but its 500/min token bucket was exhausted */
    uint64_t connections_accepted;
    uint64_t connections_closed;
} net_io_wire_stats_t;

typedef struct {
    SOCKET             listen_fd;
    bool               wsa_started;
    net_io_wire_conn_t conns[NET_IO_WIRE_MAX_CLIENTS];
    wire_ring_t       *actors[NET_IO_WIRE_MAX_ACTORS]; /* caller-owned rings */

    /* One lock-free token bucket per routable tenant_id (= session token) --
     * see NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN above. */
    token_bucket_t rate_limiters[NET_IO_WIRE_MAX_TENANTS];

    /* Optional, caller-owned (tf_log_init()'d by the caller, NULL by
     * default): if set via net_io_wire_set_audit_log(), every rate-limit
     * rejection is appended to it. NULL means "rate-limit without an
     * audit trail", never a failure. */
    tf_log_t *audit_log;

    _Atomic uint64_t frames_ingested_zero_copy;
    _Atomic uint64_t rejected_total;
    _Atomic uint64_t rejected_ring_full;
    _Atomic uint64_t rejected_rate_limited;
    _Atomic uint64_t connections_accepted;
    _Atomic uint64_t connections_closed;
} net_io_wire_server_t;

net_io_status_t net_io_wire_init(net_io_wire_server_t *srv, uint16_t bind_port);
uint16_t        net_io_wire_get_port(const net_io_wire_server_t *srv);
net_io_status_t net_io_wire_register_actor(net_io_wire_server_t *srv, uint8_t actor_id, wire_ring_t *ring);

/* Maps tenant_id (from wire_msg_t, u16) to actor_id -- O(1) direct write
 * into its own routing table (net_io.c), separate from
 * g_tenant_actor_map above (that one is keyed by the OLDER 1-byte tenant
 * format's tenant_id, a narrower type). Rejects tenant_id >=
 * NET_IO_WIRE_MAX_TENANTS. */
net_io_status_t net_io_wire_register_tenant(uint16_t tenant_id, uint8_t actor_id);

/* One non-blocking polling pass, same shape and guarantees as
 * net_io_poll_once(). Returns the number of frames fully received and
 * zero-copy-published during this call. */
uint32_t net_io_wire_poll_once(net_io_wire_server_t *srv);

void net_io_wire_get_stats(const net_io_wire_server_t *srv, net_io_wire_stats_t *out);
void net_io_wire_shutdown(net_io_wire_server_t *srv);

/* ---------------------------------------------------------------- */
/* Niveau 53: per-session (tenant_id) chat rate limiting + audit trail */
/* ---------------------------------------------------------------- */

/* Sets (or clears, with NULL) the tf_log_t this server appends a rate-
 * limit-rejection entry to. Not thread-safe against concurrent
 * net_io_wire_admit_tenant_message() calls -- boot-time/setup-time only,
 * same posture as net_io_wire_register_actor(). */
void net_io_wire_set_audit_log(net_io_wire_server_t *srv, tf_log_t *audit_log);

/* O(1), lock-free, any number of concurrent callers: adds `amount` tokens
 * to EVERY tenant's bucket (each capped at NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN).
 * This module owns no timer -- the caller drives cadence; see this
 * header's rate-limit note above for the intended once-per-real-minute
 * usage. amount <= 0 is a no-op. */
void net_io_wire_refill_rate_limits(net_io_wire_server_t *srv, int64_t amount);

/*
 * The exact admission gate net_io_wire_poll_awaiting_header() applies to
 * every fully-routed (valid magic, mapped tenant, live ring) frame BEFORE
 * wire_ring_reserve() is ever called -- exposed publicly so callers (and
 * tests) can exercise the identical logic without a real socket. O(1),
 * lock-free (token_bucket_try_consume(), rate_limit.h).
 *
 * Returns true (consumes one token) if tenant_id's bucket still has
 * budget; false (bucket unchanged) if exhausted -- backpressure, never a
 * wait. On false, atomically increments rejected_rate_limited and, if an
 * audit_log is set, appends one FNV-1a-checksummed tf_log_t entry
 * (tenant_id + the running rejection count) via tf_log_append().
 *
 * tenant_id >= NET_IO_WIRE_MAX_TENANTS (out of the routable range) or a
 * NULL srv returns false without touching any counter or log.
 */
bool net_io_wire_admit_tenant_message(net_io_wire_server_t *srv, uint16_t tenant_id);

#endif /* NET_IO_H */
