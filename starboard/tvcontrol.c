/* SPDX-License-Identifier: MIT */
/*
 * The TV control centre and power menu, as on tvOS: holding Home brings up
 * a panel in the top right corner over whatever is on screen, with Sleep,
 * the volume, the Siri Remote's battery and a way to the power menu; Power
 * or Sleep on a remote opens the power menu itself: Sleep, Restart, Shut
 * Down. Up and down choose, OK acts, left and right (or the volume keys)
 * set the volume, Back or Home goes back.
 *
 * Sleep turns the panel off through the compositor (starry_system_v1
 * version 3) and asks lunar-settingsd to put the TV in standby over
 * HDMI-CEC; the next key press turns the panel on again, and the shell
 * then asks for the TV back. Restart and Shut Down are lunar-settingsd's
 * too, as are the volume and the CEC messages: starboard runs without root.
 *
 * Both live on one surface on the lock layer, which takes the keyboard
 * while mapped: a dim stretched over the screen, and the panel, redrawn
 * whenever something on it changes.
 */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "canvas.h"
#include "constellation.h"
#include "starry-system-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "starboard.h"
#include "tv.h"

#define REMOTE_STATE "/run/siri-remote"
#define PANEL_W 560
#define PANEL_PAD 28
#define PANEL_RADIUS 32
#define PANEL_TOP 140 /* below the clock */
#define PANEL_FILL 0xff1c2029
#define TILE_RADIUS 18
#define TILE_FILL 0x24ffffff
#define TILE_FOCUS 0xfff2f4f8
#define TEXT_DARK 0xff11141a
#define BUTTON_H 84
#define VOLUME_H 118
#define GAP 16
#define VOLUME_STEP 5
#define POLL_MSEC 2000 /* the remote's battery, while the panel is up */
#define NOTE_MSEC 2500 /* an error, then the panel goes */
#define DIM_CONTROL 0x50000000
#define DIM_POWER 0xc8000000

enum item { ITEM_SLEEP, ITEM_VOLUME, ITEM_POWER, ITEM_RESTART, ITEM_SHUTDOWN, ITEM_CANCEL };

static const enum item control_items[] = { ITEM_SLEEP, ITEM_VOLUME, ITEM_POWER };
static const enum item power_items[] = { ITEM_SLEEP, ITEM_RESTART, ITEM_SHUTDOWN, ITEM_CANCEL };

static const char *item_label(enum item item) {
	switch (item) {
	case ITEM_SLEEP: return "Sleep";
	case ITEM_VOLUME: return "Volume";
	case ITEM_POWER: return "Restart or Shut Down…";
	case ITEM_RESTART: return "Restart";
	case ITEM_SHUTDOWN: return "Shut Down";
	case ITEM_CANCEL: return "Cancel";
	}
	return "";
}

static const enum item *items(struct starboard *sb, int *count) {
	if (sb->cc_page == SB_CC_POWER) {
		*count = sizeof(power_items) / sizeof(power_items[0]);
		return power_items;
	}
	*count = sizeof(control_items) / sizeof(control_items[0]);
	return control_items;
}

/* ---- lunar-settingsd: one Constellation request each ---- */

bool sb_settings(const char *request, char *reply, size_t size) {
	char answer[256];
	/* CEC retries to a TV that does not answer take about a second. */
	int result = cn_ask("settings", request, 3000, answer, sizeof(answer));
	if (result < 0) {
		char reason[32];
		snprintf(reason, sizeof(reason), "%.31s", answer);
		snprintf(answer, sizeof(answer), "err %s", reason);
	}
	answer[strcspn(answer, "\n")] = 0;
	if (reply && size) {
		snprintf(reply, size, "%s", answer);
	}
	if (result != 1) {
		sb_log("lunar-settingsd: \"%s\": %s", request, answer);
	}
	return result == 1;
}

/* ---- state: the remote from siri-remoted, the volume from Constellation ---- */

