/* SPDX-License-Identifier: MIT */
/*
 * starry-launchd: starts applications for starboard, so that the shell
 * itself needs no root. It runs as root and answers only the starry user,
 * on a SOCK_SEQPACKET socket, one request per connection:
 *
 *     exec DISPLAY\nCOMMAND   ->  pid N   |  err REASON
 *
 * COMMAND is a catalogue line, run by /bin/sh as root with the given
 * Wayland display, as starboard did before. Packaged apps will start here
 * under their own uid instead.
 *
 * "starry-launchd --ask DISPLAY COMMAND" sends one request, for testing.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
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

#define RUNTIME_DIR "/run/starry"
#define MAX_REQUEST 4096

static void say(const char *message, const char *detail) {
	fprintf(stderr, "starry-launchd: %s%s%s\n", message, detail ? ": " : "", detail ? detail : "");
}

static bool valid_display(const char *display) {
	if (strncmp(display, "wayland-", 8) != 0 || !display[8] || strlen(display) > 32) {
		return false;
	}
	return strspn(display + 8, "0123456789") == strlen(display + 8);
}

static pid_t launch(const char *display, const char *command) {
	pid_t pid = fork();
	if (pid != 0) {
		return pid;
	}
	signal(SIGCHLD, SIG_DFL);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setsid();
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
	char reply[64];
	char *display = request + 5, *command = strchr(request, '\n');
	if (strncmp(request, "exec ", 5) != 0 || !command || !command[1]) {
		snprintf(reply, sizeof(reply), "err invalid\n");
	} else {
		*command++ = 0;
		if (!valid_display(display)) {
			snprintf(reply, sizeof(reply), "err display\n");
		} else {
			pid_t pid = launch(display, command);
			if (pid < 0) {
				snprintf(reply, sizeof(reply), "err %s\n", strerror(errno));
			} else {
				fprintf(stderr, "starry-launchd: %d: %s\n", (int)pid, command);
				snprintf(reply, sizeof(reply), "pid %d\n", (int)pid);
			}
		}
	}
	send(client, reply, strlen(reply), MSG_NOSIGNAL);
}

static int ask(const char *display, const char *command) {
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", STARRY_LAUNCH_SOCKET);
	char request[MAX_REQUEST], reply[64] = "";
	int length = snprintf(request, sizeof(request), "exec %s\n%s", display, command);
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
	if (argc == 4 && strcmp(argv[1], "--ask") == 0) {
		return ask(argv[2], argv[3]);
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
