/* x11.c - niwm-x11: the XCB backend.
 *
 * Translates X events into core_* calls and implements the backend
 * table. Only libxcb is needed: key lookup, size hints and WM_CLASS are
 * parsed by hand so no xcb-util libraries get pulled in.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>
#ifdef HAVE_XRES
#include <xcb/res.h>
#endif
#ifdef HAVE_RANDR
#include <xcb/randr.h>
#endif

#include "../core/niwm.h"

#define BF_DELETE    (1u << 0) /* supports WM_DELETE_WINDOW */
#define BF_TAKEFOCUS (1u << 1) /* supports WM_TAKE_FOCUS */

enum {
	WM_PROTOCOLS, WM_DELETE_WINDOW, WM_STATE, WM_TAKE_FOCUS,
	NET_SUPPORTED, NET_WM_NAME, NET_SUPPORTING_WM_CHECK, NET_ACTIVE_WINDOW,
	NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_WINDOW_TYPE,
	NET_WM_WINDOW_TYPE_DIALOG, NET_WM_WINDOW_TYPE_DOCK,
	NET_WM_WINDOW_TYPE_SPLASH, NET_WM_WINDOW_TYPE_UTILITY,
	NET_WM_WINDOW_TYPE_TOOLBAR, NET_WM_WINDOW_TYPE_DESKTOP,
	NET_CLIENT_LIST, NET_NUMBER_OF_DESKTOPS, NET_CURRENT_DESKTOP,
	NET_WM_STRUT, NET_WM_STRUT_PARTIAL, NET_WM_STATE_ABOVE, NET_WM_PID,
	UTF8_STRING, NIWM_LAYER, ATOM_COUNT
};

static const char *atom_names[ATOM_COUNT] = {
	"WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_STATE", "WM_TAKE_FOCUS",
	"_NET_SUPPORTED", "_NET_WM_NAME", "_NET_SUPPORTING_WM_CHECK",
	"_NET_ACTIVE_WINDOW", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
	"_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DIALOG",
	"_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_SPLASH",
	"_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_TOOLBAR",
	"_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_CLIENT_LIST",
	"_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP",
	"_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL", "_NET_WM_STATE_ABOVE", "_NET_WM_PID", "UTF8_STRING",
	"_NIWM_LAYER",
};

static xcb_atom_t atoms[ATOM_COUNT];
static xcb_connection_t *conn;
static xcb_screen_t *scr;
static xcb_window_t root, wmcheck, last_focused;
static xcb_cursor_t cursor;

static xcb_keycode_t kmin, kmax;
static int kper;
static xcb_keysym_t *kmap;
static uint16_t numlock;

static volatile sig_atomic_t got_signal, want_reload;

static struct {
	client *c;
	int button, px, py, x, y, w, h;
} drag;

/* ---- helpers ----------------------------------------------------------- */

static xcb_keysym_t sym_of(xcb_keycode_t kc)
{
	if (!kmap || kc < kmin || kc > kmax)
		return 0;
	return kmap[(kc - kmin) * kper];
}

static void load_keymap(void)
{
	const xcb_setup_t *s = xcb_get_setup(conn);
	xcb_get_keyboard_mapping_reply_t *r;
	xcb_get_modifier_mapping_reply_t *m;
	int len, i, j;

	kmin = s->min_keycode;
	kmax = s->max_keycode;
	r = xcb_get_keyboard_mapping_reply(conn,
		xcb_get_keyboard_mapping(conn, kmin, kmax - kmin + 1), NULL);
	free(kmap);
	kmap = NULL;
	if (!r)
		return;
	kper = r->keysyms_per_keycode;
	len = xcb_get_keyboard_mapping_keysyms_length(r);
	if ((kmap = malloc((size_t)len * sizeof *kmap)))
		memcpy(kmap, xcb_get_keyboard_mapping_keysyms(r), (size_t)len * sizeof *kmap);
	free(r);

	numlock = 0;
	m = xcb_get_modifier_mapping_reply(conn, xcb_get_modifier_mapping(conn), NULL);
	if (m) {
		xcb_keycode_t *codes = xcb_get_modifier_mapping_keycodes(m);
		int per = m->keycodes_per_modifier;

		for (i = 0; i < 8; i++)
			for (j = 0; j < per; j++)
				if (codes[i * per + j] && sym_of(codes[i * per + j]) == 0xff7f)
					numlock = (uint16_t)(1u << i);
		free(m);
	}
}

/* Read a property as raw bytes. Returns length, fills buf (NUL-ended). */
static int get_prop(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type,
                    void *buf, int buflen)
{
	xcb_get_property_reply_t *r;
	int len = 0;

	r = xcb_get_property_reply(conn,
		xcb_get_property(conn, 0, w, prop, type, 0, (uint32_t)(buflen / 4)), NULL);
	if (r) {
		len = xcb_get_property_value_length(r);
		if (len > buflen - 1)
			len = buflen - 1;
		if (len > 0)
			memcpy(buf, xcb_get_property_value(r), (size_t)len);
		free(r);
	}
	((char *)buf)[len < 0 ? 0 : len] = '\0';
	return len;
}

