/*
 * karo_screen_capture.c
 *
 * See include/karo_screen_capture.h for the full design and honest-scope
 * notes. Real D3D11 Desktop Duplication, one-shot per call, Windows-only.
 */

#include "karo_screen_capture.h"

#if defined(_WIN32)

#define COBJMACROS
#define INITGUID
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <string.h>

karo_screen_status_t karo_screen_capture_attach(uint8_t *out_buf, size_t out_cap,
                                                 uint32_t timeout_ms, karo_screen_frame_t *out_frame) {
    if (out_frame == NULL || (out_buf == NULL && out_cap != 0u)) {
        return KARO_SCREEN_ERR_INVALID;
    }
    memset(out_frame, 0, sizeof *out_frame);

    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                    D3D11_SDK_VERSION, &device, NULL, &context);
    if (FAILED(hr) || device == NULL) {
        return KARO_SCREEN_ERR_NO_DEVICE;
    }

    IDXGIDevice *dxgi_device = NULL;
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (FAILED(hr) || dxgi_device == NULL) {
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_NO_DEVICE;
    }

    IDXGIAdapter *adapter = NULL;
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr) || adapter == NULL) {
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_NO_OUTPUT;
    }

    IDXGIOutput *output = NULL;
    hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
    IDXGIAdapter_Release(adapter);
    if (FAILED(hr) || output == NULL) {
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_NO_OUTPUT;
    }

    IDXGIOutput1 *output1 = NULL;
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    IDXGIOutput_Release(output);
    if (FAILED(hr) || output1 == NULL) {
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_NO_OUTPUT;
    }

    IDXGIOutputDuplication *dupl = NULL;
    hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &dupl);
    IDXGIOutput1_Release(output1);
    if (FAILED(hr) || dupl == NULL) {
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_UNSUPPORTED;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    memset(&frame_info, 0, sizeof frame_info);
    IDXGIResource *desktop_resource = NULL;
    hr = IDXGIOutputDuplication_AcquireNextFrame(dupl, timeout_ms, &frame_info, &desktop_resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        IDXGIOutputDuplication_Release(dupl);
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_TIMEOUT;
    }
    if (FAILED(hr) || desktop_resource == NULL) {
        IDXGIOutputDuplication_Release(dupl);
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_ACQUIRE_FAILED;
    }

    ID3D11Texture2D *acquired_tex = NULL;
    hr = IDXGIResource_QueryInterface(desktop_resource, &IID_ID3D11Texture2D, (void **)&acquired_tex);
    IDXGIResource_Release(desktop_resource);
    if (FAILED(hr) || acquired_tex == NULL) {
        IDXGIOutputDuplication_ReleaseFrame(dupl);
        IDXGIOutputDuplication_Release(dupl);
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_ACQUIRE_FAILED;
    }

    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(acquired_tex, &desc);

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0u;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0u;

    ID3D11Texture2D *staging_tex = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &staging_desc, NULL, &staging_tex);
    if (FAILED(hr) || staging_tex == NULL) {
        ID3D11Texture2D_Release(acquired_tex);
        IDXGIOutputDuplication_ReleaseFrame(dupl);
        IDXGIOutputDuplication_Release(dupl);
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_ACQUIRE_FAILED;
    }

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging_tex, (ID3D11Resource *)acquired_tex);
    ID3D11Texture2D_Release(acquired_tex);
    IDXGIOutputDuplication_ReleaseFrame(dupl); /* source texture no longer needed past the copy */

    D3D11_MAPPED_SUBRESOURCE mapped;
    memset(&mapped, 0, sizeof mapped);
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging_tex, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(staging_tex);
        IDXGIOutputDuplication_Release(dupl);
        if (context != NULL) { ID3D11DeviceContext_Release(context); }
        ID3D11Device_Release(device);
        return KARO_SCREEN_ERR_MAP_FAILED;
    }

    uint32_t width = desc.Width;
    uint32_t height = desc.Height;
    uint32_t row_bytes = width * 4u; /* BGRA8 */
    uint64_t required = (uint64_t)row_bytes * (uint64_t)height;

    out_frame->width = width;
    out_frame->height = height;
    out_frame->stride = row_bytes;

    karo_screen_status_t result;
    if (required > (uint64_t)out_cap) {
        out_frame->bytes_written = (uint32_t)(required > 0xFFFFFFFFu ? 0xFFFFFFFFu : required);
        result = KARO_SCREEN_ERR_BUFFER_TOO_SMALL;
    } else {
        const uint8_t *src = (const uint8_t *)mapped.pData;
        uint8_t *dst = out_buf;
        for (uint32_t row = 0u; row < height; row++) {
            memcpy(dst, src, row_bytes);
            src += mapped.RowPitch;
            dst += row_bytes;
        }
        out_frame->bytes_written = (uint32_t)required;
        result = KARO_SCREEN_OK;
    }

    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging_tex, 0);
    ID3D11Texture2D_Release(staging_tex);
    IDXGIOutputDuplication_Release(dupl);
    if (context != NULL) {
        ID3D11DeviceContext_Release(context);
    }
    ID3D11Device_Release(device);
    return result;
}

#endif /* defined(_WIN32) */
