/* SPDX-License-Identifier: MIT */
#ifndef STARRY_STARBOARD_H
#define STARRY_STARBOARD_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <wayland-client.h>
#include "starry-plugin.h"

#define sb_log(format, ...) fprintf(stderr, "starboard: " format "\n", ##__VA_ARGS__)

#define SB_SCALE 3
#define SB_STATUS_HEIGHT 28
#define SB_MAX_APPS 24
#define SB_MAX_TOUCH 10
#define SB_MAX_STATUS_ITEMS 16
#define SB_MAX_CARDS 16

struct canvas;
struct cn_client;
struct cn_object;

struct sb_battery_state {
	bool present;
	int level; /* percent */
	bool charging;
	bool saver;
};

/* A subsurface of the home surface; positions are logical. */
struct sb_sprite {
	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	double x, y;
};

#define SB_APP_ID_MAX 129
#define SB_ICON_STYLES 4
#define SB_DEFAULT_ACCENT 0xffb340 /* Stardust's Theme uses the same */
enum sb_icon_style { SB_ICONS_DEFAULT, SB_ICONS_DARK, SB_ICONS_TINTED, SB_ICONS_CLEAR };

struct sb_app {
	char name[64];
	char command[192]; /* catalogue lines only */
	uint32_t color;
	bool dock;
	char id[SB_APP_ID_MAX]; /* an installed package; empty for catalogue lines */
	char icons[SB_ICON_STYLES][192]; /* PNG per appearance (default, dark, tinted, clear) */
	uint8_t *image; /* the decoded icon in use, and which file it came from */
	int image_width, image_height;
	char image_path[192];
	pid_t pid;
	struct sb_sprite icon, label;
	struct canvas *icon_canvas, *lifted_canvas, *label_canvas;
	double x, y; /* current position; eases towards the slot */
};

struct sb_scene {
	struct wl_list link;
	struct starboard *sb;
	struct starry_scene_v1 *scene;
	pid_t pid;
	char app_id[64];
	bool foreground;
	uint32_t last_foreground;
};

struct sb_card {
	struct sb_scene *scene; /* NULL once the window is gone */
	struct starry_scene_view_v1 *view;
	struct sb_sprite label;
	struct canvas *label_canvas;
	double x, y, width, height; /* current geometry, eased towards the target */
	double lift;                /* vertical drag; negative is up */
	bool closing;
};

struct sb_touch {
	bool down;
	double start_x, start_y, x, y;
	double velocity_x, velocity_y; /* logical px per ms, smoothed */
	uint32_t last_msec;
	int app; /* index, -1 for none, -2 for the Done button */
};

struct sb_timer {
	uint32_t deadline; /* 0 = disarmed */
	void (*callback)(struct starboard *sb);
};

struct sb_status_item {
	struct starry_status_item item;
	char text[64];
	uint32_t next_poll;
};

struct starboard {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_touch *touch;
	struct starry_system_v1 *system;
	int width, height;
	bool running;

	/* home.c */
	struct wl_surface *home;
	struct starry_layer_surface_v1 *home_layer;
	struct canvas *wallpaper;
	struct sb_sprite done;
	struct canvas *done_canvas;
	struct sb_app apps[SB_MAX_APPS];
	/* The saved order, for packages that announce themselves later. */
	struct sb_place { char key[SB_APP_ID_MAX]; bool dock; } places[SB_MAX_APPS * 2];
	int place_count;
	/* /blinks/system/appearance: which icons to show and the accent to tint them */
	enum sb_icon_style icon_style;
	uint32_t accent;
	int app_count;
	bool editing;
	int dragging;
	struct sb_touch *drag_touch;
	double drag_dx, drag_dy;
	struct sb_touch *pressed;
	struct sb_timer long_press;
	struct wl_callback *home_frame;
	uint32_t frame_msec;
	int haptic_fd, haptic_effect;
	struct sb_touch touches[SB_MAX_TOUCH];
	struct wl_surface *touch_surface[SB_MAX_TOUCH];

