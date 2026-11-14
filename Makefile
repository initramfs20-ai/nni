# nni - shared core of the nni family (YnniOS, SGnniOS)
#
#   make                                  build nni-splash
#   make install DESTDIR=... DISTRO=ynnios    nni-base files for YnniOS
#   make install DESTDIR=... DISTRO=sgnnios   nni-base files for SGnniOS
#   make install-artwork DESTDIR=...      wallpapers, themes, desktop menu entries, icewm theme, GRUB picture
#   make check                            syntax-check every script

VERSION  := $(shell cat VERSION)
CODENAME := $(shell cat CODENAME)
DISTRO   ?= ynnios
REPO_URL ?= https://github.com/YOUR-GITHUB-USER/nni
PREFIX   ?= /usr
CC       ?= cc
CFLAGS   ?= -O2
SHARE     = $(DESTDIR)$(PREFIX)/share/nni

all: out/nni-splash

out/nni-splash: src/splash/nni-splash.c
	mkdir -p out
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -o $@ $< $(LDFLAGS)

install: all
	@case "$(DISTRO)" in ynnios|sgnnios) ;; *) echo "DISTRO must be ynnios or sgnnios"; exit 1 ;; esac
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/nni $(SHARE)/logos $(SHARE)/fastfetch
	install -d $(DESTDIR)/etc/nni/services $(DESTDIR)/etc/profile.d
	install -m755 src/bin/nni src/bin/nni-theme src/bin/svc src/bin/wifi src/bin/handbook $(DESTDIR)$(PREFIX)/bin/
	install -m755 out/nni-splash $(DESTDIR)$(PREFIX)/bin/
	if [ "$(DISTRO)" = ynnios ]; then install -m755 src/bin/pol src/bin/ynnios-wallpaper $(DESTDIR)$(PREFIX)/bin/; fi
	if [ "$(DISTRO)" = sgnnios ]; then install -m755 src/bin/spol $(DESTDIR)$(PREFIX)/bin/; fi
	install -d $(DESTDIR)$(PREFIX)/share/man/man1 $(DESTDIR)$(PREFIX)/share/man/man5 $(DESTDIR)$(PREFIX)/share/man/man8
	for m in src/man/*.[158]; do \
	  n=$$(basename $$m); sec=$${n##*.}; \
	  case "$$n:$(DISTRO)" in pol.8:sgnnios|spol.8:ynnios) continue ;; esac; \
	  sed 's/@VERSION@/$(VERSION)/g' $$m > $(DESTDIR)$(PREFIX)/share/man/man$$sec/$$n; \
	done
	install -m755 src/lib/rc.sysinit src/lib/udhcpc.script $(DESTDIR)$(PREFIX)/lib/nni/
	install -m644 src/services/*.svc $(DESTDIR)/etc/nni/services/
	install -m644 src/profile.d/nni.sh $(DESTDIR)/etc/profile.d/
	install -m644 src/share/logos/*.txt $(SHARE)/logos/
	install -m644 src/share/fastfetch/$(DISTRO).jsonc $(SHARE)/fastfetch/
	install -m644 src/share/handbook-$(DISTRO).txt $(SHARE)/
	install -m644 src/share/xresources $(SHARE)/
	install -m644 artwork/generated/splash-$(DISTRO).ppm $(SHARE)/splash.ppm
	if [ "$(DISTRO)" = sgnnios ]; then install -Dm644 artwork/generated/logo_linux_clut224.ppm $(SHARE)/kernel/logo_linux_clut224.ppm; fi
	printf '%s\n' "$(VERSION)" > $(SHARE)/VERSION
	printf '%s\n' "$(CODENAME)" > $(SHARE)/CODENAME
	printf '%s\n' "$(REPO_URL)" > $(SHARE)/home_url

install-artwork:
	install -d $(DESTDIR)$(PREFIX)/share/backgrounds/nni $(DESTDIR)$(PREFIX)/share/icewm/themes/Ynni $(SHARE)/grub
	install -d $(DESTDIR)$(PREFIX)/share/applications $(SHARE)/themes
	install -m644 artwork/wallpapers/*.png $(DESTDIR)$(PREFIX)/share/backgrounds/nni/
	install -m644 artwork/themes/*.theme $(SHARE)/themes/
	install -m644 src/share/applications/*.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -m644 artwork/icewm/Ynni/default.theme $(DESTDIR)$(PREFIX)/share/icewm/themes/Ynni/
	install -m644 artwork/generated/grub-background.png $(SHARE)/grub/background.png

check:
	for f in src/bin/* src/lib/* src/profile.d/*; do sh -n "$$f" || exit 1; done
	@if command -v groff >/dev/null; then for m in src/man/*.[158]; do w=$$(groff -man -Tutf8 -ww -z $$m 2>&1); [ -z "$$w" ] || { echo "$$m: $$w"; exit 1; }; done; echo "man pages are clean"; fi
	@echo "all scripts parse"

clean:
	rm -rf out

.PHONY: all install install-artwork check clean
