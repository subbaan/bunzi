#!/bin/sh
# appimage.sh 1.0.0: build build/Bunzi-<version>-<arch>.AppImage, one file
# holding the game, its committed assets and the libraries most systems lack
# (SDL3, libpng). Run from the project root (`make appimage`); the first run
# downloads appimagetool and the AppImage runtime into build/tools/.
#
# build/AppDir is reused between runs: if a tracked asset is removed or
# renamed, delete build/AppDir by hand so the old file doesn't ride along.
set -eu

case "${1:-}" in
-h|--help)
    echo "Usage: packaging/appimage.sh   (from the project root)"
    echo "Builds build/Bunzi-<version>-<arch>.AppImage."
    exit 0 ;;
-V|--version)
    echo "appimage.sh 1.0.0"
    exit 0 ;;
esac

[ -f src/version.h ] || { echo "appimage: run from the project root" >&2; exit 1; }

VERSION=$(sed -n 's/^#define PLAT_VERSION "\(.*\)"/\1/p' src/version.h)
ARCH=$(uname -m)
[ "$ARCH" = x86_64 ] || { echo "appimage: only x86_64 is set up (pinned tools)" >&2; exit 1; }

# pinned tools: appimagetool 1.9.1 and the type2 runtime 20251108 (static,
# needs no libfuse2 on the player's system)
TOOLS=build/tools
TOOL=$TOOLS/appimagetool-1.9.1-x86_64.AppImage
TOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
TOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME=$TOOLS/runtime-20251108-x86_64
RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

fetch() {   # fetch FILE URL SHA256
    if [ ! -f "$1" ]; then
        echo "appimage: downloading $2"
        curl -fL --retry 3 -o "$1.part" "$2"
        mv "$1.part" "$1"
    fi
    echo "$3  $1" | sha256sum -c --quiet - || {
        echo "appimage: checksum mismatch for $1 (delete it to re-download)" >&2
        exit 1
    }
}
mkdir -p "$TOOLS"
fetch "$TOOL" "$TOOL_URL" "$TOOL_SHA"
fetch "$RUNTIME" "$RUNTIME_URL" "$RUNTIME_SHA"
chmod +x "$TOOL"

# the game, built separately so it can find its libraries in ../lib
OBJDIR=build/appimage
make BUILD="$OBJDIR" RPATH='$$ORIGIN/../lib' "$OBJDIR/bunzi"

APPDIR=build/AppDir
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/bunzi" \
         "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
strip -o "$APPDIR/usr/bin/bunzi" "$OBJDIR/bunzi"

for lib in libSDL3.so.0 libpng16.so.16; do
    path=$(ldd "$OBJDIR/bunzi" | awk -v l="$lib" '$1 == l { print $3 }')
    [ -n "$path" ] || { echo "appimage: ldd can't find $lib" >&2; exit 1; }
    cp -L "$path" "$APPDIR/usr/lib/$lib"
    strip --strip-unneeded "$APPDIR/usr/lib/$lib"
done

# only the assets git tracks (the raw art and unused sound variants stay out)
git ls-files -z assets | xargs -0 cp --parents -t "$APPDIR/usr/share/bunzi"

cp packaging/bunzi.desktop "$APPDIR/bunzi.desktop"
cp packaging/bunzi.desktop "$APPDIR/usr/share/applications/bunzi.desktop"
cp packaging/bunzi.png "$APPDIR/bunzi.png"
cp packaging/bunzi.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/bunzi.png"
ln -sfn bunzi.png "$APPDIR/.DirIcon"
ln -sfn usr/bin/bunzi "$APPDIR/AppRun"

OUT=build/Bunzi-$VERSION-$ARCH.AppImage
# extract-and-run lets appimagetool work without FUSE (CI containers)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH="$ARCH" VERSION="$VERSION" \
    "$TOOL" --no-appstream --runtime-file "$RUNTIME" "$APPDIR" "$OUT"
echo "appimage: built $OUT"
