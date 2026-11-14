/* core.c - clients, tags, layouts and focus. Backend-agnostic. */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "niwm.h"

struct wm wm;

int mon_curtag(const monitor *m)
{
	int i;

	for (i = 0; i < MAX_TAGS; i++)
		if (m->tagset & (1u << i))
			return i;
	return 0;
}

int core_curtag(void)
{
	return mon_curtag(wm.selmon);
}

/* shown on its own monitor (every monitor shows its own tags) */
int core_visible(const client *c)
{
	return (c->tags & c->mon->tagset) != 0;
}

/* shown on the selected monitor: what focus and commands act on */
static int on_sel(const client *c)
{
	return c->mon == wm.selmon && core_visible(c);
}

int core_isfloat(const client *c)
{
	return c->floating || c->mon->layout[mon_curtag(c->mon)] == LAYOUT_FLOAT;
}

client *core_find(uintptr_t id)
{
	client *c;

	for (c = wm.clients; c; c = c->next) {
		if (c->id == id)
			return c;
		if (c->swallowed && c->swallowed->id == id)
			return c->swallowed;
	}
	return NULL;
}

/* ---- geometry -------------------------------------------------------- */

/* Place a client in an outer box (border included). */
static void place(client *c, int x, int y, int w, int h, int bw)
{
	int iw = w - 2 * bw, ih = h - 2 * bw;

	if (iw < 1)
		iw = 1;
	if (ih < 1)
		ih = 1;
	c->x = x;
	c->y = y;
	c->w = iw;
	c->h = ih;
	c->bw = bw;
	wm.be->configure(c, x, y, iw, ih, bw);
}

static void place_float(client *c)
{
	c->x = c->fx;
	c->y = c->fy;
	c->w = c->fw;
	c->h = c->fh;
	c->bw = wm.cfg.border;
	wm.be->configure(c, c->fx, c->fy, c->fw, c->fh, wm.cfg.border);
}

static int is_tiled(const client *c, const monitor *m)
{
	return c->mon == m && core_visible(c) && !c->fullscreen && !core_isfloat(c);
}

static void layout_tile(monitor *m, int n, int ax, int ay, int aw, int ah, int bw)
{
	int t = mon_curtag(m), g = wm.cfg.gap;
	int nm = m->nmaster[t];
	int mc = n < nm ? n : nm, sc = n - mc;
	int mw, my = ay, sy = ay, i = 0;
	client *c;

	if (mc == 0)
		mw = 0;
	else if (sc == 0)
		mw = aw;
	else
		mw = (int)((aw - g) * m->mfact[t]);

	for (c = wm.clients; c; c = c->next) {
		if (!is_tiled(c, m))
			continue;
		if (i < mc) {
			int left = mc - i;
			int h = (ah - (my - ay) - g * (left - 1)) / left;
			place(c, ax, my, mw, h, bw);
			my += h + g;
		} else {
			int left = sc - (i - mc);
			int h = (ah - (sy - ay) - g * (left - 1)) / left;
			int sx = mc ? ax + mw + g : ax;
			place(c, sx, sy, aw - (sx - ax), h, bw);
			sy += h + g;
		}
		i++;
	}
}

static void layout_monocle(monitor *m, int ax, int ay, int aw, int ah, int bw)
{
	client *c;

	for (c = wm.clients; c; c = c->next)
		if (is_tiled(c, m))
			place(c, ax, ay, aw, ah, bw);
}

static void layout_grid(monitor *m, int n, int ax, int ay, int aw, int ah, int bw)
{
	int g = wm.cfg.gap, cols = 1, rows, i = 0;
	client *c;

	while (cols * cols < n)
		cols++;
	rows = (n + cols - 1) / cols;

	for (c = wm.clients; c; c = c->next) {
		int row, col, inrow, cw, ch, x, y, w, h;

		if (!is_tiled(c, m))
			continue;
		row = i / cols;
		col = i % cols;
		inrow = (row == rows - 1) ? n - row * cols : cols;
		cw = (aw - g * (inrow - 1)) / inrow;
		ch = (ah - g * (rows - 1)) / rows;
		x = ax + col * (cw + g);
		y = ay + row * (ch + g);
		w = (col == inrow - 1) ? ax + aw - x : cw;
		h = (row == rows - 1) ? ay + ah - y : ch;
		place(c, x, y, w, h, bw);
		i++;
	}
}

