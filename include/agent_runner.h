/*
 * agent_runner.h
 *
 * Etape 2 (Zero-Trust) : wrapper d'execution pour les instructions d'agent
 * IA deja definies par agent_inspector.h -- deny-by-default (l'inspecteur
 * tourne EN PREMIER et sans exception), dispatch borne vers une table de
 * commandes fixe et pre-enregistree, budget de temps mesure, et
 * journalisation dans telemetry_fabric.h (reutilise verbatim) de CHAQUE
 * appel, accepte ou refuse.
 *
 * ============================== Honnetete, precisement =======================
 *
 * - "Sandbox isolee" : ce fichier n'offre AUCUNE isolation au niveau OS
 *   (pas de conteneur, pas de seccomp, pas de process separe) -- le
 *   handler tourne dans le MEME processus, la MEME adresse memoire que
 *   l'appelant. L'isolation reellement fournie ici est double et
 *   deterministe : (1) deny-by-default via agent_inspect_instruction()
 *   AVANT toute autre chose -- une instruction bloquee n'atteint jamais
 *   la table de commandes ; (2) une instruction, meme autorisee, ne peut
 *   JAMAIS invoquer de code arbitraire -- elle ne peut selectionner qu'UN
 *   des AGENT_RUNNER_MAX_COMMANDS handlers C explicitement pre-enregistres
 *   par l'appelant via agent_runner_register(). Il n'existe aucun
 *   interpreteur (Python/shell/JS) dans ce depot : "executer le script de
 *   l'agent" signifie concretement "distribuer son instruction, deja
 *   filtree, vers un handler pre-enregistre et borne" -- reel,
 *   deterministe, teste, mais PAS equivalent a l'execution de code
 *   Turing-complet non fiable dans un environnement durci
 *   (gVisor/Docker/seccomp). Pretendre le contraire serait le meme type
 *   de survente que barry_storage.h/infra_mesh.h refusent deja pour
 *   Kubernetes/Neo4j.
 *
 * - "Time-out CPU" : ce budget est MESURE et COOPERATIF, PAS preemptif.
 *   Aucune primitive de ce fichier n'arrete de force un handler qui ne
 *   revient jamais et ignore agent_deadline_exceeded() -- une preemption
 *   sure necessiterait une isolation au niveau processus/OS (un "thread
 *   kill" force peut laisser un verrou tenu ou un etat partage a moitie
 *   ecrit, exactement le risque que barry_storage.h's seqlock design
 *   documente et refuse d'ignorer), hors de portee ici. Concretement :
 *   agent_runner_execute() mesure le temps reel ecoule (horloge
 *   monotone -- GetTickCount64()/clock_gettime(CLOCK_MONOTONIC), un usage
 *   legitime et differe de la position "pas d'horloge murale" de
 *   swarm_tick/barry_ts.h -- CETTE position concernait la semantique
 *   METIER d'un calendrier, pas la mesure basique d'une latence
 *   d'execution) autour de l'appel handler(), et retourne
 *   AGENT_RUN_ERR_TIMEOUT si le budget est depasse UNE FOIS QUE le
 *   handler est revenu. Un handler long peut consulter
 *   agent_deadline_exceeded(deadline) lui-meme pour revenir tot -- un
 *   vrai mecanisme cooperatif, utilisable et teste -- mais rien ici ne le
 *   force a le faire.
 *
 * - "Audit cryptographique" : reutilise telemetry_fabric.h's tf_log_t
 *   verbatim -- CE fichier documente deja que sa chaine est FNV-1a
 *   (rapide, non cryptographique, detecte la falsification accidentelle
 *   ou naive, PAS une attaque avec une vraie capacite de collision). Ce
 *   fichier ne change pas cette portee ; "audit" ici signifie
 *   precisement ce que telemetry_fabric.h offre deja, reutilise, pas
 *   reimplemente.
 */

#ifndef AGENT_RUNNER_H
#define AGENT_RUNNER_H

#include <stdbool.h>
#include <stdint.h>

#include "agent_inspector.h"
#include "telemetry_fabric.h"

#define AGENT_RUNNER_MAX_COMMANDS 16u
#define AGENT_CMD_NAME_MAX        16u

