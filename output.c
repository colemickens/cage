/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_surface.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "output.h"
#include "server.h"
#include "view.h"

/* Used to move all of the data necessary to render a surface from the
 * top-level frame handler to the per-surface render function. */
struct render_data {
	struct wlr_output_layout *output_layout;
	struct wlr_output *output;
	struct cg_view *view;
	struct timespec *when;
};

static void
render_surface(struct wlr_surface *surface, int sx, int sy, void *data)
{
	struct render_data *rdata = data;
	struct cg_view *view = rdata->view;
	struct wlr_output *output = rdata->output;

	if (!wlr_surface_has_buffer(surface)) {
		return;
	}

	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		wlr_log(WLR_DEBUG, "Cannot obtain surface texture");
		return;
	}

	double ox = 0, oy = 0;
	wlr_output_layout_output_coords(rdata->output_layout, output, &ox, &oy);
	ox += view->x + sx, oy += view->y + sy;

	struct wlr_box box = {
		.x = ox * output->scale,
		.y = oy * output->scale,
		.width = surface->current.width * output->scale,
		.height = surface->current.height * output->scale,
	};

	float matrix[9];
	enum wl_output_transform transform = wlr_output_transform_invert(surface->current.transform);
	wlr_matrix_project_box(matrix, &box, transform, 0, output->transform_matrix);
	wlr_render_texture_with_matrix(surface->renderer, texture, matrix, 1);
	wlr_surface_send_frame_done(surface, rdata->when);
}

static void
view_for_each_surface(struct cg_view *view, struct render_data *rdata,
		      wlr_surface_iterator_func_t iterator)
{
	switch(view->type) {
	case CAGE_XDG_SHELL_VIEW:
		wlr_xdg_surface_for_each_surface(view->xdg_surface, iterator, rdata);
		break;
	default:
		wlr_log(WLR_ERROR, "Unrecognized view type: %d", view->type);
	}
}

static void
handle_output_frame(struct wl_listener *listener, void *data)
{
	struct cg_output *output = wl_container_of(listener, output, frame);
	struct wlr_renderer *renderer = wlr_backend_get_renderer(output->server->backend);

	if (!wlr_output_make_current(output->wlr_output, NULL)) {
		wlr_log(WLR_DEBUG, "Cannot make damage output current");
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	int width, height;
	wlr_output_effective_resolution(output->wlr_output, &width, &height);

	wlr_renderer_begin(renderer, width, height);

	float color[4] = {0.3, 0.3, 0.3, 1.0};
	wlr_renderer_clear(renderer, color);

	struct render_data rdata = {
		.output_layout = output->server->output_layout,
		.output = output->wlr_output,
		.when = &now,
	};

	struct cg_view *view;
	wl_list_for_each_reverse(view, &output->server->views, link) {
		rdata.view = view;
		view_for_each_surface(view, &rdata, render_surface);
	}

	wlr_renderer_end(renderer);
	wlr_output_swap_buffers(output->wlr_output, NULL, NULL);
}

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
        struct cg_output *output = wl_container_of(listener, output, destroy);
	struct cg_server *server = output->server;

        wl_list_remove(&output->destroy.link);
        wl_list_remove(&output->frame.link);
        free(output);
	server->output = NULL;

	/* Since there is no use in continuing without our (single)
	 * output, terminate. */
	wl_display_terminate(server->wl_display);
}

void
handle_new_output(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* On outputs that have modes, we need to set one before we
	 * can use it. Each monitor supports only a specific set of
	 * modes. We just pick the last, in the future we could pick
	 * the mode the display advertises as preferred. */
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode = wl_container_of(wlr_output->modes.prev, mode, link);
		wlr_output_set_mode(wlr_output, mode);
	}

	server->output = calloc(1, sizeof(struct cg_output));
	server->output->wlr_output = wlr_output;
	server->output->server = server;

	server->output->frame.notify = handle_output_frame;
	wl_signal_add(&wlr_output->events.frame, &server->output->frame);
	server->output->destroy.notify = handle_output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &server->output->destroy);

	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	/* Disconnect the signal now, because we only use one static output. */
	wl_list_remove(&server->new_output.link);

	if (wlr_xcursor_manager_load(server->seat->xcursor_manager, wlr_output->scale)) {
		wlr_log(WLR_ERROR, "Cannot load XCursor theme for output '%s' with scale %f",
			wlr_output->name,
			wlr_output->scale);
	}
	wlr_xcursor_manager_set_cursor_image(server->seat->xcursor_manager, DEFAULT_XCURSOR,
					     server->seat->cursor);

	/* Place the cursor in the center of the screen. */
	wlr_cursor_warp(server->seat->cursor, NULL, wlr_output->width / 2, wlr_output->height / 2);
}