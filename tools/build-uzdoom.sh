#!/usr/bin/env bash
# Build UZDoom for the phone and package it as uzdoom-s22 (aarch64), for
# the SD card (pacman-sd -U). RUNS ON: BENCbench (x86_64), cross-compiling.
#
#   tools/build-uzdoom.sh [user@host]      (default root@172.16.42.1)
#
# Neither ALARM nor UZDoom has an aarch64 build, and on the phone it would
# take hours. The sysroot is the phone's own /usr (rsync over USB, so the
# phone must be connected): the libraries it links against are the ones it
# will run with. The phone's glibc (2.43) is linked, not the cross
# toolchain's (2.44); the toolchain's headers come first so libstdc++'s
# wrappers work. UZDoom's build tools (zipdir, re2c, lemon) are built
# natively first. Vulkan is off (no driver for the Adreno 308); the GLES
# renderer is what runs, with a3xx-depth-clamp.patch (D061) and
# s22-glfinish.patch (D063).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
. ./config.sh

PHONE=${1:-root@$USBNET_ADDR}
TAG=5.0.3
PKGREL_UZ=3
die() { printf '\nFATAL: %s\n' "$*" >&2; exit 1; }

mkdir -p ../out/uzdoom "$PKG_DIR"
W=$(cd ../out/uzdoom && pwd)
PKGOUT=$(cd "$PKG_DIR" && pwd)
SYSROOT=$W/sysroot

echo "==> sysroot from $PHONE"
mkdir -p "$SYSROOT/usr/share"
ln -sfn usr/lib "$SYSROOT/lib"
ln -sfn usr/lib "$SYSROOT/lib64"
ln -sfn lib "$SYSROOT/usr/lib64"
ex=(firmware/ modules/ 'python3*/' guile/ samba/ udev/ 'libLLVM*' 'libwebkit*'
    'libjavascriptcore*' 'libicudata*' 'libgallium*')
rsync -a --delete "${ex[@]/#/--exclude=}" "$PHONE:/usr/include" "$PHONE:/usr/lib" "$SYSROOT/usr/"
rsync -a --delete "$PHONE:/usr/share/pkgconfig" "$SYSROOT/usr/share/"

echo "==> UZDoom $TAG"
if [ ! -d "$W/src/.git" ]; then
	git clone -q --depth 1 -b "$TAG" https://github.com/UZDoom/UZDoom.git "$W/src"
fi
[ "$(git -C "$W/src" describe --tags)" = "$TAG" ] || die "$W/src is not at $TAG"
# Our patches (packaging/uzdoom/*.patch), on a clean tree
git -C "$W/src" checkout -q -- .
for p in packaging/uzdoom/*.patch; do
	git -C "$W/src" apply "$REPO/$p" || die "$p does not apply"
done

echo "==> build tools (native)"
cmake -S "$W/src" -B "$W/native" -DCMAKE_BUILD_TYPE=Release >/dev/null
make -C "$W/native" -j"$(nproc)" zipdir re2c lemon >/dev/null

echo "==> UZDoom (aarch64)"
cat > "$W/aarch64.cmake" <<TC
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
# The phone's headers after the toolchain's, never as -I
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-a53 -idirafter $SYSROOT/usr/include")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a53 -idirafter $SYSROOT/usr/include")
list(APPEND CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES $SYSROOT/usr/include)
list(APPEND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES $SYSROOT/usr/include)
# The phone's glibc and start files
set(CMAKE_EXE_LINKER_FLAGS_INIT "-B$SYSROOT/usr/lib -L$SYSROOT/usr/lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-B$SYSROOT/usr/lib -L$SYSROOT/usr/lib")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_SYSROOT_DIR} $SYSROOT)
set(ENV{PKG_CONFIG_LIBDIR} $SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig)
TC
cmake -S "$W/src" -B "$W/cross" -DCMAKE_TOOLCHAIN_FILE="$W/aarch64.cmake" \
	-DIMPORT_EXECUTABLES="$W/native/ImportExecutables.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
	-DHAVE_VULKAN=OFF -DHAVE_GLES2=ON >/dev/null
make -C "$W/cross" -j"$(nproc)" >/dev/null

# Nothing newer than the phone's glibc
need=$(${CROSS_COMPILE}objdump -T "$W/cross/uzdoom" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
have=$(ls "$SYSROOT/usr/lib"/libc.so.6 >/dev/null && strings "$SYSROOT/usr/lib/libc.so.6" | grep -o '^GLIBC_[0-9.]*$' | sort -uV | tail -1)
[ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | tail -1)" = "$have" ] ||
	die "uzdoom needs $need, the phone has $have"

echo "==> package"
STAGE=$W/stage
rm -rf "$STAGE"
DESTDIR=$STAGE make -C "$W/cross" install >/dev/null
${CROSS_COMPILE}strip "$STAGE/usr/bin/uzdoom"
# The tile is ours (s22-doom.desktop, with the keypad's game mode)
rm -rf "$STAGE/usr/share/applications" "$STAGE/usr/share/metainfo" "$STAGE/usr/share/mime"
work=$W/makepkg
rm -rf "$work"
mkdir -p "$work"
cp packaging/uzdoom/* "$work/"
export UZ_STAGE=$STAGE UZ_VER=$TAG UZ_PKGREL=$PKGREL_UZ
(cd "$work" && CARCH=aarch64 PKGDEST="$PKGOUT" makepkg --force --nodeps --noconfirm --skipinteg >/dev/null)
ls -l "$PKGOUT/uzdoom-s22-$TAG-$PKGREL_UZ-aarch64.pkg.tar."*
