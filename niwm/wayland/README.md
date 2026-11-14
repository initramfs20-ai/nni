# niwm-wl (milestone 4)

The Wayland backend, built on wlroots. It isn't written yet. The core is
ready for it: nothing in `core/` touches X11, and the `backend` table in
`core/niwm.h` is the whole contract.

Plan:

- **Pin one wlroots release** (0.18.x) and only bump it on purpose. Its API breaks between releases.
- **Protocols:** xdg-shell, layer-shell for bars and launchers, xdg-decoration (server-side borders), and Xwayland behind a build switch.
- **Input:** map xkb modifiers onto the `MOD_*` bits. Keysym numbers already match, so `keysym_from_name()` is shared.
- **Renderer:** GLES2 by default, pixman (software) as a low-RAM option.
- **IPC:** reuse `core/ipc.c` unchanged. The socket is named after `WAYLAND_DISPLAY`.
- **RAM target:** 15–25 MB PSS with GLES2. Mesa is most of that.
