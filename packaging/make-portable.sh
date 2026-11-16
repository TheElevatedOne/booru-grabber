#!/bin/sh
# Standalone directory: dist/booru-grabber-<version>-portable-<arch>.tar.gz
# Extract it anywhere and run ./booru-grabber; settings and downloads data stay in ./profile.
# GTK 3, libsoup 3, libxml2, sqlite and gdk-pixbuf come from the system; quickjs-ng is bundled.
# Usage: packaging/make-portable.sh <version> [arch]
set -eu
version=$1
arch=${2:-$(uname -m)}
root=$(cd "$(dirname "$0")/.." && pwd)
name="booru-grabber-$version-portable-$arch"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

cmake -S "$root" -B "$stage/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOORU_GRABBER_PORTABLE=ON \
  -DBOORU_GRABBER_TESTS=OFF -DCMAKE_INSTALL_PREFIX="/$name" >/dev/null
cmake --build "$stage/build"
DESTDIR="$stage/out" cmake --install "$stage/build" --strip >/dev/null
dir="$stage/out/$name"
test -x "$dir/booru-grabber" && test -f "$dir/portable"
# Bundle quickjs-ng when it is a shared library (it is rarely packaged)
qjs=$(ldd "$dir/booru-grabber" | awk '/libqjs/ { print $3 }')
if [ -n "$qjs" ]; then
  cp -L "$qjs" "$dir/lib/"
fi
cp "$root/README.md" "$root/CHANGELOG.md" "$root/LICENSE" "$root/NOTICE" "$dir/"
mkdir -p "$root/dist"
tar --owner=0 --group=0 -C "$stage/out" -czf "$root/dist/$name.tar.gz" "$name"
(cd "$root/dist" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "$root/dist/$name.tar.gz"
