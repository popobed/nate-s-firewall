/*
 * karo_bridge.c
 *
 * See include/karo_bridge.h for the full design and honest-scope notes.
 */

#include "karo_bridge.h"

#if defined(_WIN32)

#include <stdio.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct __attribute__((packed)) {
    uint16_t type;      /* 'BM' */
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;
} karo_bmp_file_header_t;

typedef struct __attribute__((packed)) {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} karo_bmp_info_header_t;

_Static_assert(sizeof(karo_bmp_file_header_t) == 14u, "BMP file header must be 14 bytes");
_Static_assert(sizeof(karo_bmp_info_header_t) == 40u, "BMP info header must be 40 bytes");

static bool karo_bridge_ensure_dir(const char *dir) {
    if (CreateDirectoryA(dir, NULL)) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static karo_bridge_status_t karo_bridge_write_bmp(const char *path, const karo_screen_frame_t *frame,
                                                   const uint8_t *pixels) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return KARO_BRIDGE_ERR_IO;
    }

    uint32_t width = frame->width;
    uint32_t height = frame->height;
    uint32_t row_bytes = frame->stride; /* == width*4, BGRA8 */
    uint32_t image_size = row_bytes * height;

    karo_bmp_file_header_t fh;
    fh.type = 0x4D42u; /* 'BM' */
    fh.size = (uint32_t)(sizeof fh + sizeof(karo_bmp_info_header_t) + image_size);
    fh.reserved1 = 0u;
    fh.reserved2 = 0u;
    fh.offset = (uint32_t)(sizeof fh + sizeof(karo_bmp_info_header_t));

    karo_bmp_info_header_t ih;
    ih.header_size = (uint32_t)sizeof ih;
    ih.width = (int32_t)width;
    ih.height = (int32_t)height; /* positive: bottom-up rows, see below */
    ih.planes = 1u;
    ih.bpp = 32u;
    ih.compression = 0u; /* BI_RGB */
    ih.image_size = image_size;
    ih.x_ppm = 2835;
    ih.y_ppm = 2835;
    ih.colors_used = 0u;
    ih.colors_important = 0u;

    if (fwrite(&fh, sizeof fh, 1, f) != 1u || fwrite(&ih, sizeof ih, 1, f) != 1u) {
        fclose(f);
        return KARO_BRIDGE_ERR_IO;
    }

    /* BMP rows are stored bottom-to-top; `pixels` (from
     * karo_screen_capture_attach()) is top-to-bottom -- write rows in
     * reverse order rather than claim a top-down BMP (negative height),
     * which not every consumer honors. */
    for (uint32_t row = 0u; row < height; row++) {
        const uint8_t *src_row = pixels + (size_t)(height - 1u - row) * row_bytes;
        if (fwrite(src_row, row_bytes, 1, f) != 1u) {
            fclose(f);
            return KARO_BRIDGE_ERR_IO;
        }
    }

    fclose(f);
    return KARO_BRIDGE_OK;
}

static karo_bridge_status_t karo_bridge_write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return KARO_BRIDGE_ERR_IO;
    }
    size_t len = (text != NULL) ? strlen(text) : 0u;
    if (len > 0u && fwrite(text, 1, len, f) != len) {
        fclose(f);
        return KARO_BRIDGE_ERR_IO;
    }
    fclose(f);
    return KARO_BRIDGE_OK;
}

static uint64_t karo_bridge_read_seq(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0u;
    }
    unsigned long long v = 0ull;
    if (fscanf(f, "%llu", &v) != 1) {
        v = 0ull;
    }
    fclose(f);
    return (uint64_t)v;
}

static karo_bridge_status_t karo_bridge_write_seq(const char *path, uint64_t seq) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return KARO_BRIDGE_ERR_IO;
    }
    int n = fprintf(f, "%llu", (unsigned long long)seq);
    fclose(f);
    return (n > 0) ? KARO_BRIDGE_OK : KARO_BRIDGE_ERR_IO;
}

karo_bridge_status_t karo_bridge_write_capture(const char *bridge_dir, const karo_screen_frame_t *frame,
                                                const uint8_t *pixels, const char *system_prompt,
                                                const char *provider_name) {
    if (bridge_dir == NULL || frame == NULL || pixels == NULL) {
        return KARO_BRIDGE_ERR_INVALID;
    }
    if (!karo_bridge_ensure_dir(bridge_dir)) {
        return KARO_BRIDGE_ERR_IO;
    }

    char path[512];
    int n;

    n = snprintf(path, sizeof path, "%s\\capture.bmp", bridge_dir);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return KARO_BRIDGE_ERR_INVALID;
    }
    if (karo_bridge_write_bmp(path, frame, pixels) != KARO_BRIDGE_OK) {
        return KARO_BRIDGE_ERR_IO;
    }

    n = snprintf(path, sizeof path, "%s\\system_prompt.txt", bridge_dir);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return KARO_BRIDGE_ERR_INVALID;
    }
    (void)karo_bridge_write_text_file(path, system_prompt); /* best-effort; a missing prompt is not fatal */

    n = snprintf(path, sizeof path, "%s\\active_provider.txt", bridge_dir);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return KARO_BRIDGE_ERR_INVALID;
    }
    (void)karo_bridge_write_text_file(path, provider_name);

    n = snprintf(path, sizeof path, "%s\\capture_seq.txt", bridge_dir);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return KARO_BRIDGE_ERR_INVALID;
    }
    uint64_t seq = karo_bridge_read_seq(path) + 1u;
    if (karo_bridge_write_seq(path, seq) != KARO_BRIDGE_OK) {
        return KARO_BRIDGE_ERR_IO;
    }

    return KARO_BRIDGE_OK;
}

bool karo_bridge_poll_response(const char *bridge_dir, uint64_t *inout_last_seen_seq,
                                char *out, size_t out_cap) {
    if (bridge_dir == NULL || inout_last_seen_seq == NULL || out == NULL || out_cap == 0u) {
        return false;
    }

    char seq_path[512];
    int n = snprintf(seq_path, sizeof seq_path, "%s\\response_seq.txt", bridge_dir);
    if (n <= 0 || (size_t)n >= sizeof seq_path) {
        return false;
    }
    uint64_t current = karo_bridge_read_seq(seq_path);
    if (current == *inout_last_seen_seq) {
        return false; /* nothing new -- normal outcome */
    }

    char text_path[512];
    n = snprintf(text_path, sizeof text_path, "%s\\response.txt", bridge_dir);
    if (n <= 0 || (size_t)n >= sizeof text_path) {
        return false;
    }
    FILE *f = fopen(text_path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t got = fread(out, 1, out_cap - 1u, f);
    fclose(f);
    out[got] = '\0';

    *inout_last_seen_seq = current;
    return true;
}

#endif /* defined(_WIN32) */
