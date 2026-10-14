#!/bin/bash
# Stage 40: configure the converted rootfs for the phone. Not stamped: every
# step is an overwrite or idempotent, so a changed file always lands (BENCtix
# C042).
set -euo pipefail
. "$(dirname "$(readlink -f "$0")")/../lib/common.sh"
load_config
enter_userns "$@"
require_stamp swapped "run stages/20-convert.sh first"
[ -x "$ROOTFS/usr/bin/rmtfs" ] || die "run stages/30-packages.sh first"

R=$ROOTFS
mount_chroot

# ------------------------------------------------------------- files ---
log "installing BENCtix-CAT files"
for f in files/init.d/*; do install -m 0755 "$f" "$R/etc/init.d/${f##*/}"; done
for f in files/conf.d/*; do install -m 0644 "$f" "$R/etc/conf.d/${f##*/}"; done
install -m 0755 files/sbin/s22-partguard "$R/usr/bin/s22-partguard"
install -Dm 0755 files/bin/benctix-cat-refuse-systemd "$R/usr/local/bin/benctix-cat-refuse-systemd"
install -Dm 0755 files/bin/reboot-lk2nd "$R/usr/local/bin/reboot-lk2nd"
# The GUI's power menu (D043): s22-power, which group wheel may sudo
install -Dm 0755 files/bin/s22-power "$R/usr/local/bin/s22-power"
install -Dm 0440 files/etc/sudoers.d/20-s22-power "$R/etc/sudoers.d/20-s22-power"
# File sharing (D056): s22-fileshare, which group wheel may sudo; Samba's
# config. The service starts switched off (Settings switches it).
install -Dm 0755 files/bin/s22-fileshare "$R/usr/local/bin/s22-fileshare"
install -Dm 0440 files/etc/sudoers.d/21-s22-fileshare "$R/etc/sudoers.d/21-s22-fileshare"
install -Dm 0644 files/etc/samba/smb.conf "$R/etc/samba/smb.conf"
# Programs on the SD card (D046)
install -Dm 0755 files/bin/pacman-sd "$R/usr/local/bin/pacman-sd"
install -Dm 0755 files/bin/sd-run "$R/usr/local/bin/sd-run"
install -Dm 0644 files/etc/profile.d/pacman-sd.sh "$R/etc/profile.d/pacman-sd.sh"
install -Dm 0644 files/etc/pacman.d/hooks/zz-pacman-sd.hook "$R/etc/pacman.d/hooks/zz-pacman-sd.hook"
install -Dm 0644 files/pacman-hooks/00-benctix-cat-refuse-systemd.hook \
	"$R/etc/pacman.d/hooks/00-benctix-cat-refuse-systemd.hook"
install -Dm 0644 files/pacman-hooks/90-benctix-cat-sshd-restart.hook \
	"$R/etc/pacman.d/hooks/90-benctix-cat-sshd-restart.hook"
install -m 0644 files/etc/fstab "$R/etc/fstab"
install -m 0644 files/etc/dnsmasq.conf "$R/etc/dnsmasq.conf"
install -Dm 0644 files/etc/sshd-benctix-cat.conf "$R/etc/ssh/sshd_config.d/10-benctix-cat.conf"
install -m 0644 files/etc/chrony.conf "$R/etc/chrony.conf"
install -m 0644 files/etc/s22-console.map "$R/etc/s22-console.map"
install -Dm 0644 files/etc/logind-benctix-cat.conf "$R/etc/elogind/logind.conf.d/20-benctix-cat.conf"
install -Dm 0644 files/etc/sysctl-benctix-cat.conf "$R/etc/sysctl.d/50-benctix-cat.conf"
install -m 0644 files/etc/dhcpcd.enter-hook "$R/etc/dhcpcd.enter-hook"
install -Dm 0644 files/etc/mpv.conf "$R/etc/mpv/mpv.conf"
# Sound (D040): PipeWire's and WirePlumber's settings, the session script
install -Dm 0644 files/etc/wireplumber/wireplumber.conf.d/50-s22-card.conf \
	"$R/etc/wireplumber/wireplumber.conf.d/50-s22-card.conf"
