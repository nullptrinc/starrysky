/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <execinfo.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "starry.h"

/* Print a backtrace to the log; resolve it with addr2line on the build host. */
static void crash_handler(int signal) {
	void *frames[64];
	int count = backtrace(frames, 64);
	dprintf(STDERR_FILENO, "starry-compositor: fatal signal %d\n", signal);
	backtrace_symbols_fd(frames, count, STDERR_FILENO);
	_exit(128 + signal);
}

uint32_t starry_now_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* ---- animation: a single vertical slide, advanced on every output frame ---- */

static void animate(struct starry_server *server, struct wlr_scene_node *node,
		double to_y, uint32_t duration, void (*done)(struct starry_server *)) {
	server->animation = (struct starry_animation){
		.active = true, .node = node, .from_y = node->y, .to_y = to_y,
		.start_msec = starry_now_msec(), .duration_msec = duration, .done = done,
	};
	if (server->output) {
		wlr_output_schedule_frame(server->output);
	}
}

static void animation_step(struct starry_server *server) {
	struct starry_animation *a = &server->animation;
	if (!a->active) {
		return;
	}
	double t = fmin(1, (double)(starry_now_msec() - a->start_msec) / a->duration_msec);
	double eased = 1 - pow(1 - t, 3);
	wlr_scene_node_set_position(a->node, a->node->x,
		lround(a->from_y + (a->to_y - a->from_y) * eased));
	if (t < 1) {
		wlr_output_schedule_frame(server->output);
		return;
	}
	a->active = false;
	if (a->done) {
		a->done(server);
	}
}

/* ---- foreground application ---- */

static void set_keyboard_focus(struct starry_server *server, struct starry_toplevel *toplevel) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (!toplevel) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		return;
	}
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server->seat, toplevel->xdg_toplevel->base->surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void home_hidden(struct starry_server *server) {
	wlr_scene_node_set_enabled(&server->home_layer->node, false);
}

void starry_show_app(struct starry_server *server, struct starry_toplevel *toplevel) {
	if (server->active == toplevel && !server->animation.active) {
		return;
	}
	if (server->active && server->active != toplevel) {
		wlr_xdg_toplevel_set_activated(server->active->xdg_toplevel, false);
		wlr_scene_node_set_enabled(&server->active->tree->node, false);
		system_scene_state(server->active, false);
	}
	server->active = toplevel;
	wlr_scene_node_set_enabled(&toplevel->tree->node, true);
	wlr_scene_node_raise_to_top(&toplevel->tree->node);
	wlr_scene_node_set_position(&toplevel->tree->node, 0, server->height);
	set_keyboard_focus(server, toplevel);
	system_scene_state(toplevel, true);
	animate(server, &toplevel->tree->node, STARRY_STATUS_HEIGHT, 260, home_hidden);
}

static void app_hidden(struct starry_server *server) {
	struct starry_toplevel *toplevel = server->active;
	if (toplevel) {
		wlr_scene_node_set_enabled(&toplevel->tree->node, false);
		wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, false);
	}
	server->active = NULL;
}

void starry_go_home(struct starry_server *server, bool animated) {
	wlr_scene_node_set_enabled(&server->home_layer->node, true);
	set_keyboard_focus(server, NULL);
	if (server->active) {
		system_scene_state(server->active, false);
	}
	if (server->active && animated) {
		animate(server, &server->active->tree->node, -server->height, 240, app_hidden);
	} else {
		server->animation.active = false;
		app_hidden(server);
	}
}

/* Applications started by a starboard that later died are reparented to us. */
static int handle_sigchld(int signal, void *data) {
	while (waitpid(-1, NULL, WNOHANG) > 0) {
	}
	return 0;
}

static int handle_terminate(int signal, void *data) {
	wl_display_terminate(((struct starry_server *)data)->display);
	return 0;
}

/* ---- toplevels ---- */

static void toplevel_map(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
	system_scene_added(toplevel);
	starry_show_app(toplevel->server, toplevel);
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct starry_server *server = toplevel->server;
	wl_list_remove(&toplevel->link);
	system_scene_removed(toplevel);
	if (server->active == toplevel) {
		if (server->animation.node == &toplevel->tree->node) {
			server->animation.active = false;
		}
		starry_go_home(server, false);
	}
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	if (toplevel->xdg_toplevel->base->initial_commit) {
		struct starry_server *server = toplevel->server;
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			server->width, server->height - STARRY_STATUS_HEIGHT);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
		wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel,
			WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	}
}

static void toplevel_set_title(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	system_scene_info(toplevel);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
	struct starry_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
	system_scene_removed(toplevel);
	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->set_title.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	free(toplevel);
}

