/*
 * niwm.h - NiWM shared core.
 *
 * Nothing in core/ knows about X11 or Wayland. A backend fills in the
 * `backend` table below, translates its events into core_* calls, and
 * the core decides where every window goes.
 */
#ifndef NIWM_H
#define NIWM_H

#include <stddef.h>
#include <stdint.h>

#define NIWM_VERSION "0.7.0"
#define MAX_TAGS 9
#define MAX_MONS 8

/* Modifier bits. They match the X11 masks so the X backend needs no
 * translation; the Wayland backend maps xkb modifiers onto them. */
#define MOD_SHIFT (1u << 0)
#define MOD_CTRL  (1u << 2)
#define MOD_ALT   (1u << 3)
#define MOD_SUPER (1u << 6)
#define MOD_MASK  (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_SUPER)
#define MOD_MOD   (1u << 15) /* "mod" in a binding, resolved to cfg.modkey */

enum { LAYOUT_TILE, LAYOUT_MONOCLE, LAYOUT_GRID, LAYOUT_FLOAT, LAYOUT_COUNT };

typedef struct client client;
typedef struct monitor monitor;
struct client {
	client *next;        /* tiling order (head = master) */
	client *fnext;       /* focus history (head = most recent) */
	uintptr_t id;        /* backend window handle */
	unsigned bflags;     /* backend-private flags */
	int x, y, w, h, bw;  /* last applied geometry: outer x/y, inner w/h */
	int fx, fy, fw, fh;  /* floating geometry: outer x/y, inner w/h */
	uint32_t tags;
	monitor *mon;         /* the screen it lives on */
	int floating, fullscreen, urgent;
	int scratch, scratch_placed;   /* the drop-down scratchpad */
	int pid;                       /* 0 = unknown */
	int terminal, noswallow;
	client *swallowed;             /* the terminal this window is standing in for */
	char class[64], instance[64], title[128];
};

typedef struct {
	uint32_t mods, keysym;
	char *action;
} keybind;

typedef struct {
	char *class, *instance, *title; /* NULL = match anything */
	int floating;                   /* -1 = leave alone */
	int scratch;                    /* 1 = becomes the scratchpad */
	int terminal, noswallow;        /* 1 = set that flag */
	uint32_t tags;                  /* 0 = leave alone */
	int mon;                        /* -1 = leave alone */
} rule;

typedef struct {
	int border, gap, ntags;
	int focus_follows_mouse, smart_borders;
	int pad_top, pad_bottom;
	int scratch_w, scratch_h;  /* scratchpad size, % of the work area */
	int swallow;
	uint32_t focus_color, normal_color, urgent_color;
	uint32_t modkey;
	float mfact;
	int nmaster, layout;
	keybind *binds;
	size_t nbinds;
	rule *rules;
	size_t nrules;
} config;

/* One screen. Every monitor has its own tags, layouts and bars, like
 * awesome; windows live on one monitor. */
struct monitor {
	int num;                   /* 0 = the primary monitor */
	int x, y, w, h;
	uint32_t tagset, prevtagset;
	int layout[MAX_TAGS];      /* awesome-style: every tag keeps its own */
	float mfact[MAX_TAGS];
	int nmaster[MAX_TAGS];
	int sl, sr, st, sb;        /* space reserved by bars (struts) */
	int fs_beside;             /* fullscreen windows leave the bars visible */
};

typedef struct {
	const char *name;
	void (*configure)(client *c, int x, int y, int w, int h, int bw);
	void (*hide)(client *c);
	void (*raise)(client *c);
	void (*focus)(client *c);           /* c may be NULL */
	void (*set_border_color)(client *c, uint32_t rgb);
	void (*close)(client *c);
	void (*fullscreen)(client *c, int on);
	void (*grab_keys)(void);
	void (*sync)(void);                 /* publish state (EWMH etc.) */
	char *(*resources)(void);           /* X resources text (malloc'd), or NULL */
} backend;

struct wm {
	config cfg;
	monitor mons[MAX_MONS];
	int nmons;
	monitor *selmon;           /* where new windows go and commands act */
	client *clients, *focus_stack, *sel;
	const backend *be;
	int running, loaded_once, loading;
	long scratch_pending;      /* next window until then is the scratchpad */
};
extern struct wm wm;

/* core.c */
void core_init(const backend *be, int x, int y, int w, int h);
void core_cleanup(void);
client *core_manage(uintptr_t id, int x, int y, int w, int h,
                    const char *class, const char *instance,
                    const char *title, int want_float, int pid);
void core_unmanage(client *c);
void core_unswallow_all(void);
client *core_find(uintptr_t id);
void core_arrange(void);
void core_refresh(void);
void core_focus(client *c);
void core_focus_dir(int dir);
void core_swap_dir(int dir);
void core_zoom(void);
void core_set_title(client *c, const char *title);
void core_set_urgent(client *c, int on);
void core_set_fullscreen(client *c, int on);
void core_set_floating(client *c, int on);
void core_float_geometry(client *c, int x, int y, int w, int h);
void core_set_monitors(const int (*rect)[4], int n); /* x y w h each */
void core_set_struts(monitor *m, int l, int r, int t, int b, int fs_beside);
monitor *core_monitor_at(int x, int y);
void core_pointer_at(int x, int y);  /* the pointer moved onto (x, y) */
void core_float_moved(client *c);    /* a drag ended: maybe a new monitor */
void core_focus_mon(monitor *m);
void core_send_to_mon(client *c, monitor *m);
void monitor_init(monitor *m);
void core_activate(client *c);   /* view its tag if hidden, then focus */
size_t core_snapshot(char *buf, size_t len);
int core_scratchpad(const char *cmd);   /* toggle; spawn cmd if none */
void core_scratch_resize(void);
void core_spawn(const char *cmd);
void core_pick_terminal(void);
int core_visible(const client *c);
int core_isfloat(const client *c);
int core_curtag(void);           /* on the selected monitor */
int mon_curtag(const monitor *m);
int core_key(uint32_t mods, uint32_t keysym);

/* cmd.c - the one command language used by the config file,
 * keybindings and niwmctl. Returns 0 on success, -1 on error. */
int core_cmd(const char *line, char *out, size_t outlen);
const char *layout_name(int l);

/* config.c */
void config_defaults(config *cfg);
void config_free(config *cfg);
void config_load(void);
uint32_t keysym_from_name(const char *name);
int parse_combo(const char *s, uint32_t *mods, uint32_t *sym);

/* ipc.c */
int ipc_init(void);
void ipc_handle(void);
void ipc_publish(void);          /* send a snapshot to subscribers if changed */
void ipc_emit(const char *text); /* send "event <text>" to subscribers */
void ipc_cleanup(void);

#endif
