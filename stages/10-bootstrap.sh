#!/bin/bash
# Stage 10: an aarch64 ALARM rootfs, converged against the live mirror, with
# the packages BENCtix-CAT wants from ALARM. Runs on the build host as your
# normal user (D009). Idempotent; FORCE=1 redoes stamped steps.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/../lib/common.sh"
load_config
enter_userns "$@"

grep -q '^flags:.*F' /proc/sys/fs/binfmt_misc/qemu-aarch64 2>/dev/null ||
	die "qemu-aarch64 binfmt (with the F flag) is not registered"

# ---------------------------------------------------------------- seed ---
mkdir -p "$WORK/dl"
TARBALL=$WORK/dl/${ALARM_TARBALL_URL##*/}
if has_stamp extracted; then
	skip "rootfs already extracted"
else
	md5=$(curl -sfL --max-time 30 "$ALARM_TARBALL_URL.md5" | awk '{print $1}') ||
		die "cannot fetch the tarball's md5"
	# BENCtix may already hold the same tarball; copy it rather than download
	bt=$REPO_ROOT/$BENCTIX_OVERLAY/../dl/${ALARM_TARBALL_URL##*/}
	if [ ! -s "$TARBALL" ] && [ -s "$bt" ] && echo "$md5  $bt" | md5sum -c --quiet - 2>/dev/null; then
		log "copying the seed tarball from BENCtix (md5 matches upstream)"
		cp "$bt" "$TARBALL.part" && mv "$TARBALL.part" "$TARBALL"
	fi
	fetch "$ALARM_TARBALL_URL" "$TARBALL" "$md5"
	if [ -e "$ROOTFS" ]; then
		[ -z "$(ls -A "$ROOTFS")" ] ||
			die "$ROOTFS is not empty; move it aside (mv) to start over"
	fi
	mkdir -p "$ROOTFS"
	log "extracting the ALARM rootfs"
	bsdtar -xpf "$TARBALL" -C "$ROOTFS" --numeric-owner
	stamp extracted
fi

[ "$(in_chroot 'uname -m')" = aarch64 ] || die "the chroot does not run aarch64 binaries"
ok "chroot runs aarch64 under qemu-user"

# The mirrorlist and sandbox relaxation are cheap repairs, run every time
echo "Server = $ALARM_MIRROR/\$arch/\$repo" > "$ROOTFS/etc/pacman.d/mirrorlist"
relax_pacman_sandbox

if has_stamp keyring || [ -s "$ROOTFS/etc/pacman.d/gnupg/pubring.gpg" ]; then
	skip "pacman keyring ready"
else
	log "initialising the pacman keyring (slow under emulation)"
	in_chroot 'pacman-key --init && pacman-key --populate archlinuxarm'
	stamp keyring
fi

if has_stamp converged; then
	skip "rootfs already converged"
else
	log "converging against $ALARM_MIRROR (allows downgrades)"
	in_chroot 'pacman -Syuu --noconfirm'
	stamp converged
fi

# ----------------------------------------------------------- packages ---
# Only before the swap: afterwards ALARM's builds would replace ours
[ ! -e "$ROOTFS/usr/bin/openrc-init" ] || die "the rootfs is already converted to OpenRC (stage 20)"
log "installing from ALARM: ${ALARM_PKGS[*]}"
in_chroot "pacman -S --needed --noconfirm$(assume_installed) ${ALARM_PKGS[*]}"

# Native builds (stage 30) run artix-meson in Artix PKGBUILDs and arch-meson
# in ours; ALARM only has arch-meson
in_chroot 'ln -sf arch-meson /usr/bin/artix-meson'

# ALARM's own makepkg.conf, saved before the swap installs Artix's x86_64
# one over it (BENCtix D363); stage 20 puts it back
mkdir -p "$WORK/saved"
cp -a "$ROOTFS/etc/makepkg.conf" "$WORK/saved/"
if [ -d "$ROOTFS/etc/makepkg.conf.d" ]; then cp -a "$ROOTFS/etc/makepkg.conf.d" "$WORK/saved/"; fi
grep -qx 'CARCH="aarch64"' "$WORK/saved/makepkg.conf" || die "saved makepkg.conf is not aarch64"

mkdir -p "$WORK/snapshot"
in_chroot 'pacman -Q' > "$WORK/snapshot/stage10-packages.txt"
ok "stage 10 complete: $(wc -l < "$WORK/snapshot/stage10-packages.txt") packages"