static void restack(void)
{
	client *c, *s = wm.sel;

	if (s && core_visible(s) && !core_isfloat(s))
		wm.be->raise(s); /* matters in monocle */
	for (c = wm.clients; c; c = c->next)
		if (c != s && core_visible(c) && core_isfloat(c) && !c->fullscreen)
			wm.be->raise(c);
	if (s && core_visible(s) && core_isfloat(s))
		wm.be->raise(s);
	for (c = wm.clients; c; c = c->next)
		if (c->fullscreen && core_visible(c))
			wm.be->raise(c);
}

static void arrange_mon(monitor *m)
{
	int lt = m->layout[mon_curtag(m)];
	int g = wm.cfg.gap, bw = wm.cfg.border, n = 0;
	int ax = m->x + m->sl + g, ay = m->y + m->st + wm.cfg.pad_top + g;
	int aw = m->w - m->sl - m->sr - 2 * g;
	int ah = m->h - m->st - m->sb - wm.cfg.pad_top - wm.cfg.pad_bottom - 2 * g;
	client *c;

	for (c = wm.clients; c; c = c->next) {
		if (c->mon != m)
			continue;
		if (!core_visible(c))
			wm.be->hide(c);
		else if (c->fullscreen && m->fs_beside) /* a bar asked to stay visible */
			place(c, m->x + m->sl, m->y + m->st, m->w - m->sl - m->sr,
			      m->h - m->st - m->sb, 0);
		else if (c->fullscreen)
			place(c, m->x, m->y, m->w, m->h, 0);
		else if (core_isfloat(c))
			place_float(c);
		else
			n++;
	}

	if (n) {
		int tbw = (wm.cfg.smart_borders && (n == 1 || lt == LAYOUT_MONOCLE)) ? 0 : bw;

		switch (lt) {
		case LAYOUT_MONOCLE: layout_monocle(m, ax, ay, aw, ah, tbw); break;
		case LAYOUT_GRID: layout_grid(m, n, ax, ay, aw, ah, tbw); break;
		default: layout_tile(m, n, ax, ay, aw, ah, tbw); break;
		}
	}
}

void core_arrange(void)
{
	int i;

	for (i = 0; i < wm.nmons; i++)
		arrange_mon(&wm.mons[i]);
	if (!wm.sel || !on_sel(wm.sel)) {
		core_focus(NULL);
	} else {
		restack();
		wm.be->sync();
	}
}

/* Re-apply colours and geometry after a settings change. */
void core_refresh(void)
{
	client *c;

	for (c = wm.clients; c; c = c->next)
		wm.be->set_border_color(c, c == wm.sel ? wm.cfg.focus_color
		                        : c->urgent ? wm.cfg.urgent_color
		                        : wm.cfg.normal_color);
	core_arrange();
}

void monitor_init(monitor *m)
{
	int i;

	m->tagset = m->prevtagset = 1;
	for (i = 0; i < MAX_TAGS; i++) {
		m->layout[i] = wm.cfg.layout;
		m->mfact[i] = wm.cfg.mfact;
		m->nmaster[i] = wm.cfg.nmaster;
	}
	m->sl = m->sr = m->st = m->sb = 0;
	m->fs_beside = 0;
}

static void each_client(void (*fn)(client *, void *), void *arg)
{
	client *c, *next;

	for (c = wm.clients; c; c = next) {
		next = c->next;
		if (c->swallowed)
			fn(c->swallowed, arg);
		fn(c, arg);
	}
}

static void rehome(client *c, void *arg)
{
	int n = *(int *)arg;

	if (c->mon - wm.mons >= n) /* its monitor was unplugged */
		c->mon = &wm.mons[0];
}

/* The backend reports the monitors, primary first. Monitors that stay keep
 * their tags and layouts; windows on a monitor that went away move to the
 * first one. */
