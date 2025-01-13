#pragma once

#include <stddef.h>
#include <stdint.h>

/* Encode a color. Alpha is assumed to always be 0xff. */
struct color {
	uint8_t r, g, b;
};

struct rect {
	uint16_t x, y, w, h;
};

struct circle {
	uint16_t x, y;
	uint16_t r;
};

struct canvas;
struct rendering_ctx;

struct rendering_ctx *rendering_init(void);
void rendering_cleanup(struct rendering_ctx *);

void rendering_ctx_log(const struct rendering_ctx *);

struct canvas *canvas_init(struct rendering_ctx *);
void canvas_deinit(struct canvas *);

void rendering_fill(struct canvas *, struct color);
void rendering_draw_rect(struct canvas *, const struct rect *, struct color);

void rendering_draw_circle(
    struct canvas *, const struct circle *, struct color);

void rendering_show(struct rendering_ctx *, struct canvas *);
