#!/bin/bash
# Build a .deb release package for termshare from a meson build.
#
# Usage: run from the repository root:
#   packaging/build-deb.sh [output-dir]
#
# The .deb is built from a clean meson install into a staging
# directory (prefix /usr), so it contains exactly what
# `meson install` would install: termshare and termshare-client,
# desktop files, icons, colorschemes, locales and the man page.
# Runtime dependencies are computed with dpkg-shlibdeps, not
# hand-listed, so they track the libraries actually linked.
#
# Requirements: meson, ninja, dpkg-deb, dpkg-shlibdeps, and the
# same -dev packages as the CI build workflow (see
# .github/workflows/build.yml). If xsltproc/docbook-xsl are missing,
# pass MESON_SETUP_ARGS="-Ddoc=false".
set -euo pipefail

if [ ! -f "meson.build" ] || [ ! -f "terminal/meson.build" ]; then
  echo "Error: run this script from the termshare repository root." >&2
  exit 1
fi

VERSION="${TERMSHARE_VERSION:-$(grep -m1 "version :" meson.build | sed -E "s/.*'([^']+)'.*/\1/")}"
ARCH="$(dpkg --print-architecture)"
BUILD_DIR="${BUILD_DIR:-build-deb}"
STAGING="$(pwd)/packaging/staging"
OUT_DIR="${1:-packaging/out}"
OUT_DEB="${OUT_DIR}/termshare_${VERSION}_${ARCH}.deb"

echo "Building termshare ${VERSION} for ${ARCH}..."

rm -rf "$BUILD_DIR" "$STAGING"
mkdir -p "$OUT_DIR"

# shellcheck disable=SC2086
meson setup "$BUILD_DIR" --prefix=/usr --buildtype=release ${MESON_SETUP_ARGS:-}
meson compile -C "$BUILD_DIR"

DESTDIR="$STAGING" meson install -C "$BUILD_DIR"

if [ ! -x "$STAGING/usr/bin/termshare" ] || [ ! -x "$STAGING/usr/bin/termshare-client" ]; then
  echo "Error: meson install did not produce both binaries in $STAGING/usr/bin." >&2
  exit 1
fi

# Permissions: dpkg-deb rejects a DEBIAN directory wider than 0775,
# and a checkout under a shared/setgid directory can leave payload
# files at 0660. Normalise first, then fix the binaries back up.
find "$STAGING" -type d -exec chmod 0755 {} +
find "$STAGING" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGING/usr/bin/termshare" "$STAGING/usr/bin/termshare-client"

# Runtime dependencies from the linked libraries.
if DEPS="$(dpkg-shlibdeps -O "$STAGING/usr/bin/termshare" "$STAGING/usr/bin/termshare-client" 2>/dev/null | sed 's/^shlibs:Depends=//')"; then
  :
else
  echo "Warning: dpkg-shlibdeps failed; falling back to a static dependency list." >&2
  DEPS="libc6, libglib2.0-0, libgtk-3-0, libpcre2-8-0, libvte-2.91-0, libxfce4ui-2-0, libxfconf-0-3"
fi

mkdir -p "$STAGING/DEBIAN"
cat > "$STAGING/DEBIAN/control" <<EOF
Package: termshare
Version: ${VERSION}
Section: x11
Priority: optional
Architecture: ${ARCH}
Maintainer: James Ding <jamesd-comelang@users.noreply.github.com>
Depends: ${DEPS}
Description: Termshare terminal emulator with remote session sharing
 A terminal emulator forked from Xfce Terminal, with remote session
 sharing: a running termshare window can be viewed and driven from
 another ordinary terminal emulator via termshare-client, over a
 private Unix socket, with no shell restarted and no PTY touched.
EOF
chmod 0755 "$STAGING/DEBIAN"
chmod 0644 "$STAGING/DEBIAN/control"

echo "Building ${OUT_DEB}..."
dpkg-deb --build "$STAGING" "$OUT_DEB"
rm -rf "$STAGING"

echo "Package created: ${OUT_DEB}"
dpkg-deb --info "$OUT_DEB"
dpkg-deb --contents "$OUT_DEB"
