# BENCtix-CAT build settings, sourced by the tools.
# Paths are relative to the repo root unless absolute.

# Kernel: the msm89x7 fork, branch s22flip (github.com/bropple/linux)
KERNEL_SRC=../linux
KERNEL_BRANCH=s22flip
# Base config (postmarketOS msm89x7) and the fragments merged on top of it
KERNEL_BASE_CONFIG=../configs/config-postmarketos-qcom-msm89x7.aarch64
KERNEL_FRAGMENTS="$KERNEL_SRC/arch/arm64/configs/s22flip.config config/benckernel-cat.config"
KERNEL_DTB=qcom/qm215-cat-s22flip.dtb

# Package release; the kernel release becomes <version>-<PKGREL>-BENCkernel-CAT
PKGREL=45

# Build output (never under the kernel tree)
BUILD_DIR=../out/benckernel
PKG_DIR=../out/pkg

CROSS_COMPILE=aarch64-linux-gnu-

# ------------------------------------------------------------ rootfs -------
# The rootfs is built rootless, in a user namespace (D009). Everything below
# lives outside git.
WORK=../out/benctix-cat
ALARM_MIRROR=http://mirror.archlinuxarm.org
ALARM_TARBALL_URL=http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
ARTIX_MIRROR=https://mirrors.rit.edu/artixlinux

# The OpenRC overlay BENCtix already built for aarch64 (D010). Copied, never
# written.
BENCTIX_OVERLAY=../../BENCphone/software/BENCtix/work/repo

# Local copies of phone-derived files (never published)
FIRMWARE_DIR=../firmware/lib/firmware
SENSORS_REG=../firmware/src/sensors/sns.reg
SENSORS_GROUPS=../firmware/src/sensors/sns-groups.txt

HOSTNAME_DEFAULT=s22flip
# zsh as BENCtix has it: oh-my-zsh at BENCtix's pinned commit, af-magic
OHMYZSH_REPO=https://github.com/ohmyzsh/ohmyzsh.git
OHMYZSH_REF=4d4cfc287e9d887b81242c0e431b5f49f9cec5c1
OHMYZSH_THEME=af-magic
# The owner's account (D016): uid 1000, home on the SD card, wheel for sudo,
# the same ssh key as root. Its password is set on the phone, never here.
OWNER_USER=micro
# The image's timezone; config.local.sh can set the builder's own
TIMEZONE=UTC
SSH_AUTHORIZED_KEYS_FILE=$HOME/.ssh/id_ed25519.pub

# USB gadget network (D012): the phone is .1, the host gets .2 by DHCP
USBNET_ADDR=172.16.42.1

# OpenRC stack from the overlay, installed by the swap (BENCtix D029 order)
OVERLAY_SWAP_PKGS=(libelogind libudev udev libeinfo openrc elogind etmpfiles
	esysusers eshutdown netifrc metalog pacman pam dbus util-linux-libs
	util-linux procps-ng)
# Overlay rebuilds of packages ALARM links against systemd (BENCtix D364)
OVERLAY_POSTSWAP_PKGS=(seatd dhcpcd libusb pciutils usbutils libgudev libinput)

# arch=any packages from Artix: base wires PID 1 to openrc-init
ARTIX_ANY_PKGS=(artix-keyring artix-mirrorlist artix-cgroups base pambase)
ARTIX_OPENRC_PKGS=(elogind-openrc dbus-openrc metalog-openrc dhcpcd-openrc
	openssh-openrc dnsmasq-openrc wpa_supplicant-openrc)

# Removed by the swap
SYSTEMD_PKGS=(systemd systemd-libs systemd-sysvcompat mkinitcpio
	systemd-resolvconf netctl)
SWAP_REMOVE_PKGS=(pacman-mirrorlist dbus-units dbus-broker-units dbus-broker
	cryptsetup device-mapper mkinitcpio-busybox linux-aarch64
	linux-aarch64-headers)

# From ALARM, before the swap. ALARM packages that only declare systemd-libs
# for libudev are satisfied by ours (EXTRA_ASSUME_INSTALLED).
ALARM_PKGS=(inetutils base-devel git zsh zsh-completions
	btrfs-progs f2fs-tools mmc-utils
	wpa_supplicant iw wireless-regdb dnsmasq openssh
	alsa-utils libqmi python-gobject i2c-tools evtest
	linux-firmware-qcom terminus-font less rsync)
# (libsystemd.so=0-64: the soname form, e.g. libpipewire and wireplumber ask for it)
EXTRA_ASSUME_INSTALLED=(systemd-libs libsystemd libsystemd.so=0-64
	xdg-desktop-portal xdg-desktop-portal-gtk)
