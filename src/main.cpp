/*
    Copyright (C) 2020 dec05eba

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <signal.h>
#include <sys/stat.h>

#include <unistd.h>
#include <fcntl.h>

#include "../include/sound.hpp"

#define GLX_GLXEXT_PROTOTYPES
#include <GL/glew.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <GLFW/glfw3.h>

#include <X11/extensions/Xcomposite.h>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
}
#include <cudaGL.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "../include/NvFBCLibrary.hpp"

#include <deque>
#include <future>

//#include <CL/cl.h>

static const int VIDEO_STREAM_INDEX = 0;
static const int AUDIO_STREAM_INDEX = 1;

static thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

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

enum class VideoQuality {
    MEDIUM,
    HIGH,
    ULTRA
};

static double clock_get_monotonic_seconds() {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

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
#if 0
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
#endif
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

    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, window_id, &attr)) {
        fprintf(stderr, "Failed to get window attributes\n");
        return false;
    }

    const int pixmap_config[] = {
        GLX_BIND_TO_TEXTURE_RGB_EXT, True,
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT | GLX_WINDOW_BIT,
        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        GLX_BUFFER_SIZE, 24,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 0,
        // GLX_Y_INVERTED_EXT, (int)GLX_DONT_CARE,
        None};

    const int pixmap_attribs[] = {GLX_TEXTURE_TARGET_EXT,
                                  GLX_TEXTURE_2D_EXT,
                                  GLX_TEXTURE_FORMAT_EXT,
                                  GLX_TEXTURE_FORMAT_RGB_EXT,
                                  None};

    int c;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, 0, pixmap_config, &c);
    if (!configs) {
        fprintf(stderr, "Failed too choose fb config\n");
        return false;
    }
    ScopedGLXFBConfig scoped_configs;
    scoped_configs.configs = configs;

    bool found = false;
    GLXFBConfig config;
    for (int i = 0; i < c; i++) {
        config = configs[i];
        XVisualInfo *visual = glXGetVisualFromFBConfig(dpy, config);
        if (!visual)
            continue;

        if (attr.depth != visual->depth) {
            XFree(visual);
            continue;
        }
        XFree(visual);
        found = true;
        break;
    }

    if(!found) {
        fprintf(stderr, "No matching fb config found\n");
        return false;
    }

    Pixmap new_window_pixmap = XCompositeNameWindowPixmap(dpy, window_id);
    if (!new_window_pixmap) {
        fprintf(stderr, "Failed to get pixmap for window %ld\n", window_id);
        return false;
    }

    GLXPixmap glx_pixmap =
        glXCreatePixmap(dpy, config, new_window_pixmap, pixmap_attribs);
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_NEAREST); // GL_LINEAR );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST); // GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
    //glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,
                             &pixmap.texture_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT,
                             &pixmap.texture_height);

    if(pixmap.texture_width == 0 || pixmap.texture_height == 0) {
        pixmap.texture_width = attr.width;
        pixmap.texture_height = attr.height;
        fprintf(stderr, "Warning: failed to get texture size. You are probably running an unsupported compositor and recording the selected window doesn't work at the moment. This could also happen if you are trying to record a window with client-side decorations (GNOME issue). A black window will be displayed instead. A workaround is to record the whole monitor (which use NvFBC).\n");
    }

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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pixmap.texture_width,
                 pixmap.texture_height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    int err2 = glGetError();
    fprintf(stderr, "error: %d\n", err2);
    // glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    // glGenerateTextureMipmapEXT(glxpixmap, GL_TEXTURE_2D);

    // glGenerateMipmap(GL_TEXTURE_2D);

    // glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    // glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_NEAREST); // GL_LINEAR );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_NEAREST); // GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
    //glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return pixmap.texture_id != 0 && pixmap.target_texture_id != 0;
}

// |stream| is only required for non-replay mode
static void receive_frames(AVCodecContext *av_codec_context, int stream_index, AVStream *stream, AVFrame *frame,
                           AVFormatContext *av_format_context,
                           double replay_start_time,
                           std::deque<AVPacket> &frame_data_queue,
                           int replay_buffer_size_secs,
                           bool &frames_erased,
						   std::mutex &write_output_mutex) {
    AVPacket av_packet;
    memset(&av_packet, 0, sizeof(av_packet));
    for (;;) {
        av_packet.data = NULL;
        av_packet.size = 0;
        int res = avcodec_receive_packet(av_codec_context, &av_packet);
        if (res == 0) { // we have a packet, send the packet to the muxer
            av_packet.stream_index = stream_index;
            av_packet.pts = av_packet.dts = frame->pts;

			std::lock_guard<std::mutex> lock(write_output_mutex);
            if(replay_buffer_size_secs != -1) {
                double time_now = glfwGetTime();
                double replay_time_elapsed = time_now - replay_start_time;

                AVPacket new_pack;
                av_packet_move_ref(&new_pack, &av_packet);
                frame_data_queue.push_back(std::move(new_pack));
                if(replay_time_elapsed >= replay_buffer_size_secs) {
                    av_packet_unref(&frame_data_queue.front());
                    frame_data_queue.pop_front();
                    frames_erased = true;
                }
            } else {
                av_packet_rescale_ts(&av_packet, av_codec_context->time_base, stream->time_base);
                av_packet.stream_index = stream->index;
                int ret = av_interleaved_write_frame(av_format_context, &av_packet);
                if(ret < 0) {
                    fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet.stream_index, av_error_to_string(ret), ret);
                }
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

static AVCodecContext* create_audio_codec_context(AVFormatContext *av_format_context, int fps) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        fprintf(
            stderr,
            "Error: Could not find aac encoder\n");
        exit(1);
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    assert(codec->type == AVMEDIA_TYPE_AUDIO);
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

    codec_context->time_base.num = 1;
    codec_context->time_base.den = AV_TIME_BASE;
    codec_context->framerate.num = 1;
    codec_context->framerate.den = fps;

    // Some formats want stream headers to be seperate
    if (av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static AVCodecContext *create_video_codec_context(AVFormatContext *av_format_context, 
                            VideoQuality video_quality,
                            int record_width, int record_height,
                            int fps, bool use_hevc) {
    const AVCodec *codec = avcodec_find_encoder_by_name(use_hevc ? "hevc_nvenc" : "h264_nvenc");
    if (!codec) {
        codec = avcodec_find_encoder_by_name(use_hevc ? "nvenc_hevc" : "nvenc_h264");
    }
    if (!codec) {
        fprintf(
            stderr,
            "Error: Could not find %s encoder\n", use_hevc ? "hevc" : "h264");
        exit(1);
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    //double fps_ratio = (double)fps / 30.0;

    assert(codec->type == AVMEDIA_TYPE_VIDEO);
    codec_context->codec_id = codec->id;
    codec_context->width = record_width & ~1;
    codec_context->height = record_height & ~1;
	codec_context->bit_rate = 7500000 + (codec_context->width * codec_context->height) / 2;
    // Timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1
    codec_context->time_base.num = 1;
    codec_context->time_base.den = AV_TIME_BASE;
    codec_context->framerate.num = 1;
    codec_context->framerate.den = fps;
    codec_context->sample_aspect_ratio.num = 0;
    codec_context->sample_aspect_ratio.den = 0;
    codec_context->gop_size = fps * 2;
    codec_context->max_b_frames = use_hevc ? 0 : 2;
    codec_context->pix_fmt = AV_PIX_FMT_CUDA;
    codec_context->color_range = AVCOL_RANGE_JPEG;
    switch(video_quality) {
        case VideoQuality::MEDIUM:
	        codec_context->bit_rate = 5000000 + (codec_context->width * codec_context->height) / 2;
            codec_context->qmin = 30;
            codec_context->qmax = 51;
            //av_opt_set(codec_context->priv_data, "preset", "slow", 0);
            //av_opt_set(codec_context->priv_data, "profile", "high", 0);
            //codec_context->profile = FF_PROFILE_H264_HIGH;
            //av_opt_set(codec_context->priv_data, "preset", "p4", 0);
            break;
        case VideoQuality::HIGH:
            codec_context->qmin = 20;
            codec_context->qmax = 40;
            //av_opt_set(codec_context->priv_data, "preset", "slow", 0);
            //av_opt_set(codec_context->priv_data, "profile", "high", 0);
            //codec_context->profile = FF_PROFILE_H264_HIGH;
            //av_opt_set(codec_context->priv_data, "preset", "p5", 0);
            break;
        case VideoQuality::ULTRA:
	        codec_context->bit_rate = 10000000 + (codec_context->width * codec_context->height) / 2;
            codec_context->qmin = 16;
            codec_context->qmax = 30;
            //av_opt_set(codec_context->priv_data, "preset", "veryslow", 0);
            //av_opt_set(codec_context->priv_data, "profile", "high", 0);
            //codec_context->profile = FF_PROFILE_H264_HIGH;
            //av_opt_set(codec_context->priv_data, "preset", "p7", 0);
            break;
    }
    if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
        codec_context->mb_decision = 2;

    // stream->time_base = codec_context->time_base;
    // codec_context->ticks_per_frame = 30;
    //av_opt_set(codec_context->priv_data, "tune", "hq", 0);
    //av_opt_set(codec_context->priv_data, "rc", "vbr", 0);

    // Some formats want stream headers to be seperate
    if (av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static AVFrame* open_audio(AVCodecContext *audio_codec_context) {
    int ret;
    ret = avcodec_open2(audio_codec_context, audio_codec_context->codec, nullptr);
    if(ret < 0) {
        fprintf(stderr, "failed to open codec, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "failed to allocate audio frame\n");
        exit(1);
    }

    frame->nb_samples = audio_codec_context->frame_size;
    frame->format = audio_codec_context->sample_fmt;
	frame->channels = audio_codec_context->channels;
    frame->channel_layout = audio_codec_context->channel_layout;

    ret = av_frame_get_buffer(frame, 0);
    if(ret < 0) {
        fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n", av_error_to_string(ret));
        exit(1);
    }

    return frame;
}

static void open_video(AVCodecContext *codec_context,
                       WindowPixmap &window_pixmap, AVBufferRef **device_ctx,
                       CUgraphicsResource *cuda_graphics_resource, CUcontext cuda_context) {
    int ret;

    *device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!*device_ctx) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        exit(1);
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext *)(*device_ctx)->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext *)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = cuda_context;
    if(av_hwdevice_ctx_init(*device_ctx) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
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
    hw_frame_context->sw_format = AV_PIX_FMT_0RGB32;
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

    ret = avcodec_open2(codec_context, codec_context->codec, nullptr);
    if (ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n",
                "blabla"); // av_err2str(ret));
        exit(1);
    }

    if(window_pixmap.target_texture_id != 0) {
        CUresult res;
        CUcontext old_ctx;
        res = cuCtxPopCurrent(&old_ctx);
        res = cuCtxPushCurrent(cuda_context);
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
}

static void close_video(AVStream *video_stream, AVFrame *frame) {
    // avcodec_close(video_stream->codec);
    // av_frame_free(&frame);
}

static void usage() {
    fprintf(stderr, "usage: gpu-screen-recorder -w <window_id> -c <container_format> -f <fps> [-a <audio_input>] [-q <quality>] [-r <replay_buffer_size_sec>] [-o <output_file>]\n");
    fprintf(stderr, "OPTIONS:\n");
    fprintf(stderr, "  -w    Window to record or a display, \"screen\" or \"screen-direct\". The display is the display name in xrandr and if \"screen\" or \"screen-direct\" is selected then all displays are recorded and they are recorded in h265 (aka hevc)."
        "\"screen-direct\" skips one texture copy for fullscreen applications so it may lead to better performance and it works with VRR monitors when recording fullscreen application but may break some applications, such as mpv in fullscreen mode. Recording a display requires a gpu with NvFBC support.\n");
    fprintf(stderr, "  -s    The size (area) to record at in the format WxH, for example 1920x1080. Usually you want to set this to the size of the window. Optional, by default the size of the window, monitor or screen is used (which is passed to -w).\n");
    fprintf(stderr, "  -c    Container format for output file, for example mp4, or flv.\n");
    fprintf(stderr, "  -f    Framerate to record at. Clamped to [1,250].\n");
    fprintf(stderr, "  -a    Audio device to record from (pulse audio device). Optional, disabled by default.\n");
    fprintf(stderr, "  -q    Video quality. Should either be 'medium', 'high' or 'ultra'. Optional, set to 'medium' be default.\n");
    fprintf(stderr, "  -r    Replay buffer size in seconds. If this is set, then only the last seconds as set by this option will be stored"
        " and the video will only be saved when the gpu-screen-recorder is closed. This feature is similar to Nvidia's instant replay feature."
        " This option has be between 5 and 1200. Note that the replay buffer size will not always be precise, because of keyframes. Optional, disabled by default.\n");
    fprintf(stderr, "  -o    The output file path. If omitted then the encoded data is sent to stdout. Required in replay mode (when using -r). In replay mode this has to be an existing directory instead of a file.\n");
    fprintf(stderr, "NOTES:\n");
    fprintf(stderr, "  Send signal SIGINT (Ctrl+C) to gpu-screen-recorder to stop and save the recording (when not using replay mode).\n");
    fprintf(stderr, "  Send signal SIGUSR1 (killall -SIGUSR1 gpu-screen-recorder) to gpu-screen-recorder to save a replay.\n");
    exit(1);
}

static sig_atomic_t started = 0;
static sig_atomic_t running = 1;
static sig_atomic_t save_replay = 0;
static const char *pid_file = "/tmp/gpu-screen-recorder";

static void term_handler(int) {
    if(started)
        unlink(pid_file);
    exit(0);
}

static void int_handler(int) {
    running = 0;
}

static void save_replay_handler(int) {
    save_replay = 1;
}

struct Arg {
    const char *value;
    bool optional;
};

static bool is_hex_num(char c) {
    return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static bool contains_non_hex_number(const char *str) {
    size_t len = strlen(str);
    if(len >= 2 && memcmp(str, "0x", 2) == 0) {
        str += 2;
        len -= 2;
    }

    for(size_t i = 0; i < len; ++i) {
        char c = str[i];
        if(c == '\0')
            return false;
        if(!is_hex_num(c))
            return true;
    }
    return false;
}

static std::string get_date_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
    return str; 
}

static AVStream* create_stream(AVFormatContext *av_format_context, AVCodecContext *codec_context) {
    AVStream *stream = avformat_new_stream(av_format_context, nullptr);
    if (!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    stream->time_base = codec_context->time_base;
    stream->avg_frame_rate = av_inv_q(codec_context->framerate);
    return stream;
}

static std::future<void> save_replay_thread;
static std::vector<AVPacket> save_replay_packets;
static std::string save_replay_output_filepath;

static void save_replay_async(AVCodecContext *video_codec_context, AVCodecContext *audio_codec_context, int video_stream_index, int audio_stream_index, const std::deque<AVPacket> &frame_data_queue, bool frames_erased, std::string output_dir, std::string container_format) {
    if(save_replay_thread.valid())
        return;
    
    size_t start_index = (size_t)-1;
    for(size_t i = 0; i < frame_data_queue.size(); ++i) {
        const AVPacket &av_packet = frame_data_queue[i];
        if((av_packet.flags & AV_PKT_FLAG_KEY) && av_packet.stream_index == video_stream_index) {
            start_index = i;
            break;
        }
    }

    if(start_index == (size_t)-1)
        return;

    int64_t pts_offset = 0;
    if(frames_erased)
        pts_offset = frame_data_queue[start_index].pts;

    save_replay_packets.resize(frame_data_queue.size());
    for(size_t i = 0; i < frame_data_queue.size(); ++i) {
        av_packet_ref(&save_replay_packets[i], &frame_data_queue[i]);
    }

    save_replay_output_filepath = output_dir + "/Replay_" + get_date_str() + "." + container_format;
    save_replay_thread = std::async(std::launch::async, [video_stream_index, container_format, start_index, pts_offset, video_codec_context, audio_codec_context]() mutable {
        AVFormatContext *av_format_context;
        // The output format is automatically guessed from the file extension
        avformat_alloc_output_context2(&av_format_context, nullptr, container_format.c_str(), nullptr);

        av_format_context->flags |= AVFMT_FLAG_GENPTS;
        if (av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
            av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVStream *video_stream = create_stream(av_format_context, video_codec_context);
        AVStream *audio_stream = audio_codec_context ? create_stream(av_format_context, audio_codec_context) : nullptr;

        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);

        int ret = avio_open(&av_format_context->pb, save_replay_output_filepath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s. Make sure %s is an existing directory with write access\n", save_replay_output_filepath.c_str(), av_error_to_string(ret), save_replay_output_filepath.c_str());
            return;
        }

        ret = avformat_write_header(av_format_context, nullptr);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return;
        }

        for(size_t i = start_index; i < save_replay_packets.size(); ++i) {
            AVPacket &av_packet = save_replay_packets[i];

            AVStream *stream = av_packet.stream_index == video_stream_index ? video_stream : audio_stream;
            AVCodecContext *codec_context = av_packet.stream_index == video_stream_index ? video_codec_context : audio_codec_context;

            av_packet.stream_index = stream->index;
            av_packet.pts -= pts_offset;
            av_packet.dts -= pts_offset;
            av_packet_rescale_ts(&av_packet, codec_context->time_base, stream->time_base);

            int ret = av_interleaved_write_frame(av_format_context, &av_packet);
            if(ret < 0)
                fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", stream->index, av_error_to_string(ret), ret);
        }

        if (av_write_trailer(av_format_context) != 0)
            fprintf(stderr, "Failed to write trailer\n");

        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
    });
}

static bool is_process_running_program(pid_t pid, const char *program_name) {
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/proc/%ld/exe", (long)pid);

    char resolved_path[PATH_MAX];
    const ssize_t resolved_path_len = readlink(filepath, resolved_path, sizeof(resolved_path) - 1);
    if(resolved_path_len == -1)
        return false;
    
    resolved_path[resolved_path_len] = '\0';

    const int program_name_len = strlen(program_name);
    return resolved_path_len >= program_name_len && memcmp(resolved_path + resolved_path_len - program_name_len, program_name, program_name_len) == 0;
}

static void handle_existing_pid_file() {
    char buffer[256];
    int fd = open(pid_file, O_RDONLY);
    if(fd == -1)
        return;

    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if(bytes_read < 0) {
        perror("failed to read gpu-screen-recorder pid file");
        exit(1);
    }
    buffer[bytes_read] = '\0';
    close(fd);

    long pid = 0;
    if(sscanf(buffer, "%ld %120s", &pid, buffer) == 2) {
        if(is_process_running_program(pid, "gpu-screen-recorder")) {
            fprintf(stderr, "Error: gpu-screen-recorder is already running\n");
            exit(1);
        }
    } else {
        fprintf(stderr, "Warning: gpu-screen-recorder pid file is in incorrect format, it's possible that its corrupt. Replacing file and continuing...\n");
    }
    unlink(pid_file);
}

static void handle_new_pid_file(const char *mode) {
    int fd = open(pid_file, O_WRONLY|O_CREAT|O_TRUNC, 0777);
    if(fd == -1) {
        perror("failed to create gpu-screen-recorder pid file");
        exit(1);
    }

    char buffer[256];
    const int buffer_size = snprintf(buffer, sizeof(buffer), "%ld %s", (long)getpid(), mode);
    if(write(fd, buffer, buffer_size) == -1) {
        perror("failed to write gpu-screen-recorder pid file");
        exit(1);
    }
    close(fd);
}

int main(int argc, char **argv) {
    signal(SIGTERM, term_handler);
    signal(SIGINT, int_handler);
    signal(SIGUSR1, save_replay_handler);

    handle_existing_pid_file();

    std::map<std::string, Arg> args = {
        { "-w", Arg { nullptr, false } },
        //{ "-s", Arg { nullptr, true } },
        { "-c", Arg { nullptr, false } },
        { "-f", Arg { nullptr, false } },
        { "-s", Arg { nullptr, true } },
        { "-a", Arg { nullptr, true } },
        { "-q", Arg { nullptr, true } },
        { "-o", Arg { nullptr, true } },
        { "-r", Arg { nullptr, true } }
    };

    for(int i = 1; i < argc - 1; i += 2) {
        auto it = args.find(argv[i]);
        if(it == args.end()) {
            fprintf(stderr, "Invalid argument '%s'\n", argv[i]);
            usage();
        }
        it->second.value = argv[i + 1];
    }

    for(auto &it : args) {
        if(!it.second.optional && !it.second.value) {
            fprintf(stderr, "Missing argument '%s'\n", it.first.c_str());
            usage();
        }
    }

    Arg &audio_input_arg = args["-a"];

    uint32_t region_x = 0;
    uint32_t region_y = 0;
    uint32_t region_width = 0;
    uint32_t region_height = 0;

    /*
    TODO: Fix this. Doesn't work for some reason
    const char *screen_region = args["-s"].value;
    if(screen_region) {
        if(sscanf(screen_region, "%ux%u+%u+%u", &region_x, &region_y, &region_width, &region_height) != 4) {
            fprintf(stderr, "Invalid value for -s '%s', expected a value in format WxH+X+Y\n", screen_region);
            return 1;
        }
    }
    */

    const char *container_format = args["-c"].value;
    int fps = atoi(args["-f"].value);
    if(fps == 0) {
        fprintf(stderr, "Invalid fps argument: %s\n", args["-f"].value);
        return 1;
    }
    if(fps > 250)
        fps = 250;

    const char *quality_str = args["-q"].value;
    if(!quality_str)
        quality_str = "medium";

    VideoQuality quality;
    if(strcmp(quality_str, "medium") == 0) {
        quality = VideoQuality::MEDIUM;
    } else if(strcmp(quality_str, "high") == 0) {
        quality = VideoQuality::HIGH;
    } else if(strcmp(quality_str, "ultra") == 0) {
        quality = VideoQuality::ULTRA;
    } else {
        fprintf(stderr, "Error: -q should either be either 'medium', 'high' or 'ultra', got: '%s'\n", quality_str);
        usage();
    }

    int replay_buffer_size_secs = -1;
    const char *replay_buffer_size_secs_str = args["-r"].value;
    if(replay_buffer_size_secs_str) {
        replay_buffer_size_secs = atoi(replay_buffer_size_secs_str);
        if(replay_buffer_size_secs < 5 || replay_buffer_size_secs > 1200) {
            fprintf(stderr, "Error: option -r has to be between 5 and 1200, was: %s\n", replay_buffer_size_secs_str);
            return 1;
        }
        replay_buffer_size_secs += 5; // Add a few seconds to account of lost packets because of non-keyframe packets skipped
    }

    CUresult res;

    res = cuInit(0);
    if(res != CUDA_SUCCESS) {
        const char *err_str;
        cuGetErrorString(res, &err_str);
        fprintf(stderr, "Error: cuInit failed, error %s (result: %d)\n", err_str, res);
        return 1;
    }

    int nGpu = 0;
    cuDeviceGetCount(&nGpu);
    if (nGpu <= 0) {
        fprintf(stderr, "Error: no cuda supported devices found\n");
        return 1;
    }

    CUdevice cu_dev;
    res = cuDeviceGet(&cu_dev, 0);
    if(res != CUDA_SUCCESS) {
        const char *err_str;
        cuGetErrorString(res, &err_str);
        fprintf(stderr, "Error: unable to get CUDA device, error: %s (result: %d)\n", err_str, res);
        return 1;
    }

    CUcontext cu_ctx;
    res = cuCtxCreate_v2(&cu_ctx, CU_CTX_SCHED_AUTO, cu_dev);
    if(res != CUDA_SUCCESS) {
        const char *err_str;
        cuGetErrorString(res, &err_str);
        fprintf(stderr, "Error: unable to create CUDA context, error: %s (result: %d)\n", err_str, res);
        return 1;
    }

    uint32_t window_width = 0;
    uint32_t window_height = 0;

    NvFBCLibrary nv_fbc_library;

    const char *window_str = args["-w"].value;
    Window src_window_id = None;
    if(contains_non_hex_number(window_str)) {
        if(!nv_fbc_library.load())
            return 1;

        const char *capture_target = window_str;
        const bool direct_capture = strcmp(window_str, "screen-direct") == 0;
        if(direct_capture)
            capture_target = "screen";

        if(!nv_fbc_library.create(capture_target, fps, &window_width, &window_height, region_x, region_y, region_width, region_height, direct_capture))
            return 1;
    } else {
        errno = 0;
        src_window_id = strtol(window_str, nullptr, 0);
        if(src_window_id == None || errno == EINVAL) {
            fprintf(stderr, "Invalid window number %s\n", window_str);
            usage();
        }
    }

    int record_width = window_width;
    int record_height = window_height;
    const char *record_area = args["-s"].value;
    if(record_area) {
        if(sscanf(record_area, "%dx%d", &record_width, &record_height) != 2) {
            fprintf(stderr, "Invalid value for -s '%s', expected a value in format WxH\n", record_area);
            return 1;
        }
    }

    const char *filename = args["-o"].value;
    if(filename) {
        if(replay_buffer_size_secs != -1) {
            struct stat buf;
            if(stat(filename, &buf) == -1 || !S_ISDIR(buf.st_mode)) {
                fprintf(stderr, "%s does not exist or is not a directory\n", filename);
                usage();
            }
        }
    } else {
        if(replay_buffer_size_secs == -1) {
            filename = "/dev/stdout";
        } else {
            fprintf(stderr, "Option -o is required when using option -r\n");
            usage();
        }
    }

    const double target_fps = 1.0 / (double)fps;

    WindowPixmap window_pixmap;
    Display *dpy = nullptr;
    GLFWwindow *window = nullptr;
    if(src_window_id) {
        dpy = XOpenDisplay(nullptr);
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

        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, src_window_id, &attr)) {
            fprintf(stderr, "Error: Invalid window id: %lu\n", src_window_id);
            return 1;
        }

        window_width = attr.width;
        window_height = attr.height;

        XCompositeRedirectWindow(dpy, src_window_id, CompositeRedirectAutomatic);

        // glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read,
        // GLXContext ctx)
        if (!glfwInit()) {
            fprintf(stderr, "Error: Failed to initialize glfw\n");
            return 1;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GL_FALSE);

        window = glfwCreateWindow(1, 1, "gpu-screen-recorder", nullptr, nullptr);
        if (!window) {
            fprintf(stderr, "Error: Failed to create glfw window\n");
            glfwTerminate();
            return 1;
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(0);

    //#if defined(DEBUG)
        XSetErrorHandler(x11_error_handler);
        XSetIOErrorHandler(x11_io_error_handler);
    //#endif

        glewExperimental = GL_TRUE;
        GLenum nGlewError = glewInit();
        if (nGlewError != GLEW_OK) {
            fprintf(stderr, "%s - Error initializing GLEW! %s\n", __FUNCTION__,
                    glewGetErrorString(nGlewError));
            return 1;
        }
        glGetError(); // to clear the error caused deep in GLEW

        if (!recreate_window_pixmap(dpy, src_window_id, window_pixmap)) {
            fprintf(stderr, "Error: Failed to create glx pixmap for window: %lu\n",
                    src_window_id);
            return 1;
        }

        if(!record_area) {
            record_width = window_pixmap.texture_width;
            record_height = window_pixmap.texture_height;
            fprintf(stderr, "Record size: %dx%d\n", record_width, record_height);
        }
    } else {
        window_pixmap.texture_id = 0;
        window_pixmap.target_texture_id = 0;
        window_pixmap.texture_width = window_width;
        window_pixmap.texture_height = window_height;

        if (!glfwInit()) {
            fprintf(stderr, "Error: Failed to initialize glfw\n");
            return 1;
        }
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

    av_format_context->flags |= AVFMT_FLAG_GENPTS;
    const AVOutputFormat *output_format = av_format_context->oformat;

    bool use_hevc = strcmp(window_str, "screen") == 0 || strcmp(window_str, "screen-direct") == 0;
    if(use_hevc && strcmp(container_format, "flv") == 0) {
        use_hevc = false;
        fprintf(stderr, "Warning: hevc is not compatible with flv, falling back to h264 instead.\n");
    }

    AVStream *video_stream = nullptr;
    AVStream *audio_stream = nullptr;

    AVCodecContext *video_codec_context = create_video_codec_context(av_format_context, quality, record_width, record_height, fps, use_hevc);
    if(replay_buffer_size_secs == -1)
        video_stream = create_stream(av_format_context, video_codec_context);

    AVBufferRef *device_ctx;
    CUgraphicsResource cuda_graphics_resource;
    open_video(video_codec_context, window_pixmap, &device_ctx, &cuda_graphics_resource, cu_ctx);
    if(video_stream)
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

    AVCodecContext *audio_codec_context = nullptr;
    AVFrame *audio_frame = nullptr;
    if(audio_input_arg.value) {
        audio_codec_context = create_audio_codec_context(av_format_context, fps);
        if(replay_buffer_size_secs == -1)
            audio_stream = create_stream(av_format_context, audio_codec_context);

        audio_frame = open_audio(audio_codec_context);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);
    }

    //av_dump_format(av_format_context, 0, filename, 1);

    if (replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename, av_error_to_string(ret));
            return 1;
        }
    }

    //video_stream->duration = AV_TIME_BASE * 15;
    //audio_stream->duration = AV_TIME_BASE * 15;
    //av_format_context->duration = AV_TIME_BASE * 15;
    if(replay_buffer_size_secs == -1) {
        int ret = avformat_write_header(av_format_context, nullptr);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return 1;
        }
    }

    // av_frame_free(&rgb_frame);
    // avcodec_close(av_codec_context);

    if(dpy)
        XSelectInput(dpy, src_window_id, StructureNotifyMask | VisibilityChangeMask);

    /*
    int damage_event;
    int damage_error;
    if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        fprintf(stderr, "Error: XDamage is not supported by your X11 server\n");
        return 1;
    }

    Damage damage = XDamageCreate(dpy, src_window_id, XDamageReportNonEmpty);
    XDamageSubtract(dpy, damage,None,None);
    */

    const double start_time_pts = clock_get_monotonic_seconds();

    CUcontext old_ctx;
    CUarray mapped_array;
    if(src_window_id) {
        res = cuCtxPopCurrent(&old_ctx);
        res = cuCtxPushCurrent(cu_ctx);

        // Get texture
        res = cuGraphicsResourceSetMapFlags(
            cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
        res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

        // Map texture to cuda array
        res = cuGraphicsSubResourceGetMappedArray(&mapped_array,
                                                cuda_graphics_resource, 0, 0);
    }

    // Release texture
    // res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

    double start_time = glfwGetTime();
    double frame_timer_start = start_time;
    double window_resize_timer = start_time;
    bool window_resized = false;
    int fps_counter = 0;
    int current_fps = 30;

    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error: Failed to allocate frame\n");
        exit(1);
    }
    frame->format = video_codec_context->pix_fmt;
    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if (av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0) < 0) {
        fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
        exit(1);
    }

    if(window_pixmap.texture_width < record_width)
        frame->width = window_pixmap.texture_width & ~1;
    else
        frame->width = record_width & ~1;

    if(window_pixmap.texture_height < record_height)
        frame->height = window_pixmap.texture_height & ~1;
    else
        frame->height = record_height & ~1;

    std::mutex write_output_mutex;
    std::thread audio_thread;

    double record_start_time = glfwGetTime();
    std::deque<AVPacket> frame_data_queue;
    bool frames_erased = false;

    SoundDevice sound_device;
    uint8_t *audio_frame_buf;
    if(audio_input_arg.value) {
        if(sound_device_get_by_name(&sound_device, audio_input_arg.value, audio_codec_context->channels, audio_codec_context->frame_size) != 0) {
            fprintf(stderr, "failed to get 'pulse' sound device\n");
            exit(1);
        }

        int audio_buffer_size = av_samples_get_buffer_size(NULL, audio_codec_context->channels, audio_codec_context->frame_size, audio_codec_context->sample_fmt, 1);
        audio_frame_buf = (uint8_t *)av_malloc(audio_buffer_size);
        avcodec_fill_audio_frame(audio_frame, audio_codec_context->channels, audio_codec_context->sample_fmt, (const uint8_t*)audio_frame_buf, audio_buffer_size, 1);

        audio_thread = std::thread([record_start_time, replay_buffer_size_secs, &frame_data_queue, &frames_erased, audio_codec_context, start_time_pts, fps](AVFormatContext *av_format_context, AVStream *audio_stream, uint8_t *audio_frame_buf, SoundDevice *sound_device, AVFrame *audio_frame, std::mutex *write_output_mutex) mutable {
            
            SwrContext *swr = swr_alloc();
            if(!swr) {
                fprintf(stderr, "Failed to create SwrContext\n");
                exit(1);
            }
            av_opt_set_int(swr, "in_channel_layout", audio_codec_context->channel_layout, 0);
            av_opt_set_int(swr, "out_channel_layout", audio_codec_context->channel_layout, 0);
            av_opt_set_int(swr, "in_sample_rate", audio_codec_context->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", audio_codec_context->sample_rate, 0);
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
                    audio_frame->pts = (clock_get_monotonic_seconds() - start_time_pts) * AV_TIME_BASE;

                    int ret = avcodec_send_frame(audio_codec_context, audio_frame);
                    if(ret < 0){
                        fprintf(stderr, "Failed to encode!\n");
                        break;
                    }
                    if(ret >= 0)
                        receive_frames(audio_codec_context, AUDIO_STREAM_INDEX, audio_stream, audio_frame, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, *write_output_mutex);
                } else {
                    fprintf(stderr, "failed to read sound from device, error: %d\n", sound_buffer_size);
                }
            }

            swr_free(&swr);
        }, av_format_context, audio_stream, audio_frame_buf, &sound_device, audio_frame, &write_output_mutex);
    }

    handle_new_pid_file(replay_buffer_size_secs == -1 ? "record" : "replay");
    started = 1;

    bool redraw = true;
    XEvent e;
    while (running) {
        double frame_start = glfwGetTime();
        glfwPollEvents();
        if(window)
            glClear(GL_COLOR_BUFFER_BIT);

        redraw = true;

        if(src_window_id) {
            if (XCheckTypedWindowEvent(dpy, src_window_id, DestroyNotify, &e)) {
                running = 0;
            }

            if (XCheckTypedWindowEvent(dpy, src_window_id, VisibilityNotify, &e)) {
                window_resize_timer = glfwGetTime();
                window_resized = true;
            }

            if (XCheckTypedWindowEvent(dpy, src_window_id, ConfigureNotify, &e) && e.xconfigure.window == src_window_id) {
                // Window resize
                if(e.xconfigure.width != window_width || e.xconfigure.height != window_height) {
                    window_width = e.xconfigure.width;
                    window_height = e.xconfigure.height;
                    window_resize_timer = glfwGetTime();
                    window_resized = true;
                }
            }

            const double window_resize_timeout = 1.0; // 1 second
            if(window_resized && glfwGetTime() - window_resize_timer >= window_resize_timeout) {
                window_resized = false;
                fprintf(stderr, "Resize window!\n");
                recreate_window_pixmap(dpy, src_window_id, window_pixmap);
                // Resolution must be a multiple of two
                //video_stream->codec->width = window_pixmap.texture_width & ~1;
                //video_stream->codec->height = window_pixmap.texture_height & ~1;

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
                    running = false;
                    break;
                }

                res = cuGraphicsResourceSetMapFlags(
                    cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
                res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);
                res = cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

                av_frame_free(&frame);
                frame = av_frame_alloc();
                if (!frame) {
                    fprintf(stderr, "Error: Failed to allocate frame\n");
                    running = false;
                    break;
                }
                frame->format = video_codec_context->pix_fmt;
                frame->width = video_codec_context->width;
                frame->height = video_codec_context->height;

                if (av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0) < 0) {
                    fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
                    running = false;
                    break;
                }

                if(window_pixmap.texture_width < record_width)
                    frame->width = window_pixmap.texture_width & ~1;
                else
                    frame->width = record_width & ~1;

                if(window_pixmap.texture_height < record_height)
                    frame->height = window_pixmap.texture_height & ~1;
                else
                    frame->height = record_height & ~1;

                cuMemsetD8((CUdeviceptr)frame->data[0], 0, record_width * record_height * 4);
            }
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

        double frame_time_overflow = frame_timer_elapsed - target_fps;
        if (frame_time_overflow >= 0.0) {
            frame_timer_start = time_now - frame_time_overflow;

            bool frame_captured = true;
            if(redraw) {
                redraw = false;
                if(src_window_id) {
                    // TODO: Use a framebuffer instead. glCopyImageSubData requires
                    // opengl 4.2
                    glCopyImageSubData(
                        window_pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                        window_pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
                        window_pixmap.texture_width, window_pixmap.texture_height, 1);
                    int err = glGetError();
                    if(err != 0) {
                        static bool error_shown = false;
                        if(!error_shown) {
                            error_shown = true;
                            fprintf(stderr, "Error: glCopyImageSubData failed, gl error: %d\n", err);
                        }
                    }
                    glfwSwapBuffers(window);
                    // int err = glGetError();
                    // fprintf(stderr, "error: %d\n", err);

                    // TODO: Remove this copy, which is only possible by using nvenc directly and encoding window_pixmap.target_texture_id

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

                    frame_captured = true;
                } else {
                    // TODO: Check when src_cu_device_ptr changes and re-register resource
                    uint32_t byte_size = 0;
                    CUdeviceptr src_cu_device_ptr = 0;
                    frame_captured = nv_fbc_library.capture(&src_cu_device_ptr, &byte_size);
                    frame->data[0] = (uint8_t*)src_cu_device_ptr;
                }
                // res = cuCtxPopCurrent(&old_ctx);
            }

            frame->pts = (clock_get_monotonic_seconds() - start_time_pts) * AV_TIME_BASE;
            if (avcodec_send_frame(video_codec_context, frame) >= 0) {
                receive_frames(video_codec_context, VIDEO_STREAM_INDEX, video_stream, frame, av_format_context,
                               record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex);
            } else {
                fprintf(stderr, "Error: avcodec_send_frame failed\n");
            }
        }

        if(save_replay_thread.valid() && save_replay_thread.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            save_replay_thread.get();
            puts(save_replay_output_filepath.c_str());
            for(size_t i = 0; i < save_replay_packets.size(); ++i) {
                av_packet_unref(&save_replay_packets[i]);
            }
            save_replay_packets.clear();
        }

        if(save_replay == 1 && !save_replay_thread.valid() && replay_buffer_size_secs != -1) {
            save_replay = 0;
            save_replay_async(video_codec_context, audio_codec_context, VIDEO_STREAM_INDEX, AUDIO_STREAM_INDEX, frame_data_queue, frames_erased, filename, container_format);
        }

        // av_frame_free(&frame);
        double frame_end = glfwGetTime();
        double frame_sleep_fps = 1.0 / 250.0;
        double sleep_time = frame_sleep_fps - (frame_end - frame_start);
        if(sleep_time > 0.0)
            usleep(sleep_time * 1000.0 * 1000.0);
    }

	running = 0;

    if(save_replay_thread.valid())
        save_replay_thread.get();

    if(audio_input_arg.value) {
        audio_thread.join();
        sound_device_close(&sound_device);
    }

    if (replay_buffer_size_secs == -1 && av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    if(replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE))
        avio_close(av_format_context->pb);

    if(dpy) {
        XCompositeUnredirectWindow(dpy, src_window_id, CompositeRedirectAutomatic);
        XCloseDisplay(dpy);
    }

    unlink(pid_file);
}
