#ifndef GSR_GL_H
#define GSR_GL_H

/* OpenGL library with a hidden window context (to allow using the opengl functions) */

#include <X11/X.h>
#include <X11/Xutil.h>
#include <stdbool.h>

typedef XID GLXPixmap;
typedef XID GLXDrawable;
typedef XID GLXWindow;

typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;

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
    void *library;
    Display *dpy;
    GLXFBConfig *fbconfigs;
    XVisualInfo *visual_info;
    GLXFBConfig fbconfig;
    Colormap colormap;
    GLXContext gl_context;
    Window window;

    GLXPixmap (*glXCreatePixmap)(Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attribList);
    void (*glXDestroyPixmap)(Display *dpy, GLXPixmap pixmap);
    void (*glXBindTexImageEXT)(Display *dpy, GLXDrawable drawable, int buffer, const int *attrib_list);
    void (*glXReleaseTexImageEXT)(Display *dpy, GLXDrawable drawable, int buffer);
    GLXFBConfig* (*glXChooseFBConfig)(Display *dpy, int screen, const int *attribList, int *nitems);
    XVisualInfo* (*glXGetVisualFromFBConfig)(Display *dpy, GLXFBConfig config);
    GLXContext (*glXCreateContextAttribsARB)(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list);
    Bool (*glXMakeContextCurrent)(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx);
    void (*glXDestroyContext)(Display *dpy, GLXContext ctx);
    void (*glXSwapBuffers)(Display *dpy, GLXDrawable drawable);

    void (*glXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);
    int (*glXSwapIntervalMESA)(unsigned int interval);
    int (*glXSwapIntervalSGI)(int interval);

    void (*glClearTexImage)(unsigned int texture, unsigned int level, unsigned int format, unsigned int type, const void *data);

    unsigned int (*glGetError)(void);
    const unsigned char* (*glGetString)(unsigned int name);
    void (*glClear)(unsigned int mask);
    void (*glGenTextures)(int n, unsigned int *textures);
    void (*glDeleteTextures)(int n, const unsigned int *texture);
    void (*glBindTexture)(unsigned int target, unsigned int texture);
    void (*glTexParameteri)(unsigned int target, unsigned int pname, int param);
    void (*glGetTexLevelParameteriv)(unsigned int target, int level, unsigned int pname, int *params);
    void (*glTexImage2D)(unsigned int target, int level, int internalFormat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels);
    void (*glCopyImageSubData)(unsigned int srcName, unsigned int srcTarget, int srcLevel, int srcX, int srcY, int srcZ, unsigned int dstName, unsigned int dstTarget, int dstLevel, int dstX, int dstY, int dstZ, int srcWidth, int srcHeight, int srcDepth);
} gsr_gl;

bool gsr_gl_load(gsr_gl *self, Display *dpy);
bool gsr_gl_make_context_current(gsr_gl *self);
void gsr_gl_unload(gsr_gl *self);

#endif /* GSR_GL_H */