static void get_title(xcb_window_t w, char *buf, int len)
{
	if (get_prop(w, atoms[NET_WM_NAME], atoms[UTF8_STRING], buf, len) <= 0)
		get_prop(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, buf, len);
}

static int has_atom(xcb_window_t w, xcb_atom_t prop, xcb_atom_t want)
{
	xcb_atom_t list[32];
	int n = get_prop(w, prop, XCB_ATOM_ATOM, list, (int)sizeof list) / 4, i;

	for (i = 0; i < n; i++)
		if (list[i] == want)
			return 1;
	return 0;
}

static void send_protocol(xcb_window_t w, xcb_atom_t proto)
{
	xcb_client_message_event_t ev;

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = w;
	ev.type = atoms[WM_PROTOCOLS];
	ev.format = 32;
	ev.data.data32[0] = proto;
	ev.data.data32[1] = XCB_CURRENT_TIME;
	xcb_send_event(conn, 0, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void set_wm_state(xcb_window_t w, uint32_t state)
{
	uint32_t data[2] = { state, XCB_NONE };

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, w, atoms[WM_STATE],
	                    atoms[WM_STATE], 32, 2, data);
}

static void grab_buttons(client *c, int focused)
{
	uint16_t extra[4] = { 0, XCB_MOD_MASK_LOCK, numlock, (uint16_t)(numlock | XCB_MOD_MASK_LOCK) };
	uint16_t mod = (uint16_t)wm.cfg.modkey;
	xcb_window_t w = (xcb_window_t)c->id;
	int i;

	xcb_ungrab_button(conn, XCB_BUTTON_INDEX_ANY, w, XCB_MOD_MASK_ANY);
	if (!focused) /* click-to-focus: catch the click, then replay it */
		xcb_grab_button(conn, 0, w, XCB_EVENT_MASK_BUTTON_PRESS,
		                XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC, XCB_NONE,
		                XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
	for (i = 0; i < 4; i++) {
		xcb_grab_button(conn, 0, w, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
		                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
		                XCB_NONE, XCB_BUTTON_INDEX_1, mod | extra[i]);
		xcb_grab_button(conn, 0, w, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
		                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
		                XCB_NONE, XCB_BUTTON_INDEX_3, mod | extra[i]);
	}
}

/* ---- backend table ------------------------------------------------------ */

static void x_configure(client *c, int x, int y, int w, int h, int bw)
{
	uint32_t v[5] = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, (uint32_t)bw };

	xcb_configure_window(conn, (xcb_window_t)c->id,
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
		XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH, v);
}

static void x_hide(client *c)
{
	/* off-screen instead of unmapped, so apps never see a withdraw */
	uint32_t v[2] = { (uint32_t)(-2 * (c->w + 2 * c->bw) - 100), (uint32_t)c->y };

	xcb_configure_window(conn, (xcb_window_t)c->id,
	                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
}

static void x_raise(client *c)
{
	uint32_t v = XCB_STACK_MODE_ABOVE;

	xcb_configure_window(conn, (xcb_window_t)c->id, XCB_CONFIG_WINDOW_STACK_MODE, &v);
}

static void x_focus(client *c)
{
	client *old;

	if (last_focused && (!c || last_focused != c->id) && (old = core_find(last_focused)))
		grab_buttons(old, 0);
	if (c) {
		xcb_window_t w = (xcb_window_t)c->id;

		xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, w, XCB_CURRENT_TIME);
		xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms[NET_ACTIVE_WINDOW],
		                    XCB_ATOM_WINDOW, 32, 1, &w);
		if (c->bflags & BF_TAKEFOCUS)
			send_protocol(w, atoms[WM_TAKE_FOCUS]);
		grab_buttons(c, 1);
		last_focused = w;
	} else {
		xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
		xcb_delete_property(conn, root, atoms[NET_ACTIVE_WINDOW]);
		last_focused = 0;
	}
}

static void x_set_border_color(client *c, uint32_t rgb)
{
	xcb_change_window_attributes(conn, (xcb_window_t)c->id, XCB_CW_BORDER_PIXEL, &rgb);
}

static void x_close(client *c)
{
	if (c->bflags & BF_DELETE)
		send_protocol((xcb_window_t)c->id, atoms[WM_DELETE_WINDOW]);
	else
		xcb_kill_client(conn, (xcb_window_t)c->id);
}

static void x_fullscreen(client *c, int on)
{
	xcb_atom_t a = atoms[NET_WM_STATE_FULLSCREEN];

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, (xcb_window_t)c->id,
	                    atoms[NET_WM_STATE], XCB_ATOM_ATOM, 32, on ? 1 : 0, &a);
}

