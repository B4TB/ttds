#include "../threads/ui.h"

#include "../abort.h"
#include "../rendering/rendering.h"
#include "termination.h"

#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PANES 1024
#define PANE_DELAY 1000

struct pane {
	char *name;
	struct canvas *canvas;
};

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

	int sync_fd_rx;
	int sync_fd_tx;
};

enum sleep_result {
	SHOULD_CANCEL, SHOULD_SWITCH, SHOULD_SYNC,
};

static void *rotate_panes(void *);

/* Search for a pane with the given name, or null if none is found.
 * This does not perform any synchronization, so if there are other threads
 * accessing the panes, you must lock the mutex first. */
static struct pane *lookup_pane_thread_unsafe(
    struct pane_storage *, const char *);

char *ui_failure_strs[] = {
	[UI_OK] = "no failure",
	[UI_DUPLICATE] = "duplicate pane",
	[UI_OOM] = "oom",
	[UI_NO_SUCH_PANE] = "targeted pane doesn't exist",
	[UI_TOO_MANY_PANES] = "MAX_PANES would be exceeded",
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
	ctx->vt = vt;

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
		.r = 0x3A,
		.g = 0x22,
		.b = 0xBD,
	};

	rendering_fill(ctx->panes.panes[0].canvas, bg);

	pthread_mutex_unlock(&ctx->panes.lock);

	// Make sure that the cancellation fd is invalid until ui_thread gets
	// around to making the pipe.
	ctx->cancellation_fd = -1;

	// sync_fd, on the other hand, can be initialized immediately.
	int sync_fds[2];
	if (pipe(sync_fds) != 0) {
		FATAL_ERR("pipe(2) failed for sync fd's: %s", STR_ERR);
	}

	ctx->sync_fd_rx = sync_fds[0];
	ctx->sync_fd_tx = sync_fds[1];

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
	if (write(cancellation_pipe[1], "\0", 1) != 1)
		FATAL_ERR("ui: failed to write to cancellation pipe");

	pthread_join(pane_handle, NULL);

	fprintf(stderr, "ui: terminating\n");

	if (pthread_mutex_lock(&ctx->panes.lock) != 0)
		FATAL_ERR("Failed to lock panes.");

	size_t len = ctx->panes.count;
	for (size_t i = 0; i < len; i++) {
		canvas_deinit(ctx->panes.panes[i].canvas);
		free(ctx->panes.panes[i].name);
	}

	ctx->vt.rendering_cleanup(ctx->r_ctx);
	free(ctx);

	return NULL;
}

void ui_sync(struct ui_ctx *ctx)
{
	char data[] = { 0 };
	if (write(ctx->sync_fd_tx, data, 1) != 1)
		fprintf(stderr, "failed to write to sync_fd_tx: %s", STR_ERR);
}

static enum sleep_result cancellable_sleep(int cancellation_fd, int sync_fd, int *sleep_time)
{
	// If sleep_time is too low during a barrage of requests, poll(2) might
	// just not exhuast its timeout, and delta will be zero we'll never
	// reset sleep_time. 50 seems to be an okay threshold to correct for
	// that.
	if (*sleep_time < 50) {
		*sleep_time = PANE_DELAY;
		return SHOULD_SWITCH;
	}

	struct pollfd fds[2];
	fds[0].fd = cancellation_fd;
	fds[1].fd = sync_fd;
	fds[0].events = POLLIN;
	fds[1].events = POLLIN;
	char buf[1];

	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC_RAW, &a);

	int r = poll(fds, sizeof(fds) / sizeof(*fds), *sleep_time);
	if (r == -1) FATAL_ERR("cancellable_sleep: poll(2) failed: %s", STR_ERR);

	if (fds[0].revents &= POLLIN) {
		// Cancellation fd.
		if (read(fds[0].fd, buf, 1) != 1)
			fprintf(stderr, "cancellable_sleep: failed to read from cancellation fd.\n");

		*sleep_time = PANE_DELAY;
		return SHOULD_CANCEL;
	} else if (fds[1].revents &= POLLIN) {
		// Sync fd.
		if (read(fds[1].fd, buf, 1) != 1)
			fprintf(stderr, "cancellable_sleep: failed to read from sync fd.\n");

		clock_gettime(CLOCK_MONOTONIC_RAW, &b);

		uint64_t prior = (a.tv_sec * 1000) + (a.tv_nsec / 1000 / 1000);
		uint64_t post = (b.tv_sec * 1000) + (b.tv_nsec / 1000 / 1000);
		int delta = (int) post - prior;

		if (delta < 20) 
			delta += 20;

		if (delta >= *sleep_time) {
			*sleep_time = PANE_DELAY;
			return SHOULD_SWITCH;
		} else {
			*sleep_time -= delta;
			return SHOULD_SYNC;
		}
	}

	*sleep_time = PANE_DELAY;
	return SHOULD_SWITCH;
}

static void *rotate_panes(void *arg)
{
	int r;

	struct ui_ctx *ctx = arg;
	int sleep_time = PANE_DELAY;
	bool switching = true;

	size_t i = 0;
	for (;;) {
		r = pthread_mutex_lock(&ctx->panes.lock);

		if (r != 0)
			FATAL_ERR("ui: couldn't take lock: %s", strerror(r));

		i += switching;
		if (i >= ctx->panes.count) i = 0;

		struct pane *p = &ctx->panes.panes[i];

		fprintf(stderr, "ui: flipping pane: %s\n", p->name);
		ctx->vt.rendering_show(ctx->r_ctx, p->canvas);

		r = pthread_mutex_unlock(&ctx->panes.lock);
		if (r != 0)
			FATAL_ERR("ui: couldn't return lock: %s", strerror(r));

		enum sleep_result r = cancellable_sleep(ctx->cancellation_fd, ctx->sync_fd_rx, &sleep_time);
		switch (r) {
		case SHOULD_SWITCH:
			switching = true;
			break;
		case SHOULD_SYNC:
			switching = false;
			break;
		case SHOULD_CANCEL:
			return NULL;
			break;
		}
	}

	return NULL;
}

static struct pane *lookup_pane_thread_unsafe(
    struct pane_storage *panes, const char *name)
{
	for (size_t i = 0; i < panes->count; i++) {
		struct pane *p = &panes->panes[i];
		if (strcmp(p->name, name) == 0)
			return p;
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
	if (lookup_pane_thread_unsafe(&ctx->panes, name)) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_DUPLICATE;
	}

	if (idx >= MAX_PANES) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_TOO_MANY_PANES;
	}
	struct pane *p = &ctx->panes.panes[idx];

	p->name = strdup(name);
	if (!p->name) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_OOM;
	}

	p->canvas = ctx->vt.canvas_init(ctx->r_ctx);
	if (!p->canvas) {
		free(p->name);
		pthread_mutex_unlock(&ctx->panes.lock);
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

enum ui_failure ui_pane_draw_shape(
    struct ui_ctx *ctx, char *name, const void *shape, render_fn_t inner)
{
	int r;

	r = pthread_mutex_lock(&ctx->panes.lock);
	if (r != 0)
		FATAL_ERR(
		    "ui_pane_draw_shape: failed to lock: %s\n", strerror(r));

	struct pane *p = lookup_pane_thread_unsafe(&ctx->panes, name);
	if (!p) {
		pthread_mutex_unlock(&ctx->panes.lock);
		return UI_NO_SUCH_PANE;
	}

	inner(p->canvas, shape);

	pthread_mutex_unlock(&ctx->panes.lock);
	return UI_OK;
}
