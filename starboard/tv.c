/* SPDX-License-Identifier: MIT */
/*
 * The TV home screen (STARRY_MODE=tv), steered by a remote: one or two rows
 * of app cards on a dark backdrop, favourites (the dock apps) on top and
 * the rest below, with a top shelf that shows the focused app large and the
 * clock (status.c) in the corner. The focused card lifts on a spring, casts
 * a soft shadow, catches a sheen and leans the way the focus came from.
 *
 * Every card is drawn once, at its lifted size, and the compositor scales
 * it through wp_viewport, so a focus move redraws only the caption and,
 * once the focus rests, the shelf. All art is drawn here on the CPU in
 * 8 bits and only sampled by the GPU, so mediump-only GPUs (Mali-450) show
 * it as drawn: solid faces, short ramps, no long gradients.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "starry-system-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "starboard.h"

#define MARGIN_X 96 /* overscan-safe area: 5% of 1920 */
#define CARD_W 300
#define CARD_H 180
#define CARD_RADIUS 20
#define CARD_GAP 44
#define LIFT 1.1
#define LIFTED_W 330 /* CARD_W * LIFT: the size cards are drawn at */
#define LIFTED_H 198
#define ROW_TOP 520
#define ROW_PITCH 270 /* card, caption and air */
#define CAPTION_W 640
#define CAPTION_H 48
#define CAPTION_SIZE 28
#define BLUR 36 /* the lifted card's shadow */
#define LEAN 8 /* px the focused card leans after a move */
#define SHELF_Y 124
#define SHELF_H 346
#define POSTER_W 560
#define POSTER_H 315
#define SHELF_DELAY 140 /* msec the focus rests before the shelf redraws */
#define REPEAT_MAX 10 /* focus moves per second while an arrow is held */
#define BACKDROP 0xff0b0d12
#define TEXT 0xfff2f4f8
#define TEXT_SECONDARY 0xff9aa3b2

/* ---- rows: favourites first, then the rest; one row when either is empty ---- */

static int row_of(struct starboard *sb, int index) {
	int docked = 0;
	for (int i = 0; i < sb->app_count; i++) {
		docked += sb->apps[i].dock;
	}
	if (docked == 0 || docked == sb->app_count) {
		return 0;
	}
	return sb->apps[index].dock ? 0 : 1;
}

static int column_of(struct starboard *sb, int index) {
	int column = 0, row = row_of(sb, index);
	for (int i = 0; i < index; i++) {
		column += row_of(sb, i) == row;
	}
	return column;
}

static int row_length(struct starboard *sb, int row) {
	int count = 0;
	for (int i = 0; i < sb->app_count; i++) {
		count += row_of(sb, i) == row;
	}
	return count;
}

static int app_at(struct starboard *sb, int row, int column) {
	for (int i = 0, seen = 0; i < sb->app_count; i++) {
		if (row_of(sb, i) == row && seen++ == column) {
			return i;
		}
	}
	return -1;
}

/* Top-left corner of a card at rest, before its row scrolls. */
static void slot(struct starboard *sb, int index, double *x, double *y) {
	*x = MARGIN_X + column_of(sb, index) * (CARD_W + CARD_GAP);
	*y = ROW_TOP + row_of(sb, index) * ROW_PITCH;
}

static double max_scroll(struct starboard *sb, int row) {
	int count = row_length(sb, row);
	double content = 2 * MARGIN_X + count * CARD_W + (count > 0 ? count - 1 : 0) * CARD_GAP;
	return fmax(0, content - sb->width);
}

/* Scrolls the card's row so that it lies inside the safe area. */
static void reveal(struct starboard *sb, int index) {
	int row = row_of(sb, index);
	double x, y, target = sb->tv_scroll_target[row];
	slot(sb, index, &x, &y);
	if (x + CARD_W - target > sb->width - MARGIN_X) {
		target = x + CARD_W - (sb->width - MARGIN_X);
	}
	if (x - target < MARGIN_X) {
		target = x - MARGIN_X;
	}
	sb->tv_scroll_target[row] = fmin(fmax(0, target), max_scroll(sb, row));
}

/* ---- art, drawn once ---- */

static double box_distance(double px, double py, double width, double height, double radius) {
	double qx = fabs(px - width / 2) - (width / 2 - radius);
	double qy = fabs(py - height / 2) - (height / 2 - radius);
	return hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - radius;
}

