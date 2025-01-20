#include "abort.h"
#include "rendering/rendering.h"
#include "threads/commands.h"
#include "threads/input.h"
#include "threads/termination.h"
#include "threads/ui.h"

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

int main(void)
{
	// Set up the backend to use.
	struct rendering_vtable vt = drm_backend_new(); // TODO: hardcoded

	// When running under webproc[1] for debugging, line buffering needs to
	// be enabled explicitly. Otherwise, we don't get logs when we think we
	// should get logs.
	//
	// [1]: https://github.com/atalii/webproc
	setvbuf(stdout, NULL, _IOLBF, 32);
	setvbuf(stderr, NULL, _IOLBF, 32);

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
