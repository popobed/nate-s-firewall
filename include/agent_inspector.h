/*
 * agent_inspector.h
 *
 * Etape 1 (Zero-Trust) : inspecteur d'instructions pour les payloads
 * texte soumis par un agent IA, avant toute execution -- deny-by-default,
 * le verdict par defaut est BLOCKED des qu'un motif de la liste de
 * blocage statique est trouve.
 *
 * ============================== Honnetete, precisement =======================
 * Ce fichier N'EST PAS un analyseur syntaxique (AST) d'un langage de
 * programmation reel : aucun langage -- Python, shell POSIX, JavaScript
 * ou autre -- n'est tokenise selon sa grammaire, n'a d'arbre syntaxique
 * construit, ni de semantique evaluee ici. Ce depot ne contient aucun
 * interpreteur pour un tel langage (ni Python, ni shell, ni Lua) : rien
 * n'existe qu'un "AST" pourrait alimenter en aval. Pretendre le contraire
 * serait exactement le type de survente que ce projet refuse ailleurs
 * (voir barry_storage.h/infra_mesh.h/telemetry_fabric.h pour la meme
 * discipline appliquee a Kubernetes/Neo4j/AEAD).
 *
 * Ce que ce fichier EST reellement : un scanner de sous-chaines
 * insensible a la casse sur une liste de blocage statique et explicite
 * (os.system, subprocess, exec(, eval(, metacaracteres shell, etc.),
 * O(N*M) borne (N = longueur du payload <= AGENT_INSTR_MAX_LEN, M =
 * nombre de motifs, tous deux des constantes fixes petites), zero
 * allocation dynamique. C'est une premiere ligne de defense reelle et
 * testee contre des appels dangereux LITTERAUX -- PAS une garantie de
 * securite absolue : une liste de blocage ne peut, par construction,
 * jamais couvrir une obfuscation determinee (concatenation de chaines,
 * encodage base64, alias, homoglyphes Unicode). C'est precisement la
 * meme posture honnete que ce projet applique deja a FNV-1a dans
 * telemetry_fabric.h ("protege contre la corruption accidentelle et la
 * falsification naive, pas contre un attaquant determine avec une vraie
 * capacite de collision").
 */

#ifndef AGENT_INSPECTOR_H
#define AGENT_INSPECTOR_H

#include <stdint.h>

/* Longueur maximale d'un payload d'instruction d'agent que ce fichier
 * accepte d'inspecter -- borne fixe, zero allocation dynamique. */
#define AGENT_INSTR_MAX_LEN 256u

typedef enum {
    AGENT_INSPECT_OK = 0,
    AGENT_INSPECT_ERR_INVALID  = -1, /* payload NULL avec len != 0, ou out_result NULL */
    AGENT_INSPECT_ERR_TOO_LONG = -2  /* len > AGENT_INSTR_MAX_LEN */
} agent_inspect_status_t;

typedef enum {
    AGENT_VERDICT_ALLOWED = 0,
    AGENT_VERDICT_BLOCKED = 1
} agent_verdict_t;

typedef struct {
    agent_verdict_t verdict;
    /* Valides uniquement si verdict == AGENT_VERDICT_BLOCKED : index dans
     * la liste de blocage statique (voir agent_inspect_rule_count()) et
     * decalage en octets, dans le payload, ou le motif a ete trouve. */
    uint32_t matched_rule_id;
    uint32_t matched_offset;
} agent_inspect_result_t;

/* Taille de la liste de blocage statique -- utile pour les tests/outils,
 * pas pour interpreter matched_rule_id comme un identifiant stable entre
 * versions de ce fichier (l'ordre des motifs n'est pas une API figee). */
uint32_t agent_inspect_rule_count(void);

/*
 * Scanne `payload` (longueur `len`, PAS necessairement NUL-terminee)
 * contre la liste de blocage statique, insensible a la casse. Deny-by-
 * default : retourne AGENT_INSPECT_OK avec *out_result rempli dans tous
 * les cas (y compris ALLOWED) -- seule l'absence totale de correspondance
 * produit AGENT_VERDICT_ALLOWED. Le premier motif trouve, dans l'ordre de
 * la liste, determine matched_rule_id/matched_offset -- deterministe,
 * pas de recherche du "pire" motif.
 */
agent_inspect_status_t agent_inspect_instruction(const char *payload, uint32_t len,
                                                  agent_inspect_result_t *out_result);

#endif /* AGENT_INSPECTOR_H */
