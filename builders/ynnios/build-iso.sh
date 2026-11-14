#!/usr/bin/env bash
# YnniOS builder: debootstrap (Debian) + busybox as PID 1 + the nni core. No installer.
#
#   sudo bash builders/ynnios/build-iso.sh minimal    # -> ynnios-minimal.iso
#   sudo bash builders/ynnios/build-iso.sh full       # -> ynnios-full.iso  (Xorg + NiWM + drivers)
#   sudo bash builders/ynnios/build-iso.sh all        # both (default)
#
# Run it from anywhere inside the nni repo; the ISOs land in the folder you run it from.
# The YnniOS-specific parts (nni, svc, wifi, pol, boot script, logos, splash) come from
# the nni-base package, built from this repo on the fly; the full edition adds the
# NiWM desktop (the niwm package, built the same way) and nni-artwork (wallpapers,
# apps-menu entries).
#
# Host deps: debootstrap squashfs-tools xorriso mtools grub-pc-bin grub-efi-amd64-bin
#            build-essential dpkg-dev     (Debian 13 "trixie" or newer is the target)
set -euo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION=$(cat "$TOP/VERSION")
DISTRO_NAME="YnniOS"
SUITE="${SUITE:-trixie}"
ARCH="${ARCH:-amd64}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
HARD_MODE="${HARD_MODE:-1}"                  # 1 = strip mkfs*/mke2fs; no grub, no installer
WALLPAPER="${WALLPAPER:-1}"                  # default wallpaper (number or name, see nni wallpaper)
TIMEZONE="${TIMEZONE:-America/Argentina/Buenos_Aires}"   # any name under /usr/share/zoneinfo
NO_SYSTEMD="${NO_SYSTEMD:-1}"                # 1 = purge systemd; busybox init runs everything
SERVICES="${SERVICES:-syslogd}"              # services enabled at boot (svc list shows all)
WORK="${WORK:-/var/tmp/ynnios-build}"          # big scratch folder (never inside the repo)
# debootstrap breaks on spaces in its target path (e.g. a folder named "foo (Copy 1)")
if [[ "$WORK" =~ [[:space:]] ]]; then
  echo "NOTE: '$WORK' contains spaces and debootstrap can't handle that; using /var/tmp/ynnios-build"
  WORK=/var/tmp/ynnios-build
fi
BASE="$WORK/base"
DEBS="$WORK/debs"
EDITIONS="${1:-all}"; [ "$EDITIONS" = all ] && EDITIONS="minimal full"

COMMON_PKGS="linux-image-$ARCH live-boot initramfs-tools busybox-static kmod fastfetch tzdata ca-certificates"
# Full edition: desktop + broad driver/firmware coverage. Add more with EXTRA_FULL_PKGS.
FULL_PKGS="udev xserver-xorg-core xserver-xorg-input-all xserver-xorg-video-all \
xinit xauth x11-xserver-utils x11-xkb-utils xwallpaper xterm fonts-dejavu-core libgl1-mesa-dri \
firmware-linux firmware-iwlwifi firmware-realtek firmware-atheros firmware-brcm80211 \
firmware-misc-nonfree firmware-amd-graphics amd64-microcode \
wpasupplicant iw wireless-tools rfkill pciutils usbutils alsa-utils man-db manpages ${EXTRA_FULL_PKGS:-}"

# ----------------------------------------------------------------- pre-flight
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }
for t in debootstrap mksquashfs grub-mkrescue xorriso mformat make dpkg-deb; do
  command -v "$t" >/dev/null || { echo "missing host tool: $t (see Host deps at the top)"; exit 1; }
done
for d in i386-pc x86_64-efi; do   # without these, grub-mkrescue silently skips BIOS or UEFI
  [ -d "/usr/lib/grub/$d" ] || { echo "missing /usr/lib/grub/$d - install grub-pc-bin and grub-efi-amd64-bin"; exit 1; }
done
[ -f "$TOP/packaging/debian/build-deb.sh" ] || { echo "run this from inside the nni repo"; exit 1; }

mount_chroot()  { local r=$1; mount --bind /dev "$r/dev"; mount -t devpts devpts "$r/dev/pts"
                  mount -t proc proc "$r/proc"; mount -t sysfs sys "$r/sys"; }
