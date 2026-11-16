#!/bin/sh
# AppImage: dist/Booru_Grabber-<version>-<arch>.AppImage
# Downloads linuxdeploy and its GTK plugin into packaging/.tools on first use.
# The AppImage keeps its profile in the usual XDG directories; run it with --profile DIR (or set
# BOORU_GRABBER_PROFILE) to keep everything in one folder instead.
# Usage: packaging/build-appimage.sh <version> [arch]
set -eu
version=$1
arch=${2:-$(uname -m)}
root=$(cd "$(dirname "$0")/.." && pwd)
tools="$root/packaging/.tools"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

mkdir -p "$tools"
if [ ! -x "$tools/linuxdeploy-$arch.AppImage" ]; then
  curl -fsSL -o "$tools/linuxdeploy-$arch.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$arch.AppImage"
  chmod +x "$tools/linuxdeploy-$arch.AppImage"
fi
if [ ! -x "$tools/linuxdeploy-plugin-gtk.sh" ]; then
  curl -fsSL -o "$tools/linuxdeploy-plugin-gtk.sh" \
    "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
  chmod +x "$tools/linuxdeploy-plugin-gtk.sh"
fi

cmake -S "$root" -B "$stage/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOORU_GRABBER_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_SYSCONFDIR=/usr/etc >/dev/null
cmake --build "$stage/build"
DESTDIR="$stage/AppDir" cmake --install "$stage/build" --strip >/dev/null

mkdir -p "$root/dist"
cd "$stage"
export PATH="$tools:$PATH" DEPLOY_GTK_VERSION=3 APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$arch" LINUXDEPLOY_OUTPUT_VERSION="$version"
"$tools/linuxdeploy-$arch.AppImage" --appdir AppDir --plugin gtk --output appimage \
  --desktop-file AppDir/usr/share/applications/io.github.TheElevatedOne.BooruGrabber.desktop \
  --icon-file AppDir/usr/share/icons/hicolor/128x128/apps/booru-grabber.png \
  --executable AppDir/usr/bin/booru-grabber --executable AppDir/usr/bin/booru-grabber-cli
out=$(ls ./*.AppImage | head -n1)
mv "$out" "$root/dist/Booru_Grabber-$version-$arch.AppImage"
(cd "$root/dist" && sha256sum "Booru_Grabber-$version-$arch.AppImage" > "Booru_Grabber-$version-$arch.AppImage.sha256")
echo "$root/dist/Booru_Grabber-$version-$arch.AppImage"
