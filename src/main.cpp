#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string>
#include <vector>

#define GLX_GLXEXT_PROTOTYPES
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

// TODO: Use opencl or vulkan instead
#include <ffnvcodec/nvEncodeAPI.h>
//#include <ffnvcodec/dynlink_cuda.h>
extern "C" {
#include <libavutil/hwcontext_cuda.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <cudaGL.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <CL/cl.h>

struct ScopedGLXFBConfig {
    ~ScopedGLXFBConfig() {
        if(configs)
            XFree(configs);
    }

    GLXFBConfig *configs = nullptr;
};

struct WindowPixmap {
    WindowPixmap() : pixmap(None), glx_pixmap(None), texture_id(0), target_texture_id(0), texture_width(0), texture_height(0) {

    }

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
    if(!XCompositeQueryExtension(dpy, &extension_major, &extension_minor))
        return false;

    int major_version;
    int minor_version;
    return XCompositeQueryVersion(dpy, &major_version, &minor_version) && (major_version > 0 || minor_version >= 2);
}

static void cleanup_window_pixmap(Display *dpy, WindowPixmap &pixmap) {
    if(pixmap.target_texture_id) {
        glDeleteTextures(1, &pixmap.target_texture_id);
        pixmap.target_texture_id = 0;
    }

    if(pixmap.texture_id) {
        glDeleteTextures(1, &pixmap.texture_id);
        pixmap.texture_id = 0;
        pixmap.texture_width = 0;
        pixmap.texture_height = 0;
    }

    if(pixmap.glx_pixmap) {
        glXReleaseTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT);
		glXDestroyPixmap(dpy, pixmap.glx_pixmap);
        pixmap.glx_pixmap = None;
    }

    if(pixmap.pixmap) {
        XFreePixmap(dpy, pixmap.pixmap);
        pixmap.pixmap = None;
    }
}

static bool recreate_window_pixmap(Display *dpy, Window window_id, WindowPixmap &pixmap) {
    cleanup_window_pixmap(dpy, pixmap);
    
    const int pixmap_config[] = {
		GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
		GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
		GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
		GLX_BIND_TO_MIPMAP_TEXTURE_EXT, True,
		GLX_DOUBLEBUFFER, False,
		//GLX_Y_INVERTED_EXT, (int)GLX_DONT_CARE,
		None
    };

    const int pixmap_attribs[] = {
		GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
		GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
		GLX_MIPMAP_TEXTURE_EXT, 1,
		None
    };

	int c;
	GLXFBConfig *configs = glXChooseFBConfig(dpy, 0, pixmap_config, &c);
	if(!configs) {
		fprintf(stderr, "Failed too choose fb config\n");
		return false;
	}
    ScopedGLXFBConfig scoped_configs;
    scoped_configs.configs = configs;

	Pixmap new_window_pixmap = XCompositeNameWindowPixmap(dpy, window_id);
	if(!new_window_pixmap) {
		fprintf(stderr, "Failed to get pixmap for window %ld\n", window_id);
		return false;
	}

	GLXPixmap glx_pixmap = glXCreatePixmap(dpy, *configs, new_window_pixmap, pixmap_attribs);
	if(!glx_pixmap) {
		fprintf(stderr, "Failed to create glx pixmap\n");
        XFreePixmap(dpy, new_window_pixmap);
		return false;
	}

    pixmap.pixmap = new_window_pixmap;
    pixmap.glx_pixmap = glx_pixmap;

    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &pixmap.texture_id);
	glBindTexture(GL_TEXTURE_2D, pixmap.texture_id);

	//glEnable(GL_BLEND);
    //glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#if 1
	glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &pixmap.texture_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &pixmap.texture_height);
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);//GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);//GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    printf("texture width: %d, height: %d\n", pixmap.texture_width, pixmap.texture_height);

    glGenTextures(1, &pixmap.target_texture_id);
    glBindTexture(GL_TEXTURE_2D, pixmap.target_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixmap.texture_width, pixmap.texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenerateMipmap(GL_TEXTURE_2D);
    int err2 = glGetError();
    printf("error: %d\n", err2);
    glCopyImageSubData(
        pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
        pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
        pixmap.texture_width, pixmap.texture_height, 1);
    int err = glGetError();
    printf("error: %d\n", err);
#else
    pixmap.texture_width = 640;
    pixmap.texture_height = 480;
    uint8_t *image_data = (uint8_t*)malloc(pixmap.texture_width * pixmap.texture_height * 4);
    assert(image_data);
    for(int i = 0; i < pixmap.texture_width * pixmap.texture_height * 4; i += 4) {
        *(uint32_t*)&image_data[i] = 0xFF0000FF;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixmap.texture_width, pixmap.texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
#endif
    //glXBindTexImageEXT(dpy, pixmap.glx_pixmap, GLX_FRONT_EXT, NULL); 
	//glGenerateTextureMipmapEXT(glxpixmap, GL_TEXTURE_2D);

	//glGenerateMipmap(GL_TEXTURE_2D);

	//glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	//glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );


	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);//GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);//GL_LINEAR);//GL_LINEAR_MIPMAP_LINEAR );
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glBindTexture(GL_TEXTURE_2D, 0);
    
    return pixmap.texture_id != 0 && pixmap.target_texture_id != 0;
}

