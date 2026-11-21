/*
 * test_swarm_integration.c - Phase 4 : swarm d'agents metiers x gateway
 * reseau. Couvre le pipeline complet lead_parser -> agent_state (FSM) ->
 * script_engine, l'isolation multi-tenant O(1), et l'interconnexion
 * zero-copy avec net_gateway.h (ngw_ingest_frame -> mailbox ->
 * swarm_poll_gateway_mailbox).
 *
 * No dynamic allocation anywhere in this file: all storage is static or on
 * the stack, matching swarm_dispatch's own zero-allocation constraint.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "swarm_dispatch.h"

/* ---------------------------------------------------------------- */
/* Minimal test harness (same shape as this project's other test files) */
/* ---------------------------------------------------------------- */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_tests_run++;                                                    \
        if (!(cond)) {                                                     \
            g_tests_failed++;                                              \
            fprintf(stderr, "  [FAIL] %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                   \
    } while (0)

#define SECTION(name) printf("-- %s\n", (name))

/* ---------------------------------------------------------------- */
/* Helpers: build a raw swarm lead payload (offsets, see swarm_dispatch.h) */
/* ---------------------------------------------------------------- */

static size_t build_lead_payload(uint8_t *out, uint32_t lead_id, uint8_t intent_code,
                                  uint8_t priority, const char *contact) {
    const size_t contact_len = contact != NULL ? strlen(contact) : 0u;
    memcpy(out, &lead_id, sizeof lead_id);
    out[4] = intent_code;
    out[5] = priority;
    out[6] = (uint8_t)contact_len;
    if (contact_len != 0u) {
        memcpy(out + SWARM_LEAD_HEADER_SIZE, contact, contact_len);
    }
    return SWARM_LEAD_HEADER_SIZE + contact_len;
}

/* ---------------------------------------------------------------- */
/* lead_parser                                                        */
/* ---------------------------------------------------------------- */

static void test_lead_parse_roundtrip(void) {
    SECTION("swarm_lead_parse: fixed-layout round trip");

    uint8_t buf[SWARM_LEAD_MAX_FRAME];
    size_t len = build_lead_payload(buf, 42u, 1u, 7u, "lead@example.com");

    swarm_lead_t lead;
    CHECK(swarm_lead_parse(buf, len, &lead) == SWARM_OK);
    CHECK(lead.lead_id == 42u);
    CHECK(lead.intent_code == 1u);
    CHECK(lead.priority == 7u);
    CHECK(lead.contact_len == strlen("lead@example.com"));
    CHECK(memcmp(lead.contact, "lead@example.com", lead.contact_len) == 0);
}

static void test_lead_parse_rejections(void) {
    SECTION("swarm_lead_parse: bounded rejection paths");

    swarm_lead_t lead;
    uint8_t short_buf[SWARM_LEAD_HEADER_SIZE - 1];
    memset(short_buf, 0, sizeof short_buf);
    CHECK(swarm_lead_parse(short_buf, sizeof short_buf, &lead) == SWARM_ERR_TRUNCATED);

    uint8_t buf[SWARM_LEAD_MAX_FRAME];
    size_t len = build_lead_payload(buf, 1u, 0u, 0u, NULL);
    buf[6] = (uint8_t)(SWARM_LEAD_CONTACT_MAX + 1u); /* claims more contact bytes than the type allows */
    CHECK(swarm_lead_parse(buf, len, &lead) == SWARM_ERR_CONTACT_TOO_LARGE);

    len = build_lead_payload(buf, 1u, 0u, 0u, "abc");
    CHECK(swarm_lead_parse(buf, len - 1u, &lead) == SWARM_ERR_TRUNCATED); /* declared contact_len exceeds actual len */

    CHECK(swarm_lead_parse(NULL, 0u, &lead) == SWARM_ERR_INVALID);
    CHECK(swarm_lead_parse(buf, len, NULL) == SWARM_ERR_INVALID);
}

static void test_intent_mapping(void) {
    SECTION("swarm_intent_from_code: deterministic, bounded mapping");

    CHECK(swarm_intent_from_code(0u) == SWARM_INTENT_NONE);
    CHECK(swarm_intent_from_code(1u) == SWARM_INTENT_INTEREST);
    CHECK(swarm_intent_from_code(2u) == SWARM_INTENT_REJECT);
    CHECK(swarm_intent_from_code(3u) == SWARM_INTENT_CONVERT);
    CHECK(swarm_intent_from_code(255u) == SWARM_INTENT_NONE); /* unknown code: no signal, never an error */
}

/* ---------------------------------------------------------------- */
/* agent_state : FSM transition table                                 */
/* ---------------------------------------------------------------- */

static void test_fsm_transitions(void) {
    SECTION("swarm_fsm_next: fixed transition table, absorbing states");

    CHECK(swarm_fsm_next(SWARM_STATE_NEW, SWARM_INTENT_NONE) == SWARM_STATE_NEW);
    CHECK(swarm_fsm_next(SWARM_STATE_NEW, SWARM_INTENT_INTEREST) == SWARM_STATE_CONTACTED);
    CHECK(swarm_fsm_next(SWARM_STATE_NEW, SWARM_INTENT_REJECT) == SWARM_STATE_LOST);
    CHECK(swarm_fsm_next(SWARM_STATE_NEW, SWARM_INTENT_CONVERT) == SWARM_STATE_CONVERTED);

    CHECK(swarm_fsm_next(SWARM_STATE_CONTACTED, SWARM_INTENT_INTEREST) == SWARM_STATE_QUALIFIED);
    CHECK(swarm_fsm_next(SWARM_STATE_QUALIFIED, SWARM_INTENT_INTEREST) == SWARM_STATE_QUALIFIED);

    /* Absorbing states: no signal ever moves a converted or lost lead. */
    CHECK(swarm_fsm_next(SWARM_STATE_CONVERTED, SWARM_INTENT_REJECT) == SWARM_STATE_CONVERTED);
    CHECK(swarm_fsm_next(SWARM_STATE_LOST, SWARM_INTENT_CONVERT) == SWARM_STATE_LOST);
}

/* ---------------------------------------------------------------- */
/* script_engine + swarm_ingest : full FSM-driven dispatch             */
/* ---------------------------------------------------------------- */

static int           g_script_calls[SWARM_STATE_COUNT];
static agent_state_t g_last_old_state;
static agent_state_t g_last_new_state;
static uint16_t      g_last_tenant_id;
static uint32_t      g_last_lead_id_seen;

static void reset_script_spies(void) {
    memset(g_script_calls, 0, sizeof g_script_calls);
    g_last_old_state    = SWARM_STATE_NEW;
    g_last_new_state    = SWARM_STATE_NEW;
    g_last_tenant_id    = 0u;
    g_last_lead_id_seen = 0u;
}

static void spy_script(uint16_t tenant_id, agent_state_t old_state, agent_state_t new_state,
                        const swarm_lead_t *lead) {
    g_script_calls[new_state]++;
    g_last_old_state    = old_state;
    g_last_new_state    = new_state;
    g_last_tenant_id    = tenant_id;
    g_last_lead_id_seen = lead != NULL ? lead->lead_id : 0u;
}

static swarm_system_t g_sys;

static void test_ingest_drives_fsm_and_script(void) {
    SECTION("swarm_ingest: lead_parser -> agent_state -> script_engine, per-state dispatch");

    swarm_system_init(&g_sys);
    swarm_script_engine_init();
    reset_script_spies();
    for (unsigned s = 0; s < SWARM_STATE_COUNT; ++s) {
        CHECK(swarm_script_register((agent_state_t)s, spy_script) == SWARM_OK);
    }

    const uint16_t tenant = 5u;
    uint8_t buf[SWARM_LEAD_MAX_FRAME];
    size_t len;

    /* NEW --INTEREST--> CONTACTED : the script registered for CONTACTED fires. */
    len = build_lead_payload(buf, 100u, 1u, 0u, "a@b.com");
    CHECK(swarm_ingest(&g_sys, tenant, buf, len) == SWARM_OK);
    CHECK(g_script_calls[SWARM_STATE_CONTACTED] == 1);
    CHECK(g_last_old_state == SWARM_STATE_NEW);
    CHECK(g_last_new_state == SWARM_STATE_CONTACTED);
    CHECK(g_last_tenant_id == tenant);
    CHECK(g_last_lead_id_seen == 100u);

    const swarm_agent_t *agent = swarm_get_agent(&g_sys, tenant);
    CHECK(agent != NULL);
    CHECK(agent->state == SWARM_STATE_CONTACTED);
    CHECK(agent->transitions_total == 1u);
    CHECK(agent->scripts_invoked_total == 1u);
    CHECK(agent->last_lead_id == 100u);

    /* CONTACTED --INTEREST--> QUALIFIED : the script registered for QUALIFIED fires. */
    len = build_lead_payload(buf, 101u, 1u, 0u, NULL);
    CHECK(swarm_ingest(&g_sys, tenant, buf, len) == SWARM_OK);
    CHECK(g_script_calls[SWARM_STATE_QUALIFIED] == 1);
    CHECK(agent->state == SWARM_STATE_QUALIFIED);
    CHECK(agent->transitions_total == 2u);

    /* QUALIFIED --CONVERT--> CONVERTED : the script registered for CONVERTED fires. */
    len = build_lead_payload(buf, 102u, 3u, 0u, NULL);
    CHECK(swarm_ingest(&g_sys, tenant, buf, len) == SWARM_OK);
    CHECK(g_script_calls[SWARM_STATE_CONVERTED] == 1);
    CHECK(agent->state == SWARM_STATE_CONVERTED);
    CHECK(agent->transitions_total == 3u);

    /* CONVERTED is absorbing: state does not change, but the script for the
     * (unchanged) resulting state still fires -- every ingest dispatches. */
    len = build_lead_payload(buf, 103u, 2u, 0u, NULL); /* REJECT is ignored once converted */
    CHECK(swarm_ingest(&g_sys, tenant, buf, len) == SWARM_OK);
    CHECK(agent->state == SWARM_STATE_CONVERTED);
    CHECK(agent->transitions_total == 3u); /* unchanged: no real transition happened */
    CHECK(agent->scripts_invoked_total == 4u); /* dispatch still happened */
    CHECK(g_script_calls[SWARM_STATE_CONVERTED] == 2);
}

static void test_default_script_is_noop_until_registered(void) {
    SECTION("swarm_script_engine_init: default handler is a documented no-op");

    swarm_system_init(&g_sys);
    swarm_script_engine_init(); /* no custom handlers registered */

    uint8_t buf[SWARM_LEAD_MAX_FRAME];
    size_t len = build_lead_payload(buf, 1u, 1u, 0u, NULL);
    CHECK(swarm_ingest(&g_sys, 9u, buf, len) == SWARM_OK); /* must not crash / must succeed silently */

    const swarm_agent_t *agent = swarm_get_agent(&g_sys, 9u);
    CHECK(agent != NULL);
    CHECK(agent->state == SWARM_STATE_CONTACTED);
    CHECK(agent->scripts_invoked_total == 1u); /* dispatch still counted, even with the no-op */
}

static void test_script_register_invalid_state(void) {
    SECTION("swarm_script_register: rejects out-of-range state");
    CHECK(swarm_script_register((agent_state_t)SWARM_STATE_COUNT, spy_script) == SWARM_ERR_INVALID);
}

/* ---------------------------------------------------------------- */
/* Multi-tenant O(1) isolation                                        */
/* ---------------------------------------------------------------- */

static void test_multi_tenant_isolation(void) {
    SECTION("swarm_ingest: per-tenant isolation, O(1) direct index");

    swarm_system_init(&g_sys);
    swarm_script_engine_init();

    uint8_t buf[SWARM_LEAD_MAX_FRAME];
    size_t len;

    len = build_lead_payload(buf, 1u, 1u, 0u, NULL); /* -> CONTACTED */
    CHECK(swarm_ingest(&g_sys, 10u, buf, len) == SWARM_OK);

    len = build_lead_payload(buf, 2u, 3u, 0u, NULL); /* -> CONVERTED */
    CHECK(swarm_ingest(&g_sys, 20u, buf, len) == SWARM_OK);

    len = build_lead_payload(buf, 3u, 2u, 0u, NULL); /* -> LOST */
    CHECK(swarm_ingest(&g_sys, 30u, buf, len) == SWARM_OK);

    const swarm_agent_t *a10 = swarm_get_agent(&g_sys, 10u);
    const swarm_agent_t *a20 = swarm_get_agent(&g_sys, 20u);
    const swarm_agent_t *a30 = swarm_get_agent(&g_sys, 30u);
    CHECK(a10 != NULL && a10->state == SWARM_STATE_CONTACTED);
    CHECK(a20 != NULL && a20->state == SWARM_STATE_CONVERTED);
    CHECK(a30 != NULL && a30->state == SWARM_STATE_LOST);

    /* An untouched tenant's slot never observed any of the above. */
    const swarm_agent_t *a99 = swarm_get_agent(&g_sys, 99u);
    CHECK(a99 != NULL);
    CHECK(a99->active == false);
    CHECK(a99->state == SWARM_STATE_NEW);
    CHECK(a99->transitions_total == 0u);
}

static void test_unknown_tenant_rejected(void) {
    SECTION("swarm_ingest / swarm_get_agent: out-of-range tenant_id rejected in O(1)");

    swarm_system_init(&g_sys);
    uint8_t buf[SWARM_LEAD_MAX_FRAME];
    size_t len = build_lead_payload(buf, 1u, 1u, 0u, NULL);

    CHECK(swarm_ingest(&g_sys, (uint16_t)SWARM_MAX_TENANTS, buf, len) == SWARM_ERR_UNKNOWN_TENANT);
    CHECK(swarm_get_agent(&g_sys, (uint16_t)SWARM_MAX_TENANTS) == NULL);
}

/* ---------------------------------------------------------------- */
/* Zero-copy interconnection: net_gateway.h -> swarm_dispatch.h        */
/* ---------------------------------------------------------------- */

static const uint8_t KEY_A[NGW_API_KEY_LEN] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF
};

static ngw_gateway_t g_gw;
static ngw_mailbox_t g_mb;

static void test_gateway_to_swarm_zero_copy_pipeline(void) {
    SECTION("swarm_poll_gateway_mailbox: net_gateway ingest -> swarm FSM+script, end to end");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 7u, KEY_A, 0u) == NGW_OK);

    swarm_system_init(&g_sys);
    swarm_script_engine_init();
    reset_script_spies();
    CHECK(swarm_script_register(SWARM_STATE_CONTACTED, spy_script) == SWARM_OK);

    uint8_t lead_payload[SWARM_LEAD_MAX_FRAME];
    size_t lead_len = build_lead_payload(lead_payload, 555u, 1u, 3u, "lead@wire.test");

    uint8_t frame[NGW_MAX_FRAME_SIZE];
    size_t frame_len = 0;
    CHECK(ngw_build_frame(frame, sizeof frame, &frame_len, NGW_PROTO_WEBHOOK, 7u, KEY_A,
                           lead_payload, (uint16_t)lead_len) == NGW_OK);

    /* Nothing to poll yet. */
    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, &g_mb) == SWARM_ERR_EMPTY);

    CHECK(ngw_ingest_frame(&g_gw, frame, frame_len) == NGW_OK);
    CHECK(ngw_mailbox_count(&g_mb) == 1u);

    /* One poll: consumes the routed message, decodes it, parses the lead,
     * advances the tenant's FSM, and dispatches the registered script --
     * all in one zero-copy call. */
    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, &g_mb) == SWARM_OK);
    CHECK(ngw_mailbox_is_empty(&g_mb) == true);

    CHECK(g_script_calls[SWARM_STATE_CONTACTED] == 1);
    CHECK(g_last_tenant_id == 7u);
    CHECK(g_last_lead_id_seen == 555u);

    const swarm_agent_t *agent = swarm_get_agent(&g_sys, 7u);
    CHECK(agent != NULL);
    CHECK(agent->state == SWARM_STATE_CONTACTED);
    CHECK(agent->transitions_total == 1u);

    /* Polling again with nothing new posted is "empty", not an error. */
    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, &g_mb) == SWARM_ERR_EMPTY);

    /* Invalid arguments are rejected in O(1), no crash. */
    CHECK(swarm_poll_gateway_mailbox(NULL, &g_gw, &g_mb) == SWARM_ERR_INVALID);
    CHECK(swarm_poll_gateway_mailbox(&g_sys, NULL, &g_mb) == SWARM_ERR_INVALID);
    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, NULL) == SWARM_ERR_INVALID);
}

