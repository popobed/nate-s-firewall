/*
 * wasm_gpu_engine.c
 *
 * Voir wasm_gpu_engine.h pour la portee (honest scope) et les garanties.
 * Zero malloc/free dans ce fichier : gpu_vertex_buffer_t et gpu_backend_t
 * sont entierement fournis par l'appelant (stockage statique/global,
 * comme documente dans le header) ; les seules "allocations" sont de vrais
 * objets COM Direct3D12 crees par le pilote GPU (ID3D12Device, etc.),
 * geres par reference-counting (Release()), pas par malloc/free.
 */

#include "wasm_gpu_engine.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Vertex buffer SoA                                                    */
/* ------------------------------------------------------------------ */

void gpu_vertex_buffer_init(gpu_vertex_buffer_t *vb) {
    if (vb == NULL) {
        return;
    }
    memset(vb, 0, sizeof *vb);
}

gpu_status_t gpu_vertex_buffer_push(gpu_vertex_buffer_t *vb,
                                     float x, float y, float z,
                                     float nx, float ny, float nz,
                                     float u, float v,
                                     uint32_t *out_index) {
    if (vb == NULL) {
        return GPU_ERR_INVALID;
    }
    if (vb->count >= GPU_MAX_VERTICES) {
        return GPU_ERR_CAPACITY;
    }
    const uint32_t i = vb->count;
    vb->pos_x[i] = x;
    vb->pos_y[i] = y;
    vb->pos_z[i] = z;
    vb->normal_x[i] = nx;
    vb->normal_y[i] = ny;
    vb->normal_z[i] = nz;
    vb->uv_u[i] = u;
    vb->uv_v[i] = v;
    vb->count = i + 1u;
    if (out_index != NULL) {
        *out_index = i;
    }
    return GPU_OK;
}

/* ------------------------------------------------------------------ */
/* File de commandes (ring_buffer.h reutilise, pas reimplemente)        */
/* ------------------------------------------------------------------ */

gpu_status_t gpu_cmd_queue_push(ring_buffer_t *queue, uint32_t sender_id, const gpu_draw_cmd_t *cmd) {
    if (queue == NULL || cmd == NULL) {
        return GPU_ERR_INVALID;
    }
    rb_status_t st = rb_try_push(queue, sender_id, GPU_CMD_MSG_TYPE, cmd, sizeof *cmd);
    if (st == RB_ERR_FULL) {
        return GPU_ERR_QUEUE_FULL;
    }
    if (st != RB_OK) {
        return GPU_ERR_INVALID;
    }
    return GPU_OK;
}

gpu_status_t gpu_cmd_queue_pop(ring_buffer_t *queue, gpu_draw_cmd_t *out) {
    if (queue == NULL || out == NULL) {
        return GPU_ERR_INVALID;
    }
    ring_msg_t msg;
    rb_status_t st = rb_try_pop(queue, &msg);
    if (st == RB_ERR_EMPTY) {
        return GPU_ERR_QUEUE_EMPTY;
    }
    if (st != RB_OK) {
        return GPU_ERR_INVALID;
    }
    if (msg.type == RING_MSG_TYPE_DISCARD) {
        /* Reservation avortee en amont (voir ring_buffer.h) : rien de reel. */
        return GPU_ERR_QUEUE_EMPTY;
    }
    memcpy(out, msg.payload, sizeof *out);
    return GPU_OK;
}

/* ------------------------------------------------------------------ */
/* Pont Rust/WASM (verio) -- voir wasm_gpu_engine.h pour la portee       */
/* honnete exacte. Stockage statique interne : zero allocation           */
/* dynamique, meme discipline que le reste de ce fichier.                */
/* ------------------------------------------------------------------ */

static gpu_vertex_buffer_t g_wasm_bridge_vb;
static ring_buffer_t       g_wasm_bridge_cmd_queue;

void gpu_wasm_bridge_init(void) {
    gpu_vertex_buffer_init(&g_wasm_bridge_vb);
    rb_init(&g_wasm_bridge_cmd_queue);
}

gpu_status_t gpu_wasm_bridge_push_vertex(float x, float y, float z,
                                          float nx, float ny, float nz,
                                          float u, float v,
                                          uint32_t *out_index) {
    return gpu_vertex_buffer_push(&g_wasm_bridge_vb, x, y, z, nx, ny, nz, u, v, out_index);
}

uint32_t gpu_wasm_bridge_vertex_count(void) {
    return g_wasm_bridge_vb.count;
}

gpu_status_t gpu_wasm_bridge_push_draw_cmd(uint32_t sender_id,
                                            uint32_t vertex_offset,
                                            uint32_t vertex_count,
                                            uint32_t transform_id) {
    gpu_draw_cmd_t cmd;
    cmd.vertex_offset = vertex_offset;
    cmd.vertex_count  = vertex_count;
    cmd.transform_id  = transform_id;
    cmd._reserved     = 0;
    return gpu_cmd_queue_push(&g_wasm_bridge_cmd_queue, sender_id, &cmd);
}

