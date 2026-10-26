// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2015  Giulio Camuffo <giuliocamuffo@gmail.com>
// gamma-wl.c -- Wayland gamma adjustment header

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-util.h>
#include <fnmatch.h>

#ifndef FNM_EXTMATCH
# define FNM_EXTMATCH 0
#endif

#ifdef ENABLE_NLS
# include <libintl.h>
# define _(s) gettext(s)
#else
# define _(s) s
#endif

#include "gamma-wl.h"
#include "os-compatibility.h"
#include "colorramp.h"
#include "vlog.h"

#include "gamma-control-client-protocol.h"
#include "orbital-authorizer-client-protocol.h"

typedef struct {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_callback *callback;
	uint32_t gamma_control_manager_id;
	struct zwlr_gamma_control_manager_v1 *gamma_control_manager;
	int num_outputs;
	struct wl_list outputs;
	int authorized;
	char *output_name_pattern;
} wayland_state_t;

struct output {
	uint32_t global_id;
	struct wl_output *output;
	struct zwlr_gamma_control_v1 *gamma_control;
	uint32_t gamma_size;
	struct wl_list link;
	char *name;
};

static bool output_is_enabled(const wayland_state_t *state, const struct output *output) {
	if (state->output_name_pattern == NULL) {
		return true;
	}
	if (output->name == NULL) {
		return false;
	}
	int match = fnmatch(state->output_name_pattern, output->name, FNM_EXTMATCH);
	if (match == 0) {
		return true;
	}
	return false;
}

static int
wayland_init(wayland_state_t **state)
{
	/* Initialize state. */
	*state = malloc(sizeof(**state));
	if (*state == NULL) return -1;

	memset(*state, 0, sizeof **state);
	wl_list_init(&(*state)->outputs);
	return 0;
}

static void
authorizer_feedback_granted(void *data, struct orbital_authorizer_feedback *feedback)
{
	wayland_state_t *state = data;
	state->authorized = 1;
}

static void
authorizer_feedback_denied(void *data, struct orbital_authorizer_feedback *feedback)
{
	vlog_err(_("Not authorized to bind the wlroots gamma control manager interface."));
	exit(EXIT_FAILURE);
}

static const struct orbital_authorizer_feedback_listener authorizer_feedback_listener = {
	authorizer_feedback_granted,
	authorizer_feedback_denied
};

static void output_geometry(
	void *data,
	struct wl_output *wl_output,
	int x,
	int y,
	int phy_width,
	int phy_height,
	int subpixel,
	const char *make,
	const char *model,
	int transform
) {
	// noop
}

static void output_mode(
	void *data,
	struct wl_output *wl_output,
	unsigned int flags,
	int width,
	int height,
	int refresh
) {
	// noop
}

static void output_done(void *data, struct wl_output *wl_output) {
	// noop
}

static void output_scale(void *data, struct wl_output *wl_output, int scale) {
	// noop
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
	struct output *state = data;
	if ((state->name = strdup(name)) == NULL) {
		vlog_err("Failed to allocate memory.");
		exit(EXIT_FAILURE);
	}
}

