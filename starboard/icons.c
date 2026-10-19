/* SPDX-License-Identifier: MIT */
/*
 * App icons: PNGs from installed packages (256×256 per appearance), scaled
 * to the home grid by area averaging. The tinted appearance keeps only the
 * brightness of its pixels and shows it in the system accent, as iOS 26
 * does with its tint colour.
 */
#include <math.h>
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include "canvas.h"
#include "starboard.h"

uint8_t *icon_load(const char *path, int *width, int *height) {
	png_image image;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&image, path)) {
		sb_log("Cannot read icon %s: %s", path, image.message);
		return NULL;
	}
	if (image.width == 0 || image.height == 0 || image.width > 1024 || image.height > 1024) {
		sb_log("Icon %s is %ux%u; at most 1024 square", path, image.width, image.height);
		png_image_free(&image);
		return NULL;
	}
	image.format = PNG_FORMAT_RGBA;
	uint8_t *pixels = malloc(PNG_IMAGE_SIZE(image));
	if (!pixels || !png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
		sb_log("Cannot decode icon %s: %s", path, image.message);
		free(pixels);
		png_image_free(&image);
		return NULL;
	}
	*width = (int)image.width;
	*height = (int)image.height;
	return pixels;
}

void icon_draw(struct canvas *canvas, const uint8_t *rgba, int width, int height, int size,
		bool tint, uint32_t accent) {
	uint8_t *out = calloc((size_t)size * size, 4);
	if (!out) {
		return;
	}
	/* The tinted appearance is drawn dim. Stretch it so its 97th percentile of
	 * brightness becomes full: the very brightest pixels are the thin glass
	 * rim, and stretching to them would leave the glyph itself dark. */
	double brightest = 1;
	if (tint) {
		unsigned histogram[256] = { 0 }, opaque = 0;
		for (size_t i = 0; i < (size_t)width * height; i++) {
			const uint8_t *p = rgba + i * 4;
			if (p[3] > 128) {
				histogram[(int)lround(0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2])]++;
				opaque++;
			}
		}
		unsigned below = 0;
		int level = 255;
		for (int i = 0; i < 256; i++) {
			below += histogram[i];
			if (below >= opaque * 97 / 100) {
				level = i;
				break;
			}
		}
		brightest = level >= 13 ? level / 255.0 : 1;
	}
	double sx = (double)width / size, sy = (double)height / size;
	float tint_rgb[3] = { (accent >> 16 & 0xff) / 255.0f, (accent >> 8 & 0xff) / 255.0f,
		(accent & 0xff) / 255.0f };
	for (int oy = 0; oy < size; oy++) {
		double y0 = oy * sy, y1 = (oy + 1) * sy;
		for (int ox = 0; ox < size; ox++) {
			double x0 = ox * sx, x1 = (ox + 1) * sx;
			/* Average the source pixels under this one, weighted by overlap,
			 * in premultiplied colour so transparent edges do not darken. */
			double sum[4] = { 0, 0, 0, 0 }, area = 0;
			for (int y = (int)y0; y < (int)ceil(y1) && y < height; y++) {
				double wy = fmin(y + 1, y1) - fmax(y, y0);
				for (int x = (int)x0; x < (int)ceil(x1) && x < width; x++) {
					double w = wy * (fmin(x + 1, x1) - fmax(x, x0));
					const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
					double a = p[3] / 255.0;
					sum[0] += p[0] * a * w;
					sum[1] += p[1] * a * w;
					sum[2] += p[2] * a * w;
					sum[3] += p[3] * w;
					area += w;
				}
			}
			uint8_t *q = out + ((size_t)oy * size + ox) * 4;
			double alpha = area > 0 ? sum[3] / area : 0;
			if (alpha < 0.5) {
				continue;
			}
			double r = sum[0] / area * 255 / alpha, g = sum[1] / area * 255 / alpha;
			double b = sum[2] / area * 255 / alpha;
			if (tint) {
				double light = fmin(1, (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255 / brightest);
				r = tint_rgb[0] * 255 * light;
				g = tint_rgb[1] * 255 * light;
				b = tint_rgb[2] * 255 * light;
			}
			q[0] = (uint8_t)fmin(255, lround(r));
			q[1] = (uint8_t)fmin(255, lround(g));
			q[2] = (uint8_t)fmin(255, lround(b));
			q[3] = (uint8_t)lround(alpha);
		}
	}
	canvas_blit_rgba(canvas, 0, 0, out, size, size, false, size);
	free(out);
}
