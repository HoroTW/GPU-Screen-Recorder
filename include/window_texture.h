#ifndef WINDOW_TEXTURE_H
#define WINDOW_TEXTURE_H

#include "egl.h"

typedef struct {
    Display *display;
    Window window;
    Pixmap pixmap;
    unsigned int texture_id;
    unsigned int target_texture_id;
    int texture_width;
    int texture_height;
    int redirected;
    gsr_egl *egl;
} WindowTexture;

/* Returns 0 on success */
int window_texture_init(WindowTexture *window_texture, Display *display, Window window, gsr_egl *egl);
void window_texture_deinit(WindowTexture *self);

/*
    This should ONLY be called when the target window is resized.
    Returns 0 on success.
*/
int window_texture_on_resize(WindowTexture *self);

unsigned int window_texture_get_opengl_texture_id(WindowTexture *self);

#endif /* WINDOW_TEXTURE_H */
