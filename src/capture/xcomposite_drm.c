#include "../../include/capture/xcomposite_drm.h"
#include "../../include/egl.h"
#include "../../include/window_texture.h"
#include "../../include/time.h"
#include <stdlib.h>
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
//#include <drm_fourcc.h>
#include <assert.h>
/* TODO: Proper error checks and cleanups */

typedef struct {
    gsr_capture_xcomposite_drm_params params;
    Display *dpy;
    XEvent xev;
    bool created_hw_frame;

    vec2i window_pos;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;

    gsr_egl egl;

    int fourcc;
    int num_planes;
    uint64_t modifiers;
    int dmabuf_fd;
    int32_t stride;
    int32_t offset;

    unsigned int target_texture_id;

    unsigned int FramebufferName;
    unsigned int quad_VertexArrayID;
    unsigned int quad_vertexbuffer;
    unsigned int quadVAO;
} gsr_capture_xcomposite_drm;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static bool drm_create_codec_context(gsr_capture_xcomposite_drm *cap_xcomp, AVCodecContext *video_codec_context) {
    // TODO: "/dev/dri/card0"
    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/card0", NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_YUV420P;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_YUV420P;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = device_ctx; // TODO: av_buffer_ref? and in more places
    video_codec_context->hw_frames_ctx = frame_context;
    return true;
}

#define EGL_SURFACE_TYPE                  0x3033
#define EGL_WINDOW_BIT                    0x0004
#define EGL_PIXMAP_BIT                    0x0002
#define EGL_BIND_TO_TEXTURE_RGB           0x3039
#define EGL_TRUE                          1
#define EGL_RED_SIZE                      0x3024
#define EGL_GREEN_SIZE                    0x3023
#define EGL_BLUE_SIZE                     0x3022
#define EGL_ALPHA_SIZE                    0x3021
#define EGL_TEXTURE_FORMAT                0x3080
#define EGL_TEXTURE_RGB                   0x305D
#define EGL_TEXTURE_TARGET                0x3081
#define EGL_TEXTURE_2D                    0x305F
#define EGL_GL_TEXTURE_2D                 0x30B1

#define GL_RGBA                           0x1908

static unsigned int gl_create_texture(gsr_capture_xcomposite_drm *cap_xcomp, int width, int height) {
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
    cap_xcomp->egl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    cap_xcomp->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

#define GL_COMPILE_STATUS                 0x8B81
#define GL_INFO_LOG_LENGTH                0x8B84

unsigned int esLoadShader ( gsr_capture_xcomposite_drm *cap_xcomp, unsigned int type, const char *shaderSrc ) {
   unsigned int shader;
   int compiled;
   
   // Create the shader object
   shader = cap_xcomp->egl.glCreateShader ( type );

   if ( shader == 0 )
   	return 0;

   // Load the shader source
   cap_xcomp->egl.glShaderSource ( shader, 1, &shaderSrc, NULL );
   
   // Compile the shader
   cap_xcomp->egl.glCompileShader ( shader );

   // Check the compile status
   cap_xcomp->egl.glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

   if ( !compiled ) 
   {
      int infoLen = 0;

      cap_xcomp->egl.glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
         char* infoLog = malloc (sizeof(char) * infoLen );

         cap_xcomp->egl.glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
         fprintf (stderr, "Error compiling shader:\n%s\n", infoLog );            
         
         free ( infoLog );
      }

      cap_xcomp->egl.glDeleteShader ( shader );
      return 0;
   }

   return shader;

}

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82


