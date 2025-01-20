#include "canvas.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void draw_point(struct canvas *, uint16_t x, uint16_t y, struct color);

void canvas_deinit(struct canvas *c)
{
	free(c->buffer);
	free(c);
}

void rendering_fill(struct canvas *c, struct color color)
{
	// Color space is little endian, thus the BGRA format used below:
	for (uint16_t x = 0; x < c->width; x++)
		for (uint16_t y = 0; y < c->height; y++)
			draw_point(c, x, y, color);
}

void rendering_draw_rect(
    struct canvas *c, const struct rect *rect, struct color color)
{
	uint16_t right_edge = rect->x + rect->w;
	uint16_t bottom_edge = rect->y + rect->h;
	for (uint16_t y = rect->y; y < bottom_edge && y < bottom_edge; y++)
		for (uint16_t x = rect->x; x < right_edge && x < c->width; x++)
			draw_point(c, x, y, color);
}

void rendering_draw_circle(
    struct canvas *c, const struct circle *circle, struct color color)
{
	uint16_t x = circle->r;
	uint16_t y = 0;
	int32_t t1 = circle->r / 16;

	while (x >= y) {
		uint16_t eff_y = circle->y + y;
		for (uint16_t eff_x = circle->x + x; eff_x >= circle->x;
		    eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - y;
		for (uint16_t eff_x = circle->x + x; eff_x >= circle->x;
		    eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - y;
		for (uint16_t eff_x = circle->x - x; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + y;
		for (uint16_t eff_x = circle->x - x; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + x;
		for (uint16_t eff_x = circle->x + y; eff_x >= circle->x;
		    eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - x;
		for (uint16_t eff_x = circle->x + y; eff_x >= circle->x;
		    eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - x;
		for (uint16_t eff_x = circle->x - y; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + x;
		for (uint16_t eff_x = circle->x - y; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		y++;
		t1 = t1 + y;
		int32_t t2 = t1 - x;
		if (t2 >= 0) {
			t1 = t2;
			x--;
		}
	}
}

static void draw_point(
    struct canvas *c, uint16_t x, uint16_t y, struct color color)
{
	// Drawing a point out of bounds is a no-op. This is
	// especially important given that we're receiving
	// arbitrary commands.
	if (c->width <= x || c->height <= y)
		return;

	// Color space is little endian, thus the BGRA format used below:
	uint8_t mapped[4] = { color.b, color.g, color.r, 0xFF };

	size_t off = (c->stride * y) + (x * sizeof(mapped));
	memcpy(&c->buffer[off], mapped, sizeof(mapped));
}
