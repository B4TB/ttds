#include "canvas.h"

#include "abort.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// The function body goes after macro invocation.
#define DEFN_RENDER(type)                         \
	void rendering_draw_##type##_type_erased( \
	    struct canvas *c, const void *v)      \
	{                                         \
		rendering_draw_##type(c, v);      \
	}                                         \
	void rendering_draw_##type(struct canvas *c, const struct type *type)

static inline intmax_t min(intmax_t, intmax_t);
static inline intmax_t max(intmax_t, intmax_t);
static inline float lerp(float start, float end, float t);
static void bezier2_compute(struct bezier2 b, float t, float *x, float *y);
static float bezier2_arclen_approx(struct bezier2 b, size_t n);
static void draw_point(struct canvas *, int32_t x, int32_t y, struct color);

struct canvas *canvas_init_bgra(uint16_t width, uint16_t height)
{
	// These Linux sources were helpful in understanding the intended
	// memory layout & alignment of these pixel buffers
	// (as of commit ffd294d346d185b70e28b1a28abe367bbfe53c04):
	// - linux/drivers/gpu/drm/drm_dumb_buffers.c:60
	// - linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:942
	// - linux/drivers/gpu/drm/amd/amdgpu/amdgpu_gem.c:916

	struct canvas *ret = malloc(sizeof(struct canvas));
	if (ret == NULL)
		return NULL;

	ret->width = width;
	ret->height = height;
	ret->stride = width * 4;

	ret->buffer = calloc(ret->height, ret->stride);
	if (ret->buffer == NULL)
		return NULL;

	return ret;
}

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

DEFN_RENDER(rect)
{
	uint16_t right_edge = rect->x + rect->w;
	uint16_t bottom_edge = rect->y + rect->h;
	for (uint16_t y = rect->y; y < bottom_edge && y < c->height; y++)
		for (uint16_t x = rect->x; x < right_edge && x < c->width; x++)
			draw_point(c, x, y, rect->c);
}