std::vector<std::string> get_hardware_acceleration_device_names() {
    #if 0
    std::vector<std::string> result;

    cl_uint platform_count = 0;
    clGetPlatformIDs(0, nullptr, &platform_count);
    cl_platform_id *platforms = new cl_platform_id[platform_count];
    clGetPlatformIDs(platform_count, platforms, nullptr);

    for(cl_uint i = 0; i < platform_count; ++i) {
        cl_uint device_count = 0;
        clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, 0, nullptr, &device_count);
        cl_device_id *devices = new cl_device_id[device_count];
        clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, device_count, devices, nullptr);

        for(cl_uint j = 0; j < device_count; ++j) {
            size_t value_size = 0;
            clGetDeviceInfo(devices[j], CL_DEVICE_NAME, 0, nullptr, &value_size);
            std::string device_name(value_size, 0);
            clGetDeviceInfo(devices[j], CL_DEVICE_NAME, value_size, &device_name[0], nullptr);
            printf("Device: %s\n", device_name.c_str());
            result.push_back(std::move(device_name));
        }

        delete []devices;
    }

    delete []platforms;
    return result;
    #else
    int iGpu = 0;
    CUresult res;
    if(cuInit(0) < 0) {
        fprintf(stderr, "Error: cuInit failed\n");
        return {};
    }

    int nGpu = 0;
    cuDeviceGetCount(&nGpu);
    if(iGpu < 0 || iGpu >= nGpu) {
        fprintf(stderr, "Error: failed...\n");
        return {};
    }

    CUdevice cuDevice = 0;
    cuDeviceGet(&cuDevice, iGpu);
    char deviceName[80];
    cuDeviceGetName(deviceName, sizeof(deviceName), cuDevice);
    printf("device name: %s\n", deviceName);
    return { deviceName };
    #endif
}

static inline double ToDouble(const AVRational& r) {
	return (double) r.num / (double) r.den;
}

