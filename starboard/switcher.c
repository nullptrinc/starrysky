/* SPDX-License-Identifier: MIT */
/*
 * App switcher: a horizontal row of window snapshots. The compositor hosts the
 * snapshots (scene views); this file owns their layout, motion and gestures.
 * Card 0 is the most recently used app, older apps lie to the left.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "starry-system-v1-client-protocol.h"
#include "starboard.h"

#define CARD_SCALE 0.64
#define CARD_GAP 18
#define LABEL_WIDTH 200
#define LABEL_HEIGHT 22
#define TAP_SLOP 12
#define AXIS_SLOP 10
#define DISMISS_DISTANCE 110
#define DISMISS_VELOCITY 0.8

static double card_width(struct starboard *sb) {
	return sb->width * CARD_SCALE;
}

static double card_height(struct starboard *sb) {
	return card_width(sb) * (sb->height - SB_STATUS_HEIGHT) / sb->width;
}

static double card_pitch(struct starboard *sb) {
	return card_width(sb) + CARD_GAP;
}

static double max_scroll(struct starboard *sb) {
	return sb->card_count > 1 ? (sb->card_count - 1) * card_pitch(sb) : 0;
}

/* Where card i should be right now, given scroll, lift and an open request. */
static void card_target(struct starboard *sb, int i, double *x, double *y,
		double *width, double *height) {
	struct sb_card *card = &sb->cards[i];
	if (i == sb->opening) {
		*x = 0;
		*y = SB_STATUS_HEIGHT;
		*width = sb->width;
		*height = sb->height - SB_STATUS_HEIGHT;
		return;
	}
	*width = card_width(sb);
	*height = card_height(sb);
	*x = (sb->width - *width) / 2 - i * card_pitch(sb) + sb->scroll;
	*y = (sb->height - *height) / 2 - 8 + card->lift;
	if (card->closing) {
		*y = -*height - 40;
	}
}

static void card_place(struct starboard *sb, struct sb_card *card) {
	starry_scene_view_v1_set_geometry(card->view, lround(card->x), lround(card->y),
		lround(card->width), lround(card->height));
	sb_sprite_move(&card->label, card->x + (card->width - LABEL_WIDTH) / 2,
		card->y - LABEL_HEIGHT - 6);
}

static void card_destroy(struct sb_card *card) {
	starry_scene_view_v1_destroy(card->view);
	sb_sprite_destroy(&card->label);
	canvas_release(card->label_canvas);
}

static void remove_card(struct starboard *sb, int i) {
	card_destroy(&sb->cards[i]);
	memmove(&sb->cards[i], &sb->cards[i + 1], (sb->card_count - i - 1) * sizeof(sb->cards[0]));
	sb->card_count--;
	if (sb->opening > i) {
		sb->opening--;
	}
	sb->scroll_target = fmin(sb->scroll_target, max_scroll(sb));
}

/* ---- motion ---- */

static void schedule(struct starboard *sb);

