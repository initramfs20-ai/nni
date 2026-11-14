#!/bin/bash
# YnniOS kernel builder: chickens instead of penguins at boot.
#
# Debian's kernel is built without the boot logo, so no setting can bring it
# back. This builds Debian's own kernel again (same source with Debian's
# patches, same configuration, so the same drivers) with the logo turned on
# and the nni chicken as the picture: one chicken per CPU core, top left.
#
#   sudo bash builders/ynnios/build-kernel.sh
#
# The result lands in dist/kernel/:
#   linux-image-6.12.N+ynnios_..._amd64.deb     the kernel and its modules
#   linux-headers-6.12.N+ynnios_..._amd64.deb   only needed to compile extra drivers
# build-iso.sh picks the kernel up from there by itself. On an installed YnniOS:
#   pol install ./linux-image-6.12.N+ynnios_*.deb
#
# It takes 2-3 hours on a 4-core PC and about 20 GB of disk while it runs.
# Run it again whenever Debian updates its kernel (pol upgrade shows a new
# linux-image): it only rebuilds when the source has changed.
#
# Host deps: debootstrap   (sudo apt install debootstrap)
set -euo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SUITE="${SUITE:-trixie}"
ARCH="${ARCH:-amd64}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
WORK="${WORK:-/var/tmp/ynnios-build}"        # big scratch folder (never inside the repo)
OUTDIR="${OUTDIR:-$TOP/dist/kernel}"
REV="${REV:-1}"                              # raise to rebuild the same kernel version with changes
JOBS="${JOBS:-$(nproc)}"
FORCE="${FORCE:-0}"                          # 1 = rebuild even when this version is already built
if [[ "$WORK" =~ [[:space:]] ]]; then
  echo "NOTE: '$WORK' contains spaces and debootstrap can't handle that; using /var/tmp/ynnios-build"
  WORK=/var/tmp/ynnios-build
fi
KROOT="$WORK/kernel-root"                    # the build system, kept between runs
LOGO="$TOP/artwork/generated/logo_linux_clut224.ppm"

