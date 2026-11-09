#!/usr/bin/env bash
# SGnniOS builder: Gentoo stage3 -> chroot -> bootable live ISO, with the nni core.
#
#   sudo bash builders/sgnnios/build-sgnnios.sh            # build (resumes where it stopped)
#   sudo FRESH=1 bash builders/sgnnios/build-sgnnios.sh    # wipe the work folder and start over
#   sudo CHICKEN_KERNEL=0 bash builders/sgnnios/build-sgnnios.sh   # prebuilt kernel, much faster
#
# Run it from anywhere inside the nni repo; the ISO lands in the folder you run it from.
# The SGnniOS-specific parts come from the app-misc/nni-base ebuild in this repo's overlay.
#
# Host deps (Debian/Ubuntu/Pop!_OS):
#   sudo apt install wget xz-utils tar squashfs-tools xorriso mtools \
#                    grub-pc-bin grub-efi-amd64-bin qemu-system-x86 ovmf
# Disk: ~25 GB free in WORK. RAM: 16 GB is fine for this stage.
#
# The first run compiles a few packages and takes a while. FEATURES=buildpkg keeps
# binary packages inside the work folder, so re-runs and resumes are much faster.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP="$(cd "$SRC/../.." && pwd)"
VERSION=$(cat "$TOP/VERSION")
DISTRO_NAME="SGnniOS"
WORK="${WORK:-/var/tmp/sgnnios-build}"
ROOT="$WORK/root"
DL="$WORK/dl"
ISO="$WORK/iso"
OUT="${OUT:-$PWD/sgnnios.iso}"

JOBS="${JOBS:-$(nproc)}"                     # Ryzen 3 3200G: 4
EMERGE_JOBS="${EMERGE_JOBS:-2}"              # parallel emerges; keep low on 16 GB RAM
# Portable code that also runs on old Intel laptops. Do NOT use -march=native here:
# a Zen+ build would use instructions the RV511 does not have.
ARCH_FLAGS="${ARCH_FLAGS:--march=x86-64 -mtune=generic}"
MIRROR="${GENTOO_MIRROR:-https://distfiles.gentoo.org}"
STAGE_KIND="${STAGE_KIND:-openrc}"           # openrc | desktop-openrc (desktop = faster Xorg later)
# 1 = compile the kernel with the chicken boot logo instead of tux (adds 1-3 hours on a 4-core CPU)
CHICKEN_KERNEL="${CHICKEN_KERNEL:-1}"
if [ "$CHICKEN_KERNEL" = 1 ]; then DEFAULT_KERNEL=sys-kernel/gentoo-kernel; else DEFAULT_KERNEL=sys-kernel/gentoo-kernel-bin; fi
KERNEL_PKG="${KERNEL_PKG:-$DEFAULT_KERNEL}"
SERVICES="${SERVICES:-syslogd}"              # services enabled at boot (svc list shows all)
TIMEZONE="${TIMEZONE:-America/Argentina/Buenos_Aires}"
FIRMWARE="${FIRMWARE:-1}"                    # 1 = sys-kernel/linux-firmware (Wi-Fi etc.), bigger ISO
HARD_MODE="${HARD_MODE:-1}"                  # 1 = no mkfs, no grub: deliberately hard to install
KEEP_TREE="${KEEP_TREE:-0}"                  # 1 = keep the Portage tree inside the ISO (bigger)
UPDATE_WORLD="${UPDATE_WORLD:-0}"            # 1 = emerge -uDN @world first (slow)
FRESH="${FRESH:-0}"

