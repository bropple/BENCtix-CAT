# Shared helpers for the BENCtix-CAT rootfs stages. Sourced, bash only.
# Adapted from BENCtix's lib/common.sh (copied, not linked: BENCtix is
# read-only from here).

_c() { [ -t 1 ] && printf '\033[%sm' "$1"; }
log()  { printf '%s==>%s %s\n' "$(_c '1;34')" "$(_c 0)" "$*"; }
ok()   { printf '%s  ok%s %s\n' "$(_c 32)" "$(_c 0)" "$*"; }
warn() { printf '%swarn%s %s\n' "$(_c 33)" "$(_c 0)" "$*" >&2; }
skip() { printf '%s  -- %s%s\n' "$(_c 2)" "$*" "$(_c 0)"; }
die()  { printf '%serr%s %s\n' "$(_c 31)" "$(_c 0)" "$*" >&2; exit 1; }

# ------------------------------------------------------- user namespace ---
# D009: every stage runs as an unprivileged user. It re-executes itself in a
# new user, mount and PID namespace, where it is "root" with the host's
# subordinate uids mapped below it. Mounts made there vanish with the
# namespace, so nothing is ever left bound under the rootfs, and real root
# is never involved.
enter_userns() {
	if [ "$(id -u)" != 0 ]; then
		command -v newuidmap >/dev/null || die "newuidmap missing (shadow)"
		grep -q "^$(id -un):" /etc/subuid ||
			die "no /etc/subuid range for $(id -un)"
		exec unshare --map-auto --map-root-user --mount --pid --fork \
			--kill-child --propagation private -- bash "$0" "$@"
	fi
	# Real root has the full identity map; refuse it
	if [ "$(awk 'NR==1{print $2}' /proc/self/uid_map)" = 0 ]; then
		die "run this as your normal user, not as root (D009)"
	fi
}

# ---------------------------------------------------------------- stamps ---
stamp()     { mkdir -p "$WORK/.stamps"; date -u +%FT%TZ > "$WORK/.stamps/$1"; }
has_stamp() { [ -z "${FORCE:-}" ] && [ -f "$WORK/.stamps/$1" ]; }
require_stamp() { [ -f "$WORK/.stamps/$1" ] || die "$2"; }

# ---------------------------------------------------------------- chroot ---
# Order matters (BENCtix lib/common.sh): the self-bind first, so "/" is a
# mount point for pacman's free-space check, then everything on top of it.
# Checked on the mounts themselves: in_chroot often runs in a $(...) subshell,
# where a variable set here would be lost.
mount_chroot() {
	if mountpoint -q "$ROOTFS/boot" && mountpoint -q "$ROOTFS/mnt/local" &&
	   [ -r "$ROOTFS/proc/self/mounts" ]; then
		return 0
	fi
	mount --bind "$ROOTFS" "$ROOTFS"
	mkdir -p "$ROOTFS"/{proc,sys,dev,boot,var/cache/pacman/pkg,mnt/overlay,mnt/artix,mnt/local}
	mount -t proc proc "$ROOTFS/proc"
	mount --rbind /sys "$ROOTFS/sys"
	mount --rbind /dev "$ROOTFS/dev"
	mkdir -p "$PKGCACHE" "$BOOTDIR" "$OVERLAY" "$WORK/dl/artix"
	mount --bind "$PKGCACHE" "$ROOTFS/var/cache/pacman/pkg"
	mount --bind "$OVERLAY" "$ROOTFS/mnt/overlay"
	mount --bind "$WORK/dl/artix" "$ROOTFS/mnt/artix"
	mkdir -p "$LOCALPKGS"
	mount --bind "$LOCALPKGS" "$ROOTFS/mnt/local"
	# /boot is the cache partition on the phone; keep it out of the btrfs
	mount --bind "$BOOTDIR" "$ROOTFS/boot"
	# ALARM's resolv.conf is a link into systemd-resolved's runtime dir
	rm -f "$ROOTFS/etc/resolv.conf"
	cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf"
	[ -e "$ROOTFS/etc/mtab" ] || die "/proc is not visible in the chroot"
}