	/* status.c */
	struct wl_surface *status;
	struct starry_layer_surface_v1 *status_layer;
	struct canvas *status_canvas;
	char status_key[256];
	struct wl_surface *indicator;
	struct starry_layer_surface_v1 *indicator_layer;
	struct canvas *indicator_canvas;
	bool indicator_shown;
	struct sb_status_item status_items[SB_MAX_STATUS_ITEMS];
	int status_item_count;
	struct sb_timer status_tick;
	struct cn_client *blinks; /* Constellation, for /blinks/power */
	struct sb_battery_state battery;

	/* switcher.c */
	struct wl_surface *switcher;
	struct starry_layer_surface_v1 *switcher_layer;
	struct canvas *backdrop;
	bool switcher_shown;
	struct sb_card cards[SB_MAX_CARDS];
	int card_count;
	double scroll, scroll_target;
	int opening; /* card zooming to full screen, or -1 */
	struct sb_touch *switcher_touch;
	int touch_card;
	enum { SB_AXIS_NONE, SB_AXIS_HORIZONTAL, SB_AXIS_VERTICAL } touch_axis;
	double touch_scroll;
	struct wl_callback *switcher_frame;
	uint32_t switcher_frame_msec;

	struct wl_list scenes;
};

/* main.c */
uint32_t sb_now_msec(void);
struct wl_surface *sb_surface_create(struct starboard *sb, bool input);
void sb_timer_arm(struct sb_timer *timer, uint32_t delay_msec);
struct sb_scene *sb_foreground_scene(struct starboard *sb);
void sb_sprite_init(struct starboard *sb, struct wl_surface *parent,
	struct sb_sprite *sprite, struct canvas *canvas);
void sb_sprite_show(struct sb_sprite *sprite, struct canvas *canvas);
void sb_sprite_move(struct sb_sprite *sprite, double x, double y);
void sb_sprite_destroy(struct sb_sprite *sprite);
const char *sb_app_name_for_pid(struct starboard *sb, pid_t pid);
void sb_launch(struct starboard *sb, int index);

/* home.c */
void home_load_apps(struct starboard *sb, const char *catalog, const char *layout);
void home_create(struct starboard *sb);
/* An installed app in /blinks/apps/ appeared, changed or went away. */
void home_app_changed(struct starboard *sb, const struct cn_object *object, bool exists);
/* The system appearance changed: icon style or accent. */
void home_appearance_changed(struct starboard *sb, const struct cn_object *object, bool exists);

/* icons.c */
uint8_t *icon_load(const char *path, int *width, int *height);
/* Scales the icon into the canvas's top-left size×size square. */
void icon_draw(struct canvas *canvas, const uint8_t *rgba, int width, int height, int size,
	bool tint, uint32_t accent);
void home_touch_down(struct starboard *sb, struct sb_touch *point);
void home_touch_motion(struct starboard *sb, struct sb_touch *point);
void home_touch_up(struct starboard *sb, struct sb_touch *point);

/* switcher.c */
void switcher_create(struct starboard *sb);
void switcher_show(struct starboard *sb, int from_y);
void switcher_hide(struct starboard *sb);
void switcher_scene_closed(struct starboard *sb, struct sb_scene *scene);
void switcher_touch_down(struct starboard *sb, struct sb_touch *point);
void switcher_touch_motion(struct starboard *sb, struct sb_touch *point, uint32_t time);
void switcher_touch_up(struct starboard *sb, struct sb_touch *point, uint32_t time);

/* battery.c */
bool battery_load(const char *svg_path, const char *map_path);
/* Updates the state from /blinks/power/battery or /blinks/power/saver. */
void battery_changed(struct sb_battery_state *state, const struct cn_object *object, bool exists);
/* Draws the battery right-aligned at right; returns the width used, in pixels. */
double battery_draw(struct canvas *canvas, double right, double top, double height,
	const struct sb_battery_state *state, bool dark);

/* status.c */
void status_create(struct starboard *sb);
void status_update(struct starboard *sb);
/* Reads Constellation changes; call when status_blinks_fd() is readable. */
void status_blinks_dispatch(struct starboard *sb);
int status_blinks_fd(struct starboard *sb);
void status_load_plugins(struct starboard *sb, const char *directory);

#endif