umount_chroot() { local r=$1; for m in dev/pts dev proc sys; do umount -lf "$r/$m" 2>/dev/null || true; done; }
cleanup()       { for r in "$BASE" "$WORK"/rootfs-*; do [ -d "$r" ] && umount_chroot "$r"; done; return 0; }
trap cleanup EXIT

apt_in() { chroot "$1" /usr/bin/env DEBIAN_FRONTEND=noninteractive apt-get "${@:2}"; }
# run a command in the chroot with a clean environment (no host HOME or DISPLAY)
in_root() { chroot "$1" /usr/bin/env -i HOME=/root TERM=linux PATH=/usr/sbin:/usr/bin:/sbin:/bin:/opt/bb/bin "${@:2}"; }

# Remove systemd (PID 1, systemctl, journald, logind). libsystemd0/libudev1 stay: they are
# plain shared libraries that core packages link against. Debian's "udev" package stays too.
strip_systemd() {
  local R=$1 ed=$2 sim
  echo "==> removing systemd ($ed)"
  mkdir -p "$R/etc/apt/preferences.d"
  cat > "$R/etc/apt/preferences.d/no-systemd" <<'EOF'
Package: systemd systemd-sysv
Pin: release *
Pin-Priority: -1
EOF
  mount_chroot "$R"
  sim=$(chroot "$R" apt-get -s purge --allow-remove-essential systemd systemd-sysv init 2>&1 || true)
  if echo "$sim" | grep -E '^Remv (udev|busybox-static|live-boot|initramfs-tools|linux-image|kmod|util-linux|coreutils|dpkg|apt|libc6|nni-base)'; then
    umount_chroot "$R"
    echo "ERROR: purging systemd would also remove packages YnniOS needs (listed above)."
    echo "       Re-run with NO_SYSTEMD=0 to keep systemd, and send me this output."
    exit 1
  fi
  apt_in "$R" purge -y --allow-remove-essential systemd systemd-sysv init
  apt_in "$R" autoremove -y --purge
  umount_chroot "$R"
  if [ "$ed" = full ] && ! { [ -x "$R/usr/lib/systemd/systemd-udevd" ] || [ -x "$R/lib/systemd/systemd-udevd" ]; }; then
    echo "ERROR: systemd-udevd disappeared with systemd; the full edition needs it for Xorg input."
    echo "       Re-run with NO_SYSTEMD=0 and send me this message."
    exit 1
  fi
}

# ---------------------------------------------------------------- nni packages
build_nni() {
  echo "==> building the nni packages from $TOP"
  local url
  url=$(git -c safe.directory="$TOP" -C "$TOP" remote get-url origin 2>/dev/null || true)
  case "$url" in
    *github.com[:/]*) url=${url#*github.com[:/]}; url="https://github.com/${url%.git}" ;;
    *) url="https://github.com/YOUR-GITHUB-USER/nni" ;;
  esac
  rm -rf "$DEBS"; mkdir -p "$DEBS"
  sh "$TOP/packaging/debian/build-deb.sh" "$DEBS" "$url"
}

