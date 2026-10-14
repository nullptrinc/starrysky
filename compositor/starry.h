/* SPDX-License-Identifier: MIT */
#ifndef STARRY_H
#define STARRY_H

#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

#define STARRY_SCALE 3
#define STARRY_STATUS_HEIGHT 28
#define STARRY_GESTURE_EDGE 28
#define STARRY_MAX_TOUCH 10

struct starry_server;

enum starry_touch_role {
	STARRY_TOUCH_NONE,
	STARRY_TOUCH_CLIENT,
	STARRY_TOUCH_GESTURE,
};

struct starry_touch {
	enum starry_touch_role role;
	double start_x, start_y, x, y;
	double surface_x, surface_y; /* layout position of the touched surface */
	uint32_t start_msec;
};

struct starry_animation {
	bool active;
	struct wlr_scene_node *node;
	double from_y, to_y;
	uint32_t start_msec, duration_msec;
	void (*done)(struct starry_server *server);
};

struct starry_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wlr_seat *seat;

	struct wlr_output *output;
	struct wlr_scene_output *scene_output;
	int width, height; /* logical */
	bool screen_on;

	struct wlr_scene_tree *home_layer, *app_layer, *overlay_layer, *lock_layer;

	struct wl_list toplevels;
	struct starry_toplevel *active;
	struct starry_touch touches[STARRY_MAX_TOUCH];
	struct starry_animation animation;

	/* System shell (system.c) */
	struct wl_global *system_global;
	struct wl_client *starboard;
	struct wl_resource *system;
	struct wl_listener starboard_destroy;
	struct wl_event_source *respawn;
	uint32_t starboard_started_msec;
	int starboard_failures;
	struct wl_list layers;

	struct wl_event_source *sigchld, *sigterm, *sigint;
	struct wl_listener new_output, new_input, new_toplevel, new_popup;
	struct wl_listener touch_down, touch_up, touch_motion, touch_cancel, touch_frame;
};

struct starry_toplevel {
	struct wl_list link;
	struct starry_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *tree;
	struct wl_resource *scene; /* starry_scene_v1 held by starboard */
	pid_t pid;
	struct wl_listener map, unmap, commit, destroy, set_title;
	struct wl_listener request_maximize, request_fullscreen;
};

/* main.c */
uint32_t starry_now_msec(void);
void starry_show_app(struct starry_server *server, struct starry_toplevel *toplevel);
void starry_go_home(struct starry_server *server, bool animated);

/* system.c */
void system_init(struct starry_server *server);
void system_spawn_starboard(struct starry_server *server);
void system_output_changed(struct starry_server *server);
void system_scene_added(struct starry_toplevel *toplevel);
void system_scene_info(struct starry_toplevel *toplevel);
void system_scene_state(struct starry_toplevel *toplevel, bool foreground);
void system_scene_removed(struct starry_toplevel *toplevel);
bool system_lock_mapped(struct starry_server *server);

#endif
