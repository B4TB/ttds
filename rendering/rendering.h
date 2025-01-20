#pragma once

#include "canvas.h"

struct rendering_vtable {
	void *(*rendering_init)(void);
	void (*rendering_cleanup)(void *r_ctx);
	void (*rendering_ctx_log)(const void *r_ctx);
	void (*rendering_show)(void *r_ctx, struct canvas *);
	struct canvas *(*canvas_init)(void *r_ctx);
};

struct rendering_vtable drm_backend_new(void);
