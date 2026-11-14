# NiWM - builds on Debian (YnniOS) and Gentoo (SGnniOS) with plain make.
#   make                  niwm-x11 + niwmctl
#   make install PREFIX=/usr DESTDIR=...

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
XSESSIONS  ?= $(PREFIX)/share/xsessions
MANDIR     ?= $(PREFIX)/share/man

CC         ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -Os -pipe
override CFLAGS += -std=c99 -Wall -Wextra -pedantic -D_XOPEN_SOURCE=700 \
              -DSYSCONFDIR=\"$(SYSCONFDIR)\"
LDFLAGS    ?=

XCB_CFLAGS := $(shell $(PKG_CONFIG) --cflags xcb)
XCB_LIBS   := $(shell $(PKG_CONFIG) --libs xcb)

# Optional: XRes finds the process behind windows that don't set
# _NET_WM_PID, which helps swallowing. Used when xcb-res is installed
# (libxcb-res0-dev on Debian; part of x11-libs/libxcb on Gentoo).
# Build without it: make XRES=no
XRES ?= $(shell $(PKG_CONFIG) --exists xcb-res && echo yes)
ifeq ($(XRES),yes)
WM_CFLAGS += -DHAVE_XRES $(shell $(PKG_CONFIG) --cflags xcb-res)
WM_LIBS   += $(shell $(PKG_CONFIG) --libs xcb-res)
endif

# Multi-monitor support through RandR 1.5 (libxcb-randr0-dev on Debian;
# part of x11-libs/libxcb on Gentoo). Without it, NiWM treats the whole X
# screen as one monitor. Build without it: make RANDR=no
RANDR ?= $(shell $(PKG_CONFIG) --exists xcb-randr && echo yes)
ifeq ($(RANDR),yes)
WM_CFLAGS += -DHAVE_RANDR $(shell $(PKG_CONFIG) --cflags xcb-randr)
WM_LIBS   += $(shell $(PKG_CONFIG) --libs xcb-randr)
endif

CORE_SRC = core/core.c core/cmd.c core/config.c core/ipc.c
CORE_HDR = core/niwm.h core/sockpath.h

all: niwm-x11 niwmctl nibar

# niwmrc is compiled in as the fallback config, one string per line, so
# the two never drift apart
core/defconfig.h: niwmrc
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/",/' niwmrc > $@

niwm-x11: $(CORE_SRC) $(CORE_HDR) core/defconfig.h x11/x11.c
	$(CC) $(CFLAGS) $(XCB_CFLAGS) $(WM_CFLAGS) -o $@ $(CORE_SRC) x11/x11.c $(LDFLAGS) $(XCB_LIBS) $(WM_LIBS)

niwmctl: ctl/niwmctl.c $(CORE_HDR)
	$(CC) $(CFLAGS) -o $@ ctl/niwmctl.c $(LDFLAGS)

nibar: bar/nibar.c core/sockpath.h
	$(CC) $(CFLAGS) $(XCB_CFLAGS) -o $@ bar/nibar.c $(LDFLAGS) $(XCB_LIBS)

install: all
	install -Dm755 niwm-x11 $(DESTDIR)$(BINDIR)/niwm-x11
	install -Dm755 niwmctl $(DESTDIR)$(BINDIR)/niwmctl
	install -Dm755 nibar $(DESTDIR)$(BINDIR)/nibar
	install -Dm644 niwmrc $(DESTDIR)$(SYSCONFDIR)/niwm/niwmrc
	install -Dm644 nibarc $(DESTDIR)$(SYSCONFDIR)/niwm/nibarc
	install -Dm644 niwm.desktop $(DESTDIR)$(XSESSIONS)/niwm.desktop
	install -Dm644 niwm.1 $(DESTDIR)$(MANDIR)/man1/niwm.1
	install -Dm644 nibar.1 $(DESTDIR)$(MANDIR)/man1/nibar.1

install-strip: install
	strip $(DESTDIR)$(BINDIR)/niwm-x11 $(DESTDIR)$(BINDIR)/niwmctl $(DESTDIR)$(BINDIR)/nibar

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/niwm-x11 $(DESTDIR)$(BINDIR)/niwmctl $(DESTDIR)$(BINDIR)/nibar \
	      $(DESTDIR)$(XSESSIONS)/niwm.desktop $(DESTDIR)$(MANDIR)/man1/niwm.1 \
	      $(DESTDIR)$(MANDIR)/man1/nibar.1

clean:
	rm -f niwm-x11 niwmctl nibar core/defconfig.h

.PHONY: all install install-strip uninstall clean