#if 0
static void receive_frames(AVCodecContext *av_codec_context, AVStream *stream, AVFormatContext *av_format_context) {
    for( ; ; ) {
        AVPacket *av_packet = new AVPacket;
        av_init_packet(av_packet);
        av_packet->data = NULL;
        av_packet->size = 0;
		int res = avcodec_receive_packet(av_codec_context, av_packet);
		if(res == 0) { // we have a packet, send the packet to the muxer
			printf("Received packet!\n");
            printf("data: %p, size: %d, pts: %ld\n", (void*)av_packet->data, av_packet->size, av_packet->pts);
            printf("timebase: %f\n", ToDouble(stream->time_base));

			// prepare packet
            av_packet_rescale_ts(av_packet, av_codec_context->time_base, stream->time_base);
			av_packet->stream_index = stream->index;
/*
			if(av_packet->pts != (int64_t) AV_NOPTS_VALUE) {
				av_packet->pts = av_rescale_q(av_packet->pts, av_codec_context->time_base, stream->time_base);
			}
			if(av_packet->dts != (int64_t) AV_NOPTS_VALUE) {
				av_packet->dts = av_rescale_q(av_packet->dts, av_codec_context->time_base, stream->time_base);
			}
*/
            if(av_interleaved_write_frame(av_format_context, av_packet) < 0) {
                fprintf(stderr, "Error: Failed to write frame to muxer\n");
            }
            //av_packet_unref(&av_packet);
		} else if(res == AVERROR(EAGAIN)) { // we have no packet
            //printf("No packet!\n");
			break;
		} else if(res == AVERROR_EOF) { // this is the end of the stream
			printf("End of stream!\n");
            break;
		} else {
			printf("Unexpected error: %d\n", res);
            break;
		}
	}
    //av_packet_unref(&av_packet);
}
#else
static void receive_frames(AVCodecContext *av_codec_context, AVStream *stream, AVFormatContext *av_format_context) {
    AVPacket av_packet;
    av_init_packet(&av_packet);
    for( ; ; ) {
        av_packet.data = NULL;
        av_packet.size = 0;
		int res = avcodec_receive_packet(av_codec_context, &av_packet);
		if(res == 0) { // we have a packet, send the packet to the muxer
			//printf("Received packet!\n");
            //printf("data: %p, size: %d, pts: %ld\n", (void*)av_packet->data, av_packet->size, av_packet->pts);
            //printf("timebase: %f\n", ToDouble(stream->time_base));

			//av_packet.pts = av_rescale_q_rnd(av_packet.pts, av_codec_context->time_base, stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            //av_packet.dts = av_rescale_q_rnd(av_packet.dts, av_codec_context->time_base, stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
            //av_packet.duration = 60;//av_rescale_q(av_packet->duration, av_codec_context->time_base, stream->time_base);
            av_packet_rescale_ts(&av_packet, av_codec_context->time_base, stream->time_base);
            av_packet.pts /= 2;
            av_packet.dts /= 2;
            av_packet.stream_index = stream->index;
            //av_packet->stream_index = 0;

/*
            int written = fwrite(av_packet->data, 1, av_packet->size, output_file);

            if(written != av_packet->size) {
                fprintf(stderr, "Failed to write %d bytes to file: %d, %d\n", av_packet->size, written, ferror(output_file));
            }
*/
            if(av_write_frame(av_format_context, &av_packet) < 0) {
                fprintf(stderr, "Error: Failed to write frame to muxer\n");
            }
            //av_packet_unref(&av_packet);
		} else if(res == AVERROR(EAGAIN)) { // we have no packet
            //printf("No packet!\n");
			break;
		} else if(res == AVERROR_EOF) { // this is the end of the stream
			printf("End of stream!\n");
            break;
		} else {
			printf("Unexpected error: %d\n", res);
            break;
		}
	}
    av_packet_unref(&av_packet);
}
#endif

