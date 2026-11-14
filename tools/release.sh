#!/bin/sh
# nni release tool. Run it from anywhere inside the repo, on your build PC.
#
#   sh tools/release.sh build     make everything for the current VERSION:
#                                   dist/nni-VERSION.tar.gz       (source, for SGnniOS)
#                                   dist/niwm-NIWMVERSION.tar.gz  (NiWM source, for SGnniOS)
#                                   dist/*.deb                    (YnniOS packages)
#                                   docs/apt/                     (apt repo for GitHub Pages)
#                                   app-misc/nni-base/            (Gentoo ebuild + Manifest)
#                                   x11-wm/niwm/                  (Gentoo ebuild + Manifest)
#   sh tools/release.sh source    only the source tarball + Gentoo ebuild (no signing)
#   sh tools/release.sh publish   commit, tag vVERSION, push, and create the GitHub
#                                 release with the source tarball (needs: gh auth login)
#   sh tools/release.sh isos      upload the ISOs you built to that release, with a
#                                 signed SHA256SUMS, and update the download page
#
# To make a new version: change VERSION (and CODENAME if you like), then build + publish.
# Needs on the build PC: git, make, gcc, dpkg-deb, dpkg-scanpackages, gpg, b2sum,
# pkg-config and the xcb headers (for NiWM), and gh for publish:
#   sudo apt install git build-essential dpkg-dev gnupg gh pkg-config \
#                    libxcb1-dev libxcb-randr0-dev libxcb-res0-dev
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP"
VERSION=$(cat VERSION)
CODENAME=$(cat CODENAME)
# NiWM keeps its own version number
NIWM_VERSION=$(sed -n 's/^#define NIWM_VERSION "\(.*\)"/\1/p' niwm/core/niwm.h)
DIST=$TOP/dist
KEYNAME="nni apt repository"

die() { echo "release: $*" >&2; exit 1; }

# ---------------------------------------------------------------- where it lives on GitHub
repo_url() {
  if [ -n "${NNI_REPO_URL:-}" ]; then echo "$NNI_REPO_URL"; return; fi
  u=$(git -c safe.directory="$TOP" remote get-url origin 2>/dev/null || true)
  case "$u" in
    *github.com[:/]*)
      p=${u#*github.com[:/]}; p=${p%.git}
      echo "https://github.com/$p" ;;
    *) echo "" ;;
  esac
}
REPO_URL=$(repo_url)
if [ -z "$REPO_URL" ]; then
  REPO_URL="https://github.com/YOUR-GITHUB-USER/nni"
  echo "release: no GitHub remote yet - using $REPO_URL as a placeholder" >&2
  echo "         (after 'git remote add origin ...', run build again)" >&2
fi
OWNER=$(echo "$REPO_URL" | cut -d/ -f4)
NAME=$(echo "$REPO_URL" | cut -d/ -f5)
PAGES_URL="https://$(echo "$OWNER" | tr 'A-Z' 'a-z').github.io/$NAME"

# ---------------------------------------------------------------- build
source_tarball() {
  out="$DIST/nni-$VERSION.tar.gz"
  tmp=$(mktemp -d)
  mkdir -p "$tmp/nni-$VERSION"
  cp -a VERSION CODENAME LICENSE README.md Makefile src "$tmp/nni-$VERSION/"
  mkdir -p "$tmp/nni-$VERSION/artwork"
  cp -a artwork/generated "$tmp/nni-$VERSION/artwork/"
  # same input always gives the same bytes, so the Gentoo checksum stays valid
  epoch=$(git -c safe.directory="$TOP" log -1 --format=%ct 2>/dev/null || echo 1700000000)
  ( cd "$tmp" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
      -cf - "nni-$VERSION" ) | gzip -n -9 > "$out"
  rm -rf "$tmp"
  echo "built: $out"
}

niwm_tarball() {
  out="$DIST/niwm-$NIWM_VERSION.tar.gz"
  tmp=$(mktemp -d)
  make -C niwm clean >/dev/null
  cp -a niwm "$tmp/niwm-$NIWM_VERSION"
  cp LICENSE "$tmp/niwm-$NIWM_VERSION/"
  epoch=$(git -c safe.directory="$TOP" log -1 --format=%ct 2>/dev/null || echo 1700000000)
  ( cd "$tmp" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
      -cf - "niwm-$NIWM_VERSION" ) | gzip -n -9 > "$out"
  rm -rf "$tmp"
  echo "built: $out"
}