DEFN_RENDER(circle)
{
	uint16_t x = circle->r;
	uint16_t y = 0;
	int32_t t1 = circle->r / 16;

	const struct color color = circle->c;

	while (x > 0 && x >= y) {
		int32_t eff_y = circle->y + y;
		for (int32_t eff_x = circle->x + x; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - y;
		for (int32_t eff_x = circle->x + x; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - y;
		for (int32_t eff_x = circle->x - x; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + y;
		for (int32_t eff_x = circle->x - x; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + x;
		for (int32_t eff_x = circle->x + y; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - x;
		for (int32_t eff_x = circle->x + y; eff_x >= circle->x; eff_x--)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y - x;
		for (int32_t eff_x = circle->x - y; eff_x < circle->x; eff_x++)
			draw_point(c, eff_x, eff_y, color);

		eff_y = circle->y + x;
		for (int32_t eff_x = circle->x - y; eff_x < circle->x; eff_x++)
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

DEFN_RENDER(line)
{
	// This approach is guided by https://zingl.github.io/Bresenham.pdf
	// (A Rasterizing Algorithm for Drawing Curves, by Alois Zingl).
	// The math is based on the implicit function for a line,
	//
	// > f(x, y) = (y - y0)(x1 - x0) - (x - x0)(y1 - y0)
	//
	// which for all points on the line equals zero. Since the slope of a
	// line remains constant, we can test the "error" (evaluating the
	// function) at the next possible points and use a cheap comparison to
	// decide which to move to with the least error.
	//
	// For lines, the errors for directly adjacent pixels can be expressed
	// in terms of the error for the diagonal pixel and the total distances
	// `dy` and `dx`. This means we only need one error variable.
	//
	// The next optimization is to track the initial value and difference
	// per iteration, rather than recomputing the error each time.

	const int32_t dx = abs(line->x1 - line->x0);
	const int32_t dy = abs(line->y1 - line->y0);
	const int32_t sx = line->x0 < line->x1 ? 1 : -1;
	const int32_t sy = line->y0 < line->y1 ? 1 : -1;

	int32_t x = line->x0;
	int32_t y = line->y0;

	// We track the error for the next diagonal pixel (x + sx, y + sy).
	//
	// This relies on the assumption that the slope is positive, but we took
	// the absolute value for `dy` and `dx`, and this lie is self contained
	// in the mathy state and accounted for by `sx` and `sy`.
	int32_t e = dx - dy;

	while (true) {
		draw_point(c, x, y, line->c);

		// Check if we reached the other end.
		if (x == line->x1 && y == line->y1)
			break;

		const int32_t test = 2 * e;
		if (test > -dy) {
			x += sx;
			e -= dy;
		}
		if (test < dx) {
			y += sy;
			e += dx;
		}
	}
}

DEFN_RENDER(rect_copy)
{
	const struct rect_copy rc = *rect_copy;

	// The regions may overlap, so we care whether we are incrementing or
	// decrementing the y coordinate while traversing the source. When the
	// copy is solely horizontal, we prefer incrementing.
	const bool y_inc = rc.dst_y <= rc.src_y; // Flipped because +y is down.

	const uint32_t src_right_edge = (uint32_t)rc.src_x + rc.w;
	const uint32_t dst_right_edge = (uint32_t)rc.dst_x + rc.w;
	const uint32_t src_bottom_edge = (uint32_t)rc.src_y + rc.h;
	const uint32_t dst_bottom_edge = (uint32_t)rc.dst_y + rc.h;

	const int32_t src_safe_width =
	    (int32_t)min(c->width, src_right_edge) - rc.src_x;
	const int32_t dst_safe_width =
	    (int32_t)min(c->width, dst_right_edge) - rc.dst_x;

	const int32_t src_safe_height =
	    (int32_t)min(c->height, src_bottom_edge) - rc.src_y;
	const int32_t dst_safe_height =
	    (int32_t)min(c->height, dst_bottom_edge) - rc.dst_y;

	const int32_t safe_width = min(dst_safe_width, src_safe_width);
	const int32_t safe_height = min(dst_safe_height, src_safe_height);
	if (safe_width <= 0)
		return;

	for (int32_t dy = y_inc ? 0 : safe_height - 1;
	    dy < safe_height && dy >= 0; y_inc ? dy++ : dy--) {
		uint16_t src_row_y = rc.src_y + dy;
		uint16_t dst_row_y = rc.dst_y + dy;
		size_t src_idx = (size_t)c->stride * src_row_y + (rc.src_x * 4);
		size_t dst_idx = (size_t)c->stride * dst_row_y + (rc.dst_x * 4);
		memmove(&c->buffer[dst_idx], &c->buffer[src_idx],
		    (size_t)safe_width * 4);
	}
}

static void bezier2_compute(struct bezier2 b, float t, float *x, float *y)
{
	float px0 = lerp(b.x0, b.x1, t);
	float py0 = lerp(b.y0, b.y1, t);
	float px1 = lerp(b.x1, b.x2, t);
	float py1 = lerp(b.y1, b.y2, t);
	*x = lerp(px0, px1, t);
	*y = lerp(py0, py1, t);
}

static float bezier2_arclen_approx(struct bezier2 b, size_t n)
{
	float dist = 0;
	for (size_t i = 1; i <= n; i++) {
		float x1, y1, x0, y0;
		bezier2_compute(b, (float)i / (float)n, &x1, &y1);
		bezier2_compute(b, (float)(i - 1) / (float)n, &x0, &y0);
		// TODO: surely not good for precision and performance
		dist += sqrtf(powf(x1 - x0, 2) + powf(y1 - y0, 2));
	}
	return dist;
}

DEFN_RENDER(bezier2)
{
	const struct bezier2 b = *bezier2;

	// TODO: not pixel perfect (this approach is heuristic)

	// TODO: i care more about the point with the greatest
	// velocity than i do the average (represented by an
	// arclength), because that is where gaps in pixels may
	// appear.

	size_t arclen_precision = 13;
	float density = 2.0;
	float dt = 1.0 / (bezier2_arclen_approx(b, arclen_precision) * density);
	for (float t = 0.0; t <= 1.0; t += dt) {
		float px, py;
		bezier2_compute(b, t, &px, &py);
		draw_point(c, roundf(px), roundf(py), b.c);
	}
}

DEFN_RENDER(triangle)
{
	const struct triangle tri = *triangle;

	// Draw edges explicitly, since testing whether edge
	// coordinates are within the triangle is finicky, especially
	// for thin sections.
	rendering_draw_line(c,
	    &(struct line) { .x0 = tri.x0,
		.y0 = tri.y0,
		.x1 = tri.x1,
		.y1 = tri.y1,
		.c = tri.c });
	rendering_draw_line(c,
	    &(struct line) { .x0 = tri.x0,
		.y0 = tri.y0,
		.x1 = tri.x2,
		.y1 = tri.y2,
		.c = tri.c });
	rendering_draw_line(c,
	    &(struct line) { .x0 = tri.x1,
		.y0 = tri.y1,
		.x1 = tri.x2,
		.y1 = tri.y2,
		.c = tri.c });

	// Compute the inclusive bounding box for the triangle. We're
	// iterating through about twice as many pixels as we strictly
	// need to.
	const uint16_t bound_left = min(tri.x0, min(tri.x1, tri.x2));
	const uint16_t bound_right = max(tri.x0, max(tri.x1, tri.x2));
	const uint16_t bound_up = min(tri.y0, min(tri.y1, tri.y2));
	const uint16_t bound_down = max(tri.y0, max(tri.y1, tri.y2));

	// For each pixel, we first find its barycentric
	// coordinates[1] with respect to the triangle. Afterward we
	// simply check that each component, when scaled, is in the
	// range [0, 1].
	//
	// [1]: https://en.wikipedia.org/wiki/Barycentric_coordinate_system

	const int64_t det = ((int64_t)tri.y1 - (int64_t)tri.y2) *
		((int64_t)tri.x0 - (int64_t)tri.x2) +
	    ((int64_t)tri.x2 - (int64_t)tri.x1) *
		((int64_t)tri.y0 - (int64_t)tri.y2);

	// Compute scaled bounds for points in the triangle.
	const int64_t lo = min(0, det);
	const int64_t hi = max(0, det);

	for (int32_t y = bound_up; y <= bound_down; y++) {
		for (int32_t x = bound_left; x <= bound_right; x++) {
			// looks like lisp lmao
			const int64_t b1 = ((int64_t)tri.y1 - (int64_t)tri.y2) *
				((int64_t)x - (int64_t)tri.x2) +
			    ((int64_t)tri.x2 - (int64_t)tri.x1) *
				((int64_t)y - (int64_t)tri.y2);
			const int64_t b2 = ((int64_t)tri.y2 - (int64_t)tri.y0) *
				((int64_t)x - (int64_t)tri.x2) +
			    ((int64_t)tri.x0 - (int64_t)tri.x2) *
				((int64_t)y - (int64_t)tri.y2);
			const int64_t b3 = det - b1 - b2;

			if (lo <= b1 && b1 <= hi && lo <= b2 && b2 <= hi &&
			    lo <= b3 && b3 <= hi)
				draw_point(c, x, y, tri.c);
		}
	}
}

void rendering_dump_bgra_to_rgba(
    const struct canvas *c, DIR *dir, const char *dirpath, const char *path)
{
	const int fd = openat(dirfd(dir), path, O_RDWR | O_CREAT | O_TRUNC,
	    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0)
		FATAL_ERR("Failed to open file for writing: %s/%s: %s", dirpath,
		    path, STR_ERR);

	const size_t buffer_size = (size_t)c->height * (size_t)c->stride;
	if (ftruncate(fd, buffer_size) < 0)
		FATAL_ERR("Failed to grow file to %zu bytes: %s/%s: %s",
		    buffer_size, dirpath, path, STR_ERR);

	uint8_t *const dst =
	    mmap(NULL, buffer_size, PROT_WRITE, MAP_SHARED, fd, 0);
	if (dst == MAP_FAILED)
		FATAL_ERR("Failed to mmap file for writing: %s/%s: %s", dirpath,
		    path, STR_ERR);

	for (uint16_t y = 0; y < c->height; y++) {
		for (uint16_t x = 0; x < c->width; x++) {
			const size_t idx = (c->stride * y) + (x * 4);
			const uint8_t rgba_pixel[4] = {
				c->buffer[idx + 2], // R
				c->buffer[idx + 1], // G
				c->buffer[idx + 0], // B
				c->buffer[idx + 3], // A
			};

			memcpy(&dst[idx], rgba_pixel, sizeof(rgba_pixel));
		}
	}

	if (munmap(dst, buffer_size) < 0)
		FATAL_ERR("Failed to unmap file contents: %s/%s: %s", dirpath,
		    path, STR_ERR);

	if (close(fd) < 0)
		FATAL_ERR(
		    "Failed to close file: %s/%s: %s", dirpath, path, STR_ERR);
}

static inline intmax_t min(intmax_t a, intmax_t b)
{
	return a < b ? a : b;
}

static inline intmax_t max(intmax_t a, intmax_t b)
{
	return a < b ? b : a;
}

static inline float lerp(float start, float end, float t)
{
	return t * end + (1 - t) * start;
}

static void draw_point(
    struct canvas *c, int32_t x, int32_t y, struct color color)
{
	// Drawing a point out of bounds is a no-op. This is
	// especially important given that we're receiving
	// arbitrary commands.
	if (c->width <= x || c->height <= y || x < 0 || y < 0 ||
	    x > UINT16_MAX || y > UINT16_MAX)
		return;

	// Color space is little endian, thus the BGRA format used below:
	uint8_t mapped[4] = { color.b, color.g, color.r, 0xFF };

	size_t off = (c->stride * y) + (x * sizeof(mapped));
	memcpy(&c->buffer[off], mapped, sizeof(mapped));
}
