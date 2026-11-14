/* cmd.c - NiWM's command language.
 *
 * The config file is a list of these commands, keybindings run them,
 * and `niwmctl <command>` sends them over the IPC socket.
 */
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "niwm.h"

static char *obuf;
static size_t olen, opos;

static void say(const char *fmt, ...)
{
	va_list ap;
	int r;

	if (!obuf || opos + 1 >= olen)
		return;
	va_start(ap, fmt);
	r = vsnprintf(obuf + opos, olen - opos, fmt, ap);
	va_end(ap);
	if (r > 0)
		opos += ((size_t)r < olen - opos) ? (size_t)r : olen - opos - 1;
}

static int fail(const char *fmt, ...)
{
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	say("error: %s\n", msg);
	return -1;
}

static const char *layout_names[LAYOUT_COUNT] = { "tile", "monocle", "grid", "float" };

const char *layout_name(int l)
{
	return (l >= 0 && l < LAYOUT_COUNT) ? layout_names[l] : "?";
}

static int layout_from_name(const char *s)
{
	int i;

	for (i = 0; i < LAYOUT_COUNT; i++)
		if (strcmp(s, layout_names[i]) == 0)
			return i;
	return -1;
}

static uint32_t all_tags(void)
{
	return (1u << wm.cfg.ntags) - 1;
}

/* "3" -> bit 2, "all" -> every tag, anything else -> 0 */
static uint32_t tag_mask(const char *s)
{
	char *end;
	long n;

	if (strcmp(s, "all") == 0)
		return all_tags();
	n = strtol(s, &end, 10);
	if (*s == '\0' || *end != '\0' || n < 1 || n > wm.cfg.ntags)
		return 0;
	return 1u << (n - 1);
}

static int parse_bool(const char *s)
{
	if (!strcmp(s, "yes") || !strcmp(s, "on") || !strcmp(s, "true") || !strcmp(s, "1"))
		return 1;
	if (!strcmp(s, "no") || !strcmp(s, "off") || !strcmp(s, "false") || !strcmp(s, "0"))
		return 0;
	return -1;
}