static void read_remote(struct starboard *sb) {
	FILE *file = fopen(REMOTE_STATE, "r");
	sb->remote_known = file != NULL;
	sb->remote_connected = false;
	sb->remote_battery = -1;
	sb->remote_charging = false;
	char line[128];
	while (file && fgets(line, sizeof(line), file)) {
		int value;
		if (sscanf(line, "connected=%d", &value) == 1) {
			sb->remote_connected = value > 0;
		} else if (sscanf(line, "battery=%d", &value) == 1) {
			sb->remote_battery = value;
		} else if (sscanf(line, "charging=%d", &value) == 1) {
			sb->remote_charging = value > 0;
		}
	}
	if (file) {
		fclose(file);
	}
}

/* ---- drawing ---- */

static void fit_text(struct canvas *canvas, bool bold, double size, double x, double baseline,
		double width, uint32_t color, const char *text) {
	while (size > 16 && canvas_text_width(bold, size, text) > width) {
		size -= 2;
	}
	canvas_text(canvas, bold, size, x, baseline, color, text);
}

static void draw_button(struct canvas *canvas, double y, const char *label, bool focused,
		bool centred) {
	double x = PANEL_PAD, width = PANEL_W - 2 * PANEL_PAD;
	canvas_round_rect(canvas, x, y, width, BUTTON_H, TILE_RADIUS,
		focused ? TILE_FOCUS : TILE_FILL);
	double size = 32, text_x = x + 28;
	if (centred) {
		text_x = x + (width - canvas_text_width(true, size, label)) / 2;
	}
	fit_text(canvas, true, size, text_x, y + BUTTON_H / 2.0 + 11, width - 56,
		focused ? TEXT_DARK : TEXT, label);
}

static void draw_volume(struct starboard *sb, struct canvas *canvas, double y, bool focused) {
	double x = PANEL_PAD, width = PANEL_W - 2 * PANEL_PAD;
	uint32_t text = focused ? TEXT_DARK : TEXT;
	uint32_t secondary = focused ? 0xff4a5160 : TEXT_SECONDARY;
	canvas_round_rect(canvas, x, y, width, VOLUME_H, TILE_RADIUS,
		focused ? TILE_FOCUS : TILE_FILL);
	canvas_text(canvas, true, 30, x + 28, y + 46, text, "Volume");
	char value[32];
	if (!sb->volume_known) {
		snprintf(value, sizeof(value), "Unavailable");
	} else if (sb->volume_muted) {
		snprintf(value, sizeof(value), "Muted");
	} else {
		snprintf(value, sizeof(value), "%d%%", sb->volume);
	}
	canvas_text(canvas, false, 28, x + width - 28 - canvas_text_width(false, 28, value),
		y + 46, secondary, value);
	/* The bar, with ‹ › at its ends while chosen. */
	double bar_x = x + 28, bar_w = width - 56, bar_y = y + 72, bar_h = 14;
	if (focused) {
		canvas_text(canvas, true, 28, x + 14, bar_y + 13, secondary, "‹");
		canvas_text(canvas, true, 28, x + width - 26, bar_y + 13, secondary, "›");
	}
	canvas_round_rect(canvas, bar_x, bar_y, bar_w, bar_h, bar_h / 2,
		focused ? 0x33000000 : 0x33ffffff);
	if (sb->volume_known && sb->volume > 0) {
		double fill = bar_w * sb->volume / 100.0;
		canvas_round_rect(canvas, bar_x, bar_y, fill < bar_h ? bar_h : fill, bar_h, bar_h / 2,
			sb->volume_muted ? (focused ? 0x66000000 : 0x66ffffff) :
			(focused ? TEXT_DARK : 0xffffffff));
	}
}

static void remote_text(struct starboard *sb, char *buffer, size_t size) {
	if (!sb->remote_known) {
		snprintf(buffer, size, "No remote paired");
	} else if (!sb->remote_connected) {
		snprintf(buffer, size, "Not connected");
	} else if (sb->remote_battery < 0) {
		snprintf(buffer, size, sb->remote_charging ? "Charging" : "Connected");
	} else {
		snprintf(buffer, size, "%d%%%s", sb->remote_battery,
			sb->remote_charging ? ", charging" : "");
	}
}

static int panel_height(struct starboard *sb) {
	int count;
	items(sb, &count);
	if (sb->cc_page == SB_CC_POWER) {
		return PANEL_PAD + 56 + count * (BUTTON_H + GAP) - GAP + PANEL_PAD;
	}
	/* Title, Sleep, volume, power, then the remote. */
	return PANEL_PAD + 56 + BUTTON_H + GAP + VOLUME_H + GAP + BUTTON_H + GAP + 64 + PANEL_PAD;
}

