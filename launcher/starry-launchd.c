/* SPDX-License-Identifier: MIT */
/*
 * starry-launchd: starts applications for starboard, so that the shell
 * itself needs no root. It runs as root and answers only the starry user,
 * on a SOCK_SEQPACKET socket, one request per connection:
 *
 *     app DISPLAY\nID        ->  pid N   |  err REASON
 *     exec DISPLAY\nCOMMAND  ->  pid N   |  err REASON
 *
 * "app" starts an installed package under its own uid, with its data
 * directory as home. What to run comes from the app registry in
 * Constellation (/blinks/apps/ID), which only root can write, and must lie
 * inside the app's install directory. "exec" runs a catalogue line by
 * /bin/sh as root, as starboard did before packages existed.
 *
 * "starry-launchd --ask app|exec DISPLAY ARGUMENT" sends one request, for testing.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "constellation.h"

#define RUNTIME_DIR "/run/starry"
#define APPS_DIR "/data/cosmic/apps/"
#define DATA_DIR "/data/cosmic/data/"
#define FIRST_APP_UID 10000
#define MAX_REQUEST 4096

struct app {
	char program[PATH_MAX], home[PATH_MAX];
	bool qml;
	uid_t uid;
};

static void say(const char *message, const char *detail) {
	fprintf(stderr, "starry-launchd: %s%s%s\n", message, detail ? ": " : "", detail ? detail : "");
}

static bool valid_display(const char *display) {
	if (strncmp(display, "wayland-", 8) != 0 || !display[8] || strlen(display) > 32) {
		return false;
	}
	return strspn(display + 8, "0123456789") == strlen(display + 8);
}

static bool valid_id(const char *id) {
	size_t length = strlen(id);
	return length > 0 && length <= 128 && id[0] != '.' && !strstr(id, "..") &&
		strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789.-") == length;
}

/* Looks the app up in the registry; NULL, or why it cannot start. */
static const char *find_app(const char *id, struct app *app) {
	if (!valid_id(id)) {
		return "id";
	}
	struct cn_client *registry = cn_connect();
	if (!registry) {
		return "registry";
	}
	char path[CN_PATH_MAX];
	snprintf(path, sizeof(path), CN_NAMESPACE "apps/%s", id);
	struct cn_object object;
	int found = cn_fetch(registry, path, &object);
	cn_disconnect(registry);
	if (found <= 0) {
		return "unknown";
	}
	const char *entry = cn_get(&object, "entry", "");
	const char *program = cn_get(&object, "path", "");
	long uid = cn_get_int(&object, "uid", -1);
	char prefix[PATH_MAX];
	snprintf(prefix, sizeof(prefix), APPS_DIR "%s/", id);
	if (strncmp(program, prefix, strlen(prefix)) != 0 || strstr(program, "/..") ||
			strstr(program, "/./") || strlen(program) >= sizeof(app->program)) {
		return "path";
	}
	if (uid < FIRST_APP_UID || uid > 65000) {
		return "uid";
	}
	if (strcmp(entry, "qml") != 0 && strcmp(entry, "native") != 0) {
		return "entry";
	}
	snprintf(app->program, sizeof(app->program), "%s", program);
	snprintf(app->home, sizeof(app->home), DATA_DIR "%s", id);
	app->qml = strcmp(entry, "qml") == 0;
	app->uid = (uid_t)uid;
	return NULL;
}

static void start_child(void) {
	signal(SIGCHLD, SIG_DFL);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setsid();
}

static pid_t launch_app(const char *display, const struct app *app) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	start_child();
	const char *lang = getenv("LANG");
	char language[64];
	snprintf(language, sizeof(language), "%s", lang ? lang : "C.UTF-8");
	/* A fresh environment: nothing of ours leaks into the app. */
	clearenv();
	setenv("PATH", "/usr/bin:/bin", 1);
	setenv("HOME", app->home, 1);
	setenv("TMPDIR", app->home, 1);
	setenv("LANG", language, 1);
	setenv("XDG_RUNTIME_DIR", RUNTIME_DIR, 1);
	setenv("WAYLAND_DISPLAY", display, 1);
	if (app->qml) {
		setenv("LD_LIBRARY_PATH", STARRY_BINDIR "/../lib", 1);
	}
	umask(077);
	if (chdir(app->home) < 0 || setgroups(0, NULL) < 0 || setgid(app->uid) < 0 ||
			setuid(app->uid) < 0 || getuid() != app->uid || geteuid() != app->uid) {
		_exit(126);
	}
	if (app->qml) {
		execl(STARRY_BINDIR "/stardust", "stardust", app->program, (char *)NULL);
	} else {
		execl(app->program, app->program, (char *)NULL);
	}
	_exit(127);
}

