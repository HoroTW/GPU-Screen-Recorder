#include "../include/egl.h"
#include "../include/library_loader.h"
#include <string.h>

static bool gsr_egl_create_window(gsr_egl *self) {
    EGLDisplay egl_display = NULL;
    EGLConfig  ecfg;
    int32_t     num_config;
    EGLSurface egl_surface;
    EGLContext egl_context;
    
    int32_t attr[] = {
        EGL_BUFFER_SIZE, 24,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    int32_t ctxattr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    // TODO: Is there a way to remove the need to create a window?
    Window window = XCreateWindow(self->dpy, DefaultRootWindow(self->dpy), 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent, 0, NULL);

    if(!window) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create gl window\n");
        goto fail;
    }

    egl_display = self->eglGetDisplay(self->dpy);
    if(!egl_display) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglGetDisplay failed\n");
        return false;
    }

    if(!self->eglInitialize(egl_display, NULL, NULL)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglInitialize failed\n");
        return false;
    }
    
    // TODO: Cleanup ecfg?
    if (!self->eglChooseConfig( egl_display, attr, &ecfg, 1, &num_config ) ) {
        //cerr << "Failed to choose config (eglError: " << eglGetError() << ")" << endl;
        return false;
    }
    
    if ( num_config != 1 ) {
        //cerr << "Didn't get exactly one config, but " << num_config << endl;
        return false;
    }
    
    egl_surface = self->eglCreateWindowSurface ( egl_display, ecfg, (EGLNativeWindowType)window, NULL );
    if ( !egl_surface ) {
        //cerr << "Unable to create EGL surface (eglError: " << eglGetError() << ")" << endl;
        return false;
    }
    
    //// egl-contexts collect all state descriptions needed required for operation
    egl_context = self->eglCreateContext ( egl_display, ecfg, NULL, ctxattr );
    if ( !egl_context ) {
        //cerr << "Unable to create EGL context (eglError: " << eglGetError() << ")" << endl;
        return false;
    }
    
    //// associate the egl-context with the egl-surface
    self->eglMakeCurrent( egl_display, egl_surface, egl_surface, egl_context );

    self->egl_display = egl_display;
    self->egl_surface = egl_surface;
    self->egl_context = egl_context;
    self->window = window;
    return true;

    fail:
    // TODO:
    /*
    if(window)
        XDestroyWindow(self->dpy, window);
    if(colormap)
        XFreeColormap(self->dpy, colormap);
    if(gl_context)
        self->glXDestroyContext(self->dpy, gl_context);
    if(visual_info)
        XFree(visual_info);
    XFree(fbconfigs);
    */
    return False;
}

static bool gsr_egl_load_egl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->eglGetDisplay, "eglGetDisplay" },
        { (void**)&self->eglInitialize, "eglInitialize" },
        { (void**)&self->eglChooseConfig, "eglChooseConfig" },
        { (void**)&self->eglCreateWindowSurface, "eglCreateWindowSurface" },
        { (void**)&self->eglCreateContext, "eglCreateContext" },
        { (void**)&self->eglMakeCurrent, "eglMakeCurrent" },
        { (void**)&self->eglCreatePixmapSurface, "eglCreatePixmapSurface" },
        { (void**)&self->eglCreateImage, "eglCreateImage" }, // TODO: eglCreateImageKHR
        { (void**)&self->eglBindTexImage, "eglBindTexImage" },
        { (void**)&self->eglSwapInterval, "eglSwapInterval" },
        { (void**)&self->eglSwapBuffers, "eglSwapBuffers" },
        { (void**)&self->eglGetProcAddress, "eglGetProcAddress" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libEGL.so.1\n");
        return false;
    }

    return true;
}

static bool gsr_egl_mesa_load_egl(gsr_egl *self) {
    self->eglExportDMABUFImageQueryMESA = self->eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    self->eglExportDMABUFImageMESA = self->eglGetProcAddress("eglExportDMABUFImageMESA");
    self->glEGLImageTargetTexture2DOES = self->eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if(!self->eglExportDMABUFImageQueryMESA) {
        fprintf(stderr, "could not find eglExportDMABUFImageQueryMESA\n");
        return false;
    }

    if(!self->eglExportDMABUFImageMESA) {
        fprintf(stderr, "could not find eglExportDMABUFImageMESA\n");
        return false;
    }

    if(!self->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "could not find glEGLImageTargetTexture2DOES\n");
        return false;
    }

    return true;
}