# an ebuild + Manifest in the overlay: overlay_pkg CATEGORY/NAME VERSION TEMPLATE METADATA TARBALL
overlay_pkg() {
  d="$TOP/$1"; name=${1#*/}
  mkdir -p "$d"
  rm -f "$d/$name"-*.ebuild
  sed -e "s|@REPO_URL@|$REPO_URL|g" -e "s|@NNI_VERSION@|$VERSION|g" "$3" > "$d/$name-$2.ebuild"
  cp "$4" "$d/metadata.xml"
  f=$5
  echo "DIST $(basename "$f") $(wc -c < "$f" | tr -d ' ') BLAKE2B $(b2sum "$f" | cut -d' ' -f1) SHA512 $(sha512sum "$f" | cut -d' ' -f1)" > "$d/Manifest"
  echo "built: $1/$name-$2.ebuild (+ Manifest)"
}

gentoo_overlay() {
  d="$TOP/app-misc/nni-base"
  mkdir -p "$d"
  rm -f "$d"/nni-base-*.ebuild
  sed "s|@REPO_URL@|$REPO_URL|g" packaging/gentoo/nni-base.ebuild.in > "$d/nni-base-$VERSION.ebuild"
  cp packaging/gentoo/metadata.xml "$d/metadata.xml"
  f="$DIST/nni-$VERSION.tar.gz"
  size=$(wc -c < "$f" | tr -d ' ')
  b2=$(b2sum "$f" | cut -d' ' -f1)
  sha=$(sha512sum "$f" | cut -d' ' -f1)
  echo "DIST nni-$VERSION.tar.gz $size BLAKE2B $b2 SHA512 $sha" > "$d/Manifest"
  echo "built: app-misc/nni-base/nni-base-$VERSION.ebuild (+ Manifest)"
  overlay_pkg x11-wm/niwm "$NIWM_VERSION" packaging/gentoo/niwm.ebuild.in \
              packaging/gentoo/niwm-metadata.xml "$DIST/niwm-$NIWM_VERSION.tar.gz"
}

signing_key() {
  fpr=$(gpg --batch --with-colons --list-secret-keys "$KEYNAME" 2>/dev/null | awk -F: '/^fpr:/ {print $10; exit}')
  if [ -z "$fpr" ]; then
    echo "release: creating a signing key for the apt repo ('$KEYNAME', no passphrase)" >&2
    gpg --batch --passphrase '' --quick-gen-key "$KEYNAME" ed25519 sign never >/dev/null 2>&1 ||
      die "gpg could not create a key"
    fpr=$(gpg --batch --with-colons --list-secret-keys "$KEYNAME" | awk -F: '/^fpr:/ {print $10; exit}')
    echo "release: BACK UP this key, or future updates cannot be signed with it:" >&2
    echo "         gpg --export-secret-keys --armor '$KEYNAME' > nni-apt-key.asc   (keep it private)" >&2
  fi
  echo "$fpr"
}

apt_repo() {
  a="$TOP/docs/apt"
  mkdir -p "$a/pool"
  rm -f "$a"/pool/*.deb
  cp "$DIST"/nni-base_"$VERSION"_amd64.deb "$DIST"/nni-artwork_"$VERSION"_all.deb \
     "$DIST"/niwm_"$NIWM_VERSION"_*.deb "$a/pool/"
  ( cd "$a" && dpkg-scanpackages --multiversion pool /dev/null 2>/dev/null > Packages && gzip -n -9 -k -f Packages )
  {
    echo "Origin: nni"
    echo "Label: nni"
    echo "Suite: stable"
    echo "Codename: $(echo "$CODENAME" | tr 'A-Z' 'a-z')"
    echo "Version: $VERSION"
    echo "Date: $(LC_ALL=C date -Ru)"
    echo "Architectures: amd64 all"
    echo "Description: nni family packages for YnniOS"
    echo "MD5Sum:"
    for f in Packages Packages.gz; do echo " $(md5sum "$a/$f" | cut -d' ' -f1) $(wc -c < "$a/$f" | tr -d ' ') $f"; done
    echo "SHA256:"
    for f in Packages Packages.gz; do echo " $(sha256sum "$a/$f" | cut -d' ' -f1) $(wc -c < "$a/$f" | tr -d ' ') $f"; done
  } > "$a/Release"
  fpr=$(signing_key)
  rm -f "$a/InRelease" "$a/Release.gpg"
  gpg --batch --yes --local-user "$fpr" --clearsign -o "$a/InRelease" "$a/Release"
  gpg --batch --yes --local-user "$fpr" --armor --detach-sign -o "$a/Release.gpg" "$a/Release"
  gpg --batch --yes --export "$fpr" > "$a/nni-archive-keyring.gpg"
  cat > "$a/nni.sources" <<EOF
Types: deb
URIs: $PAGES_URL/apt/
Suites: ./
Signed-By: /usr/share/keyrings/nni-archive-keyring.gpg
EOF
  touch "$TOP/docs/.nojekyll"
  echo "built: docs/apt (signed with key $fpr)"
}

build() {
  for t in make dpkg-deb dpkg-scanpackages gpg b2sum sha512sum tar gzip; do
    command -v "$t" >/dev/null || die "missing $t (sudo apt install build-essential dpkg-dev gnupg)"
  done
  [ -n "$NIWM_VERSION" ] || die "can't read NIWM_VERSION from niwm/core/niwm.h"
  mkdir -p "$DIST"
  rm -f "$DIST"/*.deb "$DIST"/nni-*.tar.gz "$DIST"/niwm-*.tar.gz
  sh packaging/debian/build-deb.sh "$DIST" "$REPO_URL"
  source_tarball
  niwm_tarball
  gentoo_overlay
  apt_repo
  echo
  echo "nni $VERSION \"$CODENAME\" is built. Repo: $REPO_URL"
  echo "Next: sh tools/release.sh publish"
}

publish() {
  command -v gh >/dev/null || die "install the GitHub CLI first: sudo apt install gh, then: gh auth login"
  gh auth status >/dev/null 2>&1 || die "log in to GitHub first: gh auth login"
  case "$REPO_URL" in *YOUR-GITHUB-USER*) die "no GitHub remote: git remote add origin https://github.com/<you>/nni.git" ;; esac
  [ -f "$DIST/nni-$VERSION.tar.gz" ] || die "run 'sh tools/release.sh build' first"
  [ -f "$DIST/niwm-$NIWM_VERSION.tar.gz" ] || die "run 'sh tools/release.sh build' first (no NiWM tarball)"
  git add -A
  git commit -m "nni $VERSION \"$CODENAME\"" || echo "release: nothing new to commit"
  git tag -f "v$VERSION"
  git push origin HEAD
  git push -f origin "v$VERSION"
  if gh release view "v$VERSION" >/dev/null 2>&1; then
    gh release upload "v$VERSION" "$DIST/nni-$VERSION.tar.gz" "$DIST/niwm-$NIWM_VERSION.tar.gz" \
      "$DIST"/*.deb --clobber
  else
    gh release create "v$VERSION" "$DIST/nni-$VERSION.tar.gz" "$DIST/niwm-$NIWM_VERSION.tar.gz" \
      "$DIST"/*.deb \
      --title "nni $VERSION \"$CODENAME\"" --notes "nni family release $VERSION ($CODENAME)."
  fi
  echo
  echo "Published. apt repo (once GitHub Pages is on): $PAGES_URL/apt/"
  echo "Gentoo overlay: $REPO_URL.git"
}

# ---------------------------------------------------------------- ISOs
# Puts the ISOs (ynnios*minimal*.iso, ynnios*full*.iso, sgnnios*.iso in the project
# folder) on the GitHub release for VERSION, with a signed SHA256SUMS, and
# writes docs/isos.json, which the download section of the website reads.
# GitHub release files must be smaller than 2 GiB, so bigger ISOs go up in parts.
ISO_DIR=${NNI_ISO_DIR:-$TOP}             # where the builders left the ISOs
LIMIT=${NNI_ISO_LIMIT:-2000000000}       # bytes; ISOs this big or bigger are split
PART_SIZE=${NNI_PART_SIZE:-1900M}

json_iso() { # file title about size sha parts...
  f=$1; t=$2; a=$3; z=$4; h=$5; shift 5
  printf '    {"file": "%s", "title": "%s", "about": "%s", "size": %s, "sha256": "%s", "parts": [' "$f" "$t" "$a" "$z" "$h"
  sep=
  for p in "$@"; do printf '%s"%s"' "$sep" "$p"; sep=', '; done
  printf ']}'
}

isos() {
  case "$REPO_URL" in *YOUR-GITHUB-USER*) die "no GitHub remote yet (see PUBLISHING.md)" ;; esac
  if [ "${NNI_NO_UPLOAD:-0}" != 1 ]; then
    command -v gh >/dev/null && gh auth status >/dev/null 2>&1 || die "log in to GitHub first: gh auth login"
    gh release view "v$VERSION" >/dev/null 2>&1 || die "release v$VERSION does not exist yet: run 'sh tools/release.sh publish' first"
  fi
  out="$DIST/isos"
  rm -rf "$out"; mkdir -p "$out"
  entries=; upload=
  for spec in \
    "ynnios*minimal*.iso|ynnios-$VERSION-minimal-amd64.iso|YnniOS minimal|Console only. For servers, homelabs, routers and anything with a network cable." \
    "ynnios*full*.iso|ynnios-$VERSION-full-amd64.iso|YnniOS full|The NiWM desktop with drivers and Wi-Fi firmware. The everyday edition." \
    "sgnnios*.iso|sgnnios-$VERSION-amd64.iso|SGnniOS|Source-Genius: Gentoo underneath, compiled on your machine, chickens at boot."
  do
    pattern=$(echo "$spec" | cut -d'|' -f1); name=$(echo "$spec" | cut -d'|' -f2)
    title=$(echo "$spec" | cut -d'|' -f3); about=$(echo "$spec" | cut -d'|' -f4)
    # any name works: ynnios-minimal.iso, ynnios-minimal-x86_64.iso, ... (newest wins)
    # shellcheck disable=SC2086
    src=$(ls -t "$ISO_DIR"/$pattern 2>/dev/null | head -n 1)
    [ -n "$src" ] && [ -f "$src" ] || { echo "release: no $pattern in $ISO_DIR - skipped"; continue; }
    echo "release: $(basename "$src") -> $name"
    ln -f "$src" "$out/$name" 2>/dev/null || cp "$src" "$out/$name"
    size=$(wc -c < "$out/$name" | tr -d ' ')
    echo "release: checksumming $name ($size bytes)"
    sha=$(sha256sum "$out/$name" | cut -d' ' -f1)
    echo "$sha  $name" >> "$out/SHA256SUMS"
    if [ "$size" -ge "$LIMIT" ]; then
      echo "release: $name is too big for one GitHub file - splitting it into parts"
      ( cd "$out" && split -b "$PART_SIZE" -d -a 2 "$name" "$name.part" && rm -f "$name" )
      parts=$(cd "$out" && ls "$name".part* | tr '\n' ' ')
      for p in $parts; do
        echo "$(sha256sum "$out/$p" | cut -d' ' -f1)  $p" >> "$out/SHA256SUMS"
        upload="$upload $out/$p"
      done
    else
      parts=
      upload="$upload $out/$name"
    fi
    # shellcheck disable=SC2086
    e=$(json_iso "$name" "$title" "$about" "$size" "$sha" $parts)
    entries="${entries:+$entries,
}$e"
  done
  [ -n "$entries" ] || die "no ISOs found in $ISO_DIR (build them first)"

  fpr=$(signing_key)
  gpg --batch --yes --local-user "$fpr" --armor --detach-sign -o "$out/SHA256SUMS.asc" "$out/SHA256SUMS"
  {
    echo "{"
    echo "  \"version\": \"$VERSION\","
    echo "  \"codename\": \"$CODENAME\","
    echo "  \"release\": \"$REPO_URL/releases/tag/v$VERSION\","
    echo "  \"base\": \"$REPO_URL/releases/download/v$VERSION/\","
    echo "  \"sums\": \"SHA256SUMS\","
    echo "  \"signature\": \"SHA256SUMS.asc\","
    echo "  \"isos\": ["
    printf '%s\n' "$entries"
    echo "  ]"
    echo "}"
  } > "$TOP/docs/isos.json"
  echo "built: docs/isos.json"

  if [ "${NNI_NO_UPLOAD:-0}" = 1 ]; then echo "release: NNI_NO_UPLOAD=1 - not uploading"; return 0; fi
  echo "release: uploading to GitHub (big files: this takes a while, depending on your upload speed)"
  # shellcheck disable=SC2086
  gh release upload "v$VERSION" $upload "$out/SHA256SUMS" "$out/SHA256SUMS.asc" --clobber
  git add docs/isos.json
  git commit -m "ISOs for nni $VERSION" || true
  git push origin HEAD
  rm -rf "$out"
  echo
  echo "ISOs published. Download page: $PAGES_URL/#download"
}

case "${1:-}" in
  source)  # just the source tarball + Gentoo ebuild (used by the SGnniOS builder)
           mkdir -p "$DIST"; rm -f "$DIST"/nni-*.tar.gz "$DIST"/niwm-*.tar.gz
           source_tarball; niwm_tarball; gentoo_overlay ;;
  build)   build ;;
  publish) publish ;;
  isos)    isos ;;
  *)       echo "usage: sh tools/release.sh build|source|publish|isos" >&2; exit 1 ;;
esac
