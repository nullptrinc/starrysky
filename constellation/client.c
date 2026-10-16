/* SPDX-License-Identifier: MIT */
/*
 * The client side of the constellationd protocol. One SOCK_SEQPACKET message
 * per request or event, as text: requests are "verb path\n[attribute lines]",
 * replies start with "=", change events with "!". Events that arrive while a
 * request waits for its reply are queued for the next cn_dispatch.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "constellation.h"

#define REPLY_TIMEOUT_MSEC 2000

struct event {
	struct event *next;
	char text[];
};

struct cn_client {
	int fd;
	struct event *queue, **queue_tail;
	char (*subscriptions)[CN_PATH_MAX];
	int subscription_count;
	char buffer[CN_MESSAGE_MAX + 1];
};

static const char *socket_path(void) {
	const char *path = getenv("CONSTELLATION_SOCKET");
	return path && path[0] ? path : CN_SOCKET;
}

static int open_socket(void) {
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path());
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

struct cn_client *cn_connect(void) {
	int fd = open_socket();
	if (fd < 0) {
		return NULL;
	}
	struct cn_client *client = calloc(1, sizeof(*client));
	if (!client) {
		close(fd);
		return NULL;
	}
	client->fd = fd;
	client->queue_tail = &client->queue;
	return client;
}

static void drop_queue(struct cn_client *client) {
	while (client->queue) {
		struct event *next = client->queue->next;
		free(client->queue);
		client->queue = next;
	}
	client->queue_tail = &client->queue;
}

void cn_disconnect(struct cn_client *client) {
	if (!client) {
		return;
	}
	if (client->fd >= 0) {
		close(client->fd);
	}
	drop_queue(client);
	free(client->subscriptions);
	free(client);
}

int cn_fd(const struct cn_client *client) {
	return client->fd;
}

/* Receives one message into client->buffer: its length, 0 when none is waiting, -1 when gone. */
static int receive(struct cn_client *client) {
	ssize_t length = recv(client->fd, client->buffer, CN_MESSAGE_MAX, MSG_TRUNC);
	if (length < 0) {
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	}
	if (length == 0 || length > CN_MESSAGE_MAX) {
		return -1; /* closed, or a message larger than the protocol allows */
	}
	client->buffer[length] = 0;
	return (int)length;
}

static void enqueue(struct cn_client *client, const char *text, size_t length) {
	struct event *event = malloc(sizeof(*event) + length + 1);
	if (!event) {
		return;
	}
	memcpy(event->text, text, length + 1);
	event->next = NULL;
	*client->queue_tail = event;
	client->queue_tail = &event->next;
}

/* Sends a request and waits for its reply, left in client->buffer. */
static bool request(struct cn_client *client, const char *text, size_t length) {
	if (client->fd < 0) {
		errno = ENOTCONN;
		return false;
	}
	ssize_t sent;
	do {
		sent = send(client->fd, text, length, MSG_NOSIGNAL);
		if (sent < 0 && errno == EAGAIN) {
			struct pollfd pfd = { .fd = client->fd, .events = POLLOUT };
			if (poll(&pfd, 1, REPLY_TIMEOUT_MSEC) <= 0) {
				errno = ETIMEDOUT;
				return false;
			}
		}
	} while (sent < 0 && (errno == EAGAIN || errno == EINTR));
	if (sent < 0) {
		return false;
	}
	for (;;) {
		int received = receive(client);
		if (received < 0) {
			errno = ECONNRESET;
			return false;
		}
		if (received == 0) {
			struct pollfd pfd = { .fd = client->fd, .events = POLLIN };
			if (poll(&pfd, 1, REPLY_TIMEOUT_MSEC) <= 0) {
				errno = ETIMEDOUT;
				return false;
			}
			continue;
		}
		if (client->buffer[0] == '=') {
			return true;
		}
		enqueue(client, client->buffer, (size_t)received);
	}
}

static bool simple_request(struct cn_client *client, const char *verb, const char *path) {
	char text[CN_PATH_MAX + 16];
	int length = snprintf(text, sizeof(text), "%s %s\n", verb, path);
	if (!request(client, text, (size_t)length)) {
		return false;
	}
	if (strncmp(client->buffer, "=ok", 3) != 0) {
		errno = strstr(client->buffer, "owned") ? EPERM : EINVAL;
		return false;
	}
	return true;
}

bool cn_publish(struct cn_client *client, const struct cn_object *object) {
	if (!cn_valid_path(object->path, false)) {
		errno = EINVAL;
		return false;
	}
	char text[CN_MESSAGE_MAX];
	int length = snprintf(text, sizeof(text), "set %s\n", object->path);
	int attrs = cn_format_attrs(object, text + length, (int)sizeof(text) - length);
	if (attrs < 0) {
		errno = EMSGSIZE;
		return false;
	}
	if (!request(client, text, (size_t)(length + attrs))) {
		return false;
	}
	if (strncmp(client->buffer, "=ok", 3) != 0) {
		errno = strstr(client->buffer, "owned") ? EPERM : EINVAL;
		return false;
	}
	return true;
}