void core_set_monitors(const int (*r)[4], int n)
{
	int i;

	if (n < 1)
		return;
	if (n > MAX_MONS)
		n = MAX_MONS;
	for (i = wm.nmons; i < n; i++)
		monitor_init(&wm.mons[i]);
	for (i = 0; i < n; i++) {
		wm.mons[i].num = i;
		wm.mons[i].x = r[i][0];
		wm.mons[i].y = r[i][1];
		wm.mons[i].w = r[i][2];
		wm.mons[i].h = r[i][3];
	}
	each_client(rehome, &n);
	wm.nmons = n;
	if (wm.selmon - wm.mons >= n)
		wm.selmon = &wm.mons[0];
	core_arrange();
}

monitor *core_monitor_at(int x, int y)
{
	int i;

	for (i = 0; i < wm.nmons; i++) {
		monitor *m = &wm.mons[i];

		if (x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h)
			return m;
	}
	return wm.selmon;
}

void core_set_struts(monitor *m, int l, int r, int t, int b, int fs_beside)
{

	/* a bar may never claim more than half the screen */
	l = l < 0 ? 0 : l > m->w / 2 ? m->w / 2 : l;
	r = r < 0 ? 0 : r > m->w / 2 ? m->w / 2 : r;
	t = t < 0 ? 0 : t > m->h / 2 ? m->h / 2 : t;
	b = b < 0 ? 0 : b > m->h / 2 ? m->h / 2 : b;
	if (l == m->sl && r == m->sr && t == m->st && b == m->sb && fs_beside == m->fs_beside)
		return;
	m->fs_beside = fs_beside;
	m->sl = l;
	m->sr = r;
	m->st = t;
	m->sb = b;
	core_arrange();
}

/* ---- focus ----------------------------------------------------------- */

static void fstack_remove(client *c)
{
	client **p;

	for (p = &wm.focus_stack; *p; p = &(*p)->fnext) {
		if (*p == c) {
			*p = c->fnext;
			break;
		}
	}
	c->fnext = NULL;
}

void core_focus(client *c)
{
	if (c && core_visible(c))
		wm.selmon = c->mon;
	else
		for (c = wm.focus_stack; c && !on_sel(c); c = c->fnext)
			;
	if (wm.sel && wm.sel != c)
		wm.be->set_border_color(wm.sel, wm.sel->urgent ? wm.cfg.urgent_color
		                                               : wm.cfg.normal_color);
	wm.sel = c;
	if (c) {
		c->urgent = 0;
		fstack_remove(c);
		c->fnext = wm.focus_stack;
		wm.focus_stack = c;
		wm.be->set_border_color(c, wm.cfg.focus_color);
	}
	wm.be->focus(c);
	restack();
	wm.be->sync();
}

void core_activate(client *c)
{
	uint32_t m = c->tags & ((1u << wm.cfg.ntags) - 1);

	if (!core_visible(c) && m) {
		c->mon->prevtagset = c->mon->tagset;
		c->mon->tagset = m;
		core_arrange();
	}
	core_focus(c);
}

void core_focus_mon(monitor *m)
{
	if (m == wm.selmon)
		return;
	wm.selmon = m;
	core_focus(NULL); /* the last window you used there */
}

void core_pointer_at(int x, int y)
{
	monitor *m = core_monitor_at(x, y);

	if (m != wm.selmon)
		core_focus_mon(m);
}

/* move a window to another monitor, onto that monitor's current tags;
 * the focus goes with it, so sending it back is the opposite key */
void core_send_to_mon(client *c, monitor *m)
{
	if (!c || c->mon == m)
		return;
	c->fx += m->x - c->mon->x; /* a floating window keeps its spot */
	c->fy += m->y - c->mon->y;
	c->mon = m;
	c->tags = m->tagset;
	c->scratch_placed = 0;
	core_arrange();
	core_focus(c);
}

/* a floating window was dragged: it belongs to the monitor it ended on */
void core_float_moved(client *c)
{
	monitor *m = core_monitor_at(c->fx + c->fw / 2, c->fy + c->fh / 2);

	if (m == c->mon)
		return;
	c->mon = m;
	c->tags = m->tagset;
	wm.selmon = m;
	core_arrange();
	core_focus(c);
}

