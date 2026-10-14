/* SPDX-License-Identifier: MIT */
#ifndef STARRY_CANVAS_H
#define STARRY_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_shm;
struct wl_buffer;

/* CPU-drawn ARGB8888 (premultiplied) pixels shared with the compositor through wl_shm. */
struct canvas {
	uint32_t *pixels;
	int width, height;
	size_t size;
	struct wl_buffer *buffer;
	bool busy;            /* attached and not yet released */
	bool destroy_on_release;
};

bool canvas_font_init(const char *regular, const char *bold);
struct canvas *canvas_create(struct wl_shm *shm, int width, int height);
/* Frees now, or once the compositor releases the buffer. */
void canvas_release(struct canvas *canvas);
void canvas_vertical_gradient(struct canvas *canvas, uint32_t top, uint32_t bottom);
void canvas_round_rect(struct canvas *canvas, double x, double y,
	double width, double height, double radius, uint32_t argb);
double canvas_text_width(bool bold, double size, const char *text);
void canvas_text(struct canvas *canvas, bool bold, double size,
	double x, double baseline, uint32_t argb, const char *text);

#endif
