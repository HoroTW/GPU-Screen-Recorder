#include "../include/window_texture.h"
#include <X11/extensions/Xcomposite.h>
#include <stdio.h>

static int x11_supports_composite_named_window_pixmap(Display *display) {
    int extension_major;
    int extension_minor;
    if(!XCompositeQueryExtension(display, &extension_major, &extension_minor))
        return 0;

    int major_version;
    int minor_version;
    return XCompositeQueryVersion(display, &major_version, &minor_version) && (major_version > 0 || minor_version >= 2);
}

int window_texture_init(WindowTexture *window_texture, Display *display, Window window, gsr_gl *gl) {
    window_texture->display = display;
    window_texture->window = window;
    window_texture->pixmap = None;
    window_texture->glx_pixmap = None;
    window_texture->texture_id = 0;
    window_texture->redirected = 0;
    window_texture->gl = gl;
    
    if(!x11_supports_composite_named_window_pixmap(display))
        return 1;

    XCompositeRedirectWindow(display, window, CompositeRedirectAutomatic);
    window_texture->redirected = 1;
    return window_texture_on_resize(window_texture);
}

static void window_texture_cleanup(WindowTexture *self, int delete_texture) {
    if(delete_texture && self->texture_id) {
        self->gl->glDeleteTextures(1, &self->texture_id);
        self->texture_id = 0;
    }

    if(self->glx_pixmap) {
        self->gl->glXDestroyPixmap(self->display, self->glx_pixmap);
        self->gl->glXReleaseTexImageEXT(self->display, self->glx_pixmap, GLX_FRONT_EXT);
        self->glx_pixmap = None;
    }

    if(self->pixmap) {
        XFreePixmap(self->display, self->pixmap);
        self->pixmap = None;
    }
}

void window_texture_deinit(WindowTexture *self) {
    if(self->redirected) {
        XCompositeUnredirectWindow(self->display, self->window, CompositeRedirectAutomatic);
        self->redirected = 0;
    }
    window_texture_cleanup(self, 1);
}

int window_texture_on_resize(WindowTexture *self) {
    window_texture_cleanup(self, 0);

    int result = 0;
    GLXFBConfig *configs = NULL;
    Pixmap pixmap = None;
    GLXPixmap glx_pixmap = None;
    unsigned int texture_id = 0;
    int glx_pixmap_bound = 0;

    const int pixmap_config[] = {
        GLX_BIND_TO_TEXTURE_RGB_EXT, True,
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT | GLX_WINDOW_BIT,
        GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
        /*GLX_BIND_TO_MIPMAP_TEXTURE_EXT, True,*/
        GLX_BUFFER_SIZE, 24,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 0,
        None
    };

    const int pixmap_attribs[] = {
        GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
        GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGB_EXT,
        /*GLX_MIPMAP_TEXTURE_EXT, True,*/
        None
    };

    XWindowAttributes attr;
    if (!XGetWindowAttributes(self->display, self->window, &attr)) {
        fprintf(stderr, "Failed to get window attributes\n");
        return 1;
    }

    GLXFBConfig config;
    int c;
    configs = self->gl->glXChooseFBConfig(self->display, 0, pixmap_config, &c);
    if(!configs) {
        fprintf(stderr, "Failed to choose fb config\n");
        return 1;
    }

    int found = 0;
    for (int i = 0; i < c; i++) {
        config = configs[i];
        XVisualInfo *visual = self->gl->glXGetVisualFromFBConfig(self->display, config);
        if (!visual)
            continue;

        if (attr.depth != visual->depth) {
            XFree(visual);
            continue;
        }
        XFree(visual);
        found = 1;
        break;
    }

    if(!found) {
        fprintf(stderr, "No matching fb config found\n");
        result = 1;
        goto cleanup;
    }

    pixmap = XCompositeNameWindowPixmap(self->display, self->window);
    if(!pixmap) {
        result = 2;
        goto cleanup;
    }

    glx_pixmap = self->gl->glXCreatePixmap(self->display, config, pixmap, pixmap_attribs);
    if(!glx_pixmap) {
        result = 3;
        goto cleanup;
    }

    if(self->texture_id == 0) {
        self->gl->glGenTextures(1, &texture_id);
        if(texture_id == 0) {
            result = 4;
            goto cleanup;
        }
        self->gl->glBindTexture(GL_TEXTURE_2D, texture_id);
    } else {
        self->gl->glBindTexture(GL_TEXTURE_2D, self->texture_id);
    }

    self->gl->glXBindTexImageEXT(self->display, glx_pixmap, GLX_FRONT_EXT, NULL);
    glx_pixmap_bound = 1;

    self->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    self->gl->glBindTexture(GL_TEXTURE_2D, 0);

    XFree(configs);
    self->pixmap = pixmap;
    self->glx_pixmap = glx_pixmap;
    if(texture_id != 0)
        self->texture_id = texture_id;
    return 0;

    cleanup:
    if(texture_id != 0)     self->gl->glDeleteTextures(1, &texture_id);
    if(glx_pixmap)          self->gl->glXDestroyPixmap(self->display, glx_pixmap);
    if(glx_pixmap_bound)    self->gl->glXReleaseTexImageEXT(self->display, glx_pixmap, GLX_FRONT_EXT);
    if(pixmap)              XFreePixmap(self->display, pixmap);
    if(configs)             XFree(configs);
    return result;
}

unsigned int window_texture_get_opengl_texture_id(WindowTexture *self) {
    return self->texture_id;
}
