#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

#include <unistd.h>

#include "../include/sound.hpp"

#define GLX_GLXEXT_PROTOTYPES
#include <GL/glew.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <GLFW/glfw3.h>

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <cudaGL.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

//#include <CL/cl.h>

static char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

static char* av_error_to_string(int err) {
    if(av_strerror(err, av_error_buffer, sizeof(av_error_buffer) < 0))
        strcpy(av_error_buffer, "Unknown error");
    return av_error_buffer;
}

struct ScopedGLXFBConfig {
    ~ScopedGLXFBConfig() {
        if (configs)
            XFree(configs);
    }

    GLXFBConfig *configs = nullptr;
};

struct WindowPixmap {
    WindowPixmap()
        : pixmap(None), glx_pixmap(None), texture_id(0), target_texture_id(0),
          texture_width(0), texture_height(0) {}

    Pixmap pixmap;
    GLXPixmap glx_pixmap;
    GLuint texture_id;
    GLuint target_texture_id;

    GLint texture_width;
    GLint texture_height;
};

static bool x11_supports_composite_named_window_pixmap(Display *dpy) {
    int extension_major;
    int extension_minor;
    if (!XCompositeQueryExtension(dpy, &extension_major, &extension_minor))
        return false;

    int major_version;
    int minor_version;
    return XCompositeQueryVersion(dpy, &major_version, &minor_version) &&
           (major_version > 0 || minor_version >= 2);
}

static int x11_error_handler(Display *dpy, XErrorEvent *ev) {
    char type_str[128];
    XGetErrorText(dpy, ev->type, type_str, sizeof(type_str));

    char major_opcode_str[128];
    XGetErrorText(dpy, ev->type, major_opcode_str, sizeof(major_opcode_str));

    char minor_opcode_str[128];
    XGetErrorText(dpy, ev->type, minor_opcode_str, sizeof(minor_opcode_str));

    fprintf(stderr,
        "X Error of failed request:  %s\n"
        "Major opcode of failed request:  %d (%s)\n"
        "Minor opcode of failed request:  %d (%s)\n"
        "Serial number of failed request:  %d\n",
            type_str,
            ev->request_code, major_opcode_str,
            ev->minor_code, minor_opcode_str);
    return 0;
}

static int x11_io_error_handler(Display *dpy) {
    return 0;
}

static void cleanup_window_pixmap(Display *dpy, WindowPixmap &pixmap) {
    if (pixmap.target_texture_id) {
        glDeleteTextures(1, &pixmap.target_texture_id);
        pixmap.target_texture_id = 0;
    }

    if (pixmap.texture_id) {
        glDeleteTextures(1, &pixmap.texture_id);
        pixmap.texture_id = 0;
        pixmap.texture_width = 0;
        pixmap.texture_height = 0;
    }

    if (pixmap.glx_pixmap) {
        glXDestroyPixmap(dpy, pixmap.glx_pixmap);
        glXReleaseTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT);
        pixmap.glx_pixmap = None;
    }

    if (pixmap.pixmap) {
        XFreePixmap(dpy, pixmap.pixmap);
        pixmap.pixmap = None;
    }
}