/* A card's face: its colour, lit a little along the top. */
static void draw_face(struct canvas *canvas, int x, int y, int width, int height,
		double radius, uint32_t color) {
	for (int py = 0; py < height; py++) {
		double t = (py + 0.5) / height;
		double light = t < 0.45 ? 0.09 * (1 - t / 0.45) : 0;
		uint32_t white = (uint32_t)lround(light * 255) << 24 | 0xffffff;
		for (int px = 0; px < width; px++) {
			double coverage = 0.5 - box_distance(px + 0.5, py + 0.5, width, height, radius);
			if (coverage <= 0) {
				continue;
			}
			canvas_blend(canvas, x + px, y + py, color, coverage);
			if (light > 0) {
				canvas_blend(canvas, x + px, y + py, white, coverage);
			}
		}
	}
}

/* The app's icon centred in a box, or its initial when it has none. */
static void draw_mark(struct starboard *sb, struct sb_app *app, struct canvas *canvas,
		int x, int y, int width, int height, int size) {
	const uint8_t *image = home_package_icon(sb, app);
	if (image) {
		icon_draw_at(canvas, x + (width - size) / 2, y + (height - size) / 2, image,
			app->image_width, app->image_height, size, sb->icon_style == SB_ICONS_TINTED,
			sb->accent);
		return;
	}
	char initial[8] = "";
	size_t length = 1;
	while (length < sizeof(initial) - 1 && ((unsigned char)app->name[length] & 0xc0) == 0x80) {
		length++;
	}
	memcpy(initial, app->name, length);
	double text = size * 0.8;
	double text_width = canvas_text_width(true, text, initial);
	canvas_text(canvas, true, text, x + (width - text_width) / 2, y + height / 2.0 + text / 3,
		0xffffffff, initial);
}

static void draw_card(struct starboard *sb, struct sb_app *app) {
	struct canvas *canvas = canvas_create(sb->shm, LIFTED_W, LIFTED_H);
	draw_face(canvas, 0, 0, LIFTED_W, LIFTED_H, CARD_RADIUS * LIFT, app->color);
	draw_mark(sb, app, canvas, 0, 0, LIFTED_W, LIFTED_H, lround(LIFTED_H * 0.6));
	app->icon_canvas = canvas;
}

/* A soft shadow around a lifted card, as the material shader draws it. */
static struct canvas *draw_shadow(struct starboard *sb) {
	const int width = LIFTED_W + 2 * BLUR, height = LIFTED_H + 2 * BLUR;
	struct canvas *canvas = canvas_create(sb->shm, width, height);
	for (int py = 0; py < height; py++) {
		for (int px = 0; px < width; px++) {
			double d = box_distance(px + 0.5 - BLUR, py + 0.5 - BLUR, LIFTED_W, LIFTED_H,
				CARD_RADIUS * LIFT);
			double t = fmin(1, fmax(0, (d + 0.3 * BLUR) / (1.3 * BLUR)));
			double alpha = 0.62 * (1 - t * t * (3 - 2 * t));
			canvas_blend(canvas, px, py, 0xff000000, alpha);
		}
	}
	return canvas;
}

/* The sheen of the focused card: one faint spot up and to the left. */
static struct canvas *draw_sheen(struct starboard *sb) {
	struct canvas *canvas = canvas_create(sb->shm, LIFTED_W, LIFTED_H);
	const double cx = 0.32 * LIFTED_W, cy = 0.18 * LIFTED_H, r = 0.75 * LIFTED_W;
	for (int py = 0; py < LIFTED_H; py++) {
		for (int px = 0; px < LIFTED_W; px++) {
			double coverage = 0.5 - box_distance(px + 0.5, py + 0.5, LIFTED_W, LIFTED_H,
				CARD_RADIUS * LIFT);
			double dx = (px + 0.5 - cx) / r, dy = (py + 0.5 - cy) / r;
			double h = fmax(0, 1 - dx * dx - dy * dy);
			canvas_blend(canvas, px, py, 0xffffffff, fmin(1, coverage) * 0.16 * h * h);
		}
	}
	return canvas;
}

static void draw_caption(struct starboard *sb) {
	struct canvas *canvas = NULL;
	if (sb->tv_focus >= 0) {
		const char *name = sb->apps[sb->tv_focus].name;
		double size = CAPTION_SIZE;
		while (size > 18 && canvas_text_width(true, size, name) > CAPTION_W) {
			size -= 2;
		}
		canvas = canvas_create(sb->shm, CAPTION_W, CAPTION_H);
		canvas_text(canvas, true, size, (CAPTION_W - canvas_text_width(true, size, name)) / 2,
			CAPTION_H * 0.7, TEXT, name);
	}
	sb_sprite_show(&sb->tv_caption, canvas);
	if (sb->tv_caption_canvas) {
		canvas_release(sb->tv_caption_canvas);
	}
	sb->tv_caption_canvas = canvas;
}