/* dir > 0: next visible client in tiling order, dir < 0: previous. */
void core_focus_dir(int dir)
{
	client *c, *pick = NULL;

	if (!wm.sel)
		return;
	if (dir > 0) {
		for (c = wm.sel->next; c && !on_sel(c); c = c->next)
			;
		if (!c)
			for (c = wm.clients; c && !on_sel(c); c = c->next)
				;
		pick = c;
	} else {
		for (c = wm.clients; c; c = c->next) {
			if (c == wm.sel && pick)
				break;
			if (on_sel(c))
				pick = c;
		}
	}
	if (pick && pick != wm.sel)
		core_focus(pick);
}

/* Rebuild the tiling list from an array. */
static client **list_array(size_t *n)
{
	client *c, **a;
	size_t i = 0;

	*n = 0;
	for (c = wm.clients; c; c = c->next)
		(*n)++;
	if (!*n || !(a = malloc(*n * sizeof *a)))
		return NULL;
	for (c = wm.clients; c; c = c->next)
		a[i++] = c;
	return a;
}

static void list_relink(client **a, size_t n)
{
	size_t i;

	wm.clients = a[0];
	for (i = 0; i < n; i++)
		a[i]->next = (i + 1 < n) ? a[i + 1] : NULL;
}

void core_swap_dir(int dir)
{
	size_t n, i, j, cur = 0;
	client **a, *tmp;

	if (!wm.sel || !(a = list_array(&n)))
		return;
	for (i = 0; i < n; i++)
		if (a[i] == wm.sel)
			cur = i;
	for (j = cur, i = 0; i < n; i++) {
		j = (j + n + (dir > 0 ? 1 : n - 1)) % n;
		if (on_sel(a[j]))
			break;
	}
	if (j != cur) {
		tmp = a[cur];
		a[cur] = a[j];
		a[j] = tmp;
		list_relink(a, n);
		core_arrange();
	}
	free(a);
}

void core_zoom(void)
{
	client **p, *c = wm.sel;

	if (!c || wm.clients == c)
		return;
	for (p = &wm.clients; *p; p = &(*p)->next) {
		if (*p == c) {
			*p = c->next;
			break;
		}
	}
	c->next = wm.clients;
	wm.clients = c;
	core_arrange();
}

/* ---- client lifecycle -------------------------------------------------- */

static int rule_matches(const rule *r, const client *c)
{
	if (r->class && strcmp(r->class, c->class) != 0)
		return 0;
	if (r->instance && strcmp(r->instance, c->instance) != 0)
		return 0;
	if (r->title && !strstr(c->title, r->title))
		return 0;
	return 1;
}

static void apply_rules(client *c)
{
	uint32_t all = (1u << wm.cfg.ntags) - 1;
	size_t i;

	for (i = 0; i < wm.cfg.nrules; i++) {
		const rule *r = &wm.cfg.rules[i];

		if (!rule_matches(r, c))
			continue;
		if (r->floating >= 0)
			c->floating = r->floating;
		if (r->tags & all)
			c->tags = r->tags & all;
		if (r->scratch)
			c->scratch = 1;
		if (r->terminal)
			c->terminal = 1;
		if (r->noswallow)
			c->noswallow = 1;
		if (r->mon >= 0 && r->mon < wm.nmons)
			c->mon = &wm.mons[r->mon];
	}
}

/* ---- scratchpad --------------------------------------------------------
 * One floating window that drops down from the top and hides again.
 * Hidden means "on no tag" (tags == 0), so it keeps running untouched. */

static long now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec;
}

static client *scratch_client(void)
{
	client *c;

	for (c = wm.clients; c; c = c->next)
		if (c->scratch)
			return c;
	return NULL;
}

