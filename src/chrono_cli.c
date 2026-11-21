/*
 * chrono_cli.c -- chrono-blackbox-cli: standalone, offline audit tool for
 * a CHRONO-BLACKBOX .bin/.wal file.
 *
 * ============================== Scope, precisely =============================
 * Reads a file written by chrono_storage_flush_window() and: (1) extracts
 * and prints every window header (timestamp, Merkle root, block count)
 * and every block's fields, (2) recomputes each window's Merkle root
 * locally for display, and (3) runs the real, authoritative
 * chrono_storage_verify_file() (chrono_storage.c, unmodified, reused
 * verbatim) for the pass/fail integrity verdict -- the same check
 * tests/test_chrono_merkle_storage.c already validates end to end.
 *
 * Deliberately does NOT call chrono_ingest_init() or open any socket:
 * this binary never starts the network reactor. It only ever reads an
 * existing file from disk -- the "verification hors-ligne" (offline
 * verification) the name promises, literally.
 *
 * Exit code: 0 iff chrono_storage_verify_file() returns CHRONO_STORAGE_OK,
 * matching the standard CLI convention (0 = success) so this tool is
 * usable directly from a script/CI step, not only interactively.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "chrono_storage.h"

static void print_hex(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", buf[i]);
    }
}

static const char *storage_status_name(chrono_storage_status_t st) {
    switch (st) {
        case CHRONO_STORAGE_OK:            return "OK";
        case CHRONO_STORAGE_ERR_INVALID:   return "ERR_INVALID";
        case CHRONO_STORAGE_ERR_IO:        return "ERR_IO";
        case CHRONO_STORAGE_ERR_TRUNCATED: return "ERR_TRUNCATED";
        case CHRONO_STORAGE_ERR_TAMPERED:  return "ERR_TAMPERED";
        default:                           return "UNKNOWN";
    }
}

/*
 * Purely informational extraction pass: parses and prints every window
 * and block using this project's OWN canonical decode
 * (chrono_block_deserialize(), chrono_ingest.h) -- the exact inverse of
 * what chrono_storage_flush_window() wrote. This pass does NOT itself
 * decide pass/fail (it prints "MISMATCH" inline if a window's Merkle root
 * fails to recompute, but keeps walking the file so a corrupted window
 * doesn't hide the extraction of the windows around it) -- the
 * authoritative verdict is chrono_storage_verify_file(), called
 * separately by main() below.
 */
static void dump_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        printf("  (cannot open '%s' for reading)\n", filepath);
        return;
    }

    uint32_t window_index = 0;
    for (;;) {
        uint8_t header[CHRONO_STORAGE_WINDOW_HEADER_SIZE];
        size_t n = fread(header, 1, sizeof header, f);
        if (n == 0u) {
            break; /* clean EOF at a window boundary */
        }
        if (n != sizeof header) {
            printf("  [window %u] TRUNCATED header (%zu of %u bytes read) -- stopping extraction here\n",
                   window_index, n, (unsigned)sizeof header);
            break;
        }

        uint32_t magic, block_count;
        uint64_t window_timestamp_ns;
        uint8_t stored_root[SHA256_DIGEST_SIZE];
        size_t off = 0u;
        memcpy(&magic, header + off, sizeof magic); off += sizeof magic;
        memcpy(&block_count, header + off, sizeof block_count); off += sizeof block_count;
        memcpy(&window_timestamp_ns, header + off, sizeof window_timestamp_ns); off += sizeof window_timestamp_ns;
        memcpy(stored_root, header + off, SHA256_DIGEST_SIZE);

        printf("Window %u: magic=0x%08x (%s) block_count=%u window_timestamp_ns=%" PRIu64 "\n",
               window_index, magic, (magic == CHRONO_STORAGE_MAGIC) ? "valid" : "INVALID", block_count,
               window_timestamp_ns);
        printf("  merkle_root (stored) = ");
        print_hex(stored_root, SHA256_DIGEST_SIZE);
        printf("\n");

        if (magic != CHRONO_STORAGE_MAGIC || block_count == 0u || block_count > CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW) {
            printf("  [!] malformed window header -- stopping extraction here\n");
            break;
        }

        chrono_block_t blocks[CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW];
        bool truncated = false;
        for (uint32_t i = 0; i < block_count; i++) {
            uint8_t serialized[CHRONO_BLOCK_SERIALIZED_SIZE];
            n = fread(serialized, 1, sizeof serialized, f);
            if (n != sizeof serialized) {
                truncated = true;
                break;
            }
            chrono_block_deserialize(serialized, &blocks[i]);

            printf("  block[%u] sequence_id=%" PRIu64 " tenant_id=%u timestamp_ns=%" PRIu64 "\n",
                   i, blocks[i].sequence_id, blocks[i].tenant_id, blocks[i].timestamp_ns);
            printf("            payload_hash=");
            print_hex(blocks[i].payload_hash, SHA256_DIGEST_SIZE);
            printf(" block_hash=");
            print_hex(blocks[i].block_hash, SHA256_DIGEST_SIZE);
            printf("\n");
        }
        if (truncated) {
            printf("  [!] TRUNCATED mid-block -- stopping extraction here\n");
            break;
        }

        uint8_t recomputed_root[SHA256_DIGEST_SIZE];
        chrono_merkle_status_t mst = chrono_merkle_compute_root(blocks, (size_t)block_count, recomputed_root);
        bool root_matches = (mst == CHRONO_MERKLE_OK) && (memcmp(recomputed_root, stored_root, SHA256_DIGEST_SIZE) == 0);
        printf("  merkle_root (recomputed) = ");
        if (mst == CHRONO_MERKLE_OK) {
            print_hex(recomputed_root, SHA256_DIGEST_SIZE);
        } else {
            printf("(computation failed)");
        }
        printf(" -> %s\n", root_matches ? "MATCH" : "MISMATCH");

        window_index++;
    }

    fclose(f);
    printf("(%u window%s extracted)\n", window_index, (window_index == 1u) ? "" : "s");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <chrono-blackbox-audit-file.bin>\n", (argc > 0) ? argv[0] : "chrono-blackbox-cli");
        fprintf(stderr, "  Offline extraction + integrity verification of a CHRONO-BLACKBOX\n");
        fprintf(stderr, "  append-only storage file (chrono_storage.c). Never opens a socket.\n");
        return 2;
    }
    const char *filepath = argv[1];

    printf("=== chrono-blackbox-cli: offline audit of '%s' ===\n\n", filepath);

    printf("--- Block extraction ---\n");
    dump_file(filepath);

    printf("\n--- Integrity verification (chrono_storage_verify_file) ---\n");
    chrono_storage_status_t st = chrono_storage_verify_file(filepath);
    printf("Result: %s (%d)\n", storage_status_name(st), (int)st);

    return (st == CHRONO_STORAGE_OK) ? 0 : 1;
}
