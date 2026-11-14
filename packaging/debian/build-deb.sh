#!/bin/sh
# Build the YnniOS packages from this repo:
#   nni-base_VERSION_amd64.deb     the shared core (scripts, boot, splash, logos)
#   nni-artwork_VERSION_all.deb    wallpapers, themes, menu entries, GRUB picture (full edition)
#   niwm_NIWMVERSION_amd64.deb     the NiWM window manager and nibar (from niwm/)
#
#   sh packaging/debian/build-deb.sh [OUTDIR] [REPO_URL]
#
# Needs: make, a C compiler, dpkg-deb, dpkg-shlibdeps, pkg-config and the xcb
# headers: sudo apt install build-essential dpkg-dev pkg-config \
#              libxcb1-dev libxcb-randr0-dev libxcb-res0-dev
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$TOP/dist}
REPO_URL=${2:-https://github.com/YOUR-GITHUB-USER/nni}
VERSION=$(cat "$TOP/VERSION")
MAINTAINER=${NNI_MAINTAINER:-"nni project <nni@invalid>"}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

for t in make dpkg-deb dpkg-shlibdeps pkg-config; do
  command -v "$t" >/dev/null || { echo "missing $t (sudo apt install build-essential dpkg-dev pkg-config)"; exit 1; }
done
# NiWM is built with multi-monitor and swallowing support, so the package
# always means the same thing no matter which PC built it
pkg-config --exists xcb xcb-randr xcb-res ||
  { echo "missing xcb headers (sudo apt install libxcb1-dev libxcb-randr0-dev libxcb-res0-dev)"; exit 1; }

# ------------------------------------------------------------------ nni-base
B=$STAGE/nni-base
# static, so the program runs on any YnniOS no matter which glibc it has
make -C "$TOP" clean >/dev/null
make -C "$TOP" install DESTDIR="$B" DISTRO=ynnios REPO_URL="$REPO_URL" CFLAGS="-O2 -static -s" >/dev/null
gzip -9n "$B"/usr/share/man/man*/*          # Debian keeps man pages compressed
mkdir -p "$B/DEBIAN"
size=$(du -sk "$B" | cut -f1)
cat > "$B/DEBIAN/control" <<EOF
Package: nni-base
Version: $VERSION
Architecture: amd64
Maintainer: $MAINTAINER
Installed-Size: $size
Depends: busybox-static | busybox, fastfetch
Recommends: wpasupplicant, iw
Section: admin
Priority: optional
Homepage: $REPO_URL
Description: shared core of the nni family (YnniOS)
 The pieces that make YnniOS itself: the nni settings command and
 /etc/nni.conf, the svc service manager, the wifi helper, the pol package
 layer, the busybox boot script, the handbook, the chicken logos and the
 boot splash.
EOF
# files under /etc that dpkg must treat as configuration (never overwritten silently)
( cd "$B" && find etc -type f | sed 's|^|/|' | sort ) > "$B/DEBIAN/conffiles"

cat > "$B/DEBIAN/preinst" <<'EOF'
#!/bin/sh
set -e
# Debian's base-files would put its own os-release back on every upgrade;
# divert it so YnniOS keeps its name.
if [ "$1" = install ] || [ "$1" = upgrade ]; then
  if ! dpkg-divert --listpackage /etc/os-release 2>/dev/null | grep -qx nni-base; then
    [ -e /etc/os-release.debian ] && rm -f /etc/os-release.debian
    dpkg-divert --package nni-base --rename --divert /etc/os-release.debian --add /etc/os-release
  fi
fi
exit 0
EOF
cat > "$B/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
  /usr/bin/nni setup --distro ynnios || echo "nni-base: 'nni setup' had a problem; run it again by hand"
fi
exit 0
EOF
cat > "$B/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = purge ]; then
  rm -f /etc/os-release
  dpkg-divert --package nni-base --rename --remove /etc/os-release || true
fi
exit 0
EOF
chmod 755 "$B/DEBIAN/preinst" "$B/DEBIAN/postinst" "$B/DEBIAN/postrm"
( cd "$B" && find . -path ./DEBIAN -prune -o -type f -print | sed 's|^\./||' | sort | xargs md5sum ) > "$B/DEBIAN/md5sums"
dpkg-deb --root-owner-group -Zxz --build "$B" "$OUT/nni-base_${VERSION}_amd64.deb" >/dev/null

# ------------------------------------------------------------------ nni-artwork
A=$STAGE/nni-artwork
make -C "$TOP" install-artwork DESTDIR="$A" >/dev/null
mkdir -p "$A/DEBIAN"
size=$(du -sk "$A" | cut -f1)
cat > "$A/DEBIAN/control" <<EOF
Package: nni-artwork
Version: $VERSION
Architecture: all
Maintainer: $MAINTAINER
Installed-Size: $size
Depends: nni-base (>= $VERSION)
Section: x11
Priority: optional
Homepage: $REPO_URL
Description: wallpapers and desktop theme for the nni family
 The YnniOS wallpapers and the desktop themes made from them (nni theme
 list), the nni entries in the desktop's apps menu
 (handbook, settings, services, next wallpaper, reboot, power off), the
 GRUB menu background and the Ynni icewm theme. Pick a wallpaper with:
 nni wallpaper list
EOF
( cd "$A" && find . -path ./DEBIAN -prune -o -type f -print | sed 's|^\./||' | sort | xargs md5sum ) > "$A/DEBIAN/md5sums"
dpkg-deb --root-owner-group -Zxz --build "$A" "$OUT/nni-artwork_${VERSION}_all.deb" >/dev/null

# ------------------------------------------------------------------ niwm
# NiWM keeps its own version number, separate from the nni release.
NIWM_VERSION=$(sed -n 's/^#define NIWM_VERSION "\(.*\)"/\1/p' "$TOP/niwm/core/niwm.h")
[ -n "$NIWM_VERSION" ] || { echo "can't read NIWM_VERSION from niwm/core/niwm.h"; exit 1; }
N=$STAGE/niwm
make -C "$TOP/niwm" clean >/dev/null
make -C "$TOP/niwm" install-strip DESTDIR="$N" PREFIX=/usr SYSCONFDIR=/etc \
     RANDR=yes XRES=yes CFLAGS="-O2 -pipe" >/dev/null
make -C "$TOP/niwm" clean >/dev/null
gzip -9n "$N"/usr/share/man/man1/*
mkdir -p "$N/DEBIAN" "$STAGE/shlibs/debian"
# let dpkg work out which libraries (and versions) the programs need
printf 'Source: niwm\n\nPackage: niwm\nArchitecture: any\n' > "$STAGE/shlibs/debian/control"
deps=$(cd "$STAGE/shlibs" && dpkg-shlibdeps -O \
       "$N/usr/bin/niwm-x11" "$N/usr/bin/nibar" "$N/usr/bin/niwmctl" 2>/dev/null |
       sed -n 's/^shlibs:Depends=//p')
[ -n "$deps" ] || { echo "dpkg-shlibdeps found no libraries for niwm"; exit 1; }
size=$(du -sk "$N" | cut -f1)
cat > "$N/DEBIAN/control" <<EOF
Package: niwm
Version: $NIWM_VERSION
Architecture: $(dpkg --print-architecture)
Maintainer: $MAINTAINER
Installed-Size: $size
Depends: $deps
Recommends: xfonts-base, x11-xserver-utils
Suggests: x-terminal-emulator
Provides: x-window-manager
Section: x11
Priority: optional
Homepage: $REPO_URL
Description: lightweight tiling and floating window manager, with nibar
 NiWM tiles or floats windows on nine tags per monitor, in the spirit of
 awesome but without Lua or a widget toolkit: it uses well under 1 MB of
 memory. It comes with a drop-down scratchpad, window swallowing, multiple
 monitors, and nibar, a vertical taskbar with an applications menu and a
 system tray. Configure it in ~/.config/niwm/niwmrc; see man niwm.
EOF
( cd "$N" && find etc -type f | sed 's|^|/|' | sort ) > "$N/DEBIAN/conffiles"
( cd "$N" && find . -path ./DEBIAN -prune -o -type f -print | sed 's|^\./||' | sort | xargs md5sum ) > "$N/DEBIAN/md5sums"
dpkg-deb --root-owner-group -Zxz --build "$N" "$OUT/niwm_${NIWM_VERSION}_$(dpkg --print-architecture).deb" >/dev/null

make -C "$TOP" clean >/dev/null
echo "built: $OUT/nni-base_${VERSION}_amd64.deb"
echo "built: $OUT/nni-artwork_${VERSION}_all.deb"
echo "built: $OUT/niwm_${NIWM_VERSION}_$(dpkg --print-architecture).deb"