static bool recreate_window_pixmap(Display *dpy, Window window_id,
                                   WindowPixmap &pixmap) {
    cleanup_window_pixmap(dpy, pixmap);

    const int pixmap_config[] = {
        GLX_BIND_TO_TEXTURE_RGBA_EXT, True, GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_BIND_TO_MIPMAP_TEXTURE_EXT, True, GLX_DOUBLEBUFFER, False,
        // GLX_Y_INVERTED_EXT, (int)GLX_DONT_CARE,
        None};

    // Note that mipmap is generated even though its not used.
    // glCopyImageSubData fails if the texture doesn't have mipmap.
    const int pixmap_attribs[] = {GLX_TEXTURE_TARGET_EXT,
                                  GLX_TEXTURE_2D_EXT,
                                  GLX_TEXTURE_FORMAT_EXT,
                                  GLX_TEXTURE_FORMAT_RGBA_EXT,
                                  GLX_MIPMAP_TEXTURE_EXT,
                                  1,
                                  None};

    int c;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, 0, pixmap_config, &c);
    if (!configs) {
        fprintf(stderr, "Failed too choose fb config\n");
        return false;
    }
    ScopedGLXFBConfig scoped_configs;
    scoped_configs.configs = configs;

    Pixmap new_window_pixmap = XCompositeNameWindowPixmap(dpy, window_id);
    if (!new_window_pixmap) {
        fprintf(stderr, "Failed to get pixmap for window %ld\n", window_id);
        return false;
    }

    GLXPixmap glx_pixmap =
        glXCreatePixmap(dpy, *configs, new_window_pixmap, pixmap_attribs);
    if (!glx_pixmap) {
        fprintf(stderr, "Failed to create glx pixmap\n");
        XFreePixmap(dpy, new_window_pixmap);
        return false;
    }

    pixmap.pixmap = new_window_pixmap;
    pixmap.glx_pixmap = glx_pixmap;

    //glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &pixmap.texture_id);
    glBindTexture(GL_TEXTURE_2D, pixmap.texture_id);

    // glEnable(GL_BLEND);
    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &pixmap.texture_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &pixmap.texture_height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_NEAREST); // GL_LINEAR );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST); // GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
    //glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    fprintf(stderr, "texture width: %d, height: %d\n", pixmap.texture_width,
           pixmap.texture_height);

    // Generating this second texture is needed because
    // cuGraphicsGLRegisterImage cant be used with the texture that is mapped
    // directly to the pixmap.
    // TODO: Investigate if it's somehow possible to use the pixmap texture
    // directly, this should improve performance since only less image copy is
    // then needed every frame.
    glGenTextures(1, &pixmap.target_texture_id);
    glBindTexture(GL_TEXTURE_2D, pixmap.target_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixmap.texture_width,
                 pixmap.texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);
    int err2 = glGetError();
    fprintf(stderr, "error: %d\n", err2);
    glCopyImageSubData(pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                       pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                       pixmap.texture_width, pixmap.texture_height, 1);
    int err = glGetError();
    fprintf(stderr, "error: %d\n", err);
    // glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    // glGenerateTextureMipmapEXT(glxpixmap, GL_TEXTURE_2D);

    // glGenerateMipmap(GL_TEXTURE_2D);

    // glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    // glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_NEAREST); // GL_LINEAR );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST); // GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return pixmap.texture_id != 0 && pixmap.target_texture_id != 0;
}

std::vector<std::string> get_hardware_acceleration_device_names() {
    int iGpu = 0;
    int nGpu = 0;
    cuDeviceGetCount(&nGpu);
    if (iGpu < 0 || iGpu >= nGpu) {
        fprintf(stderr, "Error: failed...\n");
        return {};
    }

    CUdevice cuDevice = 0;
    cuDeviceGet(&cuDevice, iGpu);
    char deviceName[80];
    cuDeviceGetName(deviceName, sizeof(deviceName), cuDevice);
    fprintf(stderr, "device name: %s\n", deviceName);
    return {deviceName};
}

static void receive_frames(AVCodecContext *av_codec_context, AVStream *stream,
                           AVFormatContext *av_format_context,
						   std::mutex &write_output_mutex) {
    AVPacket av_packet;
    av_init_packet(&av_packet);
    for (;;) {
        av_packet.data = NULL;
        av_packet.size = 0;
        int res = avcodec_receive_packet(av_codec_context, &av_packet);
        if (res == 0) { // we have a packet, send the packet to the muxer
            assert(av_packet.stream_index == stream->id);
            av_packet_rescale_ts(&av_packet, av_codec_context->time_base,
                                 stream->time_base);
            av_packet.stream_index = stream->index;
            // Write the encoded video frame to disk
            // av_write_frame(av_format_context, &av_packet)
            // write(STDOUT_FILENO, av_packet.data, av_packet.size)
			std::lock_guard<std::mutex> lock(write_output_mutex);
            int ret = av_write_frame(av_format_context, &av_packet);
            if(ret < 0) {
                fprintf(stderr, "Error: Failed to write video frame to muxer, reason: %s (%d)\n", av_error_to_string(ret), ret);
            }
            av_packet_unref(&av_packet);
        } else if (res == AVERROR(EAGAIN)) { // we have no packet
                                             // fprintf(stderr, "No packet!\n");
            break;
        } else if (res == AVERROR_EOF) { // this is the end of the stream
            fprintf(stderr, "End of stream!\n");
            break;
        } else {
            fprintf(stderr, "Unexpected error: %d\n", res);
            break;
        }
    }
    //av_packet_unref(&av_packet);
}