static bool step(struct starboard *sb) {
	uint32_t msec = sb_now_msec();
	double elapsed = sb->switcher_frame_msec ? fmin(50, msec - sb->switcher_frame_msec) : 16;
	sb->switcher_frame_msec = msec;
	double follow = 1 - exp(-elapsed / 60.0);
	bool moving = false;

	if (!sb->switcher_touch || sb->touch_axis != SB_AXIS_HORIZONTAL) {
		double delta = sb->scroll_target - sb->scroll;
		sb->scroll = fabs(delta) < 0.5 ? sb->scroll_target : sb->scroll + delta * follow;
		moving |= sb->scroll != sb->scroll_target;
	}
	for (int i = sb->card_count - 1; i >= 0; i--) {
		struct sb_card *card = &sb->cards[i];
		double x, y, width, height;
		card_target(sb, i, &x, &y, &width, &height);
		bool held = sb->switcher_touch && sb->touch_card == i &&
			sb->touch_axis == SB_AXIS_VERTICAL;
		double k = held ? 1 : follow;
		double dx = x - card->x, dy = y - card->y;
		double dw = width - card->width, dh = height - card->height;
		if (fabs(dx) + fabs(dy) + fabs(dw) + fabs(dh) < 1) {
			card->x = x;
			card->y = y;
			card->width = width;
			card->height = height;
			if (card->closing) {
				remove_card(sb, i);
				moving = true; /* the rest slide into the gap */
				continue;
			}
		} else {
			card->x += dx * k;
			card->y += dy * k;
			card->width += dw * k;
			card->height += dh * k;
			moving = true;
		}
		card_place(sb, card);
	}

	if (sb->opening >= 0) {
		struct sb_card *card = &sb->cards[sb->opening];
		if (card->width >= sb->width - 1) {
			struct sb_scene *scene = card->scene;
			if (scene) {
				starry_scene_v1_activate(scene->scene, STARRY_SCENE_V1_ANIMATION_NONE);
			}
			switcher_hide(sb);
			return false;
		}
	}
	if (sb->card_count == 0) {
		switcher_hide(sb);
		return false;
	}
	if (!moving) {
		sb->switcher_frame_msec = 0;
	}
	return moving;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct starboard *sb = data;
	wl_callback_destroy(callback);
	sb->switcher_frame = NULL;
	if (!sb->switcher_shown) {
		return;
	}
	if (step(sb)) {
		schedule(sb);
	} else if (sb->switcher_shown) {
		wl_surface_commit(sb->switcher);
	}
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void schedule(struct starboard *sb) {
	if (!sb->switcher_frame) {
		sb->switcher_frame = wl_surface_frame(sb->switcher);
		wl_callback_add_listener(sb->switcher_frame, &frame_listener, sb);
	}
	wl_surface_commit(sb->switcher);
}

/* ---- showing and hiding ---- */

void switcher_create(struct starboard *sb) {
	sb->switcher = sb_surface_create(sb, true);
	sb->switcher_layer = starry_system_v1_get_layer_surface(sb->system, sb->switcher,
		STARRY_SYSTEM_V1_LAYER_SWITCHER);
	/* One pixel per logical pixel is plenty for a flat dim. */
	sb->backdrop = canvas_create(sb->shm, sb->width, sb->height);
	canvas_round_rect(sb->backdrop, 0, 0, sb->width, sb->height, 0, 0xb8000000);
	sb->opening = -1;
}

static int by_recency(const void *a, const void *b) {
	const struct sb_scene *x = *(struct sb_scene *const *)a, *y = *(struct sb_scene *const *)b;
	return x->last_foreground < y->last_foreground ? 1 :
		x->last_foreground > y->last_foreground ? -1 : 0;
}

void switcher_show(struct starboard *sb, int from_y) {
	if (sb->switcher_shown) {
		return;
	}
	struct sb_scene *scenes[SB_MAX_CARDS];
	int count = 0;
	struct sb_scene *scene;
	wl_list_for_each(scene, &sb->scenes, link) {
		if (count < SB_MAX_CARDS) {
			scenes[count++] = scene;
		}
	}
	if (count == 0) {
		return;
	}
	qsort(scenes, count, sizeof(scenes[0]), by_recency);

	sb->switcher_shown = true;
	sb->card_count = count;
	sb->scroll = sb->scroll_target = 0;
	sb->opening = -1;
	sb->switcher_frame_msec = 0;
	wl_surface_set_buffer_scale(sb->switcher, 1);
	wl_surface_attach(sb->switcher, sb->backdrop->buffer, 0, 0);
	sb->backdrop->busy = true;
	wl_surface_damage_buffer(sb->switcher, 0, 0, INT32_MAX, INT32_MAX);

	const int s = SB_SCALE;
	for (int i = 0; i < count; i++) {
		struct sb_card *card = &sb->cards[i];
		memset(card, 0, sizeof(*card));
		card->scene = scenes[i];
		card->view = starry_scene_v1_get_view(scenes[i]->scene);
		const char *name = sb_app_name_for_pid(sb, scenes[i]->pid);
		if (!name) {
			name = scenes[i]->app_id[0] ? scenes[i]->app_id : "App";
		}
		card->label_canvas = canvas_create(sb->shm, LABEL_WIDTH * s, LABEL_HEIGHT * s);
		double width = canvas_text_width(true, 14 * s, name);
		canvas_text(card->label_canvas, true, 14 * s, (LABEL_WIDTH * s - width) / 2, 16 * s,
			0xffffffff, name);
		sb_sprite_init(sb, sb->switcher, &card->label, card->label_canvas);

		double x, y, w, h;
		card_target(sb, i, &x, &y, &w, &h);
		if (i == 0 && from_y >= 0) {
			/* Continue from where the swiped app was left. */
			card->x = 0;
			card->y = from_y;
			card->width = sb->width;
			card->height = sb->height - SB_STATUS_HEIGHT;
		} else {
			card->x = x - sb->width * 0.4;
			card->y = y;
			card->width = w;
			card->height = h;
		}
		card_place(sb, card);
	}
	schedule(sb);
}

void switcher_hide(struct starboard *sb) {
	if (!sb->switcher_shown) {
		return;
	}
	for (int i = 0; i < sb->card_count; i++) {
		card_destroy(&sb->cards[i]);
	}
	sb->card_count = 0;
	sb->switcher_shown = false;
	sb->switcher_touch = NULL;
	sb->opening = -1;
	wl_surface_attach(sb->switcher, NULL, 0, 0);
	wl_surface_commit(sb->switcher);
}

void switcher_scene_closed(struct starboard *sb, struct sb_scene *scene) {
	for (int i = 0; i < sb->card_count; i++) {
		if (sb->cards[i].scene == scene) {
			sb->cards[i].scene = NULL;
			if (!sb->cards[i].closing && sb->switcher_shown) {
				sb->cards[i].closing = true; /* closed from elsewhere: fly it away */
				schedule(sb);
			}
		}
	}
}

/* ---- gestures: scroll sideways, fling a card up to close, tap to open ---- */

static int card_at(struct starboard *sb, double x, double y) {
	for (int i = 0; i < sb->card_count; i++) {
		struct sb_card *card = &sb->cards[i];
		if (!card->closing && x >= card->x && x < card->x + card->width &&
				y >= card->y && y < card->y + card->height) {
			return i;
		}
	}
	return -1;
}

void switcher_touch_down(struct starboard *sb, struct sb_touch *point) {
	if (!sb->switcher_shown || sb->switcher_touch || sb->opening >= 0) {
		return;
	}
	sb->switcher_touch = point;
	sb->touch_card = card_at(sb, point->x, point->y);
	sb->touch_axis = SB_AXIS_NONE;
	sb->touch_scroll = sb->scroll;
}

void switcher_touch_motion(struct starboard *sb, struct sb_touch *point, uint32_t time) {
	if (point != sb->switcher_touch) {
		return;
	}
	double dx = point->x - point->start_x, dy = point->y - point->start_y;
	if (sb->touch_axis == SB_AXIS_NONE && hypot(dx, dy) >= AXIS_SLOP) {
		sb->touch_axis = fabs(dx) > fabs(dy) || sb->touch_card < 0 ?
			SB_AXIS_HORIZONTAL : SB_AXIS_VERTICAL;
	}
	if (sb->touch_axis == SB_AXIS_HORIZONTAL) {
		double scroll = sb->touch_scroll + dx, limit = max_scroll(sb);
		/* Rubber band past either end. */
		if (scroll < 0) {
			scroll *= 0.35;
		} else if (scroll > limit) {
			scroll = limit + (scroll - limit) * 0.35;
		}
		sb->scroll = sb->scroll_target = scroll;
	} else if (sb->touch_axis == SB_AXIS_VERTICAL) {
		/* Up moves freely, down resists. */
		sb->cards[sb->touch_card].lift = dy < 0 ? dy : dy * 0.2;
	}
	schedule(sb);
}

void switcher_touch_up(struct starboard *sb, struct sb_touch *point, uint32_t time) {
	if (point != sb->switcher_touch) {
		return;
	}
	sb->switcher_touch = NULL;
	int index = sb->touch_card;
	if (sb->touch_axis == SB_AXIS_NONE &&
			hypot(point->x - point->start_x, point->y - point->start_y) < TAP_SLOP) {
		if (index >= 0) {
			sb->opening = index;
			sb->cards[index].lift = 0;
		} else {
			switcher_hide(sb); /* home is already underneath */
			return;
		}
	} else if (sb->touch_axis == SB_AXIS_HORIZONTAL) {
		/* Settle on the nearest card, nudged by the flick. */
		double projected = sb->scroll + point->velocity_x * 180;
		double nearest = round(projected / card_pitch(sb)) * card_pitch(sb);
		sb->scroll_target = fmax(0, fmin(max_scroll(sb), nearest));
	} else if (sb->touch_axis == SB_AXIS_VERTICAL && index >= 0) {
		struct sb_card *card = &sb->cards[index];
		if (card->lift < -DISMISS_DISTANCE || point->velocity_y < -DISMISS_VELOCITY) {
			card->closing = true;
			if (card->scene) {
				starry_scene_v1_close(card->scene->scene);
			}
		}
		card->lift = 0;
	}
	schedule(sb);
}
