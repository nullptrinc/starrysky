/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <execinfo.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "canvas.h"
#include "starry-system-v1-client-protocol.h"
#include "starboard.h"

static void crash_handler(int signal) {
	void *frames[64];
	int count = backtrace(frames, 64);
	dprintf(STDERR_FILENO, "starboard: fatal signal %d\n", signal);
	backtrace_symbols_fd(frames, count, STDERR_FILENO);
	_exit(128 + signal);
}

uint32_t sb_now_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void sb_timer_arm(struct sb_timer *timer, uint32_t delay_msec) {
	timer->deadline = delay_msec ? sb_now_msec() + delay_msec : 0;
}

struct wl_surface *sb_surface_create(struct starboard *sb, bool input) {
	struct wl_surface *surface = wl_compositor_create_surface(sb->compositor);
	wl_surface_set_buffer_scale(surface, SB_SCALE);
	if (!input) {
		struct wl_region *none = wl_compositor_create_region(sb->compositor);
		wl_surface_set_input_region(surface, none);
		wl_region_destroy(none);
	}
	return surface;
}

/* ---- sprites: input-less subsurfaces positioned in logical coordinates ---- */

void sb_sprite_show(struct sb_sprite *sprite, struct canvas *canvas) {
	wl_surface_attach(sprite->surface, canvas ? canvas->buffer : NULL, 0, 0);
	if (canvas) {
		canvas->busy = true;
		wl_surface_damage_buffer(sprite->surface, 0, 0, INT32_MAX, INT32_MAX);
	}
	wl_surface_commit(sprite->surface);
}

void sb_sprite_init(struct starboard *sb, struct wl_surface *parent,
		struct sb_sprite *sprite, struct canvas *canvas) {
	sprite->surface = sb_surface_create(sb, false);
	sprite->subsurface = wl_subcompositor_get_subsurface(sb->subcompositor,
		sprite->surface, parent);
	sb_sprite_show(sprite, canvas);
}

void sb_sprite_move(struct sb_sprite *sprite, double x, double y) {
	sprite->x = x;
	sprite->y = y;
	wl_subsurface_set_position(sprite->subsurface, lround(x), lround(y));
}

void sb_sprite_destroy(struct sb_sprite *sprite) {
	if (sprite->subsurface) {
		wl_subsurface_destroy(sprite->subsurface);
		wl_surface_destroy(sprite->surface);
		sprite->subsurface = NULL;
		sprite->surface = NULL;
	}
}

/* ---- scenes: application windows reported by the compositor ---- */

struct sb_scene *sb_foreground_scene(struct starboard *sb) {
	struct sb_scene *scene;
	wl_list_for_each(scene, &sb->scenes, link) {
		if (scene->foreground) {
			return scene;
		}
	}
	return NULL;
}

/* The program an app's command runs: first word, after an optional "exec". */
static void command_program(const char *command, char *program, size_t size) {
	if (strncmp(command, "exec ", 5) == 0) {
		command += 5;
	}
	size_t length = strcspn(command, " ");
	const char *slash = memrchr(command, '/', length);
	if (slash) {
		length -= slash + 1 - command;
		command = slash + 1;
	}
	snprintf(program, size, "%.*s", (int)length, command);
}

/* After a starboard restart the windows already open belong to nobody;
 * recognise them by the program running in the window's process. */
static void adopt_scene(struct starboard *sb, struct sb_scene *scene) {
	char path[64], argv0[256] = "";
	snprintf(path, sizeof(path), "/proc/%d/cmdline", scene->pid);
	FILE *file = fopen(path, "r");
	if (!file) {
		return;
	}
	size_t length = fread(argv0, 1, sizeof(argv0) - 1, file);
	fclose(file);
	argv0[length] = 0;
	const char *base = strrchr(argv0, '/');
	base = base ? base + 1 : argv0;
	for (int i = 0; i < sb->app_count; i++) {
		char program[64];
		command_program(sb->apps[i].command, program, sizeof(program));
		if (!sb->apps[i].pid && strcmp(program, base) == 0) {
			sb->apps[i].pid = scene->pid;
			sb_log("Adopted %s window of %d", sb->apps[i].name, scene->pid);
			return;
		}
	}
}

static void scene_info(void *data, struct starry_scene_v1 *object, int32_t pid,
		const char *app_id, const char *title) {
	struct sb_scene *scene = data;
	bool known = scene->pid == pid;
	scene->pid = pid;
	snprintf(scene->app_id, sizeof(scene->app_id), "%s", app_id);
	for (int i = 0; i < scene->sb->app_count && !known; i++) {
		known = scene->sb->apps[i].pid == pid;
	}
	if (!known) {
		adopt_scene(scene->sb, scene);
	}
}

static void scene_state(void *data, struct starry_scene_v1 *object, uint32_t state) {
	struct sb_scene *scene = data;
	scene->foreground = state == STARRY_SCENE_V1_STATE_FOREGROUND;
	if (scene->foreground) {
		scene->last_foreground = sb_now_msec();
		switcher_hide(scene->sb);
	}
	status_update(scene->sb);
}