typedef enum {
    AGENT_RUN_OK = 0,
    AGENT_RUN_ERR_INVALID            = -1,
    AGENT_RUN_ERR_BLOCKED            = -2, /* agent_inspect_instruction() denied it -- deny-by-default */
    AGENT_RUN_ERR_UNKNOWN_COMMAND    = -3,
    AGENT_RUN_ERR_TIMEOUT            = -4, /* handler DID return, but past its measured budget -- see file header */
    AGENT_RUN_ERR_FULL               = -5, /* command table full at registration */
    AGENT_RUN_ERR_ALREADY_REGISTERED = -6
} agent_run_status_t;

/* Real, opt-in cooperative deadline a long-running handler can consult
 * to return early. See file header for why this file cannot force a
 * non-cooperating handler to stop. */
typedef struct {
    uint64_t deadline_ms; /* monotonic clock value (see agent_runner.c's agent_now_ms()) */
} agent_deadline_t;

bool agent_deadline_exceeded(const agent_deadline_t *deadline);

typedef void (*agent_command_handler_fn)(void *ctx, const char *arg, uint32_t arg_len,
                                          const agent_deadline_t *deadline);

typedef struct {
    char                      name[AGENT_CMD_NAME_MAX];
    uint32_t                  name_len;
    agent_command_handler_fn  handler;
    void                     *ctx;
    bool                      registered;
} agent_command_slot_t;

typedef struct {
    agent_command_slot_t commands[AGENT_RUNNER_MAX_COMMANDS];
    uint32_t             command_count;
} agent_runner_t;

void agent_runner_init(agent_runner_t *runner);

/* Boot-time/admin-time only, like mesh_register_node()/tf_key_store_register():
 * not thread-safe against concurrent registration or execute() calls.
 * AGENT_RUN_ERR_FULL past AGENT_RUNNER_MAX_COMMANDS, AGENT_RUN_ERR_ALREADY_REGISTERED
 * on a duplicate name. */
agent_run_status_t agent_runner_register(agent_runner_t *runner, const char *name, uint32_t name_len,
                                          agent_command_handler_fn handler, void *ctx);

/* tf_log_t event_type tag this file uses. */
#define AGENT_RUN_TF_EVENT 0xA9E00001u

/* Audit payload this file encodes into tf_log_t entries -- well under
 * TF_LOG_MSG_MAX (48 bytes). */
typedef struct {
    int16_t  status;            /* agent_run_status_t */
    uint32_t matched_rule_id;   /* meaningful iff status == AGENT_RUN_ERR_BLOCKED */
    uint32_t payload_len;
    char     command_name[AGENT_CMD_NAME_MAX]; /* best-effort first token; zeroed if never parsed (e.g. BLOCKED) */
} agent_run_tf_audit_payload_t;

_Static_assert(sizeof(agent_run_tf_audit_payload_t) <= TF_LOG_MSG_MAX,
               "agent_run_tf_audit_payload_t must fit tf_log_t's fixed payload");

/*
 * Zero-Trust dispatch, in this exact order:
 *   1) agent_inspect_instruction(payload, len) runs unconditionally.
 *      AGENT_VERDICT_BLOCKED -> AGENT_RUN_ERR_BLOCKED, the command table
 *      is never even consulted.
 *   2) The payload's first whitespace-delimited token is looked up in
 *      `runner`'s command table (bounded linear scan,
 *      AGENT_RUNNER_MAX_COMMANDS is small and fixed). Not found ->
 *      AGENT_RUN_ERR_UNKNOWN_COMMAND.
 *   3) The matching handler runs SYNCHRONOUSLY on the caller's own
 *      thread (see file header: no preemption is offered), given the
 *      remainder of the payload as `arg` and a real deadline
 *      (now + timeout_ms) it may consult. AGENT_RUN_ERR_TIMEOUT if the
 *      measured elapsed time exceeds timeout_ms once the handler
 *      returns, else AGENT_RUN_OK.
 * `audit_log` receives exactly one tf_log_t entry per call (any outcome);
 * may be NULL to skip logging. `timeout_ms` must be >= 1.
 */
agent_run_status_t agent_runner_execute(agent_runner_t *runner, const char *payload, uint32_t len,
                                         uint32_t timeout_ms, tf_log_t *audit_log);

#endif /* AGENT_RUNNER_H */