//
///
/// \brief Load a vertex and fragment shader, create a program object, link program.
//         Errors output to log.
/// \param vertShaderSrc Vertex shader source code
/// \param fragShaderSrc Fragment shader source code
/// \return A new program object linked with the vertex/fragment shader pair, 0 on failure
//
unsigned int esLoadProgram ( gsr_capture_xcomposite_drm *cap_xcomp, const char *vertShaderSrc, const char *fragShaderSrc )
{
   unsigned int vertexShader;
   unsigned int fragmentShader;
   unsigned int programObject;
   int linked;

   // Load the vertex/fragment shaders
   vertexShader = esLoadShader ( cap_xcomp, GL_VERTEX_SHADER, vertShaderSrc );
   if ( vertexShader == 0 )
      return 0;

   fragmentShader = esLoadShader ( cap_xcomp, GL_FRAGMENT_SHADER, fragShaderSrc );
   if ( fragmentShader == 0 )
   {
      cap_xcomp->egl.glDeleteShader( vertexShader );
      return 0;
   }

   // Create the program object
   programObject = cap_xcomp->egl.glCreateProgram ( );
   
   if ( programObject == 0 )
      return 0;

   cap_xcomp->egl.glAttachShader ( programObject, vertexShader );
   cap_xcomp->egl.glAttachShader ( programObject, fragmentShader );

   // Link the program
   cap_xcomp->egl.glLinkProgram ( programObject );

   // Check the link status
   cap_xcomp->egl.glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

   if ( !linked ) 
   {
      int infoLen = 0;

      cap_xcomp->egl.glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
         char* infoLog = malloc (sizeof(char) * infoLen );

         cap_xcomp->egl.glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
         fprintf (stderr, "Error linking program:\n%s\n", infoLog );            
         
         free ( infoLog );
      }

      cap_xcomp->egl.glDeleteProgram ( programObject );
      return 0;
   }

   // Free up no longer needed shader resources
   cap_xcomp->egl.glDeleteShader ( vertexShader );
   cap_xcomp->egl.glDeleteShader ( fragmentShader );

   return programObject;
}

static unsigned int shader_program = 0;
static unsigned int texID = 0;

static void LoadShaders(gsr_capture_xcomposite_drm *cap_xcomp) {
	char vShaderStr[] =
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
		"void main()                                     \n"
		"{                                               \n"
        "  texcoords_out = texcoords;                    \n"
		"  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);   \n"
		"}                                               \n";

#if 0
	char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"


        "float imageWidth = 1920.0;\n"
        "float imageHeight = 1080.0;\n"

        "float getYPixel(vec2 position) {\n"
        "    position.y = (position.y * 2.0 / 3.0) + (1.0 / 3.0);\n"
        "    return texture2D(tex, position).x;\n"
        "}\n"
"\n"
        "vec2 mapCommon(vec2 position, float planarOffset) {\n"
        "    planarOffset += (imageWidth * floor(position.y / 2.0)) / 2.0 +\n"
        "                    floor((imageWidth - 1.0 - position.x) / 2.0);\n"
        "    float x = floor(imageWidth - 1.0 - floor(mod(planarOffset, imageWidth)));\n"
        "    float y = floor(floor(planarOffset / imageWidth));\n"
        "    return vec2((x + 0.5) / imageWidth, (y + 0.5) / (1.5 * imageHeight));\n"
        "}\n"
"\n"
        "vec2 mapU(vec2 position) {\n"
        "    float planarOffset = (imageWidth * imageHeight) / 4.0;\n"
        "    return mapCommon(position, planarOffset);\n"
        "}\n"
"\n"
        "vec2 mapV(vec2 position) {\n"
        "    return mapCommon(position, 0.0);\n"
        "}\n"

		"void main()                                               \n"
		"{                                                         \n"

        "vec2 pixelPosition = vec2(floor(imageWidth * texcoords_out.x),\n"
        "                        floor(imageHeight * texcoords_out.y));\n"
        "pixelPosition -= vec2(0.5, 0.5);\n"
"\n"
        "float yChannel = getYPixel(texcoords_out);\n"
        "float uChannel = texture2D(tex, mapU(pixelPosition)).x;\n"
        "float vChannel = texture2D(tex, mapV(pixelPosition)).x;\n"
        "vec4 channels = vec4(yChannel, uChannel, vChannel, 1.0);\n"
        "mat4 conversion = mat4(1.0,  0.0,    1.402, -0.701,\n"
        "                        1.0, -0.344, -0.714,  0.529,\n"
        "                        1.0,  1.772,  0.0,   -0.886,\n"
        "                        0, 0, 0, 0);\n"
        "vec3 rgb = (channels * conversion).xyz;\n"

		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#elif 1
    char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"
		"void main()                                               \n"
		"{                                                         \n"
        "  vec3 rgb = texture(tex, texcoords_out).rgb;             \n"
		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#else
    char fShaderStr[] =
        "#version 300 es                                           \n"
		"precision mediump float;                                  \n"
        "in vec2 texcoords_out;                                        \n"
        "uniform sampler2D tex;                                    \n"
        "out vec4 FragColor;                                       \n"

        "vec3 rgb2yuv(vec3 rgb){\n"
        "    float y = 0.299*rgb.r + 0.587*rgb.g + 0.114*rgb.b;\n"
        "    return vec3(y, 0.493*(rgb.b-y), 0.877*(rgb.r-y));\n"
        "}\n"

        "vec3 yuv2rgb(vec3 yuv){\n"
        "    float y = yuv.x;\n"
        "    float u = yuv.y;\n"
        "    float v = yuv.z;\n"
        "    \n"
        "    return vec3(\n"
        "        y + 1.0/0.877*v,\n"
        "        y - 0.39393*u - 0.58081*v,\n"
        "        y + 1.0/0.493*u\n"
        "    );\n"
        "}\n"

		"void main()                                               \n"
		"{                                                         \n"
        "   float s = 0.5;\n"
        "    vec3 lum = texture(tex, texcoords_out).rgb;\n"
        "    vec3 chr = texture(tex, floor(texcoords_out*s-.5)/s).rgb;\n"
        "    vec3 rgb = vec3(rgb2yuv(lum).x, rgb2yuv(chr).yz);\n"
		"  FragColor = vec4(rgb, 1.0);                            \n"
		"}                                                         \n";
#endif

    shader_program = esLoadProgram(cap_xcomp, vShaderStr, fShaderStr);
	if (shader_program == 0) {
        fprintf(stderr, "failed to create shader!\n");
        return;
    }

    cap_xcomp->egl.glBindAttribLocation(shader_program, 0, "pos");
    cap_xcomp->egl.glBindAttribLocation(shader_program, 1, "texcoords");
	return;
}

