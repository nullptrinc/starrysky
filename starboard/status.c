/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "canvas.h"
#include "constellation.h"
#include "starry-system-v1-client-protocol.h"
#include "starboard.h"

#define INDICATOR_WIDTH 134
#define INDICATOR_HEIGHT 5
#define TEXT_SIZE 13
#define CLOCK_SIZE 15
#define EDGE 22
#define GAP 10
#define BATTERY_HEIGHT 12

/* ---- plugin host ---- */

static bool host_add_status_item(struct starry_host *host, const struct starry_status_item *item) {
	struct starboard *sb = (struct starboard *)host;
	if (sb->status_item_count >= SB_MAX_STATUS_ITEMS || !item->text ||
			item->size < offsetof(struct starry_status_item, data) + sizeof(item->data)) {
		return false;
	}
	struct sb_status_item *slot = &sb->status_items[sb->status_item_count++];
	memset(slot, 0, sizeof(*slot));
	memcpy(&slot->item, item, item->size < sizeof(slot->item) ? item->size : sizeof(slot->item));
	return true;
}

static void host_refresh_status(struct starry_host *host) {
	struct starboard *sb = (struct starboard *)host;
	for (int i = 0; i < sb->status_item_count; i++) {
		sb->status_items[i].next_poll = 0;
	}
	status_update(sb);
}

