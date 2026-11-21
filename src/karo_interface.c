/*
 * karo_interface.c
 *
 * See include/karo_interface.h for the full design and honest-scope
 * notes. Implementation only; non-obvious decisions are explained there,
 * not duplicated here.
 */

#include "karo_interface.h"

#include <string.h>

/* ==================================================================== */
/* Part A -- Gemini API key pool, lock-free CAS rotation                 */
/* ==================================================================== */

void karo_key_pool_init(karo_key_pool_t *pool) {
    for (uint32_t i = 0; i < KARO_KEY_POOL_SIZE; i++) {
        memset(pool->slots[i].key, 0, sizeof pool->slots[i].key);
        pool->slots[i].len = 0u;
        pool->slots[i].provisioned = false;
    }
    atomic_store_explicit(&pool->active_index, 0u, memory_order_relaxed);
    atomic_store_explicit(&pool->rotations_total, 0u, memory_order_relaxed);
}

karo_status_t karo_key_pool_provision(karo_key_pool_t *pool, uint32_t slot_index, const char *key, uint32_t len) {
    if (pool == NULL || key == NULL || len == 0u || len > KARO_KEY_MAX_LEN) {
        return KARO_ERR_INVALID;
    }
    if (slot_index >= KARO_KEY_POOL_SIZE) {
        return KARO_ERR_OUT_OF_RANGE;
    }
    if (pool->slots[slot_index].provisioned) {
        return KARO_ERR_ALREADY_PROVISIONED;
    }
    memcpy(pool->slots[slot_index].key, key, len);
    if (len < KARO_KEY_MAX_LEN) {
        memset(pool->slots[slot_index].key + len, 0, KARO_KEY_MAX_LEN - len);
    }
    pool->slots[slot_index].len = len;
    pool->slots[slot_index].provisioned = true;
    return KARO_OK;
}

karo_status_t karo_key_pool_active_key(const karo_key_pool_t *pool, char *out_key, uint32_t *inout_len) {
    if (pool == NULL || out_key == NULL || inout_len == NULL) {
        return KARO_ERR_INVALID;
    }
    uint32_t idx = atomic_load_explicit(&pool->active_index, memory_order_acquire);
    const karo_key_slot_t *slot = &pool->slots[idx];
    if (!slot->provisioned) {
        return KARO_ERR_NOT_FOUND;
    }
    if (slot->len > *inout_len) {
        return KARO_ERR_INVALID; /* caller's out_key buffer too small */
    }
    memcpy(out_key, slot->key, slot->len);
    *inout_len = slot->len;
    return KARO_OK;
}

karo_status_t karo_rotate_key_cas(karo_key_pool_t *pool, uint32_t http_status) {
    if (pool == NULL) {
        return KARO_ERR_INVALID;
    }
    if (http_status != KARO_HTTP_STATUS_QUOTA_EXCEEDED) {
        return KARO_OK; /* no-op: nothing to rotate away from */
    }

    uint32_t old_idx = atomic_load_explicit(&pool->active_index, memory_order_acquire);
    for (uint32_t retries = 0u; retries < KARO_KEY_POOL_SIZE; retries++) {
        uint32_t candidate = KARO_KEY_POOL_SIZE; /* sentinel: none found this scan */
        for (uint32_t step = 1u; step <= KARO_KEY_POOL_SIZE; step++) {
            uint32_t idx = (old_idx + step) % KARO_KEY_POOL_SIZE;
            if (pool->slots[idx].provisioned) {
                candidate = idx;
                break;
            }
        }
        if (candidate == KARO_KEY_POOL_SIZE) {
            return KARO_ERR_FULL; /* no provisioned slot exists anywhere in the pool */
        }

        uint32_t expected = old_idx;
        if (atomic_compare_exchange_strong_explicit(&pool->active_index, &expected, candidate,
                                                      memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&pool->rotations_total, 1u, memory_order_relaxed);
            return KARO_OK;
        }
        old_idx = expected; /* CAS failure refreshed `expected` to the current value -- retry the scan from there */
    }
    return KARO_ERR_FULL; /* bounded retries exhausted under extreme contention -- documented, never a hang */
}

/* ==================================================================== */
/* Part B -- Owner-exclusive constant-time gate + audit trail            */
/* ==================================================================== */

