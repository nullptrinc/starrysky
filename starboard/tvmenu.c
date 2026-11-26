/* SPDX-License-Identifier: MIT */
/*
 * The TV app menu: Home pressed twice brings up a row of the running apps
 * over whatever is on screen, the app included, which stays below a dim.
 * The apps are the compositor's scenes, most recently used first, as in the
 * phone's switcher. Left and right choose, OK switches to the app, Back or
 * Home goes back to where you were, and holding OK (or Stop, or 0) closes
 * the chosen app, whose card flies up and away.
 *
 * The menu lives on the switcher layer, which takes the keyboard while it
 * is mapped. Cards look like the home screen's: the app's card art, lifted
 * when chosen, with its name and state below.
 */
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "starry-system-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "starboard.h"
#include "tv.h"

#define ROW_Y 410 /* top of the cards at rest */
#define LABEL_W 320
#define LABEL_H 80
#define MESSAGE_W 800
#define MESSAGE_H 80
#define SCRIM 0xc8000000 /* over the app or the home screen */
#define HOLD_MSEC 600 /* OK held this long closes the app */
#define MESSAGE_MSEC 1200 /* "No open apps", then the menu goes */
#define DEFAULT_COLOR 0xff5a6a80

static void schedule(struct starboard *sb);

static struct sb_app *app_of(struct starboard *sb, const struct sb_scene *scene) {
	for (int i = 0; scene && i < sb->app_count; i++) {
		if (sb->apps[i].pid && sb->apps[i].pid == scene->pid) {
			return &sb->apps[i];
		}
	}
	return NULL;
}

/* ---- art ---- */

static void draw_card(struct starboard *sb, struct sb_menu_card *card) {
	struct sb_app *app = app_of(sb, card->scene);
	struct sb_app stand_in;
	if (!app) {
		/* A window no catalogue entry or package claims: its app id, in grey. */
		memset(&stand_in, 0, sizeof(stand_in));
		snprintf(stand_in.name, sizeof(stand_in.name), "%s",
			card->scene->app_id[0] ? card->scene->app_id : "?");
		stand_in.color = DEFAULT_COLOR;
		app = &stand_in;
	}
	card->art_canvas = tv_card_art(sb, app);
	/* Name and state below the card. */
	const char *state = card->scene->foreground ? "On screen" : "Running";
	card->label_canvas = canvas_create(sb->shm, LABEL_W, LABEL_H);
	double size = 28;
	while (size > 18 && canvas_text_width(true, size, app->name) > LABEL_W) {
		size -= 2;
	}
	canvas_text(card->label_canvas, true, size,
		(LABEL_W - canvas_text_width(true, size, app->name)) / 2, 34, TEXT, app->name);
	canvas_text(card->label_canvas, false, 22,
		(LABEL_W - canvas_text_width(false, 22, state)) / 2, 66, TEXT_SECONDARY, state);
}

static void show_message(struct starboard *sb, const char *text) {
	struct canvas *canvas = canvas_create(sb->shm, MESSAGE_W, MESSAGE_H);
	canvas_text(canvas, false, 36, (MESSAGE_W - canvas_text_width(false, 36, text)) / 2, 52,
		TEXT_SECONDARY, text);
	sb_sprite_show(&sb->menu_message, canvas);
	if (sb->menu_message_canvas) {
		canvas_release(sb->menu_message_canvas);
	}
	sb->menu_message_canvas = canvas;
	sb_timer_arm(&sb->menu_close_timer, MESSAGE_MSEC);
	wl_surface_commit(sb->menu);
}

/* ---- the row ---- */

static int live_count(struct starboard *sb) {
	int count = 0;
	for (int i = 0; i < sb->menu_count; i++) {
		count += !sb->menu_cards[i].closing;
	}
	return count;
}

/* Left edge of card i at rest, before scrolling: closing cards leave their
 * place at once, so the others close the gap while it flies away. */
static double slot_x(struct starboard *sb, int index) {
	int count = live_count(sb), position = 0;
	for (int i = 0; i < index; i++) {
		position += !sb->menu_cards[i].closing;
	}
	double width = count * CARD_W + (count > 0 ? count - 1 : 0) * CARD_GAP;
	double start = width <= sb->width - 2 * MARGIN_X ? (sb->width - width) / 2 : MARGIN_X;
	return start + position * (CARD_W + CARD_GAP);
}

static double max_scroll(struct starboard *sb) {
	int count = live_count(sb);
	double width = count * CARD_W + (count > 0 ? count - 1 : 0) * CARD_GAP;
	return fmax(0, width + 2 * MARGIN_X - sb->width);
}

