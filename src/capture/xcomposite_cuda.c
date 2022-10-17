#include "../../include/capture/xcomposite_cuda.h"
#include "../../include/egl.h"
#include "../../include/cuda.h"
#include "../../include/window_texture.h"
#include "../../include/time.h"
#include <X11/extensions/Xcomposite.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_xcomposite_cuda_params params;
    Display *dpy;
    XEvent xev;
    bool should_stop;
    bool stop_is_error;
    bool window_resized;
    bool created_hw_frame;
    double window_resize_timer;

    vec2i window_size;
    vec2i window_pos;

    unsigned int target_texture_id;
    vec2i texture_size;
    Window composite_window;
    WindowTexture window_texture;

    CUgraphicsResource cuda_graphics_resource;
    CUarray mapped_array;

    gsr_egl egl;
    gsr_cuda cuda;
} gsr_capture_xcomposite_cuda;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static void gsr_capture_xcomposite_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static bool cuda_register_opengl_texture(gsr_capture_xcomposite_cuda *cap_xcomp) {
    CUresult res;
    CUcontext old_ctx;
    res = cap_xcomp->cuda.cuCtxPushCurrent_v2(cap_xcomp->cuda.cu_ctx);
    res = cap_xcomp->cuda.cuGraphicsGLRegisterImage(
        &cap_xcomp->cuda_graphics_resource, cap_xcomp->target_texture_id, GL_TEXTURE_2D,
        CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if (res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        cap_xcomp->cuda.cuGetErrorString(res, &err_str);
        fprintf(stderr,
                "Error: cuGraphicsGLRegisterImage failed, error %s, texture "
                "id: %u\n",
                err_str, cap_xcomp->target_texture_id);
        return false;
    }

    res = cap_xcomp->cuda.cuGraphicsResourceSetMapFlags(cap_xcomp->cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cap_xcomp->cuda.cuGraphicsMapResources(1, &cap_xcomp->cuda_graphics_resource, 0);

    res = cap_xcomp->cuda.cuGraphicsSubResourceGetMappedArray(&cap_xcomp->mapped_array, cap_xcomp->cuda_graphics_resource, 0, 0);
    res = cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
    return true;
}

static bool cuda_create_codec_context(gsr_capture_xcomposite_cuda *cap_xcomp, AVCodecContext *video_codec_context) {
    CUcontext old_ctx;
    cap_xcomp->cuda.cuCtxPushCurrent_v2(cap_xcomp->cuda.cu_ctx);

    AVBufferRef *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!device_ctx) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cap_xcomp->cuda.cu_ctx;
    if(av_hwdevice_ctx_init(device_ctx) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        av_buffer_unref(&device_ctx);
        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_0RGB32;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        av_buffer_unref(&frame_context);
        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
        return false;
    }

    video_codec_context->hw_device_ctx = device_ctx;
    video_codec_context->hw_frames_ctx = frame_context;
    return true;
}

static unsigned int gl_create_texture(gsr_capture_xcomposite_cuda *cap_xcomp, int width, int height) {
    // Generating this second texture is needed because
    // cuGraphicsGLRegisterImage cant be used with the texture that is mapped
    // directly to the pixmap.
    // TODO: Investigate if it's somehow possible to use the pixmap texture
    // directly, this should improve performance since only less image copy is
    // then needed every frame.
    // Ignoring failure for now.. TODO: Show proper error
    unsigned int texture_id = 0;
    cap_xcomp->egl.glGenTextures(1, &texture_id);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, texture_id);
    cap_xcomp->egl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

static int gsr_capture_xcomposite_cuda_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    XWindowAttributes attr;
    if(!XGetWindowAttributes(cap_xcomp->dpy, cap_xcomp->params.window, &attr)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_start failed: invalid window id: %lu\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->window_size.x = max_int(attr.width, 0);
    cap_xcomp->window_size.y = max_int(attr.height, 0);
    Window c;
    XTranslateCoordinates(cap_xcomp->dpy, cap_xcomp->params.window, DefaultRootWindow(cap_xcomp->dpy), 0, 0, &cap_xcomp->window_pos.x, &cap_xcomp->window_pos.y, &c);

    XSelectInput(cap_xcomp->dpy, cap_xcomp->params.window, StructureNotifyMask | ExposureMask);

    if(!gsr_egl_load(&cap_xcomp->egl, cap_xcomp->dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_start: failed to load opengl\n");
        return -1;
    }

    cap_xcomp->egl.eglSwapInterval(cap_xcomp->egl.egl_display, 0);
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->egl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    cap_xcomp->target_texture_id = gl_create_texture(cap_xcomp, cap_xcomp->texture_size.x, cap_xcomp->texture_size.y);
    if(cap_xcomp->target_texture_id == 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_start: failed to create opengl texture\n");
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    if(!gsr_cuda_load(&cap_xcomp->cuda)) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    if(!cuda_create_codec_context(cap_xcomp, video_codec_context)) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    if(!cuda_register_opengl_texture(cap_xcomp)) {
        gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
        return -1;
    }

    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
}

static void gsr_capture_xcomposite_cuda_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    window_texture_deinit(&cap_xcomp->window_texture);

    if(cap_xcomp->target_texture_id) {
        cap_xcomp->egl.glDeleteTextures(1, &cap_xcomp->target_texture_id);
        cap_xcomp->target_texture_id = 0;
    }

    if(cap_xcomp->composite_window) {
        XCompositeUnredirectWindow(cap_xcomp->dpy, cap_xcomp->composite_window, CompositeRedirectAutomatic);
        cap_xcomp->composite_window = None;
    }

    av_buffer_unref(&video_codec_context->hw_device_ctx);
    av_buffer_unref(&video_codec_context->hw_frames_ctx);

    if(cap_xcomp->cuda.cu_ctx) {
        CUcontext old_ctx;
        cap_xcomp->cuda.cuCtxPushCurrent_v2(cap_xcomp->cuda.cu_ctx);

        cap_xcomp->cuda.cuGraphicsUnmapResources(1, &cap_xcomp->cuda_graphics_resource, 0);
        cap_xcomp->cuda.cuGraphicsUnregisterResource(cap_xcomp->cuda_graphics_resource);
        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
    gsr_cuda_unload(&cap_xcomp->cuda);

    gsr_egl_unload(&cap_xcomp->egl);
    if(cap_xcomp->dpy) {
        // TODO: Why is this crashing?
        XCloseDisplay(cap_xcomp->dpy);
        cap_xcomp->dpy = NULL;
    }
}

static void gsr_capture_xcomposite_cuda_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

    if(!cap_xcomp->created_hw_frame) {
        cap_xcomp->created_hw_frame = true;
        CUcontext old_ctx;
        cap_xcomp->cuda.cuCtxPushCurrent_v2(cap_xcomp->cuda.cu_ctx);

        if(av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0) < 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_tick: av_hwframe_get_buffer failed\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
            return;
        }

        cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }

    if(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->params.window, DestroyNotify, &cap_xcomp->xev)) {
        cap_xcomp->should_stop = true;
        cap_xcomp->stop_is_error = false;
    }

    if(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->params.window, Expose, &cap_xcomp->xev) && cap_xcomp->xev.xexpose.count == 0) {
        cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
        cap_xcomp->window_resized = true;
    }

    if(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->params.window, ConfigureNotify, &cap_xcomp->xev) && cap_xcomp->xev.xconfigure.window == cap_xcomp->params.window) {
        while(XCheckTypedWindowEvent(cap_xcomp->dpy, cap_xcomp->params.window, ConfigureNotify, &cap_xcomp->xev)) {}
        Window c;
        XTranslateCoordinates(cap_xcomp->dpy, cap_xcomp->params.window, DefaultRootWindow(cap_xcomp->dpy), 0, 0, &cap_xcomp->xev.xconfigure.x, &cap_xcomp->xev.xconfigure.y, &c);
        cap_xcomp->window_pos.x = cap_xcomp->xev.xconfigure.x;
        cap_xcomp->window_pos.y = cap_xcomp->xev.xconfigure.y;

        /* Window resize */
        if(cap_xcomp->xev.xconfigure.width != cap_xcomp->window_size.x || cap_xcomp->xev.xconfigure.height != cap_xcomp->window_size.y) {
            cap_xcomp->window_size.x = max_int(cap_xcomp->xev.xconfigure.width, 0);
            cap_xcomp->window_size.y = max_int(cap_xcomp->xev.xconfigure.height, 0);
            cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
            cap_xcomp->window_resized = true;
        }
    }

    const double window_resize_timeout = 1.0; // 1 second
    if(cap_xcomp->window_resized && clock_get_monotonic_seconds() - cap_xcomp->window_resize_timer >= window_resize_timeout) {
        cap_xcomp->window_resized = false;
        fprintf(stderr, "Resize window!\n");
        if(window_texture_on_resize(&cap_xcomp->window_texture) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_tick: window_texture_on_resize failed\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            return;
        }

        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
        cap_xcomp->texture_size.x = 0;
        cap_xcomp->texture_size.y = 0;
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

        cap_xcomp->texture_size.x = min_int(video_codec_context->width, max_int(2, cap_xcomp->texture_size.x & ~1));
        cap_xcomp->texture_size.y = min_int(video_codec_context->height, max_int(2, cap_xcomp->texture_size.y & ~1));

        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, cap_xcomp->target_texture_id);
        cap_xcomp->egl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, cap_xcomp->texture_size.x, cap_xcomp->texture_size.y, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

        CUcontext old_ctx;
        CUresult res = cap_xcomp->cuda.cuCtxPushCurrent_v2(cap_xcomp->cuda.cu_ctx);

        cap_xcomp->cuda.cuGraphicsUnmapResources(1, &cap_xcomp->cuda_graphics_resource, 0);
        cap_xcomp->cuda.cuGraphicsUnregisterResource(cap_xcomp->cuda_graphics_resource);
        res = cap_xcomp->cuda.cuGraphicsGLRegisterImage(&cap_xcomp->cuda_graphics_resource, cap_xcomp->target_texture_id, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
        if (res != CUDA_SUCCESS) {
            const char *err_str = "unknown";
            cap_xcomp->cuda.cuGetErrorString(res, &err_str);
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_tick: cuGraphicsGLRegisterImage failed, error %s, texture id: %u\n", err_str, cap_xcomp->target_texture_id);
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            res = cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
            return;
        }

        res = cap_xcomp->cuda.cuGraphicsResourceSetMapFlags(cap_xcomp->cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
        res = cap_xcomp->cuda.cuGraphicsMapResources(1, &cap_xcomp->cuda_graphics_resource, 0);
        res = cap_xcomp->cuda.cuGraphicsSubResourceGetMappedArray(&cap_xcomp->mapped_array, cap_xcomp->cuda_graphics_resource, 0, 0);

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_tick: failed to allocate frame\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            res = cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = AVCOL_RANGE_JPEG;

        if(av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0) < 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_tick: av_hwframe_get_buffer failed\n");
            cap_xcomp->should_stop = true;
            cap_xcomp->stop_is_error = true;
            res = cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
            return;
        }

        // Make it completely black to clear unused parts
        // TODO: cuMemsetD32?
        res = cap_xcomp->cuda.cuMemsetD8_v2((CUdeviceptr)(*frame)->data[0], 0, (*frame)->width * (*frame)->height * 4);
        res = cap_xcomp->cuda.cuCtxPopCurrent_v2(&old_ctx);
    }
}