#define GL_FLOAT				0x1406
#define GL_FALSE				0
#define GL_TRUE					1
#define GL_TRIANGLES				0x0004
#define DRM_FORMAT_MOD_INVALID 72057594037927935

static int gsr_capture_xcomposite_drm_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    XWindowAttributes attr;
    if(!XGetWindowAttributes(cap_xcomp->dpy, cap_xcomp->params.window, &attr)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start failed: invalid window id: %lu\n", cap_xcomp->params.window);
        return -1;
    }

    cap_xcomp->window_size.x = max_int(attr.width, 0);
    cap_xcomp->window_size.y = max_int(attr.height, 0);
    Window c;
    XTranslateCoordinates(cap_xcomp->dpy, cap_xcomp->params.window, DefaultRootWindow(cap_xcomp->dpy), 0, 0, &cap_xcomp->window_pos.x, &cap_xcomp->window_pos.y, &c);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(cap_xcomp->dpy, cap_xcomp->params.window, StructureNotifyMask | ExposureMask);

    if(!gsr_egl_load(&cap_xcomp->egl, cap_xcomp->dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed to load opengl\n");
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: could not find eglExportDMABUFImageQueryMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    if(!cap_xcomp->egl.eglExportDMABUFImageMESA) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: could not find eglExportDMABUFImageMESA\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    /* Disable vsync */
    cap_xcomp->egl.eglSwapInterval(cap_xcomp->egl.egl_display, 0);
#if 0
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->gl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
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
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed to create opengl texture\n");
        gsr_capture_xcomposite_stop(cap, video_codec_context);
        return -1;
    }

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    cap_xcomp->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
#else
    // TODO: Fallback to composite window
    if(window_texture_init(&cap_xcomp->window_texture, cap_xcomp->dpy, cap_xcomp->params.window, &cap_xcomp->egl) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed get window texture for window %ld\n", cap_xcomp->params.window);
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->texture_size.x = 0;
    cap_xcomp->texture_size.y = 0;
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cap_xcomp->texture_size.x);
    cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &cap_xcomp->texture_size.y);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    #if 1
    cap_xcomp->target_texture_id = gl_create_texture(cap_xcomp, cap_xcomp->texture_size.x, cap_xcomp->texture_size.y);
    if(cap_xcomp->target_texture_id == 0) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_start: failed to create opengl texture\n");
        return -1;
    }
    #else
    // TODO:
    cap_xcomp->target_texture_id = window_texture_get_opengl_texture_id(&cap_xcomp->window_texture);
    #endif

    cap_xcomp->texture_size.x = max_int(2, cap_xcomp->texture_size.x & ~1);
    cap_xcomp->texture_size.y = max_int(2, cap_xcomp->texture_size.y & ~1);

    video_codec_context->width = cap_xcomp->texture_size.x;
    video_codec_context->height = cap_xcomp->texture_size.y;

    {
        EGLImage img = cap_xcomp->egl.eglCreateImage(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_context, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uint64_t)cap_xcomp->target_texture_id, NULL);
        if(!img) {
            fprintf(stderr, "eglCreateImage failed\n");
            return -1;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageQueryMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->fourcc, &cap_xcomp->num_planes, &cap_xcomp->modifiers) || cap_xcomp->modifiers == DRM_FORMAT_MOD_INVALID) {
            fprintf(stderr, "eglExportDMABUFImageQueryMESA failed\n"); 
            return -1;
        }

        if(cap_xcomp->num_planes != 1) {
            // TODO: FAIL!
            fprintf(stderr, "Blablalba\n");
            return -1;
        }

        if(!cap_xcomp->egl.eglExportDMABUFImageMESA(cap_xcomp->egl.egl_display, img, &cap_xcomp->dmabuf_fd, &cap_xcomp->stride, &cap_xcomp->offset)) {
            fprintf(stderr, "eglExportDMABUFImageMESA failed\n");
            return -1;
        }

        fprintf(stderr, "texture: %u, dmabuf: %d, stride: %d, offset: %d\n", cap_xcomp->target_texture_id, cap_xcomp->dmabuf_fd, cap_xcomp->stride, cap_xcomp->offset);
        fprintf(stderr, "fourcc: %d, num planes: %d, modifiers: %zu\n", cap_xcomp->fourcc, cap_xcomp->num_planes, cap_xcomp->modifiers);
    }

    cap_xcomp->egl.glGenFramebuffers(1, &cap_xcomp->FramebufferName);
    cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, cap_xcomp->FramebufferName);

    cap_xcomp->egl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cap_xcomp->target_texture_id, 0);

    // Set the list of draw buffers.
    unsigned int DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
    cap_xcomp->egl.glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers

    if(cap_xcomp->egl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "Failed to setup framebuffer\n");
        return -1;
    }

    cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, 0);

    //cap_xcomp->egl.glGenVertexArrays(1, &cap_xcomp->quad_VertexArrayID);
    //cap_xcomp->egl.glBindVertexArray(cap_xcomp->quad_VertexArrayID);

    static const float g_quad_vertex_buffer_data[] = {
        -1.0f, -1.0f, 0.0f,
        1.0f, -1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,
        1.0f, -1.0f, 0.0f,
        1.0f,  1.0f, 0.0f,
    };

    //cap_xcomp->egl.glGenBuffers(1, &cap_xcomp->quad_vertexbuffer);
    //cap_xcomp->egl.glBindBuffer(GL_ARRAY_BUFFER, cap_xcomp->quad_vertexbuffer);
    //cap_xcomp->egl.glBufferData(GL_ARRAY_BUFFER, sizeof(g_quad_vertex_buffer_data), g_quad_vertex_buffer_data, GL_STATIC_DRAW);

    // Create and compile our GLSL program from the shaders
    LoadShaders(cap_xcomp);
    texID = cap_xcomp->egl.glGetUniformLocation(shader_program, "tex");
    fprintf(stderr, "uniform id: %u\n", texID);

    float vVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,

        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    unsigned int quadVBO;
    cap_xcomp->egl.glGenVertexArrays(1, &cap_xcomp->quadVAO);
    cap_xcomp->egl.glGenBuffers(1, &quadVBO);
    cap_xcomp->egl.glBindVertexArray(cap_xcomp->quadVAO);
    cap_xcomp->egl.glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    cap_xcomp->egl.glBufferData(GL_ARRAY_BUFFER, sizeof(vVertices), &vVertices, GL_STATIC_DRAW);

    cap_xcomp->egl.glEnableVertexAttribArray(0);
    cap_xcomp->egl.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    cap_xcomp->egl.glEnableVertexAttribArray(1);
    cap_xcomp->egl.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    cap_xcomp->egl.glBindVertexArray(0);

    //cap_xcomp->egl.glUniform1i(texID, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));

    //cap_xcomp->egl.glViewport(0, 0, 1920, 1080);

    //cap_xcomp->egl.glBindBuffer(GL_ARRAY_BUFFER, 0);
    //cap_xcomp->egl.glBindVertexArray(0);

    if(!drm_create_codec_context(cap_xcomp, video_codec_context)) {
        fprintf(stderr, "failed to create hw codec context\n");
        gsr_egl_unload(&cap_xcomp->egl);
        return -1;
    }

    fprintf(stderr, "sneed: %u\n", cap_xcomp->FramebufferName);
    return 0;
