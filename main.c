#include "abort.h"
#include "rendering/rendering.h"
#include "threads/commands.h"
#include "threads/input.h"
#include "threads/termination.h"
#include "threads/ui.h"

#include <assert.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define SPAWN_THREAD(thread, handle, arg)                                   \
	pthread_t handle;                                                   \
	do {                                                                \
		if (pthread_create(&handle, NULL, thread, arg) != 0)        \
			FATAL_ERR("Couldn't spawn thread: " #thread ": %s", \
			    STR_ERR);                                       \
	} while (0);

struct backend_opt {
	enum backend backend;
	const char *as_string;
	const char *help;
};

struct args {
	enum backend backend;
};

const struct backend_opt backend_strings[BACKEND_COUNT] = {
	// The first backend is treated as the default.
	{ BACKEND_DRM, "DRM", "directly use the Linux DRM subsystem" },
};
// as a reminder to update this array when adding backends
static_assert(BACKEND_COUNT == 1);

void print_usage(FILE *f, const char *self)
{
	fprintf(f, "Usage: %s [OPTION]...\n", self);

	// Generate help text for --backend flag.
	fprintf(f, "      --backend <BACKEND>\n");
	fprintf(f,
	    "  \tSet the rendering backend to use. WARNING: currently case sensitive!\n");
	for (size_t i = 0; i < BACKEND_COUNT; i++) {
		struct backend_opt opt = backend_strings[i];
		fprintf(f, "  \t  - \"%s\"%s: %s\n", opt.as_string,
		    i == 0 ? " (default)" : "", opt.help);
	}

	fprintf(f, "  -h, --help\n");
	fprintf(f, "  \tPrint help.\n");
}

struct args parse_args(int argc, char *argv[])
{
	char *const self = argc > 0 ? argv[0] : "ttds";

	struct args args = {
		.backend = backend_strings[0].backend, // default
	};

	// i miss https://github.com/clap-rs/clap 💔

	// TODO: detect extraneous arguments

	while (true) {
		int option_index = 0;
		const struct option long_options[] = {
			{ "help", 0, NULL, 'h' },
			{ "backend", required_argument, NULL, 'b' },
			{ 0 }, // this must be terminated with some end
			       // indicator since getopt does not accept a
			       // length
		};

		int c =
		    getopt_long(argc, argv, "h", long_options, &option_index);

		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_usage(stdout, self);
			exit(0);
		case 'b':
			for (size_t i = 0; i < BACKEND_COUNT; i++) {
				struct backend_opt opt = backend_strings[i];
				// TODO: case sensitive is inconvenient
				if (strcmp(optarg, opt.as_string) == 0) {
					args.backend = opt.backend;
					goto found_backend;
				}
			}
			fprintf(stderr, "%s: unrecognized backend '%s'\n", self,
			    optarg);
			exit(1);
		found_backend:
			break;
		case '?':
			exit(1);
		default:
			assert(false && "unreachable");
			abort();
		}
	}

	return args;
}

int main(int argc, char *argv[])
{
	// When running under webproc[1] for debugging, line buffering needs to
	// be enabled explicitly. Otherwise, we don't get logs when we think we
	// should get logs.
	//
	// [1]: https://github.com/atalii/webproc
	setvbuf(stdout, NULL, _IOLBF, 32);
	setvbuf(stderr, NULL, _IOLBF, 32);

	// Parse command-line arguments. This only returns if they're valid.
	const struct args args = parse_args(argc, argv);
	const struct rendering_vtable vt = supported_backends[args.backend];

	term_init(4);

	// Prepare to block for SIGINT.
	sigset_t f, b;
	sigemptyset(&f);
	sigaddset(&f, SIGINT);
	sigprocmask(SIG_BLOCK, &f, &b);

	// Spawn child threads.
	struct ui_ctx *ui_ctx = ui_ctx_new(vt);
	SPAWN_THREAD(ui_thread, ui_handle, ui_ctx);
	SPAWN_THREAD(input_thread, input_handle, NULL);
	SPAWN_THREAD(cmd_thread, cmd_handle, ui_ctx);

	// Block for SIGINT.
	for (int s = 0; s != SIGINT; sigwait(&f, &s)) {
	}

	fprintf(stderr, "SIGINT received; cleaning up.\n");
	term();

	pthread_join(input_handle, NULL);
	pthread_join(ui_handle, NULL);
	pthread_join(cmd_handle, NULL);

	return 0;
}
