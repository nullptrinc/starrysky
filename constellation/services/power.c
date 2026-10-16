/* SPDX-License-Identifier: MIT */
/*
 * constellation-power: publishes the battery as /blinks/power/battery.
 *
 *     present:b   level:n (%)   status:: charging | discharging | full | not-charging | unknown
 *     charging:b  plugged:b     source:: the online supply's type, e.g. usb-pd
 *     voltage:n (V)  current:n (A, the kernel's sign)  temperature:n (°C)  health::
 *
 * Reads /sys/class/power_supply on every power_supply uevent and every 10 s.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../constellation.h"

#define SUPPLIES "/sys/class/power_supply"
#define REFRESH_MSEC 10000
#define RETRY_MSEC 2000

static volatile sig_atomic_t running = 1;

static bool read_value(const char *supply, const char *name, char *buffer, size_t size) {
	char path[256];
	snprintf(path, sizeof(path), SUPPLIES "/%s/%s", supply, name);
	FILE *file = fopen(path, "r");
	if (!file) {
		return false;
	}
	bool ok = fgets(buffer, (int)size, file) != NULL;
	fclose(file);
	buffer[strcspn(buffer, "\n")] = 0;
	return ok && buffer[0];
}

/* "Not charging" -> "not-charging", "USB_PD" -> "usb-pd". */
static void to_token(char *text) {
	for (char *c = text; *c; c++) {
		*c = *c == ' ' || *c == '_' ? '-' : (char)tolower((unsigned char)*c);
	}
}

static void set_scaled(struct cn_object *object, const char *name, const char *supply,
		const char *file, double scale) {
	char value[64];
	if (read_value(supply, file, value, sizeof(value))) {
		cn_set_number(object, name, strtod(value, NULL) * scale);
	} else {
		cn_set_removed(object, name);
	}
}

static void set_token(struct cn_object *object, const char *name, const char *supply,
		const char *file) {
	char value[64];
	if (read_value(supply, file, value, sizeof(value))) {
		to_token(value);
		cn_set_string(object, name, value);
	} else {
		cn_set_removed(object, name);
	}
}

/* The first online supply that is not a battery, if any. */
static bool find_source(char *type, size_t size) {
	DIR *dir = opendir(SUPPLIES);
	if (!dir) {
		return false;
	}
	bool found = false;
	for (struct dirent *entry; !found && (entry = readdir(dir));) {
		char value[64];
		if (entry->d_name[0] == '.' ||
				(read_value(entry->d_name, "type", value, sizeof(value)) &&
				strcmp(value, "Battery") == 0)) {
			continue;
		}
		if (read_value(entry->d_name, "online", value, sizeof(value)) && atoi(value) > 0) {
			found = true;
			/* USB supplies name their protocol in usb_type, e.g. "Unknown SDP [PD]". */
			if (read_value(entry->d_name, "usb_type", value, sizeof(value)) &&
					strchr(value, '[')) {
				char *start = strchr(value, '[') + 1;
				start[strcspn(start, "]")] = 0;
				snprintf(type, size, "usb-%s", start);
			} else {
				read_value(entry->d_name, "type", value, sizeof(value));
				snprintf(type, size, "%s", value);
			}
			to_token(type);
		}
	}
	closedir(dir);
	return found;
}

static void read_battery(struct cn_object *object) {
	cn_object_init(object, CN_NAMESPACE "power/battery");
	char value[64];
	bool present = read_value("battery", "capacity", value, sizeof(value));
	cn_set_bool(object, "present", present);
	if (present) {
		long level = strtol(value, NULL, 10);
		cn_set_int(object, "level", level < 0 ? 0 : level > 100 ? 100 : level);
	} else {
		cn_set_removed(object, "level");
	}
	set_token(object, "status", "battery", "status");
	set_token(object, "health", "battery", "health");
	cn_set_bool(object, "charging", strcmp(cn_get(object, "status", ""), "charging") == 0);
	char source[64];
	bool plugged = find_source(source, sizeof(source));
	cn_set_bool(object, "plugged", plugged);
	if (plugged) {
		cn_set_string(object, "source", source);
	} else {
		cn_set_removed(object, "source");
	}
	set_scaled(object, "voltage", "battery", "voltage_now", 1e-6);
	set_scaled(object, "current", "battery", "current_now", 1e-6);
	set_scaled(object, "temperature", "battery", "temp", 0.1);
}

static int open_uevents(void) {
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	if (fd >= 0 && bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		close(fd);
		fd = -1;
	}
	if (fd < 0) {
		fprintf(stderr, "constellation-power: no uevents (%s), polling only\n", strerror(errno));
	}
	return fd;
}

/* True when a queued uevent concerns a power supply. */
static bool power_uevent(int fd) {
	bool power = false;
	char buffer[4096];
	ssize_t length;
	while ((length = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
		power |= memmem(buffer, (size_t)length, "SUBSYSTEM=power_supply", 22) != NULL;
	}
	return power;
}

static void on_signal(int number) {
	running = 0;
}

int main(void) {
	struct sigaction action = { .sa_handler = on_signal };
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	int uevents = open_uevents();
	struct cn_client *client = NULL;
	bool connected = false;

	while (running) {
		if (!client) {
			client = cn_connect();
			connected = client != NULL;
		} else if (!connected) {
			connected = cn_reconnect(client);
		}
		if (connected) {
			struct cn_object battery;
			read_battery(&battery);
			if (!cn_publish(client, &battery)) {
				fprintf(stderr, "constellation-power: publish: %s\n", strerror(errno));
				connected = false;
			}
		}
		/* Until the next refresh, or sooner for a power supply uevent. */
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long deadline = now.tv_sec * 1000 + now.tv_nsec / 1000000 +
			(connected ? REFRESH_MSEC : RETRY_MSEC);
		for (;;) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			long timeout = deadline - (now.tv_sec * 1000 + now.tv_nsec / 1000000);
			struct pollfd pfd = { .fd = uevents, .events = POLLIN };
			if (!running || timeout <= 0 ||
					poll(&pfd, uevents >= 0 ? 1 : 0, (int)timeout) <= 0 || power_uevent(uevents)) {
				break;
			}
		}
	}
	cn_disconnect(client);
	return 0;
}