/* The top shelf: the focused app's name, whether it runs, and a poster. */
static struct canvas *draw_shelf(struct starboard *sb, struct sb_app *app) {
	const int width = sb->width - 2 * MARGIN_X;
	struct canvas *canvas = canvas_create(sb->shm, width, SHELF_H);
	const int poster_x = width - POSTER_W;
	draw_face(canvas, poster_x, 0, POSTER_W, POSTER_H, 28, app->color);
	draw_mark(sb, app, canvas, poster_x, 0, POSTER_W, POSTER_H, 176);
	double room = poster_x - 64, size = 72;
	while (size > 40 && canvas_text_width(true, size, app->name) > room) {
		size -= 4;
	}
	canvas_text(canvas, true, size, 0, 150, TEXT, app->name);
	if (sb_scene_for_app(sb, app)) {
		canvas_text(canvas, false, 30, 0, 210, TEXT_SECONDARY, "Running");
	}
	return canvas;
}

static void shelf_redraw(struct starboard *sb) {
	struct canvas *canvas = sb->tv_focus >= 0 ?
		draw_shelf(sb, &sb->apps[sb->tv_focus]) : NULL;
	sb_sprite_show(&sb->tv_shelf, canvas);
	if (sb->tv_shelf_canvas) {
		canvas_release(sb->tv_shelf_canvas);
	}
	sb->tv_shelf_canvas = canvas;
	wl_surface_commit(sb->home);
}

/* ---- motion: springs, advanced on frame callbacks of the home surface ---- */

/* One damped spring, stepped semi-implicitly in 4 ms slices; false once at rest. */
static bool spring(double *x, double *v, double target, double stiffness, double damping,
		double dt) {
	for (double t = 0; t < dt; t += 0.004) {
		double h = fmin(0.004, dt - t);
		*v += (-stiffness * (*x - target) - damping * *v) * h;
		*x += *v * h;
	}
	if (fabs(*x - target) < 0.0005 && fabs(*v) < 0.005) {
		*x = target;
		*v = 0;
		return false;
	}
	return true;
}

/* Where a card is drawn now: its centre and lifted scale. */
static void card_now(struct starboard *sb, int index, double *cx, double *cy, double *scale) {
	struct sb_app *app = &sb->apps[index];
	double lean = index == sb->tv_focus ? LEAN * fmin(1, fmax(0, app->focus)) : 0;
	*scale = 1 + (LIFT - 1) * app->focus;
	*cx = app->x - sb->tv_scroll[row_of(sb, index)] + CARD_W / 2.0 + sb->tv_lean_x * lean;
	*cy = app->y + CARD_H / 2.0 + sb->tv_lean_y * lean;
}

static void place_card(struct starboard *sb, int index) {
	struct sb_app *app = &sb->apps[index];
	double cx, cy, scale;
	card_now(sb, index, &cx, &cy, &scale);
	int width = lround(CARD_W * scale), height = lround(CARD_H * scale);
	sb_sprite_move(&app->icon, cx - width / 2.0, cy - height / 2.0);
	wp_viewport_set_destination(app->viewport, width, height);
	wl_surface_commit(app->icon.surface);
}

/* The shadow lags the lean a little, which reads as depth. */
static void place_focus_art(struct starboard *sb) {
	if (sb->tv_focus < 0) {
		return;
	}
	struct sb_app *app = &sb->apps[sb->tv_focus];
	double cx, cy, scale;
	card_now(sb, sb->tv_focus, &cx, &cy, &scale);
	int width = lround(CARD_W * scale), height = lround(CARD_H * scale);
	sb_sprite_move(&sb->tv_sheen, cx - width / 2.0, cy - height / 2.0);
	wp_viewport_set_destination(sb->tv_sheen_viewport, width, height);
	wl_surface_commit(sb->tv_sheen.surface);

	double k = scale / LIFT, lean = LEAN * fmin(1, fmax(0, app->focus)) * 0.6;
	int shadow_width = lround((LIFTED_W + 2 * BLUR) * k);
	int shadow_height = lround((LIFTED_H + 2 * BLUR) * k);
	sb_sprite_move(&sb->tv_shadow, cx - sb->tv_lean_x * lean - shadow_width / 2.0,
		cy - sb->tv_lean_y * lean + 0.35 * BLUR * k - shadow_height / 2.0);
	wp_viewport_set_destination(sb->tv_shadow_viewport, shadow_width, shadow_height);
	wl_surface_commit(sb->tv_shadow.surface);
	sb_sprite_move(&sb->tv_caption, cx - CAPTION_W / 2.0, cy + height / 2.0 + 10);
}

