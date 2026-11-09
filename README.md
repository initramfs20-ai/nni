# nni: YnniOS and SGnniOS

The nni family started when a toy chicken keymashed a keyboard and typed "Ynni".

| | YnniOS | SGnniOS |
|---|---|---|
| Base | Debian (debootstrap) | Gentoo (stage3, compiled from source) |
| Editions | minimal (servers, homelabs, anything on a cable), full (icewm desktop, drivers) | one, source-based |
| Package tool | `pol`, the Package Optimized Layer over apt | `spol`, the Sourced Portage-Oriented Linker |
| fastfetch logo | the chicken | the buff chicken shouting "SPOL!!" |
| Boot logo | chicken splash | chicken splash and chickens instead of tux in the kernel |

Both run busybox as PID 1, have no systemd, are deliberately hard to install,
and share the **nni core**:

- **`nni`**: every system setting in one file, `/etc/nni.conf` (`nni set TIMEZONE ...`).
- **`svc`**: a small service manager that supervises daemons and restarts them if they crash.
- **`wifi`**: Wi-Fi in one command.
- The busybox boot script, the chicken splash, the handbook, and the `Ynni` icewm theme.
- Releases with a shared version and a chicken-breed codename. 1.0.0 is "Silkie".

## Repository layout

```
src/                 the nni core: scripts, boot script, services, logos, handbooks
src/splash/          nni-splash, the C program that draws the boot splash
artwork/             wallpapers, icewm theme, generated splash/kernel-logo/GRUB pictures
packaging/debian/    builds nni-base and nni-artwork .deb packages
packaging/gentoo/    the ebuild template
app-misc/, profiles/, metadata/   the Gentoo overlay (this repo is the overlay)
docs/                GitHub Pages site, including the apt repository in docs/apt/
builders/ynnios/     YnniOS ISO builder
builders/sgnnios/    SGnniOS ISO builder
tools/release.sh     builds and publishes a release
```

## Building the ISOs

On a Debian/Ubuntu/Pop!_OS PC:

```sh
sudo apt install debootstrap squashfs-tools xorriso mtools grub-pc-bin grub-efi-amd64-bin \
                 build-essential dpkg-dev qemu-system-x86 ovmf wget
sudo bash builders/ynnios/build-iso.sh all          # ynnios-minimal.iso + ynnios-full.iso
sudo bash builders/sgnnios/build-sgnnios.sh         # sgnnios.iso
```

Useful options are set as environment variables in front of the command:

- `HARD_MODE=0` keeps the install tools on the ISO.
- `TIMEZONE=...` sets the default time zone.
- `SERVICES="syslogd ntpd"` chooses which services start at boot.
- `CHICKEN_KERNEL=0` uses the prebuilt Gentoo kernel, which is much faster to build but shows tux at boot.

## Releasing and publishing

See [PUBLISHING.md](PUBLISHING.md). It covers creating the GitHub repo for the
first time, the apt repository on GitHub Pages and the Gentoo overlay. After that,
a release is two commands:

```sh
sh tools/release.sh build
sh tools/release.sh publish
```

## License

MIT, see [LICENSE](LICENSE). The wallpapers and chicken artwork belong to the project.
