/*
 * swarm_dispatch.h
 *
 * Phase 4 -- Swarm d'agents metiers : relie la reception de trames reseau
 * (net_gateway.h / net_io.h) a l'extraction de leads (lead_parser),
 * a la machine a etats par tenant (agent_state) et a la selection
 * automatique du gestionnaire d'automatisation associe (script_engine).
 *
 * ============================ Honest scope =================================
 * "script_engine" / "script d'automatisation": ce module ne contient AUCUN
 * interpreteur de script, ni chargeur de code, ni execution de texte/
 * bytecode externe. Un "script" est ici une fonction C compilee, enregistree
 * a l'avance par l'appelant, selectionnee par une lecture O(1) dans une
 * table indexee par l'etat FSM (g_script_table[agent_state_t]). Interpreter
 * un script arbitraire fourni a l'execution demanderait une allocation
 * dynamique non bornee (parsing, AST, pile d'interpretation) et une
 * execution de donnees non fiables -- directement incompatible avec la
 * contrainte "zero malloc/free post-boot" et la posture de securite du
 * reste de ce projet (voir net_gateway.h, crypto_constant_time.h). La partie
 * legitime de la demande -- "declencher automatiquement le bon traitement
 * quand l'etat change" -- est couverte honnetement par ce dispatch table
 * O(1) sur fonctions pointeurs statiquement connues; brancher de vrais
 * gestionnaires metier (envoi d'email, mise a jour CRM, etc.) est la
 * responsabilite de l'appelant via swarm_script_register().
 *
 * ============================ Pipeline ======================================
 * net_gateway.c / net_io.c authentifient une trame et la routent -- sans
 * copie du payload au-dela de la copie unique deja documentee dans
 * net_gateway.h -- dans un ngw_mailbox_t enregistre comme cible d'acteur.
 * swarm_poll_gateway_mailbox() est le point de jonction : il consomme UN
 * message deja route, decode son descripteur zero-copy (ngw_descriptor_t),
 * et transmet le pointeur de payload directement a swarm_ingest(), qui:
 *
 *   1. lead_parser  : swarm_lead_parse() lit un layout fixe et borne
 *                      (un nombre fixe de lectures de champs + UNE copie
 *                      bornee <= SWARM_LEAD_CONTACT_MAX dans le swarm_lead_t
 *                      fourni par l'appelant -- meme posture "O(1)/parsing
 *                      borne" que net_gateway.h et msg_router.h) directement
 *                      depuis le buffer du gateway, sans tampon intermediaire.
 *   2. agent_state  : l'agent du tenant (swarm_agent_t, indexe O(1) par
 *                      tenant_id) avance dans une table de transition fixe
 *                      [state][intent] -> next_state (swarm_fsm_next()).
 *   3. script_engine: la nouvelle valeur d'etat selectionne en O(1) le
 *                      script enregistre pour cet etat (swarm_script_fn),
 *                      invoque avec le lead deja parse.
 *
 * ============================ Determinisme & isolation =====================
 *   - Zero allocation dynamique: swarm_system_t n'embarque que du stockage
 *     statique/oriente-tableau (agents[SWARM_MAX_TENANTS]); aucun
 *     malloc/free n'existe dans ce fichier ni dans swarm_dispatch.c.
 *   - Isolation multi-tenant O(1): agents[] est indexe DIRECTEMENT par
 *     tenant_id (meme table de taille que NGW_MAX_TENANTS) -- une lecture de
 *     tableau, jamais une recherche, jamais d'etat partage entre deux
 *     tenants differents. Un tenant ne peut jamais lire ni modifier le
 *     swarm_agent_t d'un autre tenant: chaque acces passe par ce meme index
 *     direct.
 *   - Chaque etape (parse, transition FSM, dispatch de script) fait un
 *     nombre BORNE d'operations independant du contenu du payload (mais PAS
 *     "temps constant" au sens cryptographique: il n'y a ici aucun secret a
 *     proteger contre un canal auxiliaire -- l'authentification/le secret
 *     sont deja traites en amont par crypto_constant_time.h/net_gateway.c
 *     avant que ce module ne voie la moindre donnee).
 *
 * ============================ Alerte + relance a compteur de cycles ========
 * SWARM_INTENT_HUMAN_REQUEST signale une interaction critique (demande d'un
 * humain, placement de commande, prise de rendez-vous) : swarm_ingest_ex()
 * leve alors swarm_agent_t.alert_flag (lu/acquitte par l'appelant via
 * swarm_get_agent()/swarm_agent_ack_alert() -- ce fichier ne "notifie"
 * personne lui-meme, voir script_engine ci-dessus pour la meme raison) et,
 * si `followup_delay_cycles` != 0, programme un rappel deterministe a
 * `followup_due_cycle = current_cycle + followup_delay_cycles`.
 *
 * Delibrement PAS d'horloge murale/calendaire ici : ce fichier reste zero-
 * dependance externe (pas de RTC, pas de fuseau horaire, pas de notion de
 * "jour"). `current_cycle`/`followup_delay_cycles` sont des compteurs de
 * cycles opaques fournis par l'appelant -- typiquement
 * atomic_load(&actor->processed_total) (actor_core.h) ou tout autre
 * compteur monotone maison. Traduire "7 jours" en un nombre de cycles
 * (cycles-par-jour * 7) est la responsabilite de l'appelant, exactement
 * comme wire_proto.h refuse de faire de la conversion d'endianness a sa
 * place : une conversion implicite serait une hypothese cachee, pas une
 * simplification honnete.
 *
 * swarm_tick() est un point d'entree SEPARE (pas appele automatiquement par
 * swarm_ingest()) : l'appelant l'invoque a son propre rythme (par exemple a
 * chaque iteration de la boucle de l'acteur) avec sa valeur de cycle
 * courante ; si un rappel est du, le hook swarm_followup_register() est
 * invoque UNE FOIS puis le rappel est efface (one-shot). Le hook par
 * defaut est un no-op documente -- meme posture que script_engine : ce
 * fichier ne sait pas envoyer un email/SMS, il fournit le point
 * d'accroche deterministe.
 */