static void host_log(struct starry_host *host, const char *format, ...) {
	va_list args;
	va_start(args, format);
	fprintf(stderr, "starboard plugin: ");
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static const struct starry_host_api host_api = {
	.size = sizeof(struct starry_host_api),
	.add_status_item = host_add_status_item,
	.refresh_status = host_refresh_status,
	.log = host_log,
};

static int plugin_filter(const struct dirent *entry) {
	size_t length = strlen(entry->d_name);
	return length > 3 && strcmp(entry->d_name + length - 3, ".so") == 0;
}

void status_load_plugins(struct starboard *sb, const char *directory) {
	struct dirent **entries;
	int count = scandir(directory, &entries, plugin_filter, alphasort);
	for (int i = 0; i < count; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", directory, entries[i]->d_name);
		free(entries[i]);
		void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		const struct starry_plugin *plugin = handle ? dlsym(handle, "starry_plugin") : NULL;
		if (!plugin || plugin->abi != STARRY_PLUGIN_ABI || !plugin->init) {
			sb_log("Skipping plugin %s: %s", path, handle ? "no compatible starry_plugin" : dlerror());
			if (handle) {
				dlclose(handle);
			}
			continue;
		}
		if (!plugin->init((struct starry_host *)sb, &host_api)) {
			sb_log("Plugin %s failed to initialise", plugin->name);
			continue;
		}
		sb_log("Loaded plugin %s", plugin->name);
	}
	if (count > 0) {
		free(entries);
	}
}

/* ---- status bar and home indicator ---- */

static void blinks_changed(const struct cn_object *object, bool exists, void *data) {
	struct starboard *sb = data;
	battery_changed(&sb->battery, object, exists);
}

/* Subscribes to /blinks/power/, or again after constellationd restarted. */
static void blinks_connect(struct starboard *sb) {
	if (sb->blinks && cn_fd(sb->blinks) < 0 && cn_reconnect(sb->blinks)) {
		sb_log("Constellation is back");
	}
	if (sb->blinks) {
		return;
	}
	sb->blinks = cn_connect();
	if (sb->blinks && !cn_subscribe(sb->blinks, CN_NAMESPACE "power/")) {
		cn_disconnect(sb->blinks);
		sb->blinks = NULL;
	}
}

int status_blinks_fd(struct starboard *sb) {
	return sb->blinks ? cn_fd(sb->blinks) : -1;
}

void status_blinks_dispatch(struct starboard *sb) {
	if (sb->blinks && !cn_dispatch(sb->blinks, blinks_changed, sb)) {
		sb_log("Constellation went away");
		sb->battery = (struct sb_battery_state){0};
	}
	status_update(sb);
}

static void status_tick(struct starboard *sb) {
	blinks_connect(sb);
	status_update(sb);
	sb_timer_arm(&sb->status_tick, 1000);
}

void status_create(struct starboard *sb) {
	sb->status = sb_surface_create(sb, true);
	sb->status_layer = starry_system_v1_get_layer_surface(sb->system, sb->status,
		STARRY_SYSTEM_V1_LAYER_OVERLAY);

	const int s = SB_SCALE;
	sb->indicator = sb_surface_create(sb, false);
	sb->indicator_layer = starry_system_v1_get_layer_surface(sb->system, sb->indicator,
		STARRY_SYSTEM_V1_LAYER_OVERLAY);
	starry_layer_surface_v1_set_position(sb->indicator_layer,
		(sb->width - INDICATOR_WIDTH) / 2, sb->height - 9);
	sb->indicator_canvas = canvas_create(sb->shm, INDICATOR_WIDTH * s, INDICATOR_HEIGHT * s);
	canvas_round_rect(sb->indicator_canvas, 0, 0, INDICATOR_WIDTH * s, INDICATOR_HEIGHT * s,
		2.5 * s, 0xe6ffffff);

	battery_load(STARRY_DATA_DIR "/icons/battery.svg", STARRY_DATA_DIR "/icons/battery.map");

	sb->status_tick.callback = status_tick;
	blinks_connect(sb);
	status_update(sb);
	sb_timer_arm(&sb->status_tick, 1000);
}

static void show_indicator(struct starboard *sb, bool shown) {
	if (sb->indicator_shown == shown) {
		return;
	}
	sb->indicator_shown = shown;
	wl_surface_attach(sb->indicator, shown ? sb->indicator_canvas->buffer : NULL, 0, 0);
	if (shown) {
		sb->indicator_canvas->busy = true;
		wl_surface_damage_buffer(sb->indicator, 0, 0, INT32_MAX, INT32_MAX);
	}
	wl_surface_commit(sb->indicator);
}

/* Polls due items and redraws only when something visible changed. */
void status_update(struct starboard *sb) {
	if (!sb->status) {
		return;
	}
	uint32_t now = sb_now_msec();
	for (int i = 0; i < sb->status_item_count; i++) {
		struct sb_status_item *item = &sb->status_items[i];
		if (item->next_poll <= now) {
			item->text[0] = 0;
			item->item.text(item->item.data, item->text, sizeof(item->text));
			item->next_poll = item->item.refresh_msec > 0 ?
				now + item->item.refresh_msec : UINT32_MAX;
		}
	}
	bool foreground = sb_foreground_scene(sb) != NULL;
	show_indicator(sb, foreground);

	time_t wall = time(NULL);
	struct tm local;
	localtime_r(&wall, &local);
	char clock[16];
	strftime(clock, sizeof(clock), "%H:%M", &local);
	const struct sb_battery_state battery = sb->battery;
	char key[sizeof(sb->status_key)];
	int length = snprintf(key, sizeof(key), "%s|%d|%d,%d,%d,%d", clock, foreground,
		battery.present, battery.level, battery.charging, battery.saver);
	for (int i = 0; i < sb->status_item_count && length < (int)sizeof(key); i++) {
		length += snprintf(key + length, sizeof(key) - length, "|%s", sb->status_items[i].text);
	}
	if (strcmp(key, sb->status_key) == 0) {
		return;
	}
	snprintf(sb->status_key, sizeof(sb->status_key), "%s", key);

	const int s = SB_SCALE, w = sb->width;
	struct canvas *canvas = canvas_create(sb->shm, w * s, SB_STATUS_HEIGHT * s);
	if (foreground) {
		canvas_round_rect(canvas, 0, 0, w * s, SB_STATUS_HEIGHT * s, 0, 0xff000000);
	}
	double baseline = 19 * s;
	canvas_text(canvas, true, CLOCK_SIZE * s, EDGE * s, baseline, 0xffffffff, clock);
	double left = EDGE * s + canvas_text_width(true, CLOCK_SIZE * s, clock) + GAP * s;
	double right = (w - EDGE) * s;
	/* The status bar is dark today, so the battery uses its light art. */
	double battery_width = battery_draw(canvas, right, (SB_STATUS_HEIGHT - BATTERY_HEIGHT) / 2.0 * s,
		BATTERY_HEIGHT * s, &battery, true);
	if (battery_width > 0) {
		right -= battery_width + GAP * s;
	}
	for (int i = 0; i < sb->status_item_count; i++) {
		struct sb_status_item *item = &sb->status_items[i];
		if (!item->text[0]) {
			continue;
		}
		double width = canvas_text_width(true, TEXT_SIZE * s, item->text);
		if (item->item.side == STARRY_STATUS_LEFT) {
			canvas_text(canvas, true, TEXT_SIZE * s, left, baseline, 0xffffffff, item->text);
			left += width + GAP * s;
		} else {
			right -= width;
			canvas_text(canvas, true, TEXT_SIZE * s, right, baseline, 0xffffffff, item->text);
			right -= GAP * s;
		}
	}
	wl_surface_attach(sb->status, canvas->buffer, 0, 0);
	canvas->busy = true;
	wl_surface_damage_buffer(sb->status, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(sb->status);
	if (sb->status_canvas) {
		canvas_release(sb->status_canvas);
	}
	sb->status_canvas = canvas;
}