static void x_grab_keys(void)
{
	uint16_t extra[4] = { 0, XCB_MOD_MASK_LOCK, numlock, (uint16_t)(numlock | XCB_MOD_MASK_LOCK) };
	size_t i;
	int kc, e;

	xcb_ungrab_key(conn, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
	for (i = 0; i < wm.cfg.nbinds; i++) {
		uint32_t m = wm.cfg.binds[i].mods;

		if (m & MOD_MOD)
			m = (m & ~MOD_MOD) | wm.cfg.modkey;
		for (kc = kmin; kc <= kmax; kc++) {
			if (sym_of((xcb_keycode_t)kc) != wm.cfg.binds[i].keysym)
				continue;
			for (e = 0; e < 4; e++)
				xcb_grab_key(conn, 1, root, (uint16_t)(m | extra[e]), (xcb_keycode_t)kc,
				             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
		}
	}
}

static void raise_docks(void);

static void x_sync(void)
{
	xcb_window_t list[256];
	uint32_t n = 0, cur = (uint32_t)core_curtag(), ntags = (uint32_t)wm.cfg.ntags;
	client *c;

	for (c = wm.clients; c && n < 256; c = c->next)
		list[n++] = (xcb_window_t)c->id;
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms[NET_CLIENT_LIST],
	                    XCB_ATOM_WINDOW, 32, n, list);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms[NET_CURRENT_DESKTOP],
	                    XCB_ATOM_CARDINAL, 32, 1, &cur);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms[NET_NUMBER_OF_DESKTOPS],
	                    XCB_ATOM_CARDINAL, 32, 1, &ntags);
	raise_docks(); /* the core just restacked: put "above" bars back on top */
}

static const backend x11_backend = {
	"x11", x_configure, x_hide, x_raise, x_focus, x_set_border_color,
	x_close, x_fullscreen, x_grab_keys, x_sync,
};

/* The process behind a window, for swallowing. _NET_WM_PID is what most
 * apps set; when it's missing, the X server itself knows (XRes 1.2). */
static int window_pid(xcb_window_t w)
{
	uint32_t pid[2] = { 0, 0 };

	if (get_prop(w, atoms[NET_WM_PID], XCB_ATOM_CARDINAL, pid, (int)sizeof pid) >= 4 && pid[0])
		return (int)pid[0];
#ifdef HAVE_XRES
	{
		xcb_res_client_id_spec_t spec = { w, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID };
		xcb_res_query_client_ids_reply_t *r = xcb_res_query_client_ids_reply(conn,
			xcb_res_query_client_ids(conn, 1, &spec), NULL);

		if (r) {
			xcb_res_client_id_value_iterator_t it =
				xcb_res_query_client_ids_ids_iterator(r);

			for (; it.rem; xcb_res_client_id_value_next(&it)) {
				if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
				    xcb_res_client_id_value_value_length(it.data) >= 1) {
					pid[0] = *xcb_res_client_id_value_value(it.data);
					break;
				}
			}
			free(r);
		}
	}
#endif
	return (int)pid[0];
}

/* ---- docks (bars) --------------------------------------------------------
 * Docks are never managed, but the space they reserve with
 * _NET_WM_STRUT(_PARTIAL) is kept free of tiled windows. */

#define MAX_DOCKS 8
enum { LAYER_BELOW, LAYER_ABOVE, LAYER_BESIDE };
static xcb_window_t docks[MAX_DOCKS];
static int dock_layer[MAX_DOCKS];
static int ndocks;

static int dock_index(xcb_window_t w)
{
	int i;

	for (i = 0; i < ndocks; i++)
		if (docks[i] == w)
			return i;
	return -1;
}

/* _NIWM_LAYER (a string) says how a dock treats fullscreen windows:
 *   below   fullscreen windows cover it (the default)
 *   above   it stays on top of them
 *   beside  they shrink to leave it visible
 * _NET_WM_STATE_ABOVE, which other bars set, counts as "above". */
static int read_layer(xcb_window_t w)
{
	char buf[16];

	if (get_prop(w, atoms[NIWM_LAYER], XCB_ATOM_STRING, buf, (int)sizeof buf) > 0) {
		if (!strcmp(buf, "above"))
			return LAYER_ABOVE;
		if (!strcmp(buf, "beside"))
			return LAYER_BESIDE;
		return LAYER_BELOW;
	}
	return has_atom(w, atoms[NET_WM_STATE], atoms[NET_WM_STATE_ABOVE]) ? LAYER_ABOVE : LAYER_BELOW;
}

/* Struts are measured from the edges of the whole X screen. A dock counts
 * for the monitor its middle is on, and reserves what it reaches past that
 * monitor's own edge: a 150px bar on the left of a monitor at x=1920
 * sets a left strut of 2070, which is 150px for that monitor. */
