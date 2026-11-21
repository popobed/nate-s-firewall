#include "agent_inspector.h"

#include <stddef.h>

/*
 * Liste de blocage statique, explicite et illustrative -- PAS exhaustive
 * (voir agent_inspector.h). Melange volontairement : appels
 * d'execution/systeme courants (Python et generiques), chemins
 * d'interpreteurs shell, et metacaracteres de chaining/redirection shell.
 */
static const char *const DENY_LIST[] = {
    "os.system",
    "os.popen",
    "subprocess",
    "exec(",
    "eval(",
    "__import__",
    "shell=true",
    "/bin/sh",
    "/bin/bash",
    "cmd.exe",
    "cmd /c",
    "powershell",
    "rm -rf",
    "del /f",
    "format c:",
    "createprocess",
    "shellexecute",
    "wscript.shell",
    "`",
    "$(",
    "||",
    "&&",
    ";",
    "curl ",
    "wget ",
    "nc -e",
    "chmod +x",
    "base64 -d",
    "sudo "
};

#define DENY_LIST_COUNT ((uint32_t)(sizeof(DENY_LIST) / sizeof(DENY_LIST[0])))

static char to_lower_ascii(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/* Recherche de sous-chaine insensible a la casse, bornee, zero
 * allocation. Retourne le decalage du premier match ou -1. */
static int32_t ci_find(const char *hay, uint32_t hay_len, const char *needle) {
    uint32_t needle_len = 0;
    while (needle[needle_len] != '\0') {
        needle_len++;
    }
    if (needle_len == 0 || needle_len > hay_len) {
        return -1;
    }
    for (uint32_t i = 0; i + needle_len <= hay_len; i++) {
        uint32_t j = 0;
        for (; j < needle_len; j++) {
            if (to_lower_ascii(hay[i + j]) != to_lower_ascii(needle[j])) {
                break;
            }
        }
        if (j == needle_len) {
            return (int32_t)i;
        }
    }
    return -1;
}

uint32_t agent_inspect_rule_count(void) {
    return DENY_LIST_COUNT;
}

agent_inspect_status_t agent_inspect_instruction(const char *payload, uint32_t len,
                                                  agent_inspect_result_t *out_result) {
    if (out_result == NULL || (payload == NULL && len != 0u)) {
        return AGENT_INSPECT_ERR_INVALID;
    }
    if (len > AGENT_INSTR_MAX_LEN) {
        return AGENT_INSPECT_ERR_TOO_LONG;
    }

    out_result->verdict = AGENT_VERDICT_ALLOWED;
    out_result->matched_rule_id = 0u;
    out_result->matched_offset = 0u;

    for (uint32_t rule = 0; rule < DENY_LIST_COUNT; rule++) {
        int32_t off = ci_find(payload, len, DENY_LIST[rule]);
        if (off >= 0) {
            out_result->verdict = AGENT_VERDICT_BLOCKED;
            out_result->matched_rule_id = rule;
            out_result->matched_offset = (uint32_t)off;
            return AGENT_INSPECT_OK;
        }
    }

    return AGENT_INSPECT_OK;
}
