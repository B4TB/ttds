#include "rendering.h"

#include "backends/drm.h"

const struct rendering_vtable supported_backends[BACKEND_COUNT] = {
	[BACKEND_DRM] = {
	    .rendering_init = drm_rendering_init,
	    .rendering_cleanup = drm_rendering_cleanup,
	    .rendering_ctx_log = drm_rendering_ctx_log,
	    .rendering_show = drm_rendering_show,
	    .canvas_init = drm_canvas_init,
	}
};

// as a reminder to update this array when adding backends
static_assert(BACKEND_COUNT == 1);
