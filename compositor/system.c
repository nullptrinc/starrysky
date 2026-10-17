/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "starry-system-v1-protocol.h"
#include "starry.h"


/* The one compositor instance, for objects that outlive their window. */
static struct starry_server *system_server;

struct starry_layer {
	struct wl_list link;
	struct starry_server *server;
	struct wl_resource *resource;
	struct wlr_surface *surface;
	struct wlr_scene_tree *tree;
	enum starry_system_v1_layer layer;
	struct wl_listener surface_destroy;
};

/* ---- layer surfaces ---- */

static void layer_role_commit(struct wlr_surface *surface) {
	if (wlr_surface_has_buffer(surface)) {
		wlr_surface_map(surface);
	} else {
		wlr_surface_unmap(surface);
	}
}

static const struct wlr_surface_role layer_role = {
	.name = "starry_layer_surface_v1",
	.commit = layer_role_commit,
};

static void layer_detach(struct starry_layer *layer) {
	if (layer->surface) {
		wl_list_remove(&layer->surface_destroy.link);
		wlr_surface_unmap(layer->surface);
		layer->surface = NULL;
	}
	if (layer->tree) {
		wlr_scene_node_destroy(&layer->tree->node);
		layer->tree = NULL;
	}
}

static void layer_surface_destroyed(struct wl_listener *listener, void *data) {
	struct starry_layer *layer = wl_container_of(listener, layer, surface_destroy);
	wl_list_remove(&layer->surface_destroy.link);
	layer->surface = NULL;
	layer->tree = NULL; /* the subsurface tree destroys itself with the surface */
}

static void layer_resource_destroy(struct wl_resource *resource) {
	struct starry_layer *layer = wl_resource_get_user_data(resource);
	layer_detach(layer);
	wl_list_remove(&layer->link);
	free(layer);
}

static void layer_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void layer_handle_set_position(struct wl_client *client,
		struct wl_resource *resource, int32_t x, int32_t y) {
	struct starry_layer *layer = wl_resource_get_user_data(resource);
	if (layer->tree) {
		wlr_scene_node_set_position(&layer->tree->node, x, y);
	}
}

static const struct starry_layer_surface_v1_interface layer_impl = {
	.destroy = layer_handle_destroy,
	.set_position = layer_handle_set_position,
};

static struct wlr_scene_tree *layer_parent(struct starry_server *server, uint32_t layer) {
	switch (layer) {
	case STARRY_SYSTEM_V1_LAYER_HOME:
		return server->home_layer;
	case STARRY_SYSTEM_V1_LAYER_OVERLAY:
		return server->overlay_layer;
	case STARRY_SYSTEM_V1_LAYER_LOCK:
		return server->lock_layer;
	case STARRY_SYSTEM_V1_LAYER_SWITCHER:
		return server->switcher_layer;
	}
	return NULL;
}

bool system_lock_mapped(struct starry_server *server) {
	struct starry_layer *layer;
	wl_list_for_each(layer, &server->layers, link) {
		if (layer->layer == STARRY_SYSTEM_V1_LAYER_LOCK && layer->surface &&
				layer->surface->mapped) {
			return true;
		}
	}
	return false;
}

/* ---- starry_system_v1 ---- */

static void system_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void system_handle_get_layer_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource,
		uint32_t layer_index) {
	struct starry_server *server = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct wlr_scene_tree *parent = layer_parent(server, layer_index);
	if (!parent) {
		wl_resource_post_error(resource, STARRY_SYSTEM_V1_ERROR_LAYER,
			"unknown layer %u", layer_index);
		return;
	}
	if (!wlr_surface_set_role(surface, &layer_role, resource, STARRY_SYSTEM_V1_ERROR_ROLE)) {
		return;
	}
	struct starry_layer *layer = calloc(1, sizeof(*layer));
	layer->server = server;
	layer->surface = surface;
	layer->layer = layer_index;
	layer->resource = wl_resource_create(client, &starry_layer_surface_v1_interface,
		wl_resource_get_version(resource), id);
	wl_resource_set_implementation(layer->resource, &layer_impl, layer, layer_resource_destroy);
	wlr_surface_set_role_object(surface, layer->resource);
	layer->tree = wlr_scene_subsurface_tree_create(parent, surface);
	layer->surface_destroy.notify = layer_surface_destroyed;
	wl_signal_add(&surface->events.destroy, &layer->surface_destroy);
	wl_list_insert(&server->layers, &layer->link);
}

static void system_handle_go_home(struct wl_client *client, struct wl_resource *resource) {
	starry_go_home(wl_resource_get_user_data(resource), true);
}

static const struct starry_system_v1_interface system_impl = {
	.destroy = system_handle_destroy,
	.get_layer_surface = system_handle_get_layer_surface,
	.go_home = system_handle_go_home,
};

static void system_resource_destroy(struct wl_resource *resource) {
	struct starry_server *server = wl_resource_get_user_data(resource);
	if (server->system == resource) {
		server->system = NULL;
	}
}

