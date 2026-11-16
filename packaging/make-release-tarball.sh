#!/bin/sh
# Release tarball laid out for /usr: dist/booru-grabber-<version>-linux-<arch>.tar.gz
# Usage: packaging/make-release-tarball.sh <build-dir> <version> [arch]
# The build directory must be configured with -DCMAKE_INSTALL_PREFIX=/usr and built.
set -eu
build=$1
version=$2
arch=${3:-$(uname -m)}
root=$(cd "$(dirname "$0")/.." && pwd)
name="booru-grabber-$version-linux-$arch"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

DESTDIR="$stage/install" cmake --install "$build" --strip >/dev/null
test -x "$stage/install/usr/bin/booru-grabber"
mkdir -p "$stage/$name"
cp -a "$stage/install/usr/." "$stage/$name/"
# /etc/booru-grabber/settings.ini (system defaults)
cp -a "$stage/install/etc" "$stage/$name/"
cp "$root/README.md" "$root/CHANGELOG.md" "$root/LICENSE" "$root/NOTICE" "$stage/$name/"
mkdir -p "$root/dist"
tar --owner=0 --group=0 -C "$stage" -czf "$root/dist/$name.tar.gz" "$name"
(cd "$root/dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "$root/dist/$name.tar.gz"
