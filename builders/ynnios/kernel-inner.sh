#!/bin/bash
# Runs inside the build chroot (called by build-kernel.sh): configure a kernel
# source tree, swap the boot logo for the chicken, and build the .deb packages.
#
#   SRC      unpacked kernel source
#   CONFIG   the configuration to start from (Debian's own)
#   LOGO     the chicken, a plain PPM with at most 224 colours
#   OUT      where the finished .deb files go
#   LOCALVER the kernel's name suffix (default +ynnios)
#   REV      package revision, raised when rebuilding the same kernel version
#   JOBS     parallel compile jobs
#   CONFIG_ONLY=1  stop after preparing and checking the configuration
set -euo pipefail
: "${SRC:?}" "${CONFIG:?}" "${LOGO:?}" "${OUT:?}"
REV="${REV:-1}"
JOBS="${JOBS:-$(nproc)}"
# "+ynnios" sorts above Debian's kernel of the same version ("+deb13-amd64"),
# so GRUB boots the chicken kernel first when both are installed
LOCALVER="${LOCALVER:-+ynnios}"

die() { echo "kernel: $*" >&2; exit 1; }
[ -f "$SRC/Makefile" ] && [ -d "$SRC/drivers/video/logo" ] || die "$SRC is not a kernel source tree"
[ -f "$CONFIG" ] || die "no configuration at $CONFIG"
head -c 2 "$LOGO" | grep -q '^P3' || die "$LOGO is not a plain (P3) PPM picture"

cd "$SRC"
cp "$CONFIG" .config

# chickens instead of penguins
cp "$LOGO" drivers/video/logo/logo_linux_clut224.ppm

# The kernel hides its logo when booted with "quiet", which is how YnniOS
# boots. Take that one check out, so the chickens show on a quiet boot too.
fbcon=drivers/video/fbdev/core/fbcon.c
if grep -A1 'console_loglevel <= CONSOLE_LOGLEVEL_QUIET' "$fbcon" | grep -q 'logo_shown = FBCON_LOGO_DONTSHOW'; then
  sed -i '/logo_shown < 0 && console_loglevel <= CONSOLE_LOGLEVEL_QUIET/,+1d' "$fbcon"
  if grep -q 'console_loglevel <= CONSOLE_LOGLEVEL_QUIET' "$fbcon"; then die "could not remove the quiet check from $fbcon"; fi
  echo "kernel: the logo will show on quiet boots too"
else
  echo "kernel: WARNING: this kernel version hides the logo differently; the chickens will only show when booting without 'quiet'" >&2
fi

cfg() { scripts/config --file .config "$@"; }
# the boot logo, which Debian leaves out
cfg --enable LOGO --enable LOGO_LINUX_CLUT224 --disable LOGO_LINUX_MONO --disable LOGO_LINUX_VGA16
# Draw the console (and so the logo) as soon as the screen works, even with
# "quiet". Debian waits for the first line of text, which hides the logo.
cfg --disable FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER
# our own name: 6.12.x+ynnios
cfg --set-str LOCALVERSION "$LOCALVER" --disable LOCALVERSION_AUTO
# Debian's configuration points at certificate files that only exist in
# Debian's own build. Without this the build stops at "No rule to make target".
cfg --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS ""
# Sign the modules with a key made for this build (and thrown away after it),
# so loading them doesn't mark the kernel as tainted.
cfg --enable MODULE_SIG --enable MODULE_SIG_ALL --set-str MODULE_SIG_KEY "certs/signing_key.pem"
# No debug information: it is only for kernel debuggers, and it would double
# the build time and need tens of GB of disk.
cfg --disable DEBUG_INFO_BTF --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT \
    --disable DEBUG_INFO_DWARF4 --disable DEBUG_INFO_DWARF5 --disable DEBUG_INFO \
    --enable DEBUG_INFO_NONE

make olddefconfig >/dev/null

# make sure every change survived (an option can be dropped when something it needs is off)
want() { grep -qx "$1" .config || die "the configuration lost '$1' (is the framebuffer console enabled in $CONFIG?)"; }
want 'CONFIG_LOGO=y'
want 'CONFIG_LOGO_LINUX_CLUT224=y'
want "CONFIG_LOCALVERSION=\"$LOCALVER\""
want 'CONFIG_SYSTEM_TRUSTED_KEYS=""'
if grep -q '^CONFIG_DEBUG_INFO_BTF=y' .config; then die "debug information is still on"; fi
if grep -q '^CONFIG_FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER=y' .config; then die "deferred console takeover is still on"; fi

release=$(make -s kernelrelease)
version=$(make -s kernelversion)
if [ "${CONFIG_ONLY:-0}" = 1 ]; then
  echo "kernel: configuration for $release is ready in $SRC/.config (CONFIG_ONLY=1: not compiling)"
  exit 0
fi
echo "kernel: building $release with $JOBS jobs ($(grep -c '=m$' .config) modules, $(grep -c '=y$' .config) built in)"

rm -f ../linux-*.deb ../linux-upstream_* 2>/dev/null || true
export KBUILD_BUILD_USER=nni KBUILD_BUILD_HOST=ynnios
make -j"$JOBS" bindeb-pkg KDEB_PKGVERSION="$version-ynnios$REV"

mkdir -p "$OUT"
# linux-libc-dev is built too, but Debian's own is the right one to keep
found=0
for f in ../linux-image-"$release"_*.deb ../linux-headers-"$release"_*.deb; do
  [ -f "$f" ] || continue
  cp "$f" "$OUT/"; found=1
done
[ "$found" = 1 ] && ls "$OUT"/linux-image-"$release"_*.deb >/dev/null 2>&1 || die "the build finished but made no linux-image package"
echo "$release" > "$OUT/.release"
echo "kernel: done: $(cd "$OUT" && ls linux-image-"$release"_*.deb)"