#endif
}

// TODO:
static void free_desc(void *opaque, uint8_t *data) {
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*)data;
    int i;

    //for (i = 0; i < desc->nb_objects; i++)
    //    close(desc->objects[i].fd);

    av_free(desc);
}


static void gsr_capture_xcomposite_drm_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;

    cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

    if(!cap_xcomp->created_hw_frame) {
        cap_xcomp->created_hw_frame = true;

        /*if(av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0) < 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_tick: av_hwframe_get_buffer failed\n");
            return;
        }*/

        AVDRMFrameDescriptor *desc = av_malloc(sizeof(AVDRMFrameDescriptor));
        if(!desc) {
            fprintf(stderr, "poop\n");
            return;
        }

        fprintf(stderr, "tick fd: %d\n", cap_xcomp->dmabuf_fd);

        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, cap_xcomp->target_texture_id);
        int xx = 0;
        int yy = 0;
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &xx);
        cap_xcomp->egl.glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &yy);
        cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

        *desc = (AVDRMFrameDescriptor) {
            .nb_objects = 1,
            .objects[0] = {
                .fd               = cap_xcomp->dmabuf_fd,
                .size             = yy * cap_xcomp->stride,
                .format_modifier  = cap_xcomp->modifiers,
            },
            .nb_layers = 1,
            .layers[0] = {
                .format           = cap_xcomp->fourcc, // DRM_FORMAT_NV12
                .nb_planes        = 1, //cap_xcomp->num_planes, // TODO: Ensure this is 1, otherwise ffmpeg cant handle it in av_hwframe_map
                .planes[0] = {
                    .object_index = 0,
                    .offset       = cap_xcomp->offset,
                    .pitch        = cap_xcomp->stride,
                },
            },
        };

        #if 0
        AVBufferRef *device_ctx;
        if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_DRM, "/dev/dri/card0", NULL, 0) < 0) {
            fprintf(stderr, "Error: Failed to create hardware device context\n");
            return;
        }

        AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
        if(!frame_context) {
            fprintf(stderr, "Error: Failed to create hwframe context\n");
            av_buffer_unref(&device_ctx);
            return;
        }

        AVHWFramesContext *hw_frame_context =
            (AVHWFramesContext *)frame_context->data;
        hw_frame_context->width = video_codec_context->width;
        hw_frame_context->height = video_codec_context->height;
        hw_frame_context->sw_format = AV_PIX_FMT_0RGB32;
        hw_frame_context->format = AV_PIX_FMT_DRM_PRIME;
        hw_frame_context->device_ref = device_ctx;
        hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

        if (av_hwframe_ctx_init(frame_context) < 0) {
            fprintf(stderr, "Error: Failed to initialize hardware frame context "
                            "(note: ffmpeg version needs to be > 4.0)\n");
            av_buffer_unref(&device_ctx);
            av_buffer_unref(&frame_context);
            return;
        }
        #endif

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: failed to allocate frame\n");
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = AVCOL_RANGE_JPEG;

        int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0);
        if(res < 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: av_hwframe_get_buffer failed 1: %d\n", res);
            return;
        }

        AVFrame *src_frame = av_frame_alloc();
        assert(src_frame);
        src_frame->format = AV_PIX_FMT_DRM_PRIME;
        src_frame->width = video_codec_context->width;
        src_frame->height = video_codec_context->height;
        src_frame->color_range = AVCOL_RANGE_JPEG;

        src_frame->buf[0] = av_buffer_create((uint8_t*)desc, sizeof(*desc),
                                     &free_desc, video_codec_context, 0);
        if (!src_frame->buf[0]) {
            fprintf(stderr, "failed to create buffer!\n");
            return;
        }

        src_frame->data[0] = (uint8_t*)desc;
        src_frame->extended_data = src_frame->data;
        src_frame->format  = AV_PIX_FMT_DRM_PRIME;

        res = av_hwframe_map(*frame, src_frame, AV_HWFRAME_MAP_DIRECT);
        if(res < 0) {
            fprintf(stderr, "av_hwframe_map failed: %d\n", res);
        }

        // Clear texture with black background because the source texture (window_texture_get_opengl_texture_id(&cap_xcomp->window_texture))
        // might be smaller than cap_xcomp->target_texture_id
        cap_xcomp->egl.glClearTexImage(cap_xcomp->target_texture_id, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
}

