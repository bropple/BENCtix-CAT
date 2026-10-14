#!/bin/bash
# Stage 20: swap systemd for OpenRC + elogind, using the overlay BENCtix
# already built for aarch64 (D010) and Artix's arch=any packages. The order
# is BENCtix's (its D029): remove systemd and install the replacements in
# one step, with --overwrite for the files libudev takes over.
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/../lib/common.sh"
load_config
enter_userns "$@"
require_stamp converged "run stages/10-bootstrap.sh first"

# ------------------------------------------------------------ overlay ---
mkdir -p "$OVERLAY"
src=$REPO_ROOT/$BENCTIX_OVERLAY
for n in "${OVERLAY_SWAP_PKGS[@]}" "${OVERLAY_POSTSWAP_PKGS[@]}"; do
	ov "$n" >/dev/null && continue
	f=$(ls "$src/$n-"[0-9]*.pkg.tar.* 2>/dev/null | grep -v '\.sig$' | sort -V | tail -1)
	[ -n "$f" ] || die "BENCtix's overlay has no $n ($src)"
	cp "$f" "$OVERLAY/"
done
(cd "$OVERLAY" && sha256sum ./*.pkg.tar.*) > "$WORK/snapshot/overlay.sha256"
ok "overlay: $(wc -l < "$WORK/snapshot/overlay.sha256") packages copied from BENCtix"

# ALARM's own kernel was unpacked into the real /boot directory, which the
# chroot's /boot mount hides; keep it out of the image
if [ -n "$(ls -A "$ROOTFS/boot" 2>/dev/null)" ]; then
	mkdir -p "$WORK/attic/alarm-boot"
	mv -n "$ROOTFS"/boot/* "$WORK/attic/alarm-boot/"
	rmdir "$ROOTFS/boot" 2>/dev/null; mkdir -p "$ROOTFS/boot"
	[ -z "$(ls -A "$ROOTFS/boot")" ] || die "$ROOTFS/boot is still not empty"
	ok "moved ALARM's stock /boot aside to $WORK/attic/alarm-boot"
fi

alarm_mirrorlist() {
	echo "Server = $ALARM_MIRROR/\$arch/\$repo" > "$ROOTFS/etc/pacman.d/mirrorlist"
}
relax_pacman_sandbox

# --------------------------------------------------------------- swap ---
if has_stamp swapped; then
	skip "systemd already swapped out"
else
	log "fetching Artix arch=any packages"
	ANY=()
	for p in "${ARTIX_ANY_PKGS[@]}" "${ARTIX_OPENRC_PKGS[@]}"; do
		f=$(fetch_artix_any "$p") || die "could not fetch $p from $ARTIX_MIRROR"
		ANY+=("/mnt/artix/${f##*/}")
		printf '    %s\n' "${f##*/}"
	done
	in_chroot "pacman -U --noconfirm --needed /mnt/artix/$(basename "$(fetch_artix_any artix-keyring)")"
	in_chroot 'pacman-key --populate artix'

	OVL=()
	for n in "${OVERLAY_SWAP_PKGS[@]}"; do OVL+=("$(ov "$n")"); done

	log "removing systemd: ${SYSTEMD_PKGS[*]} ${SWAP_REMOVE_PKGS[*]}"
	# Exact installed names only: pacman -R aborts on any missing target
	in_chroot "
		installed=\$(pacman -Qq)
		rm_list=
		for p in ${SYSTEMD_PKGS[*]} ${SWAP_REMOVE_PKGS[*]}; do
			printf '%s\n' \"\$installed\" | grep -qx \"\$p\" && rm_list=\"\$rm_list \$p\"
		done
		[ -z \"\$rm_list\" ] || pacman -Rdd --noconfirm \$rm_list
	"
	alarm_mirrorlist

	log "installing the OpenRC stack (${#OVL[@]} overlay + ${#ANY[@]} Artix packages)"
	in_chroot "pacman -U --noconfirm --overwrite '*' ${OVL[*]} ${ANY[*]}" ||
		die "the swap failed; the rootfs is half converted. Start over from stage 10 (move $WORK/fsroot aside)"
	alarm_mirrorlist
	# Once, here: anything left in /boot now is ALARM's (initramfs images
	# nothing owns). Later runs must not touch it; BENCkernel-CAT lives there.
	if [ -n "$(ls -A "$BOOTDIR")" ]; then
		mkdir -p "$WORK/attic/alarm-boot-converged"
		mv -n "$BOOTDIR"/* "$WORK/attic/alarm-boot-converged/"
		ok "moved ALARM's leftover boot files aside to $WORK/attic/alarm-boot-converged"
	fi
	stamp swapped
fi


# Overlay rebuilds of packages ALARM links against systemd. Not --needed:
# several have the same version string as ALARM's (BENCtix D364)
POST=()
for n in "${OVERLAY_POSTSWAP_PKGS[@]}"; do POST+=("$(ov "$n")"); done
in_chroot "pacman -U --noconfirm ${POST[*]}"

# Artix's pacman ships an x86_64 makepkg.conf over ALARM's (BENCtix D363)
cp -a "$WORK/saved/makepkg.conf" "$ROOTFS/etc/makepkg.conf"
if [ -d "$WORK/saved/makepkg.conf.d" ]; then
	cp -a "$WORK/saved/makepkg.conf.d/." "$ROOTFS/etc/makepkg.conf.d/"
fi
grep -qx 'CARCH="aarch64"' "$ROOTFS/etc/makepkg.conf" || die "makepkg.conf is not aarch64"
! grep -qE -e '-march=x86-64|-fcf-protection' "$ROOTFS/etc/makepkg.conf" ||
	die "x86 flags left in makepkg.conf"

# pacman.conf must still list ALARM's repos (ours is kept, Artix's lands as .pacnew)
grep -qx '\[alarm\]' "$ROOTFS/etc/pacman.conf" || die "pacman.conf lost ALARM's repos"
rm -f "$ROOTFS"/etc/pacman.d/mirrorlist.pac{new,save}

# Firmware: only Qualcomm's public files; the phone's own come from local
# copies in stage 40
# (linux-firmware-qcom depends on -atheros, so that stays too)
FW=$(in_chroot "pacman -Qq | grep -E '^linux-firmware' | grep -vxE 'linux-firmware-(qcom|whence|atheros)'" || true)
if [ -n "$FW" ]; then
	log "removing firmware for other hardware: $(echo $FW)"
	in_chroot "pacman -Rdd --noconfirm $(echo $FW)"
fi
in_chroot "pacman -S --needed --noconfirm linux-firmware-qcom linux-firmware-atheros"

# ------------------------------------------------------------- verify ---
FAIL=0
check() { if in_chroot "$2" >/dev/null 2>&1; then ok "$1"; else warn "FAILED: $1"; FAIL=1; fi; }
check "openrc-init is PID 1"   '[ "$(readlink -f /usr/bin/init)" = /usr/bin/openrc-init ]'
check "udevadm present"        '[ -x /usr/bin/udevadm ]'
check "elogind present"        '[ -x /usr/lib/elogind/elogind ]'
check "libudev.so.1 resolves"  '[ -e /usr/lib/libudev.so.1 ]'
check "no systemctl"           '! command -v systemctl'
check "no libsystemd"          '! [ -e /usr/lib/libsystemd.so.0 ]'
check "mount, agetty, pacman"  'mount --version && agetty --version && pacman --version'
# Known and not run: pcscd and polkitd come with wpa_supplicant (pcsclite ->
# polkit); wpa_supplicant uses only libpcsclite, which is clean, no init
# script starts either daemon, and elogind needs polkit only optionally.
# Rebuild both from Artix before anything needs them.
NEED=$(grep -rl --binary-files=text 'libsystemd\.so\.0' "$ROOTFS/usr/bin" "$ROOTFS/usr/lib" 2>/dev/null |
	sed "s|^$ROOTFS||" | grep -vE 'avahi-daemon|dbus-broker-launch|libQt5Core|libsubid|/usr/bin/pcscd$|libpolkit-gobject-1|/polkit-1/polkitd$' || true)
if [ -n "$NEED" ]; then
	warn "still linked against libsystemd:"; printf '    %s\n' $NEED >&2; FAIL=1
fi
[ "$FAIL" = 0 ] || die "post-swap checks failed"
in_chroot 'pacman -Q' > "$WORK/snapshot/stage20-packages.txt"
ok "stage 20 complete: OpenRC + elogind, $(wc -l < "$WORK/snapshot/stage20-packages.txt") packages"
