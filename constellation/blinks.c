/* SPDX-License-Identifier: MIT */
/* blinks: look at and poke the /blinks namespace from a shell. */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "constellation.h"

static void usage(void) {
	fprintf(stderr,
		"usage: blinks ls [PREFIX]\n"
		"       blinks get PATH...\n"
		"       blinks set PATH name:type:value... (type n, b or empty; -name removes)\n"
		"       blinks del PATH\n"
		"       blinks watch PATH...  (a PATH ending in / watches everything under it)\n");
	exit(2);
}

static void print_object(const struct cn_object *object) {
	char text[CN_MESSAGE_MAX];
	printf("@%s\n", object->path);
	if (cn_format_attrs(object, text, sizeof(text)) > 0) {
		fputs(text, stdout);
	}
}

static void print_path(const char *path, void *data) {
	puts(path);
}

static void print_change(const struct cn_object *object, bool exists, void *data) {
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	struct tm local;
	localtime_r(&now.tv_sec, &local);
	char stamp[16];
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
	printf("# %s.%03ld\n", stamp, now.tv_nsec / 1000000);
	if (exists) {
		print_object(object);
	} else {
		printf("@%s (none)\n", object->path);
	}
	fflush(stdout);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		usage();
	}
	const char *command = argv[1];
	struct cn_client *client = cn_connect();
	if (!client) {
		perror("blinks: cannot reach constellationd");
		return 1;
	}
	int status = 0;
	if (strcmp(command, "ls") == 0) {
		if (cn_list(client, argc > 2 ? argv[2] : CN_NAMESPACE, print_path, NULL) < 0) {
			perror("blinks: ls");
			status = 1;
		}
	} else if (strcmp(command, "get") == 0 && argc > 2) {
		for (int i = 2; i < argc; i++) {
			struct cn_object object;
			int found = cn_fetch(client, argv[i], &object);
			if (found > 0) {
				print_object(&object);
			} else {
				fprintf(stderr, "blinks: %s: %s\n", argv[i], found == 0 ? "no such object" : "invalid");
				status = 1;
			}
		}
	} else if (strcmp(command, "set") == 0 && argc > 2) {
		struct cn_object object;
		cn_object_init(&object, argv[2]);
		/* Each argument is one attribute line. */
		char lines[CN_MESSAGE_MAX] = "";
		int length = 0;
		for (int i = 3; i < argc; i++) {
			length += snprintf(lines + length, sizeof(lines) - length, "%s\n", argv[i]);
		}
		if (length >= (int)sizeof(lines) || !cn_parse_attrs(lines, &object)) {
			fprintf(stderr, "blinks: bad attribute\n");
			status = 2;
		} else if (!cn_publish(client, &object)) {
			perror("blinks: set");
			status = 1;
		}
	} else if (strcmp(command, "del") == 0 && argc == 3) {
		if (!cn_delete(client, argv[2])) {
			perror("blinks: del");
			status = 1;
		}
	} else if (strcmp(command, "watch") == 0 && argc > 2) {
		for (int i = 2; i < argc; i++) {
			if (!cn_subscribe(client, argv[i])) {
				fprintf(stderr, "blinks: cannot watch %s\n", argv[i]);
				return 1;
			}
		}
		for (;;) {
			if (!cn_dispatch(client, print_change, NULL)) {
				fprintf(stderr, "blinks: constellationd went away\n");
				return 1;
			}
			struct pollfd pfd = { .fd = cn_fd(client), .events = POLLIN };
			poll(&pfd, 1, -1);
		}
	} else {
		usage();
	}
	cn_disconnect(client);
	return status;
}
