#!/bin/bash
# Stage 30: build BENCtix-CAT's own packages natively in the chroot (qrtr,
# rmtfs, s22-sensord) and install them with BENCkernel-CAT.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/../lib/common.sh"
load_config
enter_userns "$@"
require_stamp swapped "run stages/20-convert.sh first"

relax_pacman_sandbox

# makepkg refuses to run as root; the account is removed again in stage 40
# uid 1500: 1000 belongs to the owner's account (D016)
in_chroot 'id builder >/dev/null 2>&1 || useradd -m -U -u 1500 builder'
as_builder() {
	in_chroot "cd /home/builder && setpriv --reuid builder --regid builder --init-groups \
		env HOME=/home/builder $1"
}

built() {	# newest local package file for NAME, or nothing
	ls "$LOCALPKGS/$1-"[0-9]*-aarch64.pkg.tar.* 2>/dev/null | sort -V | tail -1
}

for p in "${LOCAL_PKGS[@]}"; do
	pkgver=$(sed -n 's/^pkgver=//p' "packaging/$p/PKGBUILD")
	pkgrel=$(sed -n 's/^pkgrel=//p' "packaging/$p/PKGBUILD")
	if [ -z "${FORCE:-}" ] && [ -n "$(ls "$LOCALPKGS/$p-$pkgver-$pkgrel-aarch64.pkg.tar."* 2>/dev/null)" ]; then
		skip "$p $pkgver-$pkgrel already built"
	else
		log "building $p $pkgver-$pkgrel"
		# Build dependencies, as root (sudo cannot work under qemu-user)
		deps=$(bash -c ". packaging/$p/PKGBUILD; echo \${depends[*]} \${makedepends[*]}")
		for d in $deps; do
			[ "$d" = qrtr ] && continue		# ours, installed below once built
			in_chroot "pacman -Q $d >/dev/null 2>&1 || pacman -S --needed --noconfirm --asdeps $d"
		done
		rm -rf "$ROOTFS/home/builder/build/$p"
		mkdir -p "$ROOTFS/home/builder/build"
		cp -r "packaging/$p" "$ROOTFS/home/builder/build/$p"
		in_chroot "chown -R builder:builder /home/builder/build"
		as_builder "bash -c 'cd build/$p && PKGDEST=/home/builder/build/$p SRCDEST=/home/builder/src \
			makepkg --nodeps --noconfirm --nosign --nocheck --clean'"
		cp "$ROOTFS/home/builder/build/$p/$p-$pkgver-$pkgrel-aarch64.pkg.tar."* "$LOCALPKGS/"
		ok "built $p"
	fi
	in_chroot "pacman -U --noconfirm --needed /mnt/local/$(basename "$(built "$p")")"
done

