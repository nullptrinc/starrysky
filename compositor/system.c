/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "starry-system-v1-protocol.h"
#include "starry.h"


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

/* Only the starboard we started may see the system interface. */
static bool global_filter(const struct wl_client *client, const struct wl_global *global,
		void *data) {
	struct starry_server *server = data;
	return global != server->system_global || client == server->starboard;
}

/* ---- scenes ---- */

static void scene_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void scene_handle_activate(struct wl_client *client, struct wl_resource *resource) {
	struct starry_toplevel *toplevel = wl_resource_get_user_data(resource);
	if (toplevel) {
		starry_show_app(toplevel->server, toplevel);
	}
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
	wl_list_init(&server->layers);
	server->system_global = wl_global_create(server->display, &starry_system_v1_interface,
		1, server, system_bind);
	wl_display_set_global_filter(server->display, global_filter, server);
	server->respawn = wl_event_loop_add_timer(wl_display_get_event_loop(server->display),
		respawn_starboard, server);
}
