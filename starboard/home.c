/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "canvas.h"
#include "starry-system-v1-client-protocol.h"
#include "starboard.h"

#define ICON 60
#define LIFT 6 /* extra size per side while an icon is held */
#define LABEL_WIDTH 84
#define LABEL_HEIGHT 18
#define COLUMNS 4
#define ROW_PITCH 92
#define GRID_TOP (SB_STATUS_HEIGHT + 24)
#define DOCK_MARGIN 12
#define DOCK_HEIGHT 92
#define DOCK_MAX 4
#define LONG_PRESS_MSEC 450
#define TAP_SLOP 12
#define LAYOUT_DIR "/var/lib/starry"

static const struct sb_app default_apps[] = {
	{ "Terminal", "exec weston-terminal", 0xff30343b, true },
	{ "Flower", "exec weston-flower", 0xffeb5757, true },
	{ "Touch", "exec weston-simple-touch", 0xff27ae60, true },
	{ "EGL", "exec weston-simple-egl", 0xff9b51e0, false },
	{ "Smoke", "exec weston-smoke", 0xff56ccf2, false },
	{ "Clickdot", "exec weston-clickdot", 0xfff2994a, false },
	{ "Image", "exec weston-image /usr/share/weston/background.png", 0xff219653, false },
	{ "Gears", "exec weston-simple-dmabuf-egl", 0xff6fcf97, false },
};

static const char *layout_path = LAYOUT_DIR "/layout";

static int dock_count(struct starboard *sb) {
	int count = 0;
	for (int i = 0; i < sb->app_count; i++) {
		count += sb->apps[i].dock;
	}
	return count;
}

/* ---- catalogue and saved order (the IconState of this starboard) ---- */

