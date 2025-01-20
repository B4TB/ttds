#include "rendering.h"

#include "backends/drm.h"

struct rendering_vtable drm_backend_new(void)
{
	return (struct rendering_vtable) {
		.rendering_init = drm_rendering_init,
		.rendering_cleanup = drm_rendering_cleanup,
		.rendering_ctx_log = drm_rendering_ctx_log,
		.rendering_show = drm_rendering_show,
		.canvas_init = drm_canvas_init,
	};
}