static void output_description(void *data, struct wl_output *wl_output, const char *description) {
	// noop
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static void
registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version)
{
	wayland_state_t *state = data;

	if (strcmp(interface, "zwlr_gamma_control_manager_v1") == 0) {
		state->gamma_control_manager_id = id;
		state->gamma_control_manager = wl_registry_bind(registry, id, &zwlr_gamma_control_manager_v1_interface, 1);
	} else if (strcmp(interface, "wl_output") == 0) {
		state->num_outputs++;
		struct output *output = malloc(sizeof(struct output));
		if (output == NULL) {
			vlog_err(_("Failed to allocate memory"));
			return;
		}
		output->name = NULL;
		output->global_id = id;
		output->output = wl_registry_bind(registry, id, &wl_output_interface, 4);
		output->gamma_control = NULL;
		output->gamma_size = 0;
		wl_list_insert(&state->outputs, &output->link);
	} else if (strcmp(interface, "orbital_authorizer") == 0) {
		struct wl_event_queue *queue = wl_display_create_queue(state->display);

		struct orbital_authorizer *auth = wl_registry_bind(registry, id, &orbital_authorizer_interface, 1u);
		wl_proxy_set_queue((struct wl_proxy *)auth, queue);

		struct orbital_authorizer_feedback *feedback = orbital_authorizer_authorize(auth, "zwlr_gamma_control_manager_v1");
		orbital_authorizer_feedback_add_listener(feedback, &authorizer_feedback_listener, state);

		int ret = 0;
		while (!state->authorized && ret >= 0) {
			ret = wl_display_dispatch_queue(state->display, queue);
		}

		orbital_authorizer_feedback_destroy(feedback);
		orbital_authorizer_destroy(auth);
		wl_event_queue_destroy(queue);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t id)
{
	wayland_state_t *state = data;

	if (state->gamma_control_manager_id == id) {
		vlog_err(_("The zwlr_gamma_control_manager_v1 was removed"));
		exit(EXIT_FAILURE);
	}

	struct output *output;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->global_id == id) {
			if (output->gamma_control) {
				zwlr_gamma_control_v1_destroy(output->gamma_control);
				output->gamma_control = NULL;
			}
			wl_output_destroy(output->output);

			state->num_outputs--;
			wl_list_remove(&output->link);
			free(output->name);
			free(output);

			return;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove
};

static void
gamma_control_gamma_size(void *data, struct zwlr_gamma_control_v1 *control, uint32_t size)
{
	struct output *output = data;
	output->gamma_size = size;
}

static void
gamma_control_failed(void *data, struct zwlr_gamma_control_v1 *control)
{
        struct output *output = data;
        output->gamma_size = 0;
}


static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	gamma_control_gamma_size,
	gamma_control_failed
};

static int
wayland_start(wayland_state_t *state)
{
	state->display = wl_display_connect(NULL);
	if (!state->display) {
		vlog_err(_("Could not connect to wayland display, exiting."));
		return -1;
	}
	state->registry = wl_display_get_registry(state->display);

	wl_registry_add_listener(state->registry, &registry_listener, state);

	wl_display_roundtrip(state->display);
	struct output *output;
	wl_list_for_each(output, &state->outputs, link) {
		wl_output_add_listener(output->output, &output_listener, output);
	}
	wl_display_roundtrip(state->display);
	struct output *tmp;
	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output_is_enabled(state, output)) {
			continue;
		}
		if (output->name) {
			vlog_info("%s: %s",
			          _("Disabling output due to configuration"),
			          output->name);
		} else {
			vlog_info(_("Disabling output due to configuration"));
		}
		if (output->gamma_control) {
			zwlr_gamma_control_v1_destroy(output->gamma_control);
			output->gamma_control = NULL;
		}
		wl_output_destroy(output->output);
		wl_list_remove(&output->link);
		free(output->name);
		free(output);
	}
	if (!state->gamma_control_manager) {
		vlog_err(_("Could not control gamma, exiting."));
		return -1;
	}
	if (state->num_outputs > 0 && wl_list_empty(&state->outputs)) {
		vlog_err(_("No outputs available, exiting."));
		return -1;
	}

	return 0;
}

static void
wayland_restore(wayland_state_t *state)
{
	struct output *output;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->gamma_control) {
			zwlr_gamma_control_v1_destroy(output->gamma_control);
			output->gamma_control = NULL;
		}
	}
	wl_display_flush(state->display);
}

static void
wayland_free(wayland_state_t *state)
{
	int ret = 0;
	/* Wait for the sync callback to destroy everything, otherwise
	 * we could destroy the gamma control before gamma has been set */
	while (state->callback && ret >= 0) {
		ret = wl_display_dispatch(state->display);
	}
	if (state->callback) {
		vlog_err("%s: %d", _("Ignoring Wayland connection error while waiting to disconnect"), ret);
		wl_callback_destroy(state->callback);
	}

	struct output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output->gamma_control) {
			zwlr_gamma_control_v1_destroy(output->gamma_control);
			output->gamma_control = NULL;
		}
		wl_output_destroy(output->output);
		wl_list_remove(&output->link);
		free(output->name);
		free(output);
	}

	if (state->gamma_control_manager) {
		zwlr_gamma_control_manager_v1_destroy(state->gamma_control_manager);
	}
	if (state->registry) {
		wl_registry_destroy(state->registry);
	}
	if (state->display) {
		wl_display_disconnect(state->display);
	}

	free(state);
}

static void
wayland_print_help(FILE *f)
{
	fputs(_("Adjust gamma ramps with a Wayland compositor."), f);
	fputs("\n\n  output=N\t\t", f);
	fputs(_("only apply to outputs with names matching this pattern"), f);
	fputs("\n", f);
}