static bool gsr_capture_xcomposite_drm_should_stop(gsr_capture *cap, bool *err) {
    return false;
}

#define GL_FLOAT				0x1406
#define GL_FALSE				0
#define GL_TRUE					1
#define GL_TRIANGLES				0x0004

void FBO_2_PPM_file(gsr_capture_xcomposite_drm *cap_xcomp, int output_width, int output_height)
{
    FILE    *output_image;

    /// READ THE PIXELS VALUES from FBO AND SAVE TO A .PPM FILE
    int             i, j, k;
    unsigned char   *pixels = (unsigned char*)malloc(output_width*output_height*3);

    unsigned int err = cap_xcomp->egl.glGetError();
    fprintf(stderr, "opengl err 1: %u\n", err);

    /// READ THE CONTENT FROM THE FBO
    cap_xcomp->egl.glReadBuffer(GL_COLOR_ATTACHMENT0);

    err = cap_xcomp->egl.glGetError();
    fprintf(stderr, "opengl err 2: %u\n", err);

    cap_xcomp->egl.glReadPixels(0, 0, output_width, output_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    err = cap_xcomp->egl.glGetError();
    fprintf(stderr, "opengl err 3: %u\n", err);

    output_image = fopen("output.ppm", "wb");
    fprintf(output_image,"P3\n");
    fprintf(output_image,"# Created by Ricao\n");
    fprintf(output_image,"%d %d\n",output_width,output_height);
    fprintf(output_image,"255\n");

    k = 0;
    for(i=0; i<output_width; i++)
    {
        for(j=0; j<output_height; j++)
        {
            fprintf(output_image,"%u %u %u ",(unsigned int)pixels[k],(unsigned int)pixels[k+1],
                                             (unsigned int)pixels[k+2]);
            k = k+4;
        }
        fprintf(output_image,"\n");
    }
    free(pixels);
    fclose(output_image);
}

static int gsr_capture_xcomposite_drm_capture(gsr_capture *cap, AVFrame *frame) {
    gsr_capture_xcomposite_drm *cap_xcomp = cap->priv;
    vec2i source_size = cap_xcomp->texture_size;

    #if 1
    /* TODO: Remove this copy, which is only possible by using nvenc directly and encoding window_pixmap.target_texture_id */
    cap_xcomp->egl.glCopyImageSubData(
        window_texture_get_opengl_texture_id(&cap_xcomp->window_texture), GL_TEXTURE_2D, 0, 0, 0, 0,
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
    #elif 0
    cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, cap_xcomp->FramebufferName);
    cap_xcomp->egl.glViewport(0, 0, 1920, 1080);
    //cap_xcomp->egl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

    cap_xcomp->egl.glUseProgram(shader_program);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&cap_xcomp->window_texture));
    cap_xcomp->egl.glBindVertexArray(cap_xcomp->quadVAO);
    cap_xcomp->egl.glDrawArrays(GL_TRIANGLES, 0, 6);
    cap_xcomp->egl.glBindTexture(GL_TEXTURE_2D, 0);

    static int counter = 0;
    ++counter;
    static bool image_saved = false;
    if(!image_saved && counter == 5) {
        image_saved = true;
        FBO_2_PPM_file(cap_xcomp, 1920, 1080);
        fprintf(stderr, "saved image!\n");
    }

    cap_xcomp->egl.glBindVertexArray(0);
    cap_xcomp->egl.glUseProgram(0);
    cap_xcomp->egl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    #endif
    cap_xcomp->egl.eglSwapBuffers(cap_xcomp->egl.egl_display, cap_xcomp->egl.egl_surface);

    return 0;
}

static void gsr_capture_xcomposite_drm_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    if(cap->priv) {
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_drm_create(const gsr_capture_xcomposite_drm_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite_drm *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite_drm));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_drm_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_xcomp);
        return NULL;
    }

    cap_xcomp->dpy = display;
    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_drm_start,
        .tick = gsr_capture_xcomposite_drm_tick,
        .should_stop = gsr_capture_xcomposite_drm_should_stop,
        .capture = gsr_capture_xcomposite_drm_capture,
        .destroy = gsr_capture_xcomposite_drm_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