static bool gsr_egl_load_gl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->glGetError, "glGetError" },
        { (void**)&self->glGetString, "glGetString" },
        { (void**)&self->glClear, "glClear" },
        { (void**)&self->glClearColor, "glClearColor" },
        { (void**)&self->glGenTextures, "glGenTextures" },
        { (void**)&self->glDeleteTextures, "glDeleteTextures" },
        { (void**)&self->glBindTexture, "glBindTexture" },
        { (void**)&self->glTexParameteri, "glTexParameteri" },
        { (void**)&self->glGetTexLevelParameteriv, "glGetTexLevelParameteriv" },
        { (void**)&self->glTexImage2D, "glTexImage2D" },
        { (void**)&self->glCopyImageSubData, "glCopyImageSubData" },
        { (void**)&self->glGenFramebuffers, "glGenFramebuffers" },
        { (void**)&self->glBindFramebuffer, "glBindFramebuffer" },
        { (void**)&self->glViewport, "glViewport" },
        { (void**)&self->glFramebufferTexture2D, "glFramebufferTexture2D" },
        { (void**)&self->glDrawBuffers, "glDrawBuffers" },
        { (void**)&self->glCheckFramebufferStatus, "glCheckFramebufferStatus" },
        { (void**)&self->glBindBuffer, "glBindBuffer" },
        { (void**)&self->glGenBuffers, "glGenBuffers" },
        { (void**)&self->glBufferData, "glBufferData" },
        { (void**)&self->glGetUniformLocation, "glGetUniformLocation" },
        { (void**)&self->glGenVertexArrays, "glGenVertexArrays" },
        { (void**)&self->glBindVertexArray, "glBindVertexArray" },
        { (void**)&self->glCreateProgram, "glCreateProgram" },
        { (void**)&self->glCreateShader, "glCreateShader" },
        { (void**)&self->glAttachShader, "glAttachShader" },
        { (void**)&self->glBindAttribLocation, "glBindAttribLocation" },
        { (void**)&self->glCompileShader, "glCompileShader" },
        { (void**)&self->glLinkProgram, "glLinkProgram" },
        { (void**)&self->glShaderSource, "glShaderSource" },
        { (void**)&self->glUseProgram, "glUseProgram" },
        { (void**)&self->glGetProgramInfoLog, "glGetProgramInfoLog" },
        { (void**)&self->glGetShaderiv, "glGetShaderiv" },
        { (void**)&self->glGetShaderInfoLog, "glGetShaderInfoLog" },
        { (void**)&self->glGetShaderSource, "glGetShaderSource" },
        { (void**)&self->glDeleteProgram, "glDeleteProgram" },
        { (void**)&self->glDeleteShader, "glDeleteShader" },
        { (void**)&self->glGetProgramiv, "glGetProgramiv" },
        { (void**)&self->glVertexAttribPointer, "glVertexAttribPointer" },
        { (void**)&self->glEnableVertexAttribArray, "glEnableVertexAttribArray" },
        { (void**)&self->glDrawArrays, "glDrawArrays" },
        { (void**)&self->glReadBuffer, "glReadBuffer" },
        { (void**)&self->glReadPixels, "glReadPixels" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libGL.so.1\n");
        return false;
    }

    return true;
}

bool gsr_egl_load(gsr_egl *self, Display *dpy) {
    memset(self, 0, sizeof(gsr_egl));
    self->dpy = dpy;

    dlerror(); /* clear */
    void *egl_lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libEGL.so.1, error: %s\n", dlerror());
        return false;
    }

    void *gl_lib = dlopen("libGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libGL.so.1, error: %s\n", dlerror());
        dlclose(egl_lib);
        memset(self, 0, sizeof(gsr_egl));
        return false;
    }

    if(!gsr_egl_load_egl(self, egl_lib)) {
        dlclose(egl_lib);
        dlclose(gl_lib);
        memset(self, 0, sizeof(gsr_egl));
        return false;
    }

    if(!gsr_egl_load_gl(self, gl_lib)) {
        dlclose(egl_lib);
        dlclose(gl_lib);
        memset(self, 0, sizeof(gsr_egl));
        return false;
    }

    if(!gsr_egl_mesa_load_egl(self)) {
        dlclose(egl_lib);
        dlclose(gl_lib);
        memset(self, 0, sizeof(gsr_egl));
        return false;
    }

    if(!gsr_egl_create_window(self)) {
        dlclose(egl_lib);
        dlclose(gl_lib);
        memset(self, 0, sizeof(gsr_egl));
        return false;
    }

    self->egl_library = egl_lib;
    self->gl_library = gl_lib;
    return true;
}

bool gsr_egl_make_context_current(gsr_egl *self) {
    // TODO:
    return true;
    //return self->glXMakeContextCurrent(self->dpy, self->window, self->window, self->gl_context);
}

void gsr_egl_unload(gsr_egl *self) {
    // TODO: Cleanup of egl resources

    if(self->window) {
        XDestroyWindow(self->dpy, self->window);
        self->window = None;
    }

    if(self->egl_library) {
        dlclose(self->egl_library);
        self->egl_library = NULL;
    }

    if(self->gl_library) {
        dlclose(self->gl_library);
        self->gl_library = NULL;
    }

    memset(self, 0, sizeof(gsr_egl));
}
