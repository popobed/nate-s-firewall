#include "net_io.h"

#include <string.h>

/*
 * O(1) tenant -> actor routing table, direct-indexed by the wire
 * tenant_id byte. 0 = unmapped; a registered tenant stores actor_id + 1
 * so actor 0 is never confused with "unmapped". Process-wide (not part of
 * net_io_server_t) by design, matching the literal requirement -- see
 * net_io.h "Tenant routing, O(1)".
 */
static uint8_t g_tenant_actor_map[NET_IO_MAX_TENANTS];

net_io_status_t net_io_register_tenant(uint8_t tenant_id, uint8_t actor_id) {
    if (actor_id >= NET_IO_MAX_ACTORS) {
        return NET_IO_ERR_INVALID;
    }
    g_tenant_actor_map[tenant_id] = (uint8_t)(actor_id + 1u);
    return NET_IO_OK;
}

/* ================================================================== */
/* Lifecycle                                                            */
/* ================================================================== */

net_io_status_t net_io_init(net_io_server_t *srv, uint16_t bind_port) {
    if (srv == NULL) {
        return NET_IO_ERR_INVALID;
    }
    memset(srv, 0, sizeof *srv);
    srv->listen_fd = INVALID_SOCKET;
    for (uint32_t i = 0; i < NET_IO_MAX_CLIENTS; i++) {
        srv->conns[i].fd = INVALID_SOCKET;
        srv->conns[i].state = NET_IO_CONN_FREE;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return NET_IO_ERR_WSA_INIT;
    }
    srv->wsa_started = true;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listen_fd == INVALID_SOCKET) {
        WSACleanup();
        srv->wsa_started = false;
        return NET_IO_ERR_SOCKET;
    }

    /* Non-blocking listening socket: literal ioctlsocket(...FIONBIO...)
     * per this module's requirement -- no blocking accept()/recv() ever
     * follows from this fd. */
    if (ioctlsocket(srv->listen_fd, FIONBIO, &(u_long){1}) == SOCKET_ERROR) {
        closesocket(srv->listen_fd);
        WSACleanup();
        srv->listen_fd = INVALID_SOCKET;
        srv->wsa_started = false;
        return NET_IO_ERR_SOCKET;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* loopback-only: this is a test/local ingestion server */
    addr.sin_port = htons(bind_port);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        closesocket(srv->listen_fd);
        WSACleanup();
        srv->listen_fd = INVALID_SOCKET;
        srv->wsa_started = false;
        return NET_IO_ERR_BIND;
    }

    if (listen(srv->listen_fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(srv->listen_fd);
        WSACleanup();
        srv->listen_fd = INVALID_SOCKET;
        srv->wsa_started = false;
        return NET_IO_ERR_LISTEN;
    }

    for (uint32_t i = 0; i < NET_IO_MAX_ACTORS; i++) {
        srv->actors[i] = NULL;
    }

    atomic_store_explicit(&srv->frames_ingested_zero_copy, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->rejected_total, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->rejected_mailbox_full, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->connections_accepted, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->connections_closed, 0, memory_order_relaxed);

    return NET_IO_OK;
}

uint16_t net_io_get_port(const net_io_server_t *srv) {
    if (srv == NULL || srv->listen_fd == INVALID_SOCKET) {
        return 0u;
    }
    struct sockaddr_in addr;
    int addr_len = (int)sizeof addr;
    memset(&addr, 0, sizeof addr);
    if (getsockname(srv->listen_fd, (struct sockaddr *)&addr, &addr_len) == SOCKET_ERROR) {
        return 0u;
    }
    return ntohs(addr.sin_port);
}

net_io_status_t net_io_register_actor(net_io_server_t *srv, uint8_t actor_id, ngw_mailbox_t *mailbox) {
    if (srv == NULL || mailbox == NULL || actor_id >= NET_IO_MAX_ACTORS) {
        return NET_IO_ERR_INVALID;
    }
    srv->actors[actor_id] = mailbox;
    return NET_IO_OK;
}

