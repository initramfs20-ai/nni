/*
 * nibar - a vertical taskbar for NiWM.
 *
 * Top to bottom: an applications menu, the tags, the layout, the
 * window list, then battery, network and a clock at the bottom.
 *
 * It follows NiWM over `subscribe`, so it never polls the window
 * manager, and it takes its accent colours from NiWM's border colours
 * so the two always match. Only libxcb is used; text is drawn with
 * the X server's own core fonts, so no freetype or fontconfig.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <xcb/xcb.h>

#include "../core/sockpath.h"

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

#define NIBAR_VERSION "0.4.0"
#define MAX_TASKS 64
#define MAX_HITS 128
#define PAD 10

/* ---- settings -------------------------------------------------------- */

/* Everything here can be set in nibarc; command-line flags win over it.
 * conf_defaults() lists the defaults. */
enum { LAYER_BELOW, LAYER_ABOVE, LAYER_BESIDE };
#define MAX_FILTER 16

static int bar_w, on_right, all_tags, layer, my_mon; /* my_mon: 0 = first */
static int show_logo, show_battery, show_network, clock_mode; /* 0 off, 12, 24 */
static int tray_enabled, tray_size;
static char font_name[256], label[32], date_fmt[48];
static char tray_hide[MAX_FILTER][64], tray_show[MAX_FILTER][64];
static int nhide, nshow;
static uint32_t col_bg, col_fg;
static int own_accent, own_border, own_urgent;   /* 0: follow niwm ("auto") */
static int own_logo;                             /* 0: the chicken is foreground-coloured */
static uint32_t my_logo;
static uint32_t my_accent, my_border, my_urgent;

/* niwm's border colours, used for "auto" */
static uint32_t niwm_focus = 0xc23b3b, niwm_normal = 0x3a3a3a, niwm_urgent = 0xe0a030;

/* derived */
static uint32_t col_hi, col_dim, col_line, col_accent, col_urgent;
static const uint32_t col_amber = 0xe0a030;

static int g_argc;
static char **g_argv;
static const char *conf_arg; /* -C path */
static int scr_num;

/* ---- niwm state ------------------------------------------------------ */

typedef struct {
	unsigned long id;
	uint32_t tags;
	int mon;
	int focused, floating, urgent;
	char cls[64], title[96];
} task;

#define MAX_MONS 8
static struct {
	int ntags;
	uint32_t view, occ, urg;
	char layout[16];
	int nmons;
	struct {
		int x, y, w, h;
		uint32_t view, occ, urg;
		char layout[16];
	} mons[MAX_MONS];
	int ntasks;
	task tasks[MAX_TASKS];
} st, stage;

static int sub_fd = -1;
static char rbuf[32768];
static size_t rlen;

/* ---- X --------------------------------------------------------------- */

static xcb_connection_t *conn;
static xcb_screen_t *scr;
static xcb_window_t root, bar;
static xcb_pixmap_t pix;
static xcb_gcontext_t gc;
static xcb_font_t font;
static int fw, fasc, fdesc, fh, font16;
static int scr_w, scr_h;
/* the monitor the bar sits on (the whole screen until niwm says more) */
static int mon_x, mon_y, mon_w, mon_h, bar_hidden;
static xcb_keycode_t kmin, kmax;
static int kper;
static xcb_keysym_t *kmap;

enum { H_MENU, H_TAG, H_LAYOUT, H_TASK };
static struct hit {
	int x, y, w, h, kind, arg;
} hits[MAX_HITS];
static int nhits, hover = -1;

/* ---- status ------------------------------------------------------------ */

static int bat_pct = -1, bat_chg, wifi_pct = -1, wired;
static char wifi_if[16];

static volatile sig_atomic_t quit_flag, menu_flag, reload_flag;

/* ---- menu ----------------------------------------------------------- */

typedef struct {
	char *name, *exec;
	int cat, term, niwm; /* niwm: exec is a niwm command */
} app;

static const char *cat_names[] = {
	"Accessories", "Development", "Education", "Games", "Graphics",
	"Internet", "Multimedia", "Office", "Science", "Settings", "System",
	"Other", "NiWM",
};
#define CAT_OTHER 11
#define CAT_NIWM 12

static struct {
	int open;
	xcb_window_t win;
	xcb_pixmap_t pix;
	int x, y, w, h, rows, rh;
	app *apps;
	int napps;
	int *view, nview; /* >= 0: app index, < 0: -1 - category header */
	int sel, top;
	char query[64];
} menu;

/* ==== helpers ========================================================= */

static uint32_t mix(uint32_t a, uint32_t b, int pct) /* pct of b */
{
	uint32_t r = 0;
	int s;

	for (s = 0; s < 24; s += 8) {
		int ca = (a >> s) & 0xff, cb = (b >> s) & 0xff;

		r |= (uint32_t)((ca * (100 - pct) + cb * pct) / 100) << s;
	}
	return r;
}

static int parse_color(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;

	if (*s == '#')
		s++;
	v = strtoul(s, &end, 16);
	if (strlen(s) != 6 || *end)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static void spawn(const char *cmd)
{
	if (fork() == 0) {
		signal(SIGCHLD, SIG_DFL);
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

/* one-shot command to niwm; the reply is not needed */
static void niwm_cmd(const char *fmt, ...)
{
	struct sockaddr_un sa;
	char line[256];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n <= 0 || n >= (int)sizeof line - 1)
		return;
	line[n++] = '\n';
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	niwm_sockpath(sa.sun_path, sizeof sa.sun_path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0)
		send(fd, line, (size_t)n, MSG_NOSIGNAL);
	close(fd);
}

static void recompute_colors(void);

/* ==== nibarc ========================================================== */

static void conf_defaults(void)
{
	bar_w = 150;
	on_right = all_tags = my_mon = 0;
	layer = LAYER_BELOW;
	show_logo = show_battery = show_network = 1;
	clock_mode = 24;
	tray_enabled = 1;
	tray_size = 18;
	font_name[0] = '\0';
	snprintf(label, sizeof label, "Apps");
	snprintf(date_fmt, sizeof date_fmt, "%%a %%d %%b");
	nhide = nshow = 0;
	col_bg = 0x161214;
	col_fg = 0xe8dcd8;
	own_accent = own_border = own_urgent = own_logo = 0;
}

static void conf_err(const char *src, int ln, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "nibar: %s:%d: ", src, ln);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int yesno(const char *v)
{
	if (!strcmp(v, "yes") || !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "1"))
		return 1;
	if (!strcmp(v, "no") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "0"))
		return 0;
	return -1;
}

/* "auto" -> follow niwm, else a colour */
static int auto_or_color(const char *v, int *own, uint32_t *col)
{
	if (!strcmp(v, "auto")) {
		*own = 0;
		return 0;
	}
	if (parse_color(v, col) < 0)
		return -1;
	*own = 1;
	return 0;
}