static void reveal(struct starboard *sb) {
	if (sb->menu_selected < 0) {
		return;
	}
	double x = slot_x(sb, sb->menu_selected), target = sb->menu_scroll_target;
	if (x + CARD_W - target > sb->width - MARGIN_X) {
		target = x + CARD_W - (sb->width - MARGIN_X);
	}
	if (x - target < MARGIN_X) {
		target = x - MARGIN_X;
	}
	sb->menu_scroll_target = fmin(fmax(0, target), max_scroll(sb));
}

/* Cards in order, then the shadow, the chosen card and its sheen on top. */
static void restack(struct starboard *sb) {
	struct wl_surface *below = sb->menu_message.surface;
	for (int i = 0; i < sb->menu_count; i++) {
		struct sb_menu_card *card = &sb->menu_cards[i];
		wl_subsurface_place_above(card->label.subsurface, below);
		below = card->label.surface;
		if (i != sb->menu_selected) {
			wl_subsurface_place_above(card->art.subsurface, below);
			below = card->art.surface;
		}
	}
	if (sb->menu_selected >= 0) {
		struct sb_menu_card *card = &sb->menu_cards[sb->menu_selected];
		wl_subsurface_place_above(sb->menu_shadow.subsurface, below);
		wl_subsurface_place_above(card->art.subsurface, sb->menu_shadow.surface);
		wl_subsurface_place_above(sb->menu_sheen.subsurface, card->art.surface);
	}
}

static void select_card(struct starboard *sb, int index) {
	sb->menu_selected = index;
	bool shown = index >= 0;
	sb_sprite_show(&sb->menu_shadow, shown ? sb->tv_shadow_canvas : NULL);
	sb_sprite_show(&sb->menu_sheen, shown ? sb->tv_sheen_canvas : NULL);
	reveal(sb);
	restack(sb);
	schedule(sb);
}

/* The nearest card still open from index in a direction, or -1. */
static int neighbour(struct starboard *sb, int index, int direction) {
	for (int i = index + direction; i >= 0 && i < sb->menu_count; i += direction) {
		if (!sb->menu_cards[i].closing) {
			return i;
		}
	}
	return -1;
}

static void card_destroy(struct sb_menu_card *card) {
	wp_viewport_destroy(card->viewport);
	sb_sprite_destroy(&card->art);
	sb_sprite_destroy(&card->label);
	canvas_release(card->art_canvas);
	canvas_release(card->label_canvas);
}

static void remove_card(struct starboard *sb, int index) {
	card_destroy(&sb->menu_cards[index]);
	memmove(&sb->menu_cards[index], &sb->menu_cards[index + 1],
		(size_t)(sb->menu_count - index - 1) * sizeof(sb->menu_cards[0]));
	sb->menu_count--;
	if (sb->menu_selected > index) {
		sb->menu_selected--;
	}
	if (sb->menu_count == 0) {
		show_message(sb, "No open apps");
	}
}

/* ---- motion ---- */