static AVStream* add_stream(AVFormatContext *av_format_context, AVCodec **codec, enum AVCodecID codec_id) {
    //*codec = avcodec_find_encoder(codec_id);
    *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if(!*codec) {
        fprintf(stderr, "Error: Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
        exit(1);
    }

    AVStream *stream = avformat_new_stream(av_format_context, *codec);
    if(!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    AVCodecContext *codec_context = stream->codec;

    switch((*codec)->type) {
        case AVMEDIA_TYPE_AUDIO: {
            codec_context->sample_fmt = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            codec_context->bit_rate = 64000;
            codec_context->sample_rate = 44100;
            codec_context->channels = 2;
            break;
        }
        case AVMEDIA_TYPE_VIDEO: {
            codec_context->codec_id = codec_id;
            codec_context->bit_rate = 4000000;
            // Resolution must be a multiple of two
            codec_context->width = 1920;
            codec_context->height = 1080;
            // Timebase: This is the fundamental unit of time (in seconds) in terms of
            // which frame timestamps are represented. For fixed-fps content,
            // timebase should be 1/framerate and timestamp increments should be identical to 1
            codec_context->time_base.num = 1;
            codec_context->time_base.den = 60;
            codec_context->framerate.num = 60;
            codec_context->framerate.den = 1;
            codec_context->sample_aspect_ratio.num = 1;
            codec_context->sample_aspect_ratio.den = 1;
            codec_context->gop_size = 12; // Emit one intra frame every twelve frames at most
            codec_context->pix_fmt = AV_PIX_FMT_CUDA;
            if(codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
                codec_context->mb_decision = 2;

            //stream->time_base = codec_context->time_base;
            //codec_context->ticks_per_frame = 30;
            break;
        }
        default:
            break;
    }

    // Some formats want stream headers to be seperate
    if(av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        av_format_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return stream;
}

static void open_video(AVCodec *codec, AVStream *stream, WindowPixmap &window_pixmap, AVFrame **frame, AVBufferRef **device_ctx, CUgraphicsResource *cuda_graphics_resource) {
    int ret;
    AVCodecContext *codec_context = stream->codec;

    std::vector<std::string> hardware_accelerated_devices = get_hardware_acceleration_device_names();
    if(hardware_accelerated_devices.empty()) {
        fprintf(stderr, "Error: No hardware accelerated device was found on your system\n");
        exit(1);
    }

    if(av_hwdevice_ctx_create(device_ctx, AV_HWDEVICE_TYPE_CUDA, hardware_accelerated_devices[0].c_str(), NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context for gpu: %s\n", hardware_accelerated_devices[0].c_str());
        exit(1);
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(*device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        exit(1);
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = codec_context->width;
    hw_frame_context->height = codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_0BGR32;
    hw_frame_context->format = codec_context->pix_fmt;
    hw_frame_context->device_ref = *device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)(*device_ctx)->data;

    if(av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context (note: ffmpeg version needs to be > 4.0\n");
        exit(1);
    }

    codec_context->hw_device_ctx = *device_ctx;
    codec_context->hw_frames_ctx = frame_context;

    ret = avcodec_open2(codec_context, codec, nullptr);
    if(ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n", "blabla");//av_err2str(ret));
        exit(1);
    }

    *frame = av_frame_alloc();
    if(!*frame) {
        fprintf(stderr, "Error: Failed to allocate frame\n");
        exit(1);
    }
    (*frame)->format = codec_context->pix_fmt;
    (*frame)->width = codec_context->width;
    (*frame)->height = codec_context->height;

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)(*device_ctx)->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if(!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        exit(1);
    }

    CUresult res;
    CUcontext old_ctx;
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);
    res = cuGraphicsGLRegisterImage(cuda_graphics_resource, window_pixmap.target_texture_id, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if(res != CUDA_SUCCESS) {
        fprintf(stderr, "Error: cuGraphicsGLRegisterImage failed, error %d, texture id: %u\n", res, window_pixmap.target_texture_id);
        exit(1);
    }
    res = cuCtxPopCurrent(&old_ctx);
}

static void close_video(AVStream *video_stream, AVFrame *frame) {
    avcodec_close(video_stream->codec);
    //av_frame_free(&frame);
}

int main(int argc, char **argv) {
    if(argc < 2) {
        fprintf(stderr, "usage: hardware-screen-recorder <window_id>\n");
        return 1;
    }

    Window src_window_id = atoi(argv[1]);

    Display *dpy = XOpenDisplay(nullptr);
    if(!dpy) {
        fprintf(stderr, "Error: Failed to open display\n");
        return 1;
    }

    bool has_name_pixmap = x11_supports_composite_named_window_pixmap(dpy);
    if(!has_name_pixmap) {
        fprintf(stderr, "Error: XCompositeNameWindowPixmap is not supported by your X11 server\n");
        return 1;
    }
    
    // TODO: Verify if this is needed
    int screen_count = ScreenCount(dpy);
    for(int i = 0; i < screen_count; ++i) {
        XCompositeRedirectSubwindows(dpy, RootWindow(dpy, i), CompositeRedirectAutomatic);
    }

    XWindowAttributes attr;
    if(!XGetWindowAttributes(dpy, src_window_id, &attr)) {
        fprintf(stderr, "Error: Invalid window id: %lu\n", src_window_id);
        return 1;
    }

    //glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx)
    if(!glfwInit()) {
        fprintf(stderr, "Error: Failed to initialize glfw\n");
        return 1;
    }

    glfwWindowHint(GLFW_RESIZABLE, GL_FALSE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "Hello world", nullptr, nullptr);
    if(!window) {
        fprintf(stderr, "Error: Failed to create glfw window\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
	GLenum nGlewError = glewInit();
	if (nGlewError != GLEW_OK) {
		fprintf(stderr, "%s - Error initializing GLEW! %s\n", __FUNCTION__, glewGetErrorString(nGlewError));
		return 1;
	}
	glGetError(); // to clear the error caused deep in GLEW

    WindowPixmap window_pixmap;
    if(!recreate_window_pixmap(dpy, src_window_id, window_pixmap)) {
        fprintf(stderr, "Error: Failed to create glx pixmap for window: %lu\n", src_window_id);
        return 1;
    }

    const char *filename = "test_video.mp4";


    // Video start
    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, nullptr, filename);
    if(!av_format_context) {
        fprintf(stderr, "Error: Failed to deduce output format from file extension .mp4\n");
        return 1;
    }

    AVOutputFormat *output_format = av_format_context->oformat;
    AVCodec *video_codec;
    AVStream *video_stream = add_stream(av_format_context, &video_codec, output_format->video_codec);
    if(!video_stream) {
        fprintf(stderr, "Error: Failed to create video stream\n");
        return 1;
    }

    AVFrame *frame;
    AVBufferRef *device_ctx;
    CUgraphicsResource cuda_graphics_resource;
    open_video(video_codec, video_stream, window_pixmap, &frame, &device_ctx, &cuda_graphics_resource);
    av_dump_format(av_format_context, 0, filename, 1);

    if(!(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if(ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename, "blabla");//av_err2str(ret));
            return 1;
        }
    }

    int ret = avformat_write_header(av_format_context, nullptr);
    if(ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n", "blabla");//av_err2str(ret));
        return 1;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if(!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        exit(1);
    }

    //av_frame_free(&rgb_frame);
    //avcodec_close(av_codec_context);

    XSelectInput(dpy, src_window_id, StructureNotifyMask);

    int damage_event;
    int damage_error;
    if(!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
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
    res = cuGraphicsResourceSetMapFlags(cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

    // Map texture to cuda array
    CUarray mapped_array;
    res = cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

    // Release texture
    //res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

    // TODO: Remove this
    AVCodecContext *codec_context = video_stream->codec;
    if(av_hwframe_get_buffer(codec_context->hw_frames_ctx, frame, 0) < 0) {
        fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
        exit(1);
    }

    //double start_time = glfwGetTime();

    while(!glfwWindowShouldClose(window)) {
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();

        // TODO: Use a framebuffer instead. glCopyImageSubData requires opengl 4.2
        glCopyImageSubData(
            window_pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
            window_pixmap.target_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0,
            window_pixmap.texture_width, window_pixmap.texture_height, 1);
        //int err = glGetError();
        //printf("error: %d\n", err);

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
        //res = cuCtxPopCurrent(&old_ctx);

        //double time_now = glfwGetTime();
        //int frame_cc = (time_now - start_time) * 0.66666;
        //printf("elapsed time: %d\n", frame_cc);
        frame->pts = frame_count++;
        if(avcodec_send_frame(video_stream->codec, frame) < 0) {
            fprintf(stderr, "Error: avcodec_send_frame failed\n");
        }
        receive_frames(video_stream->codec, video_stream, av_format_context);
    }

#if 0
    XEvent e;
    while (1) {
        XNextEvent(dpy, &e);
        if (e.type == ConfigureNotify) {
            // Window resize
            printf("Resize window!\n");
            recreate_window_pixmap(dpy, src_window_id, window_pixmap);
        } else if (e.type == damage_event + XDamageNotify) {
            printf("Redraw!\n");
            XDamageNotifyEvent *de = (XDamageNotifyEvent*)&e;
            // de->drawable is the window ID of the damaged window
            XserverRegion region = XFixesCreateRegion(dpy, nullptr, 0);
            // Subtract all the damage, repairing the window
            XDamageSubtract(dpy, de->damage, None, region);
            XFixesDestroyRegion(dpy, region);

            //glCopyImageSubData(window_pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0, window_pixmap.dst_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0, window_pixmap.texture_width, window_pixmap.texture_height, 0);
            glBindTexture(GL_TEXTURE_2D, window_pixmap.dst_texture_id);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, window_pixmap.texture_width, window_pixmap.texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, ff);

            AVCodecContext *codec_context = video_stream->codec;
            if(av_hwframe_get_buffer(codec_context->hw_frames_ctx, frame, 0) < 0) {
                fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
                exit(1);
            }

            // Get context
            CUresult res;
            CUcontext old_ctx;
            res = cuCtxPopCurrent(&old_ctx);
            res = cuCtxPushCurrent(*cuda_context);

            // Get texture
            res = cuGraphicsResourceSetMapFlags(cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
            res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

            // Map texture to cuda array
            CUarray mapped_array;
            res = cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

            // Release texture
            res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

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
            res = cuCtxPopCurrent(&old_ctx);

            frame->pts = frame_count++;
            if(avcodec_send_frame(video_stream->codec, frame) < 0) {
                fprintf(stderr, "Error: avcodec_send_frame failed\n");
            }
            receive_frames(video_stream->codec, video_stream, av_format_context, output_file);
        }
    }
#endif
#if 0
    //avcodec_register_all();
    AVCodec *av_codec = avcodec_find_encoder_by_name("h264_nvenc"); //avcodec_find_encoder(AV_CODEC_ID_H264);
    if(!av_codec) {
        fprintf(stderr, "Error: No encoder was found for codec h264\n");
        return 1;
    }

    AVOutputFormat *format = av_guess_format("mp4", nullptr, nullptr);
    if(!format) {
        fprintf(stderr, "Error: Invalid format: mp4\n");
        return 1;
    }
    AVFormatContext *av_format_context = avformat_alloc_context();
    av_format_context->oformat = format;
    if(avio_open(&av_format_context->pb, "test_new.mp4", AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "Error: Failed to open output file: test_new.mp4");
        return 1;
    }

    AVStream *stream = avformat_new_stream(av_format_context, av_codec);
    if(!stream) {
        fprintf(stderr, "Error: Failed to create stream\n");
        return 1;
    }
    stream->id = av_format_context->nb_streams - 1;

    AVCodecContext *av_codec_context = avcodec_alloc_context3(av_codec);
    if(avcodec_get_context_defaults3(av_codec_context, av_codec) < 0) {
		fprintf(stderr, "Error: Failed to get av codec context defaults\n");
        return 1;
	}
	av_codec_context->codec_id = av_codec->id;
	av_codec_context->codec_type = av_codec->type;
    if(av_format_context->oformat->flags & AVFMT_GLOBALHEADER)
        av_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    av_codec_context->time_base.num = 1;
    av_codec_context->time_base.den = 60;
    av_codec_context->gop_size = 12;
    av_codec_context->bit_rate = 400000;
    av_codec_context->width = 720; // window_pixmap.texture_width
    av_codec_context->height = 480; // window_pixmap.texture_height
#if SSR_USE_AVSTREAM_TIME_BASE
	stream->time_base = codec_context->time_base;
#endif
	av_codec_context->sample_aspect_ratio.num = 1;
	av_codec_context->sample_aspect_ratio.den = 1;
    av_codec_context->pix_fmt = AV_PIX_FMT_CUDA;
    av_codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
    av_codec_context->sw_pix_fmt = AV_PIX_FMT_0BGR32;
	stream->sample_aspect_ratio = av_codec_context->sample_aspect_ratio;

    std::vector<std::string> hardware_accelerated_devices = get_hardware_acceleration_device_names();
    if(hardware_accelerated_devices.empty()) {
        fprintf(stderr, "Error: No hardware accelerated device was found on your system\n");
        return 1;
    }

    AVBufferRef *device_ctx = nullptr;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_CUDA, hardware_accelerated_devices[0].c_str(), NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context for gpu: %s\n", hardware_accelerated_devices[0].c_str());
        return 1;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    CUcontext *cuda_context = &(cuda_device_context->cuda_ctx);
    if(!cuda_context) {
        fprintf(stderr, "Error: No cuda context\n");
        return 1;
    }
    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = window_pixmap.texture_width;
    hw_frame_context->height = window_pixmap.texture_height;
    hw_frame_context->sw_format = AV_PIX_FMT_0BGR32;
    hw_frame_context->format = AV_PIX_FMT_CUDA;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    if(av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context (note: ffmpeg version needs to be > 4.0\n");
        return 1;
    }

    av_codec_context->hw_device_ctx = device_ctx;
    av_codec_context->hw_frames_ctx = frame_context;

    if(avcodec_open2(av_codec_context, av_codec, nullptr) < 0) {
        fprintf(stderr, "Error: avcodec_open2 failed\n");
        return 1;
    }

    if(avcodec_parameters_from_context(stream->codecpar, av_codec_context) < 0) {
        fprintf(stderr, "Error: Can't copy parameters to stream!\n");
        return 1;
    }

    //AVDictionary *opts = nullptr;
    //av_dict_set(&opts, "b", "2.5M", 0);
    // if(avcodec_open2(av_codec_context, av_codec, nullptr) < 0) {
    //     fprintf(stderr, "Error: avcodec_open2 failed\n");
    //     return 1;
    // }

    CUresult res;
    CUcontext old_ctx;
    CUgraphicsResource cuda_graphics_resource;
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);
    res = cuGraphicsGLRegisterImage(&cuda_graphics_resource, window_pixmap.dst_texture_id, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if(res != CUDA_SUCCESS) {
        fprintf(stderr, "Error: cuGraphicsGLRegisterImage failed, error %d, texture id: %u\n", res, window_pixmap.texture_id);
        return 1;
    }
    res = cuCtxPopCurrent(&old_ctx);
    

    AVFrame *rgb_frame = av_frame_alloc();
    if(av_hwframe_get_buffer(frame_context, rgb_frame, 0) < 0) {
        fprintf(stderr, "Error: av_hwframe_get_buffer failed\n");
        return 1;
    }


    // Get context
    res = cuCtxPopCurrent(&old_ctx);
    res = cuCtxPushCurrent(*cuda_context);

    // Get texture
    res = cuGraphicsResourceSetMapFlags(cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
    res = cuGraphicsMapResources(1, &cuda_graphics_resource, 0);

    // Map texture to cuda array
    CUarray mapped_array;
    res = cuGraphicsSubResourceGetMappedArray(&mapped_array, cuda_graphics_resource, 0, 0);

    // Release texture
    res = cuGraphicsUnmapResources(1, &cuda_graphics_resource, 0);

    CUDA_MEMCPY2D memcpy_struct;
    memcpy_struct.srcXInBytes = 0;
    memcpy_struct.srcY = 0;
    memcpy_struct.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_ARRAY;
    
    memcpy_struct.dstXInBytes = 0;
    memcpy_struct.dstY = 0;
    memcpy_struct.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;

    memcpy_struct.srcArray = mapped_array;
    memcpy_struct.dstDevice = (CUdeviceptr)rgb_frame->data[0];
    memcpy_struct.dstPitch = rgb_frame->linesize[0];
    memcpy_struct.WidthInBytes = rgb_frame->width * 4;
    memcpy_struct.Height = rgb_frame->height;
    cuMemcpy2D(&memcpy_struct);

    // Release context
    res = cuCtxPopCurrent(&old_ctx);

    if(avformat_write_header(av_format_context, NULL) != 0) {
		fprintf(stderr, "Error: Failed to write header\n");
        return 1;
	}

    if(avcodec_send_frame(av_codec_context, rgb_frame) < 0) {
        fprintf(stderr, "Error: avcodec_send_frame failed\n");
    }

    //av_frame_free(&rgb_frame);
    //avcodec_close(av_codec_context);

    XSelectInput(dpy, src_window_id, StructureNotifyMask);

    int damage_event;
    int damage_error;
    if(!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        fprintf(stderr, "Error: XDamage is not supported by your X11 server\n");
        return 1;
    }

    Damage xdamage = XDamageCreate(dpy, src_window_id, XDamageReportNonEmpty);

    XEvent e;
    while (1) {
        XNextEvent(dpy, &e);
        if (e.type == ConfigureNotify) {
            // Window resize
            printf("Resize window!\n");
            recreate_window_pixmap(dpy, src_window_id, window_pixmap);
        } else if (e.type == damage_event + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent*)&e;
            // de->drawable is the window ID of the damaged window
            XserverRegion region = XFixesCreateRegion(dpy, nullptr, 0);
            // Subtract all the damage, repairing the window
            XDamageSubtract(dpy, de->damage, None, region);
            XFixesDestroyRegion(dpy, region);

            glCopyImageSubData(window_pixmap.texture_id, GL_TEXTURE_2D, 0, 0, 0, 0, window_pixmap.dst_texture_id, GL_TEXTURE_2D, 0, 0, 0, 0, window_pixmap.texture_width, window_pixmap.texture_height, 0);
            res = cuCtxPopCurrent(&old_ctx);
            res = cuCtxPushCurrent(*cuda_context);
            CUDA_MEMCPY2D memcpy_struct;
            memcpy_struct.srcXInBytes = 0;
            memcpy_struct.srcY = 0;
            memcpy_struct.srcMemoryType = CUmemorytype::CU_MEMORYTYPE_ARRAY;
            
            memcpy_struct.dstXInBytes = 0;
            memcpy_struct.dstY = 0;
            memcpy_struct.dstMemoryType = CUmemorytype::CU_MEMORYTYPE_DEVICE;

            memcpy_struct.srcArray = mapped_array;
            memcpy_struct.dstDevice = (CUdeviceptr)rgb_frame->data[0];
            memcpy_struct.dstPitch = rgb_frame->linesize[0];
            memcpy_struct.WidthInBytes = rgb_frame->width * 4;
            memcpy_struct.Height = rgb_frame->height;
            cuMemcpy2D(&memcpy_struct);
            res = cuCtxPopCurrent(&old_ctx);
            if(avcodec_send_frame(av_codec_context, rgb_frame) < 0) {
                fprintf(stderr, "Error: avcodec_send_frame failed\n");
            }
            receive_frames(av_codec_context, stream, av_format_context);
        }
    }

    if(av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    close_video(video_stream, frame);

    if(!(output_format->fmt & AVFMT_NOFILE))
        avio_close(output_format->pb);
    avformat_free_context(av_format_context);

    XDamageDestroy(dpy, xdamage);
    av_buffer_unref(&device_ctx);
    avcodec_free_context(&av_codec_context);
#else
    if(av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    close_video(video_stream, frame);

    if(!(output_format->flags & AVFMT_NOFILE))
        avio_close(av_format_context->pb);
    avformat_free_context(av_format_context);
    XDamageDestroy(dpy, xdamage);
#endif
    //cleanup_window_pixmap(dpy, window_pixmap);
    for(int i = 0; i < screen_count; ++i) {
        XCompositeUnredirectSubwindows(dpy, RootWindow(dpy, i), CompositeRedirectAutomatic);
    }
    XCloseDisplay(dpy);
}