static void add_filter(char list[][64], int *n, char *names, const char *src, int ln)
{
	char *tok, *save = NULL;

	for (tok = strtok_r(names, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (*n >= MAX_FILTER) {
			conf_err(src, ln, "at most %d tray names per list", MAX_FILTER);
			return;
		}
		snprintf(list[(*n)++], 64, "%s", tok);
	}
}

static void conf_line(const char *src, int ln, char *line)
{
	char *key = line + strspn(line, " \t"), *val, *end;
	int b;

	end = key + strlen(key);
	while (end > key && isspace((unsigned char)end[-1]))
		*--end = '\0';
	if (!*key || *key == '#')
		return;
	val = key + strcspn(key, " \t");
	if (*val) {
		*val++ = '\0';
		val += strspn(val, " \t");
	}
	/* "width 150   # a note": a # with a space before and after starts a
	 * comment. Colours like #161214 have no space after the #. */
	for (end = val; *end; end++) {
		if (*end == '#' && end > val && isspace((unsigned char)end[-1]) &&
		    (!end[1] || isspace((unsigned char)end[1]))) {
			*end = '\0';
			while (end > val && isspace((unsigned char)end[-1]))
				*--end = '\0';
			break;
		}
	}
	if (!*val) {
		conf_err(src, ln, "'%s' needs a value", key);
		return;
	}

	if (!strcmp(key, "position")) {
		if (!strcmp(val, "left") || !strcmp(val, "right"))
			on_right = val[0] == 'r';
		else
			conf_err(src, ln, "position is left or right");
	} else if (!strcmp(key, "width")) {
		int w = atoi(val);

		if (w < 80 || w > 600)
			conf_err(src, ln, "width is 80 to 600");
		else
			bar_w = w;
	} else if (!strcmp(key, "monitor")) {
		int n = atoi(val);

		if (n < 1 || n > MAX_MONS)
			conf_err(src, ln, "monitor is 1 (the primary) to %d", MAX_MONS);
		else
			my_mon = n - 1;
	} else if (!strcmp(key, "font")) {
		snprintf(font_name, sizeof font_name, "%s", val);
	} else if (!strcmp(key, "background") || !strcmp(key, "foreground")) {
		if (parse_color(val, key[0] == 'b' ? &col_bg : &col_fg) < 0)
			conf_err(src, ln, "colours look like #161214");
	} else if (!strcmp(key, "accent")) {
		if (auto_or_color(val, &own_accent, &my_accent) < 0)
			conf_err(src, ln, "accent is auto or a colour like #c23b3b");
	} else if (!strcmp(key, "border")) {
		if (auto_or_color(val, &own_border, &my_border) < 0)
			conf_err(src, ln, "border is auto or a colour like #3a3a3a");
	} else if (!strcmp(key, "logo_color")) {
		if (auto_or_color(val, &own_logo, &my_logo) < 0)
			conf_err(src, ln, "logo_color is auto or a colour like #ffd640");
	} else if (!strcmp(key, "urgent")) {
		if (auto_or_color(val, &own_urgent, &my_urgent) < 0)
			conf_err(src, ln, "urgent is auto or a colour like #e0a030");
	} else if (!strcmp(key, "layer")) {
		if (!strcmp(val, "below"))
			layer = LAYER_BELOW;
		else if (!strcmp(val, "above"))
			layer = LAYER_ABOVE;
		else if (!strcmp(val, "beside"))
			layer = LAYER_BESIDE;
		else
			conf_err(src, ln, "layer is below, above or beside");
	} else if (!strcmp(key, "windows")) {
		if (!strcmp(val, "viewed") || !strcmp(val, "all"))
			all_tags = val[0] == 'a';
		else
			conf_err(src, ln, "windows is viewed or all");
	} else if (!strcmp(key, "label")) {
		snprintf(label, sizeof label, "%s", val);
	} else if (!strcmp(key, "clock")) {
		if (!strcmp(val, "24h"))
			clock_mode = 24;
		else if (!strcmp(val, "12h"))
			clock_mode = 12;
		else if (!strcmp(val, "off"))
			clock_mode = 0;
		else
			conf_err(src, ln, "clock is 24h, 12h or off");
	} else if (!strcmp(key, "date")) {
		if (!strcmp(val, "off"))
			date_fmt[0] = '\0';
		else
			snprintf(date_fmt, sizeof date_fmt, "%s", val);
	} else if (!strcmp(key, "tray_size")) {
		int n = atoi(val);

		if (n < 12 || n > 48)
			conf_err(src, ln, "tray_size is 12 to 48");
		else
			tray_size = n;
	} else if (!strcmp(key, "tray_hide")) {
		add_filter(tray_hide, &nhide, val, src, ln);
	} else if (!strcmp(key, "tray_show")) {
		add_filter(tray_show, &nshow, val, src, ln);
	} else if (!strcmp(key, "logo") || !strcmp(key, "battery") ||
	           !strcmp(key, "network") || !strcmp(key, "tray")) {
		if ((b = yesno(val)) < 0) {
			conf_err(src, ln, "%s is yes or no", key);
			return;
		}
		if (key[0] == 'l') show_logo = b;
		else if (key[0] == 'b') show_battery = b;
		else if (key[0] == 'n') show_network = b;
		else tray_enabled = b;
	} else {
		conf_err(src, ln, "unknown option '%s'", key);
	}
}

static FILE *open_conf(char *path, size_t len)
{
	const char *e;
	FILE *f;

	if (conf_arg) {
		snprintf(path, len, "%s", conf_arg);
		return fopen(path, "r");
	}
	if ((e = getenv("XDG_CONFIG_HOME")) && *e) {
		snprintf(path, len, "%s/niwm/nibarc", e);
		if ((f = fopen(path, "r")))
			return f;
	}
	if ((e = getenv("HOME")) && *e) {
		snprintf(path, len, "%s/.config/niwm/nibarc", e);
		if ((f = fopen(path, "r")))
			return f;
	}
	snprintf(path, len, "%s/niwm/nibarc", SYSCONFDIR);
	return fopen(path, "r");
}

static void usage(void);

/* Flags override nibarc, so they are applied after it on every load. */
static void apply_args(void)
{
	int i;

	for (i = 1; i < g_argc; i++) {
		const char *a = g_argv[i], *next = i + 1 < g_argc ? g_argv[i + 1] : NULL;

		if (!strcmp(a, "-r")) {
			on_right = 1;
		} else if (!strcmp(a, "-a")) {
			all_tags = 1;
		} else if (!strcmp(a, "-C") && next) {
			i++;
		} else if (!strcmp(a, "-m") && next) {
			my_mon = atoi(next) - 1;
			my_mon = my_mon < 0 ? 0 : my_mon >= MAX_MONS ? MAX_MONS - 1 : my_mon;
			i++;
		} else if (!strcmp(a, "-w") && next) {
			bar_w = atoi(next);
			bar_w = bar_w < 80 ? 80 : bar_w > 600 ? 600 : bar_w;
			i++;
		} else if (!strcmp(a, "-f") && next) {
			snprintf(font_name, sizeof font_name, "%s", next);
			i++;
		} else if (!strcmp(a, "-b") && next) {
			if (parse_color(next, &col_bg) < 0)
				usage();
			i++;
		} else if (!strcmp(a, "-c") && next) {
			if (parse_color(next, &col_fg) < 0)
				usage();
			i++;
		} else {
			usage();
		}
	}
}

/* "nibar.background: #102030" in the X resources (xrdb) works like that line
 * in nibarc, applied before nibarc. Desktop themes set colours this way;
 * nibarc and flags still win. */
static void load_resources(void)
{
	xcb_get_property_reply_t *r;
	xcb_window_t rw;
	char *db, *line, *save = NULL;
	int len, n = 0;

	if (!conn)
		return;
	rw = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
	r = xcb_get_property_reply(conn, xcb_get_property(conn, 0, rw, XCB_ATOM_RESOURCE_MANAGER,
	                           XCB_ATOM_STRING, 0, 1 << 18), NULL);
	if (!r || (len = xcb_get_property_value_length(r)) <= 0 || !(db = malloc((size_t)len + 1))) {
		free(r);
		return;
	}
	memcpy(db, xcb_get_property_value(r), (size_t)len);
	db[len] = '\0';
	free(r);
	for (line = strtok_r(db, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *c;

		n++;
		if (strncmp(line, "nibar.", 6) || !(c = strchr(line, ':')))
			continue;
		*c = ' '; /* "nibar.width: 180" -> "width  180" */
		conf_line("X resources", n, line + 6);
	}
	free(db);
}

static void load_config(void)
{
	char path[512], line[1024];
	int ln = 0;
	FILE *f;

	conf_defaults();
	load_resources();
	if ((f = open_conf(path, sizeof path))) {
		while (fgets(line, sizeof line, f))
			conf_line(path, ++ln, line);
		fclose(f);
	} else if (conf_arg) {
		fprintf(stderr, "nibar: can't read %s\n", conf_arg);
	}
	apply_args();
	recompute_colors();
}

/* ==== text ============================================================ */

/* UTF-8 -> code points (at most max). Bad bytes become '?'. */
static int utf8_decode(const char *s, uint32_t *out, int max)
{
	const unsigned char *p = (const unsigned char *)s;
	int n = 0;

	while (*p && n < max) {
		uint32_t c = *p++;
		int more = 0;

		if (c >= 0xf0 && c < 0xf8) { c &= 0x07; more = 3; }
		else if (c >= 0xe0) { c &= 0x0f; more = 2; }
		else if (c >= 0xc0) { c &= 0x1f; more = 1; }
		else if (c >= 0x80) { c = '?'; }
		while (more-- > 0) {
			if ((*p & 0xc0) != 0x80) { c = '?'; break; }
			c = (c << 6) | (*p++ & 0x3f);
		}
		out[n++] = c;
	}
	return n;
}

static void set_colors(uint32_t fg, uint32_t bg)
{
	uint32_t v[2] = { fg, bg };

	xcb_change_gc(conn, gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, v);
}

static void fill(xcb_drawable_t d, uint32_t color, int x, int y, int w, int h)
{
	xcb_rectangle_t r = { (int16_t)x, (int16_t)y, (uint16_t)(w > 0 ? w : 0),
	                      (uint16_t)(h > 0 ? h : 0) };

	set_colors(color, color);
	xcb_poly_fill_rectangle(conn, d, gc, 1, &r);
}

/* Draw text with its baseline at y, cut to maxw pixels ("~" marks a cut).
 * Returns the width drawn. */
static int text(xcb_drawable_t d, int x, int y, const char *s, uint32_t fg,
                uint32_t bg, int maxw)
{
	uint32_t cp[256];
	int n = utf8_decode(s, cp, 255), max = maxw / fw, i;

	if (max <= 0 || n == 0)
		return 0;
	if (n > max) {
		n = max;
		cp[n - 1] = font16 ? 0x2026 : '~';
	}
	set_colors(fg, bg);
	if (font16) {
		xcb_char2b_t c2[256];

		for (i = 0; i < n; i++) {
			c2[i].byte1 = (uint8_t)(cp[i] > 0xffff ? 0 : cp[i] >> 8);
			c2[i].byte2 = (uint8_t)(cp[i] > 0xffff ? '?' : cp[i] & 0xff);
		}
		xcb_image_text_16(conn, (uint8_t)n, d, gc, (int16_t)x, (int16_t)y, c2);
	} else {
		char b[256];

		for (i = 0; i < n; i++)
			b[i] = (char)(cp[i] > 0xff ? '?' : cp[i]);
		xcb_image_text_8(conn, (uint8_t)n, d, gc, (int16_t)x, (int16_t)y, b);
	}
	return n * fw;
}

static int text_w(const char *s)
{
	uint32_t cp[256];

	return utf8_decode(s, cp, 255) * fw;
}

/* baseline that centres a line of text in a box of height h at y */
static int mid(int y, int h)
{
	return y + (h + fasc - fdesc) / 2;
}

static int open_font(const char *name)
{
	xcb_query_text_extents_reply_t *r;
	xcb_generic_error_t *err;
	xcb_char2b_t m = { 0, 'M' };

	font = xcb_generate_id(conn);
	err = xcb_request_check(conn, xcb_open_font_checked(conn, font,
	                        (uint16_t)strlen(name), name));
	if (err) {
		free(err);
		return -1;
	}
	r = xcb_query_text_extents_reply(conn, xcb_query_text_extents(conn, font, 1, &m), NULL);
	if (!r || r->overall_width <= 0) {
		free(r);
		xcb_close_font(conn, font);
		return -1;
	}
	fw = r->overall_width;
	fasc = r->font_ascent;
	fdesc = r->font_descent;
	fh = fasc + fdesc;
	free(r);
	font16 = strstr(name, "10646") != NULL;
	return 0;
}

static void load_font(void)
{
	static const char *fallback[] = {
		"-misc-fixed-medium-r-semicondensed--13-*-*-*-*-*-iso10646-1",
		"-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso10646-1",
		"6x13", "fixed",
	};
	size_t i;

	if (font_name[0] && open_font(font_name) == 0)
		return;
	if (font_name[0])
		fprintf(stderr, "nibar: can't open font '%s', using the default\n", font_name);
	for (i = 0; i < sizeof fallback / sizeof fallback[0]; i++)
		if (open_font(fallback[i]) == 0)
			return;
	fprintf(stderr, "nibar: no usable font\n");
	exit(1);
}

/* ==== pixel art ========================================================= */

/* The nni chicken. r comb, W body, e eye, y beak and feet. */
static const char *chicken[12] = {
	"......rr....",
	".....rrrr...",
	".....WWWW...",
	"....WWWeWy..",
	"....WWWWWyy.",
	"......WWWW..",
	"..W..WWWWWW.",
	"..WWWWWWWWWW",
	"..WWWWWWWWWW",
	"...WWWWWWWW.",
	"....y...y...",
	"....yy..yy..",
};

static void draw_chicken(xcb_drawable_t d, int x, int y, int scale, uint32_t bg)
{
	int r, c;

	for (r = 0; r < 12; r++) {
		for (c = 0; c < 12; c++) {
			uint32_t col;

			switch (chicken[r][c]) {
			case 'r': col = col_accent; break;
			case 'W': col = own_logo ? my_logo : col_fg; break;
			case 'e': col = bg; break;
			case 'y': col = col_amber; break;
			default: continue;
			}
			fill(d, col, x + c * scale, y + r * scale, scale, scale);
		}
	}
}

/* 3x5 digits for the clock */
static const char *digits[11] = {
	"111101101101111", "010110010010111", "111001111100111",
	"111001111001111", "101101111001001", "111100111001111",
	"111100111101111", "111001010010010", "111101111101111",
	"111101111001111", "010010000010010" /* ':' drawn 1 wide */
};

static int clock_w(const char *s, int scale)
{
	int w = 0;

	for (; *s; s++)
		w += (*s == ':' ? 1 : 3) * scale + scale;
	return w - scale;
}

static void draw_clock(xcb_drawable_t d, int x, int y, const char *s, int scale)
{
	int r, c;

	for (; *s; s++) {
		int colon = *s == ':', g = colon ? 10 : *s - '0';

		if (g < 0 || g > 10)
			continue;
		for (r = 0; r < 5; r++)
			for (c = 0; c < 3; c++)
				if (digits[g][r * 3 + c] == '1')
					fill(d, colon ? col_accent : col_fg,
					     x + (colon ? 0 : c) * scale, y + r * scale, scale, scale);
		x += (colon ? 1 : 3) * scale + scale;
	}
}

/* ==== status ============================================================== */

static int read_line(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f))
		buf[0] = '\0';
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static void read_status(void)
{
	char path[300], buf[256];
	struct dirent *de;
	DIR *dir;
	FILE *f;

	bat_pct = -1;
	if ((dir = opendir("/sys/class/power_supply"))) {
		while ((de = readdir(dir))) {
			if (strncmp(de->d_name, "BAT", 3))
				continue;
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", de->d_name);
			if (read_line(path, buf, sizeof buf) < 0)
				continue;
			bat_pct = atoi(buf);
			snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", de->d_name);
			bat_chg = read_line(path, buf, sizeof buf) == 0 &&
			          (!strcmp(buf, "Charging") || !strcmp(buf, "Full"));
			break;
		}
		closedir(dir);
	}

	wifi_pct = -1;
	wired = 0;
	if ((f = fopen("/proc/net/wireless", "r"))) {
		int line = 0;

		while (fgets(buf, sizeof buf, f)) {
			char *colon = strchr(buf, ':'), *name = buf;
			double link;

			if (++line <= 2 || !colon)
				continue;
			*colon = '\0';
			while (*name == ' ')
				name++;
			if (sscanf(colon + 1, "%*s %lf", &link) == 1) {
				wifi_pct = (int)(link * 100 / 70);
				wifi_pct = wifi_pct < 0 ? 0 : wifi_pct > 100 ? 100 : wifi_pct;
				snprintf(wifi_if, sizeof wifi_if, "%.15s", name);
				break;
			}
		}
		fclose(f);
	}
	if (wifi_pct < 0 && (dir = opendir("/sys/class/net"))) {
		while ((de = readdir(dir))) {
			if (de->d_name[0] == '.' || !strcmp(de->d_name, "lo"))
				continue;
			snprintf(path, sizeof path, "/sys/class/net/%s/operstate", de->d_name);
			if (read_line(path, buf, sizeof buf) == 0 && !strcmp(buf, "up"))
				wired = 1;
		}
		closedir(dir);
	}
}

/* ==== drawing the bar ==================================================== */

static void add_hit(int x, int y, int w, int h, int kind, int arg)
{
	if (nhits < MAX_HITS)
		hits[nhits++] = (struct hit){ x, y, w, h, kind, arg };
}

static int is_hover(int kind, int arg)
{
	return hover >= 0 && hover < nhits && hits[hover].kind == kind && hits[hover].arg == arg;
}

static const char *layout_icon(const char *l)
{
	if (!strcmp(l, "tile")) return "[]=";
	if (!strcmp(l, "monocle")) return "[M]";
	if (!strcmp(l, "grid")) return "###";
	if (!strcmp(l, "float")) return "><>";
	return "???";
}

static void gauge(int x, int y, int w, int pct, uint32_t col)
{
	fill(pix, col_line, x, y, w, 6);
	fill(pix, col_bg, x + 1, y + 1, w - 2, 4);
	fill(pix, col, x + 1, y + 1, (w - 2) * pct / 100, 4);
}

static void status_row(int y, const char *label, const char *val, int pct, uint32_t col)
{
	int gw = bar_w / 3;

	text(pix, PAD, mid(y, 20), label, col_dim, col_bg, bar_w);
	text(pix, PAD + 5 * fw, mid(y, 20), val, col_fg, col_bg, bar_w);
	if (pct >= 0)
		gauge(bar_w - PAD - gw, y + 7, gw, pct, col);
}

static int tray_height(int inner);
static void tray_place(int top, int inner);

static void draw_bar(void)
{
	int y = PAD, W = bar_w, inner = bar_w - 2 * PAD;
	int bottom, i, cols = 3, gap = 4, cw, ch = 22, rows, n, rh = 22;
	char buf[64], tbuf[16];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	nhits = 0;
	fill(pix, col_bg, 0, 0, W, mon_h);

	/* applications button: the chicken */
	{
		int h = 36, hov = is_hover(H_MENU, 0) || menu.open;
		uint32_t bg = hov ? col_hi : col_bg;

		fill(pix, bg, PAD / 2, y, W - PAD, h);
		if (menu.open)
			fill(pix, col_accent, PAD / 2, y, 3, h);
		if (show_logo) {
			draw_chicken(pix, PAD, y + 6, 2, bg);
			text(pix, PAD + 32, mid(y, h), label, col_fg, bg, W - PAD - 40);
		} else {
			text(pix, PAD, mid(y, h), label, col_fg, bg, inner);
		}
		add_hit(0, y, W, h, H_MENU, 0);
		y += h + 8;
	}
	fill(pix, col_line, PAD, y, inner, 1);
	y += 9;

	/* tags, 3 per row */
	n = st.ntags ? st.ntags : 9;
	cw = (inner - gap * (cols - 1)) / cols;
	rows = (n + cols - 1) / cols;
	for (i = 0; i < n; i++) {
		int x = PAD + (i % cols) * (cw + gap), ty = y + (i / cols) * (ch + gap);
		uint32_t bit = 1u << i, bg = col_bg, fg = col_dim;

		if (st.view & bit) {
			bg = col_accent;
			fg = col_fg;
		} else if (st.urg & bit) {
			bg = col_urgent;
			fg = col_bg;
		} else if (is_hover(H_TAG, i)) {
			bg = col_hi;
			fg = col_fg;
		} else if (st.occ & bit) {
			fg = col_fg;
		}
		fill(pix, (st.view & bit) || (st.urg & bit) ? bg : col_line, x, ty, cw, ch);
		if (!(st.view & bit) && !(st.urg & bit))
			fill(pix, bg, x + 1, ty + 1, cw - 2, ch - 2);
		snprintf(buf, sizeof buf, "%d", i + 1);
		text(pix, x + (cw - fw) / 2, mid(ty, ch), buf, fg, bg, cw);
		if ((st.occ & bit) && !(st.view & bit))
			fill(pix, st.urg & bit ? col_bg : col_fg, x + cw / 2 - 3, ty + ch - 5, 6, 2);
		add_hit(x, ty, cw, ch, H_TAG, i);
	}
	y += rows * (ch + gap) + 4;

	/* layout */
	{
		uint32_t bg = is_hover(H_LAYOUT, 0) ? col_hi : col_bg;

		fill(pix, bg, PAD / 2, y, W - PAD, 22);
		snprintf(buf, sizeof buf, "%s  %s", layout_icon(st.layout), st.layout);
		text(pix, PAD, mid(y, 22), sub_fd >= 0 ? buf : "niwm not running",
		     sub_fd >= 0 ? col_fg : col_dim, bg, inner);
		add_hit(0, y, W, 22, H_LAYOUT, 0);
		y += 30;
	}
	fill(pix, col_line, PAD, y, inner, 1);
	y += 8;

	/* bottom block, laid out upwards */
	bottom = mon_h - PAD;
	if (date_fmt[0] && strftime(buf, sizeof buf, date_fmt, tm)) {
		text(pix, (W - text_w(buf)) / 2, bottom - fdesc, buf, col_dim, col_bg, inner);
		bottom -= fh + 8;
	}
	if (clock_mode) {
		int cw2, cx;
		const char *ampm = tm->tm_hour < 12 ? "am" : "pm";

		strftime(tbuf, sizeof tbuf, clock_mode == 12 ? "%I:%M" : "%H:%M", tm);
		if (clock_mode == 12 && tbuf[0] == '0')
			memmove(tbuf, tbuf + 1, strlen(tbuf));
		cw2 = clock_w(tbuf, 4) + (clock_mode == 12 ? 4 + 2 * fw : 0);
		cx = (W - cw2) / 2;
		draw_clock(pix, cx, bottom - 20, tbuf, 4);
		if (clock_mode == 12)
			text(pix, cx + clock_w(tbuf, 4) + 4, bottom - fdesc, ampm, col_dim, col_bg, 2 * fw);
		bottom -= 20 + 12;
	}
	if (date_fmt[0] || clock_mode) {
		fill(pix, col_line, PAD, bottom, inner, 1);
		bottom -= 4;
	}
	if (show_network) {
		bottom -= 20;
		if (wifi_pct >= 0) {
			snprintf(buf, sizeof buf, "%d%%", wifi_pct);
			status_row(bottom, "wifi", buf, wifi_pct, col_fg);
		} else {
			status_row(bottom, "net", wired ? "up" : "down", -1, col_fg);
		}
	}
	if (show_battery && bat_pct >= 0) {
		bottom -= 20;
		snprintf(buf, sizeof buf, "%d%%%s", bat_pct, bat_chg ? "+" : "");
		status_row(bottom, "bat", buf, bat_pct,
		           bat_pct <= 15 && !bat_chg ? col_urgent : bat_chg ? col_accent : col_fg);
	}
	/* tray icons sit just above the status rows */
	{
		int th = tray_height(inner);

		if (th) {
			bottom -= th + 6;
			tray_place(bottom + 4, inner);
		}
	}
	bottom -= 8;
	fill(pix, col_line, PAD, bottom, inner, 1);

	/* window list fills the middle */
	{
		int shown = 0, total = 0, space = (bottom - 8 - y) / rh;

		for (i = 0; i < st.ntasks; i++)
			if (st.tasks[i].tags && (!st.nmons || st.tasks[i].mon == my_mon) &&
			    (all_tags || (st.tasks[i].tags & st.view)))
				total++;
		for (i = 0; i < st.ntasks && shown < space; i++) {
			task *t = &st.tasks[i];
			int hov = is_hover(H_TASK, i), tw = inner - 4;
			uint32_t bg = t->focused || hov ? col_hi : col_bg;
			uint32_t fg = t->urgent ? col_urgent : t->focused ? col_fg
			            : (t->tags & st.view) ? mix(col_fg, col_bg, 20) : col_dim;
			const char *name = t->title[0] ? t->title : t->cls;

			if (!t->tags || (!all_tags && !(t->tags & st.view)))
				continue; /* tags 0: the hidden scratchpad */
			if (st.nmons && t->mon != my_mon)
				continue; /* another monitor's window */
			if (shown == space - 1 && total > space) {
				snprintf(buf, sizeof buf, "+%d more", total - shown);
				text(pix, PAD + 4, mid(y, rh), buf, col_dim, col_bg, tw);
				break;
			}
			fill(pix, bg, 0, y, W, rh);
			if (t->focused)
				fill(pix, col_accent, on_right ? W - 3 : 0, y, 3, rh);
			if (t->floating)
				fill(pix, fg, PAD - 4, y + rh / 2 - 1, 2, 2);
			if (all_tags) {
				int k;

				for (k = 0; k < 9 && !(t->tags & (1u << k)); k++)
					;
				snprintf(tbuf, sizeof tbuf, "%d", k + 1);
				text(pix, W - PAD - fw, mid(y, rh), tbuf, col_dim, bg, fw);
				tw -= 2 * fw;
			}
			text(pix, PAD + 4, mid(y, rh), name, fg, bg, tw);
			add_hit(0, y, W, rh, H_TASK, i);
			y += rh;
			shown++;
		}
		if (!total && sub_fd >= 0)
			text(pix, PAD + 4, mid(y, rh), "no windows", col_dim, col_bg, inner);
	}

	/* the edge facing the windows echoes niwm's border colour */
	fill(pix, col_line, on_right ? 0 : W - 1, 0, 1, mon_h);

	xcb_copy_area(conn, pix, bar, gc, 0, 0, 0, 0, (uint16_t)W, (uint16_t)mon_h);
}

/* ==== niwm subscription ================================================== */

static void sub_connect(void)
{
	struct sockaddr_un sa;
	int fd;

	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	niwm_sockpath(sa.sun_path, sizeof sa.sun_path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
	    send(fd, "subscribe\n", 10, MSG_NOSIGNAL) != 10) {
		close(fd);
		return;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	sub_fd = fd;
	rlen = 0;
}

static void sub_drop(void)
{
	if (sub_fd >= 0)
		close(sub_fd);
	sub_fd = -1;
	st.ntasks = 0;
	st.view = st.occ = st.urg = 0;
	st.layout[0] = '\0';
}

static void toggle_menu(void);

static void place_bar(void);

/* Sit on our monitor and show its tags. A bar whose monitor is gone hides
 * until it comes back. */
static void follow_monitor(void)
{
	int x, y, w, h;

	if (st.nmons) {
		if (my_mon >= st.nmons) {
			if (!bar_hidden) {
				xcb_unmap_window(conn, bar);
				bar_hidden = 1;
			}
			return;
		}
		st.view = st.mons[my_mon].view;
		st.occ = st.mons[my_mon].occ;
		st.urg = st.mons[my_mon].urg;
		snprintf(st.layout, sizeof st.layout, "%s", st.mons[my_mon].layout);
		x = st.mons[my_mon].x;
		y = st.mons[my_mon].y;
		w = st.mons[my_mon].w;
		h = st.mons[my_mon].h;
	} else {
		x = y = 0;
		w = scr_w;
		h = scr_h;
	}
	if (x != mon_x || y != mon_y || w != mon_w || h != mon_h) {
		mon_x = x;
		mon_y = y;
		mon_w = w;
		mon_h = h;
		place_bar();
	}
	if (bar_hidden) {
		xcb_map_window(conn, bar);
		bar_hidden = 0;
	}
}

/* With several monitors, select ours first so the command acts on it. */
static void mon_cmd(const char *cmd, int arg)
{
	if (st.nmons > 1)
		niwm_cmd("focusmon %d", my_mon + 1);
	if (arg)
		niwm_cmd("%s %d", cmd, arg);
	else
		niwm_cmd("%s", cmd);
}

static void recompute_colors(void)
{
	col_accent = own_accent ? my_accent : niwm_focus;
	col_line = own_border ? my_border : niwm_normal;
	col_urgent = own_urgent ? my_urgent : niwm_urgent;
	col_hi = mix(col_bg, col_fg, 9);
	col_dim = mix(col_bg, col_fg, 45);
}

static void apply_colors(uint32_t focus, uint32_t normal, uint32_t urgent)
{
	niwm_focus = focus;
	niwm_normal = normal;
	niwm_urgent = urgent;
	recompute_colors();
	if (menu.open) {
		uint32_t v = col_accent;

		xcb_change_window_attributes(conn, menu.win, XCB_CW_BORDER_PIXEL, &v);
	}
}

static void sub_line(char *l)
{
	unsigned a, b, c;

	if (!strcmp(l, "begin")) {
		memset(&stage, 0, sizeof stage);
	} else if (!strcmp(l, "end")) {
		st = stage;
		follow_monitor();
	} else if (!strncmp(l, "tags ", 5)) {
		stage.ntags = atoi(l + 5);
		if (stage.ntags < 1 || stage.ntags > 9)
			stage.ntags = 9;
	} else if (!strncmp(l, "view ", 5)) {
		stage.view = (uint32_t)strtoul(l + 5, NULL, 16);
	} else if (!strncmp(l, "occupied ", 9)) {
		stage.occ = (uint32_t)strtoul(l + 9, NULL, 16);
	} else if (!strncmp(l, "urgent ", 7)) {
		stage.urg = (uint32_t)strtoul(l + 7, NULL, 16);
	} else if (!strncmp(l, "layout ", 7)) {
		snprintf(stage.layout, sizeof stage.layout, "%s", l + 7);
	} else if (sscanf(l, "colors %x %x %x", &a, &b, &c) == 3) {
		apply_colors(a, b, c);
	} else if (!strncmp(l, "monitor ", 8) && stage.nmons < MAX_MONS) {
		int i, x, y, w, h;
		unsigned v, o, u;
		char lt[16];

		if (sscanf(l + 8, "%d %d %d %d %d %x %x %x %15s", &i, &x, &y, &w, &h,
		           &v, &o, &u, lt) == 9 && i == stage.nmons) {
			stage.mons[i].x = x;
			stage.mons[i].y = y;
			stage.mons[i].w = w;
			stage.mons[i].h = h;
			stage.mons[i].view = v;
			stage.mons[i].occ = o;
			stage.mons[i].urg = u;
			snprintf(stage.mons[i].layout, sizeof stage.mons[i].layout, "%s", lt);
			stage.nmons++;
		}
	} else if (!strncmp(l, "client ", 7) && stage.ntasks < MAX_TASKS) {
		task *t = &stage.tasks[stage.ntasks];
		char flags[8] = "", *tab;
		int off = 0;

		if (sscanf(l + 7, "%lx %x %7s %d %n", &t->id, &t->tags, flags, &t->mon, &off) < 4 ||
		    !off)
			return;
		t->focused = strchr(flags, 'f') != NULL;
		t->floating = strchr(flags, 'F') != NULL;
		t->urgent = strchr(flags, 'u') != NULL;
		tab = strchr(l + 7 + off, '\t');
		if (tab)
			*tab = '\0';
		snprintf(t->cls, sizeof t->cls, "%s", l + 7 + off);
		snprintf(t->title, sizeof t->title, "%s", tab ? tab + 1 : "");
		stage.ntasks++;
	} else if (!strcmp(l, "event menu")) {
		toggle_menu();
	} else if (!strcmp(l, "event reload")) { /* niwm reloaded: so do we */
		reload_flag = 1;
	}
}

/* returns 1 if something changed */
static int sub_read(void)
{
	int changed = 0;

	for (;;) {
		ssize_t r = read(sub_fd, rbuf + rlen, sizeof rbuf - 1 - rlen);
		char *p, *nl;

		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
			sub_drop();
			return 1;
		}
		if (r < 0)
			break;
		rlen += (size_t)r;
		rbuf[rlen] = '\0';
		p = rbuf;
		while ((nl = strchr(p, '\n'))) {
			*nl = '\0';
			sub_line(p);
			changed = 1;
			p = nl + 1;
		}
		rlen -= (size_t)(p - rbuf);
		memmove(rbuf, p, rlen);
		if (rlen >= sizeof rbuf - 1) /* absurd line: throw it away */
			rlen = 0;
	}
	return changed;
}

/* ==== .desktop files ====================================================== */

static int in_path(const char *prog)
{
	char buf[512];
	const char *path = getenv("PATH"), *p;

	if (strchr(prog, '/'))
		return access(prog, X_OK) == 0;
	if (!path)
		path = "/usr/local/bin:/usr/bin:/bin";
	for (p = path; *p;) {
		size_t n = strcspn(p, ":");

		snprintf(buf, sizeof buf, "%.*s/%.200s", (int)(n > 300 ? 300 : n), p, prog);
		if (access(buf, X_OK) == 0)
			return 1;
		p += n;
		if (*p == ':')
			p++;
	}
	return 0;
}

static int category_of(const char *cats)
{
	static const struct { const char *key; int cat; } map[] = {
		{ "Utility", 0 }, { "Development", 1 }, { "Education", 2 },
		{ "Game", 3 }, { "Graphics", 4 }, { "Network", 5 },
		{ "AudioVideo", 6 }, { "Audio", 6 }, { "Video", 6 },
		{ "Office", 7 }, { "Science", 8 }, { "Settings", 9 },
		{ "System", 10 },
	};
	char buf[512], *tok, *save = NULL;
	size_t i;

	snprintf(buf, sizeof buf, "%s", cats);
	for (tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save))
		for (i = 0; i < sizeof map / sizeof map[0]; i++)
			if (!strcmp(tok, map[i].key))
				return map[i].cat;
	return CAT_OTHER;
}

