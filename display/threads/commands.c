#include "../abort.h"
#include "rendering/canvas.h"
#include "termination.h"
#include "ui.h"

#include <ctype.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_CMD_LEN 1024

typedef void (*act_t)(struct ui_ctx *, char *target, size_t argc, char **argv);
typedef void (*cmd_t)(void);

struct command {
	char *target_name;
	char *action;
	size_t argc;
	char **argv;
};

struct parse_result {
	bool ok;
	union {
		struct command command;
		char *err;
	} val;
};

struct cmd_ctx {
	struct ui_ctx *ui_ctx;
	int cancellation_fd;
};

static void *cmd_inner(void *arg);
static char *find_target(char **);
static bool parse(char **input_cursor, struct parse_result *result);
static bool find_cmd(const char *);
static char *run(struct ui_ctx *, const struct command *);
static char *eat_whitespace(char *);
static char **collect_args(char **, size_t *, char **);

static void act_create(struct ui_ctx *, char *target, size_t argc, char **argv);
static void act_remove(struct ui_ctx *, char *target, size_t argc, char **argv);
static void act_rect(struct ui_ctx *, char *target, size_t argc, char **argv);
static void act_circle(struct ui_ctx *, char *target, size_t argc, char **argv);

static void cmd_term(void);

static bool parse_color(const char *in, struct color *out);
static bool parse_args(const char *fmt, size_t argc, char **argv, ...);

static struct {
	char *name;
	act_t hook;
} actions[] = {
	{ "CREATE", act_create },
	{ "REMOVE", act_remove },
	{ "RECT", act_rect },
	{ "CIRCLE", act_circle },
};

static struct {
	char *name;
	cmd_t hook;
} commands[] = {
	{ "TERMINATE", cmd_term },
};

void *cmd_thread(void *arg)
{
	struct cmd_ctx ctx;

	int cancellation_pipe[2];
	if (pipe(cancellation_pipe) != 0)
		FATAL_ERR("commands: can't create cancellation pipe");

	// TODO: print buffer dimensions to stdout in JSON format
	ctx.ui_ctx = (struct ui_ctx *)arg;
	ctx.cancellation_fd = cancellation_pipe[0];

	pthread_t reader;
	if (pthread_create(&reader, NULL, cmd_inner, &ctx) != 0) {
		FATAL_ERR("commands: failed to spawn thread: %s", STR_ERR);
	}

	term_block();

	uint8_t data = 0;
	if (write(cancellation_pipe[1], &data, 1) != 1) {
		fprintf(stderr, "commands: failed to cancel reader thread\n");
	} else {
		pthread_join(reader, NULL);
	}

	return NULL;
}

static void *cmd_inner(void *arg)
{
	struct cmd_ctx *ctx = (struct cmd_ctx *)arg;
	int cancellation_fd = ctx->cancellation_fd;

	struct pollfd fds[2];

	fds[0].fd = cancellation_fd;
	fds[0].events = POLLIN;

	fds[1].fd = 0; // stdin
	fds[1].events = POLLIN;

	char line[MAX_CMD_LEN];

	for (;;) {
		if (poll(fds, 2, 0) < 0)
			FATAL_ERR("commands: poll failed: %s", STR_ERR);

		if (fds[0].revents &= POLLIN)
			break;

		if (!(fds[1].revents & POLLIN))
			continue;

		if (!fgets(line, MAX_CMD_LEN, stdin))
			FATAL_ERR(
			    "commands: couldn't read from stdin: %s", STR_ERR);

		// Trim newline.
		size_t len = strlen(line);
		if (len >= 1)
			line[len - 1] = '\0';

		if (find_cmd(line))
			continue;

		char *line_cursor = line;
		char *target = find_target(&line_cursor);
		if (!target) {
			printf("target missing in command.\n");
			continue;
		}

		struct parse_result r = { 0 };
		while (parse(&line_cursor, &r)) {
			if (!r.ok) {
				printf("parsing failed: %s\n", r.val.err);
				free(r.val.err);
				continue;
			}

			r.val.command.target_name = target;
			char *err = run(ctx->ui_ctx, &r.val.command);

			if (err) {
				printf("%s\n", err); // write to the client
				fprintf(stderr, "cmd: run: %s\n",
				    err); // write to the debug log

				break;
			}
		}

		ui_sync(ctx->ui_ctx);
	}

	return NULL;
}

static char *find_target(char **input_cursor)
{
	char *target_end = *input_cursor;
	for (; *target_end != ':' && *target_end != '\0'; target_end++)
		;

	if (target_end == *input_cursor || *target_end == '\0') {
		return NULL;
	}

	*target_end = '\0';
	char *target = *input_cursor;

	if (*target == '\0')
		return NULL;

	*input_cursor = target_end + 1;

	return target;
}

static bool parse(char **input_cursor, struct parse_result *result)
{
	// Commands look like this:
	// <target>: <action> <args>...[; <action> <args>...]*
	//
	// These can be separated by as many ASCII whitespace characters as
	// desired. Don't use broader UTF-8 whitespace; I'll cry.
	//
	// We just get this here after parse_target.
	*input_cursor = eat_whitespace(*input_cursor);
	if (**input_cursor == '\0')
		return false;

	char *action_end = *input_cursor;
	while (*action_end != '\0' && !isspace(*action_end))
		action_end++;

	result->ok = true;

	char *arg_start = &action_end[1];
	if (*action_end != '\0' && *action_end != ';') {
		*action_end = '\0';
		result->val.command.argc = 0;
		result->val.command.argv =
		    collect_args(&arg_start, &result->val.command.argc, NULL);
	}

	result->val.command.action = *input_cursor;
	*input_cursor = arg_start;

	return result;
}

