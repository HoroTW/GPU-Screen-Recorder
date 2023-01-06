#include "../include/window_texture.h"
#include <X11/extensions/Xcomposite.h>

#define EGL_TRUE                          1
#define EGL_IMAGE_PRESERVED_KHR           0x30D2
#define EGL_NATIVE_PIXMAP_KHR             0x30B0

static int x11_supports_composite_named_window_pixmap(Display *display) {
    int extension_major;
    int extension_minor;
    if(!XCompositeQueryExtension(display, &extension_major, &extension_minor))
        return 0;

    int major_version;
    int minor_version;
    return XCompositeQueryVersion(display, &major_version, &minor_version) && (major_version > 0 || minor_version >= 2);
}

int window_texture_init(WindowTexture *window_texture, Display *display, Window window, gsr_egl *egl) {
    window_texture->display = display;
    window_texture->window = window;
    window_texture->pixmap = None;
    window_texture->texture_id = 0;
    window_texture->redirected = 0;
    window_texture->egl = egl;
    
    if(!x11_supports_composite_named_window_pixmap(display))
        return 1;

    XCompositeRedirectWindow(display, window, CompositeRedirectAutomatic);
    window_texture->redirected = 1;
    return window_texture_on_resize(window_texture);
}

static void window_texture_cleanup(WindowTexture *self, int delete_texture) {
    if(delete_texture && self->texture_id) {
        self->egl->glDeleteTextures(1, &self->texture_id);
        self->texture_id = 0;
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
    Pixmap pixmap = None;
    unsigned int texture_id = 0;
    EGLImage image = NULL;

    const intptr_t pixmap_attrs[] = {
		EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
		EGL_NONE,
	};

    pixmap = XCompositeNameWindowPixmap(self->display, self->window);
    if(!pixmap) {
        result = 2;
        goto cleanup;
    }

    if(self->texture_id == 0) {
        self->egl->glGenTextures(1, &texture_id);
        if(texture_id == 0) {
            result = 3;
            goto cleanup;
        }
        self->egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    } else {
        self->egl->glBindTexture(GL_TEXTURE_2D, self->texture_id);
        texture_id = self->texture_id;
    }

    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    image = self->egl->eglCreateImage(self->egl->egl_display, NULL, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)pixmap, pixmap_attrs);
    if(!image) {
        result = 4;
        goto cleanup;
    }

    self->egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    if(self->egl->glGetError() != 0) {
        result = 5;
        goto cleanup;
    }

    self->pixmap = pixmap;
    self->texture_id = texture_id;

    cleanup:
    self->egl->glBindTexture(GL_TEXTURE_2D, 0);

    if(image)
        self->egl->eglDestroyImage(self->egl->egl_display, image);

    if(result != 0) {
        if(texture_id != 0)
            self->egl->glDeleteTextures(1, &texture_id);
        if(pixmap)
            XFreePixmap(self->display, pixmap);
    }

    return result;
}

unsigned int window_texture_get_opengl_texture_id(WindowTexture *self) {
    return self->texture_id;
}