#!/bin/bash
# Build the rom128 branch of mGBA celio edition for Windows (64-bit) with MSYS2 mingw64.
#
#   bash tools/rom128/build_win.sh
#
# All changes of this branch (128MiB ROM, 16MB save, netlink, overclock, ...) are already commits here,
# so nothing is patched: this script only runs cmake + ninja and gathers the exe, DLLs and Qt plugins into dist.
#
# Environment variables (all optional):
#   MSYS2_ROOT     MSYS2 install folder as seen from Git Bash, default /c/msys64 (e.g. /d/msys64)
#   BUILD_DIR      cmake build folder inside the source tree, default build
#   DIST           output folder, default <source>/dist   (the exe, DLLs and Qt plugins go here)
#   FFMPEG         ON enables video recording (needs mingw-w64-x86_64-ffmpeg; FFmpeg is GPL, see README.md), default OFF
#   CELIO_VERSION  version string shown in the window title (e.g. rom128-5). It is written into version.cmake
#                  (the -P script runs of version.cmake do not see -D options) and the tree is left modified.
#   JOBS           parallel jobs, default 8
#   LTO            ON turns link time optimization on, default OFF (with LTO the Lua bindings break on this branch)
#
# Packages used (MSYS2 mingw64; versions of the reference build in parentheses):
#   mingw-w64-x86_64-gcc (15.2.0) cmake (4.2.3) ninja (1.13.2) pkgconf (2.5.1)
#   qt5-base qt5-multimedia qt5-tools (5.15.18)  SDL2 (2.32.10)  lua (5.4.8)  libzip (1.11.4)
#   libepoxy (1.5.10)  json-c (0.18)  brotli (1.2.1)   and, for FFMPEG=ON: ffmpeg (8.0.1)
# Install:
#   pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,qt5-base,qt5-multimedia,qt5-tools,SDL2,lua,libzip,libepoxy,json-c,brotli}
#
# Notes:
#   - Use a source path made of ASCII characters only (Qt's moc fails in paths with Japanese characters).
#   - The reported git commit is fixed to the celio edition 2.0.0 commit (MGBA_GIT_COMMIT), so that
#     Celio-mGBA-Link, which checks the commit, keeps working. Change COMMIT below if you do not need that.
#   - Settings are kept next to the exe (an empty portable.ini is created), not in %APPDATA%.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
BUILD_DIR=${BUILD_DIR:-build}
DIST=${DIST:-$SRC/dist}
FFMPEG=${FFMPEG:-OFF}
LTO=${LTO:-OFF}
JOBS=${JOBS:-8}
CELIO_VERSION=${CELIO_VERSION:-}
COMMIT=3da13060a586f2da8eb4ecbb167f642e2e4889c2   # celio edition 2.0.0 (upstream)
case "$FFMPEG" in ON|OFF) ;; *) echo "FFMPEG must be ON or OFF: $FFMPEG" >&2; exit 1;; esac
case "$LTO" in ON|OFF) ;; *) echo "LTO must be ON or OFF: $LTO" >&2; exit 1;; esac

if [ -z "$MSYS2_ROOT" ]; then MSYS2_ROOT=/c/msys64; fi
MSYS="$MSYS2_ROOT/usr/bin"
[ -x "$MSYS/bash.exe" ] || { echo "MSYS2 not found at $MSYS2_ROOT (set MSYS2_ROOT)" >&2; exit 1; }

cd "$SRC"
CMAKE_EXTRA=""
if [ -n "$CELIO_VERSION" ]; then
  sed -i -E "s/^set\(CELIO_VERSION \"[^\"]*\"/set(CELIO_VERSION \"$CELIO_VERSION\"/" version.cmake
  grep -q 'set(GIT_COMMIT_SHORT "${CELIO_VERSION}")' version.cmake || \
    sed -i -E '/^set\(VERSION_STRING "\$\{CELIO_VERSION\}"\)/a set(GIT_COMMIT_SHORT "${CELIO_VERSION}")' version.cmake
  CMAKE_EXTRA="-DCELIO_VERSION=$CELIO_VERSION"
fi

"$MSYS/env" MSYSTEM=MINGW64 MGBA_GIT_COMMIT=$COMMIT "$MSYS/bash" -lc "
set -e
cd '$SRC'
cmake -S . -B $BUILD_DIR -G Ninja -Wno-dev -DCMAKE_BUILD_TYPE=Release -DBUILD_QT=ON -DBUILD_SDL=ON \
  -DUSE_FFMPEG=$FFMPEG -DUSE_DISCORD_RPC=OFF -DUSE_LUA=ON -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_LTO=$LTO $CMAKE_EXTRA >/dev/null
cmake --build $BUILD_DIR -j $JOBS
D='$DIST'; P=/mingw64/share/qt5/plugins
# keep the user's settings and logs of an earlier dist across a rebuild
K=\$(mktemp -d); for f in config.ini qt.ini library.sqlite3 nointro.sqlite3; do if [ -f \$D/\$f ]; then cp \$D/\$f \$K/; fi; done
[ -d \$D/logs ] && cp -r \$D/logs \$K/logs
mkdir -p \$D
for e in mgba-celio.exe mGBA.exe; do if [ -f $BUILD_DIR/\$e ]; then cp $BUILD_DIR/\$e \$D/; fi; done
: > \$D/portable.ini
for s in platforms imageformats audio styles mediaservice; do [ -d \$P/\$s ] && mkdir -p \$D/\$s && cp \$P/\$s/*.dll \$D/\$s/; done
# copy the DLLs the exe and plugins need (3 passes for dependencies of dependencies)
for i in 1 2 3; do for f in \$D/*.exe \$D/*.dll \$D/*/*.dll; do ldd \"\$f\" 2>/dev/null | grep -i mingw64 | awk '{print \$3}'; done | sort -u | while read d; do [ -f \"\$D/\$(basename \$d)\" ] || cp \"\$d\" \$D/; done; done
cp -r \$K/. \$D/ 2>/dev/null || true; rm -rf \$K
echo \"OK -> \$D\"
"
