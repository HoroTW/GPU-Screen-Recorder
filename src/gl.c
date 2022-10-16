#include "../include/gl.h"
#include "../include/library_loader.h"
#include <string.h>

static bool gsr_gl_create_window(gsr_gl *self) {
    const int attr[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 0,
        None
    };

    GLXFBConfig *fbconfigs = NULL;
    XVisualInfo *visual_info = NULL;
    GLXFBConfig fbconfig = NULL;
    Colormap colormap = None;
    GLXContext gl_context = NULL;
    Window window = None;

    int numfbconfigs = 0;
    fbconfigs = self->glXChooseFBConfig(self->dpy, DefaultScreen(self->dpy), attr, &numfbconfigs);
    for(int i = 0; i < numfbconfigs; i++) {
        visual_info = self->glXGetVisualFromFBConfig(self->dpy, fbconfigs[i]);
        if(!visual_info)
            continue;

        fbconfig = fbconfigs[i];
        break;
    }

    if(!visual_info) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: no appropriate visual found\n");
        XFree(fbconfigs);
        return false;
    }

    /* TODO: Core profile? GLX_CONTEXT_CORE_PROFILE_BIT_ARB. */
    /* TODO: Remove need for 4.2 when copy texture function has been removed. */
    int context_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        GLX_CONTEXT_MINOR_VERSION_ARB, 2,
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        None
    };

    gl_context = self->glXCreateContextAttribsARB(self->dpy, fbconfig, NULL, True, context_attribs);
    if(!gl_context) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create gl context\n");
        goto fail;
    }

    colormap = XCreateColormap(self->dpy, DefaultRootWindow(self->dpy), visual_info->visual, AllocNone);
    if(!colormap) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create x11 colormap\n");
        goto fail;
    }

    XSetWindowAttributes window_attr;
    window_attr.colormap = colormap;

    // TODO: Is there a way to remove the need to create a window?
    window = XCreateWindow(self->dpy, DefaultRootWindow(self->dpy), 0, 0, 1, 1, 0, visual_info->depth, InputOutput, visual_info->visual, CWColormap, &window_attr);

    if(!window) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create gl window\n");
        goto fail;
    }

    if(!self->glXMakeContextCurrent(self->dpy, window, window, gl_context)) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to make gl context current\n");
        goto fail;
    }

    self->fbconfigs = fbconfigs;
    self->visual_info = visual_info;
    self->colormap = colormap;
    self->gl_context = gl_context;
    self->window = window;
    return true;

    fail:
    if(window)
        XDestroyWindow(self->dpy, window);
    if(colormap)
        XFreeColormap(self->dpy, colormap);
    if(gl_context)
        self->glXDestroyContext(self->dpy, gl_context);
    if(visual_info)
        XFree(visual_info);
    XFree(fbconfigs);
    return False;
}

bool gsr_gl_load(gsr_gl *self, Display *dpy) {
    memset(self, 0, sizeof(gsr_gl));
    self->dpy = dpy;

    dlerror(); /* clear */
    void *lib = dlopen("libGL.so.1", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: gsr_gl_load: failed to load libGL.so.1, error: %s\n", dlerror());
        return false;
    }

    dlsym_assign optional_dlsym[] = {
        { (void**)&self->glClearTexImage, "glClearTexImage" },
        { (void**)&self->glXSwapIntervalEXT, "glXSwapIntervalEXT" },
        { (void**)&self->glXSwapIntervalMESA, "glXSwapIntervalMESA" },
        { (void**)&self->glXSwapIntervalSGI, "glXSwapIntervalSGI" },

        { NULL, NULL }
    };

    dlsym_load_list_optional(lib, optional_dlsym);

    dlsym_assign required_dlsym[] = {
        { (void**)&self->glXCreatePixmap, "glXCreatePixmap" },
        { (void**)&self->glXDestroyPixmap, "glXDestroyPixmap" },
        { (void**)&self->glXBindTexImageEXT, "glXBindTexImageEXT" },
        { (void**)&self->glXReleaseTexImageEXT, "glXReleaseTexImageEXT" },
        { (void**)&self->glXChooseFBConfig, "glXChooseFBConfig" },
        { (void**)&self->glXGetVisualFromFBConfig, "glXGetVisualFromFBConfig" },
        { (void**)&self->glXCreateContextAttribsARB, "glXCreateContextAttribsARB" },
        { (void**)&self->glXMakeContextCurrent, "glXMakeContextCurrent" },
        { (void**)&self->glXDestroyContext, "glXDestroyContext" },
        { (void**)&self->glXSwapBuffers, "glXSwapBuffers" },

        { (void**)&self->glGetError, "glGetError" },
        { (void**)&self->glGetString, "glGetString" },
        { (void**)&self->glClear, "glClear" },
        { (void**)&self->glGenTextures, "glGenTextures" },
        { (void**)&self->glDeleteTextures, "glDeleteTextures" },
        { (void**)&self->glBindTexture, "glBindTexture" },
        { (void**)&self->glTexParameteri, "glTexParameteri" },
        { (void**)&self->glGetTexLevelParameteriv, "glGetTexLevelParameteriv" },
        { (void**)&self->glTexImage2D, "glTexImage2D" },
        { (void**)&self->glCopyImageSubData, "glCopyImageSubData" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(lib, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_gl_load failed: missing required symbols in libGL.so.1\n");
        dlclose(lib);
        memset(self, 0, sizeof(gsr_gl));
        return false;
    }

    if(!gsr_gl_create_window(self)) {
        dlclose(lib);
        memset(self, 0, sizeof(gsr_gl));
        return false;
    }

    self->library = lib;
    return true;
}

bool gsr_gl_make_context_current(gsr_gl *self) {
    return self->glXMakeContextCurrent(self->dpy, self->window, self->window, self->gl_context);
}

void gsr_gl_unload(gsr_gl *self) {
    if(self->window) {
        XDestroyWindow(self->dpy, self->window);
        self->window = None;
    }

    if(self->colormap) {
        XFreeColormap(self->dpy, self->colormap);
        self->colormap = None;
    }

    if(self->gl_context) {
        self->glXDestroyContext(self->dpy, self->gl_context);
        self->gl_context = NULL;
    }

    if(self->visual_info) {
        XFree(self->visual_info);
        self->visual_info = NULL;
    }

    if(self->fbconfigs) {
        XFree(self->fbconfigs);
        self->fbconfigs = NULL;
    }

    if(self->library) {
        dlclose(self->library);
        memset(self, 0, sizeof(gsr_gl));
    }
}
