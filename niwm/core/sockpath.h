/* sockpath.h - where the IPC socket lives; shared by niwm and niwmctl. */
#ifndef NIWM_SOCKPATH_H
#define NIWM_SOCKPATH_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* $NIWM_SOCKET, else $XDG_RUNTIME_DIR/niwm-<display>.sock,
 * else /tmp/niwm-<uid>-<display>.sock (busybox systems often have no
 * XDG_RUNTIME_DIR). <display> is DISPLAY or WAYLAND_DISPLAY minus ':'. */
static inline void niwm_sockpath(char *buf, size_t len)
{
	const char *e = getenv("NIWM_SOCKET"), *d, *rd;
	char disp[32];
	size_t i, j = 0;

	if (e && *e) {
		snprintf(buf, len, "%s", e);
		return;
	}
	d = getenv("DISPLAY");
	if (!d || !*d)
		d = getenv("WAYLAND_DISPLAY");
	if (!d || !*d)
		d = "0";
	for (i = 0; d[i] && j + 1 < sizeof disp; i++)
		if (d[i] != ':' && d[i] != '/')
			disp[j++] = d[i];
	disp[j] = '\0';

	rd = getenv("XDG_RUNTIME_DIR");
	if (rd && *rd)
		snprintf(buf, len, "%s/niwm-%s.sock", rd, disp);
	else
		snprintf(buf, len, "/tmp/niwm-%u-%s.sock", (unsigned)getuid(), disp);
}

#endif