static pid_t launch_command(const char *display, const char *command) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	start_child();
	setenv("XDG_RUNTIME_DIR", RUNTIME_DIR, 1);
	setenv("WAYLAND_DISPLAY", display, 1);
	execl("/bin/sh", "/bin/sh", "-c", command, (char *)NULL);
	_exit(127);
}

static void serve(int client, uid_t starry) {
	struct ucred peer;
	socklen_t size = sizeof(peer);
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 ||
			(peer.uid != starry && peer.uid != 0)) {
		say("refused a client that is not starry", NULL);
		return;
	}
	char request[MAX_REQUEST + 1];
	ssize_t length = recv(client, request, MAX_REQUEST, MSG_TRUNC);
	if (length <= 0 || length > MAX_REQUEST) {
		return;
	}
	request[length] = 0;
	char reply[64] = "err invalid\n";
	char *space = strchr(request, ' '), *argument = strchr(request, '\n');
	if (space && argument && space < argument && argument[1]) {
		*space = 0;
		*argument++ = 0;
		const char *verb = request, *display = space + 1;
		pid_t pid = -1;
		const char *problem = NULL;
		struct app app;
		if (!valid_display(display)) {
			problem = "display";
		} else if (strcmp(verb, "app") == 0) {
			problem = find_app(argument, &app);
			pid = problem ? -1 : launch_app(display, &app);
		} else if (strcmp(verb, "exec") == 0) {
			pid = launch_command(display, argument);
		} else {
			problem = "invalid";
		}
		if (!problem && pid < 0) {
			problem = strerror(errno);
		}
		if (problem) {
			snprintf(reply, sizeof(reply), "err %s\n", problem);
			say(argument, problem);
		} else {
			fprintf(stderr, "starry-launchd: %d: %s %s%s\n", (int)pid, verb, argument,
				strcmp(verb, "app") == 0 ? "" : " (as root)");
			snprintf(reply, sizeof(reply), "pid %d\n", (int)pid);
		}
	}
	send(client, reply, strlen(reply), MSG_NOSIGNAL);
}

static int ask(const char *verb, const char *display, const char *argument) {
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", STARRY_LAUNCH_SOCKET);
	char request[MAX_REQUEST], reply[64] = "";
	int length = snprintf(request, sizeof(request), "%s %s\n%s", verb, display, argument);
	if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
			length >= (int)sizeof(request) || send(fd, request, (size_t)length, 0) != length) {
		say("cannot reach the launcher", strerror(errno));
		return 1;
	}
	ssize_t received = recv(fd, reply, sizeof(reply) - 1, 0);
	printf("%s", received > 0 ? reply : "no answer\n");
	return received > 0 && strncmp(reply, "pid ", 4) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
	if (argc == 5 && strcmp(argv[1], "--ask") == 0) {
		return ask(argv[2], argv[3], argv[4]);
	}
	struct passwd *user = getpwnam("starry");
	if (!user) {
		say("no starry user", NULL);
		return 1;
	}
	uid_t starry = user->pw_uid;
	gid_t group = user->pw_gid;
	signal(SIGCHLD, SIG_IGN); /* the kernel reaps the apps we start */
	signal(SIGPIPE, SIG_IGN);

	int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", STARRY_LAUNCH_SOCKET);
	unlink(STARRY_LAUNCH_SOCKET);
	if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
			chown(STARRY_LAUNCH_SOCKET, 0, group) < 0 || chmod(STARRY_LAUNCH_SOCKET, 0660) < 0 ||
			listen(listener, 8) < 0) {
		say("cannot listen on " STARRY_LAUNCH_SOCKET, strerror(errno));
		return 1;
	}
	for (;;) {
		int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno != EINTR) {
				say("accept", strerror(errno));
			}
			continue;
		}
		serve(client, starry);
		close(client);
	}
}