static bool find_cmd(const char *line)
{
	for (size_t i = 0; i < sizeof(commands) / sizeof(*commands); i++) {
		if (strcmp(line, commands[i].name) == 0) {
			commands[i].hook();
			return true;
		}
	}

	return false;
}

static char *run(struct ui_ctx *ctx, const struct command *c)
{
	for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); i++) {
		if (strcmp(c->action, actions[i].name) == 0) {
			actions[i].hook(ctx, c->target_name, c->argc, c->argv);
			if (c->argv)
				free(c->argv);
			return NULL;
		}
	}

	char *err_buf = malloc(512);
	snprintf(err_buf, 512, "no such action found: %s", c->action);
	return err_buf;
}

static char *eat_whitespace(char *x)
{
	while (*x != '\0' && isspace(*x))
		x++;

	return x;
}

static char **collect_args(char **cursor, size_t *argc, char **argv)
{
	char *input = eat_whitespace(*cursor);
	if (*input == '\0' || *input == ';') {
		*input = '\0';
		*cursor = input;
		return argv;
	}

	char *input_end = input;
	while (!isspace(*input_end) && *input_end != '\0' && *input_end != ';')
		input_end++;

	(*argc)++;
	size_t alloc_size =
	    *argc * sizeof(char *); // TODO: FATAL_ERR on overflow
	argv = realloc(argv, alloc_size);
	if (!argv)
		FATAL_ERR("commands: collect_args: OOM");

	argv[*argc - 1] = input;

	if (*input_end == '\0') {
		*cursor = input_end;
		return argv;
	} else if (*input_end == ';') {
		*input_end = '\0';
		*cursor = input_end + 1;
		return argv;
	} else {
		*input_end = '\0';
		*cursor = input_end + 1;
		return collect_args(cursor, argc, argv);
	}
}

static void act_create(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	struct color fill;
	if (!parse_args("c", argc, argv, &fill))
		return;

	enum ui_failure r = ui_pane_create(ctx, target, fill);
	if (r != UI_OK)
		fprintf(stderr, "act_create: failed: %s\n", ui_failure_str(r));
}

static void act_remove(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	if (!parse_args("", argc, argv))
		return;

	enum ui_failure r = ui_pane_remove(ctx, target);
	if (r != UI_OK)
		fprintf(stderr, "act_remove: failed: %s", ui_failure_str(r));
}

static void act_rect(struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	struct rect rect;

	size_t x, y, w, h;

	if (!parse_args("ciiii", argc, argv, &rect.c, &x, &y, &w, &h))
		return;

	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &rect, rendering_draw_rect_type_erased);

	if (r != UI_OK) {
		printf("failure: ui_pane_draw_shape: %s\n", ui_failure_str(r));
		return;
	}
}

static void act_circle(
    struct ui_ctx *ctx, char *target, size_t argc, char **argv)
{
	struct circle circle;

	size_t x, y, rad;

	if (!parse_args("ciii", argc, argv, &circle.c, &x, &y, &rad))
		return;

	circle.x = x;
	circle.y = y;
	circle.r = rad;

	enum ui_failure r = ui_pane_draw_shape(
	    ctx, target, &circle, rendering_draw_circle_type_erased);

	if (r != UI_OK) {
		printf("failure: ui_pane_draw_shape: %s\n", ui_failure_str(r));
		return;
	}
}

static void cmd_term(void)
{
	term();
}

static bool parse_color(const char *in, struct color *out)
{
	if (in[0] != '#')
		return false;

	if (strlen(in) != 7)
		return false;

	errno = 0;
	long result = strtol(&in[1], NULL, 16);
	if (errno != 0)
		return false;

	out->r = result >> 16;
	out->g = result >> 8 & 0xff;
	out->b = result & 0xff;

	return true;
}

static bool parse_args(const char *fmt, size_t argc, char **argv, ...)
{
	va_list args;
	va_start(args, argv);

	if (argc != strlen(fmt)) {
		printf("failure: got %zu arguments, expected %lu.\n", argc,
		    strlen(fmt));
		return false;
	}

	for (size_t i = 0; *fmt != '\0'; fmt++, i++) {
		char c = *fmt;
		char *in = argv[i];

		switch (c) {
		case 'c':
			struct color *cout = va_arg(args, struct color *);
			if (!parse_color(in, cout)) {
				printf(
				    "failure: expected color, got: %s\n", in);
				return false;
			}

			break;
		case 'i':
			size_t *out = va_arg(args, size_t *);
			char *end = NULL;
			long iout = strtol(in, &end, 0);
			*out = iout;
			if (*end != '\0') {
				printf(
				    "failure: expected number, got: %s\n", in);
				return false;
			}
			break;
		default:
			// Violently explode in lieu of proper compile-time type
			// checks.
			FATAL_ERR("unrecognized fmt parameter: %c", c);
		}
	}

	va_end(args);

	return true;
}