/* drop %f %U ... field codes; %% becomes % */
static char *clean_exec(const char *s)
{
	char *out = malloc(strlen(s) + 1), *o = out;

	if (!out)
		return NULL;
	while (*s) {
		if (*s == '%' && s[1]) {
			if (s[1] == '%')
				*o++ = '%';
			s += 2;
			continue;
		}
		*o++ = *s++;
	}
	while (o > out && o[-1] == ' ')
		o--;
	*o = '\0';
	return out;
}

static void add_app(const char *name, const char *exec, int cat, int term, int niwm)
{
	app *na;

	if (!(na = realloc(menu.apps, (size_t)(menu.napps + 1) * sizeof *na)))
		return;
	menu.apps = na;
	na[menu.napps].name = strdup(name);
	na[menu.napps].exec = niwm ? strdup(exec) : clean_exec(exec);
	na[menu.napps].cat = cat;
	na[menu.napps].term = term;
	na[menu.napps].niwm = niwm;
	if (na[menu.napps].name && na[menu.napps].exec)
		menu.napps++;
}

static void parse_desktop(const char *path)
{
	char line[1024], name[256] = "", lname[256] = "", exec[1024] = "", cats[512] = "";
	char key1[32] = "", key2[32] = "";
	const char *lang = getenv("LC_MESSAGES");
	int in_entry = 0, term = 0, hidden = 0, is_app = 0;
	FILE *f;

	if (!lang || !*lang)
		lang = getenv("LANG");
	if (lang && *lang && strcmp(lang, "C") && strcmp(lang, "POSIX")) {
		/* es_AR.UTF-8 -> Name[es_AR]= and Name[es]= */
		size_t n = strcspn(lang, ".@");

		snprintf(key1, sizeof key1, "Name[%.*s]=", (int)n, lang);
		snprintf(key2, sizeof key2, "Name[%.*s]=", (int)strcspn(lang, "_.@"), lang);
	}
	if (!(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_entry = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in_entry)
			continue;
		if (!strncmp(line, "Name=", 5))
			snprintf(name, sizeof name, "%.255s", line + 5);
		else if (key1[0] && !strncmp(line, key1, strlen(key1)))
			snprintf(lname, sizeof lname, "%s", line + strlen(key1));
		else if (key2[0] && !lname[0] && !strncmp(line, key2, strlen(key2)))
			snprintf(lname, sizeof lname, "%s", line + strlen(key2));
		else if (!strncmp(line, "Exec=", 5))
			snprintf(exec, sizeof exec, "%s", line + 5);
		else if (!strncmp(line, "Categories=", 11))
			snprintf(cats, sizeof cats, "%.511s", line + 11);
		else if (!strcmp(line, "Terminal=true"))
			term = 1;
		else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true"))
			hidden = 1;
		else if (!strcmp(line, "Type=Application"))
			is_app = 1;
		else if (!strncmp(line, "TryExec=", 8) && !in_path(line + 8))
			hidden = 1;
	}
	fclose(f);
	if (is_app && !hidden && name[0] && exec[0])
		add_app(lname[0] ? lname : name, exec, category_of(cats), term, 0);
}