static void update_struts(void)
{
	int s[MAX_MONS][4], beside[MAX_MONS], i, k, mi;
	int sw = scr->width_in_pixels, sh = scr->height_in_pixels;

	memset(s, 0, sizeof s);
	memset(beside, 0, sizeof beside);
	for (i = 0; i < ndocks; i++) {
		uint32_t v[13]; /* 12 cardinals + room for get_prop's NUL */
		xcb_get_geometry_reply_t *g;
		monitor *m;
		int n, edge[4];

		dock_layer[i] = read_layer(docks[i]);
		g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, docks[i]), NULL);
		if (!g)
			continue;
		m = core_monitor_at(g->x + g->width / 2, g->y + g->height / 2);
		free(g);
		mi = m->num;
		if (dock_layer[i] == LAYER_BESIDE)
			beside[mi] = 1;
		n = get_prop(docks[i], atoms[NET_WM_STRUT_PARTIAL], XCB_ATOM_CARDINAL,
		             v, (int)sizeof v) / 4;
		if (n < 4)
			n = get_prop(docks[i], atoms[NET_WM_STRUT], XCB_ATOM_CARDINAL,
			             v, (int)sizeof v) / 4;
		if (n < 4)
			continue;
		edge[0] = m->x;                   /* screen left  -> monitor left   */
		edge[1] = sw - (m->x + m->w);     /* screen right -> monitor right  */
		edge[2] = m->y;
		edge[3] = sh - (m->y + m->h);
		for (k = 0; k < 4; k++)
			if (v[k] && (int)v[k] - edge[k] > s[mi][k])
				s[mi][k] = (int)v[k] - edge[k];
	}
	for (mi = 0; mi < wm.nmons; mi++)
		core_set_struts(&wm.mons[mi], s[mi][0], s[mi][1], s[mi][2], s[mi][3], beside[mi]);
	x_sync();
}

/* ---- monitors ------------------------------------------------------------
 * RandR 1.5 lists monitors, including the virtual ones made with
 * `xrandr --setmonitor` (handy for splitting one wide screen). Without it,
 * the whole X screen is one monitor. */

#ifdef HAVE_RANDR
static int randr_event = -1; /* first RandR event code, -1 = no RandR 1.5 */
#endif

typedef struct { int r[4], primary; } mon_rect;

static int mon_cmp(const void *a, const void *b)
{
	const mon_rect *x = a, *y = b;

	if (x->primary != y->primary)
		return y->primary - x->primary; /* the primary is monitor 1 */
	if (x->r[0] != y->r[0])
		return x->r[0] - y->r[0];       /* then left to right */
	return x->r[1] - y->r[1];
}

static void update_monitors(void)
{
	mon_rect mr[MAX_MONS];
	int rect[MAX_MONS][4], n = 0, i;
#ifdef HAVE_RANDR
	int j;
#endif

#ifdef HAVE_RANDR
	if (randr_event >= 0) {
		xcb_randr_get_monitors_reply_t *r = xcb_randr_get_monitors_reply(conn,
			xcb_randr_get_monitors(conn, root, 1), NULL);

		if (r) {
			xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(r);

			for (; it.rem && n < MAX_MONS; xcb_randr_monitor_info_next(&it)) {
				xcb_randr_monitor_info_t *mi = it.data;
				int dup = 0;

				if (!mi->width || !mi->height)
					continue;
				for (j = 0; j < n; j++) /* mirrored outputs: one monitor */
					if (mr[j].r[0] == mi->x && mr[j].r[1] == mi->y &&
					    mr[j].r[2] == mi->width && mr[j].r[3] == mi->height)
						dup = 1;
				if (dup)
					continue;
				mr[n].r[0] = mi->x;
				mr[n].r[1] = mi->y;
				mr[n].r[2] = mi->width;
				mr[n].r[3] = mi->height;
				mr[n].primary = mi->primary;
				n++;
			}
			free(r);
		}
	}
#endif
#ifdef HAVE_RANDR
	/* A monitor that holds another one whole is the full screen left over
	 * after `xrandr --setmonitor` split it with output "none": skip it. */
	for (i = 0; i < n; i++) {
		int inner = 0;

		for (j = 0; j < n && !inner; j++)
			inner = j != i && mr[j].r[0] >= mr[i].r[0] && mr[j].r[1] >= mr[i].r[1] &&
			        mr[j].r[0] + mr[j].r[2] <= mr[i].r[0] + mr[i].r[2] &&
			        mr[j].r[1] + mr[j].r[3] <= mr[i].r[1] + mr[i].r[3];
		if (inner) {
			if (mr[i].primary) /* it keeps being the primary through its first part */
				for (j = 0; j < n; j++)
					if (j != i && mr[j].r[0] == mr[i].r[0] && mr[j].r[1] == mr[i].r[1])
						mr[j].primary = 1;
			mr[i] = mr[--n];
			i = -1; /* start over */
		}
	}
#endif
	if (!n) {
		mr[0].r[0] = mr[0].r[1] = 0;
		mr[0].r[2] = scr->width_in_pixels;
		mr[0].r[3] = scr->height_in_pixels;
		mr[0].primary = 1;
		n = 1;
	}
	qsort(mr, (size_t)n, sizeof mr[0], mon_cmp);
	for (i = 0; i < n; i++)
		memcpy(rect[i], mr[i].r, sizeof rect[i]);
	core_set_monitors((const int (*)[4])rect, n);
	update_struts();
}

static void raise_docks(void)
{
	uint32_t v = XCB_STACK_MODE_ABOVE;
	int i;

	for (i = 0; i < ndocks; i++)
		if (dock_layer[i] == LAYER_ABOVE)
			xcb_configure_window(conn, docks[i], XCB_CONFIG_WINDOW_STACK_MODE, &v);
}