install -Dm 0644 files/etc/wireplumber/wireplumber.conf.d/51-s22-bluetooth.conf \
	"$R/etc/wireplumber/wireplumber.conf.d/51-s22-bluetooth.conf"
install -Dm 0644 files/etc/pipewire/pipewire.conf.d/50-s22-media.conf \
	"$R/etc/pipewire/pipewire.conf.d/50-s22-media.conf"
install -Dm 0755 files/lib/s22/s22-audio-session "$R/usr/lib/s22/s22-audio-session"
# WiFi and Bluetooth share one radio (D019): power save off, and a gentle
# BlueZ background (auto-connect) scan; discovery keeps its fast defaults
install -Dm 0644 files/udev/80-wifi-powersave.rules "$R/etc/udev/rules.d/80-wifi-powersave.rules"
# The GUI's Settings app sets the brightness (group video)
install -Dm 0644 files/udev/70-s22-backlight.rules "$R/etc/udev/rules.d/70-s22-backlight.rules"
install -Dm 0644 files/udev/70-s22-camera.rules "$R/etc/udev/rules.d/70-s22-camera.rules"
sed -i -E 's/^#?ScanIntervalAutoConnect=.*/ScanIntervalAutoConnect=2048/; s/^#?ScanWindowAutoConnect=.*/ScanWindowAutoConnect=18/' "$R/etc/bluetooth/main.conf"
grep -q '^ScanWindowAutoConnect=18$' "$R/etc/bluetooth/main.conf" || die "could not set BlueZ's auto-connect scan"
# Networks are added on the phone; never overwrite a configured file
[ -e "$R/etc/wpa_supplicant/wpa_supplicant.conf" ] ||
	install -Dm 0600 files/etc/wpa_supplicant.conf "$R/etc/wpa_supplicant/wpa_supplicant.conf"
sed -i "s|^USBNET_ADDR=.*|USBNET_ADDR=$USBNET_ADDR|" "$R/etc/conf.d/s22-usbnet"
# dhcpcd runs the WiFi; the USB link has a fixed address and s22-celld
# configures the cellular ones (wwan*)
sed -i '/^denyinterfaces usb0$/d' "$R/etc/dhcpcd.conf"
grep -qx 'denyinterfaces usb0 wwan\*' "$R/etc/dhcpcd.conf" ||
	echo 'denyinterfaces usb0 wwan*' >> "$R/etc/dhcpcd.conf"

# ---------------------------------------------------------- identity ---
echo "$HOSTNAME_DEFAULT" > "$R/etc/hostname"
cat > "$R/etc/hosts" <<EOF
127.0.0.1        localhost
::1              localhost
127.0.1.1        $HOSTNAME_DEFAULT.localdomain $HOSTNAME_DEFAULT
EOF
# ALARM's nsswitch.conf names systemd's NSS modules (BENCtix C063)
cat > "$R/etc/nsswitch.conf" <<'EOF'
# Name Service Switch: BENCtix-CAT (OpenRC + elogind). See nss-elogind(8).
passwd: files elogind
group: files [SUCCESS=merge] elogind
shadow: files elogind
gshadow: files elogind
publickey: files
hosts: files dns
networks: files
protocols: files
services: files
ethers: files
rpc: files
netgroup: files
EOF
cat > "$R/usr/lib/os-release" <<EOF
NAME="BENCtix-CAT"
PRETTY_NAME="BENCtix-CAT (Cat S22 Flip)"
ID=benctix-cat
ID_LIKE=arch
BUILD_ID=$(date -u +%Y%m%d)
ANSI_COLOR="0;36"
LOGO=benctix-cat
EOF
ln -sf ../usr/lib/os-release "$R/etc/os-release"
printf 'BENCtix-CAT \\r (\\l)\n\n' > "$R/etc/issue"
printf 'PRETTY_HOSTNAME="Cat S22 Flip"\nCHASSIS=handset\nDEPLOYMENT=development\n' > "$R/etc/machine-info"
# 32 hex digits and a newline: D-Bus takes no other (D047)
[ -s "$R/etc/machine-id" ] || { od -An -tx1 -N16 /dev/urandom | tr -d ' \n'; echo; } > "$R/etc/machine-id"
if [ -e "$R/usr/share/zoneinfo/$TIMEZONE" ]; then
	ln -sf "../usr/share/zoneinfo/$TIMEZONE" "$R/etc/localtime"
	echo "$TIMEZONE" > "$R/etc/timezone"