static void redraw(struct starboard *sb) {
	int height = panel_height(sb);
	struct canvas *canvas = canvas_create(sb->shm, PANEL_W, height);
	canvas_round_rect(canvas, 0, 0, PANEL_W, height, PANEL_RADIUS, PANEL_FILL);
	const char *title = sb->cc_page == SB_CC_POWER ? "Power" : "Control Center";
	canvas_text(canvas, true, 34, PANEL_PAD + 4, PANEL_PAD + 38, TEXT, title);
	double y = PANEL_PAD + 56;
	int count;
	const enum item *list = items(sb, &count);
	if (sb->cc_note[0]) {
		/* Restarting, shutting down, or what went wrong: nothing to choose. */
		fit_text(canvas, false, 30, PANEL_PAD + 4, y + 44, PANEL_W - 2 * PANEL_PAD,
			TEXT_SECONDARY, sb->cc_note);
	} else {
		for (int i = 0; i < count; i++) {
			bool focused = i == sb->cc_focus;
			if (list[i] == ITEM_VOLUME) {
				draw_volume(sb, canvas, y, focused);
				y += VOLUME_H + GAP;
			} else {
				draw_button(canvas, y, item_label(list[i]), focused,
					sb->cc_page == SB_CC_POWER);
				y += BUTTON_H + GAP;
			}
		}
		if (sb->cc_page == SB_CC_CONTROL) {
			char text[64];
			remote_text(sb, text, sizeof(text));
			canvas_text(canvas, false, 26, PANEL_PAD + 4, y + 40, TEXT_SECONDARY, "Siri Remote");
			canvas_text(canvas, false, 26,
				PANEL_W - PANEL_PAD - 4 - canvas_text_width(false, 26, text), y + 40,
				sb->remote_connected && sb->remote_battery >= 0 && sb->remote_battery <= 15 ?
				0xffff6b5a : TEXT, text);
		}
	}
	sb_sprite_show(&sb->cc_panel, canvas);
	if (sb->cc_panel_canvas) {
		canvas_release(sb->cc_panel_canvas);
	}
	sb->cc_panel_canvas = canvas;
	if (sb->cc_page == SB_CC_POWER) {
		sb_sprite_move(&sb->cc_panel, (sb->width - PANEL_W) / 2.0, (sb->height - height) / 2.0);
	} else {
		sb_sprite_move(&sb->cc_panel, sb->width - MARGIN_X - PANEL_W, PANEL_TOP);
	}
	wl_surface_commit(sb->cc);
}

/* ---- showing and hiding ---- */

static void show_page(struct starboard *sb, int page) {
	sb->cc_page = page;
	sb->cc_focus = 0;
	sb->cc_ok_down = false;
	sb->cc_note[0] = 0;
	struct canvas *dim = page == SB_CC_POWER ? sb->cc_dim_power : sb->cc_dim_control;
	dim->busy = true;
	wl_surface_attach(sb->cc, dim->buffer, 0, 0);
	wl_surface_damage_buffer(sb->cc, 0, 0, 1, 1);
	redraw(sb);
}

void tv_control_show(struct starboard *sb, bool power) {
	if (!sb->system || wl_proxy_get_version((struct wl_proxy *)sb->system) <
			STARRY_SYSTEM_V1_SET_SCREEN_SINCE_VERSION) {
		sb_log("The compositor has no screen control; no control centre");
		return;
	}
	tv_menu_hide(sb);
	read_remote(sb);
	sb->cc_shown = true;
	sb->cc_from_control = false;
	sb_timer_arm(&sb->cc_poll, POLL_MSEC);
	show_page(sb, power ? SB_CC_POWER : SB_CC_CONTROL);
}

void tv_control_hide(struct starboard *sb) {
	if (!sb->cc_shown) {
		return;
	}
	sb->cc_shown = false;
	sb_timer_arm(&sb->cc_poll, 0);
	sb_timer_arm(&sb->cc_note_timer, 0);
	sb_sprite_show(&sb->cc_panel, NULL);
	/* Unmapping gives the keyboard back to the app or the home screen. */
	wl_surface_attach(sb->cc, NULL, 0, 0);
	wl_surface_commit(sb->cc);
}