static int
wayland_set_option(wayland_state_t *state, const char *key, const char *value)
{
	if (strcasecmp(key, "output") == 0) {
		if (!(state->output_name_pattern = strdup(value))) {
			vlog_err(_("Failed to allocate memory"));
			return -1;
		}
	} else {
		vlog_err("%s: `%s'.", _("Unknown method parameter"), key);
		return -1;
	}
	return 0;
}

static void
callback_done(void *data, struct wl_callback *cb, uint32_t t)
{
	wayland_state_t *state = data;
	state->callback = NULL;
	wl_callback_destroy(cb);
}

static const struct wl_callback_listener callback_listener = {
	callback_done
};

static int
wayland_set_temperature(wayland_state_t *state, const color_setting_t *setting)
{
	int ret = 0, roundtrip = 0;

	/* We wait for the sync callback to throttle a bit and not send more
	 * requests than the compositor can manage, otherwise we'd get disconnected.
	 * This also allows us to dispatch other incoming events such as
	 * wl_registry.global_remove. */
	while (state->callback && ret >= 0) {
		ret = wl_display_dispatch(state->display);
	}
	if (ret < 0) {
		vlog_err("%s: %d", _("Wayland connection experienced a fatal error"), ret);
		return ret;
	}

	struct output *output;
	wl_list_for_each(output, &state->outputs, link) {
		/* For some reason, gamma control will be reported as
		 * no longer supported after a tty switch. For this
		 * reason, retry if gamma_size has become 0. */
		if (output->gamma_control && output->gamma_size == 0) {
			zwlr_gamma_control_v1_destroy(output->gamma_control);
			output->gamma_control = NULL;
		}
		if (!output->gamma_control) {
			output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(state->gamma_control_manager, output->output);
			zwlr_gamma_control_v1_add_listener(output->gamma_control, &gamma_control_listener, output);
			roundtrip = 1;
		}
	}
	if (roundtrip) {
		wl_display_roundtrip(state->display);
	}

	int unsupported_outputs = 0;
	wl_list_for_each(output, &state->outputs, link) {
		if (output->gamma_size == 0 || output->gamma_size > SSIZE_MAX / (3 * sizeof(uint16_t))) {
			unsupported_outputs += 1;
			continue;
		}
		ssize_t size = output->gamma_size;
		size_t ramp_bytes = size * sizeof(uint16_t);
		size_t total_bytes = ramp_bytes * 3;

		int fd = os_create_anonymous_file(total_bytes);
		if (fd < 0) {
			vlog_err("os_create_anonymous_file: %m");
			return -1;
		}

		void *ptr = mmap(NULL, total_bytes,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (ptr == MAP_FAILED) {
			vlog_err("mmap: %m");
			close(fd);
			return -1;
		}

		uint16_t *r_gamma = ptr;
		uint16_t *g_gamma = ptr + ramp_bytes;
		uint16_t *b_gamma = ptr + 2 * ramp_bytes;

		/* Initialize gamma ramps to pure state */
		for (int i = 0; i < size; i++) {
			uint16_t value = (double)i / size * (UINT16_MAX+1);
			r_gamma[i] = value;
			g_gamma[i] = value;
			b_gamma[i] = value;
		}

		colorramp_fill(r_gamma, g_gamma, b_gamma, size, setting);
		if (munmap(ptr, total_bytes) == -1) {
			vlog_err("munmap: %m");
			close(fd);
			return -1;
		}

		zwlr_gamma_control_v1_set_gamma(output->gamma_control, fd);
		close(fd);
	}

	if (state->num_outputs == unsupported_outputs) {
		vlog_warning(_("Zero outputs support gamma adjustment."));
	}
	if (unsupported_outputs > 0) {
		vlog_warning("%d/%d %s.", unsupported_outputs, state->num_outputs,
		             _("output(s) do not support gamma adjustment"));
	}

	state->callback = wl_display_sync(state->display);
	wl_callback_add_listener(state->callback, &callback_listener, state);
	wl_display_flush(state->display);

	return 0;
}

const gamma_method_t wl_gamma_method = {
	"wayland",
	1,
	(gamma_method_init_func *) wayland_init,
	(gamma_method_start_func *) wayland_start,
	(gamma_method_free_func *) wayland_free,
	(gamma_method_print_help_func *) wayland_print_help,
	(gamma_method_set_option_func *) wayland_set_option,
	(gamma_method_restore_func *) wayland_restore,
	(gamma_method_set_temperature_func *) wayland_set_temperature,
};