static void scene_closed(void *data, struct starry_scene_v1 *object) {
	struct sb_scene *scene = data;
	struct starboard *sb = scene->sb;
	switcher_scene_closed(sb, scene);
	wl_list_remove(&scene->link);
	starry_scene_v1_destroy(object);
	free(scene);
	status_update(sb);
}

static const struct starry_scene_v1_listener scene_listener = {
	.info = scene_info,
	.state = scene_state,
	.closed = scene_closed,
};

static void system_output(void *data, struct starry_system_v1 *system,
		int32_t width, int32_t height, int32_t scale) {
	struct starboard *sb = data;
	sb->width = width;
	sb->height = height;
}

static void system_scene(void *data, struct starry_system_v1 *system,
		struct starry_scene_v1 *object) {
	struct starboard *sb = data;
	struct sb_scene *scene = calloc(1, sizeof(*scene));
	scene->sb = sb;
	scene->scene = object;
	wl_list_insert(&sb->scenes, &scene->link);
	starry_scene_v1_add_listener(object, &scene_listener, scene);
}

static void system_switcher(void *data, struct starry_system_v1 *system, int32_t y) {
	switcher_show(data, y);
}

static const struct starry_system_v1_listener system_listener = {
	.output = system_output,
	.scene = system_scene,
	.switcher = system_switcher,
};

/* ---- launching (the FrontBoard part): reuse a live scene, else start the app ---- */

const char *sb_app_name_for_pid(struct starboard *sb, pid_t pid) {
	for (int i = 0; i < sb->app_count; i++) {
		if (pid && sb->apps[i].pid == pid) {
			return sb->apps[i].name;
		}
	}
	return NULL;
}

void sb_launch(struct starboard *sb, int index) {
	struct sb_app *app = &sb->apps[index];
	struct sb_scene *scene;
	wl_list_for_each(scene, &sb->scenes, link) {
		if (app->pid && scene->pid == app->pid) {
			starry_scene_v1_activate(scene->scene, STARRY_SCENE_V1_ANIMATION_SLIDE);
			return;
		}
	}
	if (app->pid) {
		return; /* Started, but no window yet. */
	}
	pid_t pid = fork();
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", app->command, (char *)NULL);
		_exit(127);
	}
	if (pid > 0) {
		app->pid = pid;
		sb_log("Launched %s as %d", app->name, pid);
	}
}

static void reap_children(struct starboard *sb, int fd) {
	struct signalfd_siginfo info;
	while (read(fd, &info, sizeof(info)) == sizeof(info)) {
	}
	pid_t pid;
	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		for (int i = 0; i < sb->app_count; i++) {
			if (sb->apps[i].pid == pid) {
				sb->apps[i].pid = 0;
			}
		}
	}
}

/* ---- touch: only the home surface takes input ---- */

static struct sb_touch *touch_point(struct starboard *sb, int32_t id) {
	return id >= 0 && id < SB_MAX_TOUCH ? &sb->touches[id] : NULL;
}

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
		struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct starboard *sb = data;
	struct sb_touch *point = touch_point(sb, id);
	if (!point) {
		return;
	}
	sb->touch_surface[id] = surface;
	*point = (struct sb_touch){
		.down = true, .app = -1, .last_msec = time,
		.start_x = wl_fixed_to_double(x), .start_y = wl_fixed_to_double(y),
		.x = wl_fixed_to_double(x), .y = wl_fixed_to_double(y),
	};
	if (surface == sb->home) {
		home_touch_down(sb, point);
	} else if (surface == sb->switcher) {
		switcher_touch_down(sb, point);
	}
}

static void touch_up(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
		int32_t id) {
	struct starboard *sb = data;
	struct sb_touch *point = touch_point(sb, id);
	if (point && point->down) {
		point->down = false;
		if (sb->touch_surface[id] == sb->home) {
			home_touch_up(sb, point);
		} else if (sb->touch_surface[id] == sb->switcher) {
			switcher_touch_up(sb, point, time);
		}
	}
}

static void touch_motion(void *data, struct wl_touch *touch, uint32_t time, int32_t id,
		wl_fixed_t x, wl_fixed_t y) {
	struct starboard *sb = data;
	struct sb_touch *point = touch_point(sb, id);
	if (point && point->down) {
		double nx = wl_fixed_to_double(x), ny = wl_fixed_to_double(y);
		uint32_t elapsed = time - point->last_msec;
		if (elapsed > 0) {
			point->velocity_x = 0.5 * point->velocity_x + 0.5 * (nx - point->x) / elapsed;
			point->velocity_y = 0.5 * point->velocity_y + 0.5 * (ny - point->y) / elapsed;
			point->last_msec = time;
		}
		point->x = nx;
		point->y = ny;
		if (sb->touch_surface[id] == sb->home) {
			home_touch_motion(sb, point);
		} else if (sb->touch_surface[id] == sb->switcher) {
			switcher_touch_motion(sb, point, time);
		}
	}
}

static void touch_frame(void *data, struct wl_touch *touch) {
}

