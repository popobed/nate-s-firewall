/*
 * swarm_dispatch.c
 *
 * Voir swarm_dispatch.h pour la portee (honest scope), le pipeline et les
 * garanties de determinisme/isolation. Ce fichier n'appelle jamais
 * malloc/free/calloc/realloc: toute la memoire est statique (le tableau
 * g_script_table) ou fournie par l'appelant (swarm_system_t, swarm_lead_t).
 */

#include "swarm_dispatch.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* script_engine                                                        */
/* ------------------------------------------------------------------ */

/* Gestionnaire par defaut : aucun effet de bord. Un vrai deploiement le
 * remplace via swarm_script_register() -- voir la note "Honest scope" du
 * header pour pourquoi ce module ne fait pas plus que cela lui-meme. */
static void swarm_script_default_noop(uint16_t tenant_id, agent_state_t old_state,
                                       agent_state_t new_state, const swarm_lead_t *lead) {
    (void)tenant_id;
    (void)old_state;
    (void)new_state;
    (void)lead;
}

static swarm_script_fn g_script_table[SWARM_STATE_COUNT];

/* swarm_followup_register() est declaree dans swarm_dispatch.h et definie
 * plus bas ("Alerte + relance") ; l'appeler ici reinitialise aussi le hook
 * de relance, par simplicite (un seul point de reset "engine"). */
void swarm_script_engine_init(void) {
    for (size_t i = 0; i < SWARM_STATE_COUNT; ++i) {
        g_script_table[i] = swarm_script_default_noop;
    }
    swarm_followup_register(NULL);
}

swarm_status_t swarm_script_register(agent_state_t state, swarm_script_fn fn) {
    if ((size_t)state >= SWARM_STATE_COUNT) {
        return SWARM_ERR_INVALID;
    }
    g_script_table[state] = fn != NULL ? fn : swarm_script_default_noop;
    return SWARM_OK;
}

/* O(1) : une seule lecture de tableau, jamais de recherche. `new_state` est
 * toujours < SWARM_STATE_COUNT par construction (issu de swarm_fsm_next()). */
static void swarm_script_dispatch(uint16_t tenant_id, agent_state_t old_state,
                                   agent_state_t new_state, const swarm_lead_t *lead) {
    swarm_script_fn fn = g_script_table[new_state];
    if (fn != NULL) {
        fn(tenant_id, old_state, new_state, lead);
    }
}

/* ------------------------------------------------------------------ */
/* agent_state / swarm_system_t                                         */
/* ------------------------------------------------------------------ */

void swarm_system_init(swarm_system_t *sys) {
    if (sys == NULL) {
        return;
    }
    memset(sys->agents, 0, sizeof sys->agents);
}

const swarm_agent_t *swarm_get_agent(const swarm_system_t *sys, uint16_t tenant_id) {
    if (sys == NULL || tenant_id >= SWARM_MAX_TENANTS) {
        return NULL;
    }
    return &sys->agents[tenant_id];
}

/* ------------------------------------------------------------------ */
/* lead_parser                                                          */
/* ------------------------------------------------------------------ */

swarm_status_t swarm_lead_parse(const uint8_t *payload, size_t len, swarm_lead_t *out) {
    if (payload == NULL || out == NULL) {
        return SWARM_ERR_INVALID;
    }
    if (len < SWARM_LEAD_HEADER_SIZE) {
        return SWARM_ERR_TRUNCATED;
    }

    uint32_t lead_id;
    memcpy(&lead_id, payload, sizeof lead_id); /* host-native, voir la note du header */
    const uint8_t intent_code = payload[4];
    const uint8_t priority    = payload[5];
    const uint8_t contact_len = payload[6];

    if (contact_len > SWARM_LEAD_CONTACT_MAX) {
        return SWARM_ERR_CONTACT_TOO_LARGE;
    }
    if (len < (size_t)SWARM_LEAD_HEADER_SIZE + contact_len) {
        return SWARM_ERR_TRUNCATED;
    }

    out->lead_id     = lead_id;
    out->intent_code = intent_code;
    out->priority    = priority;
    out->contact_len = contact_len;
    memset(out->contact, 0, sizeof out->contact);
    memcpy(out->contact, payload + SWARM_LEAD_HEADER_SIZE, contact_len);

    return SWARM_OK;
}

