#pragma once

#include "../external/NvFBC.h"
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <string.h>

class NvFBCLibrary {
public:
    ~NvFBCLibrary() {
        if(fbc_handle_created) {
            NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
            memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
            destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
            nv_fbc_function_list.nvFBCDestroyCaptureSession(nv_fbc_handle, &destroy_capture_params);

            NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
            memset(&destroy_params, 0, sizeof(destroy_params));
            destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
            nv_fbc_function_list.nvFBCDestroyHandle(nv_fbc_handle, &destroy_params);
        }

        if(library)
            dlclose(library);
    }

    bool load() {
        if(library)
            return true;

        dlerror(); // clear
        void *lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
        if(!lib) {
            fprintf(stderr, "Error: failed to load libnvidia-fbc.so.1, error: %s\n", dlerror());
            return false;
        }

        nv_fbc_create_instance = (PNVFBCCREATEINSTANCE)dlsym(lib, "NvFBCCreateInstance");
        if(!nv_fbc_create_instance) {
            fprintf(stderr, "Error: unable to resolve symbol 'NvFBCCreateInstance'\n");
            dlclose(lib);
            return false;
        }

        memset(&nv_fbc_function_list, 0, sizeof(nv_fbc_function_list));
        nv_fbc_function_list.dwVersion = NVFBC_VERSION;
        NVFBCSTATUS status = nv_fbc_create_instance(&nv_fbc_function_list);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "Error: failed to create NvFBC instance (status: %d)\n", status);
            dlclose(lib);
            return false;
        }

        library = lib;
        return true;
    }

    // If |display_to_capture| is "screen", then the entire x11 screen is captured (all displays).
    bool create(const char *display_to_capture, uint32_t fps, /*out*/ uint32_t *display_width, /*out*/ uint32_t *display_height, uint32_t x = 0, uint32_t y = 0, uint32_t width = 0, uint32_t height = 0, bool direct_capture = false) {
        if(!library || !display_to_capture || !display_width || !display_height || fbc_handle_created)
            return false;

        this->fps = fps;
        const bool capture_region = (x > 0 || y > 0 || width > 0 || height > 0);

        bool supports_direct_cursor = false;
        int driver_major_version = 0;
        int driver_minor_version = 0;
        if(direct_capture && get_driver_version(&driver_major_version, &driver_minor_version)) {
            fprintf(stderr, "Info: detected nvidia version: %d.%d\n", driver_major_version, driver_minor_version);

            if(version_at_least(driver_major_version, driver_minor_version, 515, 57) && version_less_than(driver_major_version, driver_minor_version, 520, 56)) {
                direct_capture = false;
                fprintf(stderr, "Warning: \"screen-direct\" has temporary been disabled as it causes stuttering with driver versions >= 515.57 and < 520.56. Please update your driver if possible. Capturing \"screen\" instead.\n");
            }

            // TODO:
            // Cursor capture disabled because moving the cursor doesn't update capture rate to monitor hz and instead captures at 10-30 hz
            /*
            if(direct_capture) {
                if(version_at_least(driver_major_version, driver_minor_version, 515, 57))
                    supports_direct_cursor = true;
                else
                    fprintf(stderr, "Info: capturing \"screen-direct\" but driver version appears to be less than 515.57. Disabling capture of cursor. Please update your driver if you want to capture your cursor or record \"screen\" instead.\n");
            }
            */
        }

        NVFBCSTATUS status;
        NVFBC_TRACKING_TYPE tracking_type;
        bool capture_session_created = false;
        uint32_t output_id = 0;
        fbc_handle_created = false;

        NVFBC_CREATE_HANDLE_PARAMS create_params;
        memset(&create_params, 0, sizeof(create_params));
        create_params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;

        status = nv_fbc_function_list.nvFBCCreateHandle(&nv_fbc_handle, &create_params);
        if(status != NVFBC_SUCCESS) {
            // Reverse engineering for interoperability
            const uint8_t enable_key[] = { 0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6, 0x87, 0x4f, 0x8f, 0x4b, 0xf4, 0x61, 0xf8, 0x56, 0x27, 0xe9 };
            create_params.privateData = enable_key;
            create_params.privateDataSize = 16;

            status = nv_fbc_function_list.nvFBCCreateHandle(&nv_fbc_handle, &create_params);
            if(status != NVFBC_SUCCESS) {
                fprintf(stderr, "Error: %s\n", nv_fbc_function_list.nvFBCGetLastErrorStr(nv_fbc_handle));
                return false;
            }
        }
        fbc_handle_created = true;

        NVFBC_GET_STATUS_PARAMS status_params;
        memset(&status_params, 0, sizeof(status_params));
        status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

        status = nv_fbc_function_list.nvFBCGetStatus(nv_fbc_handle, &status_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "Error: %s\n", nv_fbc_function_list.nvFBCGetLastErrorStr(nv_fbc_handle));
            goto error_cleanup;
        }

        if(status_params.bCanCreateNow == NVFBC_FALSE) {
            fprintf(stderr, "Error: it's not possible to create a capture session on this system\n");
            goto error_cleanup;
        }

        tracking_type = strcmp(display_to_capture, "screen") == 0 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
        if(tracking_type == NVFBC_TRACKING_OUTPUT) {
            if(!status_params.bXRandRAvailable) {
                fprintf(stderr, "Error: the xrandr extension is not available\n");
                goto error_cleanup;
            }

            if(status_params.bInModeset) {
                fprintf(stderr, "Error: the x server is in modeset, unable to record\n");
                goto error_cleanup;
            }

            output_id = get_output_id_from_display_name(status_params.outputs, status_params.dwOutputNum, display_to_capture, display_width, display_height);
            if(output_id == 0) {
                fprintf(stderr, "Error: display '%s' not found\n", display_to_capture);
                goto error_cleanup;
            }
        } else {
            *display_width = status_params.screenSize.w;
            *display_height = status_params.screenSize.h;
        }

        NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
        memset(&create_capture_params, 0, sizeof(create_capture_params));
        create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
        create_capture_params.eCaptureType = NVFBC_CAPTURE_SHARED_CUDA;
        create_capture_params.bWithCursor = (!direct_capture || supports_direct_cursor) ? NVFBC_TRUE : NVFBC_FALSE;
        if(capture_region) {
            create_capture_params.captureBox = { x, y, width, height };
            *display_width = width;
            *display_height = height;
        }
        create_capture_params.eTrackingType = tracking_type;
        create_capture_params.dwSamplingRateMs = 1000 / (fps + 1);
        create_capture_params.bAllowDirectCapture = direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
        create_capture_params.bPushModel = direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
        if(tracking_type == NVFBC_TRACKING_OUTPUT)
            create_capture_params.dwOutputId = output_id;

        status = nv_fbc_function_list.nvFBCCreateCaptureSession(nv_fbc_handle, &create_capture_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "Error: %s\n", nv_fbc_function_list.nvFBCGetLastErrorStr(nv_fbc_handle));
            goto error_cleanup;
        }
        capture_session_created = true;

        NVFBC_TOCUDA_SETUP_PARAMS setup_params;
        memset(&setup_params, 0, sizeof(setup_params));
        setup_params.dwVersion = NVFBC_TOCUDA_SETUP_PARAMS_VER;
        setup_params.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

        status = nv_fbc_function_list.nvFBCToCudaSetUp(nv_fbc_handle, &setup_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "Error: %s\n", nv_fbc_function_list.nvFBCGetLastErrorStr(nv_fbc_handle));
            goto error_cleanup;
        }

        return true;

        error_cleanup:
        if(fbc_handle_created) {
            if(capture_session_created) {
                NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
                memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
                destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
                nv_fbc_function_list.nvFBCDestroyCaptureSession(nv_fbc_handle, &destroy_capture_params);
            }

            NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
            memset(&destroy_params, 0, sizeof(destroy_params));
            destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
            nv_fbc_function_list.nvFBCDestroyHandle(nv_fbc_handle, &destroy_params);
            fbc_handle_created = false;
        }
        output_id = 0;
        return false;
    }

    bool capture(/*out*/ void *cu_device_ptr, uint32_t *byte_size) {
        if(!library || !fbc_handle_created || !cu_device_ptr || !byte_size)
            return false;

        NVFBCSTATUS status;
        NVFBC_FRAME_GRAB_INFO frame_info;
        memset(&frame_info, 0, sizeof(frame_info));

        NVFBC_TOCUDA_GRAB_FRAME_PARAMS grab_params;
        memset(&grab_params, 0, sizeof(grab_params));
        grab_params.dwVersion = NVFBC_TOCUDA_GRAB_FRAME_PARAMS_VER;
        grab_params.dwFlags = NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT;// | NVFBC_TOCUDA_GRAB_FLAGS_FORCE_REFRESH;//NVFBC_TOCUDA_GRAB_FLAGS_NOWAIT_IF_NEW_FRAME_READY;
        grab_params.pFrameGrabInfo = &frame_info;
        grab_params.pCUDADeviceBuffer = cu_device_ptr;
        grab_params.dwTimeoutMs = 0;//1000 / (fps + 10);

        status = nv_fbc_function_list.nvFBCToCudaGrabFrame(nv_fbc_handle, &grab_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "Error: capture: %s\n", nv_fbc_function_list.nvFBCGetLastErrorStr(nv_fbc_handle));
            return false;
        }

        *byte_size = frame_info.dwByteSize;
        // TODO: Check bIsNewFrame
        // TODO: Check dwWidth and dwHeight and update size in video output in ffmpeg. This can happen when xrandr is used to change monitor resolution

        return true;
    }