# ---------------------------------------------------------------- base system
build_base() {
  echo "==> debootstrap base ($SUITE/$ARCH)"
  rm -rf "$BASE"; mkdir -p "$BASE"
  debootstrap --arch="$ARCH" --variant=minbase \
    --components=main,contrib,non-free-firmware "$SUITE" "$BASE" "$MIRROR"

  cat > "$BASE/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main contrib non-free-firmware
deb $MIRROR $SUITE-updates main contrib non-free-firmware
deb http://security.debian.org/debian-security $SUITE-security main contrib non-free-firmware
EOF
  echo 'APT::Install-Recommends "false";' > "$BASE/etc/apt/apt.conf.d/99ynnios"

  # keep the image small: no docs, info pages or extra locales (man pages are kept)
  cat > "$BASE/etc/dpkg/dpkg.cfg.d/01_nodoc" <<'EOF'
path-exclude=/usr/share/doc/*
path-exclude=/usr/share/info/*
path-exclude=/usr/share/locale/*
path-include=/usr/share/locale/en*
EOF

  echo "$DISTRO_NAME" > "$BASE/etc/hostname"
  ln -sf "/usr/share/zoneinfo/$TIMEZONE" "$BASE/etc/localtime"
  echo "$TIMEZONE" > "$BASE/etc/timezone"

  mount_chroot "$BASE"
  apt_in "$BASE" update
  # shellcheck disable=SC2086
  apt_in "$BASE" install -y --no-install-recommends $COMMON_PKGS

  echo "==> busybox as PID 1 + the nni core"
  mkdir -p "$BASE/opt/bb/bin"
  ln -sf /bin/busybox "$BASE/opt/bb/init"                  # argv[0]=init -> init applet
  chroot "$BASE" /bin/busybox --install -s /opt/bb/bin     # fills gaps (fdisk, mkdosfs, etc.)
  mkdir -p "$BASE/tmp/nni"
  cp "$DEBS"/*.deb "$BASE/tmp/nni/"
  apt_in "$BASE" install -y "/tmp/nni/nni-base_${VERSION}_amd64.deb"
  umount_chroot "$BASE"

  cat > "$BASE/etc/inittab" <<'EOF'
::sysinit:/usr/lib/nni/rc.sysinit
tty1::respawn:-/bin/sh
tty2::respawn:-/bin/sh
::ctrlaltdel:/bin/busybox reboot
::shutdown:/usr/bin/svc shutdown
::shutdown:/bin/busybox umount -a -r
EOF

  cat > "$BASE/etc/profile" <<'EOF'
[ "$(id -u)" = 0 ] && export HOME=/root
export PATH=/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/opt/bb/bin
if [ "$(id -u)" = 0 ]; then NNI_P='#'; else NNI_P='$'; fi
export PS1="[$(id -un)@$(hostname) \$PWD]$NNI_P "
for f in /etc/profile.d/*.sh; do [ -r "$f" ] && . "$f"; done
unset f NNI_P
EOF

  # the nni apt repository (only once it exists on GitHub Pages)
  if [ -f "$TOP/docs/apt/nni-archive-keyring.gpg" ] && [ -f "$TOP/docs/apt/nni.sources" ] &&
     ! grep -qi 'your-github-user' "$TOP/docs/apt/nni.sources"; then
    echo "==> adding the nni apt repository"
    install -D -m 644 "$TOP/docs/apt/nni-archive-keyring.gpg" "$BASE/usr/share/keyrings/nni-archive-keyring.gpg"
    install -D -m 644 "$TOP/docs/apt/nni.sources" "$BASE/etc/apt/sources.list.d/nni.sources"
  else
    echo "==> (no published nni apt repo yet - nni updates come with new ISOs until you publish)"
  fi
}

# ------------------------------------------------------------- one edition
build_edition() {
  local ed=$1 R="$WORK/rootfs-$1" ISO="$WORK/iso-$1" OUT="$PWD/ynnios-$1.iso" vol
  echo "==> edition: $ed"
  rm -rf "$R" "$ISO"; cp -a "$BASE" "$R"; mkdir -p "$ISO/boot/grub" "$ISO/live"
  echo "$ed" > "$R/etc/ynnios-edition"          # older tools still read this

  mount_chroot "$R"
  in_root "$R" nni setup --distro ynnios --edition "$ed"
  for s in $SERVICES; do mkdir -p "$R/etc/nni/enabled"; touch "$R/etc/nni/enabled/$s"; done

  if [ "$ed" = full ]; then
    apt_in "$R" update
    # shellcheck disable=SC2086
    apt_in "$R" install -y --no-install-recommends $FULL_PKGS
    niwm_deb=$(cd "$DEBS" && ls niwm_*_amd64.deb | tail -n 1)
    apt_in "$R" install -y "/tmp/nni/nni-artwork_${VERSION}_all.deb" "/tmp/nni/$niwm_deb"

    echo "==> desktop setup (NiWM)"
    # NiWM's own defaults (/etc/niwm/niwmrc) start nibar, whose apps menu lists
    # the nni entries from nni-artwork next to every installed program
    in_root "$R" nni set WALLPAPER "$WALLPAPER" >/dev/null
    cp "$R/usr/share/nni/xresources" "$R/root/.Xresources"
    # keyboard layout, Xresources and wallpaper from /etc/nni.conf, then the desktop;
    # NiWM's messages (and nibar's) go to ~/.niwm.log
    printf '%s\n' 'nni apply --x' 'exec niwm-x11 2> "$HOME/.niwm.log"' > "$R/root/.xinitrc"
    # new users (adduser) inherit the desktop setup
    cp -a "$R/root/.xinitrc" "$R/root/.Xresources" "$R/etc/skel/"
    cat > "$R/etc/profile.d/ynnios-x.sh" <<'EOF'
# start X once on tty1; if X exits you get a shell (no crash loop)
if [ "$(tty)" = /dev/tty1 ] && [ ! -e "/tmp/.ynnios-x-$(id -u)" ]; then
  touch "/tmp/.ynnios-x-$(id -u)"; exec startx
fi
EOF
  fi
  umount_chroot "$R"
  rm -rf "$R/tmp/nni"

  if [ "$HARD_MODE" = 1 ]; then
    echo "==> hard mode: removing filesystem-creation tools"
    rm -f "$R"/usr/sbin/mkfs* "$R"/usr/sbin/mke2fs "$R"/usr/sbin/mkswap
  fi

  cp "$(ls "$R"/boot/vmlinuz-*    | sort -V | tail -1)" "$ISO/boot/vmlinuz"
  cp "$(ls "$R"/boot/initrd.img-* | sort -V | tail -1)" "$ISO/boot/initrd.img"
  if [ "$NO_SYSTEMD" = 1 ]; then strip_systemd "$R" "$ed"; fi
  apt_in "$R" clean; rm -rf "$R"/var/lib/apt/lists/*

  # pol replaces apt: divert the user-facing apt tools out of PATH (survives upgrades)
  echo "==> pol replaces apt"
  mkdir -p "$R/usr/lib/pol"
  for b in apt apt-get apt-cache; do
    chroot "$R" dpkg-divert --local --rename --divert "/usr/lib/pol/$b" --add "/usr/bin/$b"
  done

  mksquashfs "$R" "$ISO/live/filesystem.squashfs" -comp xz -b 1M \
    -wildcards -e 'boot/vmlinuz-*' 'boot/initrd.img-*'

  cp "$TOP/artwork/generated/grub-background.png" "$ISO/boot/grub/background.png"
  cat > "$ISO/boot/grub/grub.cfg" <<EOF
insmod all_video
if loadfont unicode ; then
  set gfxmode=auto
  # hand GRUB's graphics mode to the kernel: the screen then works from the very
  # first moment, so the boot logo and the splash can be drawn (BIOS and UEFI)
  set gfxpayload=keep
  insmod gfxterm
  insmod png
  terminal_output gfxterm
  background_image /boot/grub/background.png
  set menu_color_normal=white/black
  set menu_color_highlight=black/yellow
fi
set timeout=5
set default=0
menuentry "$DISTRO_NAME $VERSION $ed" {
  linux  /boot/vmlinuz boot=live init=/opt/bb/init quiet
  initrd /boot/initrd.img
}
menuentry "$DISTRO_NAME $ed (safe graphics)" {
  linux  /boot/vmlinuz boot=live init=/opt/bb/init nomodeset
  initrd /boot/initrd.img
}
menuentry "$DISTRO_NAME $ed (drop to initramfs shell)" {
  linux  /boot/vmlinuz boot=live init=/opt/bb/init break=premount
  initrd /boot/initrd.img
}
EOF
  vol="YNNIOS_${ed^^}"
  grub-mkrescue -o "$OUT" "$ISO" -- -volid "$vol"
  echo "==> $OUT: $(du -h "$OUT" | cut -f1)"
  if xorriso -indev "$OUT" -report_el_torito plain 2>/dev/null | grep -qi uefi; then
    echo "    UEFI boot entry: present (Secure Boot must be off; images are unsigned)"
  else
    echo "    WARNING: could not confirm a UEFI boot entry in $OUT"
  fi
}

build_nni
build_base
for ed in $EDITIONS; do build_edition "$ed"; done
echo "done. test with: qemu-system-x86_64 -enable-kvm -m 2G -cdrom ynnios-minimal.iso"
echo "UEFI test:       qemu-system-x86_64 -enable-kvm -m 3G -bios /usr/share/ovmf/OVMF.fd -cdrom ynnios-full.iso"