static bool step(struct starboard *sb) {
	uint32_t msec = sb_now_msec();
	double dt = sb->tv_frame_msec ? fmin(50, msec - sb->tv_frame_msec) / 1000.0 : 1 / 60.0;
	sb->tv_frame_msec = msec;
	bool moving = false;
	for (int row = 0; row < 2; row++) {
		moving |= spring(&sb->tv_scroll[row], &sb->tv_scroll_velocity[row],
			sb->tv_scroll_target[row], 170, 26, dt);
	}
	moving |= spring(&sb->tv_lean_x, &sb->tv_lean_vx, 0, 220, 13, dt);
	moving |= spring(&sb->tv_lean_y, &sb->tv_lean_vy, 0, 220, 13, dt);
	double follow = 1 - exp(-dt * 1000 / 70.0);
	for (int i = 0; i < sb->app_count; i++) {
		struct sb_app *app = &sb->apps[i];
		double x, y;
		slot(sb, i, &x, &y);
		if (fabs(x - app->x) < 0.5 && fabs(y - app->y) < 0.5) {
			app->x = x;
			app->y = y;
		} else {
			app->x += (x - app->x) * follow;
			app->y += (y - app->y) * follow;
			moving = true;
		}
		/* Slightly underdamped: the lift overshoots by about 9% of itself. */
		moving |= spring(&app->focus, &app->focus_velocity, i == sb->tv_focus ? 1 : 0,
			260, 19.4, dt);
		place_card(sb, i);
	}
	place_focus_art(sb);
	if (!moving) {
		sb->tv_frame_msec = 0;
	}
	return moving;
}

static void schedule(struct starboard *sb);

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct starboard *sb = data;
	wl_callback_destroy(callback);
	sb->tv_frame = NULL;
	if (step(sb)) {
		schedule(sb);
	} else {
		wl_surface_commit(sb->home);
	}
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void schedule(struct starboard *sb) {
	if (!sb->tv_frame) {
		sb->tv_frame = wl_surface_frame(sb->home);
		wl_callback_add_listener(sb->tv_frame, &frame_listener, sb);
	}
	wl_surface_commit(sb->home);
}

/* ---- focus ---- */

/* Cards in order, then the one that just lost the focus (still settling),
 * then the shadow, the focused card and its sheen on top. */
static void restack(struct starboard *sb, int previous) {
	struct wl_surface *below = sb->tv_caption.surface;
	for (int i = 0; i < sb->app_count; i++) {
		if (i != sb->tv_focus && i != previous) {
			wl_subsurface_place_above(sb->apps[i].icon.subsurface, below);
			below = sb->apps[i].icon.surface;
		}
	}
	if (previous >= 0 && previous < sb->app_count && previous != sb->tv_focus) {
		wl_subsurface_place_above(sb->apps[previous].icon.subsurface, below);
		below = sb->apps[previous].icon.surface;
	}
	if (sb->tv_focus >= 0) {
		struct sb_app *app = &sb->apps[sb->tv_focus];
		wl_subsurface_place_above(sb->tv_shadow.subsurface, below);
		wl_subsurface_place_above(app->icon.subsurface, sb->tv_shadow.surface);
		wl_subsurface_place_above(sb->tv_sheen.subsurface, app->icon.surface);
	}
}

/* (dx, dy) is the direction of travel; the card leans that way and settles. */
static void set_focus(struct starboard *sb, int index, double dx, double dy) {
	int previous = sb->tv_focus;
	if (index == previous && index >= 0) {
		return;
	}
	sb->tv_focus = index;
	bool shown = index >= 0;
	sb_sprite_show(&sb->tv_shadow, shown ? sb->tv_shadow_canvas : NULL);
	sb_sprite_show(&sb->tv_sheen, shown ? sb->tv_sheen_canvas : NULL);
	if (shown) {
		sb->tv_lean_x = dx;
		sb->tv_lean_y = dy;
		sb->tv_lean_vx = sb->tv_lean_vy = 0;
		reveal(sb, index);
	}
	restack(sb, previous);
	draw_caption(sb);
	sb_timer_arm(&sb->tv_shelf_timer, SHELF_DELAY);
	schedule(sb);
}