static void poll_remote(struct starboard *sb) {
	if (!sb->cc_shown) {
		return;
	}
	bool connected = sb->remote_connected, charging = sb->remote_charging;
	int battery = sb->remote_battery;
	read_remote(sb);
	if (sb->cc_page == SB_CC_CONTROL && !sb->cc_note[0] && (connected != sb->remote_connected ||
			charging != sb->remote_charging || battery != sb->remote_battery)) {
		redraw(sb);
	}
	sb_timer_arm(&sb->cc_poll, POLL_MSEC);
}

static void note_done(struct starboard *sb) {
	tv_control_hide(sb);
}

static void show_note(struct starboard *sb, const char *text, bool linger) {
	snprintf(sb->cc_note, sizeof(sb->cc_note), "%s", text);
	redraw(sb);
	sb_timer_arm(&sb->cc_note_timer, linger ? 0 : NOTE_MSEC);
}

/* ---- actions ---- */

void tv_sleep(struct starboard *sb) {
	tv_control_hide(sb);
	tv_menu_hide(sb);
	if (!sb->system || wl_proxy_get_version((struct wl_proxy *)sb->system) <
			STARRY_SYSTEM_V1_SET_SCREEN_SINCE_VERSION) {
		return;
	}
	sb_log("Sleep");
	sb->asleep = true;
	starry_system_v1_set_screen(sb->system, 0);
	wl_display_flush(sb->display);
	/* The TV too, if it listens to CEC; the panel is off either way. */
	sb_settings("power sleep", NULL, 0);
}

/* The compositor turned the panel off or on (a key woke it, or we asked). */
void tv_screen_changed(struct starboard *sb, bool on) {
	if (!on) {
		tv_control_hide(sb);
		return;
	}
	if (sb->asleep) {
		sb->asleep = false;
		sb_log("Woken");
		sb_settings("power wake", NULL, 0);
	}
}

static void power_off(struct starboard *sb, bool reboot) {
	char reply[128];
	show_note(sb, reboot ? "Restarting…" : "Shutting down…", true);
	wl_display_flush(sb->display);
	if (!sb_settings(reboot ? "power reboot" : "power poweroff", reply, sizeof(reply))) {
		char text[160];
		snprintf(text, sizeof(text), "Could not %s: %s", reboot ? "restart" : "shut down", reply);
		show_note(sb, text, false);
	}
}

static void set_volume(struct starboard *sb, int delta) {
	if (!sb->volume_known) {
		return;
	}
	char request[64];
	if (sb->volume_muted && delta > 0) {
		sb_settings("audio mute off", NULL, 0);
		sb->volume_muted = false;
	}
	int volume = sb->volume + delta;
	volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
	if (volume != sb->volume) {
		snprintf(request, sizeof(request), "audio volume %d", volume);
		if (sb_settings(request, NULL, 0)) {
			sb->volume = volume; /* Constellation confirms it shortly */
		}
	}
	if (sb->cc_shown) {
		redraw(sb);
	}
}

static void act(struct starboard *sb) {
	int count;
	const enum item *list = items(sb, &count);
	if (sb->cc_focus < 0 || sb->cc_focus >= count) {
		return;
	}
	switch (list[sb->cc_focus]) {
	case ITEM_SLEEP:
		tv_sleep(sb);
		break;
	case ITEM_VOLUME:
		if (sb->volume_known) {
			char request[32];
			snprintf(request, sizeof(request), "audio mute %s", sb->volume_muted ? "off" : "on");
			if (sb_settings(request, NULL, 0)) {
				sb->volume_muted = !sb->volume_muted;
			}
			redraw(sb);
		}
		break;
	case ITEM_POWER:
		sb->cc_from_control = true; /* Back returns to the control centre */
		show_page(sb, SB_CC_POWER);
		break;
	case ITEM_RESTART:
		power_off(sb, true);
		break;
	case ITEM_SHUTDOWN:
		power_off(sb, false);
		break;
	case ITEM_CANCEL:
		tv_control_hide(sb);
		break;
	}
}

