/* SPDX-License-Identifier: MIT */
/*
 * starry-shot: screenshots of the whole screen through wlr-screencopy.
 *
 *     starry-shot shot.ppm                     one frame
 *     starry-shot -n 20 -i 50 frame-%02d.ppm   twenty frames, 50 ms apart
 *
 * Writes binary PPM. starry-compositor offers screencopy only to this
 * program run as root (and to starboard).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client-protocol.h"

/* DRM fourcc codes that wl_shm also uses for these two. */
#define FORMAT_XBGR8888 0x34324258
#define FORMAT_ABGR8888 0x34324241

struct shot {
	struct wl_shm *shm;
	struct wl_output *output;
	struct zwlr_screencopy_manager_v1 *manager;
	uint32_t manager_version;

	/* the frame in flight */
	bool have_format, copying, done, failed, y_invert;
	uint32_t format, width, height, stride;
	struct wl_buffer *buffer;
	void *pixels;
	size_t size;
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct shot *shot = data;
	if (strcmp(interface, wl_shm_interface.name) == 0) {
		shot->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0 && !shot->output) {
		shot->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		shot->manager_version = version < 3 ? version : 3;
		shot->manager = wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, shot->manager_version);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static bool supported(uint32_t format) {
	return format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888 ||
		format == FORMAT_XBGR8888 || format == FORMAT_ABGR8888;
}

/* Allocates a shared-memory buffer for the announced format and copies into it. */
static void start_copy(struct shot *shot, struct zwlr_screencopy_frame_v1 *frame) {
	if (shot->copying) {
		return;
	}
	if (!shot->have_format) {
		fprintf(stderr, "starry-shot: no supported pixel format offered\n");
		shot->failed = shot->done = true;
		return;
	}
	shot->size = (size_t)shot->stride * shot->height;
	int fd = memfd_create("starry-shot", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)shot->size) < 0) {
		perror("starry-shot: memfd");
		shot->failed = shot->done = true;
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	shot->pixels = mmap(NULL, shot->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shot->shm, fd, (int32_t)shot->size);
	shot->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)shot->width,
		(int32_t)shot->height, (int32_t)shot->stride, shot->format);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (shot->pixels == MAP_FAILED) {
		perror("starry-shot: mmap");
		shot->pixels = NULL;
		shot->failed = shot->done = true;
		return;
	}
	zwlr_screencopy_frame_v1_copy(frame, shot->buffer);
	shot->copying = true;
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
		uint32_t width, uint32_t height, uint32_t stride) {
	struct shot *shot = data;
	if (!shot->have_format && supported(format)) {
		shot->have_format = true;
		shot->format = format;
		shot->width = width;
		shot->height = height;
		shot->stride = stride;
	}
	if (shot->manager_version < 3) {
		start_copy(shot, frame); /* older compositors send no buffer_done */
	}
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
	struct shot *shot = data;
	shot->y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct shot *shot = data;
	shot->done = true;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	struct shot *shot = data;
	fprintf(stderr, "starry-shot: the compositor could not copy the screen\n");
	shot->failed = shot->done = true;
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
	start_copy(data, frame);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_buffer,
	.flags = frame_flags,
	.ready = frame_ready,
	.failed = frame_failed,
	.damage = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done = frame_buffer_done,
};

static bool write_ppm(const struct shot *shot, const char *path) {
	FILE *file = fopen(path, "wb");
	if (!file) {
		fprintf(stderr, "starry-shot: %s: %s\n", path, strerror(errno));
		return false;
	}
	fprintf(file, "P6\n%u %u\n255\n", shot->width, shot->height);
	/* XRGB/ARGB are B, G, R, A in memory; XBGR/ABGR are R, G, B, A. */
	bool bgr = shot->format == WL_SHM_FORMAT_XRGB8888 || shot->format == WL_SHM_FORMAT_ARGB8888;
	unsigned char *row = malloc((size_t)shot->width * 3);
	for (uint32_t y = 0; row && y < shot->height; y++) {
		uint32_t source = shot->y_invert ? shot->height - 1 - y : y;
		const unsigned char *pixel = (const unsigned char *)shot->pixels + (size_t)source * shot->stride;
		for (uint32_t x = 0; x < shot->width; x++, pixel += 4) {
			row[x * 3] = bgr ? pixel[2] : pixel[0];
			row[x * 3 + 1] = pixel[1];
			row[x * 3 + 2] = bgr ? pixel[0] : pixel[2];
		}
		fwrite(row, 3, shot->width, file);
	}
	free(row);
	bool ok = row != NULL && !ferror(file);
	return fclose(file) == 0 && ok;
}

static bool capture(struct wl_display *display, struct shot *shot, const char *path) {
	shot->have_format = shot->copying = shot->done = shot->failed = shot->y_invert = false;
	struct zwlr_screencopy_frame_v1 *frame =
		zwlr_screencopy_manager_v1_capture_output(shot->manager, 0, shot->output);
	zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, shot);
	while (!shot->done && wl_display_dispatch(display) >= 0) {
	}
	bool ok = shot->done && !shot->failed && shot->pixels && write_ppm(shot, path);
	zwlr_screencopy_frame_v1_destroy(frame);
	if (shot->buffer) {
		wl_buffer_destroy(shot->buffer);
		shot->buffer = NULL;
	}
	if (shot->pixels) {
		munmap(shot->pixels, shot->size);
		shot->pixels = NULL;
	}
	return ok;
}

/* A pattern may hold exactly one integer conversion, like %d or %03d. */
static bool valid_pattern(const char *pattern) {
	int conversions = 0;
	for (const char *c = pattern; *c; c++) {
		if (*c != '%') {
			continue;
		}
		if (c[1] == '%') {
			c++;
			continue;
		}
		c++;
		c += strspn(c, "0123456789");
		if (*c != 'd' && *c != 'i') {
			return false;
		}
		conversions++;
	}
	return conversions <= 1;
}

static void usage(void) {
	fprintf(stderr, "usage: starry-shot [-n COUNT] [-i MSEC] OUT.ppm\n"
		"       with -n, OUT holds a printf number, e.g. frame-%%02d.ppm\n");
	exit(2);
}

int main(int argc, char **argv) {
	int count = 1, interval = 0, option;
	while ((option = getopt(argc, argv, "n:i:")) != -1) {
		if (option == 'n') {
			count = atoi(optarg);
		} else if (option == 'i') {
			interval = atoi(optarg);
		} else {
			usage();
		}
	}
	if (optind != argc - 1 || count < 1 || interval < 0) {
		usage();
	}
	const char *pattern = argv[optind];
	if (!valid_pattern(pattern) || (count > 1 && !strchr(pattern, '%'))) {
		usage();
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "starry-shot: cannot connect to the compositor "
			"(set XDG_RUNTIME_DIR=/run/starry WAYLAND_DISPLAY=wayland-0)\n");
		return 1;
	}
	struct shot shot = { 0 };
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &shot);
	wl_display_roundtrip(display);
	if (!shot.manager || !shot.shm || !shot.output) {
		fprintf(stderr, "starry-shot: the compositor offers no screencopy "
			"(only %s, run as root, may capture)\n", "starry-shot");
		return 1;
	}
	for (int i = 0; i < count; i++) {
		char path[4096];
		snprintf(path, sizeof(path), pattern, i);
		if (!capture(display, &shot, path)) {
			return 1;
		}
		printf("%s\n", path);
		if (interval > 0 && i + 1 < count) {
			struct timespec pause = { interval / 1000, (long)(interval % 1000) * 1000000 };
			nanosleep(&pause, NULL);
		}
	}
	wl_display_disconnect(display);
	return 0;
}