static bool gsr_capture_xcomposite_cuda_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;
    if(cap_xcomp->should_stop) {
        if(err)
            *err = cap_xcomp->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_xcomposite_cuda_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_cuda *cap_xcomp = cap->priv;

    // TODO: Use a framebuffer instead. glCopyImageSubData requires opengl 4.2
    vec2i source_pos = { 0, 0 };
    vec2i source_size = cap_xcomp->texture_size;

    // Requires opengl 4.2... TODO: Replace with earlier opengl if opengl < 4.2.
    cap_xcomp->egl.glCopyImageSubData(
        window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), GL_TEXTURE_2D, 0, source_pos.x, source_pos.y, 0,
        cap_xcomp->target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
        source_size.x, source_size.y, 1);
    unsigned int err = cap_xcomp->egl.glGetError();
    if(err != 0) {
        static bool error_shown = false;
        if(!error_shown) {
            error_shown = true;
            fprintf(stderr, "Error: glCopyImageSubData failed, gl error: %d\n", err);
        }
    }
    cap_xcomp->egl.eglSwapBuffers(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_surface);
    // TODO: Remove this copy, which is only possible by using nvenc directly and encoding window_pixmap.target_texture_id

    frame->linesize[0] = frame->width * 4;

    CUDA_MEMCPY2D memcpy_struct;
    memcpy_struct.srcXInBytes = 0;
    memcpy_struct.srcY = 0;
    memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

    memcpy_struct.dstXInBytes = 0;
    memcpy_struct.dstY = 0;
    memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

    memcpy_struct.srcArray = cap_xcomp->mapped_array;
    memcpy_struct.dstDevice = (CUdeviceptr)frame->data[0];
    memcpy_struct.dstPitch = frame->linesize[0];
    memcpy_struct.WidthInBytes = frame->width * 4;
    memcpy_struct.Height = frame->height;
    cap_xcomp->cuda.cuMemcpy2D_v2(&memcpy_struct);

    return 0;
}

static void gsr_capture_xcomposite_cuda_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_cuda_stop(cap, video_codec_context);
    if(cap->priv) {
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_cuda_create(const gsr_capture_xcomposite_cuda_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_cuda *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_cuda));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_cuda_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_xcomp);
        return NULL;
    }

    cap_xcomp->dpy = display;
    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_cuda_start,
        .tick = gsr_capture_xcomposite_cuda_tick,
        .should_stop = gsr_capture_xcomposite_cuda_should_stop,
        .capture = gsr_capture_xcomposite_cuda_capture,
        .destroy = gsr_capture_xcomposite_cuda_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