/* top-centre of the space bars leave free */
static void scratch_place(client *c)
{
	monitor *m = c->mon;
	int bw = wm.cfg.border;
	int ax = m->x + m->sl, aw = m->w - m->sl - m->sr;
	int ay = m->y + m->st + wm.cfg.pad_top;
	int ah = m->h - m->st - m->sb - wm.cfg.pad_top - wm.cfg.pad_bottom;
	int w = aw * wm.cfg.scratch_w / 100, h = ah * wm.cfg.scratch_h / 100;

	c->fw = w - 2 * bw > 100 ? w - 2 * bw : 100;
	c->fh = h - 2 * bw > 60 ? h - 2 * bw : 60;
	c->fx = ax + (aw - w) / 2;
	c->fy = ay + wm.cfg.gap;
	c->scratch_placed = 1;
}

static void scratch_show(client *c)
{
	if (c->mon != wm.selmon) { /* and to whatever monitor you're on */
		c->mon = wm.selmon;
		c->scratch_placed = 0;
	}
	if (!c->scratch_placed)
		scratch_place(c);
	c->floating = 1;
	c->tags = c->mon->tagset; /* follows you to whatever tag you're on */
	core_arrange();
	core_focus(c);
}

int core_scratchpad(const char *cmd)
{
	client *c = scratch_client();

	if (c) {
		if (core_visible(c) && c == wm.sel) {
			c->tags = 0; /* hide */
			core_arrange();
		} else {
			scratch_show(c);
		}
		return 0;
	}
	if (!cmd || !*cmd)
		return -1;
	/* whatever window maps next is the scratchpad. That works for any
	 * program, even lxterminal, which opens windows from one process. */
	wm.scratch_pending = now_s() + 5;
	core_spawn(cmd);
	return 0;
}

void core_scratch_resize(void)
{
	client *c = scratch_client();

	if (!c)
		return;
	c->scratch_placed = 0;
	if (core_visible(c)) {
		scratch_place(c);
		core_arrange();
	}
}

/* Make sure $TERMINAL names something installed, for mod+Return, the
 * scratchpad and nibar. `set terminal` in niwmrc overrides it. */
void core_pick_terminal(void)
{
	static const char *list[] = {
		"x-terminal-emulator", "lxterminal", "xfce4-terminal", "alacritty",
		"foot", "st", "urxvt", "xterm",
	};
	const char *t = getenv("TERMINAL"), *path = getenv("PATH");
	char buf[512], first[128];
	size_t i;

	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	for (i = 0; i <= sizeof list / sizeof list[0]; i++) {
		const char *name = i == 0 ? t : list[i - 1], *p;

		if (!name || !*name)
			continue;
		snprintf(first, sizeof first, "%.*s", (int)strcspn(name, " "), name);
		for (p = path; *p;) {
			size_t n = strcspn(p, ":");

			snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, first);
			if (access(buf, X_OK) == 0) {
				if (i > 0)
					setenv("TERMINAL", name, 1);
				return;
			}
			p += n;
			if (*p == ':')
				p++;
		}
	}
}

/* ---- swallowing ----------------------------------------------------------
 * A window started from a terminal (mpv, an image viewer) takes the
 * terminal's place, and the terminal hides until that window closes.
 * "Started from" means the terminal's process is an ancestor of the
 * window's process, found by walking parent pids in /proc. */

static int is_terminal_name(const client *c)
{
	static const char *names[] = {
		"xterm", "uxterm", "lxterminal", "st", "st-256color", "urxvt", "rxvt",
		"alacritty", "foot", "footclient", "xfce4-terminal", "kitty", "konsole",
		"gnome-terminal", "gnome-terminal-server", "mate-terminal", "terminator",
		"sakura", "termite", "tilix", "terminology", "qterminal", "wezterm",
		"org.wezfurlong.wezterm",
	};
	const char *t = getenv("TERMINAL"), *base;
	char own[64];
	size_t i;

	for (i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strcasecmp(c->class, names[i]) || !strcasecmp(c->instance, names[i]))
			return 1;
	if (t && *t) { /* whatever $TERMINAL is, by its program name */
		snprintf(own, sizeof own, "%.*s", (int)strcspn(t, " "), t);
		base = strrchr(own, '/') ? strrchr(own, '/') + 1 : own;
		if (*base && (!strcasecmp(c->class, base) || !strcasecmp(c->instance, base)))
			return 1;
	}
	return 0;
}

