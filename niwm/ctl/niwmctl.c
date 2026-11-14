/* niwmctl - send one command to a running NiWM and print the reply. */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../core/niwm.h"
#include "../core/sockpath.h"

int main(int argc, char **argv)
{
	struct sockaddr_un sa;
	char line[1024], buf[4096];
	size_t len = 0;
	ssize_t n;
	int fd, i, err = 0, first = 1;

	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		fprintf(stderr,
		        "usage: niwmctl <command> [args...]\n"
		        "       niwmctl help      list commands\n"
		        "       niwmctl query     state for bars and scripts\n");
		return argc < 2;
	}
	if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
		puts("niwmctl " NIWM_VERSION);
		return 0;
	}

	for (i = 1; i < argc; i++) {
		int r = snprintf(line + len, sizeof line - len, "%s%s", i > 1 ? " " : "", argv[i]);

		if (r < 0 || (size_t)r >= sizeof line - len) {
			fprintf(stderr, "niwmctl: command too long\n");
			return 1;
		}
		len += (size_t)r;
	}
	line[len++] = '\n';

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	niwm_sockpath(sa.sun_path, sizeof sa.sun_path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
	    connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		fprintf(stderr, "niwmctl: can't reach niwm at %s\n", sa.sun_path);
		return 1;
	}
	if (write(fd, line, len) != (ssize_t)len) {
		perror("niwmctl: write");
		return 1;
	}
	shutdown(fd, SHUT_WR);

	while ((n = read(fd, buf, sizeof buf)) > 0) {
		if (first && n >= 6 && !memcmp(buf, "error:", 6))
			err = 1;
		first = 0;
		fwrite(buf, 1, (size_t)n, err ? stderr : stdout);
		fflush(err ? stderr : stdout); /* live for `subscribe | while read` */
	}
	close(fd);
	return err;
}