fi
printf 'consolefont="ter-v16n"\n' > "$R/etc/conf.d/consolefont"
# A UTF-8 locale; login shells read /etc/locale.conf via profile.d/locale.sh
sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/' "$R/etc/locale.gen"
in_chroot 'locale-gen >/dev/null'
echo LANG=en_US.UTF-8 > "$R/etc/locale.conf"

# ---------------------------------------------------------- runlevels ---
log "runlevels"
in_chroot '[ -e /etc/init.d/agetty.tty1 ] || ln -s agetty /etc/init.d/agetty.tty1'
add() {	# runlevel service...
	local lvl=$1 s; shift
	for s; do
		[ -e "$R/etc/init.d/$s" ] || die "no init script for $s"
		in_chroot "rc-update add $s $lvl >/dev/null"
	done
}
add sysinit udev udev-trigger dmesg s22-partguard
# The PMIC RTC cannot be set from Linux: s22-clock replaces hwclock (D015)
in_chroot 'rc-update del hwclock boot >/dev/null 2>&1 || true'
add boot elogind esysusers etmpfiles-setup hostname localmount sysctl s22-zram s22-clock
add default dbus metalog dhcpcd s22-usbnet dnsmasq sshd tqftpserv rmtfs s22-sensord agetty.tty1 \
	wpa_supplicant chrony s22-outer s22-say s22-notifyd s22-outerd s22-t9d s22-lidd s22-sleepd bluetoothd s22-smsd s22-imsd s22-celld s22-gnssd s22-battery s22-call tailscaled s22-vpnd s22-btd s22-bus s22-audio \
	s22-settings seatd s22-gui

# ------------------------------------------------------------ accounts ---
log "accounts: no stock alarm user, root by ssh key only"
if in_chroot 'id alarm' >/dev/null 2>&1; then
	in_chroot 'userdel -f -r alarm 2>/dev/null || true'
	! in_chroot 'id alarm' >/dev/null 2>&1 || die "the stock alarm account is still there"
fi
in_chroot 'passwd -l root >/dev/null'
[ -s "$SSH_AUTHORIZED_KEYS_FILE" ] || die "no ssh public key at $SSH_AUTHORIZED_KEYS_FILE; the phone would be unreachable"
install -d -m 0700 "$R/root/.ssh"
install -m 0600 "$SSH_AUTHORIZED_KEYS_FILE" "$R/root/.ssh/authorized_keys"
# zsh as BENCtix has it: oh-my-zsh (pinned) with af-magic
OMZ=$R/usr/share/oh-my-zsh
if [ "$(cat "$OMZ/.benctix-cat-commit" 2>/dev/null)" != "$OHMYZSH_REF" ]; then
	if [ -e "$OMZ" ]; then
		mkdir -p "$WORK/attic"
		mv "$OMZ" "$WORK/attic/oh-my-zsh.$(date +%s)"
	fi
	git clone -q "$OHMYZSH_REPO" "$OMZ" && git -C "$OMZ" checkout -q "$OHMYZSH_REF" ||
		die "could not clone oh-my-zsh at $OHMYZSH_REF"
	echo "$OHMYZSH_REF" > "$OMZ/.benctix-cat-commit"
fi
sed "s|@THEME@|$OHMYZSH_THEME|" files/etc/zshrc > "$R/etc/skel/.zshrc"
install -m 0644 "$R/etc/skel/.zshrc" "$R/root/.zshrc"
# The logo on ssh logins only; nothing extra on the phone's own screen
: > "$R/etc/motd"
install -Dm 0644 files/brand/benco_ascii_block.txt "$R/usr/share/benctix-cat/banner"
printf '\n  BENCtix-CAT on the Cat S22 Flip\n\n' >> "$R/usr/share/benctix-cat/banner"
install -m 0644 files/profile.d/benctix-cat-ssh-banner.sh "$R/etc/profile.d/benctix-cat-ssh-banner.sh"
grep -qx /usr/bin/zsh "$R/etc/shells" || echo /usr/bin/zsh >> "$R/etc/shells"
in_chroot 'chsh -s /usr/bin/zsh root >/dev/null'

