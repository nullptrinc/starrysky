/* SPDX-License-Identifier: MIT */
/*
 * constellationd: the Constellation broker. Keeps every object of the
 * /blinks namespace in memory and passes changes to subscribers. Requests,
 * one SOCK_SEQPACKET message each:
 *
 *     set PATH\nATTRS    merge attributes ("-name" deletes one)  -> =ok | =err REASON
 *     del PATH           delete an object                         -> =ok | =err REASON
 *     get PATH           read an object                           -> =obj PATH\nATTRS | =none PATH
 *     list PREFIX        paths under a prefix                     -> =list\nPATH...
 *     sub PATH           subscribe; PATH may end in "/"           -> =ok, then events
 *     unsub PATH                                                  -> =ok
 *
 * Events are "!obj PATH\nATTRS" and "!none PATH". Only root may write the
 * app registry (/blinks/apps/) and the battery (/blinks/power/battery). A subscriber that cannot
 * keep up is not queued every state: the broker remembers which objects it
 * still owes and sends their latest state once the socket drains.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "constellation.h"

#define MAX_CLIENTS 256
#define MAX_OBJECTS 4096
#define MAX_SUBSCRIPTIONS 64
#define MAX_PENDING 1024

struct entry {
	struct cn_object object;
	struct client *owner;
};

struct client {
	int fd;
	pid_t pid;
	uid_t uid;
	bool dead;
	char (*subscriptions)[CN_PATH_MAX];
	int subscription_count;
	char (*pending)[CN_PATH_MAX]; /* objects whose latest state is still owed */
	int pending_count;
};

static struct client *clients[MAX_CLIENTS];
static int client_count;
static struct entry **entries;
static int entry_count;
static volatile sig_atomic_t running = 1;
static char message[CN_MESSAGE_MAX + 1];

static void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	fprintf(stderr, "constellationd: ");
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static struct entry *find_entry(const char *path, int *index) {
	for (int i = 0; i < entry_count; i++) {
		if (strcmp(entries[i]->object.path, path) == 0) {
			if (index) {
				*index = i;
			}
			return entries[i];
		}
	}
	return NULL;
}

static bool matches(const char *subscription, const char *path) {
	size_t length = strlen(subscription);
	return subscription[length - 1] == '/' ? strncmp(path, subscription, length) == 0 :
		strcmp(path, subscription) == 0;
}