static int parent_pid(int pid)
{
	char path[64], buf[512], *p;
	int ppid = 0;
	FILE *f;

	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	if (!(f = fopen(path, "r")))
		return 0;
	if (fgets(buf, sizeof buf, f) && (p = strrchr(buf, ')'))) /* comm may hold spaces */
		sscanf(p + 1, " %*c %d", &ppid);
	fclose(f);
	return ppid;
}

static int can_be_swallowed(const client *t)
{
	return t->terminal && !t->swallowed && core_visible(t) && !t->floating &&
	       !t->fullscreen && !t->scratch;
}

static client *find_swallower(const client *c)
{
	int p = c->pid, depth;
	client *t;

	for (depth = 0; p > 1 && depth < 64; depth++) {
		if ((p = parent_pid(p)) <= 1)
			break;
		/* Terminals like lxterminal run every window from one process, so
		 * several can match: the one you were using (focused, or most
		 * recently focused) is the one you typed the command into. */
		for (t = wm.focus_stack; t; t = t->fnext)
			if (t->pid == p && can_be_swallowed(t))
				return t;
	}
	return NULL;
}

/* c takes t's place in the tiling order; t is hidden and kept on c */
static void swallow(client *c, client *t)
{
	client **p;

	for (p = &wm.clients; *p; p = &(*p)->next) {
		if (*p == t) {
			*p = c;
			c->next = t->next;
			break;
		}
	}
	t->next = NULL;
	fstack_remove(t);
	if (wm.sel == t)
		wm.sel = NULL;
	c->tags = t->tags;
	c->mon = t->mon;
	c->swallowed = t;
	wm.be->set_border_color(t, wm.cfg.normal_color);
	wm.be->hide(t);
}

/* put every hidden terminal back next to its window (used when quitting) */
void core_unswallow_all(void)
{
	client *c;

	for (c = wm.clients; c; c = c->next) {
		client *t = c->swallowed;

		if (!t)
			continue;
		c->swallowed = NULL;
		t->tags = c->tags;
		t->mon = c->mon;
		t->next = c->next;
		c->next = t;
		t->fnext = wm.focus_stack;
		wm.focus_stack = t;
	}
}

client *core_manage(uintptr_t id, int x, int y, int w, int h,
                    const char *class, const char *instance,
                    const char *title, int want_float, int pid)
{
	client *t = NULL;
	monitor *m = wm.selmon;
	client *c = calloc(1, sizeof *c);

	if (!c)
		return NULL;
	c->id = id;
	snprintf(c->class, sizeof c->class, "%s", class ? class : "");
	snprintf(c->instance, sizeof c->instance, "%s", instance ? instance : "");
	snprintf(c->title, sizeof c->title, "%s", title ? title : "");
	c->fx = x;
	c->fy = y;
	c->fw = w > 0 ? w : 640;
	c->fh = h > 0 ? h : 480;
	c->mon = m;
	c->tags = m->tagset;
	c->floating = want_float;
	c->pid = pid;
	c->terminal = is_terminal_name(c);
	apply_rules(c);
	if (c->mon == m && c->floating && (c->fx > 0 || c->fy > 0)) {
		/* a placed floating window (a dialog) belongs where it was put */
		monitor *at = core_monitor_at(c->fx + c->fw / 2, c->fy + c->fh / 2);

		if (at != m && c->tags == m->tagset)
			c->tags = at->tagset;
		c->mon = at;
	} else if (c->mon != m && c->tags == m->tagset) {
		c->tags = c->mon->tagset; /* a rule picked another monitor */
	}
	m = c->mon;

	if (wm.scratch_pending && now_s() <= wm.scratch_pending)
		c->scratch = 1;
	if (c->scratch && scratch_client())
		c->scratch = 0; /* only one scratchpad */
	if (c->scratch) {
		wm.scratch_pending = 0;
		c->floating = 1;
		scratch_place(c);
	} else if ((c->fx <= m->x && c->fy <= m->y) || c->fx >= m->x + m->w || c->fy >= m->y + m->h) {
		/* Floating windows the app didn't place get centred. */
		c->fx = m->x + (m->w - c->fw) / 2;
		c->fy = m->y + (m->h - c->fh) / 2;
	}
	/* known geometry from the start, so hiding works before first layout */
	c->x = c->fx;
	c->y = c->fy;
	c->w = c->fw;
	c->h = c->fh;

	/* swallow only plain tiled windows that no rule sent elsewhere */
	if (wm.cfg.swallow && c->pid > 0 && !c->terminal && !c->noswallow &&
	    !c->floating && !c->scratch && c->tags == m->tagset)
		t = find_swallower(c);
	if (t) {
		swallow(c, t);
	} else {
		c->next = wm.clients;
		wm.clients = c;
	}
	c->fnext = wm.focus_stack;
	wm.focus_stack = c;
	wm.be->set_border_color(c, wm.cfg.normal_color);
	return c;
}