static bool step(struct starboard *sb) {
	uint32_t msec = sb_now_msec();
	double dt = sb->menu_frame_msec ? fmin(50, msec - sb->menu_frame_msec) / 1000.0 : 1 / 60.0;
	sb->menu_frame_msec = msec;
	bool moving = tv_spring(&sb->menu_scroll, &sb->menu_scroll_velocity,
		sb->menu_scroll_target, 170, 26, dt);
	double follow = 1 - exp(-dt * 1000 / 70.0);
	for (int i = sb->menu_count - 1; i >= 0; i--) {
		struct sb_menu_card *card = &sb->menu_cards[i];
		if (card->closing) {
			/* Up and out over the top edge, then gone. */
			double away = -(ROW_Y + LIFTED_H + 2 * BLUR);
			bool flying = tv_spring(&card->lift, &card->lift_velocity, away, 120, 16, dt);
			if (!flying || card->lift < away + 2) {
				remove_card(sb, i);
				moving = true;
				continue;
			}
			moving = true;
		} else {
			double x = slot_x(sb, i);
			if (fabs(x - card->x) < 0.5) {
				card->x = x;
			} else {
				card->x += (x - card->x) * follow;
				moving = true;
			}
		}
		moving |= tv_spring(&card->focus, &card->focus_velocity, i == sb->menu_selected ? 1 : 0,
			LIFT_STIFFNESS, LIFT_DAMPING, dt);
		double scale = 1 + (LIFT - 1) * card->focus;
		double cx = card->x - sb->menu_scroll + CARD_W / 2.0;
		double cy = ROW_Y + CARD_H / 2.0 + card->lift;
		int width = lround(CARD_W * scale), height = lround(CARD_H * scale);
		sb_sprite_move(&card->art, cx - width / 2.0, cy - height / 2.0);
		wp_viewport_set_destination(card->viewport, width, height);
		wl_surface_commit(card->art.surface);
		sb_sprite_move(&card->label, cx - LABEL_W / 2.0, cy + CARD_H * LIFT / 2 + 14);
		if (i == sb->menu_selected) {
			sb_sprite_move(&sb->menu_sheen, cx - width / 2.0, cy - height / 2.0);
			wp_viewport_set_destination(sb->menu_sheen_viewport, width, height);
			wl_surface_commit(sb->menu_sheen.surface);
			double k = scale / LIFT;
			int shadow_width = lround((LIFTED_W + 2 * BLUR) * k);
			int shadow_height = lround((LIFTED_H + 2 * BLUR) * k);
			sb_sprite_move(&sb->menu_shadow, cx - shadow_width / 2.0,
				cy + 0.35 * BLUR * k - shadow_height / 2.0);
			wp_viewport_set_destination(sb->menu_shadow_viewport, shadow_width, shadow_height);
			wl_surface_commit(sb->menu_shadow.surface);
		}
	}
	if (!moving) {
		sb->menu_frame_msec = 0;
	}
	return moving;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct starboard *sb = data;
	wl_callback_destroy(callback);
	sb->menu_frame = NULL;
	if (!sb->menu_shown) {
		return;
	}
	if (step(sb)) {
		schedule(sb);
	} else {
		wl_surface_commit(sb->menu);
	}
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void schedule(struct starboard *sb) {
	if (!sb->menu_frame) {
		sb->menu_frame = wl_surface_frame(sb->menu);
		wl_callback_add_listener(sb->menu_frame, &frame_listener, sb);
	}
	wl_surface_commit(sb->menu);
}

/* ---- showing and hiding ---- */

static int by_recent_use(const void *a, const void *b) {
	const struct sb_scene *x = *(struct sb_scene *const *)a, *y = *(struct sb_scene *const *)b;
	return x->last_foreground < y->last_foreground ? 1 :
		x->last_foreground > y->last_foreground ? -1 : 0;
}

void tv_menu_show(struct starboard *sb) {
	if (sb->menu_shown) {
		return;
	}
	struct sb_scene *scenes[SB_MAX_CARDS], *scene;
	int count = 0;
	wl_list_for_each(scene, &sb->scenes, link) {
		if (count < SB_MAX_CARDS) {
			scenes[count++] = scene;
		}
	}
	qsort(scenes, count, sizeof(scenes[0]), by_recent_use);

	sb->menu_shown = true;
	sb->menu_ok_down = false;
	sb->menu_count = 0;
	sb->menu_selected = -1;
	sb->menu_scroll = sb->menu_scroll_target = sb->menu_scroll_velocity = 0;
	for (int i = 0; i < count; i++) {
		struct sb_menu_card *card = &sb->menu_cards[sb->menu_count++];
		memset(card, 0, sizeof(*card));
		card->scene = scenes[i];
		/* Live previews would be starry_scene_v1_get_view snapshots, as the
		 * phone's switcher shows; each is a full-window render, too dear on
		 * a Mali-450 for now, so card->view stays NULL and the art stands in. */
		card->view = NULL;
		draw_card(sb, card);
		sb_sprite_init(sb, sb->menu, &card->label, card->label_canvas);
		sb_sprite_init(sb, sb->menu, &card->art, card->art_canvas);
		card->viewport = wp_viewporter_get_viewport(sb->viewporter, card->art.surface);
	}
	for (int i = 0; i < sb->menu_count; i++) {
		sb->menu_cards[i].x = slot_x(sb, i);
	}
	sb->menu_scrim->busy = true;
	wl_surface_attach(sb->menu, sb->menu_scrim->buffer, 0, 0);
	wl_surface_damage_buffer(sb->menu, 0, 0, 1, 1);
	if (count == 0) {
		show_message(sb, "No open apps");
		return;
	}
	/* The most recent app, the one on screen if any, starts chosen. */
	select_card(sb, 0);
	step(sb);
	schedule(sb);
}

void tv_menu_hide(struct starboard *sb) {
	if (!sb->menu_shown) {
		return;
	}
	sb->menu_shown = false;
	sb_timer_arm(&sb->menu_hold, 0);
	sb_timer_arm(&sb->menu_close_timer, 0);
	for (int i = 0; i < sb->menu_count; i++) {
		card_destroy(&sb->menu_cards[i]);
	}
	sb->menu_count = 0;
	sb->menu_selected = -1;
	sb_sprite_show(&sb->menu_shadow, NULL);
	sb_sprite_show(&sb->menu_sheen, NULL);
	sb_sprite_show(&sb->menu_message, NULL);
	if (sb->menu_frame) {
		/* An unmapped surface gets no more frames: drop the request. */
		wl_callback_destroy(sb->menu_frame);
		sb->menu_frame = NULL;
	}
	/* Unmapping gives the keyboard back to the app or the home screen. */
	wl_surface_attach(sb->menu, NULL, 0, 0);
	wl_surface_commit(sb->menu);
}

static void close_after_message(struct starboard *sb) {
	tv_menu_hide(sb);
}

/* Switches to the chosen app; the one already on screen just stays. */
static void choose(struct starboard *sb) {
	if (sb->menu_selected < 0) {
		return;
	}
	struct sb_scene *scene = sb->menu_cards[sb->menu_selected].scene;
	bool from_home = !sb_foreground_scene(sb);
	tv_menu_hide(sb);
	if (scene && !scene->foreground) {
		starry_scene_v1_activate(scene->scene, from_home ?
			STARRY_SCENE_V1_ANIMATION_SLIDE : STARRY_SCENE_V1_ANIMATION_NONE);
	}
}

/* Asks the chosen app to close; its card flies away at once. */
static void close_chosen(struct starboard *sb) {
	int index = sb->menu_selected;
	if (index < 0) {
		return;
	}
	struct sb_menu_card *card = &sb->menu_cards[index];
	if (card->scene) {
		starry_scene_v1_close(card->scene->scene);
	}
	card->closing = true;
	int next = neighbour(sb, index, 1);
	select_card(sb, next >= 0 ? next : neighbour(sb, index, -1));
}

static void hold(struct starboard *sb) {
	if (sb->menu_shown && sb->menu_ok_down) {
		sb->menu_ok_down = false; /* the release must not also switch */
		close_chosen(sb);
	}
}

void tv_menu_key(struct starboard *sb, uint32_t key, bool pressed) {
	if (key == KEY_OK || key == KEY_SELECT || key == KEY_ENTER || key == KEY_KPENTER) {
		if (pressed) {
			sb->menu_ok_down = true;
			sb_timer_arm(&sb->menu_hold, HOLD_MSEC);
		} else if (sb->menu_ok_down) {
			sb->menu_ok_down = false;
			sb_timer_arm(&sb->menu_hold, 0);
			choose(sb);
		}
		return;
	}
	if (!pressed) {
		return;
	}
	switch (key) {
	case KEY_LEFT:
	case KEY_RIGHT: {
		int next = neighbour(sb, sb->menu_selected, key == KEY_LEFT ? -1 : 1);
		if (next >= 0) {
			select_card(sb, next);
		}
		break;
	}
	case KEY_STOP:
	case KEY_STOPCD:
	case KEY_0:
	case KEY_NUMERIC_0:
		close_chosen(sb);
		break;
	case KEY_BACK:
	case KEY_ESC:
		tv_menu_hide(sb);
		break;
	}
}

/* The window went away by itself (or as asked): its card goes too. */
void tv_menu_scene_closed(struct starboard *sb, struct sb_scene *scene) {
	for (int i = 0; sb->menu_shown && i < sb->menu_count; i++) {
		struct sb_menu_card *card = &sb->menu_cards[i];
		if (card->scene != scene) {
			continue;
		}
		card->scene = NULL;
		if (!card->closing) {
			card->closing = true;
			if (i == sb->menu_selected) {
				int next = neighbour(sb, i, 1);
				select_card(sb, next >= 0 ? next : neighbour(sb, i, -1));
			}
			schedule(sb);
		}
	}
}

void tv_menu_create(struct starboard *sb) {
	sb->menu_hold.callback = hold;
	sb->menu_close_timer.callback = close_after_message;
	sb->menu_selected = -1;
	sb->menu = sb_surface_create(sb, false);
	sb->menu_layer = starry_system_v1_get_layer_surface(sb->system, sb->menu,
		STARRY_SYSTEM_V1_LAYER_SWITCHER);
	/* One translucent pixel, stretched over the screen. */
	sb->menu_scrim = canvas_create(sb->shm, 1, 1);
	sb->menu_scrim->pixels[0] = SCRIM;
	sb->menu_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->menu);
	wp_viewport_set_destination(sb->menu_viewport, sb->width, sb->height);
	sb_sprite_init(sb, sb->menu, &sb->menu_message, NULL);
	sb_sprite_move(&sb->menu_message, (sb->width - MESSAGE_W) / 2.0,
		(sb->height - MESSAGE_H) / 2.0);
	sb_sprite_init(sb, sb->menu, &sb->menu_shadow, NULL);
	sb->menu_shadow_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->menu_shadow.surface);
	sb_sprite_init(sb, sb->menu, &sb->menu_sheen, NULL);
	sb->menu_sheen_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->menu_sheen.surface);
	wl_surface_commit(sb->menu); /* no buffer: unmapped until shown */
}