swarm_intent_t swarm_intent_from_code(uint8_t intent_code) {
    switch (intent_code) {
        case 1u: return SWARM_INTENT_INTEREST;
        case 2u: return SWARM_INTENT_REJECT;
        case 3u: return SWARM_INTENT_CONVERT;
        case 4u: return SWARM_INTENT_HUMAN_REQUEST;
        default: return SWARM_INTENT_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* agent_state : table de transition fixe                               */
/* ------------------------------------------------------------------ */

/* [state][intent] -> next_state. CONVERTED et LOST sont absorbants : quel
 * que soit le signal recu ensuite, l'agent y reste (un lead converti ou
 * perdu ne revient pas automatiquement dans l'entonnoir). HUMAN_REQUEST est
 * une colonne identite partout : c'est un signal orthogonal a la
 * progression dans l'entonnoir (voir "Alerte + relance a compteur de
 * cycles") -- il leve alert_flag/programme un rappel dans swarm_ingest_ex()
 * mais ne fait jamais avancer ou reculer la FSM par lui-meme. */
static const agent_state_t SWARM_FSM_TABLE[SWARM_STATE_COUNT][SWARM_INTENT_COUNT] = {
    /*                    NONE                  INTEREST               REJECT            CONVERT               HUMAN_REQUEST */
    /* NEW        */ { SWARM_STATE_NEW,       SWARM_STATE_CONTACTED, SWARM_STATE_LOST, SWARM_STATE_CONVERTED, SWARM_STATE_NEW },
    /* CONTACTED  */ { SWARM_STATE_CONTACTED, SWARM_STATE_QUALIFIED, SWARM_STATE_LOST, SWARM_STATE_CONVERTED, SWARM_STATE_CONTACTED },
    /* QUALIFIED  */ { SWARM_STATE_QUALIFIED, SWARM_STATE_QUALIFIED, SWARM_STATE_LOST, SWARM_STATE_CONVERTED, SWARM_STATE_QUALIFIED },
    /* CONVERTED  */ { SWARM_STATE_CONVERTED, SWARM_STATE_CONVERTED, SWARM_STATE_CONVERTED, SWARM_STATE_CONVERTED, SWARM_STATE_CONVERTED },
    /* LOST       */ { SWARM_STATE_LOST,      SWARM_STATE_LOST,      SWARM_STATE_LOST,      SWARM_STATE_LOST,      SWARM_STATE_LOST }
};

agent_state_t swarm_fsm_next(agent_state_t current, swarm_intent_t intent) {
    if ((size_t)current >= SWARM_STATE_COUNT || (size_t)intent >= SWARM_INTENT_COUNT) {
        return current; /* defensif : entrees hors-domaine ne bougent pas l'etat */
    }
    return SWARM_FSM_TABLE[current][intent];
}

/* ------------------------------------------------------------------ */
/* Pipeline complet                                                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Alerte + relance a compteur de cycles                                */
/* ------------------------------------------------------------------ */

static void swarm_followup_default_noop(uint16_t tenant_id, agent_state_t state, uint64_t current_cycle) {
    (void)tenant_id;
    (void)state;
    (void)current_cycle;
}

static swarm_followup_fn g_followup_hook = swarm_followup_default_noop;

void swarm_followup_register(swarm_followup_fn fn) {
    g_followup_hook = fn != NULL ? fn : swarm_followup_default_noop;
}

swarm_status_t swarm_ingest_ex(swarm_system_t *sys, uint16_t tenant_id,
                                const uint8_t *payload, size_t len,
                                uint64_t current_cycle, uint64_t followup_delay_cycles) {
    if (sys == NULL) {
        return SWARM_ERR_INVALID;
    }
    if (tenant_id >= SWARM_MAX_TENANTS) {
        return SWARM_ERR_UNKNOWN_TENANT;
    }

    swarm_lead_t lead;
    swarm_status_t parse_st = swarm_lead_parse(payload, len, &lead);
    if (parse_st != SWARM_OK) {
        return parse_st;
    }

    swarm_agent_t *agent = &sys->agents[tenant_id]; /* O(1), isole par tenant */
    if (!agent->active) {
        agent->active = true;
        agent->state  = SWARM_STATE_NEW;
    }

    const swarm_intent_t intent    = swarm_intent_from_code(lead.intent_code);
    const agent_state_t  old_state = agent->state;
    const agent_state_t  new_state = swarm_fsm_next(old_state, intent);

    agent->state        = new_state;
    agent->last_lead_id = lead.lead_id;
    if (new_state != old_state) {
        agent->transitions_total++;
    }

    if (intent == SWARM_INTENT_HUMAN_REQUEST) {
        agent->alert_flag = true;
        if (followup_delay_cycles != 0u) {
            agent->followup_due_cycle = current_cycle + followup_delay_cycles;
        }
    }

    swarm_script_dispatch(tenant_id, old_state, new_state, &lead);
    agent->scripts_invoked_total++;

    return SWARM_OK;
}

swarm_status_t swarm_ingest(swarm_system_t *sys, uint16_t tenant_id,
                             const uint8_t *payload, size_t len) {
    return swarm_ingest_ex(sys, tenant_id, payload, len, 0u, 0u);
}

swarm_status_t swarm_tick(swarm_system_t *sys, uint16_t tenant_id, uint64_t current_cycle) {
    if (sys == NULL) {
        return SWARM_ERR_INVALID;
    }
    if (tenant_id >= SWARM_MAX_TENANTS) {
        return SWARM_ERR_UNKNOWN_TENANT;
    }
    swarm_agent_t *agent = &sys->agents[tenant_id]; /* O(1), isole par tenant */
    if (agent->followup_due_cycle != 0u && current_cycle >= agent->followup_due_cycle) {
        g_followup_hook(tenant_id, agent->state, current_cycle);
        agent->followup_due_cycle = 0u; /* one-shot */
    }
    return SWARM_OK;
}

swarm_status_t swarm_agent_ack_alert(swarm_system_t *sys, uint16_t tenant_id) {
    if (sys == NULL) {
        return SWARM_ERR_INVALID;
    }
    if (tenant_id >= SWARM_MAX_TENANTS) {
        return SWARM_ERR_UNKNOWN_TENANT;
    }
    sys->agents[tenant_id].alert_flag = false;
    return SWARM_OK;
}

swarm_status_t swarm_agent_raise_alert(swarm_system_t *sys, uint16_t tenant_id) {
    if (sys == NULL) {
        return SWARM_ERR_INVALID;
    }
    if (tenant_id >= SWARM_MAX_TENANTS) {
        return SWARM_ERR_UNKNOWN_TENANT;
    }
    sys->agents[tenant_id].alert_flag = true;
    return SWARM_OK;
}

swarm_status_t swarm_poll_gateway_mailbox(swarm_system_t *sys, ngw_gateway_t *gw,
                                           ngw_mailbox_t *mailbox) {
    if (sys == NULL || gw == NULL || mailbox == NULL) {
        return SWARM_ERR_INVALID;
    }

    ngw_message_t msg;
    ngw_status_t st = ngw_mailbox_consume(mailbox, &msg);
    if (st == NGW_ERR_EMPTY) {
        return SWARM_ERR_EMPTY;
    }
    if (st != NGW_OK) {
        return SWARM_ERR_INVALID;
    }
    if (msg.type == NGW_MSG_TYPE_DISCARD) {
        /* Reservation avortee en amont (voir net_gateway.h) : rien de reel
         * a traiter, pas une erreur. */
        return SWARM_ERR_EMPTY;
    }

    ngw_descriptor_t desc;
    if (ngw_decode_message(&msg, &desc) != NGW_OK) {
        return SWARM_ERR_INVALID;
    }

    /* Zero-copy : desc.ptr pointe directement dans le slab du gateway,
     * lu en place par swarm_lead_parse() a l'interieur de swarm_ingest(). */
    swarm_status_t rc = swarm_ingest(sys, desc.tenant_id, desc.ptr, desc.len);
    ngw_release_payload(gw, desc.ptr);
    return rc;
}
