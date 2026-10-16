/* SPDX-License-Identifier: MIT */
/*
 * The status bar battery, drawn from an Inkscape sheet (data/icons/battery.svg).
 * battery.map, made by tools/svg-layers.py, names each layer's shapes:
 * "base" is the empty battery, the *_overlay layers are level fills,
 * "lightning" marks charging and percents_white/N, percents_black/N are digits.
 * Each part is rasterized with NanoSVG at the size the bar needs. The state
 * comes from Constellation: /blinks/power/battery and /blinks/power/saver.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "constellation.h"
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"
#include "starboard.h"

#define MAX_PARTS 48
#define MAX_PART_SHAPES 8

struct part {
	char name[48];
	NSVGshape *shapes[MAX_PART_SHAPES];
	int count;
	float bounds[4]; /* union of the shapes' bounds, in sheet units */
};

static NSVGimage *image;
static NSVGrasterizer *rasterizer;
static struct part parts[MAX_PARTS];
static int part_count;

static struct part *find_part(const char *name) {
	for (int i = 0; i < part_count; i++) {
		if (strcmp(parts[i].name, name) == 0) {
			return &parts[i];
		}
	}
	return NULL;
}

static NSVGshape *find_shape(const char *id) {
	for (NSVGshape *shape = image->shapes; shape; shape = shape->next) {
		if (strcmp(shape->id, id) == 0) {
			return shape;
		}
	}
	return NULL;
}

bool battery_load(const char *svg_path, const char *map_path) {
	/* The sheet's viewBox is in millimetres; keep those as the units. */
	image = nsvgParseFromFile(svg_path, "mm", 96);
	FILE *map = fopen(map_path, "r");
	if (!image || !map) {
		sb_log("No battery art (%s, %s)", svg_path, map_path);
		if (map) {
			fclose(map);
		}
		return false;
	}
	char name[48], id[64];
	while (fscanf(map, "%47s %63s", name, id) == 2) {
		NSVGshape *shape = find_shape(id);
		if (!shape) {
			continue;
		}
		struct part *part = find_part(name);
		if (!part && part_count < MAX_PARTS) {
			part = &parts[part_count++];
			snprintf(part->name, sizeof(part->name), "%s", name);
			memcpy(part->bounds, shape->bounds, sizeof(part->bounds));
		}
		if (!part || part->count >= MAX_PART_SHAPES) {
			continue;
		}
		part->shapes[part->count++] = shape;
		part->bounds[0] = fminf(part->bounds[0], shape->bounds[0]);
		part->bounds[1] = fminf(part->bounds[1], shape->bounds[1]);
		part->bounds[2] = fmaxf(part->bounds[2], shape->bounds[2]);
		part->bounds[3] = fmaxf(part->bounds[3], shape->bounds[3]);
	}
	fclose(map);
	rasterizer = nsvgCreateRasterizer();
	bool ok = find_part("base") && find_part("percents_black/0");
	if (!ok) {
		sb_log("Battery art lacks the base or digit layers");
	}
	return ok;
}

static float part_width(const struct part *part) {
	return part->bounds[2] - part->bounds[0];
}

static float part_height(const struct part *part) {
	return part->bounds[3] - part->bounds[1];
}

/* Rasterizes one part with its bounding box's top-left at (x, y), in pixels. */
static void draw_part(struct canvas *canvas, const struct part *part, double x, double y,
		double scale, bool invert, double clip_width) {
	int width = (int)ceil(part_width(part) * scale) + 1;
	int height = (int)ceil(part_height(part) * scale) + 1;
	uint8_t *pixels = calloc((size_t)width * height, 4);
	if (!pixels) {
		return;
	}
	for (NSVGshape *shape = image->shapes; shape; shape = shape->next) {
		shape->flags &= ~NSVG_FLAGS_VISIBLE;
	}
	for (int i = 0; i < part->count; i++) {
		part->shapes[i]->flags |= NSVG_FLAGS_VISIBLE;
	}
	nsvgRasterize(rasterizer, image, -part->bounds[0] * scale, -part->bounds[1] * scale,
		scale, pixels, width, height, width * 4);
	for (NSVGshape *shape = image->shapes; shape; shape = shape->next) {
		shape->flags |= NSVG_FLAGS_VISIBLE;
	}
	int clip = clip_width < 0 ? width : (int)lround(clip_width);
	canvas_blit_rgba(canvas, (int)lround(x), (int)lround(y), pixels, width, height, invert, clip);
	free(pixels);
}