karo_status_t karo_verify_owner(uint32_t provided_owner_id, tf_log_t *access_log) {
    int match = ct_equal_u32(provided_owner_id, (uint32_t)KARO_OWNER_ID);

    if (access_log != NULL) {
        uint32_t event = match ? KARO_TF_EVENT_OWNER_AUTH_OK : KARO_TF_EVENT_OWNER_AUTH_DENIED;
        (void)tf_log_append(access_log, event, &provided_owner_id, (uint32_t)sizeof provided_owner_id);
    }

    return match ? KARO_OK : KARO_ERR_UNAUTHORIZED;
}

/* ==================================================================== */
/* Part C -- Audio ingestion bridge onto audio_transcribe.c              */
/* ==================================================================== */

karo_status_t karo_audio_ingest_pcm(audio_pool_t *pool, uint32_t frame_index,
                                     actor_t *inference_actor, uint16_t tenant_id,
                                     audio_feature_vec_t *out_features) {
    if (pool == NULL || inference_actor == NULL || out_features == NULL) {
        return KARO_ERR_INVALID;
    }

    if (audio_fir_filter_frame(pool, frame_index) != AUDIO_OK) {
        return KARO_ERR_INVALID;
    }

    const audio_sample_t *filtered = audio_frame_output(pool, frame_index);
    if (filtered == NULL) {
        return KARO_ERR_INVALID;
    }

    if (audio_extract_features(filtered, out_features, AUDIO_FIR_AUTO) != AUDIO_OK) {
        return KARO_ERR_INVALID;
    }
    if (audio_actor_store_features(inference_actor, out_features) != ACTOR_OK) {
        return KARO_ERR_INVALID;
    }
    if (audio_emit_frame_index(inference_actor, tenant_id, frame_index) != ACTOR_OK) {
        return KARO_ERR_INVALID;
    }
    return KARO_OK;
}

/* ==================================================================== */
/* Part D -- Translucent overlay SoA over the existing D3D12 fence backend */
/* ==================================================================== */

void karo_overlay_init(karo_overlay_t *overlay) {
    memset(overlay->pos_x, 0, sizeof overlay->pos_x);
    memset(overlay->pos_y, 0, sizeof overlay->pos_y);
    memset(overlay->opacity, 0, sizeof overlay->opacity);
    memset(overlay->vertex_offset, 0, sizeof overlay->vertex_offset);
    overlay->count = 0u;
}

karo_status_t karo_overlay_push(karo_overlay_t *overlay, float x, float y, float opacity,
                                 uint32_t vertex_offset, uint32_t *out_index) {
    if (overlay == NULL) {
        return KARO_ERR_INVALID;
    }
    if (overlay->count >= KARO_OVERLAY_MAX_ELEMENTS) {
        return KARO_ERR_FULL;
    }
    uint32_t idx = overlay->count;
    overlay->pos_x[idx] = x;
    overlay->pos_y[idx] = y;
    overlay->opacity[idx] = opacity;
    overlay->vertex_offset[idx] = vertex_offset;
    overlay->count = idx + 1u;
    if (out_index != NULL) {
        *out_index = idx;
    }
    return KARO_OK;
}

#if defined(_WIN32)
gpu_status_t karo_overlay_submit(karo_overlay_t *overlay, gpu_backend_t *backend, ring_buffer_t *queue,
                                  uint32_t *out_submitted, uint64_t *out_fence_value) {
    if (overlay == NULL || backend == NULL || queue == NULL) {
        return GPU_ERR_INVALID;
    }

    for (uint32_t i = 0; i < overlay->count; i++) {
        gpu_draw_cmd_t cmd;
        cmd.vertex_offset = overlay->vertex_offset[i];
        cmd.vertex_count = 1u; /* one overlay element == one staged vertex, see file header on opacity[] */
        cmd.transform_id = i;
        cmd._reserved = 0u;
        gpu_status_t st = gpu_cmd_queue_push(queue, i, &cmd);
        if (st != GPU_OK) {
            return st; /* GPU_ERR_QUEUE_FULL: backpressure, propagated as-is, no partial silent drop */
        }
    }

    return gpu_backend_submit_frame(backend, queue, overlay->count, out_submitted, out_fence_value);
}
#endif