#ifndef SWARM_DISPATCH_H
#define SWARM_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net_gateway.h"

/* Meme taille de table que net_gateway.h: un lead ne peut arriver que pour
 * un tenant deja authentifie et route par la gateway, donc partager la
 * borne evite toute incoherence entre les deux couches. */
#define SWARM_MAX_TENANTS NGW_MAX_TENANTS

#define SWARM_LEAD_CONTACT_MAX 32u /* stockage fixe embarque, zero allocation dynamique */

/* offset 0: lead_id (u32) | offset 4: intent_code (u8) | offset 5: priority (u8)
 * | offset 6: contact_len (u8) | offset 7: contact (contact_len bytes) */
#define SWARM_LEAD_HEADER_SIZE (4u + 1u + 1u + 1u) /* = 7 */
#define SWARM_LEAD_MAX_FRAME   (SWARM_LEAD_HEADER_SIZE + SWARM_LEAD_CONTACT_MAX)

typedef enum {
    SWARM_OK = 0,
    SWARM_ERR_INVALID           = -1,
    SWARM_ERR_EMPTY              = -2, /* mailbox: rien a consommer -- pas une erreur */
    SWARM_ERR_TRUNCATED         = -3,  /* payload plus court que son layout fixe */
    SWARM_ERR_CONTACT_TOO_LARGE = -4,
    SWARM_ERR_UNKNOWN_TENANT    = -5   /* tenant_id >= SWARM_MAX_TENANTS */
} swarm_status_t;

/* Signal deterministe derive de l'octet brut intent_code (switch borne, pas
 * de boucle) -- espace de signaux fixe et volontairement restreint. */
typedef enum {
    SWARM_INTENT_NONE          = 0, /* code inconnu ou absence de signal: statu quo */
    SWARM_INTENT_INTEREST      = 1, /* le lead a repondu positivement */
    SWARM_INTENT_REJECT        = 2, /* le lead se desiste / refuse */
    SWARM_INTENT_CONVERT       = 3, /* le lead termine l'entonnoir (achat/inscription) */
    SWARM_INTENT_HUMAN_REQUEST = 4  /* interaction critique : demande d'humain / commande / RDV -- voir "Alerte + relance" */
} swarm_intent_t;

#define SWARM_INTENT_COUNT 5u