double battery_draw(struct canvas *canvas, double right, double top, double height,
		const struct sb_battery_state *state, bool dark) {
	struct part *base = find_part("base");
	if (!base) {
		return 0;
	}
	double scale = height / part_height(base);
	double width = part_width(base) * scale;
	double left = right - width;

	/* The empty battery; the sheet draws it dark, so flip it for a dark bar.
	 * Without a battery driver only this outline shows: level unknown. */
	draw_part(canvas, base, left, top, scale, dark, -1);
	if (!state->present) {
		return width;
	}

	const char *fill = state->charging ? "green_overlay" :
		state->saver ? "yellow_overlay" :
		state->level <= 20 ? "red_overlay" :
		dark ? "white_overlay" : "black_overlay";
	struct part *overlay = find_part(fill);
	if (overlay && overlay->count > 0) {
		/* The first shape is the bar; the cap only fills at 100%. */
		const float *bar = overlay->shapes[0]->bounds;
		double bar_width = (bar[2] - bar[0]) * scale;
		double clip = state->level >= 100 ? -1 : bar_width * state->level / 100.0;
		draw_part(canvas, overlay, left, top, scale, false, clip);
	}

	/* The percentage, centred on the bar, in the colour opposite the fill. */
	const char *set = dark ? "percents_black" : "percents_white";
	char digits[8], name[64];
	snprintf(digits, sizeof(digits), "%d", state->level);
	snprintf(name, sizeof(name), "%s/0", set);
	struct part *zero = find_part(name);
	if (zero) {
		double glyph_height = height * 0.55;
		double glyph_scale = glyph_height / part_height(zero);
		double gap = glyph_height * 0.08;
		double total = 0;
		for (const char *d = digits; *d; d++) {
			snprintf(name, sizeof(name), "%s/%c", set, *d);
			struct part *glyph = find_part(name);
			total += glyph ? part_width(glyph) * glyph_scale + gap : 0;
		}
		total -= gap;
		double bar_width = overlay && overlay->count ?
			(overlay->shapes[0]->bounds[2] - overlay->shapes[0]->bounds[0]) * scale : width;
		double x = left + (bar_width - total) / 2;
		double baseline = top + (height + glyph_height) / 2;
		for (const char *d = digits; *d; d++) {
			snprintf(name, sizeof(name), "%s/%c", set, *d);
			struct part *glyph = find_part(name);
			if (!glyph) {
				continue;
			}
			draw_part(canvas, glyph, x, baseline - part_height(glyph) * glyph_scale,
				glyph_scale, false, -1);
			x += part_width(glyph) * glyph_scale + gap;
		}
	}

	/* The charging bolt sits left of the battery, at the sheet's proportions. */
	if (state->charging) {
		struct part *bolt = find_part(dark ? "lightning_white" : "lightning");
		if (bolt) {
			double bolt_width = part_width(bolt) * scale;
			double bolt_height = part_height(bolt) * scale;
			left -= bolt_width + height * 0.15;
			draw_part(canvas, bolt, left, top + (height - bolt_height) / 2, scale, false, -1);
		}
	}
	return right - left;
}

void battery_changed(struct sb_battery_state *state, const struct cn_object *object, bool exists) {
	if (strcmp(object->path, CN_NAMESPACE "power/saver") == 0) {
		state->saver = exists && cn_get_bool(object, "enabled", false);
		return;
	}
	if (strcmp(object->path, CN_NAMESPACE "power/battery") != 0) {
		return;
	}
	state->present = exists && cn_get_bool(object, "present", false);
	state->level = (int)cn_get_int(object, "level", 0);
	state->level = state->level < 0 ? 0 : state->level > 100 ? 100 : state->level;
	/* A full battery on the charger still shows the bolt. */
	state->charging = cn_get_bool(object, "charging", false) ||
		(cn_get_bool(object, "plugged", false) && strcmp(cn_get(object, "status", ""), "full") == 0);
}
