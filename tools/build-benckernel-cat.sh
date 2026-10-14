#!/usr/bin/env bash
# Build BENCkernel-CAT and package it as linux-benckernel-cat (aarch64).
# RUNS ON: BENCbench (x86_64), cross-compiling.
#
#   tools/build-benckernel-cat.sh            # build and package
#   tools/build-benckernel-cat.sh --package  # repackage the last build only
#
# The kernel comes from the s22flip branch of the msm89x7 fork, configured
# from the postmarketOS base config plus the fragments in config.sh. The
# release is <version>-<PKGREL>-BENCkernel-CAT, which is also the module
# directory, so every build is distinct (D004).
#
# Every run writes docs/logs/benckernel-<timestamp>.log, starting with the
# effective settings, and the package gets a MANIFEST next to it (commit,
# config hash, package hash), as BENCtix does for its kernels.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
. ./config.sh

PACKAGE_ONLY=0
case "${1:-}" in
	--package) PACKAGE_ONLY=1 ;;
	'') ;;
	*) echo "usage: $0 [--package]" >&2; exit 2 ;;
esac

die() { printf '\nFATAL: %s\n' "$*" >&2; exit 1; }

mkdir -p docs/logs
LOG=docs/logs/benckernel-$(date +%Y%m%d-%H%M%S).log
exec > >(tee "$LOG") 2>&1

KSRC=$(cd "$KERNEL_SRC" && pwd)
mkdir -p "$BUILD_DIR" "$PKG_DIR"
BUILD=$(cd "$BUILD_DIR" && pwd)
PKGOUT=$(cd "$PKG_DIR" && pwd)
STAGE=$BUILD/stage

# The kernel tree must be the branch we package, with nothing uncommitted:
# the package records the commit, which is only true for a clean tree.
branch=$(git -C "$KSRC" rev-parse --abbrev-ref HEAD)
[ "$branch" = "$KERNEL_BRANCH" ] || die "kernel tree is on $branch, not $KERNEL_BRANCH"
[ -z "$(git -C "$KSRC" status --porcelain --untracked-files=no)" ] ||
	die "kernel tree has uncommitted changes"
commit=$(git -C "$KSRC" rev-parse --short=12 HEAD)
kver=$(make -s -C "$KSRC" kernelversion)
release=$kver-$PKGREL-BENCkernel-CAT

# What the kernel says about its build (uname -v, /proc/version): a UTC
# time and neutral names, not the builder's timezone, account and host
export KBUILD_BUILD_TIMESTAMP="$(date -u '+%a %b %e %H:%M:%S UTC %Y')"
export KBUILD_BUILD_USER=benctix KBUILD_BUILD_HOST=benctix-cat
M=(make -C "$KSRC" O="$BUILD/obj" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE"
   LOCALVERSION="-$PKGREL-BENCkernel-CAT" -j"$(nproc)")

echo "== BENCkernel-CAT build $(date -u -Is)"
echo "   kernel      $KSRC @ $branch $commit ($kver)"
echo "   release     $release"
echo "   base config $KERNEL_BASE_CONFIG"
echo "   fragments   $KERNEL_FRAGMENTS"
echo "   build dir   $BUILD"
echo "   packages    $PKGOUT"

if [ "$PACKAGE_ONLY" = 0 ]; then
	mkdir -p "$BUILD/obj"
	# shellcheck disable=SC2086 # the fragment list is space separated
	KCONFIG_CONFIG="$BUILD/obj/.config" ARCH=arm64 \
		"$KSRC/scripts/kconfig/merge_config.sh" -m -O "$BUILD/obj" \
		"$KERNEL_BASE_CONFIG" $KERNEL_FRAGMENTS
	"${M[@]}" olddefconfig

	# Every requested option must have survived olddefconfig
	missing=0
	# shellcheck disable=SC2086
	for f in $KERNEL_FRAGMENTS; do
		while IFS= read -r line; do
			case "$line" in
				CONFIG_*=*)
					grep -qxF "$line" "$BUILD/obj/.config" ||
						{ echo "   not applied: $line ($f)"; missing=1; } ;;
			esac
		done < "$f"
	done
	[ "$missing" = 0 ] || die "config fragments did not apply cleanly"

	"${M[@]}" Image.gz modules dtbs
fi

got=$(cat "$BUILD/obj/include/config/kernel.release")
[ "$got" = "$release" ] || die "built release is $got, expected $release"

# Stage the package's file tree
[ -e "$STAGE" ] && mv "$STAGE" "$STAGE.old-$(date +%s)"
mkdir -p "$STAGE/boot/dtbs/$release/$(dirname "$KERNEL_DTB")"
cp "$BUILD/obj/arch/arm64/boot/Image.gz" "$STAGE/boot/Image-$release.gz"
cp "$BUILD/obj/arch/arm64/boot/dts/$KERNEL_DTB" "$STAGE/boot/dtbs/$release/$KERNEL_DTB"
"${M[@]}" INSTALL_MOD_PATH="$STAGE/usr" INSTALL_MOD_STRIP=1 modules_install >/dev/null
# modules_install links build/ and source/ back into this host's trees
rm -f "$STAGE/usr/lib/modules/$release/build" "$STAGE/usr/lib/modules/$release/source"
cp "$BUILD/obj/.config" "$STAGE/usr/lib/modules/$release/config"
cp "$BUILD/obj/System.map" "$STAGE/usr/lib/modules/$release/System.map"
echo "$release" > "$STAGE/usr/lib/modules/$release/pkgbase"
echo "$KERNEL_DTB" > "$STAGE/usr/lib/modules/$release/dtb"

# Package: PKGBUILD reads the stage and these values from the environment
export BENCK_STAGE=$STAGE BENCK_RELEASE=$release BENCK_KVER=$kver BENCK_PKGREL=$PKGREL BENCK_COMMIT=$commit
work=$BUILD/makepkg
[ -e "$work" ] && mv "$work" "$work.old-$(date +%s)"
mkdir -p "$work"
cp packaging/linux-benckernel-cat/* "$work/"
(cd "$work" && CARCH=aarch64 PKGDEST="$PKGOUT" makepkg --force --nodeps --noconfirm --skipinteg)

pkg=$PKGOUT/linux-benckernel-cat-$kver-$PKGREL-aarch64.pkg.tar.zst
[ -f "$pkg" ] || die "package not found: $pkg"

manifest=$PKGOUT/linux-benckernel-cat-$kver-$PKGREL.MANIFEST
{
	echo "release       $release"
	echo "kernel commit $commit ($KERNEL_BRANCH)"
	echo "built         $(date -u -Is) on $(hostname)"
	echo "config sha256 $(sha256sum < "$BUILD/obj/.config" | cut -d' ' -f1)"
	echo "package       $(basename "$pkg")"
	echo "pkg sha256    $(sha256sum < "$pkg" | cut -d' ' -f1)"
	echo "modules       $(find "$STAGE/usr/lib/modules/$release" -name '*.ko*' | wc -l)"
} > "$manifest"

echo "== done"
cat "$manifest"
echo "   log $LOG"
