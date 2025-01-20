#include "threads/ui.h"

#include "../rendering/rendering.h"
#include "abort.h"
#include "termination.h"

#include <assert.h>
#include <bits/pthreadtypes.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

#define MAX_PANES 1024

struct pane {
	char *name;
	struct canvas *canvas;
};

static void *rotate_panes(void *);

struct pane_storage {
	size_t count;
	pthread_mutex_t lock;
	struct pane panes[MAX_PANES];
};

struct ui_ctx {
	struct rendering_vtable vt;
	void *r_ctx; // type erased rendering context

	struct pane_storage panes;

	int cancellation_fd;
};

char *ui_failure_strs[] = {
	[UI_OK] = "no failure",
	[UI_DUPLICATE] = "duplicate pane",
	[UI_OOM] = "oom",
	[UI_NO_SUCH_PANE] = "targeted pane doesn't exist",
};

char *ui_failure_str(enum ui_failure f)
{
	return ui_failure_strs[f];
}

struct ui_ctx *ui_ctx_new(struct rendering_vtable vt)
{
	struct ui_ctx *ctx = malloc(sizeof(struct ui_ctx));
	if (!ctx)
		FATAL_ERR("ui: failed to allocate ctx");

	ctx->r_ctx = vt.rendering_init();
	vt.rendering_ctx_log(ctx->r_ctx);

	pthread_mutex_init(&ctx->panes.lock, NULL);

	if (pthread_mutex_lock(&ctx->panes.lock) != 0)
		FATAL_ERR("couldn't lock newly created pane mutex");

	ctx->panes.count = 1;
	ctx->panes.panes[0].canvas = vt.canvas_init(ctx->r_ctx);
	ctx->panes.panes[0].name = strdup("root");

	if (!ctx->panes.panes[0].name)
		FATAL_ERR("ui: ctx_new: pane creation OOM");

	struct color bg = {
		.r = 0x22,
		.g = 0x22,
		.b = 0x88,
	};

	rendering_fill(ctx->panes.panes[0].canvas, bg);

	pthread_mutex_unlock(&ctx->panes.lock);

	// Make sure that the cancellation fd is invalid until ui_thread gets
	// around to making the pipe.
	ctx->cancellation_fd = -1;

	return ctx;
}

void *ui_thread(void *arg)
{
	struct ui_ctx *ctx = arg;

	int cancellation_pipe[2];
	if (pipe(cancellation_pipe) != 0)
		FATAL_ERR("ui: pipe: %s", STR_ERR);

	ctx->cancellation_fd = cancellation_pipe[0];

	pthread_t pane_handle;
	pthread_create(&pane_handle, NULL, rotate_panes, ctx);

	term_block();
	write(cancellation_pipe[1], "\0", 1);
	pthread_join(pane_handle, NULL);

	fprintf(stderr, "rendering: terminating\n");

	// We don't need to take the lock as the only thread that could be using
	// it --- pane_handle --- has been pthread_join(3)ed.
	size_t len = ctx->panes.count;
	for (size_t i = 0; i < len; i++) {
		canvas_deinit(ctx->panes.panes[i].canvas);
		free(ctx->panes.panes[i].name);
	}

	ctx->vt.rendering_cleanup(ctx->r_ctx);
	free(ctx);

	return NULL;
}

static void *rotate_panes(void *arg)
{
	int r;

	struct ui_ctx *ctx = arg;
	struct pollfd fds[1];
	fds[0].fd = ctx->cancellation_fd;
	fds[0].events = POLLIN;

	for (size_t i = 0;; i++) {

		r = pthread_mutex_lock(&ctx->panes.lock);

		if (r != 0)
			FATAL_ERR("ui: couldn't take lock: %s", strerror(r));

		size_t idx = i % ctx->panes.count;
		struct pane *p = &ctx->panes.panes[idx];

		fprintf(stderr, "ui: flipping pane: %s\n", p->name);
		ctx->vt.rendering_show(ctx->r_ctx, p->canvas);

		r = pthread_mutex_unlock(&ctx->panes.lock);
		if (r != 0)
			FATAL_ERR("ui: couldn't return lock: %s", strerror(r));

		// Do a cancellable 1-second sleep.
		poll(fds, 1, 1000);
		if (fds[0].revents &= POLLIN)
			break;
	}

	return NULL;
}

/* Create a pane. */
enum ui_failure ui_pane_create(
    struct ui_ctx *ctx, char *name, struct color fill)
{
	// Really huge assumption that only one thread is calling into this or
	// the deletion function at once.
	pthread_mutex_lock(&ctx->panes.lock);

	size_t idx = ctx->panes.count;
	for (size_t i = 0; i < idx; i++) {
		struct pane *p = &ctx->panes.panes[i];
		if (strcmp(p->name, name) == 0) {
			pthread_mutex_unlock(&ctx->panes.lock);
			return UI_DUPLICATE;
		}
	}

	assert(idx < MAX_PANES);
	struct pane *p = &ctx->panes.panes[idx];

	p->name = strdup(name);
	if (!p->name)
		return UI_OOM;

	p->canvas = ctx->vt.canvas_init(ctx->r_ctx);
	if (!p->canvas) {
		free(p->name);
		return UI_OOM;
	}

	rendering_fill(p->canvas, fill);

	ctx->panes.count++;
	pthread_mutex_unlock(&ctx->panes.lock);

	return UI_OK;
}

enum ui_failure ui_pane_remove(struct ui_ctx *ctx, char *name)
{
	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR(
		    "ui_pane_remove: failed to take lock: %s", strerror(r));

	for (size_t i = 0; i < ctx->panes.count; i++) {
		if (strcmp(ctx->panes.panes[i].name, name) != 0)
			continue;

		struct pane *p = &ctx->panes.panes[i];
		free(p->name);
		canvas_deinit(p->canvas);

		for (size_t j = i + 1; j < ctx->panes.count; j++) {
			struct pane *q = &ctx->panes.panes[j];
			p->name = q->name;
			p->canvas = q->canvas;

			p = q;
		}

		ctx->panes.count--;

		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_OK;
	}

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_NO_SUCH_PANE;
}

enum ui_failure ui_pane_draw_rect(
    struct ui_ctx *ctx, char *name, const struct rect *rect, struct color c)
{
	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR(
		    "ui_pane_draw_rect: failed to lock: %s\n", strerror(r));

	struct pane *p = NULL;
	for (size_t i = 0; i < ctx->panes.count; i++) {
		p = &ctx->panes.panes[i];
		if (strcmp(p->name, name) == 0)
			break;
	}

	if (!p)
		return UI_NO_SUCH_PANE;

	rendering_draw_rect(p->canvas, rect, c);

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_OK;
}

enum ui_failure ui_pane_draw_circle(
    struct ui_ctx *ctx, char *name, const struct circle *circle, struct color c)
{
	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR(
		    "ui_pane_draw_rect: failed to lock: %s\n", strerror(r));

	struct pane *p = NULL;
	for (size_t i = 0; i < ctx->panes.count; i++) {
		p = &ctx->panes.panes[i];
		if (strcmp(p->name, name) == 0)
			break;
	}

	if (!p)
		return UI_NO_SUCH_PANE;

	rendering_draw_circle(p->canvas, circle, c);

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_OK;
}