void tv_control_key(struct starboard *sb, uint32_t key, bool pressed) {
	if (!pressed) {
		/* OK acts on release, so the press that opened nothing leaks through. */
		if ((key == KEY_OK || key == KEY_SELECT || key == KEY_ENTER || key == KEY_KPENTER) &&
				sb->cc_ok_down && !sb->cc_note[0]) {
			sb->cc_ok_down = false;
			act(sb);
		}
		return;
	}
	if (sb->cc_note[0]) {
		/* Restarting: nothing to do. An error: any key dismisses it. */
		if (sb->cc_note_timer.deadline) {
			tv_control_hide(sb);
		}
		return;
	}
	int count;
	const enum item *list = items(sb, &count);
	switch (key) {
	case KEY_UP:
	case KEY_DOWN: {
		int next = sb->cc_focus + (key == KEY_UP ? -1 : 1);
		if (next >= 0 && next < count) {
			sb->cc_focus = next;
			redraw(sb);
		}
		break;
	}
	case KEY_LEFT:
	case KEY_RIGHT:
		if (list[sb->cc_focus] == ITEM_VOLUME) {
			set_volume(sb, key == KEY_LEFT ? -VOLUME_STEP : VOLUME_STEP);
		}
		break;
	case KEY_VOLUMEUP:
	case KEY_VOLUMEDOWN:
		set_volume(sb, key == KEY_VOLUMEDOWN ? -VOLUME_STEP : VOLUME_STEP);
		break;
	case KEY_OK:
	case KEY_SELECT:
	case KEY_ENTER:
	case KEY_KPENTER:
		sb->cc_ok_down = true;
		break;
	case KEY_BACK:
	case KEY_ESC:
		/* From the power menu back to the control centre it came from. */
		if (sb->cc_page == SB_CC_POWER && sb->cc_from_control) {
			sb->cc_from_control = false;
			show_page(sb, SB_CC_CONTROL);
			sb->cc_focus = 2;
			redraw(sb);
		} else {
			tv_control_hide(sb);
		}
		break;
	case KEY_POWER:
	case KEY_SLEEP:
		tv_sleep(sb);
		break;
	}
}

/* A reserved key from the compositor: Power or Sleep. A second press
 * while the power menu is up puts the box to sleep, as a TV remote's power
 * key would. */
void tv_power_key(struct starboard *sb, uint32_t key) {
	if (sb->cc_shown && sb->cc_page == SB_CC_POWER && !sb->cc_note[0]) {
		tv_sleep(sb);
		return;
	}
	if (sb->cc_shown && sb->cc_note[0]) {
		return;
	}
	tv_control_show(sb, true);
}

void tv_control_audio_changed(struct starboard *sb, const struct cn_object *object, bool exists) {
	sb->volume_known = exists && cn_get_bool(object, "available", false);
	sb->volume = (int)cn_get_int(object, "volume", 0);
	sb->volume_muted = cn_get_bool(object, "muted", false);
	if (sb->cc_shown && sb->cc_page == SB_CC_CONTROL && !sb->cc_note[0]) {
		redraw(sb);
	}
}

void tv_control_create(struct starboard *sb) {
	sb->cc_poll.callback = poll_remote;
	sb->cc_note_timer.callback = note_done;
	sb->cc = sb_surface_create(sb, false);
	sb->cc_layer = starry_system_v1_get_layer_surface(sb->system, sb->cc,
		STARRY_SYSTEM_V1_LAYER_LOCK);
	/* One translucent pixel each, stretched over the screen. */
	sb->cc_dim_control = canvas_create(sb->shm, 1, 1);
	sb->cc_dim_control->pixels[0] = DIM_CONTROL;
	sb->cc_dim_power = canvas_create(sb->shm, 1, 1);
	sb->cc_dim_power->pixels[0] = DIM_POWER;
	sb->cc_viewport = wp_viewporter_get_viewport(sb->viewporter, sb->cc);
	wp_viewport_set_destination(sb->cc_viewport, sb->width, sb->height);
	sb_sprite_init(sb, sb->cc, &sb->cc_panel, NULL);
	wl_surface_commit(sb->cc); /* no buffer: unmapped until shown */
}