private:
    static char to_upper(char c) {
        if(c >= 'a' && c <= 'z')
            return c - 32;
        else
            return c;
    }

    static bool strcase_equals(const char *str1, const char *str2) {
        for(;;) {
            char c1 = to_upper(*str1);
            char c2 = to_upper(*str2);
            if(c1 != c2)
                return false;
            if(c1 == '\0' || c2 == '\0')
                return true;
            ++str1;
            ++str2;       
        }
    }

    // Returns 0 on failure
    static uint32_t get_output_id_from_display_name(NVFBC_RANDR_OUTPUT_INFO *outputs, uint32_t num_outputs, const char *display_name, uint32_t *display_width, uint32_t *display_height) {
        if(!outputs)
            return 0;

        for(uint32_t i = 0; i < num_outputs; ++i) {
            if(strcase_equals(outputs[i].name, display_name)) {
                *display_width = outputs[i].trackedBox.w;
                *display_height = outputs[i].trackedBox.h;
                return outputs[i].dwId;
            }
        }

        return 0;
    }

    // TODO: Test with optimus and open kernel modules
    static bool get_driver_version(int *major, int *minor) {
        *major = 0;
        *minor = 0;

        FILE *f = fopen("/proc/driver/nvidia/version", "rb");
        if(!f) {
            fprintf(stderr, "Warning: failed to get nvidia driver version (failed to read /proc/driver/nvidia/version)\n");
            return false;
        }

        char buffer[2048];
        size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, f);
        buffer[bytes_read] = '\0';

        bool success = false;
        const char *p = strstr(buffer, "Kernel Module");
        if(p) {
            p += 13;
            int driver_major_version = 0, driver_minor_version = 0;
            if(sscanf(p, "%d.%d", &driver_major_version, &driver_minor_version) == 2) {
                *major = driver_major_version;
                *minor = driver_minor_version;
                success = true;
            }
        }

        if(!success)
            fprintf(stderr, "Warning: failed to get nvidia driver version\n");

        fclose(f);
        return success;
    }

    static bool version_at_least(int major, int minor, int expected_major, int expected_minor) {
        return major > expected_major || (major == expected_major && minor >= expected_minor);
    }

    static bool version_less_than(int major, int minor, int expected_major, int expected_minor) {
        return major < expected_major || (major == expected_major && minor < expected_minor);
    }
private:
    void *library = nullptr;
    PNVFBCCREATEINSTANCE nv_fbc_create_instance = nullptr;
    NVFBC_API_FUNCTION_LIST nv_fbc_function_list;
    NVFBC_SESSION_HANDLE nv_fbc_handle;
    bool fbc_handle_created = false;
    int fps = 0;
};
