/* SPDX-License-Identifier: MIT */
#ifndef STARRY_PLUGIN_H
#define STARRY_PLUGIN_H

/*
 * Starry starboard plugin ABI.
 *
 * A plugin is a shared object in the starboard plugin directory that
 * exports `const struct starry_plugin starry_plugin`. It runs inside the
 * starboard process: a crash restarts starboard, never the compositor
 * or the running applications.
 *
 * Compatibility: the host refuses plugins whose `abi` differs from
 * STARRY_PLUGIN_ABI. Structures only ever grow at the end; `size` fields let
 * either side detect which members the other knows about.
 */

#include <stdbool.h>
#include <stddef.h>

#define STARRY_PLUGIN_ABI 1

struct starry_host;

enum starry_status_side {
	STARRY_STATUS_LEFT,
	STARRY_STATUS_RIGHT,
};

struct starry_status_item {
	size_t size;                  /* sizeof(struct starry_status_item) */
	const char *id;
	enum starry_status_side side;
	int refresh_msec;             /* how often text() is polled; 0 = only on request */
	/* Write the text to show; leave it empty to hide the item. */
	void (*text)(void *data, char *buffer, size_t size);
	void *data;
};

struct starry_host_api {
	size_t size;                  /* sizeof(struct starry_host_api) */
	bool (*add_status_item)(struct starry_host *host, const struct starry_status_item *item);
	/* Ask for status items to be polled again now. */
	void (*refresh_status)(struct starry_host *host);
	void (*log)(struct starry_host *host, const char *format, ...)
		__attribute__((format(printf, 2, 3)));
};

struct starry_plugin {
	int abi;                      /* STARRY_PLUGIN_ABI */
	const char *name;
	bool (*init)(struct starry_host *host, const struct starry_host_api *api);
	void (*fini)(void);
};

#endif