static void new_toplevel(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, new_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;
	struct starry_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->tree = wlr_scene_xdg_surface_create(server->app_layer, xdg_toplevel->base);
	toplevel->tree->node.data = toplevel;
	wlr_scene_node_set_enabled(&toplevel->tree->node, false);
	xdg_toplevel->base->data = toplevel->tree;
	wl_client_get_credentials(xdg_toplevel->base->client->client, &toplevel->pid, NULL, NULL);

	toplevel->map.notify = toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
	toplevel->set_title.notify = toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
	toplevel->request_maximize.notify = toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

struct starry_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit, destroy;
};

static void popup_commit(struct wl_listener *listener, void *data) {
	struct starry_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct starry_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void new_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent || !parent->data) {
		return;
	}
	struct starry_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent->data, xdg_popup->base);
	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- touch: the bottom-edge swipe is the system's; everything else goes to clients ---- */

static struct starry_touch *touch_point(struct starry_server *server, int32_t id) {
	return id >= 0 && id < STARRY_MAX_TOUCH ? &server->touches[id] : NULL;
}

static struct wlr_surface *surface_at(struct wlr_scene_tree *root,
		double x, double y, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&root->node, x, y, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	return scene_surface ? scene_surface->surface : NULL;
}

static void touch_down(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, touch_down);
	struct wlr_touch_down_event *event = data;
	struct starry_touch *point = touch_point(server, event->touch_id);
	if (!point || !server->screen_on) {
		return;
	}
	double x = event->x * server->width, y = event->y * server->height;
	*point = (struct starry_touch){
		.start_x = x, .start_y = y, .x = x, .y = y, .start_msec = event->time_msec,
	};
	bool locked = system_lock_mapped(server);
	if (!locked && server->active && y >= server->height - STARRY_GESTURE_EDGE) {
		point->role = STARRY_TOUCH_GESTURE;
		server->animation.active = false;
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = surface_at(locked ? server->lock_layer : &server->scene->tree,
		x, y, &sx, &sy);
	if (surface) {
		point->role = STARRY_TOUCH_CLIENT;
		point->surface_x = x - sx;
		point->surface_y = y - sy;
		wlr_seat_touch_notify_down(server->seat, surface, event->time_msec,
			event->touch_id, sx, sy);
	}
}

static void touch_motion(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, touch_motion);
	struct wlr_touch_motion_event *event = data;
	struct starry_touch *point = touch_point(server, event->touch_id);
	if (!point) {
		return;
	}
	point->x = event->x * server->width;
	point->y = event->y * server->height;
	if (point->role == STARRY_TOUCH_GESTURE && server->active) {
		/* The app follows the finger upwards, showing home underneath. */
		double lift = fmin(0, point->y - point->start_y);
		wlr_scene_node_set_enabled(&server->home_layer->node, true);
		wlr_scene_node_set_position(&server->active->tree->node, 0,
			lround(STARRY_STATUS_HEIGHT + lift));
	} else if (point->role == STARRY_TOUCH_CLIENT) {
		/* Motion stays relative to the surface that received the down event. */
		wlr_seat_touch_notify_motion(server->seat, event->time_msec, event->touch_id,
			point->x - point->surface_x, point->y - point->surface_y);
	}
}

static void touch_up(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, touch_up);
	struct wlr_touch_up_event *event = data;
	struct starry_touch *point = touch_point(server, event->touch_id);
	if (!point) {
		return;
	}
	if (point->role == STARRY_TOUCH_CLIENT) {
		wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
	} else if (point->role == STARRY_TOUCH_GESTURE && server->active) {
		double distance = point->start_y - point->y;
		double velocity = distance / fmax(1, event->time_msec - point->start_msec);
		if (distance > 90 || (distance > 24 && velocity > 0.5)) {
			starry_go_home(server, true);
		} else {
			animate(server, &server->active->tree->node, STARRY_STATUS_HEIGHT, 180, home_hidden);
		}
	}
	point->role = STARRY_TOUCH_NONE;
}

static void touch_cancel(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, touch_cancel);
	struct wlr_touch_cancel_event *event = data;
	struct starry_touch *point = touch_point(server, event->touch_id);
	if (point && point->role == STARRY_TOUCH_CLIENT) {
		wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
	}
	if (point) {
		point->role = STARRY_TOUCH_NONE;
	}
}

static void touch_frame(struct wl_listener *listener, void *data) {
	/* The frame signal carries no event, so find the server from the listener. */
	struct starry_server *server = wl_container_of(listener, server, touch_frame);
	wlr_seat_touch_notify_frame(server->seat);
}

/* ---- keys: power toggles the panel, the rest goes to the focused app ---- */

struct starry_keyboard {
	struct starry_server *server;
	struct wlr_keyboard *keyboard;
	struct wl_listener key, modifiers, destroy;
};