void net_io_shutdown(net_io_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    for (uint32_t i = 0; i < NET_IO_MAX_CLIENTS; i++) {
        if (srv->conns[i].state != NET_IO_CONN_FREE && srv->conns[i].fd != INVALID_SOCKET) {
            /* A reservation left mid-flight at shutdown must still be
             * published (discarded) so the mailbox never stalls -- see
             * the reservation contract in net_gateway.h. */
            if (srv->conns[i].state == NET_IO_CONN_RECEIVING_PAYLOAD) {
                ngw_mailbox_publish(srv->conns[i].mailbox, srv->conns[i].slot,
                                     srv->conns[i].tenant_id, NGW_MSG_TYPE_DISCARD);
            }
            closesocket(srv->conns[i].fd);
        }
        srv->conns[i].fd = INVALID_SOCKET;
        srv->conns[i].state = NET_IO_CONN_FREE;
    }
    if (srv->listen_fd != INVALID_SOCKET) {
        closesocket(srv->listen_fd);
        srv->listen_fd = INVALID_SOCKET;
    }
    if (srv->wsa_started) {
        WSACleanup();
        srv->wsa_started = false;
    }
}

/* ================================================================== */
/* Polling                                                              */
/* ================================================================== */

static int32_t net_io_find_free_conn(const net_io_server_t *srv) {
    for (uint32_t i = 0; i < NET_IO_MAX_CLIENTS; i++) {
        if (srv->conns[i].state == NET_IO_CONN_FREE) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void net_io_close_conn(net_io_server_t *srv, net_io_conn_t *c) {
    closesocket(c->fd);
    c->fd = INVALID_SOCKET;
    c->state = NET_IO_CONN_FREE;
    atomic_fetch_add_explicit(&srv->connections_closed, 1, memory_order_relaxed);
}

static void net_io_poll_accept(net_io_server_t *srv) {
    if (srv->listen_fd == INVALID_SOCKET) {
        return;
    }

    struct sockaddr_in peer;
    int peer_len = (int)sizeof peer;
    SOCKET client = accept(srv->listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (client == INVALID_SOCKET) {
        return; /* WSAEWOULDBLOCK (nothing pending) or a transient accept error: either way, try again next poll */
    }

    if (ioctlsocket(client, FIONBIO, &(u_long){1}) == SOCKET_ERROR) {
        closesocket(client);
        return;
    }

    int32_t idx = net_io_find_free_conn(srv);
    if (idx < 0) {
        closesocket(client); /* no room: deterministic, bounded-capacity drop */
        return;
    }

    net_io_conn_t *c = &srv->conns[(uint32_t)idx];
    c->fd = client;
    c->state = NET_IO_CONN_AWAITING_TENANT;
    c->tenant_id = 0u;
    c->mailbox = NULL;
    c->slot = NULL;
    c->bytes_received = 0u;
    atomic_fetch_add_explicit(&srv->connections_accepted, 1, memory_order_relaxed);
}

/* Returns true if `err` (from WSAGetLastError() after a failed recv())
 * means "nothing available yet, not a real error". */
static bool net_io_is_would_block(int err) {
    return err == WSAEWOULDBLOCK;
}

static void net_io_poll_awaiting_tenant(net_io_server_t *srv, net_io_conn_t *c) {
    uint8_t tenant_byte;
    int n = recv(c->fd, (char *)&tenant_byte, 1, 0);

    if (n == 1) {
        uint8_t mapped = g_tenant_actor_map[tenant_byte]; /* O(1): direct index, one wire byte */
        if (mapped == 0u) {
            /* Invalid Tenant ID: reject immediately, cut the socket,
             * count it. No payload byte is ever read for this connection. */
            net_io_close_conn(srv, c);
            atomic_fetch_add_explicit(&srv->rejected_total, 1, memory_order_relaxed);
            return;
        }

        uint8_t actor_id = (uint8_t)(mapped - 1u);
        ngw_mailbox_t *mb = srv->actors[actor_id];
        if (mb == NULL) {
            net_io_close_conn(srv, c);
            atomic_fetch_add_explicit(&srv->rejected_total, 1, memory_order_relaxed);
            return;
        }

        ngw_message_t *slot;
        if (ngw_mailbox_reserve(mb, &slot) != NGW_OK) {
            net_io_close_conn(srv, c); /* target mailbox saturated: deterministic O(1) drop */
            atomic_fetch_add_explicit(&srv->rejected_mailbox_full, 1, memory_order_relaxed);
            return;
        }

        c->tenant_id = tenant_byte;
        c->mailbox = mb;
        c->slot = slot;
        c->bytes_received = 0u;
        c->state = NET_IO_CONN_RECEIVING_PAYLOAD;
        return;
    }

    if (n == 0) {
        net_io_close_conn(srv, c); /* peer closed before sending the tenant byte */
        return;
    }

    int err = WSAGetLastError();
    if (!net_io_is_would_block(err)) {
        net_io_close_conn(srv, c);
    }
    /* else: tenant byte not available yet -- try again on the next poll */
}

static void net_io_poll_receiving_payload(net_io_server_t *srv, net_io_conn_t *c) {
    uint32_t remaining = NET_IO_PAYLOAD_LEN - c->bytes_received;

    /* Zero-copy: recv() writes DIRECTLY into the reserved mailbox slot's
     * own payload buffer. No staging buffer of any kind exists between
     * the socket and the mailbox. */
    uint8_t *dst = c->slot->payload + c->bytes_received;
    int n = recv(c->fd, (char *)dst, (int)remaining, 0);

    if (n > 0) {
        c->bytes_received += (uint32_t)n;
        if (c->bytes_received == NET_IO_PAYLOAD_LEN) {
            ngw_mailbox_publish(c->mailbox, c->slot, c->tenant_id, 0u);
            atomic_fetch_add_explicit(&srv->frames_ingested_zero_copy, 1, memory_order_relaxed);
            net_io_close_conn(srv, c); /* one frame per connection, by protocol */
        }
        return;
    }

    if (n == 0) {
        /* Peer closed mid-frame: publish the discard sentinel so this
         * reservation never permanently stalls the mailbox (see the
         * reservation contract in net_gateway.h), then drop. */
        ngw_mailbox_publish(c->mailbox, c->slot, c->tenant_id, NGW_MSG_TYPE_DISCARD);
        net_io_close_conn(srv, c);
        return;
    }

    int err = WSAGetLastError();
    if (!net_io_is_would_block(err)) {
        ngw_mailbox_publish(c->mailbox, c->slot, c->tenant_id, NGW_MSG_TYPE_DISCARD);
        net_io_close_conn(srv, c);
    }
    /* else: more payload bytes not available yet -- resume at the same
     * offset into the same slot on the next poll. */
}

uint32_t net_io_poll_once(net_io_server_t *srv) {
    if (srv == NULL) {
        return 0u;
    }

    net_io_poll_accept(srv);

    uint64_t completed_before = atomic_load_explicit(&srv->frames_ingested_zero_copy, memory_order_relaxed);

    for (uint32_t i = 0; i < NET_IO_MAX_CLIENTS; i++) {
        net_io_conn_t *c = &srv->conns[i];
        switch (c->state) {
            case NET_IO_CONN_FREE:
                break;
            case NET_IO_CONN_AWAITING_TENANT:
                net_io_poll_awaiting_tenant(srv, c);
                break;
            case NET_IO_CONN_RECEIVING_PAYLOAD:
                net_io_poll_receiving_payload(srv, c);
                break;
        }
    }

    uint64_t completed_after = atomic_load_explicit(&srv->frames_ingested_zero_copy, memory_order_relaxed);
    return (uint32_t)(completed_after - completed_before);
}

void net_io_get_stats(const net_io_server_t *srv, net_io_stats_t *out) {
    if (srv == NULL || out == NULL) {
        return;
    }
    out->frames_ingested_zero_copy =
        atomic_load_explicit((_Atomic uint64_t *)&srv->frames_ingested_zero_copy, memory_order_relaxed);
    out->rejected_total = atomic_load_explicit((_Atomic uint64_t *)&srv->rejected_total, memory_order_relaxed);
    out->rejected_mailbox_full =
        atomic_load_explicit((_Atomic uint64_t *)&srv->rejected_mailbox_full, memory_order_relaxed);
    out->connections_accepted =
        atomic_load_explicit((_Atomic uint64_t *)&srv->connections_accepted, memory_order_relaxed);
    out->connections_closed =
        atomic_load_explicit((_Atomic uint64_t *)&srv->connections_closed, memory_order_relaxed);
}

/* ================================================================== */
/* Compact wire-mode ingestion (wire_proto.h) -- see net_io.h            */
/* ================================================================== */

/* O(1) tenant -> actor routing table for the compact wire format
 * (wire_msg_t.tenant_id is u16; only NET_IO_WIRE_MAX_TENANTS of that
 * range are provisioned here). Deliberately separate from
 * g_tenant_actor_map above (that one is keyed by the older, narrower
 * 1-byte tenant format and must not be conflated with this one). */
static uint8_t g_wire_tenant_actor_map[NET_IO_WIRE_MAX_TENANTS];

net_io_status_t net_io_wire_register_tenant(uint16_t tenant_id, uint8_t actor_id) {
    if (tenant_id >= NET_IO_WIRE_MAX_TENANTS || actor_id >= NET_IO_WIRE_MAX_ACTORS) {
        return NET_IO_ERR_INVALID;
    }
    g_wire_tenant_actor_map[tenant_id] = (uint8_t)(actor_id + 1u);
    return NET_IO_OK;
}

net_io_status_t net_io_wire_init(net_io_wire_server_t *srv, uint16_t bind_port) {
    if (srv == NULL) {
        return NET_IO_ERR_INVALID;
    }
    memset(srv, 0, sizeof *srv);
    srv->listen_fd = INVALID_SOCKET;
    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_CLIENTS; i++) {
        srv->conns[i].fd = INVALID_SOCKET;
        srv->conns[i].state = NET_IO_WIRE_CONN_FREE;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return NET_IO_ERR_WSA_INIT;
    }
    srv->wsa_started = true;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listen_fd == INVALID_SOCKET) {
        WSACleanup();
        srv->wsa_started = false;
        return NET_IO_ERR_SOCKET;
    }

    if (ioctlsocket(srv->listen_fd, FIONBIO, &(u_long){1}) == SOCKET_ERROR) {
        closesocket(srv->listen_fd);
        WSACleanup();
        srv->listen_fd = INVALID_SOCKET;
        srv->wsa_started = false;
        return NET_IO_ERR_SOCKET;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(bind_port);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
        closesocket(srv->listen_fd);
        WSACleanup();
        srv->listen_fd = INVALID_SOCKET;
        srv->wsa_started = false;
        return NET_IO_ERR_BIND;
    }

    if (listen(srv->listen_fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(srv->listen_fd);
        WSACleanup();
        srv->listen_fd = INVALID_SOCKET;
        srv->wsa_started = false;
        return NET_IO_ERR_LISTEN;
    }

    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_ACTORS; i++) {
        srv->actors[i] = NULL;
    }

    /* Every routable tenant_id starts with a full burst allowance --
     * see NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN's note (net_io.h) on why
     * this module does not tie refilling to a wall clock itself. */
    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_TENANTS; i++) {
        token_bucket_init(&srv->rate_limiters[i], NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN,
                           NET_IO_WIRE_CHAT_RATE_LIMIT_PER_MIN);
    }
    srv->audit_log = NULL;

    atomic_store_explicit(&srv->frames_ingested_zero_copy, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->rejected_total, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->rejected_ring_full, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->rejected_rate_limited, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->connections_accepted, 0, memory_order_relaxed);
    atomic_store_explicit(&srv->connections_closed, 0, memory_order_relaxed);

    return NET_IO_OK;
}