# (the portals: GTK4 asks for them, and they ask for systemd; GTK works
# without them, D050)

# Added after the swap (stage 30), so a converted rootfs picks them up too
# PipeWire (D018): ALARM's; only libpipewire's journal and V4L2 plugins link
# libsystemd, and neither is loaded here
ALARM_POSTSWAP_PKGS=(chrony fastfetch pipewire pipewire-audio pipewire-alsa pipewire-pulse
	wlroots0.20 seatd foot ttf-dejavu grim
	v4l-utils mpv pipewire-jack openresolv
	tailscale wireguard-tools bubblewrap
	libretro-gambatte libretro-nestopia
	webkitgtk-6.0 gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav
	samba playerctl mpv-mpris wl-clipboard)
# avahi-openrc: pipewire-pulse-openrc depends on it (the daemon stays off)
ARTIX_POSTSWAP_PKGS=(chrony-openrc bluez-openrc pipewire-openrc pipewire-pulse-openrc wireplumber-openrc avahi-openrc)

# Artix's own builds of packages ALARM links against systemd (D013, now
# rebuilt): "name commit [package...]" in build order; each PKGBUILD is
# pinned to the Artix git commit that was checked. The optional list names
# which of a split PKGBUILD's packages to install (default: all of them).
# Built natively in stage 30.
ARTIX_GIT=https://gitea.artixlinux.org/packages
ARTIX_BUILD=(
	"polkit 25f27d7f94b4d2220586d3bdeaa7e17e3b20f973"
	"pcsclite 8a0c75edc3125815bfa30cc5d909873b3cfaca62"
	"btrfs-progs 55c2deefcc0eee6a41a1d9edbc02ca0376a734ac"
	"pam 3004b4b39ade3c4e8b773f18c93db31343ec2a4b"
	"util-linux 7b234e6353c68300496ce847b07b3f98d7fcd672"
	"bluez 1f3a3ae0c1f37d31e3564e2020c35dddbfba87bb bluez bluez-utils bluez-libs"
	"wireplumber f51d85bea605e7227abfbec345efb0d10142775f wireplumber libwireplumber"
	"pulseaudio 02b8d047d739774c96b4c284fac7b5b2f96de8a6 libpulse"
	"tmux 2365b64a6f72b5c15e1c73ef2a295bfabdd4b2f3"
)

# Built here, natively in the chroot (stage 30)
LOCAL_PKGS=(qrtr rmtfs tqftpserv s22-sensord s22-t9d s22-lidd s22-notify bencmouth s22-say s22-outerd s22-smsd s22-imsd s22-celld s22-gnssd s22-vpnd s22-btd aosp-sounds s22-battery s22-call s22-standin s22-gui s22-browser mepo)

# Never upgraded from ALARM on the phone (BENCtix D058)
IGNORE_PKGS=(base dbus elogind eshutdown esysusers etmpfiles libeinfo
	libelogind libudev metalog netifrc openrc pacman pam procps-ng udev
	util-linux util-linux-libs pambase seatd dhcpcd libusb pciutils usbutils
	libgudev libinput linux-aarch64 linux-aarch64-headers
	linux-benckernel-cat
	pipewire libpipewire pipewire-audio pipewire-alsa pipewire-pulse
	libpulse avahi
	webkitgtk-6.0 gtk4
	samba smbclient libwbclient)
# Everything built here is pinned as well, without a second list to keep:
# every LOCAL_PKGS package and every package an ARTIX_BUILD entry produces.
IGNORE_PKGS+=("${LOCAL_PKGS[@]}")
for _entry in "${ARTIX_BUILD[@]}"; do
	read -r -a _f <<< "$_entry"
	if [ "${#_f[@]}" -gt 2 ]; then IGNORE_PKGS+=("${_f[@]:2}"); else IGNORE_PKGS+=("${_f[0]}"); fi
done
read -r -a IGNORE_PKGS <<< "$(printf '%s\n' "${IGNORE_PKGS[@]}" | awk '!seen[$0]++' | tr '\n' ' ')"
unset _entry _f
# btrfs-progs, pcsclite, polkit: ALARM builds declare systemd/libsystemd, so
# upgrading them would pull systemd in; held until Artix rebuilds replace
# them (D013). qrtr, rmtfs, s22-sensord: ours; a same-named ALARM package
# must never replace them.

# Partition sizes in 512-byte sectors, read from the phone (sysfs), so the
# images fill them exactly. The root image is rounded down to 4 KiB.
CACHE_SECTORS=524288
USERDATA_SECTORS=23879647

# This builder's own settings (not tracked: .gitignore), over the above
[ -f "${BASH_SOURCE[0]%/*}/config.local.sh" ] && . "${BASH_SOURCE[0]%/*}/config.local.sh"