static void focus_first(struct starboard *sb) {
	int first = app_at(sb, 0, 0);
	if (first >= 0) {
		set_focus(sb, first, 0, 0);
		sb->tv_scroll_target[0] = 0;
		schedule(sb);
	}
}

/* A step along a row, or to the card nearest on screen in the other row.
 * At an edge the card only nudges, so the press is not lost silently. */
static void move(struct starboard *sb, int dx, int dy) {
	if (sb->tv_focus < 0) {
		focus_first(sb);
		return;
	}
	int row = row_of(sb, sb->tv_focus), target = -1;
	if (dx) {
		target = app_at(sb, row, column_of(sb, sb->tv_focus) + dx);
	} else if (row + dy == 0 || row + dy == 1) {
		double x, y, best = 0;
		slot(sb, sb->tv_focus, &x, &y);
		double from = x - sb->tv_scroll_target[row];
		for (int i = 0; i < sb->app_count; i++) {
			if (row_of(sb, i) != row + dy) {
				continue;
			}
			slot(sb, i, &x, &y);
			double distance = fabs(x - sb->tv_scroll_target[row + dy] - from);
			if (target < 0 || distance < best) {
				target = i;
				best = distance;
			}
		}
	}
	if (target >= 0) {
		set_focus(sb, target, dx, dy);
	} else {
		sb->tv_lean_x = 0.5 * dx;
		sb->tv_lean_y = 0.5 * dy;
		schedule(sb);
	}
}

static void repeat(struct starboard *sb) {
	int dx = sb->tv_repeat_key == KEY_LEFT ? -1 : sb->tv_repeat_key == KEY_RIGHT ? 1 : 0;
	int dy = sb->tv_repeat_key == KEY_UP ? -1 : sb->tv_repeat_key == KEY_DOWN ? 1 : 0;
	if (!dx && !dy) {
		return;
	}
	move(sb, dx, dy);
	int rate = sb->tv_repeat_rate < REPEAT_MAX ? sb->tv_repeat_rate : REPEAT_MAX;
	sb_timer_arm(&sb->tv_repeat, rate > 0 ? 1000 / rate : 0);
}

static void stop_repeat(struct starboard *sb) {
	sb->tv_repeat_key = 0;
	sb_timer_arm(&sb->tv_repeat, 0);
}

void tv_key(struct starboard *sb, uint32_t key, bool pressed) {
	int dx = key == KEY_LEFT ? -1 : key == KEY_RIGHT ? 1 : 0;
	int dy = key == KEY_UP ? -1 : key == KEY_DOWN ? 1 : 0;
	if (dx || dy) {
		if (pressed) {
			move(sb, dx, dy);
			sb->tv_repeat_key = key;
			int delay = sb->tv_repeat_delay > 0 ? sb->tv_repeat_delay : 1;
			sb_timer_arm(&sb->tv_repeat, sb->tv_repeat_rate > 0 ? delay : 0);
		} else if (key == sb->tv_repeat_key) {
			stop_repeat(sb);
		}
		return;
	}
	if (!pressed) {
		return;
	}
	stop_repeat(sb);
	switch (key) {
	case KEY_OK:
	case KEY_SELECT:
	case KEY_ENTER:
	case KEY_KPENTER:
		if (sb->tv_focus >= 0) {
			sb_launch(sb, sb->tv_focus);
		}
		break;
	case KEY_BACK:
	case KEY_ESC:
		focus_first(sb);
		break;
	}
}

void tv_keyboard_left(struct starboard *sb) {
	stop_repeat(sb);
}

/* Home from an app comes back with that app in focus; on the home screen
 * it goes back to the start, as Back does. */
void tv_home_key(struct starboard *sb) {
	struct sb_scene *scene = sb_foreground_scene(sb);
	if (!scene) {
		focus_first(sb);
		return;
	}
	for (int i = 0; i < sb->app_count; i++) {
		if (sb->apps[i].pid && sb->apps[i].pid == scene->pid) {
			set_focus(sb, i, 0, 0);
		}
	}
	starry_system_v1_go_home(sb->system);
}

void tv_scenes_changed(struct starboard *sb) {
	if (sb->tv_focus >= 0) {
		sb_timer_arm(&sb->tv_shelf_timer, SHELF_DELAY); /* "Running" may have changed */
	}
}

/* ---- apps coming and going ---- */