static void set_screen(struct starry_server *server, bool on) {
	if (!server->output || server->screen_on == on) {
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	if (wlr_output_commit_state(server->output, &state)) {
		server->screen_on = on;
	}
	wlr_output_state_finish(&state);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct starry_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_keyboard_key_event *event = data;
	struct starry_server *server = keyboard->server;
	if (event->keycode == KEY_POWER) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
			set_screen(server, !server->screen_on);
		}
		return;
	}
	wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
	wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct starry_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard->modifiers);
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct starry_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->destroy.link);
	free(keyboard);
}

static void new_input(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		struct starry_keyboard *keyboard = calloc(1, sizeof(*keyboard));
		keyboard->server = server;
		keyboard->keyboard = wlr_keyboard_from_input_device(device);
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap) {
			wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
		}
		xkb_keymap_unref(keymap);
		xkb_context_unref(context);
		keyboard->key.notify = keyboard_key;
		wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
		keyboard->modifiers.notify = keyboard_modifiers;
		wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
		keyboard->destroy.notify = keyboard_destroy;
		wl_signal_add(&device->events.destroy, &keyboard->destroy);
		wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
	} else if (device->type == WLR_INPUT_DEVICE_TOUCH && !server->touch_down.notify) {
		/* courbet has exactly one touchscreen. */
		struct wlr_touch *touch = wlr_touch_from_input_device(device);
		server->touch_down.notify = touch_down;
		wl_signal_add(&touch->events.down, &server->touch_down);
		server->touch_up.notify = touch_up;
		wl_signal_add(&touch->events.up, &server->touch_up);
		server->touch_motion.notify = touch_motion;
		wl_signal_add(&touch->events.motion, &server->touch_motion);
		server->touch_cancel.notify = touch_cancel;
		wl_signal_add(&touch->events.cancel, &server->touch_cancel);
		server->touch_frame.notify = touch_frame;
		wl_signal_add(&touch->events.frame, &server->touch_frame);
	}
	wlr_seat_set_capabilities(server->seat,
		WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_KEYBOARD);
}

/* ---- output ---- */

struct starry_output {
	struct starry_server *server;
	struct wl_listener frame, request_state, destroy;
};

static void output_frame(struct wl_listener *listener, void *data) {
	struct starry_output *output = wl_container_of(listener, output, frame);
	struct starry_server *server = output->server;
	animation_step(server);
	wlr_scene_output_commit(server->scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(server->scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct starry_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->server->output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct starry_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	output->server->output = NULL;
	free(output);
}

static void new_output(struct wl_listener *listener, void *data) {
	struct starry_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	if (server->output) {
		return; /* Only the built-in panel is driven. */
	}
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_state_set_scale(&state, STARRY_SCALE);
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "Cannot enable output %s", wlr_output->name);
		return;
	}

	struct starry_output *output = calloc(1, sizeof(*output));
	output->server = server;
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	server->output = wlr_output;
	server->screen_on = true;
	wlr_output_effective_resolution(wlr_output, &server->width, &server->height);
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	server->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, server->scene_output);
	wlr_log(WLR_INFO, "Output %s: %dx%d logical", wlr_output->name, server->width, server->height);
	system_output_changed(server);
}

int main(int argc, char *argv[]) {
	wlr_log_init(getenv("STARRY_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	signal(SIGSEGV, crash_handler);
	signal(SIGABRT, crash_handler);
	signal(SIGBUS, crash_handler);
	prctl(PR_SET_CHILD_SUBREAPER, 1);

	struct starry_server server = {0};
	wl_list_init(&server.toplevels);
	server.display = wl_display_create();
	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_backend_autocreate(loop, NULL);
	if (!server.backend) {
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	server.allocator = server.renderer ?
		wlr_allocator_autocreate(server.backend, server.renderer) : NULL;
	if (!server.allocator) {
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_data_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	server.home_layer = wlr_scene_tree_create(&server.scene->tree);
	server.app_layer = wlr_scene_tree_create(&server.scene->tree);
	server.overlay_layer = wlr_scene_tree_create(&server.scene->tree);
	server.lock_layer = wlr_scene_tree_create(&server.scene->tree);

	struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.display, 6);
	server.new_toplevel.notify = new_toplevel;
	wl_signal_add(&xdg_shell->events.new_toplevel, &server.new_toplevel);
	server.new_popup.notify = new_popup;
	wl_signal_add(&xdg_shell->events.new_popup, &server.new_popup);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.new_input.notify = new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.new_output.notify = new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);
	system_init(&server);

	server.sigchld = wl_event_loop_add_signal(loop, SIGCHLD, handle_sigchld, &server);
	server.sigterm = wl_event_loop_add_signal(loop, SIGTERM, handle_terminate, &server);
	server.sigint = wl_event_loop_add_signal(loop, SIGINT, handle_terminate, &server);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (!socket || !wlr_backend_start(server.backend)) {
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	wlr_log(WLR_INFO, "Starry running on WAYLAND_DISPLAY=%s", socket);
	system_spawn_starboard(&server);
	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	return 0;
}