static void add_dock(xcb_window_t w)
{
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;

	if (dock_index(w) < 0 && ndocks < MAX_DOCKS)
		docks[ndocks++] = w;
	xcb_change_window_attributes(conn, w, XCB_CW_EVENT_MASK, &mask);
	update_struts();
}

static void remove_dock(xcb_window_t w)
{
	int i = dock_index(w);

	if (i < 0)
		return;
	ndocks--;
	docks[i] = docks[ndocks];
	dock_layer[i] = dock_layer[ndocks];
	update_struts();
}

/* ---- managing windows ------------------------------------------------- */

static void manage(xcb_window_t w)
{
	xcb_get_window_attributes_reply_t *wa;
	xcb_get_geometry_reply_t *g;
	char cls[256], title[256];
	uint32_t hints[19], trans[2] = { 0, 0 };
	xcb_atom_t types[16];
	int i, n, want_float = 0;
	uint32_t mask;
	client *c;

	if (w == wmcheck || core_find(w))
		return;
	wa = xcb_get_window_attributes_reply(conn, xcb_get_window_attributes(conn, w), NULL);
	if (!wa)
		return;
	if (wa->override_redirect) {
		free(wa);
		return;
	}
	free(wa);

	/* docks (bars) and desktops are shown but never tiled */
	n = get_prop(w, atoms[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, types, (int)sizeof types) / 4;
	for (i = 0; i < n; i++) {
		if (types[i] == atoms[NET_WM_WINDOW_TYPE_DOCK] ||
		    types[i] == atoms[NET_WM_WINDOW_TYPE_DESKTOP]) {
			if (types[i] == atoms[NET_WM_WINDOW_TYPE_DOCK])
				add_dock(w);
			xcb_map_window(conn, w);
			return;
		}
		if (types[i] == atoms[NET_WM_WINDOW_TYPE_DIALOG] ||
		    types[i] == atoms[NET_WM_WINDOW_TYPE_SPLASH] ||
		    types[i] == atoms[NET_WM_WINDOW_TYPE_UTILITY] ||
		    types[i] == atoms[NET_WM_WINDOW_TYPE_TOOLBAR])
			want_float = 1;
	}
	if (get_prop(w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, trans, (int)sizeof trans) >= 4 && trans[0])
		want_float = 1;
	/* fixed-size windows (min == max) float */
	if (get_prop(w, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, hints, (int)sizeof hints) >= 36 &&
	    (hints[0] & (1u << 4)) && (hints[0] & (1u << 5)) &&
	    hints[5] && hints[5] == hints[7] && hints[6] == hints[8])
		want_float = 1;

	n = get_prop(w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, cls, (int)sizeof cls);
	get_title(w, title, (int)sizeof title);

	g = xcb_get_geometry_reply(conn, xcb_get_geometry(conn, w), NULL);
	c = core_manage(w, g ? g->x : 0, g ? g->y : 0, g ? g->width : 0, g ? g->height : 0,
	                n > (int)strlen(cls) ? cls + strlen(cls) + 1 : "", /* class */
	                cls,                                                   /* instance */
	                title, want_float, window_pid(w));
	free(g);
	if (!c)
		return;

	if (has_atom(w, atoms[WM_PROTOCOLS], atoms[WM_DELETE_WINDOW]))
		c->bflags |= BF_DELETE;
	if (has_atom(w, atoms[WM_PROTOCOLS], atoms[WM_TAKE_FOCUS]))
		c->bflags |= BF_TAKEFOCUS;

	mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE |
	       XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	xcb_change_window_attributes(conn, w, XCB_CW_EVENT_MASK, &mask);
	set_wm_state(w, 1); /* NormalState */
	grab_buttons(c, 0);

	if (has_atom(w, atoms[NET_WM_STATE], atoms[NET_WM_STATE_FULLSCREEN]))
		core_set_fullscreen(c, 1);
	core_arrange();
	xcb_map_window(conn, w);
	if (core_visible(c))
		core_focus(c);
}

static void unmanage(client *c)
{
	if (drag.c == c) {
		drag.c = NULL;
		xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
	}
	if (last_focused == (xcb_window_t)c->id)
		last_focused = 0;
	core_unmanage(c);
}

static void send_configure_notify(client *c)
{
	xcb_configure_notify_event_t ev;

	memset(&ev, 0, sizeof ev);
	ev.response_type = XCB_CONFIGURE_NOTIFY;
	ev.event = ev.window = (xcb_window_t)c->id;
	ev.x = (int16_t)c->x;
	ev.y = (int16_t)c->y;
	ev.width = (uint16_t)c->w;
	ev.height = (uint16_t)c->h;
	ev.border_width = (uint16_t)c->bw;
	xcb_send_event(conn, 0, (xcb_window_t)c->id, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
	               (const char *)&ev);
}

/* ---- events --------------------------------------------------------------- */

static void on_configure_request(xcb_configure_request_event_t *e)
{
	client *c = core_find(e->window);

	if (c) {
		if (!c->fullscreen && core_isfloat(c)) {
			int x = c->fx, y = c->fy, w = c->fw, h = c->fh;

			if (e->value_mask & XCB_CONFIG_WINDOW_X) x = e->x;
			if (e->value_mask & XCB_CONFIG_WINDOW_Y) y = e->y;
			if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) w = e->width;
			if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) h = e->height;
			core_float_geometry(c, x, y, w, h);
		}
		send_configure_notify(c); /* tiled windows keep their slot */
	} else {
		uint32_t v[7];
		uint16_t mask = 0;
		int i = 0;

		if (e->value_mask & XCB_CONFIG_WINDOW_X) { v[i++] = (uint32_t)e->x; mask |= XCB_CONFIG_WINDOW_X; }
		if (e->value_mask & XCB_CONFIG_WINDOW_Y) { v[i++] = (uint32_t)e->y; mask |= XCB_CONFIG_WINDOW_Y; }
		if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) { v[i++] = e->width; mask |= XCB_CONFIG_WINDOW_WIDTH; }
		if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) { v[i++] = e->height; mask |= XCB_CONFIG_WINDOW_HEIGHT; }
		if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) { v[i++] = e->border_width; mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH; }
		if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) { v[i++] = e->sibling; mask |= XCB_CONFIG_WINDOW_SIBLING; }
		if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) { v[i++] = e->stack_mode; mask |= XCB_CONFIG_WINDOW_STACK_MODE; }
		xcb_configure_window(conn, e->window, mask, v);
	}
}

