#include "../../include/capture/nvfbc.h"
#include "../../external/NvFBC.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libavutil/frame.h>

typedef struct {
    gsr_capture_nvfbc_params params;
    void *library;

    NVFBC_SESSION_HANDLE nv_fbc_handle;
    PNVFBCCREATEINSTANCE nv_fbc_create_instance;
    NVFBC_API_FUNCTION_LIST nv_fbc_function_list;
    bool fbc_handle_created;
} gsr_capture_nvfbc;

#if defined(_WIN64) || defined(__LP64__)
typedef unsigned long long CUdeviceptr_v2;
#else
typedef unsigned int CUdeviceptr_v2;
#endif
typedef CUdeviceptr_v2 CUdeviceptr;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

/* Returns 0 on failure */
static uint32_t get_output_id_from_display_name(NVFBC_RANDR_OUTPUT_INFO *outputs, uint32_t num_outputs, const char *display_name) {
    if(!outputs)
        return 0;

    for(uint32_t i = 0; i < num_outputs; ++i) {
        if(strcmp(outputs[i].name, display_name) == 0) 
            return outputs[i].dwId;
    }

    return 0;
}

/* TODO: Test with optimus and open kernel modules */
static bool driver_supports_direct_capture_cursor() {
    FILE *f = fopen("/proc/driver/nvidia/version", "rb");
    if(!f)
        return false;

    char buffer[2048];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[bytes_read] = '\0';

    bool supports_cursor = false;
    const char *p = strstr(buffer, "Kernel Module");
    if(p) {
        p += 13;
        int driver_major_version = 0, driver_minor_version = 0;
        if(sscanf(p, "%d.%d", &driver_major_version, &driver_minor_version) == 2) {
            if(driver_major_version > 515 || (driver_major_version == 515 && driver_minor_version >= 57))
                supports_cursor = true;
        }
    }

    fclose(f);
    return supports_cursor;
}

static bool gsr_capture_nvfbc_load_library(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    dlerror(); /* clear */
    void *lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: failed to load libnvidia-fbc.so.1, error: %s\n", dlerror());
        return false;
    }

    cap_nvfbc->nv_fbc_create_instance = (PNVFBCCREATEINSTANCE)dlsym(lib, "NvFBCCreateInstance");
    if(!cap_nvfbc->nv_fbc_create_instance) {
        fprintf(stderr, "gsr error: unable to resolve symbol 'NvFBCCreateInstance'\n");
        dlclose(lib);
        return false;
    }

    memset(&cap_nvfbc->nv_fbc_function_list, 0, sizeof(cap_nvfbc->nv_fbc_function_list));
    cap_nvfbc->nv_fbc_function_list.dwVersion = NVFBC_VERSION;
    NVFBCSTATUS status = cap_nvfbc->nv_fbc_create_instance(&cap_nvfbc->nv_fbc_function_list);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: failed to create NvFBC instance (status: %d)\n", status);
        dlclose(lib);
        return false;
    }

    cap_nvfbc->library = lib;
    return true;
}

static int gsr_capture_nvfbc_start(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    const uint32_t x = max_int(cap_nvfbc->params.pos.x, 0);
    const uint32_t y = max_int(cap_nvfbc->params.pos.y, 0);
    const uint32_t width = max_int(cap_nvfbc->params.size.x, 0);
    const uint32_t height = max_int(cap_nvfbc->params.size.y, 0);

    if(!cap_nvfbc->library || !cap_nvfbc->params.display_to_capture || cap_nvfbc->fbc_handle_created)
        return -1;

    const bool capture_region = (x > 0 || y > 0 || width > 0 || height > 0);

    NVFBCSTATUS status;
    NVFBC_TRACKING_TYPE tracking_type;
    bool capture_session_created = false;
    uint32_t output_id = 0;
    cap_nvfbc->fbc_handle_created = false;

    NVFBC_CREATE_HANDLE_PARAMS create_params;
    memset(&create_params, 0, sizeof(create_params));
    create_params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
    if(status != NVFBC_SUCCESS) {
        // Reverse engineering for interoperability
        const uint8_t enable_key[] = { 0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6, 0x87, 0x4f, 0x8f, 0x4b, 0xf4, 0x61, 0xf8, 0x56, 0x27, 0xe9 };
        create_params.privateData = enable_key;
        create_params.privateDataSize = 16;

        status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
            return -1;
        }
    }
    cap_nvfbc->fbc_handle_created = true;

    NVFBC_GET_STATUS_PARAMS status_params;
    memset(&status_params, 0, sizeof(status_params));
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCGetStatus(cap_nvfbc->nv_fbc_handle, &status_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    if(status_params.bCanCreateNow == NVFBC_FALSE) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: it's not possible to create a capture session on this system\n");
        goto error_cleanup;
    }

    tracking_type = strcmp(cap_nvfbc->params.display_to_capture, "screen") == 0 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
    if(tracking_type == NVFBC_TRACKING_OUTPUT) {
        if(!status_params.bXRandRAvailable) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the xrandr extension is not available\n");
            goto error_cleanup;
        }

        if(status_params.bInModeset) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the x server is in modeset, unable to record\n");
            goto error_cleanup;
        }

        output_id = get_output_id_from_display_name(status_params.outputs, status_params.dwOutputNum, cap_nvfbc->params.display_to_capture);
        if(output_id == 0) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: display '%s' not found\n", cap_nvfbc->params.display_to_capture);
            goto error_cleanup;
        }
    }

    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
    memset(&create_capture_params, 0, sizeof(create_capture_params));
    create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    create_capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
    create_capture_params.bWithCursor = (!cap_nvfbc->params.direct_capture || driver_supports_direct_capture_cursor()) ? NVFBC_TRUE : NVFBC_FALSE;
    if(capture_region)
        create_capture_params.captureBox = (NVFBC_BOX){ x, y, width, height };
    create_capture_params.eTrackingType = tracking_type;
    create_capture_params.dwSamplingRateMs = 1000u / (uint32_t)cap_nvfbc->params.fps;
    create_capture_params.bAllowDirectCapture = cap_nvfbc->params.direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bPushModel = cap_nvfbc->params.direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    if(tracking_type == NVFBC_TRACKING_OUTPUT)
        create_capture_params.dwOutputId = output_id;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateCaptureSession(cap_nvfbc->nv_fbc_handle, &create_capture_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }
    capture_session_created = true;

    NVFBC_TOCUDA_SETUP_PARAMS setup_params;
    memset(&setup_params, 0, sizeof(setup_params));
    setup_params.dwVersion = NVFBC_TOCUDA_SETUP_PARAMS_VER;
    setup_params.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCToCudaSetUp(cap_nvfbc->nv_fbc_handle, &setup_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    return 0;

    error_cleanup:
    if(cap_nvfbc->fbc_handle_created) {
        if(capture_session_created) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
            memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
            destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);
        }

        NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
        memset(&destroy_params, 0, sizeof(destroy_params));
        destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);
        cap_nvfbc->fbc_handle_created = false;
    }
    output_id = 0;
    return -1;
}