/* returns 1 if id (a .desktop basename) was already seen */
static int seen_id(char ***ids, int *n, const char *id)
{
	char **na;
	int i;

	for (i = 0; i < *n; i++)
		if (!strcmp((*ids)[i], id))
			return 1;
	if ((na = realloc(*ids, (size_t)(*n + 1) * sizeof *na))) {
		*ids = na;
		if ((na[*n] = strdup(id)))
			(*n)++;
	}
	return 0;
}

static void scan_dir(const char *dir, char ***ids, int *nids)
{
	char path[1024];
	struct dirent *de;
	DIR *d = opendir(dir);

	if (!d)
		return;
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);

		if (n < 9 || strcmp(de->d_name + n - 8, ".desktop"))
			continue;
		if (seen_id(ids, nids, de->d_name)) /* user's copy wins */
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		parse_desktop(path);
	}
	closedir(d);
}

static int app_cmp(const void *a, const void *b)
{
	const app *x = a, *y = b;

	if (x->cat != y->cat)
		return x->cat - y->cat;
	return strcasecmp(x->name, y->name);
}

/* Read the menu fresh every time it opens, so newly installed programs
 * show up without restarting anything. */
static void load_apps(void)
{
	const char *home = getenv("HOME"), *dh = getenv("XDG_DATA_HOME");
	const char *dirs = getenv("XDG_DATA_DIRS"), *p;
	char path[1024], **ids = NULL;
	int nids = 0, i;

	if (dh && *dh)
		snprintf(path, sizeof path, "%s/applications", dh);
	else
		snprintf(path, sizeof path, "%s/.local/share/applications", home ? home : "/");
	scan_dir(path, &ids, &nids);
	if (!dirs || !*dirs)
		dirs = "/usr/local/share:/usr/share";
	for (p = dirs; *p;) {
		size_t n = strcspn(p, ":");

		snprintf(path, sizeof path, "%.*s/applications", (int)n, p);
		scan_dir(path, &ids, &nids);
		p += n;
		if (*p == ':')
			p++;
	}
	for (i = 0; i < nids; i++)
		free(ids[i]);
	free(ids);

	add_app("Reload config", "reload", CAT_NIWM, 0, 1);
	add_app("Quit NiWM", "quit", CAT_NIWM, 0, 1);
	qsort(menu.apps, (size_t)menu.napps, sizeof *menu.apps, app_cmp);
}