static AVStream *add_audio_stream(AVFormatContext *av_format_context, AVCodec **codec,
                            enum AVCodecID codec_id) {
    *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!*codec) {
        fprintf(
            stderr,
            "Error: Could not find aac encoder\n");
        exit(1);
    }

    AVStream *stream = avformat_new_stream(av_format_context, *codec);
    if (!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    fprintf(stderr, "audio stream id: %d\n", stream->id);
    AVCodecContext *codec_context = stream->codec;

    assert((*codec)->type == AVMEDIA_TYPE_AUDIO);
    /*
    codec_context->sample_fmt = (*codec)->sample_fmts
                                    ? (*codec)->sample_fmts[0]
                                    : AV_SAMPLE_FMT_FLTP;
    */
	codec_context->codec_id = AV_CODEC_ID_AAC;
    codec_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    //codec_context->bit_rate = 64000;
    codec_context->sample_rate = 48000;
    codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec_context->channels = 2;

    // Some formats want stream headers to be seperate
    //if (av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
    //    av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return stream;
}

static AVStream *add_video_stream(AVFormatContext *av_format_context, AVCodec **codec,
                            enum AVCodecID codec_id,
                            const WindowPixmap &window_pixmap,
                            int fps) {
    //*codec = avcodec_find_encoder(codec_id);
    *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!*codec) {
        *codec = avcodec_find_encoder_by_name("nvenc_h264");
    }
    if (!*codec) {
        fprintf(
            stderr,
            "Error: Could not find h264_nvenc or nvenc_h264 encoder\n");
        exit(1);
    }

    AVStream *stream = avformat_new_stream(av_format_context, *codec);
    if (!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    fprintf(stderr, "video stream id: %d\n", stream->id);
    AVCodecContext *codec_context = stream->codec;

    assert((*codec)->type == AVMEDIA_TYPE_VIDEO);
    codec_context->codec_id = (*codec)->id;
    fprintf(stderr, "codec id: %d\n", (*codec)->id);
    codec_context->width = window_pixmap.texture_width & ~1;
    codec_context->height = window_pixmap.texture_height & ~1;
	codec_context->bit_rate = codec_context->width * codec_context->height; //5000000;
    // Timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1
    codec_context->time_base.num = 1;
    codec_context->time_base.den = fps;
    // codec_context->framerate.num = 60;
    // codec_context->framerate.den = 1;
    codec_context->sample_aspect_ratio.num = 1;
    codec_context->sample_aspect_ratio.den = 1;
    codec_context->gop_size =
        32; // Emit one intra frame every 32 frames at most
    codec_context->pix_fmt = AV_PIX_FMT_CUDA;
    if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
        codec_context->mb_decision = 2;

    // stream->time_base = codec_context->time_base;
    // codec_context->ticks_per_frame = 30;

    // Some formats want stream headers to be seperate
    if (av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return stream;
}

static AVFrame* open_audio(AVCodec *codec, AVStream *stream) {
    int ret;
    AVCodecContext *codec_context = stream->codec;

    ret = avcodec_open2(codec_context, codec, nullptr);
    if(ret < 0) {
        fprintf(stderr, "failed to open codec, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "failed to allocate audio frame\n");
        exit(1);
    }

    frame->nb_samples = codec_context->frame_size;
    frame->format = codec_context->sample_fmt;
	frame->channels = codec_context->channels;
    frame->channel_layout = codec_context->channel_layout;

    ret = av_frame_get_buffer(frame, 0);
    if(ret < 0) {
        fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    return frame;
}

static void open_video(AVCodec *codec, AVStream *stream,
                       WindowPixmap &window_pixmap, AVBufferRef **device_ctx,
                       CUgraphicsResource *cuda_graphics_resource) {
    int ret;
    AVCodecContext *codec_context = stream->codec;

    std::vector<std::string> hardware_accelerated_devices =
        get_hardware_acceleration_device_names();
    if (hardware_accelerated_devices.empty()) {
        fprintf(
            stderr,
            "Error: No hardware accelerated device was found on your system\n");
        exit(1);
    }

    if (av_hwdevice_ctx_create(device_ctx, AV_HWDEVICE_TYPE_CUDA,
                               hardware_accelerated_devices[0].c_str(), NULL,
                               0) < 0) {
        fprintf(stderr,
                "Error: Failed to create hardware device context for gpu: %s\n",
                hardware_accelerated_devices[0].c_str());
        exit(1);
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(*device_ctx);
    if (!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        exit(1);
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = codec_context->width;
    hw_frame_context->height = codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_0BGR32;
    hw_frame_context->format = codec_context->pix_fmt;
    hw_frame_context->device_ref = *device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext *)(*device_ctx)->data;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0\n");
        exit(1);
    }

    codec_context->hw_device_ctx = *device_ctx;
    codec_context->hw_frames_ctx = frame_context;

    ret = avcodec_open2(codec_context, codec, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n",
                "blabla"); // av_err2str(ret));
        exit(1);
    }

    AVHWDeviceContext *hw_device_context =
        (AVHWDeviceContext *)(*device_ctx)->data;
    AVCUDADeviceContext *cuda_device_context =
        (AVCUDADeviceContext *)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if (!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        exit(1);
    }

    CUresult res;
    CUcontext old_ctx;
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);
    res = cuGraphicsGLRegisterImage(
        cuda_graphics_resource, window_pixmap.target_texture_id, GL_TEXTURE_2D,
        CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    // cuGraphicsUnregisterResource(*cuda_graphics_resource);
    if (res != CUDA_SUCCESS) {
        const char *err_str;
        cuGetErrorString(res, &err_str);
        fprintf(stderr,
                "Error: cuGraphicsGLRegisterImage failed, error %s, texture "
                "id: %u\n",
                err_str, window_pixmap.target_texture_id);
        exit(1);
    }
    res = cuCtxPopCurrent(&old_ctx);
}

static void close_video(AVStream *video_stream, AVFrame *frame) {
    // avcodec_close(video_stream->codec);
    // av_frame_free(&frame);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: gpu-screen-recorder <window_id> <container_format> <fps>\n");
        return 1;
    }

    Window src_window_id = strtol(argv[1], nullptr, 0);
    const char *container_format = argv[2];
    int fps = atoi(argv[3]);
    if(fps <= 0 || fps > 255) {
        fprintf(stderr, "invalid fps argument: %s\n", argv[3]);
        return 1;
    }

    const char *filename = "/dev/stdout";

    const double target_fps = 1.0f / (double)fps;

    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, "Error: Failed to open display\n");
        return 1;
    }

    bool has_name_pixmap = x11_supports_composite_named_window_pixmap(dpy);
    if (!has_name_pixmap) {
        fprintf(stderr, "Error: XCompositeNameWindowPixmap is not supported by "
                        "your X11 server\n");
        return 1;
    }

    XCompositeRedirectWindow(dpy, src_window_id, CompositeRedirectAutomatic);

    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, src_window_id, &attr)) {
        fprintf(stderr, "Error: Invalid window id: %lu\n", src_window_id);
        return 1;
    }

    // glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read,
    // GLXContext ctx)
    if (!glfwInit()) {
        fprintf(stderr, "Error: Failed to initialize glfw\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);

    GLFWwindow *window =
        glfwCreateWindow(1280, 720, "Hello world", nullptr, nullptr);
    if (!window) {
        fprintf(stderr, "Error: Failed to create glfw window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

#if defined(DEBUG)
    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);
#endif

    glewExperimental = GL_TRUE;
    GLenum nGlewError = glewInit();
    if (nGlewError != GLEW_OK) {
        fprintf(stderr, "%s - Error initializing GLEW! %s\n", __FUNCTION__,
                glewGetErrorString(nGlewError));
        return 1;
    }
    glGetError(); // to clear the error caused deep in GLEW

    WindowPixmap window_pixmap;
    if (!recreate_window_pixmap(dpy, src_window_id, window_pixmap)) {
        fprintf(stderr, "Error: Failed to create glx pixmap for window: %lu\n",
                src_window_id);
        return 1;
    }

    // Video start
    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, container_format,
                                   nullptr);
    if (!av_format_context) {
        fprintf(
            stderr,
            "Error: Failed to deduce output format from file extension\n");
        return 1;
    }

    AVOutputFormat *output_format = av_format_context->oformat;

    AVCodec *video_codec;
    AVStream *video_stream =
        add_video_stream(av_format_context, &video_codec, output_format->video_codec,
                   window_pixmap, fps);
    if (!video_stream) {
        fprintf(stderr, "Error: Failed to create video stream\n");
        return 1;
    }

    AVCodec *audio_codec;
    AVStream *audio_stream =
        add_audio_stream(av_format_context, &audio_codec, output_format->audio_codec);
    if (!audio_stream) {
        fprintf(stderr, "Error: Failed to create audio stream\n");
        return 1;
    }

    if (cuInit(0) < 0) {
        fprintf(stderr, "Error: cuInit failed\n");
        return {};
    }

    AVBufferRef *device_ctx;
    CUgraphicsResource cuda_graphics_resource;
    open_video(video_codec, video_stream, window_pixmap, &device_ctx,
               &cuda_graphics_resource);

    AVFrame *audio_frame = open_audio(audio_codec, audio_stream);

    //av_dump_format(av_format_context, 0, filename, 1);

    if (!(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename,
                    "blabla"); // av_err2str(ret));
            return 1;
        }
    }

    int ret = avformat_write_header(av_format_context, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                "blabla"); // av_err2str(ret));
        return 1;
    }

    AVHWDeviceContext *hw_device_context =
        (AVHWDeviceContext *)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context =
        (AVCUDADeviceContext *)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if (!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        exit(1);
    }

    // av_frame_free(&rgb_frame);
    // avcodec_close(av_codec_context);

    XSelectInput(dpy, src_window_id, StructureNotifyMask);

    int damage_event;
    int damage_error;
    if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        fprintf(stderr, "Error: XDamage is not supported by your X11 server\n");
        return 1;
    }

    Damage xdamage = XDamageCreate(dpy, src_window_id, XDamageReportNonEmpty);

    int frame_count = 0;

    CUresult res;
    CUcontext old_ctx;
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);

    // Get texture
    res = cuGraphicsResourceSetMapFlags(
        cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

    // Map texture to cuda array
    CUarray mapped_array;
    res = cuGraphicsSubResourceGetMappedArray(&mapped_array,
                                              cuda_graphics_resource, 0, 0);

    // Release texture
    // res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

    double start_time = glfwGetTime();
    double frame_timer_start = start_time;
    double window_resize_timer = start_time;
    bool window_resized = true;
    int fps_counter = 0;
    int current_fps = 30;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error: Failed to allocate frame\n");
        exit(1);
    }
    frame->format = video_stream->codec->pix_fmt;
    frame->width = video_stream->codec->width;
    frame->height = video_stream->codec->height;

    if (av_hwframe_get_buffer(video_stream->codec->hw_frames_ctx, frame, 0) < 0) {
        fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
        exit(1);
    }

    XWindowAttributes xwa;
    XGetWindowAttributes(dpy, src_window_id, &xwa);
    int window_width = xwa.width;
    int window_height = xwa.height;

    SoundDevice sound_device;
    if(sound_device_get_by_name(&sound_device, "pulse", audio_stream->codec->channels, audio_stream->codec->frame_size) != 0) {
        fprintf(stderr, "failed to get 'pulse' sound device\n");
        exit(1);
    }

	int audio_buffer_size = av_samples_get_buffer_size(NULL, audio_stream->codec->channels, audio_stream->codec->frame_size, audio_stream->codec->sample_fmt, 1);
	uint8_t *audio_frame_buf = (uint8_t *)av_malloc(audio_buffer_size);
	avcodec_fill_audio_frame(audio_frame, audio_stream->codec->channels, audio_stream->codec->sample_fmt, (const uint8_t*)audio_frame_buf, audio_buffer_size, 1);

	AVPacket audio_packet;
	av_new_packet(&audio_packet, audio_buffer_size);

	std::mutex write_output_mutex;

	bool running = true;
	std::thread audio_thread([&running](AVFormatContext *av_format_context, AVStream *audio_stream, AVPacket *audio_packet, uint8_t *audio_frame_buf, SoundDevice *sound_device, AVFrame *audio_frame, std::mutex *write_output_mutex) {
		SwrContext *swr = swr_alloc();
		if(!swr) {
			fprintf(stderr, "Failed to create SwrContext\n");
			exit(1);
		}
		av_opt_set_int(swr, "in_channel_layout", audio_stream->codec->channel_layout, 0);
		av_opt_set_int(swr, "out_channel_layout", audio_stream->codec->channel_layout, 0);
		av_opt_set_int(swr, "in_sample_rate", audio_stream->codec->sample_rate, 0);
		av_opt_set_int(swr, "out_sample_rate", audio_stream->codec->sample_rate, 0);
		av_opt_set_sample_fmt(swr, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
		av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
		swr_init(swr);

		while(running) {
			void *sound_buffer;
			int sound_buffer_size = sound_device_read_next_chunk(sound_device, &sound_buffer);
			if(sound_buffer_size >= 0) {
				// TODO: Instead of converting audio, get float audio from alsa. Or does alsa do conversion internally to get this format?
				swr_convert(swr, &audio_frame_buf, audio_frame->nb_samples, (const uint8_t**)&sound_buffer, sound_buffer_size);
				audio_frame->extended_data = &audio_frame_buf;
				// TODO: Fix this. Warning from ffmpeg:
				// Timestamps are unset in a packet for stream 1. This is deprecated and will stop working in the future. Fix your code to set the timestamps properly
				//audio_frame->pts=audio_frame_index*100;
				//++audio_frame_index;

				int got_frame = 0;
				int ret = avcodec_encode_audio2(audio_stream->codec, audio_packet, audio_frame, &got_frame);
				if(ret < 0){
					printf("Failed to encode!\n");
					break;
				}
				if (got_frame==1){
					//printf("Succeed to encode 1 frame! \tsize:%5d\n",pkt.size);
					audio_packet->stream_index = audio_stream->index;
					std::lock_guard<std::mutex> lock(*write_output_mutex);
					ret = av_write_frame(av_format_context, audio_packet);
					av_free_packet(audio_packet);
				}
			} else {
				fprintf(stderr, "failed to read sound from device, error: %d\n", sound_buffer_size);
			}
		}

		swr_free(&swr);
	}, av_format_context, audio_stream, &audio_packet, audio_frame_buf, &sound_device, audio_frame, &write_output_mutex);

    XEvent e;
    while (!glfwWindowShouldClose(window)) {
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();

        if (XCheckTypedWindowEvent(dpy, src_window_id, ConfigureNotify, &e) && e.xconfigure.window == src_window_id) {
            // Window resize
            if(e.xconfigure.width != window_width || e.xconfigure.height != window_height) {
                window_width = e.xconfigure.width;
                window_height = e.xconfigure.height;
                window_resize_timer = glfwGetTime();
                window_resized = false;
            }
        }

        if (XCheckTypedWindowEvent(dpy, src_window_id, damage_event + XDamageNotify, &e)) {
            // fprintf(stderr, "Redraw!\n");
            XDamageNotifyEvent *de = (XDamageNotifyEvent *)&e;
            // de->drawable is the window ID of the damaged window
            XserverRegion region = XFixesCreateRegion(dpy, nullptr, 0);
            // Subtract all the damage, repairing the window
            XDamageSubtract(dpy, de->damage, None, region);
            XFixesDestroyRegion(dpy, region);

            // TODO: Use a framebuffer instead. glCopyImageSubData requires
            // opengl 4.2
            glCopyImageSubData(
                window_pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                window_pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                window_pixmap.texture_width, window_pixmap.texture_height, 1);
            // int err = glGetError();
            // fprintf(stderr, "error: %d\n", err);

            CUDA_MEMCPY2D memcpy_struct;
            memcpy_struct.srcXInBytes = 0;
            memcpy_struct.srcY = 0;
            memcpy_struct.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_ARRAY;

            memcpy_struct.dstXInBytes = 0;
            memcpy_struct.dstY = 0;
            memcpy_struct.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;

            memcpy_struct.srcArray = mapped_array;
            memcpy_struct.dstDevice = (CUdeviceptr)frame->data[0];
            memcpy_struct.dstPitch = frame->linesize[0];
            memcpy_struct.WidthInBytes = frame->width * 4;
            memcpy_struct.Height = frame->height;
            cuMemcpy2D(&memcpy_struct);
            // res = cuCtxPopCurrent(&old_ctx);
        }

        ++fps_counter;

        double time_now = glfwGetTime();
        double frame_timer_elapsed = time_now - frame_timer_start;
        double elapsed = time_now - start_time;
        if (elapsed >= 1.0) {
            fprintf(stderr, "fps: %d\n", fps_counter);
            start_time = time_now;
            current_fps = fps_counter;
            fps_counter = 0;
        }

        const double window_resize_timeout = 1.0; // 1 second
        if(!window_resized && time_now - window_resize_timer >= window_resize_timeout) {
            window_resized = true;
            fprintf(stderr, "Resize window!\n");
            recreate_window_pixmap(dpy, src_window_id, window_pixmap);
            // Resolution must be a multiple of two
            video_stream->codec->width = window_pixmap.texture_width & ~1;
            video_stream->codec->height = window_pixmap.texture_height & ~1;

            cuGraphicsUnregisterResource(cuda_graphics_resource);
            res = cuGraphicsGLRegisterImage(
                &cuda_graphics_resource, window_pixmap.target_texture_id, GL_TEXTURE_2D,
                CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
            if (res != CUDA_SUCCESS) {
                const char *err_str;
                cuGetErrorString(res, &err_str);
                fprintf(stderr,
                        "Error: cuGraphicsGLRegisterImage failed, error %s, texture "
                        "id: %u\n",
                        err_str, window_pixmap.target_texture_id);
                break;
            }

            res = cuGraphicsResourceSetMapFlags(
                cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
            res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);
            res = cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

            av_frame_unref(frame);
            if (av_hwframe_get_buffer(video_stream->codec->hw_frames_ctx, frame, 0) < 0) {
                fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
                break;
            }
        }

        double frame_time_overflow = frame_timer_elapsed - target_fps;
        if (frame_time_overflow >= 0.0) {
            frame_timer_start = time_now - frame_time_overflow;
            frame->pts = frame_count;
            frame_count += 1;
            if (avcodec_send_frame(video_stream->codec, frame) >= 0) {
                receive_frames(video_stream->codec, video_stream,
                               av_format_context, write_output_mutex);
            } else {
                fprintf(stderr, "Error: avcodec_send_frame failed\n");
            }
        }

        // av_frame_free(&frame);

        usleep(5000);
    }

	running = false;
	audio_thread.join();

    sound_device_close(&sound_device);

	//Flush Encoder
	#if 0
	ret = flush_encoder(pFormatCtx,0);
	if (ret < 0) {
		printf("Flushing encoder failed\n");
		return -1;
	}
	#endif

    if (av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    /* add sequence end code to have a real MPEG file */
    /*
    const uint8_t endcode[] = { 0, 0, 1, 0xb7 };
    if (video_codec->id == AV_CODEC_ID_MPEG1VIDEO || video_codec->id == AV_CODEC_ID_MPEG2VIDEO)
        write(STDOUT_FILENO, endcode, sizeof(endcode));
    */

    // close_video(video_stream, NULL);

     if(!(output_format->flags & AVFMT_NOFILE))
        avio_close(av_format_context->pb);
    // avformat_free_context(av_format_context);
    // XDamageDestroy(dpy, xdamage);

    // cleanup_window_pixmap(dpy, window_pixmap);
    XCompositeUnredirectWindow(dpy, src_window_id, CompositeRedirectAutomatic);
    XCloseDisplay(dpy);
}