in_chroot() {
	mount_chroot
	chroot "$ROOTFS" /usr/bin/env -i HOME=/root TERM="${TERM:-dumb}" \
		PATH=/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl \
		LANG=C.UTF-8 PAGER=cat GIT_PAGER=cat \
		/bin/bash -euo pipefail -c "$1"
}

# ---------------------------------------------------------- downloading ---
fetch() {	# URL DEST [MD5]
	if [ ! -s "$2" ]; then
		curl -fL --retry 3 -o "$2.part" "$1" || die "download failed: $1"
		mv -f "$2.part" "$2"
	fi
	if [ -n "${3:-}" ]; then
		echo "$3  $2" | md5sum -c --quiet - || die "md5 mismatch for $2"
	fi
}

# An arch=any package from the Artix x86_64 mirror (init scripts, base)
fetch_artix_any() {
	local name=$1 repo listing fn
	fn=$(ls "$WORK/dl/artix/$name-"[0-9]*.pkg.tar.* 2>/dev/null | grep -v '\.sig$' | head -1)
	[ -n "$fn" ] && { printf '%s' "$fn"; return 0; }
	for repo in system world galaxy; do
		listing=$WORK/dl/artix/.list-$repo
		[ -s "$listing" ] || curl -sfL --max-time 90 "$ARTIX_MIRROR/$repo/os/x86_64/" -o "$listing" || continue
		fn=$(grep -oE "\"$name-[0-9][^\"]*-any\.pkg\.tar\.[a-z]+\"" "$listing" | tr -d '"' | sort -V | tail -1)
		[ -n "$fn" ] || continue
		curl -sfL --max-time 180 -o "$WORK/dl/artix/$fn" "$ARTIX_MIRROR/$repo/os/x86_64/$fn" || continue
		printf '%s' "$WORK/dl/artix/$fn"
		return 0
	done
	return 1
}

# ------------------------------------------------------- pacman sandbox ---
# pacman 7's download sandbox (Landlock, seccomp, DownloadUser) does not work
# under qemu-user. Relaxed for the build only; stage 30 restores it.
TAG='# BENCTIX-CAT-BUILD-ONLY'
relax_pacman_sandbox() {
	local conf=$ROOTFS/etc/pacman.conf
	grep -qx DisableSandboxFilesystem "$conf" && return 0
	sed -i -E "
		s|^#(DisableSandboxFilesystem)\$|$TAG\n\1|
		s|^#(DisableSandboxSyscalls)\$|$TAG\n\1|
		s|^(DownloadUser[[:space:]]*=.*)\$|$TAG\n#\1|
	" "$conf"
	grep -qx DisableSandboxFilesystem "$conf" || die "could not relax the pacman sandbox"
}
restore_pacman_sandbox() {
	local conf=$ROOTFS/etc/pacman.conf
	sed -i -E "
		/^$TAG\$/d
		s|^(DisableSandboxFilesystem)\$|#\1|
		s|^(DisableSandboxSyscalls)\$|#\1|
		s|^#(DownloadUser[[:space:]]*=.*)\$|\1|
	" "$conf"
	! grep -q BENCTIX-CAT-BUILD-ONLY "$conf" || die "build-only lines left in $conf"
}

# Absolute in-chroot path of an overlay package
ov() {
	local f
	f=$(ls "$OVERLAY/$1-"[0-9]*.pkg.tar.* 2>/dev/null | grep -v '\.sig$' | sort -V | tail -1)
	[ -n "$f" ] || return 1
	printf '/mnt/overlay/%s' "${f##*/}"
}

assume_installed() {
	local n
	for n in "${EXTRA_ASSUME_INSTALLED[@]}"; do printf ' --assume-installed %s' "$n"; done
}

# ----------------------------------------------------------------- setup ---
load_config() {
	cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
	. ./config.sh
	mkdir -p "$WORK"
	WORK=$(readlink -f "$WORK")
	ROOTFS=$WORK/fsroot/@
	BOOTDIR=$WORK/boot
	PKGCACHE=$WORK/pkgcache
	OVERLAY=$WORK/overlay
	LOCALPKGS=$WORK/localpkgs
	REPO_ROOT=$PWD
}