void system_switcher(struct starry_server *server, int y) {
	if (server->system) {
		starry_system_v1_send_switcher(server->system, y);
	}
}

void system_output_changed(struct starry_server *server) {
	if (server->system && server->output) {
		starry_system_v1_send_output(server->system, server->width, server->height, STARRY_SCALE);
	}
}

static void system_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct starry_server *server = data;
	struct wl_resource *resource = wl_resource_create(client, &starry_system_v1_interface,
		version, id);
	wl_resource_set_implementation(resource, &system_impl, server, system_resource_destroy);
	server->system = resource;
	system_output_changed(server);
	struct starry_toplevel *toplevel;
	wl_list_for_each_reverse(toplevel, &server->toplevels, link) {
		system_scene_added(toplevel);
	}
}

/* Screen capture: starboard, and root (starry-shot over a developer shell).
 * Root can read the display device anyway; apps never run as root. */
static bool may_capture(struct starry_server *server, const struct wl_client *client) {
	if (client == server->starboard) {
		return true;
	}
	pid_t pid;
	uid_t uid;
	gid_t gid;
	wl_client_get_credentials((struct wl_client *)client, &pid, &uid, &gid);
	return uid == 0;
}

/* Only the starboard we started may see the system interface. */
static bool global_filter(const struct wl_client *client, const struct wl_global *global,
		void *data) {
	struct starry_server *server = data;
	if (global == server->screencopy_global) {
		return may_capture(server, client);
	}
	return global != server->system_global || client == server->starboard;
}

/* ---- scenes ---- */

static void scene_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void scene_handle_activate(struct wl_client *client, struct wl_resource *resource,
		uint32_t animation) {
	struct starry_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		starry_show_app(toplevel->server, toplevel,
			animation == STARRY_SCENE_V1_ANIMATION_SLIDE);
	}
}

/* ---- scene views: snapshots the shell places, like CALayerHost on iOS ---- */

struct snapshot_pass {
	struct wlr_render_pass *pass;
	double scale;
};

static void snapshot_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct snapshot_pass *snapshot = data;
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture) {
		return;
	}
	wlr_render_pass_add_texture(snapshot->pass, &(struct wlr_render_texture_options){
		.texture = texture,
		.dst_box = {
			.x = lround(sx * snapshot->scale), .y = lround(sy * snapshot->scale),
			.width = lround(surface->current.width * snapshot->scale),
			.height = lround(surface->current.height * snapshot->scale),
		},
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	});
}

/* Renders the window as it would appear full screen, at half the panel's density. */
static struct wlr_buffer *snapshot_toplevel(struct starry_toplevel *toplevel) {
	struct starry_server *server = toplevel->server;
	const double scale = STARRY_SCALE / 2.0;
	int width = lround(server->width * scale);
	int height = lround((server->height - STARRY_STATUS_HEIGHT) * scale);
	uint64_t implicit = DRM_FORMAT_MOD_INVALID;
	struct wlr_drm_format fallback = {
		.format = DRM_FORMAT_XRGB8888, .len = 1, .capacity = 1, .modifiers = &implicit,
	};
	struct wlr_buffer *buffer = NULL;
	if (server->output && server->output->swapchain) {
		buffer = wlr_allocator_create_buffer(server->allocator, width, height,
			&server->output->swapchain->format);
	}
	if (!buffer) {
		buffer = wlr_allocator_create_buffer(server->allocator, width, height, &fallback);
	}
	if (!buffer) {
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
	if (!pass) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = width, .height = height },
		.color = { 0.08, 0.08, 0.1, 1 },
	});
	struct snapshot_pass snapshot = { .pass = pass, .scale = scale };
	wlr_xdg_surface_for_each_surface(toplevel->xdg_toplevel->base, snapshot_surface, &snapshot);
	wlr_render_pass_submit(pass);
	return buffer;
}

struct starry_view {
	struct wlr_scene_buffer *node;
};

static bool view_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	return false; /* touches go to the switcher surface underneath */
}

static void view_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void view_handle_set_geometry(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct starry_view *view = wl_resource_get_user_data(resource);
	wlr_scene_node_set_position(&view->node->node, x, y);
	if (width > 0 && height > 0) {
		wlr_scene_buffer_set_dest_size(view->node, width, height);
	}
}

static const struct starry_scene_view_v1_interface view_impl = {
	.destroy = view_handle_destroy,
	.set_geometry = view_handle_set_geometry,
};

static void view_resource_destroy(struct wl_resource *resource) {
	struct starry_view *view = wl_resource_get_user_data(resource);
	wlr_scene_node_destroy(&view->node->node);
	free(view);
}