static int find_app(struct sb_app *apps, int count, const char *name) {
	for (int i = 0; i < count; i++) {
		if (strcmp(apps[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

/* Catalogue: one app per line, Name|#RRGGBB|dock(0/1)|shell command.
 * Layout: "dock Name" or "grid Name" per line, in display order. */
void home_load_apps(struct starboard *sb, const char *catalog, const char *layout) {
	struct sb_app apps[SB_MAX_APPS];
	int count = 0;
	FILE *file = fopen(catalog, "r");
	if (file) {
		char line[320];
		while (count < SB_MAX_APPS && fgets(line, sizeof(line), file)) {
			struct sb_app *app = &apps[count];
			unsigned color;
			int dock;
			memset(app, 0, sizeof(*app));
			if (line[0] == '#' || sscanf(line, "%31[^|]|#%6x|%d|%191[^\n]",
					app->name, &color, &dock, app->command) != 4) {
				continue;
			}
			app->color = 0xff000000 | color;
			app->dock = dock;
			count++;
		}
		fclose(file);
	}
	if (count == 0) {
		count = sizeof(default_apps) / sizeof(default_apps[0]);
		memcpy(apps, default_apps, sizeof(default_apps));
	}

	sb->app_count = 0;
	bool used[SB_MAX_APPS] = {0};
	int docked = 0;
	file = fopen(layout, "r");
	if (file) {
		char place[8], name[32];
		while (fscanf(file, "%7s %31[^\n]\n", place, name) == 2) {
			int index = find_app(apps, count, name);
			if (index < 0 || used[index]) {
				continue;
			}
			used[index] = true;
			apps[index].dock = strcmp(place, "dock") == 0 && docked < DOCK_MAX;
			docked += apps[index].dock;
			sb->apps[sb->app_count++] = apps[index];
		}
		fclose(file);
	}
	/* Apps new to the catalogue keep their default place where it fits. */
	for (int i = 0; i < count; i++) {
		if (!used[i]) {
			apps[i].dock = apps[i].dock && docked < DOCK_MAX;
			docked += apps[i].dock;
			sb->apps[sb->app_count++] = apps[i];
		}
	}
}

static void save_layout(struct starboard *sb) {
	mkdir(LAYOUT_DIR, 0755);
	char temporary[64];
	snprintf(temporary, sizeof(temporary), "%s.new", layout_path);
	FILE *file = fopen(temporary, "w");
	if (!file) {
		sb_log("Cannot save %s", layout_path);
		return;
	}
	for (int i = 0; i < sb->app_count; i++) {
		fprintf(file, "%s %s\n", sb->apps[i].dock ? "dock" : "grid", sb->apps[i].name);
	}
	fflush(file);
	fsync(fileno(file));
	fclose(file);
	rename(temporary, layout_path);
}

/* ---- geometry ---- */

static int group_position(struct starboard *sb, int index) {
	int position = 0;
	for (int i = 0; i < index; i++) {
		position += sb->apps[i].dock == sb->apps[index].dock;
	}
	return position;
}

/* Top-left corner of an app's icon slot in logical coordinates. */
static void slot_position(struct starboard *sb, int index, double *x, double *y) {
	int position = group_position(sb, index);
	if (sb->apps[index].dock) {
		int count = dock_count(sb);
		double inner = sb->width - 2 * DOCK_MARGIN;
		double gap = (inner - count * ICON) / (count + 1);
		*x = DOCK_MARGIN + gap + position * (ICON + gap);
		*y = sb->height - DOCK_MARGIN - DOCK_HEIGHT + (DOCK_HEIGHT - ICON) / 2.0;
	} else {
		double gap = (sb->width - COLUMNS * ICON) / (COLUMNS + 1.0);
		*x = gap + (position % COLUMNS) * (ICON + gap);
		*y = GRID_TOP + (position / COLUMNS) * ROW_PITCH;
	}
}

static int app_at(struct starboard *sb, double x, double y) {
	for (int i = 0; i < sb->app_count; i++) {
		double ix, iy;
		slot_position(sb, i, &ix, &iy);
		if (x >= ix - 8 && x < ix + ICON + 8 && y >= iy - 8 && y < iy + ICON + 8) {
			return i;
		}
	}
	return -1;
}

static bool in_done_button(struct starboard *sb, double x, double y) {
	return sb->editing && x >= sb->width - 90 && y >= SB_STATUS_HEIGHT &&
		y < SB_STATUS_HEIGHT + 40;
}

/* ---- drawing ---- */

static void draw_app(struct starboard *sb, struct sb_app *app) {
	const double s = SB_SCALE;
	for (int lifted = 0; lifted < 2; lifted++) {
		double size = (ICON + (lifted ? 2 * LIFT : 0)) * s;
		struct canvas *icon = canvas_create(sb->shm, size, size);
		canvas_round_rect(icon, 0, 0, size, size, size * 14 / ICON, app->color);
		/* Soft top highlight gives the flat colour some depth. */
		canvas_round_rect(icon, 0, 0, size, size / 2, size * 14 / ICON, 0x1affffff);
		/* The first character, which may take several UTF-8 bytes. */
		char initial[8] = "";
		size_t length = 1;
		while (length < sizeof(initial) - 1 &&
				((unsigned char)app->name[length] & 0xc0) == 0x80) {
			length++;
		}
		memcpy(initial, app->name, length);
		double text = size / 2;
		double width = canvas_text_width(true, text, initial);
		canvas_text(icon, true, text, size / 2 - width / 2, size / 2 + text / 3,
			0xffffffff, initial);
		*(lifted ? &app->lifted_canvas : &app->icon_canvas) = icon;
	}
	app->label_canvas = canvas_create(sb->shm, LABEL_WIDTH * s, LABEL_HEIGHT * s);
	double width = canvas_text_width(false, 12 * s, app->name);
	canvas_text(app->label_canvas, false, 12 * s, LABEL_WIDTH * s / 2 - width / 2, 13 * s,
		0xffffffff, app->name);
}

static void place_app(struct sb_app *app, bool lifted) {
	double offset = lifted ? -LIFT : 0;
	sb_sprite_move(&app->icon, app->x + offset, app->y + offset);
	sb_sprite_move(&app->label, app->x + (ICON - LABEL_WIDTH) / 2.0, app->y + ICON + 3);
}

static void set_lifted(struct starboard *sb, struct sb_app *app, bool lifted) {
	sb_sprite_show(&app->icon, lifted ? app->lifted_canvas : app->icon_canvas);
	sb_sprite_show(&app->label, lifted || app->dock ? NULL : app->label_canvas);
	if (lifted) {
		wl_subsurface_place_above(app->icon.subsurface, sb->done.surface);
	}
	place_app(app, lifted);
}

static void long_press(struct starboard *sb);

void home_create(struct starboard *sb) {
	const int s = SB_SCALE, w = sb->width, h = sb->height;
	sb->long_press.callback = long_press;
	sb->home = sb_surface_create(sb, true);
	sb->home_layer = starry_system_v1_get_layer_surface(sb->system, sb->home,
		STARRY_SYSTEM_V1_LAYER_HOME);

	sb->wallpaper = canvas_create(sb->shm, w * s, h * s);
	canvas_vertical_gradient(sb->wallpaper, 0xff141e46, 0xff4a1d5c);
	canvas_round_rect(sb->wallpaper, -0.3 * w * s, 0.55 * h * s, 1.1 * w * s, 1.1 * w * s,
		0.55 * w * s, 0x26ff9ec7);
	canvas_round_rect(sb->wallpaper, 0.45 * w * s, 0.08 * h * s, 0.8 * w * s, 0.8 * w * s,
		0.4 * w * s, 0x1f7fd8ff);
	canvas_round_rect(sb->wallpaper, DOCK_MARGIN * s, (h - DOCK_MARGIN - DOCK_HEIGHT) * s,
		(w - 2 * DOCK_MARGIN) * s, DOCK_HEIGHT * s, 30 * s, 0x40ffffff);

	for (int i = 0; i < sb->app_count; i++) {
		struct sb_app *app = &sb->apps[i];
		draw_app(sb, app);
		sb_sprite_init(sb, sb->home, &app->icon, app->icon_canvas);
		sb_sprite_init(sb, sb->home, &app->label, app->dock ? NULL : app->label_canvas);
		slot_position(sb, i, &app->x, &app->y);
		place_app(app, false);
	}

	sb->done_canvas = canvas_create(sb->shm, 64 * s, 28 * s);
	canvas_round_rect(sb->done_canvas, 0, 0, 64 * s, 28 * s, 14 * s, 0x59ffffff);
	double width = canvas_text_width(true, 14 * s, "Done");
	canvas_text(sb->done_canvas, true, 14 * s, 32 * s - width / 2, 19 * s, 0xffffffff, "Done");
	sb_sprite_init(sb, sb->home, &sb->done, NULL);
	sb_sprite_move(&sb->done, w - 78, SB_STATUS_HEIGHT + 6);

	sb->dragging = -1;
	sb->haptic_fd = -1;
	wl_surface_attach(sb->home, sb->wallpaper->buffer, 0, 0);
	sb->wallpaper->busy = true;
	wl_surface_commit(sb->home);
}

/* ---- motion: icons ease towards their slots on frame callbacks ---- */

static void home_schedule(struct starboard *sb);

static bool home_step(struct starboard *sb) {
	uint32_t msec = sb_now_msec();
	double elapsed = sb->frame_msec ? fmin(50, msec - sb->frame_msec) : 16;
	sb->frame_msec = msec;
	double follow = 1 - exp(-elapsed / 70.0);
	bool moving = false;
	for (int i = 0; i < sb->app_count; i++) {
		struct sb_app *app = &sb->apps[i];
		if (i == sb->dragging) {
			continue;
		}
		double x, y;
		slot_position(sb, i, &x, &y);
		if (fabs(x - app->x) < 0.5 && fabs(y - app->y) < 0.5) {
			if (app->x != x || app->y != y) {
				app->x = x;
				app->y = y;
				place_app(app, false);
			}
			continue;
		}
		app->x += (x - app->x) * follow;
		app->y += (y - app->y) * follow;
		place_app(app, false);
		moving = true;
	}
	if (!moving) {
		sb->frame_msec = 0;
	}
	return moving;
}

static void home_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct starboard *sb = data;
	wl_callback_destroy(callback);
	sb->home_frame = NULL;
	if (home_step(sb)) {
		home_schedule(sb);
	} else {
		wl_surface_commit(sb->home);
	}
}

static const struct wl_callback_listener home_frame_listener = {
	.done = home_frame_done,
};

static void home_schedule(struct starboard *sb) {
	if (!sb->home_frame) {
		sb->home_frame = wl_surface_frame(sb->home);
		wl_callback_add_listener(sb->home_frame, &home_frame_listener, sb);
	}
	wl_surface_commit(sb->home);
}

/* ---- haptics: one short AW8624 tick when an icon is picked up ---- */

static void haptic_open(struct starboard *sb) {
	sb->haptic_effect = -1;
	DIR *dir = opendir("/sys/class/input");
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		if (strncmp(entry->d_name, "event", 5) != 0) {
			continue;
		}
		char path[320], name[64] = "";
		snprintf(path, sizeof(path), "/sys/class/input/%s/device/name", entry->d_name);
		FILE *file = fopen(path, "r");
		if (file) {
			if (!fgets(name, sizeof(name), file)) {
				name[0] = 0;
			}
			fclose(file);
		}
		if (strncmp(name, "AW8624", 6) == 0) {
			snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
			sb->haptic_fd = open(path, O_RDWR | O_CLOEXEC);
			break;
		}
	}
	if (dir) {
		closedir(dir);
	}
	struct ff_effect effect = {
		.type = FF_RUMBLE, .id = -1,
		.u.rumble.strong_magnitude = 0xc000,
		.replay.length = 25,
	};
	if (sb->haptic_fd >= 0 && ioctl(sb->haptic_fd, EVIOCSFF, &effect) == 0) {
		sb->haptic_effect = effect.id;
	}
}

static void haptic_tick(struct starboard *sb) {
	if (sb->haptic_fd < 0) {
		haptic_open(sb);
	}
	if (sb->haptic_fd < 0 || sb->haptic_effect < 0) {
		return;
	}
	struct input_event event = { .type = EV_FF, .code = sb->haptic_effect, .value = 1 };
	if (write(sb->haptic_fd, &event, sizeof(event)) < 0) {
		sb_log("Haptic tick failed");
	}
}

/* ---- edit mode and dragging ---- */

static void set_editing(struct starboard *sb, bool editing) {
	sb->editing = editing;
	sb_sprite_show(&sb->done, editing ? sb->done_canvas : NULL);
	wl_surface_commit(sb->home);
}

static void start_drag(struct starboard *sb, struct sb_touch *point) {
	struct sb_app *app = &sb->apps[point->app];
	sb->dragging = point->app;
	sb->drag_touch = point;
	sb->drag_dx = point->x - app->x;
	sb->drag_dy = point->y - app->y;
	set_lifted(sb, app, true);
	set_editing(sb, true);
	haptic_tick(sb);
}

static int clamp(int value, int low, int high) {
	return value < low ? low : value > high ? high : value;
}

/* Moves the dragged app to the slot under the finger; the others ease aside. */
static void reorder(struct starboard *sb, double x, double y) {
	struct sb_app *app = &sb->apps[sb->dragging];
	int others_docked = dock_count(sb) - app->dock;
	bool to_dock = y >= sb->height - DOCK_MARGIN - DOCK_HEIGHT - 6 && others_docked < DOCK_MAX;
	int position;
	if (to_dock) {
		int count = others_docked + 1;
		double cell = (sb->width - 2.0 * DOCK_MARGIN) / count;
		position = clamp((int)((x - DOCK_MARGIN) / cell), 0, count - 1);
	} else {
		int count = sb->app_count - others_docked;
		int column = clamp((int)(x / (sb->width / (double)COLUMNS)), 0, COLUMNS - 1);
		int row = fmax(0, floor((y - GRID_TOP + (ROW_PITCH - ICON) / 2.0) / ROW_PITCH));
		position = clamp(row * COLUMNS + column, 0, count - 1);
	}
	if (to_dock == app->dock && position == group_position(sb, sb->dragging)) {
		return;
	}

	struct sb_app moved = *app;
	int count = sb->app_count - 1;
	memmove(&sb->apps[sb->dragging], &sb->apps[sb->dragging + 1],
		(count - sb->dragging) * sizeof(moved));
	moved.dock = to_dock;
	int insert = count, seen = 0;
	for (int i = 0; i < count; i++) {
		if (sb->apps[i].dock == to_dock && seen++ == position) {
			insert = i;
			break;
		}
	}
	memmove(&sb->apps[insert + 1], &sb->apps[insert], (count - insert) * sizeof(moved));
	sb->apps[insert] = moved;
	sb->dragging = insert;
	home_schedule(sb);
}

static void long_press(struct starboard *sb) {
	struct sb_touch *point = sb->pressed;
	sb->pressed = NULL;
	if (point && point->down && point->app >= 0 && sb->dragging < 0) {
		start_drag(sb, point);
	}
}

static bool moved_far(struct sb_touch *point) {
	return hypot(point->x - point->start_x, point->y - point->start_y) >= TAP_SLOP;
}

void home_touch_down(struct starboard *sb, struct sb_touch *point) {
	if (in_done_button(sb, point->x, point->y)) {
		point->app = -2;
		return;
	}
	point->app = app_at(sb, point->x, point->y);
	if (point->app < 0 || sb->dragging >= 0) {
		return;
	}
	if (sb->editing) {
		start_drag(sb, point);
	} else {
		sb->pressed = point;
		sb_timer_arm(&sb->long_press, LONG_PRESS_MSEC);
	}
}

void home_touch_motion(struct starboard *sb, struct sb_touch *point) {
	if (point == sb->drag_touch) {
		struct sb_app *app = &sb->apps[sb->dragging];
		app->x = point->x - sb->drag_dx;
		app->y = point->y - sb->drag_dy;
		place_app(app, true);
		reorder(sb, point->x, point->y);
		wl_surface_commit(sb->home);
		return;
	}
	if (point == sb->pressed && moved_far(point)) {
		sb->pressed = NULL;
		sb_timer_arm(&sb->long_press, 0);
	}
}

void home_touch_up(struct starboard *sb, struct sb_touch *point) {
	if (point == sb->pressed) {
		sb->pressed = NULL;
		sb_timer_arm(&sb->long_press, 0);
	}
	if (point == sb->drag_touch) {
		struct sb_app *app = &sb->apps[sb->dragging];
		sb->dragging = -1;
		sb->drag_touch = NULL;
		set_lifted(sb, app, false);
		save_layout(sb);
		home_schedule(sb);
		return;
	}
	bool tap = !moved_far(point);
	if (point->app == -2) {
		set_editing(sb, false);
	} else if (sb->editing) {
		if (tap && point->app < 0) {
			set_editing(sb, false);
		}
	} else if (tap && point->app >= 0 && app_at(sb, point->x, point->y) == point->app) {
		sb_launch(sb, point->app);
	}
}
