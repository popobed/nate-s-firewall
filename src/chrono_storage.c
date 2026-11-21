#include "chrono_storage.h"

#include <stdio.h>
#include <string.h>
#include <windows.h> /* CreateFileA/WriteFile/CloseHandle -- see chrono_storage.h's "Append-only, precisely" */

static void chrono_storage_build_header(uint32_t block_count, uint64_t window_timestamp_ns,
                                         const uint8_t merkle_root[SHA256_DIGEST_SIZE],
                                         uint8_t out[CHRONO_STORAGE_WINDOW_HEADER_SIZE]) {
    uint32_t magic = CHRONO_STORAGE_MAGIC;
    size_t off = 0u;

    memcpy(out + off, &magic, sizeof magic); off += sizeof magic;
    memcpy(out + off, &block_count, sizeof block_count); off += sizeof block_count;
    memcpy(out + off, &window_timestamp_ns, sizeof window_timestamp_ns); off += sizeof window_timestamp_ns;
    memcpy(out + off, merkle_root, SHA256_DIGEST_SIZE); off += SHA256_DIGEST_SIZE;
}

chrono_storage_status_t chrono_storage_flush_window(const char *filepath, chrono_block_t *blocks, size_t count,
                                                      uint8_t merkle_root[SHA256_DIGEST_SIZE]) {
    if (filepath == NULL || blocks == NULL || merkle_root == NULL ||
        count == 0u || count > CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW) {
        return CHRONO_STORAGE_ERR_INVALID;
    }

    /* FILE_APPEND_DATA, not fopen("ab"): a kernel-enforced atomic
     * "always write at the current end of file" guarantee -- see file
     * header. OPEN_ALWAYS: create on first flush, append thereafter. */
    HANDLE h = CreateFileA(filepath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return CHRONO_STORAGE_ERR_IO;
    }

    uint8_t header[CHRONO_STORAGE_WINDOW_HEADER_SIZE];
    chrono_storage_build_header((uint32_t)count, chrono_wall_clock_ns(), merkle_root, header);

    DWORD written = 0;
    if (!WriteFile(h, header, (DWORD)sizeof header, &written, NULL) || written != sizeof header) {
        CloseHandle(h);
        return CHRONO_STORAGE_ERR_IO;
    }

    for (size_t i = 0; i < count; i++) {
        uint8_t serialized[CHRONO_BLOCK_SERIALIZED_SIZE];
        chrono_block_serialize(&blocks[i], serialized);

        written = 0;
        if (!WriteFile(h, serialized, (DWORD)sizeof serialized, &written, NULL) || written != sizeof serialized) {
            CloseHandle(h);
            return CHRONO_STORAGE_ERR_IO; /* already-written bytes stay on disk -- see file header */
        }
    }

    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        return CHRONO_STORAGE_ERR_IO;
    }
    CloseHandle(h);
    return CHRONO_STORAGE_OK;
}

chrono_storage_status_t chrono_storage_verify_file(const char *filepath) {
    if (filepath == NULL) {
        return CHRONO_STORAGE_ERR_INVALID;
    }

    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        return CHRONO_STORAGE_ERR_IO;
    }

    /* Carried CONTINUOUSLY across window boundaries -- see file header:
     * windows are a storage/Merkle-batching unit only, never a break in
     * the underlying hash chain's own continuity. */
    uint8_t expected_prev[SHA256_DIGEST_SIZE];
    memcpy(expected_prev, CHRONO_GENESIS_HASH, SHA256_DIGEST_SIZE);

    /* Bounded, streaming: at most CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW
     * blocks live at once, reused window to window -- see file header. */
    chrono_block_t window_blocks[CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW];

    for (;;) {
        uint8_t header[CHRONO_STORAGE_WINDOW_HEADER_SIZE];
        size_t n = fread(header, 1, sizeof header, f);
        if (n == 0u) {
            break; /* clean EOF exactly at a window boundary -- done, every window checked out */
        }
        if (n != sizeof header) {
            fclose(f);
            return CHRONO_STORAGE_ERR_TRUNCATED;
        }

        uint32_t magic;
        uint32_t block_count;
        uint8_t stored_root[SHA256_DIGEST_SIZE];
        size_t off = 0u;
        memcpy(&magic, header + off, sizeof magic); off += sizeof magic;
        memcpy(&block_count, header + off, sizeof block_count); off += sizeof block_count;
        off += 8u; /* window_timestamp_ns: read into the file format, not independently verifiable -- see file header */
        memcpy(stored_root, header + off, SHA256_DIGEST_SIZE);

        if (magic != CHRONO_STORAGE_MAGIC) {
            fclose(f);
            return CHRONO_STORAGE_ERR_TAMPERED;
        }
        if (block_count == 0u || block_count > CHRONO_STORAGE_MAX_BLOCKS_PER_WINDOW) {
            fclose(f);
            return CHRONO_STORAGE_ERR_TAMPERED;
        }

        for (uint32_t i = 0; i < block_count; i++) {
            uint8_t serialized[CHRONO_BLOCK_SERIALIZED_SIZE];
            n = fread(serialized, 1, sizeof serialized, f);
            if (n != sizeof serialized) {
                fclose(f);
                return CHRONO_STORAGE_ERR_TRUNCATED;
            }
            chrono_block_deserialize(serialized, &window_blocks[i]);
        }

        uint8_t recomputed_root[SHA256_DIGEST_SIZE];
        if (chrono_merkle_compute_root(window_blocks, (size_t)block_count, recomputed_root) != CHRONO_MERKLE_OK) {
            fclose(f);
            return CHRONO_STORAGE_ERR_TAMPERED;
        }
        if (memcmp(recomputed_root, stored_root, SHA256_DIGEST_SIZE) != 0) {
            fclose(f);
            return CHRONO_STORAGE_ERR_TAMPERED;
        }

        uint64_t broken_index = 0u;
        if (chrono_ingest_verify_block_chain(window_blocks, (size_t)block_count, expected_prev, &broken_index)
            != CHRONO_INGEST_OK) {
            fclose(f);
            return CHRONO_STORAGE_ERR_TAMPERED;
        }

        memcpy(expected_prev, window_blocks[block_count - 1u].block_hash, SHA256_DIGEST_SIZE);
    }

    fclose(f);
    return CHRONO_STORAGE_OK;
}