/* 1 sent, 0 the socket is full, -1 the client is gone. */
static int send_message(struct client *client, const char *text, int length) {
	ssize_t sent;
	do {
		sent = send(client->fd, text, (size_t)length, MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	if (sent >= 0) {
		return 1;
	}
	if (errno == EAGAIN) {
		return 0;
	}
	client->dead = true;
	return -1;
}

static int format_state(char prefix, const char *path, char *buffer, int size) {
	const struct entry *entry = find_entry(path, NULL);
	if (!entry) {
		return snprintf(buffer, size, "%cnone %s\n", prefix, path);
	}
	int length = snprintf(buffer, size, "%cobj %s\n", prefix, path);
	int attrs = cn_format_attrs(&entry->object, buffer + length, size - length);
	return attrs < 0 ? -1 : length + attrs;
}

static int send_state(struct client *client, const char *path) {
	char text[CN_MESSAGE_MAX];
	int length = format_state('!', path, text, sizeof(text));
	return length < 0 ? 1 : send_message(client, text, length);
}

static void owe(struct client *client, const char *path) {
	for (int i = 0; i < client->pending_count; i++) {
		if (strcmp(client->pending[i], path) == 0) {
			return;
		}
	}
	void *grown = client->pending_count < MAX_PENDING ? realloc(client->pending,
		(size_t)(client->pending_count + 1) * sizeof(*client->pending)) : NULL;
	if (!grown) {
		say("pid %d does not read its events; dropping it", client->pid);
		client->dead = true;
		return;
	}
	client->pending = grown;
	snprintf(client->pending[client->pending_count++], CN_PATH_MAX, "%s", path);
}

/* Sends the object's state now, or remembers to once the socket drains. */
static void tell(struct client *client, const char *path) {
	if (client->dead) {
		return;
	}
	if (client->pending_count > 0 || send_state(client, path) == 0) {
		owe(client, path);
	}
}

static void flush(struct client *client) {
	int sent = 0;
	while (sent < client->pending_count && !client->dead) {
		int result = send_state(client, client->pending[sent]);
		if (result == 0) {
			break;
		}
		sent++;
	}
	memmove(client->pending, client->pending + sent,
		(size_t)(client->pending_count - sent) * sizeof(*client->pending));
	client->pending_count -= sent;
}

static void notify(const char *path) {
	for (int i = 0; i < client_count; i++) {
		struct client *client = clients[i];
		for (int s = 0; s < client->subscription_count; s++) {
			if (matches(client->subscriptions[s], path)) {
				tell(client, path);
				break;
			}
		}
	}
}

static void reply(struct client *client, const char *text, int length) {
	if (send_message(client, text, length) == 0) {
		/* Replies are small; a client whose socket stays full is not reading. */
		struct pollfd pfd = { .fd = client->fd, .events = POLLOUT };
		if (poll(&pfd, 1, 100) <= 0 || send_message(client, text, length) == 0) {
			say("pid %d does not read its replies; dropping it", client->pid);
			client->dead = true;
		}
	}
}

static void reply_text(struct client *client, const char *text) {
	reply(client, text, (int)strlen(text));
}

static bool set_attr(struct cn_object *object, const struct cn_attr *attr) {
	int index = -1;
	for (int i = 0; i < object->count; i++) {
		if (strcmp(object->attrs[i].name, attr->name) == 0) {
			index = i;
			break;
		}
	}
	if (attr->type == CN_REMOVED) {
		if (index < 0) {
			return false;
		}
		object->attrs[index] = object->attrs[--object->count];
		return true;
	}
	if (index >= 0) {
		struct cn_attr *old = &object->attrs[index];
		if (old->type == attr->type && strcmp(old->value, attr->value) == 0) {
			return false;
		}
		*old = *attr;
		return true;
	}
	if (object->count >= CN_MAX_ATTRS) {
		return false;
	}
	object->attrs[object->count++] = *attr;
	return true;
}

/* Objects that system services publish as root: the app registry, which
 * tells starry-launchd what to run, and the battery. */
static bool root_only(const char *path) {
	static const char *const paths[] = { CN_NAMESPACE "apps/", CN_NAMESPACE "power/battery" };
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (matches(paths[i], path)) {
			return true;
		}
	}
	return false;
}

static void handle_set(struct client *client, const char *path, const char *attrs) {
	struct cn_object update;
	if (!cn_valid_path(path, false) || !cn_parse_attrs(attrs, &update)) {
		reply_text(client, "=err invalid\n");
		return;
	}
	if (client->uid != 0 && root_only(path)) {
		reply_text(client, "=err denied\n");
		return;
	}
	struct entry *entry = find_entry(path, NULL);
	if (entry && entry->owner && entry->owner != client) {
		reply_text(client, "=err owned\n");
		return;
	}
	bool changed = false;
	if (!entry) {
		void *grown = entry_count < MAX_OBJECTS ?
			realloc(entries, (size_t)(entry_count + 1) * sizeof(*entries)) : NULL;
		entry = grown ? calloc(1, sizeof(*entry)) : NULL;
		if (!entry) {
			reply_text(client, "=err full\n");
			return;
		}
		entries = grown;
		entries[entry_count++] = entry;
		cn_object_init(&entry->object, path);
		changed = true;
	}
	entry->owner = client;
	for (int i = 0; i < update.count; i++) {
		changed |= set_attr(&entry->object, &update.attrs[i]);
	}
	reply_text(client, "=ok\n");
	if (changed) {
		notify(path);
	}
}

static void handle_del(struct client *client, const char *path) {
	int index;
	struct entry *entry = find_entry(path, &index);
	if (!entry) {
		reply_text(client, cn_valid_path(path, false) ? "=ok\n" : "=err invalid\n");
		return;
	}
	if (entry->owner && entry->owner != client) {
		reply_text(client, "=err owned\n");
		return;
	}
	if (client->uid != 0 && root_only(path)) {
		reply_text(client, "=err denied\n");
		return;
	}
	free(entry);
	entries[index] = entries[--entry_count];
	reply_text(client, "=ok\n");
	notify(path);
}

static void handle_get(struct client *client, const char *path) {
	char text[CN_MESSAGE_MAX];
	int length = cn_valid_path(path, false) ? format_state('=', path, text, sizeof(text)) : -1;
	if (length < 0) {
		reply_text(client, "=err invalid\n");
		return;
	}
	reply(client, text, length);
}

static void handle_list(struct client *client, const char *prefix) {
	if (!cn_valid_path(prefix, true)) {
		reply_text(client, "=err invalid\n");
		return;
	}
	char text[CN_MESSAGE_MAX];
	int length = snprintf(text, sizeof(text), "=list\n");
	for (int i = 0; i < entry_count; i++) {
		const char *path = entries[i]->object.path;
		bool under = prefix[strlen(prefix) - 1] == '/' ?
			strncmp(path, prefix, strlen(prefix)) == 0 : strcmp(path, prefix) == 0;
		if (!under) {
			continue;
		}
		length += snprintf(text + length, sizeof(text) - length, "%s\n", path);
		if (length >= (int)sizeof(text)) {
			reply_text(client, "=err toobig\n");
			return;
		}
	}
	reply(client, text, length);
}

static void handle_sub(struct client *client, const char *path) {
	if (!cn_valid_path(path, true)) {
		reply_text(client, "=err invalid\n");
		return;
	}
	bool known = false;
	for (int i = 0; i < client->subscription_count; i++) {
		known |= strcmp(client->subscriptions[i], path) == 0;
	}
	if (!known) {
		void *grown = client->subscription_count < MAX_SUBSCRIPTIONS ? realloc(client->subscriptions,
			(size_t)(client->subscription_count + 1) * sizeof(*client->subscriptions)) : NULL;
		if (!grown) {
			reply_text(client, "=err full\n");
			return;
		}
		client->subscriptions = grown;
		snprintf(client->subscriptions[client->subscription_count++], CN_PATH_MAX, "%s", path);
	}
	reply_text(client, "=ok\n");
	/* The current state, so the subscriber needs no separate get. */
	if (path[strlen(path) - 1] != '/') {
		tell(client, path);
		return;
	}
	for (int i = 0; i < entry_count; i++) {
		if (matches(path, entries[i]->object.path)) {
			tell(client, entries[i]->object.path);
		}
	}
}

static void handle_unsub(struct client *client, const char *path) {
	for (int i = 0; i < client->subscription_count; i++) {
		if (strcmp(client->subscriptions[i], path) == 0) {
			memmove(client->subscriptions[i], client->subscriptions[i + 1],
				(size_t)(client->subscription_count - i - 1) * sizeof(*client->subscriptions));
			client->subscription_count--;
			break;
		}
	}
	reply_text(client, "=ok\n");
}

static void handle_message(struct client *client, char *text) {
	char *body = strchr(text, '\n');
	if (body) {
		*body++ = 0;
	} else {
		body = text + strlen(text);
	}
	char *path = strchr(text, ' ');
	if (!path) {
		reply_text(client, "=err invalid\n");
		return;
	}
	*path++ = 0;
	if (strcmp(text, "set") == 0) {
		handle_set(client, path, body);
	} else if (strcmp(text, "del") == 0) {
		handle_del(client, path);
	} else if (strcmp(text, "get") == 0) {
		handle_get(client, path);
	} else if (strcmp(text, "list") == 0) {
		handle_list(client, path);
	} else if (strcmp(text, "sub") == 0) {
		handle_sub(client, path);
	} else if (strcmp(text, "unsub") == 0) {
		handle_unsub(client, path);
	} else {
		reply_text(client, "=err unknown\n");
	}
}

static void read_client(struct client *client) {
	ssize_t length = recv(client->fd, message, CN_MESSAGE_MAX, MSG_DONTWAIT | MSG_TRUNC);
	if (length < 0) {
		if (errno != EAGAIN && errno != EINTR) {
			client->dead = true;
		}
		return;
	}
	if (length == 0) {
		client->dead = true;
		return;
	}
	if (length > CN_MESSAGE_MAX) {
		reply_text(client, "=err toobig\n");
		return;
	}
	message[length] = 0;
	handle_message(client, message);
}

static void accept_client(int listener) {
	int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0) {
		return;
	}
	struct client *client = client_count < MAX_CLIENTS ? calloc(1, sizeof(*client)) : NULL;
	if (!client) {
		say("too many clients");
		close(fd);
		return;
	}
	struct ucred credentials;
	socklen_t size = sizeof(credentials);
	bool known = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0;
	client->pid = known ? credentials.pid : -1;
	client->uid = known ? credentials.uid : (uid_t)-1;
	client->fd = fd;
	clients[client_count++] = client;
}

