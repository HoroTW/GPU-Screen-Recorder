#ifndef GSR_CAPTURE_XCOMPOSITE_DRM_H
#define GSR_CAPTURE_XCOMPOSITE_DRM_H

#include "capture.h"
#include "../vec2.h"
#include <X11/X.h>

typedef struct _XDisplay Display;

typedef struct {
    Window window;
    bool follow_focused; /* If this is set then |window| is ignored */
    vec2i region_size; /* This is currently only used with |follow_focused| */
} gsr_capture_xcomposite_drm_params;

gsr_capture* gsr_capture_xcomposite_drm_create(const gsr_capture_xcomposite_drm_params *params);

#endif /* GSR_CAPTURE_XCOMPOSITE_DRM_H */
