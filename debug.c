#include "abort.h"
#include "rendering/rendering.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const struct color BG = { .r = 0x3A, .g = 0x22, .b = 0xBD };
const struct color FG = { .r = 0xFF, .g = 0xFF, .b = 0xFF };

int main(void)
{
	// Initialize in-memory canvas.
	struct canvas *canvas = canvas_init_bgra(32, 32);
	if (canvas == NULL)
		FATAL_ERR("out of memory");
	rendering_fill(canvas, BG);

	// Draw a glider.
	const uint16_t glider[5][2] = {
		{ 1, 0 },
		{ 0, 1 },
		{ 0, 2 },
		{ 1, 2 },
		{ 2, 2 },
	};
	const int16_t x0 = 22;
	const int16_t y0 = 16;
	for (size_t i = 0; i < sizeof(glider) / sizeof(glider[0]); i++) {
		uint16_t x = x0 + glider[i][0];
		uint16_t y = y0 + glider[i][1];
		rendering_draw_rect(canvas,
		    &(struct rect) { .w = 1, .h = 1, .x = x, .y = y }, FG);
	}

	// Save canvas as raw RGBA pixel data.
	const char *pathname = "canvas-rgba.data";
	rendering_dump_bgra_to_rgba(canvas, pathname);
	fprintf(stderr, "Wrote RGBA pixel data to file: %s\n", pathname);

	return 0;
}