static int parse_color(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (*s == '#')
		s++;
	if (strlen(s) != 6)
		return -1;
	v = strtoul(s, &end, 16);
	if (*end)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

/* "+0.05" / "-1" adjust, "0.6" / "2" set */
static double adjust(const char *s, double cur, int *ok)
{
	char *end;
	double v = strtod(s, &end);

	*ok = (*s && !*end);
	return (*s == '+' || *s == '-') ? cur + v : v;
}

void core_spawn(const char *cmd)
{
	if (fork() == 0) {
		/* niwm ignores SIGCHLD to avoid zombies; children must not inherit that */
		signal(SIGCHLD, SIG_DFL);
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

/* ---- commands ---------------------------------------------------------- */

static int c_spawn(char *a)
{
	if (!*a)
		return fail("spawn needs a command");
	core_spawn(a);
	return 0;
}

static int c_autostart(char *a)
{
	if (!*a)
		return fail("autostart needs a command");
	if (!wm.loaded_once)
		core_spawn(a);
	return 0;
}

/* "0x40000c" or "40000c" -> client */
static client *client_arg(const char *a)
{
	char *end;
	unsigned long id = strtoul(a, &end, 16);

	return (*a && !*end) ? core_find((uintptr_t)id) : NULL;
}

static int c_kill(char *a)
{
	client *c = *a ? client_arg(a) : wm.sel;

	if (*a && !c)
		return fail("no window '%s'", a);
	if (c)
		wm.be->close(c);
	return 0;
}

static int c_activate(char *a)
{
	client *c = client_arg(a);

	if (!c)
		return fail("no window '%s'", a);
	core_activate(c);
	return 0;
}

static int c_emit(char *a)
{
	if (!*a)
		return fail("emit needs some text");
	ipc_emit(a);
	return 0;
}

static int c_scratchpad(char *a)
{
	if (core_scratchpad(a) < 0)
		return fail("no scratchpad yet; give a command to start one, e.g. scratchpad $TERMINAL");
	return 0;
}

static int c_subscribe(char *a)
{
	(void)a;
	return fail("subscribe only works through niwmctl");
}

static int dir_arg(const char *a)
{
	if (!strcmp(a, "next") || !strcmp(a, "+1"))
		return 1;
	if (!strcmp(a, "prev") || !strcmp(a, "-1"))
		return -1;
	return 0;
}

static int c_focus(char *a)
{
	int d = dir_arg(a);

	if (!d)
		return fail("focus takes next or prev");
	core_focus_dir(d);
	return 0;
}

static int c_swap(char *a)
{
	int d = dir_arg(a);

	if (!d)
		return fail("swap takes next or prev");
	core_swap_dir(d);
	return 0;
}

/* "next", "prev" or a monitor number (1 = the primary) */
static monitor *mon_arg(const char *a)
{
	int cur = wm.selmon->num, n;

	if (!strcmp(a, "next") || !strcmp(a, "+1"))
		return &wm.mons[(cur + 1) % wm.nmons];
	if (!strcmp(a, "prev") || !strcmp(a, "-1"))
		return &wm.mons[(cur + wm.nmons - 1) % wm.nmons];
	n = atoi(a);
	return (n >= 1 && n <= wm.nmons) ? &wm.mons[n - 1] : NULL;
}

static int c_focusmon(char *a)
{
	monitor *m = mon_arg(a);

	if (!m)
		return fail("no monitor '%s' (there are %d)", a, wm.nmons);
	core_focus_mon(m);
	return 0;
}

static int c_tagmon(char *a)
{
	monitor *m = mon_arg(a);

	if (!m)
		return fail("no monitor '%s' (there are %d)", a, wm.nmons);
	core_send_to_mon(wm.sel, m);
	return 0;
}

static int c_zoom(char *a)
{
	(void)a;
	core_zoom();
	return 0;
}

static void set_view(uint32_t mask)
{
	if (!mask || mask == wm.selmon->tagset)
		return;
	wm.selmon->prevtagset = wm.selmon->tagset;
	wm.selmon->tagset = mask;
	core_arrange();
}

static int c_view(char *a)
{
	uint32_t m;

	if (!strcmp(a, "prev")) {
		set_view(wm.selmon->prevtagset);
		return 0;
	}
	if (!(m = tag_mask(a)))
		return fail("no such tag '%s'", a);
	set_view(m); /* already viewing it: no-op, so scripts are predictable */
	return 0;
}

static int c_toggleview(char *a)
{
	uint32_t m = tag_mask(a);

	if (!m)
		return fail("no such tag '%s'", a);
	set_view(wm.selmon->tagset ^ m);
	return 0;
}

static int c_tag(char *a)
{
	uint32_t m = tag_mask(a);

	if (!m)
		return fail("no such tag '%s'", a);
	if (wm.sel) {
		wm.sel->tags = m;
		core_arrange();
	}
	return 0;
}

static int c_toggletag(char *a)
{
	uint32_t m = tag_mask(a);

	if (!m)
		return fail("no such tag '%s'", a);
	if (wm.sel && (wm.sel->tags ^ m)) {
		wm.sel->tags ^= m;
		core_arrange();
	}
	return 0;
}

static int c_layout(char *a)
{
	int t = core_curtag(), l;

	if (!strcmp(a, "next"))
		l = (wm.selmon->layout[t] + 1) % LAYOUT_COUNT;
	else if (!strcmp(a, "prev"))
		l = (wm.selmon->layout[t] + LAYOUT_COUNT - 1) % LAYOUT_COUNT;
	else if ((l = layout_from_name(a)) < 0)
		return fail("unknown layout '%s' (tile, monocle, grid, float)", a);
	wm.selmon->layout[t] = l;
	core_arrange();
	return 0;
}

static int c_float(char *a)
{
	(void)a;
	if (wm.sel)
		core_set_floating(wm.sel, !wm.sel->floating);
	return 0;
}

static int c_fullscreen(char *a)
{
	(void)a;
	if (wm.sel)
		core_set_fullscreen(wm.sel, !wm.sel->fullscreen);
	return 0;
}

static int c_mfact(char *a)
{
	int t = core_curtag(), ok;
	double v = adjust(a, wm.selmon->mfact[t], &ok);

	if (!ok)
		return fail("mfact takes a number like 0.6 or +0.05");
	if (v < 0.05)
		v = 0.05;
	if (v > 0.95)
		v = 0.95;
	wm.selmon->mfact[t] = (float)v;
	core_arrange();
	return 0;
}

static int c_nmaster(char *a)
{
	int t = core_curtag(), ok;
	double v = adjust(a, wm.selmon->nmaster[t], &ok);

	if (!ok)
		return fail("nmaster takes a number like 2 or +1");
	wm.selmon->nmaster[t] = v < 0 ? 0 : (int)v;
	core_arrange();
	return 0;
}

static int c_set(char *a)
{
	char *key = a, *val = a + strcspn(a, " \t");
	int i, b;

	if (*val) {
		*val++ = '\0';
		val += strspn(val, " \t");
	}
	if (!*key || !*val)
		return fail("usage: set <key> <value>");

	if (!strcmp(key, "border")) {
		wm.cfg.border = atoi(val) < 0 ? 0 : atoi(val);
	} else if (!strcmp(key, "gap")) {
		wm.cfg.gap = atoi(val) < 0 ? 0 : atoi(val);
	} else if (!strcmp(key, "pad_top")) {
		wm.cfg.pad_top = atoi(val) < 0 ? 0 : atoi(val);
	} else if (!strcmp(key, "pad_bottom")) {
		wm.cfg.pad_bottom = atoi(val) < 0 ? 0 : atoi(val);
	} else if (!strcmp(key, "focus_color")) {
		if (parse_color(val, &wm.cfg.focus_color))
			return fail("colours look like #c23b3b");
	} else if (!strcmp(key, "normal_color")) {
		if (parse_color(val, &wm.cfg.normal_color))
			return fail("colours look like #3a3a3a");
	} else if (!strcmp(key, "urgent_color")) {
		if (parse_color(val, &wm.cfg.urgent_color))
			return fail("colours look like #e0a030");
	} else if (!strcmp(key, "mod")) {
		if (!strcmp(val, "super") || !strcmp(val, "mod4"))
			wm.cfg.modkey = MOD_SUPER;
		else if (!strcmp(val, "alt") || !strcmp(val, "mod1"))
			wm.cfg.modkey = MOD_ALT;
		else if (!strcmp(val, "ctrl"))
			wm.cfg.modkey = MOD_CTRL;
		else
			return fail("mod is super, alt or ctrl");
		if (wm.loaded_once && !wm.loading)
			wm.be->grab_keys();
	} else if (!strcmp(key, "tags")) {
		int n = atoi(val);

		if (n < 1 || n > MAX_TAGS)
			return fail("tags is 1 to %d", MAX_TAGS);
		wm.cfg.ntags = n;
		for (i = 0; i < wm.nmons; i++) {
			monitor *m = &wm.mons[i];

			if (!(m->tagset & all_tags()))
				m->tagset = 1;
			m->tagset &= all_tags();
		}
	} else if (!strcmp(key, "mfact")) {
		float f = (float)atof(val);

		if (f < 0.05f || f > 0.95f)
			return fail("mfact is between 0.05 and 0.95");
		wm.cfg.mfact = f;
		for (i = 0; i < wm.nmons * MAX_TAGS; i++)
			wm.mons[i / MAX_TAGS].mfact[i % MAX_TAGS] = f;
	} else if (!strcmp(key, "nmaster")) {
		wm.cfg.nmaster = atoi(val) < 0 ? 0 : atoi(val);
		for (i = 0; i < wm.nmons * MAX_TAGS; i++)
			wm.mons[i / MAX_TAGS].nmaster[i % MAX_TAGS] = wm.cfg.nmaster;
	} else if (!strcmp(key, "layout")) {
		int l = layout_from_name(val);

		if (l < 0)
			return fail("unknown layout '%s'", val);
		wm.cfg.layout = l;
		for (i = 0; i < wm.nmons * MAX_TAGS; i++)
			wm.mons[i / MAX_TAGS].layout[i % MAX_TAGS] = l;
	} else if (!strcmp(key, "scratchpad_size")) {
		int w, h;

		if (sscanf(val, "%dx%d", &w, &h) != 2 || w < 10 || w > 100 || h < 10 || h > 100)
			return fail("scratchpad_size is WIDTHxHEIGHT in percent, e.g. 60x45");
		wm.cfg.scratch_w = w;
		wm.cfg.scratch_h = h;
		core_scratch_resize();
	} else if (!strcmp(key, "swallow")) {
		if ((b = parse_bool(val)) < 0)
			return fail("swallow is yes or no");
		wm.cfg.swallow = b;
	} else if (!strcmp(key, "terminal")) {
		setenv("TERMINAL", val, 1); /* everything started after this sees it */
	} else if (!strcmp(key, "focus_follows_mouse") || !strcmp(key, "smart_borders")) {
		if ((b = parse_bool(val)) < 0)
			return fail("%s is yes or no", key);
		if (key[0] == 'f')
			wm.cfg.focus_follows_mouse = b;
		else
			wm.cfg.smart_borders = b;
	} else {
		return fail("unknown setting '%s'", key);
	}
	if (wm.loaded_once && !wm.loading)
		core_refresh();
	return 0;
}

static int c_bind(char *a)
{
	char *combo = a, *act = a + strcspn(a, " \t");
	uint32_t mods, sym;
	keybind *nb;
	size_t i;

	if (*act) {
		*act++ = '\0';
		act += strspn(act, " \t");
	}
	if (!*combo || !*act)
		return fail("usage: bind <mod+key> <command>");
	if (parse_combo(combo, &mods, &sym) < 0)
		return fail("unknown key in '%s'", combo);

	for (i = 0; i < wm.cfg.nbinds; i++) {
		if (wm.cfg.binds[i].mods == mods && wm.cfg.binds[i].keysym == sym) {
			char *s = strdup(act);

			if (!s)
				return fail("out of memory");
			free(wm.cfg.binds[i].action);
			wm.cfg.binds[i].action = s;
			return 0;
		}
	}
	nb = realloc(wm.cfg.binds, (wm.cfg.nbinds + 1) * sizeof *nb);
	if (!nb)
		return fail("out of memory");
	wm.cfg.binds = nb;
	nb[wm.cfg.nbinds].mods = mods;
	nb[wm.cfg.nbinds].keysym = sym;
	if (!(nb[wm.cfg.nbinds].action = strdup(act)))
		return fail("out of memory");
	wm.cfg.nbinds++;
	if (wm.loaded_once && !wm.loading)
		wm.be->grab_keys();
	return 0;
}

static int c_unbind(char *a)
{
	uint32_t mods, sym;
	size_t i;

	if (parse_combo(a, &mods, &sym) < 0)
		return fail("unknown key in '%s'", a);
	for (i = 0; i < wm.cfg.nbinds; i++) {
		if (wm.cfg.binds[i].mods == mods && wm.cfg.binds[i].keysym == sym) {
			free(wm.cfg.binds[i].action);
			wm.cfg.binds[i] = wm.cfg.binds[--wm.cfg.nbinds];
			if (wm.loaded_once && !wm.loading)
				wm.be->grab_keys();
			return 0;
		}
	}
	return fail("'%s' is not bound", a);
}

/* rule class=mpv float | rule instance=foo title=Bar tag 3 */
static int c_rule(char *a)
{
	rule r = { NULL, NULL, NULL, -1, 0, 0, 0, 0, -1 }, *nr;
	char *tok, *save = NULL;
	int want_tag = 0, want_mon = 0;

	for (tok = strtok_r(a, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		char **field = NULL, *v = strchr(tok, '=');

		if (want_tag) {
			if (!(r.tags = tag_mask(tok)))
				goto bad;
			want_tag = 0;
			continue;
		}
		if (want_mon) {
			if ((r.mon = atoi(tok) - 1) < 0 || r.mon >= MAX_MONS)
				goto bad;
			want_mon = 0;
			continue;
		}
		if (v) {
			*v++ = '\0';
			if (!strcmp(tok, "class"))
				field = &r.class;
			else if (!strcmp(tok, "instance"))
				field = &r.instance;
			else if (!strcmp(tok, "title"))
				field = &r.title;
			if (!field || *field || !(*field = strdup(v)))
				goto bad;
		} else if (!strcmp(tok, "float")) {
			r.floating = 1;
		} else if (!strcmp(tok, "scratchpad")) {
			r.scratch = 1;
		} else if (!strcmp(tok, "terminal")) {
			r.terminal = 1;
		} else if (!strcmp(tok, "noswallow")) {
			r.noswallow = 1;
		} else if (!strcmp(tok, "tile")) {
			r.floating = 0;
		} else if (!strcmp(tok, "tag")) {
			want_tag = 1;
		} else if (!strcmp(tok, "monitor")) {
			want_mon = 1;
		} else {
			goto bad;
		}
	}
	if (want_tag || want_mon || (!r.class && !r.instance && !r.title))
		goto bad;
	if (!(nr = realloc(wm.cfg.rules, (wm.cfg.nrules + 1) * sizeof *nr)))
		goto bad;
	wm.cfg.rules = nr;
	nr[wm.cfg.nrules++] = r;
	return 0;
bad:
	free(r.class);
	free(r.instance);
	free(r.title);
	return fail("usage: rule class=X|instance=X|title=X ... [float|tile|scratchpad|terminal|noswallow] [tag N] [monitor N]");
}

static int c_reload(char *a)
{
	(void)a;
	config_load();
	wm.be->grab_keys();
	core_refresh();
	ipc_emit("reload"); /* bars reload their own config too */
	return 0;
}

static int c_quit(char *a)
{
	(void)a;
	wm.running = 0;
	return 0;
}

static void say_tags(const char *label, uint32_t mask)
{
	int i;

	say("%s", label);
	for (i = 0; i < wm.cfg.ntags; i++)
		if (mask & (1u << i))
			say(" %d", i + 1);
	say("\n");
}

static int c_query(char *a)
{
	uint32_t occ = 0, urg = 0;
	int t = core_curtag(), n = 0;
	client *c;

	(void)a;
	for (c = wm.clients; c; c = c->next) {
		n++;
		if (c->mon != wm.selmon)
			continue;
		occ |= c->tags;
		if (c->urgent)
			urg |= c->tags;
	}
	say("version %s\nbackend %s\ntags %d\n", NIWM_VERSION, wm.be->name, wm.cfg.ntags);
	say("monitor %d of %d %dx%d+%d+%d\n", wm.selmon->num + 1, wm.nmons, wm.selmon->w,
	    wm.selmon->h, wm.selmon->x, wm.selmon->y);
	say_tags("view", wm.selmon->tagset);
	say_tags("occupied", occ);
	say_tags("urgent", urg);
	for (c = wm.clients; c && !c->scratch; c = c->next)
		;
	say("scratchpad %s\n", !c ? "none" : core_visible(c) ? "shown" : "hidden");
	say("layout %s\nmfact %.2f\nnmaster %d\nclients %d\n",
	    layout_name(wm.selmon->layout[t]), wm.selmon->mfact[t], wm.selmon->nmaster[t], n);
	if (wm.sel)
		say("focused %s %s %s\ntitle %s\n", wm.sel->class,
		    wm.sel->floating ? "floating" : "tiled",
		    wm.sel->fullscreen ? "fullscreen" : "-", wm.sel->title);
	return 0;
}

static int c_clients(char *a)
{
	client *c;
	int i;

	(void)a;
	for (c = wm.clients; c; c = c->next) {
		say("0x%lx %s tags", (unsigned long)c->id, c == wm.sel ? "*" : "-");
		for (i = 0; i < wm.cfg.ntags; i++)
			if (c->tags & (1u << i))
				say(",%d", i + 1);
		say(" mon,%d %s %s\n", c->mon->num + 1, c->class, c->title);
	}
	return 0;
}

static int c_version(char *a)
{
	(void)a;
	say("niwm %s (%s)\n", NIWM_VERSION, wm.be->name);
	return 0;
}

static int c_help(char *a);

static const struct {
	const char *name;
	int (*fn)(char *);
	const char *help;
} cmds[] = {
	{ "spawn",      c_spawn,      "spawn <shell command>" },
	{ "autostart",  c_autostart,  "autostart <shell command>  (first load only)" },
	{ "kill",       c_kill,       "kill [id]  (close focused window, or that one)" },
	{ "activate",   c_activate,   "activate <id>  (jump to its tag and focus it)" },
	{ "focus",      c_focus,      "focus next|prev" },
	{ "swap",       c_swap,       "swap next|prev" },
	{ "zoom",       c_zoom,       "zoom  (focused window becomes master)" },
	{ "focusmon",   c_focusmon,   "focusmon next|prev|N  (select a monitor; 1 = primary)" },
	{ "tagmon",     c_tagmon,     "tagmon next|prev|N  (send focused window to a monitor)" },
	{ "view",       c_view,       "view N|all|prev" },
	{ "toggleview", c_toggleview, "toggleview N" },
	{ "tag",        c_tag,        "tag N|all  (move focused window)" },
	{ "toggletag",  c_toggletag,  "toggletag N" },
	{ "layout",     c_layout,     "layout tile|monocle|grid|float|next|prev" },
	{ "float",      c_float,      "float  (toggle focused window)" },
	{ "scratchpad", c_scratchpad, "scratchpad [cmd]  (show/hide the drop-down; starts cmd the first time)" },
	{ "fullscreen", c_fullscreen, "fullscreen  (toggle focused window)" },
	{ "mfact",      c_mfact,      "mfact 0.6|+0.05|-0.05" },
	{ "nmaster",    c_nmaster,    "nmaster 2|+1|-1" },
	{ "set",        c_set,        "set <key> <value>" },
	{ "bind",       c_bind,       "bind <mod+key> <command>" },
	{ "unbind",     c_unbind,     "unbind <mod+key>" },
	{ "rule",       c_rule,       "rule class=X [instance=X] [title=X] [float|tile|scratchpad|terminal|noswallow] [tag N] [monitor N]" },
	{ "reload",     c_reload,     "reload  (re-read niwmrc)" },
	{ "quit",       c_quit,       "quit" },
	{ "query",      c_query,      "query  (state for bars and scripts)" },
	{ "clients",    c_clients,    "clients  (list managed windows)" },
	{ "subscribe",  c_subscribe,  "subscribe  (stream state changes, for bars)" },
	{ "emit",       c_emit,       "emit <text>  (send \"event <text>\" to subscribers)" },
	{ "version",    c_version,    "version" },
	{ "help",       c_help,       "help" },
};

static int c_help(char *a)
{
	size_t i;

	(void)a;
	for (i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
		say("%s\n", cmds[i].help);
	return 0;
}

int core_cmd(const char *line, char *out, size_t outlen)
{
	char buf[1024], *cmd, *arg, *end;
	char *sb = obuf;
	size_t sl = olen, sp = opos;
	size_t i;
	int r = -1;

	/* commands nest (reload runs the config), so save the reply state */
	obuf = out;
	olen = outlen;
	opos = 0;
	if (out && outlen)
		out[0] = '\0';

	snprintf(buf, sizeof buf, "%s", line);
	cmd = buf + strspn(buf, " \t\r\n");
	end = cmd + strlen(cmd);
	while (end > cmd && isspace((unsigned char)end[-1]))
		*--end = '\0';
	if (!*cmd || *cmd == '#') {
		r = 0;
		goto done;
	}
	arg = cmd + strcspn(cmd, " \t");
	if (*arg) {
		*arg++ = '\0';
		arg += strspn(arg, " \t");
	}
	for (i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
		if (!strcmp(cmd, cmds[i].name)) {
			r = cmds[i].fn(arg);
			goto done;
		}
	}
	r = fail("unknown command '%s' (try: niwmctl help)", cmd);
done:
	obuf = sb;
	olen = sl;
	opos = sp;
	return r;
}
