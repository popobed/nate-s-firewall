#include "chrono_merkle.h"

#include <string.h>

chrono_merkle_status_t chrono_merkle_compute_root(chrono_block_t *blocks, size_t count,
                                                   uint8_t out_root[SHA256_DIGEST_SIZE]) {
    if (blocks == NULL || out_root == NULL || count == 0u || count > CHRONO_MERKLE_MAX_LEAVES) {
        return CHRONO_MERKLE_ERR_INVALID;
    }

    /* The one working buffer this file ever needs -- see file header's
     * "Zero-malloc, precisely". */
    uint8_t level[CHRONO_MERKLE_MAX_LEAVES][SHA256_DIGEST_SIZE];
    size_t level_len = count;

    for (size_t i = 0; i < count; i++) {
        uint8_t serialized[CHRONO_BLOCK_SERIALIZED_SIZE];
        chrono_block_serialize(&blocks[i], serialized);
        sha256_hash(serialized, sizeof serialized, level[i]);
    }

    while (level_len > 1u) {
        size_t next_len = (level_len + 1u) / 2u; /* ceil division: an odd level has one unpaired last node */

        for (size_t i = 0; i < next_len; i++) {
            size_t left_idx = 2u * i;
            /* Odd node at this level: duplicate the last hash as its own
             * sibling, the standard rule this file's requirement names
             * (see file header for how this differs from RFC 6962). */
            size_t right_idx = (left_idx + 1u < level_len) ? (left_idx + 1u) : left_idx;

            uint8_t pair[SHA256_DIGEST_SIZE * 2u];
            memcpy(pair, level[left_idx], SHA256_DIGEST_SIZE);
            memcpy(pair + SHA256_DIGEST_SIZE, level[right_idx], SHA256_DIGEST_SIZE);

            uint8_t parent[SHA256_DIGEST_SIZE];
            sha256_hash(pair, sizeof pair, parent);

            /* Safe in-place write: i <= left_idx always (i <= 2*i for
             * every i >= 0), so this write never clobbers a pair this
             * same pass has not read yet. */
            memcpy(level[i], parent, SHA256_DIGEST_SIZE);
        }
        level_len = next_len;
    }

    memcpy(out_root, level[0], SHA256_DIGEST_SIZE);
    return CHRONO_MERKLE_OK;
}