# ----------------------------------------------- Artix rebuilds, D013 ---
# Artix's PKGBUILDs at pinned commits, with aarch64 added to arch=(). Their
# build dependencies are installed --asdeps and removed again at the end of
# this stage. PGP checks of upstream sources are skipped: the PKGBUILD (and
# with it every source checksum) is pinned by its Artix commit.
for entry in "${ARTIX_BUILD[@]}"; do
	read -r name commit want <<<"$entry"
	dir=/home/builder/build/artix-$name
	if [ -z "${FORCE:-}" ] && [ -f "$LOCALPKGS/.artix-$name" ] &&
	   [ "$(cat "$LOCALPKGS/.artix-$name")" = "$commit" ]; then
		skip "$name (Artix ${commit:0:12}) already built"
	else
		log "building $name from Artix ${commit:0:12}"
		rm -rf "$ROOTFS$dir"
		in_chroot "setpriv --reuid builder --regid builder --init-groups env HOME=/home/builder \
			git clone -q '$ARTIX_GIT/$name.git' '$dir' && cd '$dir' &&
			setpriv --reuid builder --regid builder --init-groups git -c advice.detachedHead=false checkout -q '$commit'"
		sed -i -E "s/^arch=\(.*\)/arch=('x86_64' 'aarch64')/" "$ROOTFS$dir/PKGBUILD"
		grep -q aarch64 "$ROOTFS$dir/PKGBUILD" || die "could not add aarch64 to $name's arch=()"
		# Package-specific changes (e.g. dropping a part that needs systemd)
		if [ -f "packaging/artix-patches/$name.sh" ]; then
			bash "packaging/artix-patches/$name.sh" "$ROOTFS$dir/PKGBUILD" ||
				die "packaging/artix-patches/$name.sh failed (did Artix's PKGBUILD change?)"
		fi
		# Dependencies not yet satisfied, as root (sudo cannot work here)
		# (quoted: versioned names like 'glib2>=2.80' must not become redirections)
		deps=$(bash -c ". '$ROOTFS$dir/PKGBUILD'; printf \"'%s' \" \"\${depends[@]}\" \"\${makedepends[@]}\"")
		missing=$(in_chroot "pacman -T $deps" || true)
		[ -z "$missing" ] || in_chroot "pacman -S --needed --noconfirm --asdeps$(assume_installed) $(printf "'%s' " $missing)"
		as_builder "bash -c 'cd $dir && PKGDEST=$dir SRCDEST=/home/builder/src \
			makepkg --nodeps --noconfirm --nosign --nocheck --skippgpcheck --clean'"
		# Every package the PKGBUILD makes (util-linux also makes util-linux-libs)
		: > "$LOCALPKGS/.artix-$name.files"
		for f in "$ROOTFS$dir"/*-aarch64.pkg.tar.*; do
			cp "$f" "$LOCALPKGS/"
			echo "${f##*/}" >> "$LOCALPKGS/.artix-$name.files"
		done
		echo "$commit" > "$LOCALPKGS/.artix-$name"
		ok "built $name: $(tr '\n' ' ' < "$LOCALPKGS/.artix-$name.files")"
	fi
	# Not --needed: Artix's build can carry the same version string as ALARM's.
	# Only the wanted packages of a split PKGBUILD (all if none are named).
	files=()
	while read -r f; do
		pkg=$(bsdtar -xOf "$LOCALPKGS/$f" .PKGINFO | sed -n 's/^pkgname = //p')
		if [ -z "$want" ] || [[ " $want " == *" $pkg "* ]]; then
			files+=("/mnt/local/$f")
		fi
	done < "$LOCALPKGS/.artix-$name.files"
	[ ${#files[@]} -gt 0 ] || die "$name: none of '$want' was built"
	in_chroot "pacman -U --noconfirm ${files[*]}"
done
# What only the builds needed
orph=$(in_chroot 'pacman -Qdtq || true')
[ -z "$orph" ] || in_chroot "pacman -Rns --noconfirm $(echo $orph)"

# ------------------------------------------- packages added post-swap ---
log "post-swap packages: ${ALARM_POSTSWAP_PKGS[*]} ${ARTIX_POSTSWAP_PKGS[*]}"
in_chroot "pacman -S --needed --noconfirm$(assume_installed) ${ALARM_POSTSWAP_PKGS[*]}"
ANY=()
for p in "${ARTIX_POSTSWAP_PKGS[@]}"; do
	f=$(fetch_artix_any "$p") || die "could not fetch $p from $ARTIX_MIRROR"
	ANY+=("/mnt/artix/${f##*/}")
done
in_chroot "pacman -U --needed --noconfirm ${ANY[*]}"

# ------------------------------------------------------------- kernel ---
K=$(ls "$REPO_ROOT/$PKG_DIR"/linux-benckernel-cat-[0-9]*-aarch64.pkg.tar.zst 2>/dev/null | sort -V | tail -1)
[ -n "$K" ] || die "no BENCkernel-CAT package in $PKG_DIR; run tools/build-benckernel-cat.sh"
cp -n "$K" "$LOCALPKGS/"
log "installing ${K##*/}"
in_chroot "pacman -U --noconfirm --needed /mnt/local/${K##*/}"
[ -f "$BOOTDIR/extlinux/extlinux.conf" ] || die "the kernel's hook wrote no extlinux.conf"
sed 's/^/    /' "$BOOTDIR/extlinux/extlinux.conf"

(cd "$LOCALPKGS" && sha256sum ./*.pkg.tar.*) > "$WORK/snapshot/localpkgs.sha256"
ok "stage 30 complete"