# The owner's account (D016). Its home is on the SD card, which the image
# does not hold and a reflash does not touch; on a new card, create it with
# `mkhomedir_helper <user>` and copy the ssh key in. The password stays
# locked until it is set on the phone with passwd.
# The build account goes first; an older build gave it uid 1000
in_chroot 'if id builder >/dev/null 2>&1; then userdel -f -r builder; fi'
if [ -n "${OWNER_USER:-}" ] && ! in_chroot "id $OWNER_USER" >/dev/null 2>&1; then
	in_chroot "useradd -M -d /home/$OWNER_USER -u 1000 -U -G wheel,audio,video,input,network,rfkill -s /usr/bin/zsh $OWNER_USER"
fi
# Older images: the Settings app switches radios (group rfkill)
[ -n "${OWNER_USER:-}" ] && in_chroot "usermod -aG rfkill $OWNER_USER"
printf '%%wheel ALL=(ALL:ALL) ALL\n' > "$R/etc/sudoers.d/10-wheel"
chmod 0440 "$R/etc/sudoers.d/10-wheel"
in_chroot 'visudo -cf /etc/sudoers.d/10-wheel >/dev/null' || die "sudoers.d/10-wheel does not parse"

# ------------------------------------------------- phone-derived files ---
# Local copies only; the image never leaves this host and the phone
if [ -d "$REPO_ROOT/$FIRMWARE_DIR" ]; then
	log "firmware from the local copy"
	cp -r "$REPO_ROOT/$FIRMWARE_DIR/." "$R/usr/lib/firmware/"
	in_chroot 'chown -R root:root /usr/lib/firmware'
else
	warn "no local firmware at $FIRMWARE_DIR: no modem, WiFi, audio DSP or sensors"
fi
install -d -m 0755 "$R/var/lib/s22-sensord"
for pair in "$SENSORS_GROUPS:groups.txt" "$SENSORS_REG:sns.reg"; do
	src=$REPO_ROOT/${pair%%:*}
	if [ -s "$src" ]; then
		install -m 0644 "$src" "$R/var/lib/s22-sensord/${pair#*:}"
	else
		warn "no ${pair#*:} at ${pair%%:*}: s22-sensord will not start until it is installed"
	fi
done

# ------------------------------------------------------------- pacman ---
CONF=$R/etc/pacman.conf
sed -i -E '/^[[:space:]]*#?[[:space:]]*IgnorePkg[[:space:]]*=.*$/d' "$CONF"
sed -i -E "/^\[options\]/a IgnorePkg   = ${IGNORE_PKGS[*]}" "$CONF"
grep -qE '^IgnorePkg   = .*\budev\b' "$CONF" || die "IgnorePkg was not written"

# ------------------------------------------------- undo build-time setup ---
log "removing build-time setup"
in_chroot 'if id builder >/dev/null 2>&1; then userdel -f -r builder; fi'
! grep -q '^builder:' "$R/etc/passwd" || die "the builder account is still there"
sed -i '/^builder:/d' "$R/etc/group"
restore_pacman_sandbox
# dhcpcd writes resolv.conf on the phone; drop the build host's copy
: > "$R/etc/resolv.conf"

# -------------------------------------------------------------- checks ---
DK=$(in_chroot 'pacman -Dk 2>&1' || true)
if printf '%s\n' "$DK" | grep -q '^error:'; then
	warn "pacman -Dk:"; printf '%s\n' "$DK" | grep '^error:' | sed 's/^/    /' >&2
fi
ORPH=$(in_chroot 'pacman -Qdtq || true')
[ -z "$ORPH" ] || warn "orphans: $(echo $ORPH)"
in_chroot 'rc-update show -v | grep -E "\| *(sysinit|boot|default)"' | sed 's/^/    /'
in_chroot 'pacman -Q' > "$WORK/snapshot/rootfs-packages.txt"
stamp configured
ok "stage 40 complete"