uint16_t net_io_wire_get_port(const net_io_wire_server_t *srv) {
    if (srv == NULL || srv->listen_fd == INVALID_SOCKET) {
        return 0u;
    }
    struct sockaddr_in addr;
    int addr_len = (int)sizeof addr;
    memset(&addr, 0, sizeof addr);
    if (getsockname(srv->listen_fd, (struct sockaddr *)&addr, &addr_len) == SOCKET_ERROR) {
        return 0u;
    }
    return ntohs(addr.sin_port);
}

net_io_status_t net_io_wire_register_actor(net_io_wire_server_t *srv, uint8_t actor_id, wire_ring_t *ring) {
    if (srv == NULL || ring == NULL || actor_id >= NET_IO_WIRE_MAX_ACTORS) {
        return NET_IO_ERR_INVALID;
    }
    srv->actors[actor_id] = ring;
    return NET_IO_OK;
}

void net_io_wire_shutdown(net_io_wire_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_CLIENTS; i++) {
        if (srv->conns[i].state != NET_IO_WIRE_CONN_FREE && srv->conns[i].fd != INVALID_SOCKET) {
            if (srv->conns[i].state == NET_IO_WIRE_CONN_RECEIVING) {
                /* A reservation left mid-flight at shutdown must still be
                 * published (discarded) so the ring never stalls -- same
                 * contract as wire_ring_reserve()'s (see wire_proto.h). */
                wire_ring_publish(srv->conns[i].ring, srv->conns[i].slot, WIRE_MSG_TYPE_DISCARD);
            }
            closesocket(srv->conns[i].fd);
        }
        srv->conns[i].fd = INVALID_SOCKET;
        srv->conns[i].state = NET_IO_WIRE_CONN_FREE;
    }
    if (srv->listen_fd != INVALID_SOCKET) {
        closesocket(srv->listen_fd);
        srv->listen_fd = INVALID_SOCKET;
    }
    if (srv->wsa_started) {
        WSACleanup();
        srv->wsa_started = false;
    }
}