static void test_gateway_to_swarm_multiple_tenants(void) {
    SECTION("swarm_poll_gateway_mailbox: several tenants through the same mailbox stay isolated");

    ngw_gateway_init(&g_gw);
    ngw_mailbox_init(&g_mb);
    CHECK(ngw_register_actor(&g_gw, 0u, &g_mb) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 1u, KEY_A, 0u) == NGW_OK);
    CHECK(ngw_register_tenant(&g_gw, 2u, KEY_A, 0u) == NGW_OK);

    swarm_system_init(&g_sys);
    swarm_script_engine_init();

    uint8_t lead1[SWARM_LEAD_MAX_FRAME];
    uint8_t lead2[SWARM_LEAD_MAX_FRAME];
    size_t len1 = build_lead_payload(lead1, 1u, 1u, 0u, NULL); /* tenant 1: -> CONTACTED */
    size_t len2 = build_lead_payload(lead2, 2u, 3u, 0u, NULL); /* tenant 2: -> CONVERTED */

    uint8_t frame1[NGW_MAX_FRAME_SIZE];
    uint8_t frame2[NGW_MAX_FRAME_SIZE];
    size_t frame1_len = 0, frame2_len = 0;
    CHECK(ngw_build_frame(frame1, sizeof frame1, &frame1_len, NGW_PROTO_UDP, 1u, KEY_A, lead1, (uint16_t)len1) == NGW_OK);
    CHECK(ngw_build_frame(frame2, sizeof frame2, &frame2_len, NGW_PROTO_UDP, 2u, KEY_A, lead2, (uint16_t)len2) == NGW_OK);

    CHECK(ngw_ingest_frame(&g_gw, frame1, frame1_len) == NGW_OK);
    CHECK(ngw_ingest_frame(&g_gw, frame2, frame2_len) == NGW_OK);
    CHECK(ngw_mailbox_count(&g_mb) == 2u);

    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, &g_mb) == SWARM_OK);
    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, &g_mb) == SWARM_OK);
    CHECK(swarm_poll_gateway_mailbox(&g_sys, &g_gw, &g_mb) == SWARM_ERR_EMPTY);

    const swarm_agent_t *a1 = swarm_get_agent(&g_sys, 1u);
    const swarm_agent_t *a2 = swarm_get_agent(&g_sys, 2u);
    CHECK(a1 != NULL && a1->state == SWARM_STATE_CONTACTED);
    CHECK(a2 != NULL && a2->state == SWARM_STATE_CONVERTED);
}

/* ---------------------------------------------------------------- */

int main(void) {
    printf("=== test_swarm_integration ===\n");

    test_lead_parse_roundtrip();
    test_lead_parse_rejections();
    test_intent_mapping();
    test_fsm_transitions();
    test_ingest_drives_fsm_and_script();
    test_default_script_is_noop_until_registered();
    test_script_register_invalid_state();
    test_multi_tenant_isolation();
    test_unknown_tenant_rejected();
    test_gateway_to_swarm_zero_copy_pipeline();
    test_gateway_to_swarm_multiple_tenants();

    printf("=== %d run, %d failed ===\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