static void card_init(struct starboard *sb, int index) {
	struct sb_app *app = &sb->apps[index];
	draw_card(sb, app);
	sb_sprite_init(sb, sb->home, &app->icon, app->icon_canvas);
	app->viewport = wp_viewporter_get_viewport(sb->viewporter, app->icon.surface);
	app->focus = app->focus_velocity = 0;
	slot(sb, index, &app->x, &app->y);
	place_card(sb, index);
}

void tv_app_added(struct starboard *sb, int index) {
	card_init(sb, index);
	if (sb->tv_focus < 0) {
		focus_first(sb);
	} else {
		restack(sb, -1); /* the new card came in on top */
		reveal(sb, sb->tv_focus); /* rows may have changed */
	}
	schedule(sb);
}

void tv_app_changed(struct starboard *sb, int index) {
	struct sb_app *app = &sb->apps[index];
	draw_card(sb, app);
	sb_sprite_show(&app->icon, app->icon_canvas);
	if (index == sb->tv_focus) {
		draw_caption(sb);
		sb_timer_arm(&sb->tv_shelf_timer, SHELF_DELAY);
	}
	schedule(sb);
}

void tv_app_removing(struct starboard *sb, int index) {
	struct sb_app *app = &sb->apps[index];
	if (app->viewport) {
		wp_viewport_destroy(app->viewport);
		app->viewport = NULL;
	}
}

/* The focus stays in place, on the card that slid into the gap. */
void tv_app_removed(struct starboard *sb, int index) {
	int focus = sb->tv_focus;
	if (focus > index || focus >= sb->app_count) {
		focus--;
	}
	sb->tv_focus = -1; /* its index may be stale: set everything afresh */
	set_focus(sb, sb->app_count > 0 ? focus : -1, 0, 0);
	for (int row = 0; row < 2; row++) {
		sb->tv_scroll_target[row] = fmin(sb->tv_scroll_target[row], max_scroll(sb, row));
	}
	schedule(sb);
}

/* The icon style or accent changed. */
void tv_redraw_cards(struct starboard *sb) {
	for (int i = 0; i < sb->app_count; i++) {
		struct sb_app *app = &sb->apps[i];
		if (app->icons[0][0]) {
			canvas_release(app->icon_canvas);
			draw_card(sb, app);
			sb_sprite_show(&app->icon, app->icon_canvas);
		}
	}
	sb_timer_arm(&sb->tv_shelf_timer, SHELF_DELAY);
	wl_surface_commit(sb->home);
}

/* ---- setup ---- */

void tv_create(struct starboard *sb) {
	sb->dragging = -1;
	sb->haptic_fd = -1;
	sb->tv_focus = -1;
	sb->tv_repeat.callback = repeat;
	sb->tv_shelf_timer.callback = shelf_redraw;
	sb->tv_repeat_rate = 25;
	sb->tv_repeat_delay = 400;

	/* A one-pixel backdrop, stretched by the compositor to the screen. */
	sb->home = sb_surface_create(sb, false);
	sb->home_layer = starry_system_v1_get_layer_surface(sb->system, sb->home,
		STARRY_SYSTEM_V1_LAYER_HOME);
	sb->tv_backdrop = canvas_create(sb->shm, 1, 1);
	sb->tv_backdrop->pixels[0] = BACKDROP;
	sb->backdrop_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->home);
	wp_viewport_set_destination(sb->backdrop_viewport, sb->width, sb->height);

	sb_sprite_init(sb, sb->home, &sb->tv_shelf, NULL);
	sb_sprite_move(&sb->tv_shelf, MARGIN_X, SHELF_Y);
	sb_sprite_init(sb, sb->home, &sb->tv_caption, NULL);
	for (int i = 0; i < sb->app_count; i++) {
		card_init(sb, i);
	}
	sb->tv_shadow_canvas = draw_shadow(sb);
	sb_sprite_init(sb, sb->home, &sb->tv_shadow, NULL);
	sb->tv_shadow_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->tv_shadow.surface);
	sb->tv_sheen_canvas = draw_sheen(sb);
	sb_sprite_init(sb, sb->home, &sb->tv_sheen, NULL);
	sb->tv_sheen_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->tv_sheen.surface);

	wl_surface_attach(sb->home, sb->tv_backdrop->buffer, 0, 0);
	sb->tv_backdrop->busy = true;
	wl_surface_damage_buffer(sb->home, 0, 0, 1, 1);
	focus_first(sb);
	schedule(sb);
}