static void free_apps(void)
{
	int i;

	for (i = 0; i < menu.napps; i++) {
		free(menu.apps[i].name);
		free(menu.apps[i].exec);
	}
	free(menu.apps);
	free(menu.view);
	menu.apps = NULL;
	menu.view = NULL;
	menu.napps = menu.nview = 0;
}

/* ==== the menu window ====================================================== */

static int ci_contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return 1;
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return 1;
	return 0;
}

static void build_view(void)
{
	int i, last_cat = -1;

	free(menu.view);
	menu.nview = 0;
	if (!(menu.view = malloc((size_t)(2 * menu.napps + 1) * sizeof *menu.view)))
		return;
	for (i = 0; i < menu.napps; i++) {
		app *a = &menu.apps[i];

		if (menu.query[0] && !ci_contains(a->name, menu.query) &&
		    !ci_contains(a->exec, menu.query))
			continue;
		if (!menu.query[0] && a->cat != last_cat) {
			menu.view[menu.nview++] = -1 - a->cat;
			last_cat = a->cat;
		}
		menu.view[menu.nview++] = i;
	}
	/* select the first app */
	menu.sel = -1;
	for (i = 0; i < menu.nview; i++)
		if (menu.view[i] >= 0) {
			menu.sel = i;
			break;
		}
	menu.top = 0;
}

static void keep_visible(void)
{
	if (menu.sel < 0)
		return;
	if (menu.sel < menu.top)
		menu.top = menu.sel;
	/* show the category header above the first app too */
	if (menu.top > 0 && menu.sel == menu.top && menu.view[menu.top - 1] < 0)
		menu.top--;
	if (menu.sel >= menu.top + menu.rows)
		menu.top = menu.sel - menu.rows + 1;
}

static void move_sel(int delta)
{
	int i = menu.sel, step = delta > 0 ? 1 : -1, moved = 0;

	if (i < 0)
		return;
	while (moved < abs(delta)) {
		int j = i + step;

		while (j >= 0 && j < menu.nview && menu.view[j] < 0)
			j += step;
		if (j < 0 || j >= menu.nview)
			break;
		i = j;
		moved++;
	}
	menu.sel = i;
	keep_visible();
}

static void draw_menu(void)
{
	int W = menu.w, H = menu.h, rh = menu.rh, y, i;
	char buf[96];

	fill(menu.pix, col_bg, 0, 0, W, H);

	/* filter line */
	fill(menu.pix, col_hi, 0, 0, W, rh + 8);
	snprintf(buf, sizeof buf, "%s_", menu.query);
	text(menu.pix, PAD, mid(0, rh + 8), ">", col_accent, col_hi, fw);
	text(menu.pix, PAD + 2 * fw, mid(0, rh + 8),
	     menu.query[0] ? buf : "type to search", menu.query[0] ? col_fg : col_dim,
	     col_hi, W - 2 * PAD - 2 * fw);
	y = rh + 12;

	for (i = menu.top; i < menu.nview && y + rh <= H; i++, y += rh) {
		int v = menu.view[i];

		if (v < 0) {
			const char *c = cat_names[-1 - v];
			char up[32];
			size_t k;

			for (k = 0; c[k] && k + 1 < sizeof up; k++)
				up[k] = (char)toupper((unsigned char)c[k]);
			up[k] = '\0';
			text(menu.pix, PAD, mid(y, rh), up, col_dim, col_bg, W - 2 * PAD);
			fill(menu.pix, col_line, PAD + (int)k * fw + 8, y + rh / 2, W - 2 * PAD - (int)k * fw - 8, 1);
		} else {
			uint32_t bg = i == menu.sel ? col_hi : col_bg;

			fill(menu.pix, bg, 0, y, W, rh);
			if (i == menu.sel)
				fill(menu.pix, col_accent, 0, y, 3, rh);
			text(menu.pix, PAD + (menu.query[0] ? 0 : 8), mid(y, rh), menu.apps[v].name,
			     i == menu.sel ? col_fg : mix(col_fg, col_bg, 15), bg, W - 2 * PAD - 8);
		}
	}
	if (!menu.nview)
		text(menu.pix, PAD, mid(y, rh), "nothing found", col_dim, col_bg, W - 2 * PAD);
	xcb_copy_area(conn, menu.pix, menu.win, gc, 0, 0, 0, 0, (uint16_t)W, (uint16_t)H);
}

static void close_menu(void)
{
	if (!menu.open)
		return;
	xcb_ungrab_keyboard(conn, XCB_CURRENT_TIME);
	xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
	xcb_destroy_window(conn, menu.win);
	xcb_free_pixmap(conn, menu.pix);
	free_apps();
	menu.open = 0;
	draw_bar();
}