die() { echo "build-kernel: $*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run as root (sudo bash builders/ynnios/build-kernel.sh)"
command -v debootstrap >/dev/null || die "missing debootstrap (sudo apt install debootstrap)"
[ -f "$LOGO" ] || die "no logo at $LOGO - run this from inside the nni repo"
[ -f "$TOP/builders/ynnios/kernel-inner.sh" ] || die "builders/ynnios/kernel-inner.sh is missing"
mkdir -p "$WORK"
free_gb=$(df -BG --output=avail "$WORK" | tail -1 | tr -dc 0-9)
[ "$free_gb" -ge 20 ] || die "$WORK has ${free_gb} GB free; the build needs about 20 GB (set WORK=/some/bigger/folder)"

mount_chroot()  { local r=$1; mount --bind /dev "$r/dev"; mount -t devpts devpts "$r/dev/pts"
                  mount -t proc proc "$r/proc"; mount -t sysfs sys "$r/sys"; }
umount_chroot() { local r=$1; for m in dev/pts dev proc sys; do umount -lf "$r/$m" 2>/dev/null || true; done; }
trap 'umount_chroot "$KROOT"' EXIT
apt_in()  { chroot "$KROOT" /usr/bin/env DEBIAN_FRONTEND=noninteractive apt-get "$@"; }
in_root() { chroot "$KROOT" /usr/bin/env -i HOME=/root TERM=linux PATH=/usr/sbin:/usr/bin:/sbin:/bin "$@"; }

# ------------------------------------------------------------ build system
# A small Debian of the same release as YnniOS, so the kernel is compiled by
# the same compiler Debian uses, whatever this PC runs.
if [ ! -x "$KROOT/usr/bin/apt-get" ]; then
  echo "==> debootstrap build system ($SUITE/$ARCH)"
  rm -rf "$KROOT"; mkdir -p "$KROOT"
  debootstrap --arch="$ARCH" --variant=minbase "$SUITE" "$KROOT" "$MIRROR"
  cat > "$KROOT/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main
deb $MIRROR $SUITE-updates main
deb http://security.debian.org/debian-security $SUITE-security main
EOF
  echo 'APT::Install-Recommends "false";' > "$KROOT/etc/apt/apt.conf.d/99ynnios"
fi
umount_chroot "$KROOT"; mount_chroot "$KROOT"

echo "==> build tools and Debian's kernel source"
apt_in update
apt_in dist-upgrade -y
apt_in install -y build-essential bc bison flex libelf-dev libssl-dev debhelper \
       rsync kmod cpio xz-utils zstd python3 openssl ca-certificates

# Debian's kernel source of this release: the newest linux-source-N.M package
series=$(in_root apt-cache search --names-only '^linux-source-[0-9]+\.[0-9]+$' | cut -d' ' -f1 |
         sed 's/^linux-source-//' | sort -V | tail -1)
[ -n "$series" ] || die "Debian $SUITE has no linux-source-N.M package (is the mirror reachable?)"
apt_in install -y "linux-source-$series" "linux-config-$series"
tarball=$(ls "$KROOT"/usr/src/linux-source-"$series".tar.* 2>/dev/null | head -1)
[ -n "$tarball" ] || die "linux-source-$series installed no /usr/src/linux-source-$series.tar.*"
srcver=$(in_root dpkg-query -W -f='${Version}' "linux-source-$series")          # e.g. 6.12.57-1

# Debian's configuration for a normal PC kernel (not the cloud or real-time ones)
cdir="$KROOT/usr/src/linux-config-$series"
config=$(ls "$cdir"/config."${ARCH}"_none_"${ARCH}"* 2>/dev/null | head -1)
if [ -z "$config" ]; then
  echo "configurations in linux-config-$series:" >&2; ls "$cdir" >&2 || true
  die "no Debian configuration named config.${ARCH}_none_${ARCH} there"
fi

# ---------------------------------------------------------------- up to date?
stamp="$OUTDIR/.built-from"
want="linux-source-$series $srcver rev $REV logo $(sha256sum "$LOGO" | cut -c1-16)"
if [ "$FORCE" != 1 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ] &&
   ls "$OUTDIR"/linux-image-*+ynnios_*.deb >/dev/null 2>&1; then
  echo "==> already built from Debian's kernel source $srcver: $(cd "$OUTDIR" && ls linux-image-*+ynnios_*.deb | tail -1)"
  echo "    (FORCE=1 builds it again)"
  exit 0
fi

# --------------------------------------------------------------------- build
echo "==> unpacking Debian's kernel source $srcver"
B="$KROOT/usr/src/ynnios-kernel"
rm -rf "$B"; mkdir -p "$B/out"
tar -xf "$tarball" -C "$B"
src=$(ls -d "$B"/linux-source-*/ 2>/dev/null | head -1); src=${src%/}
[ -n "$src" ] && [ -f "$src/Makefile" ] || die "$tarball did not unpack into a kernel source folder"
case "$config" in
  *.xz) xz -dc "$config" > "$B/debian.config" ;;
  *.gz) gzip -dc "$config" > "$B/debian.config" ;;
  *)    cp "$config" "$B/debian.config" ;;
esac
grep -q '^CONFIG_X86_64=y' "$B/debian.config" || [ "$ARCH" != amd64 ] || die "$config does not look like a kernel configuration"
cp "$LOGO" "$B/logo.ppm"
cp "$TOP/builders/ynnios/kernel-inner.sh" "$B/kernel-inner.sh"

echo "==> compiling (2-3 hours on 4 cores; the log is $WORK/kernel-build.log)"
start=$(date +%s)
if ! in_root env SRC="/usr/src/ynnios-kernel/$(basename "$src")" CONFIG=/usr/src/ynnios-kernel/debian.config \
       LOGO=/usr/src/ynnios-kernel/logo.ppm OUT=/usr/src/ynnios-kernel/out REV="$REV" JOBS="$JOBS" \
       bash /usr/src/ynnios-kernel/kernel-inner.sh > "$WORK/kernel-build.log" 2>&1; then
  echo "---- last lines of $WORK/kernel-build.log ----" >&2
  tail -n 25 "$WORK/kernel-build.log" >&2
  die "the kernel build failed (full log: $WORK/kernel-build.log)"
fi
mins=$(( ($(date +%s) - start) / 60 ))

# ------------------------------------------------------------------- collect
mkdir -p "$OUTDIR"
rm -f "$OUTDIR"/linux-image-*+ynnios_*.deb "$OUTDIR"/linux-headers-*+ynnios_*.deb
cp "$B"/out/*.deb "$OUTDIR/"
echo "$want" > "$stamp"
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER": "$OUTDIR" 2>/dev/null || true
rm -rf "$B"                                   # the build tree is several GB; the debs are what matter

image=$(cd "$OUTDIR" && ls linux-image-*+ynnios_*.deb | tail -1)
echo "==> built in $mins minutes: $OUTDIR/$image ($(du -h "$OUTDIR/$image" | cut -f1))"
echo "    ISO:        sudo bash builders/ynnios/build-iso.sh all     (uses it by itself)"
echo "    installed:  copy it over, then  pol install ./$image"
