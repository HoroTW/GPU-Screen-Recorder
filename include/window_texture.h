#ifndef WINDOW_TEXTURE_H
#define WINDOW_TEXTURE_H

#include "gl.h"

typedef struct {
    Display *display;
    Window window;
    Pixmap pixmap;
    GLXPixmap glx_pixmap;
    unsigned int texture_id;
    int redirected;
    gsr_gl *gl;
} WindowTexture;

/* Returns 0 on success */
int window_texture_init(WindowTexture *window_texture, Display *display, Window window, gsr_gl *gl);
void window_texture_deinit(WindowTexture *self);

/*
    This should ONLY be called when the target window is resized.
    Returns 0 on success.
*/
int window_texture_on_resize(WindowTexture *self);

unsigned int window_texture_get_opengl_texture_id(WindowTexture *self);

#endif /* WINDOW_TEXTURE_H */