static int32_t net_io_wire_find_free_conn(const net_io_wire_server_t *srv) {
    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_CLIENTS; i++) {
        if (srv->conns[i].state == NET_IO_WIRE_CONN_FREE) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void net_io_wire_close_conn(net_io_wire_server_t *srv, net_io_wire_conn_t *c) {
    closesocket(c->fd);
    c->fd = INVALID_SOCKET;
    c->state = NET_IO_WIRE_CONN_FREE;
    atomic_fetch_add_explicit(&srv->connections_closed, 1, memory_order_relaxed);
}

static void net_io_wire_poll_accept(net_io_wire_server_t *srv) {
    if (srv->listen_fd == INVALID_SOCKET) {
        return;
    }

    struct sockaddr_in peer;
    int peer_len = (int)sizeof peer;
    SOCKET client = accept(srv->listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (client == INVALID_SOCKET) {
        return;
    }

    if (ioctlsocket(client, FIONBIO, &(u_long){1}) == SOCKET_ERROR) {
        closesocket(client);
        return;
    }

    int32_t idx = net_io_wire_find_free_conn(srv);
    if (idx < 0) {
        closesocket(client); /* no room: deterministic, bounded-capacity drop */
        return;
    }

    net_io_wire_conn_t *c = &srv->conns[(uint32_t)idx];
    c->fd = client;
    c->state = NET_IO_WIRE_CONN_AWAITING_HEADER;
    c->ring = NULL;
    c->slot = NULL;
    c->bytes_received = 0u;
    atomic_fetch_add_explicit(&srv->connections_accepted, 1, memory_order_relaxed);
}

static bool net_io_wire_is_would_block(int err) {
    return err == WSAEWOULDBLOCK;
}

/* ==================================================================== */
/* Niveau 53: per-session (tenant_id) chat rate limiting + audit trail   */
/* ==================================================================== */

#define NET_IO_TF_EVENT_CHAT_RATE_LIMITED 0x524C4D31u /* "RLM1" as bytes */

typedef struct {
    uint16_t tenant_id;
    uint64_t rejected_rate_limited_total;
} net_io_tf_rate_limit_payload_t;

void net_io_wire_set_audit_log(net_io_wire_server_t *srv, tf_log_t *audit_log) {
    if (srv == NULL) {
        return;
    }
    srv->audit_log = audit_log;
}

void net_io_wire_refill_rate_limits(net_io_wire_server_t *srv, int64_t amount) {
    if (srv == NULL || amount <= 0) {
        return;
    }
    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_TENANTS; i++) {
        token_bucket_refill(&srv->rate_limiters[i], amount);
    }
}

bool net_io_wire_admit_tenant_message(net_io_wire_server_t *srv, uint16_t tenant_id) {
    if (srv == NULL || tenant_id >= NET_IO_WIRE_MAX_TENANTS) {
        return false;
    }

    if (token_bucket_try_consume(&srv->rate_limiters[tenant_id], 1)) {
        return true;
    }

    uint64_t total = atomic_fetch_add_explicit(&srv->rejected_rate_limited, 1, memory_order_relaxed) + 1u;

    if (srv->audit_log != NULL) {
        net_io_tf_rate_limit_payload_t payload;
        payload.tenant_id = tenant_id;
        payload.rejected_rate_limited_total = total;
        /* Best-effort: a full audit log (TF_ERR_FULL) must never turn a
         * correct rate-limit rejection into a different outcome -- same
         * posture as self_replicate.c's agn_predictive_auto_heal_audited(). */
        (void)tf_log_append(srv->audit_log, NET_IO_TF_EVENT_CHAT_RATE_LIMITED, &payload, (uint32_t)sizeof payload);
    }
    return false;
}

static void net_io_wire_poll_awaiting_header(net_io_wire_server_t *srv, net_io_wire_conn_t *c) {
    uint8_t peek_buf[NET_IO_WIRE_PEEK_LEN];
    int n = recv(c->fd, (char *)peek_buf, (int)sizeof peek_buf, MSG_PEEK);

    if (n == (int)sizeof peek_buf) {
        uint32_t magic;
        uint16_t tenant_id;
        memcpy(&magic, peek_buf, sizeof magic);
        memcpy(&tenant_id, peek_buf + sizeof magic, sizeof tenant_id);

        if (magic != WIRE_MAGIC) {
            net_io_wire_close_conn(srv, c);
            atomic_fetch_add_explicit(&srv->rejected_total, 1, memory_order_relaxed);
            return;
        }

        /* Bound check BEFORE indexing: tenant_id is u16 (up to 65535) but
         * the table only has NET_IO_WIRE_MAX_TENANTS entries -- indexing
         * first would be an out-of-bounds read. */
        if (tenant_id >= NET_IO_WIRE_MAX_TENANTS) {
            net_io_wire_close_conn(srv, c);
            atomic_fetch_add_explicit(&srv->rejected_total, 1, memory_order_relaxed);
            return;
        }
        uint8_t mapped = g_wire_tenant_actor_map[tenant_id]; /* O(1) direct index */
        if (mapped == 0u) {
            net_io_wire_close_conn(srv, c);
            atomic_fetch_add_explicit(&srv->rejected_total, 1, memory_order_relaxed);
            return;
        }

        uint8_t actor_id = (uint8_t)(mapped - 1u);
        wire_ring_t *ring = srv->actors[actor_id];
        if (ring == NULL) {
            net_io_wire_close_conn(srv, c);
            atomic_fetch_add_explicit(&srv->rejected_total, 1, memory_order_relaxed);
            return;
        }

        /* Niveau 53: strict 500/min-per-session backpressure, BEFORE any
         * ring slot is ever reserved -- O(1) instant rejection, the same
         * "reject before touching storage" discipline as the checks
         * above. net_io_wire_admit_tenant_message() also handles the
         * rejected_rate_limited counter and the telemetry_fabric.c audit
         * entry -- not duplicated here. */
        if (!net_io_wire_admit_tenant_message(srv, tenant_id)) {
            net_io_wire_close_conn(srv, c);
            return;
        }

        wire_msg_t *slot;
        if (wire_ring_reserve(ring, &slot) != WIRE_OK) {
            net_io_wire_close_conn(srv, c); /* target ring saturated: deterministic O(1) drop */
            atomic_fetch_add_explicit(&srv->rejected_ring_full, 1, memory_order_relaxed);
            return;
        }

        c->ring = ring;
        c->slot = slot;
        c->bytes_received = 0u;
        c->state = NET_IO_WIRE_CONN_RECEIVING;
        return;
    }

    if (n == 0) {
        net_io_wire_close_conn(srv, c); /* peer closed before sending a full header */
        return;
    }

    if (n > 0) {
        return; /* fewer than NET_IO_WIRE_PEEK_LEN bytes available so far -- try again next poll */
    }

    int err = WSAGetLastError();
    if (!net_io_wire_is_would_block(err)) {
        net_io_wire_close_conn(srv, c);
    }
    /* else: WSAEWOULDBLOCK, nothing available at all yet -- try again next poll */
}

static void net_io_wire_poll_receiving(net_io_wire_server_t *srv, net_io_wire_conn_t *c) {
    uint32_t remaining = (uint32_t)sizeof(wire_msg_t) - c->bytes_received;

    /* Zero-copy: recv() writes DIRECTLY into the reserved slot's raw
     * memory. Once all sizeof(wire_msg_t) bytes have arrived, that memory
     * already IS a valid wire_msg_t -- no decode/hydration step. */
    uint8_t *dst = (uint8_t *)c->slot + c->bytes_received;
    int n = recv(c->fd, (char *)dst, (int)remaining, 0);

    if (n > 0) {
        c->bytes_received += (uint32_t)n;
        if (c->bytes_received == (uint32_t)sizeof(wire_msg_t)) {
            wire_ring_publish(c->ring, c->slot, c->slot->type);
            atomic_fetch_add_explicit(&srv->frames_ingested_zero_copy, 1, memory_order_relaxed);
            net_io_wire_close_conn(srv, c); /* one frame per connection, by protocol */
        }
        return;
    }

    if (n == 0) {
        wire_ring_publish(c->ring, c->slot, WIRE_MSG_TYPE_DISCARD);
        net_io_wire_close_conn(srv, c);
        return;
    }

    int err = WSAGetLastError();
    if (!net_io_wire_is_would_block(err)) {
        wire_ring_publish(c->ring, c->slot, WIRE_MSG_TYPE_DISCARD);
        net_io_wire_close_conn(srv, c);
    }
    /* else: more bytes not available yet -- resume at the same offset
     * into the same slot on the next poll. */
}

uint32_t net_io_wire_poll_once(net_io_wire_server_t *srv) {
    if (srv == NULL) {
        return 0u;
    }

    net_io_wire_poll_accept(srv);

    uint64_t completed_before = atomic_load_explicit(&srv->frames_ingested_zero_copy, memory_order_relaxed);

    for (uint32_t i = 0; i < NET_IO_WIRE_MAX_CLIENTS; i++) {
        net_io_wire_conn_t *c = &srv->conns[i];
        switch (c->state) {
            case NET_IO_WIRE_CONN_FREE:
                break;
            case NET_IO_WIRE_CONN_AWAITING_HEADER:
                net_io_wire_poll_awaiting_header(srv, c);
                break;
            case NET_IO_WIRE_CONN_RECEIVING:
                net_io_wire_poll_receiving(srv, c);
                break;
        }
    }

    uint64_t completed_after = atomic_load_explicit(&srv->frames_ingested_zero_copy, memory_order_relaxed);
    return (uint32_t)(completed_after - completed_before);
}

void net_io_wire_get_stats(const net_io_wire_server_t *srv, net_io_wire_stats_t *out) {
    if (srv == NULL || out == NULL) {
        return;
    }
    out->frames_ingested_zero_copy =
        atomic_load_explicit((_Atomic uint64_t *)&srv->frames_ingested_zero_copy, memory_order_relaxed);
    out->rejected_total = atomic_load_explicit((_Atomic uint64_t *)&srv->rejected_total, memory_order_relaxed);
    out->rejected_ring_full =
        atomic_load_explicit((_Atomic uint64_t *)&srv->rejected_ring_full, memory_order_relaxed);
    out->rejected_rate_limited =
        atomic_load_explicit((_Atomic uint64_t *)&srv->rejected_rate_limited, memory_order_relaxed);
    out->connections_accepted =
        atomic_load_explicit((_Atomic uint64_t *)&srv->connections_accepted, memory_order_relaxed);
    out->connections_closed =
        atomic_load_explicit((_Atomic uint64_t *)&srv->connections_closed, memory_order_relaxed);
}
