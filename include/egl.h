#ifndef GSR_EGL_H
#define GSR_EGL_H

/* OpenGL EGL library with a hidden window context (to allow using the opengl functions) */

#include <X11/X.h>
#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN64
typedef signed   long long int khronos_intptr_t;
typedef unsigned long long int khronos_uintptr_t;
typedef signed   long long int khronos_ssize_t;
typedef unsigned long long int khronos_usize_t;
#else
typedef signed   long  int     khronos_intptr_t;
typedef unsigned long  int     khronos_uintptr_t;
typedef signed   long  int     khronos_ssize_t;
typedef unsigned long  int     khronos_usize_t;
#endif

typedef void* EGLDisplay;
typedef void* EGLNativeDisplayType;
typedef uintptr_t EGLNativeWindowType;
typedef uintptr_t EGLNativePixmapType;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLClientBuffer;
typedef void* EGLImage;
typedef void* EGLImageKHR;
typedef void *GLeglImageOES;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);

#define EGL_BUFFER_SIZE                         0x3020
#define EGL_RENDERABLE_TYPE                     0x3040
#define EGL_OPENGL_ES2_BIT                      0x0004
#define EGL_NONE                                0x3038
#define EGL_CONTEXT_CLIENT_VERSION              0x3098
#define EGL_BACK_BUFFER                         0x3084

#define GL_TEXTURE_2D                           0x0DE1
#define GL_RGB                                  0x1907
#define GL_UNSIGNED_BYTE                        0x1401
#define GL_COLOR_BUFFER_BIT                     0x00004000
#define GL_TEXTURE_WRAP_S                       0x2802
#define GL_TEXTURE_WRAP_T                       0x2803
#define GL_TEXTURE_MAG_FILTER                   0x2800
#define GL_TEXTURE_MIN_FILTER                   0x2801
#define GL_TEXTURE_WIDTH                        0x1000
#define GL_TEXTURE_HEIGHT                       0x1001
#define GL_NEAREST                              0x2600
#define GL_CLAMP_TO_EDGE                        0x812F
#define GL_LINEAR                               0x2601
#define GL_FRAMEBUFFER                          0x8D40
#define GL_COLOR_ATTACHMENT0                    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE                 0x8CD5
#define GL_STATIC_DRAW                          0x88E4
#define GL_ARRAY_BUFFER                         0x8892

#define GL_VENDOR                               0x1F00
#define GL_RENDERER                             0x1F01

#define GLX_BUFFER_SIZE                         2
#define GLX_DOUBLEBUFFER                        5
#define GLX_RED_SIZE                            8
#define GLX_GREEN_SIZE                          9
#define GLX_BLUE_SIZE                           10
#define GLX_ALPHA_SIZE                          11
#define GLX_DEPTH_SIZE                          12

#define GLX_RGBA_BIT                            0x00000001
#define GLX_RENDER_TYPE                         0x8011
#define GLX_FRONT_EXT                           0x20DE
#define GLX_BIND_TO_TEXTURE_RGB_EXT             0x20D0
#define GLX_DRAWABLE_TYPE                       0x8010
#define GLX_WINDOW_BIT                          0x00000001
#define GLX_PIXMAP_BIT                          0x00000002
#define GLX_BIND_TO_TEXTURE_TARGETS_EXT         0x20D3
#define GLX_TEXTURE_2D_BIT_EXT                  0x00000002
#define GLX_TEXTURE_TARGET_EXT                  0x20D6
#define GLX_TEXTURE_2D_EXT                      0x20DC
#define GLX_TEXTURE_FORMAT_EXT                  0x20D5
#define GLX_TEXTURE_FORMAT_RGB_EXT              0x20D9
#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB  0x00000002
#define GLX_CONTEXT_MAJOR_VERSION_ARB           0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB           0x2092
#define GLX_CONTEXT_FLAGS_ARB                   0x2094