static void launch(int v)
{
	app *a = &menu.apps[v];
	char cmd[1400];

	if (a->niwm) {
		niwm_cmd("%s", a->exec);
	} else if (a->term) {
		/* same search order as niwm's mod+Return */
		snprintf(cmd, sizeof cmd,
		         "for t in \"$TERMINAL\" x-terminal-emulator xterm st urxvt alacritty foot "
		         "lxterminal xfce4-terminal; do command -v \"$t\" >/dev/null 2>&1 && "
		         "exec \"$t\" -e %s; done", a->exec);
		spawn(cmd);
	} else {
		snprintf(cmd, sizeof cmd, "exec %s", a->exec);
		spawn(cmd);
	}
}

static int grab_input(void)
{
	int tries;

	/* the key that opened us may still be held (niwm grabbed it): retry */
	for (tries = 0; tries < 50; tries++) {
		xcb_grab_keyboard_reply_t *k = xcb_grab_keyboard_reply(conn,
			xcb_grab_keyboard(conn, 1, menu.win, XCB_CURRENT_TIME,
			                  XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
		int ok = k && k->status == XCB_GRAB_STATUS_SUCCESS;

		struct timespec wait = { 0, 10000000 };

		free(k);
		if (ok)
			break;
		nanosleep(&wait, NULL);
	}
	if (tries == 50)
		return -1;
	xcb_grab_pointer(conn, 1, menu.win,
	                 XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION,
	                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
	                 XCB_CURRENT_TIME);
	return 0;
}

static void open_menu(void)
{
	uint32_t v[4];
	int maxh;

	load_apps();
	menu.query[0] = '\0';
	build_view();

	menu.rh = fh + 9;
	menu.w = 34 * fw + 2 * PAD;
	if (menu.w < 240)
		menu.w = 240;
	maxh = mon_h - 2 * PAD - 4;
	menu.rows = (maxh - menu.rh - 12) / menu.rh;
	if (menu.rows > menu.nview + 1)
		menu.rows = menu.nview + 1;
	if (menu.rows < 3)
		menu.rows = 3;
	menu.h = menu.rh + 12 + menu.rows * menu.rh;
	menu.x = on_right ? mon_x + mon_w - bar_w - menu.w - 8 : mon_x + bar_w + 4;
	menu.y = mon_y + PAD;

	menu.win = xcb_generate_id(conn);
	v[0] = col_bg;
	v[1] = col_accent;
	v[2] = 1;
	v[3] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
	       XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS;
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, menu.win, root,
	                  (int16_t)menu.x, (int16_t)menu.y, (uint16_t)menu.w, (uint16_t)menu.h, 2,
	                  XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
	                  XCB_CW_EVENT_MASK, v);
	menu.pix = xcb_generate_id(conn);
	xcb_create_pixmap(conn, scr->root_depth, menu.pix, menu.win,
	                  (uint16_t)menu.w, (uint16_t)menu.h);
	xcb_map_window(conn, menu.win);
	menu.open = 1;
	xcb_flush(conn);
	if (grab_input() < 0) {
		fprintf(stderr, "nibar: couldn't grab the keyboard\n");
		close_menu();
		return;
	}
	draw_menu();
	draw_bar();
}

static void toggle_menu(void)
{
	if (menu.open)
		close_menu();
	else
		open_menu();
}

static int menu_row_at(int y)
{
	int top = menu.rh + 12, i;

	if (y < top)
		return -1;
	i = menu.top + (y - top) / menu.rh;
	return (i < menu.nview && menu.view[i] >= 0) ? i : -1;
}

static void menu_key(xcb_key_press_event_t *e)
{
	int shift = (e->state & XCB_MOD_MASK_SHIFT) != 0, ctrl = (e->state & XCB_MOD_MASK_CONTROL) != 0;
	xcb_keysym_t sym = 0;
	size_t len = strlen(menu.query);

	if (kmap && e->detail >= kmin && e->detail <= kmax) {
		xcb_keysym_t *row = &kmap[(e->detail - kmin) * kper];

		sym = (shift && kper > 1 && row[1]) ? row[1] : row[0];
	}
	switch (sym) {
	case 0xff1b: /* Escape */
		close_menu();
		return;
	case 0xff0d: case 0xff8d: { /* Return, KP_Enter */
		int v = menu.sel >= 0 ? menu.view[menu.sel] : -1;

		if (v >= 0) {
			launch(v);
			close_menu();
		}
		return;
	}
	case 0xff52: move_sel(-1); break;                 /* Up */
	case 0xff54: case 0xff09: move_sel(1); break;     /* Down, Tab */
	case 0xff55: move_sel(-(menu.rows - 1)); break;   /* Page_Up */
	case 0xff56: move_sel(menu.rows - 1); break;      /* Page_Down */
	case 0xff50: move_sel(-menu.nview); break;        /* Home */
	case 0xff57: move_sel(menu.nview); break;         /* End */
	case 0xff08: /* BackSpace: drop one UTF-8 character */
		while (len > 0 && ((unsigned char)menu.query[--len] & 0xc0) == 0x80)
			;
		menu.query[len] = '\0';
		build_view();
		break;
	default:
		if (ctrl && (sym == 'u' || sym == 'U')) {
			menu.query[0] = '\0';
			build_view();
		} else if (!ctrl && sym >= 0x20 && sym <= 0xff && len + 3 < sizeof menu.query) {
			/* Latin-1 keysyms are their own code points (ñ, á ...) */
			if (sym < 0x80) {
				menu.query[len++] = (char)sym;
			} else {
				menu.query[len++] = (char)(0xc0 | (sym >> 6));
				menu.query[len++] = (char)(0x80 | (sym & 0x3f));
			}
			menu.query[len] = '\0';
			build_view();
		} else {
			return;
		}
	}
	draw_menu();
}

static void menu_button(xcb_button_press_event_t *e)
{
	int inside = e->event == menu.win && e->event_x >= 0 && e->event_y >= 0 &&
	             e->event_x < menu.w && e->event_y < menu.h;

	if (!inside) {
		close_menu();
		return;
	}
	if (e->detail == 4 || e->detail == 5) {
		int max = menu.nview - menu.rows;

		menu.top += e->detail == 4 ? -3 : 3;
		menu.top = menu.top > max ? max : menu.top;
		menu.top = menu.top < 0 ? 0 : menu.top;
		draw_menu();
		return;
	}
	if (e->detail == 1) {
		int i = menu_row_at(e->event_y);

		if (i >= 0) {
			launch(menu.view[i]);
			close_menu();
		}
	}
}

/* ==== bar input ============================================================ */

static int hit_at(int x, int y)
{
	int i;

	for (i = 0; i < nhits; i++)
		if (x >= hits[i].x && x < hits[i].x + hits[i].w &&
		    y >= hits[i].y && y < hits[i].y + hits[i].h)
			return i;
	return -1;
}

static void bar_button(xcb_button_press_event_t *e)
{
	int i = hit_at(e->event_x, e->event_y), b = e->detail;
	struct hit *h;

	if (menu.open) { /* any click on the bar closes the menu */
		int was_apps = i >= 0 && hits[i].kind == H_MENU;

		close_menu();
		if (was_apps)
			return;
	}
	if (i < 0)
		return;
	h = &hits[i];
	switch (h->kind) {
	case H_MENU:
		if (b == 1)
			open_menu();
		break;
	case H_TAG:
		if (b == 1) mon_cmd("view", h->arg + 1);
		else if (b == 3) mon_cmd("toggleview", h->arg + 1);
		else if (b == 2) mon_cmd("tag", h->arg + 1);
		break;
	case H_LAYOUT:
		if (b == 1 || b == 5) mon_cmd("layout next", 0);
		else if (b == 3 || b == 4) mon_cmd("layout prev", 0);
		break;
	case H_TASK:
		if (h->arg >= st.ntasks)
			break;
		if (b == 1) niwm_cmd("activate %lx", st.tasks[h->arg].id);
		else if (b == 2) niwm_cmd("kill %lx", st.tasks[h->arg].id);
		break;
	}
}

/* ==== setup ================================================================= */

static xcb_atom_t atom(const char *name)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn,
		xcb_intern_atom(conn, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_NONE;

	free(r);
	return a;
}

static void load_keymap(void)
{
	const xcb_setup_t *s = xcb_get_setup(conn);
	xcb_get_keyboard_mapping_reply_t *r;
	int len;

	kmin = s->min_keycode;
	kmax = s->max_keycode;
	r = xcb_get_keyboard_mapping_reply(conn,
		xcb_get_keyboard_mapping(conn, kmin, (uint8_t)(kmax - kmin + 1)), NULL);
	free(kmap);
	kmap = NULL;
	if (!r)
		return;
	kper = r->keysyms_per_keycode;
	len = xcb_get_keyboard_mapping_keysyms_length(r);
	if ((kmap = malloc((size_t)len * sizeof *kmap)))
		memcpy(kmap, xcb_get_keyboard_mapping_keysyms(r), (size_t)len * sizeof *kmap);
	free(r);
}

/* ==== system tray ========================================================
 * The freedesktop system tray: apps such as nm-applet ask the owner of
 * _NET_SYSTEM_TRAY_S<screen> to dock an icon window, and we embed it in
 * the bar with XEmbed. nibarc's tray_hide / tray_show pick which icons
 * are shown; hidden ones stay docked so their apps keep working. */

#define MAX_ICONS 32
static struct icon {
	xcb_window_t win;
	int wanted, mapped, onscreen, x, y;
	char cls[64], inst[64];
} icons[MAX_ICONS];
static int nicons, tray_on;
static xcb_window_t tray_win;
static xcb_atom_t a_tray_sel, a_tray_opcode, a_manager, a_xembed, a_xembed_info;

static void handle(xcb_generic_event_t *ev);

static int prop_bytes(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, void *buf, int len)
{
	xcb_get_property_reply_t *r = xcb_get_property_reply(conn,
		xcb_get_property(conn, 0, w, prop, type, 0, (uint32_t)(len / 4)), NULL);
	int n = 0;

	if (r) {
		n = xcb_get_property_value_length(r);
		n = n > len - 1 ? len - 1 : n;
		if (n > 0)
			memcpy(buf, xcb_get_property_value(r), (size_t)n);
		free(r);
	}
	((char *)buf)[n < 0 ? 0 : n] = '\0';
	return n;
}

static int icon_index(xcb_window_t w)
{
	int i;

	for (i = 0; i < nicons; i++)
		if (icons[i].win == w)
			return i;
	return -1;
}

static int name_in(char list[][64], int n, const struct icon *ic)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcasecmp(list[i], ic->cls) || !strcasecmp(list[i], ic->inst))
			return 1;
	return 0;
}

