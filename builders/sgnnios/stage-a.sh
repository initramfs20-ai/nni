#!/bin/bash
# Runs INSIDE the Gentoo chroot. Started by build-sgnnios.sh - not meant to be run by hand.
set -euo pipefail
set +u; source /etc/profile; set -u
. /etc/sgnnios-build.env

echo "==> Portage tree"
if [ ! -d /var/db/repos/gentoo/profiles ]; then
  if [ ! -e /etc/portage/repos.conf/gentoo.conf ] && [ -f /usr/share/portage/config/repos.conf ]; then
    mkdir -p /etc/portage/repos.conf
    cp /usr/share/portage/config/repos.conf /etc/portage/repos.conf/gentoo.conf
  fi
  emerge-webrsync
fi

echo "==> package settings"
mkdir -p /etc/portage/package.use /etc/portage/package.accept_keywords
cat > /etc/portage/package.use/sgnnios <<'EOF'
sys-apps/busybox static -pam
# a static busybox needs static versions of the crypt libraries
virtual/libcrypt static-libs
sys-libs/libxcrypt static-libs
# dracut is run by hand below, with the live-boot modules
sys-kernel/gentoo-kernel-bin -initramfs
sys-kernel/gentoo-kernel -initramfs
# dracut's live-boot module (dmsquash-live) needs device-mapper's dmsetup;
# -lvm builds only device-mapper, not the full LVM tool set
sys-fs/lvm2 -lvm udev
EOF
cat > /etc/portage/package.accept_keywords/sgnnios <<'EOF'
app-misc/fastfetch ~amd64
app-misc/nni-base ~amd64
EOF

# chickens instead of penguins: swap the kernel's boot logo before it is compiled
mkdir -p /etc/portage/env/sys-kernel
cat > /etc/portage/env/sys-kernel/gentoo-kernel <<'EOF'
# nni: chicken boot logo (installed by the SGnniOS builder; the picture comes from nni-base)
post_src_prepare() {
	local logo=/usr/share/nni/kernel/logo_linux_clut224.ppm
	if [[ -f ${logo} && -d ${S}/drivers/video/logo ]]; then
		cp "${logo}" "${S}/drivers/video/logo/logo_linux_clut224.ppm" &&
			einfo "nni: chicken boot logo in place"
	else
		ewarn "nni: chicken logo not found, keeping tux"
	fi
}
EOF

if [ "$UPDATE_WORLD" = 1 ]; then
  echo "==> updating @world"
  emerge --verbose --update --deep --newuse @world
fi

echo "==> time zone and name (nni reads these when it is installed)"
ln -sf "/usr/share/zoneinfo/$TIMEZONE" /etc/localtime
echo "$TIMEZONE" > /etc/timezone
echo SGnniOS > /etc/hostname
echo '# SGnniOS: add your mounts here (the live system needs none)' > /etc/fstab

echo "==> the nni core (from the nni overlay)"
# first, because the kernel build below takes its chicken logo from it
emerge --verbose --update app-misc/nni-base
nni setup --distro sgnnios --edition base
for s in $SERVICES; do mkdir -p /etc/nni/enabled; touch "/etc/nni/enabled/$s"; done

echo "==> emerging the SGnniOS base"
# gentoo-kernel and gentoo-kernel-bin install the same files: remove the one we are not using
for k in sys-kernel/gentoo-kernel sys-kernel/gentoo-kernel-bin; do
  if [ "$k" != "$KERNEL_PKG" ] && portageq has_version / "$k" 2>/dev/null; then
    echo "    switching kernels: removing $k"
    emerge --unmerge "$k"
  fi
done
[ "$CHICKEN_KERNEL" = 1 ] && echo "    (compiling the kernel with the chicken logo - this is the long part)"
PKGS=(sys-apps/busybox "$KERNEL_PKG" sys-kernel/dracut sys-fs/lvm2
      net-wireless/wpa_supplicant net-wireless/iw app-misc/fastfetch dev-vcs/git)
[ "$FIRMWARE" = 1 ] && PKGS+=(sys-kernel/linux-firmware)
emerge --verbose --noreplace "${PKGS[@]}"

echo "==> locale"
printf 'en_US.UTF-8 UTF-8\nC.UTF-8 UTF-8\n' > /etc/locale.gen
locale-gen
eselect locale set en_US.utf8 || true
env-update

echo "==> busybox as PID 1"
mkdir -p /opt/bb/bin
ln -sf /bin/busybox /opt/bb/init
/bin/busybox --install -s /opt/bb/bin
# make busybox the init even if the init= parameter is ever lost
rm -f /sbin/init /sbin/halt /sbin/poweroff /sbin/reboot /sbin/shutdown /sbin/telinit /sbin/runlevel
ln -s /bin/busybox /sbin/init

cat > /etc/inittab <<'EOF'
::sysinit:/usr/lib/nni/rc.sysinit
tty1::respawn:-/bin/sh
tty2::respawn:-/bin/sh
::ctrlaltdel:/bin/busybox reboot
::shutdown:/usr/bin/svc shutdown
::shutdown:/bin/busybox umount -a -r
EOF
# older builds kept their own copies here; the packaged ones replace them
rm -f /etc/rc.sysinit

echo "==> dracut live initramfs"
KVER=$(ls /lib/modules | sort -V | tail -1)
mkdir -p /etc/dracut.conf.d
cat > /etc/dracut.conf.d/sgnnios.conf <<'EOF'
hostonly="no"
add_dracutmodules+=" dmsquash-live "
add_drivers+=" squashfs loop overlay iso9660 sr_mod sd_mod usb_storage uas "
EOF
command -v dmsetup >/dev/null || { echo 'ERROR: dmsetup is missing (sys-fs/lvm2 did not install it); dracut live boot needs it'; exit 1; }
dracut --force --kver "$KVER" "/boot/initramfs-$KVER.img"
ls -l /boot

# Hard mode is applied by build-sgnnios.sh when it makes the squashfs (it leaves the
# mkfs tools out of the ISO). The work folder keeps them, so switching HARD_MODE
# between builds never needs a fresh build.

echo "==> stage A done"
