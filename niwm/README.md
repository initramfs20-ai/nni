# NiWM

A lightweight tiling and floating window manager for the nni family
(YnniOS and SGnniOS), in the spirit of awesome but without Lua or a
widget toolkit. One backend-agnostic core, small per-display-server
backends.

**Status: 0.6.0.** The X11 backend and nibar work. Wayland is planned (see `wayland/`).

## Memory

Measured under Xvfb with 5 windows open:

| | niwm-x11 | nibar |
|---|---|---|
| PSS (real cost) | ~0.47 MB | ~0.45 MB |
| RSS | ~2.2 MB | ~2.4 MB |

The only runtime dependency is `libxcb`. Measure your own with
`grep Pss /proc/$(pidof niwm-x11)/smaps_rollup`; RSS counts shared
libraries in full, so it overstates the cost.

## Build

```sh
# YnniOS / Debian:  pol install build-essential libxcb1-dev pkg-config
#                   (optional: libxcb-randr0-dev for multiple monitors,
#                    libxcb-res0-dev for better swallowing)
# SGnniOS / Gentoo: spol x11-libs/libxcb
make
make install PREFIX=/usr          # or: make install-strip PREFIX=/usr
```

Start it from `~/.xinitrc` with `exec niwm-x11`, or pick **NiWM** in a
display manager (`niwm.desktop` is installed to `share/xsessions`).

## Layout

```
core/      tags, layouts, focus, rules, config, IPC (no X or Wayland code)
x11/       niwm-x11, the XCB backend
ctl/       niwmctl, the command-line client
bar/       nibar, the vertical taskbar and apps menu
wayland/   niwm-wl, planned (wlroots)
niwmrc     stock config, also compiled in as the fallback
```

## Features

- **Tags, awesome-style.** A window can be on several tags, and you can view several tags at once. Each tag keeps its own layout, mfact and nmaster.
- **Layouts:** tile, monocle, grid and float. Any window can float on its own.
- **Mouse:** mod+drag moves a window, mod+right-drag resizes it. Click to focus; focus-follows-mouse is optional.
- **Rules:** match on class, instance or title, then float, tile or send to a tag.
- **EWMH basics:** fullscreen, active window, client list, current desktop. Dialogs, transients and fixed-size windows float automatically. Docks are left untiled.
- **Smart borders:** a window that is alone, or in monocle, gets no border.
- **One command language.** The config file, key bindings and `niwmctl` all use the same commands.
- **Bars get their own space:** `_NET_WM_STRUT` is honoured, so nibar, lemonbar, polybar or tint2 never sit under tiled windows.
- **Scriptable:** `niwmctl subscribe` streams a state block on every change.
- **Scratchpad:** mod+s drops a terminal down from the top of the screen and hides it again, still running. It follows you to whatever tag you're on, and any program can be the scratchpad.
- **Multiple monitors:** each has its own tags, layouts and bar, as in awesome. mod+period and mod+comma move between monitors, and adding shift sends the window over. Hotplugging works, and so does `xrandr --setmonitor` for splitting one screen.
- **Swallowing:** run `mpv video.mp4` from a terminal and the video takes the terminal's place until it closes. This works with terminals like lxterminal that run every window from one process, and `rule class=X noswallow` turns it off per app.
- **Finds your terminal:** `$TERMINAL` is set to the first installed terminal, or to the one in `set terminal`.

## nibar

A vertical taskbar that ships with NiWM and is started by the stock
niwmrc. Top to bottom: an applications menu (the chicken), a 3×3 tag
grid, the layout, the window list, then battery, network and a pixel
clock. It takes its accent colours from NiWM's border colours, so the
two always match.

The menu reads `.desktop` files each time it opens, so newly installed
programs show up straight away. Open it with mod+p, then type to filter.

nibar is also a **system tray** (nm-applet, volume icons, chat apps), and
it has its own config, `~/.config/niwm/nibarc`. Unlike niwmrc it only
needs the lines you change:

```
layer beside          # fullscreen windows leave the bar visible
accent #3355ff        # instead of following niwm's focus colour
clock 12h
tray_hide Blueman-applet
```

mod+ctrl+r reloads it along with niwmrc. See `man nibar`.

## Configure

Every line of `niwmrc` is a command you can also send live:

```sh
niwmctl set gap 12
niwmctl layout grid
niwmctl rule class=Gimp float
niwmctl bind mod+e spawn pcmanfm
niwmctl query          # state for bars and scripts
niwmctl help
```

`~/.config/niwm` does not exist until you create it. Your niwmrc replaces the stock one rather than adding to it, so start from a copy:

```sh
mkdir -p ~/.config/niwm && cp /etc/niwm/niwmrc ~/.config/niwm/
```

The man page (SETTING UP NIWMRC) has a full example.
Reload with mod+ctrl+r, `niwmctl reload` or `kill -HUP`. `man niwm`
lists the default keys.

Java apps need `export _JAVA_AWT_WM_NONREPARENTING=1`, the same as with
any non-reparenting WM.

## Roadmap

1. **M1** (done): core, `niwm-x11`, `niwmctl`
2. **M2** (done): multiple monitors (RandR 1.5), struts
3. **M3** (done): `nibar`, fed by `niwmctl subscribe`
4. **M4:** `niwm-wl` on a pinned wlroots release
5. **M5:** `pol` and `spol` packages