bool cn_delete(struct cn_client *client, const char *path) {
	return simple_request(client, "del", path);
}

/* Splits "verb path\nattributes" into the object. */
static bool parse_object(const char *text, struct cn_object *object) {
	const char *space = strchr(text, ' ');
	if (!space) {
		return false;
	}
	const char *end = strchr(space + 1, '\n');
	size_t length = end ? (size_t)(end - space - 1) : strlen(space + 1);
	if (length >= CN_PATH_MAX) {
		return false;
	}
	memcpy(object->path, space + 1, length);
	object->path[length] = 0;
	object->count = 0;
	return !end || cn_parse_attrs(end + 1, object);
}

int cn_fetch(struct cn_client *client, const char *path, struct cn_object *object) {
	char text[CN_PATH_MAX + 16];
	int length = snprintf(text, sizeof(text), "get %s\n", path);
	if (!request(client, text, (size_t)length)) {
		return -1;
	}
	if (strncmp(client->buffer, "=obj ", 5) == 0) {
		return parse_object(client->buffer, object) ? 1 : -1;
	}
	if (strncmp(client->buffer, "=none ", 6) == 0) {
		cn_object_init(object, path);
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int cn_list(struct cn_client *client, const char *prefix,
		void (*each)(const char *path, void *data), void *data) {
	char text[CN_PATH_MAX + 16];
	int length = snprintf(text, sizeof(text), "list %s\n", prefix);
	if (!request(client, text, (size_t)length)) {
		return -1;
	}
	if (strncmp(client->buffer, "=list", 5) != 0) {
		errno = EINVAL;
		return -1;
	}
	int count = 0;
	for (char *line = strchr(client->buffer, '\n'); line && line[1]; count++) {
		char *path = line + 1;
		line = strchr(path, '\n');
		if (line) {
			*line = 0;
		}
		each(path, data);
		if (line) {
			*line = '\n';
		}
	}
	return count;
}

bool cn_subscribe(struct cn_client *client, const char *path) {
	if (!simple_request(client, "sub", path)) {
		return false;
	}
	for (int i = 0; i < client->subscription_count; i++) {
		if (strcmp(client->subscriptions[i], path) == 0) {
			return true;
		}
	}
	void *grown = realloc(client->subscriptions,
		(size_t)(client->subscription_count + 1) * sizeof(*client->subscriptions));
	if (grown) {
		client->subscriptions = grown;
		snprintf(client->subscriptions[client->subscription_count++], CN_PATH_MAX, "%s", path);
	}
	return true;
}

bool cn_unsubscribe(struct cn_client *client, const char *path) {
	for (int i = 0; i < client->subscription_count; i++) {
		if (strcmp(client->subscriptions[i], path) == 0) {
			memmove(client->subscriptions[i], client->subscriptions[i + 1],
				(size_t)(client->subscription_count - i - 1) * sizeof(*client->subscriptions));
			client->subscription_count--;
			break;
		}
	}
	return simple_request(client, "unsub", path);
}

bool cn_reconnect(struct cn_client *client) {
	if (client->fd >= 0) {
		close(client->fd);
	}
	drop_queue(client);
	client->fd = open_socket();
	if (client->fd < 0) {
		return false;
	}
	for (int i = 0; i < client->subscription_count; i++) {
		if (!simple_request(client, "sub", client->subscriptions[i])) {
			close(client->fd);
			client->fd = -1;
			return false;
		}
	}
	return true;
}

static void deliver(const char *text, cn_changed_fn changed, void *data) {
	struct cn_object object;
	if (strncmp(text, "!obj ", 5) == 0 && parse_object(text, &object)) {
		changed(&object, true, data);
	} else if (strncmp(text, "!none ", 6) == 0 && parse_object(text, &object)) {
		changed(&object, false, data);
	}
}

bool cn_dispatch(struct cn_client *client, cn_changed_fn changed, void *data) {
	if (client->fd < 0) {
		return false;
	}
	for (;;) {
		/* Handlers may make requests, which can queue more events. */
		while (client->queue) {
			struct event *event = client->queue;
			client->queue = event->next;
			if (!client->queue) {
				client->queue_tail = &client->queue;
			}
			deliver(event->text, changed, data);
			free(event);
		}
		int received = receive(client);
		if (received < 0) {
			close(client->fd);
			client->fd = -1;
			return false;
		}
		if (received == 0) {
			return true;
		}
		enqueue(client, client->buffer, (size_t)received);
	}
}