die() { echo "ERROR: $*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
[[ "$WORK" =~ [[:space:]] ]] && die "WORK must not contain spaces: '$WORK'"
for t in wget tar xz sha512sum mksquashfs grub-mkrescue xorriso mformat chroot; do
  command -v "$t" >/dev/null || die "missing host tool: $t (see Host deps at the top)"
done
for d in i386-pc x86_64-efi; do
  [ -d "/usr/lib/grub/$d" ] || die "missing /usr/lib/grub/$d - install grub-pc-bin and grub-efi-amd64-bin"
done
[ -f "$SRC/stage-a.sh" ] && [ -f "$TOP/tools/release.sh" ] || die "run this from inside the nni repo"

mount_chroot() {
  mountpoint -q "$ROOT/proc" || mount --types proc /proc "$ROOT/proc"
  mountpoint -q "$ROOT/sys"  || { mount --rbind /sys "$ROOT/sys"; mount --make-rslave "$ROOT/sys"; }
  mountpoint -q "$ROOT/dev"  || { mount --rbind /dev "$ROOT/dev"; mount --make-rslave "$ROOT/dev"; }
  mountpoint -q "$ROOT/run"  || { mount --bind /run "$ROOT/run"; mount --make-slave "$ROOT/run"; }
}
umount_chroot() {
  for m in run dev sys proc; do
    mountpoint -q "$ROOT/$m" 2>/dev/null && { umount -R "$ROOT/$m" 2>/dev/null || umount -l "$ROOT/$m" || true; }
  done
  return 0
}
trap umount_chroot EXIT

# NEVER delete the work folder while bind mounts of /dev, /sys, /proc are still inside it.
if [ "$FRESH" = 1 ] && [ -d "$WORK" ]; then
  umount_chroot
  grep -q " $WORK/" /proc/mounts && die "mounts still active under $WORK; unmount them first"
  rm -rf "$WORK"
fi
mkdir -p "$ROOT" "$DL"
avail_kb=$(df -Pk "$WORK" | awk 'NR==2 {print $4}')
[ "${FORCE:-0}" = 1 ] || [ "$avail_kb" -gt 25000000 ] || die "need ~25 GB free in $WORK (FORCE=1 to ignore)"

# ------------------------------------------------------------ 1. stage3
if [ ! -e "$ROOT/etc/portage" ]; then
  echo "==> finding the latest $STAGE_KIND stage3"
  base="$MIRROR/releases/amd64/autobuilds"
  wget -qO "$DL/latest.txt" "$base/latest-stage3-amd64-$STAGE_KIND.txt" || die "could not fetch the stage3 index"
  rel=$(awk '!/^#/ && /\.tar\.xz/ {print $1; exit}' "$DL/latest.txt")
  [ -n "$rel" ] || die "could not parse $DL/latest.txt (the format may have changed)"
  name=$(basename "$rel")
  echo "    $name"
  wget -c -O "$DL/$name" "$base/$rel"
  wget -qO "$DL/$name.DIGESTS" "$base/$rel.DIGESTS" || die "could not fetch .DIGESTS"
  want=$(awk -v n="$name" '/SHA512 HASH/ {f=1; next} f && $2==n {print $1; exit}' "$DL/$name.DIGESTS")
  have=$(sha512sum "$DL/$name" | awk '{print $1}')
  [ -n "$want" ] && [ "$want" = "$have" ] || die "stage3 checksum mismatch or unreadable .DIGESTS - verify $DL/$name by hand (see the Gentoo handbook)"
  echo "==> unpacking stage3 (checksum OK)"
  tar xpf "$DL/$name" --xattrs-include='*.*' --numeric-owner -C "$ROOT"
fi

# ------------------------------------------------------------ 2. configure
echo "==> configuring Portage"
mkdir -p "$ROOT/etc/portage"
sed -i '/^# --- SGnniOS begin/,$d' "$ROOT/etc/portage/make.conf"
cat >> "$ROOT/etc/portage/make.conf" <<EOF
# --- SGnniOS begin (generated by build-sgnnios.sh; edits below this line are replaced)
COMMON_FLAGS="-O2 -pipe $ARCH_FLAGS"
CFLAGS="\${COMMON_FLAGS}"
CXXFLAGS="\${COMMON_FLAGS}"
FCFLAGS="\${COMMON_FLAGS}"
FFLAGS="\${COMMON_FLAGS}"
MAKEOPTS="-j$JOBS"
EMERGE_DEFAULT_OPTS="--jobs=$EMERGE_JOBS --load-average=$JOBS"
FEATURES="buildpkg"
ACCEPT_LICENSE="-* @FREE @BINARY-REDISTRIBUTABLE"
GENTOO_MIRRORS="$MIRROR"
# busybox is PID 1 on SGnniOS: never let sysvinit put its own init/shutdown tools back
INSTALL_MASK="/sbin/init /sbin/halt /sbin/poweroff /sbin/reboot /sbin/shutdown /sbin/telinit /sbin/runlevel"
LC_MESSAGES=C.utf8
EOF
cp --dereference /etc/resolv.conf "$ROOT/etc/resolv.conf"

cat > "$ROOT/etc/sgnnios-build.env" <<EOF
KERNEL_PKG="$KERNEL_PKG"
CHICKEN_KERNEL="$CHICKEN_KERNEL"
SERVICES="$SERVICES"
TIMEZONE="$TIMEZONE"
FIRMWARE="$FIRMWARE"
HARD_MODE="$HARD_MODE"
UPDATE_WORLD="$UPDATE_WORLD"
EOF

echo "==> nni $VERSION: source tarball + overlay"
# Reuse what "tools/release.sh build" made, so the checksum matches the published release.
# REBUILD_NNI=1 makes a fresh one from your working copy (for testing local changes).
if [ "${REBUILD_NNI:-0}" = 1 ] || [ ! -f "$TOP/dist/nni-$VERSION.tar.gz" ] || [ ! -f "$TOP/app-misc/nni-base/nni-base-$VERSION.ebuild" ]; then
  sh "$TOP/tools/release.sh" source
  chown -R --reference="$TOP" "$TOP/dist" "$TOP/app-misc" 2>/dev/null || true   # keep them yours, not root's
fi
rm -rf "$ROOT/var/db/repos/nni"
mkdir -p "$ROOT/var/db/repos/nni" "$ROOT/var/cache/distfiles" "$ROOT/etc/portage/repos.conf"
cp -a "$TOP/profiles" "$TOP/metadata" "$TOP/app-misc" "$ROOT/var/db/repos/nni/"
cp "$TOP/dist/nni-$VERSION.tar.gz" "$ROOT/var/cache/distfiles/"
# a plain local overlay for now; "nni update" switches it to the GitHub one later
cat > "$ROOT/etc/portage/repos.conf/nni.conf" <<EOF
[nni]
location = /var/db/repos/nni
EOF
install -m 755 "$SRC/stage-a.sh" "$ROOT/root/stage-a.sh"

# ------------------------------------------------------------ 3. build inside the chroot
echo "==> entering the chroot (this is the long part)"
mount_chroot
if ! chroot "$ROOT" /usr/bin/env -i HOME=/root TERM="${TERM:-linux}" /bin/bash -l /root/stage-a.sh; then
  umount_chroot
  die "the build inside the chroot FAILED, so no ISO was made. Scroll up to the last emerge error, fix it, then re-run the same command (it resumes where it stopped)."
fi
umount_chroot

# ------------------------------------------------------------ 4. ISO
echo "==> assembling the ISO"
rm -rf "$ISO"; mkdir -p "$ISO/boot/grub" "$ISO/LiveOS"
kernel=$(ls "$ROOT"/boot/vmlinuz-* 2>/dev/null | sort -V | tail -1)
initrd=$(ls "$ROOT"/boot/initramfs-*.img 2>/dev/null | sort -V | tail -1)
[ -n "$kernel" ] && [ -n "$initrd" ] || die "no kernel/initramfs in $ROOT/boot - see the emerge/dracut output above"
cp "$kernel" "$ISO/boot/vmlinuz"
cp "$initrd" "$ISO/boot/initramfs.img"
cp "$TOP/artwork/generated/grub-background.png" "$ISO/boot/grub/background.png"

EXCL=('var/cache/distfiles/*' 'var/cache/binpkgs/*' 'var/tmp/portage/*' 'root/stage-a.sh' 'root/.bash_history')
[ "$KEEP_TREE" = 1 ] || EXCL+=('var/db/repos/gentoo/*')
if [ "$HARD_MODE" = 1 ]; then
  echo "==> hard mode: leaving mkfs, mke2fs, mkswap and GRUB tools out of the ISO"
  for d in sbin usr/sbin usr/bin; do
    EXCL+=("$d/mkfs*" "$d/mke2fs" "$d/mkswap" "$d/grub-*")
  done
else
  echo "==> normal mode (HARD_MODE=0): the ISO keeps its install tools"
fi
# /boot stays inside the squashfs on purpose: installing = copying the system, kernel included
mksquashfs "$ROOT" "$ISO/LiveOS/squashfs.img" -comp xz -b 1M -noappend -wildcards -e "${EXCL[@]}"

# fbcon=nodefer: show the boot tuxes even with quiet (the kernel defers console takeover)
cmdline="root=live:CDLABEL=SGNNIOS rd.live.image rd.live.overlay.overlayfs=1 init=/opt/bb/init fbcon=nodefer"
cat > "$ISO/boot/grub/grub.cfg" <<EOF
insmod all_video
if loadfont unicode ; then
  set gfxmode=auto
  insmod gfxterm
  insmod png
  terminal_output gfxterm
  background_image /boot/grub/background.png
  set menu_color_normal=white/black
  set menu_color_highlight=black/yellow
fi
set timeout=5
set default=0
menuentry "$DISTRO_NAME $VERSION" {
  linux  /boot/vmlinuz $cmdline quiet
  initrd /boot/initramfs.img
}
menuentry "$DISTRO_NAME (safe graphics)" {
  linux  /boot/vmlinuz $cmdline nomodeset
  initrd /boot/initramfs.img
}
menuentry "$DISTRO_NAME (drop to initramfs shell)" {
  linux  /boot/vmlinuz $cmdline rd.break=pre-mount
  initrd /boot/initramfs.img
}
EOF
grub-mkrescue -o "$OUT" "$ISO" -- -volid SGNNIOS
echo "==> $OUT: $(du -h "$OUT" | cut -f1)"
if xorriso -indev "$OUT" -report_el_torito plain 2>/dev/null | grep -qi uefi; then
  echo "    UEFI boot entry: present (Secure Boot must be off; images are unsigned)"
else
  echo "    WARNING: could not confirm a UEFI boot entry in $OUT"
fi
echo "test with: qemu-system-x86_64 -enable-kvm -m 2G -cdrom $OUT"