static void on_button_press(xcb_button_press_event_t *e)
{
	client *c = core_find(e->event);
	uint32_t mods = e->state & MOD_MASK;

	if (c && c != wm.sel && core_visible(c))
		core_focus(c);
	if (c && !c->fullscreen && mods == wm.cfg.modkey &&
	    (e->detail == XCB_BUTTON_INDEX_1 || e->detail == XCB_BUTTON_INDEX_3)) {
		if (!core_isfloat(c))
			core_set_floating(c, 1);
		drag.c = c;
		drag.button = e->detail;
		drag.px = e->root_x;
		drag.py = e->root_y;
		drag.x = c->fx;
		drag.y = c->fy;
		drag.w = c->fw;
		drag.h = c->fh;
		xcb_grab_pointer(conn, 0, root,
		                 XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
		                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE,
		                 cursor, XCB_CURRENT_TIME);
	}
	xcb_allow_events(conn, XCB_ALLOW_REPLAY_POINTER, e->time);
}

static void on_motion(xcb_motion_notify_event_t *e)
{
	int dx, dy;

	if (!drag.c) { /* the pointer moved over the desktop of another monitor? */
		if (e->event == root && wm.nmons > 1)
			core_pointer_at(e->root_x, e->root_y);
		return;
	}
	dx = e->root_x - drag.px;
	dy = e->root_y - drag.py;
	if (drag.button == XCB_BUTTON_INDEX_1)
		core_float_geometry(drag.c, drag.x + dx, drag.y + dy, drag.w, drag.h);
	else
		core_float_geometry(drag.c, drag.x, drag.y,
		                    drag.w + dx < 32 ? 32 : drag.w + dx,
		                    drag.h + dy < 32 ? 32 : drag.h + dy);
}

static void on_client_message(xcb_client_message_event_t *e)
{
	client *c = core_find(e->window);

	if (!c)
		return;
	if (e->type == atoms[NET_WM_STATE]) {
		xcb_atom_t fs = atoms[NET_WM_STATE_FULLSCREEN];

		if (e->data.data32[1] == fs || e->data.data32[2] == fs) {
			uint32_t act = e->data.data32[0]; /* 0 remove, 1 add, 2 toggle */

			core_set_fullscreen(c, act == 1 || (act == 2 && !c->fullscreen));
		}
	} else if (e->type == atoms[NET_ACTIVE_WINDOW]) {
		if (core_visible(c) || e->data.data32[0] == 2) /* 2 = from a pager/bar */
			core_activate(c);
		else
			core_set_urgent(c, 1);
	}
}

static void on_property(xcb_property_notify_event_t *e)
{
	client *c = core_find(e->window);
	char title[256];

	if ((e->atom == atoms[NET_WM_STRUT] || e->atom == atoms[NET_WM_STRUT_PARTIAL] ||
	     e->atom == atoms[NIWM_LAYER] || e->atom == atoms[NET_WM_STATE]) &&
	    dock_index(e->window) >= 0) {
		update_struts();
		return;
	}
	if (!c || e->state == XCB_PROPERTY_DELETE)
		return;
	if (e->atom == XCB_ATOM_WM_NAME || e->atom == atoms[NET_WM_NAME]) {
		get_title(e->window, title, (int)sizeof title);
		core_set_title(c, title);
	} else if (e->atom == XCB_ATOM_WM_HINTS) {
		uint32_t h[10];

		if (get_prop(e->window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, h, (int)sizeof h) >= 4)
			core_set_urgent(c, (h[0] & 256u) != 0); /* XUrgencyHint */
	}
}