static void scene_handle_get_view(struct wl_client *client, struct wl_resource *resource,
		uint32_t id) {
	struct starry_toplevel *toplevel = wl_resource_get_user_data(resource);
	struct starry_view *view = calloc(1, sizeof(*view));
	struct wl_resource *view_resource = wl_resource_create(client,
		&starry_scene_view_v1_interface, wl_resource_get_version(resource), id);
	/* A scene whose window is already gone still gets a (blank) view. */
	struct wlr_buffer *buffer = toplevel ? snapshot_toplevel(toplevel) : NULL;
	view->node = wlr_scene_buffer_create(system_server->view_layer, buffer);
	if (buffer) {
		wlr_buffer_drop(buffer);
	}
	view->node->point_accepts_input = view_accepts_input;
	wl_resource_set_implementation(view_resource, &view_impl, view, view_resource_destroy);
}

static void scene_handle_close(struct wl_client *client, struct wl_resource *resource) {
	struct starry_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
	}
}

static const struct starry_scene_v1_interface scene_impl = {
	.destroy = scene_handle_destroy,
	.activate = scene_handle_activate,
	.get_view = scene_handle_get_view,
	.close = scene_handle_close,
};

static void scene_resource_destroy(struct wl_resource *resource) {
	struct starry_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		toplevel->scene = NULL;
	}
}

void system_scene_added(struct starry_toplevel *toplevel) {
	struct starry_server *server = toplevel->server;
	if (!server->system || toplevel->scene) {
		return;
	}
	toplevel->scene = wl_resource_create(wl_resource_get_client(server->system),
		&starry_scene_v1_interface, wl_resource_get_version(server->system), 0);
	wl_resource_set_implementation(toplevel->scene, &scene_impl, toplevel, scene_resource_destroy);
	starry_system_v1_send_scene(server->system, toplevel->scene);
	system_scene_info(toplevel);
	system_scene_state(toplevel, server->active == toplevel);
}

void system_scene_info(struct starry_toplevel *toplevel) {
	if (toplevel->scene) {
		struct wlr_xdg_toplevel *xdg = toplevel->xdg_toplevel;
		starry_scene_v1_send_info(toplevel->scene, toplevel->pid,
			xdg->app_id ? xdg->app_id : "", xdg->title ? xdg->title : "");
	}
}

void system_scene_state(struct starry_toplevel *toplevel, bool foreground) {
	if (toplevel->scene) {
		starry_scene_v1_send_state(toplevel->scene, foreground ?
			STARRY_SCENE_V1_STATE_FOREGROUND : STARRY_SCENE_V1_STATE_BACKGROUND);
	}
}

void system_scene_removed(struct starry_toplevel *toplevel) {
	if (toplevel->scene) {
		starry_scene_v1_send_closed(toplevel->scene);
		wl_resource_set_user_data(toplevel->scene, NULL);
		toplevel->scene = NULL;
	}
}

/* ---- starboard process: started on a private socket, restarted if it dies ---- */

static int respawn_starboard(void *data) {
	system_spawn_starboard(data);
	return 0;
}

static void starboard_destroyed(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, starboard_destroy);
	wl_list_remove(&server->starboard_destroy.link);
	server->starboard = NULL;
	server->system = NULL;
	struct starry_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		toplevel->scene = NULL; /* the resources died with the client */
	}
	/* Quick repeated crashes back off up to ten seconds. */
	bool quick = starry_now_msec() - server->starboard_started_msec < 3000;
	server->starboard_failures = quick ? server->starboard_failures + 1 : 0;
	int delay = server->starboard_failures > 10 ? 10000 :
		200 * (1 << (server->starboard_failures > 5 ? 5 : server->starboard_failures));
	wlr_log(WLR_ERROR, "Starboard exited; restarting in %d ms", delay);
	wl_event_source_timer_update(server->respawn, delay);
}

void system_spawn_starboard(struct starry_server *server) {
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) {
		wlr_log_errno(WLR_ERROR, "socketpair");
		return;
	}
	const char *path = getenv("STARRY_STARBOARD");
	if (!path) {
		path = STARRY_STARBOARD_PATH;
	}
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		int fd = dup(fds[1]); /* without close-on-exec */
		char value[16];
		snprintf(value, sizeof(value), "%d", fd);
		setenv("WAYLAND_SOCKET", value, true);
		execl(path, path, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		return;
	}
	server->starboard = wl_client_create(server->display, fds[0]);
	if (!server->starboard) {
		close(fds[0]);
		return;
	}
	server->starboard_started_msec = starry_now_msec();
	server->starboard_destroy.notify = starboard_destroyed;
	wl_client_add_destroy_listener(server->starboard, &server->starboard_destroy);
	wlr_log(WLR_INFO, "Started starboard %s as %d", path, pid);
}

void system_init(struct starry_server *server) {
	system_server = server;
	wl_list_init(&server->layers);
	server->system_global = wl_global_create(server->display, &starry_system_v1_interface,
		1, server, system_bind);
	wl_display_set_global_filter(server->display, global_filter, server);
	server->respawn = wl_event_loop_add_timer(wl_display_get_event_loop(server->display),
		respawn_starboard, server);
}
