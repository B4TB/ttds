#pragma once

#include "canvas.h"

#include <stddef.h>
#include <stdint.h>

struct rendering_ctx;

struct rendering_ctx *rendering_init(void);
void rendering_cleanup(struct rendering_ctx *);

void rendering_ctx_log(const struct rendering_ctx *);

struct canvas *canvas_init(struct rendering_ctx *);

void rendering_show(struct rendering_ctx *, struct canvas *);