gpu_status_t gpu_wasm_bridge_pop_draw_cmd(uint32_t *out_vertex_offset,
                                           uint32_t *out_vertex_count,
                                           uint32_t *out_transform_id) {
    gpu_draw_cmd_t cmd;
    gpu_status_t st = gpu_cmd_queue_pop(&g_wasm_bridge_cmd_queue, &cmd);
    if (st != GPU_OK) {
        return st;
    }
    if (out_vertex_offset != NULL) {
        *out_vertex_offset = cmd.vertex_offset;
    }
    if (out_vertex_count != NULL) {
        *out_vertex_count = cmd.vertex_count;
    }
    if (out_transform_id != NULL) {
        *out_transform_id = cmd.transform_id;
    }
    return GPU_OK;
}

/* ------------------------------------------------------------------ */
/* Backend Direct3D12 reel -- Windows-only, voir wasm_gpu_engine.h       */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

gpu_status_t gpu_backend_init(gpu_backend_t *backend) {
    if (backend == NULL) {
        return GPU_ERR_INVALID;
    }
    memset(backend, 0, sizeof *backend);

    ID3D12Device *device = NULL;
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr) || device == NULL) {
        /* Pas de pilote d'affichage D3D12 compatible expose a ce
         * processus -- resultat normal en environnement headless/VM sans
         * GPU, voir le header. Pas une erreur de logique de ce module. */
        return GPU_ERR_NO_ADAPTER;
    }

    D3D12_COMMAND_QUEUE_DESC qdesc;
    memset(&qdesc, 0, sizeof qdesc);
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ID3D12CommandQueue *queue = NULL;
    hr = ID3D12Device_CreateCommandQueue(device, &qdesc, &IID_ID3D12CommandQueue, (void **)&queue);
    if (FAILED(hr) || queue == NULL) {
        ID3D12Device_Release(device);
        return GPU_ERR_QUEUE_CREATE_FAILED;
    }

    ID3D12Fence *fence = NULL;
    hr = ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    if (FAILED(hr) || fence == NULL) {
        ID3D12CommandQueue_Release(queue);
        ID3D12Device_Release(device);
        return GPU_ERR_FENCE_CREATE_FAILED;
    }

    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (ev == NULL) {
        ID3D12Fence_Release(fence);
        ID3D12CommandQueue_Release(queue);
        ID3D12Device_Release(device);
        return GPU_ERR_EVENT_CREATE_FAILED;
    }

    backend->device      = device;
    backend->queue       = queue;
    backend->fence       = fence;
    backend->fence_event = ev;
    backend->fence_value = 0;
    backend->initialized = true;
    return GPU_OK;
}

void gpu_backend_shutdown(gpu_backend_t *backend) {
    if (backend == NULL) {
        return;
    }
    if (backend->fence_event != NULL) {
        CloseHandle(backend->fence_event);
    }
    if (backend->fence != NULL) {
        ID3D12Fence_Release(backend->fence);
    }
    if (backend->queue != NULL) {
        ID3D12CommandQueue_Release(backend->queue);
    }
    if (backend->device != NULL) {
        ID3D12Device_Release(backend->device);
    }
    memset(backend, 0, sizeof *backend);
}

gpu_status_t gpu_backend_submit_frame(gpu_backend_t *backend, ring_buffer_t *queue,
                                       uint32_t max_commands,
                                       uint32_t *out_submitted, uint64_t *out_fence_value) {
    if (backend == NULL || queue == NULL) {
        return GPU_ERR_INVALID;
    }

    uint32_t submitted = 0;
    gpu_draw_cmd_t cmd;
    while (submitted < max_commands) {
        gpu_status_t st = gpu_cmd_queue_pop(queue, &cmd);
        if (st == GPU_ERR_QUEUE_EMPTY) {
            break;
        }
        if (st != GPU_OK) {
            return st;
        }
        /* Enregistrer un vrai appel de dessin necessiterait une command
         * allocator/list + PSO + shaders compiles -- hors de portee ici,
         * voir le header. Le travail reel et borne effectue ici est le
         * depilement deterministe, zero allocation, de la file. */
        (void)cmd;
        submitted++;
    }
    if (out_submitted != NULL) {
        *out_submitted = submitted;
    }

    if (!backend->initialized) {
        if (out_fence_value != NULL) {
            *out_fence_value = 0;
        }
        return GPU_OK; /* depilement reel effectue ; pas de peripherique pour synchroniser */
    }

    backend->fence_value++;
    HRESULT hr = ID3D12CommandQueue_Signal(backend->queue, backend->fence, backend->fence_value);
    if (FAILED(hr)) {
        return GPU_ERR_WAIT_FAILED;
    }
    if (ID3D12Fence_GetCompletedValue(backend->fence) < backend->fence_value) {
        hr = ID3D12Fence_SetEventOnCompletion(backend->fence, backend->fence_value, backend->fence_event);
        if (FAILED(hr)) {
            return GPU_ERR_WAIT_FAILED;
        }
        WaitForSingleObject(backend->fence_event, INFINITE);
    }
    if (out_fence_value != NULL) {
        *out_fence_value = backend->fence_value;
    }
    return GPU_OK;
}

#endif /* defined(_WIN32) */
