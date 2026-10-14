#define _GNU_SOURCE
#include <ft2build.h>
#include FT_FREETYPE_H
#include <math.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "canvas.h"
#include "springboard.h"

static FT_Library library;
static FT_Face faces[2];

static void buffer_release(void *data, struct wl_buffer *buffer) {
	struct canvas *canvas = data;
	canvas->busy = false;
	if (canvas->destroy_on_release) {
		canvas_release(canvas);
	}
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

bool canvas_font_init(const char *regular, const char *bold) {
	if (FT_Init_FreeType(&library) ||
			FT_New_Face(library, regular, 0, &faces[0]) ||
			FT_New_Face(library, bold, 0, &faces[1])) {
		sb_log("Cannot load fonts %s, %s", regular, bold);
		return false;
	}
	return true;
}

struct canvas *canvas_create(struct wl_shm *shm, int width, int height) {
	struct canvas *canvas = calloc(1, sizeof(*canvas));
	canvas->width = width;
	canvas->height = height;
	canvas->size = (size_t)width * height * 4;
	int fd = memfd_create("starry-canvas", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, canvas->size) < 0) {
		sb_log("Cannot allocate %dx%d canvas", width, height);
		abort();
	}
	canvas->pixels = mmap(NULL, canvas->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, canvas->size);
	canvas->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(canvas->buffer, &buffer_listener, canvas);
	return canvas;
}

void canvas_release(struct canvas *canvas) {
	if (canvas->busy) {
		canvas->destroy_on_release = true;
		return;
	}
	wl_buffer_destroy(canvas->buffer);
	munmap(canvas->pixels, canvas->size);
	free(canvas);
}

static uint32_t premultiply(uint32_t argb, double coverage) {
	double a = ((argb >> 24) & 0xff) / 255.0 * coverage;
	uint32_t r = lround(((argb >> 16) & 0xff) * a);
	uint32_t g = lround(((argb >> 8) & 0xff) * a);
	uint32_t b = lround((argb & 0xff) * a);
	return (uint32_t)lround(a * 255) << 24 | r << 16 | g << 8 | b;
}

static void blend(uint32_t *dst, uint32_t src) {
	uint32_t inv = 255 - (src >> 24);
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		uint32_t s = (src >> shift) & 0xff, d = (*dst >> shift) & 0xff;
		out |= (s + (d * inv + 127) / 255) << shift;
	}
	*dst = out;
}

void canvas_vertical_gradient(struct canvas *canvas, uint32_t top, uint32_t bottom) {
	for (int y = 0; y < canvas->height; y++) {
		double t = (double)y / (canvas->height - 1);
		uint32_t argb = 0;
		for (int shift = 0; shift < 32; shift += 8) {
			double a = (top >> shift) & 0xff, b = (bottom >> shift) & 0xff;
			argb |= (uint32_t)lround(a + (b - a) * t) << shift;
		}
		uint32_t value = premultiply(argb, 1);
		for (int x = 0; x < canvas->width; x++) {
			canvas->pixels[y * canvas->width + x] = value;
		}
	}
}

/* Antialiased by the signed distance to the rounded rectangle at each pixel centre. */
void canvas_round_rect(struct canvas *canvas, double x, double y,
		double width, double height, double radius, uint32_t argb) {
	int x0 = fmax(0, floor(x)), x1 = fmin(canvas->width, ceil(x + width));
	int y0 = fmax(0, floor(y)), y1 = fmin(canvas->height, ceil(y + height));
	double cx = x + width / 2, cy = y + height / 2;
	double hx = width / 2 - radius, hy = height / 2 - radius;
	for (int py = y0; py < y1; py++) {
		for (int px = x0; px < x1; px++) {
			double dx = fmax(fabs(px + 0.5 - cx) - hx, 0);
			double dy = fmax(fabs(py + 0.5 - cy) - hy, 0);
			double coverage = fmin(1, fmax(0, radius - hypot(dx, dy) + 0.5));
			if (coverage > 0) {
				blend(&canvas->pixels[py * canvas->width + px], premultiply(argb, coverage));
			}
		}
	}
}

static uint32_t next_codepoint(const char **text) {
	const unsigned char *s = (const unsigned char *)*text;
	uint32_t c = *s++;
	int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
	if (extra) {
		c &= 0x3f >> extra;
	}
	while (extra-- && (*s & 0xc0) == 0x80) {
		c = c << 6 | (*s++ & 0x3f);
	}
	*text = (const char *)s;
	return c;
}

double canvas_text_width(bool bold, double size, const char *text) {
	FT_Face face = faces[bold];
	FT_Set_Pixel_Sizes(face, 0, lround(size));
	double width = 0;
	while (*text) {
		if (!FT_Load_Char(face, next_codepoint(&text), FT_LOAD_DEFAULT)) {
			width += face->glyph->advance.x / 64.0;
		}
	}
	return width;
}

void canvas_text(struct canvas *canvas, bool bold, double size,
		double x, double baseline, uint32_t argb, const char *text) {
	FT_Face face = faces[bold];
	FT_Set_Pixel_Sizes(face, 0, lround(size));
	double pen = x;
	while (*text) {
		if (FT_Load_Char(face, next_codepoint(&text), FT_LOAD_RENDER)) {
			continue;
		}
		FT_GlyphSlot glyph = face->glyph;
		int left = lround(pen) + glyph->bitmap_left;
		int top = lround(baseline) - glyph->bitmap_top;
		for (unsigned row = 0; row < glyph->bitmap.rows; row++) {
			int py = top + (int)row;
			if (py < 0 || py >= canvas->height) {
				continue;
			}
			for (unsigned col = 0; col < glyph->bitmap.width; col++) {
				int px = left + (int)col;
				uint8_t value = glyph->bitmap.buffer[row * glyph->bitmap.pitch + col];
				if (px >= 0 && px < canvas->width && value) {
					blend(&canvas->pixels[py * canvas->width + px],
						premultiply(argb, value / 255.0));
				}
			}
		}
		pen += glyph->advance.x / 64.0;
	}
}