static void remove_client(int index) {
	struct client *client = clients[index];
	/* Its objects stay published; a restarted service takes them over. */
	for (int i = 0; i < entry_count; i++) {
		if (entries[i]->owner == client) {
			entries[i]->owner = NULL;
		}
	}
	close(client->fd);
	free(client->subscriptions);
	free(client->pending);
	free(client);
	clients[index] = clients[--client_count];
}

static int listen_on(const char *path) {
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(address.sun_path)) {
		say("socket path too long: %s", path);
		return -1;
	}
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	/* A socket file that nobody answers on is left over from a crash. */
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
		say("already running on %s", path);
		close(fd);
		return -1;
	}
	close(fd);
	struct stat info;
	if (lstat(path, &info) == 0 && S_ISSOCK(info.st_mode)) {
		unlink(path);
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
			chmod(path, 0666) < 0 || listen(fd, 32) < 0) {
		say("cannot listen on %s: %s", path, strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

static void on_signal(int number) {
	running = 0;
}

int main(int argc, char **argv) {
	const char *path = getenv("CONSTELLATION_SOCKET");
	if (argc == 3 && strcmp(argv[1], "-s") == 0) {
		path = argv[2];
	} else if (argc != 1) {
		fprintf(stderr, "usage: constellationd [-s SOCKET]\n");
		return 2;
	}
	path = path && path[0] ? path : CN_SOCKET;
	int listener = listen_on(path);
	if (listener < 0) {
		return 1;
	}
	struct sigaction action = { .sa_handler = on_signal };
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
	say("listening on %s", path);

	struct pollfd fds[MAX_CLIENTS + 1];
	while (running) {
		fds[0] = (struct pollfd){ .fd = listener, .events = POLLIN };
		for (int i = 0; i < client_count; i++) {
			/* A client that is owed events sends nothing more until it reads them. */
			fds[i + 1] = (struct pollfd){ .fd = clients[i]->fd,
				.events = clients[i]->pending_count ? POLLOUT : POLLIN };
		}
		int count = client_count;
		if (poll(fds, (nfds_t)count + 1, -1) < 0) {
			if (errno == EINTR) {
				continue;
			}
			say("poll: %s", strerror(errno));
			break;
		}
		for (int i = 0; i < count; i++) {
			short events = fds[i + 1].revents;
			if (events & POLLOUT) {
				flush(clients[i]);
			}
			if (events & POLLIN) {
				read_client(clients[i]);
			} else if (events & (POLLHUP | POLLERR)) {
				clients[i]->dead = true;
			}
		}
		for (int i = client_count - 1; i >= 0; i--) {
			if (clients[i]->dead) {
				remove_client(i);
			}
		}
		if (fds[0].revents & POLLIN) {
			accept_client(listener);
		}
	}
	close(listener);
	unlink(path);
	return 0;
}