void core_unmanage(client *c)
{
	client **p, *t = c->swallowed, *o;

	/* a hidden terminal went away (closed some other way): just forget it */
	for (o = wm.clients; o; o = o->next) {
		if (o->swallowed == c) {
			o->swallowed = NULL;
			free(c);
			return;
		}
	}
	for (p = &wm.clients; *p; p = &(*p)->next) {
		if (*p == c) {
			if (t) { /* the terminal comes back in this window's place */
				t->next = c->next;
				*p = t;
			} else {
				*p = c->next;
			}
			break;
		}
	}
	fstack_remove(c);
	if (wm.sel == c)
		wm.sel = NULL;
	if (t) {
		t->tags = c->tags;
		t->mon = c->mon;
		t->fnext = wm.focus_stack; /* and gets the focus back */
		wm.focus_stack = t;
	}
	free(c);
	core_arrange();
}

void core_set_title(client *c, const char *title)
{
	snprintf(c->title, sizeof c->title, "%s", title ? title : "");
}

void core_set_urgent(client *c, int on)
{
	if (c == wm.sel)
		on = 0;
	c->urgent = on;
	wm.be->set_border_color(c, c == wm.sel ? wm.cfg.focus_color
	                        : on ? wm.cfg.urgent_color
	                        : wm.cfg.normal_color);
}

void core_set_fullscreen(client *c, int on)
{
	on = !!on;
	if (c->fullscreen == on)
		return;
	c->fullscreen = on;
	wm.be->fullscreen(c, on);
	core_arrange();
}

void core_set_floating(client *c, int on)
{
	on = !!on;
	if (on && !c->floating) {
		/* float in place */
		c->fx = c->x;
		c->fy = c->y;
		c->fw = c->w;
		c->fh = c->h;
	}
	c->floating = on;
	core_arrange();
}

void core_float_geometry(client *c, int x, int y, int w, int h)
{
	c->fx = x;
	c->fy = y;
	c->fw = w < 1 ? 1 : w;
	c->fh = h < 1 ? 1 : h;
	if (core_visible(c) && core_isfloat(c) && !c->fullscreen)
		place_float(c);
}

/* ---- keys ------------------------------------------------------------ */

int core_key(uint32_t mods, uint32_t keysym)
{
	char action[1024];
	size_t i;

	mods &= MOD_MASK;
	for (i = 0; i < wm.cfg.nbinds; i++) {
		const keybind *b = &wm.cfg.binds[i];
		uint32_t bm = b->mods;

		if (bm & MOD_MOD)
			bm = (bm & ~MOD_MOD) | wm.cfg.modkey;
		if (bm == mods && b->keysym == keysym) {
			/* copy: the action may be "reload", which frees binds */
			snprintf(action, sizeof action, "%s", b->action);
			core_cmd(action, NULL, 0);
			return 1;
		}
	}
	return 0;
}

/* ---- state snapshot (for `subscribe`) ---------------------------------
 *
 *   begin
 *   tags 9
 *   view 1            tag masks are hex
 *   occupied 3
 *   urgent 0
 *   layout tile
 *   colors c23b3b 3a3a3a e0a030     focus normal urgent
 *   monitor 0 0 0 1366 768 1 3 0 tile *   num x y w h view occupied
 *                                      urgent layout, * = selected
 *   client 40000c 1 f 0 XTerm<TAB>title  id tags flags monitor class title
 *   end                                  flags: f focused, F floating,
 *                                        S fullscreen, u urgent,
 *                                        s scratchpad, w swallowing, - none
 * view/occupied/urgent/layout describe the selected monitor. */