/* Machine a etats metier -- CONVERTED et LOST sont des etats absorbants
 * (voir swarm_fsm_next()). */
typedef enum {
    SWARM_STATE_NEW       = 0,
    SWARM_STATE_CONTACTED = 1,
    SWARM_STATE_QUALIFIED = 2,
    SWARM_STATE_CONVERTED = 3,
    SWARM_STATE_LOST      = 4
} agent_state_t;

#define SWARM_STATE_COUNT 5u

/* Lead a layout fixe, produit par swarm_lead_parse(). */
typedef struct {
    uint32_t lead_id;
    uint8_t  intent_code; /* octet brut du fil -- voir swarm_intent_from_code() */
    uint8_t  priority;
    uint8_t  contact_len; /* <= SWARM_LEAD_CONTACT_MAX */
    uint8_t  contact[SWARM_LEAD_CONTACT_MAX]; /* stockage embarque fixe */
} swarm_lead_t;

/* ------------------------------------------------------------------ */
/* script_engine : table O(1) de gestionnaires enregistres              */
/* ------------------------------------------------------------------ */

typedef void (*swarm_script_fn)(uint16_t tenant_id, agent_state_t old_state,
                                 agent_state_t new_state, const swarm_lead_t *lead);

/* Reinitialise la table a son gestionnaire par defaut (no-op documente) pour
 * chacun des SWARM_STATE_COUNT etats. A appeler avant tout enregistrement. */
void swarm_script_engine_init(void);

/* Enregistre `fn` comme script pour `state` (O(1), simple ecriture de
 * tableau). fn == NULL restaure le gestionnaire par defaut. */
swarm_status_t swarm_script_register(agent_state_t state, swarm_script_fn fn);

/* ------------------------------------------------------------------ */
/* agent_state : slot isole par tenant                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    agent_state_t state;
    uint32_t      last_lead_id;
    uint64_t      transitions_total;      /* incremente uniquement quand l'etat change reellement */
    uint64_t      scripts_invoked_total;  /* incremente a chaque dispatch, meme en auto-boucle */
    bool          active;                 /* initialise paresseusement au premier lead du tenant */
    bool          alert_flag;             /* leve par SWARM_INTENT_HUMAN_REQUEST -- acquitte via swarm_agent_ack_alert() */
    uint64_t      followup_due_cycle;     /* 0 = aucun rappel programme -- voir "Alerte + relance a compteur de cycles" */
} swarm_agent_t;

typedef struct {
    /* Indexe DIRECTEMENT par tenant_id: O(1), isolation stricte -- aucun
     * champ ici n'est jamais partage entre deux index differents. */
    swarm_agent_t agents[SWARM_MAX_TENANTS];
} swarm_system_t;

void swarm_system_init(swarm_system_t *sys);

/* ------------------------------------------------------------------ */
/* lead_parser                                                          */
/* ------------------------------------------------------------------ */

/* Parse O(1)/borne (nombre fixe de lectures de champs + une copie bornee
 * <= SWARM_LEAD_CONTACT_MAX) d'un layout fixe -- pas de scan de delimiteur,
 * meme posture que net_gateway.h/msg_router.h. Les entiers multi-octets
 * sont lus host-native (memcpy direct), comme wire_proto.h: le payload
 * provient du meme processus/machine (slab de net_gateway.c), voir la note
 * d'honnetete de wire_proto.h sur la portee de ce choix. */
swarm_status_t swarm_lead_parse(const uint8_t *payload, size_t len, swarm_lead_t *out);

/* Mappage deterministe et borne (switch, jamais de boucle) de l'octet brut
 * vers l'espace de signaux fixe. Un code inconnu retourne SWARM_INTENT_NONE
 * -- ce n'est jamais une erreur. */
swarm_intent_t swarm_intent_from_code(uint8_t intent_code);

/* Table de transition fixe [state][intent] -> next_state, lecture O(1). */
agent_state_t swarm_fsm_next(agent_state_t current, swarm_intent_t intent);

/* ------------------------------------------------------------------ */
/* Pipeline complet                                                     */
/* ------------------------------------------------------------------ */