static int tray_wanted(const struct icon *ic)
{
	if (name_in(tray_hide, nhide, ic))
		return 0;
	return nshow ? name_in(tray_show, nshow, ic) : 1;
}

/* a real server timestamp, which owning a selection needs */
static xcb_timestamp_t server_time(xcb_window_t w)
{
	xcb_generic_event_t *ev;
	xcb_timestamp_t t = XCB_CURRENT_TIME;

	xcb_change_property(conn, XCB_PROP_MODE_APPEND, w, XCB_ATOM_WM_NAME,
	                    XCB_ATOM_STRING, 8, 0, "");
	xcb_flush(conn);
	while ((ev = xcb_wait_for_event(conn))) {
		if ((ev->response_type & ~0x80) == XCB_PROPERTY_NOTIFY &&
		    ((xcb_property_notify_event_t *)ev)->window == w) {
			t = ((xcb_property_notify_event_t *)ev)->time;
			free(ev);
			break;
		}
		handle(ev);
		free(ev);
	}
	return t;
}

static void tray_start(void)
{
	xcb_get_selection_owner_reply_t *r;
	xcb_client_message_event_t ev;
	xcb_timestamp_t t;
	uint32_t v = XCB_EVENT_MASK_PROPERTY_CHANGE, orient = 0;
	char name[40];

	snprintf(name, sizeof name, "_NET_SYSTEM_TRAY_S%d", scr_num);
	a_tray_sel = atom(name);
	a_tray_opcode = atom("_NET_SYSTEM_TRAY_OPCODE");
	a_manager = atom("MANAGER");
	a_xembed = atom("_XEMBED");
	a_xembed_info = atom("_XEMBED_INFO");

	r = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, a_tray_sel), NULL);
	if (r && r->owner != XCB_NONE) {
		fprintf(stderr, "nibar: another system tray is running, so nibar's tray stays off\n");
		free(r);
		return;
	}
	free(r);

	tray_win = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, tray_win, root, -1, -1, 1, 1, 0,
	                  XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
	                  XCB_CW_EVENT_MASK, &v);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray_win,
	                    atom("_NET_SYSTEM_TRAY_ORIENTATION"), XCB_ATOM_CARDINAL, 32, 1, &orient);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, tray_win,
	                    atom("_NET_SYSTEM_TRAY_VISUAL"), XCB_ATOM_VISUALID, 32, 1,
	                    &scr->root_visual);
	t = server_time(tray_win);
	xcb_set_selection_owner(conn, tray_win, a_tray_sel, t);
	r = xcb_get_selection_owner_reply(conn, xcb_get_selection_owner(conn, a_tray_sel), NULL);
	if (!r || r->owner != tray_win) {
		fprintf(stderr, "nibar: couldn't become the system tray\n");
		free(r);
		xcb_destroy_window(conn, tray_win);
		return;
	}
	free(r);

	/* tell waiting apps there is a tray now */
	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = root;
	ev.type = a_manager;
	ev.format = 32;
	ev.data.data32[0] = t;
	ev.data.data32[1] = a_tray_sel;
	ev.data.data32[2] = tray_win;
	xcb_send_event(conn, 0, root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
	tray_on = 1;
}

/* give every icon back (unmapped, on the root window) so its app can
 * dock it again with the next tray. own: we still own the selection */
static void tray_stop(int own)
{
	int i;

	if (!tray_on)
		return;
	for (i = 0; i < nicons; i++) {
		xcb_unmap_window(conn, icons[i].win);
		xcb_reparent_window(conn, icons[i].win, root, 0, 0);
		xcb_change_save_set(conn, XCB_SET_MODE_DELETE, icons[i].win);
	}
	nicons = 0;
	if (own)
		xcb_set_selection_owner(conn, XCB_NONE, a_tray_sel, XCB_CURRENT_TIME);
	xcb_destroy_window(conn, tray_win);
	tray_on = 0;
	/* Wait for the server to finish the above. If we exited first, the
	 * server's save-set rule would map the icons on the bare root window. */
	free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
}

static void read_xembed(struct icon *ic)
{
	uint32_t info[3];

	/* no _XEMBED_INFO at all: treat as mapped */
	if (prop_bytes(ic->win, a_xembed_info, a_xembed_info, info, (int)sizeof info) >= 8)
		ic->mapped = (info[1] & 1) != 0; /* XEMBED_MAPPED */
	else
		ic->mapped = 1;
}

static void tray_dock(xcb_window_t w)
{
	struct icon *ic;
	xcb_client_message_event_t ev;
	uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
	uint32_t size[2] = { (uint32_t)tray_size, (uint32_t)tray_size };
	char cls[128];
	int n;

	if (icon_index(w) >= 0 || nicons >= MAX_ICONS)
		return;
	ic = &icons[nicons];
	memset(ic, 0, sizeof *ic);
	ic->win = w;
	ic->x = ic->y = -1;
	n = prop_bytes(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, cls, (int)sizeof cls);
	snprintf(ic->inst, sizeof ic->inst, "%.63s", n > 0 ? cls : "");
	snprintf(ic->cls, sizeof ic->cls, "%.63s",
	         n > (int)strlen(cls) + 1 ? cls + strlen(cls) + 1 : ic->inst);
	ic->wanted = tray_wanted(ic);

	xcb_change_save_set(conn, XCB_SET_MODE_INSERT, w);
	xcb_change_window_attributes(conn, w, XCB_CW_EVENT_MASK, &mask);
	xcb_reparent_window(conn, w, bar, 0, 0);
	xcb_configure_window(conn, w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = w;
	ev.type = a_xembed;
	ev.format = 32;
	ev.data.data32[0] = XCB_CURRENT_TIME;
	ev.data.data32[1] = 0; /* XEMBED_EMBEDDED_NOTIFY */
	ev.data.data32[3] = bar;
	xcb_send_event(conn, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);

	read_xembed(ic);
	nicons++;
	fprintf(stderr, "nibar: tray icon '%s' (instance '%s')%s\n", ic->cls, ic->inst,
	        ic->wanted ? "" : " is hidden by nibarc");
}

static void tray_forget(int i)
{
	icons[i] = icons[--nicons];
}

static void tray_refilter(void)
{
	int i;

	for (i = 0; i < nicons; i++) {
		icons[i].wanted = tray_wanted(&icons[i]);
		icons[i].x = -1; /* size or place may have changed too */
	}
}

static int tray_visible(void)
{
	int i, n = 0;

	for (i = 0; i < nicons; i++)
		if (icons[i].wanted && icons[i].mapped)
			n++;
	return n;
}

static int tray_cols(int inner)
{
	int c = (inner + 6) / (tray_size + 6);

	return c < 1 ? 1 : c;
}

static int tray_height(int inner)
{
	int n = tray_visible(), c = tray_cols(inner);

	return n ? ((n + c - 1) / c) * (tray_size + 6) - 6 : 0;
}

/* called while drawing: move icons into their grid cells */
static void tray_place(int top, int inner)
{
	int i, k = 0, c = tray_cols(inner);

	for (i = 0; i < nicons; i++) {
		struct icon *ic = &icons[i];

		if (!ic->wanted || !ic->mapped) {
			if (ic->onscreen) {
				xcb_unmap_window(conn, ic->win);
				ic->onscreen = 0;
			}
			continue;
		}
		{
			int x = PAD + (k % c) * (tray_size + 6), y = top + (k / c) * (tray_size + 6);

			if (x != ic->x || y != ic->y) {
				uint32_t v[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)tray_size,
				                  (uint32_t)tray_size };

				xcb_configure_window(conn, ic->win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
				                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
				ic->x = x;
				ic->y = y;
			}
		}
		if (!ic->onscreen) {
			xcb_map_window(conn, ic->win);
			ic->onscreen = 1;
		}
		k++;
	}
}

/* returns 1 if the event was about the tray */
static int tray_event(xcb_generic_event_t *ev)
{
	int i;

	if (!tray_on)
		return 0;
	switch (ev->response_type & ~0x80) {
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *e = (xcb_client_message_event_t *)ev;

		if (e->window != tray_win || e->type != a_tray_opcode)
			return 0;
		if (e->data.data32[1] == 0) /* SYSTEM_TRAY_REQUEST_DOCK */
			tray_dock(e->data.data32[2]);
		draw_bar();
		return 1;
	}
	case XCB_DESTROY_NOTIFY:
		if ((i = icon_index(((xcb_destroy_notify_event_t *)ev)->window)) < 0)
			return 0;
		tray_forget(i);
		draw_bar();
		return 1;
	case XCB_REPARENT_NOTIFY: {
		xcb_reparent_notify_event_t *e = (xcb_reparent_notify_event_t *)ev;

		if ((i = icon_index(e->window)) < 0)
			return 0;
		if (e->parent != bar) { /* the app took its icon back */
			xcb_change_save_set(conn, XCB_SET_MODE_DELETE, e->window);
			tray_forget(i);
			draw_bar();
		}
		return 1;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)ev;

		if (e->atom != a_xembed_info || (i = icon_index(e->window)) < 0)
			return 0;
		read_xembed(&icons[i]);
		draw_bar();
		return 1;
	}
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;

		if ((i = icon_index(e->window)) < 0)
			return 0;
		if (e->width != tray_size || e->height != tray_size) { /* keep icons square */
			uint32_t v[2] = { (uint32_t)tray_size, (uint32_t)tray_size };

			xcb_configure_window(conn, e->window,
			                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
		}
		return 1;
	}
	case XCB_SELECTION_CLEAR:
		if (((xcb_selection_clear_event_t *)ev)->owner != tray_win)
			return 0;
		fprintf(stderr, "nibar: another program took over the system tray\n");
		tray_stop(0);
		draw_bar();
		return 1;
	}
	return 0;
}

/* tell niwm how to treat fullscreen windows (see niwm(1), BARS) */
static void set_layer(void)
{
	static const char *names[] = { "below", "above", "beside" };
	xcb_atom_t above = atom("_NET_WM_STATE_ABOVE");

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, atom("_NIWM_LAYER"),
	                    XCB_ATOM_STRING, 8, (uint32_t)strlen(names[layer]), names[layer]);
	if (layer == LAYER_ABOVE)
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, atom("_NET_WM_STATE"),
		                    XCB_ATOM_ATOM, 32, 1, &above);
	else
		xcb_delete_property(conn, bar, atom("_NET_WM_STATE"));
}

