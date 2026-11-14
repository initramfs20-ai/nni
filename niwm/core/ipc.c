/* ipc.c - the command socket.
 *
 * Normal use is one-shot: connect, send a line, read the reply, done.
 * "subscribe" keeps the connection open instead, and the subscriber gets
 * a state snapshot (see core_snapshot) every time something changes,
 * plus "event <text>" lines from `emit`.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "niwm.h"
#include "sockpath.h"

#define MAX_SUBS 8
#define SNAP_LEN 24576

static int sfd = -1;
static char spath[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int subs[MAX_SUBS];
static int nsubs;
static char *last; /* last snapshot sent, allocated on first subscriber */

int ipc_init(void)
{
	struct sockaddr_un sa;

	niwm_sockpath(spath, sizeof spath);
	setenv("NIWM_SOCKET", spath, 1); /* children find us without guessing */

	if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		goto err;
	fcntl(sfd, F_SETFD, FD_CLOEXEC);
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", spath);
	unlink(spath);
	if (bind(sfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(sfd, 8) < 0)
		goto err;
	chmod(spath, 0600);
	return sfd;
err:
	perror("niwm: ipc socket");
	if (sfd >= 0)
		close(sfd);
	sfd = -1;
	return -1;
}

/* Send all of buf or report failure. MSG_NOSIGNAL: a client that hung up
 * must not kill the window manager with SIGPIPE. */
static int send_all(int fd, const char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		ssize_t w = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		sent += (size_t)w;
	}
	return 0;
}

static void drop_sub(int i)
{
	close(subs[i]);
	subs[i] = subs[--nsubs];
	if (!nsubs) {
		free(last);
		last = NULL;
	}
}

/* Subscribers are non-blocking: one that stops reading and fills its
 * socket buffer gets dropped rather than stalling niwm. */
static void broadcast(const char *buf, size_t len)
{
	int i;

	for (i = nsubs - 1; i >= 0; i--)
		if (send_all(subs[i], buf, len) < 0)
			drop_sub(i);
}

static int add_sub(int fd)
{
	char snap[SNAP_LEN];
	size_t len;

	if (nsubs >= MAX_SUBS)
		return -1;
	if (!last && !(last = calloc(1, SNAP_LEN)))
		return -1;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	len = core_snapshot(snap, sizeof snap);
	if (send_all(fd, snap, len) < 0)
		return -1;
	subs[nsubs++] = fd;
	return 0;
}

void ipc_handle(void)
{
	struct timeval tv = { 0, 300000 };
	char in[1024], out[8192];
	size_t n = 0;
	int fd;

	if ((fd = accept(sfd, NULL, NULL)) < 0)
		return;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	/* a stuck client must never freeze the window manager */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	while (n + 1 < sizeof in) {
		ssize_t r = read(fd, in + n, sizeof in - 1 - n);

		if (r <= 0)
			break;
		n += (size_t)r;
		if (memchr(in, '\n', n))
			break;
	}
	in[n] = '\0';
	in[strcspn(in, "\r\n")] = '\0';

	if (!strcmp(in, "subscribe")) {
		if (add_sub(fd) < 0) {
			send_all(fd, "error: too many subscribers\n", 28);
			close(fd);
		}
		return;
	}

	core_cmd(in, out, sizeof out);
	send_all(fd, out, strlen(out));
	close(fd);
}

void ipc_publish(void)
{
	char snap[SNAP_LEN];
	size_t len;

	if (!nsubs)
		return;
	len = core_snapshot(snap, sizeof snap);
	if (!strcmp(snap, last))
		return;
	memcpy(last, snap, len + 1);
	broadcast(snap, len);
}

void ipc_emit(const char *text)
{
	char line[512];
	int n = snprintf(line, sizeof line, "event %s\n", text);

	if (n > 0 && (size_t)n < sizeof line)
		broadcast(line, (size_t)n);
}

void ipc_cleanup(void)
{
	while (nsubs)
		drop_sub(nsubs - 1);
	if (sfd >= 0) {
		close(sfd);
		unlink(spath);
		sfd = -1;
	}
}