static void gsr_capture_nvfbc_stop(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    /* Intentionally ignore failure on destroy */
    if(!cap_nvfbc->nv_fbc_handle)
        return;

    NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
    memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
    destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
    cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);

    NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
    memset(&destroy_params, 0, sizeof(destroy_params));
    destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
    cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);

    cap_nvfbc->nv_fbc_handle = 0;
}

static int gsr_capture_nvfbc_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    if(!cap_nvfbc->library || !cap_nvfbc->fbc_handle_created)
        return -1;

    CUdeviceptr cu_device_ptr = 0;

    NVFBC_FRAME_GRAB_INFO frame_info;
    memset(&frame_info, 0, sizeof(frame_info));

    NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab_params;
    memset(&grab_params, 0, sizeof(grab_params));
    grab_params.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
    grab_params.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT;/* | NVFBC_TOCUDA_GRAB_FLAGS_FORCE_REFRESH;*/
    grab_params.pFrameGrabInfo = &frame_info;
    grab_params.pCUDADeviceBuffer = &cu_device_ptr;

    NVFBCSTATUS status = cap_nvfbc->nv_fbc_function_list.nvFBCToCudaGrabFrame(cap_nvfbc->nv_fbc_handle, &grab_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_capture failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        return -1;
    }

    /*
        *byte_size = frame_info.dwByteSize;

        TODO: Check bIsNewFrame
        TODO: Check dwWidth and dwHeight and update size in video output in ffmpeg. This can happen when xrandr is used to change monitor resolution
    */

    frame->data[0] = (uint8_t*)cu_device_ptr;
    frame->linesize[0] = frame->width * 4;
    return 0;
}

static void gsr_capture_nvfbc_destroy(gsr_capture *cap) {
    if(cap) {
        gsr_capture_nvfbc *cap_nvfbc = cap->priv;
        gsr_capture_nvfbc_stop(cap);
        if(cap_nvfbc) {
            dlclose(cap_nvfbc->library);
            free((void*)cap_nvfbc->params.display_to_capture);
            free(cap->priv);
            cap->priv = NULL;
        }
        free(cap);
    }
}

gsr_capture* gsr_capture_nvfbc_create(const gsr_capture_nvfbc_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_nvfbc *cap_nvfbc = calloc(1, sizeof(gsr_capture_nvfbc));
    if(!cap_nvfbc) {
        free(cap);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_nvfbc);
        return NULL;
    }

    cap_nvfbc->params = *params;
    cap_nvfbc->params.display_to_capture = display_to_capture;
    cap_nvfbc->params.fps = max_int(cap_nvfbc->params.fps, 1);
    
    *cap = (gsr_capture) {
        .start = gsr_capture_nvfbc_start,
        .stop = gsr_capture_nvfbc_stop,
        .capture = gsr_capture_nvfbc_capture,
        .destroy = gsr_capture_nvfbc_destroy,
        .priv = cap_nvfbc
    };

    if(!gsr_capture_nvfbc_load_library(cap)) {
        gsr_capture_nvfbc_destroy(cap);
        return NULL;
    }

    return cap;
}