static void place_bar(void)
{
	uint32_t geo[4] = { (uint32_t)(on_right ? mon_x + mon_w - bar_w : mon_x), (uint32_t)mon_y,
	                    (uint32_t)bar_w, (uint32_t)mon_h };
	uint32_t strut[12] = { 0 };

	/* left, right, top, bottom, then start/end y for left and right.
	 * Struts count from the edges of the whole X screen. */
	if (on_right) {
		strut[1] = (uint32_t)(scr_w - (mon_x + mon_w) + bar_w);
		strut[6] = (uint32_t)mon_y;
		strut[7] = (uint32_t)(mon_y + mon_h - 1);
	} else {
		strut[0] = (uint32_t)(mon_x + bar_w);
		strut[4] = (uint32_t)mon_y;
		strut[5] = (uint32_t)(mon_y + mon_h - 1);
	}
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, atom("_NET_WM_STRUT_PARTIAL"),
	                    XCB_ATOM_CARDINAL, 32, 12, strut);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, atom("_NET_WM_STRUT"),
	                    XCB_ATOM_CARDINAL, 32, 4, strut);
	xcb_configure_window(conn, bar, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
	                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
	if (pix)
		xcb_free_pixmap(conn, pix);
	pix = xcb_generate_id(conn);
	xcb_create_pixmap(conn, scr->root_depth, pix, bar, (uint16_t)bar_w, (uint16_t)mon_h);
}

static void setup(void)
{
	uint32_t v[2], hints[9] = { 1, 0 }; /* InputHint, input = False */
	xcb_atom_t dock;

	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));

	for (int k = 0; k < scr_num && it.rem > 1; k++)
		xcb_screen_next(&it);
	scr = it.data;
	root = scr->root;
	scr_w = scr->width_in_pixels;
	scr_h = scr->height_in_pixels;
	mon_w = scr_w;
	mon_h = scr_h;

	load_font();
	load_keymap();

	bar = xcb_generate_id(conn);
	v[0] = col_bg;
	v[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
	       XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_LEAVE_WINDOW;
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, bar, root, 0, 0, (uint16_t)bar_w,
	                  (uint16_t)scr_h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
	                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, v);

	gc = xcb_generate_id(conn);
	v[0] = (uint32_t)font;
	v[1] = 0;
	xcb_create_gc(conn, gc, bar, XCB_GC_FONT | XCB_GC_GRAPHICS_EXPOSURES, v);

	dock = atom("_NET_WM_WINDOW_TYPE_DOCK");
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, atom("_NET_WM_WINDOW_TYPE"),
	                    XCB_ATOM_ATOM, 32, 1, &dock);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, XCB_ATOM_WM_NAME,
	                    XCB_ATOM_STRING, 8, 5, "nibar");
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, XCB_ATOM_WM_CLASS,
	                    XCB_ATOM_STRING, 8, 12, "nibar\0nibar\0");
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, bar, XCB_ATOM_WM_HINTS,
	                    XCB_ATOM_WM_HINTS, 32, 9, hints);
	place_bar();
	set_layer();

	/* follow screen size changes */
	v[0] = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	xcb_change_window_attributes(conn, root, XCB_CW_EVENT_MASK, v);

	xcb_map_window(conn, bar);
}

/* re-read nibarc: Super+Ctrl+R (niwm's reload), or SIGHUP */
static void reload(void)
{
	char old_font[256];
	int old_w = bar_w, old_r = on_right;
	uint32_t v;

	snprintf(old_font, sizeof old_font, "%s", font_name);
	close_menu();
	load_config();
	mon_w = -1; /* "monitor" may have changed: place the bar again */
	follow_monitor();
	if (strcmp(old_font, font_name)) {
		xcb_close_font(conn, font);
		load_font();
		v = (uint32_t)font;
		xcb_change_gc(conn, gc, XCB_GC_FONT, &v);
	}
	if (old_w != bar_w || old_r != on_right)
		place_bar();
	v = col_bg; /* what tray icons with no background of their own show */
	xcb_change_window_attributes(conn, bar, XCB_CW_BACK_PIXEL, &v);
	set_layer();
	if (tray_enabled && !tray_on)
		tray_start();
	else if (!tray_enabled && tray_on)
		tray_stop(1);
	else
		tray_refilter();
	hover = -1;
	draw_bar();
}

static void handle(xcb_generic_event_t *ev)
{
	if (tray_event(ev))
		return;
	switch (ev->response_type & ~0x80) {
	case XCB_EXPOSE: {
		xcb_expose_event_t *e = (xcb_expose_event_t *)ev;

		if (e->window == bar)
			xcb_copy_area(conn, pix, bar, gc, (int16_t)e->x, (int16_t)e->y,
			              (int16_t)e->x, (int16_t)e->y, e->width, e->height);
		else if (menu.open && e->window == menu.win)
			draw_menu();
		break;
	}
	case XCB_BUTTON_PRESS: {
		xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;

		if (menu.open && e->event != bar)
			menu_button(e);
		else if (e->event == bar)
			bar_button(e);
		break;
	}
	case XCB_MOTION_NOTIFY: {
		xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)ev;

		if (e->event == bar) {
			int h = hit_at(e->event_x, e->event_y);

			if (h != hover) {
				hover = h;
				draw_bar();
			}
		} else if (menu.open && e->event == menu.win) {
			int i = menu_row_at(e->event_y);

			if (i >= 0 && i != menu.sel && e->event_x >= 0 && e->event_x < menu.w) {
				menu.sel = i;
				draw_menu();
			}
		}
		break;
	}
	case XCB_LEAVE_NOTIFY:
		if (((xcb_leave_notify_event_t *)ev)->event == bar && hover != -1) {
			hover = -1;
			draw_bar();
		}
		break;
	case XCB_KEY_PRESS:
		if (menu.open)
			menu_key((xcb_key_press_event_t *)ev);
		break;
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;

		if (e->window == root && (e->width != scr_w || e->height != scr_h)) {
			scr_w = e->width;
			scr_h = e->height;
			if (!st.nmons) { /* niwm will report monitors; until then, all of it */
				mon_w = scr_w;
				mon_h = scr_h;
			}
			close_menu();
			place_bar();
			draw_bar();
		}
		break;
	}
	case XCB_MAPPING_NOTIFY:
		load_keymap();
		break;
	}
}

/* ==== main =================================================================== */

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		menu_flag = 1;
	else if (sig == SIGHUP)
		reload_flag = 1;
	else
		quit_flag = 1;
}

static void usage(void)
{
	fputs("usage: nibar [-C nibarc] [-m monitor] [-r] [-a] [-w width] [-f font] [-b #bg] [-c #fg]\n"
	      "  -C file   read this instead of ~/.config/niwm/nibarc\n"
	      "  -m N      sit on monitor N (1 = the primary)\n"
	      "  -r        sit on the right instead of the left\n"
	      "  -a        list windows from every tag, not just the viewed ones\n"
	      "  -w px     bar width (default 150)\n"
	      "  -f font   X core font (default: misc-fixed 6x13)\n"
	      "  -b, -c    background and text colours\n"
	      "Flags win over nibarc. Accent colours follow niwm unless nibarc sets them.\n"
	      "Reload nibarc with niwm's reload (mod+ctrl+r) or `pkill -HUP nibar`.\n"
	      "Open the menu with `niwmctl emit menu` or `pkill -USR1 nibar`.\n", stderr);
	exit(1);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	struct pollfd pfd[2];
	xcb_generic_event_t *ev;
	time_t last_status = 0, last_try = 0;
	int i;

	g_argc = argc;
	g_argv = argv;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("nibar " NIBAR_VERSION);
			return 0;
		}
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
			usage();
		if (!strcmp(argv[i], "-C") && i + 1 < argc)
			conf_arg = argv[++i];
	}

	setlocale(LC_TIME, "");
	conn = xcb_connect(NULL, &scr_num);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "nibar: cannot open display\n");
		return 1;
	}
	load_config(); /* after connecting: it reads the X resources too */
	fcntl(xcb_get_file_descriptor(conn), F_SETFD, FD_CLOEXEC);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT;
	sigaction(SIGCHLD, &sa, NULL);

	setup();
	if (tray_enabled)
		tray_start();
	read_status();
	last_status = time(NULL);
	sub_connect();
	if (sub_fd >= 0)
		sub_read();
	draw_bar();

	while (!quit_flag) {
		time_t now = time(NULL);
		int timeout, nfds = 1, redraw = 0;
		struct timespec ts;

		/* wake at the next minute for the clock, and for status/reconnects */
		clock_gettime(CLOCK_REALTIME, &ts);
		timeout = (int)((60 - ts.tv_sec % 60) * 1000 - ts.tv_nsec / 1000000) + 20;
		if (timeout > 20000)
			timeout = 20000;
		if (sub_fd < 0 && timeout > 1000)
			timeout = 1000;

		xcb_flush(conn);
		pfd[0].fd = xcb_get_file_descriptor(conn);
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		if (sub_fd >= 0) {
			pfd[1].fd = sub_fd;
			pfd[1].events = POLLIN;
			pfd[1].revents = 0;
			nfds = 2;
		}
		if (poll(pfd, (nfds_t)nfds, timeout) < 0 && errno != EINTR)
			break;

		if (menu_flag) {
			menu_flag = 0;
			toggle_menu();
		}
		while ((ev = xcb_poll_for_event(conn))) {
			handle(ev);
			free(ev);
		}
		if (xcb_connection_has_error(conn))
			break;
		if (nfds == 2 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR)))
			redraw |= sub_read();

		now = time(NULL);
		if (sub_fd < 0 && now != last_try) {
			last_try = now;
			sub_connect();
			if (sub_fd >= 0)
				redraw |= sub_read();
		}
		if (now - last_status >= 20) {
			read_status();
			last_status = now;
			redraw = 1;
		}
		if (pfd[0].revents == 0 && (nfds < 2 || pfd[1].revents == 0))
			redraw = 1; /* poll timed out: clock tick */
		if (reload_flag) { /* SIGHUP, or "event reload" from niwm */
			reload_flag = 0;
			reload();
			redraw = 0;
		}
		if (redraw)
			draw_bar();
	}

	close_menu();
	tray_stop(1);
	if (sub_fd >= 0)
		close(sub_fd);
	xcb_flush(conn);
	xcb_disconnect(conn);
	free(kmap);
	return 0;
}
