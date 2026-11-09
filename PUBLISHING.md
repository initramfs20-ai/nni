# Publishing nni on GitHub (first time)

This puts three things online, all from this one repository:

- **the source code**, at `https://github.com/<you>/nni`
- **an apt repository for YnniOS**, served by GitHub Pages at `https://<you>.github.io/nni/apt/`
- **a Gentoo overlay for SGnniOS**. The repository itself is the overlay.

Do everything on your build PC (Pop!_OS), as your normal user (not `sudo`), from
inside the `nni` folder.

## 1. Install the tools

```sh
sudo apt install git gh build-essential dpkg-dev gnupg
```

`gh` is GitHub's command-line tool. It creates the repository and the release for you.

## 2. Tell git who you are

GitHub can hide your real email address. On github.com, go to **Settings → Emails**,
tick **Keep my email addresses private**, and copy the address that looks like
`12345678+yourname@users.noreply.github.com`. Then:

```sh
git config --global user.name  "your GitHub username"
git config --global user.email "12345678+yourname@users.noreply.github.com"
```

## 3. Log in to GitHub from the terminal

```sh
gh auth login
```

Pick **GitHub.com**, then **HTTPS**, then **Login with a web browser**. It shows a
code; paste it into the page that opens. When asked whether to authenticate Git with
your GitHub credentials, answer yes.

## 4. Create the repository and push

```sh
cd nni
git init -b main
git add -A
git commit -m "nni 1.0.0 Silkie: first commit"
gh repo create nni --public --source=. --remote=origin --push
```

The repo must be **public**, because GitHub Pages is only free for public repos.
Any name works instead of `nni`; the tools read it from the `origin` remote.

## 5. Build the release

```sh
sh tools/release.sh build
```

This builds the `.deb` packages, the source tarball, the Gentoo ebuild and its
Manifest, and the signed apt repository in `docs/apt/`.

**The first run creates a signing key** called "nni apt repository". YnniOS uses it to
check that packages really come from you. Back it up somewhere safe and private.
Without it, you can't sign future updates with the same key.

```sh
gpg --export-secret-keys --armor "nni apt repository" > ~/nni-apt-key.asc
```

To restore it on another PC, run `gpg --import nni-apt-key.asc`.

## 6. Publish

```sh
sh tools/release.sh publish
```

This commits and pushes, then creates the tag `v1.0.0` and a GitHub **Release** with
the source tarball and the `.deb` files. SGnniOS downloads the tarball from that release.

## 7. Switch on GitHub Pages (once)

On github.com, open your repo and go to **Settings → Pages**:

- **Source:** Deploy from a branch
- **Branch:** `main`, folder **`/docs`**, then **Save**

After a minute or two, `https://<you>.github.io/nni/` shows the project page, and
`https://<you>.github.io/nni/apt/` is the apt repository.

## 8. Build ISOs that know about the repo

Rebuild the ISOs so they ship with the repository already configured:

```sh
sudo bash builders/ynnios/build-iso.sh all
sudo bash builders/sgnnios/build-sgnnios.sh
```

On those systems, `nni update` upgrades the nni core straight from GitHub.

## 9. Put the ISOs up for download

Not everyone has a build PC set up, so publish the ISOs themselves. After the ISOs
are built, and after `publish` has created the release:

```sh
sh tools/release.sh isos
```

This picks up the YnniOS minimal, YnniOS full and SGnniOS ISOs from the project folder
(any name that starts with `ynnios` and contains `minimal` or `full`, or starts with
`sgnnios`). It uploads them to the GitHub release as
`ynnios-VERSION-minimal-amd64.iso` and so on, together with a `SHA256SUMS` file
signed with the same key as the apt repo. It also updates `docs/isos.json`, which
fills the **Download** section of `https://<you>.github.io/nni/`.

- GitHub release files must be smaller than 2 GiB. An ISO that size or bigger
  (SGnniOS can be) goes up in parts, and the website shows how to join them:
  `cat sgnnios-*.iso.part* > sgnnios.iso`.
- Uploading several GB takes a while on a home connection. Leave it running.
- GitHub serves the files for free: the release is your download server.

## Every release after this

1. Make your changes.
2. Raise the number in `VERSION`. Change `CODENAME` too if you want a new breed.
3. Run `sh tools/release.sh build`.
4. Run `sh tools/release.sh publish`.
5. Rebuild the ISOs, then run `sh tools/release.sh isos`.

Machines pick the update up with `nni update`, `pol upgrade` or `spol upgrade`.

---

# Moving machines you already installed onto the nni core

New ISOs need none of this. These steps are for the YnniOS and SGnniOS installs you
already have. Do them after step 7, so the repository is online.

## YnniOS

```sh
pol update && pol install ca-certificates
wget -O /usr/share/keyrings/nni-archive-keyring.gpg https://<you>.github.io/nni/apt/nni-archive-keyring.gpg
wget -O /etc/apt/sources.list.d/nni.sources https://<you>.github.io/nni/apt/nni.sources
pol update
pol install nni-base            # full edition: pol install nni-base nni-artwork
```

The install moves the old copies of `wifi`, `pol` and `handbook` from `/usr/local/bin` to
`/var/backups/nni-old/`, and creates `/etc/nni.conf` from your current settings. Then
switch the boot over to the packaged boot script:

```sh
sed -i 's|^::sysinit:.*|::sysinit:/usr/lib/nni/rc.sysinit|' /etc/inittab
grep -q 'svc shutdown' /etc/inittab || sed -i 's|^::shutdown:|::shutdown:/usr/bin/svc shutdown\n::shutdown:|' /etc/inittab
grep -q profile.d /etc/profile || echo 'for f in /etc/profile.d/*.sh; do [ -r "$f" ] && . "$f"; done' >> /etc/profile
sed -i '/Welcome to YnniOS/d' /etc/profile        # nni prints its own welcome now
svc enable syslogd
reboot
```

On the full edition, also put `nni apply --x` as the first line of `~/.xinitrc`.

## SGnniOS

```sh
spol install dev-vcs/git
mkdir -p /etc/portage/repos.conf
printf '[nni]\nlocation = /var/db/repos/nni\nsync-type = git\nsync-uri = https://github.com/<you>/nni.git\nsync-depth = 1\n' \
  > /etc/portage/repos.conf/nni.conf
echo 'app-misc/nni-base ~amd64' >> /etc/portage/package.accept_keywords/sgnnios
emerge --sync nni
spol install app-misc/nni-base
sed -i 's|^::sysinit:.*|::sysinit:/usr/lib/nni/rc.sysinit|' /etc/inittab
svc enable syslogd
reboot
```

The chicken kernel logo arrives with the next kernel you compile. The handbook
(`handbook`, section "Boot logo") explains how to set that up.