static void touch_cancel(void *data, struct wl_touch *touch) {
	struct starboard *sb = data;
	for (int id = 0; id < SB_MAX_TOUCH; id++) {
		struct sb_touch *point = &sb->touches[id];
		if (point->down) {
			point->down = false;
			point->app = -1; /* never a tap */
			if (sb->touch_surface[id] == sb->home) {
				home_touch_up(sb, point);
			} else if (sb->touch_surface[id] == sb->switcher) {
				switcher_touch_up(sb, point, 0);
			}
		}
	}
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
	struct starboard *sb = data;
	if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !sb->touch) {
		sb->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(sb->touch, &touch_listener, sb);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- globals ---- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct starboard *sb = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		sb->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		sb->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		sb->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !sb->seat) {
		sb->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(sb->seat, &seat_listener, sb);
	} else if (strcmp(interface, starry_system_v1_interface.name) == 0) {
		sb->system = wl_registry_bind(registry, name, &starry_system_v1_interface, 1);
		starry_system_v1_add_listener(sb->system, &system_listener, sb);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static const char *env_or(const char *name, const char *fallback) {
	const char *value = getenv(name);
	return value && *value ? value : fallback;
}

static int next_timeout(struct starboard *sb) {
	struct sb_timer *timers[] = { &sb->long_press, &sb->status_tick };
	uint32_t now = sb_now_msec();
	int timeout = -1;
	for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
		if (timers[i]->deadline) {
			int remaining = (int32_t)(timers[i]->deadline - now);
			remaining = remaining < 0 ? 0 : remaining;
			timeout = timeout < 0 || remaining < timeout ? remaining : timeout;
		}
	}
	return timeout;
}

static void run_timers(struct starboard *sb) {
	struct sb_timer *timers[] = { &sb->long_press, &sb->status_tick };
	uint32_t now = sb_now_msec();
	for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
		if (timers[i]->deadline && (int32_t)(timers[i]->deadline - now) <= 0) {
			timers[i]->deadline = 0;
			timers[i]->callback(sb);
		}
	}
}

int main(void) {
	signal(SIGSEGV, crash_handler);
	signal(SIGABRT, crash_handler);
	signal(SIGBUS, crash_handler);
	struct starboard sb = { .running = true };
	wl_list_init(&sb.scenes);

	sb.display = wl_display_connect(NULL);
	unsetenv("WAYLAND_SOCKET"); /* applications must use WAYLAND_DISPLAY */
	if (!sb.display) {
		sb_log("Cannot connect to the compositor");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(sb.display);
	wl_registry_add_listener(registry, &registry_listener, &sb);
	wl_display_roundtrip(sb.display);
	wl_display_roundtrip(sb.display);
	if (!sb.compositor || !sb.subcompositor || !sb.shm || !sb.system || !sb.width) {
		sb_log("Missing globals; was starboard started by starry-compositor?");
		return 1;
	}
	if (!canvas_font_init("/usr/share/fonts/ttf/LiberationSans-Regular.ttf",
			"/usr/share/fonts/ttf/LiberationSans-Bold.ttf")) {
		return 1;
	}

	sigset_t chld;
	sigemptyset(&chld);
	sigaddset(&chld, SIGCHLD);
	sigprocmask(SIG_BLOCK, &chld, NULL);
	int sigchld_fd = signalfd(-1, &chld, SFD_NONBLOCK | SFD_CLOEXEC);

	home_load_apps(&sb, env_or("STARRY_APPS", STARRY_APPS_PATH), "/var/lib/starry/layout");
	/* Windows announced while binding arrived before the catalogue was loaded. */
	struct sb_scene *scene;
	wl_list_for_each(scene, &sb.scenes, link) {
		adopt_scene(&sb, scene);
	}
	home_create(&sb);
	status_load_plugins(&sb, env_or("STARRY_PLUGIN_DIR", STARRY_PLUGIN_DIR));
	status_create(&sb);
	switcher_create(&sb);
	sb_log("Ready: %dx%d, %d apps, %d status items", sb.width, sb.height,
		sb.app_count, sb.status_item_count);

	while (sb.running) {
		while (wl_display_prepare_read(sb.display) != 0) {
			if (wl_display_dispatch_pending(sb.display) < 0) {
				return 1;
			}
		}
		if (wl_display_flush(sb.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(sb.display);
			return 1;
		}
		struct pollfd fds[2] = {
			{ .fd = wl_display_get_fd(sb.display), .events = POLLIN },
			{ .fd = sigchld_fd, .events = POLLIN },
		};
		if (poll(fds, 2, next_timeout(&sb)) < 0 && errno != EINTR) {
			wl_display_cancel_read(sb.display);
			return 1;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(sb.display) < 0) {
				return 1;
			}
		} else {
			wl_display_cancel_read(sb.display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) {
			sb_log("Compositor went away");
			return 1;
		}
		if (wl_display_dispatch_pending(sb.display) < 0) {
			return 1;
		}
		if (fds[1].revents & POLLIN) {
			reap_children(&sb, sigchld_fd);
		}
		run_timers(&sb);
	}
	return 0;
}