static size_t put(char *buf, size_t len, size_t pos, const char *fmt, ...)
{
	va_list ap;
	int r;

	if (pos >= len)
		return pos;
	va_start(ap, fmt);
	r = vsnprintf(buf + pos, len - pos, fmt, ap);
	va_end(ap);
	if (r < 0)
		return pos;
	return pos + (size_t)r >= len ? len : pos + (size_t)r;
}

static void clean(char *dst, const char *src, size_t n)
{
	size_t i;

	for (i = 0; src[i] && i + 1 < n; i++)
		dst[i] = ((unsigned char)src[i] < 0x20) ? ' ' : src[i];
	dst[i] = '\0';
}

size_t core_snapshot(char *buf, size_t len)
{
	const size_t room = 16; /* always leave space for "end\n" */
	uint32_t occ[MAX_MONS] = { 0 }, urg[MAX_MONS] = { 0 };
	char cls[64], title[96], fl[8];
	monitor *sm = wm.selmon;
	size_t pos = 0;
	client *c;
	int k, i;

	if (len <= room)
		return 0;
	for (c = wm.clients; c; c = c->next) {
		occ[c->mon->num] |= c->tags;
		if (c->urgent)
			urg[c->mon->num] |= c->tags;
	}
	pos = put(buf, len - room, pos,
	          "begin\ntags %d\nview %x\noccupied %x\nurgent %x\nlayout %s\n"
	          "colors %06x %06x %06x\n",
	          wm.cfg.ntags, sm->tagset, occ[sm->num], urg[sm->num],
	          layout_name(sm->layout[mon_curtag(sm)]),
	          wm.cfg.focus_color, wm.cfg.normal_color, wm.cfg.urgent_color);
	for (i = 0; i < wm.nmons; i++) {
		monitor *m = &wm.mons[i];

		pos = put(buf, len - room, pos, "monitor %d %d %d %d %d %x %x %x %s %s\n",
		          i, m->x, m->y, m->w, m->h, m->tagset, occ[i], urg[i],
		          layout_name(m->layout[mon_curtag(m)]), m == sm ? "*" : "-");
	}
	for (c = wm.clients; c; c = c->next) {
		size_t before = pos;

		k = 0;
		if (c == wm.sel) fl[k++] = 'f';
		if (c->floating) fl[k++] = 'F';
		if (c->fullscreen) fl[k++] = 'S';
		if (c->urgent) fl[k++] = 'u';
		if (c->scratch) fl[k++] = 's';
		if (c->swallowed) fl[k++] = 'w';
		if (!k) fl[k++] = '-';
		fl[k] = '\0';
		clean(cls, c->class, sizeof cls);
		clean(title, c->title, sizeof title);
		pos = put(buf, len - room, pos, "client %lx %x %s %d %s\t%s\n",
		          (unsigned long)c->id, c->tags, fl, c->mon->num, cls, title);
		if (pos >= len - room) { /* out of room: drop the partial line */
			pos = before;
			break;
		}
	}
	buf[pos] = '\0';
	return put(buf, len, pos, "end\n");
}

/* ---- init ------------------------------------------------------------ */

void core_init(const backend *be, int x, int y, int w, int h)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT; /* no zombies from spawn */
	sigaction(SIGCHLD, &sa, NULL);

	wm.be = be;
	wm.running = 1;
	/* one monitor to start with; the backend reports the real ones */
	wm.nmons = 1;
	wm.selmon = &wm.mons[0];
	wm.mons[0].x = x;
	wm.mons[0].y = y;
	wm.mons[0].w = w;
	wm.mons[0].h = h;
	wm.mons[0].tagset = wm.mons[0].prevtagset = 1;
	core_pick_terminal();
	config_load();
	wm.be->grab_keys();
}

void core_cleanup(void)
{
	while (wm.clients) {
		client *c = wm.clients;

		wm.clients = c->next;
		free(c->swallowed);
		free(c);
	}
	wm.focus_stack = wm.sel = NULL;
	config_free(&wm.cfg);
}