/*
 * Pipeline lead_parser -> agent_state -> script_engine pour un payload deja
 * authentifie et route par tenant (la verification d'authenticite reste la
 * responsabilite de net_gateway.c/net_io.c, en amont). O(1) hormis la copie
 * bornee de swarm_lead_parse(): une lecture de tableau (isolation tenant),
 * un parse borne, une lecture de table FSM, une lecture de table de script.
 */
swarm_status_t swarm_ingest(swarm_system_t *sys, uint16_t tenant_id,
                             const uint8_t *payload, size_t len);

/*
 * Comme swarm_ingest(), avec en plus la logique d'alerte + relance
 * deterministe (voir "Alerte + relance a compteur de cycles" ci-dessus).
 * `current_cycle` est le compteur de cycles opaque de l'appelant au moment
 * de cet ingest. `followup_delay_cycles` == 0 signifie "ne programme rien
 * cette fois" (un rappel deja programme, le cas echeant, n'est PAS annule).
 * swarm_ingest(sys,t,p,l) == swarm_ingest_ex(sys,t,p,l,0,0).
 */
swarm_status_t swarm_ingest_ex(swarm_system_t *sys, uint16_t tenant_id,
                                const uint8_t *payload, size_t len,
                                uint64_t current_cycle, uint64_t followup_delay_cycles);

/* Hook de relance : invoque UNE FOIS par swarm_tick() quand un rappel
 * programme arrive a echeance, puis efface followup_due_cycle (one-shot).
 * Gestionnaire par defaut : no-op documente (voir script_engine) --
 * l'envoi reel (email/SMS/etc.) est la responsabilite de l'appelant. */
typedef void (*swarm_followup_fn)(uint16_t tenant_id, agent_state_t state, uint64_t current_cycle);
void swarm_followup_register(swarm_followup_fn fn); /* fn == NULL restaure le gestionnaire par defaut */

/* A appeler par l'appelant a son propre rythme (ex: chaque iteration de la
 * boucle de son acteur) avec sa valeur de cycle courante. O(1) : une
 * lecture de tableau (isolation tenant) + une comparaison. Ne fait rien
 * (SWARM_OK) si aucun rappel n'est du. */
swarm_status_t swarm_tick(swarm_system_t *sys, uint16_t tenant_id, uint64_t current_cycle);

/* Acquitte (efface) alert_flag pour ce tenant -- typiquement appele une
 * fois qu'un operateur/dashboard a pris connaissance de l'alerte. */
swarm_status_t swarm_agent_ack_alert(swarm_system_t *sys, uint16_t tenant_id);

/*
 * Point d'accroche additif (n'altere aucune fonction existante de ce
 * fichier) : leve alert_flag pour ce tenant SANS passer par la detection
 * d'intention de swarm_ingest_ex() -- reserve aux appelants qui ont deja
 * detecte, par un autre moyen, une condition critique meritant la meme
 * alerte qu'un SWARM_INTENT_HUMAN_REQUEST (ex: telemetry_fabric.h y
 * recourt quand tf_log_verify() detecte une chaine de logs corrompue).
 * O(1), meme isolation tenant (index direct) que le reste de ce fichier.
 */
swarm_status_t swarm_agent_raise_alert(swarm_system_t *sys, uint16_t tenant_id);

/*
 * Interconnexion zero-copy avec net_gateway.h/net_io.h : consomme EXACTEMENT
 * un message deja route dans `mailbox` (un ngw_mailbox_t prealablement
 * enregistre comme cible via ngw_register_actor() ou
 * net_io_register_actor()/net_io_wire_register_actor()), decode son
 * descripteur zero-copy, transmet son pointeur directement a swarm_ingest()
 * (aucune copie supplementaire au-dela de celle, deja bornee, de
 * swarm_lead_parse()), puis restitue le bloc de slab a `gw`. SWARM_ERR_EMPTY
 * signifie "rien de pret" -- ce n'est pas un echec, meme convention que
 * ngw_mailbox_consume().
 */
swarm_status_t swarm_poll_gateway_mailbox(swarm_system_t *sys, ngw_gateway_t *gw,
                                           ngw_mailbox_t *mailbox);

/* Lecture O(1) diagnostique; NULL si tenant_id est hors bornes. */
const swarm_agent_t *swarm_get_agent(const swarm_system_t *sys, uint16_t tenant_id);

#endif /* SWARM_DISPATCH_H */
