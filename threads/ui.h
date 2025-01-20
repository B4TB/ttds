#pragma once

#include "../rendering/rendering.h"

struct ui_ctx;

struct ui_ctx *ui_ctx_new(void);

void *ui_thread(void *);

enum ui_failure {
	UI_OK,
	UI_DUPLICATE,
	UI_OOM,
	UI_NO_SUCH_PANE,
};

char *ui_failure_str(enum ui_failure);

/* Flush an update to a pane. */
void ui_sync(struct ui_ctx *ctx);

/* Create a pane. */
enum ui_failure ui_pane_create(
    struct ui_ctx *ctx, char *name, struct color fill);

enum ui_failure ui_pane_remove(struct ui_ctx *ctx, char *name);

enum ui_failure ui_pane_draw_rect(
    struct ui_ctx *ctx, char *name, const struct rect *, struct color);

enum ui_failure ui_pane_draw_circle(
    struct ui_ctx *ctx, char *name, const struct circle *, struct color);