static void handle(xcb_generic_event_t *ev)
{
	client *c;

#ifdef HAVE_RANDR
	if (randr_event >= 0 &&
	    ((ev->response_type & ~0x80) == randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY ||
	     (ev->response_type & ~0x80) == randr_event + XCB_RANDR_NOTIFY)) {
		update_monitors(); /* plugged, unplugged, rotated or --setmonitor */
		return;
	}
#endif

	switch (ev->response_type & ~0x80) {
	case 0: /* X error: windows vanish under us all the time, ignore */
		if (getenv("NIWM_DEBUG")) {
			xcb_generic_error_t *err = (xcb_generic_error_t *)ev;

			fprintf(stderr, "niwm: X error %d (request %d)\n",
			        err->error_code, err->major_code);
		}
		break;
	case XCB_MAP_REQUEST:
		manage(((xcb_map_request_event_t *)ev)->window);
		break;
	case XCB_UNMAP_NOTIFY: {
		xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *)ev;

		if ((c = core_find(e->window))) {
			set_wm_state(e->window, 0); /* WithdrawnState */
			unmanage(c);
		} else {
			remove_dock(e->window);
		}
		break;
	}
	case XCB_DESTROY_NOTIFY: {
		xcb_window_t w = ((xcb_destroy_notify_event_t *)ev)->window;

		if ((c = core_find(w)))
			unmanage(c);
		else
			remove_dock(w);
		break;
	}
	case XCB_CONFIGURE_REQUEST:
		on_configure_request((xcb_configure_request_event_t *)ev);
		break;
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;

		if (e->window == root) {
			scr->width_in_pixels = e->width;
			scr->height_in_pixels = e->height;
			update_monitors();
		}
		break;
	}
	case XCB_KEY_PRESS: {
		xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;

		core_key(e->state & MOD_MASK, sym_of(e->detail));
		break;
	}
	case XCB_BUTTON_PRESS:
		on_button_press((xcb_button_press_event_t *)ev);
		break;
	case XCB_MOTION_NOTIFY:
		on_motion((xcb_motion_notify_event_t *)ev);
		break;
	case XCB_BUTTON_RELEASE:
		if (drag.c) {
			client *c = drag.c;

			drag.c = NULL;
			xcb_ungrab_pointer(conn, XCB_CURRENT_TIME);
			core_float_moved(c); /* dropped on another monitor? */
		}
		break;
	case XCB_ENTER_NOTIFY: {
		xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *)ev;

		if (wm.cfg.focus_follows_mouse && !drag.c &&
		    e->mode == XCB_NOTIFY_MODE_NORMAL && e->detail != XCB_NOTIFY_DETAIL_INFERIOR &&
		    (c = core_find(e->event)) && c != wm.sel)
			core_focus(c);
		break;
	}
	case XCB_FOCUS_IN: {
		xcb_focus_in_event_t *e = (xcb_focus_in_event_t *)ev;

		/* don't let apps steal focus */
		if (wm.sel && e->event != (xcb_window_t)wm.sel->id &&
		    e->mode == XCB_NOTIFY_MODE_NORMAL && core_find(e->event))
			x_focus(wm.sel);
		break;
	}
	case XCB_PROPERTY_NOTIFY:
		on_property((xcb_property_notify_event_t *)ev);
		break;
	case XCB_CLIENT_MESSAGE:
		on_client_message((xcb_client_message_event_t *)ev);
		break;
	case XCB_MAPPING_NOTIFY:
		if (((xcb_mapping_notify_event_t *)ev)->request != XCB_MAPPING_POINTER) {
			load_keymap();
			x_grab_keys();
		}
		break;
	}
}

/* ---- setup ------------------------------------------------------------- */

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		want_reload = 1;
	else
		got_signal = 1;
}

static int setup(void)
{
	xcb_intern_atom_cookie_t ck[ATOM_COUNT];
	xcb_generic_error_t *err;
	xcb_font_t font;
	uint32_t mask;
	int i;

	scr = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
	root = scr->root;

	mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	       XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE |
	       XCB_EVENT_MASK_POINTER_MOTION;
	err = xcb_request_check(conn,
		xcb_change_window_attributes_checked(conn, root, XCB_CW_EVENT_MASK, &mask));
	if (err) {
		fprintf(stderr, "niwm: another window manager is already running\n");
		free(err);
		return -1;
	}

	for (i = 0; i < ATOM_COUNT; i++)
		ck[i] = xcb_intern_atom(conn, 0, (uint16_t)strlen(atom_names[i]), atom_names[i]);
	for (i = 0; i < ATOM_COUNT; i++) {
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, ck[i], NULL);

		atoms[i] = r ? r->atom : XCB_NONE;
		free(r);
	}

	/* left_ptr cursor instead of the X-shaped default */
	font = xcb_generate_id(conn);
	cursor = xcb_generate_id(conn);
	xcb_open_font(conn, font, 6, "cursor");
	xcb_create_glyph_cursor(conn, cursor, font, font, 68, 69, 0, 0, 0, 0xffff, 0xffff, 0xffff);
	xcb_close_font(conn, font);
	xcb_change_window_attributes(conn, root, XCB_CW_CURSOR, &cursor);

	/* EWMH: tell clients who we are and what we support */
	wmcheck = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, wmcheck, root, -1, -1, 1, 1, 0,
	                  XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wmcheck, atoms[NET_SUPPORTING_WM_CHECK],
	                    XCB_ATOM_WINDOW, 32, 1, &wmcheck);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wmcheck, atoms[NET_WM_NAME],
	                    atoms[UTF8_STRING], 8, 4, "NiWM");
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms[NET_SUPPORTING_WM_CHECK],
	                    XCB_ATOM_WINDOW, 32, 1, &wmcheck);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atoms[NET_SUPPORTED],
	                    XCB_ATOM_ATOM, 32, UTF8_STRING - NET_SUPPORTED, &atoms[NET_SUPPORTED]);

	load_keymap();
