#ifndef STARRY_SPRINGBOARD_H
#define STARRY_SPRINGBOARD_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <wayland-client.h>
#include "starry-plugin.h"

#define sb_log(format, ...) fprintf(stderr, "springboard: " format "\n", ##__VA_ARGS__)

#define SB_SCALE 3
#define SB_STATUS_HEIGHT 28
#define SB_MAX_APPS 24
#define SB_MAX_TOUCH 10
#define SB_MAX_STATUS_ITEMS 16

struct canvas;

/* A subsurface of the home surface; positions are logical. */
struct sb_sprite {
	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	double x, y;
};

struct sb_app {
	char name[32];
	char command[192];
	uint32_t color;
	bool dock;
	pid_t pid;
	struct sb_sprite icon, label;
	struct canvas *icon_canvas, *lifted_canvas, *label_canvas;
	double x, y; /* current position; eases towards the slot */
};

struct sb_scene {
	struct wl_list link;
	struct springboard *sb;
	struct starry_scene_v1 *scene;
	pid_t pid;
	char app_id[64];
	bool foreground;
};

struct sb_touch {
	bool down;
	double start_x, start_y, x, y;
	int app; /* index, -1 for none, -2 for the Done button */
};

struct sb_timer {
	uint32_t deadline; /* 0 = disarmed */
	void (*callback)(struct springboard *sb);
};

struct sb_status_item {
	struct starry_status_item item;
	char text[64];
	uint32_t next_poll;
};

struct springboard {
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

	struct wl_list scenes;
};

/* main.c */
uint32_t sb_now_msec(void);
struct wl_surface *sb_surface_create(struct springboard *sb, bool input);
void sb_timer_arm(struct sb_timer *timer, uint32_t delay_msec);
struct sb_scene *sb_foreground_scene(struct springboard *sb);
void sb_launch(struct springboard *sb, int index);

/* home.c */
void home_load_apps(struct springboard *sb, const char *catalog, const char *layout);
void home_create(struct springboard *sb);
void home_touch_down(struct springboard *sb, struct sb_touch *point);
void home_touch_motion(struct springboard *sb, struct sb_touch *point);
void home_touch_up(struct springboard *sb, struct sb_touch *point);

/* status.c */
void status_create(struct springboard *sb);
void status_update(struct springboard *sb);
void status_load_plugins(struct springboard *sb, const char *directory);

#endif