typedef struct {
    void *egl_library;
    void *gl_library;
    Display *dpy;
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLContext egl_context;
    Window window;

    EGLDisplay (*eglGetDisplay)(EGLNativeDisplayType display_id);
    unsigned int (*eglInitialize)(EGLDisplay dpy, int32_t *major, int32_t *minor);
    unsigned int (*eglTerminate)(EGLDisplay dpy);
    unsigned int (*eglChooseConfig)(EGLDisplay dpy, const int32_t *attrib_list, EGLConfig *configs, int32_t config_size, int32_t *num_config);
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const int32_t *attrib_list);
    EGLContext (*eglCreateContext)(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const int32_t *attrib_list);
    unsigned int (*eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    EGLSurface (*eglCreatePixmapSurface)(EGLDisplay dpy, EGLConfig config, EGLNativePixmapType pixmap, const int32_t *attrib_list);
    EGLImage (*eglCreateImage)(EGLDisplay dpy, EGLContext ctx, unsigned int target, EGLClientBuffer buffer, const intptr_t *attrib_list);
    unsigned int (*eglDestroyContext)(EGLDisplay dpy, EGLContext ctx);
    unsigned int (*eglDestroySurface)(EGLDisplay dpy, EGLSurface surface);
    unsigned int (*eglDestroyImage)(EGLDisplay dpy, EGLImage image);
    unsigned int (*eglBindTexImage)(EGLDisplay dpy, EGLSurface surface, int32_t buffer);
    unsigned int (*eglSwapInterval)(EGLDisplay dpy, int32_t interval);
    unsigned int (*eglSwapBuffers)(EGLDisplay dpy, EGLSurface surface);
    __eglMustCastToProperFunctionPointerType (*eglGetProcAddress)(const char *procname);

    unsigned int (*eglExportDMABUFImageQueryMESA)(EGLDisplay dpy, EGLImageKHR image, int *fourcc, int *num_planes, uint64_t *modifiers);
    unsigned int (*eglExportDMABUFImageMESA)(EGLDisplay dpy, EGLImageKHR image, int *fds, int32_t *strides, int32_t *offsets);
    void (*glEGLImageTargetTexture2DOES)(unsigned int target, GLeglImageOES image);

    unsigned int (*glGetError)(void);
    const unsigned char* (*glGetString)(unsigned int name);
    void (*glClear)(unsigned int mask);
    void (*glClearColor)(float red, float green, float blue, float alpha);
    void (*glGenTextures)(int n, unsigned int *textures);
    void (*glDeleteTextures)(int n, const unsigned int *texture);
    void (*glBindTexture)(unsigned int target, unsigned int texture);
    void (*glTexParameteri)(unsigned int target, unsigned int pname, int param);
    void (*glGetTexLevelParameteriv)(unsigned int target, int level, unsigned int pname, int *params);
    void (*glTexImage2D)(unsigned int target, int level, int internalFormat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels);
    void (*glCopyImageSubData)(unsigned int srcName, unsigned int srcTarget, int srcLevel, int srcX, int srcY, int srcZ, unsigned int dstName, unsigned int dstTarget, int dstLevel, int dstX, int dstY, int dstZ, int srcWidth, int srcHeight, int srcDepth);
    void (*glClearTexImage)(unsigned int texture, unsigned int level, unsigned int format, unsigned int type, const void *data);
    void (*glGenFramebuffers)(int n, unsigned int *framebuffers);
    void (*glBindFramebuffer)(unsigned int target, unsigned int framebuffer);
    void (*glViewport)(int x, int y, int width, int height);
    void (*glFramebufferTexture2D)(unsigned int target, unsigned int attachment, unsigned int textarget, unsigned int texture, int level);
    void (*glDrawBuffers)(int n, const unsigned int *bufs);
    unsigned int (*glCheckFramebufferStatus)(unsigned int target);
    void (*glBindBuffer)(unsigned int target, unsigned int buffer);
    void (*glGenBuffers)(int n, unsigned int *buffers);
    void (*glBufferData)(unsigned int target, khronos_ssize_t size, const void *data, unsigned int usage);
    int (*glGetUniformLocation)(unsigned int program, const char *name);
    void (*glGenVertexArrays)(int n, unsigned int *arrays);
    void (*glBindVertexArray)(unsigned int array);

    unsigned int (*glCreateProgram)(void);
    unsigned int (*glCreateShader)(unsigned int type);
    void (*glAttachShader)(unsigned int program, unsigned int shader);
    void (*glBindAttribLocation)(unsigned int program, unsigned int index, const char *name);
    void (*glCompileShader)(unsigned int shader);
    void (*glLinkProgram)(unsigned int program);
    void (*glShaderSource)(unsigned int shader, int count, const char *const*string, const int *length);
    void (*glUseProgram)(unsigned int program);
    void (*glGetProgramInfoLog)(unsigned int program, int bufSize, int *length, char *infoLog);
    void (*glGetShaderiv)(unsigned int shader, unsigned int pname, int *params);
    void (*glGetShaderInfoLog)(unsigned int shader, int bufSize, int *length, char *infoLog);
    void (*glGetShaderSource)(unsigned int shader, int bufSize, int *length, char *source);
    void (*glDeleteProgram)(unsigned int program);
    void (*glDeleteShader)(unsigned int shader);
    void (*glGetProgramiv)(unsigned int program, unsigned int pname, int *params);
    void (*glVertexAttribPointer)(unsigned int index, int size, unsigned int type, unsigned char normalized, int stride, const void *pointer);
    void (*glEnableVertexAttribArray)(unsigned int index);
    void (*glDrawArrays)(unsigned int mode, int first, int count );
    void (*glReadBuffer)( unsigned int mode );
    void (*glReadPixels)(int x, int y,
                                    int width, int height,
                                    unsigned int format, unsigned int type,
                                    void *pixels );
} gsr_egl;

bool gsr_egl_load(gsr_egl *self, Display *dpy);
void gsr_egl_unload(gsr_egl *self);

#endif /* GSR_EGL_H */