#ifdef HAVE_RANDR
	{
		const xcb_query_extension_reply_t *ext = xcb_get_extension_data(conn, &xcb_randr_id);

		if (ext && ext->present) {
			xcb_randr_query_version_reply_t *v = xcb_randr_query_version_reply(conn,
				xcb_randr_query_version(conn, 1, 5), NULL);

			if (v && (v->major_version > 1 || v->minor_version >= 5)) {
				randr_event = ext->first_event;
				xcb_randr_select_input(conn, root,
				                       XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
				                       XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
				                       XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);
			}
			free(v);
		}
	}
#endif
#ifdef HAVE_XRES
	/* client-id queries need XRes 1.2 agreed first */
	free(xcb_res_query_version_reply(conn, xcb_res_query_version(conn, 1, 2), NULL));
#endif
	return 0;
}

static void scan(void)
{
	xcb_query_tree_reply_t *t = xcb_query_tree_reply(conn, xcb_query_tree(conn, root), NULL);
	xcb_window_t *kids;
	int i, n;

	if (!t)
		return;
	kids = xcb_query_tree_children(t);
	n = xcb_query_tree_children_length(t);
	for (i = 0; i < n; i++) {
		xcb_get_window_attributes_reply_t *wa = xcb_get_window_attributes_reply(conn,
			xcb_get_window_attributes(conn, kids[i]), NULL);

		if (wa && !wa->override_redirect && wa->map_state == XCB_MAP_STATE_VIEWABLE)
			manage(kids[i]);
		free(wa);
	}
	free(t);
}

static void cleanup(void)
{
	client *c;
	int i;

	/* bring every window back on screen for whatever runs next */
	for (i = 0; i < wm.nmons; i++)
		wm.mons[i].tagset = (1u << wm.cfg.ntags) - 1;
	core_unswallow_all();
	for (c = wm.clients; c; c = c->next) {
		c->fullscreen = 0;
		c->floating = 1;
		if (!c->tags) /* a hidden scratchpad */
			c->tags = c->mon->tagset;
	}
	core_arrange();
	xcb_ungrab_key(conn, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
	xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_INPUT_FOCUS_POINTER_ROOT,
	                    XCB_CURRENT_TIME);
	xcb_delete_property(conn, root, atoms[NET_ACTIVE_WINDOW]);
	xcb_destroy_window(conn, wmcheck);
	xcb_free_cursor(conn, cursor);
	/* wait until the server has done all of the above: requests still
	 * queued when we disconnect can be dropped, leaving windows off screen */
	free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), NULL));
	core_cleanup();
	ipc_cleanup();
	free(kmap);
	xcb_disconnect(conn);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	struct pollfd pfd[2];
	xcb_generic_event_t *ev;
	int nfds = 1;

	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
		puts("niwm-x11 " NIWM_VERSION);
		return 0;
	}
	if (argc > 1) {
		fprintf(stderr, "usage: niwm-x11 [-v]\n");
		return 1;
	}

	conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "niwm: cannot open display\n");
		return 1;
	}
	fcntl(xcb_get_file_descriptor(conn), F_SETFD, FD_CLOEXEC);
	if (setup() < 0)
		return 1;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	pfd[0].fd = xcb_get_file_descriptor(conn);
	pfd[0].events = POLLIN;
	if ((pfd[1].fd = ipc_init()) >= 0) {
		pfd[1].events = POLLIN;
		nfds = 2;
	}

	core_init(&x11_backend, 0, 0, scr->width_in_pixels, scr->height_in_pixels);
	update_monitors();
	scan();

	while (wm.running && !got_signal) {
		while ((ev = xcb_poll_for_event(conn))) {
			handle(ev);
			free(ev);
		}
		if (xcb_connection_has_error(conn))
			break;
		if (want_reload) {
			want_reload = 0;
			core_cmd("reload", NULL, 0);
		}
		ipc_publish(); /* bars hear about whatever just changed */
		xcb_flush(conn);
		pfd[1].revents = 0;
		if (poll(pfd, (nfds_t)nfds, -1) < 0 && errno != EINTR)
			break;
		if (nfds == 2 && (pfd[1].revents & POLLIN))
			ipc_handle();
	}

	cleanup();
	return 0;
}
